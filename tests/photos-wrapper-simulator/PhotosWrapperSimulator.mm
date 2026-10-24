// Include contract for another harness:
//   #define PV2_PHOTOS_WRAPPER_NO_MAIN 1
//   #include "tests/photos-wrapper-simulator/PhotosWrapperSimulator.mm"
//   NSDictionary *result = PV2RunPhotosWrapperChecks(rootViewController, YES);
// Standalone installs the production playbackRateRequested and
// seekRequested callbacks. Rate commands target the real wrapper; seek maps
// coordinates and executes once inside its actual player transaction.
// A missing/incompatible bridge is a FAILURE, never a skipped integration pass.
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import <objc/runtime.h>
#import <dlfcn.h>
#import <math.h>
#import <atomic>
#import "NativeTimeline.h"
#import "PhotosAuthority.h"

@protocol PV2RealWrapperInitializer <NSObject>
- (instancetype)_initWithAVPlayer:(AVPlayer *)player __attribute__((objc_method_family(init)));
@end
@protocol PV2RealContentProvider <NSObject>
- (instancetype)initWithPlayerItem:(AVPlayerItem *)item url:(NSURL *)url timeRange:(CMTimeRange)range timeRangeMapper:(id)mapper error:(NSError *)error priority:(long long)priority description:(NSString *)description;
- (void)performChanges:(void (^)(id))changes;
- (void)setLoadingResult:(id)result;
@end
@protocol PV2RealSessionInitializer <NSObject>
- (instancetype)initWithContentProvider:(id)provider videoPlayer:(id)wrapper;
- (void)enterPresentationContext:(long long)context presenter:(void*)presenter;
- (void)leavePresentationContext:(long long)context presenter:(void*)presenter;
- (void)setLoopingEnabled:(BOOL)value;
@end
namespace PV2PhotosChecks {
static NSMutableArray *checks;
static NSMutableDictionary *report;
static NSString *Docs(NSString *name) {
    return [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:name];
}
static void Check(BOOL ok, NSString *name, id detail=nil) {
    [checks addObject:@{@"name":name,@"passed":@(ok),@"detail":detail ?: @""}];
    NSLog(@"PHOTOSWRAPPER %@ %@ %@",ok ? @"PASS" : @"FAIL",name,detail ?: @"");
    if (!ok) @throw [NSException exceptionWithName:@"PhotosWrapperFailure" reason:name userInfo:nil];
}
static BOOL Wait(double seconds, BOOL (^condition)(void)) {
    double end=NSProcessInfo.processInfo.systemUptime+seconds;
    do {
        if (condition()) return YES;
        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    } while (NSProcessInfo.processInfo.systemUptime<end);
    return condition();
}
static void ABI(Class cls, NSString *name, const char *expected) {
    Method m=class_getInstanceMethod(cls,NSSelectorFromString(name));
    const char *actual=m ? method_getTypeEncoding(m) : "<missing>";
    Check(m && !strcmp(actual,expected),[@"ABI " stringByAppendingString:name],
          @{@"class":cls ? NSStringFromClass(cls) : @"<missing>",@"expected":@(expected),@"actual":@(actual)});
}
static NSInvocation *Invoke(id object, NSString *name) {
    SEL sel=NSSelectorFromString(name);
    Check([object respondsToSelector:sel],[@"selector " stringByAppendingString:name],NSStringFromClass([object class]));
    NSInvocation *inv=[NSInvocation invocationWithMethodSignature:[object methodSignatureForSelector:sel]];
    inv.target=object; inv.selector=sel; return inv;
}
static id Object(id object, NSString *name) {
    NSInvocation *inv=Invoke(object,name);
    Check(inv.methodSignature.numberOfArguments==2 && !strcmp(inv.methodSignature.methodReturnType,"@"),@"object getter ABI",name);
    [inv invoke]; __unsafe_unretained id value=nil; [inv getReturnValue:&value]; return value;
}
static double NativeNumber(id object,NSString *name) {
    NSInvocation *inv=Invoke(object,name);[inv invoke];
    const char *t=inv.methodSignature.methodReturnType;
    if (!strcmp(t,@encode(float))) {float v=0;[inv getReturnValue:&v];return v;}
    if (!strcmp(t,@encode(double))) {double v=0;[inv getReturnValue:&v];return v;}
    Check(NO,@"native numeric ABI",name);return NAN;
}
static void NativeValue(UIControl *slider,double value) {
    NSInvocation *inv=Invoke(slider,@"setValue:");
    const char *t=[inv.methodSignature getArgumentTypeAtIndex:2];
    if (!strcmp(t,@encode(float))) {float v=value;[inv setArgument:&v atIndex:2];}
    else if (!strcmp(t,@encode(double))) {[inv setArgument:&value atIndex:2];}
    else Check(NO,@"native slider value ABI");
    [inv invoke];[slider sendActionsForControlEvents:UIControlEventValueChanged];
}
static float Rate(id object) {
    NSInvocation *inv=Invoke(object,@"rate");
    Check(!strcmp(inv.methodSignature.methodReturnType,@encode(float)),@"rate float ABI");
    [inv invoke]; float value=0; [inv getReturnValue:&value]; return value;
}
static void SetRate(id object, float value) {
    NSInvocation *inv=Invoke(object,@"setRate:");
    Check(inv.methodSignature.numberOfArguments==3,@"setRate argument count");
    const char *type=[inv.methodSignature getArgumentTypeAtIndex:2];
    if (!strcmp(type,@encode(float))) [inv setArgument:&value atIndex:2];
    else if (!strcmp(type,@encode(double))) { double native=value; [inv setArgument:&native atIndex:2]; }
    else Check(NO,@"setRate numeric ABI",@(type));
    [inv invoke];
}
static void Pause(id object) { [Invoke(object,@"pause") invoke]; }
static void Seek(id object, double seconds, void (^completion)(BOOL)) {
    NSInvocation *inv=Invoke(object,@"seekToTime:toleranceBefore:toleranceAfter:completionHandler:");
    Check(inv.methodSignature.numberOfArguments==6 &&
          !strcmp([inv.methodSignature getArgumentTypeAtIndex:2],@encode(CMTime)) &&
          !strcmp([inv.methodSignature getArgumentTypeAtIndex:3],@encode(CMTime)) &&
          !strcmp([inv.methodSignature getArgumentTypeAtIndex:4],@encode(CMTime)) &&
          !strcmp([inv.methodSignature getArgumentTypeAtIndex:5],"@?"),@"seek CMTime and completion ABI");
    CMTime time=CMTimeMakeWithSeconds(seconds,600), zero=kCMTimeZero;
    [inv setArgument:&time atIndex:2]; [inv setArgument:&zero atIndex:3];
    [inv setArgument:&zero atIndex:4]; [inv setArgument:&completion atIndex:5]; [inv invoke];
}
// Always fetch the live player INSIDE the real wrapper transaction. Completion
// hops to main: a queue barrier alone does not flush Photos' cached state/KVO.
static void Transaction(id wrapper, void (^operation)(AVPlayer *)) {
    __block BOOL done=NO;
    __block NSException *failure=nil;
    void (^block)(id)=^(id currentWrapper) {
        @try {
            Check(currentWrapper==wrapper,@"transaction receives actual wrapper");
            AVPlayer *live=Object(currentWrapper,@"_playerQueue_avPlayer");
            Check([live isKindOfClass:AVPlayer.class],@"live queue AVPlayer exists");
            operation(live);
        } @catch (NSException *exception) { failure=exception; }
        dispatch_async(dispatch_get_main_queue(),^{ done=YES; });
    };
    NSInvocation *inv=Invoke(wrapper,@"_performPlayerTransaction:");
    [inv setArgument:&block atIndex:2]; [inv invoke];
    Check(Wait(8,^BOOL { return done; }),@"real player transaction completed");
    if (failure) @throw failure;
}
static void SeekAndWait(id wrapper, double seconds) {
    // Completion is delivered on an unspecified queue; marshal flags to main.
    __block BOOL done=NO, finished=NO;
    Seek(wrapper,seconds,^(BOOL value) {
        dispatch_async(dispatch_get_main_queue(),^{ finished=value; done=YES; });
    });
    Check(Wait(8,^BOOL { return done; }) && finished,@"wrapper seek completion",@(seconds));
    __block double actual=NAN;
    Transaction(wrapper,^(AVPlayer *live) { actual=CMTimeGetSeconds(live.currentTime); });
    Check(isfinite(actual) && fabs(actual-seconds)<.15,@"queue seek reaches live current item",@{@"target":@(seconds),@"actual":@(actual)});
}
static void Converged(id wrapper, AVPlayer *player, float expected) {
    Check(Wait(5,^BOOL { return fabsf(player.rate-expected)<.02f && fabsf(Rate(wrapper)-expected)<.02f; }),
          @"wrapper cache and raw rate converge",@{@"expected":@(expected),@"raw":@(player.rate),@"wrapper":@(Rate(wrapper))});
    __block float rate=NAN;
    Transaction(wrapper,^(AVPlayer *live) { Check(live==player,@"transaction live player identity"); rate=live.rate; });
    Check(fabsf(rate-expected)<.02f,@"queue rate matches delivered wrapper state",@(rate));
}
static NSURL *MakeAsset(void) {
    NSURL *url=[NSURL fileURLWithPath:Docs(@"six-seconds.mp4")];
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
    NSError *error=nil;
    AVAssetWriter *writer=[[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeMPEG4 error:&error];
    Check(writer!=nil,@"create real AVAssetWriter",error.localizedDescription);
    AVAssetWriterInput *input=[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:@{AVVideoCodecKey:AVVideoCodecTypeH264,AVVideoWidthKey:@320,AVVideoHeightKey:@180}];
    input.expectsMediaDataInRealTime=NO;
    AVAssetWriterInputPixelBufferAdaptor *adaptor=[AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input sourcePixelBufferAttributes:@{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32ARGB),(id)kCVPixelBufferWidthKey:@320,(id)kCVPixelBufferHeightKey:@180,(id)kCVPixelBufferIOSurfacePropertiesKey:@{}}];
    [writer addInput:input]; Check([writer startWriting],@"start H264 encoding",writer.error.localizedDescription);
    [writer startSessionAtSourceTime:kCMTimeZero];
    for (int frame=0;frame<180;frame++) {
        Check(Wait(20,^BOOL { return input.readyForMoreMediaData || writer.status==AVAssetWriterStatusFailed; }) && input.readyForMoreMediaData,@"encoder ready",@(frame));
        CVPixelBufferRef buffer=NULL;
        CVReturn code=CVPixelBufferPoolCreatePixelBuffer(NULL,adaptor.pixelBufferPool,&buffer);
        Check(code==kCVReturnSuccess && buffer,@"allocate video frame",@(code));
        CVPixelBufferLockBaseAddress(buffer,0);
        uint8_t *base=(uint8_t *)CVPixelBufferGetBaseAddress(buffer);
        size_t stride=CVPixelBufferGetBytesPerRow(buffer);
        for (int y=0;y<180;y++) for (int x=0;x<320;x++) {
            uint8_t *p=base+y*stride+x*4; p[0]=255; p[1]=(frame*3)%255; p[2]=(x+frame)%255; p[3]=(y*2)%255;
        }
        CVPixelBufferUnlockBaseAddress(buffer,0);
        BOOL ok=[adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMake(frame,30)];
        CVPixelBufferRelease(buffer); Check(ok,@"append real H264 frame",@(frame));
    }
    [input markAsFinished]; __block BOOL done=NO;
    [writer finishWritingWithCompletionHandler:^{ done=YES; }];
    Check(Wait(15,^BOOL { return done; }) && writer.status==AVAssetWriterStatusCompleted,@"finish six-second H264 asset",writer.error.localizedDescription);
    return url;
}

} // namespace PV2PhotosChecks

// Real AVPlayer subclass only counts entry to the real superclass seek. No
// cached state, fabricated completion, or replaced media engine.
@interface PV2PhotosObservedPlayer : AVPlayer
@property(atomic) NSUInteger exactSeekCalls;
@end
@implementation PV2PhotosObservedPlayer
- (void)seekToTime:(CMTime)time toleranceBefore:(CMTime)before toleranceAfter:(CMTime)after completionHandler:(void (^)(BOOL))completion {
    self.exactSeekCalls+=1;
    [super seekToTime:time toleranceBefore:before toleranceAfter:after completionHandler:completion];
}
@end

// This adapter records the production controller's commands and forwards them
// to the REAL ISWrappedAVPlayer. It never implements or caches player state.
@interface PV2PhotosCommandProbe : NSObject
@property(nonatomic,strong) id wrapper;
@property(nonatomic) NSUInteger rateCalls;
@property(nonatomic) NSUInteger pauseCalls;
@property(nonatomic) NSUInteger seekCalls;
@property(nonatomic) float lastRate;
- (void)setRate:(float)value;
- (void)pause;
- (void)seekToTime:(CMTime)time toleranceBefore:(CMTime)before toleranceAfter:(CMTime)after completionHandler:(void (^)(BOOL))completion;
@end
@implementation PV2PhotosCommandProbe
- (void)setRate:(float)value {
    self.rateCalls+=1; self.lastRate=value;
    PV2PhotosChecks::SetRate(self.wrapper,value);
}
- (void)pause { self.pauseCalls+=1; PV2PhotosChecks::Pause(self.wrapper); }
- (void)seekToTime:(CMTime)time toleranceBefore:(CMTime)before toleranceAfter:(CMTime)after completionHandler:(void (^)(BOOL))completion {
    self.seekCalls+=1;
    NSInvocation *inv=PV2PhotosChecks::Invoke(self.wrapper,@"seekToTime:toleranceBefore:toleranceAfter:completionHandler:");
    [inv setArgument:&time atIndex:2]; [inv setArgument:&before atIndex:3];
    [inv setArgument:&after atIndex:4]; [inv setArgument:&completion atIndex:5]; [inv invoke];
}
@end

// Read-only observer: no replacement implementation, swizzling, or fake events.
@interface PV2PhotosRateEvents : NSObject
@property(nonatomic) NSUInteger delivered;
@end
@implementation PV2PhotosRateEvents
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context {
    if (context==(__bridge void *)self) {
        dispatch_async(dispatch_get_main_queue(),^{ self.delivered+=1; });
    } else [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
}
@end

static NSDictionary *PV2RunPhotosWrapperChecks(UIViewController *root, BOOL requireNativeBridge) {
    using namespace PV2PhotosChecks;
    checks=[NSMutableArray array];
    report=[@{@"schema":@1,@"completed":@NO,@"passed":@NO,@"checks":checks,
              @"runtime":UIDevice.currentDevice.systemVersion,@"realPhotosWrapper":@YES,
              @"bridgeRequired":@(requireNativeBridge),@"bridgeTested":@NO,
              @"sessionTested":@NO,@"intentTested":@NO,@"fullAcceptance":@NO,@"physicalTouchTested":@NO,
              @"sessionNote":@"Raw wrapper coverage. PXVideoSession nil-provider lifecycle and desiredPlayState are not assumed."} mutableCopy];
    __block AVPlayer *player=nil;
    id wrapper=nil;
    id session=nil;
    NSObject *presenterToken=[NSObject new];
    void *sessionPresenter=(__bridge void*)presenterToken;
    BOOL enteredPresentation=NO;
    PV2NativeTimelineViewController *host=nil;
    PV2PhotosRateEvents *events=[PV2PhotosRateEvents new];
    BOOL observing=NO;
    @try {
        Check(NSThread.isMainThread,@"harness runs on main thread");
        NSOperatingSystemVersion os=NSProcessInfo.processInfo.operatingSystemVersion;
        Check(os.majorVersion==17 && os.minorVersion==5,@"required iOS 17.5 runtime",UIDevice.currentDevice.systemVersion);
        const char *path="/System/Library/PrivateFrameworks/PhotosPlayer.framework/PhotosPlayer";
        void *image=dlopen(path,RTLD_NOW|RTLD_LOCAL);
        const char *error=image ? NULL : dlerror();
        Check(image!=NULL,@"dlopen real PhotosPlayer framework",error ? @(error) : @(path));
        Check(dlopen("/System/Library/Frameworks/AVKit.framework/AVKit",RTLD_NOW|RTLD_LOCAL)!=NULL,@"load actual AVKit");
        Class cls=NSClassFromString(@"ISWrappedAVPlayer");
        Check(cls!=Nil,@"real ISWrappedAVPlayer class available");
        ABI(cls,@"_initWithAVPlayer:","@24@0:8@16");
        ABI(cls,@"_performPlayerTransaction:","v24@0:8@?16");
        ABI(cls,@"_playerQueue_avPlayer","@16@0:8");
        ABI(cls,@"setRate:","v20@0:8f16"); ABI(cls,@"rate","f16@0:8");
        ABI(cls,@"pause","v16@0:8");
        ABI(cls,@"replaceCurrentItemWithPlayerItem:","v24@0:8@16");
        ABI(cls,@"seekToTime:toleranceBefore:toleranceAfter:completionHandler:","v96@0:8{?=qiIq}16{?=qiIq}40{?=qiIq}64@?88");
        NSURL *url=MakeAsset();
        player=[[PV2PhotosObservedPlayer alloc] initWithURL:url];
        Check(Wait(10,^BOOL { return player.currentItem.status!=AVPlayerItemStatusUnknown; }) && player.currentItem.status==AVPlayerItemStatusReadyToPlay,@"real player ready",player.currentItem.error.localizedDescription);
        Check(fabs(CMTimeGetSeconds(player.currentItem.duration)-6)<.05,@"six-second real asset");
        // NSInvocation cannot express ARC init-family ownership for this private
        // underscore initializer. Use an exact ABI-validated typed init message.
        wrapper=[(id<PV2RealWrapperInitializer>)[cls alloc] _initWithAVPlayer:player];
        Check(wrapper && [wrapper isKindOfClass:cls],@"initialize real wrapper over supplied player");
        Transaction(wrapper,^(AVPlayer *live) { Check(live==player,@"wrapper uses supplied AVPlayer"); });
        // ISWrappedAVPlayer publishes proxy/change-observer events, not a
        // documented KVO rate contract. Verify real wrapper/raw convergence.
        (void)events;
        SeekAndWait(wrapper,.25);

        SetRate(wrapper,1); Converged(wrapper,player,1);

        double start=CMTimeGetSeconds(player.currentTime);
        Check(Wait(2,^BOOL { return CMTimeGetSeconds(player.currentTime)>start+.15; }),@"real playback advances");

        Pause(wrapper); Converged(wrapper,player,0);

        SetRate(wrapper,2); Converged(wrapper,player,2);
        Pause(wrapper); Converged(wrapper,player,0);
        // Exercise repeated end-to-start loop seeks using wrapper's queue and
        // verify actual decoded timing, not cached UI state.
        for (NSUInteger loop=0;loop<3;loop++) {
            SeekAndWait(wrapper,5.75); SeekAndWait(wrapper,.2);
            Converged(wrapper,player,0);
        }
        // Replace via the wrapper, retain the old item only for identity checks.
        // No time observer or seek command is ever installed on that old item.
        AVPlayerItem *old=player.currentItem;
        AVPlayerItem *fresh=[AVPlayerItem playerItemWithURL:url];
        NSInvocation *replace=Invoke(wrapper,@"replaceCurrentItemWithPlayerItem:");
        [replace setArgument:&fresh atIndex:2]; [replace invoke];
        Check(Wait(10,^BOOL { return player.currentItem==fresh && fresh.status==AVPlayerItemStatusReadyToPlay; }),@"wrapper installs replacement current item");
        Transaction(wrapper,^(AVPlayer *live) { Check(live.currentItem==fresh && live.currentItem!=old,@"queue fetches fresh item after replacement"); });
        SeekAndWait(wrapper,2.25); SeekAndWait(wrapper,.2);
        report[@"rawWrapperPassed"]=@YES;
        if (requireNativeBridge) {
            PV2PhotosCommandProbe *probe=[PV2PhotosCommandProbe new]; probe.wrapper=wrapper;
            host=[[PV2NativeTimelineViewController alloc] initWithPlayer:player];
            __block NSUInteger temporaryCalls=0;
            __block double persistentIntent=0;
            host.playbackRateRequested=^(double rate,BOOL temporary) {
                if (temporary) temporaryCalls++; else persistentIntent=rate;
                if (rate==0) [probe pause]; else [probe setRate:(float)rate];
            };
            host.seekRequested=^(CMTime target,CMTime before,CMTime after,void (^completed)(BOOL)) {
                probe.seekCalls+=1;
                Transaction(wrapper,^(AVPlayer *live) {
                    Check(live==player && live.currentItem==player.currentItem && live.currentItem!=old && live.currentItem.status==AVPlayerItemStatusReadyToPlay,@"seek bridge fetches live replacement item");
                });
                PV2PhotosNativeSeek(wrapper,player,target,before,after,completed);
            };
            Check(host.playbackRateRequested && host.seekRequested,@"production callbacks installed");
            [root addChildViewController:host];
            host.view.frame=CGRectMake(12,CGRectGetHeight(root.view.bounds)-160,CGRectGetWidth(root.view.bounds)-24,88);
            [root.view addSubview:host.view]; [host didMoveToParentViewController:root];
            Check(Wait(12,^BOOL { return host.timelineView && host.slider && host.timelineView.window; }),@"production native host attached",host.failureReason);
            id owner=host.childViewControllers.firstObject;
            id controller=Object(owner,@"playerController");
            Check([controller isKindOfClass:NSClassFromString(@"AVPlayerController")],@"production native AVPlayerController subclass");
            Check([controller class]!=NSClassFromString(@"AVPlayerController"),@"dedicated production command controller subclass");
            Check(Object(controller,@"player")==player,@"native controller binds live raw player for observation");
            NSUInteger calls=probe.rateCalls;
            SetRate(controller,1); // ACTUAL native controller entry point.
            Check(Wait(3,^BOOL { return probe.rateCalls>calls; }) && fabsf(probe.lastRate-1)<.02f,@"native setRate routes through wrapper command bridge");
            Converged(wrapper,player,1);
            calls=probe.rateCalls; NSUInteger pauses=probe.pauseCalls;
            SetRate(controller,0);
            Check(Wait(3,^BOOL { return probe.pauseCalls>pauses || (probe.rateCalls>calls && probe.lastRate==0); }),@"native pause routes through wrapper bridge");
            Converged(wrapper,player,0);
            calls=probe.seekCalls;
            NSUInteger actualSeeks=((PV2PhotosObservedPlayer *)player).exactSeekCalls;
            NSInvocation *nativeSeek=Invoke(controller,@"seekToCMTime:toleranceBefore:toleranceAfter:");
            CMTime target=CMTimeMakeWithSeconds(3.25,600),zero=kCMTimeZero;
            [nativeSeek setArgument:&target atIndex:2]; [nativeSeek setArgument:&zero atIndex:3]; [nativeSeek setArgument:&zero atIndex:4];
            [nativeSeek invoke];
            Check(Wait(8,^BOOL { return probe.seekCalls>calls && fabs(CMTimeGetSeconds(player.currentTime)-3.25)<.15; }),@"native seek completes through actual wrapper queue");
            Check(probe.seekCalls==calls+1,@"one native seek bridge request");
            Wait(.25,^BOOL { return NO; });
            Check(((PV2PhotosObservedPlayer *)player).exactSeekCalls==actualSeeks+1,@"exactly one real seek with no raw superclass duplicate");
            Transaction(wrapper,^(AVPlayer *live) {
                Check(live.currentItem==fresh && fabs(CMTimeGetSeconds(live.currentTime)-3.25)<.15,@"native seek targets fresh queue item");
            });
            Converged(wrapper,player,0);
            SeekAndWait(wrapper,.2); SetRate(controller,1); Converged(wrapper,player,1);
            double intent=persistentIntent; NSUInteger temporaryBefore=temporaryCalls;
            ABI([controller class],@"beginPlaybackSuspension","v16@0:8");
            ABI([controller class],@"endPlaybackSuspension","v16@0:8");
            [Invoke(controller,@"beginPlaybackSuspension") invoke];
            Converged(wrapper,player,0);
            Check(temporaryCalls>temporaryBefore && persistentIntent==intent,@"native suspension pauses wrapper without changing recorded explicit callback rate");
            [Invoke(controller,@"endPlaybackSuspension") invoke];
            Converged(wrapper,player,1);
            Check(persistentIntent==intent,@"native suspension restores playback and preserves recorded explicit callback rate");
            SetRate(controller,0); Converged(wrapper,player,0);
            report[@"temporaryRateCalls"]=@(temporaryCalls);
            report[@"bridgeTested"]=@YES;
            report[@"bridgeCalls"]=@{@"rate":@(probe.rateCalls),@"pause":@(probe.pauseCalls),@"seek":@(probe.seekCalls)};
        }
        if (requireNativeBridge) {
            Check(dlopen("/System/Library/PrivateFrameworks/PhotosUICore.framework/PhotosUICore",RTLD_NOW|RTLD_LOCAL)!=NULL,@"load actual PhotosUICore");
            Class sessionClass=NSClassFromString(@"PXVideoSession");
            ABI(sessionClass,@"initWithContentProvider:videoPlayer:","@32@0:8@16@24");
            ABI(sessionClass,@"performChanges:withPresentationContext:presenter:","v40@0:8@?16q24^v32");
            ABI(sessionClass,@"desiredPlayState","q16@0:8");
            ABI(sessionClass,@"setDesiredPlayState:","v24@0:8q16");
            Class providerClass=NSClassFromString(@"PXVideoContentProvider");
            Class resultClass=NSClassFromString(@"PXVideoContentProviderLoadingResult");
            ABI(providerClass,@"setLoadingResult:","v24@0:8@16");
            ABI(resultClass,@"initWithPlayerItem:url:timeRange:timeRangeMapper:error:priority:description:","@112@0:8@16@24{?={?=qiIq}{?=qiIq}}32@80@88q96@104");
            id<PV2RealContentProvider> provider=(id)[[providerClass alloc] init];
            CMTimeRange fullRange=CMTimeRangeMake(kCMTimeZero,CMTimeMake(6,1));
            id loaded=[(id<PV2RealContentProvider>)[resultClass alloc] initWithPlayerItem:fresh url:url timeRange:fullRange timeRangeMapper:nil error:nil priority:0 description:@"real H264 fixture"];
            Check(provider && loaded,@"create actual Photos content provider and loading result");
            [provider performChanges:^(id change){[(id<PV2RealContentProvider>)change setLoadingResult:loaded];}];
            session=[(id<PV2RealSessionInitializer>)[sessionClass alloc] initWithContentProvider:provider videoPlayer:wrapper];
            Check(Wait(5,^BOOL { return player.currentItem==fresh && fresh.status==AVPlayerItemStatusReadyToPlay; }),@"session content provider retains the actual tested media item");
            Check(session!=nil,@"actual Photos session initialized with supplied wrapper");
            Check(Wait(5,^BOOL { return Object(session,@"videoPlayer")==wrapper; }),@"session owns same real wrapper");
            Check(Wait(5,^BOOL { return isfinite(NativeNumber(Object(host.childViewControllers.firstObject,@"playerController"),@"maxTime")); }),@"native range ready after actual session media binding");
            ABI(sessionClass,@"enterPresentationContext:presenter:","v32@0:8q16^v24");
            [(id<PV2RealSessionInitializer>)session enterPresentationContext:1 presenter:sessionPresenter];
            enteredPresentation=YES;
            report[@"sessionTested"]=@YES;
            host.playbackRateRequested=^(double rate,BOOL temporary) { PV2PhotosRequestRateWithContext(session,wrapper,rate,temporary,1,sessionPresenter); };
            id controller=Object(host.childViewControllers.firstObject,@"playerController");
            SetRate(controller,1);
            Check(Wait(5,^BOOL { return [session desiredPlayState]==1 && player.rate>0 && Rate(wrapper)>0; }),@"native play updates actual Photos session intent and both rates");
            SetRate(controller,0);
            Check(Wait(5,^BOOL { return [session desiredPlayState]==0 && player.rate==0 && Rate(wrapper)==0; }),@"native pause updates actual Photos session intent and both rates");
            SetRate(controller,1);
            Check(Wait(5,^BOOL { return [session desiredPlayState]==1 && player.rate>0; }),@"Photos intent starts playback again");
            [Invoke(controller,@"beginPlaybackSuspension") invoke];
            Check(Wait(3,^BOOL { return [session desiredPlayState]==1 && player.rate==0; }),@"native drag suspension preserves actual session intent");
            [session performChanges:^(id change){[change setDesiredPlayState:0];} withPresentationContext:1 presenter:sessionPresenter];
            Check(Wait(3,^BOOL { return [session desiredPlayState]==0; }),@"Photos own pause while dragging changes authoritative intent");
            [Invoke(controller,@"endPlaybackSuspension") invoke];
            Check(Wait(3,^BOOL { return player.rate==0 && Rate(wrapper)==0; }),@"native drag end does not override Photos own pause");
            // Exact user symptom: native progress must agree with actual time,
            // not just accept a seek completion on a different clock.
            NativeValue(host.slider,0.7565);
            BOOL reaches=Wait(6,^BOOL { return fabs(CMTimeGetSeconds(player.currentTime)-4.539)<.18; });
            Check(reaches,@"native slider75.65percent seeks same actual playback time",@{@"time":@(CMTimeGetSeconds(player.currentTime)),@"itemPresent":@(player.currentItem!=nil),@"originalItem":@(player.currentItem==fresh),@"itemStatus":@(player.currentItem.status),@"duration":@(CMTimeGetSeconds(player.currentItem.duration)),@"nativeMin":@(NativeNumber(controller,@"minTime")),@"nativeMax":@(NativeNumber(controller,@"maxTime")),@"seekCalls":@(((PV2PhotosObservedPlayer *)player).exactSeekCalls),@"sliderValue":@(NativeNumber(host.slider,@"value"))});
            Check(Wait(3,^BOOL { return fabs(NativeNumber(host.slider,@"value")-CMTimeGetSeconds(player.currentTime)/6)<.025; }),@"native slider ratio agrees with real player clock");
            ABI(cls,@"setLoopingEnabled:","v20@0:8B16");
            ABI(cls,@"isLoopingEnabled","B16@0:8");
            [session performChanges:^(id state){ [(id<PV2RealSessionInitializer>)state setLoopingEnabled:YES]; } withPresentationContext:1 presenter:sessionPresenter];
            Check(Wait(5,^BOOL {return [(id<PV2PhotosWrapperCommands>)wrapper isLoopingEnabled];}),@"session presenter enables real wrapper looping");
            Transaction(wrapper,^(AVPlayer *live){
                CMTimeRange range=live.currentItem.loopTimeRange;
                Check(CMTIMERANGE_IS_VALID(range) && CMTimeGetSeconds(range.duration)>5.9,@"actual Photos wrapper installs native loop range");
            });
            BOOL loopReady=Wait(5,^BOOL { return player.currentItem.status==AVPlayerItemStatusReadyToPlay && host.slider.enabled && fabs(NativeNumber(controller,@"maxTime")-6)<.05; });
            Check(loopReady,@"native range ready after session loop change",[NSString stringWithFormat:@"item=%@ status=%ld enabled=%d min=%g max=%g duration=%g rate=%g",player.currentItem,(long)player.currentItem.status,host.slider.enabled,NativeNumber(controller,@"minTime"),NativeNumber(controller,@"maxTime"),CMTimeGetSeconds(player.currentItem.duration),player.rate]);
            for (NSUInteger lap=0;lap<2;lap++) {
                SetRate(controller,0);
                __block NSString *clockState=@"";
                Transaction(wrapper,^(AVPlayer *live){
                    AVPlayerItem *it=live.currentItem;
                    CMTimebaseRef f=it.timebase, u=[it _copyProxyUnfoldedTimebase];
                    CMTime req=CMTimeMakeWithSeconds(5.4,600), m=PV2LivePlaybackSeekCoordinate(it,req);
                    clockState=[NSString stringWithFormat:@"folded=%@ unfolded=%@ rates=%g/%g effective=%g/%g mapped=%@ current=%@",f?(__bridge_transfer NSString*)CMTimeCopyDescription(NULL,CMTimebaseGetTime(f)):@"nil",u?(__bridge_transfer NSString*)CMTimeCopyDescription(NULL,CMTimebaseGetTime(u)):@"nil",f?CMTimebaseGetRate(f):NAN,u?CMTimebaseGetRate(u):NAN,f?CMTimebaseGetEffectiveRate(f):NAN,u?CMTimebaseGetEffectiveRate(u):NAN,(__bridge_transfer NSString*)CMTimeCopyDescription(NULL,m),(__bridge_transfer NSString*)CMTimeCopyDescription(NULL,it.currentTime)];
                    if (u) CFRelease(u);
                });
                NativeValue(host.slider,0.9);
                BOOL reached=Wait(5,^BOOL { return fabs(CMTimeGetSeconds(player.currentTime)-5.4)<.2; });
                Check(reached,@"paused native scrub reaches requested point in looping item",@{@"lap":@(lap),@"actual":@(CMTimeGetSeconds(player.currentTime)),@"clocks":clockState,@"nativeMin":@(NativeNumber(controller,@"minTime")),@"nativeMax":@(NativeNumber(controller,@"maxTime")),@"sliderValue":@(NativeNumber(host.slider,@"value")),@"enabled":@(host.slider.enabled),@"actualSeekCount":@(((PV2PhotosObservedPlayer*)player).exactSeekCalls)});
                SetRate(controller,1);
                Check(Wait(5,^BOOL { double t=CMTimeGetSeconds(player.currentTime);return t>=0 && t<1.3; }),@"native Photos loop naturally returns to beginning",@{@"lap":@(lap),@"actual":@(CMTimeGetSeconds(player.currentTime)),@"rate":@(player.rate),@"loops":@([(id<PV2PhotosWrapperCommands>)wrapper isLoopingEnabled])});
                SetRate(controller,0);
                NativeValue(host.slider,0.6);
                Check(Wait(5,^BOOL { return fabs(CMTimeGetSeconds(player.currentTime)-3.6)<.2; }),@"native scrub after actual loop controls displayed playback",@{@"lap":@(lap),@"actual":@(CMTimeGetSeconds(player.currentTime))});
                Check(Wait(3,^BOOL { return fabs(NativeNumber(host.slider,@"value")-0.6)<.025; }),@"looped native slider progress agrees with target");
            }
            report[@"actualLoopTested"]=@YES;
            report[@"intentTested"]=@YES; report[@"fullAcceptance"]=@YES;
            report[@"sessionNote"]=@"Real PXVideoSession + ISWrappedAVPlayer + AVKit command bridge; no Photos one-up UI or physical touch.";
        }
        report[@"partialCoveragePassed"]=@YES;
        Check(!requireNativeBridge || [report[@"intentTested"] boolValue],@"full acceptance requires real Photos session intent coverage");
        report[@"passed"]=@YES;
    } @catch (NSException *exception) {
        report[@"exception"]=@{@"name":exception.name,@"reason":exception.reason ?: @"",@"stack":exception.callStackSymbols ?: @[]};
    } @finally {
        (void)observing;
        if (host) { [host invalidate]; [host willMoveToParentViewController:nil]; [host.view removeFromSuperview]; [host removeFromParentViewController]; }
        if (enteredPresentation) [(id<PV2RealSessionInitializer>)session leavePresentationContext:1 presenter:sessionPresenter];
        if (player) [player pause];
        report[@"completed"]=@YES;
    }
    return [report copy];
}

#ifndef PV2_PHOTOS_WRAPPER_NO_MAIN
@interface PV2PhotosHarness : UIResponder <UIApplicationDelegate>
@property(nonatomic,strong) UIWindow *window;
@end
@implementation PV2PhotosHarness
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    self.window=[[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController=[UIViewController new];
    self.window.rootViewController.view.backgroundColor=UIColor.blackColor;
    [self.window makeKeyAndVisible];
    [self performSelector:@selector(run) withObject:nil afterDelay:.2];
    return YES;
}
- (void)run {
    BOOL rawOnly=[NSProcessInfo.processInfo.arguments containsObject:@"--raw-wrapper-only"];
    NSDictionary *result=PV2RunPhotosWrapperChecks(self.window.rootViewController,!rawOnly);
    NSError *error=nil;
    NSData *json=nil;
    @try { json=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:&error]; }
    @catch (NSException *exception) {
        NSDictionary *fallback=@{@"completed":@YES,@"passed":@NO,@"serializationFailure":exception.reason ?: @"",@"details":[result description]};
        json=[NSJSONSerialization dataWithJSONObject:fallback options:NSJSONWritingPrettyPrinted error:&error];
    }
    [json writeToFile:PV2PhotosChecks::Docs(@"result.json") options:NSDataWritingAtomic error:&error];
    NSString *text=[NSString stringWithFormat:@"%@ real PhotosPlayer wrapper; bridgeTested=%@ rawOnly=%@\n%@\n",[result[@"passed"] boolValue] ? @"PASS" : @"FAIL",result[@"bridgeTested"],@(rawOnly),result];
    [text writeToFile:PV2PhotosChecks::Docs(@"result.txt") atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSLog(@"PHOTOSWRAPPER COMPLETED %@ writeError=%@",result[@"passed"],error);
}
@end
int main(int argc,char **argv) {
    @autoreleasepool { return UIApplicationMain(argc,argv,nil,NSStringFromClass(PV2PhotosHarness.class)); }
}
#endif
