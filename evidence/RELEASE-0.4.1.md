# PhotosVideo2x 0.4.1 — 循环结束后进度条定位修复

## 录屏观察

用户文件 `video_A0D32722.mp4`：886×1920，约 6.968s，视频轨 60fps。已完整抽取 2fps 对照，并对 2.0–6.9s 的进度条区域按 60fps 抽帧。

- 视频画面在变化，但时间读数多次回到并停留在 0:00。
- 5.50–5.83s：手指拖动，UI 临时显示多个目标，最终约 0:50。
- 约 5.87s：松手后归零为 0:00 / −1:23，此后直到录屏末尾保持不动。
- 所以不是单纯“刷新率低”，也不等于视频画面已完全冻结。
- 本录屏不足以独立证明自然播放完整到结尾的时序；“自然结束和双击到末尾都会触发”来自用户反馈。

## 已证实的代码缺陷与 native 行为

0.4.0 只从 `PUBrowsingVideoPlayer.currentTime` 读时间。iOS 17.3 DSC 显示它转发 `PXVideoSession.currentTime`，后者是 state queue 的缓存读，不是 live AVPlayer 时间。`PXVideoSession.isReadyForSeeking` 也是缓存状态，由 wrapper/item 状态变化驱动更新。

循环播放涉及实际播放 item 与模板 item 的区分。固定的会话缓存及就绪硬门不足以保证切换后的进度操作。0.4.0 的单飞 seek 队列还缺超时释放：若 completion 不到，UI 会一直优先显示请求目标并堵住后续请求。

这些缺陷已经由源码/DSC 确认；本设备具体命中了哪个 native 回调丢失分支尚未动态抓取，不将推断写成真机调用证据。

## 0.4.1 修改

1. `LivePlayback.h`：使用已核验的 `ISWrappedAVPlayer _performPlayerTransaction:`，在原生 player queue 内取得 `_playerQueue_avPlayer`，读取实际 `AVPlayer.currentItem/currentTime`、item.duration 和当前就绪状态。没有全 window 扫描，没有把 wrapper 当 AVPlayer，没有直接读取私有 ivar。
2. 时间轴优先使用 live replica 快照，实际 item.ready 不再被 session 的旧 false 缓存否定。初始无快照时保留 native browser 路径，真实 player/item 未就绪仍不强制定位。
3. 有快照时，在相同原生队列上针对精确的当前 item 执行 AVPlayer 四参数 seek。旧 replica 被拒绝，item 更换时旧请求失效，必要的待提交最新拖动目标可接续。
4. seek 加 1s watchdog。超时释放 UI 单飞槽位并把最新 pending 请求交接；不无限重试，不伪造成功。每次请求带 serial/epoch，超时后的晚回调不会释放下一次新请求。finished=NO 也释放。
5. live 采样也有代次/超时保护，避免 native 事务没有执行时永久停止后续读取。owner/asset/tile/browser/session/wrapper/item 身份隔离保持。
6. 用 `CADisplayLink` 跟随屏幕允许的刷新率，目标 60fps/设备最高帧率；每 0.15s 才重做完整资格/布局检查，时间读数独立更新。隐藏/后台/退出注销刷新。没有更改源视频帧率、解码策略或 play/pause/rate，也不承诺每种设备实际满 120fps。

## 验证

- 提交 `211b3f2` + `bdcd143`。
- Actions run `38053738533`：全部成功；十组 PASS。
- 生产方法抽取回归：尾部缺 completion、后续拖动、晚回调不释放新请求、cancelled seek、缓存 0:00/live clock 9→10s、session false/live ready true。
- 生产 LivePlayback helper 回归：live replica A→B、实际 clock/duration、旧 item 拒绝、就绪和失败状态、无 rate/play 副作用。
- 原有高倍速/导出/循环/生命周期/双击边界回归保留。
- 本地 iOS arm64e strict syntax 检查通过。
- RootHide 包 genuine arm64e slice `0x80000002` 带签名，数据根 `Library` 无 `/var/jb`。
- SHA256 `4d78dec331750312374efc7f127a1dace32f972dea34f5ecd4d3f69e596b595e`。

## 真机验收边界

尚未安装 0.4.1 或重启 Photos。设备只读确认仍为 0.4.0。请安装后验证：接近末尾双击 +5、自然结束后继续循环、两种情况之后来回拖动并松手、暂停后拖动不自动播放、切换视频、隐藏再显示上下栏。特别检查慢动作/编辑视频是否仍与画面同步。CI 模拟回归不等于真实 Photos 已验收。
