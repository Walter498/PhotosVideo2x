#pragma once
// ExportComposition.h - shared time-scaling / edit-preserving composition core for the
// PhotosVideo2x export options (video at current rate, audio at normal rate, audio at current rate).
//
// Two layers, both header-only:
//   Part 1: portable scalar core. No Foundation, no media frameworks, deterministic, host-testable
//           (compiled and executed by tests/ExportCoreHostCheck.cpp on Linux and by
//            tests/ExportMediaTests.mm on macOS).
//   Part 2: AVFoundation bridge (compiled only when the SDK is present). It turns the Part 1 plan
//           into an AVMutableComposition, and re-maps the item's existing videoComposition /
//           audioMix onto the new track IDs. If a construct cannot be re-mapped safely it returns
//           nil with a reason instead of silently dropping the effect.
//
// Requires C++17 (the tweak and the tests are built with -std=c++17) and ARC at the call site.
// No private API, no private class casts, no `typeof`.

#if !defined(__cplusplus)
#error "ExportComposition.h requires C++ (build the tweak/tests as ObjC++ with -std=c++17)."
#endif

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// ---------------------------------------------------------------------------------------------
// Part 1 - portable scalar core
// ---------------------------------------------------------------------------------------------

// Export modes: 0 = video + audio at the current rate, 1 = audio only, normal rate (rate forced to
// 1.0 regardless of the caller's speed), 2 = audio only at the current rate.
typedef enum {
    PV2ExportModeVideo       = 0,
    PV2ExportModeAudioNormal = 1,
    PV2ExportModeAudioSpeed  = 2
} PV2ExportMode;

typedef enum {
    PV2ExportOutcomeSucceeded = 0,
    PV2ExportOutcomeFailed    = 1,
    PV2ExportOutcomeCancelled = 2
} PV2ExportOutcome;

typedef enum {
    PV2ExportReasonNone = 0,
    PV2ExportReasonBadRequest,
    PV2ExportReasonNoOwner,
    PV2ExportReasonAlreadyRunning,
    PV2ExportReasonAssetNotLoaded,
    PV2ExportReasonNotPlayable,
    PV2ExportReasonBadDuration,
    PV2ExportReasonBadRate,
    PV2ExportReasonNoVideoTrack,
    PV2ExportReasonNoAudioTrack,
    PV2ExportReasonUnsupportedEdit,
    PV2ExportReasonCompositionFailed,
    PV2ExportReasonExportFailed,
    PV2ExportReasonCancelled
} PV2ExportReason;

// Rate bounds. AVFoundation itself accepts a wide range; anything outside this window is treated as
// a caller bug (NaN / inf / 0 / negative / absurd) and reported instead of reaching the exporter.
#define PV2_EXPORT_MIN_RATE 0.0625
#define PV2_EXPORT_MAX_RATE 16.0
// Longest single resource we are willing to scale (guards against a bogus duration such as
// kCMTimeIndefinite being read as a huge number).
#define PV2_EXPORT_MAX_DURATION_SECONDS 86400.0
// Ramp probing: ramp boundaries cannot be enumerated through public API, so ramps are discovered by
// probing at frame cadence and jumping to each discovered ramp's exact end. The step is clamped into
// [1/240s, 1/10s]; ramps shorter than the step can be missed (documented limitation).
#define PV2_EXPORT_PROBE_STEP_MIN_SECONDS (1.0 / 240.0)
#define PV2_EXPORT_PROBE_STEP_MAX_SECONDS (1.0 / 10.0)
#define PV2_EXPORT_MAX_RAMP_PROBES 1000000UL
// Serialised plan capacity. A slow-motion composition made by Photos has a handful of segments.
#define PV2_EXPORT_MAX_SEGMENTS 4096U
#define PV2_EXPORT_MAX_ACTIONS  8192U
// Tolerance used to decide whether two edit-segment boundaries are the same instant.
#define PV2_EXPORT_TIME_EPSILON 0.000001
// Files kept because the owner was gone when the export finished are swept later (seconds).
#define PV2_EXPORT_ORPHAN_TTL_SECONDS 900.0

typedef struct {
    int mode;              // one of PV2ExportMode
    bool includeVideo;
    bool includeAudio;
    double rate;           // already resolved: mode 1 always resolves to 1.0
} PV2ExportSpec;

typedef struct {
    bool isEmpty;          // edit segment with no samples (gap in an existing composition)
    double targetStart;    // start inside the source track's own time base (seconds)
    double targetDuration; // segment duration (seconds)
} PV2ExportSegment;

typedef enum {
    PV2ExportActionInsertMedia = 0,
    PV2ExportActionInsertEmpty = 1
} PV2ExportActionKind;

typedef struct {
    PV2ExportActionKind kind;
    double sourceStart;     // media: range to copy out of the source track (seconds)
    double sourceDuration;
    double outputStart;     // where it lands in the new composition (seconds)
    double outputDuration;  // scaled duration (seconds)
} PV2ExportAction;

typedef struct {
    bool presentShare;      // owner still alive and visible -> share sheet
    bool keepOrphanFile;    // finished but owner gone -> keep output for the TTL window
    bool deleteFile;        // failure / cancellation -> output must not survive
    bool showError;         // surface the failure to the user
} PV2ExportPresentation;

static inline const char *PV2ExportModeName(int mode) {
    switch (mode) {
        case PV2ExportModeVideo:       return "video-speed";
        case PV2ExportModeAudioNormal: return "audio-normal";
        case PV2ExportModeAudioSpeed:  return "audio-speed";
        default:                       return "unknown";
    }
}

static inline const char *PV2ExportReasonString(PV2ExportReason reason) {
    switch (reason) {
        case PV2ExportReasonNone:               return "none";
        case PV2ExportReasonBadRequest:         return "bad-request";
        case PV2ExportReasonNoOwner:            return "no-owner";
        case PV2ExportReasonAlreadyRunning:     return "already-running";
        case PV2ExportReasonAssetNotLoaded:     return "asset-load-failed";
        case PV2ExportReasonNotPlayable:        return "not-playable";
        case PV2ExportReasonBadDuration:        return "bad-duration";
        case PV2ExportReasonBadRate:            return "bad-rate";
        case PV2ExportReasonNoVideoTrack:       return "no-video-track";
        case PV2ExportReasonNoAudioTrack:       return "no-audio-track";
        case PV2ExportReasonUnsupportedEdit:    return "unsupported-edit";
        case PV2ExportReasonCompositionFailed:  return "composition-failed";
        case PV2ExportReasonExportFailed:       return "export-failed";
        case PV2ExportReasonCancelled:          return "cancelled";
        default:                                return "unknown";
    }
}

static inline bool PV2ExportRateIsValid(double rate) {
    if (!isfinite(rate)) return false;
    return rate >= PV2_EXPORT_MIN_RATE && rate <= PV2_EXPORT_MAX_RATE;
}

static inline bool PV2ExportDurationIsValid(double seconds) {
    if (!isfinite(seconds)) return false;
    return seconds > 0.0 && seconds <= PV2_EXPORT_MAX_DURATION_SECONDS;
}

static inline bool PV2ExportTimeIsValid(double seconds) {
    return isfinite(seconds) && seconds >= 0.0 && seconds <= PV2_EXPORT_MAX_DURATION_SECONDS;
}

static inline double PV2ExportScaledTime(double seconds, double rate) {
    if (!isfinite(seconds) || !PV2ExportRateIsValid(rate)) return NAN;
    return seconds / rate;
}

static inline bool PV2ExportResolveSpec(int mode, double speed, PV2ExportSpec *out) {
    if (!out) return false;
    PV2ExportSpec spec;
    spec.mode = mode;
    if (mode == (int)PV2ExportModeVideo) {
        spec.includeVideo = true;
        spec.includeAudio = true;
        spec.rate = speed;
    } else if (mode == (int)PV2ExportModeAudioNormal) {
        spec.includeVideo = false;
        spec.includeAudio = true;
        spec.rate = 1.0;              // mode 1 is normal speed by definition
    } else if (mode == (int)PV2ExportModeAudioSpeed) {
        spec.includeVideo = false;
        spec.includeAudio = true;
        spec.rate = speed;
    } else {
        return false;
    }
    if (!PV2ExportRateIsValid(spec.rate)) return false;
    *out = spec;
    return true;
}

// allowMissingAudio: the module is strict by default (a missing audio track is an error, as
// requested). Callers that prefer a silent video export can pass true and get video-only output.
static inline PV2ExportReason PV2ExportValidateTracks(const PV2ExportSpec *spec, bool hasVideo,
                                                      bool hasAudio, bool allowMissingAudio) {
    if (!spec) return PV2ExportReasonBadRequest;
    if (spec->includeVideo && !hasVideo) return PV2ExportReasonNoVideoTrack;
    if (spec->includeAudio && !hasAudio && !allowMissingAudio) return PV2ExportReasonNoAudioTrack;
    if (!spec->includeVideo && !spec->includeAudio) return PV2ExportReasonBadRequest;
    return PV2ExportReasonNone;
}

static inline bool PV2ExportRequestIsValid(bool hasOwner, bool hasItem, int mode, double speed) {
    if (!hasOwner || !hasItem) return false;
    PV2ExportSpec spec;
    return PV2ExportResolveSpec(mode, speed, &spec);
}

// Maps the source track's edit segments onto the scaled output timeline.
// Contiguous non-empty segments are merged into one media action (so a single copy covers a whole
// slow-motion edit), gaps become empty actions, and every action carries the scaled output position.
// Returns the total output duration in seconds, or <= 0 when the plan does not fit in `capacity`.
static inline double PV2ExportBuildPlan(const PV2ExportSegment *segments, size_t count, double rate,
                                        bool scaleTimes, PV2ExportAction *actions, size_t capacity,
                                        size_t *outCount, double *outDuration) {
    if (outCount) *outCount = 0;
    if (outDuration) *outDuration = 0.0;
    if (!segments || count == 0 || !actions || capacity == 0 || !outCount || !outDuration) return 0.0;
    if (count > PV2_EXPORT_MAX_SEGMENTS) return 0.0;
    if (scaleTimes && !PV2ExportRateIsValid(rate)) return 0.0;
    const double factor = scaleTimes ? (1.0 / rate) : 1.0;

    size_t used = 0;
    double cursor = 0.0;
    size_t index = 0;
    while (index < count) {
        const PV2ExportSegment segment = segments[index];
        if (!(segment.targetDuration > PV2_EXPORT_TIME_EPSILON)) { index++; continue; }
        if (segment.isEmpty) {
            if (used >= capacity) return 0.0;
            PV2ExportAction action;
            action.kind = PV2ExportActionInsertEmpty;
            action.sourceStart = segment.targetStart;
            action.sourceDuration = segment.targetDuration;
            action.outputStart = cursor;
            action.outputDuration = segment.targetDuration * factor;
            actions[used++] = action;
            cursor += action.outputDuration;
            index++;
            continue;
        }
        const double runStart = segment.targetStart;
        double runDuration = segment.targetDuration;
        size_t next = index + 1;
        while (next < count) {
            const PV2ExportSegment candidate = segments[next];
            if (candidate.isEmpty) break;
            if (!(candidate.targetDuration > PV2_EXPORT_TIME_EPSILON)) { next++; continue; }
            const double expected = runStart + runDuration;
            if (fabs(candidate.targetStart - expected) > PV2_EXPORT_TIME_EPSILON) break;
            runDuration += candidate.targetDuration;
            next++;
        }
        if (used >= capacity) return 0.0;
        PV2ExportAction action;
        action.kind = PV2ExportActionInsertMedia;
        action.sourceStart = runStart;
        action.sourceDuration = runDuration;
        action.outputStart = cursor;
        action.outputDuration = runDuration * factor;
        actions[used++] = action;
        cursor += action.outputDuration;
        index = next;
    }
    *outCount = used;
    *outDuration = cursor;
    return cursor;
}

static inline double PV2ExportPlanTotalDuration(const PV2ExportAction *actions, size_t count) {
    if (!actions) return 0.0;
    double total = 0.0;
    for (size_t i = 0; i < count; i++) total += actions[i].outputDuration;
    return total;
}

// What to do once the export finished (pure policy, so it can be tested without UIKit).
static inline PV2ExportPresentation PV2ExportDecidePresentation(PV2ExportOutcome outcome,
                                                               bool ownerAlive, bool ownerVisible) {
    PV2ExportPresentation decision;
    decision.presentShare = false;
    decision.keepOrphanFile = false;
    decision.deleteFile = false;
    decision.showError = false;
    if (outcome == PV2ExportOutcomeSucceeded) {
        const bool reachable = ownerAlive && ownerVisible;
        decision.presentShare = reachable;
        decision.keepOrphanFile = !reachable;
    } else if (outcome == PV2ExportOutcomeFailed) {
        decision.deleteFile = true;
        decision.showError = ownerAlive && ownerVisible;
    } else {
        decision.deleteFile = true;
    }
    return decision;
}

static inline double PV2ExportScaledFrameDurationSeconds(double frameDurationSeconds, double rate) {
    if (!isfinite(frameDurationSeconds) || frameDurationSeconds <= 0.0) return frameDurationSeconds;
    if (!PV2ExportRateIsValid(rate)) return frameDurationSeconds;
    return frameDurationSeconds / rate;
}

static inline double PV2ExportProbeStepSeconds(double frameDurationSeconds) {
    double step = (isfinite(frameDurationSeconds) && frameDurationSeconds > 0.0)
                      ? frameDurationSeconds : (1.0 / 30.0);
    if (step < PV2_EXPORT_PROBE_STEP_MIN_SECONDS) step = PV2_EXPORT_PROBE_STEP_MIN_SECONDS;
    if (step > PV2_EXPORT_PROBE_STEP_MAX_SECONDS) step = PV2_EXPORT_PROBE_STEP_MAX_SECONDS;
    return step;
}

static inline bool PV2ExportProbeBudgetFits(double durationSeconds, double stepSeconds) {
    if (!(durationSeconds > 0.0) || !(stepSeconds > 0.0)) return false;
    return (durationSeconds / stepSeconds) <= (double)PV2_EXPORT_MAX_RAMP_PROBES;
}

// Output-container policy: video keeps the current rate into mp4 when the exporter offers it (mov
// otherwise); both audio modes are m4a.
static inline const char *PV2ExportFileExtension(int mode, bool mp4Available) {
    if (mode == (int)PV2ExportModeVideo) return mp4Available ? "mp4" : "mov";
    return "m4a";
}

static inline void PV2ExportFileStem(char *buffer, size_t capacity, int mode, double rate) {
    if (!buffer || capacity == 0) return;
    const double shown = (mode == (int)PV2ExportModeAudioNormal) ? 1.0 : rate;
    if (mode == (int)PV2ExportModeVideo) {
        snprintf(buffer, capacity, "Video-%gx", shown);
    } else if (mode == (int)PV2ExportModeAudioNormal || mode == (int)PV2ExportModeAudioSpeed) {
        snprintf(buffer, capacity, "Audio-%gx", shown);
    } else {
        snprintf(buffer, capacity, "Media-%gx", shown);
    }
}

// ---------------------------------------------------------------------------------------------
// Part 2 - AVFoundation bridge (present only when the SDK is available)
// ---------------------------------------------------------------------------------------------

#if defined(__OBJC__) && defined(__has_include)
#if __has_include(<AVFoundation/AVFoundation.h>)

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreGraphics/CoreGraphics.h>

// iOS 16 / macOS 13 SDKs deprecate the synchronous AVAsset/AVAssetTrack accessors in favour of the
// async `load(...)` API. The tweak must stay on the iOS 16.5 SDK and the export module must not
// block the main thread, so every deprecated accessor is funnelled through the small helpers below
// (called only after the async key loading succeeded) instead of being sprinkled over the module.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static inline CMTime PV2ExportAssetDuration(AVAsset *asset) {
    return asset ? asset.duration : kCMTimeInvalid;
}
static inline NSArray<AVAssetTrack *> *PV2ExportAssetTracks(AVAsset *asset) {
    return asset ? asset.tracks : (NSArray<AVAssetTrack *> *)@[];
}
static inline NSArray<AVAssetTrack *> *PV2ExportAssetTracksOfType(AVAsset *asset, AVMediaType type) {
    return asset ? [asset tracksWithMediaType:type] : (NSArray<AVAssetTrack *> *)@[];
}
static inline BOOL PV2ExportAssetIsPlayable(AVAsset *asset) {
    return asset ? asset.playable : NO;
}
static inline CGAffineTransform PV2ExportTrackPreferredTransform(AVAssetTrack *track) {
    return track ? track.preferredTransform : CGAffineTransformIdentity;
}
static inline float PV2ExportTrackPreferredVolume(AVAssetTrack *track) {
    return track ? track.preferredVolume : 1.0f;
}
static inline CMTimeRange PV2ExportTrackTimeRange(AVAssetTrack *track) {
    return track ? track.timeRange : kCMTimeRangeZero;
}
static inline CMTimeScale PV2ExportTrackNaturalTimeScale(AVAssetTrack *track) {
    return track ? track.naturalTimeScale : 0;
}
static inline BOOL PV2ExportTrackIsEnabled(AVAssetTrack *track) {
    return track ? track.enabled : YES;
}
static inline float PV2ExportTrackNominalFrameRate(AVAssetTrack *track) {
    return track ? track.nominalFrameRate : 0.0f;
}
#pragma clang diagnostic pop

@interface PV2ExportCompositionResult : NSObject
@property (nonatomic, strong) AVMutableComposition *composition;
@property (nonatomic, strong) NSDictionary<NSNumber *, NSNumber *> *trackIDMap;
@property (nonatomic) double targetDurationSeconds;
@property (nonatomic) double outputDurationSeconds;
@property (nonatomic) PV2ExportReason reason;
@property (nonatomic, copy) NSString *failureDetail;
@end

@implementation PV2ExportCompositionResult
@end

static inline BOOL PV2ExportCMTimeIsUsable(CMTime time) {
    return CMTIME_IS_VALID(time) && CMTIME_IS_NUMERIC(time) && isfinite(CMTimeGetSeconds(time));
}

static inline double PV2ExportCMTimeSeconds(CMTime time) {
    return PV2ExportCMTimeIsUsable(time) ? CMTimeGetSeconds(time) : NAN;
}

static inline CMTime PV2ExportCMTimeMake(double seconds, CMTimeScale timescale) {
    const CMTimeScale usable = timescale>=60000 ? timescale : 60000;
    if (!isfinite(seconds)) return kCMTimeInvalid;
    return CMTimeMakeWithSeconds(seconds, usable);
}

static inline CMTime PV2ExportCMTimeScaled(CMTime time, double rate) {
    if (!PV2ExportCMTimeIsUsable(time) || !PV2ExportRateIsValid(rate)) return time;
    return PV2ExportCMTimeMake(CMTimeGetSeconds(time) / rate, time.timescale);
}

static inline CMTimeRange PV2ExportCMTimeRangeScaled(CMTimeRange range, double rate) {
    if (!CMTIMERANGE_IS_VALID(range) || !PV2ExportRateIsValid(rate)) return range;
    CMTimeRange scaled;
    scaled.start = PV2ExportCMTimeScaled(range.start, rate);
    scaled.duration = PV2ExportCMTimeScaled(range.duration, rate);
    return scaled;
}

static inline BOOL PV2ExportCMTimeNearlyEqual(CMTime a, CMTime b, double toleranceSeconds) {
    if (!PV2ExportCMTimeIsUsable(a) || !PV2ExportCMTimeIsUsable(b)) return NO;
    return fabs(CMTimeGetSeconds(a) - CMTimeGetSeconds(b)) <= toleranceSeconds;
}

static inline CMTimeScale PV2ExportTrackOutputTimescale(AVAssetTrack *track) {
    const CMTimeScale natural = PV2ExportTrackNaturalTimeScale(track);
    if (natural > 0) return natural;
    return [track.mediaType isEqualToString:AVMediaTypeAudio] ? 44100 : 600;
}

static inline BOOL PV2ExportTrackIDMapIsIdentity(NSDictionary<NSNumber *, NSNumber *> *map) {
    if (map.count == 0) return YES;
    for (NSNumber *key in map) {
        NSNumber *value = map[key];
        if (![key isEqual:value]) return NO;
    }
    return YES;
}

// Copies the presentation-relevant properties of a source track onto a composition track.
// preferredTransform carries the vertical/horizontal orientation of the source video.
static inline void PV2ExportCopyTrackProperties(AVAssetTrack *source, AVMutableCompositionTrack *destination) {
    if (!source || !destination) return;
    destination.preferredTransform = PV2ExportTrackPreferredTransform(source);
    destination.preferredVolume = PV2ExportTrackPreferredVolume(source);
    destination.enabled = PV2ExportTrackIsEnabled(source);
    destination.naturalTimeScale = PV2ExportTrackNaturalTimeScale(source);
    if (source.languageCode.length > 0) destination.languageCode = source.languageCode;
}

// Copies the content of one source track into `destination`, honouring the source's own edit
// segments (a slow-motion AVComposition has several, including gaps) and applying the time scaling
// per contiguous run so that empty segments are never scaled over.
static inline BOOL PV2ExportCopyTrackContent(AVAssetTrack *source, AVMutableCompositionTrack *destination,
                                      double rate, BOOL scaleTimes, NSString **detailOut) {
    if (!source || !destination) {
        if (detailOut) *detailOut = @"missing source or destination track";
        return NO;
    }
    const CMTimeRange sourceRange = PV2ExportTrackTimeRange(source);
    const double sourceStart = PV2ExportCMTimeSeconds(sourceRange.start);
    const double sourceDuration = PV2ExportCMTimeSeconds(sourceRange.duration);
    if (!PV2ExportDurationIsValid(sourceDuration) || !PV2ExportTimeIsValid(sourceStart)) {
        if (detailOut) *detailOut = @"source track has no usable time range";
        return NO;
    }

    PV2ExportSegment segmentBuffer[PV2_EXPORT_MAX_SEGMENTS];
    size_t segmentCount = 0;
    NSArray *compositionSegments = nil;
    if ([source isKindOfClass:[AVCompositionTrack class]]) {
        compositionSegments = [(AVCompositionTrack *)source segments];
    }
    if (compositionSegments.count > 0 && compositionSegments.count <= PV2_EXPORT_MAX_SEGMENTS) {
        for (AVCompositionTrackSegment *segment in compositionSegments) {
            const CMTimeRange target = segment.timeMapping.target;
            const double start = PV2ExportCMTimeSeconds(target.start);
            const double duration = PV2ExportCMTimeSeconds(target.duration);
            if (!PV2ExportTimeIsValid(start) || !PV2ExportDurationIsValid(duration)) continue;
            PV2ExportSegment item;
            item.isEmpty = segment.isEmpty ? true : false;
            item.targetStart = start;
            item.targetDuration = duration;
            segmentBuffer[segmentCount++] = item;
        }
    }
    if (segmentCount == 0) {
        // Plain URL asset track (or unreadable segments): one media segment covering the track.
        PV2ExportSegment whole;
        whole.isEmpty = false;
        whole.targetStart = sourceStart;
        whole.targetDuration = sourceDuration;
        segmentBuffer[segmentCount++] = whole;
    }

    PV2ExportAction actionBuffer[PV2_EXPORT_MAX_ACTIONS];
    size_t actionCount = 0;
    double planDuration = 0.0;
    if (PV2ExportBuildPlan(segmentBuffer, segmentCount, rate, scaleTimes ? true : false,
                           actionBuffer, PV2_EXPORT_MAX_ACTIONS, &actionCount, &planDuration) <= 0.0
        || actionCount == 0) {
        if (detailOut) *detailOut = @"time plan could not be built from the source segments";
        return NO;
    }

    // A track that starts later than the composition origin keeps its (scaled) offset.
    const double offset = scaleTimes ? (sourceStart / rate) : sourceStart;
    const CMTimeScale timescale = PV2ExportTrackOutputTimescale(source);
    for (size_t i = 0; i < actionCount; i++) {
        const PV2ExportAction action = actionBuffer[i];
        const CMTime outputStart = PV2ExportCMTimeMake(action.outputStart + offset, timescale);
        const CMTime unscaledDuration = PV2ExportCMTimeMake(action.sourceDuration, timescale);
        const CMTime scaledDuration = PV2ExportCMTimeMake(action.outputDuration, timescale);
        if (!PV2ExportCMTimeIsUsable(outputStart) || !PV2ExportCMTimeIsUsable(scaledDuration)) {
            if (detailOut) *detailOut = @"non-numeric time while placing segments";
            return NO;
        }
        if (action.kind == PV2ExportActionInsertEmpty) {
            [destination insertEmptyTimeRange:CMTimeRangeMake(outputStart, scaledDuration)];
            continue;
        }
        const CMTimeRange sourceSub = CMTimeRangeMake(PV2ExportCMTimeMake(action.sourceStart, timescale),
                                                      unscaledDuration);
        NSError *insertError = nil;
        if (![destination insertTimeRange:sourceSub ofTrack:source atTime:outputStart error:&insertError]) {
            if (detailOut) {
                *detailOut = [NSString stringWithFormat:@"segment insert failed (%@)",
                              insertError.localizedDescription ?: @"no error"];
            }
            return NO;
        }
        if (scaleTimes && fabs(rate - 1.0) > 1e-9) {
            // The range must be the one currently present in the track (unscaled length).
            [destination scaleTimeRange:CMTimeRangeMake(outputStart, unscaledDuration) toDuration:scaledDuration];
        }
    }

    const double expected = offset + planDuration;
    const double actual = PV2ExportCMTimeSeconds(destination.timeRange.duration);
    if (!PV2ExportDurationIsValid(actual) || fabs(actual - expected) > 0.05) {
        if (detailOut) {
            *detailOut = [NSString stringWithFormat:@"track duration mismatch (expected %.4fs, got %.4fs)",
                          expected, actual];
        }
        return NO;
    }
    return YES;
}

// Builds the time-scaled composition for one export run. Existing edit segments, gaps and the
// source orientation are preserved; source track IDs are requested for the new tracks so that the
// item's videoComposition / audioMix keep pointing at the same media.
static inline PV2ExportCompositionResult *PV2ExportBuildScaledComposition(AVAsset *asset, double rate,
                                                                  BOOL includeVideo, BOOL includeAudio,
                                                                  BOOL allowMissingAudio) {
    PV2ExportCompositionResult *result = [PV2ExportCompositionResult new];
    result.reason = PV2ExportReasonCompositionFailed;
    if (!asset) {
        result.reason = PV2ExportReasonBadRequest;
        result.failureDetail = @"no asset";
        return result;
    }
    if (!PV2ExportRateIsValid(rate)) {
        result.reason = PV2ExportReasonBadRate;
        result.failureDetail = [NSString stringWithFormat:@"unsupported rate %.4f", rate];
        return result;
    }
    const double assetDuration = PV2ExportCMTimeSeconds(PV2ExportAssetDuration(asset));
    if (!PV2ExportDurationIsValid(assetDuration)) {
        result.reason = PV2ExportReasonBadDuration;
        result.failureDetail = @"asset duration is not a usable finite value";
        return result;
    }

    NSArray<AVAssetTrack *> *videoTracks = includeVideo ? PV2ExportAssetTracksOfType(asset, AVMediaTypeVideo)
                                                       : (NSArray<AVAssetTrack *> *)@[];
    NSArray<AVAssetTrack *> *audioTracks = includeAudio ? PV2ExportAssetTracksOfType(asset, AVMediaTypeAudio)
                                                       : (NSArray<AVAssetTrack *> *)@[];
    PV2ExportSpec spec;
    spec.mode = -1;
    spec.includeVideo = includeVideo ? true : false;
    spec.includeAudio = includeAudio ? true : false;
    spec.rate = rate;
    const PV2ExportReason trackReason = PV2ExportValidateTracks(&spec, videoTracks.count > 0,
                                                               audioTracks.count > 0,
                                                               allowMissingAudio ? true : false);
    if (trackReason != PV2ExportReasonNone) {
        result.reason = trackReason;
        result.failureDetail = [NSString stringWithUTF8String:PV2ExportReasonString(trackReason)];
        return result;
    }

    AVMutableComposition *composition = [AVMutableComposition composition];
    NSMutableDictionary<NSNumber *, NSNumber *> *idMap = [NSMutableDictionary dictionary];
    const BOOL scaleTimes = (fabs(rate - 1.0) > 1e-9);

    NSMutableArray<AVAssetTrack *> *orderedSources = [NSMutableArray array];
    [orderedSources addObjectsFromArray:videoTracks];
    [orderedSources addObjectsFromArray:audioTracks];
    for (AVAssetTrack *source in orderedSources) {
        AVMutableCompositionTrack *destination =
            [composition addMutableTrackWithMediaType:source.mediaType
                                    preferredTrackID:source.trackID];
        if (!destination) {
            result.failureDetail = @"composition refused a track";
            return result;
        }
        PV2ExportCopyTrackProperties(source, destination);
        NSString *detail = nil;
        if (!PV2ExportCopyTrackContent(source, destination, rate, scaleTimes, &detail)) {
            result.failureDetail = detail ?: @"track copy failed";
            return result;
        }
        if (destination.trackID != source.trackID) {
            idMap[@(source.trackID)] = @(destination.trackID);
        }
    }

    double mediaDuration=assetDuration;
    if (!includeVideo && includeAudio) {
        // Trailing empty composition edits are not encoded as silence by AVFoundation.
        // Export the complete audio timeline, not the silent tail of a longer video.
        mediaDuration=0;
        for (AVAssetTrack *a in audioTracks) mediaDuration=fmax(mediaDuration,PV2ExportCMTimeSeconds(CMTimeRangeGetEnd(a.timeRange)));
    }
    const double target = PV2ExportScaledTime(mediaDuration, rate);
    if (!includeVideo && includeAudio) {
        // Audio-only output must still cover the whole resource, so pad short audio tracks with
        // silence instead of returning a truncated file.
        for (AVAssetTrack *track in audioTracks) {
            AVMutableCompositionTrack *destination = nil;
            for (AVMutableCompositionTrack *candidate in [composition tracksWithMediaType:AVMediaTypeAudio]) {
                CMPersistentTrackID mappedID = idMap[@(track.trackID)] ? idMap[@(track.trackID)].intValue : track.trackID;
                if (candidate.trackID == mappedID) { destination = candidate; break; }
            }
            if (!destination) continue;
            const double current = PV2ExportCMTimeSeconds(destination.timeRange.duration);
            if (PV2ExportTimeIsValid(current) && current < target - PV2_EXPORT_TIME_EPSILON) {
                const CMTimeScale timescale = PV2ExportTrackOutputTimescale(track);
                [destination insertEmptyTimeRange:
                    CMTimeRangeMake(PV2ExportCMTimeMake(current, timescale),
                                    PV2ExportCMTimeMake(target - current, timescale))];
            }
        }
    }

    double presentDuration = PV2ExportCMTimeSeconds(composition.duration);
    if (!includeVideo && includeAudio && isfinite(presentDuration) && presentDuration<target-PV2_EXPORT_TIME_EPSILON)
        [composition insertEmptyTimeRange:CMTimeRangeMake(PV2ExportCMTimeMake(presentDuration,60000),PV2ExportCMTimeMake(target-presentDuration,60000))];
    const double outputDuration = PV2ExportCMTimeSeconds(composition.duration);
    const double tolerance = fmax(0.5, target * 0.005);
    if (!PV2ExportDurationIsValid(outputDuration) || fabs(outputDuration - target) > tolerance) {
        result.failureDetail = [NSString stringWithFormat:@"composition duration %.3fs differs from %.3fs",
                                outputDuration, target];
        return result;
    }

    result.composition = composition;
    result.trackIDMap = idMap;
    result.targetDurationSeconds = target;
    result.outputDurationSeconds = outputDuration;
    result.reason = PV2ExportReasonNone;
    return result;
}

// ---------------------------------------------------------------------------------------------
// Existing-edit remapping: videoComposition / audioMix
//
// Public API cannot enumerate the transform / opacity / crop ramps of a layer instruction
// (getXxxRampForTime: only answers "which ramp covers this instant"). Ramps are therefore
// rediscovered by probing forward at frame cadence and jumping to each discovered ramp's exact end,
// then re-applied with scaled time ranges. Anything that cannot be re-mapped exactly (custom
// instructions, custom compositors, a CoreAnimation tool, ambiguous audio parameters, an
// exhausted probe budget, a forced track-ID remap) is *reported* through `detailOut` and returns
// nil - the caller must abort instead of exporting a silently degraded video.
// ---------------------------------------------------------------------------------------------

static inline BOOL PV2ExportRewriteLayerRamps(AVVideoCompositionLayerInstruction *source,
                                       AVMutableVideoCompositionLayerInstruction *destination,
                                       double rate, CMTime probe, CMTime end, NSString **detailOut) {
    if (!source || !destination) return YES;
    if (CMTimeCompare(end, kCMTimeZero) <= 0) return YES;
    const BOOL scale = (fabs(rate - 1.0) > 1e-9);

    for (int pass = 0; pass < 3; pass++) {
        CMTime cursor = kCMTimeZero;
        NSUInteger probes = 0;
        while (CMTimeCompare(cursor, end) < 0) {
            if (probes++ >= PV2_EXPORT_MAX_RAMP_PROBES) {
                if (detailOut) *detailOut = @"ramp probe budget exhausted";
                return NO;
            }
            CMTimeRange range = kCMTimeRangeZero;
            BOOL found = NO;
            if (pass == 0) {
                CGAffineTransform startTransform = CGAffineTransformIdentity;
                CGAffineTransform endTransform = CGAffineTransformIdentity;
                found = [source getTransformRampForTime:cursor startTransform:&startTransform
                                           endTransform:&endTransform timeRange:&range];
                if (found) {
                    if (PV2ExportCMTimeSeconds(range.duration) > 0.0) {
                        [destination setTransformRampFromStartTransform:startTransform
                                                          toEndTransform:endTransform
                                                               timeRange:(scale ? PV2ExportCMTimeRangeScaled(range, rate) : range)];
                    } else {
                        [destination setTransform:startTransform
                                           atTime:(scale ? PV2ExportCMTimeScaled(range.start, rate) : range.start)];
                    }
                }
            } else if (pass == 1) {
                float startOpacity = 0.0f, endOpacity = 0.0f;
                found = [source getOpacityRampForTime:cursor startOpacity:&startOpacity
                                           endOpacity:&endOpacity timeRange:&range];
                if (found) {
                    if (PV2ExportCMTimeSeconds(range.duration) > 0.0) {
                        [destination setOpacityRampFromStartOpacity:startOpacity
                                                        toEndOpacity:endOpacity
                                                          timeRange:(scale ? PV2ExportCMTimeRangeScaled(range, rate) : range)];
                    } else {
                        [destination setOpacity:startOpacity
                                          atTime:(scale ? PV2ExportCMTimeScaled(range.start, rate) : range.start)];
                    }
                }
            } else {
                CGRect startCrop = CGRectNull, endCrop = CGRectNull;
                found = [source getCropRectangleRampForTime:cursor startCropRectangle:&startCrop
                                           endCropRectangle:&endCrop timeRange:&range];
                if (found) {
                    if (PV2ExportCMTimeSeconds(range.duration) > 0.0) {
                        [destination setCropRectangleRampFromStartCropRectangle:startCrop
                                                             toEndCropRectangle:endCrop
                                                                      timeRange:(scale ? PV2ExportCMTimeRangeScaled(range, rate) : range)];
                    } else {
                        [destination setCropRectangle:startCrop
                                               atTime:(scale ? PV2ExportCMTimeScaled(range.start, rate) : range.start)];
                    }
                }
            }
            if (found && PV2ExportCMTimeSeconds(range.duration) > 0.0) {
                cursor = CMTimeAdd(range.start, range.duration);
            } else {
                cursor = CMTimeAdd(cursor, probe);
            }
        }
    }
    return YES;
}

// `customVideoCompositorClass` is declared through the instruction API, but which class exposes it
// differs between SDK revisions; query it dynamically instead of assuming a declaration.
static inline BOOL PV2ExportInstructionHasCustomCompositor(id instruction) {
    if (![instruction respondsToSelector:@selector(customVideoCompositorClass)]) return NO;
    return [instruction customVideoCompositorClass] != nil;
}

static inline AVMutableVideoComposition *PV2ExportDefaultVideoComposition(AVMutableComposition *asset) {
    AVAssetTrack *track=PV2ExportAssetTracksOfType(asset,AVMediaTypeVideo).firstObject;
    if (!track) return nil;
    CGAffineTransform transform=track.preferredTransform;
    CGRect rect=CGRectApplyAffineTransform(CGRectMake(0,0,track.naturalSize.width,track.naturalSize.height),transform);
    transform=CGAffineTransformConcat(transform,CGAffineTransformMakeTranslation(-rect.origin.x,-rect.origin.y));
    AVMutableVideoComposition *v=[AVMutableVideoComposition videoComposition];
    v.renderSize=CGSizeMake(fabs(rect.size.width),fabs(rect.size.height));
    float fps=PV2ExportTrackNominalFrameRate(track);v.frameDuration=CMTimeMake(1,(int32_t)(fps>0 ? lround(fps) : 30));
    AVMutableVideoCompositionInstruction *i=[AVMutableVideoCompositionInstruction videoCompositionInstruction];
    i.timeRange=CMTimeRangeMake(kCMTimeZero,asset.duration);
    AVMutableVideoCompositionLayerInstruction *l=[AVMutableVideoCompositionLayerInstruction videoCompositionLayerInstructionWithAssetTrack:track];
    [l setTransform:transform atTime:kCMTimeZero];i.layerInstructions=@[l];v.instructions=@[i];
    return v;
}

// Clones `source` and re-maps it onto the new composition: instruction time ranges and layer
// animation ramps are scaled by 1/rate, track IDs are re-pointed through `trackIDMap`, and the
// video frame cadence is scaled along with the timeline.
// Returns nil with *detailOut == nil when there is nothing to do (no source video composition).
// Returns nil with *detailOut != nil when the combination cannot be preserved and must be reported.
static inline AVMutableVideoComposition *PV2ExportCompatibleVideoComposition(
    AVVideoComposition *source, NSDictionary<NSNumber *, NSNumber *> *trackIDMap, double rate,
    NSString **detailOut) {
    if (detailOut) *detailOut = nil;
    if (!source) return nil;
    if (!PV2ExportRateIsValid(rate)) {
        if (detailOut) *detailOut = @"unsupported rate";
        return nil;
    }
    const BOOL scale = (fabs(rate - 1.0) > 1e-9);
    const BOOL identityMap = PV2ExportTrackIDMapIsIdentity(trackIDMap);

    AVMutableVideoComposition *copy = [source mutableCopy];
    if (!copy) {
        if (detailOut) *detailOut = @"videoComposition could not be copied";
        return nil;
    }
    if (scale && copy.animationTool != nil) {
        if (detailOut) *detailOut = @"videoComposition 使用 CoreAnimation 动画工具，变速时无法重映射其时间轴";
        return nil;
    }
    const CMTime frameDuration = copy.frameDuration;
    const double frameSeconds = PV2ExportCMTimeSeconds(frameDuration);
    CMTime probe = PV2ExportCMTimeMake(PV2ExportProbeStepSeconds(frameSeconds), 60000);
    if (scale) {
        const double scaledFrame = PV2ExportScaledFrameDurationSeconds(frameSeconds, rate);
        if (!PV2ExportDurationIsValid(scaledFrame)) {
            if (detailOut) *detailOut = @"videoComposition frameDuration 无效";
            return nil;
        }
        copy.frameDuration = PV2ExportCMTimeMake(scaledFrame, frameDuration.timescale);
    }

    NSArray *instructions = source.instructions;
    if (instructions.count == 0) {
        if (scale) {
            if (detailOut) *detailOut = @"videoComposition 没有可重映射的 instruction";
            return nil;
        }
        return copy;
    }
    if (instructions.count > PV2_EXPORT_MAX_SEGMENTS) {
        if (detailOut) *detailOut = @"videoComposition instruction 数量异常";
        return nil;
    }

    NSMutableArray *rewritten = [NSMutableArray arrayWithCapacity:instructions.count];
    for (id instruction in instructions) {
        if (![instruction isKindOfClass:[AVVideoCompositionInstruction class]]) {
            // Custom instruction object (custom compositor): times cannot be rewritten safely.
            if (scale || !identityMap) {
                if (detailOut) *detailOut = @"videoComposition 含自定义 instruction，变速时无法安全重映射";
                return nil;
            }
            [rewritten addObject:instruction];
            continue;
        }
        AVVideoCompositionInstruction *typed = (AVVideoCompositionInstruction *)instruction;
        AVMutableVideoCompositionInstruction *mutableInstruction = [typed mutableCopy];
        if (!mutableInstruction) {
            if (detailOut) *detailOut = @"videoComposition instruction 无法复制";
            return nil;
        }
        if (scale && PV2ExportInstructionHasCustomCompositor(mutableInstruction)) {
            if (detailOut) *detailOut = @"videoComposition 使用自定义合成器，变速时无法验证其时间假设";
            return nil;
        }
        if (!identityMap && typed.requiredSourceTrackIDs.count > 0) {
            if (detailOut) *detailOut = @"videoComposition instruction 声明了 requiredSourceTrackIDs，且轨道 ID 已变化";
            return nil;
        }
        CMTime end = kCMTimeZero;
        if (scale) {
            mutableInstruction.timeRange = PV2ExportCMTimeRangeScaled(typed.timeRange, rate);
            end = PV2ExportCMTimeScaled(CMTimeRangeGetEnd(typed.timeRange), rate);
        } else {
            end = CMTimeRangeGetEnd(typed.timeRange);
        }
        NSMutableArray *layers = [NSMutableArray array];
        for (AVVideoCompositionLayerInstruction *layer in mutableInstruction.layerInstructions) {
            AVMutableVideoCompositionLayerInstruction *layerCopy = scale
                ? [[AVMutableVideoCompositionLayerInstruction alloc] init] : [layer mutableCopy];
            layerCopy.trackID=layer.trackID;
            if (!layerCopy) {
                if (detailOut) *detailOut = @"videoComposition layer instruction 无法复制";
                return nil;
            }
            NSNumber *mapped = trackIDMap[@(layer.trackID)];
            if (mapped && mapped.intValue != layer.trackID) {
                layerCopy.trackID = (CMPersistentTrackID)mapped.intValue;
            }
            if (scale) {
                NSString *rampDetail = nil;
                if (!PV2ExportRewriteLayerRamps(layer, layerCopy, rate, probe, end, &rampDetail)) {
                    if (detailOut) *detailOut = rampDetail ?: @"layer 动画重映射失败";
                    return nil;
                }
            }
            [layers addObject:layerCopy];
        }
        mutableInstruction.layerInstructions = layers;
        [rewritten addObject:mutableInstruction];
    }
    copy.instructions = rewritten;
    return copy;
}

// `audioTimePitchAlgorithm` is the public AVAudioMixInputParameters property that carries the pitch
// policy to the exporter, but not every SDK revision used to build/verify this module declares it, so
// it is applied through a locally declared protocol on the public class behind a guard.
@protocol PV2ExportPitchAlgorithmHost <NSObject>
@property (nonatomic, copy) NSString *audioTimePitchAlgorithm;
@end

static inline void PV2ExportSetPitchAlgorithm(AVMutableAudioMixInputParameters *parameters,
                                              NSString *algorithm) {
    if (!parameters || algorithm.length == 0) return;
    if (![parameters respondsToSelector:@selector(setAudioTimePitchAlgorithm:)]) return;
    [(id<PV2ExportPitchAlgorithmHost>)parameters setAudioTimePitchAlgorithm:algorithm];
}

static inline NSString *PV2ExportPitchAlgorithmOf(AVAudioMixInputParameters *parameters) {
    if (!parameters || ![parameters respondsToSelector:@selector(audioTimePitchAlgorithm)]) return nil;
    return [(id<PV2ExportPitchAlgorithmHost>)parameters audioTimePitchAlgorithm];
}

// Rebuilds the item's audioMix on the new composition: volume ramps are scaled, track IDs are
// re-pointed and the pitch algorithm is (re)applied. Returns nil with *detailOut == nil when no mix
// is needed; nil with *detailOut != nil when the source mix cannot be preserved.
static inline AVAudioMix *PV2ExportCompatibleAudioMix(AVComposition *composition, AVAudioMix *sourceMix,
                                               NSDictionary<NSNumber *, NSNumber *> *trackIDMap,
                                               double rate, NSString *pitchAlgorithm,
                                               double targetDurationSeconds, NSString **detailOut) {
    if (detailOut) *detailOut = nil;
    if (!composition) {
        if (detailOut) *detailOut = @"missing composition";
        return nil;
    }
    NSArray<AVAssetTrack *> *audioTracks = [composition tracksWithMediaType:AVMediaTypeAudio];
    if (audioTracks.count == 0) return nil;
    const BOOL scale = (fabs(rate - 1.0) > 1e-9);
    if (!sourceMix && !(scale && pitchAlgorithm.length > 0)) return nil;
    const double probeStep = PV2ExportProbeStepSeconds(1.0 / 30.0);
    if (!PV2ExportProbeBudgetFits(targetDurationSeconds, probeStep)) {
        if (detailOut) *detailOut = @"audioMix 探测量超出预算（资源过长）";
        return nil;
    }
    const CMTime probe = PV2ExportCMTimeMake(probeStep, 60000);
    const CMTime end = PV2ExportCMTimeMake(targetDurationSeconds * rate, 60000);

    NSMutableDictionary<NSNumber *, NSMutableArray<AVAudioMixInputParameters *> *> *grouped =
        [NSMutableDictionary dictionary];
    for (AVAudioMixInputParameters *parameter in sourceMix.inputParameters) {
        NSNumber *mapped = trackIDMap[@(parameter.trackID)];
        NSNumber *targetID = mapped ?: @(parameter.trackID);
        NSMutableArray<AVAudioMixInputParameters *> *bucket = grouped[targetID];
        if (!bucket) {
            bucket = [NSMutableArray array];
            grouped[targetID] = bucket;
        }
        [bucket addObject:parameter];
    }

    NSMutableArray<AVAudioMixInputParameters *> *parameters = [NSMutableArray array];
    for (AVAssetTrack *track in audioTracks) {
        NSMutableArray<AVAudioMixInputParameters *> *bucket = grouped[@(track.trackID)];
        if (bucket.count > 1) {
            if (detailOut) *detailOut = @"audioMix 存在映射到同一轨道的多个 inputParameters";
            return nil;
        }
        if (bucket.count == 0 && !(scale && pitchAlgorithm.length > 0)) continue;
        AVMutableAudioMixInputParameters *parameter = [AVMutableAudioMixInputParameters audioMixInputParameters];
        parameter.trackID = track.trackID;
        AVAudioMixInputParameters *sourceParameter = bucket.firstObject;
        if (sourceParameter) {
            CMTime cursor = kCMTimeZero;
            NSUInteger probes = 0;
            while (CMTimeCompare(cursor, end) < 0) {
                if (probes++ >= PV2_EXPORT_MAX_RAMP_PROBES) {
                    if (detailOut) *detailOut = @"volume ramp 探测量超出预算";
                    return nil;
                }
                float startVolume = 1.0f, endVolume = 1.0f;
                CMTimeRange range = kCMTimeRangeZero;
                const BOOL found = [sourceParameter getVolumeRampForTime:cursor startVolume:&startVolume
                                                                endVolume:&endVolume timeRange:&range];
                if (found && PV2ExportCMTimeSeconds(range.duration) > 0.0) {
                    [parameter setVolumeRampFromStartVolume:startVolume toEndVolume:endVolume
                                                  timeRange:(scale ? PV2ExportCMTimeRangeScaled(range, rate) : range)];
                    cursor = CMTimeAdd(range.start, range.duration);
                } else if (found) {
                    [parameter setVolume:startVolume
                                  atTime:(scale ? PV2ExportCMTimeScaled(range.start, rate) : range.start)];
                    cursor = CMTimeAdd(cursor, probe);
                } else {
                    cursor = CMTimeAdd(cursor, probe);
                }
            }
            if (pitchAlgorithm.length > 0) {
                PV2ExportSetPitchAlgorithm(parameter, pitchAlgorithm);
            } else {
                NSString *inherited = PV2ExportPitchAlgorithmOf(sourceParameter);
                if (inherited.length > 0) PV2ExportSetPitchAlgorithm(parameter, inherited);
            }
        } else if (pitchAlgorithm.length > 0) {
            PV2ExportSetPitchAlgorithm(parameter, pitchAlgorithm);
        }
        [parameters addObject:parameter];
    }
    if (parameters.count == 0) return nil;
    AVMutableAudioMix *mix = [AVMutableAudioMix audioMix];
    mix.inputParameters = parameters;
    return mix;
}

#endif // __has_include(<AVFoundation/AVFoundation.h>)
#endif // __OBJC__ && __has_include
