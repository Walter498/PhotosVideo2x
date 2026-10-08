#pragma once
#import <Foundation/Foundation.h>
#import <math.h>
static inline double PV2DownloadClampProgress(double oldValue, double newValue) {
    return isfinite(newValue) ? MAX(oldValue,MIN(1.0,MAX(0.0,newValue))) : oldValue;
}
static inline BOOL PV2DownloadResultIsLocal(BOOL hasError, BOOL fileURL, BOOL readable, unsigned long long bytes, BOOL hasItem) {
    return !hasError && fileURL && readable && bytes>0 && hasItem;
}
static inline BOOL PV2DownloadCallbackIsCurrent(BOOL active, BOOL sameBrowser, BOOL sameSession, BOOL sameProvider, BOOL sameRequest) {
    return active && sameBrowser && sameSession && sameProvider && sameRequest;
}
static inline unsigned long long PV2DownloadEstimatedBytes(unsigned long long originalBytes,double progress) {
    double p=PV2DownloadClampProgress(0,progress);
    return (unsigned long long)((long double)originalBytes*p);
}
static inline BOOL PV2DownloadShouldShowPanel(BOOL currentOneUp,BOOL active,BOOL presentationActive,
    BOOL videoAsset,BOOL compatibleProvider) {
    return currentOneUp && active && presentationActive && videoAsset && compatibleProvider;
}
static inline BOOL PV2OwnerAllowsPanel(BOOL visible, BOOL attached, BOOL selectedTile, BOOL video) {
    return visible && attached && selectedTile && video;
}
static inline BOOL PV2ChromeAllowsPanel(BOOL ownerValid, BOOL topBar, BOOL bottomBar) {
    return ownerValid && topBar && bottomBar;
}
static inline BOOL PV2IsNetworkDownload(NSInteger pass, BOOL pending) {
    return pass == 2 && pending;
}
static inline BOOL PV2DownloadAssetChanged(const char *oldIdentifier,const char *newIdentifier) {
    if (!oldIdentifier || !newIdentifier) return oldIdentifier != newIdentifier;
    return strcmp(oldIdentifier,newIdentifier)!=0;
}
