#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import <AVKit/AVKit.h>
#import <CoreMedia/CoreMedia.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <math.h>
#import "NativeTimeline.h"

// This translation unit includes the production implementation, not a replica.
static NSMutableArray *checks;
static NSMutableDictionary *report;
static NSString *Docs(NSString *name) {
    return [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:name];
}
static void Check(BOOL ok, NSString *name, id detail) {
    [checks addObject:@{@"name":name, @"passed":@(ok), @"detail":detail ?: @""}];
    NSLog(@"PV2TEST %@ %@ %@", ok ? @"PASS" : @"FAIL", name, detail ?: @"");
    if (!ok) @throw [NSException exceptionWithName:@"PV2TestFailure" reason:name userInfo:nil];
}
static BOOL Wait(NSTimeInterval timeout, BOOL (^condition)(void)) {
    NSTimeInterval end = NSProcessInfo.processInfo.systemUptime + timeout;
    do {
        if (condition()) return YES;
        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.025]];
    } while (NSProcessInfo.processInfo.systemUptime < end);
    return condition();
}
static void Settle(double seconds) { Wait(seconds, ^BOOL { return NO; }); }
static BOOL Visible(UIView *v) {
    if (!v.window || CGRectIsEmpty(v.bounds)) return NO;
    for (UIView *p=v; p; p=p.superview) if (p.hidden || p.alpha <= 0.01) return NO;
    CGRect rect=[v convertRect:v.bounds toView:v.window];
    return CGRectIntersectsRect(rect, v.window.bounds);
}
static NSArray<UIView *> *Tree(UIView *root) {
    NSMutableArray *out=[NSMutableArray arrayWithObject:root];
    for (UIView *v in root.subviews) [out addObjectsFromArray:Tree(v)];
    return out;
}
// NSInvocation respects float/double ABI; no guessed objc_msgSend cast for sliders.
static NSInvocation *Invocation(id obj, NSString *name, NSUInteger argc) {
    SEL sel=NSSelectorFromString(name);
    NSMethodSignature *sig=[obj methodSignatureForSelector:sel];
    Check([obj respondsToSelector:sel] && sig.numberOfArguments==argc+2,
          [@"runtime signature " stringByAppendingString:name], NSStringFromClass([obj class]));
    NSInvocation *inv=[NSInvocation invocationWithMethodSignature:sig];
    inv.target=obj; inv.selector=sel;
    return inv;
}
static double Number(id obj, NSString *name) {
    NSInvocation *inv=Invocation(obj,name,0); [inv invoke];
    const char *t=inv.methodSignature.methodReturnType;
    if (!strcmp(t,@encode(float))) { float v=0; [inv getReturnValue:&v]; return v; }
    if (!strcmp(t,@encode(double))) { double v=0; [inv getReturnValue:&v]; return v; }
    if (!strcmp(t,@encode(BOOL))) { BOOL v=NO; [inv getReturnValue:&v]; return v; }
    if (!strcmp(t,@encode(NSUInteger))) { NSUInteger v=0; [inv getReturnValue:&v]; return v; }
    Check(NO,[@"numeric ABI " stringByAppendingString:name],@(t)); return NAN;
}
static id Object(id obj, NSString *name) {
    NSInvocation *inv=Invocation(obj,name,0);
    Check(inv.methodSignature.methodReturnType[0]=='@', @"object getter ABI", name);
    [inv invoke]; __unsafe_unretained id value=nil; [inv getReturnValue:&value]; return value;
}
static void Value(UIControl *slider, double value) {
    NSInvocation *inv=Invocation(slider,@"setValue:",1);
    const char *t=[inv.methodSignature getArgumentTypeAtIndex:2];
    if (!strcmp(t,@encode(float))) { float v=value; [inv setArgument:&v atIndex:2]; }
    else if (!strcmp(t,@encode(double))) { double v=value; [inv setArgument:&v atIndex:2]; }
    else Check(NO,@"slider setter numeric ABI",@(t));
    [inv invoke];
}
static void Track(id owner, UIControl *slider, NSString *name) {
    Check(Object(slider,@"delegate")==owner,@"native delegate identity before tracking",name);
    NSInvocation *inv=Invocation(owner,name,1);
    Check(!strcmp(inv.methodSignature.methodReturnType,"v") &&
          [inv.methodSignature getArgumentTypeAtIndex:2][0]=='@',@"tracking callback ABI",name);
    id arg=slider; [inv setArgument:&arg atIndex:2]; [inv invoke];
}
static void Seek(AVPlayer *p, double seconds) {
    __block BOOL done=NO, finished=NO;
    [p seekToTime:CMTimeMakeWithSeconds(seconds,600) toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero completionHandler:^(BOOL f){ finished=f; done=YES; }];
    Check(Wait(5,^BOOL { return done; }) && finished,@"prepare exact player seek",@(seconds));
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
        Check(Wait(4,^BOOL { return input.readyForMoreMediaData || writer.status==AVAssetWriterStatusFailed; }) && input.readyForMoreMediaData,@"encoder ready",@(frame));
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

@interface PV2Harness : UIResponder <UIApplicationDelegate>
@property(nonatomic,strong) UIWindow *window;
@property(nonatomic,strong) UIViewController *root;
@property(nonatomic,strong) PV2NativeTimelineViewController *host;
@property(nonatomic,strong) AVPlayer *player;
@end
@implementation PV2Harness
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    checks=[NSMutableArray array];
    report=[@{@"schema":@1,@"completed":@NO,@"checks":checks,
              @"runtime":UIDevice.currentDevice.systemVersion,
              @"os":NSProcessInfo.processInfo.operatingSystemVersionString,
              @"nativeHostOnly":@YES,@"physicalTouchTested":@NO,
              @"inputMode":@"programmatic UIControl valueChanged + verified native delegate tracking callbacks"} mutableCopy];
    self.window=[[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.root=[UIViewController new]; self.root.view.backgroundColor=UIColor.blackColor;
    self.window.rootViewController=self.root; [self.window makeKeyAndVisible];
    [self performSelector:@selector(run) withObject:nil afterDelay:0.2];
    return YES;
}
- (void)attach:(AVPlayer *)player {
    self.host=[[PV2NativeTimelineViewController alloc] initWithPlayer:player];
    [self.root addChildViewController:self.host];
    self.host.view.frame=CGRectMake(12,CGRectGetHeight(self.root.view.bounds)-160,CGRectGetWidth(self.root.view.bounds)-24,88);
    self.host.view.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleTopMargin;
    [self.host.view layoutIfNeeded];
    Check(self.host.view.window==nil && self.host.timelineView==nil && self.host.playbackControlsView==nil && self.host.slider==nil,@"native setup deferred until visible window attachment",self.host.failureReason);
    [self.root.view addSubview:self.host.view]; [self.host didMoveToParentViewController:self.root];
    Check(Wait(12,^BOOL { return self.host.timelineView && self.host.slider && Visible(self.host.timelineView); }),@"real visible native timeline setup",self.host.failureReason);
}
- (id)verifyNative {
    Check(self.host.boundPlayer==self.player,@"external player identity binding",nil);
    UIView *timeline=self.host.timelineView; UIView *playback=self.host.playbackControlsView; UIControl *slider=self.host.slider;
    Check([timeline isKindOfClass:NSClassFromString(@"AVMobileChromelessTimelineView")],@"actual timeline class",NSStringFromClass(timeline.class));
    NSString *name=NSStringFromClass(slider.class);
    Check([name isEqual:@"AVMobileChromelessFluidSlider"] || [name isEqual:@"AVMobileChromelessSlider"],@"actual fluid or normal native slider",name);
    id owner=self.host.childViewControllers.firstObject;
    Check([owner isKindOfClass:NSClassFromString(@"AVMobileChromelessControlsViewController")],@"actual controls owner via containment",NSStringFromClass([owner class]));
    Check(Number(owner,@"optimizeForPerformance")==0 && Number(owner,@"includedControls")==9,@"native configuration: timeline plus playback control",nil);
    Check(playback && Visible(playback),@"native playback control region visible",NSStringFromClass(playback.class));
    id pc=Object(owner,@"playerController");
    Check([pc isKindOfClass:NSClassFromString(@"AVPlayerController")] && Object(pc,@"player")==self.player,@"actual AVPlayerController binding",NSStringFromClass([pc class]));
    Check(Object(slider,@"delegate")==owner,@"untouched native slider delegate",nil);
    Check(Wait(4,^BOOL { return Visible(slider) && slider.enabled; }),@"native slider visible and interactive",@{@"frame":NSStringFromCGRect(slider.frame),@"hidden":@(slider.hidden),@"alpha":@(slider.alpha),@"enabled":@(slider.enabled)});
    Check(Number(timeline,@"labelPosition")==1,@"native elapsed/remaining labels below slider",nil);
    NSMutableArray *visible=[NSMutableArray array], *extras=[NSMutableArray array];
    for (UIView *v in Tree(self.host.view)) {
        if (!Visible(v)) continue;
        NSString *cls=NSStringFromClass(v.class); [visible addObject:cls];
        BOOL inside=v==timeline || [v isDescendantOfView:timeline] || v==playback || [v isDescendantOfView:playback] || objc_getAssociatedObject(v,PV2NativeTransportKey)!=nil;
        if (!inside && ([v isKindOfClass:UIControl.class] || v.isAccessibilityElement ||
            [cls containsString:@"PlaybackControls"] || [cls containsString:@"Volume"] ||
            [cls containsString:@"DisplayMode"] || [cls containsString:@"TransportControls"])) [extras addObject:cls];
        Check(![cls containsString:@"PV2TimelineController"],@"no legacy custom wrapper in hierarchy",cls);
    }
    report[@"visibleNativeHierarchy"]=visible;
    Check(extras.count==0,@"no extra visible native controls or accessibility elements",extras);
    NSArray *actions=[slider actionsForTarget:owner forControlEvent:UIControlEventValueChanged];
    Check([slider.allTargets containsObject:owner] && actions.count>0,@"native owner registered for valueChanged",actions);
    report[@"nativeValueChangedActions"]=actions;
    report[@"timelineClass"]=NSStringFromClass(timeline.class); report[@"sliderClass"]=name;
    return owner;
}
- (void)scrubPlaying:(BOOL)playing owner:(id)owner {
    AVPlayer *p=self.player; UIControl *s=self.host.slider;
    [p pause]; Seek(p,0.5);
    if (playing) { [p play]; Check(Wait(4,^BOOL { return p.rate>0; }),@"play intent before scrub",nil); }
    else Check(p.rate==0,@"paused intent before scrub",nil);
    SEL begin=NSSelectorFromString(@"sliderDidBeginTracking:"), end=NSSelectorFromString(@"sliderDidEndTracking:");
    IMP beginIMP=class_getMethodImplementation([owner class],begin), endIMP=class_getMethodImplementation([owner class],end);
    Check([owner respondsToSelector:begin] && [owner respondsToSelector:end],@"native tracking methods retained",nil);
    Track(owner,s,@"sliderDidBeginTracking:");
    Check(Wait(2,^BOOL { return self.host.isScrubbing; }),@"native begin enters scrubbing",nil);
    double lo=Number(s,@"minimumValue"), hi=Number(s,@"maximumValue");
    Check(isfinite(lo) && isfinite(hi) && hi>lo,@"native slider timing range",@[@(lo),@(hi)]);
    double target=lo+(hi-lo)*0.6;
    Value(s,target); [s sendActionsForControlEvents:UIControlEventValueChanged];
    Check(Wait(4,^BOOL { double t=CMTimeGetSeconds(p.currentTime); return isfinite(t) && fabs(t-3.6)<0.55; }),@"native valueChanged seeks actual player",@(CMTimeGetSeconds(p.currentTime)));
    Track(owner,s,@"sliderDidEndTracking:");
    Check(Wait(3,^BOOL { return !self.host.isScrubbing; }),@"native end leaves scrubbing",nil);
    Check(Wait(3,^BOOL { return playing ? p.rate>0 : p.rate==0; }),playing ? @"playing intent restored" : @"paused intent preserved",@(p.rate));
    Settle(0.25);
    Check(playing ? p.rate>0 : p.rate==0,@"scrub intent remains stable",@(p.rate));
    Check(Object(s,@"delegate")==owner && beginIMP==class_getMethodImplementation([owner class],begin) && endIMP==class_getMethodImplementation([owner class],end),@"native delegate and tracking IMPs unchanged",nil);
}
- (void)detach {
    [self.host willMoveToParentViewController:nil]; [self.host.view removeFromSuperview];
    [self.host removeFromParentViewController]; self.host=nil;
}
- (void)run {
    @try {
        (void)dlopen("/System/Library/Frameworks/AVKit.framework/AVKit", RTLD_NOW | RTLD_LOCAL);
        for (NSString *name in @[@"AVPlayerController",@"AVMobileChromelessControlsViewController",@"AVMobileChromelessTimelineView"])
            Check(NSClassFromString(name)!=Nil,@"required native runtime class",name);
        NSURL *url=MakeAsset();
        __weak id weakOwner=nil;
        __weak UIView *weakTimeline=nil;
        __weak UIControl *weakSlider=nil;
        @autoreleasepool {
        self.player=[AVPlayer playerWithURL:url];
        Check(Wait(10,^BOOL { return self.player.currentItem.status!=AVPlayerItemStatusUnknown; }) && self.player.currentItem.status==AVPlayerItemStatusReadyToPlay,@"real AVPlayerItem ready",self.player.currentItem.error.localizedDescription);
        Check(fabs(CMTimeGetSeconds(self.player.currentItem.duration)-6)<0.12,@"local H264 duration six seconds",@(CMTimeGetSeconds(self.player.currentItem.duration)));
        [self attach:self.player];
        @autoreleasepool {
            id owner=[self verifyNative]; UIControl *slider=self.host.slider;
            Seek(self.player,0.3); double time0=CMTimeGetSeconds(self.player.currentTime), value0=Number(slider,@"value");
            [self.player play];
            BOOL advanced=Wait(5,^BOOL { return CMTimeGetSeconds(self.player.currentTime)>time0+0.65 && Number(slider,@"value")>value0+0.01; });
            id nativePC=Object(owner,@"playerController");
            Check(advanced,@"natural playback advances native progress",@{@"playerTime":@(CMTimeGetSeconds(self.player.currentTime)),@"playerRate":@(self.player.rate),@"timeControl":@(self.player.timeControlStatus),@"value0":@(value0),@"value":@(Number(slider,@"value")),@"timing": [Object(nativePC,@"timing") description] ?: @"nil",@"rootHidden":@(((UIViewController*)owner).view.hidden)});
            [self.player pause];
            [@"visible" writeToFile:Docs(@"screenshot-ready") atomically:YES encoding:NSUTF8StringEncoding error:nil];
            Check(Wait(12,^BOOL { return [[NSFileManager defaultManager] fileExistsAtPath:Docs(@"screenshot-captured")]; }),@"simctl screenshot captured while timeline visible",nil);
            [self scrubPlaying:NO owner:owner]; [self scrubPlaying:YES owner:owner];
            CGRect old=self.root.view.frame;
            self.root.view.frame=CGRectMake(0,0,old.size.height,old.size.width);
            [self.root setOverrideTraitCollection:[UITraitCollection traitCollectionWithHorizontalSizeClass:UIUserInterfaceSizeClassRegular] forChildViewController:self.host];
            [self.root.view setNeedsLayout]; [self.root.view layoutIfNeeded]; Settle(0.3);
            CGRect r=[self.host.slider convertRect:self.host.slider.bounds toView:self.host.view];
            Check(Visible(self.host.timelineView) && Visible(self.host.slider) && CGRectContainsRect(CGRectInset(self.host.view.bounds,-2,-2),r),@"rotation-shaped root resize keeps slider inside host",NSStringFromCGRect(r));
            Check(self.host.traitCollection.horizontalSizeClass==UIUserInterfaceSizeClassRegular,@"active child trait propagation",nil);
            self.root.view.frame=old; [self.root setOverrideTraitCollection:nil forChildViewController:self.host];
            [self.root.view layoutIfNeeded];
        }
        weakOwner=self.host.childViewControllers.firstObject;
        weakTimeline=self.host.timelineView;
        weakSlider=self.host.slider;
        [self.player pause]; Seek(self.player,0.5); [self.player play];
        Check(Wait(3,^BOOL { return self.player.rate>0; }),@"external player active before invalidate",nil);
        double before=CMTimeGetSeconds(self.player.currentTime);
        [self.host invalidate]; [self.host invalidate];
        Check(self.host.childViewControllers.count==0 && self.host.timelineView==nil && self.host.playbackControlsView==nil && self.host.slider==nil,@"idempotent invalidate detaches native controls",nil);
        Check(Wait(4,^BOOL { return self.player.rate>0 && CMTimeGetSeconds(self.player.currentTime)>before+0.35; }),@"invalidate does not pause external player",@(self.player.rate));
        [self detach];
        }
        Check(Wait(4,^BOOL { return weakOwner==nil && weakTimeline==nil && weakSlider==nil; }),@"native owner timeline slider released",@{@"ownerAlive":@(weakOwner!=nil),@"timelineAlive":@(weakTimeline!=nil),@"sliderAlive":@(weakSlider!=nil)});
        AVPlayer *oldPlayer=self.player;
        // A distinct item and player: replacement is intentionally a new host, not a made-up setter.
        self.player=[AVPlayer playerWithURL:url]; [self attach:self.player];
        @autoreleasepool { [self verifyNative]; }
        Check(self.host.boundPlayer!=oldPlayer,@"replacement video uses new player binding",nil);
        [oldPlayer pause]; Seek(oldPlayer,1.0); Seek(self.player,0.5);
        @autoreleasepool { [self scrubPlaying:NO owner:self.host.childViewControllers.firstObject]; }
        Check(fabs(CMTimeGetSeconds(oldPlayer.currentTime)-1)<0.1,@"replacement scrub cannot seek previous player",@(CMTimeGetSeconds(oldPlayer.currentTime)));
        [self.host invalidate]; [self detach]; report[@"passed"]=@YES;
    } @catch (NSException *e) {
        report[@"passed"]=@NO; report[@"exception"]=@{@"name":e.name ?: @"",@"reason":e.reason ?: @"",@"stack":e.callStackSymbols ?: @[]};
    }
    report[@"completed"]=@YES;
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
    [json writeToFile:Docs(@"result.json") atomically:YES];
    NSMutableString *text=[NSMutableString stringWithFormat:@"%@ iOS %@\n",[report[@"passed"] boolValue]?@"PASS":@"FAIL",report[@"runtime"]];
    for (NSDictionary *c in checks) [text appendFormat:@"%@ %@ %@\n",[c[@"passed"] boolValue]?@"PASS":@"FAIL",c[@"name"],c[@"detail"]];
    [text appendString:@"Native host only; not Photos private wrappers. Programmatic events/callbacks, not physical touch.\n"];
    [text writeToFile:Docs(@"result.txt") atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSLog(@"PV2TEST COMPLETED %@",report[@"passed"]);
}
@end
int main(int argc,char **argv) { @autoreleasepool { return UIApplicationMain(argc,argv,nil,NSStringFromClass(PV2Harness.class)); } }
