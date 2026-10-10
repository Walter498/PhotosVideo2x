// Build: xcrun clang++ -std=c++17 -fobjc-arc -fblocks -Wall -Wextra -Werror \
//   -framework Foundation -framework AVFoundation -framework CoreMedia \
//   tests/LivePlaybackTests.mm -o /tmp/pv2-live-tests
// /tmp/pv2-live-tests
//
// Focused regression for the production helpers PV2LivePlaybackRead /
// PV2LivePlaybackSeek. A fake ISWrappedAVPlayer exposes the verified ABI
// (_performPlayerTransaction:v24@0:8@?16 / _playerQueue_avPlayer @16@0:8), the
// transaction block receives the wrapper itself, and the wrapper resolves a real
// AVPlayer subclass. That makes the helpers run their production code against
// native-queue semantics: currentItem is re-read every call (no cached replica
// template), the seek demands the exact live ready replica, and neither helper
// ever touches rate/play.
//
// Only this file is added; no production source is modified.

#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#import <string.h>
#import <stdlib.h>
#import "../LivePlayback.h"

static void Check(BOOL ok, const char *message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

// ---------------------------------------------------------------------------
// Real AVPlayerItem subclass: a synthetic AVMutableComposition supplies the
// asset, while status/duration are overridden so the replica can be driven
// Ready / Unknown deterministically.
// ---------------------------------------------------------------------------
@interface FakeItem : AVPlayerItem {
@public
    AVPlayerItemStatus itemStatus;
    CMTime itemDuration;
}
@end

@implementation FakeItem
- (AVPlayerItemStatus)status { return itemStatus; }
- (CMTime)duration { return itemDuration; }
@end

// ---------------------------------------------------------------------------
// AVPlayer subclass: exposes a swappable live replica + clock + status, and
// captures the four-argument seek so the helper's native call can be inspected
// and completed later. setRate:/play are counted to prove they are never used.
// ---------------------------------------------------------------------------
@interface FakePlayer : AVPlayer {
@public
    AVPlayerItem *liveItem;
    CMTime liveTime;
    AVPlayerStatus playerStatus;
    float rateValue;
    NSUInteger setRateCalls;
    NSUInteger playCalls;
    NSUInteger seekCalls;
    CMTime seekTarget;
    CMTime seekToleranceBefore;
    CMTime seekToleranceAfter;
    void (^seekCompletion)(BOOL finished);
}
@end

@implementation FakePlayer
- (AVPlayerItem *)currentItem { return liveItem; }
- (CMTime)currentTime { return liveTime; }
- (AVPlayerStatus)status { return playerStatus; }
- (float)rate { return rateValue; }
- (void)setRate:(float)value { setRateCalls += 1; rateValue = value; }
- (void)play { playCalls += 1; }
- (void)seekToTime:(CMTime)time
   toleranceBefore:(CMTime)toleranceBefore
    toleranceAfter:(CMTime)toleranceAfter
  completionHandler:(void (^)(BOOL))completionHandler {
    seekCalls += 1;
    seekTarget = time;
    seekToleranceBefore = toleranceBefore;
    seekToleranceAfter = toleranceAfter;
    seekCompletion = completionHandler;
}
@end

// ---------------------------------------------------------------------------
// Fake ISWrappedAVPlayer. The two selectors carry the exact verified encodings,
// the transaction hands the block `self`, and the player getter succeeds.
// ---------------------------------------------------------------------------
@interface FakeWrapper : NSObject {
@public
    FakePlayer *player;
    NSUInteger transactions;
    NSUInteger playerQueries;
}
- (void)_performPlayerTransaction:(void (^)(id wrapper))transaction;
- (AVPlayer *)_playerQueue_avPlayer;
@end

@implementation FakeWrapper
- (void)_performPlayerTransaction:(void (^)(id))transaction {
    transactions += 1;
    transaction(self);
}
- (AVPlayer *)_playerQueue_avPlayer {
    playerQueries += 1;
    return player;
}
@end

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// A real AVPlayerItem backed by a synthetic composition; status/duration come
// from the subclass override.
static FakeItem *PV2MakeItem(CMTime duration, AVPlayerItemStatus status) {
    AVMutableComposition *composition = [AVMutableComposition composition];
    [composition insertEmptyTimeRange:CMTimeRangeMake(kCMTimeZero, duration)];
    FakeItem *item = [[FakeItem alloc] initWithAsset:composition];
    item->itemStatus = status;
    item->itemDuration = duration;
    return item;
}

// The read helper delivers its snapshot asynchronously on the main queue, so
// drive that queue while waiting for the completion.
static BOOL readDone;
static PV2LivePlaybackSnapshot *readSnapshot;
static void PV2PumpRead(void) {
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
    while (!readDone && deadline.timeIntervalSinceNow > 0) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
    }
}
static PV2LivePlaybackSnapshot *PV2Read(FakeWrapper *wrapper) {
    readDone = NO;
    readSnapshot = nil;
    PV2LivePlaybackRead(wrapper, ^(PV2LivePlaybackSnapshot *snapshot) {
        readSnapshot = snapshot;
        readDone = YES;
    });
    PV2PumpRead();
    Check(readDone, "read completion delivered on the main queue");
    return readSnapshot;
}

int main(void) {
    @autoreleasepool {
        Check(PV2LivePlaybackABI([FakeWrapper class]),
              "fake wrapper exposes the verified ABI type encodings");

        FakeWrapper *wrapper = [FakeWrapper new];
        FakePlayer *player = [FakePlayer new];
        wrapper->player = player;
        player->playerStatus = AVPlayerStatusReadyToPlay;
        player->rateValue = 0.0f;
        NSUInteger rateCalls = player->setRateCalls;
        NSUInteger plays = player->playCalls;

        FakeItem *replicaA = PV2MakeItem(CMTimeMake(4, 1), AVPlayerItemStatusReadyToPlay);
        FakeItem *replicaB = PV2MakeItem(CMTimeMake(9, 1), AVPlayerItemStatusReadyToPlay);

        // --- snapshot reads the live currentItem, not a cached template ------
        player->liveItem = replicaA;
        player->liveTime = CMTimeMake(11, 1);
        NSUInteger queriesBefore = wrapper->playerQueries;
        PV2LivePlaybackSnapshot *snapA = PV2Read(wrapper);
        Check(snapA != nil, "A: snapshot produced");
        Check(snapA.item == replicaA, "A: snapshot.item is the live currentItem replica A");
        Check(CMTimeCompare(snapA.time, CMTimeMake(11, 1)) == 0, "A: snapshot.time mirrors live clock");
        Check(CMTimeCompare(snapA.duration, replicaA->itemDuration) == 0,
              "A: duration read from the live replica");
        Check(snapA.ready, "A: ready only when player and item are both ready");
        Check(wrapper->transactions == 1, "A: exactly one native player transaction");
        Check(wrapper->playerQueries == queriesBefore + 1,
              "A: native player resolved through the wrapper passed to the block");

        // Swap the live replica + clock; the next read must reflect the swap.
        player->liveItem = replicaB;
        player->liveTime = CMTimeMake(120, 1);
        PV2LivePlaybackSnapshot *snapB = PV2Read(wrapper);
        Check(snapB.item == replicaB, "B: snapshot re-reads the new live replica (no cached template)");
        Check(snapB.item != replicaA, "B: stale replica A is no longer reported");
        Check(CMTimeCompare(snapB.time, CMTimeMake(120, 1)) == 0, "B: time follows the new clock");
        Check(CMTimeCompare(snapB.duration, replicaB->itemDuration) == 0, "B: duration follows the new replica");
        Check(CMTimeCompare(snapB.duration, replicaA->itemDuration) != 0,
              "B: duration is not a stale replica template");

        // ready=NO whenever either side is not ready.
        player->liveItem = replicaA;
        player->liveTime = CMTimeMake(1, 1);
        replicaA->itemStatus = AVPlayerItemStatusUnknown;
        Check(!PV2Read(wrapper).ready, "non-ready live item yields ready=NO");
        replicaA->itemStatus = AVPlayerItemStatusReadyToPlay;
        player->playerStatus = AVPlayerStatusFailed;
        Check(!PV2Read(wrapper).ready, "failed player yields ready=NO");
        player->playerStatus = AVPlayerStatusReadyToPlay;

        // Missing native player yields an empty snapshot, never nil.
        FakeWrapper *empty = [FakeWrapper new];
        PV2LivePlaybackSnapshot *snapNil = PV2Read(empty);
        Check(snapNil != nil && snapNil.item == nil && !snapNil.ready,
              "missing native player yields an empty snapshot");
        Check(snapNil.time.value == 0 && snapNil.time.timescale == 0 && snapNil.time.flags == 0,
              "empty snapshot leaves time unset");

        // --- seek: the exact live ready replica is required -------------------
        NSUInteger seeks = player->seekCalls;
        player->playerStatus = AVPlayerStatusReadyToPlay;
        replicaA->itemStatus = AVPlayerItemStatusReadyToPlay;
        player->liveItem = replicaA;

        CMTime target = CMTimeMake(33, 1);
        CMTime tolerance = CMTimeMake(1, 10);

        FakeItem *staleItem = PV2MakeItem(CMTimeMake(2, 1), AVPlayerItemStatusReadyToPlay);
        __block BOOL called = NO;
        __block BOOL result = YES;
        PV2LivePlaybackSeek(wrapper, staleItem, target, tolerance, ^(BOOL finished) {
            result = finished;
            called = YES;
        });
        Check(called && !result, "seek rejects an expected item that is not the live replica");
        Check(player->seekCalls == seeks, "stale expected item produces no native seek");

        // --- seek: live ready replica issues exactly one native seek ----------
        seeks = player->seekCalls;
        called = NO;
        result = NO;
        PV2LivePlaybackSeek(wrapper, replicaA, target, tolerance, ^(BOOL finished) {
            result = finished;
            called = YES;
        });
        Check(!called, "seek completion is withheld until the native player finishes");
        Check(player->seekCalls == seeks + 1, "seek issued exactly once for the live replica");
        Check(CMTimeCompare(player->seekTarget, target) == 0, "seek forwards the requested time");
        Check(CMTimeCompare(player->seekToleranceBefore, tolerance) == 0, "seek forwards toleranceBefore");
        Check(CMTimeCompare(player->seekToleranceAfter, tolerance) == 0, "seek forwards toleranceAfter");
        void (^nativeSeekCompletion)(BOOL) = player->seekCompletion;
        Check(nativeSeekCompletion != NULL, "native seek completion captured");
        nativeSeekCompletion(YES);
        Check(called && result, "native seek completion is forwarded to the caller");

        // --- seek: non-ready player / item release completion NO --------------
        seeks = player->seekCalls;
        player->playerStatus = AVPlayerStatusFailed;
        called = NO;
        result = YES;
        PV2LivePlaybackSeek(wrapper, replicaA, target, tolerance, ^(BOOL finished) {
            result = finished;
            called = YES;
        });
        Check(called && !result, "failed player releases completion NO");
        Check(player->seekCalls == seeks, "failed player issues no seek");

        player->playerStatus = AVPlayerStatusReadyToPlay;
        replicaA->itemStatus = AVPlayerItemStatusUnknown;
        called = NO;
        result = YES;
        PV2LivePlaybackSeek(wrapper, replicaA, target, tolerance, ^(BOOL finished) {
            result = finished;
            called = YES;
        });
        Check(called && !result, "non-ready item releases completion NO");
        Check(player->seekCalls == seeks, "non-ready item issues no seek");
        replicaA->itemStatus = AVPlayerItemStatusReadyToPlay;

        // --- neither helper ever touches playback rate / play -----------------
        Check(player->setRateCalls == rateCalls, "read/seek never call setRate:");
        Check(player->playCalls == plays, "read/seek never call play");
        Check(player->rateValue == 0.0f, "paused rate is preserved across read and seek");

        // --- unsupported / nil wrappers are refused ---------------------------
        called = NO;
        result = YES;
        PV2LivePlaybackSeek([NSObject new], replicaA, target, tolerance, ^(BOOL finished) {
            result = finished;
            called = YES;
        });
        Check(called && !result, "unsupported wrapper rejected without a transaction");

        __block id unsupportedSnapshot = @"sentinel";
        called = NO;
        PV2LivePlaybackRead([NSObject new], ^(PV2LivePlaybackSnapshot *snapshot) {
            unsupportedSnapshot = snapshot;
            called = YES;
        });
        Check(called && unsupportedSnapshot == nil, "unsupported wrapper yields a nil snapshot");

        __block id nilSnapshot = @"sentinel";
        called = NO;
        PV2LivePlaybackRead(nil, ^(PV2LivePlaybackSnapshot *snapshot) {
            nilSnapshot = snapshot;
            called = YES;
        });
        Check(called && nilSnapshot == nil, "nil wrapper yields a nil snapshot");

        called = NO;
        result = YES;
        PV2LivePlaybackSeek(nil, replicaA, target, tolerance, ^(BOOL finished) {
            result = finished;
            called = YES;
        });
        Check(called && !result, "nil wrapper releases completion NO");

        puts("PASS: live snapshot reads live replica, seek requires the exact ready replica, "
             "no rate/play side effects, nonready and unsupported wrappers refusal");
    }
    return 0;
}
