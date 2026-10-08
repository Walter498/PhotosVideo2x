# PhotosVideo2x 0.2.2

User screenshots confirmed 0.2.1 window panel could remain over Library/JPEG and labels truncated. 0.2.1 policy tests did not verify owner visibility or queued re-entry; saying these tests demonstrated the symptom resolved was incorrect.

## Changes
- OneUp viewDidAppear establishes a weak visible page owner. viewWillDisappear clears visibility, owner and current tile BEFORE native method; late tile callbacks check page owner/current tile/epoch again on main queue.
- Only OneUp _currentContentTileController chooses current tile. Active/preheated tile callbacks never set it. Selected asset comes from OneUp _currentAssetViewModel.asset, video-only PHAsset with matching provider ID.
- Panel added to the owner page view, never UIWindow; no page owner means no panel. render/ensurePanel/current callback all revalidate.
- Labels allow multiline wrapping, adaptive 48% safe-area width and content-driven height, no fixed 210pt single-line ellipsis.
- Panel visible only when both native pu_wantsNavigationBarVisible and pu_wantsToolbarVisible are true. PUOneUpBarsController _updateChromeVisibilityIfNeeded triggers sync. Hiding panel does not cancel download; leaving OneUp/backgounding still cancels per prior behavior.
- Download scope shown explicitly: pending network-enabled requests initiated by this plugin, excluding local checks and local item preparation. Current rank is submission order within that set, not Apple's global iCloud queue. Provider priority0 -> DSC downloadPriority1, labeled foreground high priority. Current-only cancellation means observed counts usually 0 or 1.
- Token/resource/epoch isolation preserved. requestURLOnly:NO; no startup network request; long-press 2x/haptic unchanged.

## Verification
Code fc43603; Actions37777606255 success. Three macOS suites PASS: Rate; Download policy+chrome+count; production extracted lifecycle helpers with mock page/tile (preheat, queued exit, photo selection, old owner exit). Actual arm64 and arm64e compiled/signed. Artifact11550586493 digest aa6a01eefa4ba510ba3550b689ddf4ef2f86464dc0e390632c03820a3beaf16e. RootHide no var/jb prefix; arm64e slice0x80000002 with LC_CODE_SIGNATURE. RootHide deb SHA2567a86b13d234b109c620f122a4158b5acf592ddd83329b85d6a4efb8ee6f279e2.

No 0.2.2 device install or real iCloud transfer/queue inspection performed. Native Photos transition timing, final pill sizing and real provider download/seek require target runtime tests. Statistics do not report system-wide background downloads.
