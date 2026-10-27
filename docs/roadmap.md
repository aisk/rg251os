# 进度、已知问题和待办

2026-10-27 更新。硬件调研和设计决定见 [hardware-research.md](hardware-research.md)，这里记录还没验证的、已知的问题、以后可以做的事，以及开发过程中踩过的坑。

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
- 系统信息页（ROM 列表的设置里和游戏菜单里的 System info）：打开时日志里有 `sysinfo:` 开头的几行，看找到了哪些温度传感器。2026-10-19 实机上 tsadc 的两个 thermal zone（`soc-thermal`、`gpu-thermal`）都能看到，GPU 节点关掉了 `gpu-thermal` 也照样注册。没有 hwmon（没开 `CONFIG_HWMON`），RK817 的驱动不报电池温度。还要确认 CPU 占用、频率、内存和剩余空间的数字是否合理。
- 菜单里的两次确认：游戏菜单的 Restart with、Power off 和 ROM 列表设置里新加的 Power off 现在和 Restart game 一样要 3 秒内按两次 A。从设置关机时日志里是 `power off: settings`，看看保存和关机流程是否和长按电源键一样。
- 退出时的即时存档：退出、关机、长按电源键关机、换核心、休眠后再开同一个游戏，日志里应该有 `state: wrote N bytes to <rom>.<core>.state (原因): serialize X ms, write Y ms` 和下次的 `state: loaded ... resuming`，画面接着上次的位置。qemu 下（PC 硬盘）序列化 0.3 到 3 ms，写 416 到 520KiB 约 9 到 27 ms，真卡上的写入时间要看日志，关机路径上它在 "Saving..." 画面之后、`dmesg-poweroff.txt` 之前。还要看 Restart game 后的 `state: removed`，以及用电脑换掉 `.sav` 后是否显示 `skipped: the save data changed`。另外看看从即时存档恢复时游戏内的 RTC（宝可梦等）是否正常。
- 日期和时间（ROM 列表设置里的 Date & time，`clock.c`）：开机日志里的 `clock: system ..., RTC ...` 是否读到了 RTC（内核新开了 `CONFIG_RTC_INTF_DEV`，要有 `/dev/rtc0`），没设过时是否带 `looks never set`。设置一次后看 `clock: setting`、`clock: settimeofday(...)`、`clock: RTC_SET_TIME on /dev/rtc0` 和 `clock: RTC reads back` 几行是否都是 ok，系统信息页的 Clock 和 ROM 列表标题栏的时间是否跟着变。然后关机再开、隔一段时间再开，看时间有没有走对；再试一次电池完全耗尽后充电开机，看 RTC 是否还保持（RK817 的 RTC 应该和 PMIC 一起由电池供电，没有单独的纽扣电池）。rk808 驱动里有 Rockchip 日历 11 月 31 日的换算，跨 11 月底设时间时留意 read back 的结果。最后用宝可梦宝石系列确认游戏里的时间。
- 关机时卸载 GBA 分区：关机时 `frontend.txt` 里应该有 `power off: no child processes left`（`dmesg-poweroff.txt` 里也有），最后一行是 `powering off: unmounting /mnt/data`。卸载本身的结果（`power off: /mnt/data unmounted`，或者 `umount ...: ..., remounting read-only`）只进内核日志，关机后就没了，所以要看下次开机 `frontend.txt` 开头的 `mounted /dev/mmcblk0p3 ...` 一行，`clean` 说明上次干净地卸载了，`dirty bit set` 说明没有。注意 Linux 挂载时发现脏标记就不会再清它，以前的版本一直没卸载过，所以卡现在是脏的，要先在 Windows 上修复一次（或者 `fsck.fat -a`），之后正常关机几次，再插到 Windows 上看还会不会提示修复，开机日志是否一直是 `clean`。
- 亮度下限：背光调到最低时屏幕什么都看不见，现在用户亮度最低是背光范围的 25%（`BRIGHTNESS_MIN`），菜单里是 25%、30%、40% 一直到 100%，卡上存的更低的值开机时会被提上来，日志里是 `settings: brightness N% raised to the minimum 25%`。开机日志的 `backlight ...: ... user minimum 25% = N` 一行是对应的原始值。要确认最低一档还看得清。如果太亮或者还是太暗，只改这一个常量。
- 界面字体换成了 JetBrains Mono Regular（20px，灰度抗锯齿，按原生分辨率画），不再是 8x8 放大两倍。JetBrains Mono 20px 的字宽正好是 12px，字格和之前的 Spleen 12x24 一样，布局没有变。要看实机上抗锯齿的文字是否清晰（尤其灰色字在深色背景上和选中条上的边缘）、大小是否合适，Date & time 的两倍大数字是直接放大的，看是否发虚。各个画面（ROM 列表、设置、游戏菜单、游戏内设置、System info、Date & time、出错和关机画面）有没有文字重叠或被截掉。现在每个文字像素要和底色混合，看翻页、跑马灯滚动和 System info 每秒刷新时有没有可见的卡顿。ROM 列表一页从 14 行变成 9 行（行高 28px），名字从 29 列变成 38 列。游戏菜单的电量挪进了菜单框里游戏名的右边。System info 一页正好放 11 行，B:back 挪到了标题栏右边，如果温度一行放不下要换行，最后的 Kernel 一行会被挤掉。开机日志里有一行 `ui: font 12x24, ...` 记录这些尺寸。
- 前端界面改成了页面栈（`ui.c`），游戏菜单和两个设置页改成共用的行表（`menu.c` 的 `struct row`）。selftest 的截图逐像素没有变化，但页面跳转换了实现，要在实机上点一遍：ROM 列表按 SELECT 进设置，进 System info 再按两次 B 回到列表，Date & time 进出一次，设置里的 Power off 按两次 A。游戏中 SELECT+START 打开菜单，进 Settings 后 B、B 回到游戏，在游戏内设置页和 System info 页直接按 SELECT+START 也应该回到游戏。再试 Restart game、换核心后的 Restart with、Frame blend（现在在 Settings 面板里，mGBA 下显示 gpSP only，不能改）和 Quit to ROM list（回到原来选中的 ROM）。换页后新页面在同一帧就画出来，以前有几处要等下一帧。打开 Debug log 时每次换页有一行 `ui: push ...`、`ui: pop ...` 或 `ui: back to game ...`。日志格式也有变化，设置一行变成 `settings: ... volume=255 brightness=50 ...` 这样的 `key=value`，mGBA 下切 Frame blend 是 `core: gpsp_frame_mixing=enabled kept for gpSP, mGBA does not have it`。
- 跳帧（游戏菜单的 Frameskip，按游戏）：先用 mGBA 跑龙珠大冒险（Dragon Ball Advanced Adventure），在屏幕上有敌人时试 1 和 2，再切到 gpSP 试 Auto。日志里开局有一行 `core: game settings for <核心>: ... frameskip=<值> (<发给核心的键>)`，菜单里改时有 `core: <键>=<值>, applied live`，gpSP 的 Auto 会有 `core: audio buffer status callback registered` 和 `core: asks for at least 128 ms of audio latency`。每局结束有 `core: frameskip <值>: N of M frames skipped (X%), buffer status ...: avg A%, lowest L%, U under one frame`，`perf summary` 和 perf.csv 的 `skipped` 列也有跳帧数，帧耗时 `work_avg` 只算画出来的帧。要看：Auto 在平时（不卡的场景）跳帧数是否接近 0、平均占用是否在 50% 左右（低于 33% 就会一直跳，说明换算不合适），重负载时跳了多少、是否还有 `audio: underrun`；固定间隔下跳帧比例是否正好是 1/2、2/3、3/4；跳帧时 CPU 频率（perf.csv 各频率的行）是否没有因为跳帧被拉低；画面看起来是否比卡顿时顺，声音是否不再断；mGBA 下 Auto 是否显示 "Auto: gpSP only" 并且左右切换时跳过 Auto。Frame blend 挪进了游戏菜单的 Settings 面板（第 2 行），确认在那里能开关，mGBA 下显示 gpSP only。
- 充电诊断（`battery.c`，不用开 Debug log）：开机日志里应该有 `charger: PMIC registers from /sys/kernel/debug/regmap/<设备>/registers, 7 bytes per line, ...`（如果是 `charger: no ...`，说明 debugfs 或 regmap 没找到，寄存器就不记了），然后是 `charger: start: ...` 和 `charger: start: regs ...` 两行。测试步骤：先用电池开机，打开 System info；插上充电器，等 30 秒，记下屏幕上电池一行显示什么（Charging 还是 Not charging，电流正负）；拔掉等 10 秒再插上，重复 5 次，每次记下有没有充上；再在关机状态下插上充电器后开机一次；最后把 `log/` 拷回来，连同每次的结果一起发给我。要看的日志：每次插拔后有一行 `charger: change (<变了什么>): ...`，紧跟一行 `charger: change (...): regs e4=... e5=... e6=... eb=... f0=... (PLUG_IN_STS N, CHRG_STS ..., BAT_EXS N, CHRG_EN N, in: VLIM on 4.5 V ILIM on 1.50 A, out: 4.20 V 2.00 A), read in X ms`，之后 30 行 `charger: +N s: ...` 和一行 `charger: samples done, 30 in 30 s, read time avg ...`（每次读的耗时，游戏里应该远低于 1 ms）。没充上的那几次重点看 PLUG_IN_STS 是 0 还是 1，CHRG_STS 是不是 CHRG_OFF，e5 的 VLIM 和 ILIM 是否还是 on 4.5 V 和 1.50 A，`IRQs plug_in N plug_out M` 有没有加一，以及 online 有没有变。PLUG_IN_STS=0 是第一种情况，PLUG_IN_STS=1 而 e5 变了是第二种。插拔会把休眠唤醒，休眠前后还各有一组 `charger: sleep #N before: ...` 和 `charger: sleep #N after: ...`。
- 菜单只在有变化时重绘：以前 ROM 列表和菜单每个 vblank 都整屏重绘（约 60 次每秒），现在停着不动时不重绘，主循环睡在 `poll()` 里。打开 Debug log 后，在菜单里每 60 秒和离开菜单（进游戏、休眠、关机）时有一行 `ui: menus for X s (...): N redraws (R/s), M updates, P% asleep in poll; bat ...`。要看：停在 ROM 列表、设置或游戏菜单不动时 R 应该接近 0（选中的名字太长在滚动时，滚动期间约 60，两端各停 1 秒；System info 每秒最多 3 次，Date & time 每秒 1 次），P 应该接近 100%；菜单按键、方向键按住连发是否和以前一样跟手；走马灯速度是否和以前一样（每秒 60 像素，开头和结尾各停 1 秒）；两次确认的 "Press A to ..." 是否 3 秒后恢复；标题栏时间是否按分钟跳；电量变化、插拔充电线、按音量键时菜单上的电量和 Volume 是否马上更新；ROM 列表里有 ROM 在算 CRC 时名字是否还会改过来。省电效果：同一行里的 `bat` 有功率（mW），和以前版本 Debug log 里 ROM 列表停着时的 `battery:` 行（每分钟一行）比较，同样亮度下看功率差多少。selftest 里模拟的结果是：菜单静止 10 秒 0 次重绘，滚动时 0.5 秒 30 次，System info 3 秒 9 次。

## 已知问题

- 声音还有轻微的爆音。还没定位。先看每局游戏结束时的摘要行里 underrun 次数。如果是 0，最可能是重采样，现在 32768Hz 到 48000Hz 用的是线性插值（`resample.c`），可以换成多相 FIR。另外 gpSP 的音频模拟本身比 mGBA 粗糙，可以拿同一个游戏两个核心对比。
- 界面字体只有 ASCII（JetBrains Mono，`app/frontend/font.c`），中文文件名显示不了。
- 不支持 zip 格式的 ROM。
- 硬件时钟出厂时可能没设过，用到 RTC 的游戏（宝可梦宝石系列的树果和潮汐等）时间会不对，要先在设置的 Date & time 里设一次，开机日志会标出没设过的情况。没有时区，输入的时间按 UTC 存进系统时钟和 RTC，游戏看到的就是输入的时间，日志和 perf.csv 里写的 UTC 其实是这个时间。
- 插拔充电线也会把机器从休眠唤醒（PMIC 中断），现在唤醒后一律亮屏。
- 插上充电器经常显示 Not charging，电池也确实没充进去，同样的线和充电器要拔插几次才开始充。指示灯颜色会变，但那是 VBUS 直接点亮的另一颗灯，和 PMIC 无关。旧日志里 boot-0003、0004、0019 是插着线开机的：电池电流为正（+0.09 到 +0.15A），电压在 4.12V 以上，但 rk817-charger 的 ONLINE=0，STATUS 是 Not charging。看驱动（`rk817_charger.c`）：电流不为负时 STATUS 取 CHRG_STS（0xeb）的 6:4 位，CHRG_OFF 就报 Not charging；ONLINE 只在 probe 时从 SYS_STS（0xf0）的 PLUG_IN_STS 读一次，之后只跟着 plug_in 和 plug_out 两个中断变；输入限制 CHRG_IN（0xe5，4.5V、1.5A）只在 probe 和拔出中断里写（驱动注释说拔线时这个寄存器会复位），插入时不写；充电使能位驱动从来不写；这些值每 8 秒刷新一次。BSP 用来检测充电器的 dc_det GPIO（gpio0 PB3）主线和我们的 DTS 都没用。怀疑两个方向：一是 PMIC 没认到插入（PLUG_IN_STS=0，充电状态机停在 OFF），二是 CHRG_IN 被复位，要等下一次拔出中断才写回。新加的 `charger:` 日志用来区分这两种，见待实机验证。

## 待办和想法

大致按优先级排。

- 太阳传感器亮度（`mgba_solar_sensor_level`），Boktai 系列需要，只有 mGBA 支持。加一个按 ROM 的核心选项现在是 `settings.h` 的 `struct rom_settings` 加字段，`settings.c` 的 `rom_fields` 和 `core.c` 的 `rom_options` 各加一项（一个设置可以对应几个键，核心没有的值在 `values` 里写 NULL；默认值不是 0 时再改 `rom_defaults`），要在菜单里改就在 `menu.c` 定义一行（`.option = 1`）加进行表。游戏菜单已经放满 11 行（`menu.h` 的 `IG_ROWS`，再多框就超出屏幕），新的行放进游戏菜单的 Settings 面板。
- mGBA 的 Auto 跳帧：这个版本的 mGBA libretro 只有固定间隔。要做的话可以由前端按音频缓冲占用实时改 `mgba_frameskip`。
- 跳帧时前端还是每帧把上一帧画面重新缩放旋转一遍（blit），这部分开销没有省下来。
- 前端自己实现帧间混合（smart 模式只混合闪烁的像素，以及 mGBA 下也能用）。
- 中文文件名：GBA 分区已经用 utf8 挂载，还缺一套 CJK 字体。
- 记住上次所在的文件夹和选中的 ROM，跨开机恢复。现在只在本次开机内保留。
- zip 格式 ROM。
- 震动卡带（`gpsp_rumble`），需要前端驱动 pwm-vibrator。
- mGBA 的性能：多线程视频、LTO、PGO。
- 开机速度优化，见 hardware-research.md 里“对精简和快速开机的初步判断”。2026-10-18 的一次实机日志（debug 开着）：内核启动后 0.81 s 进入前端，0.86 s 挂好 GBA 分区，2.43 s 才打开日志，2.49 s 出 ROM 列表。挂载后到打开日志之间这 1.5 s 还没分析。bootloader 阶段的耗时没量过，要接串口或者用高速摄像来测。
- 接上 UART2 以后再查主线 U-Boot。
- 把 RG351P 的 DTS 整理后提交到主线内核。

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
