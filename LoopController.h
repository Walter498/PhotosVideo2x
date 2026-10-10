#pragma once
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <string.h>

@interface NSObject (PV2LoopTransaction)
- (void)performChanges:(void (^)(id state))changes withPresentationContext:(NSInteger)context presenter:(void *)presenter;
- (void)setLoopingEnabled:(BOOL)value;
@end

// iOS17.3 Photos uses context=1 and the browsing player's existing presenter identity.
// PXVideoSession's plain performChanges: is intentionally an unconditional assertion.
static BOOL PV2LoopEnableForBrowsing(id browsing,id session) {
    if (!browsing || !session) return NO;
    Method m=class_getInstanceMethod([session class],@selector(performChanges:withPresentationContext:presenter:));
    if (!m || strcmp(method_getTypeEncoding(m),"v40@0:8@?16q24^v32")!=0) return NO;
    Ivar ivar=class_getInstanceVariable([browsing class],"_videoSessionPresenter");
    if (!ivar || strcmp(ivar_getTypeEncoding(ivar),"^v")!=0) return NO;
    ptrdiff_t offset=ivar_getOffset(ivar);
    if (offset<0 || (size_t)offset+sizeof(void *)>class_getInstanceSize([browsing class])) return NO;
    void *presenter=NULL;
    memcpy(&presenter,(const char *)(__bridge const void *)browsing+offset,sizeof(presenter));
    if (!presenter) return NO;
    [session performChanges:^(id state) {
        // The callback receives the existing presentation state, NOT PXVideoSession.
        if ([state respondsToSelector:@selector(setLoopingEnabled:)]) [state setLoopingEnabled:YES];
    } withPresentationContext:1 presenter:presenter];
    return YES;
}
