# 0.4.3 rc1 — 基于真机负时间证据的循环 seek 坐标修正

## 已取得的真机事实

用户安装 0.4.2~diag1 后复现；SSH 取回 `/rootfs/private/var/mobile/tmp/PhotosVideo2x-runtime.log`（8894 bytes，82行），boot build=0.4.2~diag1、pid59235。

- 所有记录到的 seek 都 finished=1，没有 seekTimeout。
- wrapper 的 live item 与可见 AVPlayerLayer item 始终一致，player/item status=1/1，rate=1。
- 片长 658.499s。第一次定位到 658.499s 完成后循环到 0.085/0.102s，随后仍前进到 1.107/1.132s。
- 下一次拖动定位到 375.274s 后，实际显示层播放器时间约 -283.260s（差值约一整圈）。后续定位 0→-658.682s、128.336→-530.318s、440.770→-217.036s。
- 进度条读取函数把负时间当无效，退到 browser/session 缓存 0，因而显示冻结；播放器 seek 不是未执行。

以上证据推翻此前把丢失回调/就绪缓存当作这次故障主因的推断。旧超时保护可保留，但不能据此宣称已定位此次根因。

## 定向系统核查

提取同机 AVFCore 和 CoreMedia DSC 镜像。静态证据：
- ISAVPlayer 没有覆盖 currentTime 或 seek；不要把负时间归咎于它覆写了这两个方法。
- `AVPlayer currentTime` 转发 `AVPlayerItem currentTime`；后者选 `_copyFoldedTimebase`。
- `AVPlayer seekToTime:toleranceBefore:toleranceAfter:completionHandler:` 0x1993ff610 只把传入时间原样交给当前 item；改为 item.seek 等价，不是修复。
- `AVPlayerItem currentUnfoldedTime` 0x19941f4d0 返回未折叠时间。
- public item.timebase 是 folded proxy；`_copyProxyUnfoldedTimebase` 返回 retained 未折叠 proxy，ABI `^{OpaqueCMTimebase=}16@0:8`。
- CoreMedia 循环时钟更新 anchor/offset；不能每次用 epoch0 的“本轮秒数”直接当底层 seek 坐标，也不能只把显示负数加一个片长。

## 本包改动

只改定位目标转换，不改变默认循环、play/pause/rate、媒体或导出功能：
- 在原生 player queue 内针对精确当前 item，通过 `CMSyncConvertTime(displayedTarget, foldedTimebase, unfoldedTimebase)` 换算后执行原来的四参数 seek。
- 保留当前 CMTime epoch，保持容差和回调语义。
- 时间基临时未准备好时不把错误坐标发给有效循环 item；普通非循环视频仍可使用原坐标。
- 未读取或写入 AVPlayerItem 的私有 ivar；retained unfold proxy 成对释放。
- 保留诊断并增加 folded/raw/origin 的完整 epoch，便于真机验证第二圈、第三圈及暂停拖动，不伪造显示时间。

## 验证与边界

使用真正的 CoreMedia 时间基测试生产换算 helper：0偏移、+658.499s、负 folded 时间、非单位相对速率、反向换算、暂停、epoch、未准备好时间基、CF引用计数。原有十组回归保留。

这是 0.4.3~rc1 候选修复，最终 CI/包哈希另补。修正方向来自真机证据和系统实现，但新包尚未真机安装验收，不能称故障已根治。安装需重启 Photos，重新测试 +5 到末尾→拖动，以及自然结束→拖动，并核对日志中 folded 时间不再落入负区间。
