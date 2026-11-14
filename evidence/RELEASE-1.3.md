# PhotosVideo2x 1.3 — remove forced repeat playback

## Removed
- LoopController.h and session-associated repeat intent.
- PXVideoSession / ISWrappedAVPlayer looping setter hooks and their ABI gates.
- VideoTools' recurring enable-loop calls and enable-on-bind behavior.
- Folded/unfolded timebase mapping, loop-range endpoint correction, loop clock snapshot fields and diagnostics.
- AVKit range overrides for repeating items; loop periodic/KVO/notification observers, delayed retries and direct slider-value repairs.
- Loop controller / loop clock unit tests and their CI commands; looping integration scenarios.

## Retained
- System-owned ordinary playback timing, pause/play intent and native timeline tracking.
- Selected-item identity validation, finite seek argument checks, endpoint clamping to 0..duration, completion-once and stale callback protections.
- Fixed speed, temporary 2x gesture, double-tap seek, download handling and export.
- Existing UI geometry is retained. Fixed the dedicated transport button's missing hit-test marker found during complete syntax checking.

The tweak no longer changes Photos' repeat policy. Native Live Photo/effect behavior remains Apple's responsibility; no replacement auto-replay or forced-disable hook was added.

## Tests
- Added source guard tools/verify_single_playback.py to prevent loop code returning to production.
- Replaced actual-wrapper loop tests with ordinary 6s/0.8s endpoint and natural-end checks (stop at end, no automatic seek/restart, explicit playback remains usable).
- Kept ordinary seek arguments, exception recovery, pause/rate and sustained scrub stress tests.
- Historical evidence and Git history remain archival records, not compiled code or claims about this release.

Local syntax checks and host C++ tests passed. Cloud build/runtime results must be recorded separately after execution; real-device UI behavior is not established by syntax checks.
