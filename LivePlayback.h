#pragma once
#import <AVFoundation/AVFoundation.h>
#import <objc/runtime.h>

// Verified iOS17.3: _performPlayerTransaction: passes the wrapper as its block
// argument on the native player queue. _playerQueue_avPlayer returns AVPlayer.
// Never treat ISWrappedAVPlayer as an AVPlayer or read its private ivars directly.
@interface NSObject (PV2LivePlaybackNative)
- (void)_performPlayerTransaction:(void (^)(id wrapper))transaction;
- (AVPlayer *)_playerQueue_avPlayer;
@end
@interface AVPlayerItem (PV2LoopClockNative)
- (CMTimebaseRef)_copyProxyUnfoldedTimebase CF_RETURNS_RETAINED;
- (CMTime)currentUnfoldedTime;
- (CMTimeRange)loopTimeRange;
@end
// currentTime uses the folded (per-lap) timebase, but AVPlayerItem's seek
// forwards its numeric target to the unfolded FigPlaybackItem. Convert using
// the same live item's timebases instead of reconstructing loop counts.
static inline CMTime PV2LivePlaybackSeekCoordinate(AVPlayerItem *item,CMTime displayed) {
    Method copy=class_getInstanceMethod(item.class,@selector(_copyProxyUnfoldedTimebase));
    BOOL compatible=copy && strcmp(method_getTypeEncoding(copy),"^{OpaqueCMTimebase=}16@0:8")==0;
    CMTimebaseRef folded=item.timebase;
    if (compatible && folded) {
        CMTimebaseRef unfolded=[item _copyProxyUnfoldedTimebase];
        if (unfolded) {
            CMTime current=item.currentTime;
            if (CMTIME_IS_NUMERIC(current)) displayed.epoch=current.epoch;
            CMTime mapped=CMSyncConvertTime(displayed,folded,unfolded);
            if (!CMTIME_IS_NUMERIC(mapped) && CMTIME_IS_NUMERIC(displayed) &&
                CMTimebaseGetEffectiveRate(folded)==0 && CMTimebaseGetEffectiveRate(unfolded)==0) {
                // Both are this item's media-seconds proxies, stopped by their
                // source chain. CMSync cannot invert a 0/0 rate. Translate the
                // requested duration using freshly measured, stable anchors;
                // never infer a lap count or cache a duration-derived offset.
                CMTime f=CMTimebaseGetTime(folded), u=CMTimebaseGetTime(unfolded);
                CMTime f2=CMTimebaseGetTime(folded), u2=CMTimebaseGetTime(unfolded);
                if (CMTIME_IS_NUMERIC(f) && CMTIME_IS_NUMERIC(u) &&
                    CMTimeCompare(f,f2)==0 && CMTimeCompare(u,u2)==0 &&
                    item.timebase==folded && (displayed.epoch==0 || displayed.epoch==f.epoch)) {
                    displayed.epoch=f.epoch;
                    CMTime delta=CMTimeSubtract(displayed,f);
                    if (CMTIME_IS_NUMERIC(delta)) mapped=CMTimeAdd(u,delta);
                }
            }
            CFRelease(unfolded);
            return CMTIME_IS_NUMERIC(mapped) ? mapped : kCMTimeInvalid;
        }
    }
    Method loop=class_getInstanceMethod(item.class,@selector(loopTimeRange));
    if (loop && strcmp(method_getTypeEncoding(loop),"{?={?=qiIq}{?=qiIq}}16@0:8")==0) {
        CMTimeRange range=[item loopTimeRange];
        if (CMTIMERANGE_IS_VALID(range) && CMTIME_IS_NUMERIC(range.duration) && CMTimeCompare(range.duration,kCMTimeZero)>0)
            return kCMTimeInvalid; // no raw-coordinate seek on an unmappable loop
    }
    return displayed; // ordinary non-looping media has no fold offset
}
@interface PV2LivePlaybackSnapshot : NSObject
@property(nonatomic,strong) AVPlayer *player;
@property(nonatomic,strong) AVPlayerItem *item;
@property(nonatomic) CMTime time;
@property(nonatomic) CMTime duration;
@property(nonatomic) BOOL ready;
@property(nonatomic) CMTime unfoldedTime;
@property(nonatomic) CMTime seekOrigin;
@property(nonatomic) BOOL clockMapped;
@end
@implementation PV2LivePlaybackSnapshot
@end
static BOOL PV2LivePlaybackABI(Class cls) {
    Method transaction=class_getInstanceMethod(cls,@selector(_performPlayerTransaction:));
    Method player=class_getInstanceMethod(cls,@selector(_playerQueue_avPlayer));
    return transaction && player && strcmp(method_getTypeEncoding(transaction),"v24@0:8@?16")==0 &&
        strcmp(method_getTypeEncoding(player),"@16@0:8")==0;
}
static inline void PV2LivePlaybackRead(id wrapper,void (^completion)(PV2LivePlaybackSnapshot *snapshot)) {
    if (!wrapper || !PV2LivePlaybackABI([wrapper class])) { completion(nil);return; }
    @try {
        [wrapper _performPlayerTransaction:^(id nativeWrapper) {
            AVPlayer *player=[nativeWrapper _playerQueue_avPlayer];
            PV2LivePlaybackSnapshot *snapshot=[PV2LivePlaybackSnapshot new];
            if ([player isKindOfClass:AVPlayer.class]) {
                snapshot.player=player;
                snapshot.item=player.currentItem;
                snapshot.time=player.currentTime;snapshot.duration=snapshot.item.duration;
                snapshot.seekOrigin=PV2LivePlaybackSeekCoordinate(snapshot.item,kCMTimeZero);
                snapshot.clockMapped=CMTIME_IS_NUMERIC(snapshot.seekOrigin);
                Method raw=class_getInstanceMethod(snapshot.item.class,@selector(currentUnfoldedTime));
                if (raw && strcmp(method_getTypeEncoding(raw),"{?=qiIq}16@0:8")==0)
                    snapshot.unfoldedTime=[snapshot.item currentUnfoldedTime];
                snapshot.ready=player.status==AVPlayerStatusReadyToPlay && snapshot.item.status==AVPlayerItemStatusReadyToPlay;
            }
            dispatch_async(dispatch_get_main_queue(),^{completion(snapshot);});
        }];
    } @catch (__unused NSException *e) { completion(nil); }
}
// Resolve the actual loop replica again INSIDE the native player queue. A stale
// PXVideoSession.ready flag must not prevent seeking a ready live replica.
static inline void PV2LivePlaybackSeek(id wrapper,AVPlayerItem *expectedItem,CMTime target,CMTime tolerance,void (^completion)(BOOL)) {
    if (!wrapper || !PV2LivePlaybackABI([wrapper class])) { completion(NO);return; }
    @try {
        [wrapper _performPlayerTransaction:^(id nativeWrapper) {
            AVPlayer *player=[nativeWrapper _playerQueue_avPlayer];
            AVPlayerItem *item=player.currentItem;
            if (![player isKindOfClass:AVPlayer.class] || !item || item!=expectedItem ||
                player.status!=AVPlayerStatusReadyToPlay || item.status!=AVPlayerItemStatusReadyToPlay) { completion(NO);return; }
            CMTime mapped=PV2LivePlaybackSeekCoordinate(item,target);
            if (!CMTIME_IS_NUMERIC(mapped)) { completion(NO);return; }
            [player seekToTime:mapped toleranceBefore:tolerance toleranceAfter:tolerance completionHandler:completion];
        }];
    } @catch (__unused NSException *e) { completion(NO); }
}
