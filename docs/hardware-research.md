# RG351P 硬件与上游支持调研

调研日期 2026-10-15。目标是给 Anbernic RG351P 做一个基于 Linux、只用来玩 GBA 的精简发行版，不需要网络，尽量缩短开机时间。

## 硬件配置

| 项目 | RG351P |
|---|---|
| SoC | 瑞芯微 RK3326，4 核 Cortex-A35，1.3GHz，Mali-G31 GPU |
| 内存 | 1GB DDR3L |
| 存储 | 没有 eMMC，只能从 TF 卡启动 |
| 屏幕 | 3.5 英寸 480×320 IPS，MIPI DSI 接口，屏驱 IC 是 Elida KD35T133，竖屏贴装，需要旋转 270° |
| 电源管理和声卡 | RK817，PMIC、音频 codec、充电器集成在一颗芯片里，挂在 i2c0 上 |
| 按键摇杆 | 机内一个 USB HID 手柄，接在 USB host 口上 |
| 网络 | 大部分 P 没有 WiFi，只有很早期的批次带 |
| 其他 | PWM 背光（pwm1），PWM 震动马达（pwm0），三色充电灯（gpio2 PB5），调试串口 UART2 |

RG351P 是 Odroid Go Advance 的克隆，RG351M 是它的金属外壳版，两者主板基本相同。

GBA 原生分辨率 240×160，正好按 2 倍整数放大铺满 480×320 的屏幕，不需要缩放滤镜。

## 上游 Linux 内核

- 主线没有单独的 `rk3326-anbernic-rg351p.dts`，但有 `rk3326-anbernic-rg351m.dts` 和 `rk3326-anbernic-rg351m.dtsi`。它们 2022-11 加入，2023-12 从 Odroid Go Advance 拆出来单独维护。ROCKNIX 在 RG351P 上用的也是 RG351M 的 dtb。
- 主线已经支持的部分
  - DSI 屏，屏驱是 `elida,kd35t133`，DTS 里设置了 `rotation = <270>`
  - PWM 背光
  - Panfrost GPU
  - RK817 PMIC 和音频，走 `simple-audio-card`，耳机检测接在 gpio2 PC6
  - SD 卡，支持 UHS（SDR104）
  - USB host
  - 震动马达（`pwm-vibrator`）
  - 温度传感器（tsadc）
  - 串口 UART2，波特率 115200
- 手柄是 USB HID 设备，内核开着 `usbhid` 就能用，不需要专门的驱动。
- 主线 DTS 没有启用 `rk817_charger`。原因是这块板子上没有电流检测电阻，充电驱动没法正常算电量。ROCKNIX 自己补了 `simple-battery` 节点和 OCV 曲线，但电流检测那一行还是注释掉的。所以电量显示很可能只能按电压估算，需要自己处理。
- 充电灯的控制方式比较特殊。拉高是红，拉低是绿，引脚设为输入是黄。主线只描述成了一个红灯。
- 主线 DTS 里还有一个 SPI NOR 节点（sfc）。这是从 Odroid Go Advance 继承来的，RG351P 上是否真的焊了 SPI flash 还需要实机确认。

## 上游 U-Boot 和 TF-A

- 主线 U-Boot 有 `odroid-go2_defconfig`，板级代码在 `board/hardkernel/odroid_go2`。目前有人维护（Heiko Stuebner 和 Jonas Karlman），2025-11 刚合进一批改进，包括在 SPL 里从 FIT 选择 FDT，以及从加载 FIT 的那个存储读取 env。
- 主线 U-Boot 没有 RG351 专用的 target。不过硬件相同，可以在 odroid-go2 的基础上改出一个。
- 主线 U-Boot 里有开源的 PX30/RK3326 DDR 初始化代码（`drivers/ram/rockchip/sdram_px30.c`，在 TPL 阶段运行）。odroid-go2 用的是默认的 DDR3 配置，和 RG351P 的 DDR3L 一致。
- 主线 TF-A 有 `plat/rockchip/px30`。
- 所以理论上整条启动链（TPL、SPL、TF-A BL31、U-Boot）都可以不用瑞芯微闭源 blob。这一点需要实机验证。

### ROCKNIX 的做法

- 同时出两套 bootloader
  - `u-boot-legacy` 是 Hardkernel 的旧版 U-Boot（ROCKNIX/hardkernel-uboot 仓库）
  - `u-boot` 是主线 U-Boot v2025.10 加补丁，自定义 defconfig `rk3326-handheld_defconfig`，基于 `TARGET_ODROID_GO2`
- 两套都用了 rkbin 里的闭源 blob，包括 `rk3326_ddr_333MHz_v2.11.bin`、`rk3326_miniloader_v1.40.bin` 和 `rk3326_bl31_v1.34.elf`。
- 机型识别靠读 SARADC 的一个通道（hwid ADC），再加上几个 GPIO，在 boot.ini 脚本里选择 dtb，最后交给 extlinux 启动。
- RG351M 的 dtb 选择逻辑只出现在 `a_boot.ini` 里（ADC 值落在 651 到 703 之间，并且 GPIO c22 为低）。由此推测 RG351P 现在走的是 legacy U-Boot 那套镜像，这一点还没有验证。

## AmberELEC（原 351ELEC）的做法

351ELEC 的 GitHub 仓库已经重定向到 AmberELEC，AmberELEC 目前仍在活跃维护。它是 RG351 系列最早的社区系统，走的是厂商 BSP 路线，和 ROCKNIX 的主线路线正好相反，适合拿来对照硬件细节。

### 软件栈

- 内核是 `AmberELEC/kernel_rg351`，基于瑞芯微 BSP 的 4.4.189，RG351P 用的 DTS 是 `rk3326-rg351p-linux.dts`。
- U-Boot 是 `AmberELEC/uboot_rg351`，基于 2017.09 的厂商 U-Boot。
- 启动链完全依赖 rkbin 闭源 blob，包括 `rk3326_ddr_333MHz_v1.15.bin`、`rk3326_miniloader_v1.20.bin` 和 `rk3326_bl31_v1.21.elf`。
- SD 卡布局是老式的瑞芯微格式
  - `idbloader.img`（DDR blob 加 miniloader）写在 32KiB 处（扇区 64）
  - `uboot.img` 写在 8MiB 处（扇区 16384）
  - `trust.img`（BL31）写在 12MiB 处（扇区 24576）
  - 第一个分区是 FAT，里面放 `boot.ini`、内核、dtb 和 extlinux 配置
- 机型识别在 U-Boot 的 `hwrev` 命令里实现（`cmd/hwrev.c`）。它读 SARADC 通道 0，读数在 655 到 695 之间就判定为 RG351P，然后加载 `rk3326-rg351p-linux.dtb`。RG351P 专用镜像的 `boot.ini` 干脆写死了 dtb，不做判断。
- 内核命令行是 `console=ttyFIQ0 console=tty0 fbcon=rotate:3 consoleblank=0 loglevel=0 quiet panic=20`。BSP 内核用 FIQ debugger 当串口控制台，主线内核对应的是 `ttyS2`。
- GPU 用的是闭源 libmali（Bifrost G31），不是 Panfrost。
- 上层沿用 LibreELEC 的框架，有 systemd、PulseAudio、Samba、SFTP、EmulationStation，还会预装一堆 USB WiFi 网卡驱动。对我们来说这些都是要砍掉的。

### 从 BSP DTS 里得到的硬件信息

- `sfc` 是 `disabled`。说明 RG351P 很可能没有 SPI flash，主线 dtsi 里的 SPI NOR 节点是从 Odroid Go Advance 继承来的，我们自己的 DTS 可以关掉它，省一次无用的探测。
- 屏幕时序是 320×480，像素时钟 17MHz，物理尺寸 42mm×82mm，屏驱 `elida,kd35t133`，和主线一致。
- 没有 gpio-keys 或 ADC 摇杆节点，再次确认按键和摇杆全部走机内 USB 手柄，只有电源键由 RK817 的 `pwrkey` 提供。
- 电池参数写的是 3450mAh（`design_capacity`），`sample_res = <10>`，即 10mΩ 采样电阻，并且用的是 BSP 带库仑计的 `rk817,battery` 驱动。这和主线 commit 里“没有电流检测电阻”的说法矛盾。BSP DTS 有可能是从 Odroid Go Advance 照抄的，要实机读一下 RK817 的电流寄存器才能确定。
- 充电器检测 GPIO 是 gpio0 PB3（`dc_det_gpio`）。
- `sdio` 控制器在 BSP DTS 里是开着的，但没有挂任何设备。RG351P 早期批次的 WiFi 和社区的加装方案都是 USB 网卡（AmberELEC 休眠脚本里提到的 esp8089 应该是从 Odroid Go Advance 照抄来的注释），所以这个控制器大概率可以直接关掉。
- 串口是 UART2 的 m1 引脚组，波特率 115200。

### 值得借鉴的细节

- 充电灯在 sysfs 里是 GPIO 77（gpio2 PB5），AmberELEC 用一个脚本切换方向和电平来实现红、绿、黄三种颜色，低电量时红黄交替闪烁。
- 音频输出通过 ALSA 控件 `Playback Path` 在 `SPK` 和 `HP` 之间切换，耳机检测由一个脚本轮询处理。主线的 `simple-audio-card,hp-det-gpios` 可以直接替代这个脚本。
- 休眠唤醒时 USB 控制器 dwc2 有问题，AmberELEC 的做法是休眠前卸载 `dwc2` 模块，唤醒后重新加载。手柄挂在 USB 上，所以这个问题直接影响休眠后能不能继续玩。主线内核上是否还有这个问题需要验证。

## 技术选型

### 内核

已确定使用主线内核，dtb 以主线的 `rk3326-anbernic-rg351m.dts` 为基础，再按 RG351P 的实际情况修改（比如关掉 sfc 和 sdio）。

### U-Boot（暂用 legacy，主线待查）

实机结果（2026-10-18）：主线 U-Boot v2026.10 在 RG351P 上没有任何反应，换 OGA 设备树、换 rkbin BL31 都一样，bootcmd 里的震动和指示灯都没出现，说明 U-Boot proper 没有跑起来。ROCKNIX PR #2120 提到主线 U-Boot 在 RK3326 上处理 SD 卡的 vmmc/vqmmc 有问题，部分卡初始化失败，这和现象吻合，修复已经加进 `rk3326-rg351p-u-boot.dtsi`，还没上机验证。

目前默认用 ROCKNIX 的 legacy U-Boot 2017.09（`external/u-boot-legacy`，`BOOTLOADER=legacy`），从源码编译，配合 rkbin 的 DDR、miniloader 和 BL31，已经在实机上引导主线内核成功。它从 p1 读 `boot.ini`，再用 `sysboot` 转到 `extlinux.conf`。`BOOTLOADER=mainline` 保留，等有串口后继续查。

主线 U-Boot 的现状

- 能直接用 `odroid-go2_defconfig` 起步，ROCKNIX 的 B 镜像证明主线 U-Boot v2025.10 加主线内核在 RK3326 掌机上能跑。
- 机型识别有坑。`board/hardkernel/odroid_go2/go2.c` 读 SARADC 通道 0，在标称值上下 50 的窗口里匹配。RG351P 的读数在 655 到 695 之间（取自 AmberELEC 的 `hwrev.c`），正好落进 Odroid Go Advance Black Edition（677）的窗口。结果 U-Boot 会把 RG351P 当成 OGA v11，自己用 `rk3326-odroid-go2-v11` 的 DT，并且把 `fdtfile` 设成 OGA v11 的 dtb 交给 Linux。
- 这会引出第二个坑。主线 RG351M DTS 注释说 `vcc_host`（USB 5V 输出）和震动马达冲突，ROCKNIX 也专门改过一次，避免 U-Boot 打开 USB 5V 导致 RG351M 开机震动。用 OGA v11 的 DT 很可能会触发这个问题。
- ROCKNIX 在 2025-12 修过一次主线 U-Boot 的 SD 卡兼容性问题，改动只是把 `kernel_addr_r` 挪到 0x09000000，看起来是加载地址重叠。
- ROCKNIX 在 2025-11 把 RK3326 全部切到主线 U-Boot，2025-12 又改成 A 镜像用 legacy、B 镜像用主线，RG351 系列分在 A 组。commit 里没有写原因，暂时没查到为什么 RG351 退回了 legacy。

建议的路线

1. 先用主线 U-Boot 加 rkbin 的 DDR blob 和 BL31。这是 ROCKNIX B 镜像已经跑通的组合，最快能点亮。
2. 给 RG351P 单独做一个 board 配置，或者至少关掉自动识别、写死 dtb，并且确保 U-Boot 不打开 `vcc_host`。
3. 点亮之后再换开源 TPL DDR 初始化和主线 TF-A，最终做到整条启动链没有闭源 blob。如果开源 TPL 在 RG351P 上不稳定，就保留 rkbin 的 DDR blob。
4. 最后做开机速度优化，见下一节。

厂商 U-Boot 2017.09 只作为备选。它的问题在于版本太老，新版 GCC 需要加编译参数绕过报错，SD 卡布局是老式的 idbloader、uboot.img、trust.img 三段，而且裁剪起来不如主线方便。

### 构建系统（顶层 Makefile 加 Bootlin 工具链）

- 不用 Buildroot。整个系统只有内核、bootloader 和几个静态程序，用一个顶层 `Makefile` 加 `mk/*.mk` 就能管住，比维护一个 `BR2_EXTERNAL` 外部树简单，增量构建也更直观。
- 交叉工具链用 Bootlin 预编译的 `aarch64--musl--stable-2026.08-1`（gcc 15.3.0，binutils 2.45.1，musl 1.2.6），第一次构建时下载到 `dl/` 并校验 sha256。
- 上游源码全部用 git submodule 管理，浅克隆，版本写死。
  - `external/linux`：stable v6.18.57
  - `external/u-boot`：v2026.10
  - `external/trusted-firmware-a`：lts-v2.14.10
  - `external/rkbin`：只用 DDR blob，BL31 也可以选它
- 板级的东西放在 `board/rg351p/` 下，包括内核 DTS 和配置片段、U-Boot 配置片段和 dtsi、`genimage.cfg`、`extlinux.conf`。对上游没有打补丁。
- 内核基于 allnoconfig 合并配置片段，所有驱动内置，没有模块和 initramfs。makefile 会检查片段里的符号是否都生效。
- U-Boot 从 `odroid-go2_defconfig` 合并片段，proper 阶段改用主线 RG351M 的 DT 并通过 `DEVICE_TREE_INCLUDES` 加上自己的 dtsi，避开 OGA DT 里 `vcc_host` 打开震动马达的问题。`DDR_INIT=rkbin|open` 和 `BL31=tfa|rkbin` 可以切换。
- 正式镜像里用静态程序直接当 PID 1，不用 BusyBox init。之后可以做一个调试变体，带上静态 BusyBox，接 USB 键盘开 shell。
- 镜像用 genimage 生成，MBR 分区表，`u-boot-rockchip.bin` 写在 32KiB 处，p1 是 FAT 的 BOOT 分区（从 16MiB 开始），p2 是只读的 ext4 rootfs，p3 是 FAT 的 GBA 分区，放 ROM 和存档。
- 主机依赖：`swig python3-setuptools python3-pyelftools genimage mtools dosfstools`。

### 显示输出（CPU 加 KMS dumb buffer，不用 GPU）

- 直接用 DRM/KMS 的 dumb buffer。分配两块做双缓冲，通过 page flip 提交，翻页对齐垂直同步，不会撕裂，vblank 事件还可以用来给模拟器定节奏。只依赖 libdrm。
- 不用 `/dev/fb0`。它只是 DRM 上面的一层兼容模拟（`CONFIG_DRM_FBDEV_EMULATION`），双缓冲和垂直同步不可靠，容易撕裂。开发初期可以拿它快速出图验证。
- 不用 SDL2 的 KMSDRM 后端。它要走 GBM 和 EGL，会把 Mesa 带进来。
- 放大和旋转由 CPU 一次完成。屏幕是竖装的 320×480，PX30 的 VOP 应该不支持硬件 90° 旋转（待确认）。GBA 的 240×160 放大 2 倍再旋转，每帧写 15.4 万像素，60fps 时大约每秒 920 万像素。用 NEON 估算只占一个核心的 1% 到 3%，相比模拟器本身可以忽略。
- 不用 GPU 的理由
  - 每帧上传纹理、提交任务、等 fence，驱动在 CPU 上的固定开销很可能比直接画还大
  - GPU 电源域要一直开着
  - 要带上 Mesa 加 Panfrost 或者 libmali，rootfs 变大，开机多初始化一个驱动
- 省电相关
  - 不加载 GPU 驱动，genpd 会在启动后期关掉没人用的电源域，Mali 可以完全断电（待实机确认）
  - 扫描输出用 RGB565。GBA 原生是 15 位色，不损失颜色，VOP 每秒的内存读取量从约 37MB/s 降到约 18MB/s。PX30 VOP 是否支持 RGB565 扫描输出还需要确认
  - CPU 调频用 schedutil，模拟器负载平稳，可以停在较低的频率档
  - 整机最耗电的是背光和屏幕，背光档位要设计合理
- 色彩校正用 CPU 查一张 32768 项的表就能做，不需要 GPU 着色器。
- 可以试但多半不用的优化：CPU 只做旋转，2 倍放大交给 VOP 的硬件缩放，这样 CPU 写入量和扫描读取量都降到四分之一。问题是硬件缩放一般是双线性插值，像素会变糊，做不到清晰的整数倍放大。
- 刷新率不一致的问题：GBA 是 59.73Hz，屏幕大约 60Hz。按 vblank 出帧的话，音频需要做动态重采样，避免爆音或者逐渐不同步。

## 对精简和快速开机的初步判断

- 可以去掉的东西：网络协议栈、WiFi/BT、显示服务器（用 KMS/DRM 直接出图，不需要 Sway 或 Weston）、EmulationStation 这类前端，还有 systemd。
- Bootloader 优化：`BOOTDELAY=0`，关掉 USB 和网络初始化，不用 bootflow 扫描而是直接加载固定路径的内核。开源 TPL 加 SPL 直接加载内核（falcon mode）也值得试一下。
- 内核：只编译这块板子用到的驱动并且全部内置，不用 initramfs，直接挂载 rootfs。
- 手柄挂在 USB 上，枚举要几百毫秒，可以和模拟器启动并行进行。
- 模拟器：RK3326 跑 gpSP 绰绰有余（有 ARM64 动态重编译），跑 mGBA 也基本能全速，不需要 GPU 也行。

## 实机验证结果

2026-10-18，legacy U-Boot 加主线 6.18.57 内核加 hwtest：

- 显示正常，DSI 面板、背光、fbcon 都工作，`fbcon=rotate:3` 和 hwtest 的 270° 旋转方向正确
- VOP 支持 RGB565 扫描输出
- 屏幕实际刷新率 60.02Hz
- 内核启动到 hwtest 出第一帧约 2014ms（CLOCK_BOOTTIME，不含 bootloader）
- 机内 USB 手柄在 dwc2 上工作，按键和摇杆都有事件
- 按电源键能真正断电，`rk817` 的 `system-power-controller` 有效
- 指示灯：GPIO2_B5 拉高红、拉低绿，上电默认是绿色

## 待确认

- RG351P 和 RG351M 的屏幕是否完全一致，早期批次 RG351P 有没有换过屏
- RG351P 有没有 SPI flash（BSP DTS 里是关掉的，大概率没有）
- 主线 U-Boot 去掉 SD 卡供电后能否启动，开源 TPL DDR 初始化能否工作
- 到底有没有电流检测电阻（主线 commit 说没有，BSP DTS 写的是 10mΩ），这决定了电量能不能用库仑计来算
- 主线内核上休眠唤醒后 dwc2 和 USB 手柄能不能正常恢复
- PX30 VOP 是否支持硬件旋转
- 不加载 GPU 驱动时 Mali 的电源域是否真的会被关掉
- RG351P 主板上 UART2 测试点的位置
- 机内 USB 手柄的具体芯片和枚举耗时

## 下一步

根据 dmesg 分析内核启动耗时，压缩开机时间。之后选定模拟器核心，开始写前端。接上 UART2 串口后再回头查主线 U-Boot。

## 参考资料

- [Linux 主线 RK3326 DTS 目录](https://github.com/torvalds/linux/tree/master/arch/arm64/boot/dts/rockchip)
- [U-Boot odroid_go2 板级代码](https://github.com/u-boot/u-boot/tree/master/board/hardkernel/odroid_go2)
- [U-Boot Rockchip 文档](https://github.com/u-boot/u-boot/blob/master/doc/board/rockchip/rockchip.rst)
- [TF-A Rockchip 平台](https://github.com/ARM-software/arm-trusted-firmware/tree/master/plat/rockchip)
- [ROCKNIX RK3326 设备配置](https://github.com/ROCKNIX/distribution/tree/next/projects/ROCKNIX/devices/RK3326)
- [U-Boot go2.c 机型识别代码](https://github.com/u-boot/u-boot/blob/master/board/hardkernel/odroid_go2/go2.c)
- [ROCKNIX commit：A 镜像用 legacy、B 镜像用主线](https://github.com/ROCKNIX/distribution/commit/d2b00cd91a8d)
- [ROCKNIX commit：修复主线 U-Boot 的 SD 卡兼容性](https://github.com/ROCKNIX/distribution/commit/53a6f20944e5)
- [AmberELEC 主仓库](https://github.com/AmberELEC/AmberELEC)
- [AmberELEC RG351 BSP 内核](https://github.com/AmberELEC/kernel_rg351)
- [AmberELEC RG351 U-Boot](https://github.com/AmberELEC/uboot_rg351)
- [ROCKNIX RG351P/M/V 设备页](https://rocknix.org/devices/anbernic/rg351pmv/)
- [RetroGameCorps：RG351P 加装 WiFi 指南](https://retrogamecorps.com/2020/11/16/guide-install-your-own-wi-fi-chip-inside-the-rg351p/)
- [CNX Software：RK3326 datasheet](https://www.cnx-software.com/2018/07/23/rockchip-rk3308-rk3326-datasheet/)
