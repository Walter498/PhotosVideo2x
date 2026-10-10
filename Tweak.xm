#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import "RateController.h"

// Photos tiles are NSObject controllers, not UIViewController subclasses.
@interface PUTileController : NSObject
- (BOOL)isActive;
- (BOOL)isPresentationActive;
- (UIView *)tilingView;
@end
@interface PUTileViewController : PUTileController
- (UIView *)view;
- (UIView *)loadView;
- (NSArray *)gestureRecognizers;
@end
@interface PUVideoTileViewController : PUTileViewController
- (id)_browsingVideoPlayer;
- (id)videoSession;
@end
@interface PUOneUpViewController : UIViewController
- (id)_currentContentTileController;
- (void)viewWillDisappear:(BOOL)animated;
- (void)viewDidDisappear:(BOOL)animated;
@end
@interface PUOneUpBarsController : NSObject
- (id)viewController;
@end
@interface ISWrappedAVPlayer : NSObject <PV2RatePlayer>
- (float)rate;
- (void)setRate:(float)rate;
@end
@interface NSObject (PV2KnownGetters)
- (id)videoSession;
- (id)videoPlayer;
@end

static void PV2Log(NSString *message) {
    NSLog(@"[PhotosVideo2x] %@", message);
    // Use the actual Photos container; RootHide /var/mobile is a different view.
    NSString *base = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
    NSString *path = [base stringByAppendingPathComponent:@"PhotosVideo2x.log"];
    NSData *data = [[NSString stringWithFormat:@"[%@] %@\n", NSDate.date, message] dataUsingEncoding:NSUTF8StringEncoding];
    @try {
        if (![[NSFileManager defaultManager] fileExistsAtPath:path])
            [[NSFileManager defaultManager] createFileAtPath:path contents:nil attributes:nil];
        NSFileHandle *handle = [NSFileHandle fileHandleForWritingAtPath:path];
        [handle seekToEndOfFile]; [handle writeData:data]; [handle closeFile];
    } @catch (__unused NSException *e) {}
}

#import "DownloadController.h"

static id<PV2RatePlayer> PV2Wrapper(PUVideoTileViewController *tile) {
    if (!tile || ![tile isActive]) return nil;
    @try {
        id browsing = [tile _browsingVideoPlayer];
        id session = [browsing respondsToSelector:@selector(videoSession)] ? [browsing videoSession] : [tile videoSession];
        id wrapper = [session respondsToSelector:@selector(videoPlayer)] ? [session videoPlayer] : nil;
        return [wrapper isKindOfClass:NSClassFromString(@"ISWrappedAVPlayer")] ? wrapper : nil;
    } @catch (__unused NSException *e) { return nil; }
}

#import "VideoTools.h"
#import "TimelineController.h"
#import "SeekFeedback.h"

@interface PUDoubleTapZoomController : NSObject
- (id)delegate;
- (void)_handleDoubleTapGestureRecognizer:(UIGestureRecognizer *)gesture;
@end
@interface NSObject (PV2DoubleTapNative)
- (id)_doubleTapZoomController;
@end

@class PV2GestureTarget;

@interface PV2OverlayView : UIVisualEffectView
@property(nonatomic, strong) NSLayoutConstraint *topConstraint;
@end

@implementation PV2OverlayView
- (instancetype)init {
    UIBlurEffect *effect = [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemChromeMaterialDark];
    if ((self = [super initWithEffect:effect])) {
        self.userInteractionEnabled = NO;
        self.layer.cornerRadius = 16.0;
        self.layer.masksToBounds = YES;
        self.alpha = 0.0;
        self.isAccessibilityElement = YES;
        self.accessibilityLabel = @"2x";
        UILabel *label = [UILabel new];
        label.text = @"2x";
        label.textColor = UIColor.whiteColor;
        label.font = [UIFont systemFontOfSize:17.0 weight:UIFontWeightSemibold];
        UIImageSymbolConfiguration *config = [UIImageSymbolConfiguration configurationWithPointSize:12.0 weight:UIImageSymbolWeightSemibold];
        UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"forward.fill" withConfiguration:config]];
        icon.tintColor = [UIColor.whiteColor colorWithAlphaComponent:0.8];
        icon.contentMode = UIViewContentModeScaleAspectFit;
        UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[label, icon]];
        stack.spacing = 5.0; stack.alignment = UIStackViewAlignmentCenter;
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        [self.contentView addSubview:stack];
        [NSLayoutConstraint activateConstraints:@[
            [stack.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor constant:13.0],
            [stack.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor constant:-13.0],
            [stack.centerYAnchor constraintEqualToAnchor:self.contentView.centerYAnchor],
            [icon.widthAnchor constraintEqualToConstant:16.0],
            [icon.heightAnchor constraintEqualToConstant:14.0]
        ]];
    }
    return self;
}
@end

static void PV2ShowOverlay(PV2GestureTarget *target, UIView *host);
static void PV2HideOverlay(PV2GestureTarget *target);

@interface PV2GestureTarget : NSObject <UIGestureRecognizerDelegate>
@property(nonatomic, weak) PUVideoTileViewController *tile;
@property(nonatomic, strong) UILongPressGestureRecognizer *recognizer;
@property(nonatomic, strong) id<PV2RatePlayer> heldWrapper;
@property(nonatomic, strong) PV2RateToken *token;
@property(nonatomic, strong) PV2OverlayView *overlay;
- (void)handle:(UILongPressGestureRecognizer *)gesture;
@end
static const void *PV2GestureKey = &PV2GestureKey;
static void PV2HideOverlay(PV2GestureTarget *target) {
    if (!NSThread.isMainThread) {
        __weak PV2GestureTarget *weakTarget = target;
        dispatch_async(dispatch_get_main_queue(), ^{ PV2HideOverlay(weakTarget); });
        return;
    }
    [target.overlay.layer removeAllAnimations];
    [target.overlay removeFromSuperview];
    target.overlay = nil;
}

static void PV2ShowOverlay(PV2GestureTarget *target, UIView *host) {
    if (!host.window || !NSThread.isMainThread || !PV2TileIsSelected(target.tile)) return;
    [PV2ToolsForOwner(PV2VisibleOneUp,YES) updateSpeedDisplay];
    UIImpactFeedbackGenerator *feedback = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    [feedback prepare]; [feedback impactOccurred];
}


static BOOL PV2AcceptPoint(PUVideoTileViewController *tile, CGPoint point, UIView *host) {
    if (!host || !tile || !PV2TileIsSelected(tile) || ![tile isActive]) return NO;
    UIView *content = [tile view];
    if (!content.window || content.window != host.window) return NO;
    CGPoint local = [content convertPoint:point fromView:host];
    if (!CGRectContainsPoint(content.bounds, local)) return NO;
    CGRect rect = host.bounds;
    if (!CGRectContainsPoint(rect, point)) return NO;
    CGFloat x = point.x - CGRectGetMinX(rect), width = CGRectGetWidth(rect);
    return width > 1 && (x <= width * 0.25 || x >= width * 0.75);
}

@implementation PV2GestureTarget
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gesture shouldReceiveTouch:(UITouch *)touch {
    for (UIView *v = touch.view; v; v = v.superview) {
        if ([v isKindOfClass:UIControl.class]) return NO;
        NSString *name = NSStringFromClass(v.class);
        if ([name containsString:@"Scrubber"] || [name containsString:@"Button"]) return NO;
        if (v == gesture.view) break;
    }
    return PV2AcceptPoint(self.tile, [touch locationInView:gesture.view], gesture.view);
}
- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer *)gesture {
    if (!PV2AcceptPoint(self.tile, [gesture locationInView:gesture.view], gesture.view)) return NO;
    id<PV2RatePlayer> wrapper = PV2Wrapper(self.tile);
    return wrapper && isfinite([wrapper rate]) && [wrapper rate] > 0;
}
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)a shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)b {
    // Only coexist with Photos' contact observer; paging/zoom retain their rules.
    return [b isKindOfClass:NSClassFromString(@"PUTouchingGestureRecognizer")];
}
- (void)handle:(UILongPressGestureRecognizer *)gesture {
    if (gesture.state == UIGestureRecognizerStateBegan) {
        id<PV2RatePlayer> wrapper = PV2Wrapper(self.tile);
        self.token = PV2RateBegin(self, wrapper);
        if (self.token) {
            self.heldWrapper = wrapper;
            PV2ShowOverlay(self, gesture.view);
            PV2Log([NSString stringWithFormat:@"begin wrapper=%p", wrapper]);
        }
    } else if (gesture.state == UIGestureRecognizerStateChanged) {
        if (self.heldWrapper && PV2Wrapper(self.tile) != self.heldWrapper) {
            PV2HideOverlay(self);
            PV2RateEnd(self.token, NO); self.token = nil; self.heldWrapper = nil;
        }
    } else if (gesture.state == UIGestureRecognizerStateEnded ||
               gesture.state == UIGestureRecognizerStateCancelled ||
               gesture.state == UIGestureRecognizerStateFailed) {
        PV2HideOverlay(self);
        PV2RateEnd(self.token, YES); self.token = nil; self.heldWrapper = nil;
        PV2Log(@"end");
    }
}
@end

static PV2GestureTarget *PV2Target(PUVideoTileViewController *tile, BOOL create) {
    PV2GestureTarget *target = objc_getAssociatedObject(tile, PV2GestureKey);
    if (!target && create) {
        target = [PV2GestureTarget new]; target.tile = tile;
        UILongPressGestureRecognizer *gesture = [[UILongPressGestureRecognizer alloc] initWithTarget:target action:@selector(handle:)];
        gesture.minimumPressDuration = 0.35; gesture.allowableMovement = 18;
        gesture.numberOfTouchesRequired = 1; gesture.cancelsTouchesInView = NO;
        gesture.delaysTouchesBegan = NO; gesture.delaysTouchesEnded = NO;
        gesture.delegate = target; target.recognizer = gesture;
        objc_setAssociatedObject(tile, PV2GestureKey, target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return target;
}
static void PV2EndTile(PUVideoTileViewController *tile, BOOL restore) {
    PV2GestureTarget *target = PV2Target(tile, NO);
    if (target) {
        PV2HideOverlay(target);
        PV2RateEnd(target.token, restore);
        target.token = nil; target.heldWrapper = nil;
    }
}

%group PV2Hooks
%hook PUVideoTileViewController
- (NSArray *)gestureRecognizers {
    NSArray *original = %orig;
    PV2GestureTarget *target = PV2Target(self, YES);
    // Native addToTilingView installs these on the interactive outer host.
    if ([original containsObject:target.recognizer]) return original;
    return [(original ?: @[]) arrayByAddingObject:target.recognizer];
}
- (void)didChangeActive {
    %orig;
    if (![self isActive]) PV2EndTile(self, NO);
    PV2DownloadRefresh(self);
}
- (void)setTilingView:(id)view {
    if ([self tilingView] != view) PV2EndTile(self, NO);
    %orig(view);
    PV2DownloadRefresh(self);
}
- (void)setVideoSession:(id)session {
    if ([self videoSession] != session) PV2EndTile(self, NO);
    %orig(session);
    PV2DownloadRefresh(self);
}
- (void)_setBrowsingVideoPlayer:(id)player {
    if ([self _browsingVideoPlayer] != player) PV2EndTile(self, NO);
    %orig(player);
    PV2DownloadRefresh(self);
}
- (void)becomeReusable {
    PV2EndTile(self, NO);
    PV2DownloadDetach(self);
    %orig;
}
%end
%end
%group PV2DownloadHooks
%hook PXVideoContentProvider
- (void)setLoadingProgress:(double)progress {
    %orig(progress);
    PV2DownloadEvent(self, NO);
}
- (void)setLoadingResult:(id)result {
    %orig(result);
    PV2DownloadEvent(self, YES);
}
%end
%end
%group PV2OneUpHooks
%hook PUOneUpViewController
- (void)viewDidLayoutSubviews {
    %orig;
    PV2TimelineRefresh(self);
}
- (void)viewDidAppear:(BOOL)animated {
    %orig(animated);
    PV2DownloadEnter(self);
    PV2ToolsRefresh(self);
    PV2TimelineRefresh(self);
}
- (void)_updateVideoPlayerIfNeeded {
    %orig;
    PV2DownloadRefreshOneUp(self);
    PV2ToolsRefresh(self);
    PV2TimelineRefresh(self);
}
- (void)_updateViewModelWithCurrentScrollPosition {
    %orig;
    PV2DownloadRefreshOneUp(self);
    PV2ToolsRefresh(self);
    PV2TimelineRefresh(self);
}
- (void)viewWillDisappear:(BOOL)animated {
    PV2TimelineLeave(self);
    PV2SeekFeedbackLeave(self);
    PV2ToolsLeave(self);
    PV2DownloadLeave(self);
    %orig(animated);
}
- (void)viewDidDisappear:(BOOL)animated {
    PV2TimelineLeave(self);
    PV2SeekFeedbackLeave(self);
    PV2ToolsLeave(self);
    PV2DownloadLeave(self);
    %orig(animated);
}
%end
%hook PUOneUpBarsController
- (void)_updateChromeVisibilityIfNeeded {
    %orig;
    if ([self viewController] == PV2VisibleOneUp) {
        PV2DownloadRefreshOneUp(PV2VisibleOneUp);
        PV2TimelineRefresh(PV2VisibleOneUp);
    }
}
%end
%end
%group PV2LoopHooks
%hook PXVideoSession
- (void)setLoopingEnabled:(BOOL)enabled {
    %orig(PV2LoopEffectiveValue(self,enabled));
}
%end
%end

%group PV2DoubleTapHooks
%hook PUDoubleTapZoomController
- (void)_handleDoubleTapGestureRecognizer:(UIGestureRecognizer *)gesture {
    UIViewController *owner=PV2VisibleOneUp;
    id asset=PV2SelectedAsset();
    if (owner && self==[(id)owner _doubleTapZoomController] && [asset isKindOfClass:PHAsset.class]) {
        // Keep Photos' original double-tap recognizer and its control/edit exclusions.
        // Do not replace delegates, single-tap handling, pinching or page swipes.
        if (((PHAsset *)asset).mediaType==PHAssetMediaTypeVideo) {
            if (gesture.state==UIGestureRecognizerStateEnded) {
                CGPoint point=[gesture locationInView:owner.view];
                UIView *hit=[owner.view hitTest:point withEvent:nil];
                for (UIView *v=hit; v && v!=owner.view; v=v.superview) {
                    if ([v isKindOfClass:UIControl.class]) return;
                }
                UIToolbar *toolbar=owner.navigationController.toolbar;
                CGFloat bottom=owner.view.bounds.size.height-owner.view.safeAreaInsets.bottom;
                if (toolbar.window && !toolbar.hidden) bottom=CGRectGetMinY([toolbar convertRect:toolbar.bounds toView:owner.view]);
                CGFloat top=owner.view.safeAreaInsets.top+([owner pu_wantsNavigationBarVisible] ? 44 : 0);
                if (point.y>=top && point.y<bottom) {
                    double delta=PV2DoubleTapDelta(point.x,owner.view.bounds.size.width);
                    if (delta && PV2TimelineSeekBy(owner,delta)) PV2ShowSeekFeedback(owner,delta);
                }
            }
            return;
        }
        if (((PHAsset *)asset).mediaType==PHAssetMediaTypeImage) return;
    }
    %orig(gesture);
}
%end
%end

%group PV2ToolsHooks
%hook PUBarButtonItemCollection
- (NSArray *)orderedBarButtonsItemsForIdentifiers:(id)identifiers {
    NSArray *original = %orig(identifiers);
    return PV2ToolsInsertItems(self,original);
}
%end
%end
%group PV2Hooks
%hook ISWrappedAVPlayer
- (void)setRate:(float)rate {
    PV2RateAroundSet(self, rate, ^(float effective) { %orig(effective); });
}
- (void)pause {
    PV2RateAroundPause(self, ^{
        %orig;
    });
}
- (void)replaceCurrentItemWithPlayerItem:(id)item {
    PV2RateAroundPause(self, ^{ %orig(item); });
}
- (void)replaceCurrentItemWithPlayerItem:(id)item thenCall:(id)completion {
    PV2RateAroundPause(self, ^{ %orig(item, completion); });
}
%end
%end

static BOOL PV2ABI(Class cls, NSString *name, const char *ret, const char *arg) {
    Method m = cls ? class_getInstanceMethod(cls, NSSelectorFromString(name)) : NULL;
    if (!m) return NO;
    char *r = method_copyReturnType(m);
    BOOL ok = r && strcmp(r, ret) == 0;
    free(r);
    if (arg) {
        char *a = method_copyArgumentType(m, 2);
        ok = ok && a && strcmp(a, arg) == 0; free(a);
    }
    PV2Log([NSString stringWithFormat:@"ABI %@ %s %@", name, method_getTypeEncoding(m), ok ? @"PASS" : @"SKIP"]);
    return ok;
}
%ctor {
    if (![NSBundle.mainBundle.bundleIdentifier isEqualToString:@"com.apple.mobileslideshow"]) return;
    Class tile = NSClassFromString(@"PUVideoTileViewController");
    Class oneUp = NSClassFromString(@"PUOneUpViewController");
    Class player = NSClassFromString(@"ISWrappedAVPlayer");
    BOOL ok = PV2ABI(tile, @"loadView", "@", NULL) && PV2ABI(tile, @"gestureRecognizers", "@", NULL)
        && PV2ABI(tile, @"setTilingView:", "v", "@") && PV2ABI(tile, @"setVideoSession:", "v", "@")
        && PV2ABI(tile, @"_setBrowsingVideoPlayer:", "v", "@") && PV2ABI(tile, @"didChangeActive", "v", NULL)
        && PV2ABI(tile, @"becomeReusable", "v", NULL) && PV2ABI(player, @"rate", "f", NULL)
        && PV2ABI(player, @"setRate:", "v", "f") && PV2ABI(player, @"pause", "v", NULL)
        && PV2ABI(player, @"replaceCurrentItemWithPlayerItem:", "v", "@");
    if (ok) {
        %init(PV2Hooks);
        BOOL ownerABI = PV2ABI(oneUp,@"_currentContentTileController","@",NULL)
            && PV2ABI(oneUp,@"_currentAssetViewModel","@",NULL)
            && PV2ABI(oneUp,@"_updateVideoPlayerIfNeeded","v",NULL)
            && PV2ABI(oneUp,@"_updateViewModelWithCurrentScrollPosition","v",NULL)
            && PV2ABI(oneUp,@"viewDidAppear:","v","B")
            && PV2ABI(oneUp,@"viewWillDisappear:","v","B")
            && PV2ABI(oneUp,@"viewDidDisappear:","v","B")
            && PV2ABI(oneUp,@"pu_wantsNavigationBarVisible","B",NULL)
            && PV2ABI(oneUp,@"pu_wantsToolbarVisible","B",NULL)
            && PV2ABI(NSClassFromString(@"PUOneUpBarsController"),@"_updateChromeVisibilityIfNeeded","v",NULL);
        if (ownerABI) {
            %init(PV2OneUpHooks);
            PV2TimelineInstall();
            if (PV2DownloadABI(NSClassFromString(@"PXVideoSession"),@"setLoopingEnabled:","v20@0:8B16")) { %init(PV2LoopHooks); }
            if (PV2DownloadABI(NSClassFromString(@"PUDoubleTapZoomController"),@"_handleDoubleTapGestureRecognizer:","v24@0:8@16") &&
                PV2DownloadABI(oneUp,@"_doubleTapZoomController","@16@0:8")) { %init(PV2DoubleTapHooks); }
        }
        PV2VideoToolsEnabled = ownerABI
            && PV2ABI(oneUp,@"_barsController","@",NULL)
            && PV2ABI(NSClassFromString(@"PUOneUpBarsController"),@"_toolbarButtonItemCollection","@",NULL)
            && PV2ABI(NSClassFromString(@"PUOneUpBarsController"),@"barButtonItemToggleDetails","@",NULL)
            && PV2ABI(NSClassFromString(@"PUBarButtonItemCollection"),@"orderedBarButtonsItemsForIdentifiers:","@","@");
        if (PV2VideoToolsEnabled) { %init(PV2ToolsHooks); }
        Class providerClass = NSClassFromString(@"PXVideoContentProvider");
        PV2DownloadEnabled = ownerABI && PV2DownloadABI(providerClass,@"setLoadingProgress:","v24@0:8d16")
            && PV2DownloadABI(providerClass,@"setLoadingResult:","v24@0:8@16");
        if (PV2DownloadEnabled) { %init(PV2DownloadHooks); }
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        for (NSString *name in @[UIApplicationWillResignActiveNotification,
                                  UIApplicationDidEnterBackgroundNotification,
                                  UISceneWillDeactivateNotification,
                                  UISceneDidEnterBackgroundNotification]) {
            [nc addObserverForName:name object:nil queue:nil usingBlock:^(__unused NSNotification *note) {
                PV2RateEndAll(YES);
                dispatch_async(dispatch_get_main_queue(), ^{
                    PV2TimelineLeave(PV2VisibleOneUp);
                    PV2SeekFeedbackLeave(PV2VisibleOneUp);
                    PV2ToolsLeave(PV2VisibleOneUp);
                    PV2DownloadEndAll();
                });
            }];
        }
        [nc addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(__unused NSNotification *note) {
            PV2DownloadRefreshOneUp(PV2VisibleOneUp);
            PV2ToolsRefresh(PV2VisibleOneUp);
            PV2TimelineRefresh(PV2VisibleOneUp);
        }];
        PV2Log(@"0.4.0 hooks installed; native loadView preserved");
    }
    else PV2Log(@"0.4.0 ABI mismatch; hooks skipped");
}
