// Build: xcrun clang++ -fobjc-arc -fblocks -Wall -Wextra -Werror -framework Foundation tests/RateControllerTests.mm -o /tmp/pv2-rate-tests
// /tmp/pv2-rate-tests
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#import <stdlib.h>
#define PV2_RATE_TEST 1
static BOOL PV2RateTestApplicationActive = YES;
#import "../RateController.h"

static void Check(BOOL ok, const char *message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
@interface MockPlayer : NSObject <PV2RatePlayer>
@property(nonatomic, strong) NSLock *lock;
@property(nonatomic) float storedRate;
@property(nonatomic) NSUInteger writes;
- (void)pause;
@end
@implementation MockPlayer
- (instancetype)init {
    if ((self = [super init])) { _lock = [NSLock new]; _storedRate = 1; }
    return self;
}
- (float)rate {
    // Detect controller lock accidentally held over native getter.
    NSLock *stateLock = [PV2RateController shared].stateLock;
    Check([stateLock tryLock], "native getter called under state lock");
    [stateLock unlock];
    [self.lock lock]; float value = self.storedRate; [self.lock unlock];
    return value;
}
- (void)setRate:(float)value {
    PV2RateAroundSet(self, value, ^(float output) {
        // tryLock is meaningful in sequential tests; concurrent tests exercise timeout.
        [self.lock lock]; self.storedRate = output; self.writes += 1; [self.lock unlock];
    });
}
- (void)pause {
    PV2RateAroundPause(self, ^{
        // Deliberately does NOT call setRate:.
        [self.lock lock]; self.storedRate = 0; [self.lock unlock];
    });
}
@end

int main(void) {
    @autoreleasepool {
        NSObject *owner = [NSObject new];
        MockPlayer *p = [MockPlayer new];
        PV2RateToken *t = PV2RateBegin(owner, p);
        Check(t != nil && p.rate == 2, "begin writes 2x");
        [p setRate:1]; Check(p.rate == 2, "external 1x remains boosted");
        PV2RateEnd(t, YES); Check(p.rate == 1, "release restores external rate");

        t = PV2RateBegin(owner, p);
        NSUInteger count = p.writes;
        [p pause]; Check(p.writes == count, "pause bypasses setter");
        PV2RateEnd(t, YES); Check(p.rate == 0, "release after pause does not restart");

        [p setRate:1]; t = PV2RateBegin(owner, p);
        [p setRate:0]; PV2RateEnd(t, YES);
        Check(p.rate == 0, "external stop survives release");

        [p setRate:1];
        PV2RateToken *old = PV2RateBegin(owner, p);
        NSObject *newOwner = [NSObject new];
        Check(PV2RateBegin(newOwner, p) == nil, "live shared wrapper rejects competing token");
        PV2RateEndOwner(owner, YES);
        PV2RateToken *fresh = PV2RateBegin(newOwner, p);
        Check(fresh != nil, "wrapper can be reused after previous token ends");
        PV2RateEnd(old, YES); PV2RateEndOwner(owner, YES);
        Check(p.rate == 2 && fresh.active, "stale owner/token cannot retire new boost");
        PV2RateEnd(fresh, YES);

        t = PV2RateBegin(owner, p);
        PV2RateTestApplicationActive = NO;
        PV2RateEndAll(YES);
        Check(p.rate == 1 && !t.active, "background cleanup restores rate");
        Check(PV2RateBegin(owner, p) == nil, "background begin rejected");
        PV2RateTestApplicationActive = YES;

        t = PV2RateBegin(owner, p);
        dispatch_group_t group = dispatch_group_create();
        dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
        dispatch_group_async(group, queue, ^{
            @autoreleasepool { for (int i = 0; i < 2000; ++i) [p setRate:1]; }
        });
        dispatch_group_async(group, queue, ^{
            @autoreleasepool { for (int i = 0; i < 2000; ++i) PV2RateEnd(t, NO); }
        });
        Check(dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0,
              "concurrent external set/end completes without deadlock");
        Check(!t.active, "concurrent end retired token");
        PV2RateEndAll(NO);
        Check([PV2RateController shared].tokens.count == 0, "token registry empty");
        [p setRate:1];
        PV2SetFixedRate(p,1.5f);
        Check(p.rate==1.5f && PV2FixedRate(p)==1.5f,"fixed speed applied");
        [p setRate:1];Check(p.rate==1.5f,"native 1x rewritten to fixed speed");
        t=PV2RateBegin(owner,p);Check(p.rate==2,"long press uses absolute 2x");
        [p setRate:1];PV2RateEnd(t,YES);Check(p.rate==1.5f,"release returns to fixed speed");
        t=PV2RateBegin(owner,p);PV2SetFixedRate(p,1.25f);PV2RateEnd(t,YES);
        Check(p.rate==1.25f,"fixed speed selection during boost is remembered");
        [p pause];PV2SetFixedRate(p,0.5f);Check(p.rate==0,"fixed speed does not resume paused video");
        [p setRate:1];Check(p.rate==0.5f,"resume uses selected fixed speed");
        PV2ClearFixedRate(p);[p setRate:1];Check(p.rate==1,"unbound wrapper returns native behavior");
        puts("PASS: rate, pause, stale token, background, concurrency, fixed speed and temporary 2x");
    }
    return 0;
}
