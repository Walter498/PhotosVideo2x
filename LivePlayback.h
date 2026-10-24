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
// Matched iOS17.3 AVFCore narrows a looping seek's after tolerance to
// min(incomingAfter, loopEnd-fold(target)-1ms) BEFORE validating it. Even an
// incoming zero tolerance becomes negative in the final millisecond.
static inline CMTimeRange PV2LivePlaybackLoopRange(AVPlayerItem *item) {
    Method method=item ? class_getInstanceMethod(item.class,@selector(loopTimeRange)):NULL;
    if (!method || strcmp(method_getTypeEncoding(method),"{?={?=qiIq}{?=qiIq}}16@0:8")) return kCMTimeRangeInvalid;
    CMTimeRange range=[item loopTimeRange];
    if (!CMTIMERANGE_IS_VALID(range) || !CMTIME_IS_NUMERIC(range.start) ||
        !CMTIME_IS_NUMERIC(range.duration) || range.start.timescale<=0 ||
        range.duration.timescale<=0 || range.duration.epoch!=0 || range.duration.value<=0) return kCMTimeRangeInvalid;
    return range;
}
static inline BOOL PV2LivePlaybackToleranceValid(CMTime t) {
    return t.epoch==0 && (CMTIME_IS_POSITIVE_INFINITY(t) ||
        (CMTIME_IS_NUMERIC(t) && t.timescale>0 && t.value>=0));
}
static inline void (^PV2LivePlaybackOnceCompletion(void (^completion)(BOOL)))(BOOL) {
    NSObject *gate=[NSObject new]; __block BOOL delivered=NO;
    return [^(BOOL finished){
        @synchronized(gate) { if (delivered) return; delivered=YES; }
        if (completion) completion(finished);
    } copy];
}
static inline BOOL PV2LivePlaybackNormalizeSeek(AVPlayerItem *item,CMTime *target,CMTime *before,CMTime *after) {
    if (!item || !target || !before || !after || !CMTIME_IS_NUMERIC(*target) || target->timescale<=0 ||
        !PV2LivePlaybackToleranceValid(*before) || !PV2LivePlaybackToleranceValid(*after)) return NO;
    CMTimeRange range=PV2LivePlaybackLoopRange(item);
    if (CMTIMERANGE_IS_VALID(range)) {
        CMTime end=CMTimeRangeGetEnd(range), inset=CMTimeMake(1,1000);
        CMTime cap=CMTimeSubtract(end,inset), point=*target;
        if (!CMTIME_IS_NUMERIC(cap) || CMTimeCompare(cap,range.start)<0 ||
            (point.epoch!=0 && point.epoch!=range.start.epoch)) return NO;
        point.epoch=range.start.epoch;
        if (CMTimeCompare(point,range.start)<0) point=range.start;
        if (CMTimeCompare(point,cap)>0) point=cap;
        // Re-evaluate the exact CMTime arithmetic used by native AVFCore; no
        // floating-point epsilon, frame-rate guess or cached lap offset.
        CMTime allowance=CMTimeSubtract(CMTimeSubtract(end,point),inset);
        if (!CMTIME_IS_NUMERIC(allowance) || allowance.epoch!=0 || allowance.value<0) return NO;
        *target=point;
    } else {
        CMTime duration=item.duration, point=*target;
        if (point.epoch==0 && point.value<0) point=kCMTimeZero;
        if (point.epoch==0 && CMTIME_IS_NUMERIC(duration) && duration.timescale>0 && duration.epoch==0 &&
            duration.value>=0 && CMTimeCompare(point,duration)>0) point=duration;
        *target=point;
    }
    return YES;
}
static inline BOOL PV2LivePlaybackMappedSeekValid(AVPlayerItem *item,CMTime mapped) {
    if (!CMTIME_IS_NUMERIC(mapped) || mapped.timescale<=0) return NO;
    CMTimeRange range=PV2LivePlaybackLoopRange(item);
    if (!CMTIMERANGE_IS_VALID(range)) return YES;
    CMTime folded=CMTimeCompare(mapped,range.start)>=0 ? CMTimeFoldIntoRange(mapped,range) : mapped;
    CMTime allowance=CMTimeSubtract(CMTimeSubtract(CMTimeRangeGetEnd(range),folded),CMTimeMake(1,1000));
    return CMTIME_IS_NUMERIC(folded) && CMTIME_IS_NUMERIC(allowance) && allowance.epoch==0 && allowance.value>=0;
}
static inline BOOL PV2LivePlaybackFinalizeMappedSeek(AVPlayerItem *item,CMTime *mapped) {
    if (!mapped) return NO;
    if (PV2LivePlaybackMappedSeekValid(item,*mapped)) return YES;
    CMTimeRange range=PV2LivePlaybackLoopRange(item);
    if (!CMTIMERANGE_IS_VALID(range) || !CMTIME_IS_NUMERIC(*mapped) || mapped->timescale<=0) return NO;
    CMTime folded=CMTimeFoldIntoRange(*mapped,range);
    CMTime cap=CMTimeSubtract(CMTimeRangeGetEnd(range),CMTimeMake(1,1000));
    CMTime excess=CMTimeSubtract(folded,cap);
    if (!CMTIME_IS_NUMERIC(excess) || excess.epoch!=0 || excess.value<=0) return NO;
    // A supported affine conversion may round the exactly normalized edge
    // outward. Move by its measured excess plus one output tick, not a frame
    // or a fabricated lap offset, and verify native's window again once.
    CMTime correction=CMTimeAdd(excess,CMTimeMake(1,mapped->timescale));
    CMTime adjusted=CMTimeSubtract(*mapped,correction);
    if (!PV2LivePlaybackMappedSeekValid(item,adjusted)) return NO;
    *mapped=adjusted; return YES;
}
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
    void (^done)(BOOL)=PV2LivePlaybackOnceCompletion(completion);
    if (!wrapper || !PV2LivePlaybackABI([wrapper class])) { done(NO);return; }
    @try {
        [wrapper _performPlayerTransaction:^(id nativeWrapper) {
            @try {
                AVPlayer *player=[nativeWrapper _playerQueue_avPlayer];
                AVPlayerItem *item=player.currentItem;
                if (![player isKindOfClass:AVPlayer.class] || !item || item!=expectedItem ||
                    player.status!=AVPlayerStatusReadyToPlay || item.status!=AVPlayerItemStatusReadyToPlay) { done(NO);return; }
                CMTime point=target,b=tolerance,a=tolerance;
                if (!PV2LivePlaybackNormalizeSeek(item,&point,&b,&a)) { done(NO);return; }
                CMTime mapped=PV2LivePlaybackSeekCoordinate(item,point);
                if (!PV2LivePlaybackFinalizeMappedSeek(item,&mapped) || player.currentItem!=item) { done(NO);return; }
                [player seekToTime:mapped toleranceBefore:b toleranceAfter:a completionHandler:done];
            } @catch (__unused NSException *e) { done(NO); }
        }];
    } @catch (__unused NSException *e) { done(NO); }
}
