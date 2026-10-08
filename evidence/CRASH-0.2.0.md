# PhotosVideo2x 0.2.0 crash fix

The supplied crash is a startup crash on iOS 17.3. Thread 9:
`PXDisplayAssetVideoContentProviderRequest _loadMediaWithStrategyAtIndex:` -> `PUWrappingPXMediaProvider requestURLForVideo:options:resultHandler:` -> Objective-C exception -> SIGABRT.

The trigger was the automatic local probe using `requestURLOnly:YES`. On this device the Photos wrapper raises inside `requestURLForVideo:`. The UI stack and long-press code are not on the crashing thread. The utility thread reading `originalFileSize` is separate and is not the abort stack.

Fix: every Photos provider request uses `requestURLOnly:NO`, so Photos returns its supported player-item/loading-result path with native timeRange and timeRangeMapper. Local status still requires a completed result, readable file URL and nonzero file size. The two-stage flow remains provider-first: local/network request, then a network-disabled provider request to prepare the native player item before publishing its loading result to the existing provider.

## Fix verification

Fix commit: `9534b0f`. GitHub Actions run `37763476804` completed successfully with rate regression, download policy regression, arm64/arm64e build and signature checks. The new RootHide package has a genuine signed arm64e slice (`0x80000002`). SHA256: `b1d42b823863cc6112e38932446d06033c0e6eb630d17786c6eb7f97375616b5`.

The old 0.2.0 package from commit `8edda1d` should be removed before installing the fix. No target device installation was performed automatically.
