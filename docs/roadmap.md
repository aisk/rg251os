# 进度、已知问题和待办

2026-11-03 更新。硬件调研和设计决定见 [hardware-research.md](hardware-research.md)，这里记录还没验证的、已知的问题、以后可以做的事，以及开发过程中踩过的坑。

## 开发流程

- 没有串口线。每次迭代是烧卡、上机、把 GBA 分区里的 `log/` 拷回来分析。所以前端的日志和 dmesg 转存一定要保留，新功能也要带上足够的日志，尽量做到一次上机就能定位问题。
- 卡上的 Debug log 开关默认关。排查问题前先在设置里打开。
- 重新烧镜像会把 GBA 分区恢复成 64MiB 的空分区，烧之前要先备份 `saves/`。
- 只改了前端或核心（内核、dtb 和 bootloader 都没变）时不用重烧整张卡。把 `build/rootfs.ext4` 用 dd 写到 p2（从扇区 98304 开始，16 MiB，和 `ROOTFS_SIZE` 一样大），GBA 分区里要更新的内容文件直接拷过去，BOOT 和 GBA 分区都不动，存档、设置和日志都还在。用户的读卡器插在 192.168.1.17 的树莓派上，卡在那边一般是 `/dev/sdb`，每次写之前都要先用 `lsblk` 确认。
- 主机上的验证：`make image` 和 `make frontend-test`（qemu-user 跑 selftest、headless 游戏、核心切换、休眠的浅睡眠路径、开机恢复的几次连续开机、分区扩展的断电恢复、用 ffmpeg 生成的测试片段跑视频播放）。要求零 warning。
- 开机速度暂时不优化，先把功能做完。

## 已在真机验证

启动、显示、按键、关机、电量读数（RK817 库仑计，读到过 78%、3.8V）、耳机检测、喇叭和耳机出声（simple 和 adaptive 两种模式）、存档、uclamp 调频、两个核心。超级马里奥 GBA 在 50% 背光下整机约 1.5 到 1.7W。mGBA 跑马里奥已经接近满负荷，所以默认核心改成了 gpSP。龙珠大冒险（Dragon Ball Advanced Adventure）在 mGBA 下屏幕上有敌人就卡，切到 gpSP 后目测不卡了。

- adaptive 音频去掉开机校准以后：2026-10-19 到 10-25 的 17 次开机，第一局都没有 underrun。唯一的一次 underrun 是休眠唤醒后马上出现的，已经改了（见待实机验证的电源键休眠）。
- 系统信息页：2026-10-19 到 10-24 的日志里 CPU 占用、频率、内存（MemTotal 约 993 MiB）、温度（tsadc 的两个 thermal zone `soc-thermal` 和 `gpu-thermal`，39 到 46 °C）和剩余空间（15114 MiB 里 14898 MiB 空闲）都合理。GPU 节点关掉了 `gpu-thermal` 也照样注册。没有 hwmon（没开 `CONFIG_HWMON`），RK817 的驱动不报电池温度。
- ROM 名称识别：20 个 ROM 冷缓存扫描 44 到 50 ms，名字都对。
- 视频播放第一次实机测试（2026-10-30，566x320 和 568x320、30fps、MP2 48 kHz 立体声）：跳转（约 35 ms）、退出再进接着播、Fit 和 Crop 现场切换都正常。但是不流畅，1296 MHz 下解码加转换一帧平均 13 到 18 ms，最大 22 到 36 ms，全在一次 retro_run 里做，放不进 16.7 ms 的 vblank。约 30 秒里有几百次 missed vblank、80 到 230 次 underrun，perf 的 work 平均 16 到 23 ms，p99 25 到 38 ms，频率一直在 1296 MHz，整机 1.5 到 2.1 W，和玩游戏差不多。qemu 里估的约 4 ms 差得很远，qemu 的耗时不能拿来推算真机。已经把解码挪进单独的线程，结果见下面。

下面是 2026-10-30 到 11-03 的 14 次开机（boot-0001 到 0014，Debug log 都关着，boot-0001 的前端比后面的旧一点，视频跳转的日志还不带按键名，也没有 OSD）。

- 开机恢复：8 次恢复都直接进了游戏，`(power off: in-game menu)` 和 `(power off: power key held 2 s)` 两种记录都有。跑满 5 秒后是 `record removed (the resumed game ran 5 s)`，5 秒内退出是 `record removed (quit)`，在列表或设置里关机后下次开机是 `resume: none`。手柄在开始等它之后 143 到 236 ms 出现（有几次超过 200 ms），游戏第一帧在前端启动后 864 到 1140 ms，比预计的 1.2 到 1.4 s 快。boot-0008 那次第一帧比平时晚了约 70 ms，开局记了 missed 8 和一次 underrun，其余几次都没有。
- 退出时的即时存档：退出、关机、休眠、Restart with 换核心、视频的 L/R 和自动播放都写了，下次都是 `state: loaded ... resuming`。gpSP 和 mGBA 的序列化 1.2 到 6.8 ms，写入一般 31 到 71 ms（视频的只有 64 字节，约 12 ms），偶尔很慢，见已知问题。Restart game 后的 `state: removed (restart game)` 有。`skipped: the save data changed` 在 boot-0008 出现过一次，是银河战士从 mGBA 换回 gpSP，mGBA 玩的时候写过 `.sav`，跳过是对的。
- 日期和时间：每次开机都读到了 RTC，没有 `looks never set`。boot-0012 设了一次，`settimeofday`、`RTC_SET_TIME` 和读回都是 ok，之后两次开机时间接着走。用户输入的是本地时间，比原来的 UTC 快约 9 小时，和已知问题里的没有时区一致。
- 设置里关机：`power off: settings` 和长按电源键走同样的流程，约 165 ms 到卸载。游戏菜单的 Restart game、Restart with 和 Power off 也都用过。每次正常关机都有 `no child processes left` 和最后一行 `powering off: unmounting /mnt/data`。
- 休眠唤醒的声音：boot-0008 有一次深度睡眠（IRQ 27，435 ms 恢复），声音等唤醒后第一帧上屏（311 ms）才开始，之后没有 underrun，不过唤醒 2.4 s 后就关机了。
- 唤醒后手柄的重新枚举：前端的修改有效，内核建好新节点后约 1 ms 就打开了。剩下约 2.2 s 是内核的问题，见已知问题。
- 开局和关掉游戏菜单后的 missed vblank：gpSP 的游戏都是 0 或个位数，不再把菜单里的时间算进去。
- ROM 的第一次 CRC：boot-0001 缓存是空的，6 个 ROM（约 50 MiB，含 16 MiB 的最终幻想战略版）`CRCs done, 21 ROMs in 1247.4 ms`，约 40 MB/s，列表在 2.536 s 就出来了，没有等 CRC。
- ROM 列表的子文件夹：识别、进出文件夹和退出视频后的选中项都对。根目录 21 个 ROM 和 2 个文件夹，开机第一次扫描 38 到 55 ms，之后 2 到 3 ms；文件夹第一次 10 到 15 ms，之后约 1 ms。子文件夹里只放了视频，所以子文件夹里算 CRC 和 `romdb-cache.txt` 的路径还没测到。
- 视频解码线程：0 dropped、0 runs late、0 runs dry，late 每局 0 到 2 帧，`ring at retro_run` 的 avg 2.67 到 2.87，解码加转换平均 15 到 17.5 ms（其中转换和拷贝 7.5 到 9.5 ms），播放时工作线程占墙钟 45 到 53%，perf 的 work 平均 5 到 6.6 ms，underrun 0（只有 boot-0001 那版有过一次）。不卡了。但没有比玩游戏省电，见已知问题。
- 视频的按键和 OSD：左右单次跳转（`shown after` 2 到 4 次）、L/R、播完自动播放（`video: end: next video ..., playing it`）、B 开关 OSD 都正常，只有一个视频的文件夹里按 L/R 是 `... staying`，自动播放后退出时列表选中的是最后播的那一集。L/R 切换时冷读文件约 0.3 s，再切回来约 0.1 s。OSD 每帧平均 1.5 到 2.2 ms，最大 2.3 到 5.4 ms，比预计的 1 ms 以内高。切换后的 missed 和没测到的部分见待实机验证。

## 待实机验证

- 格斗 POC 已接入标准 libretro 核心、480×320 软件渲染、即时存档与自动四场景压力测试；原生 ASan/UBSan、实际共享库回调测试、qemu 前端加载/坏包/恢复/休眠/菜单重启回归通过。`make image`、完整 `make frontend-test` 零 warning 通过，36010 帧原生长测试也通过。首轮真机 boot-0015 已返回：基础两人和额外 8 精灵可约 60fps，额外 24/48 精灵即使 Max 也降至约 30/20fps 并有大量 underrun；核心合成约 5.49/9.46/18.74/32.91ms，旋转另约 3ms。之后已换成 FIGHTP 格式（现为 FIGHTP11）并导入 KOF 2002 Terry（两人都是 Terry，P2 蓝色配色），渲染改为 span 复制，前端旋转改为分块转置，并已有判定框、Hitstop/硬直/推开和站蹲八个普通技，以及输入缓冲、Terry 的必杀技、选人闯关、11 个角色（KOF02 七人、KOF03 八神庵和舞、月华真之介、侍魂 V Special 霸王丸，大多有必杀技）、数值体系第一步，以及舞台、镜头、打击效果、音效和音乐，主机回归通过；boot-0016 真机上正常对战核心渲染约 1.4 ms、blit 约 1.5 ms，零 missed vblank 和 underrun，详见格斗文档。boot-0017 带舞台和音乐，对战中仍然零 missed vblank 和 underrun（run 均值约 2.5 ms，音频约 0.65 ms），掉帧和爆音只出在选人移光标和开打时重建 RGB565 缓存（每次 20 到 55 ms）。已改为按需分片展开（每次 run 最多约 4.8 万像素加场景缺的帧，选人只展开 P1，开打的展开放进新加的 0.75 秒对阵卡），主机回归逐像素一致，待上机确认没有 missed vblank。另加了追加输入（FIGHTP07、FGSTATE7）：庵的葵花三段、May Lee 龙尾脚后的三个方向分支。之后加了开局 READY/FIGHT（按住的键不会在开打时出招）、真连段计数（N HITS，日志记每局最长连段）、暂停和选人界面的出招表（从包数据生成，字体加了箭头，FIGHTP08 起带招式名）和 Easy/Normal/Hard 难度（核心选项 `fight_difficulty` 是初始值，状态 FGSTAT11），主机回归通过；设备前端菜单没有核心选项，所以在选人界面按 R 切换难度。倒地和起身已加（FIGHTP09、FGSTAT12，扫腿和多数必杀技倒地，起身前无敌，AI 会压起身；起身帧暂时借 KO 和蹲姿帧，图集里的真帧还没切）。通常投已加（FIGHTP10、FGSTAT13，前或后加 HP/HK，KOF 第 1 帧生效、6 帧内 HP/HK 拆投、起身 8 帧不可投，侍魂不能拆；动画暂用站 HP 帧做通用抓取，AI 会投也会拆）。跑动和后撤步已加（FIGHTP11、FGSTAT14，66 跑、44 后撤，KOF 2002 的 3/10~/3 和 3/18/3，后撤步不能被投，侍魂体系可按住冲刺、后撤前 2 帧无敌；动作暂用 walk_forward 加速和 walk_back/jump 帧生成，AI 按难度跑近和后撤）。选人界面按 L 切游戏模式（FGSTAT15，状态 256 个字）：Arcade 闯关、Survival（血量带入下一场，每胜回复 15%，记胜场和本次开机最高纪录）、Practice（选 P1 和木桩，R 切站/蹲/全防/CPU，血量不归零、连段后回满，显示连段伤害）；暂停时 B 回选人。下一次上机要测真实角色负载、新 blit 计时和十分钟稳定性，结果与剩余项见 [POC 实现和验证](fighter.md)。
- 格斗阵容加了 Waku Waku 7 的 Arina、Politank-Z 和 Rage of the Dragons 的 Sonia（共 18 人），主机 `fighter-test` 通过，待上机看手感和 Politank-Z 的大体型；指令、按键归属和数值是估计，见各清单 notes。
- 格斗语音补了一轮：色、莉姆露露（都是 Tin 的 MUGEN 包）和 Arina（NHK）有了必杀技、攻击、受击和 KO 语音，Mature 的 Death Row 三段、Vice 的 Deicide 和葵花后两段按招式名补上，Vice 原来挂在 deicide 上的其实是 Mayhem 的语音，已改回。Politank-Z 和 Sonia 找不到来源，只有通用音效。声音包约 13.1 MiB（音效 ADPCM 879 到 1026 KiB）。待上机试听，尤其 Arina 的 KO 声（包里没有 11,0，用的是输掉一局后的语音）和 Arina Tornado 借用的攻击喊声，映射见 `assets/fighter/sound/README.md`。
- 格斗舞台从 2 张增加到 7 张（KOF 2002 China、KOF 2003 Hong Kong、SVC 的守护犬寺和 Power Generation Room、月华剑士庭院，都取自 KOF EXUM 的 MUGEN 移植），多数角色有了主场，舞台可以指定自己的战斗音乐。每张展开后都不超过 Korea，常驻 RGB565 不变，包大了约 1.9 MB。待上机看画面、地面位置和换舞台时的展开耗时。
- 格斗数值按原作调查（本机 `build/fighter/source/MECHANICS.md`）做了一轮修正：指令匹配加了半圈 41236/63214 和 421；KOF 体系的硬直、hitstop、走速和跳跃改用 KOF 2002 的通用值；Chin、Vice、Mature、Shermie、Chris、八神庵、舞、Zero、霸王丸、莉姆露露、真之介的指令和帧数据按 SRK、Dream Cancel、Mizuumi 改正，Mature 加了 Death Row 三段，Vice 加了真正的 Outrage。主机 `fighter-test` 通过，待上机看手感（整体比以前慢、出招后破绽更大）。明细见[格斗文档](fighter.md)第 6 项。

- 电源键休眠：实机上两次休眠都进了深度睡眠（`result: suspended to RAM, mem (deep)`，唤醒中断是 IRQ 27，427 ms 恢复），唤醒的那一下按键被正确忽略，SD 卡和声音都恢复了。唤醒时声音等第一帧上屏再启动、前端马上打开重新枚举的手柄，这两处 2026-11 已经在真机上确认（见上面）。还要看：深睡 1.95 小时掉了 2%（约 35 mA，对 RK3326 来说偏高），这批日志里只有一次不到 1 秒的休眠，没有新数据；屏幕、pop 声和指示灯要用眼睛看，声音晚启动以后唤醒时的 pop 声还在不在；有用户说用电源键唤醒时偶尔亮一下又灭掉，要再按一次，还没在日志里看到过，要开着 Debug log 再看。
- 首次开机扩展 GBA 分区：`sdgrow:` 日志，扩展后 Windows 和 macOS 能否读写。
- ROM 列表的子文件夹：识别、选中项和扫描耗时已经确认（见上面），但子文件夹里只放过视频。还要放几个需要算 CRC 的 ROM 进子文件夹，看改名是否正常，`config/romdb-cache.txt` 里的条目是否带路径并且在别的文件夹扫描后保留，以及退出子文件夹里的游戏后的选中项。注意存档和单游戏设置只按文件名，不同文件夹里的同名 ROM 会共用。
- `color=auto` 的效果是否合适。现在是 2004 年及以前用 `gba`，2005 年以后用 `sp101`，年份未知用 `off`。按 2005 年分界是推断出来的，没有出处。例外表 `color_exceptions[]` 是空的，遇到不合适的游戏再加。
- Frame blend 开启后的实际开销（用 perf.csv 对比开关时的 `work_avg`）。
- 开机时 LED 心跳闪烁，前端起来后接管。
- 即时存档：从即时存档恢复时游戏内的 RTC（宝可梦宝石系列等）是否正常，其余已经确认（见上面）。
- 日期和时间：设置、读回和关机后接着走已经确认（见上面）。还要看系统信息页的 Clock 和 ROM 列表标题栏的时间是否跟着变；电池完全耗尽后充电开机，RTC 是否还保持（RK817 的 RTC 应该和 PMIC 一起由电池供电，没有单独的纽扣电池）；rk808 驱动里有 Rockchip 日历 11 月 31 日的换算，跨 11 月底设时间时留意 read back 的结果；最后用宝可梦宝石系列确认游戏里的时间。
- 关机时卸载 GBA 分区：关机前的日志都对了（见上面），但卸载本身的结果（`power off: /mnt/data unmounted`，或者 `umount ...: ..., remounting read-only`）只进内核日志，关机后就没了，所以要看下次开机 `frontend.txt` 开头的 `mounted /dev/mmcblk0p3 ...` 一行，`clean` 说明上次干净地卸载了，`dirty bit set` 说明没有。Linux 挂载时发现脏标记就不会再清它，以前的版本一直没卸载过，所以卡一直是脏的，boot-0001 到 0014 全是 `dirty bit set`。要先在 Windows 上修复一次（或者 `fsck.fat -a`），之后正常关机几次，再插到 Windows 上看还会不会提示修复，开机日志是否一直是 `clean`。
- 亮度下限：背光调到最低时屏幕什么都看不见，现在用户亮度最低是背光范围的 25%（`BRIGHTNESS_MIN`），菜单里是 25%、30%、40% 一直到 100%，卡上存的更低的值开机时会被提上来，日志里是 `settings: brightness N% raised to the minimum 25%`。开机日志的 `backlight ...: ... user minimum 25% = N` 一行是对应的原始值。要确认最低一档还看得清。如果太亮或者还是太暗，只改这一个常量。
- 界面字体换成了 JetBrains Mono Regular（20px，灰度抗锯齿，按原生分辨率画），不再是 8x8 放大两倍。JetBrains Mono 20px 的字宽正好是 12px，字格和之前的 Spleen 12x24 一样，布局没有变。要看实机上抗锯齿的文字是否清晰（尤其灰色字在深色背景上和选中条上的边缘）、大小是否合适，Date & time 的两倍大数字是直接放大的，看是否发虚。各个画面（ROM 列表、设置、游戏菜单、游戏内设置、System info、Date & time、出错和关机画面）有没有文字重叠或被截掉。现在每个文字像素要和底色混合，看翻页、跑马灯滚动和 System info 每秒刷新时有没有可见的卡顿。ROM 列表一页从 14 行变成 9 行（行高 28px），名字从 29 列变成 38 列。游戏菜单的电量挪进了菜单框里游戏名的右边。System info 一页正好放 11 行，B:back 挪到了标题栏右边，如果温度一行放不下要换行，最后的 Kernel 一行会被挤掉。开机日志里有一行 `ui: font 12x24, ...` 记录这些尺寸。
- 前端界面改成了页面栈（`ui.c`），游戏菜单和两个设置页改成共用的行表（`menu.c` 的 `struct row`）。selftest 的截图逐像素没有变化，但页面跳转换了实现，要在实机上点一遍：ROM 列表按 SELECT 进设置，进 System info 再按两次 B 回到列表，Date & time 进出一次，设置里的 Power off 按两次 A。游戏中 SELECT+START 打开菜单，进 Settings 后 B、B 回到游戏，在游戏内设置页和 System info 页直接按 SELECT+START 也应该回到游戏。再试 Restart game、换核心后的 Restart with、Frame blend（现在在 Settings 面板里，mGBA 下显示 gpSP only，不能改）和 Quit to ROM list（回到原来选中的 ROM）。换页后新页面在同一帧就画出来，以前有几处要等下一帧。打开 Debug log 时每次换页有一行 `ui: push ...`、`ui: pop ...` 或 `ui: back to game ...`。日志格式也有变化，设置一行变成 `settings: ... volume=255 brightness=50 ...` 这样的 `key=value`，mGBA 下切 Frame blend 是 `core: gpsp_frame_mixing=enabled kept for gpSP, mGBA does not have it`。
- 跳帧（游戏菜单的 Frameskip，按游戏）：先用 mGBA 跑龙珠大冒险（Dragon Ball Advanced Adventure），在屏幕上有敌人时试 1 和 2，再切到 gpSP 试 Auto。日志里开局有一行 `core: game settings for <核心>: ... frameskip=<值> (<发给核心的键>)`，菜单里改时有 `core: <键>=<值>, applied live`，gpSP 的 Auto 会有 `core: audio buffer status callback registered` 和 `core: asks for at least 128 ms of audio latency`。每局结束有 `core: frameskip <值>: N of M frames skipped (X%), buffer status ...: avg A%, lowest L%, U under one frame`，`perf summary` 和 perf.csv 的 `skipped` 列也有跳帧数，帧耗时 `work_avg` 只算画出来的帧。要看：Auto 在平时（不卡的场景）跳帧数是否接近 0、平均占用是否在 50% 左右（低于 33% 就会一直跳，说明换算不合适），重负载时跳了多少、是否还有 `audio: underrun`；固定间隔下跳帧比例是否正好是 1/2、2/3、3/4；跳帧时 CPU 频率（perf.csv 各频率的行）是否没有因为跳帧被拉低；画面看起来是否比卡顿时顺，声音是否不再断；mGBA 下 Auto 是否显示 "Auto: gpSP only" 并且左右切换时跳过 Auto。Frame blend 挪进了游戏菜单的 Settings 面板（第 2 行），确认在那里能开关，mGBA 下显示 gpSP only。
- 充电诊断（`battery.c`，不用开 Debug log）：2026-10-30 到 11-03 的日志已经说明是第一种情况（见已知问题），读寄存器在视频播放时平均 2.38 ms、最大 4.39 ms，比预计的远低于 1 ms 慢，30 秒的采样实际记了 29 次（`samples done, 29 in 29 s`）。下面的 5 次插拔测试还没做，要做的话照旧。开机日志里应该有 `charger: PMIC registers from /sys/kernel/debug/regmap/<设备>/registers, 7 bytes per line, ...`（如果是 `charger: no ...`，说明 debugfs 或 regmap 没找到，寄存器就不记了），然后是 `charger: start: ...` 和 `charger: start: regs ...` 两行。测试步骤：先用电池开机，打开 System info；插上充电器，等 30 秒，记下屏幕上电池一行显示什么（Charging 还是 Not charging，电流正负）；拔掉等 10 秒再插上，重复 5 次，每次记下有没有充上；再在关机状态下插上充电器后开机一次；最后把 `log/` 拷回来，连同每次的结果一起发给我。要看的日志：每次插拔后有一行 `charger: change (<变了什么>): ...`，紧跟一行 `charger: change (...): regs e4=... e5=... e6=... eb=... f0=... (PLUG_IN_STS N, CHRG_STS ..., BAT_EXS N, CHRG_EN N, in: VLIM on 4.5 V ILIM on 1.50 A, out: 4.20 V 2.00 A), read in X ms`，之后 30 行 `charger: +N s: ...` 和一行 `charger: samples done, 30 in 30 s, read time avg ...`（每次读的耗时，游戏里应该远低于 1 ms）。没充上的那几次重点看 PLUG_IN_STS 是 0 还是 1，CHRG_STS 是不是 CHRG_OFF，e5 的 VLIM 和 ILIM 是否还是 on 4.5 V 和 1.50 A，`IRQs plug_in N plug_out M` 有没有加一，以及 online 有没有变。PLUG_IN_STS=0 是第一种情况，PLUG_IN_STS=1 而 e5 变了是第二种。插拔会把休眠唤醒，休眠前后还各有一组 `charger: sleep #N before: ...` 和 `charger: sleep #N after: ...`。
- 菜单只在有变化时重绘：以前 ROM 列表和菜单每个 vblank 都整屏重绘（约 60 次每秒），现在停着不动时不重绘，主循环睡在 `poll()` 里。打开 Debug log 后，在菜单里每 60 秒和离开菜单（进游戏、休眠、关机）时有一行 `ui: menus for X s (...): N redraws (R/s), M updates, P% asleep in poll; bat ...`。要看：停在 ROM 列表、设置或游戏菜单不动时 R 应该接近 0（选中的名字太长在滚动时，滚动期间约 60，两端各停 1 秒；System info 每秒最多 3 次，Date & time 每秒 1 次），P 应该接近 100%；菜单按键、方向键按住连发是否和以前一样跟手；走马灯速度是否和以前一样（每秒 60 像素，开头和结尾各停 1 秒）；两次确认的 "Press A to ..." 是否 3 秒后恢复；标题栏时间是否按分钟跳；电量变化、插拔充电线、按音量键时菜单上的电量和 Volume 是否马上更新；ROM 列表里有 ROM 在算 CRC 时名字是否还会改过来。省电效果：同一行里的 `bat` 有功率（mW），和以前版本 Debug log 里 ROM 列表停着时的 `battery:` 行（每分钟一行）比较，同样亮度下看功率差多少。selftest 里模拟的结果是：菜单静止 10 秒 0 次重绘，滚动时 0.5 秒 30 次，System info 3 秒 9 次。
- 开机恢复（`resume.c`）：正常恢复、游戏菜单关机、5 秒后删记录和在列表里关机已经确认（见上面）。还要看：一、设置页正好一页 9 行，没有 Back 行，Resume on boot 在 Power off 上面。二、玩一个子文件夹里的游戏，长按电源键关机再开，应该接着玩，Quit to ROM list 回到那个文件夹并选中这个 ROM。三、游戏中关机后按住 B 开机，应该显示列表并选中那个游戏，日志 `resume: skipped ...: B held at boot; gamepad up after N ms, buttons 0x1`。如果按住 B 还是进了游戏，看 buttons 是不是 0x0，以及有没有 `input event5: buttons 0x1 already held`，那说明手柄的第一个报告比 100 ms 来得晚，要加长 `RESUME_PAD_SETTLE_NS`。四、把 Resume on boot 关掉，游戏中关机再开，是列表，日志 `Resume on boot is off`。出问题时看 `resume: skipped` 一行后面的原因。
- 视频的按键和 OSD（`video_core.c`、`osd.c`，细节见 hardware-research.md 的默认行为）：单次跳转、L/R、自动播放和 B 已经确认（见上面）。还没测到的：一、按住左右或上下的连发，日志里只有一次 `Right repeat 1`，要看节奏是否合适、`shown after N runs` 是否和单次跳转一样小、松开后不再跳，连发中按 SELECT+START 打开菜单再回来不应该接着连发。二、新版本的上下跳 60 秒（只有 boot-0001 那版跳过）。三、音量键的 `video: volume key ...` 和 OSD 右上角的 `Volume -NdB`。四、有好几个视频的文件夹里第一个和最后一个的 `... staying`（OSD 上的 First video、Last video），以及最后一个播完时的 `video: end: ... is the last video ...`。五、尺寸不同的两个视频用 L/R 来回切换，画面不应该花屏或者崩溃（这次切换的两集都是 568x320，只有音频采样率不同）。六、OSD 的样子（Fit 的黑边和 Crop 的画面上是否看得清，截断，3 秒消失），ROM 列表里视频的小三角和 `A:resume m:ss`。七、L/R 和自动播放切过去的那一局 perf 都记了 missed 5 到 18（cpu 0），从列表打开的是 0。`start_game` 最后已经调用了 `display_resync()`，所以不是少了这一步，原因还没查清，下次开着 Debug log 看切换前后的 perf 行。

## 已知问题

- 声音的轻微爆音目前只在 gpSP 跑超级马里奥 Advance 4（FC 超级马里奥兄弟 3 的复刻）时出现，进关卡后有一种乐器（像是钢琴）会爆音，同一个游戏换成 mGBA 就没有。perf.csv 里这个游戏的第一局 underrun 是 0，所以初步判断是 gpSP 对这个游戏的声音模拟有问题，不是 underrun，也不是重采样。暂时不修，这个游戏把核心设成 mGBA 就能绕过。
- 界面字体只有 ASCII（JetBrains Mono，`app/frontend/font.c`），中文文件名显示不了。
- 不支持 zip 格式的 ROM。
- 视频只支持 pl_mpeg 能解的 MPEG-PS（MPEG-1 视频加 MP2 音频），`.mp4` 等要先用 ffmpeg 转。缩放是最近邻，像素宽高比按正方形处理。跳转只到关键帧，不精确。
- 硬件时钟出厂时可能没设过（这台是设过的），用到 RTC 的游戏（宝可梦宝石系列的树果和潮汐等）时间会不对，要先在设置的 Date & time 里设一次，开机日志会标出没设过的情况。没有时区，输入的时间按 UTC 存进系统时钟和 RTC，游戏看到的就是输入的时间，日志和 perf.csv 里写的 UTC 其实是这个时间。
- 插拔充电线也会把机器从休眠唤醒（PMIC 中断），现在唤醒后一律亮屏。
- 插上充电器时系统一直报 Not charging（ONLINE=0），但电池其实在充，只是慢。2026-10-30 到 11-02 插着线开机的三次（boot-0003、0004、0012）开机时寄存器都是 PLUG_IN_STS 0、CHRG_STS CHRG_OFF，e5=0xdb（4.5 V、1.50 A）没变，所以是第一种情况，PMIC 没认到输入，不是 CHRG_IN 被复位。boot-0004 在 1058.6 s 拔线，只看到 status 从 Not charging 变成 Discharging，没有 plug_out 中断。同时电池电流是 +0.13 到 +0.21 A，boot-0003 插着线在列表停了 55 分钟，到下次开机电量从 59% 到 63%，boot-0011 和 0012 之间关机放着，从 50% 到了 100%。以前说的电池没充进去在这批日志里不成立。指示灯颜色会变，但那是 VBUS 直接点亮的另一颗灯，和 PMIC 无关。驱动（`rk817_charger.c`）的背景：电流不为负时 STATUS 取 CHRG_STS（0xeb）的 6:4 位，CHRG_OFF 就报 Not charging；ONLINE 只在 probe 时从 SYS_STS（0xf0）的 PLUG_IN_STS 读一次，之后只跟着 plug_in 和 plug_out 两个中断变；CHRG_IN（0xe5）只在 probe 和拔出中断里写；充电使能位驱动从来不写。BSP 用来检测充电器的 dc_det GPIO（gpio0 PB3）主线和我们的 DTS 都没用，下一步可以从这里查 PMIC 为什么看不到输入。
- 唤醒后 USB 手柄约 2.2 s 不能用，是内核的问题：dwc2 唤醒后重新枚举连续两次失败（`dwc2 ff300000.usb: Not connected`），`usb usb1-port1: attempt power cycle` 之后才成功。休眠前还有约 20 行 `dwc2: Mode Mismatch Interrupt: currently in Device mode`。这批的 boot-0008 和上一批日志里的 boot-0002 都是这样。前端在内核建好节点后约 1 ms 就打开了，前端这边没有可以再省的。
- GBA 分区的 FAT 脏标记一直在（boot-0001 到 0014 全是 `dirty bit set`），因为卡从来没在电脑上修复过，正常关机能不能清掉还不知道，见待实机验证的卸载一项。
- 写卡偶尔很慢：即时存档平时写 31 到 71 ms，但有过 1572 ms（boot-0009 关机）、868 ms（boot-0010 关机）和 898 ms（boot-0008 退出）。挂载后到建好日志目录平时约 75 到 105 ms，boot-0004 卡了 2.5 s，boot-0001 卡了 1.5 s。可能是卡本身的写入延迟，也可能和脏标记有关，还没查。
- mGBA 跑重的游戏还是会卡：银河战士融合（Metroid Fusion）一局 10 分钟 missed 87、underrun 27，阿童木（Astro Boy - Omega Factor）37 秒 missed 77、underrun 26，大部分时间已经在 1296 MHz，work 平均约 11 ms，最大 21 到 36 ms。同样的游戏用 gpSP 基本是 0，所以默认 gpSP 是对的。
- 视频播放不比玩游戏省电：50% 背光下 boot-0004 的视频 1.41 到 1.52 W，boot-0005 以后 gpSP 的游戏是 1.23 到 1.35 W。不过同在 boot-0001 里视频 1.44 W，gpSP 的星之卡比和龙珠是 1.56 到 1.68 W，不同次开机之间差别很大（电池电压、温度都不同），要同一次开机对比才说得准。工作线程占了约一半的墙钟，Debug log 关着，没有它的频率数据。
- boot-0001 没有关机时的 dmesg 转存，`frontend.txt` 最后一行是 `video: paused at 573.60 s`，用户也不记得是怎么关的机，原因不明。同一次开机里一个视频有两次 +10 s 跳转 `failed, back to the start`，之后没再出现过（那一版前端比后面的旧）。

## 待办和想法

- 原生格斗引擎的方案调研见 [fighting-engine-research.md](fighting-engine-research.md)（2026-11-03）：480×320、拳皇式传统 1 对 1、背景加共同地面，以拳皇/侍魂等角色跨作品对战为方向，暂不考虑四人和平台规则。建议 C 数据驱动引擎与 CPU 渲染，先做实机性能原型；已有软件 POC 和 boot-0015 的初次真机结果；后续执行顺序见 [格斗文档](fighter.md)。完整 MUGEN 人物兼容不作为首版承诺。

大致按优先级排。

- 太阳传感器亮度（`mgba_solar_sensor_level`），Boktai 系列需要，只有 mGBA 支持。加一个按 ROM 的核心选项现在是 `settings.h` 的 `struct rom_settings` 加字段，`settings.c` 的 `rom_fields` 和 `core.c` 的 `rom_options` 各加一项（一个设置可以对应几个键，核心没有的值在 `values` 里写 NULL；默认值不是 0 时再改 `rom_defaults`），要在菜单里改就在 `menu.c` 定义一行（`.option = 1`）加进行表。游戏菜单已经放满 11 行（`menu.h` 的 `IG_ROWS`，再多框就超出屏幕），新的行放进游戏菜单的 Settings 面板。
- mGBA 的 Auto 跳帧：这个版本的 mGBA libretro 只有固定间隔。要做的话可以由前端按音频缓冲占用实时改 `mgba_frameskip`。
- 跳帧时前端还是每帧把上一帧画面重新缩放旋转一遍（blit），这部分开销没有省下来。
- 前端自己实现帧间混合（smart 模式只混合闪烁的像素，以及 mGBA 下也能用）。
- 中文文件名：GBA 分区已经用 utf8 挂载，还缺一套 CJK 字体。
- 记住上次所在的文件夹和选中的 ROM，跨开机恢复。现在只在本次开机内保留，只有开机恢复（以及跳过恢复）时会定位到关机时的那个游戏。开机恢复的记录退回列表时就删了，用不上，要做的话另存一个文件。
- zip 格式 ROM。
- 震动卡带（`gpsp_rumble`），需要前端驱动 pwm-vibrator。
- mGBA 的性能：多线程视频、LTO、PGO。
- 开机速度优化，见 hardware-research.md 里“对精简和快速开机的初步判断”。2026-10-18 的一次实机日志（debug 开着）：内核启动后 0.81 s 进入前端，0.86 s 挂好 GBA 分区，2.43 s 才打开日志，2.49 s 出 ROM 列表。挂载后到打开日志之间的间隔从 2026-10-19 的版本起是 75 到 105 ms，只有一次 850 ms（boot-0011，可能是 FAT 脏标记还在时的第一次写入），2026-10-30 那批又有 2.5 s 和 1.5 s 两次（见已知问题的写卡偶尔很慢）。现在前端在 0.75 到 0.85 s 启动，第一帧在 0.96 到 1.09 s。bootloader 阶段的耗时没量过，要接串口或者用高速摄像来测。
- 接上 UART2 以后再查主线 U-Boot。
- 把 RG351P 的 DTS 整理后提交到主线内核。
- 格斗：给各角色切真正的倒地和起身帧（清单的 `fall`/`down`/`getup`，现在借 KO 和蹲姿帧），图集位置见 [fighter.md](fighter.md) 倒地一节；按角色的起身时间（Terry 25、Chris 29、Mature 21、Shermie 31、Vice 26）要等 `wakeup` 能按角色覆盖。
- 格斗：给各角色切真正的投技帧（清单的 `throw`/`thrown`，现在是站 HP 帧的通用抓取和受击帧），图集位置见 [fighter.md](fighter.md) 投技一节；投范围和拆投窗口要上机按手感调（现在 4 px 和 6 帧都是估计），之后可以加指令投（Shermie、Vice 等）。

## 踩过的坑

这些问题都花了不少时间，以后遇到类似现象可以先往这些方向查。

- **qemu 里的耗时不能推算真机**：pl_mpeg 解一帧 480x270 在 qemu-user 里约 4 ms，真机 1296 MHz 下是 13 到 18 ms，按 qemu 的数字以为一次 vblank 里放得下，结果视频第一次上机就卡。性能要在真机上量，qemu 只用来看功能和相对变化。

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

2026-11-03 格斗方案补充：明确采用标准 libretro core（本机静态、桌面动态），通过统一绘制指令保留软件/GPU 后端扩展空间；先实现软件路径并用真实规模素材测量。MUGEN 行为兼容不作为目标。详见 [方案补充](fighting-engine-research.md#补充决定标准-libretro-core-与可替换渲染器)。
