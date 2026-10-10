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
- (void)performChanges:(void (^)(id))changes;
- (void)performChanges:(void (^)(id))changes withPresentationContext:(NSInteger)context presenter:(void *)presenter;
@end
@implementation FakeSession
- (instancetype)init {if((self=[super init]))_state=[FakeState new];return self;}
- (void)performChanges:(void (^)(id))changes {
 (void)changes;Check(NO,"plain PXVideoSession performChanges must never be used");
}
- (void)performChanges:(void (^)(id))changes withPresentationContext:(NSInteger)context presenter:(void *)presenter {
 self.calls++;self.context=context;self.presenter=presenter;changes(self.state);
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
 puts("PASS: no plain session transaction, existing presentation context/presenter, loop state, ABI guards");
}return 0;}
