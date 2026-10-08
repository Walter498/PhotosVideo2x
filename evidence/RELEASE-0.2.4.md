# PhotosVideo2x 0.2.4

Problem reported with 0.2.3: every video displayed 本地, and only after one tap did it fall back to iCloud and allow download. Cause: the previous build treated "not a cloud placeholder" (PHAsset isInCloud / cloudPlaceholderKind==2) as proof of local availability. On this device iCloud-optimized videos do not satisfy that placeholder check, so the state was wrong.

Fix: determine local availability from PhotoKit's resource flags instead of the placeholder heuristic.
- PhotoKit binary evidence: `-[PHAssetResource isLocallyAvailable]` at 0x19f04ddfc reads the resource's stored local-availability byte, and `+[PHAssetResource assetResourcesForAsset:]` enumerates resources. Photos itself uses this family (`PXPhotoKitAssetLocalAvailabilityHelper`, `PUPhotosGridDownloadHelpContext` reference `PHResourceLocalAvailabilityRequest`).
- Video resource types considered: 2 (video), 6 (full-size video), 12 (adjustment base video).
- Result is tri-state: known-local, known-not-local, or unknown (no video resource / flag unavailable). Only known-local shows 本地. Unknown shows the iCloud download entry rather than claiming local.
- Detection runs off the main thread with the original-size lookup, and the panel re-checks asset identity, provider identity and page ownership before applying the result.
- Tap behavior: local -> adopt existing result or prepare local playback with network disabled; not local/unknown -> start the network download.

Unchanged: counters removed, multiline panel, panel follows native top/bottom bar visibility, panel owned by the visible One Up page, no auto network probe, URL-only request path unused, 2x long-press and haptics.

Actions 37784544920 success; three macOS suites PASS (rate; file/callback/one-up/chrome policy; production lifecycle helpers). arm64/arm64e built and signed; genuine arm64e slice 0x80000002; RootHide layout without /var/jb. RootHide deb SHA256 94285c7b69696482139b6882210a031ef6214005d5f7550c494d88d713b77773.

Limits: not installed on device in this step. The resource flag is the same signal Photos uses, but actual behaviour (including edited/slow-motion assets and assets whose resources are partially present) still needs device verification.
