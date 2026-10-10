# PhotosVideo2x 0.4.0

## This update

- Fixed speeds: 0.5 / 1 / 1.25 / 1.5 / 2 / 3 / 4 / 8x. The choice is persisted. Choosing a speed while paused does not start playback.
- One unified top speed capsule shows the selected non-1x speed. A side long press temporarily shows and plays at absolute 2x; release returns to the fixed speed. At 1x with no hold, the capsule is hidden.
- Double tapping a photo no longer runs double-tap zoom. Pinching is left intact. The native `PUDoubleTapZoomController` gesture is reused, so normal single tapping, panning, bars/control/accessory exclusion, and editing exclusions retain Photos' delegate behavior.
- Double tap a video's left/right half to seek backward/forward 5s; clamp to the native timeline range. Repeated requests accumulate from the latest requested target, including when chrome is hidden. Brief -5/+5 feedback.
- New integrated PiP-inspired timeline: compact 44pt dark material rounded panel; white/gray fine slider; 12pt thumb (18pt dragging); monospaced elapsed and negative remaining labels. Panel bottom is 4pt above the real navigation toolbar's top, after coordinate conversion; safe-area fallback for an absent toolbar. Default on the selected video, visible only when BOTH native top and bottom bars are visible, exactly matching the download/local-status gate.
- Visible refresh ~30Hz; dragging requests at most ~10Hz, with a serialized single-flight latest-target-wins queue and exact final seek. Old asset/owner/tile/browser/session callbacks cannot update a new video's controls. Leave/background cleans timers and pending requests.
- Native `PUBrowsingVideoPlayer` currentTime/duration and seek APIs preserve Photos' timeline mapping. Raw PHAsset duration is not used to invent an edited/slow-motion timeline. Slider is disabled until native duration and seeking readiness exist. Timeline never writes play/pause/rate; native seek owns intent.

## Why 0.3.1 did not reliably loop

0.3.1 successfully avoided the asserting ordinary `PXVideoSession performChanges:` API, but setting the presentation state only once did not cover the native reset path:

- `PUBrowsingVideoPlayer _updateVideoSession` (`0x1af34fd30`) sends a presentation change for context 1. Its block (`0x1af35009c`) calls `setLoopingEnabled:` on the callback's presentation state at `0x1af350114`, with the asset's `canPlayLoopingVideo` value. Ordinary videos normally yield NO.
- `PXVideoSession _updateFromCurrentPresentationState` (`0x1ac153364`) reapplies the chosen presentation state's loop setting through `PXVideoSession setLoopingEnabled:` (`0x1ac15359c`).
- Previously tools marked the session as initialized before knowing that its presenter state callback had executed. If setup was too early, no later attempt happened.

0.4.0 marks only the current selected video's session. The session's existing loop setter has its requested value rewritten to YES while marked; unmarked sessions remain native. No new plain session transaction is used. Initialization uses the existing context/presenter transaction; its success reflects actual callback execution, and missing setup is retried. No desiredPlayState write and no forced play after pause.

Key ABI, iOS 17.3 DSC:
```
PXVideoSession setLoopingEnabled: v20@0:8B16
PXVideoSession isReadyForSeeking B16@0:8
PUBrowsingVideoPlayer duration/currentTime {?=qiIq}16@0:8
PUBrowsingVideoPlayer seekToTime:toleranceBefore:toleranceAfter:completionHandler:
  v96@0:8{?=qiIq}16{?=qiIq}40{?=qiIq}64@?88
PUDoubleTapZoomController _handleDoubleTapGestureRecognizer: v24@0:8@16
PUOneUpViewController _doubleTapZoomController @16@0:8
```

## Reference plugin analysis

The supplied `系统相册视频进度条-0.0.5-iphoneos-arm64.deb` was preserved and extracted separately (SHA256 `667d5cbd…16436`; exact hash in separate report). It hooks `AVPlayerLayer setPlayer:` and implements `MSHProgressManager` / `MSHProgressSlider`, with a display-link, enlarged hit region and a single-flight scrub queue. It toggles its overlay using separate screen-tap logic rather than reading Photos chrome visibility.

This update does not ship or copy the reference dylib. It implements a new selected-OneUp-scoped timeline and reuses native chrome and mapped browsing-player semantics. Do not leave the reference plugin enabled alongside this update: two independent timelines could overlap or seek concurrently.

## High-speed export test correction

The first additional high-speed test exported a 2-second audio fixture without the production audioMix. Its 3x/4x/8x AAC results differed by roughly 0.08s from the ideal very-short timeline, while all video outputs passed. This was not silently ignored: the final test uses the same explicit TimeDomain audioMix as production and a 16-second audio fixture. All rates pass the 0.07s duration tolerance. Short AAC clips still have encoder/algorithm edge-padding limits; these tests do not promise sample-exact duration for every subsecond output. The SDK AVAudioProcessingSettings.h documents both TimeDomain and Spectral rate support from 1/32 to 32, so 8x is not outside their declared range.

## Verification boundary

Local C++ timeline/gesture/export-core tests and iOS arm64e strict syntax check have passed. Remote suite includes rate and badge at 3/4/8x, download policy, extracted production lifecycle, export core, actual synthetic video/audio export, loop transaction/late-context/selected-session override, timeline geometry/seek state, and double-tap geometry.

Actions run `38051123385` at commit `254c5ca` succeeded with all nine regression groups, including extracted production timeline callback methods and real 3x/4x/8x media export. RootHide package SHA256: `1ae8e8eac96e044b45e4c806928b602df44698ae4785eb58ab5b8b6513226847`. Genuine signed arm64e slice (`0x80000002`), no `/var/jb` prefix. Shared and attachments copies match. No device package installation or Photos restart has been performed. Real-device acceptance is still required for native looping, mapped seeks, toolbar placement, rotation and exact PiP-like feel. Selecting 8x does not guarantee audible intelligible audio or full video frames for every codec/device.
