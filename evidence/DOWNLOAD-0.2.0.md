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
