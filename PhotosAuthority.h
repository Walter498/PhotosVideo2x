#pragma once
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
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
static void PV2PhotosRequestRateWithContext(id session,id wrapper,double rate,BOOL temporary,long long context,void *presenter) {
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
    [session performChanges:^(id mutableObject){ [mutableObject setDesiredPlayState:desired]; } withPresentationContext:context presenter:presenter];
    // Also execute on the wrapper queue when the intent was already equal (the
    // setter may then be a no-op). RateController's existing hooks preserve the
    // selected fixed/boost speed for positive rates.
    if (rate==0) [(id<PV2PhotosWrapperCommands>)wrapper pause]; else [(id<PV2PhotosWrapperCommands>)wrapper setRate:(float)rate];
}
static void *PV2PhotosBrowsingPresenter(id browsing) {
    if (!browsing) return NULL;
    Ivar ivar=class_getInstanceVariable([browsing class],"_videoSessionPresenter");
    if (!ivar || strcmp(ivar_getTypeEncoding(ivar),"^v")) return NULL;
    ptrdiff_t offset=ivar_getOffset(ivar);
    if (offset<0 || (size_t)offset+sizeof(void*)>class_getInstanceSize([browsing class])) return NULL;
    void *presenter=NULL;
    memcpy(&presenter,(const char*)(__bridge const void*)browsing+offset,sizeof(presenter));
    return presenter;
}
static id PV2PhotosToolbarStateController(id owner) {
    SEL bars=NSSelectorFromString(@"_barsController");
    SEL state=NSSelectorFromString(@"_playPauseBarItemsController");
    Method a=owner ? class_getInstanceMethod([owner class],bars):NULL;
    if (!a || strcmp(method_getTypeEncoding(a),"@16@0:8")) return nil;
    id b=((id (*)(id,SEL))objc_msgSend)(owner,bars);
    Method m=b ? class_getInstanceMethod([b class],state):NULL;
    if (!m || strcmp(method_getTypeEncoding(m),"@16@0:8")) return nil;
    return ((id (*)(id,SEL))objc_msgSend)(b,state);
}
static NSInteger PV2PhotosToolbarPlaybackIntent(id owner) {
    id state=PV2PhotosToolbarStateController(owner);
    SEL get=NSSelectorFromString(@"playPauseState");
    Method m=state ? class_getInstanceMethod([state class],get):NULL;
    if (!m || strcmp(method_getTypeEncoding(m),"q16@0:8")) return -1;
    long long value=((long long (*)(id,SEL))objc_msgSend)(state,get);
    // iOS17.3 toolbar: 1 offers Pause, 2 offers Play/replay, 0 unavailable.
    return value==1 ? 1 : value==2 ? 0 : -1;
}
static void PV2PhotosToolbarToggle(id owner) {
    SEL toggle=NSSelectorFromString(@"togglePlayback:");
    Method m=owner ? class_getInstanceMethod([owner class],toggle):NULL;
    if (m && strcmp(method_getTypeEncoding(m),"v24@0:8@16")==0)
        ((void (*)(id,SEL,id))objc_msgSend)(owner,toggle,nil);
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
