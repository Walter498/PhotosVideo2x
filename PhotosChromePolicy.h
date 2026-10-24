#pragma once
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>

// Filter only play/pause toolbar identifiers before native arrangement.
// Never disable allowShowingPlayPauseButton: Photos also gates Mute with it.
// Leave shouldShowScrubber entirely native so the original filmstrip returns.
@interface PV2PhotosChromePolicy : NSObject
@property(nonatomic,weak) id bars;
@property(nonatomic,weak) id collection;
@property(nonatomic) BOOL active;
@end
@implementation PV2PhotosChromePolicy
@end
static const void *PV2PhotosChromeCollectionKey=&PV2PhotosChromeCollectionKey;
static const void *PV2PhotosChromeOwnerKey=&PV2PhotosChromeOwnerKey;
static BOOL PV2PhotosChromeABI(id object,NSString *name,const char *encoding) {
    Method m=object ? class_getInstanceMethod([object class],NSSelectorFromString(name)):NULL;
    return m && strcmp(method_getTypeEncoding(m),encoding)==0;
}
static id PV2PhotosChromeObject(id object,NSString *selector) {
    return ((id (*)(id,SEL))objc_msgSend)(object,NSSelectorFromString(selector));
}
static BOOL PV2PhotosChromeCollectionActive(id collection) {
    PV2PhotosChromePolicy *p=objc_getAssociatedObject(collection,PV2PhotosChromeCollectionKey);
    return p.active && p.collection==collection;
}
static NSIndexSet *PV2PhotosChromeToolbarIdentifiers(id collection,id requested) {
    if (!PV2PhotosChromeCollectionActive(collection) || ![requested isKindOfClass:NSIndexSet.class]) return requested;
    // Verified iOS17.3 updateBars: 11=Play, 12=Pause; 34/35=Mute/Unmute.
    // Remove before _newEntry/_arranged... generate spacers or cache an array.
    if (![requested containsIndex:11] && ![requested containsIndex:12]) return requested;
    NSMutableIndexSet *result=[requested mutableCopy];
    [result removeIndex:11];[result removeIndex:12];
    return [result copy];
}
static NSArray *PV2PhotosChromeCompactToolbar(id collection,NSArray *items) {
    if (!PV2PhotosChromeCollectionActive(collection) || !items.count) return items;
    if (!PV2PhotosChromeABI(collection,@"_flexibleSpaceBarButtonItem","@16@0:8") ||
        !PV2PhotosChromeABI(collection,@"_placeholderBarButtonItem","@16@0:8")) return items;
    id flex=PV2PhotosChromeObject(collection,@"_flexibleSpaceBarButtonItem");
    id placeholder=PV2PhotosChromeObject(collection,@"_placeholderBarButtonItem");
    if (!flex) return items;
    NSMutableArray *result=[NSMutableArray array];
    for (id item in items) {
        // Keep every real native item, including both mute states and custom
        // speed/export. Rebuild only known native spacers, not arbitrary views.
        if (item==flex || item==placeholder) continue;
        if (result.count) [result addObject:flex];
        [result addObject:item];
    }
    return [result copy];
}
static void PV2PhotosChromeUpdateBars(id bars) {
    if (PV2PhotosChromeABI(bars,@"updateBars","v16@0:8"))
        ((void (*)(id,SEL))objc_msgSend)(bars,NSSelectorFromString(@"updateBars"));
}
static void PV2PhotosChromeRestore(id owner) {
    PV2PhotosChromePolicy *p=objc_getAssociatedObject(owner,PV2PhotosChromeOwnerKey);
    if (!p) return;
    id bars=p.bars, collection=p.collection; p.active=NO;
    if (objc_getAssociatedObject(collection,PV2PhotosChromeCollectionKey)==p)
        objc_setAssociatedObject(collection,PV2PhotosChromeCollectionKey,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(owner,PV2PhotosChromeOwnerKey,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    PV2PhotosChromeUpdateBars(bars);
}
static void PV2PhotosChromeApply(id owner,BOOL enabled) {
    if (!owner || !NSThread.isMainThread) return;
    if (!enabled) { PV2PhotosChromeRestore(owner); return; }
    if (!PV2PhotosChromeABI(owner,@"_barsController","@16@0:8")) return;
    id bars=PV2PhotosChromeObject(owner,@"_barsController");
    if (!PV2PhotosChromeABI(bars,@"_toolbarButtonItemCollection","@16@0:8")) return;
    id collection=PV2PhotosChromeObject(bars,@"_toolbarButtonItemCollection");
    PV2PhotosChromePolicy *p=objc_getAssociatedObject(owner,PV2PhotosChromeOwnerKey);
    if (p.active && p.bars==bars && p.collection==collection) return;
    if (p) PV2PhotosChromeRestore(owner);
    if (!PV2PhotosChromeABI(collection,@"orderedBarButtonsItemsForIdentifiers:","@24@0:8@16") ||
        !PV2PhotosChromeABI(collection,@"_flexibleSpaceBarButtonItem","@16@0:8") ||
        !PV2PhotosChromeABI(collection,@"_placeholderBarButtonItem","@16@0:8")) return;
    p=[PV2PhotosChromePolicy new];p.bars=bars;p.collection=collection;p.active=YES;
    objc_setAssociatedObject(owner,PV2PhotosChromeOwnerKey,p,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(collection,PV2PhotosChromeCollectionKey,p,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    PV2PhotosChromeUpdateBars(bars);
}
