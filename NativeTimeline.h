#pragma once

// Implementation header: include in exactly one ARC Objective-C++ translation unit.
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import <AVKit/AVKit.h>
#import <objc/runtime.h>
#import <dispatch/dispatch.h>
#include <dlfcn.h>
#include <string.h>
#include <math.h>

// These declarations describe messages, not replacement AVKit implementations.
@protocol PV2NativeTimelineMessages <NSObject>
- (instancetype)initWithPlayer:(AVPlayer *)player;
- (void)setPlayer:(AVPlayer *)player;
- (void)setPlayerController:(id)controller;
- (void)setIncludedControls:(unsigned long long)controls;
- (void)setExcludedControls:(unsigned long long)controls;
- (void)setOptimizeForPerformance:(BOOL)optimize;
- (void)updateVisibilityPolicy:(unsigned long long)policy animated:(BOOL)animated;
- (void)setIgnoresTouches:(BOOL)ignores;
- (UIView *)timelineView;
- (UIView *)playbackControlsView;
- (UIControl *)slider;
- (BOOL)isScrubbing;
@end

@interface PV2NativeTimelineViewController : UIViewController
- (instancetype)initWithPlayer:(AVPlayer *)player NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithNibName:(NSString *)name bundle:(NSBundle *)bundle NS_UNAVAILABLE;
- (instancetype)initWithCoder:(NSCoder *)coder NS_UNAVAILABLE;
- (instancetype)init NS_UNAVAILABLE;
@property(nonatomic,strong,readonly) AVPlayer *boundPlayer;
@property(nonatomic,readonly) UIView *timelineView;
@property(nonatomic,readonly) UIView *playbackControlsView;
@property(nonatomic,readonly) UIControl *slider;
// nil means no terminal failure; before window attachment timelineView/slider may be nil.
@property(nonatomic,copy,readonly) NSString *failureReason;
@property(nonatomic,readonly,getter=isScrubbing) BOOL scrubbing;
// Delivered on main, including the current state when installed. Capture hosts weakly.
@property(nonatomic,copy) void (^scrubbingDidChange)(BOOL scrubbing);
- (void)invalidate;
@end

// The wrapper only filters hit testing. It never draws a substitute control or
// moves any native view out of its AVKit owner.
@interface PV2NativeTimelineHostView : UIView
@property(nonatomic,weak) UIView *permittedTimeline;
@property(nonatomic,weak) UIView *permittedPlayback;
@property(nonatomic,copy) void (^windowDidChange)(void);
@end
@implementation PV2NativeTimelineHostView
- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event {
    UIView *hit = [super hitTest:point withEvent:event];
    UIView *timeline = self.permittedTimeline;
    UIView *playback = self.permittedPlayback;
    BOOL inTimeline = timeline && (hit == timeline || [hit isDescendantOfView:timeline]);
    BOOL inPlayback = playback && (hit == playback || [hit isDescendantOfView:playback]);
    return (inTimeline || inPlayback) ? hit : nil;
}
- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (self.windowDidChange) self.windowDidChange();
}
@end

static BOOL PV2NativeTimelineABI(Class cls, SEL selector, const char *encoding) {
    Method method = cls ? class_getInstanceMethod(cls, selector) : NULL;
    const char *actual = method ? method_getTypeEncoding(method) : NULL;
    return actual && strcmp(actual, encoding) == 0;
}

static NSString *PV2NativeTimelineRequire(Class cls, SEL selector, const char *encoding) {
    if (PV2NativeTimelineABI(cls, selector, encoding)) return nil;
    return [NSString stringWithFormat:@"Unsupported AVKit ABI: %s %s (expected %s)",
            cls ? class_getName(cls) : "<missing class>", sel_getName(selector), encoding];
}

static void *PV2NativeTimelineScrubbingContext = &PV2NativeTimelineScrubbingContext;

// All calls here run on main. In particular, do not pause, seek, replace an item,
// endScrubbing manually, or restore a guessed playback rate on the Photos player.
static void PV2NativeTimelineDisconnect(UIViewController *controls, id controller,
                                       UIControl *slider) {
    // Native cancelTracking forwards sliderDidEndTracking: to the original delegate,
    // balancing native playback suspension before either binding is disconnected.
    if (slider.isTracking) [slider cancelTrackingWithEvent:nil];
    if (controls) [(id<PV2NativeTimelineMessages>)controls setPlayerController:nil];
    if (controller) [(id<PV2NativeTimelineMessages>)controller setPlayer:nil];
    if (controls.isViewLoaded) [controls.view removeFromSuperview];
    if (controls.parentViewController) [controls removeFromParentViewController];
}

@interface PV2NativeTimelineViewController () {
    AVPlayer *_boundPlayer;
    UIViewController *_nativeControls;
    id _nativePlayerController;
    PV2NativeTimelineHostView *_timelineHost;
    __weak UIView *_nativeTimeline;
    __weak UIControl *_nativeSlider;
    __weak UIView *_nativePlayback;
    NSString *_failureReason;
    void (^_scrubbingDidChange)(BOOL);
    BOOL _invalidated;
    BOOL _observingScrubbing;
    BOOL _observedScrubbing;
    BOOL _layingOutNative;
    BOOL _nativeDidMove;
    BOOL _readinessCheckPending;
    NSUInteger _layoutAttempts;
    NSUInteger _bindingGeneration;
}
- (void)pv2_fail:(NSString *)reason;
- (void)pv2_layoutNative;
- (void)pv2_discoverTimelineRequiringReady:(BOOL)required;
- (void)pv2_publishScrubbing:(BOOL)scrubbing generation:(NSUInteger)generation;
@end

@implementation PV2NativeTimelineViewController

- (instancetype)initWithPlayer:(AVPlayer *)player {
    // UIKit construction/containment and all public reads are main-thread APIs.
    if (![NSThread isMainThread]) {
        @throw [NSException exceptionWithName:NSInternalInconsistencyException
                                       reason:@"PV2NativeTimeline must be initialized on main"
                                     userInfo:nil];
    }
    self = [super initWithNibName:nil bundle:nil];
    if (!self) return nil;
    _bindingGeneration = 1;
    if (![player isKindOfClass:[AVPlayer class]]) {
        [self pv2_fail:@"A live, existing AVPlayer is required"];
        return self;
    }

    // Photos need not already have loaded AVKit. Keep the framework loaded for
    // process lifetime; dlclose while native classes/observers exist is unsafe.
    if (!NSClassFromString(@"AVMobileChromelessControlsViewController")) {
        (void)dlopen("/System/Library/Frameworks/AVKit.framework/AVKit", RTLD_NOW | RTLD_LOCAL);
    }
    Class playerClass = NSClassFromString(@"AVPlayerController");
    Class controlsClass = NSClassFromString(@"AVMobileChromelessControlsViewController");
    Class rootClass = NSClassFromString(@"AVMobileChromelessControlsView");
    Class timelineClass = NSClassFromString(@"AVMobileChromelessTimelineView");
    if (!playerClass || ![controlsClass isSubclassOfClass:[UIViewController class]] ||
        ![rootClass isSubclassOfClass:[UIView class]] ||
        ![timelineClass isSubclassOfClass:[UIView class]]) {
        [self pv2_fail:@"Required native AVKit classes are unavailable"];
        return self;
    }

    // Exact arm64 method encodings, including inherited registered methods.
    // No symbol-address calls, objc_direct entry points, forwarding, or swizzling.
    struct Requirement { Class cls; SEL sel; const char *types; } requirements[] = {
        {playerClass, @selector(initWithPlayer:), "@24@0:8@16"},
        {playerClass, @selector(setPlayer:), "v24@0:8@16"},
        {playerClass, @selector(isScrubbing), "B16@0:8"},
        {controlsClass, @selector(init), "@16@0:8"},
        {controlsClass, @selector(setPlayerController:), "v24@0:8@16"},
        {controlsClass, @selector(setIncludedControls:), "v24@0:8Q16"},
        {controlsClass, @selector(setExcludedControls:), "v24@0:8Q16"},
        {controlsClass, @selector(setOptimizeForPerformance:), "v20@0:8B16"},
        {controlsClass, @selector(updateVisibilityPolicy:animated:), "v28@0:8Q16B24"},
        {rootClass, @selector(timelineView), "@16@0:8"},
        {rootClass, @selector(playbackControlsView), "@16@0:8"},
        {rootClass, @selector(setIgnoresTouches:), "v20@0:8B16"},
        {timelineClass, @selector(slider), "@16@0:8"}
    };
    for (const auto &requirement : requirements) {
        NSString *reason = PV2NativeTimelineRequire(requirement.cls, requirement.sel,
                                                    requirement.types);
        if (reason) {
            [self pv2_fail:reason];
            return self;
        }
    }

    @try {
        _nativePlayerController = [(id<PV2NativeTimelineMessages>)[playerClass alloc]
                                  initWithPlayer:player];
        _nativeControls = [[controlsClass alloc] init];
        if (![_nativePlayerController isKindOfClass:playerClass] ||
            ![_nativeControls isKindOfClass:controlsClass]) {
            [self pv2_fail:@"Native AVKit controller initialization failed"];
            return self;
        }
        _boundPlayer = player;
        id<PV2NativeTimelineMessages> controls = (id)_nativeControls;
        [controls setOptimizeForPerformance:NO];
        [controls setExcludedControls:0];
        [controls setIncludedControls:(1ULL | 8ULL)]; // timeline plus native playback transport only.
        [controls updateVisibilityPolicy:0 animated:NO];
        [controls setPlayerController:_nativePlayerController];
        // KVO's initial notification is synchronous. Install the flag first.
        _observingScrubbing = YES;
        @try {
            [_nativePlayerController addObserver:self forKeyPath:@"scrubbing"
                                        options:NSKeyValueObservingOptionInitial |
                                                NSKeyValueObservingOptionNew
                                        context:PV2NativeTimelineScrubbingContext];
        } @catch (NSException *exception) {
            _observingScrubbing = NO;
            @throw exception;
        }
    } @catch (NSException *exception) {
        [self pv2_fail:[NSString stringWithFormat:@"Native AVKit binding failed: %@",
                       exception.name]];
    }
    return self;
}

- (void)pv2_fail:(NSString *)reason { if (!_failureReason) _failureReason = [reason copy]; }

- (void)loadView {
    _timelineHost = [[PV2NativeTimelineHostView alloc] initWithFrame:CGRectZero];
    _timelineHost.backgroundColor = UIColor.clearColor;
    __weak typeof(self) weakSelf = self;
    _timelineHost.windowDidChange = ^{ [weakSelf pv2_layoutNative]; };
    self.view = _timelineHost;
}
- (void)viewDidAppear:(BOOL)animated { [super viewDidAppear:animated]; [self pv2_layoutNative]; }
- (void)viewDidLayoutSubviews { [super viewDidLayoutSubviews]; [self pv2_layoutNative]; }

- (void)pv2_layoutNative {
    if (_invalidated || !_nativeControls || _layingOutNative) return;
    _layingOutNative = YES;
    UIView *root = _nativeControls.view;
    if (!root) { _layingOutNative = NO; return; }
    root.frame = _timelineHost.bounds;
    root.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    if (_nativeControls.parentViewController != self)
        [self addChildViewController:_nativeControls];
    if (root.superview != _timelineHost) {
        [_timelineHost addSubview:root];
        [(id<PV2NativeTimelineMessages>)root setIgnoresTouches:NO];
    }
    if (!_nativeDidMove) { [_nativeControls didMoveToParentViewController:self]; _nativeDidMove = YES; }
    [root setNeedsLayout]; [root layoutIfNeeded];
    [self pv2_discoverTimelineRequiringReady:NO];
    _layingOutNative = NO;
}

- (void)pv2_discoverTimelineRequiringReady:(BOOL)required {
    if (_invalidated || !_nativeControls) return;
    UIView *root = _nativeControls.view;
    UIView *timeline = [root respondsToSelector:@selector(timelineView)]
        ? [(id<PV2NativeTimelineMessages>)root timelineView] : nil;
    UIView *playback = [root respondsToSelector:@selector(playbackControlsView)]
        ? [(id<PV2NativeTimelineMessages>)root playbackControlsView] : nil;
    UIControl *slider = [timeline respondsToSelector:@selector(slider)]
        ? [(id<PV2NativeTimelineMessages>)timeline slider] : nil;
    if (timeline && slider && playback) {
        _nativeTimeline = timeline; _nativeSlider = slider; _nativePlayback = playback;
        _timelineHost.permittedTimeline = timeline;
        _timelineHost.permittedPlayback = playback;
        _readinessCheckPending = NO;
        _layoutAttempts = 0;
        // Policy 0 is AVKit's persistent-visible policy; this only asks the
        // owner to apply its own visibility state after natural setup.
        [(id<PV2NativeTimelineMessages>)_nativeControls updateVisibilityPolicy:0 animated:NO];
        return;
    }
    // Native setup is naturally window-driven and may complete after several
    // run-loop turns. Give it three deferred opportunities, with no recursion.
    if (!_readinessCheckPending && _timelineHost.window && _layoutAttempts < 3) {
        _readinessCheckPending = YES; _layoutAttempts++; __weak typeof(self) weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            PV2NativeTimelineViewController *strongSelf = weakSelf;
            if (!strongSelf || strongSelf->_invalidated) return;
            strongSelf->_readinessCheckPending = NO;
            [strongSelf pv2_layoutNative];
            [strongSelf pv2_discoverTimelineRequiringReady:YES];
        });
    } else if (required && _layoutAttempts >= 3) {
        [self pv2_fail:@"AVKit controls owner did not expose a native timeline and slider"];
    }
}
- (UIView *)timelineView { if (!_invalidated) [self pv2_discoverTimelineRequiringReady:NO]; return _nativeTimeline; }
- (UIView *)playbackControlsView { if (!_invalidated) [self pv2_discoverTimelineRequiringReady:NO]; return _nativePlayback; }
- (UIControl *)slider { if (!_invalidated) [self pv2_discoverTimelineRequiringReady:NO]; return _nativeSlider; }
- (AVPlayer *)boundPlayer { return _boundPlayer; }
- (NSString *)failureReason { return _failureReason; }
- (BOOL)isScrubbing { return !_invalidated && _nativePlayerController && [(id<PV2NativeTimelineMessages>)_nativePlayerController isScrubbing]; }

- (void)setScrubbingDidChange:(void (^)(BOOL))callback {
    NSParameterAssert([NSThread isMainThread]); _scrubbingDidChange = [callback copy];
    if (_scrubbingDidChange && !_invalidated && _nativePlayerController) _scrubbingDidChange(self.isScrubbing);
}
- (void (^)(BOOL))scrubbingDidChange { return _scrubbingDidChange; }

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey,id> *)change context:(void *)context {
    if (context != PV2NativeTimelineScrubbingContext) {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context]; return;
    }
    if (object != _nativePlayerController || ![keyPath isEqualToString:@"scrubbing"] || _invalidated) return;
    NSNumber *newValue = change[NSKeyValueChangeNewKey];
    BOOL value = newValue ? newValue.boolValue : NO;
    NSUInteger generation = _bindingGeneration;
    [self pv2_publishScrubbing:value generation:generation];
}
- (void)pv2_publishScrubbing:(BOOL)value generation:(NSUInteger)generation {
    if (!_scrubbingDidChange || _invalidated || generation != _bindingGeneration) return;
    void (^notify)(BOOL) = [_scrubbingDidChange copy]; if (!notify) return;
    if ([NSThread isMainThread]) notify(value);
    else { __weak typeof(self) weakSelf = self; dispatch_async(dispatch_get_main_queue(), ^{
        PV2NativeTimelineViewController *strongSelf = weakSelf;
        if (strongSelf && !strongSelf->_invalidated && generation == strongSelf->_bindingGeneration) notify(value);
    }); }
}

- (void)invalidate {
    if (![NSThread isMainThread]) { __weak typeof(self) weakSelf = self; dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf invalidate]; }); return; }
    if (_invalidated) return; _invalidated = YES; _bindingGeneration++; _readinessCheckPending = NO;
    if (_observingScrubbing && _nativePlayerController) {
        [_nativePlayerController removeObserver:self forKeyPath:@"scrubbing" context:PV2NativeTimelineScrubbingContext];
        _observingScrubbing = NO;
    }
    if (_nativeDidMove) {
        [_nativeControls willMoveToParentViewController:nil];
        _nativeDidMove = NO;
    }
    PV2NativeTimelineDisconnect(_nativeControls, _nativePlayerController, _nativeSlider);
    _timelineHost.windowDidChange = nil; _timelineHost.permittedTimeline = nil; _timelineHost.permittedPlayback = nil;
    _nativeTimeline = nil; _nativeSlider = nil; _nativePlayback = nil; _nativeControls = nil; _nativePlayerController = nil;
    _boundPlayer = nil; _scrubbingDidChange = nil;
}
- (void)dealloc {
    // The integration normally calls invalidate on main. Keep release-only
    // teardown complete for harnesses that simply drop the last strong ref.
    if (_observingScrubbing && _nativePlayerController)
        [_nativePlayerController removeObserver:self forKeyPath:@"scrubbing" context:PV2NativeTimelineScrubbingContext];
    if (_nativeControls || _nativePlayerController) {
        if ([NSThread isMainThread])
            PV2NativeTimelineDisconnect(_nativeControls, _nativePlayerController, _nativeSlider);
        else
            [(id<PV2NativeTimelineMessages>)_nativeControls setPlayerController:nil];
        [(id<PV2NativeTimelineMessages>)_nativePlayerController setPlayer:nil];
    }
}
@end
#undef PV2NativeTimelineScrubbingContext
