# RG351P 硬件与上游支持调研

调研日期 2026-10-15，之后随实机结果更新。目标是给 Anbernic RG351P 做一个基于 Linux、只用来玩 GBA 的精简发行版，不需要网络，尽量缩短开机时间。现在的实现见文末的“当前状态”。

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
- 主线 DTS 没有启用 `rk817_charger`。主线 commit 的说法是这块板子上没有电流检测电阻。ROCKNIX 自己补了 `simple-battery` 节点和 OCV 曲线，但电流检测那一行还是注释掉的。我们的 DTS 按 BSP 的 10mΩ 把充电器和电池节点加回来了，详见 DTS 里的注释。
- 充电灯的控制方式比较特殊。拉高是红，拉低是绿，引脚设为输入是黄。主线只描述成了一个红灯。
- 主线 DTS 里还有一个 SPI NOR 节点（sfc）。这是从 Odroid Go Advance 继承来的，BSP DTS 里是关掉的，我们的 DTS 也关掉了。

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

已确定使用主线内核，dtb 以主线的 `rk3326-anbernic-rg351m.dtsi` 为基础，再按 RG351P 的实际情况修改。sdio 在 px30.dtsi 里本来就是关的，不用再管。

内核配置是 allnoconfig 加 `board/rg351p/linux/rg351p.config`（调试时再加 `debug.config`），片段一改 `.config` 就会重新生成。RTC 用 RK817 自带的那一个（`CONFIG_RTC_DRV_RK808`），开机时 `CONFIG_RTC_HCTOSYS` 把它拷进系统时钟，`CONFIG_RTC_INTF_DEV` 提供 `/dev/rtc0` 给前端读写，sysfs 和 proc 接口没开，`CONFIG_RTC_SYSTOHC` 也没开（前端设时间时自己写 RTC）。

### U-Boot（已定：legacy，主线留待以后）

**结论（2026-10-18）**：默认使用 ROCKNIX 的 legacy U-Boot 2017.09（`external/u-boot-legacy`，`BOOTLOADER=legacy`），从源码编译，配合 rkbin 的 DDR、miniloader 和 BL31。它已经在实机上引导主线内核成功，从 p1 读 `boot.ini`，再用 `sysboot` 转到 `extlinux.conf`。主线 U-Boot（`BOOTLOADER=mainline`）的构建保留，以后有串口再查。

#### 主线 U-Boot 的实机测试记录

所有组合都是同一个现象：上电后指示灯保持默认的绿色，没有震动。bootcmd 开头会控制马达震一下、让指示灯闪红，这些都没有出现，说明 U-Boot proper 没有执行到 bootcmd，很可能停在 SPL 甚至更早。

| 设备树 | BL31 | SD 供电修复 | 结果 |
|---|---|---|---|
| RG351M 加自己的 dtsi | 主线 TF-A | 无 | 不启动 |
| RG351M 加自己的 dtsi | rkbin v1.37 | 无 | 不启动 |
| 原版 OGA | rkbin v1.37 | 无 | 不启动 |
| RG351M 加自己的 dtsi | rkbin v1.37 | 有 | 不启动 |
| RG351M 加自己的 dtsi | 主线 TF-A | 有 | 不启动 |

"SD 供电修复"指的是 ROCKNIX PR #2120 的做法，在 U-Boot 设备树里删掉 `&sdmmc` 的 `vmmc-supply` 和 `vqmmc-supply`。在 RG351P 上它没有解决问题。

ROCKNIX 在 2025-11 把 RK3326 全部切到主线 U-Boot（PR #1986），2025-12 又把 RG351 系列等不带 eMMC 的机型退回 legacy（PR #2105，没有写原因）。主线 U-Boot 在 RG351 上很可能一直就没跑通。

#### legacy（能启动）和主线（不能启动）的区别

| 环节 | legacy | 主线 |
|---|---|---|
| sector 64 的 idbloader | rkbin DDR v2.12 加 rkbin miniloader v1.40（闭源 SPL），`mkimage -T rksd` 打包 | rkbin DDR v2.12 加 U-Boot SPL，binman 打包 |
| 8MiB 处 | `uboot.img`（`loaderimage` 打包的 U-Boot proper，4 份） | FIT（`u-boot.itb`，内含 U-Boot proper、BL31、dtb） |
| 12MiB 处 | `trust.img`（rkbin BL31，`trust_merger` 打包） | 没有，BL31 在 FIT 里 |
| 谁加载 BL31 和 U-Boot proper | miniloader | U-Boot SPL，带 SD 驱动、PMIC（RK817）、SARADC 识别机型 |
| U-Boot proper 用的设备树 | 内置 odroidgoa 设备树 | RG351M 或 OGA 设备树 |

DDR blob 在两边是同一个文件，legacy 能启动，说明 DDR 初始化没问题。问题最可能出在 U-Boot SPL 这一段：SD 卡初始化、SPL 里的 RK817 访问、SARADC 读数，或者 SPL 的设备树。

#### 以后排查主线的思路

1. 接 UART2 串口（DDR blob 输出 1500000 波特率，主线 SPL 是 115200），直接看 SPL 停在哪里。
2. 做混合镜像缩小范围：用 legacy 的 miniloader 加载主线 U-Boot proper，如果能跑，问题就确定在主线 SPL。
3. 在 SPL 里尽早拉 GPIO（指示灯 GPIO2_B5 或马达 GPIO0_B7）打点，不需要串口也能判断执行到哪一步。需要对 SPL 加板级代码，要打补丁。
4. 逐个关掉 SPL 里的可选功能（`SPL_PMIC_RK8XX`、SARADC 机型识别等），对比 ROCKNIX B 组能启动的机型和 RG351 在硬件上的差别。

#### 主线 U-Boot 的已知坑（保留备查）

- 机型识别。`board/hardkernel/odroid_go2/go2.c` 读 SARADC 通道 0，在标称值上下 50 的窗口里匹配。RG351P 的读数在 655 到 695 之间，正好落进 OGA Black Edition（677）的窗口，会被当成 OGA v11。
- 震动马达。OGA 设备树里 `vcc_host` 是常开的，使能脚 GPIO0_B7 在 RG351 上接的是震动马达，所以 U-Boot 一启动马达就会转。改用 RG351M 设备树可以避开。
- ROCKNIX 把 `kernel_addr_r` 挪到 0x09000000，避免加载地址重叠。legacy 的 `boot.ini` 也用这个地址。

### 构建系统（顶层 Makefile 加 Bootlin 工具链）

- 不用 Buildroot。整个系统只有内核、bootloader 和一个静态程序，用一个顶层 `Makefile` 加 `mk/*.mk` 就能管住，比维护一个 `BR2_EXTERNAL` 外部树简单，增量构建也更直观。
- 交叉工具链用 Bootlin 预编译的 `aarch64--musl--stable-2026.08-1`（gcc 15.3.0，binutils 2.45.1，musl 1.2.6），第一次构建时下载到 `dl/` 并校验 sha256。
- 上游源码全部用 git submodule 管理，浅克隆，版本写死，构建产物都在 `build/`，子模块不改动也不打补丁。
  - `external/linux`：stable v6.18.57
  - `external/u-boot-legacy`：ROCKNIX/hardkernel-uboot 的 RK3326 分支，默认 bootloader
  - `external/rkbin`：DDR blob、miniloader、BL31 和打包工具（legacy 用全部，主线只用 DDR blob，BL31 可选）
  - `external/u-boot`：v2026.10，只给 `BOOTLOADER=mainline` 用
  - `external/trusted-firmware-a`：lts-v2.14.10，只给 `BOOTLOADER=mainline BL31=tfa` 用
  - `external/mgba`：0.10.5，libretro 核心，静态链接进前端
  - `external/gpsp`：libretro 版 gpSP，带 ARM64 动态重编译，静态链接进前端
  - `external/tinyalsa`：v2.0.0，前端的 ALSA 库
- 板级的东西放在 `board/rg351p/` 下，包括内核 DTS 和配置片段、两套 U-Boot 的配置片段、主线 U-Boot 的 dtsi、`genimage.cfg`、`boot.ini` 和 `extlinux.conf`。
- 内核基于 allnoconfig 合并 `rg351p.config`，所有驱动内置，没有模块和 initramfs。`debug.config` 默认也合并进去（`LINUX_DEBUG=0` 去掉），里面是 fbcon 和前端调试日志要读的 `/proc/asound`、debugfs、`/dev/mem`。makefile 会检查片段里的符号是否都生效。
- 两个核心各自用 `mk/core-archive.sh` 合成一个只导出 `<core>_retro_*` 的目标文件，这样能静态链接进同一个程序而不撞符号。
- 主线 U-Boot 从 `odroid-go2_defconfig` 合并片段，proper 阶段改用主线 RG351M 的 DT 并通过 `DEVICE_TREE_INCLUDES` 加上自己的 dtsi，避开 OGA DT 里 `vcc_host` 打开震动马达的问题。`DDR_INIT=rkbin|open` 和 `BL31=tfa|rkbin` 留着给以后排查用。
- 镜像里用前端直接当 PID 1（`/sbin/init`），不用 BusyBox。rootfs 里只有这一个程序。
- 镜像用 genimage 生成，布局见“当前状态”。
- 主机依赖：`genimage mtools dosfstools fakeroot cmake ninja-build`，主线 U-Boot 另外要 `swig python3-setuptools python3-pyelftools python3-dev libssl-dev bison flex`，`make frontend-test` 要 `qemu-user`。

### 显示输出（CPU 加 KMS dumb buffer，不用 GPU）

- 直接用 DRM/KMS 的 dumb buffer。分配两块做双缓冲，通过 page flip 提交，翻页对齐垂直同步，不会撕裂，vblank 事件还可以用来给模拟器定节奏。前端直接调 ioctl，连 libdrm 也不用。
- 不用 `/dev/fb0`。它只是 DRM 上面的一层兼容模拟（`CONFIG_DRM_FBDEV_EMULATION`），双缓冲和垂直同步不可靠，容易撕裂。开发初期可以拿它快速出图验证。
- 不用 SDL2 的 KMSDRM 后端。它要走 GBM 和 EGL，会把 Mesa 带进来。
- 放大和旋转由 CPU 一次完成。屏幕是竖装的 320×480，PX30 的 VOP 应该不支持硬件 90° 旋转（待确认）。GBA 的 240×160 放大 2 倍再旋转，每帧写 15.4 万像素，60fps 时大约每秒 920 万像素。用 NEON 估算只占一个核心的 1% 到 3%，相比模拟器本身可以忽略。
- 不用 GPU 的理由
  - 每帧上传纹理、提交任务、等 fence，驱动在 CPU 上的固定开销很可能比直接画还大
  - GPU 电源域要一直开着
  - 要带上 Mesa 加 Panfrost 或者 libmali，rootfs 变大，开机多初始化一个驱动
- 省电相关
  - 不加载 GPU 驱动，genpd 会在启动后期关掉没人用的电源域，Mali 可以完全断电（实机日志已确认 gpu 和 vpu 都是 off-0）
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

2026-10-18，legacy U-Boot 加主线 6.18.57 内核加 hwtest（早期的硬件测试程序，已删除）：

- 显示正常，DSI 面板、背光、fbcon 都工作，`fbcon=rotate:3` 和 hwtest 的 270° 旋转方向正确
- VOP 支持 RGB565 扫描输出
- 屏幕实际刷新率 60.02Hz
- 内核启动到 hwtest 出第一帧约 2014ms（CLOCK_BOOTTIME，不含 bootloader）
- 机内 USB 手柄在 dwc2 上工作，按键和摇杆都有事件
- 按电源键能真正断电，`rk817` 的 `system-power-controller` 有效
- 指示灯：GPIO2_B5 拉高红、拉低绿，上电默认是绿色

## 待确认

已经确认的：Mali 和 VPU 的电源域在不加载驱动时会断电（genpd 显示 off-0）。按 10mΩ 配的库仑计能读出合理的电量和电压。

还没确认的：

- RG351P 和 RG351M 的屏幕是否完全一致，早期批次 RG351P 有没有换过屏
- 主线 U-Boot 为什么停在 SPL 之前或之中，开源 TPL DDR 初始化能否工作
- 电流读数是否准确（电量和电压正常，电流没有专门核对）
- 主线内核上深度睡眠能否正常进入和恢复，包括 dwc2 和 USB 手柄
- PX30 VOP 是否支持硬件旋转
- RG351P 主板上 UART2 测试点的位置
- 机内 USB 手柄的具体芯片和枚举耗时

待实机验证的功能和已知问题见 [roadmap.md](roadmap.md)。

## 当前状态

2026-10-27 更新。legacy U-Boot 加主线 6.18 内核，前端（`app/frontend`）作为 PID 1 直接启动，内置 gpSP 和 mGBA 两个核心。

### SD 卡布局

| 位置 | 内容 |
|---|---|
| 32KiB 到 16MiB，分区表之外 | bootloader：legacy 是 idbloader（扇区 64）、`uboot.img`（8MiB）、`trust.img`（12MiB） |
| p1，FAT，`BOOT`，从 16MiB 开始 | `Image`、`rk3326-anbernic-rg351p.dtb`、`boot.ini`、`extlinux/extlinux.conf` |
| p2，ext4，只读 | rootfs：`/sbin/init`（前端） |
| p3，FAT，`GBA` | ROM、存档、设置和日志，前端挂在 `/mnt/data`。镜像里只有 64MiB，第一次开机时扩到整张卡并格式化成 FAT32（见下） |

GBA 分区（下面写作 `<data>`）里的文件

| 路径 | 用途 |
|---|---|
| `roms/*.gba` | ROM，可以放在任意深度的子文件夹里 |
| `saves/<rom>.sav` | 存档，两个核心通用，只按文件名（不含文件夹）；没有 `.sav` 时会读同名的 RetroArch `.srm`，之后写回 `.sav` |
| `saves/<rom>.<core>.state` | 即时存档，退出游戏时自动写，下次启动同一个 ROM 用同一个核心时恢复（见下面的默认行为）。和 `.sav` 一样只按文件名，按核心分开（`gpsp`、`mgba`），因为两个核心的格式不通用。mGBA 约 420 到 520KiB（随存档类型变），gpSP 固定约 416KiB |
| `bios/gba_bios.bin` | 可选的 GBA BIOS。没有的话 gpSP 用内置 BIOS，mGBA 用 HLE |
| `config/settings.txt` | 全局设置：音量、亮度、LCD 网格、CPU 模式、音频模式、调试日志 |
| `config/roms/<rom>.txt` | 单个游戏的设置：`color`（auto、off、gba、gba-light、sp101）、`core`、`blend`（gpSP 的帧混合）、学到的起始频率 `cpu_start`。只写和默认值不同的项。和存档一样只按文件名，不同文件夹里的同名 ROM 共用 |
| `config/romdb-cache.txt` | ROM 识别时算过的 CRC32 缓存，按相对 `roms/` 的路径（顶层的 ROM 就是文件名，和旧缓存兼容）、大小和 mtime |
| `mixer.txt` | 可选，开机时应用的 ALSA mixer 设置，行前可加 `speaker:` 或 `headphones:` |
| `log/boot-NNNN/` | 每次开机一个目录，保留最近 20 个，有 `frontend.txt` 和各个时间点的 dmesg；`log/LATEST.txt` 指向最新的一个 |
| `log/perf.csv` | 每局游戏的性能、功耗和音频统计，跨开机追加 |

`roms/` 和 `saves/` 在镜像里就有，其余目录前端第一次启动时创建。

第一次开机时前端在挂载 GBA 分区之前把它扩到卡的末尾（`app/frontend/sdgrow.c`，4MiB 对齐，MBR 最多 2TiB）：文件先拷进 tmpfs，旧分区原样备份到卡末尾的空闲区，改 MBR 里 p3 的大小，格式化成 FAT32（卷标 `GBA`，簇大小按 Windows 默认），再把文件拷回去并逐个比对。屏幕上显示 "Preparing SD card"。中途断电的话下次开机从备份恢复再重来，连续中断 3 次或者当次出错就恢复原样不再尝试。日志在那次开机的 `frontend.txt` 里，都以 `sdgrow:` 开头。之后的开机只读 MBR 和分区末尾 4KiB，记一行 `nothing to do`。

### 默认行为

- 核心默认 gpSP（ARM64 动态重编译），可以在游戏菜单里按游戏改成 mGBA，只有改过的游戏才会写 `core=`。
- 音频默认 adaptive，48kHz 非阻塞写入加动态速率控制，跟屏幕的 60.02Hz 对齐。`audio=simple` 是固定比例加阻塞写入的简单路径。
- CPU 调频用 schedutil，游戏中前端根据帧耗时通过 uclamp 设频率下限（`cpu=adaptive`，也可以选 `max` 或固定频率）。
- 调试日志默认关闭。打开后（设置里的 debug，或 `debug=on`）会多记周期性的 perf 行、mixer、`/proc/asound`、ALSA xrun 报告、内核日志和休眠调试信息。这些依赖内核的 `debug.config`。
- ROM 列表显示 No-Intro 标准名称（`romdb.c`，数据由 `make romdb` 从 libretro-database 的固定 commit 生成到 `romdb_data.c`）。先按 ROM 头的游戏代码查，同一代码有多条时用 CRC32 区分，代码对上但 CRC 对不上的名字后面加 `*`，查不到显示文件名。
- 色彩校正默认 `auto`，按首次发行年份选：2004 年及以前 `gba`，2005 年以后 `sp101`，年份未知 `off`。
- Frame blend（gpSP 的 50:50 帧混合）默认关，按游戏在菜单里开，切换立即生效。mGBA 的 libretro 接口没有这个功能。
- LCD 网格效果每次开机随机选 2x2 里的一个像素当亮点。
- ROM 列表支持子文件夹：文件夹排在 ROM 前面，名字后面带 `/`，黄色显示。A 进入，B 回上一层并选中刚才的文件夹，顶层按 B 没有反应。标题栏在文件夹里显示相对 `roms/` 的路径，太长时从左边截掉。每次进入文件夹只扫描这一层。退出游戏回到原来的文件夹和 ROM，但当前文件夹和选中项不跨开机保存。
- 按键：ROM 列表里按 SELECT 进设置，游戏中 SELECT+START 打开菜单。设置和游戏菜单里都有只读的 System info 页（`sysinfo.c`），每秒刷新 CPU 占用和频率、内存、温度、电池、日期时间、开机时长和 GBA 分区剩余空间。
- 日期和时间：ROM 列表设置里的 Date & time（`clock.c`）可以设 年-月-日 时:分，左右选字段，上下改数值（月份和闰年会限制日期），A 保存，B 取消。保存时先 `settimeofday` 再用 `RTC_SET_TIME` 写 RK817 的 RTC（秒归零），写 RTC 失败时页面上显示红色提示。没有时区，输入的时间按 UTC 存，`localtime()` 等于 `gmtime()`，所以核心给游戏卡带 RTC 的就是输入的时间。系统信息页显示当前日期时间，ROM 列表标题栏在时钟设过以后显示 HH:MM。开机日志记 `clock: system ..., RTC ...`，RTC 早于 2024 年会标 `looks never set`；每次设置记旧时间、新时间、每一步的结果和 RTC 读回的值。
- 会中断游戏的菜单项都要按两次 A：游戏菜单里的 Restart game、换了核心后的 Restart with 和 Power off，以及 ROM 列表设置里的 Power off。第一次按 A 该行变成黄色的 "Press A to restart" 或 "Press A to power off"，3 秒内再按一次才执行，移到别的行、超时或者再换核心就取消。
- 退出游戏时自动保存即时存档，下次启动接着玩。退回 ROM 列表、关机（游戏菜单、设置、长按电源键）、Restart with 换核心（存在旧核心名下，换回来时还能接上）和休眠时，先写 `.sav`，再把 `retro_serialize` 的结果原子地写到 `saves/<rom>.<core>.state` 并 fsync。启动游戏时在读完 `.sav` 之后、第一帧之前读回来。`.sav` 仍然是主记录：文件被截断或损坏（头里有 CRC）、ROM 换了（大小和 ROM 头 CRC 对不上）、或者存档内容和写即时存档时不一样（比如从电脑换了 `.sav`）就跳过，正常开机，什么都不删。没有用文件时间比较，因为从电脑拷过来的文件可能保留旧时间，RTC 没设过时每次开机的时钟也是从同一个点开始。游戏菜单的 Restart game 会删掉当前核心的即时存档，出错退出（fatal）不写。日志都以 `state:` 开头，记文件名、大小、耗时和跳过的原因。
- 电源键短按休眠，先尝试挂起到内存，内核挂不起来就退回浅睡眠（关背光和屏幕，CPU 降到最低频率），再按一次唤醒。长按 2 秒保存并关机。
- 指示灯：内核阶段是 heartbeat 闪烁，前端启动时变红，第一帧出来后变绿，低电量时绿灯里隔 2 秒闪一下红，出错时红绿交替。

### 以后

见 [roadmap.md](roadmap.md)。

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
