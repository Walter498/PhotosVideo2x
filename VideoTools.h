#pragma once
#import "VideoExport.h"
@interface NSObject (PV2VideoToolsNative)
- (id)_barsController;
- (id)_toolbarButtonItemCollection;
- (UIBarButtonItem *)barButtonItemToggleDetails;
- (BOOL)isLoopingEnabled;
- (void)setLoopingEnabled:(BOOL)enabled;
- (void)updateBars;
@end
static const void *PV2VideoToolsKey=&PV2VideoToolsKey;
static BOOL PV2VideoToolsEnabled=NO;
static BOOL PV2UpdatingTools=NO;

@interface PV2VideoTools : NSObject
@property(nonatomic,weak) UIViewController *owner;
@property(nonatomic,strong) UIButton *button;
@property(nonatomic,strong) UIBarButtonItem *item;
@property(nonatomic,strong) id<PV2RatePlayer> wrapper;
@property(nonatomic,strong) id session;
@property(nonatomic) float selectedRate;
- (void)sync;
- (void)detach;
@end

static BOOL PV2ToolsVideoSelected(UIViewController *owner) {
    id asset=PV2SelectedAsset();
    return PV2VideoToolsEnabled && PV2OneUpVisible && owner==PV2VisibleOneUp &&
        [asset isKindOfClass:PHAsset.class] && ((PHAsset *)asset).mediaType==PHAssetMediaTypeVideo;
}
@implementation PV2VideoTools
- (instancetype)init {
    if ((self=[super init])) {
        NSNumber *saved=[NSUserDefaults.standardUserDefaults objectForKey:@"PV2FixedPlaybackRate"];
        float r=saved ? saved.floatValue : 1;
        _selectedRate=(isfinite(r) && r>=0.5 && r<=2) ? r : 1;
        _button=[UIButton buttonWithType:UIButtonTypeSystem];
        _button.frame=CGRectMake(0,0,48,32);
        _button.titleLabel.font=[UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightSemibold];
        [_button addTarget:self action:@selector(speedMenu:) forControlEvents:UIControlEventTouchUpInside];
        _button.accessibilityHint=@"点击选择速度，长按导出";
        UILongPressGestureRecognizer *g=[[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(exportMenu:)];
        g.minimumPressDuration=0.5;g.cancelsTouchesInView=YES;
        [_button addGestureRecognizer:g];
        _item=[[UIBarButtonItem alloc] initWithCustomView:_button];
    }
    return self;
}
- (void)detach {
    if (self.wrapper) PV2ClearFixedRate(self.wrapper);
    self.wrapper=nil;self.session=nil;self.button.menu=nil;
}
- (void)sync {
    if (!PV2ToolsVideoSelected(self.owner)) { [self detach]; return; }
    id tile=[(id)self.owner _currentContentTileController];
    id session=[[tile _browsingVideoPlayer] videoSession];
    id<PV2RatePlayer> wrapper=[session videoPlayer];
    if (![wrapper isKindOfClass:NSClassFromString(@"ISWrappedAVPlayer")]) { [self detach];return; }
    if (self.wrapper!=wrapper) { [self detach]; self.wrapper=wrapper; }
    PV2SetFixedRate(wrapper,self.selectedRate);
    if (session!=self.session) {
        self.session=session;
        if (PV2DownloadABI([session class],@"setLoopingEnabled:","v20@0:8B16") &&
            PV2DownloadABI([session class],@"isLoopingEnabled","B16@0:8")) {
            [session performChanges:^(id mutableSession){ [mutableSession setLoopingEnabled:YES]; }];
        }
    }
    NSString *title=[NSString stringWithFormat:@"%gx",PV2FixedRate(self.wrapper)];
    [self.button setTitle:title forState:UIControlStateNormal];self.item.accessibilityLabel=[@"播放速度 " stringByAppendingString:title];

}
- (void)speedMenu:(UIButton *)sender {
    if (!PV2ToolsVideoSelected(self.owner) || self.owner.presentedViewController) return;
    UIAlertController *menu=[UIAlertController alertControllerWithTitle:@"播放速度" message:@"侧边长按临时 2x，松手恢复此速度" preferredStyle:UIAlertControllerStyleActionSheet];
    __weak PV2VideoTools *weakTools=self;
    for (NSNumber *r in @[@0.5,@1,@1.25,@1.5,@2]) {
        NSString *title=[NSString stringWithFormat:@"%@%gx",fabs(r.floatValue-self.selectedRate)<0.001 ? @"✓ " : @"",r.floatValue];
        [menu addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action){
            PV2VideoTools *t=weakTools; if (!t) return;
            t.selectedRate=r.floatValue;
            [NSUserDefaults.standardUserDefaults setFloat:t.selectedRate forKey:@"PV2FixedPlaybackRate"];
            // Apply immediately without starting a paused player. The next page refresh
            // rebinds the saved choice after the sheet closes.
            if (t.wrapper) PV2SetFixedRate(t.wrapper,t.selectedRate);
            [t.button setTitle:[NSString stringWithFormat:@"%gx",t.selectedRate] forState:UIControlStateNormal];
        }]];
    }
    [menu addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    menu.popoverPresentationController.barButtonItem=self.item;
    [self.owner presentViewController:menu animated:YES completion:nil];
}
- (void)exportMenu:(UILongPressGestureRecognizer *)gesture {
    if (gesture.state!=UIGestureRecognizerStateBegan || !PV2ToolsVideoSelected(self.owner)) return;
    if (self.owner.presentedViewController) return;
    id tile=[(id)self.owner _currentContentTileController];
    id session=[[tile _browsingVideoPlayer] videoSession];
    // session playerItem is the editable template; looping currentItem may be a clone.
    AVPlayerItem *item=[session playerItem];
    if (![item isKindOfClass:AVPlayerItem.class]) item=[session currentPlayerItem];
    float rate=[self.wrapper rate];
    if (!isfinite(rate) || rate<=0) rate=self.selectedRate;
    UIAlertController *menu=[UIAlertController alertControllerWithTitle:@"导出" message:[NSString stringWithFormat:@"当前速度 %gx",rate] preferredStyle:UIAlertControllerStyleActionSheet];
    NSArray *names=@[@"导出当前倍速视频",@"导出正常倍速音频",@"导出当前倍速音频"];
    __weak PV2VideoTools *weakTools=self;
    __weak UIAlertController *weakSheet=menu;
    for (NSInteger mode=0;mode<3;mode++) {
        [menu addAction:[UIAlertAction actionWithTitle:names[mode] style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action){
            PV2VideoTools *t=weakTools;
            if (!t || !t.owner || !t.owner.isViewLoaded || !t.owner.view.window) return;
            // Presenting the sheet temporarily deactivates OneUp; capture the original
            // item/rate and wait until the action sheet has been dismissed.
            void (^start)(void)=^{
                if (!t.owner.presentedViewController && t.owner.view.window)
                    PV2ExportMedia(t.owner,t.item,item,mode==1 ? 1.0 : rate,mode);
            };
            UIAlertController *sheet=weakSheet;
            if (sheet.presentingViewController) [sheet dismissViewControllerAnimated:YES completion:start];
            else dispatch_async(dispatch_get_main_queue(),start);
        }]];
    }
    [menu addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    menu.popoverPresentationController.barButtonItem=self.item;
    [self.owner presentViewController:menu animated:YES completion:nil];
}
@end

static PV2VideoTools *PV2ToolsForOwner(UIViewController *owner,BOOL create) {
    if (!owner)return nil;
    PV2VideoTools *tools=objc_getAssociatedObject(owner,PV2VideoToolsKey);
    if (!tools && create) { tools=[PV2VideoTools new];tools.owner=owner;objc_setAssociatedObject(owner,PV2VideoToolsKey,tools,OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
    return tools;
}
static NSArray *PV2ToolsInsertItems(id collection,NSArray *original) {
    UIViewController *owner=PV2VisibleOneUp;
    if (!NSThread.isMainThread || !PV2ToolsVideoSelected(owner))return original;
    id bars=[(id)owner _barsController];
    if (collection!=[bars _toolbarButtonItemCollection])return original;
    UIBarButtonItem *info=[bars barButtonItemToggleDetails];
    NSUInteger index=[original indexOfObjectIdenticalTo:info];
    if (index==NSNotFound)return original; // never insert in unrelated/navigation arrays
    PV2VideoTools *tools=PV2ToolsForOwner(owner,YES);[tools sync];
    if (!tools.wrapper || [original containsObject:tools.item])return original;
    NSMutableArray *result=[original mutableCopy];[result insertObject:tools.item atIndex:index];return result;
}
static void PV2ToolsRefresh(UIViewController *owner) {
    if (!NSThread.isMainThread || owner!=PV2VisibleOneUp || !PV2OneUpVisible || PV2UpdatingTools)return;
    PV2UpdatingTools=YES;
    [PV2ToolsForOwner(owner,YES) sync];
    id bars=[(id)owner _barsController];
    if ([bars respondsToSelector:@selector(updateBars)]) [bars updateBars];
    PV2UpdatingTools=NO;
}
static void PV2ToolsLeave(UIViewController *owner) { [PV2ToolsForOwner(owner,NO) detach]; }
