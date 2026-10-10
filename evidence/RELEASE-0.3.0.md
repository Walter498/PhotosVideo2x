# PhotosVideo2x 0.3.0

## User-facing features
- Adds a bottom-toolbar speed button next to the details/info control. It only inserts into the native One Up toolbar collection and keeps the original Photos buttons.
- Short tap presents 0.5x / 1x / 1.25x / 1.5x / 2x. Choice is stored and applied to the current `ISWrappedAVPlayer`; side long-press remains an absolute temporary 2x and releases back to the selected fixed speed.
- Long press on the speed button offers:
  - 导出当前倍速视频
  - 导出正常倍速音频
  - 导出当前倍速音频
- Every video session enables the native `PXVideoSession` looping flag through `performChanges:` and `setLoopingEnabled:YES`.
- Export captures the current AVPlayerItem, speed, edit objects and owner before starting. It uses the item's AVAsset, retains video transforms/audio policy, scales existing edit segments, refuses unsupported custom compositor cases, exports to a UUID temp directory, shows progress/cancel UI, presents system share after success and removes temp output on failure/cancel/share completion. The export is a real re-encode, not a passthrough copy.
- Audio export uses M4A. A video with no audio can export as silent video; audio-only modes require an audio track.

## Evidence and validation
- DSC evidence: `ISWrappedAVPlayer` loop methods `setLoopingEnabled:`/`setLoopingEnabled:withTemplateItem:`; `PXVideoSession _updatePlayerItemInPlayer` checks session loop state and applies it to the new item; native toolbar collection uses `PUBarButtonItemCollection orderedBarButtonsItemsForIdentifiers:` and `PUOneUpBarsController _toolbarButtonItemCollection`.
- Actions run `38045134591` success.
- PASS suites: rate/pause/fixed temporary 2x; download/panel policy; production lifecycle/preheat/queued exit/owner isolation; export core; synthetic AVFoundation media export (video+audio, audio normal/current rate, transform, edit segments, audio mix and unsupported instruction handling).
- RootHide package: `/var/minis/attachments/PhotosVideo2x/PhotosVideo2x_0.3.0_RootHide_iphoneos-arm64e.deb`, SHA256 `bb7e01a00a3e36647c7de11f58dab8d00b9b3c7b7b2f7af6616ca637b0528e14`.
- Actual package contains signed arm64 and arm64e (`0x80000002`) slices and no `/var/jb` prefix.

## Device validation still required
The package has not been installed automatically. Test toolbar insertion, short tap menu, long-press export menu, loop behavior across next/previous video, edited/slow-motion exports, iCloud assets, orientation, audio pitch, cancellation, and share-sheet completion on the target iOS 17.3 device.
