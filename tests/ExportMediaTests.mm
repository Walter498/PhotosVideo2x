// tests/ExportMediaTests.mm
//
// Build (macOS, CI):
//   xcrun clang++ -fobjc-arc -fblocks -Wall -Wextra -Werror -framework Foundation -framework AVFoundation \
//     tests/ExportMediaTests.mm -o /tmp/pv2-export-tests && /tmp/pv2-export-tests
//
// What it covers:
//   1. the shared time-scaling core (ExportComposition.h part 1) - mode/rate/duration policy,
//      segment -> action plan (contiguous runs, gaps, scaling), presentation decisions;
//   2. the AVFoundation bridge (part 2) against synthetic media generated at runtime:
//        source.mov : 2s, 320x240, 30fps, H.264, written with a 90 degree track transform
//        source.wav : 2s, mono 44.1kHz PCM sine
//        mixed      : AVMutableComposition holding a 2-segment video track (one slow-motion segment
//                     plus an explicit gap) and an audio track - the stand-in for a Photos slow-motion
//                     AVComposition with existing edits;
//   3. real AVAssetExportSession runs: audio-only m4a at 2x and 1x (duration must scale), and a
//      video+audio mp4/mov export whose track transform must survive (vertical video stays vertical);
//   4. videoComposition / audioMix remapping: instruction ranges, frame cadence and layer animation
//      ramps must be scaled, and combinations that cannot be re-timed must be *refused*, not ignored.
//
// Synthetic media only: nothing here reads or writes the user's Photos library or any media location.
// Set PV2_EXPORT_TEST_REQUIRE_VIDEO=1 to turn the (environment dependent) video encoder step into a
// hard requirement.

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import "../ExportComposition.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>

static int PV2TestFailures = 0;

static void PV2TestFail(const char *file, int line, NSString *message) {
    fprintf(stderr, "FAIL %s:%d %s\n", file, line, message.UTF8String ?: "?");
    PV2TestFailures++;
}

#define PV2CHECK(condition, ...) \
    do { if (!(condition)) PV2TestFail(__FILE__, __LINE__, [NSString stringWithFormat:__VA_ARGS__]); } while (0)

static void PV2TestSkip(NSString *message) {
    fprintf(stdout, "skip: %s\n", message.UTF8String ?: "?");
}

static BOOL PV2TestApproximately(double a, double b, double tolerance) {
    if (!isfinite(a) || !isfinite(b)) return NO;
    return fabs(a - b) <= tolerance;
}

static BOOL PV2TestTransformEquals(CGAffineTransform a, CGAffineTransform b) {
    return PV2TestApproximately(a.a, b.a, 1e-3) && PV2TestApproximately(a.b, b.b, 1e-3) &&
           PV2TestApproximately(a.c, b.c, 1e-3) && PV2TestApproximately(a.d, b.d, 1e-3) &&
           PV2TestApproximately(a.tx, b.tx, 1e-3) && PV2TestApproximately(a.ty, b.ty, 1e-3);
}

static BOOL PV2TestRequireVideo(void) {
    NSString *value = [[[NSProcessInfo processInfo] environment] objectForKey:@"PV2_EXPORT_TEST_REQUIRE_VIDEO"];
    return value.length > 0 && ![value isEqualToString:@"0"];
}

#pragma mark - Synthetic media

// Canonical 16-bit mono PCM WAV. AVFoundation reads it as an ordinary audio asset.
static BOOL PV2TestWriteWAV(NSURL *url, double seconds, double frequency, double sampleRate) {
    const unsigned int frames = (unsigned int)lround(seconds * sampleRate);
    const unsigned int dataBytes = frames * 2;
    FILE *file = fopen(url.fileSystemRepresentation, "wb");
    if (!file) return NO;
    unsigned char header[44];
    const unsigned int riffSize = 36 + dataBytes;
    memcpy(header, "RIFF", 4);
    header[4] = (unsigned char)(riffSize & 0xFF);
    header[5] = (unsigned char)((riffSize >> 8) & 0xFF);
    header[6] = (unsigned char)((riffSize >> 16) & 0xFF);
    header[7] = (unsigned char)((riffSize >> 24) & 0xFF);
    memcpy(header + 8, "WAVEfmt ", 8);
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;   // fmt chunk size
    header[20] = 1; header[21] = 0;                                    // PCM
    header[22] = 1; header[23] = 0;                                    // mono
    const unsigned int sr = (unsigned int)lround(sampleRate);
    header[24] = (unsigned char)(sr & 0xFF);
    header[25] = (unsigned char)((sr >> 8) & 0xFF);
    header[26] = (unsigned char)((sr >> 16) & 0xFF);
    header[27] = (unsigned char)((sr >> 24) & 0xFF);
    const unsigned int byteRate = sr * 2;
    header[28] = (unsigned char)(byteRate & 0xFF);
    header[29] = (unsigned char)((byteRate >> 8) & 0xFF);
    header[30] = (unsigned char)((byteRate >> 16) & 0xFF);
    header[31] = (unsigned char)((byteRate >> 24) & 0xFF);
    header[32] = 2; header[33] = 0;                                    // block align
    header[34] = 16; header[35] = 0;                                   // bits per sample
    memcpy(header + 36, "data", 4);
    header[40] = (unsigned char)(dataBytes & 0xFF);
    header[41] = (unsigned char)((dataBytes >> 8) & 0xFF);
    header[42] = (unsigned char)((dataBytes >> 16) & 0xFF);
    header[43] = (unsigned char)((dataBytes >> 24) & 0xFF);
    BOOL written = fwrite(header, 1, sizeof(header), file) == sizeof(header);
    for (unsigned int i = 0; written && i < frames; i++) {
        const double sample = sin(2.0 * M_PI * frequency * (double)i / sampleRate) * 0.3;
        short value = (short)lround(sample * 32767.0);
        unsigned char pair[2];
        pair[0] = (unsigned char)(value & 0xFF);
        pair[1] = (unsigned char)((value >> 8) & 0xFF);
        if (fwrite(pair, 1, 2, file) != 2) written = NO;
    }
    fclose(file);
    return written;
}

// Small H.264 movie with an explicit 90 degree track transform (portrait source material).
static BOOL PV2TestWriteMovie(NSURL *url, double seconds, int width, int height, double fps,
                              CGAffineTransform transform) {
    NSError *error = nil;
    AVAssetWriter *writer = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeQuickTimeMovie error:&error];
    if (!writer) return NO;
    AVAssetWriterInput *input = [[AVAssetWriterInput alloc]
        initWithMediaType:AVMediaTypeVideo
           outputSettings:@{ AVVideoCodecKey: AVVideoCodecTypeH264,
                             AVVideoWidthKey: @(width),
                             AVVideoHeightKey: @(height) }];
    input.expectsMediaDataInRealTime = NO;
    input.transform = transform;
    AVAssetWriterInputPixelBufferAdaptor *adaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc]
        initWithAssetWriterInput:input
     sourcePixelBufferAttributes:@{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                    (id)kCVPixelBufferWidthKey: @(width),
                                    (id)kCVPixelBufferHeightKey: @(height) }];
    if (![writer canAddInput:input]) return NO;
    [writer addInput:input];
    if (![writer startWriting]) return NO;
    [writer startSessionAtSourceTime:kCMTimeZero];

    const int frameCount = (int)lround(seconds * fps);
    BOOL ok = YES;
    for (int frame = 0; ok && frame < frameCount; frame++) {
        while (!input.isReadyForMoreMediaData) usleep(2000);
        CVPixelBufferRef buffer = NULL;
        if (CVPixelBufferPoolCreatePixelBuffer(NULL, adaptor.pixelBufferPool, &buffer) != kCVReturnSuccess ||
            buffer == NULL) { ok = NO; break; }
        CVPixelBufferLockBaseAddress(buffer, 0);
        unsigned char *base = (unsigned char *)CVPixelBufferGetBaseAddress(buffer);
        const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
        for (int y = 0; y < height; y++) {
            unsigned char *row = base + (size_t)y * stride;
            for (int x = 0; x < width; x++) {
                const unsigned char value = (unsigned char)(25 + ((x + y + frame * 4) % 200));
                row[x * 4 + 0] = value;
                row[x * 4 + 1] = (unsigned char)(255 - value);
                row[x * 4 + 2] = (unsigned char)((value * 2) & 0xFF);
                row[x * 4 + 3] = 255;
            }
        }
        CVPixelBufferUnlockBaseAddress(buffer, 0);
        const CMTime time = CMTimeMakeWithSeconds((double)frame / fps, 600);
        if (![adaptor appendPixelBuffer:buffer withPresentationTime:time]) ok = NO;
        CVPixelBufferRelease(buffer);
    }
    [input markAsFinished];
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    [writer finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(semaphore); }];
    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(120 * NSEC_PER_SEC)));
    return ok && writer.status == AVAssetWriterStatusCompleted;
}

static AVAssetTrack *PV2TestFirstTrack(AVAsset *asset, AVMediaType type) {
    NSArray<AVAssetTrack *> *tracks = [asset tracksWithMediaType:type];
    return tracks.count > 0 ? tracks.firstObject : nil;
}

// Synchronous wrapper around one AVAssetExportSession run (test only).
static NSError *PV2TestRunExport(AVAsset *asset, NSString *preset, AVFileType fileType, NSURL *outputURL,
                                 AVVideoComposition *videoComposition, AVAudioMix *audioMix) {
    AVAssetExportSession *session = [[AVAssetExportSession alloc] initWithAsset:asset presetName:preset];
    if (!session) return [NSError errorWithDomain:@"pv2.test" code:1 userInfo:@{ NSLocalizedDescriptionKey: @"preset unsupported" }];
    if (![session.supportedFileTypes containsObject:fileType]) {
        return [NSError errorWithDomain:@"pv2.test" code:2 userInfo:@{ NSLocalizedDescriptionKey: @"file type unsupported" }];
    }
    session.outputURL = outputURL;
    session.outputFileType = fileType;
    session.shouldOptimizeForNetworkUse = NO;
    if (videoComposition) session.videoComposition = videoComposition;
    if (audioMix) session.audioMix = audioMix;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    [session exportAsynchronouslyWithCompletionHandler:^{ dispatch_semaphore_signal(semaphore); }];
    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(300 * NSEC_PER_SEC)));
    if (session.status != AVAssetExportSessionStatusCompleted) {
        return session.error ?: [NSError errorWithDomain:@"pv2.test" code:(NSInteger)session.status
                                                userInfo:@{ NSLocalizedDescriptionKey:
                                                                [NSString stringWithFormat:@"export status %ld", (long)session.status] }];
    }
    return nil;
}

static double PV2TestFileDuration(NSURL *url) {
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
    return PV2ExportCMTimeSeconds(PV2ExportAssetDuration(asset));
}

// Custom instruction stand-in: the protocol only, no AVVideoCompositionInstruction subclass. A real
// custom compositor looks like this and must never be re-timed silently.
@interface PV2TestCustomInstruction : NSObject <AVVideoCompositionInstruction>
@property (nonatomic) CMTimeRange timeRange;
@property (nonatomic) BOOL enablePostProcessing;
@property (nonatomic,readonly) BOOL containsTweening;
@property (nonatomic,readonly) NSArray<NSValue *> *requiredSourceTrackIDs;
@property (nonatomic,readonly) CMPersistentTrackID passthroughTrackID;
@end

@implementation PV2TestCustomInstruction
- (BOOL)containsTweening { return NO; }
- (NSArray<NSValue *> *)requiredSourceTrackIDs { return @[]; }
- (CMPersistentTrackID)passthroughTrackID { return kCMPersistentTrackID_Invalid; }
- (instancetype)init {
    if ((self = [super init])) {
        _timeRange = CMTimeRangeMake(kCMTimeZero, CMTimeMake(3, 2));
        _enablePostProcessing = NO;
    }
    return self;
}
@end

static unsigned long long PV2TestFileSize(NSURL *url) {
    NSDictionary *attributes = [[NSFileManager defaultManager] attributesOfItemAtPath:url.path error:NULL];
    return [attributes fileSize];
}

int main(void) {
    @autoreleasepool {
#pragma mark 1. time-scaling core policy
        PV2ExportSpec spec;
        PV2CHECK(PV2ExportResolveSpec(0, 2.0, &spec) && spec.includeVideo && spec.includeAudio &&
                     PV2TestApproximately(spec.rate, 2.0, 1e-9),
                 @"mode 0 keeps video+audio at the current rate");
        PV2CHECK(PV2ExportResolveSpec(1, 2.0, &spec) && !spec.includeVideo && spec.includeAudio &&
                     PV2TestApproximately(spec.rate, 1.0, 1e-9),
                 @"mode 1 forces the normal rate and audio only");
        PV2CHECK(PV2ExportResolveSpec(2, 1.5, &spec) && !spec.includeVideo && spec.includeAudio &&
                     PV2TestApproximately(spec.rate, 1.5, 1e-9),
                 @"mode 2 keeps the current rate for audio only");
        PV2CHECK(!PV2ExportResolveSpec(7, 2.0, &spec), @"unknown mode is rejected");
        PV2CHECK(!PV2ExportResolveSpec(0, 0.0, &spec) && !PV2ExportResolveSpec(0, NAN, &spec) &&
                     !PV2ExportResolveSpec(0, INFINITY, &spec) && !PV2ExportResolveSpec(0, 500.0, &spec),
                 @"illegal rates are rejected");
        PV2CHECK(PV2ExportRateIsValid(PV2_EXPORT_MIN_RATE) && PV2ExportRateIsValid(PV2_EXPORT_MAX_RATE) &&
                     !PV2ExportRateIsValid(PV2_EXPORT_MIN_RATE / 2.0) && !PV2ExportRateIsValid(-1.0),
                 @"rate bounds");
        PV2CHECK(!PV2ExportDurationIsValid(0.0) && !PV2ExportDurationIsValid(-2.0) &&
                     !PV2ExportDurationIsValid(NAN) && !PV2ExportDurationIsValid(1e12) &&
                     PV2ExportDurationIsValid(12.5) && PV2ExportDurationIsValid(1.0 / 240.0),
                 @"duration guards (zero/negative/non-finite/absurd rejected)");
        PV2CHECK(!PV2ExportRequestIsValid(NO, YES, 0, 2.0) && !PV2ExportRequestIsValid(YES, NO, 0, 2.0) &&
                     PV2ExportRequestIsValid(YES, YES, 2, 2.0) && !PV2ExportRequestIsValid(YES, YES, 9, 2.0),
                 @"request validation");

        PV2ExportSpec videoSpec;
        PV2ExportResolveSpec(0, 2.0, &videoSpec);
        PV2CHECK(PV2ExportValidateTracks(&videoSpec, NO, YES, false) == PV2ExportReasonNoVideoTrack,
                 @"mode 0 without a video track is refused");
        PV2CHECK(PV2ExportValidateTracks(&videoSpec, YES, NO, false) == PV2ExportReasonNoAudioTrack,
                 @"mode 0 without an audio track is refused by default");
        PV2CHECK(PV2ExportValidateTracks(&videoSpec, YES, NO, true) == PV2ExportReasonNone,
                 @"silent video only when explicitly allowed");
        PV2ExportSpec audioSpec;
        PV2ExportResolveSpec(2, 2.0, &audioSpec);
        PV2CHECK(PV2ExportValidateTracks(&audioSpec, NO, NO, false) == PV2ExportReasonNoAudioTrack,
                 @"audio mode without an audio track is refused");
        PV2CHECK(PV2ExportValidateTracks(&audioSpec, YES, YES, false) == PV2ExportReasonNone,
                 @"audio mode accepts a resource that has audio");

#pragma mark 2. segment -> action plan
        PV2ExportSegment segments[4];
        segments[0].isEmpty = false; segments[0].targetStart = 0.00; segments[0].targetDuration = 0.50;
        segments[1].isEmpty = true;  segments[1].targetStart = 0.50; segments[1].targetDuration = 0.25;
        segments[2].isEmpty = false; segments[2].targetStart = 0.75; segments[2].targetDuration = 0.50;
        segments[3].isEmpty = false; segments[3].targetStart = 1.25; segments[3].targetDuration = 0.50;

        PV2ExportAction actions[8];
        size_t actionCount = 0;
        double planDuration = 0.0;
        double reported = PV2ExportBuildPlan(segments, 4, 2.0, true, actions, 8, &actionCount, &planDuration);
        PV2CHECK(actionCount == 3, @"plan merges contiguous media segments (got %zu)", actionCount);
        PV2CHECK(PV2TestApproximately(planDuration, 0.875, 1e-9) && PV2TestApproximately(reported, 0.875, 1e-9),
                 @"plan duration %.6f", planDuration);
        PV2CHECK(actions[0].kind == PV2ExportActionInsertMedia &&
                     PV2TestApproximately(actions[0].sourceStart, 0.0, 1e-9) &&
                     PV2TestApproximately(actions[0].outputStart, 0.0, 1e-9) &&
                     PV2TestApproximately(actions[0].outputDuration, 0.25, 1e-9),
                 @"first media action scaled by 1/rate");
        PV2CHECK(actions[1].kind == PV2ExportActionInsertEmpty &&
                     PV2TestApproximately(actions[1].outputStart, 0.25, 1e-9) &&
                     PV2TestApproximately(actions[1].outputDuration, 0.125, 1e-9),
                 @"gap preserved and scaled, never merged into media");
        PV2CHECK(actions[2].kind == PV2ExportActionInsertMedia &&
                     PV2TestApproximately(actions[2].sourceStart, 0.75, 1e-9) &&
                     PV2TestApproximately(actions[2].sourceDuration, 1.0, 1e-9) &&
                     PV2TestApproximately(actions[2].outputStart, 0.375, 1e-9) &&
                     PV2TestApproximately(actions[2].outputDuration, 0.5, 1e-9),
                 @"second run merged and placed after the scaled gap");
        PV2CHECK(PV2TestApproximately(PV2ExportPlanTotalDuration(actions, actionCount), 0.875, 1e-9),
                 @"plan total duration");

        size_t untouchedCount = 0;
        double untouchedTotal = 0.0;
        PV2ExportBuildPlan(segments, 4, 1.0, false, actions, 8, &untouchedCount, &untouchedTotal);
        PV2CHECK(untouchedCount == 3 && PV2TestApproximately(untouchedTotal, 1.75, 1e-9) &&
                     PV2TestApproximately(actions[0].outputDuration, 0.5, 1e-9) &&
                     PV2TestApproximately(actions[2].outputDuration, 1.0, 1e-9),
                 @"rate 1 / no scaling leaves the timeline untouched (total %.6f)", untouchedTotal);
        PV2CHECK(PV2ExportBuildPlan(segments, 4, 2.0, true, actions, 2, &untouchedCount, &untouchedTotal) <= 0.0,
                 @"plan reports capacity overflow instead of truncating");
        PV2CHECK(PV2ExportBuildPlan(segments, 4, 0.0, true, actions, 8, &untouchedCount, &untouchedTotal) <= 0.0,
                 @"scaling with an illegal rate fails the plan");
        PV2CHECK(PV2ExportBuildPlan(NULL, 0, 2.0, true, actions, 8, &untouchedCount, &untouchedTotal) <= 0.0,
                 @"empty segment list fails the plan");

#pragma mark 3. presentation policy
        PV2ExportPresentation decision = PV2ExportDecidePresentation(PV2ExportOutcomeSucceeded, true, true);
        PV2CHECK(decision.presentShare && !decision.keepOrphanFile && !decision.deleteFile && !decision.showError,
                 @"success + reachable owner shares immediately, deletes after sharing");
        decision = PV2ExportDecidePresentation(PV2ExportOutcomeSucceeded, true, false);
        PV2CHECK(!decision.presentShare && decision.keepOrphanFile && !decision.deleteFile,
                 @"success + hidden owner keeps the file and never forces the sheet");
        decision = PV2ExportDecidePresentation(PV2ExportOutcomeSucceeded, false, false);
        PV2CHECK(!decision.presentShare && decision.keepOrphanFile, @"success + dead owner keeps the file");
        decision = PV2ExportDecidePresentation(PV2ExportOutcomeFailed, true, true);
        PV2CHECK(decision.deleteFile && decision.showError && !decision.presentShare && !decision.keepOrphanFile,
                 @"failure deletes the output and reports when reachable");
        decision = PV2ExportDecidePresentation(PV2ExportOutcomeFailed, true, false);
        PV2CHECK(decision.deleteFile && !decision.showError, @"failure while hidden deletes without UI");
        decision = PV2ExportDecidePresentation(PV2ExportOutcomeCancelled, true, true);
        PV2CHECK(decision.deleteFile && !decision.showError && !decision.keepOrphanFile,
                 @"cancellation always deletes the output, never reports an error");

#pragma mark 4. small helpers
        PV2CHECK(PV2TestApproximately(PV2ExportScaledFrameDurationSeconds(1.0 / 30.0, 2.0), 1.0 / 60.0, 1e-9) &&
                     PV2TestApproximately(PV2ExportScaledFrameDurationSeconds(1.0 / 30.0, 1.0), 1.0 / 30.0, 1e-9) &&
                     PV2TestApproximately(PV2ExportScaledFrameDurationSeconds(0.0, 2.0), 0.0, 1e-9),
                 @"video frame cadence scales with the timeline");
        PV2CHECK(PV2TestApproximately(PV2ExportProbeStepSeconds(1.0 / 1000.0), PV2_EXPORT_PROBE_STEP_MIN_SECONDS, 1e-12) &&
                     PV2TestApproximately(PV2ExportProbeStepSeconds(0.0), 1.0 / 30.0, 1e-12) &&
                     PV2TestApproximately(PV2ExportProbeStepSeconds(1.0), PV2_EXPORT_PROBE_STEP_MAX_SECONDS, 1e-12),
                 @"ramp probe step clamp");
        PV2CHECK(PV2ExportProbeBudgetFits(60.0, 1.0 / 30.0) && !PV2ExportProbeBudgetFits(1e9, 1.0 / 240.0),
                 @"ramp probe budget guard");
        PV2CHECK(strcmp(PV2ExportFileExtension(0, true), "mp4") == 0 &&
                     strcmp(PV2ExportFileExtension(0, false), "mov") == 0 &&
                     strcmp(PV2ExportFileExtension(1, true), "m4a") == 0 &&
                     strcmp(PV2ExportFileExtension(2, false), "m4a") == 0,
                 @"container policy");
        char stem[64];
        PV2ExportFileStem(stem, sizeof(stem), 0, 2.0);
        PV2CHECK(strcmp(stem, "Video-2x") == 0, @"video stem %s", stem);
        PV2ExportFileStem(stem, sizeof(stem), 1, 3.0);
        PV2CHECK(strcmp(stem, "Audio-1x") == 0, @"mode 1 stem must not advertise a rate it does not use (%s)", stem);
        PV2ExportFileStem(stem, sizeof(stem), 2, 1.5);
        PV2CHECK(strcmp(stem, "Audio-1.5x") == 0, @"audio stem %s", stem);
        PV2CHECK(strcmp(PV2ExportReasonString(PV2ExportReasonNoAudioTrack), "no-audio-track") == 0 &&
                     strcmp(PV2ExportReasonString(PV2ExportReasonUnsupportedEdit), "unsupported-edit") == 0,
                 @"reason strings");

#pragma mark 5. synthetic media
        NSURL *root = [[NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES]
                          URLByAppendingPathComponent:[NSString stringWithFormat:@"pv2-export-tests-%d", (int)getpid()]
                                          isDirectory:YES];
        [[NSFileManager defaultManager] removeItemAtURL:root error:NULL];
        NSError *directoryError = nil;
        PV2CHECK([[NSFileManager defaultManager] createDirectoryAtURL:root withIntermediateDirectories:YES
                                              attributes:nil error:&directoryError],
                 @"temp directory: %@", directoryError.localizedDescription);

        NSURL *wavURL = [root URLByAppendingPathComponent:@"source.wav"];
        PV2CHECK(PV2TestWriteWAV(wavURL, 2.0, 440.0, 44100.0), @"synthetic wav written");
        AVURLAsset *audioAsset = [AVURLAsset URLAssetWithURL:wavURL options:nil];
        AVAssetTrack *audioSourceTrack = PV2TestFirstTrack(audioAsset, AVMediaTypeAudio);
        const CMPersistentTrackID audioSourceID = audioSourceTrack ? audioSourceTrack.trackID : 0;
        PV2CHECK(audioSourceTrack != nil, @"synthetic wav exposes an audio track");
        PV2CHECK(PV2TestApproximately(PV2ExportCMTimeSeconds(PV2ExportAssetDuration(audioAsset)), 2.0, 0.05),
                 @"synthetic wav duration %.4f", PV2ExportCMTimeSeconds(PV2ExportAssetDuration(audioAsset)));

        const CGAffineTransform rotation = CGAffineTransformMakeRotation((CGFloat)(M_PI / 2.0));
        NSURL *movieURL = [root URLByAppendingPathComponent:@"source.mov"];
        BOOL videoAvailable = PV2TestWriteMovie(movieURL, 2.0, 320, 240, 30.0, rotation);
        if (!videoAvailable) {
            if (PV2TestRequireVideo()) {
                PV2TestFail(__FILE__, __LINE__, @"H.264 writer unavailable and PV2_EXPORT_TEST_REQUIRE_VIDEO=1");
            } else {
                PV2TestSkip(@"H.264 writer unavailable on this host: video assertions are skipped");
            }
        }
        AVURLAsset *videoAsset = videoAvailable ? [AVURLAsset URLAssetWithURL:movieURL options:nil] : nil;
        AVAssetTrack *videoSourceTrack = videoAvailable ? PV2TestFirstTrack(videoAsset, AVMediaTypeVideo) : nil;
        PV2CHECK(!videoAvailable || videoSourceTrack != nil, @"synthetic movie exposes a video track");
        PV2CHECK(!videoAvailable ||
                     PV2TestApproximately(PV2ExportCMTimeSeconds(PV2ExportAssetDuration(videoAsset)), 2.0, 0.1),
                 @"synthetic movie duration");
        PV2CHECK(!videoAvailable ||
                     PV2TestTransformEquals(PV2ExportTrackPreferredTransform(videoSourceTrack), rotation),
                 @"synthetic movie carries the 90 degree track transform");

#pragma mark 6. audio-only builds (mode 1 / mode 2) and refusals
        PV2ExportCompositionResult *audioNormal = PV2ExportBuildScaledComposition(audioAsset, 1.0, NO, YES, NO);
        PV2CHECK(audioNormal.composition != nil && audioNormal.reason == PV2ExportReasonNone,
                 @"mode 1 composition failed: %@ (%s)", audioNormal.failureDetail,
                 PV2ExportReasonString(audioNormal.reason));
        PV2CHECK(audioNormal.composition != nil &&
                     PV2TestApproximately(audioNormal.outputDurationSeconds, 2.0, 0.05),
                 @"mode 1 keeps the normal duration (%.4f)", audioNormal.outputDurationSeconds);
        PV2CHECK(audioNormal.composition.tracks.count == 1, @"mode 1 composition holds audio only");

        PV2ExportCompositionResult *audioSpeed = PV2ExportBuildScaledComposition(audioAsset, 2.0, NO, YES, NO);
        PV2CHECK(audioSpeed.composition != nil && audioSpeed.reason == PV2ExportReasonNone,
                 @"mode 2 composition failed: %@ (%s)", audioSpeed.failureDetail,
                 PV2ExportReasonString(audioSpeed.reason));
        PV2CHECK(audioSpeed.composition != nil &&
                     PV2TestApproximately(audioSpeed.outputDurationSeconds, 1.0, 0.05),
                 @"mode 2 halves the duration (%.4f)", audioSpeed.outputDurationSeconds);
        PV2CHECK(PV2TestApproximately(audioSpeed.targetDurationSeconds, 1.0, 0.05),
                 @"mode 2 target duration %.4f", audioSpeed.targetDurationSeconds);

        PV2ExportCompositionResult *noVideo = PV2ExportBuildScaledComposition(audioAsset, 2.0, YES, YES, NO);
        PV2CHECK(noVideo.composition == nil && noVideo.reason == PV2ExportReasonNoVideoTrack,
                 @"mode 0 refuses an asset without video (%s)", PV2ExportReasonString(noVideo.reason));
        PV2CHECK(PV2ExportBuildScaledComposition(audioAsset, 0.0, NO, YES, NO).reason == PV2ExportReasonBadRate,
                 @"rate 0 refused");
        PV2CHECK(PV2ExportBuildScaledComposition(audioAsset, NAN, NO, YES, NO).reason == PV2ExportReasonBadRate,
                 @"NaN rate refused");
        PV2CHECK(PV2ExportBuildScaledComposition(audioAsset, 100.0, NO, YES, NO).reason == PV2ExportReasonBadRate,
                 @"absurd rate refused");
        PV2CHECK(PV2ExportBuildScaledComposition(nil, 2.0, YES, YES, NO).reason == PV2ExportReasonBadRequest,
                 @"nil asset refused");
        if (videoAvailable) {
            PV2ExportCompositionResult *noAudio =
                PV2ExportBuildScaledComposition(videoAsset, 2.0, YES, YES, NO);
            PV2CHECK(noAudio.composition == nil && noAudio.reason == PV2ExportReasonNoAudioTrack,
                     @"mode 0 refuses an asset without audio (%s)", PV2ExportReasonString(noAudio.reason));
            PV2CHECK(PV2ExportBuildScaledComposition(videoAsset, 2.0, YES, NO, NO).composition != nil,
                     @"video-only build succeeds when audio is not requested");
        }

#pragma mark 7. existing edit segments (slow-motion style source composition)
        NSString *(^describeTransform)(CGAffineTransform) = ^NSString *(CGAffineTransform transform) {
            return [NSString stringWithFormat:@"[%.3f %.3f %.3f %.3f %.3f %.3f]",
                    (double)transform.a, (double)transform.b, (double)transform.c,
                    (double)transform.d, (double)transform.tx, (double)transform.ty];
        };

        AVMutableComposition *mixed = nil;
        AVMutableCompositionTrack *mixedVideoTrack = nil;
        if (videoAvailable) {
            NSError *insertError = nil;
            mixed = [AVMutableComposition composition];
            mixedVideoTrack =
                [mixed addMutableTrackWithMediaType:AVMediaTypeVideo
                               preferredTrackID:kCMPersistentTrackID_Invalid];
            AVMutableCompositionTrack *mixedAudioTrack =
                [mixed addMutableTrackWithMediaType:AVMediaTypeAudio
                               preferredTrackID:kCMPersistentTrackID_Invalid];
            BOOL ok = [mixedVideoTrack insertTimeRange:CMTimeRangeMake(kCMTimeZero, CMTimeMake(1, 1))
                                               ofTrack:videoSourceTrack atTime:kCMTimeZero error:&insertError];
            if (ok) {
                // A slow-motion style edit: one second of source occupies half a second of timeline.
                [mixedVideoTrack scaleTimeRange:CMTimeRangeMake(kCMTimeZero, CMTimeMake(1, 1))
                                     toDuration:CMTimeMake(1, 2)];
            }
            if (ok) {
                [mixedVideoTrack insertEmptyTimeRange:CMTimeRangeMake(CMTimeMake(1, 2), CMTimeMake(1, 4))];
            }
            if (ok) {
                ok = [mixedVideoTrack insertTimeRange:CMTimeRangeMake(CMTimeMake(1, 1), CMTimeMake(1, 1))
                                              ofTrack:videoSourceTrack
                                               atTime:CMTimeMake(3, 4) error:&insertError];
            }
            if (ok) {
                ok = [mixedAudioTrack insertTimeRange:CMTimeRangeMake(kCMTimeZero, CMTimeMakeWithSeconds(1.75, 600))
                                              ofTrack:audioSourceTrack atTime:kCMTimeZero error:&insertError];
            }
            mixedVideoTrack.preferredTransform = rotation;
            PV2CHECK(ok, @"mixed source composition could not be built: %@", insertError.localizedDescription);
            PV2CHECK(PV2TestApproximately(PV2ExportCMTimeSeconds(mixed.duration), 1.75, 0.05),
                     @"mixed source duration %.4f (expected 1.75)", PV2ExportCMTimeSeconds(mixed.duration));
        }

        if (mixed) {
            PV2ExportCompositionResult *bundle = PV2ExportBuildScaledComposition(mixed, 2.0, YES, YES, NO);
            PV2CHECK(bundle.composition != nil, @"mixed mode 0 build failed: %@ (%s)", bundle.failureDetail,
                     PV2ExportReasonString(bundle.reason));
            PV2CHECK(bundle.composition != nil &&
                         PV2TestApproximately(bundle.outputDurationSeconds, 0.875, 0.05),
                     @"mixed output duration %.4f (expected 0.875)", bundle.outputDurationSeconds);
            AVAssetTrack *scaledVideo = nil;
            AVAssetTrack *scaledAudio = nil;
            for (AVAssetTrack *track in bundle.composition.tracks) {
                if ([track.mediaType isEqualToString:AVMediaTypeVideo]) scaledVideo = track;
                if ([track.mediaType isEqualToString:AVMediaTypeAudio]) scaledAudio = track;
            }
            PV2CHECK(scaledVideo != nil && scaledAudio != nil,
                     @"mixed composition keeps the video and audio tracks");
            PV2CHECK(scaledVideo != nil &&
                         PV2TestTransformEquals(PV2ExportTrackPreferredTransform(scaledVideo), rotation),
                     @"vertical transform copied onto the scaled video track (%@)",
                     scaledVideo ? describeTransform(PV2ExportTrackPreferredTransform(scaledVideo)) : @"nil");
            PV2CHECK(scaledAudio != nil &&
                         PV2TestApproximately(PV2ExportCMTimeSeconds(scaledAudio.timeRange.duration), 0.875, 0.05),
                     @"scaled audio track covers the scaled duration (%.4f)",
                     scaledAudio ? PV2ExportCMTimeSeconds(scaledAudio.timeRange.duration) : -1.0);
            if (scaledVideo) {
                NSArray *segments = [(AVCompositionTrack *)scaledVideo segments];
                PV2CHECK(segments.count == 3, @"source edit segments preserved (got %lu)",
                         (unsigned long)segments.count);
                BOOL sawEmpty = NO;
                double mediaSeconds = 0.0;
                for (AVCompositionTrackSegment *segment in segments) {
                    if (segment.isEmpty) {
                        sawEmpty = YES;
                    } else {
                        mediaSeconds += PV2ExportCMTimeSeconds(segment.timeMapping.target.duration);
                    }
                }
                PV2CHECK(sawEmpty, @"the gap between the two edit segments survived the scaling");
                PV2CHECK(PV2TestApproximately(mediaSeconds, 0.75, 0.05),
                         @"scaled media segments total %.4f (expected 0.75)", mediaSeconds);
            }

            PV2ExportCompositionResult *audioFromMixed = PV2ExportBuildScaledComposition(mixed, 2.0, NO, YES, NO);
            PV2CHECK(audioFromMixed.composition != nil && audioFromMixed.composition.tracks.count == 1 &&
                         PV2TestApproximately(audioFromMixed.outputDurationSeconds, 0.875, 0.05),
                     @"mode 2 on the mixed source: %@ (tracks %lu, %.4f)", audioFromMixed.failureDetail,
                     (unsigned long)audioFromMixed.composition.tracks.count, audioFromMixed.outputDurationSeconds);

            // Audio shorter than the resource: the audio-only output must not be truncated.
            NSError *padError = nil;
            AVMutableComposition *shortAudioSource = [AVMutableComposition composition];
            AVMutableCompositionTrack *shortVideo = [shortAudioSource addMutableTrackWithMediaType:AVMediaTypeVideo
                                                                            preferredTrackID:kCMPersistentTrackID_Invalid];
            AVMutableCompositionTrack *shortAudio = [shortAudioSource addMutableTrackWithMediaType:AVMediaTypeAudio
                                                                            preferredTrackID:kCMPersistentTrackID_Invalid];
            BOOL padOK = [shortVideo insertTimeRange:CMTimeRangeMake(kCMTimeZero, CMTimeMake(2, 1))
                                             ofTrack:videoSourceTrack atTime:kCMTimeZero error:&padError];
            padOK = padOK && [shortAudio insertTimeRange:CMTimeRangeMake(kCMTimeZero, CMTimeMake(1, 1))
                                                 ofTrack:audioSourceTrack atTime:kCMTimeZero error:&padError];
            PV2CHECK(padOK, @"short-audio source could not be built: %@", padError.localizedDescription);
            PV2ExportCompositionResult *padded = PV2ExportBuildScaledComposition(shortAudioSource, 2.0, NO, YES, NO);
            PV2CHECK(padded.composition != nil && PV2TestApproximately(padded.outputDurationSeconds, 1.0, 0.05),
                     @"audio-only output padded to the scaled resource duration (%.4f, %@)",
                     padded.outputDurationSeconds, padded.failureDetail);
        } else {
            PV2TestSkip(@"edit-segment / mixed-source assertions skipped (no encoder)");
        }

#pragma mark 8. videoComposition remapping
        if (mixed) {
            BOOL manualComposition = NO;
            AVMutableVideoComposition *sourceVideoComposition = [AVMutableVideoComposition videoComposition];
            if (sourceVideoComposition) {
                // SDK-neutral construction: prefer the documented factories, but verify the class of
                // what they return and fall back to a plain instance so a factory that returns an
                // immutable object can never be used by accident.
                AVMutableVideoCompositionInstruction *instruction = nil;
                if ([AVMutableVideoCompositionInstruction respondsToSelector:@selector(videoCompositionInstruction)]) {
                    id created = [AVMutableVideoCompositionInstruction videoCompositionInstruction];
                    if ([created isKindOfClass:[AVMutableVideoCompositionInstruction class]]) instruction = created;
                }
                if (!instruction) instruction = [[AVMutableVideoCompositionInstruction alloc] init];

                AVMutableVideoCompositionLayerInstruction *layer = nil;
                if ([AVMutableVideoCompositionLayerInstruction respondsToSelector:
                         @selector(videoCompositionLayerInstructionWithAssetTrack:)]) {
                    id created = [AVMutableVideoCompositionLayerInstruction
                        videoCompositionLayerInstructionWithAssetTrack:mixedVideoTrack];
                    if ([created isKindOfClass:[AVMutableVideoCompositionLayerInstruction class]]) layer = created;
                }
                if (!layer) {
                    layer = [[AVMutableVideoCompositionLayerInstruction alloc] init];
                    layer.trackID = mixedVideoTrack.trackID;
                }
                sourceVideoComposition.frameDuration = CMTimeMake(1, 30);
                sourceVideoComposition.renderSize = CGSizeMake(320.0, 240.0);
                [layer setTransformRampFromStartTransform:CGAffineTransformIdentity
                                            toEndTransform:rotation
                                                 timeRange:CMTimeRangeMake(kCMTimeZero, CMTimeMake(1, 1))];
                [layer setOpacityRampFromStartOpacity:0.0f toEndOpacity:1.0f
                                            timeRange:CMTimeRangeMake(CMTimeMake(1, 2), CMTimeMake(1, 4))];
                [layer setOpacity:1.0f atTime:CMTimeMakeWithSeconds(1.6, 600)];
                instruction.timeRange = CMTimeRangeMake(kCMTimeZero, mixed.duration);
                instruction.layerInstructions = @[ layer ];
                sourceVideoComposition.instructions = @[ instruction ];
                manualComposition = YES;
            } else {
                PV2TestSkip(@"AVMutableVideoComposition unavailable: manual instruction assertions skipped");
            }

            if (manualComposition) {
                NSString *detail = nil;
                AVMutableVideoComposition *scaled =
                    PV2ExportCompatibleVideoComposition(sourceVideoComposition, @{}, 2.0, &detail);
                PV2CHECK(scaled != nil && detail == nil, @"videoComposition remap at 2x: %@", detail);
                PV2CHECK(scaled != nil &&
                             PV2TestApproximately(PV2ExportCMTimeSeconds(scaled.frameDuration), 1.0 / 60.0, 1e-6),
                         @"frame cadence scaled to 1/60 (%.6f)",
                         scaled ? PV2ExportCMTimeSeconds(scaled.frameDuration) : -1.0);
                PV2CHECK(scaled.instructions.count == 1, @"instruction count preserved");
                if (scaled.instructions.count == 1) {
                    id instruction = scaled.instructions.firstObject;
                    CMTimeRange instructionRange = kCMTimeRangeZero;
                    NSArray *layers = nil;
                    if ([instruction isKindOfClass:[AVVideoCompositionInstruction class]]) {
                        AVVideoCompositionInstruction *typed = (AVVideoCompositionInstruction *)instruction;
                        instructionRange = typed.timeRange;
                        layers = typed.layerInstructions;
                    }
                    PV2CHECK(PV2TestApproximately(PV2ExportCMTimeSeconds(instructionRange.duration), 0.875, 1e-3),
                             @"instruction range scaled (%.4f)",
                             PV2ExportCMTimeSeconds(instructionRange.duration));
                    PV2CHECK(layers.count == 1, @"layer instructions preserved");
                    if (layers.count == 1) {
                        AVVideoCompositionLayerInstruction *layer = layers.firstObject;
                        PV2CHECK(layer.trackID == mixedVideoTrack.trackID,
                                 @"layer instruction still targets the video track (%d vs %d)",
                                 (int)layer.trackID, (int)mixedVideoTrack.trackID);
                        CGAffineTransform start = CGAffineTransformIdentity;
                        CGAffineTransform end = CGAffineTransformIdentity;
                        CMTimeRange transformRange = kCMTimeRangeZero;
                        const BOOL hasTransform =
                            [layer getTransformRampForTime:CMTimeMakeWithSeconds(0.25, 600)
                                            startTransform:&start endTransform:&end timeRange:&transformRange];
                        PV2CHECK(hasTransform &&
                                     PV2TestApproximately(PV2ExportCMTimeSeconds(transformRange.start), 0.0, 0.01) &&
                                     PV2TestApproximately(PV2ExportCMTimeSeconds(transformRange.duration), 0.5, 0.01) &&
                                     PV2TestTransformEquals(end, rotation),
                                 @"transform ramp scaled to [0, 0.5] (start %.4f dur %.4f)",
                                 PV2ExportCMTimeSeconds(transformRange.start),
                                 PV2ExportCMTimeSeconds(transformRange.duration));
                        float startOpacity = 0.0f, endOpacity = 0.0f;
                        CMTimeRange opacityRange = kCMTimeRangeZero;
                        const BOOL hasOpacity =
                            [layer getOpacityRampForTime:CMTimeMakeWithSeconds(0.30, 600)
                                             startOpacity:&startOpacity endOpacity:&endOpacity
                                               timeRange:&opacityRange];
                        PV2CHECK(hasOpacity &&
                                     PV2TestApproximately(PV2ExportCMTimeSeconds(opacityRange.start), 0.25, 0.01) &&
                                     PV2TestApproximately(PV2ExportCMTimeSeconds(opacityRange.duration), 0.125, 0.01),
                                 @"opacity ramp scaled to [0.25, 0.375] (start %.4f dur %.4f)",
                                 PV2ExportCMTimeSeconds(opacityRange.start),
                                 PV2ExportCMTimeSeconds(opacityRange.duration));
                        float stepStart = 0.0f, stepEnd = 0.0f;
                        CMTimeRange stepRange = kCMTimeRangeZero;
                        const BOOL hasStep =
                            [layer getOpacityRampForTime:CMTimeMakeWithSeconds(0.8, 600)
                                             startOpacity:&stepStart endOpacity:&stepEnd timeRange:&stepRange];
                        PV2CHECK(!hasStep ||
                                     PV2TestApproximately(PV2ExportCMTimeSeconds(stepRange.start), 0.8, 0.02),
                                 @"zero-length opacity step republished at the scaled time (%.4f)",
                                 PV2ExportCMTimeSeconds(stepRange.start));
                    }
                }

                // Rate 1 must not touch any timing at all.
                NSString *unchangedDetail = nil;
                AVMutableVideoComposition *unchanged =
                    PV2ExportCompatibleVideoComposition(sourceVideoComposition, @{}, 1.0, &unchangedDetail);
                PV2CHECK(unchanged != nil && unchangedDetail == nil, @"videoComposition clone at 1x: %@", unchangedDetail);
                if (unchanged && unchanged.instructions.count == 1 &&
                    [unchanged.instructions.firstObject isKindOfClass:[AVVideoCompositionInstruction class]]) {
                    AVVideoCompositionInstruction *typed = (AVVideoCompositionInstruction *)unchanged.instructions.firstObject;
                    PV2CHECK(PV2TestApproximately(PV2ExportCMTimeSeconds(typed.timeRange.duration), 1.75, 1e-3) &&
                                 PV2TestApproximately(PV2ExportCMTimeSeconds(unchanged.frameDuration), 1.0 / 30.0, 1e-6),
                             @"rate 1 keeps instruction ranges and frame cadence untouched");
                    AVVideoCompositionLayerInstruction *layer = typed.layerInstructions.firstObject;
                    CMTimeRange transformRange = kCMTimeRangeZero;
                    CGAffineTransform start = CGAffineTransformIdentity;
                    CGAffineTransform end = CGAffineTransformIdentity;
                    const BOOL hasTransform = layer &&
                        [layer getTransformRampForTime:CMTimeMakeWithSeconds(0.9, 600) startTransform:&start
                                        endTransform:&end timeRange:&transformRange];
                    PV2CHECK(hasTransform &&
                                 PV2TestApproximately(PV2ExportCMTimeSeconds(transformRange.duration), 1.0, 1e-3),
                             @"rate 1 keeps ramp ranges untouched (dur %.4f)",
                             PV2ExportCMTimeSeconds(transformRange.duration));
                }
            }

            // A custom instruction (own compositor) cannot be re-timed safely: must be refused.
            AVMutableVideoComposition *customCarrier = [AVMutableVideoComposition videoComposition];
            customCarrier.frameDuration = CMTimeMake(1, 30);
            customCarrier.renderSize = CGSizeMake(320.0, 240.0);
            PV2TestCustomInstruction *customInstruction = [PV2TestCustomInstruction new];
            customCarrier.instructions = @[ customInstruction ];
            NSString *customDetail = nil;
            AVMutableVideoComposition *customResult =
                PV2ExportCompatibleVideoComposition(customCarrier, @{}, 2.0, &customDetail);
            PV2CHECK(customResult == nil && customDetail != nil,
                     @"custom instruction refused instead of silently dropped (%@)", customDetail ?: @"nil");

            // Same shape Photos itself produces for slow-motion material.
            AVMutableVideoComposition *propertiesComposition =
                [AVMutableVideoComposition videoCompositionWithPropertiesOfAsset:mixed];
            if (propertiesComposition) {
                NSString *propertiesDetail = nil;
                AVMutableVideoComposition *rescaled =
                    PV2ExportCompatibleVideoComposition(propertiesComposition, @{}, 2.0, &propertiesDetail);
                PV2CHECK(rescaled != nil && propertiesDetail == nil,
                         @"videoCompositionWithPropertiesOfAsset: remap: %@", propertiesDetail);
                if (rescaled && rescaled.instructions.count > 0) {
                    CMTimeRange range = kCMTimeRangeZero;
                    id first = rescaled.instructions.firstObject;
                    if ([first isKindOfClass:[AVVideoCompositionInstruction class]]) {
                        range = ((AVVideoCompositionInstruction *)first).timeRange;
                    }
                    double covered=0;
                    for (AVVideoCompositionInstruction *i in rescaled.instructions) covered=MAX(covered,PV2ExportCMTimeSeconds(CMTimeRangeGetEnd(i.timeRange)));
                    PV2CHECK(PV2TestApproximately(covered,0.875,0.01),
                             @"Photos-style instructions cover scaled duration (%.4f)",covered);
                    (void)range;
                }
            } else {
                PV2TestSkip(@"videoCompositionWithPropertiesOfAsset: returned nil on this host");
            }
        }

#pragma mark 9. audioMix remapping
        if (audioSpeed.composition && audioNormal.composition) {
            AVAssetTrack *scaledAudioTrack = [audioSpeed.composition tracksWithMediaType:AVMediaTypeAudio].firstObject;
            PV2CHECK(scaledAudioTrack != nil, @"scaled audio composition exposes an audio track");
            AVMutableAudioMix *sourceMix = [AVMutableAudioMix audioMix];
            AVMutableAudioMixInputParameters *sourceParameters = [AVMutableAudioMixInputParameters audioMixInputParameters];
            sourceParameters.trackID = audioSourceID;
            [sourceParameters setVolumeRampFromStartVolume:0.2f toEndVolume:1.0f
                                                 timeRange:CMTimeRangeMake(CMTimeMake(1, 4), CMTimeMake(1, 2))];
            sourceMix.inputParameters = @[ sourceParameters ];
            NSString *mixDetail = nil;
            AVAudioMix *scaledMix = PV2ExportCompatibleAudioMix(audioSpeed.composition, sourceMix,
                                                                @{ @(audioSourceID): @(scaledAudioTrack.trackID) }, 2.0,
                                                                @"AVAudioTimePitchAlgorithmTimeDomain",
                                                                1.0, &mixDetail);
            PV2CHECK(scaledMix != nil && mixDetail == nil, @"audioMix remap: %@", mixDetail);
            if (scaledMix) {
                PV2CHECK(scaledMix.inputParameters.count == 1, @"one remapped audio parameter");
                AVAudioMixInputParameters *parameters = scaledMix.inputParameters.firstObject;
                PV2CHECK(parameters.trackID == scaledAudioTrack.trackID, @"audio parameter follows the track-ID map (%d)",
                         (int)parameters.trackID);
                PV2CHECK([PV2ExportPitchAlgorithmOf(parameters) isEqualToString:@"AVAudioTimePitchAlgorithmTimeDomain"],
                         @"pitch policy handed to the exporter (%@)", PV2ExportPitchAlgorithmOf(parameters));
                float startVolume = 0.0f, endVolume = 0.0f;
                CMTimeRange volumeRange = kCMTimeRangeZero;
                const BOOL hasRamp = [parameters getVolumeRampForTime:CMTimeMakeWithSeconds(0.15, 600)
                                                          startVolume:&startVolume endVolume:&endVolume
                                                            timeRange:&volumeRange];
                PV2CHECK(hasRamp &&
                             PV2TestApproximately(PV2ExportCMTimeSeconds(volumeRange.start), 0.125, 0.01) &&
                             PV2TestApproximately(PV2ExportCMTimeSeconds(volumeRange.duration), 0.25, 0.01) &&
                             PV2TestApproximately((double)startVolume, 0.2, 0.01) &&
                             PV2TestApproximately((double)endVolume, 1.0, 0.01),
                         @"volume ramp scaled by 1/rate (start %.4f dur %.4f)",
                         PV2ExportCMTimeSeconds(volumeRange.start),
                         PV2ExportCMTimeSeconds(volumeRange.duration));
            }
            AVMutableAudioMix *ambiguous = [AVMutableAudioMix audioMix];
            AVMutableAudioMixInputParameters *first = [AVMutableAudioMixInputParameters audioMixInputParameters];
            first.trackID = audioSourceID;
            AVMutableAudioMixInputParameters *second = [AVMutableAudioMixInputParameters audioMixInputParameters];
            second.trackID = audioSourceID;
            ambiguous.inputParameters = @[ first, second ];
            NSString *ambiguousDetail = nil;
            AVAudioMix *ambiguousResult = PV2ExportCompatibleAudioMix(audioSpeed.composition, ambiguous, @{},
                                                                     2.0, nil, 1.0, &ambiguousDetail);
            PV2CHECK(ambiguousResult == nil && ambiguousDetail != nil,
                     @"ambiguous source audioMix refused (%@)", ambiguousDetail ?: @"nil");
            PV2CHECK(PV2ExportCompatibleAudioMix(audioNormal.composition, nil, @{}, 1.0, nil, 2.0, NULL) == nil,
                     @"rate 1 with no source mix and no pitch policy needs no audioMix");
        }

#pragma mark 10. real exports
        NSURL *audioTwoX = [root URLByAppendingPathComponent:@"Audio-2x.m4a"];
        NSError *twoXError = PV2TestRunExport(audioSpeed.composition, AVAssetExportPresetAppleM4A,
                                              AVFileTypeAppleM4A, audioTwoX, nil, nil);
        PV2CHECK(twoXError == nil, @"audio 2x export: %@", twoXError.localizedDescription);
        PV2CHECK(PV2TestFileSize(audioTwoX) > 0, @"audio 2x output is not empty");
        PV2CHECK(PV2TestApproximately(PV2TestFileDuration(audioTwoX), 1.0, 0.1),
                 @"audio 2x output duration %.4f (expected 1.0)", PV2TestFileDuration(audioTwoX));

        NSURL *audioOneX = [root URLByAppendingPathComponent:@"Audio-1x.m4a"];
        NSError *oneXError = PV2TestRunExport(audioNormal.composition, AVAssetExportPresetAppleM4A,
                                              AVFileTypeAppleM4A, audioOneX, nil, nil);
        PV2CHECK(oneXError == nil, @"audio 1x export: %@", oneXError.localizedDescription);
        PV2CHECK(PV2TestApproximately(PV2TestFileDuration(audioOneX), 2.0, 0.15),
                 @"audio 1x output duration %.4f (expected 2.0)", PV2TestFileDuration(audioOneX));

        if (mixed) {
            PV2ExportCompositionResult *bundle = PV2ExportBuildScaledComposition(mixed, 2.0, YES, YES, NO);
            PV2CHECK(bundle.composition != nil, @"video export composition: %@", bundle.failureDetail);
            if (bundle.composition) {
                AVAssetExportSession *probe = [[AVAssetExportSession alloc]
                    initWithAsset:bundle.composition presetName:AVAssetExportPresetHighestQuality];
                AVFileType container = (probe &&
                                        [probe.supportedFileTypes containsObject:AVFileTypeMPEG4])
                                           ? AVFileTypeMPEG4 : AVFileTypeQuickTimeMovie;
                NSString *name = [container isEqualToString:AVFileTypeMPEG4] ? @"Video-2x.mp4" : @"Video-2x.mov";
                NSURL *videoOut = [root URLByAppendingPathComponent:name];
                NSError *videoError = PV2TestRunExport(bundle.composition, AVAssetExportPresetHighestQuality,
                                                       container, videoOut, PV2ExportDefaultVideoComposition(bundle.composition), nil);
                PV2CHECK(videoError == nil, @"video 2x export: %@", videoError.localizedDescription);
                PV2CHECK(PV2TestFileSize(videoOut) > 0, @"video 2x output is not empty");
                PV2CHECK(PV2TestApproximately(PV2TestFileDuration(videoOut), 0.875, 0.15),
                         @"video 2x output duration %.4f (expected 0.875)", PV2TestFileDuration(videoOut));
                AVURLAsset *outputAsset = [AVURLAsset URLAssetWithURL:videoOut options:nil];
                AVAssetTrack *outputVideoTrack = PV2TestFirstTrack(outputAsset, AVMediaTypeVideo);
                PV2CHECK(outputVideoTrack != nil, @"video output has a video track");
                CGRect visible=outputVideoTrack ? CGRectApplyAffineTransform(CGRectMake(0,0,outputVideoTrack.naturalSize.width,outputVideoTrack.naturalSize.height),outputVideoTrack.preferredTransform) : CGRectZero;
                PV2CHECK(outputVideoTrack != nil && fabs(visible.size.height)>fabs(visible.size.width),
                         @"vertical video survives export with portrait presentation (%@)",NSStringFromCGRect(visible));
                PV2CHECK(PV2TestFirstTrack(outputAsset, AVMediaTypeAudio) != nil,
                         @"video output kept its audio track");
            }
        }

#pragma mark 11. summary
        [[NSFileManager defaultManager] removeItemAtURL:root error:NULL];
        if (PV2TestFailures == 0) {
            fprintf(stdout, "PASS: export media tests (%s)\n",
                    videoAvailable ? "synthetic video + audio" : "audio only, encoder unavailable");
        } else {
            fprintf(stderr, "FAILED: %d check(s)\n", PV2TestFailures);
        }
    }
    return PV2TestFailures == 0 ? 0 : 1;
}
