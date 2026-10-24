#pragma once
// Genuine AVKit timeline + Photos-selected identity/seek coordinator.
// No custom slider, time labels, display link or timer drives native progress.
#import <UIKit/UIKit.h>
#import <CoreMedia/CoreMedia.h>
#import <objc/runtime.h>
#import <math.h>
#include "TimelineCore.h"
#import "LivePlayback.h"
#import "NativeTimeline.h"
#import "PhotosAuthority.h"
@interface PV2NativeBrowsing (PV2Timeline)
- (CMTime)duration;
- (id)videoSession;
- (void)seekToTime:(CMTime)t completionHandler:(void (^)(BOOL))completion;
- (void)seekToTime:(CMTime)t toleranceBefore:(CMTime)b toleranceAfter:(CMTime)a completionHandler:(void (^)(BOOL))completion;
@end
@interface NSObject (PV2TimelinePlayer)
- (id)videoPlayer;
- (float)rate;
- (BOOL)isReadyForSeeking;
@end
static BOOL PV2TimelineABI(void) {
    Class browsing = NSClassFromString(@"PUBrowsingVideoPlayer");
    Class tile = NSClassFromString(@"PUVideoTileViewController");
    return PV2DownloadABI(browsing, @"currentTime", "{?=qiIq}16@0:8")
        && PV2DownloadABI(browsing, @"duration", "{?=qiIq}16@0:8")
        && PV2DownloadABI(browsing, @"seekToTime:completionHandler:", "v48@0:8{?=qiIq}16@?40")
        && PV2DownloadABI(browsing, @"seekToTime:toleranceBefore:toleranceAfter:completionHandler:", "v96@0:8{?=qiIq}16{?=qiIq}40{?=qiIq}64@?88")
        && PV2DownloadABI(NSClassFromString(@"PXVideoSession"), @"isReadyForSeeking", "B16@0:8")
        && PV2DownloadABI(browsing, @"videoSession", "@16@0:8")
        && PV2DownloadABI(tile, @"_browsingVideoPlayer", "@16@0:8")
        && PV2DownloadABI(tile, @"videoSession", "@16@0:8");
}

static BOOL PV2TimelineEnabled = NO;
static void PV2TimelineInstall(void) { PV2TimelineEnabled=PV2TimelineABI(); }
static UIToolbar *PV2TimelineVisibleToolbar(UIViewController *owner) {
    if (!owner || !owner.isViewLoaded) return nil;
    UIToolbar *toolbar = owner.navigationController.toolbar;
    if (toolbar && !toolbar.hidden && toolbar.alpha > 0.01 && toolbar.window != nil && toolbar.bounds.size.height > 1.0)
        return toolbar;
    return nil;
}

static BOOL PV2TimelineAssetIsVideo(void) {
    id asset = PV2SelectedAsset();
    if (![asset isKindOfClass:PHAsset.class]) return NO;
    return ((PHAsset *)asset).mediaType == PHAssetMediaTypeVideo;
}

static CGFloat PV2TimelineFilmstripTop(UIView *host) {
    CGFloat top=CGFLOAT_MAX;
    NSMutableArray<UIView *> *views=[NSMutableArray arrayWithObject:host];
    Class film=NSClassFromString(@"PUFilmstripWrapperView");
    Class strip=NSClassFromString(@"PUFilmstripView");
    for (NSUInteger i=0;i<views.count && i<800;i++) {
        UIView *v=views[i];
        if (!v.window || v.hidden || v.alpha<0.01) continue;
        if ((film && [v isKindOfClass:film]) || (strip && [v isKindOfClass:strip])) {
            CGRect rect=[v convertRect:v.bounds toView:host];
            if (CGRectGetWidth(rect)>CGRectGetWidth(host.bounds)*0.5 && CGRectGetHeight(rect)>12 && CGRectGetMinY(rect)>CGRectGetHeight(host.bounds)*0.5)
                top=MIN(top,CGRectGetMinY(rect));
        }
        [views addObjectsFromArray:v.subviews];
    }
    return top;
}
static const void *PV2NativeTimelineHitKey=&PV2NativeTimelineHitKey;
static BOOL PV2TimelineHitIsNative(UIView *view) {
    for (UIView *v=view;v;v=v.superview)
        if (objc_getAssociatedObject(v,PV2NativeTimelineHitKey)) return YES;
    return NO;
}
@interface PV2TimelineController : NSObject
@property(nonatomic,weak) UIViewController *owner;
@property(nonatomic,weak) id tile;
@property(nonatomic,weak) id browsing;
@property(nonatomic,weak) id sessionToken;
@property(nonatomic,strong) id nativeWrapper;
@property(nonatomic,copy) NSString *assetIdentifier;
@property(nonatomic,strong) PV2NativeTimelineViewController *nativeTimeline;
@property(nonatomic,strong) PV2LivePlaybackSnapshot *liveSnapshot;
@property(nonatomic) PV2TimelineSeekState seekState;
@property(nonatomic) NSUInteger seekSerial;
@property(nonatomic) NSUInteger identityGeneration;
@property(nonatomic) NSUInteger sampleGeneration;
@property(nonatomic) BOOL samplingLive;
@property(nonatomic) NSUInteger tapGeneration;
@property(nonatomic) BOOL tapReading;
@property(nonatomic) double tapDelta;
@property(nonatomic) BOOL refreshing;
@property(nonatomic) BOOL playerEventScheduled;
@property(nonatomic) BOOL dragging; // seek policy test compatibility; not native slider state
- (void)refresh;
- (void)teardown;
- (void)sampleLivePlayback;
- (BOOL)readyForSeeking;
- (BOOL)seekBySeconds:(double)seconds;
- (BOOL)nativeScrubbing;
- (void)syncNativeTimeline;
@end
@implementation PV2TimelineController
- (instancetype)init { if ((self=[super init])) PV2TimelineSeekReset(&_seekState); return self; }
- (BOOL)isEligible {
    if (!PV2TimelineEnabled || !PV2OneUpVisible || !PV2VisibleOneUp) return NO;
    UIViewController *owner = self.owner;
    if (!owner || owner != PV2VisibleOneUp || !owner.isViewLoaded || owner.view.window == nil) return NO;
    if (!PV2TimelineAssetIsVideo()) return NO;
    PHAsset *asset=PV2SelectedAsset();
    return [self.assetIdentifier isEqualToString:asset.localIdentifier] && PV2TileIsSelected(self.tile);
}

- (id)resolveBrowsing {
    id tile = self.tile;
    if (!tile || ![tile respondsToSelector:@selector(_browsingVideoPlayer)]) return nil;
    id browsing = nil;
    @try { browsing = [tile _browsingVideoPlayer]; }
    @catch (__unused NSException *exception) { return nil; }
    if (!browsing) return nil;
    if (!PV2DownloadABI([browsing class], @"currentTime", "{?=qiIq}16@0:8")) return nil;
    return browsing;
}

- (BOOL)readyForSeeking {
    if (self.liveSnapshot.item) return self.liveSnapshot.ready;
    return self.sessionToken && [self.sessionToken isReadyForSeeking];
}

- (double)playbackDuration {
    if (self.liveSnapshot.item && CMTIME_IS_NUMERIC(self.liveSnapshot.duration)) {
        double live=CMTimeGetSeconds(self.liveSnapshot.duration);
        if (PV2TimelineHasDuration(live)) return live;
    }
    double nativeSeconds = 0.0;
    BOOL nativeValid = NO;
    id browsing = self.browsing;
    if (browsing) {
        @try {
            CMTime time = [(PV2NativeBrowsing *)browsing duration];
            if (CMTIME_IS_VALID(time) && CMTIME_IS_NUMERIC(time)) {
                nativeSeconds = CMTimeGetSeconds(time);
                nativeValid = isfinite(nativeSeconds) && nativeSeconds > 0.0;
            }
        } @catch (__unused NSException *exception) {}
    }
    // Native only: PHAsset.duration is NOT used because slow-motion / edited
    // clips need the time-range-mapped native length. An invalid native duration
    // means the timeline is not ready yet (the slider is disabled), not that we
    // should substitute the asset length.
    return PV2TimelineNativeTimelineSeconds(nativeSeconds, nativeValid);
}

- (double)currentSeconds {
    if (self.liveSnapshot.item && CMTIME_IS_NUMERIC(self.liveSnapshot.time)) {
        double live=CMTimeGetSeconds(self.liveSnapshot.time);
        if (isfinite(live) && live>=0) return live;
    }
    id browsing = self.browsing;
    if (!browsing) return 0.0;
    @try {
        CMTime time = [(PV2NativeBrowsing *)browsing currentTime];
        if (CMTIME_IS_VALID(time) && CMTIME_IS_NUMERIC(time)) {
            double seconds = CMTimeGetSeconds(time);
            if (isfinite(seconds) && seconds >= 0.0) return seconds;
        }
    } @catch (__unused NSException *exception) {}
    return 0.0;
}

- (void)invalidateSeeks {
    self.seekSerial++;
    PV2TimelineSeekInvalidate(&_seekState);
}

- (void)requestSeekSeconds:(double)seconds {
    if (!self.browsing) return;
    double duration = [self playbackDuration];
    if (!PV2TimelineHasDuration(duration)) return;
    [self submitSeekTarget:PV2TimelineClampSeconds(seconds, duration)];
}

- (void)submitSeekTarget:(double)target {
    double emitNow = 0.0;
    if (!PV2TimelineSeekRequest(&_seekState, target, &emitNow)) return; // coalesced into pending
    [self issueNativeSeekTo:emitNow];
}

- (void)issueNativeSeekTo:(double)target {
    if ([self nativeScrubbing]) { [self invalidateSeeks]; return; }
    id browsing = self.browsing;
    if (!browsing || !self.sessionToken || ![self readyForSeeking]) {
        [self invalidateSeeks];[self sampleLivePlayback];return;
    }
    // Capture the identity this seek belongs to. A completion whose identity no
    // longer matches (left the page / different asset+browsing+session) is stale.
    unsigned long epoch = _seekState.epoch;
    id session = self.sessionToken;
    NSUInteger serial=++self.seekSerial;
    PV2Log([NSString stringWithFormat:@"seek serial=%lu epoch=%lu target=%.3f liveitem=%p route=%@",(unsigned long)serial,epoch,target,self.liveSnapshot.item,self.liveSnapshot.item ? @"live" : @"browser"]);
    CMTime time = CMTimeMakeWithSeconds(target, 60000);
    __weak PV2TimelineController *weakSelf = self;
    void (^completed)(BOOL)=^(BOOL finished) {
        PV2Log([NSString stringWithFormat:@"seekCallback serial=%lu finished=%d",(unsigned long)serial,finished]);
        dispatch_async(dispatch_get_main_queue(), ^{
            PV2TimelineController *t=weakSelf;
            if (t && t.seekSerial==serial) [t nativeSeekCompletedForEpoch:epoch browsing:browsing session:session];
        });
    };
    // A loop replica replacement may cancel/lose AVPlayer's seek callback. The
    // UI queue must recover; late callbacks carry a serial and cannot finish a
    // newer request. Keep the pending last target, do not endlessly retry it.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(1.0*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
        PV2TimelineController *t=weakSelf;
        if (!t || t.seekSerial!=serial || t->_seekState.epoch!=epoch || !t->_seekState.inFlight) return;
        PV2Log([NSString stringWithFormat:@"seekTimeout serial=%lu target=%.3f",(unsigned long)serial,target]);
        t.seekSerial++;
        [t nativeSeekCompletedForEpoch:epoch browsing:browsing session:session];
        [t sampleLivePlayback];
    });
    @try {
        CMTime tolerance=self.dragging ? CMTimeMakeWithSeconds(0.05,60000) : kCMTimeZero;
        if (self.liveSnapshot.item) {
            PV2LivePlaybackSeek([session videoPlayer],self.liveSnapshot.item,time,tolerance,completed);
        } else {
            [(PV2NativeBrowsing *)browsing seekToTime:time toleranceBefore:tolerance toleranceAfter:tolerance completionHandler:completed];
        }
    } @catch (__unused NSException *exception) {
        PV2Log(@"timeline seek failed");completed(NO);
    }
}

- (void)nativeSeekCompletedForEpoch:(unsigned long)epoch browsing:(id)browsing session:(id)session {
    // Old callbacks must not invalidate a newer identity's live seek slot.
    if (epoch!=_seekState.epoch || self.browsing!=browsing || self.sessionToken!=session) return;
    BOOL live=[self isEligible] && [self.tile _browsingVideoPlayer]==browsing && [browsing videoSession]==session;
    if (!live) { [self invalidateSeeks]; return; }
    if ([self nativeScrubbing]) { [self invalidateSeeks]; return; }
    BOOL stale = NO;
    double next = 0.0;
    if (PV2TimelineSeekCompletion(&_seekState, epoch, &stale, &next)) {
        [self issueNativeSeekTo:next]; // latest coalesced target wins
    }
}

- (BOOL)nativeScrubbing { return self.nativeTimeline.isScrubbing; }

- (void)detachNativeTimeline {
    PV2NativeTimelineViewController *native=self.nativeTimeline;
    if (!native) return;
    self.nativeTimeline=nil;
    native.scrubbingDidChange=nil;
    [native willMoveToParentViewController:nil];
    [native invalidate];
    [native.view removeFromSuperview];
    [native removeFromParentViewController];
}

- (BOOL)bindSelectedIdentity {
    UIViewController *owner=self.owner;
    if (!owner || owner!=PV2VisibleOneUp || !PV2OneUpVisible || !owner.isViewLoaded) return NO;
    id asset=PV2SelectedAsset();
    NSString *identifier=[asset isKindOfClass:PHAsset.class] ? ((PHAsset*)asset).localIdentifier : nil;
    id tile=[owner _currentContentTileController];
    BOOL changed=tile!=self.tile || ![identifier isEqualToString:self.assetIdentifier];
    self.tile=tile;
    id browsing=[self resolveBrowsing];
    id session=nil; id wrapper=nil;
    @try { session=[browsing videoSession]; wrapper=[session videoPlayer]; }
    @catch (__unused NSException *e) { session=nil; wrapper=nil; }
    changed=changed || browsing!=self.browsing || session!=self.sessionToken || wrapper!=self.nativeWrapper;
    if (changed) {
        self.identityGeneration++; self.sampleGeneration++; self.tapGeneration++;
        self.samplingLive=NO; self.tapReading=NO; self.tapDelta=0;
        [self invalidateSeeks]; [self detachNativeTimeline]; self.liveSnapshot=nil;
    }
    self.assetIdentifier=identifier; self.browsing=browsing;
    self.sessionToken=session; self.nativeWrapper=wrapper;
    return changed;
}

// Read the real player only on the wrapper's player queue, then bind native UI
// on main. No periodic UI sampling: AVKit owns its timing/observation chain.
- (void)sampleLivePlayback {
    if (self.samplingLive || ![self isEligible] || !self.nativeWrapper) return;
    id session=self.sessionToken, browsing=self.browsing, wrapper=self.nativeWrapper, tile=self.tile;
    NSUInteger identity=self.identityGeneration, generation=++self.sampleGeneration;
    self.samplingLive=YES;
    __weak PV2TimelineController *weakSelf=self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{
        PV2TimelineController *t=weakSelf;
        if (t && t.samplingLive && t.sampleGeneration==generation) {
            t.samplingLive=NO; t.sampleGeneration++;
        }
    });
    PV2LivePlaybackRead(wrapper,^(PV2LivePlaybackSnapshot *snapshot){
        PV2TimelineController *t=weakSelf;
        if (!t || identity!=t.identityGeneration || generation!=t.sampleGeneration) return;
        t.samplingLive=NO;
        if (![t isEligible] || t.tile!=tile || t.browsing!=browsing || t.sessionToken!=session || [session videoPlayer]!=wrapper) return;
        if (t.liveSnapshot.player && t.liveSnapshot.player!=snapshot.player) [t invalidateSeeks];
        t.liveSnapshot=snapshot;
        [t syncNativeTimeline];
    });
}

- (void)layoutNativeTimeline {
    UIViewController *owner=self.owner;
    UIView *host=owner.isViewLoaded ? owner.view : nil;
    if (!host || !self.nativeTimeline) return;
    UIToolbar *toolbar=PV2TimelineVisibleToolbar(owner);
    double top=0; BOOL hasToolbar=NO;
    if (toolbar) {
        top=CGRectGetMinY([toolbar convertRect:toolbar.bounds toView:host]);
        hasToolbar=isfinite(top) && top>0 && top<=CGRectGetHeight(host.bounds);
    }
    CGFloat filmstripTop=PV2TimelineFilmstripTop(host);
    if (isfinite(filmstripTop) && filmstripTop<CGFLOAT_MAX && (!hasToolbar || filmstripTop<top)) {
        top=filmstripTop; hasToolbar=YES;
    }
    UIEdgeInsets insets=host.safeAreaInsets;
    PV2TimelinePanelSpec spec={};
    spec.hostWidth=CGRectGetWidth(host.bounds); spec.hostHeight=CGRectGetHeight(host.bounds);
    spec.safeTop=insets.top; spec.safeLeft=insets.left; spec.safeRight=insets.right; spec.safeBottom=insets.bottom;
    spec.hasToolbar=hasToolbar; spec.toolbarTop=top; spec.topClearance=insets.top+52;
    spec.height=88; spec.horizontalMargin=PV2TimelineHorizontalMarginDefault; spec.gap=PV2TimelineToolbarGapDefault;
    PV2TimelineRect r=PV2TimelinePanelRect(&spec);
    UIView *view=self.nativeTimeline.view;
    CGRect frame=CGRectMake(r.x,r.y,r.width,r.height);
    if (!CGRectEqualToRect(view.frame,frame)) view.frame=frame;
    [host bringSubviewToFront:view];
}

- (void)syncNativeTimeline {
    if (![self isEligible] || !PV2ResourceChromeVisible()) { [self detachNativeTimeline]; return; }
    AVPlayer *player=self.liveSnapshot.player;
    if (![player isKindOfClass:AVPlayer.class]) return;
    if (self.nativeTimeline && self.nativeTimeline.boundPlayer!=player) [self detachNativeTimeline];
    if (!self.nativeTimeline) {
        PV2NativeTimelineViewController *native=[[PV2NativeTimelineViewController alloc] initWithPlayer:player];
        if (!native) return;
        __weak PV2TimelineController *weakSelf=self;
        __weak PV2NativeTimelineViewController *weakNative=native;
        native.playbackIntent=^NSInteger{
            PV2TimelineController *t=weakSelf;
            return t && [t isEligible] ? PV2PhotosToolbarPlaybackIntent(t.owner) : -1;
        };
        native.playbackToggleRequested=^{
            PV2TimelineController *t=weakSelf; PV2NativeTimelineViewController *n=weakNative;
            if (!t || !n || ![t isEligible] || t.nativeTimeline!=n) return;
            // Exactly the lower blue toolbar's action; browsing desired state,
            // activation, observer publication and PXVideoSession stay native.
            PV2PhotosToolbarToggle(t.owner);
            [n refreshPlaybackIntent];
        };
        native.playbackRateRequested=^(double rate,BOOL temporary){
            (void)temporary;
            PV2TimelineController *t=weakSelf; PV2NativeTimelineViewController *n=weakNative;
            if (!t || !n || ![t isEligible] || t.nativeTimeline!=n) return;
            void *presenter=PV2PhotosBrowsingPresenter(t.browsing);
            // All low-level AVKit rates here are transport/scrub effects. A
            // genuine explicit user action has its own callback above.
            if (presenter) PV2PhotosRequestRateWithContext(t.sessionToken,t.nativeWrapper,rate,YES,1,presenter);
        };
        native.seekRequested=^(CMTime target,CMTime before,CMTime after,void (^complete)(BOOL)){
            PV2TimelineController *t=weakSelf; PV2NativeTimelineViewController *n=weakNative;
            if (!t || !n || ![t isEligible] || t.nativeTimeline!=n) {complete(NO);return;}
            PV2PhotosNativeSeek(t.nativeWrapper,n.boundPlayer,target,before,after,complete);
        };
        native.scrubbingDidChange=^(BOOL scrubbing){
            PV2TimelineController *t=weakSelf;
            if (!t || !scrubbing) return;
            [t invalidateSeeks]; t.tapGeneration++; t.tapReading=NO; t.tapDelta=0;
        };
        self.nativeTimeline=native;
        UIViewController *owner=self.owner;
        [owner addChildViewController:native];
        objc_setAssociatedObject(native.view,PV2NativeTimelineHitKey,@YES,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [owner.view addSubview:native.view];
        [self layoutNativeTimeline];
        [native didMoveToParentViewController:owner];
    } else [self layoutNativeTimeline];
    [self.nativeTimeline refreshPlaybackIntent];
}

- (void)refresh {
    if (!NSThread.isMainThread) {
        __weak PV2TimelineController *weakSelf=self;
        dispatch_async(dispatch_get_main_queue(),^{[weakSelf refresh];}); return;
    }
    if (self.refreshing) return;
    self.refreshing=YES;
    @try {
        [self bindSelectedIdentity];
        if (![self isEligible]) { [self teardown]; return; }
        if (!PV2ResourceChromeVisible()) { [self detachNativeTimeline]; return; }
        [self sampleLivePlayback];
        [self syncNativeTimeline];
    } @finally { self.refreshing=NO; }
}

// Each double-tap begins with a new transaction read rather than the old UI
// clock cache. Concurrent taps accumulate deltas, and the seek queue preserves
// the newest absolute target while a previous seek is still outstanding.
- (BOOL)seekBySeconds:(double)seconds {
    [self bindSelectedIdentity];
    if (![self isEligible] || !isfinite(seconds) || !self.nativeWrapper || [self nativeScrubbing]) return NO;
    self.tapDelta+=seconds;
    if (self.tapReading) return YES;
    self.tapReading=YES;
    NSUInteger generation=++self.tapGeneration, identity=self.identityGeneration;
    id wrapper=self.nativeWrapper, session=self.sessionToken, browsing=self.browsing, tile=self.tile;
    __weak PV2TimelineController *weakSelf=self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{
        PV2TimelineController *t=weakSelf;
        if (t && t.tapReading && t.tapGeneration==generation) {
            t.tapGeneration++; t.tapReading=NO; t.tapDelta=0;
        }
    });
    PV2LivePlaybackRead(wrapper,^(PV2LivePlaybackSnapshot *snapshot){
        PV2TimelineController *t=weakSelf;
        if (!t || t.tapGeneration!=generation || t.identityGeneration!=identity) return;
        t.tapReading=NO;
        double delta=t.tapDelta; t.tapDelta=0;
        if (![t isEligible] || t.tile!=tile || t.browsing!=browsing || t.sessionToken!=session || [session videoPlayer]!=wrapper || [t nativeScrubbing]) return;
        if (!snapshot.ready || !snapshot.item) return;
        t.liveSnapshot=snapshot;
        double duration=[t playbackDuration];
        if (!PV2TimelineHasDuration(duration)) return;
        double target=PV2TimelineNextSeekTarget([t currentSeconds],t->_seekState.inFlight,t->_seekState.hasPending,t->_seekState.pendingTarget,t->_seekState.emittedTarget,delta,duration);
        [t submitSeekTarget:target];
    });
    return YES;
}

- (void)teardown {
    self.identityGeneration++; self.sampleGeneration++; self.tapGeneration++;
    self.samplingLive=NO; self.tapReading=NO; self.tapDelta=0;
    [self invalidateSeeks]; [self detachNativeTimeline];
    self.liveSnapshot=nil; self.nativeWrapper=nil;
    self.tile=nil; self.browsing=nil; self.sessionToken=nil; self.assetIdentifier=nil;
}
@end

static const void *PV2TimelineOwnerKey=&PV2TimelineOwnerKey;
static PV2TimelineController *PV2TimelineForOwner(UIViewController *owner,BOOL create) {
    PV2TimelineController *t=owner ? objc_getAssociatedObject(owner,PV2TimelineOwnerKey) : nil;
    if (!t && owner && create) { t=[PV2TimelineController new]; t.owner=owner; objc_setAssociatedObject(owner,PV2TimelineOwnerKey,t,OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
    return t;
}
static void PV2TimelineRefresh(UIViewController *owner) {
    if (!PV2TimelineEnabled || !owner) return;
    if (!NSThread.isMainThread) { __weak UIViewController *weakOwner=owner; dispatch_async(dispatch_get_main_queue(),^{PV2TimelineRefresh(weakOwner);}); return; }
    if (owner!=PV2VisibleOneUp || !PV2OneUpVisible) return;
    [PV2TimelineForOwner(owner,YES) refresh];
}
static void PV2TimelineLeave(UIViewController *owner) {
    if (!owner) return;
    if (!NSThread.isMainThread) { __weak UIViewController *weakOwner=owner; dispatch_async(dispatch_get_main_queue(),^{PV2TimelineLeave(weakOwner);}); return; }
    PV2TimelineController *t=PV2TimelineForOwner(owner,NO);
    [t teardown]; objc_setAssociatedObject(owner,PV2TimelineOwnerKey,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
static const void *PV2TimelinePlayerEventKey=&PV2TimelinePlayerEventKey;
static const void *PV2TimelineToolbarEventKey=&PV2TimelineToolbarEventKey;
static NSLock *PV2TimelineEventLock(void) {
    static NSLock *lock; static dispatch_once_t once;
    dispatch_once(&once,^{ lock=[NSLock new]; }); return lock;
}
static BOOL PV2TimelineClaimEvent(id object,const void *key) {
    if (!object) return NO;
    NSLock *lock=PV2TimelineEventLock(); [lock lock];
    BOOL schedule=objc_getAssociatedObject(object,key)==nil;
    if (schedule) objc_setAssociatedObject(object,key,@YES,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [lock unlock]; return schedule;
}
static void PV2TimelineReleaseEvent(id object,const void *key) {
    NSLock *lock=PV2TimelineEventLock(); [lock lock];
    objc_setAssociatedObject(object,key,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [lock unlock];
}
static void PV2TimelineToolbarStateChanged(id stateController) {
    if (!PV2TimelineClaimEvent(stateController,PV2TimelineToolbarEventKey)) return;
    dispatch_async(dispatch_get_main_queue(),^{
        PV2TimelineReleaseEvent(stateController,PV2TimelineToolbarEventKey);
        PV2TimelineController *t=PV2TimelineForOwner(PV2VisibleOneUp,NO);
        if (t && [t isEligible] && PV2PhotosToolbarStateController(t.owner)==stateController)
            [t.nativeTimeline refreshPlaybackIntent];
    });
}
static void PV2TimelinePlayerChanged(id wrapper) {
    // Coalesce *before* enqueueing main work, not after a KVO storm has already
    // allocated one main block per intermediate rate/current-item event.
    if (!PV2TimelineClaimEvent(wrapper,PV2TimelinePlayerEventKey)) return;
    dispatch_async(dispatch_get_main_queue(),^{
        PV2TimelineReleaseEvent(wrapper,PV2TimelinePlayerEventKey);
        PV2TimelineController *t=PV2TimelineForOwner(PV2VisibleOneUp,NO);
        if (!t || t.nativeWrapper!=wrapper || ![t isEligible]) return;
        [t.nativeTimeline refreshPlaybackIntent];
        [t sampleLivePlayback];
    });
}
static BOOL PV2TimelineSeekBy(UIViewController *owner,double seconds) {
    if (!PV2TimelineEnabled || !owner || !NSThread.isMainThread || owner!=PV2VisibleOneUp || !PV2OneUpVisible) return NO;
    return [PV2TimelineForOwner(owner,YES) seekBySeconds:seconds];
}
