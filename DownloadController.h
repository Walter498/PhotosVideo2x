#pragma once
#import <Photos/Photos.h>
#import <AVFoundation/AVFoundation.h>
#import <objc/runtime.h>
#import "DownloadPolicy.h"

// DSC iOS17.3 signatures; instantiate dynamically to avoid private class link refs.
@interface NSObject (PV2DownloadNative)
- (id)asset;
- (id)mediaProvider;
- (id)audioSession;
- (id)contentProvider;
- (id)videoSession;
- (id)_browsingVideoPlayer;
- (BOOL)isActive;
- (UIView *)tilingView;
- (id)initWithAsset:(id)asset mediaProvider:(id)media deliveryStrategies:(NSArray *)strategies audioSession:(id)audio requestURLOnly:(BOOL)urlOnly;
- (void)setIsNetworkAccessAllowed:(BOOL)value;
- (void)setIsStreamingAllowed:(BOOL)value;
- (void)setQuality:(NSInteger)value;
- (void)beginLoadingWithPriority:(NSInteger)priority;
- (void)cancelLoading;
- (double)loadingProgress;
- (id)loadingResult;
- (void)setLoadingResult:(id)result;
- (void)performChanges:(void (^)(id mutableObject))changes;
- (NSError *)error;
- (NSURL *)url;
- (AVPlayerItem *)playerItem;
- (AVPlayerItem *)currentPlayerItem;
- (CMTime)currentTime;
- (void)seekToTime:(CMTime)time completionHandler:(void (^)(BOOL))completion;
@end

@interface NSObject (PV2KnownCloudStatus)
- (BOOL)isInCloud;
- (NSString *)localIdentifier;
@end
@interface PV2NativeProvider : NSObject
- (double)loadingProgress;
@end
@interface PV2NativeBrowsing : NSObject
- (CMTime)currentTime;
@end

@class PV2DownloadController;
static const void *PV2DownloadTileKey = &PV2DownloadTileKey;
static const void *PV2DownloadProviderKey = &PV2DownloadProviderKey;
static NSHashTable<PV2DownloadController *> *PV2Downloads;
static __weak id PV2CurrentOneUpTile;
static BOOL PV2DownloadEnabled = NO;

static BOOL PV2DownloadABI(Class cls, NSString *selector, const char *encoding) {
    Method m = cls ? class_getInstanceMethod(cls, NSSelectorFromString(selector)) : NULL;
    return m && strcmp(method_getTypeEncoding(m), encoding) == 0;
}
static BOOL PV2DownloadCompatible(id provider) {
    Class cls = [provider class], strategy = NSClassFromString(@"PXDisplayAssetVideoContentDeliveryStrategy");
    return [provider isKindOfClass:NSClassFromString(@"PXDisplayAssetVideoContentProvider")]
        && PV2DownloadABI(cls,@"initWithAsset:mediaProvider:deliveryStrategies:audioSession:requestURLOnly:","@52@0:8@16@24@32@40B48")
        && PV2DownloadABI(cls,@"beginLoadingWithPriority:","v24@0:8q16")
        && PV2DownloadABI(cls,@"cancelLoading","v16@0:8")
        && PV2DownloadABI(cls,@"asset","@16@0:8")
        && PV2DownloadABI(cls,@"mediaProvider","@16@0:8")
        && PV2DownloadABI(strategy,@"setIsStreamingAllowed:","v20@0:8B16")
        && PV2DownloadABI(strategy,@"setIsNetworkAccessAllowed:","v20@0:8B16")
        && PV2DownloadABI(strategy,@"setQuality:","v24@0:8q16");
}

// Local means a completed non-streaming provider result, not AVPlayer buffer state.
static NSURL *PV2DownloadLocalURL(id result) {
    if ([result error]) return nil;
    NSURL *url = [result url];
    if (!url && [[result playerItem].asset isKindOfClass:AVURLAsset.class])
        url = ((AVURLAsset *)[result playerItem].asset).URL;
    NSNumber *size = nil;
    BOOL readable = url.isFileURL && [url getResourceValue:&size forKey:NSURLFileSizeKey error:NULL]
        && [[NSFileManager defaultManager] isReadableFileAtPath:url.path];
    if (!PV2DownloadResultIsLocal(NO,url.isFileURL,readable,size.unsignedLongLongValue,YES)) return nil;
    return url;
}
static NSString *PV2DownloadBytes(unsigned long long bytes) {
    return [NSByteCountFormatter stringFromByteCount:(long long)MIN(bytes,(unsigned long long)LLONG_MAX) countStyle:NSByteCountFormatterCountStyleFile];
}

@interface PV2DownloadController : NSObject
@property(nonatomic, weak) id tile;
@property(nonatomic, strong) id browsing;
@property(nonatomic, strong) id session;
@property(nonatomic, strong) id originalProvider;
@property(nonatomic, strong) id requestProvider;
@property(nonatomic, strong) id asset;
@property(nonatomic, strong) id result;
@property(nonatomic, strong) NSURL *verifiedURL;
@property(nonatomic) BOOL buildingItem;
@property(nonatomic, strong) UIButton *button;
@property(nonatomic, strong) UIVisualEffectView *panel;
@property(nonatomic, strong) UILabel *titleLabel;
@property(nonatomic, strong) UILabel *detailLabel;
@property(nonatomic, strong) UIImageView *icon;
@property(nonatomic) NSUInteger generation;
@property(nonatomic) BOOL downloading;
@property(nonatomic) BOOL readyLocal;
@property(nonatomic) BOOL supported;
@property(nonatomic) BOOL probing;
@property(nonatomic) BOOL localProbeFailed;
@property(nonatomic) BOOL switchPending;
@property(nonatomic) CMTime switchTime;
@property(nonatomic) unsigned long long originalBytes;
@property(nonatomic) unsigned long long localBytes;
@property(nonatomic) NSTimeInterval duration;
@property(nonatomic) double progress;
@property(nonatomic, copy) NSString *status;
- (void)refresh;
- (void)start:(BOOL)network;
- (void)progressFrom:(id)provider;
- (void)resultFrom:(id)provider;
- (void)detach;
@end

@implementation PV2DownloadController
- (BOOL)isCurrent {
    return PV2DownloadCallbackIsCurrent(self.tile && [self.tile isActive] &&
        (![self.tile respondsToSelector:@selector(isPresentationActive)] || [self.tile isPresentationActive]),
        [self.tile _browsingVideoPlayer] == self.browsing, [self.browsing videoSession] == self.session,
        [self.session contentProvider] == self.originalProvider, YES);
}
- (BOOL)isSameAsset:(id)asset {
    if (asset == self.asset) return YES;
    NSString *a = [self.asset respondsToSelector:@selector(localIdentifier)] ? [self.asset localIdentifier] : nil;
    NSString *b = [asset respondsToSelector:@selector(localIdentifier)] ? [asset localIdentifier] : nil;
    return a.length && b.length && [a isEqualToString:b];
}
- (void)hidePanel {
    [self.panel removeFromSuperview];
    self.panel = nil; self.button = nil; self.titleLabel = nil; self.detailLabel = nil; self.icon = nil;
}
- (void)cancelRequest {
    id p = self.requestProvider; self.requestProvider = nil;
    if (p) { objc_setAssociatedObject(p,PV2DownloadProviderKey,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC); [p cancelLoading]; }
}
- (void)detach {
    self.generation++; self.downloading = NO; self.switchPending = NO;
    [self cancelRequest]; [self hidePanel];
    self.browsing = nil; self.session = nil; self.originalProvider = nil; self.asset = nil; self.result = nil;
}
- (void)render {
    if (!NSThread.isMainThread) return;
    NSString *size = self.readyLocal && self.localBytes ? PV2DownloadBytes(self.localBytes)
        : (self.originalBytes ? [@"原片 " stringByAppendingString:PV2DownloadBytes(self.originalBytes)] : @"点击按钮下载");
    if (self.downloading) {
        self.titleLabel.text = [NSString stringWithFormat:@"下载中 %.0f%%",floor(self.progress*100)];
        self.detailLabel.text = self.originalBytes ? [NSString stringWithFormat:@"≈%@ / 原片 %@",PV2DownloadBytes(PV2DownloadEstimatedBytes(self.originalBytes,self.progress)),PV2DownloadBytes(self.originalBytes)] : size;
    } else {
        NSInteger seconds = isfinite(self.duration) ? (NSInteger)self.duration : 0;
        self.titleLabel.text = [NSString stringWithFormat:@"%@ · %ld:%02ld",self.status ?: @"检查中",(long)(seconds/60),(long)(seconds%60)];
        self.detailLabel.text = size;
    }
    self.icon.image = [UIImage systemImageNamed:self.downloading ? @"arrow.down.circle" : (self.readyLocal ? @"checkmark.circle" : @"icloud.and.arrow.down")];
    self.button.enabled = self.supported && !self.downloading && !self.switchPending;
    self.button.accessibilityLabel = [NSString stringWithFormat:@"%@，%@",self.titleLabel.text,self.detailLabel.text];
}
- (void)ensurePanel {
    UIWindow *window = [[self.tile tilingView] window];
    if (!window) return;
    if (!self.panel) {
        self.panel = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemChromeMaterialDark]];
        self.panel.layer.cornerRadius = 13; self.panel.clipsToBounds = YES;
        self.panel.translatesAutoresizingMaskIntoConstraints = NO;
        self.titleLabel = [UILabel new]; self.detailLabel = [UILabel new];
        self.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightSemibold];
        self.detailLabel.font = [UIFont monospacedDigitSystemFontOfSize:11 weight:UIFontWeightMedium];
        self.titleLabel.textColor = UIColor.whiteColor; self.detailLabel.textColor = [UIColor.whiteColor colorWithAlphaComponent:0.8];
        self.icon = [UIImageView new]; self.icon.tintColor = UIColor.whiteColor; self.icon.contentMode = UIViewContentModeScaleAspectFit;
        UIStackView *labels = [[UIStackView alloc] initWithArrangedSubviews:@[self.titleLabel,self.detailLabel]];
        labels.axis = UILayoutConstraintAxisVertical; labels.spacing = 2;
        UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[self.icon,labels]];
        row.spacing = 8; row.alignment = UIStackViewAlignmentCenter; row.translatesAutoresizingMaskIntoConstraints = NO;
        [self.panel.contentView addSubview:row];
        [NSLayoutConstraint activateConstraints:@[[row.leadingAnchor constraintEqualToAnchor:self.panel.contentView.leadingAnchor constant:11],
            [row.trailingAnchor constraintEqualToAnchor:self.panel.contentView.trailingAnchor constant:-11],
            [row.topAnchor constraintEqualToAnchor:self.panel.contentView.topAnchor constant:8],
            [row.bottomAnchor constraintEqualToAnchor:self.panel.contentView.bottomAnchor constant:-8],
            [self.icon.widthAnchor constraintEqualToConstant:25],[self.icon.heightAnchor constraintEqualToConstant:25]]];
        self.button = [UIButton buttonWithType:UIButtonTypeCustom]; self.button.translatesAutoresizingMaskIntoConstraints = NO;
        [self.button addTarget:self action:@selector(tap) forControlEvents:UIControlEventTouchUpInside];
        [self.panel.contentView addSubview:self.button];
        [NSLayoutConstraint activateConstraints:@[[self.button.leadingAnchor constraintEqualToAnchor:self.panel.contentView.leadingAnchor],
            [self.button.trailingAnchor constraintEqualToAnchor:self.panel.contentView.trailingAnchor],
            [self.button.topAnchor constraintEqualToAnchor:self.panel.contentView.topAnchor],
            [self.button.bottomAnchor constraintEqualToAnchor:self.panel.contentView.bottomAnchor]]];
    }
    if (self.panel.superview != window) {
        [self.panel removeFromSuperview]; [window addSubview:self.panel];
        [NSLayoutConstraint activateConstraints:@[[self.panel.leadingAnchor constraintEqualToAnchor:window.safeAreaLayoutGuide.leadingAnchor constant:12],
            [self.panel.topAnchor constraintEqualToAnchor:window.safeAreaLayoutGuide.topAnchor constant:52],
            [self.panel.widthAnchor constraintLessThanOrEqualToConstant:210],
            [self.panel.heightAnchor constraintGreaterThanOrEqualToConstant:44]]];
    }
}
- (void)refresh {
    if (!NSThread.isMainThread) { __weak PV2DownloadController * weakControllerRef = self; dispatch_async(dispatch_get_main_queue(), ^{ [weakControllerRef refresh]; }); return; }
    if (![self.tile isActive] || ([self.tile respondsToSelector:@selector(isPresentationActive)] && ![self.tile isPresentationActive]) || ![[self.tile tilingView] window]) { [self detach]; return; }
    id browsing = [self.tile _browsingVideoPlayer], session = [browsing videoSession], provider = [session contentProvider];
    id nextAsset = [provider respondsToSelector:@selector(asset)] ? [provider asset] : nil;
    NSString *oldID = [self.asset respondsToSelector:@selector(localIdentifier)] ? [self.asset localIdentifier] : nil;
    NSString *newID = [nextAsset respondsToSelector:@selector(localIdentifier)] ? [nextAsset localIdentifier] : nil;
    BOOL assetChanged = PV2DownloadAssetChanged(oldID.UTF8String,newID.UTF8String);
    BOOL changed = provider != self.originalProvider || browsing != self.browsing || session != self.session || assetChanged;
    if (changed) {
        [self detach]; self.browsing = browsing; self.session = session; self.originalProvider = provider;
        self.asset = nextAsset;
        self.supported = PV2DownloadCompatible(provider);
        BOOL isVideo = [self.asset isKindOfClass:PHAsset.class] && ((PHAsset *)self.asset).mediaType == PHAssetMediaTypeVideo;
        self.supported = PV2DownloadShouldShowPanel(
            PV2CurrentOneUpTile == self.tile, [self.tile isActive],
            ![self.tile respondsToSelector:@selector(isPresentationActive)] || [self.tile isPresentationActive],
            isVideo, self.supported);
        if (!self.supported) { [self hidePanel]; return; }
        self.verifiedURL = nil; self.buildingItem = NO; self.localProbeFailed = NO;
        self.originalBytes = 0; self.localBytes = 0; self.readyLocal = NO; self.probing = NO; self.result = nil;
        self.duration = [self.asset isKindOfClass:PHAsset.class] ? ((PHAsset *)self.asset).duration : 0;
        self.status = [self.asset respondsToSelector:@selector(isInCloud)] && [self.asset isInCloud]
            ? @"iCloud · 点击下载" : @"本地状态待确认 · 点击检查";
        id currentResult = [provider respondsToSelector:@selector(loadingResult)] ? [provider loadingResult] : nil;
        NSURL *currentLocalURL = currentResult ? PV2DownloadLocalURL(currentResult) : nil;
        if (currentLocalURL) {
            NSNumber *currentSize = nil;
            [currentLocalURL getResourceValue:&currentSize forKey:NSURLFileSizeKey error:NULL];
            self.verifiedURL = currentLocalURL;
            self.localBytes = currentSize.unsignedLongLongValue;
            self.result = currentResult;
            self.readyLocal = self.localBytes > 0;
            self.status = self.readyLocal ? @"本地" : @"本地状态待确认 · 点击检查";
        }
        if (self.supported) {
            id asset = self.asset;
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
                unsigned long long bytes = 0;
                Method m = class_getInstanceMethod([asset class],NSSelectorFromString(@"originalFileSize"));
                if (m) {
                    char *r = method_copyReturnType(m);
                    if (r && (strcmp(r,"Q")==0 || strcmp(r,"q")==0)) {
                        @try { bytes = ((unsigned long long (*)(id,SEL))method_getImplementation(m))(asset,NSSelectorFromString(@"originalFileSize")); } @catch (__unused NSException *e) {}
                    }
                    free(r);
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (self.asset != asset || ![self isCurrent]) return;
                    self.originalBytes = bytes; [self render];
                });
            });
        }
    }
    if (!self.supported) { [self hidePanel]; return; }
    [self ensurePanel]; [self render];
    // Do not auto-probe media. Only an explicit tap starts a provider request.
}
- (void)tap {
    if (![self isCurrent] || !self.supported || self.downloading || self.switchPending) return;
    if (self.readyLocal && self.result && [self.result playerItem]) [self adoptResult:self.result];
    else if (self.readyLocal && self.verifiedURL) { self.buildingItem = YES; [self start:NO]; }
    else {
        BOOL cloudPlaceholder = [self.asset respondsToSelector:@selector(isInCloud)] && [self.asset isInCloud];
        self.buildingItem = NO;
        if (!cloudPlaceholder && !self.localProbeFailed) [self start:NO];
        else [self start:YES];
    }
}
- (void)start:(BOOL)network {
    if (![self isCurrent] || !self.supported) return;
    [self cancelRequest]; self.generation++;
    self.downloading = network; self.progress = 0; self.status = network ? @"下载中" : @"检查中";
    @try {
        id strategy = [NSClassFromString(@"PXDisplayAssetVideoContentDeliveryStrategy") new];
        [strategy setQuality:0]; // DSC quality 0 => PHVideoRequestOptions HighQuality(1).
        [strategy setIsNetworkAccessAllowed:network]; [strategy setIsStreamingAllowed:NO];
        id p = [[[self.originalProvider class] alloc] initWithAsset:self.asset mediaProvider:[self.originalProvider mediaProvider]
            deliveryStrategies:@[strategy] audioSession:[self.originalProvider audioSession] requestURLOnly:NO];
        if (!p) { self.downloading = NO; self.status = @"请求失败·重试"; [self render]; return; }
        self.requestProvider = p;
        // weak controller box prevents request-provider/controller retain cycle.
        objc_setAssociatedObject(p,PV2DownloadProviderKey,[NSHashTable weakObjectsHashTable],OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [(NSHashTable *)objc_getAssociatedObject(p,PV2DownloadProviderKey) addObject:self];
        [p beginLoadingWithPriority:0]; // verified intent=2, downloadPriority=1 (foreground).
    } @catch (NSException *e) { self.downloading = NO; self.status = @"请求失败·重试"; PV2Log(e.reason); }
    [self render];
}
- (void)progressFrom:(id)provider {
    if (provider != self.requestProvider || ![self isCurrent]) return;
    double progress = [(PV2NativeProvider *)provider loadingProgress];
    self.progress = PV2DownloadClampProgress(self.progress,progress);
    [self render];
}
- (void)resultFrom:(id)provider {
    if (provider != self.requestProvider || ![self isCurrent]) return;
    id result = [provider loadingResult]; if (!result) return;
    BOOL wasDownload = self.downloading; self.downloading = NO;
    NSURL *url = PV2DownloadLocalURL(result);
    if (url && !self.buildingItem && !wasDownload && ![result playerItem]) {
        self.verifiedURL = url;
        NSNumber *size = nil; [url getResourceValue:&size forKey:NSURLFileSizeKey error:NULL];
        self.localBytes = size.unsignedLongLongValue; self.readyLocal = self.localBytes > 0;
        self.status = self.readyLocal ? @"本地" : @"本地状态待确认 · 点击检查";
        self.localProbeFailed = !self.readyLocal; [self render]; return;
    }
    if (!url && self.buildingItem && [result playerItem] && ![result error])
        url = self.verifiedURL;
    if (!url || (self.buildingItem && ![result playerItem])) {
        self.localProbeFailed = !wasDownload && !self.buildingItem;
        self.status = wasDownload ? @"下载失败·重试" : @"iCloud · 点击下载";
        [self render]; return;
    }
    NSNumber *size = nil; [url getResourceValue:&size forKey:NSURLFileSizeKey error:NULL];
    self.verifiedURL = url;
    self.localBytes = size.unsignedLongLongValue; self.result = result;
    self.readyLocal = YES; self.progress = 1; self.status = @"本地";
    [self render];
    if (self.buildingItem) [self adoptResult:result];
    else if (wasDownload) { self.buildingItem = YES; [self start:NO]; }
}
- (void)adoptResult:(id)result {
    if (![self isCurrent] || !self.verifiedURL || ![result playerItem] || [result error] || self.switchPending) return;
    // Preserve provider's edited/slow-motion timeRangeMapper. Never create a bare item.
    id session = self.session, browsing = self.browsing, original = self.originalProvider;
    if (!PV2DownloadABI([original class],@"setLoadingResult:","v24@0:8@16") ||
        !PV2DownloadABI([browsing class],@"currentTime","{?=qiIq}16@0:8") ||
        !PV2DownloadABI([browsing class],@"seekToTime:completionHandler:","v48@0:8{?=qiIq}16@?40")) {
        self.status = @"已下载·切换待确认"; [self render]; return;
    }
    self.switchTime = [(PV2NativeBrowsing *)browsing currentTime];
    // Prevent an earlier streaming request from racing and replacing this complete local result.
    if ([original respondsToSelector:@selector(cancelLoading)]) [original cancelLoading];
    self.switchPending = YES; self.status = @"本地·切换中"; [self render];
    __weak PV2DownloadController * weakControllerRef = self;
    NSUInteger generation = self.generation;
    // Existing provider publishes to existing session; Photos handles mapping, audio,
    // video composition, outputs and current playback intent. No new session is installed.
    [original performChanges:^(id mutableProvider) { [mutableProvider setLoadingResult:result]; }];
    [self finishSwitch:result session:session browsing:browsing generation:generation attempt:0 weakController:weakControllerRef];
}
- (void)finishSwitch:(id)result session:(id)session browsing:(id)browsing generation:(NSUInteger)generation attempt:(NSUInteger)attempt weakController:(PV2DownloadController *)controller {
    if (generation != self.generation || ![self isCurrent] || self.session != session) return;
    // The desired position is captured immediately BEFORE publication (see adoptResult).
    if ([session currentPlayerItem] != [result playerItem]) {
        if (attempt >= 20) { self.switchPending = NO; self.status = @"已下载·切换待确认"; [self render]; return; }
        __weak PV2DownloadController * weakControllerRef = controller;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(0.1*NSEC_PER_SEC)),dispatch_get_main_queue(), ^{
            [weakControllerRef finishSwitch:result session:session browsing:browsing generation:generation attempt:attempt+1 weakController:weakControllerRef];
        });
        return;
    }
    CMTime time = self.switchTime;
    if (CMTIME_IS_NUMERIC(time) && CMTimeCompare(time,kCMTimeZero)>=0) {
        __weak PV2DownloadController * weakControllerRef = self;
        [browsing seekToTime:time completionHandler:^(BOOL finished) {
            dispatch_async(dispatch_get_main_queue(), ^{
                PV2DownloadController *c = weakControllerRef;
                if (c.generation != generation || ![c isCurrent]) return;
                c.switchPending = NO; c.status = finished ? @"本地" : @"本地·定位待确认"; [c render];
            });
        }];
    } else { self.switchPending = NO; self.status = @"本地"; [self render]; }
    PV2Log(@"provider completed: native session adopted local playerItem");
}
@end

static void PV2DownloadDetach(id tile);
static void PV2DownloadEndAll(void);
static void PV2DownloadSetCurrentTile(id tile) {
    if (PV2CurrentOneUpTile == tile) return;
    id old = PV2CurrentOneUpTile;
    PV2CurrentOneUpTile = tile;
    if (old && old != tile) PV2DownloadDetach(old);
}
static void PV2DownloadRefresh(id tile);
static void PV2DownloadRefreshOneUp(id oneUp) {
    id tile = [oneUp respondsToSelector:@selector(_currentContentTileController)] ? [oneUp _currentContentTileController] : nil;
    if ([tile isKindOfClass:NSClassFromString(@"PUVideoTileViewController")]) PV2DownloadRefresh(tile);
    else {
        id old = PV2CurrentOneUpTile; PV2CurrentOneUpTile = nil;
        if (old) PV2DownloadDetach(old);
        PV2DownloadEndAll();
    }
}
static void PV2DownloadRefresh(id tile) {
    if (![tile respondsToSelector:@selector(isPresentationActive)] || ![tile isPresentationActive]) {
        if (PV2CurrentOneUpTile == tile) { PV2CurrentOneUpTile = nil; PV2DownloadDetach(tile); }
        return;
    }
    PV2DownloadSetCurrentTile(tile);
    if (!PV2DownloadEnabled) return;
    __weak id weakTile = tile;
    dispatch_async(dispatch_get_main_queue(), ^{
        id strongTile = weakTile; if (!strongTile) return;
        PV2DownloadController *c = objc_getAssociatedObject(strongTile,PV2DownloadTileKey);
        if (!c && [strongTile isActive]) {
            c = [PV2DownloadController new]; c.tile = strongTile;
            objc_setAssociatedObject(strongTile,PV2DownloadTileKey,c,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            if (!PV2Downloads) PV2Downloads = [NSHashTable weakObjectsHashTable]; [PV2Downloads addObject:c];
        }
        [c refresh];
    });
}
static void PV2DownloadDetach(id tile) {
    PV2DownloadController *c = objc_getAssociatedObject(tile,PV2DownloadTileKey);
    if (NSThread.isMainThread) [c detach];
    else dispatch_async(dispatch_get_main_queue(), ^{ [c detach]; });
}
static void PV2DownloadEvent(id provider, BOOL result) {
    NSHashTable *box = objc_getAssociatedObject(provider,PV2DownloadProviderKey);
    if (!box) return;
    __weak PV2DownloadController *weakControllerRef = box.allObjects.firstObject;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (result) [weakControllerRef resultFrom:provider]; else [weakControllerRef progressFrom:provider];
    });
}
static void PV2DownloadEndAll(void) {
    void (^work)(void) = ^{ for (PV2DownloadController *c in PV2Downloads.allObjects) [c detach]; };
    if (NSThread.isMainThread) work(); else dispatch_async(dispatch_get_main_queue(),work);
}

