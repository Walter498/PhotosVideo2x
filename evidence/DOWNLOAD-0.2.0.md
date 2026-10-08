# PhotosVideo2x 0.2.0 provider download trial

## Evidence on iOS17.3 DSC
- PXDisplayAssetVideoContentDeliveryStrategy has separate network/streaming BOOL setters.
- _loadMediaWithStrategyAtIndex: configures PXVideoRequestOptions including includeTimeRangeMapper and restrictToPlayableOnCurrentDevice.
- PXSetupPXVideoRequestOptionsForDeliveryQuality table: quality 0 branches to deliveryMode 1 (high quality), quality3 is streaming medium default.
- provider priority enum permitted 0..4. Priority0 maps downloadIntent2 and downloadPriority1. This is a Photos foreground-style request, not a bypass of iCloud throttling or a promise of maximum bandwidth.
- requestURLOnly chooses mediaProvider requestURLForVideo:options:resultHandler:. False chooses requestPlayerItemForVideo:options:resultHandler:.
- request progress is double, not byte count.
- PXVideoSession _handleContentLoadingResult: and _updatePlayerItem transfer playerItem, timeRange, timeRangeMapper into setPlayerItem:segmentTimeRangeOfOriginalVideo:timeRangeMapper:.
- PHAsset isInCloud is a cloudPlaceholderKind==2 check, not definitive full-resource residency.
- PHAsset originalFileSize fetches original metadata filesize; it may differ from edited/playable local output.

## Implementation
- Preserve successful 0.1.1 long-press/haptic/UI baseline; do not replace loadView.
- Add current-video top-left status panel to the same Photos window, 12pt leading / safeArea.top+52.
- Instantiate the current provider's actual subclass with same asset/mediaProvider/audioSession. Use one high-quality strategy, no streaming.
- First probe local URL with network disabled, then on user tap allow network foreground download.
- Verify file URL, nonzero size, readable file; completion path remains in Photos managed resource storage. Not a permanent Keep Original directive.
- On download complete, second network-disabled provider request prepares playerItem with native edits/composition/time mapper.
- Publish the complete native loadingResult to the existing provider inside performChanges. Existing session owns item replacement and playback intent. Check currentPlayerItem identity, then restore the just-before-switch browsing currentTime through native seek.
- Never construct a bare playerItem from URL, never install a replacement video session, never rewrite playState enum.
- Provider callbacks bound to clone/provider/tile/session identity; cancel on tile reuse/leaving/background. Download stops when leaving the current video or backgrounding.
- A pending local probe failure says iCloud / pending download, not definitive cloud membership. Estimated downloaded bytes explicitly prefixed ≈, derived from provider fraction * original metadata size, not exact network traffic / pre-existing streaming cache count.
- ABI guard skips download hooks if unsupported; existing long-press hook group remains separate.

## Validation limits
Syntax check and CI policy tests verify code/type consistency and decision rules only. Actual iCloud speed, native resource residency semantics, local item adoption/currentTime preservation, edited/slow-motion output and UI positioning require target-device tests.
No test downloads from user's iCloud or device installs are performed automatically.

## Delivery verification
Commit 8edda1d, Actions37761245541 success, artifact11542756159 archive digest71d6803b126211e585896bc8b46ced892279965134e909948a3419863bf9187f. Rate/lifecycle regression and download-file/current-callback/progress policy tests PASS. macOS arm64+arm64e compilation/link/sign PASS. Extracted actual RootHide package: no var/jb prefix, Architecture iphoneos-arm64e; both slices have LC_CODE_SIGNATURE, arm64e cpusubtype0x80000002. RootHide deb SHA25619c09acbc9fa904e8099cde20547bbf099988269e14618d50ac189ccb0ce4580.
Package not installed and no actual iCloud download occurred during validation. UI/download/publication/seek all require target device runtime testing. Photos managed local cache can be reclaimed later; this does not enforce indefinite local retention. Active video's download is cancelled on leaving/background. iCloud/pending status merges non-network probe failures; it is not a definitive cloud-only classification.

## 0.2.1 lifecycle/state correction
- Removed automatic network-disabled provider probing from tile refresh; iCloud videos stay actionable instead of waiting for a result before user input.
- Panels require the One Up current content tile, tile active/presentation-active, PHAsset video mediaType, and compatible provider. One Up refresh is triggered after its native video-player update. Adjacent preheated tiles cannot claim the global panel slot.
- Asset changes compare stable PHAsset localIdentifier in addition to provider/session/browser identity; old request is cancelled and all panel/result/progress/file-size state reset.
- Photos/photos JPEG are hidden, not shown as unsupported. One Up empty content and disappear clear all panel state.
- 0.2.0 URL-only crash remains fixed: all Photos provider calls use requestURLOnly:NO.
- Regression policy tests cover current/active/one-up/video/provider visibility, left/right asset identity reset, stale callbacks and local file verification.

## 0.2.1 lifecycle fix delivery
Commit `8e5d74f` plus tile-gating follow-up `b7bcb14`. Actions run `37773233148` success: rate/lifecycle and download-policy regression passed; arm64/arm64e compiled, signature/deb variants passed.

Fixes: no automatic local provider probe; only the current PUOneUpViewController `_currentContentTileController` is eligible. Preheated neighbor tiles (`isActive` alone is insufficient) are rejected unless current One Up + presentation-active. Empty One Up and disappearance detach all panels/cancel requests. Photos/JPEG/non-video assets hide the button. Asset state resets on provider/browser/session/localIdentifier change. Non-network local check is user-triggered; cloud placeholders start foreground request only after tap. URL-only request path remains removed. RootHide arm64e package SHA256 `df8a81db3faedf0ae578ff6c8e599495ffa195e4b15db33e466b3b2e78dd8922`, genuine signed arm64e slice `0x80000002`, no `/var/jb` prefix.

Build and mocked/static tests pass. The uploaded screenshots identify stale/preheating behavior but no 0.2.1 install/runtime verification has been done; actual UI persistence/download/currentTime still needs device retest. Device dpkg query returned no version string; installed package state is therefore unconfirmed.
