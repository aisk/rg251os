# 进度、已知问题和待办

2026-10-19 更新。硬件调研和设计决定见 [hardware-research.md](hardware-research.md)，这里记录还没验证的、已知的问题、以后可以做的事，以及开发过程中踩过的坑。

## 开发流程

- 没有串口线。每次迭代是烧卡、上机、把 GBA 分区里的 `log/` 拷回来分析。所以前端的日志和 dmesg 转存一定要保留，新功能也要带上足够的日志，尽量做到一次上机就能定位问题。
- 卡上的 Debug log 开关默认关。排查问题前先在设置里打开。
- 重新烧镜像会把 GBA 分区恢复成 64MiB 的空分区，烧之前要先备份 `saves/`。
- 主机上的验证：`make image` 和 `make frontend-test`（qemu-user 跑 selftest、headless 游戏、核心切换、休眠的浅睡眠路径、分区扩展的断电恢复）。要求零 warning。
- 开机速度暂时不优化，先把功能做完。

## 已在真机验证

启动、显示、按键、关机、电量读数（RK817 库仑计，读到过 78%、3.8V）、耳机检测、喇叭和耳机出声（simple 和 adaptive 两种模式）、存档、uclamp 调频、两个核心。超级马里奥 GBA 在 50% 背光下整机约 1.5 到 1.7W。mGBA 跑马里奥已经接近满负荷，所以默认核心改成了 gpSP。龙珠大冒险（Dragon Ball Advanced Adventure）在 mGBA 下屏幕上有敌人就卡，切到 gpSP 后目测不卡了。

## 待实机验证

- 电源键休眠：是否真的进了深度睡眠（日志 `sleep #N: result: suspended to RAM`），唤醒后屏幕、USB 手柄、SD 卡、声音是否正常，唤醒时有没有 pop 声，深睡时指示灯是否熄灭。
- 首次开机扩展 GBA 分区：`sdgrow:` 日志，扩展后 Windows 和 macOS 能否读写。
- ROM 名称识别：名字是否正确，冷缓存下扫描耗时（`romdb: scanned N ROMs in X ms`），大 ROM 首次算 CRC 的耗时。
- ROM 列表的子文件夹：真卡（vfat）上文件夹的识别和排序，进出文件夹和退出游戏后的选中项，每层扫描的耗时（`roms: folder /<路径>: N ROMs, M folders (X ms)`），子文件夹里需要算 CRC 的 ROM 改名是否正常，`config/romdb-cache.txt` 里子文件夹的条目是否带路径并且在别的文件夹扫描后保留。注意存档和单游戏设置只按文件名，不同文件夹里的同名 ROM 会共用。
- `color=auto` 的效果是否合适。现在是 2004 年及以前用 `gba`，2005 年以后用 `sp101`，年份未知用 `off`。按 2005 年分界是推断出来的，没有出处。例外表 `color_exceptions[]` 是空的，遇到不合适的游戏再加。
- Frame blend 开启后的实际开销（用 perf.csv 对比开关时的 `work_avg`）。
- adaptive 音频去掉开机校准以后，第一局开头有没有爆音或 underrun。
- 开机时 LED 心跳闪烁，前端起来后接管。
- 系统信息页（ROM 列表的设置里和游戏菜单里的 System info）：打开时日志里有 `sysinfo:` 开头的几行，看找到了哪些温度传感器。按 DTS 和内核配置应该有 tsadc 的两个 thermal zone（`soc-thermal`、`gpu-thermal`），没有 hwmon（没开 `CONFIG_HWMON`），RK817 的驱动不报电池温度。另外确认 CPU 占用、频率、内存和剩余空间的数字是否合理。

## 已知问题

- 声音还有轻微的爆音。还没定位。先看每局游戏结束时的摘要行里 underrun 次数。如果是 0，最可能是重采样，现在 32768Hz 到 48000Hz 用的是线性插值（`resample.c`），可以换成多相 FIR。另外 gpSP 的音频模拟本身比 mGBA 粗糙，可以拿同一个游戏两个核心对比。
- 界面字体只有 ASCII（`app/hwtest/font8x8.h`），中文文件名显示不了。
- 不支持 zip 格式的 ROM。
- 硬件时钟可能从来没设过，用到 RTC 的游戏（宝可梦宝石系列的树果和潮汐等）时间不对。
- 插拔充电线也会把机器从休眠唤醒（PMIC 中断），现在唤醒后一律亮屏。
- 卡上旧版本只在显式选了非默认核心时才写 `core=`，默认核心改成 gpSP 以后，当初手动选过 mGBA 但文件里没记下来的 ROM 会变成 gpSP。

## 待办和想法

大致按优先级排。

- 跳帧（`gpsp_frameskip` 自动模式加阈值，`mgba_frameskip`），按 ROM 设置，给重负载的游戏用。
- 太阳传感器亮度（`mgba_solar_sensor_level`），Boktai 系列需要，只有 mGBA 支持。
- 设置时间的菜单项，配合 RTC 游戏。
- 前端自己实现帧间混合（smart 模式只混合闪烁的像素，以及 mGBA 下也能用）。
- 中文文件名：vfat 挂载用 utf8，再加一套 CJK 字体。
- 记住上次所在的文件夹和选中的 ROM，跨开机恢复。现在只在本次开机内保留。
- zip 格式 ROM。
- 震动卡带（`gpsp_rumble`），需要前端驱动 pwm-vibrator。
- mGBA 的性能：多线程视频、LTO、PGO。
- ROM 列表和菜单现在每个 vblank 都整屏重绘一次（约 60 Hz），列表停着不动时也一样。可以改成只在有变化时重绘（按键、走马灯、电量更新、CRC 算完改名），省一点电。
- 开机速度优化，见 hardware-research.md 里“对精简和快速开机的初步判断”。2026-10-18 的一次实机日志（debug 开着）：内核启动后 0.81 s 进入前端，0.86 s 挂好 GBA 分区，2.43 s 才打开日志，2.49 s 出 ROM 列表。挂载后到打开日志之间这 1.5 s 还没分析。bootloader 阶段的耗时没量过，要接串口或者用高速摄像来测。
- 接上 UART2 以后再查主线 U-Boot。
- 把 RG351P 的 DTS 整理后提交到主线内核。
- hwtest 已经基本不用了，可以删掉，前提是先把 `font8x8.h` 挪进 `app/frontend`。

## 踩过的坑

这些问题都花了不少时间，以后遇到类似现象可以先往这些方向查。

- **音频爆音、播放速率像是两倍、完全没声音**，最后都是同一个原因：tinyalsa 的 `pcm_writei` 走 SYNC_PTR 时会把内核里的 `appl_ptr` 回退，设备一直在播旧数据或空数据。修法是每次写完调用 `pcm_avail_update()` 重新同步（`audio.c` 里的 `dev_sync()`）。这之前怀疑过采样率、codec 时钟、I2S 配置，都不是原因，但顺手把 I2S1 的时钟固定在了 12.288MHz。
- **RK817 的 Playback Mux 默认是 HP**，RG351P 的喇叭也挂在 HP 这一路上。`mixer.txt` 里可以用 `speaker:` 或 `headphones:` 前缀的行来改。
- **RG351P 的耳机检测是低电平有效**，继承自 351M 的 DTS 是高电平有效，表现是插拔耳机时喇叭和耳机切换颠倒。
- **mGBA 的 DMA 日志**每秒能刷出约 7000 行，造成过 1.1 秒的卡顿，还把性能数据算大了。现在核心日志每局有上限（INFO 100 条、WARN 200 条）。
- **从菜单回到游戏后 CPU 卡在 816MHz、游戏变慢**：菜单期间负载低，schedutil 把频率降了下去，回到游戏后跟不上。现在游戏中用 uclamp_min 按帧耗时给频率设下限（`cpuctl.c`）。
- **日志被覆盖**：用户经常在出问题后重启再拷卡，所以改成每次开机一个目录，保留最近 20 个。
- **GPU 电源域一直开着**：要在 DTS 里把 gpu、vpu、vpu_mmu 都关掉，genpd 才会断电。
- **串口控制台**会让开机多卡约 0.9 秒，所以去掉了 console，加了 quiet。
- **换内核版本后 .config 没有重新合并**：kbuild 的 syncconfig 会悄悄给新符号取默认值，被改名或隐藏的符号也不报错。现在 `mk/linux.mk` 把内核版本记进 `.fragments`，版本一变就重跑 allnoconfig 加 merge_config。检查符号没生效的 grep 同时认新版 merge_config.sh 的 `WARNING:` 前缀和 `differs:` 写法。
- **主线 U-Boot 在 RG351P 上没有任何输出**，所有组合都试过，见 hardware-research.md。
