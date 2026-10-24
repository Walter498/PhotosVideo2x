#pragma once
#import "PhotosChromePolicy.h"

@protocol PV2ChromeNativeCollection <NSObject>
- (instancetype)initWithOptions:(NSUInteger)options;
- (void)setIdentifiersOrder:(NSArray *)order;
- (void)setCenteredItemIdentifiers:(NSIndexSet *)identifiers;
- (void)setDataSource:(id)source;
- (NSArray *)orderedBarButtonsItemsForIdentifiers:(NSIndexSet *)identifiers;
@end
@interface PV2ChromeBarsFixture : NSObject
@property(nonatomic,strong) id collection;
@property(nonatomic) BOOL allowShowingPlayPauseButton;
@property(nonatomic) BOOL shouldShowScrubber;
@property(nonatomic) NSUInteger visibilityWrites;
@property(nonatomic) NSUInteger updateCount;
@property(nonatomic) NSInteger playPauseState;
@property(nonatomic) NSUInteger toggles;
@end
@implementation PV2ChromeBarsFixture
- (id)_toolbarButtonItemCollection { return _collection; }
- (void)setAllowShowingPlayPauseButton:(BOOL)value { _visibilityWrites++;_allowShowingPlayPauseButton=value; }
- (void)setShouldShowScrubber:(BOOL)value { _visibilityWrites++;_shouldShowScrubber=value; }
- (void)updateBars { _updateCount++; }
- (void)togglePlayback { _toggles++;_playPauseState=_playPauseState==1 ? 2 : 1; }
@end
@interface PV2ChromeOwnerFixture : NSObject
@property(nonatomic,strong) PV2ChromeBarsFixture *bars;
@end
@implementation PV2ChromeOwnerFixture
- (id)_barsController { return _bars; }
@end
@interface PV2ChromeItemSource : NSObject
@property(nonatomic,strong) NSMutableDictionary<NSNumber *,UIBarButtonItem *> *items;
@property(nonatomic) NSUInteger muteCalls;
@property(nonatomic) BOOL muted;
@end
@implementation PV2ChromeItemSource
- (void)mute:(id)sender { (void)sender;_muted=!_muted;_muteCalls++; }
- (UIBarButtonItem *)barButtonItemCollection:(id)collection newBarButtonItemForIdentifier:(long long)identifier {
    (void)collection;NSNumber *key=@(identifier);UIBarButtonItem *item=_items[key];
    if (!item) {
        item=[[UIBarButtonItem alloc] initWithTitle:[key stringValue] style:UIBarButtonItemStylePlain target:self action:@selector(mute:)];
        _items[key]=item;
    }
    return item;
}
@end
static NSDictionary *PV2RunChromePolicyChecks(void) {
    __block NSUInteger count=0;
    void (^check)(BOOL,NSString*)=^(BOOL ok,NSString *name) {
        count++;if (!ok) @throw [NSException exceptionWithName:@"ChromePolicyFailure" reason:name userInfo:nil];
    };
    void *image=dlopen("/System/Library/PrivateFrameworks/PhotosUIPrivate.framework/PhotosUIPrivate",RTLD_NOW|RTLD_LOCAL);
    check(image!=NULL,@"load actual PhotosUIPrivate for native toolbar arrangement");
    Class cls=NSClassFromString(@"PUBarButtonItemCollection");
    check(cls!=Nil,@"actual Photos toolbar collection exists");
    PV2ChromeItemSource *source=[PV2ChromeItemSource new];source.items=[NSMutableDictionary dictionary];
    id<PV2ChromeNativeCollection> collection=[(id<PV2ChromeNativeCollection>)[cls alloc] initWithOptions:1];
    [collection setIdentifiersOrder:@[@1,@2,@11,@12,@34,@35,@20,@40]];
    [collection setCenteredItemIdentifiers:[NSIndexSet indexSetWithIndex:11]];
    [collection setDataSource:source];
    PV2ChromeOwnerFixture *owner=[PV2ChromeOwnerFixture new];
    PV2ChromeBarsFixture *bars=[PV2ChromeBarsFixture new];owner.bars=bars;bars.collection=collection;
    bars.allowShowingPlayPauseButton=YES;bars.shouldShowScrubber=YES;bars.playPauseState=1;
    NSUInteger writes=bars.visibilityWrites;
    NSMutableIndexSet *ids=[NSMutableIndexSet indexSet];
    for (NSNumber *n in @[@1,@2,@11,@12,@34,@35,@20]) [ids addIndex:n.unsignedIntegerValue];
    NSIndexSet *saved=[ids copy];
    check(PV2PhotosChromeToolbarIdentifiers(collection,ids)==ids,@"no filtering before replacement is ready");
    PV2PhotosChromeApply(owner,YES);
    check(bars.allowShowingPlayPauseButton && bars.shouldShowScrubber && bars.visibilityWrites==writes,@"mute gating and original filmstrip remain entirely native");
    check(bars.playPauseState==1,@"filtering does not change authoritative playback state");
    NSUInteger updates=bars.updateCount;PV2PhotosChromeApply(owner,YES);
    check(bars.updateCount==updates,@"idle apply does not rearm toolbar updates");
    NSIndexSet *filtered=PV2PhotosChromeToolbarIdentifiers(collection,ids);
    check(![filtered containsIndex:11] && ![filtered containsIndex:12],@"both play and pause removed before native arrangement");
    check([filtered containsIndex:34] && [filtered containsIndex:35] && filtered.count==ids.count-2,@"both mute states and every unrelated identifier preserved");
    check([ids isEqualToIndexSet:saved],@"requested identifiers never mutated");
    NSArray *native=[collection orderedBarButtonsItemsForIdentifiers:filtered];
    UIBarButtonItem *speed=[[UIBarButtonItem alloc] initWithTitle:@"1x" style:UIBarButtonItemStylePlain target:nil action:nil];
    NSMutableArray *withSpeed=[native mutableCopy];
    NSUInteger anchor=[withSpeed indexOfObjectIdenticalTo:source.items[@20]];
    check(anchor!=NSNotFound,@"native info anchor survives arrangement");
    [withSpeed insertObject:speed atIndex:anchor];
    NSArray *compact=PV2PhotosChromeCompactToolbar(collection,withSpeed);
    id flex=PV2PhotosChromeObject(collection,@"_flexibleSpaceBarButtonItem");
    id placeholder=PV2PhotosChromeObject(collection,@"_placeholderBarButtonItem");
    NSArray *actions=@[source.items[@1],source.items[@2],source.items[@34],source.items[@35],speed,source.items[@20]];
    check(compact.count==actions.count*2-1,@"exactly one flexible space per actual button gap, no empty slot");
    check(![compact containsObject:placeholder],@"native centering placeholder removed from final toolbar");
    for (NSUInteger i=0;i<compact.count;i++) check(compact[i]==(i%2 ? flex : actions[i/2]),@"all real button identities and order retained");
    check([PV2PhotosChromeCompactToolbar(collection,compact) isEqualToArray:compact],@"toolbar reflow is idempotent");
    UIBarButtonItem *mute=source.items[@34];
    check(mute.target==source && mute.action==@selector(mute:),@"original mute target and action retained");
    [UIApplication.sharedApplication sendAction:mute.action to:mute.target from:mute forEvent:nil];
    check(source.muted && source.muteCalls==1,@"mute remains callable after removing play slot");
    UIBarButtonItem *unmute=source.items[@35];
    [UIApplication.sharedApplication sendAction:unmute.action to:unmute.target from:unmute forEvent:nil];
    check(!source.muted && source.muteCalls==2,@"unmute remains callable after toolbar reflow");
    [bars togglePlayback];check(bars.playPauseState==2,@"upper button underlying Photos toggle stays available");
    // Real UIKit toolbar lays out the real Photos collection output. This is
    // not a full Photos one-up screenshot or physical touch validation.
    NSMutableArray *layouts=[NSMutableArray array];
    for (NSNumber *width in @[@393,@852]) {
        UIToolbar *toolbar=[[UIToolbar alloc] initWithFrame:CGRectMake(0,0,width.doubleValue,44)];
        [toolbar setItems:compact animated:NO];[toolbar layoutIfNeeded];
        check(toolbar.items.count==compact.count,@"UIKit receives compact array without hidden playback item");
        [layouts addObject:@{@"width":width,@"actions":@(actions.count),@"spaces":@(actions.count-1)}];
        UIGraphicsBeginImageContextWithOptions(toolbar.bounds.size,YES,2);
        [toolbar.layer renderInContext:UIGraphicsGetCurrentContext()];
        UIImage *picture=UIGraphicsGetImageFromCurrentImageContext();UIGraphicsEndImageContext();
        NSString *name=[NSString stringWithFormat:@"toolbar-reflow-%@.png",width];
        NSString *path=[NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject stringByAppendingPathComponent:name];
        check([UIImagePNGRepresentation(picture) writeToFile:path atomically:YES],@"save actual UIKit toolbar reflow preview");
    }
    bars.shouldShowScrubber=NO;writes=bars.visibilityWrites;
    PV2PhotosChromeApply(owner,NO);
    check(bars.visibilityWrites==writes && !bars.shouldShowScrubber,@"detach never overwrites newer native filmstrip request");
    check(PV2PhotosChromeToolbarIdentifiers(collection,ids)==ids,@"detach restores original play identifiers");
    check(PV2PhotosChromeCompactToolbar(collection,native)==native,@"detach restores native spacing unchanged");
    check(objc_getAssociatedObject(collection,PV2PhotosChromeCollectionKey)==nil,@"detach clears collection ownership");
    return @{@"passed":@YES,@"assertions":@(count),@"realNativeCollection":@YES,@"muteActionTested":@YES,@"nativeFilmstripPolicyUntouched":@YES,@"noPlaceholder":@YES,@"layouts":layouts,@"fixture":@"real PUBarButtonItemCollection and UIToolbar with fixture data source; not full Photos one-up UI"};
}
