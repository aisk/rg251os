# 格斗引擎数值与属性系统设计

2026-11-03。本文是设计稿；第一步（属性、来源、体系档案）已经实现，现状见[格斗文档](fighter.md)，与本文的差异是侍魂跳跃改用 -42/2。现状、格式和执行顺序见[格斗文档](fighter.md)，引擎取舍见[方案调研](fighting-engine-research.md)。目标是让不同原作（KOF 2002、侍魂 V Special、龙虎之拳、饿狼/Real Bout、原创角色，以后还有来自清版动作游戏的角色）的角色共用一个战斗内核，同时各自保留原作的手感和数值尺度。平衡不是优先级，好玩和"像原作"才是，但必须守住可玩底线：不能有意外的无限连、秒杀循环，画面和数值要能读懂。

## 现状和问题

今天的数值几乎都是全局常量或从 Terry 复制来的估计值。

| 项目 | 现在在哪 | 值 |
|---|---|---|
| 体力 | `fighter.c` `place()` 和 `fight_decode` 范围检查 | 所有人 1000 |
| 走路速度 | `update()` 写死 | 前后都是 2 px/tick |
| 起跳速度、重力 | `update()` 写死 | vy = -48（1/4 px 单位，即 12 px/tick），每 tick +3 |
| 起跳前摇 | `JUMP_SQUAT` | 3 tick |
| 按键缓冲、指令窗口、蓄力 | `BUFFER_TICKS` 等常量 | 4 / 15 / 20 / 30 / 10 / 4 |
| 防御显示距离 | `GUARD_RANGE` | 160 px |
| 推开速率 | `slide()` | 每 tick 剩余量的 1/4 向上取整 |
| 身体推挤 | `combat()` 末尾 | 距离小于 45 则推到相距 46 |
| 招式数值 | 清单 `moves`/`specials`/`projectiles` | 自己估的，六个角色照抄 Terry |
| 上限 | `MAX_DAMAGE` 1000、`MAX_STUN` 120、`MAX_HITSTOP` 30、`MAX_PUSH` 120 | 加载时校验 |

招式的 startup/active/recovery 已经由步推导，这点保留。本设计不改变"规则只给动作和已过 tick，包里的步决定帧、框和位移"的分层。

## 一、统一内核

### 时间与单位

逻辑固定 60 Hz，一切时长都是 tick。位置是整数像素，竖直速度已经用 1/4 px 单位，水平移动也改成 1/4 px 单位（`x` 存 4 倍值，渲染时右移 2），这样走路速度可以写 7/4 px 这类原作常见的小数，又保持整数确定性。伤害、体力、防御倍率都是整数，倍率一律用千分比（1000 = 100%），乘法后右移或整除并明确取整方向，中间值用 `int64_t`。

### 每个角色都有的属性（`stats`）

| 字段 | 含义 | 默认 | 来源 |
|---|---|---|---|
| `life` | 体力，参考尺度 1000 | 系统给 | 必填或系统默认 |
| `defense` | 受伤倍率千分比，SS 的"damage taken 120%"即 1200 | 1000 | 角色 |
| `walk` / `walk_back` | 前后走路速度，1/4 px/tick | 系统给 | 角色 |
| `jump` | 起跳 vy、重力、横向速度，见下 | 系统给 | 角色 |
| `prejump` | 起跳前摇 tick | 系统给 | 角色可覆盖 |
| `weight` | 千分比。被推开距离乘以 1000/weight，浮空重力乘以 weight/1000 | 1000 | 角色 |
| `height` | 站/蹲受击高度组 low/mid/high，只用于 AI 和工具提示 | mid | 角色 |
| `stun` | 眩晕槽容量，0 为关闭 | 系统给 | 角色可覆盖 |
| `guard` | 防御槽容量，0 为关闭 | 系统给 | 角色可覆盖 |
| `meters` | 最多两条资源槽，由系统定义，这里只能改容量和初始值 | 系统给 | 角色可覆盖 |

跳跃写成一张小表，每种跳法一行：`{"vy": -48, "gravity": 3, "vx": 8, "prejump": 3}`，键名 `jump`、`hop`、`super_jump`、`hyper_hop` 之类，系统决定哪些存在。vx 是前跳/后跳的横向速度（1/4 px/tick），后跳取相反数或单独给 `vx_back`。

### 招式描述

招式字段分三类。

时序：startup/active/recovery 照旧由步推导。新增可选的 `cancel_window`，默认是 startup + active，允许写成 `[from, to]` 覆盖（原作里很多普通技的取消点只在命中后几帧）。

命中属性（打击招式和飞行道具共用，现有 `struct hit` 扩展）：

| 字段 | 含义 |
|---|---|
| `damage`、`chip` | 原作单位，见第二节换算；chip 默认按系统规则算（KOF 必杀 1/4 左右，普通技 0） |
| `hitstun`、`blockstun`、`hitstop` | 可省略，由系统按 `class` 给默认 |
| `class` | light / heavy / command / special / super / throw，决定默认硬直、hitstop、补正组 |
| `pushback`、`block_pushback` | 像素，照旧 |
| `guard` | high / low / overhead / unblockable / throw |
| `effect` | none / knockdown / launch / sweep / wallbounce / crumple。launch 带 `[vx, vy]` |
| `juggle` | 本招命中浮空对手需要的 juggle 点数，默认 1；0 表示任何浮空都能打 |
| `counter` | 被反击（对手在 startup 里挨打）时的额外效果，默认由系统给 |
| `meter` | 命中/防御/挥空时给自己和对手的槽变化，默认由系统按 class 给 |
| `armor` | 本招有效步里能吃几下不中断（超级霸体），默认 0 |

无敌与状态：步已经可以用 `"hurt": []` 表示全身无敌，补一个 `"invuln": ["strike", "throw", "projectile"]` 的组合，`[]` 等价于全无敌，这样投技无敌和打击无敌可以分开，不用删受击框。空中状态由步的 `vy` 和 `airborne: true` 标记，落地规则不变。

### 哪些从 C 常量搬进数据

搬进角色或系统：体力、走路速度、跳跃表、起跳前摇、推挤距离（45/23）、防御显示距离、按键缓冲和指令窗口（系统级，KOF 和侍魂的宽容度确实不同）。留在 C 里的：推开速率 1/4、`MAX_*` 上限（加载校验，可以适当放宽，比如 `MAX_DAMAGE` 提到 1500 给侍魂留余量）、KO/结算画面时长、HISTORY 长度。原则是"手感相关的进数据，安全边界留代码"。

## 二、"系统"作为数据

每个角色清单声明一个 `system`，系统档案放在 `assets/fighter/systems.json`，打包时一起写进包（上限 8 个系统）。系统给出默认值和开启哪些机制，角色可以覆盖默认值，招式再覆盖角色。引擎里每个机制是一个用 `enabled` 标志守护的模块，数据不能发明新机制，只能开关和调参数，这样状态仍然有界、可序列化。

### 系统档案字段

```json
{
  "kof2002": {
    "life": 1000,
    "damage_scale": 1000,
    "walk": 7, "walk_back": 6,
    "jump": {"vy": -48, "gravity": 3, "vx": 8, "prejump": 3},
    "hop": {"vy": -30, "gravity": 3, "vx": 8, "prejump": 3},
    "stun_table": {"light": [11, 9], "heavy": [19, 17], "command": [19, 17],
                   "special": [19, 17], "super": [19, 17]},
    "hitstop_table": {"light": 6, "heavy": 9, "special": 10, "super": 12},
    "chip": {"special": 250, "super": 250, "normal": 0},
    "input": {"buffer": 4, "quarter": 15, "dragon": 20, "charge": 30, "grace": 10},
    "push_distance": 46,
    "guard_range": 160,
    "meters": [{"name": "power", "style": "stocks", "stocks": 3, "per_stock": 1000,
                "start": 0, "gain": {"hit": 45, "block": 45, "whiff_special": 60,
                "taken": 60}}],
    "mechanics": ["run", "hop", "roll", "guard_cancel_roll", "max_mode", "counter_hit"],
    "scaling": "kof"
  }
}
```

招式的 `damage` 一律写成"原作体力的千分之几"，不管原作内部用什么单位，查到百分比就乘 10，查到绝对值就除以该作品的满体力再乘 1000。`damage_scale` 是加载时乘上去的千分比，KOF 2002 是参考尺度所以写 1000，换算后正好是体力 1000 下的点数。换算在加载时做一次，运行时的 `struct hit` 里只有参考单位，规则代码不知道系统的存在。

`meters` 每条槽有名字、HUD 风格（stocks 分段、bar 单条、reverse 从满往下走）、容量和各事件的增量。引擎只提供"事件→加减"和"够不够花"两个操作，花槽的具体用途由 `mechanics` 里的模块决定。

`scaling` 选一张连段补正表，见第五节。

### 跨系统换算规则

伤害只在发出方换算，不看对手是谁。这是刻意的：同一个霸王丸无论打谁，一刀都是同一个数，玩家能建立预期；而对手的 `defense` 只表示"这个角色皮厚不厚"。所以 KOF 打侍魂和侍魂打侍魂是同一套数，没有对局依赖的隐藏换算。

侍魂的问题在于原作一记重斩约为体力的四分之一到三分之一，而且 Rage 满时更高。给 `samsho5sp` 的 `damage_scale` 取 800，重斩落在 220 到 260，轻斩 60 到 80，必杀 150 到 200。这样一刀仍然"吓人"（相当于 KOF 一套三四下的连段），但在 KOF 角色能靠跑动和小跳贴身的前提下不至于两刀半就结束。侍魂角色的补偿是：没有 KOF 的连段取消链，普通技硬直长，被 KOF 角色压起来很难受；而 Rage 槽从挨打里涨，被压到一定程度就爆发。这符合"各打各的"而不是把两边抹平。

"Dream Match" 开关把所有系统的 `damage_scale` 视为 1000（侍魂一刀真的三分之一），默认关闭。

体力统一 1000，所有系统相同，避免"KOF 角色血厚"这种不直观的事；厚薄差异通过 `defense` 表达，侍魂角色沿用原作的 damage taken 百分比。

### HUD 并存

每侧只画自己的槽。体力条下面是第一条槽，再下面（或体力条上方小字）是第二条槽。KOF 侧显示分段的 Power 槽和 MAX 计时条，侍魂侧显示单条的 Rage 槽，满了闪烁，Rage 爆发时变成倒计时条。双方槽的风格不同是故意的，玩家一眼就知道对面是什么系统。眩晕槽和防御槽如果开启，画在体力条末端的小段里，没开的系统不画。

### 示例

Terry（KOF 2002）：`system: "kof2002"`，`stats` 只写 `walk: 7, walk_back: 6`（原作 Terry 走得不快，估值）和 `height: "mid"`，其余用系统默认。普通技的 hitstun/blockstun 全部省略，由 class 决定（Dream Cancel 记录 KOF 2002 轻攻击 11/9、重攻击 19/17、必杀 19/17，CD 击倒，防御 21）。damage 按 2002UM 记录的百分比乘 10 写：cl.C 约 70、A 约 25，Burn Knuckle 100，Power Geyser 220。这与现在的 30/70/90/100 相差不大，说明目前的猜测没有离谱，但以后每个数要有来源字段。

Chin（KOF 2002）：同一系统，`height: "low"`（蹲下极矮），`walk: 6`，`hop` 的 vy 取 -27（原作小跳偏低，估值），`defense: 1000`。普通技伤害比 Terry 低 10% 左右，这点现在的清单已经体现。Chin 在 02 里有翻滚、睡倒等姿态系的必杀，本设计用 `armor`、`invuln` 和 `effect` 字段已经够描述，不需要新字段。

Haohmaru（侍魂 V Special）：`system: "samsho5sp"`，`defense: 1000`（初代霸王丸是 100%，V Special 的值待 Mizuumi 核对），`walk: 5, walk_back: 4`，只有 `jump`（侍魂跳得高且慢，vy -56、gravity 2，估值），没有 hop。Rage 槽容量按 Mizuumi 的"Amount to rage"记录，霸王丸的值待查，先估 30，持续 12 秒（Ukyo 是 45/20 秒，Rasetsumaru 15/8 秒，可见差别很大，必须查表）。招式：5A 轻斩 damage 70（换算后 56）、5B 中斩 140、5AB 重斩 300（换算后 240，startup 约 13，recovery 30 以上，blockstun 用 class heavy 但系统的 heavy 硬直比 KOF 长），2AB 约 260，旋风裂斩（236+斩）多段共 180，弧月斩（623+斩）200 带起手打击无敌，武器飞ばし（Rage 满）450 且 `effect: "disarm"`。这些数字是估计，都标 `estimate`，等拿到 Mizuumi 数据再换。

```json
"characters": {
  "haohmaru": {
    "system": "samsho5sp",
    "stats": {"defense": 1000, "walk": 5, "walk_back": 4,
              "jump": {"vy": -56, "gravity": 2, "vx": 7},
              "meters": [{"name": "rage", "capacity": 30, "duration": 720}]},
    "moves": {
      "stand_hp": {"class": "heavy", "damage": 300, "guard": "high",
                   "effect": "knockdown", "cancel": [],
                   "src": {"damage": "estimate", "steps": "sheet"}, "steps": [...]}
    }
  }
}
```

侍魂特有机制的模块化：`rage`（挨打涨槽，满时伤害倍率 1250 千分比并可用武器飞ばし，持续 `duration` tick 后清空）、`rage_explosion`（每局一次，ABC 启动，期间可用一闪，伤害随剩余时间减少）、`clash`（双方 heavy 斩击同一 tick 互相命中则不扣血、双方 hitstop 并推开，这是简化版的鍔迫り合い）、`disarm`（被武器飞ばし命中后进入徒手状态，普通技换成 `unarmed` 动作组，伤害乘 500 千分比，走到地上的武器处按键捡回）、`recoil`（重斩被防后攻方额外硬直，可用轻攻击取消）。首批只做 `rage`，其余按需要逐个加，每个模块都要在状态里有自己的有界字段并写进 `fight_encode`。

龙虎之拳的 `spirit` 槽是反过来的：满槽开局，出必杀扣，蓄力或挑衅涨，必杀威力随槽变化。用同一套 meters 描述（`start` 等于容量，`gain` 为负），`mechanics` 里加 `spirit_damage` 模块把必杀伤害乘以槽百分比。Real Bout 的线移动我们没有平面，用 `sidestep` 模块代替：短暂的打击无敌后退（像 KOF 的紧急回避但不换位），只给饿狼系角色。原创角色的系统 `original` 从 `kof2002` 复制一份再改，不绑定任何原作。

## 三、数据来源与记录

### 来源优先级

1. 原作社区帧数据：KOF 2002 用 Dream Cancel（如 [Mature 02](https://dreamcancel.com/wiki/Mature_02) 的 Startup/Active/Recovery/Cancel/Block 表）和 [KOF 2002 系统页](https://dreamcancel.com/wiki/The_King_of_Fighters_2002)（通用硬直表、槽增长、MAX 模式规则），SRK wiki 的 02 页面作为补充；伤害百分比用 [Dream Cancel 2002UM 角色页](https://dreamcancel.com/wiki/Terry_Bogard_(2k2UM))和其外链表格。侍魂 V Special 用 Mizuumi（`mizuumi.wiki/w/Samurai_Shodown_V_Special/<角色>`，原 wiki.gbl.gg 已跳转），角色页顶部有 damage taken、rage duration、amount to rage，正文有逐招表；该站有机器人验证，脚本抓不到，需要人工打开复制。日文来源（如各作品的攻略 wiki）在英文站缺数据时补。
2. MUGEN `.air` 只在帧序和原作图集一致时借用时长（很多作者是对着录像数的），借用时记录人物包作者和版本，不借用 `.cns` 里的伤害，那些是 MUGEN 自己的尺度。
3. 自己估计：按原作录像数帧，或按同类角色类推。

### 清单里的记录方式

清单顶层加 `sources`，每条给一个短 id，记 URL、标题、抓取日期和对应版本（02 还是 02UM）。每个招式加 `src`，键是字段名或 `"*"`，值是来源 id 或 `estimate` / `mugen:<包名>` / `sheet`（从图集帧数推得）/ `measured`（自己录像数帧）。缺 `src` 的字段视为 `estimate`。

```json
"sources": {
  "dc02": {"url": "https://dreamcancel.com/wiki/The_King_of_Fighters_2002",
           "title": "Dream Cancel KOF 2002 system", "fetched": "2026-11-03", "game": "kof2002"},
  "dc02um-terry": {"url": "https://dreamcancel.com/wiki/Terry_Bogard_(2k2UM)",
                   "fetched": "2026-11-03", "game": "kof2002um"}
},
"moves": {
  "stand_hp": {"class": "heavy", "damage": 70, "guard": "high", "cancel": ["special"],
               "src": {"damage": "dc02um-terry", "steps": "sheet", "*": "estimate"},
               "steps": [...]}
}
```

`pack.py data` 打印每个角色的来源统计（多少字段是 wiki、多少是估计），并在估计比例超过阈值时打一行提醒，不阻止打包。二进制包不带来源信息，只有工具看。

### KOF 2002 与 2002 UM

图集是 Neo Geo 2002 的，时序优先用 02 的数据。02UM 重做了大部分平衡、加了新招、MAX 模式的取消改成消耗两格并提高了 MAX 内补正（max cancel 后普通技和必杀约二分之一，否则约三分之二，超必杀不补正）；原版 02 的 MAX 模式里伤害约减三分之一。两者都有"MAX 模式用取消会消耗剩余时间"的规则。做法：系统档案只有一个 `kof2002`，机制按 02 原版，补正常数取 02UM 的保守值作为护栏；招式数据哪个版本有就用哪个，`sources` 里的 `game` 字段区分，以后要严格还原时按来源筛。不单独维护 `kof2002um` 档案，除非有 UM 专属角色的图集。

## 四、未来模式

### 清版动作游戏的角色

用户确定（2026-11-03）：不做清版动作模式，状态保持两个对战角色，不扩成多 actor。将来可能把清版动作游戏（如 SNK 或其他厂商的横版过关作品）的角色拉进来当对战角色。这类原作没有对战用的完整招式表，常见情况是只有普通连打、跳攻、投技和一两个费血的全屏技，也没有防御和蹲姿。处理方式和其他体系一样：给它一个自己的系统档案（例如 `beatemup`），缺的动作用模板补（防御可以用受击前一帧或原地后退帧代替，标 `src: design`），原作的连打链用 chain 取消表达，费血大招按"消耗自己体力"做成一个 mechanics 模块。体型普遍比 KOF 大，原像素 1:1 不缩放，按 `height` 和 `weight` 处理推挤和判定。

### 原创角色

原创角色从模板开始。模板就是一份完整的清单（`templates/rushdown.json` 之类），带 `system: "original"` 和一组按类型调好的 `stats` 与招式骨架，作者换帧、调框、改数。建议四个模板：

| 模板 | 体力/防御 | 速度 | 特点 |
|---|---|---|---|
| rushdown | 1000 / 1050 | 走 8、有 hop 和 run | 轻攻击链长，伤害低 |
| grappler | 1000 / 900 | 走 5、无 hop | 投技伤害 200+，霸体必杀 |
| zoner | 1000 / 1100 | 走 6 | 飞行道具多，近身弱 |
| heavy | 1000 / 850 | 走 5、跳慢 | 单下 150 以上，硬直长 |

原创角色也要填 `src`，值统一 `design`，表示是设计值而非还原。

## 五、好玩优先的旋钮和护栏

### 全局与闯关修饰

通过 libretro 核心选项和游戏内菜单提供，存进状态的 `rules` 字段，回放时一起记录：

- 伤害倍率 50 / 75 / 100 / 150 / 200%。
- Dream Match：`damage_scale` 全部按 1000，各系统按原作比例，侍魂一刀见血。
- 槽规则：正常 / 开局满槽 / 无限槽 / 无槽。
- 体力让步：某一方体力 ×1.5 或 ×0.5，闯关后期关卡给电脑。
- 连段补正开关（默认开）和 hitstop 倍率（给喜欢重手感的玩家）。
- 闯关修饰：某一关随机套用上面的组合，HUD 提示本关规则。

### 护栏

这些默认开启，Dream Match 也不关：

| 护栏 | 规则 |
|---|---|
| 连段补正 | 第 n 下伤害乘 scaling 表，`kof` 表为 100、90、80、70、60、50、40、30，之后 30，超必杀最低 50；`samsho` 表为 100、70、50，之后 50（侍魂连段短，但一刀重） |
| 单段最低伤害 | 补正后至少 1，chip 至少 1 |
| 连段伤害上限 | 一套连段（对手没有回到自由状态）总伤最多 600，超过的部分丢弃；超必杀收尾可到 700 |
| Juggle 点数 | 对手被 launch 时得 3 点，每次空中命中减去招式的 `juggle`，点数不够不命中（穿过） |
| 硬直衰减 | 连段第 6 下起 hitstun 减 2，第 10 下起减 4，最低 4；blockstun 不衰减 |
| 循环断路器 | 同一招在一套连段里第三次命中起 hitstun 置 0，对手直接恢复，防止意外无限连 |
| 眩晕 | 开启眩晕槽的系统每局最多晕一次，晕后槽清空且 10 秒内不再积累 |
| 削血不 KO | 默认 chip 最低留 1 点体力，侍魂系统关闭此条（原作削血能杀） |
| 投技保护 | 起身 9 tick 投技无敌，受击硬直中不可投，连段中投技伤害按补正表 |
| 霸体上限 | `armor` 最多 3，吃到超必杀或投技一律中断 |

连段计数和 juggle 点数是防守方的字段（`combo`、`juggle`），对手回到自由状态即清零，进状态序列化。

## 六、分阶段实施

### 第一步：属性、来源、系统档案（KOF 2002 为参考尺度）

1. `systems.json` 和加载：打包器把系统表写进包，`struct assets` 加 `struct system sys[MAX_SYSTEMS]`，角色带系统索引。角色 `stats` 缺省从系统取，加载后角色结构里是完整值，规则代码只读角色。
2. 把体力、走路、跳跃表、前摇、推挤距离、防御距离、输入窗口从 C 常量改成读角色/系统，`place()` 用角色 `life`。
3. 招式加 `class`，hitstun/blockstun/hitstop 可省略；加载时按系统表填。`damage` 写原作体力的千分比，加载时乘 `damage_scale` 再除以 1000；`struct hit` 里存换算后的值。
4. `src`/`sources` 只在 `pack.py` 处理，`data` 子命令输出来源统计。
5. 包格式升到 FIGHTP05，状态升到 FGSTATE5（角色结构和 `fighter` 结构的字段变了，水平坐标改 1/4 px），旧包和旧状态直接拒绝，不迁移。`fight_decode` 的范围检查改成读角色属性（体力上限、vy 范围）。
6. 现有七个清单：加 `system: "kof2002"`，普通技删掉 hitstun/blockstun 改用 class，damage 按千分比单位复核（KOF 的值数字上基本不变）并标 `estimate`，Terry 的必杀和 Power Wave 用 Dream Cancel 查到的值替换并标来源。

清单片段：

```json
{
  "sources": {"dc02": {"url": "https://dreamcancel.com/wiki/The_King_of_Fighters_2002",
                       "fetched": "2026-11-03", "game": "kof2002"}},
  "characters": {
    "terry": {
      "system": "kof2002",
      "stats": {"walk": 7, "walk_back": 6, "height": "mid"},
      "moves": {
        "stand_lp": {"class": "light", "damage": 25, "guard": "high",
                     "cancel": ["chain", "special"], "pushback": 14, "block_pushback": 18,
                     "src": {"hitstun": "dc02", "*": "estimate"}, "steps": [...]}
      },
      "specials": {
        "burn_knuckle": {"class": "special", "trigger": {"motion": "214", "buttons": "P"},
                         "damage": 100, "guard": "high", "effect": "knockdown",
                         "src": {"damage": "dc02um-terry", "*": "estimate"}, "steps": [...]}
      }
    }
  }
}
```

验收：`pack.py data` 表格与改前的 startup/active/recovery 一致，回归测试里的确定性对局哈希更新一次并说明原因，`make fighter-test` 和 `make frontend-test` 零 warning。

### 第二步：护栏和命中效果

连段计数、补正表、juggle 点数、硬直衰减、循环断路器、`effect` 的 knockdown/launch/sweep，与 P1 第 5 步（投技、倒地、起身）一起做，因为倒地状态是它们的落点。反击判定（`counter`）同时加，默认反击伤害 ×1250 千分比并延长 hitstun 4 tick。

### 第三步：槽与 KOF 机制

meters 框架和 HUD，Power 槽的增长、超必杀消耗（Terry 的 Power Geyser 作为第一个 super），然后 run/hop、紧急回避、Guard Cancel 回避。MAX 模式放最后，它牵扯取消规则。

### 第四步：霸王丸与 `samsho5sp`

导入图集，先只开 `rage` 模块与侍魂的硬直表，验证一刀的手感和跨系统对局是否好玩，再决定 `clash`、`disarm`、`rage_explosion` 的顺序。这一步会第一次检验"发出方换算"是否够用。

### 第五步：旋钮与闯关修饰

核心选项、菜单项、`rules` 进状态，闯关随机修饰。

### 第六步：原创模板和清版动作游戏角色

模板清单、`original` 和 `beatemup` 系统档案。这步最远，放在前面都稳定之后。不做清版模式。

### 迁移

没有向后兼容。每次格式升级同时改 `pack.py` 开头的布局说明、`render.c` 的魔数、`fight_decode` 的版本号和 `assets/fighter/README.md` 的 schema，旧包和旧存档被拒绝并记录原因。清单的改动用一个脚本一次性改写七个 JSON（删 hitstun/blockstun、加 class 和 src），不留两种写法。

## 参考

- Dream Cancel KOF 2002 系统页，通用硬直表与槽规则：https://dreamcancel.com/wiki/The_King_of_Fighters_2002
- Dream Cancel KOF 2002 Mature，帧数据表格式示例：https://dreamcancel.com/wiki/Mature_02
- Dream Cancel KOF 2002UM Terry，超必杀伤害百分比：https://dreamcancel.com/wiki/Terry_Bogard_(2k2UM)
- Dream Cancel KOF 2002UM FAQ，MAX 模式补正与 02/02UM 差异：https://dreamcancel.com/wiki/The_King_of_Fighters_2002_UM/FAQ
- SRK wiki KOF 2002 FAQ，原版 MAX 模式减伤约三分之一（站点有机器人验证）：https://srk.shib.live/w/The_King_of_Fighters_2002/FAQ
- Mizuumi 侍魂 V Special 角色页（damage taken、rage 数据，需人工打开）：https://mizuumi.wiki/w/Samurai_Shodown_V_Special/Haohmaru 、https://mizuumi.wiki/w/Samurai_Shodown_V_SP/Ukyo_Tachibana 、https://mizuumi.wiki/w/Samurai_Shodown_V_SP/Rasetsumaru
