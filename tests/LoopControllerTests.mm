#import <Foundation/Foundation.h>
#import "../LoopController.h"
#include <stdlib.h>
static void Check(BOOL ok,const char *text){if(!ok){fprintf(stderr,"FAIL %s\n",text);exit(1);}}
@interface FakeBrowser : NSObject {
@public void *_videoSessionPresenter;
}
@end
@implementation FakeBrowser
@end
@interface FakeState : NSObject
@property(nonatomic) BOOL loop;
@end
@implementation FakeState
- (void)setLoopingEnabled:(BOOL)value { self.loop=value; }
@end
@interface FakeSession : NSObject
@property(nonatomic,strong) FakeState *state;
@property(nonatomic) NSInteger context;
@property(nonatomic) void *presenter;
@property(nonatomic) NSUInteger calls;
@property(nonatomic) BOOL skipCallback;
- (void)performChanges:(void (^)(id))changes;
- (void)performChanges:(void (^)(id))changes withPresentationContext:(NSInteger)context presenter:(void *)presenter;
@end
@implementation FakeSession
- (instancetype)init {if((self=[super init]))_state=[FakeState new];return self;}
- (void)performChanges:(void (^)(id))changes {
 (void)changes;Check(NO,"plain PXVideoSession performChanges must never be used");
}
- (void)performChanges:(void (^)(id))changes withPresentationContext:(NSInteger)context presenter:(void *)presenter {
 self.calls++;self.context=context;self.presenter=presenter;
 if (!self.skipCallback) changes(self.state);
}
@end
int main(void){@autoreleasepool{
 FakeSession *s=[FakeSession new];FakeBrowser *b=[FakeBrowser new];int identity=123;
 b->_videoSessionPresenter=&identity;
 Check(PV2LoopEnableForBrowsing(b,s),"existing presenter enables looping");
 Check(s.calls==1 && s.context==1 && s.presenter==&identity,"native context and exact presenter identity used");
 Check(s.state.loop,"transaction changes presentation state");
 b->_videoSessionPresenter=NULL;
 Check(!PV2LoopEnableForBrowsing(b,s) && s.calls==1,"missing presenter skips transaction");
 Check(!PV2LoopEnableForBrowsing([NSObject new],s),"unknown browser without presenter skipped");
 Check(!PV2LoopEnableForBrowsing(b,[NSObject new]),"unsupported session skipped");
 b->_videoSessionPresenter=&identity;s.skipCallback=YES;s.state.loop=NO;
 Check(!PV2LoopEnableForBrowsing(b,s) && !s.state.loop,"missing presentation state not falsely reported successful");
 s.skipCallback=NO;
 Check(PV2LoopEnableForBrowsing(b,s) && s.state.loop,"retry after late context becomes ready");
 Check(!PV2LoopEffectiveValue(s,NO),"unmarked native session can disable loop");
 PV2LoopMarkSession(s,YES);
 Check(PV2LoopEffectiveValue(s,NO),"marked selected session preserves default loop");
 FakeSession *other=[FakeSession new];
 Check(!PV2LoopEffectiveValue(other,NO),"unselected session untouched");
 PV2LoopMarkSession(s,NO);
 Check(!PV2LoopEffectiveValue(s,NO) && PV2LoopEffectiveValue(s,YES),"exit removes override preserves native true");
 puts("PASS: presentation transaction, late context retry, marked loop persistence, unselected/exit isolation");
}return 0;}
