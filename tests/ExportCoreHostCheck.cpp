// tests/ExportCoreHostCheck.cpp
//
// Portable host check for the time-scaling core in ExportComposition.h (part 1). It needs no Apple
// SDK, so it runs on any C++17 toolchain:
//
//   c++ -std=c++17 -Wall -Wextra -Werror tests/ExportCoreHostCheck.cpp -o /tmp/pv2-export-core && /tmp/pv2-export-core
//
// It duplicates a subset of tests/ExportMediaTests.mm on purpose: the macOS/CI test proves the
// AVFoundation side, this one lets the plan/policy logic be verified anywhere (and in a few
// milliseconds).

#include "../ExportComposition.h"

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

int main(void) {
    PV2ExportSpec spec;

    // mode -> resolved spec ----------------------------------------------------------------
    CHECK(PV2ExportResolveSpec(0, 2.0, &spec) && spec.includeVideo && spec.includeAudio &&
              near(spec.rate, 2.0, 1e-9), "mode 0 must keep video+audio at the current rate");
    CHECK(PV2ExportResolveSpec(1, 3.5, &spec) && !spec.includeVideo && spec.includeAudio &&
              near(spec.rate, 1.0, 1e-9), "mode 1 must force normal speed and drop video");
    CHECK(PV2ExportResolveSpec(2, 1.5, &spec) && !spec.includeVideo && spec.includeAudio &&
              near(spec.rate, 1.5, 1e-9), "mode 2 must keep the current rate for audio only");
    CHECK(!PV2ExportResolveSpec(3, 2.0, &spec), "unknown mode must be rejected");
    CHECK(!PV2ExportResolveSpec(0, NAN, &spec) && !PV2ExportResolveSpec(0, INFINITY, &spec) &&
              !PV2ExportResolveSpec(0, 0.0, &spec) && !PV2ExportResolveSpec(0, -2.0, &spec) &&
              !PV2ExportResolveSpec(0, 1000.0, &spec), "illegal rates must be rejected");

    CHECK(PV2ExportRateIsValid(PV2_EXPORT_MIN_RATE) && PV2ExportRateIsValid(PV2_EXPORT_MAX_RATE) &&
              !PV2ExportRateIsValid(PV2_EXPORT_MIN_RATE / 2.0), "rate window");
    CHECK(!PV2ExportDurationIsValid(0.0) && !PV2ExportDurationIsValid(-1.0) &&
              !PV2ExportDurationIsValid(NAN) && !PV2ExportDurationIsValid(1e12) &&
              PV2ExportDurationIsValid(3.5), "duration window");
    CHECK(!PV2ExportRequestIsValid(false, true, 0, 2.0) && !PV2ExportRequestIsValid(true, false, 0, 2.0) &&
              PV2ExportRequestIsValid(true, true, 0, 2.0) && !PV2ExportRequestIsValid(true, true, 0, 0.0),
          "request validation");

    // track presence policy ---------------------------------------------------------------
    PV2ExportSpec videoSpec;
    PV2ExportResolveSpec(0, 2.0, &videoSpec);
    PV2ExportSpec audioSpec;
    PV2ExportResolveSpec(2, 2.0, &audioSpec);
    CHECK(PV2ExportValidateTracks(&videoSpec, false, true, false) == PV2ExportReasonNoVideoTrack,
          "mode 0 without video must be refused");
    CHECK(PV2ExportValidateTracks(&videoSpec, true, false, false) == PV2ExportReasonNoAudioTrack,
          "mode 0 without audio must be refused unless explicitly allowed");
    CHECK(PV2ExportValidateTracks(&videoSpec, true, false, true) == PV2ExportReasonNone,
          "explicitly allowed silent video");
    CHECK(PV2ExportValidateTracks(&audioSpec, false, false, false) == PV2ExportReasonNoAudioTrack,
          "audio modes without audio must be refused");
    CHECK(PV2ExportValidateTracks(&audioSpec, true, true, false) == PV2ExportReasonNone,
          "audio modes accept a resource with audio");

    // segment -> action plan ---------------------------------------------------------------
    PV2ExportSegment segments[4];
    segments[0].isEmpty = false; segments[0].targetStart = 0.00; segments[0].targetDuration = 0.50;
    segments[1].isEmpty = true;  segments[1].targetStart = 0.50; segments[1].targetDuration = 0.25;
    segments[2].isEmpty = false; segments[2].targetStart = 0.75; segments[2].targetDuration = 0.50;
    segments[3].isEmpty = false; segments[3].targetStart = 1.25; segments[3].targetDuration = 0.50;

    PV2ExportAction actions[8];
    size_t count = 0;
    double duration = 0.0;
    double reported = PV2ExportBuildPlan(segments, 4, 2.0, true, actions, 8, &count, &duration);
    CHECK(count == 3, "plan must merge the two contiguous media runs (got %zu)", count);
    CHECK(near(duration, 0.875, 1e-9) && near(reported, 0.875, 1e-9), "plan duration");
    CHECK(actions[0].kind == PV2ExportActionInsertMedia && near(actions[0].outputStart, 0.0, 1e-9) &&
              near(actions[0].outputDuration, 0.25, 1e-9), "first media run scaled by 1/rate");
    CHECK(actions[1].kind == PV2ExportActionInsertEmpty && near(actions[1].outputStart, 0.25, 1e-9) &&
              near(actions[1].outputDuration, 0.125, 1e-9), "gap scaled and never merged");
    CHECK(actions[2].kind == PV2ExportActionInsertMedia && near(actions[2].sourceStart, 0.75, 1e-9) &&
              near(actions[2].sourceDuration, 1.0, 1e-9) && near(actions[2].outputStart, 0.375, 1e-9) &&
              near(actions[2].outputDuration, 0.5, 1e-9), "second run placed after the scaled gap");
    CHECK(near(PV2ExportPlanTotalDuration(actions, count), 0.875, 1e-9), "plan total duration");

    PV2ExportBuildPlan(segments, 4, 1.0, false, actions, 8, &count, &duration);
    CHECK(count == 3 && near(duration, 1.75, 1e-9) && near(actions[2].outputDuration, 1.0, 1e-9),
          "rate 1 must leave the timeline untouched");
    CHECK(PV2ExportBuildPlan(segments, 4, 2.0, true, actions, 2, &count, &duration) <= 0.0,
          "plan overflow must be reported, not truncated");
    CHECK(PV2ExportBuildPlan(segments, 4, 0.0, true, actions, 8, &count, &duration) <= 0.0,
          "scaling with an illegal rate must fail");
    CHECK(PV2ExportBuildPlan(NULL, 0, 2.0, true, actions, 8, &count, &duration) <= 0.0,
          "empty segment list must fail");

    // presentation decisions ---------------------------------------------------------------
    PV2ExportPresentation decision = PV2ExportDecidePresentation(PV2ExportOutcomeSucceeded, true, true);
    CHECK(decision.presentShare && !decision.keepOrphanFile && !decision.deleteFile && !decision.showError,
          "success + visible owner: share, then delete");
    decision = PV2ExportDecidePresentation(PV2ExportOutcomeSucceeded, true, false);
    CHECK(!decision.presentShare && decision.keepOrphanFile && !decision.deleteFile,
          "success + hidden owner: keep the file, never force the sheet");
    decision = PV2ExportDecidePresentation(PV2ExportOutcomeFailed, true, true);
    CHECK(decision.deleteFile && decision.showError && !decision.presentShare, "failure: delete + report");
    decision = PV2ExportDecidePresentation(PV2ExportOutcomeCancelled, true, true);
    CHECK(decision.deleteFile && !decision.showError, "cancellation: delete, no error UI");

    // helpers ------------------------------------------------------------------------------
    CHECK(near(PV2ExportScaledFrameDurationSeconds(1.0 / 30.0, 2.0), 1.0 / 60.0, 1e-9) &&
              near(PV2ExportScaledFrameDurationSeconds(1.0 / 30.0, 1.0), 1.0 / 30.0, 1e-9) &&
              near(PV2ExportScaledFrameDurationSeconds(0.0, 2.0), 0.0, 1e-9), "frame cadence scaling");
    CHECK(near(PV2ExportProbeStepSeconds(1.0 / 1000.0), PV2_EXPORT_PROBE_STEP_MIN_SECONDS, 1e-12) &&
              near(PV2ExportProbeStepSeconds(0.0), 1.0 / 30.0, 1e-12) &&
              near(PV2ExportProbeStepSeconds(1.0), PV2_EXPORT_PROBE_STEP_MAX_SECONDS, 1e-12),
          "probe step clamp");
    CHECK(PV2ExportProbeBudgetFits(60.0, 1.0 / 30.0) && !PV2ExportProbeBudgetFits(1e9, 1.0 / 240.0),
          "probe budget guard");
    CHECK(strcmp(PV2ExportFileExtension(0, true), "mp4") == 0 &&
              strcmp(PV2ExportFileExtension(0, false), "mov") == 0 &&
              strcmp(PV2ExportFileExtension(1, true), "m4a") == 0 &&
              strcmp(PV2ExportFileExtension(2, true), "m4a") == 0, "container policy");
    char stem[64];
    PV2ExportFileStem(stem, sizeof(stem), 0, 2.0);
    CHECK(strcmp(stem, "Video-2x") == 0, "video stem (%s)", stem);
    PV2ExportFileStem(stem, sizeof(stem), 1, 3.0);
    CHECK(strcmp(stem, "Audio-1x") == 0, "mode 1 stem (%s)", stem);
    PV2ExportFileStem(stem, sizeof(stem), 2, 1.5);
    CHECK(strcmp(stem, "Audio-1.5x") == 0, "audio stem (%s)", stem);
    CHECK(strcmp(PV2ExportReasonString(PV2ExportReasonNoAudioTrack), "no-audio-track") == 0 &&
              strcmp(PV2ExportReasonString(PV2ExportReasonUnsupportedEdit), "unsupported-edit") == 0,
          "reason strings");

    if (failures == 0) {
        printf("PASS: export core host check\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d check(s)\n", failures);
    return 1;
}
