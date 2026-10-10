#pragma once
// VideoExport.h - Photos plugin: share the current video/audio resource with three export options.
//
//   mode 0: video + audio at the *current* rate            -> .mp4 (falls back to .mov)
//   mode 1: audio only, forced to normal rate (rate == 1)  -> .m4a
//   mode 2: audio only at the *current* rate               -> .m4a
//
// Interface used by the tweak (Tweak.xm), see PV2ExportMedia at the bottom of this file.
// Include this header exactly once, from ARC ObjC++ code (the tweak target), with -std=c++17.
//
// Design rules honoured here:
//   * never rebuild a bare asset from a URL: the item's own asset (possibly a slow-motion
//     AVComposition) plus its videoComposition/audioMix are what gets exported, so the user's
//     existing edits are respected;
//   * asset keys are loaded asynchronously, the composition is built on a private serial queue, so
//     neither the main thread nor the Photos player is ever blocked or mutated;
//   * one export per owner view controller at a time; cancel button + main-queue progress timer;
//   * the output lands in a dedicated temp directory (UUID) and is removed after sharing, on
//     failure and on cancellation;
//   * the item, its speed and its editing objects are captured when the export starts, so switching
//     pages mid-export can never turn an old resource into the newly selected one;
//   * if the owner is gone/unreachable when the export finishes the share sheet is *not* forced:
//     the file is kept briefly (orphan TTL) instead.
//
// The export re-encodes (AVAssetExportPresetHighestQuality / AVAssetExportPresetAppleM4A). It is NOT
// a passthrough copy: expect a new encoded track and a different bitrate.

#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import "ExportComposition.h"

// A missing audio track is an error (mode 0/1/2 all ask for audio). Set to 1 to export silent video
// instead of reporting "no audio track".
#ifndef PV2_EXPORT_ALLOW_SILENT_VIDEO
#define PV2_EXPORT_ALLOW_SILENT_VIDEO 1
#endif

// Audio pitch policy for re-timed audio. AVAudioTimePitchAlgorithmTimeDomain keeps the pitch of
// speech-like material by retiming in the time domain; it is handed to the exporter through
// AVAudioMixInputParameters.audioTimePitchAlgorithm. If the exporter ignores the audio mix the result
// degenerates to a varispeed retime (pitch follows the rate) - see the report for how to verify.
#ifndef PV2_EXPORT_KEEP_PITCH
#define PV2_EXPORT_KEEP_PITCH 1
#endif

static inline NSString *PV2ExportPitchAlgorithmFor(double rate) {
    if (!isfinite(rate) || fabs(rate - 1.0) <= 1e-9) return nil;
#if PV2_EXPORT_KEEP_PITCH
    return AVAudioTimePitchAlgorithmTimeDomain;
#else
    return nil;
#endif
}

static inline NSString *PV2ExportPitchPolicyName(double rate) {
    if (fabs(rate - 1.0) <= 1e-9) return @"原速";
#if PV2_EXPORT_KEEP_PITCH
    return @"时间域保持音调";
#else
    return @"变调(随倍速)";
#endif
}

#pragma mark - Task state

@interface PV2ExportTask : NSObject
@property (nonatomic, weak) UIViewController *owner;          // explicit weak type (C++17 + ARC)
@property (nonatomic, weak) UIBarButtonItem *anchor;          // iPad popover anchor
@property (nonatomic, strong) AVAsset *asset;                 // captured at initiation
@property (nonatomic, strong) AVVideoComposition *sourceVideoComposition;
@property (nonatomic, strong) AVAudioMix *sourceAudioMix;
@property (nonatomic) double speed;                           // captured at initiation
@property (nonatomic) NSInteger mode;
@property (nonatomic) PV2ExportSpec spec;
@property (nonatomic, strong) NSURL *directoryURL;
@property (nonatomic, strong) NSURL *outputURL;
@property (nonatomic, strong) AVAssetExportSession *session;
@property (nonatomic, weak) UIAlertController *progressAlert;
@property (nonatomic, strong) NSTimer *progressTimer;
@property (nonatomic) double lastProgress;
@property (nonatomic) BOOL cancelled;
@property (nonatomic) BOOL finished;
- (void)pv2ProgressTick:(NSTimer *)timer;
@end

@implementation PV2ExportTask
- (void)pv2ProgressTick:(NSTimer *)timer {
    if (self.finished) {
        [timer invalidate];
        return;
    }
    double progress = self.session ? (double)self.session.progress : 0.0;
    if (!(progress >= 0.0)) progress = 0.0;
    if (progress > 1.0) progress = 1.0;
    if (progress < self.lastProgress) progress = self.lastProgress;
    self.lastProgress = progress;
    UIAlertController *alert = self.progressAlert;
    if (!alert) return;
    NSString *phase = self.cancelled ? @"取消中" : (self.session ? @"导出中" : @"准备中");
    alert.message = [NSString stringWithFormat:@"%@… %d%%\n音调策略：%@",
                     phase, (int)lround(progress * 100.0), PV2ExportPitchPolicyName(self.speed)];
}
@end

#pragma mark - Small helpers

static inline NSMutableArray<PV2ExportTask *> *PV2ExportRegistry(void) {
    static NSMutableArray<PV2ExportTask *> *tasks;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ tasks = [NSMutableArray array]; });
    return tasks;
}

static inline dispatch_queue_t PV2ExportWorkQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_queue_attr_t attributes =
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);
        queue = dispatch_queue_create("com.photosvideo2x.media-export", attributes);
    });
    return queue;
}

// Any registered task still owning this view controller blocks a second export from the same screen.
static inline PV2ExportTask *PV2ExportTaskForOwner(UIViewController *owner) {
    if (!owner) return nil;
    for (PV2ExportTask *task in PV2ExportRegistry()) {
        if (task.owner == owner) return task;
    }
    return nil;
}

static inline BOOL PV2ExportOwnerIsVisible(UIViewController *owner) {
    if (!owner) return NO;
    if (!owner.isViewLoaded) return NO;
    if (owner.view.window == nil) return NO;
    if (owner.isBeingDismissed || owner.isMovingFromParentViewController) return NO;
    if ([UIApplication sharedApplication].applicationState != UIApplicationStateActive) return NO;
    return YES;
}

static inline UIViewController *PV2ExportPresentationHost(UIViewController *owner) {
    UIViewController *host = owner;
    while (host.presentedViewController) host = host.presentedViewController;
    return host;
}

static inline NSURL *PV2ExportTempRootURL(void) {
    NSURL *root = [[NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES]
                      URLByAppendingPathComponent:@"PhotosVideo2x-Export" isDirectory:YES];
    [[NSFileManager defaultManager] createDirectoryAtURL:root withIntermediateDirectories:YES
                                              attributes:nil error:NULL];
    return root;
}

// Only ever touches our own export root; never any Photos / user media location.
static inline void PV2ExportSweepStaleDirectories(void) {
    NSFileManager *manager = [NSFileManager defaultManager];
    NSURL *root = PV2ExportTempRootURL();
    NSArray<NSURL *> *entries =
        [manager contentsOfDirectoryAtURL:root
               includingPropertiesForKeys:@[NSURLContentModificationDateKey]
                                  options:NSDirectoryEnumerationSkipsHiddenFiles error:NULL];
    NSDate *cutoff = [NSDate dateWithTimeIntervalSinceNow:-PV2_EXPORT_ORPHAN_TTL_SECONDS];
    for (NSURL *entry in entries) {
        BOOL inUse = NO;
        for (PV2ExportTask *task in PV2ExportRegistry()) {
            if (task.directoryURL && [task.directoryURL.path isEqualToString:entry.path]) { inUse = YES; break; }
        }
        if (inUse) continue;
        NSDate *modified = nil;
        if (![entry getResourceValue:&modified forKey:NSURLContentModificationDateKey error:NULL]) continue;
        if (modified && [modified compare:cutoff] == NSOrderedAscending) {
            [manager removeItemAtURL:entry error:NULL];
        }
    }
}

static inline void PV2ExportRemoveDirectory(PV2ExportTask *task) {
    if (!task.directoryURL) return;
    [[NSFileManager defaultManager] removeItemAtURL:task.directoryURL error:NULL];
    task.directoryURL = nil;
    task.outputURL = nil;
}

static inline void PV2ExportDismissProgress(PV2ExportTask *task, void (^completion)(void)) {
    UIAlertController *alert = task.progressAlert;
    task.progressAlert = nil;
    if (alert && alert.presentingViewController) {
        [alert dismissViewControllerAnimated:YES completion:completion];
    } else if (completion) {
        completion();
    }
}

// Ends the export: stops the timer, drops the progress UI, deletes the temp directory when asked and
// releases the owner slot so the next export can start.
static inline void PV2ExportTeardown(PV2ExportTask *task, BOOL deleteFile) {
    if (!task) return;
    task.finished = YES;
    [task.progressTimer invalidate];
    task.progressTimer = nil;
    if (deleteFile) PV2ExportRemoveDirectory(task);
    PV2ExportDismissProgress(task, nil);
    [PV2ExportRegistry() removeObject:task];
}

static inline void PV2ExportFail(PV2ExportTask *task, PV2ExportReason reason, NSString *detail) {
    NSLog(@"[PhotosVideo2x] export mode=%ld failed: %s (%@)",
          (long)task.mode, PV2ExportReasonString(reason), detail ?: @"-");
    UIViewController *owner = task.owner;
    const BOOL reachable = PV2ExportOwnerIsVisible(owner);
    PV2ExportPresentation decision = PV2ExportDecidePresentation(PV2ExportOutcomeFailed,
                                                                owner != nil ? true : false,
                                                                reachable ? true : false);
    PV2ExportDismissProgress(task, ^{
        if (!decision.showError) return;
        UIViewController *host = PV2ExportPresentationHost(owner);
        if (host.presentedViewController) return;   // never fight an unrelated presentation
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"导出失败"
                                                                     message:detail ?: @"未知错误"
                                                              preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
        [host presentViewController:alert animated:YES completion:nil];
    });
    PV2ExportTeardown(task, YES);
}

static inline void PV2ExportFailOnMain(PV2ExportTask *task, PV2ExportReason reason, NSString *detail) {
    if ([NSThread isMainThread]) {
        PV2ExportFail(task, reason, detail);
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{ PV2ExportFail(task, reason, detail); });
    }
}

#pragma mark - Export pipeline

static inline void PV2ExportStartWork(PV2ExportTask *task);

// Main thread: the output location is created here (never on the work queue) right before the
// export session starts writing into it.
static inline void PV2ExportSessionReady(PV2ExportTask *task, AVAssetExportSession *session,
                                         AVMutableVideoComposition *videoComposition,
                                         AVAudioMix *audioMix, NSString *container) {
    if (task.finished) return;
    if (task.cancelled) {
        PV2ExportTeardown(task, YES);
        return;
    }
    if (!session || !container) {
        PV2ExportFail(task, PV2ExportReasonCompositionFailed, @"导出会话无法创建（预设或容器不受支持）");
        return;
    }
    char stemBuffer[64];
    PV2ExportFileStem(stemBuffer, sizeof(stemBuffer), (int)task.mode, task.speed);
    NSString *stem = [NSString stringWithUTF8String:stemBuffer] ?: @"Media";
    NSString *extension = [container isEqualToString:AVFileTypeAppleM4A] ? @"m4a"
                        : ([container isEqualToString:AVFileTypeMPEG4] ? @"mp4" : @"mov");
    NSURL *directory = task.directoryURL;
    if (!directory) {
        PV2ExportFail(task, PV2ExportReasonCompositionFailed, @"临时目录缺失");
        return;
    }
    NSURL *outputURL = [directory URLByAppendingPathComponent:
                            [stem stringByAppendingPathExtension:extension]];
    task.outputURL = outputURL;
    session.outputURL = outputURL;
    session.outputFileType = container;
    session.shouldOptimizeForNetworkUse = NO;
    if (videoComposition) session.videoComposition = videoComposition;
    if (audioMix) session.audioMix = audioMix;
    task.session = session;

    if (PV2ExportOwnerIsVisible(task.owner) && !task.progressTimer) {
        task.progressTimer = [NSTimer scheduledTimerWithTimeInterval:0.15 target:task
                                                           selector:@selector(pv2ProgressTick:)
                                                           userInfo:nil repeats:YES];
        [task pv2ProgressTick:task.progressTimer];
    }
    AVAssetExportSession *active = session;
    [active exportAsynchronouslyWithCompletionHandler:^{
        const AVAssetExportSessionStatus status = active.status;
        NSError *exportError = active.error;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (task.finished) return;
            if (status == AVAssetExportSessionStatusCompleted) {
                NSDictionary *attributes = [[NSFileManager defaultManager]
                    attributesOfItemAtPath:task.outputURL.path error:NULL];
                unsigned long long bytes = [attributes fileSize];
                if (bytes == 0) {
                    PV2ExportFail(task, PV2ExportReasonExportFailed, @"导出文件为空");
                    return;
                }
                UIViewController *owner = task.owner;
                PV2ExportPresentation decision =
                    PV2ExportDecidePresentation(PV2ExportOutcomeSucceeded,
                                                owner != nil ? true : false,
                                                PV2ExportOwnerIsVisible(owner) ? true : false);
                if (!decision.presentShare) {
                    // Owner gone / backgrounded: keep the file for the TTL window, do not force UI.
                    NSLog(@"[PhotosVideo2x] export finished while owner unreachable, keeping %@ for %.0fs",
                          task.outputURL.path, PV2_EXPORT_ORPHAN_TTL_SECONDS);
                    PV2ExportTask *retained = task;
                    PV2ExportTeardown(task, NO);
                    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                                 (int64_t)(PV2_EXPORT_ORPHAN_TTL_SECONDS * NSEC_PER_SEC)),
                                   dispatch_get_main_queue(), ^{
                        PV2ExportRemoveDirectory(retained);
                    });
                    return;
                }
                const BOOL keepFile = !decision.deleteFile;
                PV2ExportDismissProgress(task, ^{
                    if (!keepFile) return;
                    if (!PV2ExportOwnerIsVisible(owner)) { PV2ExportTeardown(task,YES); return; }
                    UIViewController *host = PV2ExportPresentationHost(owner);
                    UIActivityViewController *share = [[UIActivityViewController alloc]
                        initWithActivityItems:@[task.outputURL] applicationActivities:nil];
                    UIPopoverPresentationController *popover = share.popoverPresentationController;
                    if (popover) {
                        UIBarButtonItem *anchor = task.anchor;
                        // Fallback source, used when the bar button item is no longer installed.
                        popover.sourceView = owner.view;
                        popover.sourceRect = CGRectMake(CGRectGetMidX(owner.view.bounds),
                                                        CGRectGetMidY(owner.view.bounds), 1.0, 1.0);
                        popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
                        if (anchor) popover.barButtonItem = anchor;
                    }
                    share.completionWithItemsHandler = ^(UIActivityType activityType, BOOL completed,
                                                         NSArray *returnedItems, NSError *activityError) {
                        (void)activityType; (void)completed; (void)returnedItems; (void)activityError;
                        // Cleanup happens after the sheet is gone so the destination copy is done.
                        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.4 * NSEC_PER_SEC)),
                                       dispatch_get_main_queue(), ^{
                            PV2ExportTeardown(task, YES);
                        });
                    };
                    [host presentViewController:share animated:YES completion:nil];
                });
                return;
            }
            if (status == AVAssetExportSessionStatusCancelled) {
                NSLog(@"[PhotosVideo2x] export cancelled by user");
                PV2ExportTeardown(task, YES);
                return;
            }
            PV2ExportFail(task, PV2ExportReasonExportFailed,
                          exportError.localizedDescription ?: @"导出失败");
        });
    }];
}

// Runs on the private work queue: composition, edit remapping and session creation all happen off
// the main thread. Only the captured asset/videoComposition/audioMix/speed/mode are used - never the
// player item's current state - so a page switch cannot retarget an export in flight.
static inline void PV2ExportStartWork(PV2ExportTask *task) {
    AVAsset *asset = task.asset;
    AVVideoComposition *sourceVideoComposition = task.sourceVideoComposition;
    AVAudioMix *sourceAudioMix = task.sourceAudioMix;
    PV2ExportSpec spec = task.spec;
    NSString *pitchAlgorithm = PV2ExportPitchAlgorithmFor(spec.rate);
    dispatch_async(PV2ExportWorkQueue(), ^{
        PV2ExportCompositionResult *result =
            PV2ExportBuildScaledComposition(asset, spec.rate,
                                            spec.includeVideo ? YES : NO,
                                            spec.includeAudio ? YES : NO,
                                            PV2_EXPORT_ALLOW_SILENT_VIDEO ? YES : NO);
        if (!result.composition) {
            PV2ExportFailOnMain(task, result.reason, result.failureDetail);
            return;
        }
        NSString *videoDetail = nil;
        AVMutableVideoComposition *videoComposition =
            PV2ExportCompatibleVideoComposition(spec.includeVideo ? sourceVideoComposition : nil, result.trackIDMap, spec.rate,
                                                &videoDetail);
        if (spec.includeVideo && !sourceVideoComposition && !videoDetail)
            videoComposition=PV2ExportDefaultVideoComposition(result.composition);
        if (videoDetail) {
            // The item carries an edit combination we cannot re-time safely. Refuse loudly instead
            // of exporting a version that silently loses the user's animation/crop/opacity edits.
            PV2ExportFailOnMain(task, PV2ExportReasonUnsupportedEdit, videoDetail);
            return;
        }
        NSString *audioDetail = nil;
        AVAudioMix *audioMix = PV2ExportCompatibleAudioMix(result.composition, sourceAudioMix,
                                                          result.trackIDMap, spec.rate, pitchAlgorithm,
                                                          result.targetDurationSeconds, &audioDetail);
        if (audioDetail) {
            PV2ExportFailOnMain(task, PV2ExportReasonUnsupportedEdit, audioDetail);
            return;
        }

        NSString *preset = spec.includeVideo ? AVAssetExportPresetHighestQuality
                                            : AVAssetExportPresetAppleM4A;
        AVAssetExportSession *session = [[AVAssetExportSession alloc]
            initWithAsset:result.composition presetName:preset];
        if (!session) {
            PV2ExportFailOnMain(task, PV2ExportReasonCompositionFailed,
                                @"导出会话无法创建（预设不受支持）");
            return;
        }
        NSArray<AVFileType> *supported = session.supportedFileTypes;
        NSString *container = nil;
        if (spec.includeVideo) {
            container = [supported containsObject:AVFileTypeMPEG4] ? AVFileTypeMPEG4 : nil;
            if (!container && [supported containsObject:AVFileTypeQuickTimeMovie]) {
                container = AVFileTypeQuickTimeMovie;
            }
        } else if ([supported containsObject:AVFileTypeAppleM4A]) {
            container = AVFileTypeAppleM4A;
        }
        if (!container) {
            PV2ExportFailOnMain(task, PV2ExportReasonCompositionFailed,
                                @"没有可用的输出容器类型");
            return;
        }
        NSLog(@"[PhotosVideo2x] export mode=%ld rate=%.3f container=%@ pitch=%@ tracks=%lu duration=%.3fs",
              (long)task.mode, spec.rate, container, pitchAlgorithm ?: @"none",
              (unsigned long)result.composition.tracks.count, result.outputDurationSeconds);
        dispatch_async(dispatch_get_main_queue(), ^{
            PV2ExportSessionReady(task, session, videoComposition, audioMix, container);
        });
    });
}

static inline void PV2ExportBeginLoading(PV2ExportTask *task) {
    AVAsset *asset = task.asset;
    NSArray<NSString *> *keys = @[@"duration", @"tracks", @"playable"];
    // Asynchronous key loading: nothing here blocks the main thread, and the asset is retained by
    // the task for as long as the load is in flight.
    [asset loadValuesAsynchronouslyForKeys:keys completionHandler:^{
        NSError *loadError = nil;
        const AVKeyValueStatus durationStatus = [asset statusOfValueForKey:@"duration" error:&loadError];
        const AVKeyValueStatus tracksStatus = [asset statusOfValueForKey:@"tracks" error:NULL];
        const AVKeyValueStatus playableStatus = [asset statusOfValueForKey:@"playable" error:NULL];
        dispatch_async(dispatch_get_main_queue(), ^{
            if (task.finished) return;
            if (task.cancelled) { PV2ExportTeardown(task, YES); return; }
            if (durationStatus != AVKeyValueStatusLoaded || tracksStatus != AVKeyValueStatusLoaded ||
                playableStatus != AVKeyValueStatusLoaded) {
                PV2ExportFail(task, PV2ExportReasonAssetNotLoaded,
                              loadError.localizedDescription ?: @"资源信息无法加载");
                return;
            }
            if (!PV2ExportAssetIsPlayable(asset)) {
                PV2ExportFail(task, PV2ExportReasonNotPlayable, @"资源不可播放");
                return;
            }
            const double seconds = PV2ExportCMTimeSeconds(PV2ExportAssetDuration(asset));
            if (!PV2ExportDurationIsValid(seconds)) {
                PV2ExportFail(task, PV2ExportReasonBadDuration, @"资源时长无效");
                return;
            }
            PV2ExportSpec spec = task.spec;
            const BOOL hasVideo = PV2ExportAssetTracksOfType(asset, AVMediaTypeVideo).count > 0;
            const BOOL hasAudio = PV2ExportAssetTracksOfType(asset, AVMediaTypeAudio).count > 0;
            const PV2ExportReason trackReason =
                PV2ExportValidateTracks(&spec, hasVideo ? true : false, hasAudio ? true : false,
                                        PV2_EXPORT_ALLOW_SILENT_VIDEO ? true : false);
            if (trackReason != PV2ExportReasonNone) {
                NSString *detail = (trackReason == PV2ExportReasonNoVideoTrack)
                                       ? @"该资源没有视频轨道"
                                       : @"该资源没有音频轨道";
                PV2ExportFail(task, trackReason, detail);
                return;
            }
            PV2ExportStartWork(task);
        });
    }];
}

#pragma mark - Public interface

// Starts one export for `owner`. Safe to call from the main thread only; hops internally otherwise.
// `anchor` is the iPad popover anchor (the bar button item that triggered the export). `speed` is the
// currently displayed rate; `mode` selects the export option (PV2ExportMode*).
static inline void PV2ExportMedia(UIViewController *owner, UIBarButtonItem *anchor, AVPlayerItem *item,
                                  double speed, NSInteger mode) {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            PV2ExportMedia(owner, anchor, item, speed, mode);
        });
        return;
    }
    PV2ExportSpec spec;
    if (!PV2ExportResolveSpec((int)mode, speed, &spec)) {
        NSLog(@"[PhotosVideo2x] export rejected: mode=%ld speed=%.3f (unsupported mode or rate)",
              (long)mode, speed);
        return;
    }
    if (!owner) {
        NSLog(@"[PhotosVideo2x] export rejected: no owner view controller");
        return;
    }
    if (!item) {
        PV2ExportTask *placeholder = [PV2ExportTask new];
        placeholder.owner = owner;
        placeholder.mode = mode;
        placeholder.speed = spec.rate;
        placeholder.spec = spec;
        PV2ExportFail(placeholder, PV2ExportReasonBadRequest, @"没有可导出的播放项目");
        return;
    }
    if (PV2ExportTaskForOwner(owner)) {
        // Same screen, export already in flight: refuse the re-entry (no second session, no second
        // temp directory, no second share sheet).
        NSLog(@"[PhotosVideo2x] export rejected: another export is already running for this owner");
        if (PV2ExportOwnerIsVisible(owner)) {
            UIViewController *host = PV2ExportPresentationHost(owner);
            if (!host.presentedViewController) {
                UIAlertController *alert = [UIAlertController
                    alertControllerWithTitle:@"正在导出" message:@"请等待当前导出完成或取消后再试"
                             preferredStyle:UIAlertControllerStyleAlert];
                [alert addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault
                                                        handler:nil]];
                [host presentViewController:alert animated:YES completion:nil];
            }
        }
        return;
    }

    PV2ExportTask *task = [PV2ExportTask new];
    task.owner = owner;
    task.anchor = anchor;
    task.mode = mode;
    task.speed = (mode == (NSInteger)PV2ExportModeAudioNormal) ? 1.0 : speed;
    task.spec = spec;
    // Capture at initiation: asset + editing objects + speed are frozen here, so switching pages or
    // replacing the player item afterwards cannot change what is being exported.
    task.asset = item.asset;
    task.sourceVideoComposition = item.videoComposition;
    task.sourceAudioMix = item.audioMix;
    if (!task.asset) {
        PV2ExportFail(task, PV2ExportReasonBadRequest, @"播放项目没有可用的媒体资源");
        return;
    }
    NSString *uuid = [[NSUUID UUID] UUIDString];
    NSURL *directory = [PV2ExportTempRootURL() URLByAppendingPathComponent:uuid isDirectory:YES];
    if (![[NSFileManager defaultManager] createDirectoryAtURL:directory
                                   withIntermediateDirectories:YES attributes:nil error:NULL]) {
        PV2ExportFail(task, PV2ExportReasonCompositionFailed, @"无法创建临时导出目录");
        return;
    }
    task.directoryURL = directory;
    [PV2ExportRegistry() addObject:task];
    PV2ExportSweepStaleDirectories();

    if (PV2ExportOwnerIsVisible(owner)) {
        UIViewController *host = PV2ExportPresentationHost(owner);
        if (!host.presentedViewController) {
            UIAlertController *alert = [UIAlertController
                alertControllerWithTitle:@"导出中"
                                 message:[NSString stringWithFormat:@"准备中…\n音调策略：%@",
                                          PV2ExportPitchPolicyName(task.speed)]
                          preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel
                                                    handler:^(UIAlertAction *action) {
                (void)action;
                PV2ExportTask *active = PV2ExportTaskForOwner(owner);
                if (!active) return;
                active.cancelled = YES;
                active.progressAlert.message = @"取消中…";
                if (active.session) {
                    [active.session cancelExport];
                }
                // Without a session yet the queued load/build steps will tear the task down.
            }]];
            task.progressAlert = alert;
            [host presentViewController:alert animated:YES completion:nil];
        }
    } else {
        // Owner not reachable (backgrounded or mid-transition): run headless, keep the file, no UI.
        NSLog(@"[PhotosVideo2x] export starting without UI: owner not reachable");
    }
    PV2ExportBeginLoading(task);
}

// True while an export belonging to this owner is registered (UI can grey out its own button).
static inline BOOL PV2ExportIsRunningForOwner(UIViewController *owner) {
    return PV2ExportTaskForOwner(owner) != nil;
}

// Cancels the export of one owner, e.g. when its page is torn down. The temp directory is removed by
// the normal cancellation path.
static inline void PV2ExportCancelForOwner(UIViewController *owner) {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{ PV2ExportCancelForOwner(owner); });
        return;
    }
    PV2ExportTask *task = PV2ExportTaskForOwner(owner);
    if (!task) return;
    task.cancelled = YES;
    if (task.session) {
        // The session completion handler removes the temp directory through the cancel path.
        [task.session cancelExport];
        return;
    }
    // Still loading or building: tear down now; the queued steps check `cancelled`/`finished`.
    PV2ExportTeardown(task, YES);
}
