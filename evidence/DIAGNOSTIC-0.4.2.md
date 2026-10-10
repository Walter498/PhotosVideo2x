# 0.4.2~diag1 — 未解决循环尾部冻结的诊断包

用户确认 0.4.1 没有修复。SSH 核实安装包版本为 0.4.1。本轮不宣称根因已定或问题已解决。

## 本轮调查

- 真机 SSH 可用，Frida server 17.9.10 存活。官方 musl 客户端与 SDK 已接通远程进程枚举。
- 对 Photos 的运行时 attach 超时；没有获得有效 JS 播放追踪，不将连接服务成功当作附加成功。
- 尝试临时启动 Photos。附加超时后终止该 Photos 进程并重开，未重启 SpringBoard、未重签设备 JB 文件。
- 没有安装本轮新包，也未改动或导出照片/视频。

## 诊断包改动

保留 0.4.1 播放逻辑，只增加有上限的诊断：
- 启动记录实际 dylib 构建版本与 PID。
- 当前选中 OneUp 页面每秒记录缓存时间、live item 时间/时长/就绪标志、wrapper/item 指针、seek 单飞/待发/epoch/serial。
- 只遍历当前 OneUp 视图内的 AVPlayerLayer，对比实际显示层的播放器与绑定播放器。不遍历所有 window，不用于改变播放或推断第一个 player 为目标。
- seek 提交、完成/取消、超时分别记录。
- 日志不含媒体名称、路径、URL、PHAsset ID 或画面内容。
- 后台串行写日志，1MiB 轮转。路径 `/var/mobile/tmp/PhotosVideo2x-runtime.log`（RootHide 只读查看也可能需要 `/rootfs/private/var/mobile/tmp/PhotosVideo2x-runtime.log`）。

## 下一步需要的真机证据

安装诊断包并重启 Photos，打开同一个问题视频：接近结尾时右边双击 +5，然后拖动一次并松手；再测试自然结束后拖动。用户报告复现后读取上述日志，从以下分支定位：
1. 是否实际加载新 dylib；
2. live sampling 是否执行/返回；
3. 可见 AVPlayerLayer 与 wrapper 的 player/item 是否一致；
4. 进度条到底未提交 seek、seek 取消、seek 超时还是 seek 完成但读错时间源；
5. 循环更换 item 后代次/就绪状态是否恢复。

静态及模拟测试只验证诊断代码编译与原行为回归，不构成问题已经解决的证据。
