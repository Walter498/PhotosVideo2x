// tests/TimelineCoreTests.cpp
//
// Portable host check for the pure geometry/time policy in TimelineCore.h. It
// needs no Apple SDK, so it builds and runs anywhere with a C++17 compiler:
//
//   c++ -std=c++17 -Wall -Wextra -Werror tests/TimelineCoreTests.cpp -o /tmp/pv2-timeline-core && /tmp/pv2-timeline-core
//
// These are boundary/outcome checks, not a re-implementation: every assertion
// pins an externally observable result (a panel edge, a mapped second, a clock
// string, a coalescing decision) rather than internal state.

#include "../TimelineCore.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition, ...)                                  \
    do {                                                       \
        if (!(condition)) {                                    \
            fprintf(stderr, "FAIL %d: ", __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                      \
            fprintf(stderr, "\n");                             \
            failures++;                                        \
        }                                                      \
    } while (0)

static bool near(double a, double b, double tolerance) {
    return isfinite(a) && isfinite(b) && fabs(a - b) <= tolerance;
}

static const char *clock(double seconds, int (*formatter)(double, char *, int)) {
    static char buffer[32];
    formatter(seconds, buffer, (int)sizeof(buffer));
    return buffer;
}

static const char *remaining(double current, double duration) {
    static char buffer[32];
    PV2TimelineFormatRemaining(current, duration, buffer, (int)sizeof(buffer));
    return buffer;
}

int main(void) {
    // --- geometry: bottom sits 4pt above the real toolbar top -----------------------------
    PV2TimelinePanelSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.hostWidth = 390;  spec.hostHeight = 844;
    spec.safeTop = 47;     spec.safeLeft = 0; spec.safeBottom = 34; spec.safeRight = 0;
    spec.toolbarTop = 760; spec.hasToolbar = true;
    spec.topClearance = 99; // safeTop + 52 (top-bar clearance)
    spec.height = 44; spec.horizontalMargin = 12; spec.gap = 4;

    PV2TimelineRect r = PV2TimelinePanelRect(&spec);
    CHECK(near(r.y + r.height, 756, 1e-9), "toolbar: panel bottom must be toolbarTop-4 (got %.3f)", r.y + r.height);
    CHECK(near(r.y, 712, 1e-9), "toolbar: panel top (got %.3f)", r.y);
    CHECK(near(r.x, 12, 1e-9) && near(r.width, 366, 1e-9), "toolbar: horizontal frame (x=%.3f w=%.3f)", r.x, r.width);
    CHECK(near(r.height, 44, 1e-9), "toolbar: panel height");

    // --- geometry: no toolbar falls back to the safe-area bottom ---------------------------
    spec.hasToolbar = false;
    r = PV2TimelinePanelRect(&spec);
    CHECK(near(r.y + r.height, 844 - 34 - 4, 1e-9), "fallback: panel bottom must be hostBottom-safeBottom-4 (got %.3f)", r.y + r.height);

    // fallback with a notch-free device (zero bottom inset) still keeps the 4pt gap
    spec.safeBottom = 0;
    r = PV2TimelinePanelRect(&spec);
    CHECK(near(r.y + r.height, 840, 1e-9), "fallback: zero inset keeps the 4pt gap (got %.3f)", r.y + r.height);

    // --- geometry: never float up into the top bar ----------------------------------------
    spec.safeBottom = 250; spec.hostHeight = 300; spec.hasToolbar = false; spec.topClearance = 99;
    r = PV2TimelinePanelRect(&spec);
    CHECK(near(r.y, 99, 1e-9), "top clearance lifts the panel below the top bar (got %.3f)", r.y);
    CHECK(r.y + r.height <= spec.hostHeight, "top clearance never pushes past the host bottom");

    // --- geometry: host bottom wins over a top clearance that cannot both fit --------------
    spec.safeBottom = 0; spec.hostHeight = 100; spec.topClearance = 99;
    r = PV2TimelinePanelRect(&spec);
    CHECK(near(r.y, 56, 1e-9), "short host: bottom clamp wins over clearance (got %.3f)", r.y);
    CHECK(r.y + r.height <= spec.hostHeight, "short host: panel stays on screen");

    // --- geometry: horizontal margins collapse instead of going negative -------------------
    spec.hostWidth = 40; spec.safeLeft = 10; spec.safeRight = 10; spec.horizontalMargin = 12;
    spec.hostHeight = 844; spec.hasToolbar = false; spec.safeBottom = 34; spec.topClearance = 99;
    r = PV2TimelinePanelRect(&spec);
    CHECK(r.width >= 0, "tiny host: width never negative (got %.3f)", r.width);
    CHECK(near(r.x, 22, 1e-9) && near(r.width, 0, 1e-9), "tiny host: remaining width clamps to host edge");

    // degenerate/null specs must not read garbage
    r = PV2TimelinePanelRect(NULL);
    CHECK(r.width == 0 && r.height == 0 && r.x == 0 && r.y == 0, "null spec is a zero rect");
    memset(&spec, 0, sizeof(spec));
    spec.hostWidth = NAN; spec.hostHeight = NAN; spec.height = -1; spec.horizontalMargin = -1; spec.gap = -1;
    r = PV2TimelinePanelRect(&spec);
    CHECK(near(r.height, 44, 1e-9) && near(r.x, 12, 1e-9) && r.width >= 0, "non-finite host falls back to defaults");

    // --- time mapping boundaries ----------------------------------------------------------
    CHECK(PV2TimelineHasDuration(10) && !PV2TimelineHasDuration(0) && !PV2TimelineHasDuration(-5) &&
              !PV2TimelineHasDuration(NAN), "duration validity");
    CHECK(near(PV2TimelineClampSeconds(-1, 10), 0, 1e-9) && near(PV2TimelineClampSeconds(4, 10), 4, 1e-9) &&
              near(PV2TimelineClampSeconds(99, 10), 10, 1e-9), "seconds clamp at both edges");
    CHECK(near(PV2TimelineClampSeconds(5, 0), 0, 1e-9), "no duration collapses to zero");
    CHECK(near(PV2TimelineClampSeconds(NAN, 10), 0, 1e-9), "non-finite seconds collapse to zero");

    CHECK(near(PV2TimelineProgress(0, 10), 0, 1e-9) && near(PV2TimelineProgress(5, 10), 0.5, 1e-9) &&
              near(PV2TimelineProgress(10, 10), 1, 1e-9), "progress at endpoints");
    CHECK(near(PV2TimelineProgress(-2, 10), 0, 1e-9) && near(PV2TimelineProgress(20, 10), 1, 1e-9),
          "progress clamps out of range");
    CHECK(near(PV2TimelineProgress(5, 0), 0, 1e-9), "progress without duration is zero");
    CHECK(near(PV2TimelineSecondsForProgress(0.25, 8), 2, 1e-9) &&
              near(PV2TimelineSecondsForProgress(-1, 8), 0, 1e-9) &&
              near(PV2TimelineSecondsForProgress(2, 8), 8, 1e-9), "progress->seconds clamps");

    CHECK(near(PV2TimelineJump(3, 5, 10), 8, 1e-9) && near(PV2TimelineJump(3, -5, 10), 0, 1e-9) &&
              near(PV2TimelineJump(8, 5, 10), 10, 1e-9), "double-tap jump clamps to [0,duration]");
    CHECK(near(PV2TimelineJump(NAN, 5, 10), 5, 1e-9), "jump from non-finite position");

    // --- duration is native-only: PHAsset length is never a fallback (slow-mo) ------------
    CHECK(near(PV2TimelineNativeTimelineSeconds(12.5, true), 12.5, 1e-9), "native duration accepted");
    CHECK(near(PV2TimelineNativeTimelineSeconds(NAN, false), 0, 1e-9), "missing native duration => not ready");
    CHECK(near(PV2TimelineNativeTimelineSeconds(0, true), 0, 1e-9), "zero native duration => not ready");
    CHECK(near(PV2TimelineNativeTimelineSeconds(-4, true), 0, 1e-9), "negative native duration => not ready");
    CHECK(near(PV2TimelineNativeTimelineSeconds(NAN, true), 0, 1e-9), "non-finite native duration => not ready");
    CHECK(!PV2TimelineAcceptsAssetDurationFallback(), "asset duration fallback is explicitly rejected");

    // --- clock text: monospaced elapsed and negative remaining ----------------------------
    CHECK(strcmp(clock(0, PV2TimelineFormatElapsed), "0:00") == 0, "elapsed 0");
    CHECK(strcmp(clock(59, PV2TimelineFormatElapsed), "0:59") == 0, "elapsed 59");
    CHECK(strcmp(clock(60, PV2TimelineFormatElapsed), "1:00") == 0, "elapsed 60 rolls the minute");
    CHECK(strcmp(clock(3599, PV2TimelineFormatElapsed), "59:59") == 0, "elapsed 59:59");
    CHECK(strcmp(clock(3600, PV2TimelineFormatElapsed), "1:00:00") == 0, "elapsed rolls to hours");
    CHECK(strcmp(clock(-3, PV2TimelineFormatElapsed), "0:00") == 0, "negative elapsed clamps to 0:00");
    CHECK(strcmp(clock(NAN, PV2TimelineFormatElapsed), "0:00") == 0, "non-finite elapsed");

    CHECK(strcmp(remaining(0, 3), "-0:03") == 0, "remaining seconds");
    CHECK(strcmp(remaining(0, 60), "-1:00") == 0, "remaining minutes");
    CHECK(strcmp(remaining(0, 3600 + 60 + 5), "-1:01:05") == 0, "remaining hours");
    CHECK(strcmp(remaining(10, 10), "-0:00") == 0, "remaining at start reads -0:00");
    CHECK(strcmp(remaining(12, 10), "-0:00") == 0, "remaining never goes positive");
    CHECK(strcmp(remaining(0, 0.4), "-0:01") == 0, "remaining rounds the last partial second up");

    // ---- coalescing: first move emits, then no faster than interval ----------------------
    double interval = PV2TimelineSeekInterval();
    CHECK(near(interval, 0.1, 1e-12), "seek interval is 10Hz");
    CHECK(PV2TimelineShouldEmitSeek(1.00, NAN, interval), "first drag move always emits");
    CHECK(!PV2TimelineShouldEmitSeek(1.05, 1.00, interval), "moves inside the interval are deferred");
    CHECK(PV2TimelineShouldEmitSeek(1.10, 1.00, interval), "move on the interval boundary emits");
    CHECK(PV2TimelineShouldEmitSeek(1.20, 1.00, interval), "move past the interval emits");
    CHECK(PV2TimelineShouldEmitSeek(1.00, 1.00, 0.0), "zero interval emits (disabled coalescing)");
    CHECK(!PV2TimelineShouldEmitSeek(NAN, 1.00, interval), "non-finite clock never emits");

    // ---- refresh cadence: ~30Hz repaint while visible ------------------------------------
    CHECK(near(PV2TimelineRefreshInterval(), 1.0 / 30.0, 1e-12), "refresh interval is 30Hz");
    CHECK(PV2TimelineRefreshInterval() < interval, "repaint is faster than drag requests");

    // ---- single-flight seek queue: serialize + coalesce + latest wins --------------------
    PV2TimelineSeekState q;
    PV2TimelineSeekReset(&q);
    double emit = -1.0;
    CHECK(PV2TimelineSeekRequest(&q, 5.0, &emit) && near(emit, 5.0, 1e-9), "idle request emits immediately");
    CHECK(q.inFlight && near(q.emittedTarget, 5.0, 1e-9) && !q.hasPending, "queue records the in-flight seek");
    CHECK(!PV2TimelineSeekRequest(&q, 6.0, &emit) && q.hasPending && near(q.pendingTarget, 6.0, 1e-9),
          "second request while in flight is coalesced, not emitted");
    CHECK(!PV2TimelineSeekRequest(&q, 7.0, &emit) && q.inFlight && near(q.pendingTarget, 7.0, 1e-9),
          "later request replaces the single pending target");
    bool stale = true;
    double next = -1.0;
    CHECK(PV2TimelineSeekCompletion(&q, q.epoch, &stale, &next) && !stale && near(next, 7.0, 1e-9),
          "completion hands off the LATEST pending target (final request wins)");
    CHECK(q.inFlight && near(q.emittedTarget, 7.0, 1e-9) && !q.hasPending, "hand-off keeps a single seek in flight");
    CHECK(!PV2TimelineSeekCompletion(&q, q.epoch, &stale, &next) && !stale, "completion with nothing pending releases the slot");
    CHECK(!q.inFlight && !q.hasPending, "queue is idle after the last completion");

    // completion after identity change is stale and must be ignored
    PV2TimelineSeekReset(&q);
    (void)PV2TimelineSeekRequest(&q, 3.0, &emit);
    unsigned long issuedEpoch = q.epoch;
    PV2TimelineSeekInvalidate(&q); // leave / asset change / chrome hide
    CHECK(q.epoch != issuedEpoch, "invalidate advances the identity epoch");
    CHECK(!PV2TimelineSeekCompletion(&q, issuedEpoch, &stale, &next), "stale completion issues no seek");
    CHECK(stale, "completion with an old epoch is flagged stale");
    CHECK(!q.inFlight && !q.hasPending, "stale completion cannot revive the queue");

    // a late callback must NOT release a newer seek issued after the identity change
    PV2TimelineSeekReset(&q);
    (void)PV2TimelineSeekRequest(&q, 3.0, &emit);
    issuedEpoch = q.epoch;
    PV2TimelineSeekInvalidate(&q);
    (void)PV2TimelineSeekRequest(&q, 11.0, &emit); // fresh identity, new seek in flight
    CHECK(PV2TimelineSeekCompletion(&q, issuedEpoch, &stale, &next) == false && stale,
          "old callback is stale even with a newer seek in flight");
    CHECK(q.inFlight && near(q.emittedTarget, 11.0, 1e-9), "newer in-flight seek is untouched by the stale callback");
    CHECK(!PV2TimelineSeekCompletion(&q, q.epoch, &stale, &next) && !stale, "the newer seek still completes normally");

    // cancel/invalidate drops a pending target so it can never fire later
    PV2TimelineSeekReset(&q);
    (void)PV2TimelineSeekRequest(&q, 1.0, &emit);
    (void)PV2TimelineSeekRequest(&q, 2.0, &emit); // coalesced
    PV2TimelineSeekInvalidate(&q);
    CHECK(!q.inFlight && !q.hasPending, "invalidate clears in-flight + pending (drag cancel)");
    CHECK(PV2TimelineSeekRequest(&q, 9.0, &emit) && near(emit, 9.0, 1e-9), "next request after cancel emits immediately");

    // a fresh request after a native seek threw (completion forced) still works
    PV2TimelineSeekReset(&q);
    (void)PV2TimelineSeekRequest(&q, 4.0, &emit);
    (void)PV2TimelineSeekCompletion(&q, q.epoch, &stale, &next); // treat a thrown native call as completion
    CHECK(PV2TimelineSeekRequest(&q, 8.0, &emit) && near(emit, 8.0, 1e-9), "slot is released when a seek throws");

    // ---- rapid double-taps accumulate onto the newest requested target -------------------
    // initial tap: idle, base is the native position
    CHECK(near(PV2TimelineNextSeekTarget(10.0, false, false, 0, 0, 5.0, 100.0), 15.0, 1e-9),
          "first +5 from a settled position");
    // the seek is now in flight (emitted 15) and two more taps arrive before it completes
    CHECK(near(PV2TimelineNextSeekTarget(10.0, true, false, 0, 15.0, 5.0, 100.0), 20.0, 1e-9),
          "second +5 stacks on the in-flight target, not the stale native time");
    CHECK(near(PV2TimelineNextSeekTarget(10.0, true, true, 20.0, 15.0, 5.0, 100.0), 25.0, 1e-9),
          "third +5 stacks on the pending target (+15 total)");
    // backward taps accumulate/saturate the same way
    CHECK(near(PV2TimelineNextSeekTarget(3.0, true, true, 2.0, 1.0, -5.0, 100.0), 0.0, 1e-9),
          "-5 saturates at zero from the pending base");
    CHECK(near(PV2TimelineNextSeekTarget(96.0, true, false, 0, 98.0, 5.0, 100.0), 100.0, 1e-9),
          "+5 saturates at the duration boundary");
    CHECK(near(PV2TimelineNextSeekTarget(50.0, false, false, 0, 0, 5.0, 0.0), 55.0, 1e-9),
          "no duration: only the lower bound is enforced");
    CHECK(near(PV2TimelineSeekBase(7.0, false, false, 0, 0), 7.0, 1e-9), "idle base is the native time");
    CHECK(near(PV2TimelineSeekBase(7.0, false, true, 12.0, 9.0), 12.0, 1e-9), "pending base wins");
    CHECK(near(PV2TimelineSeekBase(7.0, true, false, 0, 9.0), 9.0, 1e-9), "in-flight base is the emitted target");

    // null/degenerate guards on the queue policy
    CHECK(!PV2TimelineSeekRequest(NULL, 1.0, &emit), "null state request is a no-op");
    CHECK(!PV2TimelineSeekRequest(&q, NAN, &emit), "non-finite target is rejected");
    PV2TimelineSeekReset(NULL); PV2TimelineSeekInvalidate(NULL);
    CHECK(!PV2TimelineSeekCompletion(NULL, 0, &stale, &next), "null state completion is a no-op");

    // ---- drag policy: never restart playback that the controller did not pause ------------
    CHECK(!PV2TimelineDragRestoresPlayback(false, true, true), "no controller pause => no restart (preserve state)");
    CHECK(!PV2TimelineDragRestoresPlayback(true, false, true), "paused while already stopped => stay stopped");
    CHECK(!PV2TimelineDragRestoresPlayback(true, true, false), "identity changed => do not resume the old player");
    CHECK(PV2TimelineDragRestoresPlayback(true, true, true), "resume only when we paused a playing, still-current video");

    CHECK(PV2TimelineRateIsPlaying(1.0f) && PV2TimelineRateIsPlaying(0.5f) &&
              !PV2TimelineRateIsPlaying(0.0f) && !PV2TimelineRateIsPlaying(-1.0f) &&
              !PV2TimelineRateIsPlaying(NAN), "rate read is a play/pause test only");

    if (failures == 0) {
        printf("PASS: timeline core host check\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d check(s)\n", failures);
    return 1;
}
