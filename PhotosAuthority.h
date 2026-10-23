#pragma once
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <objc/runtime.h>
#import "LivePlayback.h"

@protocol PV2PhotosWrapperCommands <NSObject>
- (void)pause;
- (void)setRate:(float)rate;
- (BOOL)isLoopingEnabled;
@end
@interface NSObject (PV2PhotosPlaybackIntent)
- (long long)desiredPlayState;
- (void)setDesiredPlayState:(long long)state;
- (void)performChanges:(void (^)(id mutableObject))changes withPresentationContext:(long long)context presenter:(void *)presenter;
- (void)pause;
- (void)setRate:(float)rate;
- (id)videoPlayer;
@end

static BOOL PV2PhotosIntentABI(id session) {
    Method get=session ? class_getInstanceMethod([session class],@selector(desiredPlayState)):NULL;
    Method set=session ? class_getInstanceMethod([session class],@selector(setDesiredPlayState:)):NULL;
    return get && set && strcmp(method_getTypeEncoding(get),"q16@0:8")==0 && strcmp(method_getTypeEncoding(set),"v24@0:8q16")==0;
}
// Native AVKit remains the timeline. All explicit transport changes enter the
// Photos session first, so its blue toolbar cannot keep a conflicting intent.
static void PV2PhotosRequestRate(id session,id wrapper,double rate,BOOL temporary) {
    if (!session || !wrapper || [session videoPlayer]!=wrapper || !isfinite(rate)) return;
    if (temporary) {
        // Scrubbing's temporary pause must not rewrite user intent. If Photos
        // was paused from its own toolbar during the drag, never resume it.
        if (rate>0 && PV2PhotosIntentABI(session) && [session desiredPlayState]==0) return;
        if (rate==0) [(id<PV2PhotosWrapperCommands>)wrapper pause]; else [(id<PV2PhotosWrapperCommands>)wrapper setRate:(float)rate];
        return;
    }
    if (!PV2PhotosIntentABI(session)) return;
    long long desired=rate==0 ? 0 : 1;
    SEL selector=@selector(performChanges:withPresentationContext:presenter:);
    Method transaction=class_getInstanceMethod([session class],selector);
    if (!transaction || strcmp(method_getTypeEncoding(transaction),"v40@0:8@?16q24^v32")!=0) return;
    [session performChanges:^(id mutableObject){ [mutableObject setDesiredPlayState:desired]; } withPresentationContext:0 presenter:NULL];
    // Also execute on the wrapper queue when the intent was already equal (the
    // setter may then be a no-op). RateController's existing hooks preserve the
    // selected fixed/boost speed for positive rates.
    if (rate==0) [(id<PV2PhotosWrapperCommands>)wrapper pause]; else [(id<PV2PhotosWrapperCommands>)wrapper setRate:(float)rate];
}
static void PV2PhotosNativeSeek(id wrapper,AVPlayer *expectedPlayer,CMTime displayed,
                               CMTime before,CMTime after,void (^completion)(BOOL)) {
    if (!wrapper || !expectedPlayer || !PV2LivePlaybackABI([wrapper class]) || !CMTIME_IS_NUMERIC(displayed)) { completion(NO);return; }
    Method looping=class_getInstanceMethod([wrapper class],@selector(isLoopingEnabled));
    BOOL loopKnown=looping && strcmp(method_getTypeEncoding(looping),"B16@0:8")==0;
    BOOL loops=loopKnown && [(id<PV2PhotosWrapperCommands>)wrapper isLoopingEnabled];
    @try {
        [wrapper _performPlayerTransaction:^(id nativeWrapper){
            AVPlayer *player=[nativeWrapper _playerQueue_avPlayer];
            AVPlayerItem *item=player.currentItem;
            if (player!=expectedPlayer || !item || item.status!=AVPlayerItemStatusReadyToPlay) { completion(NO);return; }
            CMTime mapped=loopKnown && !loops ? displayed : PV2LivePlaybackSeekCoordinate(item,displayed);
            if (!CMTIME_IS_NUMERIC(mapped)) { completion(NO);return; }
            [player seekToTime:mapped toleranceBefore:before toleranceAfter:after completionHandler:completion];
        }];
    } @catch (__unused NSException *exception) { completion(NO); }
}
