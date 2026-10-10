#import <Foundation/Foundation.h>
#import <CoreMedia/CoreMedia.h>
#import "../TimelineCore.h"
#import "../LivePlayback.h"
#include <stdlib.h>
static void Check(BOOL ok,const char *text){if(!ok){fprintf(stderr,"FAIL %s\n",text);exit(1);}}
static void PV2Log(__unused NSString *s) {}
@interface FakeSession : NSObject
@property(nonatomic) BOOL isReadyForSeeking;
- (id)videoPlayer;
@end
@implementation FakeSession
- (id)videoPlayer {return nil;}
@end
@interface PV2NativeBrowsing : NSObject
@property(nonatomic,strong) FakeSession *videoSession;
@property(nonatomic,strong) NSMutableArray *targets;
@property(nonatomic,strong) NSMutableArray *callbacks;
@property(nonatomic) CMTime lastTolerance;
- (void)seekToTime:(CMTime)time toleranceBefore:(CMTime)before toleranceAfter:(CMTime)after completionHandler:(void (^)(BOOL))completion;
@end
@implementation PV2NativeBrowsing
- (instancetype)init {if((self=[super init])){_targets=[NSMutableArray new];_callbacks=[NSMutableArray new];_videoSession=[FakeSession new];_videoSession.isReadyForSeeking=YES;}return self;}
- (void)seekToTime:(CMTime)time toleranceBefore:(CMTime)before toleranceAfter:(CMTime)after completionHandler:(void (^)(BOOL))completion {
 Check(CMTimeCompare(before,after)==0,"symmetric seek tolerance");
 self.lastTolerance=before;[self.targets addObject:@(CMTimeGetSeconds(time))];[self.callbacks addObject:[completion copy]];
}
@end
@interface FakeTile : NSObject
@property(nonatomic,strong) PV2NativeBrowsing *browser;
- (id)_browsingVideoPlayer;
@end
@implementation FakeTile
- (id)_browsingVideoPlayer {return self.browser;}
@end
@interface Harness : NSObject
@property(nonatomic,strong) PV2NativeBrowsing *browsing;
@property(nonatomic,strong) FakeSession *sessionToken;
@property(nonatomic,strong) FakeTile *tile;
@property(nonatomic) PV2TimelineSeekState seekState;
@property(nonatomic) BOOL dragging;
@property(nonatomic) BOOL eligible;
@property(nonatomic) NSUInteger seekSerial;
@property(nonatomic,strong) PV2LivePlaybackSnapshot *liveSnapshot;
- (BOOL)readyForSeeking;
- (void)sampleLivePlayback;
- (void)requestSeekSeconds:(double)seconds;
- (void)invalidateSeeks;
- (void)submitSeekTarget:(double)target;
- (void)issueNativeSeekTo:(double)target;
- (void)nativeSeekCompletedForEpoch:(unsigned long)epoch browsing:(id)browsing session:(id)session;
@end
#define PV2TimelineController Harness
@implementation Harness
- (instancetype)init {if((self=[super init])){PV2TimelineSeekReset(&_seekState);_tile=[FakeTile new];_eligible=YES;}return self;}
- (double)playbackDuration {return 30;}
- (BOOL)isEligible {return self.eligible;}
- (BOOL)readyForSeeking {return self.sessionToken.isReadyForSeeking;}
- (void)sampleLivePlayback {}
/* PRODUCTION_METHODS */
@end
static void Drain(void){for(int i=0;i<5;i++)[NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];}
static void Complete(PV2NativeBrowsing *b,NSUInteger index){void (^cb)(BOOL)=b.callbacks[index];cb(YES);Drain();}
static void Bind(Harness *h,PV2NativeBrowsing *b){[h invalidateSeeks];h.browsing=b;h.sessionToken=b.videoSession;h.tile.browser=b;}
int main(void){@autoreleasepool{
 Harness *h=[Harness new];PV2NativeBrowsing *b=[PV2NativeBrowsing new];Bind(h,b);
 h.dragging=YES;[h requestSeekSeconds:5];[h requestSeekSeconds:10];[h requestSeekSeconds:15];
 Check(b.targets.count==1 && h.seekState.hasPending,"one native seek outstanding, latest drag coalesced");
 Check(CMTimeGetSeconds(b.lastTolerance)>0,"drag tolerance is finite nonzero");
 h.dragging=NO;[h requestSeekSeconds:20];Complete(b,0);
 Check(b.targets.count==2 && [b.targets[1] doubleValue]==20,"exact final target wins");
 Check(CMTimeCompare(b.lastTolerance,kCMTimeZero)==0,"release uses exact zero tolerance");
 Complete(b,1);Check(!h.seekState.inFlight,"completion releases seek queue");
 [h requestSeekSeconds:7];
 PV2NativeBrowsing *newBrowser=[PV2NativeBrowsing new];Bind(h,newBrowser);[h requestSeekSeconds:25];
 Complete(b,2);Check(h.seekState.inFlight && newBrowser.targets.count==1,"old browser completion cannot clear new flight");
 Complete(newBrowser,0);Check(!h.seekState.inFlight,"new completion releases own flight");
 [h requestSeekSeconds:12];[h requestSeekSeconds:14];[h invalidateSeeks];[h requestSeekSeconds:26];
 Complete(newBrowser,1);Check(h.seekState.inFlight && h.seekState.emittedTarget==26,"stale epoch same browser ignored");
 Complete(newBrowser,2);
 newBrowser.videoSession.isReadyForSeeking=NO;[h requestSeekSeconds:10];
 Check(!h.seekState.inFlight && newBrowser.targets.count==3,"not-ready seek releases slot without native pending");
 newBrowser.videoSession.isReadyForSeeking=YES;[h requestSeekSeconds:29];[h requestSeekSeconds:30];h.eligible=NO;
 Complete(newBrowser,3);Check(!h.seekState.inFlight && newBrowser.targets.count==4,"exit drops pending seek");
 h.eligible=YES;[h requestSeekSeconds:30];[h requestSeekSeconds:8];
 // Simulate the end-of-video native seek never calling its completion.
 [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.1]];
 Check(newBrowser.targets.count==6 && [newBrowser.targets[5] doubleValue]==8 && h.seekState.inFlight,"missing tail callback watchdog emits latest scrub target");
 Complete(newBrowser,4);
 Check(h.seekState.inFlight && h.seekState.emittedTarget==8,"late tail completion cannot release replacement request");
 Complete(newBrowser,5);Check(!h.seekState.inFlight,"post-end scrub releases normally");
 [h requestSeekSeconds:30];
 [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.1]];
 Check(!h.seekState.inFlight,"missing callback without pending cannot freeze displayed time forever");
 [h requestSeekSeconds:4];Check(newBrowser.targets.count==8,"subsequent drag remains usable after end timeout");
 void (^cancelled)(BOOL)=newBrowser.callbacks[7];cancelled(NO);Drain();
 Check(!h.seekState.inFlight,"cancelled seek still releases flight");
 puts("PASS: extracted production seek callbacks, exact final, stale identity/serial, readiness, lost end callback watchdog and cancelled seek");
}return 0;}
