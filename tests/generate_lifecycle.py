from pathlib import Path
h=Path('DownloadController.h').read_text()
a=h.index('static void PV2DownloadDetach(id tile) {');b=h.index('static void PV2DownloadEvent(id provider, BOOL result) {',a)
helpers=h[a:b]
a=h.index('static BOOL PV2TileIsSelected(id tile) {');b=h.index('static BOOL PV2ResourceChromeVisible',a)
gate=h[a:b]
preamble='''#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#import "../DownloadPolicy.h"
@interface FakeView : NSObject
@property(nonatomic,strong) NSObject *window;
@end
@implementation FakeView
@end
@interface UIViewController : NSObject
@property(nonatomic,strong) FakeView *view;
@property(nonatomic) BOOL isViewLoaded;
@property(nonatomic,strong) id tile;
@end
@implementation UIViewController
- (id)_currentContentTileController { return self.tile; }
@end
@interface PHAsset : NSObject
@property(nonatomic) NSInteger mediaType;
@end
@implementation PHAsset
@end
static const NSInteger PHAssetMediaTypeVideo=2;
@interface FakeTile : NSObject
@property(nonatomic) BOOL active;
@property(nonatomic,strong) PHAsset *asset;
@end
@implementation FakeTile
- (BOOL)isActive { return self.active; }
@end
@interface PUVideoTileViewController : FakeTile
@end
@implementation PUVideoTileViewController
@end
@interface FakePanel : NSObject
@property(nonatomic) BOOL hidden;
@end
@implementation FakePanel
@end
@interface PV2DownloadController : NSObject
@property(nonatomic,weak) id tile;
@property(nonatomic,strong) FakePanel *panel;
@property(nonatomic) NSUInteger refreshes;
@property(nonatomic) NSUInteger detaches;
- (void)refresh;
- (void)detach;
@end
@implementation PV2DownloadController
- (void)refresh { self.refreshes++; }
- (void)detach { self.detaches++; }
@end
#import <objc/runtime.h>
static const void *PV2DownloadTileKey=&PV2DownloadTileKey;
static NSHashTable *PV2Downloads;
static __weak UIViewController *PV2VisibleOneUp;
static __weak id PV2CurrentOneUpTile;
static BOOL PV2OneUpVisible=NO, PV2DownloadEnabled=YES;
static NSUInteger PV2OneUpEpoch;
static PHAsset *PV2SelectedAsset(void) { return [(FakeTile *)PV2VisibleOneUp.tile asset]; }
static BOOL PV2ResourceChromeVisible(void) { return YES; }
'''
test='''
static void Check(BOOL ok,const char *m){if(!ok){fprintf(stderr,"FAIL %s\\n",m);exit(1);}}
static void Drain(void){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.03]];}
static UIViewController *Owner(FakeTile *tile){
 UIViewController *o=[UIViewController new];o.view=[FakeView new];o.view.window=[NSObject new];o.isViewLoaded=YES;o.tile=tile;return o;
}
int main(void){@autoreleasepool{
 PUVideoTileViewController *t=[PUVideoTileViewController new];t.active=YES;t.asset=[PHAsset new];t.asset.mediaType=2;
 UIViewController *o=Owner(t);PV2DownloadEnter(o);Drain();
 PV2DownloadController *c=objc_getAssociatedObject(t,PV2DownloadTileKey);
 Check(c.refreshes==1,"visible owner establishes current tile");
 PUVideoTileViewController *preheat=[PUVideoTileViewController new];preheat.active=YES;
 PV2DownloadRefresh(preheat);Drain();
 Check(PV2CurrentOneUpTile==t && c.detaches==0,"preheat event cannot replace current");
 PV2DownloadRefresh(t);NSUInteger previous=c.refreshes;PV2DownloadLeave(o);Drain();
 Check(c.refreshes==previous && PV2CurrentOneUpTile==nil && !PV2OneUpVisible,"queued refresh rejected after exit");
 PV2DownloadRefresh(t);Drain();Check(c.refreshes==previous,"post-exit tile event cannot rebuild");
 PV2DownloadEnter(o);Drain();
 t.asset.mediaType=1;PV2DownloadRefreshOneUp(o);Drain();
 Check(PV2CurrentOneUpTile==nil,"photo selection detaches video panel");
 t.asset.mediaType=2;PV2DownloadRefreshOneUp(o);Drain();Check(PV2CurrentOneUpTile==t,"back to video eligible");
 UIViewController *newOwner=Owner(t);PV2DownloadEnter(newOwner);Drain();
 PV2DownloadLeave(o);Check(PV2VisibleOneUp==newOwner && PV2OneUpVisible,"old owner exit cannot clear new page");
 PV2DownloadLeave(newOwner);Drain();
 puts("PASS: production lifecycle helpers, preheat isolation, queued exit, photo selection, owner isolation");
}return 0;}
'''
Path('tests/GeneratedLifecycleTests.mm').write_text(preamble+gate+helpers+test)
