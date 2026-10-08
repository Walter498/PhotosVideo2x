#import "../DownloadPolicy.h"
#import <stdlib.h>
static void Check(BOOL ok,const char *message) { if (!ok) { fprintf(stderr,"FAIL %s\n",message);exit(1); } }
int main(void) { @autoreleasepool {
 Check(!PV2DownloadResultIsLocal(NO,NO,YES,123,YES),"remote URL is not local");
 Check(!PV2DownloadResultIsLocal(NO,YES,YES,0,YES),"empty file is not local");
 Check(!PV2DownloadResultIsLocal(NO,YES,NO,100,YES),"unreadable file is not local");
 Check(!PV2DownloadResultIsLocal(YES,YES,YES,100,YES),"error is not completion");
 Check(PV2DownloadResultIsLocal(NO,YES,YES,100,YES),"readable completed file is local");
 Check(!PV2DownloadCallbackIsCurrent(YES,NO,YES,YES,YES),"old asset callback ignored");
 Check(!PV2DownloadCallbackIsCurrent(YES,YES,NO,YES,YES),"old session callback ignored");
 Check(!PV2DownloadCallbackIsCurrent(YES,YES,YES,YES,NO),"cancelled request ignored");
 Check(!PV2DownloadCallbackIsCurrent(NO,YES,YES,YES,YES),"inactive tile ignored");
 Check(PV2DownloadClampProgress(0.6,0.4)==0.6,"progress monotonic");
 Check(PV2DownloadClampProgress(0.1,2)==1,"progress capped");
 Check(PV2DownloadClampProgress(0.3,NAN)==0.3,"NaN ignored");
 Check(PV2DownloadEstimatedBytes(1000,0.25)==250,"estimated bytes bounded");
 Check(PV2DownloadShouldShowPanel(YES,YES,YES,YES,YES),"current active one-up video shows panel");
 Check(!PV2DownloadShouldShowPanel(NO,YES,YES,YES,YES),"library preheat tile never shows panel");
 Check(!PV2DownloadShouldShowPanel(YES,YES,NO,YES,YES),"non-presented tile never shows panel");
 Check(!PV2DownloadShouldShowPanel(YES,YES,YES,NO,YES),"photo hides video panel");
 Check(!PV2DownloadShouldShowPanel(YES,YES,YES,YES,NO),"unsupported provider hides panel");
 Check(PV2DownloadAssetChanged("asset-A","asset-B"),"left/right asset switch resets state");
 Check(!PV2DownloadAssetChanged("asset-A","asset-A"),"same asset retains state");
 puts("PASS: file validation, callback isolation, estimates, one-up/video visibility, asset reset");
} return 0; }
