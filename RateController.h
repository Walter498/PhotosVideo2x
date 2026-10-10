#pragma once
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#ifndef PV2_RATE_TEST
#import <UIKit/UIKit.h>
#endif
#import <objc/runtime.h>
#import <math.h>

// Include once from ARC Tweak.xm. Native calls are NEVER made under stateLock.
@protocol PV2RatePlayer <NSObject>
- (float)rate;
- (void)setRate:(float)rate;
@end
@interface PV2RateToken : NSObject
@property(nonatomic, weak) id<PV2RatePlayer> wrapper;
@property(nonatomic, weak) id owner;
@property(nonatomic) float initialRate;
@property(nonatomic) float latestRate;
@property(nonatomic) BOOL hasLatestRate;
@property(nonatomic) BOOL active;
@end
@implementation PV2RateToken
@end
static const void *PV2RateOwnerKey = &PV2RateOwnerKey;
static const void *PV2RateWrapperKey = &PV2RateWrapperKey;
static const void *PV2FixedRateKey = &PV2FixedRateKey;
@interface PV2RateController : NSObject
@property(nonatomic, strong) NSLock *stateLock;
@property(nonatomic, strong) NSMutableSet<PV2RateToken *> *tokens;
+ (instancetype)shared;
@end
@implementation PV2RateController
+ (instancetype)shared {
    static PV2RateController *c;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        c = [PV2RateController new]; c.stateLock = [NSLock new];
        c.tokens = [NSMutableSet new];
    });
    return c;
}
@end
// Only state and associated objects may be accessed while holding stateLock.
static void PV2RateRetire(PV2RateController *c, PV2RateToken *t) {
    if (!t) return;
    t.active = NO;
    id wrapper = t.wrapper, owner = t.owner;
    if (wrapper && objc_getAssociatedObject(wrapper, PV2RateWrapperKey) == t)
        objc_setAssociatedObject(wrapper, PV2RateWrapperKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    if (owner && objc_getAssociatedObject(owner, PV2RateOwnerKey) == t)
        objc_setAssociatedObject(owner, PV2RateOwnerKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [c.tokens removeObject:t];
}
// Thread-local, wrapper-specific, nesting-safe guard. No async dispatch here.
static NSString *PV2RateGuardKey(id wrapper) {
    return [NSString stringWithFormat:@"PhotosVideo2x.internal.%p", wrapper];
}
static BOOL PV2RateIsInternal(id wrapper) {
    return [NSThread.currentThread.threadDictionary[PV2RateGuardKey(wrapper)] unsignedIntegerValue] != 0;
}
static void PV2RateWrite(id<PV2RatePlayer> wrapper, float rate) {
    NSMutableDictionary *d = NSThread.currentThread.threadDictionary;
    NSString *key = PV2RateGuardKey(wrapper);
    NSUInteger depth = [d[key] unsignedIntegerValue];
    d[key] = @(depth + 1);
    @try { [wrapper setRate:rate]; }
    @finally { if (depth) d[key] = @(depth); else [d removeObjectForKey:key]; }
}

static float PV2FixedRate(id wrapper) {
    PV2RateController *c = [PV2RateController shared];
    [c.stateLock lock];
    NSNumber *value = objc_getAssociatedObject(wrapper,PV2FixedRateKey);
    float rate = value ? value.floatValue : 1.0f;
    [c.stateLock unlock];
    return rate;
}
static float PV2DisplayRate(id wrapper) {
    PV2RateController *c=[PV2RateController shared];
    [c.stateLock lock];
    PV2RateToken *t=objc_getAssociatedObject(wrapper,PV2RateWrapperKey);
    NSNumber *fixed=objc_getAssociatedObject(wrapper,PV2FixedRateKey);
    float rate=t.active ? 2.0f : (fixed ? fixed.floatValue : 1.0f);
    [c.stateLock unlock];
    return rate;
}
static void PV2SetFixedRate(id<PV2RatePlayer> wrapper, float rate) {
    if (!wrapper || !isfinite(rate) || rate<=0 || rate>8) return;
    // Native getter/setter stay outside lock, retaining pause rather than starting play.
    float actual = [wrapper rate];
    PV2RateController *c = [PV2RateController shared];
    [c.stateLock lock];
    objc_setAssociatedObject(wrapper,PV2FixedRateKey,@(rate),OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    PV2RateToken *token = objc_getAssociatedObject(wrapper,PV2RateWrapperKey);
    BOOL boosted = token.active;
    if (boosted) { token.latestRate=rate; token.hasLatestRate=YES; }
    [c.stateLock unlock];
    if (isfinite(actual) && actual>0 && !boosted && fabsf(actual-rate)>0.0001f) PV2RateWrite(wrapper,rate);
}
static void PV2ClearFixedRate(id wrapper) {
    PV2RateController *c = [PV2RateController shared]; [c.stateLock lock];
    objc_setAssociatedObject(wrapper,PV2FixedRateKey,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [c.stateLock unlock];
}

// End only the captured token, never whatever now happens to be on its wrapper.
// Caller must use restore=NO before resource changes and destruction.
static void PV2RateEnd(PV2RateToken *t, BOOL restore) {
    if (!t) return;
    PV2RateController *c = [PV2RateController shared];
    id<PV2RatePlayer> wrapper = t.wrapper; // strong local through native call
    // Native getter can dispatch_sync internally: read BEFORE acquiring stateLock.
    float actual = 0;
    if (restore && wrapper) {
        @try { actual = [wrapper rate]; } @catch (__unused NSException *e) {}
    }
    BOOL write = NO;
    float rate = 0;
    [c.stateLock lock];
    @try {
        write = restore && t.active && wrapper &&
            objc_getAssociatedObject(wrapper, PV2RateWrapperKey) == t &&
            isfinite(actual) && actual > 0;
        rate = t.hasLatestRate ? t.latestRate : t.initialRate;
        PV2RateRetire(c, t);
    } @finally { [c.stateLock unlock]; }
    if (write && isfinite(rate) && rate > 0) {
        @try { PV2RateWrite(wrapper, rate); } @catch (__unused NSException *e) {}
    }
}
static void PV2RateEndOwner(id owner, BOOL restore) {
    if (!owner) return;
    PV2RateController *c = [PV2RateController shared];
    PV2RateToken *t;
    [c.stateLock lock];
    @try { t = objc_getAssociatedObject(owner, PV2RateOwnerKey); }
    @finally { [c.stateLock unlock]; }
    PV2RateEnd(t, restore);
}
// Gesture helper retains returned token; tile/helper separately retains wrapper.
// Refuse competing ownership rather than overwriting another live token.
static PV2RateToken *PV2RateBegin(id owner, id<PV2RatePlayer> wrapper) {
    if (!owner || !wrapper || !NSThread.isMainThread) return nil;
#ifdef PV2_RATE_TEST
    if (!PV2RateTestApplicationActive) return nil;
#else
    if (UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return nil;
#endif
    PV2RateEndOwner(owner, YES);
    float rate = 0;
    @try { rate = [wrapper rate]; } @catch (__unused NSException *e) { return nil; }
    if (!isfinite(rate) || rate <= 0) return nil;
    PV2RateController *c = [PV2RateController shared];
    PV2RateToken *t = [PV2RateToken new];
    t.wrapper = wrapper; t.owner = owner; t.initialRate = rate;
    [c.stateLock lock];
    @try {
        if (objc_getAssociatedObject(wrapper, PV2RateWrapperKey) ||
            objc_getAssociatedObject(owner, PV2RateOwnerKey)) return nil;
        t.active = YES;
        objc_setAssociatedObject(wrapper, PV2RateWrapperKey, t, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        objc_setAssociatedObject(owner, PV2RateOwnerKey, t, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [c.tokens addObject:t];
    } @finally { [c.stateLock unlock]; }
    @try { PV2RateWrite(wrapper, 2.0f); }
    @catch (__unused NSException *e) { PV2RateEnd(t, YES); return nil; }
    [c.stateLock lock];
    BOOL active;
    @try { active = t.active; } @finally { [c.stateLock unlock]; }
    return active ? t : nil;
}
static void PV2RateAroundSet(id wrapper, float rate, void (^original)(float)) {
    PV2RateController *c = [PV2RateController shared];
    BOOL internal = PV2RateIsInternal(wrapper);
    float output = rate;
    [c.stateLock lock];
    @try {
        PV2RateToken *t = objc_getAssociatedObject(wrapper, PV2RateWrapperKey);
        NSNumber *fixed = objc_getAssociatedObject(wrapper,PV2FixedRateKey);
        float requested = rate;
        if (!internal && isfinite(rate) && rate>0 && fixed) requested=fixed.floatValue;
        output = requested;
        if (t.active && !internal) {
            id owner = t.owner;
            if (!owner || objc_getAssociatedObject(owner, PV2RateOwnerKey) != t ||
                !isfinite(rate) || rate <= 0) PV2RateRetire(c, t);
            else { t.latestRate = requested; t.hasLatestRate = YES; output = 2.0f; }
        }
    } @finally { [c.stateLock unlock]; }
    original(output); // MAY synchronously enter native player queues
}
static void PV2RateAroundPause(id wrapper, void (^original)(void)) {
    PV2RateController *c = [PV2RateController shared];
    [c.stateLock lock];
    @try { PV2RateRetire(c, objc_getAssociatedObject(wrapper, PV2RateWrapperKey)); }
    @finally { [c.stateLock unlock]; }
    original(); // pause bypasses wrapper setRate:, always retire first
}
static void PV2RateEndAll(BOOL restore) {
    PV2RateController *c = [PV2RateController shared];
    NSArray *snapshot;
    [c.stateLock lock];
    @try { snapshot = [c.tokens allObjects]; }
    @finally { [c.stateLock unlock]; }
    for (PV2RateToken *t in snapshot) PV2RateEnd(t, restore);
}
// Notifications belong to the integrating gesture module; do not duplicate them.
// Concurrency limit: native rate/pause calls are outside stateLock. This prevents
// lock/ivarQueue cycles but cannot atomically order native writes against a
// concurrent pause. Exact ordering requires the verified native player executor,
// not a per-wrapper lock around methods that may synchronously enter that executor.
