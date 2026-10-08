# Photos 视频长按 2x 分析

## 当前结论

目标设备为 iPhone16,2、iOS 17.3、RootHide。目标进程是 `com.apple.mobileslideshow`，主程序位于：

`/rootfs/Applications/MobileSlideShow.app/MobileSlideShow`

系统代码不在 Photos 应用包内，而是在 dyld shared cache 中。已从设备 DSC 提取并验证以下 arm64e Mach-O：

- `PhotosPlayer`：647647 bytes，3900 级别符号，包含 `ISWrappedAVPlayer`
- `PhotosUICore`：32551832 bytes，172056 级别符号，包含 `PXVideoSession`
- `PhotosUIPrivate`：14989848 bytes，81679 级别符号，包含 `PUBrowsingVideoPlayer`、`PUVideoTileViewController`、`PUOneUpViewController`

文件产物：

- `dsc/extracted/PhotosPlayer`
- `dsc/extracted/PhotosUICore`
- `dsc/extracted/PhotosUIPrivate`
- `dsc/index/*.nm`

## 播放链路

相册单图浏览视频的静态链路为：

```text
PUOneUpViewController
  -> PUVideoTileViewController
    -> PUBrowsingVideoPlayer
      -> PXVideoSession
        -> ISWrappedAVPlayer
          -> AVPlayer
```

关键证据：

```text
PUOneUpViewController _videoPlayerAtIndexPath:layout:
PUOneUpViewController _browsingVideoPlayerDidPlayToEndTime:
PUVideoTileViewController _browsingVideoPlayer
PUVideoTileViewController setVideoView:
PUVideoTileViewController setVideoSession:
PUBrowsingVideoPlayer avPlayer
PUBrowsingVideoPlayer videoSession
PUBrowsingVideoPlayer setVideoSession:
PXVideoSession videoPlayer
PXVideoSession setDesiredPlayState:
PXVideoSession _updateAVPlayerPlayState
ISWrappedAVPlayer rate
ISWrappedAVPlayer setRate:
```

`PUCuratedLibraryVideoPlaybackController`、`PXGVideoPlayerView` 主要属于网格内联播放，不是本需求的首要目标。若后续要求网格预览也支持 2x，再单独扩展。

## 速率控制关键发现

`PXVideoSession _updateAVPlayerPlayState` 的 arm64e 反汇编显示：当内部播放状态进入播放分支时，会对 `videoPlayer` 写入浮点值 `1.0`：

```asm
fmov    s0, #1.00000000
bl      _objc_msgSend$setRate:
```

因此只在长按开始时对 `AVPlayer` 写入 `2.0`，很可能会被 Photos 自己的状态同步改回 `1.0`。全局 hook `AVPlayer setRate:` 又会影响 Photos 中所有播放器，范围过大。

推荐在 `ISWrappedAVPlayer setRate:` 层拦截，并且只对当前长按 token 所绑定的 wrapper 生效：

```text
系统 setRate:1.0
  -> ISWrappedAVPlayer hook
    -> 当前 wrapper 有 active boost token
      -> 实际写入 2.0
```

`PXVideoSession` 的正常暂停分支会调用 `pause`，因此暂停/停止请求必须原样透传，并标记本次长按结束时不得恢复旧的正速率。

已确认 `PXVideoSession` 写入的是浮点 rate，草稿按 `float` 声明；真正安装前仍应在设备运行时用 `method_getTypeEncoding` 复核 `rate` 与 `setRate:` 的编码，避免私有 ABI 变化造成数值类型错配。

## 手势挂载点

首选入口：

```text
PUVideoTileViewController setVideoView:
```

原因：

- 当前视频承载 view 在这里可获得
- 方法属于单个视频 tile，不需要扫描整个 Photos window
- 可以在 view 替换时移除旧 recognizer，避免复用时叠加
- tile 可通过 `_browsingVideoPlayer` getter 重新解析当前播放对象

识别器参数建议：

```text
UILongPressGestureRecognizer
minimumPressDuration = 0.35s
allowableMovement = 18pt
numberOfTouchesRequired = 1
cancelsTouchesInView = NO
```

只接受触点位于视频 view 左侧 25% 或右侧 25%。`shouldReceiveTouch:` 和 `shouldBegin` 都要做区域判断；不能等到 `.began` 才过滤，否则会提前参与系统分页、缩放或现有长按手势竞争。

控制条、按钮及其祖先控件的触摸应排除。第一版不应全局允许所有 recognizer 同时识别，也不应让系统翻页 recognizer 等待自定义长按失败。真机确认冲突后，再针对具体 recognizer 配置失败优先级。

## Token 状态机

每次长按绑定一个具体 `ISWrappedAVPlayer`，不能只把状态放在 tile 上。token 至少保存：

```text
wrapper
initialRate
latestExternalPositiveRate
active
sawStopOrPause
internalWriteDepth
```

开始：

1. 根据 tile 当前的 `PUBrowsingVideoPlayer -> videoSession -> videoPlayer` 重新解析 wrapper。
2. 读取当前 rate；当前为暂停或非有限值时不启动。
3. 保存 `initialRate`。
4. 关联 token 到 wrapper。
5. 立即内部写入 `2.0`，不能只设置 active 标志。

运行中：

- 外部正速率请求：保存为 `latestExternalPositiveRate`，实际仍写入 `2.0`
- `0` 或负数：原样透传，并标记暂停/停止；结束时不恢复旧正速率
- 非有限值：原样透传，不强制转成 `2.0`
- 内部写入：用 guard 避免被自己的 hook 再次改写

结束：

1. 先清除 active 和 wrapper 关联，阻止后续系统写回继续被强制到 2x。
2. 如果本次收到过暂停/停止/资源切换，不恢复正速率。
3. 若仍是同一个 wrapper 且仍允许播放，内部恢复最新外部正速率；没有更新时恢复 `initialRate`。
4. `.ended`、`.cancelled`、`.failed`、tile 复用、view 替换、session 替换、后台/scene 失活都走同一个幂等结束函数。

特别要避免：长按前视频正在播放，长按期间系统暂停，松手后插件把旧的 `1.0` 写回并意外重新播放。

## 推荐 hook 集合

第一版建议：

```text
PUVideoTileViewController setVideoView:
    安装/移除长按 recognizer

PUVideoTileViewController setVideoSession:
    session 替换时结束旧 token

PUVideoTileViewController becomeReusable:
    tile 离屏复用时结束旧 token

ISWrappedAVPlayer setRate:
    只拦截带 active token 的当前 wrapper
```

后台/scene 失活可使用通知结束 token，不必先增加更多私有方法 hook。`dealloc` 只做兜底，避免在对象销毁阶段重新查询复杂播放链。

不建议第一版 hook：

```text
AVPlayer setRate:
PXVideoSession _updateAVPlayerPlayState
PUOneUpViewController _handleTouchGesture:
```

前两个范围或副作用较大，最后一个当前主要处理视觉搜索/主体分析触摸，不是视频速率入口。

## 当前限制

静态 DSC 分析已经完成，尚未完成以下运行时验证：

- `rate`/`setRate:` 的真实 type encoding
- `setVideoView:` 是否覆盖所有单图视频 view 的创建与复用
- `PUBrowsingVideoPlayer` session 更换时序
- 自定义长按与系统分页、缩放、现有长按的实际互斥关系
- 视频播放、暂停、切换、播完、退出、后台场景
- RootHide 当前设备上的 dylib 加载状态

因此当前结果是“已定位并形成可实现方案”，不是“已经安装验证完成”。

## 验证顺序

1. 在第三方视频 App 上验证 dylib 的签名和构造函数日志。
2. 只注入 `com.apple.mobileslideshow`，打开普通视频。
3. 中央区域长按：播放行为不变。
4. 左/右侧长按：保持 2x，松手恢复原速。
5. 长按中系统重复写 `1.0`：实际仍为 2x。
6. 长按中点暂停：松手不能自动恢复播放。
7. 长按中翻页、拖动退出、时间轴拖动、切换资源：必须结束旧 token。
8. 进入后台、锁屏、回到前台：没有旧 wrapper 残留 2x。
9. 检查未参与长按的视频 wrapper 完全不受影响。
