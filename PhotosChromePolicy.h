#pragma once
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>

// Hide only the redundant Photos controls, not their player/state controller.
// The replacement AVKit button still invokes the original Photos toggle action.
@interface PV2PhotosChromePolicy : NSObject
@property(nonatomic,weak) id bars;
@property(nonatomic) BOOL active;
@property(nonatomic) BOOL writing;
@property(nonatomic) BOOL requestedPlay;
@property(nonatomic) BOOL requestedScrubber;
@end
@implementation PV2PhotosChromePolicy
@end
static const void *PV2PhotosChromeBarsKey=&PV2PhotosChromeBarsKey;
static const void *PV2PhotosChromeOwnerKey=&PV2PhotosChromeOwnerKey;
static BOOL PV2PhotosChromeABI(id object,NSString *name,const char *encoding) {
    Method m=object ? class_getInstanceMethod([object class],NSSelectorFromString(name)):NULL;
    return m && strcmp(method_getTypeEncoding(m),encoding)==0;
}
static BOOL PV2PhotosChromeRequested(id bars,BOOL play,BOOL requested) {
    PV2PhotosChromePolicy *p=objc_getAssociatedObject(bars,PV2PhotosChromeBarsKey);
    if (!p.active) return requested;
    if (!p.writing) { if (play) p.requestedPlay=requested; else p.requestedScrubber=requested; }
    return NO;
}
static void PV2PhotosChromeWrite(id bars,NSString *name,BOOL value) {
    ((void (*)(id,SEL,BOOL))objc_msgSend)(bars,NSSelectorFromString(name),value);
}
static void PV2PhotosChromeRestore(id owner) {
    PV2PhotosChromePolicy *p=objc_getAssociatedObject(owner,PV2PhotosChromeOwnerKey);
    if (!p) return;
    id bars=p.bars; p.active=NO;
    if (bars) {
        PV2PhotosChromeWrite(bars,@"setAllowShowingPlayPauseButton:",p.requestedPlay);
        PV2PhotosChromeWrite(bars,@"setShouldShowScrubber:",p.requestedScrubber);
        objc_setAssociatedObject(bars,PV2PhotosChromeBarsKey,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    objc_setAssociatedObject(owner,PV2PhotosChromeOwnerKey,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
static void PV2PhotosChromeApply(id owner,BOOL enabled) {
    if (!owner || !NSThread.isMainThread) return;
    if (!enabled) { PV2PhotosChromeRestore(owner); return; }
    if (!PV2PhotosChromeABI(owner,@"_barsController","@16@0:8")) return;
    id bars=((id (*)(id,SEL))objc_msgSend)(owner,NSSelectorFromString(@"_barsController"));
    PV2PhotosChromePolicy *p=objc_getAssociatedObject(owner,PV2PhotosChromeOwnerKey);
    if (p.active && p.bars==bars) return;
    if (p) PV2PhotosChromeRestore(owner);
    if (!PV2PhotosChromeABI(bars,@"allowShowingPlayPauseButton","B16@0:8") ||
        !PV2PhotosChromeABI(bars,@"shouldShowScrubber","B16@0:8") ||
        !PV2PhotosChromeABI(bars,@"setAllowShowingPlayPauseButton:","v20@0:8B16") ||
        !PV2PhotosChromeABI(bars,@"setShouldShowScrubber:","v20@0:8B16")) return;
    p=[PV2PhotosChromePolicy new];p.bars=bars;
    p.requestedPlay=((BOOL (*)(id,SEL))objc_msgSend)(bars,NSSelectorFromString(@"allowShowingPlayPauseButton"));
    p.requestedScrubber=((BOOL (*)(id,SEL))objc_msgSend)(bars,NSSelectorFromString(@"shouldShowScrubber"));
    p.active=YES;p.writing=YES;
    objc_setAssociatedObject(owner,PV2PhotosChromeOwnerKey,p,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(bars,PV2PhotosChromeBarsKey,p,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    @try {
        PV2PhotosChromeWrite(bars,@"setAllowShowingPlayPauseButton:",NO);
        PV2PhotosChromeWrite(bars,@"setShouldShowScrubber:",NO);
        if (PV2PhotosChromeABI(bars,@"updateBars","v16@0:8"))
            ((void (*)(id,SEL))objc_msgSend)(bars,NSSelectorFromString(@"updateBars"));
    } @finally { p.writing=NO; }
}
