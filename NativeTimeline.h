#pragma once

// Implementation header: include in exactly one ARC Objective-C++ translation unit.
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import <AVKit/AVKit.h>
#import <objc/runtime.h>
#import <objc/message.h>
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
- (void)setInspectionSuspended:(BOOL)suspended;
- (void)setLabelPosition:(unsigned long long)position;
- (CMTimeRange)loopTimeRange;
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
// Only commands are bridged. UI/labels/timing remain system-owned.
@property(nonatomic,copy) void (^playbackRateRequested)(double rate, BOOL temporarySuspension);
// Explicit UI actions never infer persistent intent from temporary player.rate.
@property(nonatomic,copy) void (^playbackToggleRequested)(void);
@property(nonatomic,copy) NSInteger (^playbackIntent)(void); // -1 unknown, 0 paused, 1 playing
- (void)refreshPlaybackIntent;
@property(nonatomic,copy) void (^controlsDidBecomeReady)(void);
@property(nonatomic,copy) void (^seekRequested)(CMTime target, CMTime before, CMTime after, void (^completed)(BOOL finished));
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

// A dedicated subclass for this tweak's AVPlayerController, not a global
// AVPlayer hook. Keep native suspension/coalescing/completions; route only the
// final rate request and displayed-to-Photos seek coordinate resolution.
@interface PV2NativeCommandBridge : NSObject
@property(nonatomic,copy) void (^rate)(double,BOOL);
@property(nonatomic) NSUInteger suspensionDepth;
@property(nonatomic) NSUInteger temporaryRateDepth;
@property(nonatomic,copy) void (^toggle)(void);
@property(nonatomic,copy) NSInteger (^intent)(void);
@property(nonatomic,copy) void (^seek)(CMTime,CMTime,CMTime,void (^)(BOOL));
@property(nonatomic) BOOL seekInFlight;
@property(nonatomic) BOOL hasPendingSeek;
@property(nonatomic) CMTime pendingTime;
@property(nonatomic) CMTime pendingBefore;
@property(nonatomic) CMTime pendingAfter;
@property(nonatomic,weak) id controller;
@property(nonatomic,weak) AVPlayer *player;
@property(nonatomic) BOOL valid;
@property(nonatomic) NSUInteger seekGeneration;
@end
@implementation PV2NativeCommandBridge
@end
static const void *PV2NativeCommandKey=&PV2NativeCommandKey;
static const void *PV2NativeTimelineOwnerKey=&PV2NativeTimelineOwnerKey;
static const void *PV2NativeTransportHiddenKey=&PV2NativeTransportHiddenKey;
static void PV2NativeAuthorityRate(id controller,SEL cmd,double rate) {
    PV2NativeCommandBridge *bridge=objc_getAssociatedObject(controller,PV2NativeCommandKey);
    if (bridge && !bridge.valid) return;
    if (bridge.rate) { bridge.rate(rate,bridge.suspensionDepth>0 || bridge.temporaryRateDepth>0); return; }
    struct objc_super parent={controller,NSClassFromString(@"AVPlayerController")};
    ((void (*)(struct objc_super*,SEL,double))objc_msgSendSuper)(&parent,cmd,rate);
}
static void PV2NativeReconcileRestoreIntent(PV2NativeCommandBridge *b) {
    if (!b.valid || !b.intent || !b.controller) return;
    NSInteger intent=b.intent(); if (intent<0) return;
    id controller=b.controller;
    for (NSString *name in @[@"suspendedRate",@"rateBeforeScrubBegan"]) {
        SEL get=NSSelectorFromString(name);
        SEL set=NSSelectorFromString([name isEqualToString:@"suspendedRate"] ? @"_setSuspendedRate:" : @"setRateBeforeScrubBegan:");
        if (!PV2NativeTimelineABI([controller class],get,"d16@0:8") ||
            !PV2NativeTimelineABI([controller class],set,"v24@0:8d16")) continue;
        double cached=((double (*)(id,SEL))objc_msgSend)(controller,get);
        // NaN means no native suspension/scrub restore is armed. Never create
        // one, and never clear it before native AVKit balances the gesture.
        if (!isfinite(cached)) continue;
        double desired=intent==0 ? 0 : cached>0 ? cached : 1;
        if (cached!=desired) ((void (*)(id,SEL,double))objc_msgSend)(controller,set,desired);
    }
}
static void PV2NativeAuthorityToggle(id controller,SEL cmd,id sender) {
    PV2NativeCommandBridge *b=objc_getAssociatedObject(controller,PV2NativeCommandKey);
    if (b && !b.valid) return;
    if (b.toggle) { b.toggle(); return; }
    struct objc_super parent={controller,NSClassFromString(@"AVPlayerController")};
    ((void (*)(struct objc_super*,SEL,id))objc_msgSendSuper)(&parent,cmd,sender);
}
static void PV2NativeAuthorityUpdateRate(id controller,SEL cmd) {
    PV2NativeCommandBridge *b=objc_getAssociatedObject(controller,PV2NativeCommandKey);
    if (b && !b.valid) return;
    b.temporaryRateDepth++;
    PV2NativeReconcileRestoreIntent(b);
    @try {
        struct objc_super parent={controller,NSClassFromString(@"AVPlayerController")};
        ((void (*)(struct objc_super*,SEL))objc_msgSendSuper)(&parent,cmd);
    } @finally { if (b.temporaryRateDepth) b.temporaryRateDepth--; }
}
// Project Photos' *actual toolbar state* into the original native button. Its
// image, accessibility and target remain AVKit's; never falsify rate/isPlaying.
static void PV2NativeAuthorityIcon(id view,SEL cmd,BOOL showsPlay) {
    PV2NativeCommandBridge *b=objc_getAssociatedObject(view,PV2NativeCommandKey);
    if (b.valid && b.intent) {
        NSInteger intent=b.intent();
        if (intent>=0) showsPlay=intent==0;
    }
    struct objc_super parent={view,NSClassFromString(@"AVMobileChromelessPlaybackControlsView")};
    ((void (*)(struct objc_super*,SEL,BOOL))objc_msgSendSuper)(&parent,cmd,showsPlay);
}
static BOOL PV2NativeConfigurePlaybackIntent(UIView *view,PV2NativeCommandBridge *bridge) {
    Class base=NSClassFromString(@"AVMobileChromelessPlaybackControlsView");
    SEL setter=NSSelectorFromString(@"setPlayPauseButtonShowsPlayIcon:");
    if (![view isKindOfClass:base] || !PV2NativeTimelineABI(base,setter,"v20@0:8B16")) return NO;
    static Class projected=Nil; static dispatch_once_t once;
    dispatch_once(&once,^{
        projected=objc_allocateClassPair(base,"PV2PhotosPlaybackControlsView",0);
        if (projected && class_addMethod(projected,setter,(IMP)PV2NativeAuthorityIcon,"v20@0:8B16")) objc_registerClassPair(projected);
        else { if (projected) objc_disposeClassPair(projected); projected=Nil; }
    });
    if (!projected) return NO;
    if (object_getClass(view)==base) object_setClass(view,projected);
    if (![view isKindOfClass:projected]) return NO;
    objc_setAssociatedObject(view,PV2NativeCommandKey,bridge,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return YES;
}
static void PV2NativeAuthoritySeek(id controller,SEL cmd,CMTime target,CMTime before,CMTime after) {
    PV2NativeCommandBridge *bridge=objc_getAssociatedObject(controller,PV2NativeCommandKey);
    if (bridge && !bridge.valid) return;
    if (!bridge.seek) {
        struct objc_super parent={controller,NSClassFromString(@"AVPlayerController")};
        ((void (*)(struct objc_super*,SEL,CMTime,CMTime,CMTime))objc_msgSendSuper)(&parent,cmd,target,before,after); return;
    }
    if (bridge.seekInFlight) {
        bridge.hasPendingSeek=YES; bridge.pendingTime=target;
        bridge.pendingBefore=before; bridge.pendingAfter=after; return;
    }
    bridge.seekInFlight=YES;
    NSUInteger generation=++bridge.seekGeneration;
    ((void (*)(id,SEL,BOOL))objc_msgSend)(controller,NSSelectorFromString(@"setSeeking:"),YES);
    ((void (*)(id,SEL,BOOL))objc_msgSend)(controller,NSSelectorFromString(@"setSeekingInternal:"),YES);
    ((void (*)(id,SEL,CMTime))objc_msgSend)(controller,NSSelectorFromString(@"setSeekToTimeInternal:"),target);
    __weak id weakController=controller;
    __weak PV2NativeCommandBridge *weakBridge=bridge;
    bridge.seek(target,before,after,^(BOOL finished){
        (void)finished;
        dispatch_async(dispatch_get_main_queue(),^{
            id live=weakController; PV2NativeCommandBridge *b=weakBridge;
            if (!live || !b.valid || b.seekGeneration!=generation) return;
            b.seekInFlight=NO;
            if (b.hasPendingSeek) {
                b.hasPendingSeek=NO;
                PV2NativeAuthoritySeek(live,cmd,b.pendingTime,b.pendingBefore,b.pendingAfter);
                return;
            }
            ((void (*)(id,SEL,BOOL))objc_msgSend)(live,NSSelectorFromString(@"setSeekingInternal:"),NO);
            ((void (*)(id,SEL,CMTime))objc_msgSend)(live,NSSelectorFromString(@"setSeekToTimeInternal:"),kCMTimeInvalid);
            ((void (*)(id,SEL,BOOL))objc_msgSend)(live,NSSelectorFromString(@"setSeeking:"),NO);
            SEL update=NSSelectorFromString(@"_updateRateForScrubbingAndSeeking");
            if (PV2NativeTimelineABI([live class],update,"v16@0:8")) ((void (*)(id,SEL))objc_msgSend)(live,update);
        });
    });
}
static void PV2NativeAuthorityBeginSuspension(id controller,SEL cmd) {
    PV2NativeCommandBridge *b=objc_getAssociatedObject(controller,PV2NativeCommandKey);
    if (b && !b.valid) return;
    b.suspensionDepth++;
    struct objc_super parent={controller,NSClassFromString(@"AVPlayerController")};
    ((void (*)(struct objc_super*,SEL))objc_msgSendSuper)(&parent,cmd);
    PV2NativeReconcileRestoreIntent(b);
}
static void PV2NativeAuthorityEndSuspension(id controller,SEL cmd) {
    PV2NativeCommandBridge *b=objc_getAssociatedObject(controller,PV2NativeCommandKey);
    if (b && !b.valid) return;
    PV2NativeReconcileRestoreIntent(b);
    struct objc_super parent={controller,NSClassFromString(@"AVPlayerController")};
    ((void (*)(struct objc_super*,SEL))objc_msgSendSuper)(&parent,cmd);
    if (b.suspensionDepth) b.suspensionDepth--;
    PV2NativeTimelineViewController *owner=objc_getAssociatedObject(controller,PV2NativeTimelineOwnerKey);
    if (owner) ((void (*)(id,SEL,id,CMTime))objc_msgSend)(owner,NSSelectorFromString(@"pv2_refreshLoopTimingForItem:observedTime:"),owner.boundPlayer.currentItem,kCMTimeInvalid);
}
// Photos represents a finite loop as an indefinitely repeating AVPlayerItem.
// Report the item's real per-lap media range to native AVKit, not infinity.
// The native timebase/timing and timeline observation remain unchanged.
static CMTimeRange PV2NativePhotosLoopRange(id controller) {
    PV2NativeCommandBridge *b=objc_getAssociatedObject(controller,PV2NativeCommandKey);
    AVPlayerItem *item=b.player.currentItem;
    SEL selector=@selector(loopTimeRange);
    if (b.valid && PV2NativeTimelineABI([item class],selector,"{?={?=qiIq}{?=qiIq}}16@0:8")) {
        CMTimeRange range=[(id<PV2NativeTimelineMessages>)item loopTimeRange];
        if (CMTIMERANGE_IS_VALID(range) && CMTIME_IS_NUMERIC(range.start) && CMTIME_IS_NUMERIC(range.duration) &&
            CMTimeCompare(range.duration,kCMTimeZero)>0) return range;
    }
    return kCMTimeRangeInvalid;
}
static double PV2FoldLoopSeconds(double value,double low,double high) {
    if (!isfinite(value) || !isfinite(low) || !isfinite(high) || high<=low) return NAN;
    double duration=high-low;
    double offset=fmod(value-low,duration);
    if (offset<0) offset+=duration;
    return low+offset;
}
static double PV2NativePhotosRangeValue(id controller,SEL selector) {
    CMTimeRange range=PV2NativePhotosLoopRange(controller);
    if (CMTIMERANGE_IS_VALID(range)) {
        if (selector==NSSelectorFromString(@"minTime")) return CMTimeGetSeconds(range.start);
        if (selector==NSSelectorFromString(@"maxTime")) return CMTimeGetSeconds(CMTimeRangeGetEnd(range));
        return CMTimeGetSeconds(range.duration);
    }
    struct objc_super parent={controller,NSClassFromString(@"AVPlayerController")};
    return ((double (*)(struct objc_super*,SEL))objc_msgSendSuper)(&parent,selector);
}
static Class PV2NativeAuthorityClass(void) {
    Class base=NSClassFromString(@"AVPlayerController");
    if (!PV2NativeTimelineABI(base,NSSelectorFromString(@"togglePlayback:"),"v24@0:8@16") ||
        !PV2NativeTimelineABI(base,NSSelectorFromString(@"_updateRateForScrubbingAndSeeking"),"v16@0:8") ||
        !PV2NativeTimelineABI(base,NSSelectorFromString(@"_playAtRate:"),"v24@0:8d16") ||
        !PV2NativeTimelineABI(base,NSSelectorFromString(@"seekToCMTime:toleranceBefore:toleranceAfter:"),"v88@0:8{?=qiIq}16{?=qiIq}40{?=qiIq}64") ||
        !PV2NativeTimelineABI(base,NSSelectorFromString(@"setSeeking:"),"v20@0:8B16") ||
        !PV2NativeTimelineABI(base,NSSelectorFromString(@"setSeekingInternal:"),"v20@0:8B16") ||
        !PV2NativeTimelineABI(base,NSSelectorFromString(@"setSeekToTimeInternal:"),"v40@0:8{?=qiIq}16")) return Nil;
    static Class authority=Nil; static dispatch_once_t once;
    dispatch_once(&once,^{
        authority=objc_allocateClassPair(base,"PV2PhotosAVPlayerController",0);
        Method rate=class_getInstanceMethod(base,NSSelectorFromString(@"_playAtRate:"));
        Method seek=class_getInstanceMethod(base,NSSelectorFromString(@"seekToCMTime:toleranceBefore:toleranceAfter:"));
        Method begin=class_getInstanceMethod(base,NSSelectorFromString(@"beginPlaybackSuspension"));
        Method end=class_getInstanceMethod(base,NSSelectorFromString(@"endPlaybackSuspension"));
        BOOL ranges=authority!=Nil;
        for (NSString *name in @[@"contentDuration",@"minTime",@"maxTime"]) {
            SEL sel=NSSelectorFromString(name);
            Method method=class_getInstanceMethod(base,sel);
            ranges=ranges && method && strcmp(method_getTypeEncoding(method),"d16@0:8")==0 &&
                class_addMethod(authority,sel,(IMP)PV2NativePhotosRangeValue,method_getTypeEncoding(method));
        }
        if (authority && ranges &&
            class_addMethod(authority,NSSelectorFromString(@"togglePlayback:"),(IMP)PV2NativeAuthorityToggle,"v24@0:8@16") &&
            class_addMethod(authority,NSSelectorFromString(@"_updateRateForScrubbingAndSeeking"),(IMP)PV2NativeAuthorityUpdateRate,"v16@0:8") &&
            class_addMethod(authority,method_getName(rate),(IMP)PV2NativeAuthorityRate,method_getTypeEncoding(rate)) &&
            class_addMethod(authority,method_getName(seek),(IMP)PV2NativeAuthoritySeek,method_getTypeEncoding(seek)) &&
            begin && end && class_addMethod(authority,method_getName(begin),(IMP)PV2NativeAuthorityBeginSuspension,method_getTypeEncoding(begin)) &&
            class_addMethod(authority,method_getName(end),(IMP)PV2NativeAuthorityEndSuspension,method_getTypeEncoding(end))) objc_registerClassPair(authority);
        else { if (authority) objc_disposeClassPair(authority); authority=Nil; }
    });
    return authority;
}
// AVKit assumes fullscreen margins when no fullscreen presentation owner is
// present. Override only this root's spacing, leaving its real subview layout,
// targets, time resolver and tracking entirely in AVKit.
static void PV2CompactRootSetMargins(UIView *view, SEL selector, UIEdgeInsets proposed) {
    (void)proposed;
    struct objc_super parent = {view, NSClassFromString(@"AVMobileChromelessControlsView")};
    ((void (*)(struct objc_super *,SEL,UIEdgeInsets))objc_msgSendSuper)(
        &parent,selector,UIEdgeInsetsMake(4,8,4,8));
}
// Geometry adapter only: both views remain AVKit's original objects in the
// original owner. Native targets/delegates/observations are never replaced.
static void PV2HidePlaybackVisuals(UIView *view) {
    if (objc_getAssociatedObject(view,PV2NativeTransportHiddenKey)) return;
    objc_setAssociatedObject(view,PV2NativeTransportHiddenKey,@YES,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    for (UIView *subview in view.subviews) {
        subview.hidden=YES; subview.userInteractionEnabled=NO;
        PV2HidePlaybackVisuals(subview);
    }
}
static void PV2CompactRootLayout(UIView *root, SEL selector) {
    // This root is a compact geometry adapter, not a fullscreen canvas. Calling
    // AVKit's fullscreen layout first makes it overwrite every frame/transform
    // which we then undo, invalidating the same layout transaction forever.
    // Preserve UIView's layout contract and all genuine child implementations.
    struct objc_super parent={root,UIView.class};
    ((void (*)(struct objc_super*,SEL))objc_msgSendSuper)(&parent,selector);
    UIView *timeline=[(id<PV2NativeTimelineMessages>)root timelineView];
    UIView *playback=[(id<PV2NativeTimelineMessages>)root playbackControlsView];
    CGFloat width=CGRectGetWidth(root.bounds), height=CGRectGetHeight(root.bounds);
    if (!timeline || width<40 || height<60) return;
    [UIView performWithoutAnimation:^{
        SEL label=NSSelectorFromString(@"labelPosition");
        if (PV2NativeTimelineABI([timeline class],label,"Q16@0:8") &&
            ((NSUInteger (*)(id,SEL))objc_msgSend)(timeline,label)!=1) {
            [(id<PV2NativeTimelineMessages>)timeline setLabelPosition:1];
            [timeline invalidateIntrinsicContentSize];
        }
        CGFloat timelineHeight=timeline.intrinsicContentSize.height;
        if (!isfinite(timelineHeight) || timelineHeight<1) timelineHeight=44;
        timelineHeight=MIN(timelineHeight,height-36);
        CGFloat timelineWidth=MAX(40,width-16);
        BOOL landscape=root.window && CGRectGetWidth(root.window.bounds)>CGRectGetHeight(root.window.bounds);
        CGFloat bottomInset=landscape ? -12 : 12;
        CGRect frame=CGRectMake(8,height-bottomInset-timelineHeight,timelineWidth,timelineHeight);
        if (!CGRectEqualToRect(timeline.frame,frame)) timeline.frame=frame;
        if (timeline.hidden) timeline.hidden=NO;
        if (playback) {
            CGSize nativeSize=playback.intrinsicContentSize;
            CGFloat nativeHeight=MAX(1,nativeSize.height);
            CGFloat nativeWidth=MAX(nativeHeight+42,nativeSize.width);
            CGFloat availableHeight=MAX(24,height-timelineHeight-12);
            CGFloat scale=MIN(1,MIN(42/nativeHeight,availableHeight/nativeHeight));
            CGRect bounds=CGRectMake(0,0,nativeWidth,nativeHeight);
            CGPoint center=CGPointMake(width-8-nativeWidth*scale/2,height-bottomInset-timelineHeight/2);
            CGAffineTransform transform=CGAffineTransformMakeScale(scale,scale);
            if (!CGRectEqualToRect(playback.bounds,bounds)) playback.bounds=bounds;
            if (!CGPointEqualToPoint(playback.center,center)) playback.center=center;
            if (!CGAffineTransformEqualToTransform(playback.transform,transform)) playback.transform=transform;
            playback.hidden=NO;
            playback.userInteractionEnabled=NO;
            PV2HidePlaybackVisuals(playback);
        }
    }];
}
static BOOL PV2ConfigureCompactNativeRoot(UIView *view) {
    Class base=NSClassFromString(@"AVMobileChromelessControlsView");
    if (![view isKindOfClass:base]) return NO;
    static Class compact=Nil; static dispatch_once_t once;
    dispatch_once(&once,^{
        compact=objc_allocateClassPair(base,"PV2CompactAVKitControlsView",0);
        Method setter=class_getInstanceMethod(base,@selector(setLayoutMargins:));
        Method layout=class_getInstanceMethod(base,@selector(layoutSubviews));
        if (compact && setter && layout && class_addMethod(compact,@selector(setLayoutMargins:),
            (IMP)PV2CompactRootSetMargins,method_getTypeEncoding(setter)) &&
            class_addMethod(compact,@selector(layoutSubviews),(IMP)PV2CompactRootLayout,method_getTypeEncoding(layout))) objc_registerClassPair(compact);
        else { if (compact) objc_disposeClassPair(compact); compact=Nil; }
    });
    if (!compact) return NO;
    if (object_getClass(view)!=compact) object_setClass(view,compact);
    view.preservesSuperviewLayoutMargins=NO; view.insetsLayoutMarginsFromSafeArea=NO;
    UIEdgeInsets margins=UIEdgeInsetsMake(4,8,4,8);
    if (!UIEdgeInsetsEqualToEdgeInsets(view.layoutMargins,margins)) view.layoutMargins=margins;
    return YES;
}
// The internal _updateControlsVisibilityStateAnimated: is objc_direct on
// iOS17.3. Only this registered NSNumber-taking message boundary is overridden.
static void PV2CompactVisibility(id owner,SEL selector,id animated) {
    (void)animated;
    [UIView performWithoutAnimation:^{
        struct objc_super parent={owner,NSClassFromString(@"AVMobileChromelessControlsViewController")};
        ((void (*)(struct objc_super*,SEL,id))objc_msgSendSuper)(&parent,selector,@NO);
    }];
}
static Class PV2CompactControlsClass(void) {
    Class base=NSClassFromString(@"AVMobileChromelessControlsViewController");
    SEL selector=NSSelectorFromString(@"_updateControlsVisibilityStateIfNeededAnimated:");
    if (!PV2NativeTimelineABI(base,selector,"v24@0:8@16")) return Nil;
    static Class compact=Nil; static dispatch_once_t once;
    dispatch_once(&once,^{
        compact=objc_allocateClassPair(base,"PV2PhotosCompactControlsController",0);
        if (compact && class_addMethod(compact,selector,(IMP)PV2CompactVisibility,"v24@0:8@16")) objc_registerClassPair(compact);
        else { if (compact) objc_disposeClassPair(compact); compact=Nil; }
    });
    return compact;
}
static void *PV2NativeTimelineScrubbingContext = &PV2NativeTimelineScrubbingContext;

// All calls here run on main. In particular, do not pause, seek, replace an item,
// endScrubbing manually, or restore a guessed playback rate on the Photos player.
static void PV2NativeTimelineDisconnect(UIViewController *controls, id controller,
                                       UIControl *slider) {
    // Native cancelTracking forwards sliderDidEndTracking: to the original delegate,
    // balancing native playback suspension before either binding is disconnected.
    if (slider.isTracking) [slider cancelTrackingWithEvent:nil];
    if (controls) [(id<PV2NativeTimelineMessages>)controls updateVisibilityPolicy:1 animated:NO];
    if (controls) [NSObject cancelPreviousPerformRequestsWithTarget:controls];
    if (controls) [(id<PV2NativeTimelineMessages>)controls setPlayerController:nil];
    if (controller) [(id<PV2NativeTimelineMessages>)controller setPlayer:nil];
    if (controls.isViewLoaded) [controls.view removeFromSuperview];
    if (controls.parentViewController) [controls removeFromParentViewController];
}

@interface PV2NativeTimelineViewController () {
    AVPlayer *_boundPlayer;
    PV2NativeCommandBridge *_commandBridge;
    UIViewController *_nativeControls;
    id _nativePlayerController;
    PV2NativeTimelineHostView *_timelineHost;
    __weak UIView *_nativeTimeline;
    __weak UIControl *_nativeSlider;
    __weak UIView *_nativePlayback;
    UIButton *_transportButton;
    NSString *_failureReason;
    void (^_scrubbingDidChange)(BOOL);
    BOOL _invalidated;
    BOOL _observingScrubbing;
    BOOL _observedScrubbing;
    BOOL _layingOutNative;
    BOOL _nativeDidMove;
    BOOL _setupRequested;
    BOOL _readinessCheckPending;
    NSUInteger _layoutAttempts;
    NSUInteger _bindingGeneration;
    NSMutableArray *_loopTimingObservationTokens;
    BOOL _observingLoopPlayer;
    BOOL _loopTimingPending;
    BOOL _loopRepairRetryScheduled;
    BOOL _loopSliderRepairScheduled;
    CFTimeInterval _loopRepairUntil;
    id _loopPeriodicTimeToken;
    CMTime _lastLoopObservedTime;
    __weak AVPlayerItem *_loopObservedItem;
}
- (void)pv2_fail:(NSString *)reason;
- (void)pv2_layoutNative;
- (void)pv2_discoverTimelineRequiringReady:(BOOL)required;
- (void)pv2_publishScrubbing:(BOOL)scrubbing generation:(NSUInteger)generation;
- (void)pv2_installLoopTimingObserversForItem:(AVPlayerItem *)item;
- (void)pv2_refreshLoopTimingForItem:(AVPlayerItem *)item observedTime:(CMTime)observedTime;
- (void)pv2_transportTap;
- (void)pv2_observeLoopPlayer:(AVPlayer *)player;
- (void)pv2_syncNativeSliderValueForItem:(AVPlayerItem *)item observedTime:(CMTime)observedTime generation:(NSUInteger)generation;
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
    _loopTimingObservationTokens=[NSMutableArray array];
    if (![player isKindOfClass:[AVPlayer class]]) {
        [self pv2_fail:@"A live, existing AVPlayer is required"];
        return self;
    }

    // Photos need not already have loaded AVKit. Keep the framework loaded for
    // process lifetime; dlclose while native classes/observers exist is unsafe.
    if (!NSClassFromString(@"AVMobileChromelessControlsViewController")) {
        (void)dlopen("/System/Library/Frameworks/AVKit.framework/AVKit", RTLD_NOW | RTLD_LOCAL);
    }
    Class playerClass = PV2NativeAuthorityClass();
    Class controlsClass = PV2CompactControlsClass();
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
        {playerClass, @selector(setInspectionSuspended:), "v20@0:8B16"},
        {controlsClass, @selector(init), "@16@0:8"},
        {controlsClass, @selector(setPlayerController:), "v24@0:8@16"},
        {controlsClass, @selector(setIncludedControls:), "v24@0:8Q16"},
        {controlsClass, @selector(setExcludedControls:), "v24@0:8Q16"},
        {controlsClass, @selector(setOptimizeForPerformance:), "v20@0:8B16"},
        {controlsClass, @selector(updateVisibilityPolicy:animated:), "v28@0:8Q16B24"},
        {rootClass, @selector(timelineView), "@16@0:8"},
        {rootClass, @selector(playbackControlsView), "@16@0:8"},
        {rootClass, @selector(setIgnoresTouches:), "v20@0:8B16"},
        {timelineClass, @selector(setLabelPosition:), "v24@0:8Q16"},
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
        [self pv2_installLoopTimingObserversForItem:player.currentItem];
        _commandBridge=[PV2NativeCommandBridge new];
        _commandBridge.valid=YES; _commandBridge.controller=_nativePlayerController; _commandBridge.player=player;
        objc_setAssociatedObject(_nativePlayerController,PV2NativeCommandKey,_commandBridge,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        objc_setAssociatedObject(_nativePlayerController,PV2NativeTimelineOwnerKey,self,OBJC_ASSOCIATION_ASSIGN);
        // initWithPlayer: defaults inspectionSuspended=YES. This native setter
        // starts AVKit KVO, current-item timing, timebase notifications and
        // subsequent interpolation without any custom refresh timer.
        [(id<PV2NativeTimelineMessages>)_nativePlayerController setInspectionSuspended:NO];
        id<PV2NativeTimelineMessages> controls = (id)_nativeControls;
        [controls setOptimizeForPerformance:YES]; // defer setup until the actual root is in a window
        [controls setExcludedControls:6]; // exclude native backward/forward; playback view is hidden below.
        [controls setIncludedControls:(1ULL | 8ULL)]; // retain native object for state/ABI, hide its visible button.
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

- (void)pv2_syncNativeSliderValueForItem:(AVPlayerItem *)item observedTime:(CMTime)observedTime generation:(NSUInteger)generation {
    if (_invalidated || generation!=_bindingGeneration || !item || item!=_boundPlayer.currentItem || !_nativeSlider || !_nativePlayerController) return;
    SEL currentSel=NSSelectorFromString(@"currentTime");
    SEL minSel=NSSelectorFromString(@"minTime");
    SEL maxSel=NSSelectorFromString(@"maxTime");
    if (!PV2NativeTimelineABI([_nativePlayerController class],currentSel,"d16@0:8") ||
        !PV2NativeTimelineABI([_nativePlayerController class],minSel,"d16@0:8") ||
        !PV2NativeTimelineABI([_nativePlayerController class],maxSel,"d16@0:8") ||
        !PV2NativeTimelineABI([_nativeSlider class],@selector(setValue:animated:),"v24@0:8f16B20")) return;
    double current=((double (*)(id,SEL))objc_msgSend)(_nativePlayerController,currentSel);
    double low=((double (*)(id,SEL))objc_msgSend)(_nativePlayerController,minSel);
    double high=((double (*)(id,SEL))objc_msgSend)(_nativePlayerController,maxSel);
    CMTimeRange liveRange=PV2NativePhotosLoopRange(_nativePlayerController);
    if (CMTIMERANGE_IS_VALID(liveRange)) {
        low=CMTimeGetSeconds(liveRange.start);
        high=CMTimeGetSeconds(CMTimeRangeGetEnd(liveRange));
        if (CMTIME_IS_NUMERIC(observedTime)) current=CMTimeGetSeconds(observedTime);
        else if (CMTIME_IS_NUMERIC(item.currentTime)) current=CMTimeGetSeconds(item.currentTime);
        current=PV2FoldLoopSeconds(current,low,high);
    }
    if (isfinite(current) && isfinite(low) && isfinite(high) && high>low) {
        float value=(float)MAX(0,MIN(1,(current-low)/(high-low)));
        ((void (*)(id,SEL,float,BOOL))objc_msgSend)(_nativeSlider,@selector(setValue:animated:),value,NO);
    }
}

- (void)pv2_refreshLoopTimingForItem:(AVPlayerItem *)item observedTime:(CMTime)observedTime {
    if (_invalidated || !item || item!=_boundPlayer.currentItem || !_nativePlayerController) return;
    double rate=_boundPlayer.rate;
    if (rate<=0 ||
        ([_nativePlayerController respondsToSelector:@selector(isScrubbing)] && ((BOOL (*)(id,SEL))objc_msgSend)(_nativePlayerController,NSSelectorFromString(@"isScrubbing"))) ||
        ([_nativePlayerController respondsToSelector:@selector(isSeeking)] && ((BOOL (*)(id,SEL))objc_msgSend)(_nativePlayerController,NSSelectorFromString(@"isSeeking"))) ||
        _commandBridge.suspensionDepth>0) {
        _loopTimingPending=YES;
        if (!_loopRepairRetryScheduled) {
            _loopRepairRetryScheduled=YES;
            __weak PV2NativeTimelineViewController *weakSelf=self;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(0.05*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
                PV2NativeTimelineViewController *strongSelf=weakSelf;
                if (!strongSelf || strongSelf->_invalidated) return;
                strongSelf->_loopRepairRetryScheduled=NO;
                if (strongSelf->_loopTimingPending)
                    [strongSelf pv2_refreshLoopTimingForItem:item observedTime:observedTime];
            });
        }
        return;
    }
    if (!_loopTimingPending) return;
    _loopTimingPending=NO;
    SEL minmax=NSSelectorFromString(@"updateMinMaxTiming");
    SEL timing=NSSelectorFromString(@"updateTiming");
    if (PV2NativeTimelineABI([_nativePlayerController class],minmax,"v16@0:8"))
        ((void (*)(id,SEL))objc_msgSend)(_nativePlayerController,minmax);
    if (PV2NativeTimelineABI([_nativePlayerController class],timing,"v16@0:8"))
        ((void (*)(id,SEL))objc_msgSend)(_nativePlayerController,timing);
    NSUInteger generation=_bindingGeneration;
    if (!CMTIME_IS_NUMERIC(observedTime)) return;
    [self pv2_syncNativeSliderValueForItem:item observedTime:observedTime generation:generation];
    __weak PV2NativeTimelineViewController *weakSelf=self;
    for (NSNumber *delayNumber in @[@0.02,@0.12,@0.35]) {
        NSTimeInterval delay=delayNumber.doubleValue;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(delay*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
            PV2NativeTimelineViewController *strongSelf=weakSelf;
            if (strongSelf && !strongSelf->_invalidated && generation==strongSelf->_bindingGeneration && strongSelf->_boundPlayer.rate>0) {
                CMTime latest=strongSelf->_lastLoopObservedTime;
                [strongSelf pv2_syncNativeSliderValueForItem:item observedTime:latest generation:generation];
            }
        });
    }
}
- (void)pv2_observeLoopPlayer:(AVPlayer *)player {
    if (_invalidated || !player || _observingLoopPlayer) return;
    _observingLoopPlayer=YES;
    @try {
        [player addObserver:self forKeyPath:@"rate" options:NSKeyValueObservingOptionNew context:PV2NativeTimelineScrubbingContext];
        [player addObserver:self forKeyPath:@"timeControlStatus" options:NSKeyValueObservingOptionNew context:PV2NativeTimelineScrubbingContext];
        __weak PV2NativeTimelineViewController *weakSelf=self;
        _loopPeriodicTimeToken=[player addPeriodicTimeObserverForInterval:CMTimeMakeWithSeconds(.25,600) queue:dispatch_get_main_queue() usingBlock:^(CMTime time){
            PV2NativeTimelineViewController *s=weakSelf; if (!s || s->_invalidated) return;
            AVPlayerItem *activeItem=s->_boundPlayer.currentItem;
            if (s->_loopObservedItem!=activeItem) {
                s->_loopObservedItem=activeItem;
                s->_lastLoopObservedTime=kCMTimeInvalid;
                s->_loopTimingPending=NO;
                s->_loopRepairUntil=0;
                s->_loopRepairRetryScheduled=NO;
                s->_loopSliderRepairScheduled=NO;
                return;
            }
            CMTimeRange range=PV2NativePhotosLoopRange(s->_nativePlayerController);
            double previousRaw=CMTimeGetSeconds(s->_lastLoopObservedTime), currentRaw=CMTimeGetSeconds(time), duration=CMTimeGetSeconds(range.duration);
            double low=CMTimeGetSeconds(range.start), high=CMTimeGetSeconds(CMTimeRangeGetEnd(range));
            double current=PV2FoldLoopSeconds(currentRaw,low,high);
            double previous=PV2FoldLoopSeconds(previousRaw,low,high);
            s->_lastLoopObservedTime=CMTimeMakeWithSeconds(current,600);
            BOOL playing=s->_boundPlayer.rate>0;
            BOOL scrubbing=([s->_nativePlayerController respondsToSelector:@selector(isScrubbing)] && ((BOOL (*)(id,SEL))objc_msgSend)(s->_nativePlayerController,NSSelectorFromString(@"isScrubbing")));
            BOOL seeking=([s->_nativePlayerController respondsToSelector:@selector(isSeeking)] && ((BOOL (*)(id,SEL))objc_msgSend)(s->_nativePlayerController,NSSelectorFromString(@"isSeeking")));
            BOOL mismatch=NO;
            if ([s->_nativeSlider respondsToSelector:@selector(value)] && isfinite(current) && isfinite(duration) && duration>0) {
                float slider=((float (*)(id,SEL))objc_msgSend)(s->_nativeSlider,@selector(value));
                mismatch=fabs(slider-(float)((current-low)/duration))>.08;
            }
            if (playing && !scrubbing && !seeking && mismatch && !s->_loopSliderRepairScheduled) {
                s->_loopSliderRepairScheduled=YES;
                NSUInteger repairGeneration=s->_bindingGeneration;
                AVPlayerItem *expectedItem=s->_boundPlayer.currentItem;
                __weak PV2NativeTimelineViewController *weakOwner=s;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(0.08*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
                    PV2NativeTimelineViewController *owner=weakOwner;
                    if (!owner || owner->_invalidated) return;
                    if (owner->_bindingGeneration!=repairGeneration || owner->_boundPlayer.currentItem!=expectedItem) {
                        owner->_loopSliderRepairScheduled=NO;
                        return;
                    }
                    owner->_loopSliderRepairScheduled=NO;
                    CMTimeRange latestRange=PV2NativePhotosLoopRange(owner->_nativePlayerController);
                    double latest=CMTimeGetSeconds(owner->_boundPlayer.currentTime);
                    double latestLow=CMTimeGetSeconds(latestRange.start), latestHigh=CMTimeGetSeconds(CMTimeRangeGetEnd(latestRange));
                    latest=PV2FoldLoopSeconds(latest,latestLow,latestHigh);
                    if (owner->_boundPlayer.rate>0 && isfinite(latest))
                        [owner pv2_syncNativeSliderValueForItem:owner->_boundPlayer.currentItem observedTime:CMTimeMakeWithSeconds(latest,600) generation:owner->_bindingGeneration];
                });
            }
            if (isfinite(previous) && isfinite(current) && isfinite(duration) && duration>0 && previous>duration*.72 && current<duration*.28) {
                s->_loopRepairUntil=CACurrentMediaTime()+0.9;
                s->_loopTimingPending=YES;
                [s pv2_refreshLoopTimingForItem:s->_boundPlayer.currentItem observedTime:time];
            }
        }];
    } @catch (__unused NSException *e) { _observingLoopPlayer=NO; }
}
- (void)pv2_installLoopTimingObserversForItem:(AVPlayerItem *)item {
    if (_invalidated || !item) return;
    [self pv2_observeLoopPlayer:_boundPlayer];
    NSNotificationCenter *center=NSNotificationCenter.defaultCenter;
    __weak PV2NativeTimelineViewController *weakSelf=self;
    for (NSString *name in @[AVPlayerItemTimeJumpedNotification,AVPlayerItemDidPlayToEndTimeNotification]) {
        id token=[center addObserverForName:name object:item queue:NSOperationQueue.mainQueue usingBlock:^(__unused NSNotification *note){
            PV2NativeTimelineViewController *strongSelf=weakSelf;
            if (strongSelf) {
                strongSelf->_loopTimingPending=YES;
                [strongSelf pv2_refreshLoopTimingForItem:item observedTime:kCMTimeInvalid];
            }
        }];
        if (token) [_loopTimingObservationTokens addObject:token];
    }
}
- (void)pv2_clearLoopTimingObservers {
    NSNotificationCenter *center=NSNotificationCenter.defaultCenter;
    for (id token in [_loopTimingObservationTokens copy]) [center removeObserver:token];
    [_loopTimingObservationTokens removeAllObjects];
    if (_observingLoopPlayer && _boundPlayer) {
        @try {
            [_boundPlayer removeObserver:self forKeyPath:@"rate" context:PV2NativeTimelineScrubbingContext];
            [_boundPlayer removeObserver:self forKeyPath:@"timeControlStatus" context:PV2NativeTimelineScrubbingContext];
        } @catch (__unused NSException *e) {}
    }
    _loopTimingPending=NO;
    _loopRepairRetryScheduled=NO;
    _loopSliderRepairScheduled=NO;
    _loopRepairUntil=0;
    _loopObservedItem=nil;
    if (_loopPeriodicTimeToken && _boundPlayer) {
        [_boundPlayer removeTimeObserver:_loopPeriodicTimeToken];
        _loopPeriodicTimeToken=nil;
    }
}

- (void)pv2_fail:(NSString *)reason { if (!_failureReason) _failureReason = [reason copy]; }

- (void)loadView {
    _timelineHost = [[PV2NativeTimelineHostView alloc] initWithFrame:CGRectZero];
    _timelineHost.backgroundColor = UIColor.clearColor;
    __weak PV2NativeTimelineViewController *weakSelf = self;
    _timelineHost.windowDidChange = ^{ [weakSelf pv2_layoutNative]; };
    self.view = _timelineHost;
    _transportButton=[UIButton buttonWithType:UIButtonTypeSystem];
    _transportButton.tintColor=UIColor.whiteColor;
    _transportButton.accessibilityTraits=UIAccessibilityTraitButton;
    [_transportButton addTarget:self action:@selector(pv2_transportTap) forControlEvents:UIControlEventTouchUpInside];
    [_timelineHost addSubview:_transportButton];
}
- (void)viewDidAppear:(BOOL)animated { [super viewDidAppear:animated]; [self pv2_layoutNative]; }
- (void)viewDidLayoutSubviews { [super viewDidLayoutSubviews]; [self pv2_layoutNative]; }

- (void)pv2_transportTap {
    if (_invalidated || !_commandBridge.toggle) return;
    _commandBridge.toggle();
    [self refreshPlaybackIntent];
}

- (void)pv2_layoutNative {
    if (_invalidated || !_nativeControls || _layingOutNative) return;
    _layingOutNative = YES;
    UIView *root = _nativeControls.view;
    if (!root) { _layingOutNative = NO; return; }
    if (!PV2ConfigureCompactNativeRoot(root)) {
        [self pv2_fail:@"Cannot apply compact native layout policy"];
        _layingOutNative=NO; return;
    }
    if (!CGRectEqualToRect(root.frame,_timelineHost.bounds)) root.frame = _timelineHost.bounds;
    root.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    if (_nativeControls.parentViewController != self)
        [self addChildViewController:_nativeControls];
    if (root.superview != _timelineHost) {
        [_timelineHost addSubview:root];
        [(id<PV2NativeTimelineMessages>)root setIgnoresTouches:NO];
    }
    if (!_nativeDidMove) { [_nativeControls didMoveToParentViewController:self]; _nativeDidMove = YES; }
    if (root.window && !_setupRequested) {
        _setupRequested=YES;
        [(id<PV2NativeTimelineMessages>)_nativeControls setOptimizeForPerformance:NO];
    }
    [root layoutIfNeeded];
    [self pv2_discoverTimelineRequiringReady:NO];
    if (_transportButton && _nativeTimeline) {
        BOOL landscape=_timelineHost.window && CGRectGetWidth(_timelineHost.window.bounds)>CGRectGetHeight(_timelineHost.window.bounds);
        CGFloat gap=landscape?58:50;
        CGFloat size=46;
        CGFloat y=MAX(0,CGRectGetMinY(_nativeTimeline.frame)-gap);
        _transportButton.frame=CGRectMake(CGRectGetWidth(_timelineHost.bounds)-size-8,y,size,size);
        [_timelineHost bringSubviewToFront:_transportButton];
    }
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
        // A fullscreen gradient is not part of the requested compact strip.
        // Leave native timing/controls intact; suppress only that background.
        SEL backgroundSelector=NSSelectorFromString(@"backgroundView");
        if (PV2NativeTimelineABI([root class],backgroundSelector,"@16@0:8")) {
            UIView *background=((id (*)(id,SEL))objc_msgSend)(root,backgroundSelector);
            background.hidden=YES; background.accessibilityElementsHidden=YES;
        }
        BOOL discovered=_nativeTimeline!=timeline || _nativeSlider!=slider || _nativePlayback!=playback;
        _nativeTimeline = timeline; _nativeSlider = slider; _nativePlayback = playback;
        _timelineHost.permittedTimeline = timeline;
        _timelineHost.permittedPlayback = playback;
        _readinessCheckPending = NO;
        _layoutAttempts = 0;
        // Getter/discovery must not schedule another native visibility update.
        // That update itself lays out this hierarchy; doing this every pass
        // re-arms the same CFRunLoop observer indefinitely (device watchdog).
        if (discovered) {
            [(id<PV2NativeTimelineMessages>)_nativeControls updateVisibilityPolicy:0 animated:NO];
            if (playback) {
                PV2NativeConfigurePlaybackIntent(playback,_commandBridge);
                [self refreshPlaybackIntent];
            }
            if (_controlsDidBecomeReady && root.window) _controlsDidBecomeReady();
        }
        return;
    }
    // Native setup is naturally window-driven and may complete after several
    // run-loop turns. Give it three deferred opportunities, with no recursion.
    if (!_readinessCheckPending && _timelineHost.window && _layoutAttempts < 3) {
        _readinessCheckPending = YES; _layoutAttempts++; __weak PV2NativeTimelineViewController *weakSelf = self;
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
    if (_invalidated) return;
    if (object==_boundPlayer && ([keyPath isEqualToString:@"rate"] || [keyPath isEqualToString:@"timeControlStatus"])) {
        __weak PV2NativeTimelineViewController *weakSelf=self;
        dispatch_async(dispatch_get_main_queue(),^{
            PV2NativeTimelineViewController *strongSelf=weakSelf;
            if (strongSelf && !strongSelf->_invalidated && strongSelf->_loopTimingPending)
                [strongSelf pv2_refreshLoopTimingForItem:strongSelf->_boundPlayer.currentItem observedTime:kCMTimeInvalid];
        });
        return;
    }
    if (object != _nativePlayerController || ![keyPath isEqualToString:@"scrubbing"]) return;
    NSNumber *newValue = change[NSKeyValueChangeNewKey];
    BOOL value = newValue ? newValue.boolValue : NO;
    NSUInteger generation = _bindingGeneration;
    [self pv2_publishScrubbing:value generation:generation];
}
- (void)pv2_publishScrubbing:(BOOL)value generation:(NSUInteger)generation {
    if (!_scrubbingDidChange || _invalidated || generation != _bindingGeneration) return;
    void (^notify)(BOOL) = [_scrubbingDidChange copy]; if (!notify) return;
    if ([NSThread isMainThread]) notify(value);
    else { __weak PV2NativeTimelineViewController *weakSelf = self; dispatch_async(dispatch_get_main_queue(), ^{
        PV2NativeTimelineViewController *strongSelf = weakSelf;
        if (strongSelf && !strongSelf->_invalidated && generation == strongSelf->_bindingGeneration) notify(value);
    }); }
}

- (void)setPlaybackToggleRequested:(void (^)(void))handler { _commandBridge.toggle=[handler copy]; }
- (void (^)(void))playbackToggleRequested { return _commandBridge.toggle; }
- (void)setPlaybackIntent:(NSInteger (^)(void))handler { _commandBridge.intent=[handler copy]; [self refreshPlaybackIntent]; }
- (NSInteger (^)(void))playbackIntent { return _commandBridge.intent; }
- (void)refreshPlaybackIntent {
    if (_invalidated || !_commandBridge.intent) return;
    NSInteger intent=_commandBridge.intent();
    if (_transportButton) {
        UIImageSymbolConfiguration *config=[UIImageSymbolConfiguration configurationWithPointSize:20 weight:UIImageSymbolWeightSemibold];
        [_transportButton setImage:[UIImage systemImageNamed:(intent==1?@"pause.fill":@"play.fill") withConfiguration:config] forState:UIControlStateNormal];
        _transportButton.accessibilityLabel=(intent==1?@"暂停":@"播放");
    }
    PV2NativeReconcileRestoreIntent(_commandBridge);
    if (!_nativePlayback) return;
    if (intent<0) return;
    SEL getter=NSSelectorFromString(@"playPauseButtonShowsPlayIcon");
    SEL setter=NSSelectorFromString(@"setPlayPauseButtonShowsPlayIcon:");
    if (!PV2NativeTimelineABI([_nativePlayback class],getter,"B16@0:8") ||
        !PV2NativeTimelineABI([_nativePlayback class],setter,"v20@0:8B16")) return;
    BOOL expected=intent==0;
    if (((BOOL (*)(id,SEL))objc_msgSend)(_nativePlayback,getter)!=expected)
        ((void (*)(id,SEL,BOOL))objc_msgSend)(_nativePlayback,setter,expected);
}
- (void)setPlaybackRateRequested:(void (^)(double,BOOL))handler { _commandBridge.rate=[handler copy]; }
- (void (^)(double,BOOL))playbackRateRequested { return _commandBridge.rate; }
- (void)setSeekRequested:(void (^)(CMTime,CMTime,CMTime,void (^)(BOOL)))handler { _commandBridge.seek=[handler copy]; }
- (void (^)(CMTime,CMTime,CMTime,void (^)(BOOL)))seekRequested { return _commandBridge.seek; }
- (void)invalidate {
    if (![NSThread isMainThread]) { __weak PV2NativeTimelineViewController *weakSelf = self; dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf invalidate]; }); return; }
    if (_invalidated) return; _invalidated = YES; _bindingGeneration++; _readinessCheckPending = NO;
    [self pv2_clearLoopTimingObservers];
    if (_observingScrubbing && _nativePlayerController) {
        [_nativePlayerController removeObserver:self forKeyPath:@"scrubbing" context:PV2NativeTimelineScrubbingContext];
        _observingScrubbing = NO;
    }
    if (_nativeDidMove) {
        [_nativeControls willMoveToParentViewController:nil];
        _nativeDidMove = NO;
    }
    PV2NativeTimelineDisconnect(_nativeControls, _nativePlayerController, _nativeSlider);
    _commandBridge.valid=NO; _commandBridge.seekGeneration++; _commandBridge.rate=nil; _commandBridge.seek=nil;
    _commandBridge.toggle=nil; _commandBridge.intent=nil; _commandBridge.hasPendingSeek=NO;
    _commandBridge=nil;
    objc_setAssociatedObject(_nativePlayerController,PV2NativeTimelineOwnerKey,nil,OBJC_ASSOCIATION_ASSIGN);
    _timelineHost.windowDidChange = nil; _timelineHost.permittedTimeline = nil; _timelineHost.permittedPlayback = nil;
    [_transportButton removeFromSuperview]; _transportButton=nil;
    _nativeTimeline = nil; _nativeSlider = nil; _nativePlayback = nil; _nativeControls = nil; _nativePlayerController = nil;
    _boundPlayer = nil; _scrubbingDidChange = nil; _controlsDidBecomeReady=nil;
}
- (void)dealloc {
    [self pv2_clearLoopTimingObservers];
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
