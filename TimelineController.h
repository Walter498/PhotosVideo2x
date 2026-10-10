#pragma once
// TimelineController.h
//
// PiP-style video timeline panel for Photos one-up video playback.
//
// A compact dark-material rounded panel sits low in the video chrome, its
// bottom edge 4pt above the real UIToolbar top (converted into owner.view
// coordinates), or above the safe-area bottom when no toolbar is visible. It
// shows monospaced elapsed time on the left, negative remaining time on the
// right, and a thin white/grey UISlider with a small thumb (12pt, 18pt while
// dragging) and a 44pt-tall vertical hit target.
//
// It is shown only for the currently selected video tile and only when the
// native top AND bottom bars are both visible -- exactly the same gate the
// resource/download panel uses (PV2TileIsSelected + PV2ResourceChromeVisible).
// Photos and unrelated pages never get controls.
//
// Include contract (identical to VideoTools.h): import this AFTER
// DownloadController.h (which Tweak.xm already imports) and after PV2Log has
// been defined. It deliberately does not import DownloadController.h itself so
// Tweak.xm controls the ordering:
//
//   #import "DownloadController.h"   // PV2DownloadABI, PV2TileIsSelected,
//                                    // PV2ResourceChromeVisible, PV2SelectedAsset,
//                                    // PV2VisibleOneUp, PV2OneUpVisible,
//                                    // PV2NativeBrowsing, PV2CurrentOneUpTile, PV2Log
//   #import "VideoTools.h"
//   #import "TimelineController.h"
//   ...
//   PV2TimelineInstall();            // in %ctor, after the other ABI gates
//
// Native objects are only reached through the *selected tile's* browsing
// player (`_browsingVideoPlayer`); nothing is derived from a global "first"
// player, and the loop clone (currentPlayerItem) is never used as the source of
// truth. Seeking always goes through the native browsing player so slow-motion
// and edited timelines stay mapped by Photos itself. There are no UISlider
// hooks and no rate / looping writes.
//
// Seeking is serialized through a single-flight queue (TimelineCore.h): at most
// one native seekToTime:completionHandler: runs at a time, extra requests
// coalesce into one pending "latest target", and a completion immediately hands
// off to that pending target -- so the final drag position / newest double-tap
// always wins. Every request carries an epoch; the epoch advances (and any
// pending target is dropped) whenever the tile, browsing player or video session
// changes, on chrome hide, backgrounding, teardown and drag start, so a late
// callback from a previous asset or from leave is ignored. The completion
// handler is therefore meaningful, not decorative. While visible the panel
// repaints from native currentTime/duration at ~30Hz and the timer is stopped on
// chrome hide / background / exit. Duration is native-only (PHAsset.duration is
// never substituted) and the slider stays disabled until a valid native
// duration is available. Double-tap +/- works with the chrome hidden.

#import <UIKit/UIKit.h>
#import <CoreMedia/CoreMedia.h>
#import <objc/runtime.h>
#import <math.h>
#include "TimelineCore.h"

// ---------------------------------------------------------------------------
// Native selectors. currentTime / seekToTime:completionHandler: are already
// declared by DownloadController.h; duration and videoSession are added here on
// the PV2NativeBrowsing shell so the browsing-only ABI stays explicit. The
// runtime encoding is re-checked in PV2TimelineABI() before any use.
// ---------------------------------------------------------------------------
@interface PV2NativeBrowsing (PV2Timeline)
- (CMTime)duration;
- (id)videoSession;
- (void)seekToTime:(CMTime)time completionHandler:(void (^)(BOOL finished))completion;
- (void)seekToTime:(CMTime)time toleranceBefore:(CMTime)before toleranceAfter:(CMTime)after completionHandler:(void (^)(BOOL finished))completion;
@end

@interface NSObject (PV2TimelinePlayer)
- (id)videoPlayer;
- (float)rate;
- (BOOL)isReadyForSeeking;
@end

static inline double PV2TimelineNow(void) {
    return NSProcessInfo.processInfo.systemUptime;
}

// Native browsing player exposes CMTime currentTime/seekToTime:completionHandler:
// as v48@0:8{?=qiIq}16@?40, and duration as {?=qiIq}16@0:8 (verified iOS17.3 DSC).
static BOOL PV2TimelineABI(void) {
    Class browsing = NSClassFromString(@"PUBrowsingVideoPlayer");
    Class tile = NSClassFromString(@"PUVideoTileViewController");
    return PV2DownloadABI(browsing, @"currentTime", "{?=qiIq}16@0:8")
        && PV2DownloadABI(browsing, @"duration", "{?=qiIq}16@0:8")
        && PV2DownloadABI(browsing, @"seekToTime:completionHandler:", "v48@0:8{?=qiIq}16@?40")
        && PV2DownloadABI(browsing, @"seekToTime:toleranceBefore:toleranceAfter:completionHandler:", "v96@0:8{?=qiIq}16{?=qiIq}40{?=qiIq}64@?88")
        && PV2DownloadABI(NSClassFromString(@"PXVideoSession"), @"isReadyForSeeking", "B16@0:8")
        && PV2DownloadABI(browsing, @"videoSession", "@16@0:8")
        && PV2DownloadABI(tile, @"_browsingVideoPlayer", "@16@0:8")
        && PV2DownloadABI(tile, @"videoSession", "@16@0:8");
}

static BOOL PV2TimelineEnabled = NO;

static void PV2TimelineInstall(void) {
    PV2TimelineEnabled = PV2TimelineABI();
    PV2Log([NSString stringWithFormat:@"timeline %@", PV2TimelineEnabled ? @"ABI ok" : @"ABI mismatch; skipped"]);
}

static UIToolbar *PV2TimelineVisibleToolbar(UIViewController *owner) {
    if (!owner || !owner.isViewLoaded) return nil;
    UIToolbar *toolbar = owner.navigationController.toolbar;
    if (toolbar && !toolbar.hidden && toolbar.alpha > 0.01 && toolbar.window != nil && toolbar.bounds.size.height > 1.0)
        return toolbar;
    return nil;
}

static BOOL PV2TimelineAssetIsVideo(void) {
    id asset = PV2SelectedAsset();
    if (![asset isKindOfClass:PHAsset.class]) return NO;
    return ((PHAsset *)asset).mediaType == PHAssetMediaTypeVideo;
}

@class PV2TimelineController;

// Keeps the repeating timer from retaining the controller (which would keep the
// panel alive past its owner). The controller invalidates the timer on teardown.
@interface PV2TimelineTicker : NSObject
@property (nonatomic, weak) PV2TimelineController *controller;
- (void)tick:(NSTimer *)timer;
@end

@interface PV2TimelineController : NSObject
@property (nonatomic, weak) UIViewController *owner;
@property (nonatomic, weak) id tile;
@property (nonatomic, weak) id browsing;
@property (nonatomic, strong) UIVisualEffectView *panel;
@property (nonatomic, strong) UILabel *elapsedLabel;
@property (nonatomic, strong) UILabel *remainingLabel;
@property (nonatomic, strong) UISlider *slider;
@property (nonatomic, strong) NSTimer *timer;
@property (nonatomic, strong) PV2TimelineTicker *ticker;
@property (nonatomic, strong) NSMutableArray *observerTokens;
@property (nonatomic) BOOL dragging;
@property (nonatomic) BOOL pausedByController;
@property (nonatomic) BOOL wasPlaying;
@property (nonatomic) double dragTargetSeconds;
@property (nonatomic) double lastEmitTime;
// Serialized single-flight seek queue (pure policy in TimelineCore.h). The
// controller only owns what the policy cannot: the identity (tile / browsing /
// video session) it was built against.
@property (nonatomic) PV2TimelineSeekState seekState;
@property (nonatomic, weak) id sessionToken;
@property (nonatomic, copy) NSString *assetIdentifier;
- (instancetype)init;
- (void)refresh;
- (BOOL)seekBySeconds:(double)seconds;
- (void)teardown;
- (void)tick:(NSTimer *)timer;
@end

@implementation PV2TimelineController

- (instancetype)init {
    self = [super init];
    if (self) PV2TimelineSeekReset(&_seekState);
    return self;
}

#pragma mark - Identity

// Bind the controller to whatever the live one-up currently selects, even when
// no panel exists yet (double-tap seek must work with the chrome hidden). Any
// change of tile / browsing player / video session advances the seek epoch so a
// callback belonging to the previous identity is ignored.
- (BOOL)bindSelectedIdentity {
    UIViewController *owner = self.owner;
    if (!owner || !PV2OneUpVisible || owner != PV2VisibleOneUp || !owner.isViewLoaded) return NO;
    BOOL changed = NO;
    id asset=PV2SelectedAsset();
    NSString *identifier=[asset isKindOfClass:PHAsset.class] ? ((PHAsset *)asset).localIdentifier : nil;
    if (![self.assetIdentifier isEqualToString:identifier]) {
        self.assetIdentifier=identifier;[self invalidateSeeks];changed=YES;
    }
    id tile = [owner _currentContentTileController];
    if (tile != self.tile) {
        self.tile = tile;
        [self invalidateSeeks];
        changed = YES;
    }
    id browsing = [self resolveBrowsing];
    if (browsing != self.browsing) {
        self.browsing = browsing;
        [self invalidateSeeks];
        changed = YES;
    }
    id session = nil;
    if (browsing) {
        @try { session = [(PV2NativeBrowsing *)browsing videoSession]; }
        @catch (__unused NSException *exception) { session = nil; }
    }
    if (session != self.sessionToken) {
        self.sessionToken = session;
        [self invalidateSeeks];
        changed = YES;
    }
    return changed;
}

// Drop any in-flight/pending seek and advance the epoch: the outstanding native
// callback (if any) is now stale and will be ignored.
- (void)invalidateSeeks {
    PV2TimelineSeekInvalidate(&_seekState);
}

#pragma mark - Eligibility

// Chosen video tile on the live one-up, ignoring chrome: used to decide between
// "hide, chrome is off" and "tear the controller down, different page".
- (BOOL)isEligible {
    if (!PV2TimelineEnabled || !PV2OneUpVisible || !PV2VisibleOneUp) return NO;
    UIViewController *owner = self.owner;
    if (!owner || owner != PV2VisibleOneUp || !owner.isViewLoaded || owner.view.window == nil) return NO;
    if (!PV2TimelineAssetIsVideo()) return NO;
    PHAsset *asset=PV2SelectedAsset();
    return [self.assetIdentifier isEqualToString:asset.localIdentifier] && PV2TileIsSelected(self.tile);
}

// Visible panel: selected video, both native bars visible (same as the resource
// panel), and the native browsing ABI present at runtime.
- (BOOL)isCurrent {
    if (![self isEligible]) return NO;
    return PV2ResourceChromeVisible();
}

- (id)resolveBrowsing {
    id tile = self.tile;
    if (!tile || ![tile respondsToSelector:@selector(_browsingVideoPlayer)]) return nil;
    id browsing = nil;
    @try { browsing = [tile _browsingVideoPlayer]; }
    @catch (__unused NSException *exception) { return nil; }
    if (!browsing) return nil;
    if (!PV2DownloadABI([browsing class], @"currentTime", "{?=qiIq}16@0:8")) return nil;
    return browsing;
}

#pragma mark - Time

- (double)playbackDuration {
    double nativeSeconds = 0.0;
    BOOL nativeValid = NO;
    id browsing = self.browsing;
    if (browsing) {
        @try {
            CMTime time = [(PV2NativeBrowsing *)browsing duration];
            if (CMTIME_IS_VALID(time) && CMTIME_IS_NUMERIC(time)) {
                nativeSeconds = CMTimeGetSeconds(time);
                nativeValid = isfinite(nativeSeconds) && nativeSeconds > 0.0;
            }
        } @catch (__unused NSException *exception) {}
    }
    // Native only: PHAsset.duration is NOT used because slow-motion / edited
    // clips need the time-range-mapped native length. An invalid native duration
    // means the timeline is not ready yet (the slider is disabled), not that we
    // should substitute the asset length.
    return PV2TimelineNativeTimelineSeconds(nativeSeconds, nativeValid);
}

- (double)currentSeconds {
    id browsing = self.browsing;
    if (!browsing) return 0.0;
    @try {
        CMTime time = [(PV2NativeBrowsing *)browsing currentTime];
        if (CMTIME_IS_VALID(time) && CMTIME_IS_NUMERIC(time)) {
            double seconds = CMTimeGetSeconds(time);
            if (isfinite(seconds) && seconds >= 0.0) return seconds;
        }
    } @catch (__unused NSException *exception) {}
    return 0.0;
}

- (BOOL)isPlaying {
    id browsing = self.browsing;
    if (!browsing) return NO;
    @try {
        id player = [[browsing videoSession] videoPlayer];
        if ([player respondsToSelector:@selector(rate)]) return PV2TimelineRateIsPlaying([player rate]);
    } @catch (__unused NSException *exception) {}
    return NO;
}

// Clamp to the native range and submit to the single-flight queue.
- (void)requestSeekSeconds:(double)seconds {
    if (!self.browsing) return;
    double duration = [self playbackDuration];
    if (!PV2TimelineHasDuration(duration)) return;
    [self submitSeekTarget:PV2TimelineClampSeconds(seconds, duration)];
}

- (void)submitSeekTarget:(double)target {
    double emitNow = 0.0;
    if (!PV2TimelineSeekRequest(&_seekState, target, &emitNow)) return; // coalesced into pending
    [self issueNativeSeekTo:emitNow];
}

- (void)issueNativeSeekTo:(double)target {
    id browsing = self.browsing;
    if (!browsing || !self.sessionToken || ![self.sessionToken isReadyForSeeking]) {
        [self invalidateSeeks];return;
    }
    // Capture the identity this seek belongs to. A completion whose identity no
    // longer matches (left the page / different asset+browsing+session) is stale.
    unsigned long epoch = _seekState.epoch;
    id session = self.sessionToken;
    CMTime time = CMTimeMakeWithSeconds(target, 600);
    __weak PV2TimelineController *weakSelf = self;
    @try {
        CMTime tolerance=self.dragging ? CMTimeMakeWithSeconds(0.1,600) : kCMTimeZero;
        [(PV2NativeBrowsing *)browsing seekToTime:time toleranceBefore:tolerance toleranceAfter:tolerance completionHandler:^(__unused BOOL finished) {
            // PhotoKit may call back off the main thread; hop back before touching UI state.
            dispatch_async(dispatch_get_main_queue(), ^{
                [weakSelf nativeSeekCompletedForEpoch:epoch browsing:browsing session:session];
            });
        }];
    } @catch (__unused NSException *exception) {
        PV2Log(@"timeline seek failed");
        [self nativeSeekCompletedForEpoch:epoch browsing:browsing session:session]; // never deadlock the slot
    }
}

- (void)nativeSeekCompletedForEpoch:(unsigned long)epoch browsing:(id)browsing session:(id)session {
    // Old callbacks must not invalidate a newer identity's live seek slot.
    if (epoch!=_seekState.epoch || self.browsing!=browsing || self.sessionToken!=session) return;
    BOOL live=[self isEligible] && [self.tile _browsingVideoPlayer]==browsing && [browsing videoSession]==session;
    if (!live) { [self invalidateSeeks]; return; }
    BOOL stale = NO;
    double next = 0.0;
    if (PV2TimelineSeekCompletion(&_seekState, epoch, &stale, &next)) {
        [self issueNativeSeekTo:next]; // latest coalesced target wins
    }
}

#pragma mark - Panel construction

- (UILabel *)clockLabel {
    UILabel *label = [UILabel new];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.font = [UIFont monospacedDigitSystemFontOfSize:12.0 weight:UIFontWeightMedium];
    label.textColor = UIColor.whiteColor;
    label.adjustsFontSizeToFitWidth = YES;
    label.minimumScaleFactor = 0.7;
    label.text = @"0:00";
    return label;
}

+ (UIImage *)thumbImageDiameter:(CGFloat)diameter {
    CGSize size = CGSizeMake(diameter, diameter);
    UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:size];
    return [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
        (void)context;
        [[UIColor colorWithWhite:1.0 alpha:1.0] setFill];
        UIBezierPath *path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(0.0, 0.0, diameter, diameter)];
        [path fill];
    }];
}

- (UISlider *)makeSlider {
    UISlider *slider = [UISlider new];
    slider.translatesAutoresizingMaskIntoConstraints = NO;
    slider.minimumValue = 0.0;
    slider.maximumValue = 1.0;
    slider.minimumTrackTintColor = [UIColor colorWithWhite:1.0 alpha:0.95];
    slider.maximumTrackTintColor = [UIColor colorWithWhite:1.0 alpha:0.30];
    slider.tintColor = UIColor.whiteColor;
    [slider setThumbImage:[PV2TimelineController thumbImageDiameter:12.0] forState:UIControlStateNormal];
    [slider setThumbImage:[PV2TimelineController thumbImageDiameter:18.0] forState:UIControlStateHighlighted];
    [slider addTarget:self action:@selector(sliderTouchDown) forControlEvents:UIControlEventTouchDown];
    [slider addTarget:self action:@selector(sliderValueChanged) forControlEvents:UIControlEventValueChanged];
    [slider addTarget:self action:@selector(sliderTouchUp)
     forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];
    return slider;
}

- (void)ensurePanel {
    if (self.panel) return;
    [self addObservers];
    UIVisualEffectView *panel = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemChromeMaterialDark]];
    panel.layer.cornerRadius = 12.0;
    panel.clipsToBounds = YES;
    panel.translatesAutoresizingMaskIntoConstraints = YES; // absolute frame placement
    self.elapsedLabel = [self clockLabel];
    self.remainingLabel = [self clockLabel];
    self.elapsedLabel.textAlignment = NSTextAlignmentLeft;
    self.remainingLabel.textAlignment = NSTextAlignmentRight;
    self.slider = [self makeSlider];
    self.slider.accessibilityLabel = @"播放进度";
    UIView *content = panel.contentView;
    for (UIView *view in @[ self.elapsedLabel, self.remainingLabel, self.slider ]) [content addSubview:view];
    // The slider is pinned to the full panel height, so its vertical hit target
    // is 44pt while the drawn track stays thin.
    [NSLayoutConstraint activateConstraints:@[
        [self.elapsedLabel.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:12.0],
        [self.elapsedLabel.centerYAnchor constraintEqualToAnchor:content.centerYAnchor],
        [self.elapsedLabel.widthAnchor constraintEqualToConstant:52.0],
        [self.remainingLabel.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-12.0],
        [self.remainingLabel.centerYAnchor constraintEqualToAnchor:content.centerYAnchor],
        [self.remainingLabel.widthAnchor constraintEqualToConstant:52.0],
        [self.slider.leadingAnchor constraintEqualToAnchor:self.elapsedLabel.trailingAnchor constant:8.0],
        [self.slider.trailingAnchor constraintEqualToAnchor:self.remainingLabel.leadingAnchor constant:-8.0],
        [self.slider.topAnchor constraintEqualToAnchor:content.topAnchor],
        [self.slider.bottomAnchor constraintEqualToAnchor:content.bottomAnchor]
    ]];
    self.panel = panel;
}

- (void)attachPanel {
    UIView *host = self.owner.view;
    if (!host || !self.panel) return;
    if (self.panel.superview != host) {
        [self.panel removeFromSuperview];
        [host addSubview:self.panel];
    }
    [host bringSubviewToFront:self.panel];
}

- (void)layoutPanel {
    UIView *host = self.owner.view;
    if (!host || !self.panel) return;
    UIToolbar *toolbar = PV2TimelineVisibleToolbar(self.owner);
    BOOL hasToolbar = NO;
    double toolbarTop = 0.0;
    if (toolbar) {
        CGRect frame = [toolbar convertRect:toolbar.bounds toView:host];
        double top = CGRectGetMinY(frame);
        double hostHeight = CGRectGetHeight(host.bounds);
        if (isfinite(top) && top > 0.0 && top <= hostHeight) { hasToolbar = YES; toolbarTop = top; }
    }
    UIEdgeInsets insets = host.safeAreaInsets;
    PV2TimelinePanelSpec spec;
    spec.hostWidth = CGRectGetWidth(host.bounds);
    spec.hostHeight = CGRectGetHeight(host.bounds);
    spec.safeTop = insets.top;
    spec.safeLeft = insets.left;
    spec.safeBottom = insets.bottom;
    spec.safeRight = insets.right;
    spec.toolbarTop = toolbarTop;
    spec.hasToolbar = hasToolbar;
    spec.topClearance = insets.top + 52.0; // same top-bar reserve as the resource panel
    spec.height = PV2TimelinePanelHeightDefault;
    spec.horizontalMargin = PV2TimelineHorizontalMarginDefault;
    spec.gap = PV2TimelineToolbarGapDefault;
    PV2TimelineRect rect = PV2TimelinePanelRect(&spec);
    self.panel.frame = CGRectMake(rect.x, rect.y, rect.width, rect.height);
}

#pragma mark - Content refresh

- (void)updateLabelsForSeconds:(double)current duration:(double)duration {
    char buffer[32];
    PV2TimelineFormatElapsed(current, buffer, (int)sizeof(buffer));
    self.elapsedLabel.text = [NSString stringWithUTF8String:buffer];
    PV2TimelineFormatRemaining(current, duration, buffer, (int)sizeof(buffer));
    self.remainingLabel.text = [NSString stringWithUTF8String:buffer];
}

- (void)refreshNow {
    if (self.dragging) return;
    double duration = [self playbackDuration];
    BOOL ready = PV2TimelineHasDuration(duration) && self.sessionToken && [self.sessionToken isReadyForSeeking];
    // No valid native duration => not ready: keep the slider disabled instead of
    // fabricating a range from the asset (which would desync slow-motion clips).
    self.slider.enabled = ready;
    self.slider.userInteractionEnabled = ready;
    if (!ready) {
        self.slider.value = 0.0;
        [self updateLabelsForSeconds:0.0 duration:0.0];
        return;
    }
    // While a seek is in flight, show the newest requested target rather than the
    // native position, which has not caught up yet.
    double shown = _seekState.hasPending ? _seekState.pendingTarget
                : (_seekState.inFlight ? _seekState.emittedTarget : [self currentSeconds]);
    self.slider.value = (float)PV2TimelineProgress(shown, duration);
    [self updateLabelsForSeconds:shown duration:duration];
}

#pragma mark - Timer

- (void)startTimerIfNeeded {
    if (self.timer || self.dragging) return;
    if (!self.ticker) self.ticker = [PV2TimelineTicker new];
    self.ticker.controller = self;
    // ~30Hz while visible; stopped on chrome hide / background / exit.
    NSTimer *timer = [NSTimer timerWithTimeInterval:PV2TimelineRefreshInterval() target:self.ticker selector:@selector(tick:) userInfo:nil repeats:YES];
    self.timer = timer;
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
}

- (void)stopTimer {
    [self.timer invalidate];
    self.timer = nil;
}

- (void)tick:(NSTimer *)timer {
    (void)timer;
    if (self.dragging) return;
    [self refresh]; // re-evaluates chrome/identity and repaints the timeline
}

#pragma mark - Lifecycle

- (void)hidePanelOnly {
    BOOL wasDragging=self.dragging;
    [self cancelDrag];
    if (wasDragging) [self invalidateSeeks]; // cancel only a hidden active drag, not double-tap seeks
    [self stopTimer];
    self.panel.hidden = YES;
}

- (void)teardown {
    [self cancelDrag];
    [self invalidateSeeks];
    [self stopTimer];
    self.panel.hidden = YES;
    [self.panel removeFromSuperview];
    self.panel = nil;
    self.elapsedLabel = nil;
    self.remainingLabel = nil;
    self.slider = nil;
    self.browsing = nil;
    self.tile = nil;
    self.sessionToken = nil;self.assetIdentifier=nil;
    [self removeObservers];
}

- (void)refresh {
    if (!NSThread.isMainThread) {
        __weak PV2TimelineController *weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf refresh]; });
        return;
    }
    UIViewController *owner = self.owner;
    if (!owner || !PV2OneUpVisible || owner != PV2VisibleOneUp) { [self teardown]; return; }
    // Rebind the selected tile / browsing player / session even before any panel
    // exists; a changed identity cancels any drag and stales pending seeks.
    if ([self bindSelectedIdentity]) [self cancelDrag];

    if (![self isEligible]) { [self teardown]; return; }
    if (!PV2ResourceChromeVisible()) { [self hidePanelOnly]; return; } // both native bars required

    [self ensurePanel];
    [self attachPanel];
    [self layoutPanel];
    self.panel.hidden = NO;
    [self refreshNow];
    [self startTimerIfNeeded];
}

#pragma mark - Dragging

- (double)sliderSeconds {
    return PV2TimelineSecondsForProgress(self.slider.value, [self playbackDuration]);
}

- (void)sliderTouchDown {
    if (![self isCurrent]) return;
    if (!PV2TimelineHasDuration([self playbackDuration]) || ![self.sessionToken isReadyForSeeking]) return;
    self.dragging = YES;
    // Never pause and never write rate: native browsing seek preserves the
    // player's own play/pause intent and the fixed/boosted rate.
    self.pausedByController = NO;
    self.wasPlaying = [self isPlaying];
    self.lastEmitTime = NAN; // first move emits immediately
    self.dragTargetSeconds = [self sliderSeconds];
    [self invalidateSeeks]; // a fresh drag supersedes any pending double-tap target
    [self stopTimer];
    [self updateLabelsForSeconds:self.dragTargetSeconds duration:[self playbackDuration]];
}

- (void)sliderValueChanged {
    if (!self.dragging) return;
    double duration = [self playbackDuration];
    if (!PV2TimelineHasDuration(duration)) return;
    double target = PV2TimelineSecondsForProgress(self.slider.value, duration);
    self.dragTargetSeconds = target;
    [self updateLabelsForSeconds:target duration:duration];
    double now = PV2TimelineNow();
    // Rate-limit *requests* to ~10Hz; the queue then coalesces them so at most
    // one native seek runs at a time and the newest target always wins.
    if (PV2TimelineShouldEmitSeek(now, self.lastEmitTime, PV2TimelineSeekInterval())) {
        self.lastEmitTime = now;
        [self requestSeekSeconds:target];
    }
}

- (void)sliderTouchUp {
    if (!self.dragging) return;
    double target = self.dragTargetSeconds;
    BOOL restore = PV2TimelineDragRestoresPlayback(self.pausedByController, self.wasPlaying, [self isCurrent]);
    self.dragging = NO;
    if ([self isCurrent]) [self requestSeekSeconds:target]; // exact final target wins (pending if in flight)
    [self applyDragPlaybackOutcome:restore];
    [self refresh]; // resync from native state and restart the timer
}

// The controller does not pause during a drag, so this is a no-op today; it
// keeps the "resume only what we paused" rule explicit (and host-testable).
- (void)applyDragPlaybackOutcome:(BOOL)restore {
    if (!restore) return;
    // Intentionally no play()/setRate: rate and looping state must stay untouched.
}

- (void)cancelDrag {
    if (!self.dragging) return;
    self.dragging = NO;
    self.dragTargetSeconds = 0.0;
    self.pausedByController = NO;
    self.wasPlaying = NO;
    self.slider.highlighted = NO;
}

#pragma mark - Double-tap seek

- (BOOL)seekBySeconds:(double)seconds {
    // Double-tap seek only needs a selected video tile, not the visible panel:
    // it works while the native bars are hidden too. Bind the identity on demand
    // so the very first tap still works before any panel exists.
    [self bindSelectedIdentity];
    if (![self isEligible]) return NO;
    if (!self.browsing || !self.sessionToken || ![self.sessionToken isReadyForSeeking]) return NO;
    double duration = [self playbackDuration];
    if (!PV2TimelineHasDuration(duration)) return NO;
    double nativeCurrent = self.dragging ? self.dragTargetSeconds : [self currentSeconds];
    // Accumulate onto the newest *requested* target so rapid taps stack
    // (+5, +5, +5 => +15) even while earlier native seeks are still in flight.
    double target = PV2TimelineNextSeekTarget(nativeCurrent, _seekState.inFlight, _seekState.hasPending,
                                              _seekState.pendingTarget, _seekState.emittedTarget,
                                              seconds, duration);
    if (self.dragging) {
        self.dragTargetSeconds = target;
        self.lastEmitTime = PV2TimelineNow();
    }
    self.slider.value = (float)PV2TimelineProgress(target, duration);
    [self updateLabelsForSeconds:target duration:duration];
    [self submitSeekTarget:target];
    return YES;
}

#pragma mark - Notifications

- (void)addObservers {
    if (self.observerTokens) return;
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    self.observerTokens = [NSMutableArray array];
    __weak PV2TimelineController *weakSelf = self;
    NSArray<NSString *> *names = @[
        UIApplicationWillResignActiveNotification,
        UIApplicationDidEnterBackgroundNotification,
        UISceneWillDeactivateNotification,
        UISceneDidEnterBackgroundNotification
    ];
    for (NSString *name in names) {
        id token = [center addObserverForName:name object:nil queue:NSOperationQueue.mainQueue
                                   usingBlock:^(__unused NSNotification *note) { [weakSelf handleBackground]; }];
        if (token) [self.observerTokens addObject:token];
    }
    id token = [center addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue
                               usingBlock:^(__unused NSNotification *note) { [weakSelf refresh]; }];
    if (token) [self.observerTokens addObject:token];
}

- (void)removeObservers {
    if (!self.observerTokens) return;
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    for (id token in self.observerTokens) [center removeObserver:token];
    self.observerTokens = nil;
}

// Backgrounding must not leave a seek mid-flight or a timer running: cancel the
// drag silently (no seek) and hide.
- (void)handleBackground {
    [self cancelDrag];
    [self invalidateSeeks];
    [self stopTimer];
    self.panel.hidden = YES;
}

- (void)dealloc {
    [self.timer invalidate];
    [self removeObservers];
}

@end

@implementation PV2TimelineTicker
- (void)tick:(NSTimer *)timer {
    PV2TimelineController *controller = self.controller;
    if (!controller) { [timer invalidate]; return; }
    [controller tick:timer];
}
@end

// ---------------------------------------------------------------------------
// Integration entry points (called from Tweak.xm hooks by the main module).
// ---------------------------------------------------------------------------
static const void *PV2TimelineOwnerKey = &PV2TimelineOwnerKey;

static PV2TimelineController *PV2TimelineForOwner(UIViewController *owner, BOOL create) {
    if (!owner) return nil;
    PV2TimelineController *controller = objc_getAssociatedObject(owner, PV2TimelineOwnerKey);
    if (!controller && create) {
        controller = [PV2TimelineController new];
        controller.owner = owner;
        objc_setAssociatedObject(owner, PV2TimelineOwnerKey, controller, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return controller;
}

// Recompute eligibility/geometry and repaint. Safe to call frequently (scroll,
// chrome change, view model update); it only reaches native objects on the main
// thread and never creates controls for photos or other pages.
static void PV2TimelineRefresh(UIViewController *owner) {
    if (!PV2TimelineEnabled || !owner) return;
    if (!NSThread.isMainThread) {
        __weak UIViewController *weakOwner = owner;
        dispatch_async(dispatch_get_main_queue(), ^{ PV2TimelineRefresh(weakOwner); });
        return;
    }
    if (owner != PV2VisibleOneUp || !PV2OneUpVisible) return;
    [PV2TimelineForOwner(owner, YES) refresh];
}

// Owner is leaving: stop timers, cancel any drag, drop the panel.
static void PV2TimelineLeave(UIViewController *owner) {
    if (!owner) return;
    PV2TimelineController *controller = PV2TimelineForOwner(owner, NO);
    if (!controller) return;
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [controller teardown];
            objc_setAssociatedObject(owner, PV2TimelineOwnerKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        });
        return;
    }
    [controller teardown];
    objc_setAssociatedObject(owner, PV2TimelineOwnerKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

// Double-tap +/-5s: jump the selected video's native timeline. Returns NO when
// there is no eligible selected video or native timeline.
static BOOL PV2TimelineSeekBy(UIViewController *owner, double seconds) {
    if (!PV2TimelineEnabled || !owner || !NSThread.isMainThread) return NO;
    // The panel may never have been built (chrome hidden at first tap): bind the
    // controller on demand so the selected tile's browsing player is still
    // reachable. It only ever binds the live visible one-up.
    if (owner != PV2VisibleOneUp || !PV2OneUpVisible) return NO;
    PV2TimelineController *controller = PV2TimelineForOwner(owner, YES);
    return controller ? [controller seekBySeconds:seconds] : NO;
}
