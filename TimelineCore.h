#pragma once
// TimelineCore.h
//
// Pure, dependency-free policy for the PiP-style video timeline panel
// implemented by TimelineController.h. It has NO Foundation/UIKit/CoreMedia
// dependency, so the same source is:
//   1. consumed by the ARC ObjC++ tweak header (TimelineController.h), and
//   2. compiled as plain C++17 by tests/TimelineCoreTests.cpp on any host.
//
// Keep every function here decision-only: it must never touch a native player,
// a view, or a timer. Time values are already-mapped seconds (the caller feeds
// native browsing currentTime/duration); geometry values are already-converted
// host coordinates. That separation is what makes the boundaries testable.

#include <math.h>
#include <stdio.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Constants (single source of truth shared by the controller and the tests).
// ---------------------------------------------------------------------------
enum {
    PV2TimelinePanelHeightDefault = 44,
    PV2TimelineHorizontalMarginDefault = 12,
    PV2TimelineToolbarGapDefault = 4,
    PV2TimelineSeekHz = 10,     // drag-move requests are rate-limited to ~10Hz
    PV2TimelineRefreshHz = 30   // visible panel repaints at ~30Hz
};

// Request-rate limiter for drag moves: emit the first move immediately, then at
// most one *request* per interval. Requests are then coalesced by the
// single-flight queue below, so this only bounds how often we ask.
static inline double PV2TimelineSeekInterval(void) {
    return 1.0 / (double)PV2TimelineSeekHz;
}

// Repaint cadence while the panel is visible and the app is foreground.
static inline double PV2TimelineRefreshInterval(void) {
    return 1.0 / (double)PV2TimelineRefreshHz;
}

// ---------------------------------------------------------------------------
// Geometry: where the low compact panel sits inside the owner's view.
//
// The panel is bottom-anchored `gap` points above the real UIToolbar top
// (converted to owner.view coordinates by the caller). When no visible toolbar
// is supplied it falls back just above the safe-area bottom inset.
// ---------------------------------------------------------------------------
struct PV2TimelineRect {
    double x;
    double y;
    double width;
    double height;
};

struct PV2TimelinePanelSpec {
    double hostWidth;        // owner.view.bounds width
    double hostHeight;       // owner.view.bounds height
    double safeTop;          // owner.view.safeAreaInsets.top
    double safeLeft;
    double safeBottom;
    double safeRight;
    double toolbarTop;       // toolbar.frame.origin.y in host coords; used iff hasToolbar
    bool   hasToolbar;
    double topClearance;     // lowest allowed y (top-bar clearance); <=0 disables clamping
    double height;           // <=0 => PV2TimelinePanelHeightDefault
    double horizontalMargin; // <0  => PV2TimelineHorizontalMarginDefault
    double gap;              // <0  => PV2TimelineToolbarGapDefault
};

static inline double PV2TimelineDefaulted(double value, double fallback) {
    return (isfinite(value) && value >= 0.0) ? value : fallback;
}

static inline struct PV2TimelineRect PV2TimelinePanelRect(const struct PV2TimelinePanelSpec *spec) {
    struct PV2TimelineRect r;
    r.x = 0.0; r.y = 0.0; r.width = 0.0; r.height = 0.0;
    if (spec == NULL) return r;

    double hostW = (isfinite(spec->hostWidth) && spec->hostWidth > 0.0) ? spec->hostWidth : 0.0;
    double hostH = (isfinite(spec->hostHeight) && spec->hostHeight > 0.0) ? spec->hostHeight : 0.0;
    double safeL = (isfinite(spec->safeLeft) && spec->safeLeft > 0.0) ? spec->safeLeft : 0.0;
    double safeR = (isfinite(spec->safeRight) && spec->safeRight > 0.0) ? spec->safeRight : 0.0;
    double safeB = (isfinite(spec->safeBottom) && spec->safeBottom > 0.0) ? spec->safeBottom : 0.0;

    double height = PV2TimelineDefaulted(spec->height, (double)PV2TimelinePanelHeightDefault);
    double margin = PV2TimelineDefaulted(spec->horizontalMargin, (double)PV2TimelineHorizontalMarginDefault);
    double gap = PV2TimelineDefaulted(spec->gap, (double)PV2TimelineToolbarGapDefault);
    if (height < 0.0) height = 0.0;

    double left = safeL + margin;
    double width = hostW - safeL - safeR - 2.0 * margin;
    if (width < 0.0) width = 0.0;

    // Bottom edge: real toolbar top when present, otherwise the safe-area bottom.
    double bottomLimit;
    if (spec->hasToolbar && isfinite(spec->toolbarTop)) {
        bottomLimit = spec->toolbarTop - gap;
    } else {
        bottomLimit = hostH - safeB - gap;
    }

    double y = bottomLimit - height;
    if (y < 0.0) y = 0.0;
    if (isfinite(spec->topClearance) && spec->topClearance > 0.0 && y < spec->topClearance)
        y = spec->topClearance;
    if (hostH > 0.0 && y + height > hostH) {
        y = hostH - height;
        if (y < 0.0) y = 0.0;
    }
    if (hostW > 0.0 && left + width > hostW) {
        width = hostW - left;
        if (width < 0.0) width = 0.0;
    }

    r.x = left; r.y = y; r.width = width; r.height = height;
    return r;
}

// ---------------------------------------------------------------------------
// Time mapping. `duration <= 0` / non-finite means "no timeline": progress is 0
// and seeks stay at 0 rather than fabricating a range.
// ---------------------------------------------------------------------------
static inline bool PV2TimelineHasDuration(double duration) {
    return isfinite(duration) && duration > 0.0;
}

static inline double PV2TimelineClampSeconds(double seconds, double duration) {
    if (!PV2TimelineHasDuration(duration)) return 0.0;
    if (!isfinite(seconds)) return 0.0;
    if (seconds < 0.0) return 0.0;
    if (seconds > duration) return duration;
    return seconds;
}

static inline double PV2TimelineProgress(double seconds, double duration) {
    if (!PV2TimelineHasDuration(duration) || !isfinite(seconds)) return 0.0;
    double p = seconds / duration;
    if (p < 0.0) return 0.0;
    if (p > 1.0) return 1.0;
    return p;
}

static inline double PV2TimelineSecondsForProgress(double progress, double duration) {
    if (!PV2TimelineHasDuration(duration) || !isfinite(progress)) return 0.0;
    if (progress < 0.0) progress = 0.0;
    if (progress > 1.0) progress = 1.0;
    return progress * duration;
}

// Double-tap +/- jump: clamp to [0, duration]; ignore a non-finite duration by
// only enforcing the lower bound.
static inline double PV2TimelineJump(double seconds, double delta, double duration) {
    if (!isfinite(seconds)) seconds = 0.0;
    if (!isfinite(delta)) delta = 0.0;
    double t = seconds + delta;
    if (t < 0.0) t = 0.0;
    if (PV2TimelineHasDuration(duration) && t > duration) t = duration;
    return t;
}

// The timeline duration MUST come from the native browsing player: its
// currentTime/duration are mapped through Photos' own time-range mapper, so
// slow-motion and trimmed/edited clips report the real (e.g. slowed-down or
// shortened) range. PHAsset.duration is the raw source length and does not go
// through that mapping, so using it as a fallback would desync the slider and
// the +/- jumps. A missing/invalid native duration therefore means "not ready
// yet" (slider disabled), never "use the asset length".
static inline double PV2TimelineNativeTimelineSeconds(double nativeSeconds, bool nativeValid) {
    if (nativeValid && isfinite(nativeSeconds) && nativeSeconds > 0.0) return nativeSeconds;
    return 0.0;
}

// The asset length is intentionally NOT used: kept as an explicit, documented
// non-decision so a future reader does not "helpfully" reintroduce the
// slow-motion-incompatible fallback.
static inline bool PV2TimelineAcceptsAssetDurationFallback(void) {
    return false;
}

// ---------------------------------------------------------------------------
// Clock text. Elapsed is a plain magnitude; remaining is prefixed with '-'.
// ---------------------------------------------------------------------------
static inline int PV2TimelineFormatClock(double nonNegativeSeconds, char *out, int cap) {
    if (out == NULL || cap <= 0) return 0;
    if (!isfinite(nonNegativeSeconds) || nonNegativeSeconds < 0.0) nonNegativeSeconds = 0.0;
    long total = (long)floor(nonNegativeSeconds + 1e-9);
    long hours = total / 3600;
    int minutes = (int)((total % 3600) / 60);
    int seconds = (int)(total % 60);
    if (hours > 0) return snprintf(out, (size_t)cap, "%ld:%02d:%02d", hours, minutes, seconds);
    return snprintf(out, (size_t)cap, "%d:%02d", minutes, seconds);
}

static inline int PV2TimelineFormatElapsed(double seconds, char *out, int cap) {
    if (isfinite(seconds) && seconds < 0.0) seconds = 0.0;
    return PV2TimelineFormatClock(seconds, out, cap);
}

static inline int PV2TimelineFormatRemaining(double current, double duration, char *out, int cap) {
    if (out == NULL || cap <= 0) return 0;
    double remaining = 0.0;
    if (isfinite(current) && isfinite(duration) && duration > current)
        remaining = duration - current;
    if (remaining <= 0.0) return snprintf(out, (size_t)cap, "-0:00");
    // Count up to the next second so the last second reads -0:01, not -0:00.
    long total = (long)ceil(remaining - 1e-9);
    long hours = total / 3600;
    int minutes = (int)((total % 3600) / 60);
    int seconds = (int)(total % 60);
    if (hours > 0) return snprintf(out, (size_t)cap, "-%ld:%02d:%02d", hours, minutes, seconds);
    return snprintf(out, (size_t)cap, "-%d:%02d", minutes, seconds);
}

// ---------------------------------------------------------------------------
// Drag coalescing: emit the first move immediately, then at most one native
// seek per interval; the release always performs one exact final seek.
// ---------------------------------------------------------------------------
static inline bool PV2TimelineShouldEmitSeek(double now, double lastEmit, double interval) {
    if (!isfinite(now)) return false;
    if (!isfinite(lastEmit)) return true;
    if (!isfinite(interval) || interval <= 0.0) return true;
    return (now - lastEmit) >= interval;
}

// ---------------------------------------------------------------------------
// Single-flight seek queue (serialized, coalesced, latest-target-wins, epoch
// guarded). Every native seek request goes through this. At most one native
// seekToTime:completionHandler: is outstanding at a time; while it is in flight
// any further request only replaces the single pending target (coalescing), so
// intermediate drag positions are dropped and the newest request always wins.
//
// Epoch: the controller bumps the epoch whenever the identity (owner tile,
// browsing player, or its video session) changes, or the panel is torn down /
// hidden. A completion callback issued on the old identity carries the epoch it
// was issued at; PV2TimelineSeekCompletion() rejects it as stale, so a late
// callback from a previous asset/leave can never revive a seek for the new one.
// ---------------------------------------------------------------------------
struct PV2TimelineSeekState {
    bool          inFlight;       // a native seek is outstanding
    bool          hasPending;     // a newer coalesced target is waiting
    double        pendingTarget;  // newest requested target (valid iff hasPending)
    double        emittedTarget;  // target of the in-flight seek (valid iff inFlight)
    unsigned long epoch;          // identity generation
    unsigned long inFlightEpoch;  // epoch captured when the seek was issued
};

static inline void PV2TimelineSeekReset(struct PV2TimelineSeekState *state) {
    if (state == NULL) return;
    state->inFlight = false;
    state->hasPending = false;
    state->pendingTarget = 0.0;
    state->emittedTarget = 0.0;
    state->epoch = 0;
    state->inFlightEpoch = 0;
}

// Identity changed (leave / asset change / chrome hide / teardown): drop any
// in-flight bookkeeping and pending target, and advance the epoch so a callback
// still in the system is recognized as stale. The next request starts fresh.
static inline void PV2TimelineSeekInvalidate(struct PV2TimelineSeekState *state) {
    if (state == NULL) return;
    state->inFlight = false;
    state->hasPending = false;
    state->pendingTarget = 0.0;
    state->emittedTarget = 0.0;
    state->inFlightEpoch = 0;
    state->epoch += 1;
}

// Submit a new target. Returns true and writes *emitNow when the native seek
// should be issued immediately (queue was idle); returns false when the target
// was coalesced into the single pending slot because a seek is in flight.
static inline bool PV2TimelineSeekRequest(struct PV2TimelineSeekState *state, double target, double *emitNow) {
    if (state == NULL || !isfinite(target)) return false;
    if (state->inFlight) {
        state->hasPending = true;
        state->pendingTarget = target;
        return false;
    }
    state->inFlight = true;
    state->inFlightEpoch = state->epoch;
    state->emittedTarget = target;
    if (emitNow) *emitNow = target;
    return true;
}

// A native completion arrived; `issuedEpoch` is the epoch the seek was issued
// at (== state->epoch unless the identity has since changed). Returns true and
// writes *nextTarget when the coalesced latest pending request must now be
// issued (single-flight hand-off). Writes *stale=true, and touches nothing, for
// a callback whose epoch is behind the live identity (a late callback from a
// previous asset/leave must never release or revive a newer seek).
static inline bool PV2TimelineSeekCompletion(struct PV2TimelineSeekState *state,
                                             unsigned long issuedEpoch,
                                             bool *stale,
                                             double *nextTarget) {
    if (stale) *stale = false;
    if (state == NULL) return false;
    if (issuedEpoch != state->epoch) {
        if (stale) *stale = true;
        return false;
    }
    if (!state->inFlight) return false;
    state->inFlight = false;
    if (state->hasPending) {
        state->inFlight = true;
        state->inFlightEpoch = state->epoch;
        state->emittedTarget = state->pendingTarget;
        state->pendingTarget = 0.0;
        state->hasPending = false;
        if (nextTarget) *nextTarget = state->emittedTarget;
        return true;
    }
    state->emittedTarget = 0.0;
    return false;
}

// Base position a fresh +/- request should be measured from so that repeated
// fast double-taps accumulate: while a seek is in flight the newest *requested*
// target (pending, else emitted) is the base rather than the still-stale native
// currentTime. When idle, the native position is authoritative.
static inline double PV2TimelineSeekBase(double nativeCurrent, bool inFlight, bool hasPending,
                                         double pendingTarget, double emittedTarget) {
    if (hasPending) return isfinite(pendingTarget) ? pendingTarget : 0.0;
    if (inFlight) return isfinite(emittedTarget) ? emittedTarget : 0.0;
    return isfinite(nativeCurrent) ? nativeCurrent : 0.0;
}

// Target for a double-tap +/- jump, accumulated onto the seek base and clamped
// to [0, duration] exactly like PV2TimelineJump.
static inline double PV2TimelineNextSeekTarget(double nativeCurrent, bool inFlight, bool hasPending,
                                               double pendingTarget, double emittedTarget,
                                               double delta, double duration) {
    double base = PV2TimelineSeekBase(nativeCurrent, inFlight, hasPending, pendingTarget, emittedTarget);
    return PV2TimelineJump(base, delta, duration);
}

// Playback is preserved, never restarted: the controller does not pause, so
// nothing is resumed. If a future integration opts to pause, resume only when
// it was the one that paused, the video was actually playing, and the identity
// (tile/session) is still the one the drag started on.
static inline bool PV2TimelineDragRestoresPlayback(bool pausedByController, bool wasPlaying, bool stillCurrent) {
    return pausedByController && wasPlaying && stillCurrent;
}

// Rate is only a read for deciding whether the timeline is live; the controller
// must never write it (no fix-rate, no looping change).
static inline bool PV2TimelineRateIsPlaying(float rate) {
    return isfinite((double)rate) && rate > 0.0f;
}
