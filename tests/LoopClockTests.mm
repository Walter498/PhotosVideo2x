//
// tests/LoopClockTests.mm
//
// Real-CoreMedia regression for PV2LivePlaybackSeekCoordinate() in
// LivePlayback.h: the folded (per-lap) -> unfolded (raw FigPlaybackItem) seek
// coordinate conversion the live helper uses so scrubbing a looping video
// targets the correct raw coordinate instead of restarting near 0.
//
// Build (macOS host, real AVFoundation + CoreMedia timebases):
//   xcrun clang++ -std=c++17 -fobjc-arc -fblocks -Wall -Wextra -Werror \
//     -framework Foundation -framework AVFoundation -framework CoreMedia \
//     tests/LoopClockTests.mm -o /tmp/pv2-loop-clock-tests
//   /tmp/pv2-loop-clock-tests
//
// The test is black box: it never re-implements the mapping. It builds two
// genuinely distinct CMTimebases on the host clock (CMClockGetHostTimeClock(),
// the same root the live FigPlaybackItem uses) and asserts the numbers
// CoreMedia itself produces. Only this file is added; no production source is
// touched and the helper is imported, not duplicated.
//
// CoreMedia affine model. CMSyncConvertTime "syncs the clock or timebase based
// on the rates in the common tree rooted in that host":
//
//   folded_now   = anchorF + rateF * (hostNow - sharedSource)
//   unfolded_now = anchorU + rateU * (hostNow - sharedSource)
//   convert(t)   = unfolded_now + (t - folded_now) * (rateU / rateF)
//
// Because both timebases share one host root and one source anchor, hostNow and
// sharedSource cancel:
//
//   rateF == rateU == 1 : convert(t) = t + (anchorU - anchorF)   -> fold offset D
//   rateF == 1, rateU==2: convert(t) = 2*t + (anchorU - 2*anchorF) -> affine slope 2
//
// So a folded 375.274 s on the second lap (raw = folded + D, D = 658.499 s) must
// convert to 1033.773 s, not stay at 375.274 s, and the two-lap raw window is 2D.

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <objc/runtime.h>
#import <math.h>
#import <stdio.h>
#import <stdlib.h>
#import <string.h>
#import "../LivePlayback.h"

static void PV2Fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}
static void PV2Check(BOOL condition, const char *message) {
    if (!condition) PV2Fail(message);
}
static BOOL PV2SecondsClose(CMTime a, CMTime b, double tolerance) {
    if (!CMTIME_IS_NUMERIC(a) || !CMTIME_IS_NUMERIC(b)) return NO;
    return fabs(CMTimeGetSeconds(a) - CMTimeGetSeconds(b)) <= tolerance;
}

// --- real timebases ---------------------------------------------------------
// A timebase rooted in the host clock, paused or running at `rate`, whose clock
// therefore reads `anchor + rate * (hostNow - sourceTime)`.
static CMTimebaseRef PV2MakeTimebase(Float64 rate, CMTime anchor, CMTime sourceTime) {
    CMTimebaseRef timebase = NULL;
    OSStatus status = CMTimebaseCreateWithSourceClock(kCFAllocatorDefault,
                                                      CMClockGetHostTimeClock(), &timebase);
    PV2Check(status == noErr && timebase != NULL, "host-rooted timebase created");
    CMTimebaseSetRate(timebase, rate);
    CMTimebaseSetAnchorTime(timebase, anchor, sourceTime);
    return timebase;
}

typedef struct {
    CMTimebaseRef folded;   // per-lap clock, mirrors AVPlayerItem.timebase
    CMTimebaseRef unfolded; // raw clock, mirrors _copyProxyUnfoldedTimebase
} PV2ClockPair;

static PV2ClockPair PV2MakePair(Float64 foldedRate, CMTime foldedAnchor,
                                Float64 unfoldedRate, CMTime unfoldedAnchor,
                                CMTime sourceTime) {
    PV2ClockPair pair;
    pair.folded = PV2MakeTimebase(foldedRate, foldedAnchor, sourceTime);
    pair.unfolded = PV2MakeTimebase(unfoldedRate, unfoldedAnchor, sourceTime);
    return pair;
}

// --- the production item shape ---------------------------------------------
// AVPlayerItem subclass that supplies the same two clocks the live item exposes:
// -timebase is the folded per-lap clock, _copyProxyUnfoldedTimebase returns a +1
// (CF_RETURNS_RETAINED) reference to the raw clock (or NULL when the proxy is
// unavailable), -currentTime carries the epoch the helper copies, and
// -loopTimeRange reports the loop window the helper probes when it cannot map.
@interface PV2FakeLoopItem : AVPlayerItem {
@public
    CMTimebaseRef foldedTimebase;
    CMTimebaseRef rawTimebase;
    CMTime pretendCurrentTime;
    CMTimeRange pretendLoopRange;
}
@end

@implementation PV2FakeLoopItem
- (CMTimebaseRef)timebase { return foldedTimebase; }
- (CMTimebaseRef)_copyProxyUnfoldedTimebase {
    return rawTimebase ? (CMTimebaseRef)CFRetain(rawTimebase) : NULL;
}
- (CMTime)currentTime { return pretendCurrentTime; }
- (CMTimeRange)loopTimeRange { return pretendLoopRange; }
@end

// A real AVPlayerItem needs a real asset; the fake only overrides the clocks.
static PV2FakeLoopItem *PV2MakeItem(void) {
    AVMutableComposition *asset = [AVMutableComposition composition];
    PV2FakeLoopItem *item = [[PV2FakeLoopItem alloc] initWithAsset:asset];
    item->foldedTimebase = NULL;
    item->rawTimebase = NULL;
    item->pretendCurrentTime = kCMTimeZero;      // epoch 0: the numeric map is the thing under test
    item->pretendLoopRange = kCMTimeRangeInvalid; // ordinary media has no loop window
    return item;
}

// Detach the clocks before freeing them so a late -timebase/-copyProxy call on
// the still-live item can never touch freed memory.
static void PV2Dispose(PV2FakeLoopItem *item,
                       CMTimebaseRef folded, CMTimebaseRef unfolded) {
    item->foldedTimebase = NULL;
    item->rawTimebase = NULL;
    if (folded) CFRelease(folded);
    if (unfolded) CFRelease(unfolded);
}

int main(void) {
    @autoreleasepool {
        // --- ABI: the exact encodings the live helper keys on ----------------
        Method copyMethod = class_getInstanceMethod([PV2FakeLoopItem class],
                                                    @selector(_copyProxyUnfoldedTimebase));
        Method loopMethod = class_getInstanceMethod([PV2FakeLoopItem class],
                                                    @selector(loopTimeRange));
        PV2Check(copyMethod != NULL &&
                 strcmp(method_getTypeEncoding(copyMethod), "^{OpaqueCMTimebase=}16@0:8") == 0,
                 "_copyProxyUnfoldedTimebase must encode ^{OpaqueCMTimebase=}16@0:8");
        PV2Check(loopMethod != NULL &&
                 strcmp(method_getTypeEncoding(loopMethod), "{?={?=qiIq}{?=qiIq}}16@0:8") == 0,
                 "loopTimeRange must encode {?={?=qiIq}{?=qiIq}}16@0:8");
        // Exercise the header's wrapper-ABI probe so it is not an unused static:
        // a plain item must never satisfy the wrapped-player ABI.
        PV2Check(!PV2LivePlaybackABI([PV2FakeLoopItem class]),
                 "an AVPlayerItem subclass is not a wrapped player");

        // Fold duration D and the two-lap raw window 2D, all exact at 1/1000.
        const CMTime D          = CMTimeMake(658499, 1000);   // 658.499 s
        const CMTime twoD       = CMTimeMake(1316998, 1000);  // 1316.998 s
        const CMTime displayed  = CMTimeMake(375274, 1000);   // 375.274 s
        const CMTime expected   = CMTimeMake(1033773, 1000);  // 1033.773 s = 375.274 + 658.499
        CMTime source = CMClockGetTime(CMClockGetHostTimeClock());

        // ---------- second lap: raw = folded + D -----------------------------
        {
            PV2ClockPair pair = PV2MakePair(1.0, kCMTimeZero, 1.0, D, source);
            PV2FakeLoopItem *item = PV2MakeItem();
            item->foldedTimebase = pair.folded;
            item->rawTimebase = pair.unfolded;
            item->pretendCurrentTime = kCMTimeZero;

            // Sanity: two genuinely distinct timebases reading folded~0 and raw~D.
            PV2Check(pair.folded != pair.unfolded,
                     "folded and unfolded are distinct timebase objects");
            PV2Check(PV2SecondsClose(CMTimebaseGetTime(pair.folded), kCMTimeZero, 0.5),
                     "folded clock anchored at 0");
            PV2Check(PV2SecondsClose(CMTimebaseGetTime(pair.unfolded), D, 0.5),
                     "unfolded clock anchored one lap (D) ahead");

            CFIndex foldedBefore = CFGetRetainCount(pair.folded);
            CFIndex rawBefore = CFGetRetainCount(pair.unfolded);

            CMTime mapped = PV2LivePlaybackSeekCoordinate(item, displayed);
            PV2Check(CMTIME_IS_NUMERIC(mapped), "folded->raw mapping is numeric");
            PV2Check(PV2SecondsClose(mapped, expected, 1e-6),
                     "375.274 folded maps to 1033.773 raw (offset +658.499)");
            PV2Check(!PV2SecondsClose(mapped, displayed, 1e-6),
                     "375.274 is not returned unchanged");

            // Real CoreMedia roundtrip: raw -> folded restores the request.
            CMTime back = CMSyncConvertTime(mapped, pair.unfolded, pair.folded);
            PV2Check(PV2SecondsClose(back, displayed, 1e-6),
                     "unfolded->folded roundtrip yields the requested 375.274");

            // The end of lap 1 (t = D) lands on 2D, the far edge of the raw window.
            CMTime lapEdge = PV2LivePlaybackSeekCoordinate(item, D);
            PV2Check(PV2SecondsClose(lapEdge, twoD, 1e-6),
                     "lap edge D maps to 2D in the raw window");

            // --- copyRef lifecycle -------------------------------------------
            PV2Check(CFGetRetainCount(pair.folded) == foldedBefore,
                     "helper never releases the borrowed folded timebase");
            PV2Check(CFGetRetainCount(pair.unfolded) == rawBefore,
                     "helper balances the CF_RETURNS_RETAINED unfolded copy (no leak)");

            CMTimebaseRef copy = [item _copyProxyUnfoldedTimebase];
            PV2Check(copy == pair.unfolded, "copy yields the same underlying unfolded timebase");
            PV2Check(CFGetRetainCount(pair.unfolded) == rawBefore + 1,
                     "copy is returned +1 retained (CF_RETURNS_RETAINED honoured)");
            CFRelease(copy);
            PV2Check(CFGetRetainCount(pair.unfolded) == rawBefore,
                     "releasing the copy balances the retain (no destructor leak)");

            PV2Dispose(item, pair.folded, pair.unfolded);
        }

        // ---------- first lap: offset 0, unchanged ---------------------------
        {
            PV2ClockPair pair = PV2MakePair(1.0, kCMTimeZero, 1.0, kCMTimeZero, source);
            PV2FakeLoopItem *item = PV2MakeItem();
            item->foldedTimebase = pair.folded;
            item->rawTimebase = pair.unfolded;
            CMTime mapped = PV2LivePlaybackSeekCoordinate(item, displayed);
            PV2Check(CMTIME_IS_NUMERIC(mapped), "first-lap mapping is numeric");
            PV2Check(PV2SecondsClose(mapped, displayed, 1e-6),
                     "before the first lap the fold offset is 0 -> unchanged");
            PV2Dispose(item, pair.folded, pair.unfolded);
        }

        // ---------- negative folded clock: folded=-283, raw=375 --------------
        // Offset is raw - folded = 375 - (-283) = 658, so 400 must map to 1058.
        {
            PV2ClockPair pair = PV2MakePair(1.0, CMTimeMake(-283, 1),
                                            1.0, CMTimeMake(375, 1), source);
            PV2FakeLoopItem *item = PV2MakeItem();
            item->foldedTimebase = pair.folded;
            item->rawTimebase = pair.unfolded;
            item->pretendCurrentTime = CMTimeMake(-283, 1);

            PV2Check(PV2SecondsClose(CMTimebaseGetTime(pair.folded), CMTimeMake(-283, 1), 0.5),
                     "negative folded clock reads ~-283");
            PV2Check(PV2SecondsClose(CMTimebaseGetTime(pair.unfolded), CMTimeMake(375, 1), 0.5),
                     "unfolded clock reads ~375");

            CMTime mapped = PV2LivePlaybackSeekCoordinate(item, CMTimeMake(400, 1));
            PV2Check(PV2SecondsClose(mapped, CMTimeMake(1058, 1), 1e-6),
                     "folded=-283 / raw=375: 400 maps to 1058");
            PV2Check(!PV2SecondsClose(mapped, CMTimeMake(400, 1), 1e-6),
                     "negative-clock map is not the raw target");
            PV2Dispose(item, pair.folded, pair.unfolded);
        }

        // ---------- non-unit relative rate: affine, slope 2 ------------------
        {
            PV2ClockPair pair = PV2MakePair(1.0, kCMTimeZero, 2.0, D, source);
            PV2FakeLoopItem *item = PV2MakeItem();
            item->foldedTimebase = pair.folded;
            item->rawTimebase = pair.unfolded;

            CMTime m1 = PV2LivePlaybackSeekCoordinate(item, displayed);
            PV2Check(PV2SecondsClose(m1, CMTimeMake(1409047, 1000), 1e-6),
                     "rate 1:2 maps 375.274 to 1409.047 (D + 2*375.274)");
            PV2Check(!PV2SecondsClose(m1, expected, 1e-6),
                     "rate-2 map differs from the unit-rate map");

            CMTime mA = PV2LivePlaybackSeekCoordinate(item, CMTimeMake(100, 1));
            CMTime mB = PV2LivePlaybackSeekCoordinate(item, CMTimeMake(200, 1));
            PV2Check(PV2SecondsClose(mA, CMTimeMake(858499, 1000), 1e-6),
                     "rate 1:2 maps 100 to 858.499");
            PV2Check(PV2SecondsClose(mB, CMTimeMake(1058499, 1000), 1e-6),
                     "rate 1:2 maps 200 to 1058.499");
            double slope = (CMTimeGetSeconds(mB) - CMTimeGetSeconds(mA)) / 100.0;
            PV2Check(fabs(slope - 2.0) < 1e-6,
                     "relative rate 2 yields an affine slope of exactly 2");
            PV2Dispose(item, pair.folded, pair.unfolded);
        }

        // ---------- paused playback: rate 0 ----------------------------------
        // Both clocks share the host root and are stopped, so the fold offset is
        // frozen. CoreMedia may or may not define a conversion for a 0/0 relative
        // rate; either way the helper must not silently hand back the raw number.
        {
            PV2ClockPair pair = PV2MakePair(0.0, kCMTimeZero, 0.0, D, source);
            PV2FakeLoopItem *item = PV2MakeItem();
            item->foldedTimebase = pair.folded;
            item->rawTimebase = pair.unfolded;

            PV2Check(PV2SecondsClose(CMTimebaseGetTime(pair.folded), kCMTimeZero, 0.5),
                     "paused folded clock frozen at 0");
            PV2Check(PV2SecondsClose(CMTimebaseGetTime(pair.unfolded), D, 0.5),
                     "paused unfolded clock frozen at D");

            CMTime mapped = PV2LivePlaybackSeekCoordinate(item, displayed);
            if (CMTIME_IS_NUMERIC(mapped)) {
                PV2Check(PV2SecondsClose(mapped, expected, 1e-6),
                         "paused (rate 0) pair still converts with the fold offset");
                printf("INFO: rate-0 conversion is numeric on this host\n");
            } else {
                PV2Check(CMTimeCompare(mapped, kCMTimeInvalid) == 0,
                         "paused (rate 0) conversion fails closed with kCMTimeInvalid");
                PV2Check(!PV2SecondsClose(mapped, displayed, 1e-6),
                         "paused conversion never falls back to the raw coordinate");
                printf("INFO: rate-0 conversion not numeric on this host; helper fails closed\n");
            }
            PV2Dispose(item, pair.folded, pair.unfolded);
        }

        // ---------- epoch: 0 is exercised by every map above -----------------
        // A non-zero current epoch is host-dependent; probe it but fail closed.
        {
            PV2ClockPair pair = PV2MakePair(1.0, kCMTimeZero, 1.0, D, source);
            PV2FakeLoopItem *item = PV2MakeItem();
            item->foldedTimebase = pair.folded;
            item->rawTimebase = pair.unfolded;
            item->pretendCurrentTime = CMTimeMakeWithEpoch(11, 1, 42); // non-zero epoch

            CMTime mapped = PV2LivePlaybackSeekCoordinate(item, displayed);
            if (CMTIME_IS_NUMERIC(mapped)) {
                PV2Check(PV2SecondsClose(mapped, expected, 1e-6),
                         "non-zero current epoch keeps the fold offset");
                printf("INFO: non-zero-epoch conversion numeric (epoch %lld)\n",
                       (long long)mapped.epoch);
            } else {
                PV2Check(CMTimeCompare(mapped, kCMTimeInvalid) == 0,
                         "non-zero current epoch fails closed with kCMTimeInvalid");
                printf("INFO: non-zero-epoch conversion not numeric on this host; fails closed\n");
            }
            PV2Dispose(item, pair.folded, pair.unfolded);
        }

        // ---------- unmappable loop: no raw-coordinate fallback --------------
        // Folded clock present but the unfolded proxy is unavailable while a real
        // loop window exists -> the helper must refuse, never seek the raw number.
        {
            PV2ClockPair pair = PV2MakePair(1.0, kCMTimeZero, 1.0, D, source);
            PV2FakeLoopItem *item = PV2MakeItem();
            item->foldedTimebase = pair.folded;
            item->rawTimebase = NULL;
            item->pretendLoopRange = CMTimeRangeMake(kCMTimeZero, D);

            CMTime mapped = PV2LivePlaybackSeekCoordinate(item, displayed);
            PV2Check(!CMTIME_IS_NUMERIC(mapped),
                     "unmappable loop does not fall back to a raw coordinate");
            PV2Check(CMTimeCompare(mapped, kCMTimeInvalid) == 0,
                     "unmappable loop returns exactly kCMTimeInvalid");
            PV2Dispose(item, pair.folded, pair.unfolded);
        }
        {
            // Folded timebase missing entirely but a positive loop window remains.
            PV2FakeLoopItem *item = PV2MakeItem();
            item->pretendLoopRange = CMTimeRangeMake(kCMTimeZero, D);
            CMTime mapped = PV2LivePlaybackSeekCoordinate(item, displayed);
            PV2Check(CMTimeCompare(mapped, kCMTimeInvalid) == 0,
                     "missing folded clock + valid loop range => kCMTimeInvalid");
            PV2Dispose(item, NULL, NULL);
        }

        // ---------- ordinary media: raw coordinate allowed -------------------
        {
            PV2FakeLoopItem *item = PV2MakeItem();
            item->pretendLoopRange = kCMTimeRangeInvalid;
            CMTime target = CMTimeMake(400, 1);
            CMTime mapped = PV2LivePlaybackSeekCoordinate(item, target);
            PV2Check(CMTimeCompare(mapped, target) == 0,
                     "missing clock + invalid loop range => raw coordinate unchanged");
            PV2Dispose(item, NULL, NULL);
        }
        {
            // A valid but zero-length loop window is still "no loop".
            PV2FakeLoopItem *item = PV2MakeItem();
            item->pretendLoopRange = CMTimeRangeMake(kCMTimeZero, kCMTimeZero);
            CMTime target = CMTimeMake(400, 1);
            CMTime mapped = PV2LivePlaybackSeekCoordinate(item, target);
            PV2Check(CMTimeCompare(mapped, target) == 0,
                     "zero-length loop range => raw coordinate unchanged");
            PV2Dispose(item, NULL, NULL);
        }

        puts("PASS: folded<->unfolded seek mapping via real CoreMedia timebases "
             "(+D, affine rate, roundtrip, negative clock, paused, epoch, "
             "unmappable-loop refusal, copyRef lifecycle)");
    }
    return 0;
}
