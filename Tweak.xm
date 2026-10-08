#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import <objc/runtime.h>
#import <math.h>
#import <substrate.h>

@interface ISWrappedAVPlayer : NSObject
- (float)rate;
- (void)setRate:(float)rate;
@end

@interface PXVideoSession : NSObject
- (ISWrappedAVPlayer *)videoPlayer;
@end

@interface PUBrowsingVideoPlayer : NSObject
- (PXVideoSession *)videoSession;
@end

@interface PUVideoTileViewController : UIViewController
- (PUBrowsingVideoPlayer *)_browsingVideoPlayer;
- (UIView *)videoView;
- (PXVideoSession *)videoSession;
@end

static const void *PV2TargetKey = &PV2TargetKey;
static const void *PV2BoostKey = &PV2BoostKey;
static const NSTimeInterval PV2MinimumPressDuration = 0.35;
static const CGFloat PV2SideFraction = 0.25;

@interface PV2Boost : NSObject
@property(nonatomic, weak) ISWrappedAVPlayer *wrapper;
@property(nonatomic, weak) PUVideoTileViewController *tile;
@property(nonatomic) float initialRate;
@property(nonatomic) float latestPositiveRate;
@property(nonatomic) BOOL hasLatestPositiveRate;
@property(nonatomic) BOOL active;
@property(nonatomic) BOOL sawStop;
@property(nonatomic) BOOL internalWrite;
@end

@implementation PV2Boost
@end

static void PV2Log(NSString *message) {
    NSString *line = [NSString stringWithFormat:@"[%@] %@\n", [NSDate date], message ?: @""];
    NSString *path = @"/var/mobile/Library/Logs/PhotosVideo2x.log";
    NSFileHandle *handle = nil;
    @try {
        if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
            [[NSFileManager defaultManager] createFileAtPath:path contents:nil attributes:nil];
        }
        handle = [NSFileHandle fileHandleForWritingAtPath:path];
        [handle seekToEndOfFile];
        [handle writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
    } @catch (__unused NSException *exception) {
    } @finally {
        [handle closeFile];
    }
}

static ISWrappedAVPlayer *PV2CurrentWrapper(PUVideoTileViewController *tile) {
    if (!tile) return nil;
    PUBrowsingVideoPlayer *browsing = nil;
    @try { browsing = [tile _browsingVideoPlayer]; } @catch (__unused NSException *e) {}
    if (!browsing) return nil;
    PXVideoSession *session = nil;
    @try { session = [browsing videoSession]; } @catch (__unused NSException *e) {}
    if (!session) return nil;
    ISWrappedAVPlayer *wrapper = nil;
    @try { wrapper = [session videoPlayer]; } @catch (__unused NSException *e) {}
    return wrapper;
}

static void PV2SetRateInternally(ISWrappedAVPlayer *wrapper, float rate) {
    PV2Boost *boost = objc_getAssociatedObject(wrapper, PV2BoostKey);
    if (!boost) return;
    boost.internalWrite = YES;
    @try { [wrapper setRate:rate]; } @catch (__unused NSException *e) {}
    boost.internalWrite = NO;
}

static void PV2EndBoostForWrapper(ISWrappedAVPlayer *wrapper) {
    PV2Boost *boost = objc_getAssociatedObject(wrapper, PV2BoostKey);
    if (!boost || !boost.active) return;

    boost.active = NO;
    objc_setAssociatedObject(wrapper, PV2BoostKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    // A pause, reverse request, resource replacement, or end-of-playback wins over restore.
    if (boost.sawStop) return;
    float restoreRate = boost.hasLatestPositiveRate ? boost.latestPositiveRate : boost.initialRate;
    if (!isfinite(restoreRate) || restoreRate <= 0.0f) return;
    @try { [wrapper setRate:restoreRate]; } @catch (__unused NSException *e) {}
}

static void PV2EndBoostForTile(PUVideoTileViewController *tile) {
    PV2Boost *boost = objc_getAssociatedObject(tile, PV2BoostKey);
    if (!boost) return;
    ISWrappedAVPlayer *wrapper = boost.wrapper;
    if (wrapper) PV2EndBoostForWrapper(wrapper);
    objc_setAssociatedObject(tile, PV2BoostKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

static BOOL PV2PointIsInSideRegion(UILongPressGestureRecognizer *gesture, UIView *view) {
    if (!view || view.bounds.size.width <= 1.0) return NO;
    CGPoint point = [gesture locationInView:view];
    CGFloat width = CGRectGetWidth(view.bounds);
    return point.x <= width * PV2SideFraction || point.x >= width * (1.0 - PV2SideFraction);
}

static BOOL PV2TouchBelongsToControl(UITouch *touch) {
    for (UIView *view = touch.view; view; view = view.superview) {
        if ([view isKindOfClass:UIControl.class]) return YES;
        NSString *name = NSStringFromClass(view.class);
        if ([name containsString:@"Scrubber"] || [name containsString:@"Button"]) return YES;
    }
    return NO;
}

@interface PV2GestureTarget : NSObject <UIGestureRecognizerDelegate>
@property(nonatomic, weak) PUVideoTileViewController *tile;
@property(nonatomic, weak) UIView *videoView;
@end

@implementation PV2GestureTarget

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gesture shouldReceiveTouch:(UITouch *)touch {
    if (PV2TouchBelongsToControl(touch)) return NO;
    UIView *view = self.videoView;
    if (!view) return NO;
    CGPoint point = [touch locationInView:view];
    CGFloat width = CGRectGetWidth(view.bounds);
    return width > 1.0 && (point.x <= width * PV2SideFraction ||
                           point.x >= width * (1.0 - PV2SideFraction));
}

- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer *)gesture {
    UILongPressGestureRecognizer *longPress = (id)gesture;
    if (![longPress isKindOfClass:UILongPressGestureRecognizer.class]) return NO;
    if (!PV2PointIsInSideRegion(longPress, self.videoView)) return NO;
    ISWrappedAVPlayer *wrapper = PV2CurrentWrapper(self.tile);
    if (!wrapper) return NO;
    float rate = 0.0f;
    @try { rate = [wrapper rate]; } @catch (__unused NSException *e) { return NO; }
    return isfinite(rate) && rate > 0.0f;
}

- (void)handleLongPress:(UILongPressGestureRecognizer *)gesture {
    PUVideoTileViewController *tile = self.tile;
    if (!tile) return;

    if (gesture.state == UIGestureRecognizerStateBegan) {
        PV2EndBoostForTile(tile);
        ISWrappedAVPlayer *wrapper = PV2CurrentWrapper(tile);
        if (!wrapper) return;
        float rate = 0.0f;
        @try { rate = [wrapper rate]; } @catch (__unused NSException *e) { return; }
        if (!isfinite(rate) || rate <= 0.0f) return;

        PV2Boost *boost = [PV2Boost new];
        boost.wrapper = wrapper;
        boost.tile = tile;
        boost.initialRate = rate;
        boost.active = YES;
        objc_setAssociatedObject(wrapper, PV2BoostKey, boost, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        objc_setAssociatedObject(tile, PV2BoostKey, boost, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        PV2SetRateInternally(wrapper, 2.0f);
        PV2Log([NSString stringWithFormat:@"boost begin rate=%.3f wrapper=%p", rate, wrapper]);
        return;
    }

    if (gesture.state == UIGestureRecognizerStateEnded ||
        gesture.state == UIGestureRecognizerStateCancelled ||
        gesture.state == UIGestureRecognizerStateFailed) {
        PV2Boost *boost = objc_getAssociatedObject(tile, PV2BoostKey);
        ISWrappedAVPlayer *wrapper = boost.wrapper;
        PV2EndBoostForTile(tile);
        PV2Log([NSString stringWithFormat:@"boost end wrapper=%p", wrapper]);
    }
}
@end

static void PV2InstallGesture(PUVideoTileViewController *tile, UIView *view);

static void PV2RemoveGesture(UIView *view) {
    if (!view) return;
    PV2GestureTarget *target = objc_getAssociatedObject(view, PV2TargetKey);
    if (target) {
        for (UIGestureRecognizer *recognizer in [view.gestureRecognizers copy]) {
            if (recognizer.delegate == target) {
                [view removeGestureRecognizer:recognizer];
            }
        }
    }
    objc_setAssociatedObject(view, PV2TargetKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

static void PV2InstallCurrentGesture(PUVideoTileViewController *tile) {
    if (!tile) return;
    UIView *view = nil;
    @try { view = [tile videoView]; } @catch (__unused NSException *e) {}
    PV2InstallGesture(tile, view);
}

static void PV2InstallGesture(PUVideoTileViewController *tile, UIView *view) {
    if (!tile || !view) return;
    PV2GestureTarget *oldTarget = objc_getAssociatedObject(view, PV2TargetKey);
    if (oldTarget) {
        BOOL hasGesture = NO;
        for (UIGestureRecognizer *recognizer in view.gestureRecognizers) {
            if ([recognizer.delegate isEqual:oldTarget]) { hasGesture = YES; break; }
        }
        if (hasGesture) { oldTarget.tile = tile; return; }
    }

    PV2GestureTarget *target = [PV2GestureTarget new];
    target.tile = tile;
    target.videoView = view;
    UILongPressGestureRecognizer *gesture =
        [[UILongPressGestureRecognizer alloc] initWithTarget:target action:@selector(handleLongPress:)];
    gesture.minimumPressDuration = PV2MinimumPressDuration;
    gesture.allowableMovement = 18.0;
    gesture.numberOfTouchesRequired = 1;
    gesture.cancelsTouchesInView = NO;
    gesture.delegate = target;
    [view addGestureRecognizer:gesture];
    objc_setAssociatedObject(view, PV2TargetKey, target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

%hook PUVideoTileViewController

- (void)loadView {
    %orig;
    PV2InstallCurrentGesture(self);
}

- (void)setVideoView:(UIView *)view {
    UIView *oldView = nil;
    @try { oldView = [self videoView]; } @catch (__unused NSException *e) {}
    if (oldView != view) {
        PV2EndBoostForTile(self);
        PV2RemoveGesture(oldView);
    }
    %orig(view);
    PV2InstallGesture(self, view);
}

- (void)setVideoSession:(PXVideoSession *)session {
    PV2EndBoostForTile(self);
    %orig(session);
    PV2InstallCurrentGesture(self);
}

- (void)becomeReusable {
    PV2EndBoostForTile(self);
    %orig;
}

- (void)dealloc {
    PV2EndBoostForTile(self);
    %orig;
}

%end

%hook ISWrappedAVPlayer

- (void)setRate:(float)rate {
    PV2Boost *boost = objc_getAssociatedObject(self, PV2BoostKey);
    if (!boost || !boost.active || boost.internalWrite) {
        %orig(rate);
        return;
    }

    if (isfinite(rate) && rate > 0.0f) {
        boost.latestPositiveRate = rate;
        boost.hasLatestPositiveRate = YES;
        %orig(2.0f);
        return;
    }

    // Stop/pause/reverse requests are never hidden by the temporary boost.
    boost.sawStop = YES;
    boost.active = NO;
    objc_setAssociatedObject(self, PV2BoostKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    %orig(rate);
}

%end

%ctor {
    Class wrapperClass = NSClassFromString(@"ISWrappedAVPlayer");
    Method rateMethod = wrapperClass ? class_getInstanceMethod(wrapperClass, @selector(rate)) : NULL;
    Method setRateMethod = wrapperClass ? class_getInstanceMethod(wrapperClass, @selector(setRate:)) : NULL;
    if (rateMethod && setRateMethod) {
        PV2Log([NSString stringWithFormat:@"loaded rate=%s setRate=%s",
                method_getTypeEncoding(rateMethod), method_getTypeEncoding(setRateMethod)]);
    } else {
        PV2Log(@"loaded but ISWrappedAVPlayer rate methods were not found");
    }
}
