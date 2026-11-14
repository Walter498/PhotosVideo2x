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
    CMTime duration=item.duration, point=*target;
    if (point.epoch!=0) return NO;
    if (point.value<0) point=kCMTimeZero;
    if (CMTIME_IS_NUMERIC(duration) && duration.timescale>0 && duration.epoch==0 &&
        duration.value>=0 && CMTimeCompare(point,duration)>0) point=duration;
    *target=point;
    return YES;
}
@interface PV2LivePlaybackSnapshot : NSObject
@property(nonatomic,strong) AVPlayer *player;
@property(nonatomic,strong) AVPlayerItem *item;
@property(nonatomic) CMTime time;
@property(nonatomic) CMTime duration;
@property(nonatomic) BOOL ready;
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
                snapshot.ready=player.status==AVPlayerStatusReadyToPlay && snapshot.item.status==AVPlayerItemStatusReadyToPlay;
            }
            dispatch_async(dispatch_get_main_queue(),^{completion(snapshot);});
        }];
    } @catch (__unused NSException *e) { completion(nil); }
}
// Resolve the selected item again inside the native player queue; reject stale identities.
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
                if (player.currentItem!=item) { done(NO);return; }
                [player seekToTime:point toleranceBefore:b toleranceAfter:a completionHandler:done];
            } @catch (__unused NSException *e) { done(NO); }
        }];
    } @catch (__unused NSException *e) { done(NO); }
}
