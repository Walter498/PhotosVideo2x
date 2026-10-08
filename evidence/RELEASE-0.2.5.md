# PhotosVideo2x 0.2.5

Reported with 0.2.4: the panel still showed 本地 before a tap and only switched to iCloud after tapping. Screen recording confirmed the sequence 本地 -> (tap) -> iCloud · 点击下载.

Root cause: two unreliable sources.
1. 0.2.3's placeholder heuristic (isInCloud / cloudPlaceholderKind) does not identify iCloud-optimized videos on this device.
2. 0.2.4's replacement, `-[PHAssetResource isLocallyAvailable]` (0x19f04ddfc, reads one stored byte), returned true for assets that are not fully downloaded, and the earlier `provider loadingResult` shortcut could also accept a streaming cache file.

Fix: do what Photos itself does. `-[PHResourceLocalAvailabilityRequest resourceAvailabilityForOptions:resourceInfo:]` calls `_fetchResourcesWithOptions:networkAccessAllowed:handler:` with networkAccessAllowed=0 and is the same check PhotosUICore's `PXPhotoKitAssetLocalAvailabilityHelper` performs. The tweak now performs one automatic, network-disabled provider request when a video panel is first shown:
- readable local file URL + nonzero size -> 本地 with the cloud-upload icon;
- otherwise -> iCloud · 点击下载, and only a tap starts the network download.

The check runs once per asset (autoChecked), is skipped when a request is already pending, and its result is applied only after re-checking asset identity, provider identity and page ownership. The previous PHAssetResource flag path and the provider loadingResult shortcut were removed.

Unchanged: counters removed, multiline panel, panel only while native top/bottom bars are visible, panel owned by the visible One Up page, no user-visible 检查 step, URL-only request path unused, 2x long-press and haptics.

Actions 37786338302 success; rate, download-policy and production-lifecycle suites PASS. arm64/arm64e built and signed; arm64e slice 0x80000002; RootHide layout without /var/jb. RootHide deb SHA256 3e4c46af9d2cd3c83ebfc9b333271dd9a44e772b363d6b7435ba5de17017fccd.

Limits: not installed on the device in this step. The automatic check reuses the provider request path, so device testing is required to confirm it resolves quickly for iCloud assets and does not delay the first frame; edited/slow-motion and partially-downloaded assets still need verification.
