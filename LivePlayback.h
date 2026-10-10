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
@interface PV2LivePlaybackSnapshot : NSObject
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
                snapshot.item=player.currentItem;
                snapshot.time=player.currentTime;snapshot.duration=snapshot.item.duration;
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
            [player seekToTime:target toleranceBefore:tolerance toleranceAfter:tolerance completionHandler:completion];
        }];
    } @catch (__unused NSException *e) { completion(NO); }
}
