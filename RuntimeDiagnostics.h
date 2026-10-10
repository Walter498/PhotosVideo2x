#pragma once
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
// Diagnostics deliberately contain no URLs, asset IDs, names or media content.
// Layer traversal is scoped to the selected OneUp view, not every app window.
static const void *PV2DiagnosticStampKey=&PV2DiagnosticStampKey;
static void PV2Diag(NSString *message) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once,^{queue=dispatch_queue_create("PhotosVideo2x.diagnostic",DISPATCH_QUEUE_SERIAL);});
    NSString *line=[NSString stringWithFormat:@"%.3f %@\n",NSProcessInfo.processInfo.systemUptime,message];
    dispatch_async(queue,^{
        NSString *path=@"/var/mobile/tmp/PhotosVideo2x-runtime.log";
        NSFileManager *fm=NSFileManager.defaultManager;
        @try {
            if ([[fm attributesOfItemAtPath:path error:NULL] fileSize]>1048576) {
                [fm removeItemAtPath:[path stringByAppendingString:@".1"] error:NULL];
                [fm moveItemAtPath:path toPath:[path stringByAppendingString:@".1"] error:NULL];
            }
            if (![fm fileExistsAtPath:path]) [fm createFileAtPath:path contents:nil attributes:nil];
            NSFileHandle *file=[NSFileHandle fileHandleForWritingAtPath:path];
            [file seekToEndOfFile];[file writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];[file closeFile];
        } @catch (__unused NSException *e) {}
    });
}
static double PV2DiagSeconds(CMTime t) {return CMTIME_IS_NUMERIC(t) ? CMTimeGetSeconds(t) : -1;}
static void PV2DiagLayers(CALayer *layer,UIView *host,NSMutableArray *rows,int *budget) {
    if (!layer || --*budget<0) return;
    if ([layer isKindOfClass:AVPlayerLayer.class]) {
        AVPlayerLayer *video=(AVPlayerLayer *)layer;AVPlayer *p=video.player;
        CGRect r=[layer convertRect:layer.bounds toLayer:host.layer];
        if (p && CGRectIntersectsRect(host.bounds,r)) {
            [rows addObject:[NSString stringWithFormat:@"layer=%p player=%p item=%p t=%.3f d=%.3f rate=%.2f status=%ld/%ld rect=%.0f,%.0f,%.0f,%.0f",video,p,p.currentItem,PV2DiagSeconds(p.currentTime),PV2DiagSeconds(p.currentItem.duration),p.rate,(long)p.status,(long)p.currentItem.status,r.origin.x,r.origin.y,r.size.width,r.size.height]];
        }
    }
    for (CALayer *child in layer.sublayers) {if (*budget<=0)break;PV2DiagLayers(child,host,rows,budget);}
}
static void PV2DiagTimeline(PV2TimelineController *controller) {
    UIViewController *owner=[controller owner];
    if (!owner || owner!=PV2VisibleOneUp || !owner.view.window) return;
    double now=NSProcessInfo.processInfo.systemUptime;
    NSNumber *stamp=objc_getAssociatedObject(controller,PV2DiagnosticStampKey);
    if (stamp && now-stamp.doubleValue<1) return;
    objc_setAssociatedObject(controller,PV2DiagnosticStampKey,@(now),OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    PV2LivePlaybackSnapshot *live=[controller liveSnapshot];
    id session=[controller sessionToken];id browsing=[controller browsing];
    PV2TimelineSeekState state=[controller seekState];
    PV2Diag([NSString stringWithFormat:@"tick owner=%p browser=%p session=%p wrapper=%p cached=%.3f ready=%d liveitem=%p livet=%.3f lived=%.3f liveready=%d sampling=%d drag=%d flight=%d pending=%d target=%.3f epoch=%lu serial=%lu",owner,browsing,session,[session videoPlayer],PV2DiagSeconds([(PV2NativeBrowsing *)browsing currentTime]),[session isReadyForSeeking],live.item,PV2DiagSeconds(live.time),PV2DiagSeconds(live.duration),live.ready,[controller samplingLive],[controller dragging],state.inFlight,state.hasPending,state.hasPending?state.pendingTarget:state.emittedTarget,state.epoch,(unsigned long)[controller seekSerial]]);
    NSMutableArray *layers=[NSMutableArray new];int budget=300;
    PV2DiagLayers(owner.view.layer,owner.view,layers,&budget);
    PV2Diag([NSString stringWithFormat:@"visiblePlayers %@",[layers componentsJoinedByString:@" | "]]);
}
