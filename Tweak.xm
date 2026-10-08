#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import "RateController.h"

// Photos tiles are NSObject controllers, not UIViewController subclasses.
@interface PUTileController : NSObject
- (BOOL)isActive;
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

static id<PV2RatePlayer> PV2Wrapper(PUVideoTileViewController *tile) {
    if (!tile || ![tile isActive]) return nil;
    @try {
        id browsing = [tile _browsingVideoPlayer];
        id session = [browsing respondsToSelector:@selector(videoSession)] ? [browsing videoSession] : [tile videoSession];
        id wrapper = [session respondsToSelector:@selector(videoPlayer)] ? [session videoPlayer] : nil;
        return [wrapper isKindOfClass:NSClassFromString(@"ISWrappedAVPlayer")] ? wrapper : nil;
    } @catch (__unused NSException *e) { return nil; }
}

@interface PV2GestureTarget : NSObject <UIGestureRecognizerDelegate>
@property(nonatomic, weak) PUVideoTileViewController *tile;
@property(nonatomic, strong) UILongPressGestureRecognizer *recognizer;
@property(nonatomic, strong) id<PV2RatePlayer> heldWrapper;
- (void)handle:(UILongPressGestureRecognizer *)gesture;
@end
static const void *PV2GestureKey = &PV2GestureKey;

static BOOL PV2AcceptPoint(PUVideoTileViewController *tile, CGPoint point, UIView *host) {
    if (!host || !tile || ![tile isActive]) return NO;
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
        if (PV2RateBegin(self, wrapper)) {
            self.heldWrapper = wrapper;
            PV2Log([NSString stringWithFormat:@"begin wrapper=%p", wrapper]);
        }
    } else if (gesture.state == UIGestureRecognizerStateChanged) {
        if (self.heldWrapper && PV2Wrapper(self.tile) != self.heldWrapper) {
            PV2RateEndOwner(self, YES); self.heldWrapper = nil;
        }
    } else if (gesture.state == UIGestureRecognizerStateEnded ||
               gesture.state == UIGestureRecognizerStateCancelled ||
               gesture.state == UIGestureRecognizerStateFailed) {
        PV2RateEndOwner(self, YES); self.heldWrapper = nil;
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
static void PV2EndTile(PUVideoTileViewController *tile) {
    PV2GestureTarget *target = PV2Target(tile, NO);
    if (target) { PV2RateEndOwner(target, YES); target.heldWrapper = nil; }
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
    if (![self isActive]) PV2EndTile(self);
}
- (void)setTilingView:(id)view {
    if ([self tilingView] != view) PV2EndTile(self);
    %orig(view);
}
- (void)setVideoSession:(id)session {
    if ([self videoSession] != session) PV2EndTile(self);
    %orig(session);
}
- (void)_setBrowsingVideoPlayer:(id)player {
    if ([self _browsingVideoPlayer] != player) PV2EndTile(self);
    %orig(player);
}
- (void)becomeReusable {
    PV2EndTile(self);
    %orig;
}
%end
%hook ISWrappedAVPlayer
- (void)setRate:(float)rate {
    PV2RateAroundSet(self, rate, ^(float effective) { %orig(effective); });
}
- (void)pause {
    PV2RateAroundPause(self, ^{ %orig; });
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
    Class player = NSClassFromString(@"ISWrappedAVPlayer");
    BOOL ok = PV2ABI(tile, @"loadView", "@", NULL) && PV2ABI(tile, @"gestureRecognizers", "@", NULL)
        && PV2ABI(tile, @"setTilingView:", "v", "@") && PV2ABI(tile, @"setVideoSession:", "v", "@")
        && PV2ABI(tile, @"_setBrowsingVideoPlayer:", "v", "@") && PV2ABI(tile, @"didChangeActive", "v", NULL)
        && PV2ABI(tile, @"becomeReusable", "v", NULL) && PV2ABI(player, @"rate", "f", NULL)
        && PV2ABI(player, @"setRate:", "v", "f") && PV2ABI(player, @"pause", "v", NULL)
        && PV2ABI(player, @"replaceCurrentItemWithPlayerItem:", "v", "@");
    if (ok) {
        %init(PV2Hooks);
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        for (NSString *name in @[UIApplicationWillResignActiveNotification,
                                  UIApplicationDidEnterBackgroundNotification,
                                  UISceneWillDeactivateNotification,
                                  UISceneDidEnterBackgroundNotification]) {
            [nc addObserverForName:name object:nil queue:nil usingBlock:^(__unused NSNotification *note) {
                PV2RateEndAll(YES);
            }];
        }
        PV2Log(@"0.1.1 hooks installed; native loadView preserved");
    }
    else PV2Log(@"0.1.1 ABI mismatch; hooks skipped");
}
