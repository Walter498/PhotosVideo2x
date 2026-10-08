# PhotosVideo2x 0.1.0 crash investigation

Device: iPhone16,2, iOS 17.3 (21D50).

Crash at app launch +2.282s, EXC_CRASH SIGABRT, main thread:
`objc forwarding -> UIView _addSubview:positioned:relativeTo: -> PUTileViewController addToTilingView: +88`.
The crash log lists PhotosVideo2x.dylib arm64e UUID DFF1FB57-0A74-3D4B-909A-9F6C8E9310B1. It omits the specific unrecognized selector / exception reason, so the log alone is not an exact exception message.

Confirmed source defect: 0.1.0 declares PUVideoTileViewController as UIViewController and hooks `-(void)loadView`. Actual Photos tile hierarchy derives from PUTileViewController/PUTileController. DSC method metadata:

- PUTileViewController loadView: `@16@0:8`, IMP 0x1af652b58
- PUVideoTileViewController loadView: `@16@0:8`, IMP 0x1af69daf8
- ISWrappedAVPlayer rate: `f16@0:8`, IMP 0x1d57f82fc

Caller evidence: PUTileViewController view at 0x1af652a40 calls loadView, retains returned x0 and at 0x1af652a54 sends `_setLoadedView:` with it. The 0.1.0 hook discarded the object return and called a void helper afterwards, corrupting this contract. addToTilingView at 0x1af652c54 obtains `view`, and at 0x1af652c68 passes it to `addSubview:`; crash reports +88, the next instruction address 0x1af652c6c.

Native touch evidence: PUVideoTileViewController loadView at 0x1af69db4c sends `setUserInteractionEnabled:NO` to its root view. addToTilingView at 0x1af652c70 obtains tile gestureRecognizers, then at 0x1af652cd0 registers each recognizer on PUTilingView. Attaching a new recognizer to the non-interactive inner view bypasses this Photos contract.

Pause evidence: ISWrappedAVPlayer pause 0x1d57f703c queues a transaction; its block obtains `_playerQueue_avPlayer` and sends `pause` directly. It does not route through ISWrappedAVPlayer setRate:. Therefore wrapper pause needs its own stop handler.

0.1.1 removes the loadView hook entirely, uses the native tile gestureRecognizers registration channel, and addresses token lifecycle/stop paths. A successful CI build is not a device reproduction. Runtime startup and gestures still require device verification.

## 0.1.1 delivery verification

Git commit: `abaf37a`. GitHub Actions run: `37749429873`, success. Artifact ID `11537013659`, archive digest `7343ad23ef4b64ae9dc41a8899ecc44dcff16210dc1b6072512ca1ad868b40fe`.

Tests passed on macOS: initial 2x, external 1x overwrite, separate pause that bypasses setRate, stop then release, stale owner/token isolation, background cleanup, concurrent set/end completion. These are mock-based rate regression, not real Photos playback tests.

Actual RootHide deb: Architecture iphoneos-arm64e, no var/jb prefix. Fat dylib contains arm64 and arm64e subtype `0x80000002`; both slices have nonempty bounded LC_CODE_SIGNATURE.

RootHide package SHA256: `7178efec012c3b941aa7a528c04867842c98a753192fde7b2014352f5bdea53d`.

Logs now write to Photos' own Library/Caches/PhotosVideo2x.log, plus NSLog prefix [PhotosVideo2x]. Startup checks native method encodings before installing hooks. No device installation/restart was performed during this fix.

Limit: native setter/pause calls run outside the token-state lock to avoid synchronous ivarQueue deadlocks. The concurrency test demonstrates completion, not arbitrary inter-thread native write ordering. Real video types, gesture arbitration and restart remain to be verified on device.
