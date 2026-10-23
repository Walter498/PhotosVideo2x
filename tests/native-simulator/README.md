# Native timeline simulator integration

Run on **macOS with full Xcode and an installed iOS Simulator device/runtime**:

```sh
python3 tools/run_native_simulator.py --timeout 300 \
  --output "$RUNNER_TEMP/pv2-native-simulator"
```

Select a particular installed runtime or existing simulator explicitly when needed:

```sh
python3 tools/run_native_simulator.py --runtime iOS-18-5 --device iPhone --timeout 300
python3 tools/run_native_simulator.py --udid SIMULATOR-UUID --timeout 300
```

The default selection is the newest matching available iOS runtime and an existing
available iPhone; a booted device wins ties. `--udid` is validated against simctl's
iOS simulator inventory. This script never addresses a connected physical device,
creates/erases/deletes a simulator, edits source, or submits a build. It boots the
selected existing simulator if needed, installs a dedicated test app, then
terminates only that app on completion. It leaves the simulator booted.

`--timeout` is the total build/boot/test deadline (300 seconds by default,
adjustable); each subprocess is bounded by the remaining deadline. On failure,
three diagnostic commands may take up to 10 seconds each, followed by at most
5 seconds to terminate the test app. Result polling is bounded. There is no
third-party Python dependency, CocoaPods, Xcode project, XCTest runner, or ffmpeg
requirement. The host script writes a manual Info.plist, invokes `xcrun clang++`
against the simulator SDK, links UIKit/Foundation/AVFoundation/AVKit/CoreMedia/
CoreVideo/QuartzCore, and ad-hoc signs the `.app`.

`NativeSimulator.mm` imports **the same production `NativeTimeline.h`** into one
ARC Objective-C++ translation unit. It does not reproduce, subclass, replace,
swizzle, or mock any native AVKit class. Missing system classes, changed runtime
signatures, failureReason/readiness failures, and missing native registered
valueChanged actions are hard failures. No `_timelineView`, `_attemptSetup`,
objc_direct, or guessed selector invocation is used. Native owner discovery uses
`host.childViewControllers.firstObject`; timeline/slider access uses the declared
production contract. Any production header compile dependency or ABI mismatch
fails the build/test instead of supplying a substitute.

The app creates a real visible window/root, generates a local six-second H264
video using AVAssetWriter (180 frames at 30 fps), and uses real AVPlayer items.
It verifies:

- Production player identity, actual AVPlayerController and chromeless owner,
  actual AVMobileChromelessTimelineView and fluid/normal native slider.
- optimizeForPerformance=NO, includedControls=1, a visible interactive slider,
  native owner/delegate/registered valueChanged callbacks, and absence of extra
  visible controls or accessibility elements outside the timeline subtree.
- Real playback time and native slider progress advance without synthetic ticks.
- Paused and playing scrubs traverse native delegate begin/end tracking callbacks
  and UIControl valueChanged; seek reaches approximately 60% of the actual item.
  Paused intent stays paused; playing intent resumes. Delegate identity and
  native tracking method IMPs remain unchanged.
- Rotation-shaped root frame resizing, slider containment, and active trait
  override propagation. This is a root layout/traits integration check, not an
  assertion that a hardware orientation event was synthesized.
- Idempotent invalidate detaches controls, releases the native owner/timeline/
  slider, and leaves the externally owned player playing and advancing.
- Replacement by a newly initialized host bound to a distinct AVPlayer/item;
  scrubbing it leaves the previous player's time unchanged. The public contract
  has no replacement setter, so the test does not invent one.

The runner screenshots the actual simulator while the timeline is visible using
a Documents handshake, then acknowledges the screenshot so the test can proceed.
Success requires that screenshot. The app commits completed JSON/text to its
Documents directory; the runner obtains the directory with simctl and exports:

- `result.json`, `result.txt`: native assertions, actual iOS version, visible view
  classes, native registered actions, explicit programmatic-input disclosure.
- `timeline-visible.png`: actual simulator screenshot.
- `runner-result.json`, `selection.json`, `commands.log`, app stdout/stderr logs.
- On failure: `failure.png`, `simulator.log`, best-effort simctl diagnose archive,
  matching host DiagnosticReports crash files, and `runner-error.txt`.

The process exits 0 only after a completed passing app result plus screenshot;
assertion/build/launch/timeouts exit 1. Previous result/handshake files are removed
before launch to prevent stale success. Private classes may differ or be absent
on simulator runtimes, including Intel versus Apple Silicon ABI differences;
those are intentionally strict failures. Runtime selection and app-reported iOS
versions are recorded rather than claiming any fixed supported simulator OS.

This covers the **native AVKit host only**. It does not exercise Photos private
browsing/video-session wrappers, edited/slow-motion mapping, Photos selection or
chrome gates. Programmatic UIControl actions and verified native owner callbacks
do not prove physical touch routing, gesture recognition, real drag tracking,
or interrupted-drag cancellation; those and the Photos integration still need
true-device testing. The Linux/iSH authoring environment cannot compile or run
the Apple simulator app. No native runtime pass is claimed without macOS output.
