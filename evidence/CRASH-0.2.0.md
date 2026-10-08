# PhotosVideo2x 0.2.0 crash fix

The supplied crash is a startup crash on iOS 17.3. Thread 9:
`PXDisplayAssetVideoContentProviderRequest _loadMediaWithStrategyAtIndex:` -> `PUWrappingPXMediaProvider requestURLForVideo:options:resultHandler:` -> Objective-C exception -> SIGABRT.

The trigger was the automatic local probe using `requestURLOnly:YES`. On this device the Photos wrapper raises inside `requestURLForVideo:`. The UI stack and long-press code are not on the crashing thread. The utility thread reading `originalFileSize` is separate and is not the abort stack.

Fix: every Photos provider request uses `requestURLOnly:NO`, so Photos returns its supported player-item/loading-result path with native timeRange and timeRangeMapper. Local status still requires a completed result, readable file URL and nonzero file size. The two-stage flow remains provider-first: local/network request, then a network-disabled provider request to prepare the native player item before publishing its loading result to the existing provider.
