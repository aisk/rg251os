# 格斗引擎交接：从软件 POC 继续

2026-11-03。供没有本会话历史的新会话独立接手。当前任务是保存已经实现并完成首轮硬件测试的阶段，不在本次交接时继续扩展引擎。后续执行从下文的优先事项开始。

代码阶段已提交为 `8fe0f28`（`Add a libretro fighting game prototype`）；此 handover 与硬件记录作为后续文档提交，工作分支为 `main`。交接时重新执行 `make image`、完整 `make frontend-test`，均退出 0 且零 warning；本机输出保存在 `/tmp/fighter-handover-image.log` 和 `/tmp/fighter-handover-test.log`，不随 git 分发。

## 先读什么

按顺序读仓库根目录的开发约定、[README](../README.md)、[硬件当前状态](hardware-research.md#当前状态)、[路线图](roadmap.md)、[POC 实现和真机结果](fighter-poc.md)，最后按需要查 [方案调研](fighting-engine-research.md)。方案包含未来设计，实际完成范围以 POC 文档和代码为准。

仓库是 `/home/asaka/rg251os`，目标设备 Anbernic RG351P（仓库名不是设备型号），RK3326/Cortex-A35，480×320 逻辑画面、320×480 旋转扫描、约 60.02 Hz。静态 musl 用户态，`app/frontend` 是 PID 1，没有 shell、图形桌面或串口；GPU 驱动/用户态栈未启用。legacy ROCKNIX U-Boot 是已选定路线，此任务不改启动链。

## 用户已经确定的目标和约束

- 自研拳皇式传统横版 1 对 1，以拳皇、侍魂等跨作品角色对战为方向。只需背景、共同地面与左右边界；不做平台规则，不考虑四人。
- 无需兼容 MUGEN 人物行为。可以参考 MUGEN/相关开源项目的实现，也可以从人物包或已提取图集取素材；代码借鉴须核对许可。
- 人物优先原作像素、高质量、版本与来源明确的素材，避免重绘、滤镜放大。当前合成测试剪影只是验证工具，不能当成已交付的原作角色。
- 标准 libretro core，本机静态链接、桌面动态库。先做软件渲染，规则与绘制指令隔离，保留将来其他设备/GPU 后端空间；暂不同时维护多条设备渲染路径。
- 尽量减少人工上电：先做好主机正确性回归、真实素材准备和足够的诊断，再合并成一次硬件测试。不要要求用户仅为补足本次压力测试的十分钟再次上电。
- 用户已授权实现 POC；当前仍是 P0 阶段，未完成 P1 的完整格斗系统。下一步以优化与资源验证为主，不自行扩大到联网、通用脚本或完整角色库。

## 已有代码和工具

| 路径 | 作用 |
|---|---|
| `app/fighter/fighter.h` | 有界状态、资源、绘制指令；固定 POC 常量 |
| `app/fighter/fighter.c` | 确定性移动、跳跃、一种攻击、后方向防御、相打结算、AI、KO/重开及场景命令 |
| `app/fighter/render.c` | 固定包校验与 RGB565 软件渲染：裁剪、翻转、透明/alpha/add |
| `app/fighter/libretro.c` | libretro 导出，输出格式协商、输入、音频、存档、每窗口计时和完成标记 |
| `app/fighter/test.c` | 原生回调宿主、确定性/战斗边界/坏包/输出格式/状态/端口/自动压力回归 |
| `tools/fighter/pack.py` | 自制测试精灵生成，或导入 16 张严格 P6 PPM |
| `tools/fighter/frontend-test.sh` | 真实前端的 qemu 加载、坏包、状态、休眠、菜单重启、开机恢复和场景日志测试 |
| `tools/fighter/report.py` | 从 `frontend.txt` 或 boot 目录汇总计时窗口 |
| `mk/fighter.mk` | ARM64 静态重命名库、桌面 `.so`、资源包和两种宿主回归 |

前端新增 `CORE_FIGHTER` 和 `.fight` 识别，自动按后缀选择核心，使用独立游戏菜单，不加 GBA 色彩或网格效果。存档使用 `saves/<文件名含.fight>.fighter.state`，不写 `.sav`。菜单、休眠、电源和开机续玩复用宿主。`mk/image.mk` 将两个自制内容包放进新镜像的 `roms/`。

画面：480×320，核心 RGB565 优先，宿主不接受时转 XRGB8888；旋转只在前端。音频：60 Hz/48 kHz，每次 run 生成 800 个立体声采样帧，仅测试蜂鸣。核心有 port 0/1 和禁用端口支持，桌面选项 `fight_players=two`；设备前端目前只提供 port 0。

按键：方向移动/上跳，A/B/X/Y 都是同一种攻击，后方向防御，L 判定框、R 换压力、START 暂停，SELECT+START 系统菜单。无必杀、投技、Hitstop、取消、倒地系统和选人。

## 当前资源和存档格式

`.fight` 是固定 POC 二进制，不是最终人物格式：8 字节 `FIGHTP01`，随后 6 个小端 uint32（宽 128、高 160、精灵数 16、automatic 0/1、payload FNV-1a、reserved=0），随后每像素 RGB565 小端 2 字节加 alpha 1 字节，共 983072 字节。2 角色各 8 姿势：idle A/B、jump、windup、strike、recovery、hurt、KO；脚底锚点 (64,160)。PPM 导入洋红透明，不会自动切图或生成判定。

核心状态是 144 字节：`FGSTATE1`、内容摘要、automatic 模式和 32 个明确编码的小端字段；设备前端加外层校验后文件为 184 字节。改变规则/序列化字段时要明确升级或拒绝旧状态，不要误恢复。渲染缓存不存入状态；坏存档不得部分修改现有状态。

原作素材方面，已下载并目视检查 Sprite Database 的 KOF 2002 Terry 整张与原尺寸局部；提取者标注 Heatpark。PNG 1144×6860，位于忽略的 `build/fighter/source/KOF02_Terry.png`，**尚未切图或导入，也未进入镜像/源码**。详情、下载链接、SHA256 与侍魂霸王丸候选见方案末尾“原作素材候选与实际检查”。其他机器可能没有该 build 文件，需按记录重新获取。Spriters Resource 直接请求曾返回 403，Sprite Database 的直接 PNG 地址可下载；不要把“有网页”当成已验收图集。

## 已有验证和产物

已通过 `make image`、完整 `make frontend-test`，零构建 warning；原生 ASan/UBSan 源码测试与实际动态链接 `.so` 的回调测试通过。另跑 36010 帧长测试，确认 36000 次有效推进只输出一次完成标记。没有人工启动 RetroArch。LeakSanitizer 因沙箱 ptrace 限制关闭，不宣称泄漏检查通过。

```sh
make fighter
make fighter-test
make frontend-test
make image
ASAN_OPTIONS=detect_leaks=0 FIGHTER_LONG_TEST=1 build/fighter/test \
  build/fighter/Fighter-POC.fight build/fighter/Fighter-Auto.fight \
  build/fighter/preview-long.ppm
```

产物：`build/sdcard.img`（128 MiB）、`build/rootfs.ext4`、`build/frontend/frontend`、`build/fighter/fighter_libretro.so`、`build/fighter/Fighter-{POC,Auto}.fight`。源码编译时使用 `external/mgba/src/platform/libretro/libretro.h`，并不表示核心依赖 mGBA 的模拟逻辑。主机/QEMU 时间仅用于正确性，不能当 RG351P 性能。

构建环境里沙箱内的 fakeroot 曾报 chown Invalid argument；在允许的沙箱外执行同一个 `make image` 后成功，无需为这个问题修改 rootfs 配方。后续出现同样情况，先判断执行环境。

## 首轮硬件证据：只看 boot-0015

本机原始资料：`build/cardlog-20261103b/log/boot-0015/frontend.txt`、`dmesg-poweroff.txt`、同批 `log/perf.csv`。用户确认目标是 boot-0015，其他 boot 是历史记录。原始日志/存档/配置不提交，换机器时依赖已写入正式文档的结果，必要时向用户索取原始日志。

```sh
python3 tools/fighter/report.py build/cardlog-20261103b/log/boot-0015/
```

Max 下稳定窗口的核心合成平均：正常两人 5.49 ms，额外 8/24/48 个 128×160 混合精灵分别 9.46/18.74/32.91 ms；旋转输出另约 3 ms。前两档约 60fps，后两档约 30/20fps。战斗逻辑约 3–5 微秒，瓶颈主要是合成，不是 AI。最高温度 47.7°C，采样频率仍 1296MHz；没有支持热降频为主因的证据。整个前端峰值 RSS 始终 21920KiB，包含先前 GBA 会话，不能视作纯核心内存。

长 Auto 会话约 384.2 秒有效游戏时间（Adaptive 79.4、Max 304.8）；累计 2724 次音频 underrun，集中在过载场景，普通负载完整窗口为零。核心每次 run 只产 800 音频帧，过载使核心调用频率下降，声音也跟着缺；增大缓冲不能持续补救。宿主当前每 vblank 至多推进一次，没做严格 60Hz 或过载时的渲染跳过/追赶策略。

尚未达到十分钟/36000 tick，不能宣布长期稳定性通过；已足够定位，**无需单独补跑旧版本**。已看到加载、系统菜单、退出保存和再次加载恢复；此 boot 没有 fighter 休眠、菜单重启、开机续玩记录，不能把主机回归通过写成这些真机也通过。主观画面/声音效果也不能从日志推断。

报告工具会合并同一日志里所有 fighter 会话，打印的部分 perf 总结还包含该 boot 的 GBA 会话；正式表格是另外筛选开机 250 秒之后的 Max 窗口得到的。第一段 perf 计数存在前一会话残留（665 vs 600 等），后续值得修复会话边界计数。窗口 p99 只能报“最大窗口 p99”，不能拼成总体 p99。详细采样范围和限制见 POC 文档。

## 后续执行顺序和验收

1. **先优化软件合成并保证像素等价。** 审查 `render.c` 的通用逐像素循环，将不透明矩形填充/精灵复制分开，预处理透明范围或 span，减少逐像素分支和重复 alpha 运算。先用独立标量参考实现或固定样例验证裁剪、翻转、透明、alpha/add、RGB565 舍入和饱和的逐像素结果，再测主机热点。不要用视觉相似代替等价；不要只加重复实现本身的测试。存在多种优化选择，尚未选定 NEON，也没有已经写好的优化分支。
2. **准备真实素材样例。** 从已有 Terry 与一个持刀型角色候选开始，记录版本/提取者/摘要，保持原像素比例，整理锚点与帧序；必要时扩大 POC 格式以避免把宽刀光裁掉或强制拉伸成统一身高。当前固定 128×160/8 姿势不能冒充最终角色管线。先做动作和特效负载样例，不立刻做完整招式库。
3. **补齐诊断与过载设计。** 区分普通角色和最坏特效预算，隔离会话/场景/CPU 档位的统计；考虑前端 perf 会话残留和报告工具混合会话问题。明确负载超时的逻辑、声音和显示策略，不盲目扩大缓冲或在核心里按墙钟追赶。正常 60.02Hz 与 60Hz 的约 0.03% 偏差目前接受并记日志。
4. **主机验证后集中一次上机。** 必须 `make image`、`make frontend-test` 零 warning 后再提交；保留可比较的同场景日志。下一次同一开机测 Max/Adaptive、正常真实资源、受限特效、持续十分钟、恢复/休眠与主观延迟。初始正常对战总工作目标 p99<12ms，不要求任意数量混合精灵满速。软件仍缺余量时才单独比较 GPU 成本；当前不启用 Panfrost/EGL。
5. 这些完成后才按方案推进 P1（普通技/指令必杀/投技、防御硬直、Hitstop/取消/倒地、完整对局与可重放规则），每阶段保持可验收。

## 部署和文档注意事项

每个前端源文件保持准确的功能头注释；功能落地或硬件结果返回后更新 roadmap 与 hardware-research 当前状态。新功能必须有一次测试能诊断的日志。当前 fighter 日志默认开启聚合，不需要用户手动打开 Debug log。

本次只交接并提交/push，不再次写卡或上电。将来若只更新前端/核心且内核、dtb、bootloader 没变，可以按 roadmap 的现有流程仅更新 p2 rootfs 和 GBA 分区的内容文件，保留存档/配置/日志；设备路径与分区布局每次重新确认，不能照抄旧 `/dev/sdb`。不要因“减少上电”把远程写卡当成已执行或默认授权的本次步骤。

旧的 compact 阶段临时交接文件 `fighter-poc-handoff.md` 已删除，其有效信息已并入正式 POC 文档；本文件是供独立新会话使用的新 handover。后续工作完成后可将剩余内容并回正式文档，再删除本文件。
