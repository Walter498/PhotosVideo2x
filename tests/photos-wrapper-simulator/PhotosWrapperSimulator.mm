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
#import <pthread.h>
#import <time.h>
#import <unistd.h>
#import <fcntl.h>
#import <limits.h>
#import "NativeTimeline.h"
#import "PhotosAuthority.h"
#import "PhotosChromePolicyTests.h"

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
static NSURL *MakeAsset(NSUInteger frameCount=180,NSString *name=@"six-seconds.mp4") {
    NSURL *url=[NSURL fileURLWithPath:Docs(name)];
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
    NSError *error=nil;
    AVAssetWriter *writer=[[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeMPEG4 error:&error];
    Check(writer!=nil,@"create real AVAssetWriter",error.localizedDescription);
    AVAssetWriterInput *input=[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:@{AVVideoCodecKey:AVVideoCodecTypeH264,AVVideoWidthKey:@320,AVVideoHeightKey:@180}];
    input.expectsMediaDataInRealTime=NO;
    AVAssetWriterInputPixelBufferAdaptor *adaptor=[AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input sourcePixelBufferAttributes:@{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32ARGB),(id)kCVPixelBufferWidthKey:@320,(id)kCVPixelBufferHeightKey:@180,(id)kCVPixelBufferIOSurfacePropertiesKey:@{}}];
    [writer addInput:input]; Check([writer startWriting],@"start H264 encoding",writer.error.localizedDescription);
    [writer startSessionAtSourceTime:kCMTimeZero];
    for (NSUInteger frame=0;frame<frameCount;frame++) {
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

namespace PV2PhotosChecks {
static NSDictionary *TimeRecord(CMTime t) {
    double seconds=CMTimeGetSeconds(t);
    return @{ @"value":@(t.value), @"timescale":@(t.timescale), @"flags":@(t.flags),
              @"epoch":@(t.epoch), @"seconds":isfinite(seconds) ? (id)@(seconds) : @"nonfinite" };
}
static CMTimeRange ObservedLoopRange(AVPlayerItem *item) {
    Method m=item ? class_getInstanceMethod(item.class,@selector(loopTimeRange)):NULL;
    if (!m || strcmp(method_getTypeEncoding(m),"{?={?=qiIq}{?=qiIq}}16@0:8")) return kCMTimeRangeInvalid;
    CMTimeRange r=item.loopTimeRange;
    return CMTIMERANGE_IS_VALID(r) && CMTIME_IS_NUMERIC(r.start) &&
        CMTIME_IS_NUMERIC(r.duration) && r.duration.value>0 ? r : kCMTimeRangeInvalid;
}
static BOOL NativeBool(id object,NSString *name);
static void NativeObjectAction(id object,NSString *name,id argument);
static void SessionIntent(PV2NativeTimelineViewController *host,id session,void *presenter,BOOL playing);
static void RequestNativeIntent(PV2NativeTimelineViewController *host,id controller,id session,BOOL playing);
static double Monotonic();
static NSDictionary *PV2ResolverSnapshot(id controller, double sliderValue) {
    Ivar ivar=class_getInstanceVariable([controller class],"_timeResolver");
    id resolver=ivar ? object_getIvar(controller,ivar) : nil;
    NSMutableDictionary *s=[@{ @"slider":@(sliderValue), @"resolverPresent":@(resolver!=nil) } mutableCopy];
    for (NSString *name in @[ @"currentTime", @"minTime", @"maxTime" ]) {
        CMTime value=kCMTimeInvalid;
        NSMethodSignature *sig=resolver ? [resolver methodSignatureForSelector:NSSelectorFromString(name)] : nil;
        if (sig && !strcmp(sig.methodReturnType,@encode(CMTime)) && sig.numberOfArguments==2) {
            NSInvocation *inv=[NSInvocation invocationWithMethodSignature:sig]; inv.target=resolver; inv.selector=NSSelectorFromString(name); [inv invoke]; [inv getReturnValue:&value];
            s[name]=TimeRecord(value);
        } else s[name]=@"unavailable";
    }
    CMTime interval=kCMTimeInvalid;
    NSMethodSignature *intervalSig=resolver ? [resolver methodSignatureForSelector:@selector(interval)] : nil;
    if (intervalSig && !strcmp(intervalSig.methodReturnType,@encode(CMTime))) {
        NSInvocation *inv=[NSInvocation invocationWithMethodSignature:intervalSig]; inv.target=resolver; inv.selector=@selector(interval); [inv invoke]; [inv getReturnValue:&interval];
        s[@"timing"]=TimeRecord(interval); s[@"interval"]=TimeRecord(interval);
    } else {
        id timing=(resolver && [resolver respondsToSelector:@selector(timing)]) ? Object(resolver,@"timing") : nil;
        s[@"timing"] = timing ? [timing description] : @"unavailable";
    }
    return s;
}
static void NativeLoopContinuity(PV2NativeTimelineViewController *host,id session,id wrapper,AVPlayer *player,id controller,void *presenter,double duration,NSString *label) {
    id observed=player;
    UIView *playback=host.playbackControlsView;
    NSMutableArray *samples=[NSMutableArray array];
    [session performChanges:^(id state){ [(id<PV2RealSessionInitializer>)state setLoopingEnabled:YES]; } withPresentationContext:1 presenter:presenter];
    Check(Wait(5,^BOOL { return [(id<PV2PhotosWrapperCommands>)wrapper isLoopingEnabled] && player.currentItem && host.slider.enabled && fabs(NativeNumber(controller,@"maxTime")-duration)<.08; }),[label stringByAppendingString:@" loop-ready native range"]);
    SessionIntent(host,session,presenter,NO);
    Check(Wait(3,^BOOL { return player.rate==0 && [session desiredPlayState]==0; }),[label stringByAppendingString:@" starts paused"]);
    NativeValue(host.slider,.03);
    Check(Wait(5,^BOOL { return fabs(CMTimeGetSeconds(player.currentTime)-.18)<.15 && NativeNumber(host.slider,@"value")<.15; }),[label stringByAppendingString:@" starts from explicit near-head native slider position"],@{ @"currentTime":@(CMTimeGetSeconds(player.currentTime)), @"slider":@(NativeNumber(host.slider,@"value")), @"rate":@(player.rate), @"seeks":@([[observed valueForKey:@"exactSeekCalls"] unsignedIntegerValue]), @"currentItem": player.currentItem.description ?: @"nil" });
    NSUInteger seeks=[[observed valueForKey:@"exactSeekCalls"] unsignedIntegerValue];
    // This is the real AVKit button action, not a direct rate/timing update.
    NativeObjectAction(controller,@"togglePlayback:",playback);
    Check(Wait(3,^BOOL { return player.rate>0 && [session desiredPlayState]==1 && !NativeBool(playback,@"playPauseButtonShowsPlayIcon"); }),[label stringByAppendingString:@" native button starts intent and icon"]);
    double previous=NAN; NSUInteger wraps=0; BOOL postWrapAdvance=NO;
    double deadline=Monotonic()+duration*3.0+12.0;
    while (wraps<3 && Monotonic()<deadline) {
        double now=CMTimeGetSeconds(player.currentTime); double slider=NativeNumber(host.slider,@"value");
        NSDictionary *sample=PV2ResolverSnapshot(controller,slider); NSMutableDictionary *record=[sample mutableCopy]; record[@"playerCurrentTime"]=@(now); record[@"wallTime"]=@(Monotonic()); record[@"wrap"] = @(wraps); [samples addObject:record];
        if (isfinite(previous) && previous>duration*.72 && now<duration*.28) {
            wraps++; record[@"wrap"] = @(wraps);
            double before=now; double beforeSlider=slider; double beforeWall=Monotonic();
            __block double settled=NAN; __block double settledSlider=NAN;
            NSDictionary *beforeSnapshot=PV2ResolverSnapshot(controller,beforeSlider);
            Check(Wait(.5,^BOOL { settled=CMTimeGetSeconds(player.currentTime); settledSlider=NativeNumber(host.slider,@"value"); return isfinite(settled) && isfinite(settledSlider) && fabs(settledSlider-(settled/duration))<.08 && (settled>before+.08 || settled<before-.08); }),[label stringByAppendingFormat:@" wrap%lu clock and slider continue after settling",(unsigned long)wraps],@{ @"before":@(before), @"after":@(settled), @"beforeSlider":@(beforeSlider), @"afterSlider":@(settledSlider), @"elapsed":@(Monotonic()-beforeWall), @"beforeResolver":beforeSnapshot, @"afterResolver":PV2ResolverSnapshot(controller,settledSlider) });
            NSDictionary *settledRecord=PV2ResolverSnapshot(controller,settledSlider); NSMutableDictionary *sr=[settledRecord mutableCopy]; sr[@"playerCurrentTime"]=@(settled); sr[@"wrap"]=@(wraps); sr[@"postWrap"] = @YES; [samples addObject:sr]; postWrapAdvance=YES;
        }
        previous=now;
        Wait(.12,^BOOL { return NO; });
    }
    Check(wraps>=3 && postWrapAdvance,[label stringByAppendingString:@" observes three natural wraps and continued timeline motion"],@{ @"wraps":@(wraps), @"samples":samples });
    Check([[observed valueForKey:@"exactSeekCalls"] unsignedIntegerValue]==seeks,[label stringByAppendingString:@" natural loop performs no exact seek"],@{ @"before":@(seeks), @"after":@([[observed valueForKey:@"exactSeekCalls"] unsignedIntegerValue]), @"samples":samples });
    RequestNativeIntent(host,controller,session,NO);
    double pausedTime=CMTimeGetSeconds(player.currentTime), pausedSlider=NativeNumber(host.slider,@"value");
    Check(Wait(.8,^BOOL { return player.rate==0 && fabs(CMTimeGetSeconds(player.currentTime)-pausedTime)<.06 && fabs(NativeNumber(host.slider,@"value")-pausedSlider)<.02; }),[label stringByAppendingString:@" pause leaves slider stable"] ,@{ @"time":@(pausedTime), @"slider":@(pausedSlider), @"samples":samples });
}
static BOOL LegalTolerance(CMTime t) {
    // AVFoundation permits +infinity (unbounded tolerance), never -infinity,
    // indefinite, invalid, a negative finite duration, or a nonzero epoch.
    return t.epoch==0 && (CMTIME_IS_POSITIVE_INFINITY(t) ||
        (CMTIME_IS_NUMERIC(t) && t.timescale>0 && CMTimeCompare(t,kCMTimeZero)>=0));
}
}

// Real decoding/seeking is retained except for an explicitly armed, one-shot
// TEST injection. This is not evidence of the crashing iOS17.3 request values.
@interface PV2PhotosObservedPlayer : AVPlayer
@property(atomic) NSUInteger exactSeekCalls;
@property(atomic) NSUInteger illegalSeekCalls;
@property(atomic) NSUInteger injectedExceptions;
@property(atomic) BOOL throwNextSeek;
@property(atomic) BOOL lastSeekWasMainThread;
@property(atomic) CMTime recordedTarget;
@property(atomic) CMTime recordedBefore;
@property(atomic) CMTime recordedAfter;
- (NSDictionary *)seekObservation;
@end
@implementation PV2PhotosObservedPlayer
- (NSDictionary *)seekObservation {
    @synchronized(self) {
        return @{ @"calls":@(self.exactSeekCalls), @"illegalCalls":@(self.illegalSeekCalls),
                  @"injectedExceptions":@(self.injectedExceptions), @"onMain":@(self.lastSeekWasMainThread),
                  @"target":PV2PhotosChecks::TimeRecord(self.recordedTarget),
                  @"before":PV2PhotosChecks::TimeRecord(self.recordedBefore),
                  @"after":PV2PhotosChecks::TimeRecord(self.recordedAfter) };
    }
}
- (void)seekToTime:(CMTime)time toleranceBefore:(CMTime)before toleranceAfter:(CMTime)after completionHandler:(void (^)(BOOL))completion {
    BOOL inject=NO;
    @synchronized(self) {
        self.exactSeekCalls+=1; self.recordedTarget=time; self.recordedBefore=before; self.recordedAfter=after;
        self.lastSeekWasMainThread=NSThread.isMainThread;
        BOOL legal=CMTIME_IS_NUMERIC(time) && time.timescale>0 &&
            PV2PhotosChecks::LegalTolerance(before) && PV2PhotosChecks::LegalTolerance(after);
        CMTimeRange loop=PV2PhotosChecks::ObservedLoopRange(self.currentItem);
        if (legal && CMTIMERANGE_IS_VALID(loop)) {
            CMTime folded=CMTimeCompare(time,loop.start)>=0 ? CMTimeFoldIntoRange(time,loop) : time;
            CMTime allowance=CMTimeSubtract(CMTimeSubtract(CMTimeRangeGetEnd(loop),folded),CMTimeMake(1,1000));
            legal=CMTIME_IS_NUMERIC(allowance) && allowance.epoch==0 && allowance.value>=0;
        }
        if (!legal) self.illegalSeekCalls+=1;
        inject=self.throwNextSeek;
        if (inject) { self.throwNextSeek=NO; self.injectedExceptions+=1; }
    }
    if (inject) @throw [NSException exceptionWithName:NSInvalidArgumentException
        reason:@"TEST ONLY: injected player-queue seek exception, not reproduced device arguments" userInfo:nil];
    [super seekToTime:time toleranceBefore:before toleranceAfter:after completionHandler:completion];
}
@end

// Deterministic fault-path fixture ONLY: the first main hop guarantees that
// PV2PhotosNativeSeek has returned before its transaction runs off-main. This
// proves the async catch boundary even when the real wrapper uses main queue.
// It does NOT claim ISWrappedAVPlayer/ISAVPlayer queue integration coverage.
@interface PV2PhotosDeferredTransactionFixture : NSObject
@property(nonatomic,strong) AVPlayer *player;
@property(nonatomic,strong) dispatch_queue_t queue;
@end
@implementation PV2PhotosDeferredTransactionFixture
- (void)_performPlayerTransaction:(void (^)(id))transaction {
    dispatch_async(dispatch_get_main_queue(),^{
        dispatch_async(self.queue,^{ transaction(self); });
    });
}
- (AVPlayer *)_playerQueue_avPlayer { return self.player; }
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

@protocol PV2PhotosIntentHost <NSObject>
@property(nonatomic,copy) void (^playbackToggleRequested)(void);
@property(nonatomic,copy) NSInteger (^playbackIntent)(void);
- (void)refreshPlaybackIntent;
@end

// Stress instrumentation is test-only: methods on this harness's dedicated
// compact classes forward the saved actual IMP; Apple/KVO classes are untouched.
namespace PV2PhotosChecks {
static std::atomic<uint64_t> stressLayouts{0}, stressVisibility{0}, heartbeats{0};
static std::atomic<double> lastHeartbeat{0}, watchdogDeadline{0};
static std::atomic<bool> watchdogRunning{false};
static char watchdogPath[PATH_MAX];
static double Monotonic() { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static void *Watchdog(void *) {
    while (watchdogRunning.load()) {
        struct timespec interval={0,100000000}; nanosleep(&interval,NULL);
        double now=Monotonic(), lag=now-lastHeartbeat.load();
        if (watchdogRunning.load() && (lag>4 || now>watchdogDeadline.load())) {
            // No main queue, UIKit, report mutation or runloop needed to persist
            // evidence and exit a wedged AVKit visibility/layout animation.
            char text[1024]; int count=snprintf(text,sizeof(text),
                "{\"completed\":true,\"passed\":false,\"watchdog\":true,\"heartbeatLag\":%.3f,\"heartbeats\":%llu,\"layoutCalls\":%llu,\"visibilityCalls\":%llu,\"reason\":\"main heartbeat stalled or stress deadline exceeded\"}\n",
                lag,(unsigned long long)heartbeats.load(),(unsigned long long)stressLayouts.load(),(unsigned long long)stressVisibility.load());
            int fd=open(watchdogPath,O_CREAT|O_TRUNC|O_WRONLY,0600);
            if (fd>=0) { write(fd,text,(size_t)count); fsync(fd); close(fd); }
            // Leave a short sampling window for the macOS runner. A completed
            // failure is already durable; no main-thread cooperation is needed.
            struct timespec sampleWindow={12,0}; nanosleep(&sampleWindow,NULL);
            _exit(124);
        }
    }
    return NULL;
}
class LivenessGuard {
    dispatch_source_t timer;
    pthread_t thread;
public:
    LivenessGuard():timer(nil) {
        strlcpy(watchdogPath,[Docs(@"result.json") fileSystemRepresentation],sizeof(watchdogPath));
        heartbeats=0; lastHeartbeat=Monotonic(); watchdogDeadline=Monotonic()+150;
        watchdogRunning=true;
        int error=pthread_create(&thread,NULL,Watchdog,NULL);
        if (error) { watchdogRunning=false; Check(NO,@"independent watchdog thread starts",@(error)); }
        timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
        dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,0),100*NSEC_PER_MSEC,5*NSEC_PER_MSEC);
        dispatch_source_set_event_handler(timer,^{ lastHeartbeat=Monotonic(); heartbeats++; });
        dispatch_resume(timer);
    }
    ~LivenessGuard() { watchdogRunning=false; dispatch_source_cancel(timer); pthread_join(thread,NULL); }
};
static BOOL NativeBool(id object,NSString *name) {
    NSInvocation *inv=Invoke(object,name);
    Check(inv.methodSignature.numberOfArguments==2 && !strcmp(inv.methodSignature.methodReturnType,@encode(BOOL)),@"native BOOL getter ABI",name);
    BOOL value=NO; [inv invoke]; [inv getReturnValue:&value]; return value;
}
static void NativeObjectAction(id object,NSString *name,id argument) {
    ABI(object_getClass(object),name,"v24@0:8@16");
    NSInvocation *inv=Invoke(object,name); [inv setArgument:&argument atIndex:2]; [inv invoke];
}
static void ObserveCalls(id object,NSString *selectorName,BOOL objectArgument) {
    Class base=object_getClass(object); SEL selector=NSSelectorFromString(selectorName);
    // Do not isa-subclass NSKVONotifying classes. Their class/teardown machinery
    // depends on their immediate superclass; adding a layer makes -class nil on
    // the real AVKit owner and invalidates KVO (observed in the first run).
    while ([NSStringFromClass(base) hasPrefix:@"NSKVONotifying_"]) base=class_getSuperclass(base);
    Check([NSStringFromClass(base) hasPrefix:@"PV2"],@"instrument only this harness's dedicated compact classes",NSStringFromClass(base));
    ABI(base,selectorName,objectArgument ? "v24@0:8@16" : "v16@0:8");
    Method method=class_getInstanceMethod(base,selector);
    IMP actual=method_getImplementation(method);
    IMP replacement=objectArgument ? imp_implementationWithBlock(^(id obj,id animated) {
        stressVisibility++; ((void(*)(id,SEL,id))actual)(obj,selector,animated);
    }) : imp_implementationWithBlock(^(id obj) {
        stressLayouts++; ((void(*)(id,SEL))actual)(obj,selector);
    });
    // Add on our dedicated subclass when inherited; otherwise replace only its
    // own method. No Apple class or KVO-generated class is modified.
    if (!class_addMethod(base,selector,replacement,method_getTypeEncoding(method)))
        class_replaceMethod(base,selector,replacement,method_getTypeEncoding(method));
}
static BOOL Visible(UIView *view) {
    if (!view.window || CGRectIsEmpty(view.bounds)) return NO;
    for (UIView *v=view;v;v=v.superview) if (v.hidden || v.alpha<.01) return NO;
    return YES;
}
static void SessionIntent(PV2NativeTimelineViewController *host,id session,void *presenter,BOOL playing) {
    // Session toolbar-equivalent fixture ONLY: not the real Photos browsing
    // controller/blue toolbar, whose authoritative desiredPlayState is separate.
    [session performChanges:^(id state){ [state setDesiredPlayState:playing ? 1 : 0]; }
        withPresentationContext:1 presenter:presenter];
    [(id<PV2PhotosIntentHost>)host refreshPlaybackIntent];
}
static void RequestNativeIntent(PV2NativeTimelineViewController *host,id controller,id session,BOOL playing) {
    if ([session desiredPlayState]!=(playing ? 1 : 0))
        NativeObjectAction(controller,@"togglePlayback:",host.playbackControlsView);
    else [(id<PV2PhotosIntentHost>)host refreshPlaybackIntent];
}
static void AssertIntent(PV2NativeTimelineViewController *host,id controller,id session,id wrapper,AVPlayer *player,BOOL playing) {
    UIView *playback=host.playbackControlsView;
    Check(Wait(1.2,^BOOL {
        return [session desiredPlayState]==(playing ? 1 : 0) &&
            NativeBool(controller,@"isPlaying")==playing &&
            NativeBool(playback,@"playPauseButtonShowsPlayIcon")==!playing &&
            (playing ? player.rate>0 && Rate(wrapper)>0 : player.rate==0 && Rate(wrapper)==0);
    }),@"session/native playing getter/genuine icon/rates agree",@{@"expectedPlaying":@(playing),@"time":@(CMTimeGetSeconds(player.currentTime))});
    BOOL visibleImmediately=Visible(playback) && Visible(host.slider) && host.slider.enabled;
    BOOL visibleSettled=Wait(.8,^BOOL { return Visible(playback) && Visible(host.slider) && host.slider.enabled; });
    NSMutableArray *geometry=[NSMutableArray array];
    for (UIView *v=playback;v;v=v.superview) [geometry addObject:@{@"class":NSStringFromClass(v.class),@"hidden":@(v.hidden),@"alpha":@(v.alpha),@"bounds":NSStringFromCGRect(v.bounds),@"window":@(v.window!=nil)}];
    Check(visibleSettled,@"native controls remain attached visible and enabled",@{@"immediate":@(visibleImmediately),@"playbackHierarchy":geometry,@"sliderHidden":@(host.slider.hidden),@"sliderAlpha":@(host.slider.alpha),@"sliderEnabled":@(host.slider.enabled)});
    if (!visibleImmediately) report[@"deferredVisibilitySettles"]=@([report[@"deferredVisibilitySettles"] unsignedIntegerValue]+1);
}
static void AssertIdle(PV2NativeTimelineViewController *host,id controller) {
    PV2NativeCommandBridge *bridge=objc_getAssociatedObject(controller,PV2NativeCommandKey);
    Check(bridge!=nil,@"observe actual production command bridge");
    Check(Wait(1.2,^BOOL { return !host.isScrubbing && !NativeBool(controller,@"isScrubbing") &&
        !NativeBool(controller,@"isSeeking") && !NativeBool(controller,@"isSeekingInternal") && bridge.suspensionDepth==0 &&
        !bridge.seekInFlight && !bridge.hasPendingSeek && Object(controller,@"scrubCoordinatorSuspension")==nil;
    }),@"scrubbing seeking native suspension and pending commands drain to zero",
        @{@"suspensionDepth":@(bridge.suspensionDepth),@"seekInFlight":@(bridge.seekInFlight),@"pending":@(bridge.hasPendingSeek)});
}
// Native slider events/delegates, not a call straight into the seek helper.
static void EndpointPosition(PV2NativeTimelineViewController *host,id controller,id wrapper,
                             AVPlayer *player,double duration,double ratio,NSString *label) {
    AssertIdle(host,controller); Converged(wrapper,player,0);
    CMTimeRange range=ObservedLoopRange(player.currentItem);
    double expected=duration*ratio;
    if (ratio==1 && CMTIMERANGE_IS_VALID(range))
        expected=CMTimeGetSeconds(CMTimeSubtract(CMTimeRangeGetEnd(range),CMTimeMake(1,1000)));
    Check(Wait(2,^BOOL {
        return fabs(CMTimeGetSeconds(player.currentTime)-expected)<.075 &&
            fabs(NativeNumber(host.slider,@"value")-ratio)<.02;
    }),[label stringByAppendingString:@" actual paused endpoint and slider agree"],
        @{ @"expected":@(expected), @"actual":@(CMTimeGetSeconds(player.currentTime)),
           @"ratio":@(NativeNumber(host.slider,@"value")), @"duration":@(duration) });
    // A transient sample at the end of a natural loop cannot pass this check.
    // Ordinary media keeps the exact duration. A looping item uses end-1ms
    // (5.999s on the six-second fixture); next-lap zero must NEVER pass.
    double deadline=Monotonic()+.25;
    do {
        double actual=CMTimeGetSeconds(player.currentTime);
        Check(isfinite(actual) && actual>=-.005 && fabs(actual-expected)<.075 &&
            player.rate==0 && fabs(NativeNumber(host.slider,@"value")-ratio)<.02,
            [label stringByAppendingString:@" remains stable while paused"],@(actual));
        Wait(.04,^BOOL { return NO; });
    } while (Monotonic()<deadline);
    AssertIdle(host,controller);
}
static void NativeEndpoints(PV2NativeTimelineViewController *host,id session,id wrapper,
                            PV2PhotosObservedPlayer *player,void *presenter,double duration,NSString *asset) {
    watchdogDeadline=Monotonic()+100;
    id owner=host.childViewControllers.firstObject, controller=Object(owner,@"playerController");
    NSMutableArray *cases=[NSMutableArray array];
    for (NSUInteger mode=0;mode<4;mode++) {
        BOOL looping=mode>=2, playing=(mode%2)==1;
        SessionIntent(host,session,presenter,NO); AssertIdle(host,controller);
        [session performChanges:^(id state){ [(id<PV2RealSessionInitializer>)state setLoopingEnabled:looping]; }
            withPresentationContext:1 presenter:presenter];
        Check(Wait(3,^BOOL { return [(id<PV2PhotosWrapperCommands>)wrapper isLoopingEnabled]==looping &&
            host.slider.enabled && fabs(NativeNumber(controller,@"maxTime")-duration)<.05; }),
            @"endpoint mode and native duration ready",@{ @"asset":asset,@"mode":@(mode) });
        // Three independent visits to 100%, retreat, and actual replay.
        for (NSUInteger cycle=0;cycle<3;cycle++) {
            NSString *label=[NSString stringWithFormat:@"%@ loop=%d playing=%d cycle=%lu",asset,looping,playing,(unsigned long)cycle];
            SessionIntent(host,session,presenter,NO);
            NativeValue(host.slider,.35); AssertIdle(host,controller);
            SessionIntent(host,session,presenter,playing);
            AssertIntent(host,controller,session,wrapper,player,playing);
            NSUInteger seeks=player.exactSeekCalls;
            NativeObjectAction(owner,@"sliderDidBeginTracking:",host.slider);
            Check(Wait(.5,^BOOL { return host.isScrubbing; }),@"endpoint real delegate begins tracking",label);
            NativeValue(host.slider,1.0);
            Check(Wait(3,^BOOL { return player.exactSeekCalls>seeks; }),@"slider100% enters production native seek",label);
            // Pause BEFORE releasing tracking and before measuring the end.
            // This covers a playing-start gesture without racing a live loop.
            SessionIntent(host,session,presenter,NO);
            NativeObjectAction(owner,@"sliderDidEndTracking:",host.slider);
            AssertIntent(host,controller,session,wrapper,player,NO);
            EndpointPosition(host,controller,wrapper,player,duration,1,label);
            NSDictionary *end=player.seekObservation;
            Check(player.illegalSeekCalls==0,@"actual slider endpoint emitted no illegal CMTime arguments",end);
            NativeObjectAction(owner,@"sliderDidBeginTracking:",host.slider);
            NativeValue(host.slider,.4);
            NativeObjectAction(owner,@"sliderDidEndTracking:",host.slider);
            EndpointPosition(host,controller,wrapper,player,duration,.4,label);
            RequestNativeIntent(host,controller,session,YES);
            AssertIntent(host,controller,session,wrapper,player,YES);
            double start=CMTimeGetSeconds(player.currentTime);
            Check(Wait(1.5,^BOOL { return CMTimeGetSeconds(player.currentTime)>start+.08; }),
                @"retreat from100% resumes real advancing playback",label);
            // Test 0% from the same paused/playing starting-intent matrix.
            SessionIntent(host,session,presenter,playing);
            NativeObjectAction(owner,@"sliderDidBeginTracking:",host.slider);
            seeks=player.exactSeekCalls; NativeValue(host.slider,0.0);
            Check(Wait(3,^BOOL { return player.exactSeekCalls>seeks; }),@"slider0% enters production native seek",label);
            SessionIntent(host,session,presenter,NO);
            NativeObjectAction(owner,@"sliderDidEndTracking:",host.slider);
            AssertIntent(host,controller,session,wrapper,player,NO);
            EndpointPosition(host,controller,wrapper,player,duration,0,label);
            [cases addObject:@{ @"mode":@(mode),@"cycle":@(cycle),@"endSeek":end,
                               @"startSeek":player.seekObservation,@"actual":@(CMTimeGetSeconds(player.currentTime)) }];
        }
    }
    NSMutableDictionary *results=report[@"nativeEndpoints"];
    if (!results) { results=[NSMutableDictionary dictionary]; report[@"nativeEndpoints"]=results; }
    results[asset]=@{ @"passed":@YES,@"duration":@(duration),@"cases":cases,
        @"endpointPolicy":@"nonloop exact duration; loop end-1ms (5.999s for six-second asset), never next-lap0",
        @"source":@"real AVKit slider valueChanged and original tracking delegates; not physical touch" };
}
static void NativeSeekArguments(id wrapper,PV2PhotosObservedPlayer *player) {
    Pause(wrapper); Converged(wrapper,player,0);
    struct Case { const char *name; CMTime target,before,after; BOOL mustSucceed; };
    CMTime middle=CMTimeMake(3,1), negative=CMTimeMake(-1,600);
    const Case cases[]={
        {"valid exact",middle,kCMTimeZero,kCMTimeZero,YES},
        {"negative before",middle,negative,kCMTimeZero,NO},
        {"negative after",middle,kCMTimeZero,negative,NO},
        {"invalid before",middle,kCMTimeInvalid,kCMTimeZero,NO},
        {"invalid after",middle,kCMTimeZero,kCMTimeInvalid,NO},
        {"indefinite before",middle,kCMTimeIndefinite,kCMTimeZero,NO},
        {"indefinite after",middle,kCMTimeZero,kCMTimeIndefinite,NO},
        {"negative infinity before",middle,kCMTimeNegativeInfinity,kCMTimeZero,NO},
        {"negative infinity after",middle,kCMTimeZero,kCMTimeNegativeInfinity,NO},
        {"positive infinity before",middle,kCMTimePositiveInfinity,kCMTimeZero,YES},
        {"positive infinity after",middle,kCMTimeZero,kCMTimePositiveInfinity,YES},
        {"positive infinity both",middle,kCMTimePositiveInfinity,kCMTimePositiveInfinity,YES},
        {"negative target",CMTimeMake(-1,1),kCMTimeZero,kCMTimeZero,NO},
        {"beyond duration target",CMTimeMake(8,1),kCMTimeZero,kCMTimeZero,NO},
        {"invalid target",kCMTimeInvalid,kCMTimeZero,kCMTimeZero,NO},
        {"positive infinite target",kCMTimePositiveInfinity,kCMTimeZero,kCMTimeZero,NO},
        {"negative infinite target",kCMTimeNegativeInfinity,kCMTimeZero,kCMTimeZero,NO},
        {"indefinite target",kCMTimeIndefinite,kCMTimeZero,kCMTimeZero,NO}
    };
    NSMutableArray *results=[NSMutableArray array];
    for (const Case &test:cases) {
        NSUInteger calls=player.exactSeekCalls, illegal=player.illegalSeekCalls;
        __block NSUInteger completions=0; __block BOOL finished=NO;
        PV2PhotosNativeSeek(wrapper,player,test.target,test.before,test.after,^(BOOL ok){
            dispatch_async(dispatch_get_main_queue(),^{ completions++; finished=ok; });
        });
        Check(Wait(5,^BOOL { return completions>0; }),@"argument case reports success or explicit rejection",@(test.name));
        Transaction(wrapper,^(AVPlayer *live){ Check(live==player,@"argument case still uses live player"); });
        Wait(.05,^BOOL { return NO; });
        NSUInteger emitted=player.exactSeekCalls-calls;
        Check(completions==1 && emitted<=1 && player.illegalSeekCalls==illegal,
            @"production emits only legal parameters and completes exactly once",
            @{ @"case":@(test.name),@"finished":@(finished),@"emitted":@(emitted),@"observed":player.seekObservation });
        // Invalid/negative durations must be rejected, not silently changed.
        // +infinity is a legal duration and must remain supported.
        BOOL reject=!CMTIME_IS_NUMERIC(test.target) || !LegalTolerance(test.before) || !LegalTolerance(test.after);
        Check(reject ? (!finished && emitted==0) : (finished && emitted==1),
            @"invalid arguments reject before AVPlayer; legal inputs remain seekable",@(test.name));
        if (emitted && CMTIME_IS_POSITIVE_INFINITY(test.before))
            Check(CMTIME_IS_POSITIVE_INFINITY(player.recordedBefore),@"positive infinity before is preserved");
        if (emitted && CMTIME_IS_POSITIVE_INFINITY(test.after))
            Check(CMTIME_IS_POSITIVE_INFINITY(player.recordedAfter),@"positive infinity after is preserved");
        if (test.mustSucceed) Check(finished && emitted==1,@"ordinary finite exact seek remains supported");
        if (emitted) {
            CMTime t=player.recordedTarget;
            Check(CMTIME_IS_NUMERIC(t) && CMTimeCompare(t,kCMTimeZero)>=0 &&
                CMTimeCompare(t,player.currentItem.duration)<=0,@"nonloop emitted target is within actual item duration",player.seekObservation);
            // Exact finite seeks can be checked against their normalized
            // target; an unbounded tolerance may legally choose a keyframe.
            if (CMTimeCompare(player.recordedBefore,kCMTimeZero)==0 &&
                CMTimeCompare(player.recordedAfter,kCMTimeZero)==0)
                Check(fabs(CMTimeGetSeconds(player.currentTime)-CMTimeGetSeconds(t))<.075,
                    @"normalized exact seek reaches real media position",player.seekObservation);
        }
        [results addObject:@{ @"case":@(test.name),@"inputTarget":TimeRecord(test.target),
            @"inputBefore":TimeRecord(test.before),@"inputAfter":TimeRecord(test.after),
            @"policy":emitted ? @"accepted/normalized" : @"explicitly rejected",
            @"finished":@(finished),@"emitted":@(emitted),@"output":emitted ? (id)player.seekObservation : @"no AVPlayer call" }];
    }
    report[@"seekArgumentCases"]=results;
}
static void NativeSeekExceptionRecovery(PV2NativeTimelineViewController *host,id session,id wrapper,
                                        PV2PhotosObservedPlayer *player,void *presenter) {
    id owner=host.childViewControllers.firstObject, controller=Object(owner,@"playerController");
    SessionIntent(host,session,presenter,NO); AssertIntent(host,controller,session,wrapper,player,NO);
    AssertIdle(host,controller);
    void (^original)(CMTime,CMTime,CMTime,void (^)(BOOL))=[host.seekRequested copy];
    __block NSUInteger failed=0,completed=0,submitted=0;
    NSMutableArray *deliveries=[NSMutableArray array];
    PV2PhotosDeferredTransactionFixture *deferred=[PV2PhotosDeferredTransactionFixture new];
    deferred.player=player; deferred.queue=dispatch_queue_create("PV2.test.deferred-seek",DISPATCH_QUEUE_SERIAL);
    NSUInteger injected=player.injectedExceptions;
    @try {
        host.seekRequested=^(CMTime t,CMTime b,CMTime a,void (^done)(BOOL)){
            NSUInteger index=submitted++; [deliveries addObject:@0];
            // No test catch. On the first request only, a test transaction
            // fixture guarantees execution after the submitting call returns.
            PV2PhotosNativeSeek(index==0 ? (id)deferred : wrapper,player,t,b,a,^(BOOL ok){
                dispatch_async(dispatch_get_main_queue(),^{
                    deliveries[index]=@([deliveries[index] unsignedIntegerValue]+1);
                    completed++; if (!ok) failed++; done(ok);
                });
            });
        };
        player.throwNextSeek=YES;
        NativeObjectAction(owner,@"sliderDidBeginTracking:",host.slider);
        NativeValue(host.slider,.55);
        Check(Wait(4,^BOOL { return failed==1; }),@"production player-queue catch delivers completionNO for injected exception");
        Check(!player.lastSeekWasMainThread && [deliveries.firstObject unsignedIntegerValue]==1,
            @"injection ran off-main after submission and delivered exactly oneNO",player.seekObservation);
        NativeObjectAction(owner,@"sliderDidEndTracking:",host.slider);
        AssertIdle(host,controller); AssertIntent(host,controller,session,wrapper,player,NO);
        Check(player.injectedExceptions==injected+1 && !player.throwNextSeek,
            @"exactly one test-only NSInvalidArgumentException was injected",player.seekObservation);
        NSUInteger calls=completed;
        NativeObjectAction(owner,@"sliderDidBeginTracking:",host.slider);
        NativeValue(host.slider,.4);
        NativeObjectAction(owner,@"sliderDidEndTracking:",host.slider);
        EndpointPosition(host,controller,wrapper,player,6,.4,@"post-exception recovery");
        Check(completed>calls && failed==1,@"later real native seek succeeds after rejected asynchronous seek");
        RequestNativeIntent(host,controller,session,YES); AssertIntent(host,controller,session,wrapper,player,YES);
        double start=CMTimeGetSeconds(player.currentTime);
        Check(Wait(2,^BOOL { return CMTimeGetSeconds(player.currentTime)>start+.1; }),@"playback advances after exception recovery");
        RequestNativeIntent(host,controller,session,NO); AssertIntent(host,controller,session,wrapper,player,NO);
        AssertIdle(host,controller);
        report[@"injectedSeekException"]=@{ @"passed":@YES,@"failedCompletions":@(failed),@"completions":@(completed),
            @"provenance":@"TEST ONLY subclass injection; not reproduction or proof of iOS17.3 original target/tolerances",
            @"observed":player.seekObservation };
    } @finally { player.throwNextSeek=NO; host.seekRequested=original; }
}
static void Stress(PV2NativeTimelineViewController *host,id session,id wrapper,AVPlayer *player,void *presenter) {
    watchdogDeadline=Monotonic()+90; // burst + 60s sustained scrub + idle sampling
    double started=Monotonic(); uint64_t beatsBefore=heartbeats.load();
    id owner=host.childViewControllers.firstObject;
    id controller=Object(owner,@"playerController");
    UIView *playback=host.playbackControlsView;
    Check([playback isKindOfClass:NSClassFromString(@"AVMobileChromelessPlaybackControlsView")],@"genuine AVKit playback view, not a reconstructed icon");
    // Selectors/encodings come from evidence/photos-binding-abis.json and
    // evidence/objc-metadata.txt (AVMobileChromelessPlaybackControlsView).
    ABI(object_getClass(controller),@"togglePlayback:","v24@0:8@16");
    for (NSString *getter in @[@"isPlaying",@"isScrubbing",@"isSeeking"]) ABI(object_getClass(controller),getter,"B16@0:8");
    ABI(object_getClass(playback),@"playPauseButtonShowsPlayIcon","B16@0:8");
    ABI(object_getClass(playback),@"showsPlayPauseButton","B16@0:8");
    ABI(object_getClass(playback),@"playPauseButtonWasPressed","v16@0:8");
    ABI(object_getClass(owner),@"sliderDidBeginTracking:","v24@0:8@16");
    ABI(object_getClass(owner),@"sliderDidEndTracking:","v24@0:8@16");
    ABI(object_getClass(controller),@"scrubCoordinatorSuspension","@16@0:8");
    Check(NativeBool(playback,@"showsPlayPauseButton"),@"genuine play pause button is included");
    Check(Object(host.slider,@"delegate")==owner,@"real native slider delegate remains installed");
    ObserveCalls(((UIViewController *)owner).view,@"layoutSubviews",NO);
    ObserveCalls(playback,@"layoutSubviews",NO);
    ObserveCalls(owner,@"_updateControlsVisibilityStateIfNeededAnimated:",YES);
    stressLayouts=0; stressVisibility=0;
    // Registered NSNumber-taking boundary. Its internal animated implementation
    // is objc_direct on iOS17.3 and must never be invoked as an ObjC message.
    NativeObjectAction(owner,@"_updateControlsVisibilityStateIfNeededAnimated:",@YES);
    [((UIViewController *)owner).view setNeedsLayout];
    [((UIViewController *)owner).view layoutIfNeeded];
    Check(stressLayouts.load()>0 && stressVisibility.load()>0,@"actual compact layout and AVKit visibility implementations were exercised");
    report[@"stressEventSource"]=@"programmatic native actions and original slider tracking delegates; NOT physical touch";
    SessionIntent(host,session,presenter,NO); AssertIntent(host,controller,session,wrapper,player,NO);
    for (NSUInteger cycle=0;cycle<4;cycle++) {
        // Direct real controller action, NOT setRate: masquerading as a button.
        NativeObjectAction(controller,@"togglePlayback:",playback);
        AssertIntent(host,controller,session,wrapper,player,YES);
        // Invoke the real view's registered button implementation in the other
        // direction as well; AVKit's actual delegate must route the pause.
        [Invoke(playback,@"playPauseButtonWasPressed") invoke];
        AssertIntent(host,controller,session,wrapper,player,NO);
        SessionIntent(host,session,presenter,YES); AssertIntent(host,controller,session,wrapper,player,YES);
        SessionIntent(host,session,presenter,NO); AssertIntent(host,controller,session,wrapper,player,NO);
    }
    NSUInteger updates=0, batches=0;
    // Four separate tracking transactions: paused/playing, non-looping/looping.
    // Each has 24 alternating valueChanged events (96 total), queued seek
    // overlap, and explicit opposing intents before async completions drain.
    for (NSUInteger batch=0;batch<4;batch++) {
        BOOL looping=batch>=2, initiallyPlaying=(batch%2)==1;
        [session performChanges:^(id state){ [(id<PV2RealSessionInitializer>)state setLoopingEnabled:looping]; }
            withPresentationContext:1 presenter:presenter];
        Check(Wait(1.2,^BOOL { return [(id<PV2PhotosWrapperCommands>)wrapper isLoopingEnabled]==looping && host.slider.enabled && fabs(NativeNumber(controller,@"maxTime")-6)<.05; }),@"stress loop mode and native media range ready",@(batch));
        SessionIntent(host,session,presenter,initiallyPlaying);
        AssertIntent(host,controller,session,wrapper,player,initiallyPlaying);
        NativeObjectAction(owner,@"sliderDidBeginTracking:",host.slider);
        Check(Wait(.4,^BOOL { return host.isScrubbing; }),@"actual tracking delegate enters scrubbing",@(batch));
        for (NSUInteger step=0;step<24;step++) {
            NativeValue(host.slider,(step%2) ? .12+.002*step : .88-.002*step); updates++;
            if (step==6 || step==14) {
                // Enqueue after the seek request but before waiting for its
                // completion: Photos pause and actual native play compete.
                dispatch_async(dispatch_get_main_queue(),^{ SessionIntent(host,session,presenter,NO); });
                dispatch_async(dispatch_get_main_queue(),^{ NativeObjectAction(controller,@"togglePlayback:",playback); });
            }
            if (step%6==5) Wait(.025,^BOOL { return NO; });
        }
        // All batches deliberately finish paused: otherwise the real clock
        // advances and a stale seek can hide behind the positional tolerance.
        // Alternate the final command source to test both authority directions.
        SessionIntent(host,session,presenter,YES);
        if (batch%2) NativeObjectAction(controller,@"togglePlayback:",playback);
        else SessionIntent(host,session,presenter,NO);
        double finalRatio=.31+.08*batch;
        NativeValue(host.slider,finalRatio); updates++;
        NativeObjectAction(owner,@"sliderDidEndTracking:",host.slider); batches++;
        AssertIdle(host,controller);
        AssertIntent(host,controller,session,wrapper,player,NO);
        Check(Wait(1.2,^BOOL { return fabs(CMTimeGetSeconds(player.currentTime)-6*finalRatio)<.15 && fabs(NativeNumber(host.slider,@"value")-finalRatio)<.025; }),@"latest native seek reaches exact displayed coordinate after overlapping seeks",@{@"batch":@(batch),@"target":@(6*finalRatio),@"actual":@(CMTimeGetSeconds(player.currentTime))});
        Wait(.12,^BOOL { return NO; });
        AssertIdle(host,controller); AssertIntent(host,controller,session,wrapper,player,NO);
    }
    // Sustained physical-time soak as well as the burst above. Exercise many
    // separate gestures, changing explicit intent while each gesture is live.
    // This is still programmatic tracking, not an injected physical touch.
    double soakStart=Monotonic(); NSUInteger soakUpdates=0;
    while (Monotonic()-soakStart<60) {
        SessionIntent(host,session,presenter,YES);
        AssertIntent(host,controller,session,wrapper,player,YES);
        NativeObjectAction(owner,@"sliderDidBeginTracking:",host.slider);
        for (NSUInteger step=0;step<80 && Monotonic()-soakStart<60;step++) {
            NativeValue(host.slider,step%2 ? .13+.001*step : .87-.001*step);
            updates++; soakUpdates++;
            if (step==20) SessionIntent(host,session,presenter,NO);
            if (step==40) RequestNativeIntent(host,controller,session,YES);
            Wait(.05,^BOOL { return NO; });
        }
        RequestNativeIntent(host,controller,session,YES);
        NativeObjectAction(owner,@"sliderDidEndTracking:",host.slider); batches++;
        AssertIdle(host,controller);
        AssertIntent(host,controller,session,wrapper,player,YES);
    }
    double soakSeconds=Monotonic()-soakStart;
    Check(soakSeconds>=60 && soakUpdates>=500,@"continuous 60-second real-time back-and-forth scrub stays responsive",@{@"seconds":@(soakSeconds),@"updates":@(soakUpdates)});
    // Opposite winner as well: native PLAY issued while a seek is pending must
    // survive the earlier Photos pause and the eventual suspension completion.
    NativeObjectAction(owner,@"sliderDidBeginTracking:",host.slider);
    NativeValue(host.slider,.47); updates++;
    SessionIntent(host,session,presenter,NO);
    NativeObjectAction(controller,@"togglePlayback:",playback);
    NativeObjectAction(owner,@"sliderDidEndTracking:",host.slider); batches++;
    AssertIdle(host,controller); AssertIntent(host,controller,session,wrapper,player,YES);
    Wait(.15,^BOOL { return NO; });
    AssertIntent(host,controller,session,wrapper,player,YES);
    [Invoke(playback,@"playPauseButtonWasPressed") invoke];
    AssertIntent(host,controller,session,wrapper,player,NO);
    NativeValue(host.slider,.47); updates++;
    AssertIdle(host,controller);
    Check(Wait(1.2,^BOOL { return fabs(CMTimeGetSeconds(player.currentTime)-2.82)<.15; }),@"exact final seek after latest-play-wins race",@(CMTimeGetSeconds(player.currentTime)));
    Check(updates>=80 && batches>=4,@"at least 80 back-and-forth native value changes across multiple batches",@{@"updates":@(updates),@"batches":@(batches)});
    // Allow pending animations to finish, then sample WITHOUT forcing layout.
    // The 0.5.2 invalidation loop must not be normalized away with a huge budget.
    Wait(.8,^BOOL { return NO; });
    NSMutableArray *idleSamples=[NSMutableArray array];
    uint64_t previousLayouts=stressLayouts.load(),previousVisibility=stressVisibility.load();
    for (NSUInteger sample=0;sample<3;sample++) {
        Wait(.35,^BOOL { return NO; });
        uint64_t layouts=stressLayouts.load(),visibility=stressVisibility.load();
        [idleSamples addObject:@{@"layouts":@(layouts-previousLayouts),@"visibility":@(visibility-previousVisibility)}];
        Check(layouts-previousLayouts<=2 && visibility-previousVisibility<=1,@"idle native layout and visibility settle instead of animation/layout livelock",idleSamples.lastObject);
        if (sample==2) Check(layouts==previousLayouts && visibility==previousVisibility,@"final stable idle sample has zero continuing layout or visibility growth",idleSamples.lastObject);
        previousLayouts=layouts; previousVisibility=visibility;
    }
    // Actual button response AFTER idle: merely attached or unhidden is not
    // sufficient to accept a frozen page. Also test native hit-testing geometry.
    CGPoint point=CGPointMake(CGRectGetMidX(playback.bounds),CGRectGetMidY(playback.bounds));
    Check(Visible(playback) && Visible(host.slider) && [playback hitTest:point withEvent:nil]!=nil,@"attached native controls remain visible and hit-testable after stress");
    NativeObjectAction(controller,@"togglePlayback:",playback); AssertIntent(host,controller,session,wrapper,player,YES);
    [Invoke(playback,@"playPauseButtonWasPressed") invoke]; AssertIntent(host,controller,session,wrapper,player,NO);
    AssertIdle(host,controller);
    Check(heartbeats.load()-beatsBefore>=10,@"real main dispatch timer continues firing during stress",@(heartbeats.load()-beatsBefore));
    report[@"stress"]=@{@"passed":@YES,@"updates":@(updates),@"soakSeconds":@(soakSeconds),@"soakUpdates":@(soakUpdates),@"batches":@(batches),@"seconds":@(Monotonic()-started),@"heartbeats":@(heartbeats.load()-beatsBefore),@"layoutCalls":@(stressLayouts.load()),@"visibilityCalls":@(stressVisibility.load()),@"idleSamples":idleSamples};
    watchdogDeadline=Monotonic()+40; // existing cleanup/loop checks remain guarded
}
} // namespace PV2PhotosChecks

static NSDictionary *PV2RunPhotosWrapperChecks(UIViewController *root, BOOL requireNativeBridge) {
    using namespace PV2PhotosChecks;
    checks=[NSMutableArray array];
    report=[@{@"schema":@1,@"completed":@NO,@"passed":@NO,@"checks":checks,
              @"runtime":UIDevice.currentDevice.systemVersion,@"realPhotosWrapper":@YES,
              @"bridgeRequired":@(requireNativeBridge),@"bridgeTested":@NO,
              @"sessionTested":@NO,@"intentTested":@NO,@"fullAcceptance":@NO,@"physicalTouchTested":@NO,
              @"sessionNote":@"Raw wrapper coverage. PXVideoSession nil-provider lifecycle and desiredPlayState are not assumed."} mutableCopy];
    LivenessGuard liveness;
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
            ABI(object_getClass(host),@"setPlaybackToggleRequested:","v24@0:8@?16");
            ABI(object_getClass(host),@"setPlaybackIntent:","v24@0:8@?16");
            ABI(object_getClass(host),@"refreshPlaybackIntent","v16@0:8");
            __weak PV2NativeTimelineViewController *intentHost=host;
            id<PV2PhotosIntentHost> intentAPI=(id)host;
            intentAPI.playbackIntent=^NSInteger { return [session desiredPlayState]; };
            intentAPI.playbackToggleRequested=^{
                SessionIntent(intentHost,session,sessionPresenter,[session desiredPlayState]!=1);
            };
            [intentAPI refreshPlaybackIntent];
            report[@"realBrowsingToolbarTested"]=@NO;
            report[@"intentFixture"]=@"PXVideoSession desiredPlayState substitutes for browsing authority; actual Photos browsing/blue toolbar NOT covered";
            // From the session phase onward exercise the exact nonblocking
            // production callback. The earlier identity probe used Transaction
            // + nested runloop waiting; that test-only waiter must not run
            // reentrantly inside a real slider callback under seek pressure.
            host.seekRequested=^(CMTime target,CMTime before,CMTime after,void (^completed)(BOOL)) {
                PV2PhotosNativeSeek(wrapper,player,target,before,after,completed);
            };
            host.playbackRateRequested=^(double rate,BOOL temporary) {
                Check(temporary,@"all low-level native rates are temporary, never explicit user intent");
                PV2PhotosRequestRateWithContext(session,wrapper,rate,YES,1,sessionPresenter);
            };
            id controller=Object(host.childViewControllers.firstObject,@"playerController");
            RequestNativeIntent(host,controller,session,YES);
            Check(Wait(5,^BOOL { return [session desiredPlayState]==1 && player.rate>0 && Rate(wrapper)>0; }),@"native play updates actual Photos session intent and both rates");
            RequestNativeIntent(host,controller,session,NO);
            Check(Wait(5,^BOOL { return [session desiredPlayState]==0 && player.rate==0 && Rate(wrapper)==0; }),@"native pause updates actual Photos session intent and both rates");
            RequestNativeIntent(host,controller,session,YES);
            Check(Wait(5,^BOOL { return [session desiredPlayState]==1 && player.rate>0; }),@"Photos intent starts playback again");
            [Invoke(controller,@"beginPlaybackSuspension") invoke];
            Check(Wait(3,^BOOL { return [session desiredPlayState]==1 && player.rate==0; }),@"native drag suspension preserves actual session intent");
            SessionIntent(host,session,sessionPresenter,NO);
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
            // Natural loop continuity is deliberately outside Stress: no seek,
            // toggle, timing update, or production refresh is performed while
            // the three wraps are sampled. The longer watchdog covers ~20s.
            watchdogDeadline=Monotonic()+150;
            NativeLoopContinuity(host,session,wrapper,player,controller,sessionPresenter,6,@"sixSecondsNaturalLoop");
            NativeEndpoints(host,session,wrapper,(PV2PhotosObservedPlayer *)player,sessionPresenter,6,@"sixSeconds");
            SessionIntent(host,session,sessionPresenter,NO);
            [session performChanges:^(id state){ [(id<PV2RealSessionInitializer>)state setLoopingEnabled:NO]; } withPresentationContext:1 presenter:sessionPresenter];
            Check(Wait(3,^BOOL { return !CMTIMERANGE_IS_VALID(ObservedLoopRange(player.currentItem)) && player.rate==0; }),@"argument tests use nonloop current item");
            NativeSeekArguments(wrapper,(PV2PhotosObservedPlayer *)player);
            NativeSeekExceptionRecovery(host,session,wrapper,(PV2PhotosObservedPlayer *)player,sessionPresenter);
            report[@"chromePolicy"]=PV2RunChromePolicyChecks(); // policy fixture, NOT full Photos UI
            Stress(host,session,wrapper,player,sessionPresenter);
            for (NSUInteger lap=0;lap<2;lap++) {
                RequestNativeIntent(host,controller,session,NO);
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
                RequestNativeIntent(host,controller,session,YES);
                Check(Wait(5,^BOOL { double t=CMTimeGetSeconds(player.currentTime);return t>=0 && t<1.3; }),@"native Photos loop naturally returns to beginning",@{@"lap":@(lap),@"actual":@(CMTimeGetSeconds(player.currentTime)),@"rate":@(player.rate),@"loops":@([(id<PV2PhotosWrapperCommands>)wrapper isLoopingEnabled])});
                RequestNativeIntent(host,controller,session,NO);
                NativeValue(host.slider,0.6);
                Check(Wait(5,^BOOL { return fabs(CMTimeGetSeconds(player.currentTime)-3.6)<.2; }),@"native scrub after actual loop controls displayed playback",@{@"lap":@(lap),@"actual":@(CMTimeGetSeconds(player.currentTime))});
                Check(Wait(3,^BOOL { return fabs(NativeNumber(host.slider,@"value")-0.6)<.025; }),@"looped native slider progress agrees with target");
            }
            // Repeat the exact endpoints AFTER two natural lap crossings;
            // unfolded/folded mapping must not turn100% into the next lap0.
            NativeEndpoints(host,session,wrapper,(PV2PhotosObservedPlayer *)player,sessionPresenter,6,@"sixSecondsAfterTwoLaps");
            SessionIntent(host,session,sessionPresenter,NO);
            [session performChanges:^(id state){ [(id<PV2RealSessionInitializer>)state setLoopingEnabled:NO]; } withPresentationContext:1 presenter:sessionPresenter];
            NSURL *shortURL=MakeAsset(24,@"short-eight-tenths.mp4");
            AVPlayerItem *shortItem=[AVPlayerItem playerItemWithURL:shortURL];
            id shortLoaded=[(id<PV2RealContentProvider>)[resultClass alloc] initWithPlayerItem:shortItem url:shortURL
                timeRange:CMTimeRangeMake(kCMTimeZero,CMTimeMake(24,30)) timeRangeMapper:nil error:nil priority:0 description:@"real short H264 endpoint fixture"];
            [provider performChanges:^(id change){ [(id<PV2RealContentProvider>)change setLoadingResult:shortLoaded]; }];
            Check(Wait(8,^BOOL { return player.currentItem==shortItem && shortItem.status==AVPlayerItemStatusReadyToPlay &&
                fabs(NativeNumber(controller,@"maxTime")-.8)<.05 && host.slider.enabled; }),@"actual session installs short clip and native duration");
            watchdogDeadline=Monotonic()+150;
            NativeLoopContinuity(host,session,wrapper,player,controller,sessionPresenter,.8,@"eightTenthsNaturalLoop");
            // A notification emitted by the detached item must not reach the
            // new host after replacement; this is intentionally notification
            // delivery only, not a production refresh or timing update.
            double detachedSlider=NativeNumber(host.slider,@"value"), detachedMax=NativeNumber(controller,@"maxTime");
            [[NSNotificationCenter defaultCenter] postNotificationName:AVPlayerItemTimeJumpedNotification object:fresh];
            [[NSNotificationCenter defaultCenter] postNotificationName:@"AVPlayerItemTimebaseChangedNotification" object:fresh];
            Check(Wait(.6,^BOOL { return player.currentItem==shortItem && fabs(NativeNumber(host.slider,@"value")-detachedSlider)<.02 && fabs(NativeNumber(controller,@"maxTime")-detachedMax)<.05; }),@"detached old-item notifications do not mutate new host",@{ @"slider":@(detachedSlider), @"max":@(detachedMax) });
            NativeEndpoints(host,session,wrapper,(PV2PhotosObservedPlayer *)player,sessionPresenter,.8,@"shortEightTenths");
            report[@"actualLoopTested"]=@YES;
            report[@"intentTested"]=@YES; report[@"fullAcceptance"]=@YES;
            report[@"sessionNote"]=@"Real PXVideoSession + ISWrappedAVPlayer + AVKit command bridge; no Photos one-up UI or physical touch.";
        }
        report[@"partialCoveragePassed"]=@YES;
        Check(!requireNativeBridge || ([report[@"intentTested"] boolValue] && [report[@"stress"][@"passed"] boolValue]),@"full acceptance requires actual session intent and native stress coverage");
        report[@"passed"]=@YES;
    } @catch (NSException *exception) {
        report[@"exception"]=@{@"name":exception.name,@"reason":exception.reason ?: @"",@"stack":exception.callStackSymbols ?: @[]};
        // Persist the original assertion before UIKit/session teardown. A second
        // failure during cleanup must not erase the first causal evidence.
        NSDictionary *failure=@{@"completed":@YES,@"passed":@NO,@"reason":exception.reason ?: @"",@"details":[report description]};
        NSData *data=[NSJSONSerialization dataWithJSONObject:failure options:0 error:nil];
        [data writeToFile:Docs(@"result.json") atomically:YES];
    } @finally {
        (void)observing;
        if (host) { [host invalidate]; [host willMoveToParentViewController:nil]; [host.view removeFromSuperview]; [host removeFromParentViewController]; }
        if (enteredPresentation) [(id<PV2RealSessionInitializer>)session leavePresentationContext:1 presenter:sessionPresenter];
        if (player) [player pause];
        report[@"liveness"]=@{@"heartbeats":@(heartbeats.load()),@"lastHeartbeatLag":@(Monotonic()-lastHeartbeat.load()),@"layoutCalls":@(stressLayouts.load()),@"visibilityCalls":@(stressVisibility.load()),@"stressDeadlineSeconds":@150,@"stallDeadlineSeconds":@4};
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
