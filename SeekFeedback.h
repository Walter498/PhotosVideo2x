#pragma once
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import "GestureCore.h"
static const void *PV2SeekFeedbackKey=&PV2SeekFeedbackKey;
static void PV2ShowSeekFeedback(UIViewController *owner,double delta) {
    if (owner!=PV2VisibleOneUp || !owner.view.window) return;
    UIView *old=objc_getAssociatedObject(owner,PV2SeekFeedbackKey);[old removeFromSuperview];
    UIVisualEffectView *view=[[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemChromeMaterialDark]];
    view.userInteractionEnabled=NO;view.layer.cornerRadius=24;view.clipsToBounds=YES;
    UILabel *label=[UILabel new];label.text=delta>0 ? @"+5 秒" : @"−5 秒";
    label.font=[UIFont monospacedDigitSystemFontOfSize:16 weight:UIFontWeightSemibold];label.textColor=UIColor.whiteColor;label.textAlignment=NSTextAlignmentCenter;
    label.frame=CGRectMake(0,0,80,48);[view.contentView addSubview:label];
    CGFloat width=owner.view.bounds.size.width;
    view.frame=CGRectMake((delta>0 ? width*0.75 : width*0.25)-40,owner.view.bounds.size.height*0.5-24,80,48);
    [owner.view addSubview:view];objc_setAssociatedObject(owner,PV2SeekFeedbackKey,view,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    __weak UIViewController *weakOwner=owner;
    [UIView animateWithDuration:0.2 delay:0.5 options:UIViewAnimationOptionBeginFromCurrentState animations:^{view.alpha=0;} completion:^(__unused BOOL finished){
        [view removeFromSuperview];
        UIViewController *o=weakOwner;
        if (o && objc_getAssociatedObject(o,PV2SeekFeedbackKey)==view) objc_setAssociatedObject(o,PV2SeekFeedbackKey,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }];
}
static void PV2SeekFeedbackLeave(UIViewController *owner) {
    UIView *view=objc_getAssociatedObject(owner,PV2SeekFeedbackKey);[view removeFromSuperview];
    objc_setAssociatedObject(owner,PV2SeekFeedbackKey,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
