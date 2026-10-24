#pragma once
#import "PhotosChromePolicy.h"

// Policy fixture only. It does not claim full Photos one-up UI coverage.
@interface PV2ChromeBarsFixture : NSObject
@property(nonatomic) BOOL allowShowingPlayPauseButton;
@property(nonatomic) BOOL shouldShowScrubber;
@property(nonatomic) NSUInteger updateCount;
@property(nonatomic) NSInteger playPauseState;
@property(nonatomic) NSUInteger toggles;
@end
@implementation PV2ChromeBarsFixture
- (void)setAllowShowingPlayPauseButton:(BOOL)value { _allowShowingPlayPauseButton=PV2PhotosChromeRequested(self,YES,value); }
- (void)setShouldShowScrubber:(BOOL)value { _shouldShowScrubber=PV2PhotosChromeRequested(self,NO,value); }
- (void)updateBars { _updateCount++; }
- (void)togglePlayback { _toggles++; _playPauseState=_playPauseState==1 ? 2 : 1; }
@end
@interface PV2ChromeOwnerFixture : NSObject
@property(nonatomic,strong) PV2ChromeBarsFixture *bars;
@end
@implementation PV2ChromeOwnerFixture
- (id)_barsController { return _bars; }
@end
static NSDictionary *PV2RunChromePolicyChecks(void) {
    __block NSUInteger count=0;
    void (^check)(BOOL,NSString*)=^(BOOL ok,NSString *name) {
        count++; if (!ok) @throw [NSException exceptionWithName:@"ChromePolicyFailure" reason:name userInfo:nil];
    };
    PV2ChromeOwnerFixture *owner=[PV2ChromeOwnerFixture new];
    PV2ChromeBarsFixture *bars=[PV2ChromeBarsFixture new];owner.bars=bars;
    bars.allowShowingPlayPauseButton=YES;bars.shouldShowScrubber=YES;bars.playPauseState=1;
    PV2PhotosChromeApply(owner,YES);
    check(!bars.allowShowingPlayPauseButton && !bars.shouldShowScrubber,@"hide only redundant original controls");
    check(bars.playPauseState==1,@"hiding does not change authoritative playback state");
    NSUInteger updates=bars.updateCount;PV2PhotosChromeApply(owner,YES);
    check(bars.updateCount==updates,@"idle apply does not schedule perpetual bars updates");
    [bars togglePlayback];
    check(bars.playPauseState==2 && bars.toggles==1,@"Photos transport remains callable while original button hidden");
    bars.allowShowingPlayPauseButton=YES;bars.shouldShowScrubber=YES;
    check(!bars.allowShowingPlayPauseButton && !bars.shouldShowScrubber,@"subsequent native requests stay hidden while replacement attached");
    PV2PhotosChromeApply(owner,NO);
    check(bars.allowShowingPlayPauseButton && bars.shouldShowScrubber,@"detach restores original requested visibility");
    PV2PhotosChromeApply(owner,YES);bars.allowShowingPlayPauseButton=NO;bars.shouldShowScrubber=YES;
    PV2PhotosChromeApply(owner,NO);
    check(!bars.allowShowingPlayPauseButton && bars.shouldShowScrubber,@"restore latest native requests rather than stale captured values");
    PV2ChromeBarsFixture *other=[PV2ChromeBarsFixture new];other.allowShowingPlayPauseButton=YES;other.shouldShowScrubber=YES;
    PV2PhotosChromeApply(owner,YES);owner.bars=other;PV2PhotosChromeApply(owner,YES);
    check(!other.allowShowingPlayPauseButton && !other.shouldShowScrubber,@"replacement bars gets own scoped policy");
    check(!bars.allowShowingPlayPauseButton && bars.shouldShowScrubber,@"previous bars restored when owner changes identity");
    PV2ChromeBarsFixture *unrelated=[PV2ChromeBarsFixture new];unrelated.allowShowingPlayPauseButton=YES;unrelated.shouldShowScrubber=YES;
    check(unrelated.allowShowingPlayPauseButton && unrelated.shouldShowScrubber,@"unrelated Photos controller unaffected");
    PV2PhotosChromeApply(owner,NO);
    check(other.allowShowingPlayPauseButton && other.shouldShowScrubber,@"photo or exit restores old native UI");
    check(objc_getAssociatedObject(owner,PV2PhotosChromeOwnerKey)==nil && objc_getAssociatedObject(other,PV2PhotosChromeBarsKey)==nil,@"teardown clears suppression ownership");
    return @{@"passed":@YES,@"assertions":@(count),@"fixture":@"policy-only; no full Photos one-up UI"};
}
