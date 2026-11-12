// timefreeze.cpp —— 时间冻结 (TimeFreeze v1.3.6 for 1.30 build 25640656：睡觉/强制睡眠时解除冻结)
//
// v1.3.5 → v1.3.6-diag（2026-10-31，**只加诊断，不改冻结/放行行为**）：
//   新增 G7 机理探针三个计数器，经 obs 每秒发布（OBS_KEYS=8，本 MOD 用满 8 个）：
//     adddayCalls  = FUN_14048d340 累计调用次数（进 A 站点即计）
//     adddayFrozen = 其中处于冻结状态的次数（>0 即证明 G7 在冻结期偷写时钟）
//     adddayJump   = 最近一次 G7 会推掉的秒数（≈86400 = 直接跳整天）
//   依据（反编译 FUN_14048d340 实证，KB-099）：
//     *(save+0x3268) = 0;                              // A 站点 0x48D3DE（被 NOP 冻结）
//     lVar = (*(save+0x3270)/86400 + 1) * 86400;       // 算下一个整天边界
//     *(save+0x3270) = lVar; *(save+0x3278) = lVar;     // G7  0x48D40D（放行）
//     A 与 G7 **无条件顺序执行**，故「该函数被调用 N 次」==「时钟被推 N 天」。
//   探针只读 cur、只写本 MOD 全局变量，不触碰游戏内存。

// v1.3.4 → v1.3.5（2026-10-29，实机 A/B 实测归因 + 冻结机制重构）：
//   实测（AI 自主驾驶全流程：读档→F2 菜单开冻结→床边 J 睡觉）：
//   ① 冻结中睡觉 → Now Loading 卡死（50s+ 不动，复现 v1.3.2 症状）；
//   ② **两个睡觉 hook（WaitUntil/SetTime）全程零触发**（[tf][probe] 一条都没打）
//      → 1.30 的睡觉推进根本不走这两个函数（v1.3.2 的"错位 hook 毁代码"不是
//      主凶，只是叠加风险）；【修正认知】
//   ③ 手动解冻瞬间 → 流程立即恢复（时钟推进、亲密度事件结算、自动存档落盘）
//      → **卡死元凶实锤 = 5 个时钟写点 NOP 封死了睡觉的推进路径**。
//   另一个关键教训：解冻"晚到"会让睡觉流程半僵（时钟推到 0:00 后停住，
//   床交互失效，流程状态机错过设时刻步骤）→ 解冻必须发生在**游戏试图
//   推进时钟的那一条指令上**，100ms 轮询信号源无法做到。
//   方案（本版核心）：写点从"无脑 NOP"升级为"**条件放行 trampoline**"：
//     - 每个写点装动态生成的小桩（保存寄存器→调用判定器→恢复→jmp 回站点尾）；
//     - 判定器 TfSiteGate 读 base(=save 指针) 与 src(=待写入的新时钟值)：
//       src 跨天（dayIndex 变化）或单次跳变 ≥ TF_PASS_MINJUMP 秒（默认 300，
//       自然流逝不可能达到）→ **当场放行写入** + 置解冻信号；否则拦截（什么都不写，
//       等效原 NOP），时钟冻结语义不变；
//     - worker 100ms 内恢复 .text 原字节完成解冻（桩在独立 RWX 页，恢复无竞态）。
//   无论 1.30 睡觉走哪条内部路径，它最终必然写时钟——只要写，就会被当场
//   放行并解冻。这比"猜睡觉函数再 hook 它"健壮一个数量级。
//
// v1.3.3 → v1.3.4（2026-10-29，实测归因 + 入口硬化）：
//   症状：v1.3.2 上"冻结中按 J 睡觉 → 卡 Now Loading 90s → 只能强退"。
//   归因（日志 + 字节实证，非推测）：v1.3.2 用的两个 hook 地址是 1.20 旧值，
//   在 25640656 上**都不是函数入口**——
//     0x278A80 = c1 e8 08 …（某函数中段，shr eax,8 之后）
//     0x47B100 = 48 8b 4b 10 83 b9 80 03 00 00 02 …（中段，cmp [rcx+0x380],2）
//   qol::HookInstall 只校验可写性/可覆盖长度，**不校验语义**，于是往中段写了
//   5 字节 jmp → 该处后续指令被撕碎 → 睡觉流程必然走不动（卡加载，不是死机）。
//   真正的入口（1.30）已由 RTTI + 函数体双证重定位：
//     0x2812E0 GameTimeWaitUntil 实现，入口 48 89 5C 24 08 ✓
//     0x491620 Lua GameTimeSetTime，入口 48 89 5C 24 08 ✓
//   v1.3.3 已改地址但**从未实测**。v1.3.4 在改地址基础上补两件事：
//   ① VerifyHookEntry()：hook 前强制核对入口 5 字节序言，不匹配就**拒装**并打日志
//      （同类事故的通用防线：错位 = 宁可不动，也不能写坏游戏代码）——
//      代价是若某版序言变了，睡觉解冻会静默失效，但日志会明确写 entry mismatch；
//   ② [tf][probe] 限频探针：WaitUntil/SetTime 每次被调都记录 fmt/target/now，
//      用来在实测里确认"睡觉到底走哪条路"，避免再次无证据空转。
//   行为语义不变：只有"等待目标跨天 / 设时间后日期真的跨天"才算睡觉；
//   解冻后保持解冻，需要冻结时玩家自己在 F2 菜单重新开启。
//
// v1.3.2 → v1.3.3（2026-10-29，build 25640656 适配）：睡觉解冻三地址重定位。
//   时钟补丁点（A-store/B/C/G1/G2）逐字节核对在新 exe 全部 MATCH —— 函数内位置未动，
//   冻结本体无需改。但**睡觉解冻的三个地址已漂**：0x278A80 在新 exe 处是
//   `c1 e8 08 …`（某函数中段，根本不是入口）、0x47B100 / 0x1040D0 也都不再是对应函数。
//   若照旧值 HookInstall，等于往代码中间写 5 字节 jmp → 必毁游戏逻辑。
//   重定位依据见常量区注释（RTTI lambda 名 + 函数体逐句对应，双证非猜测）。
//   结论：v1.3.0~v1.3.2 在 1.30 上从未真正装上睡觉 hook（qol::HookInstall 只验字节
//   长度、不验语义），因此"冻结中睡觉"必须在游戏中重测。
//
// v1.2.19 → v1.3.0（2026-10-19）：睡觉解冻（用户拍板：解冻后不自动复冻，由玩家自行重新开启）
//   v1.3.1：明确取消“醒来 2s 自动复冻”——解冻即保持解冻；覆盖强制睡眠（跨天跳变同样判定）
//   症状：冻结中睡觉 → 游戏卡在黑屏（时间不推进，睡觉流程永久等待）。
//   根因链（Ghidra 反编译实锤）：
//     ① Lua 脚本用 GameTimeWaitUntil(GameTimeFormat)（0x47C000 → 实现 0x278A80）
//        阻塞协程等待时刻到达；时钟被冻结 → 条件永不满足 → 黑屏。
//     ② 另有 GameTimeSetTime（0x47B100）直接写 save+0x3270/0x3278，但本 MOD 的
//        100ms 写回会把跳天值拉回基准 → 同样让等待落空。
//   方案（用户要求：只有睡觉才解冻，其它时间等待/设时间一律不响应）：
//   ① GameTimeWaitUntil 目标跨到下一天（睡觉等 daybreak）→ 睡觉信号；
//   ② GameTimeSetTime 调用后 save+0x3270 的日期真的跨天 → 睡觉信号（含强制睡眠）；
//   信号只置位（不阻塞、不改游戏逻辑），worker 100ms 内解冻（恢复补丁 + 停写回）。
//   **解冻后保持解冻**：不自动复冻，需要冻结时由玩家自己在快捷菜单重新开启。
//   玩家手动开关优先（手动开启时会清掉残留的睡觉信号）。
//   hook 点（v1.20 build 25311578，入口均为 48 89 5C 24 08 = mov [rsp+8],rbx，
//   5 字节整指令，无 RIP-relative，hook 安全）：
//     - 0x278A80 GameTimeWaitUntil 实现（0x47C000 唯一调用者）
//     - 0x47B100 Lua GameTimeSetTime
//   GameAddDay(0x476E20) 的跳天由既有 dayIndex 检测兜底（同样只解除、不复冻）。
//
// v1.2.18 → v1.2.19（2026-10-17）：接入统一游戏版本闸门 exe_guard.h——village.exe 非 v1.20 build 25311578 时本 MOD 不启用（不再在未验证的 exe 上打补丁）。//
// v1.2.16 → v1.2.17（2026-10-17 玩家实测反馈修复，capstone 逆向实锤）
//   症状：冻结中保存 → 存档选择界面日期显示“第153349955年”（天文数字）。
//   根因（RVA 0x476E20 = 游戏函数 GameAddDay，字符串 "GameAddDay" 实锤）：
//     0x476EB4  mov  rax, 0x1845C8A0CE512957  ← 除以 86400 的定点魔数
//     0x476EBE  imul qword [r8+0x3270]        ← rdx:rax = rax*b（商落在 rdx）
//     0x476EC5  mov  [r8+0x3268], rsi
//     0x476ECC  sar rdx,0xD / 四舍五入 / imul rdx, rcx, 0x15180(=86400)
//     0x476EED  mov  [r8+0x3270], rdx         ← 天边界对齐值写回时钟
//   v1.2.9 起把 F_A 当“写点”整段 NOP（14B = imul + mov），**把 imul 也抹掉了 →
//   rdx 保留垃圾值 → G7 把“垃圾 × 86400”写进时钟 b**。v1.2.13/14 因 G5-G8
//   一并 NOP（G7 不执行）而未暴露；v1.2.16 放行 G7/G8 后立刻现形。
//   修复：F_A 只 NOP 存储指令（+7 起 7B = mov [r8+0x3268],rsi），**保留 imul**
//   （纯寄存器运算、无内存副作用；rdx 在 0x476EFB 被重载，保留无副作用）；
//   G7/G8 继续放行 → GameAddDay 语义恢复原生（睡觉/跳天路径不受影响）。
//   教训：补丁写点前必须确认该指令是否参与后续计算链——NOP 纯运算指令=灾难。
//
// v1.2.14 → v1.2.15（玩家反馈修复，2026-10-16）
//   问题：冻结中直接睡觉 → 有几率“原时间起床且作物减少一天”。
//   根因：v1.2.13 把跨天进位写点 G5/G6 也 NOP 了 → 睡觉时天数无法进位，
//   时钟被写回拉住基准 → 天数/时钟状态分裂，作物按天差扣产量。
//   修复（DeepSeek reasoner 交叉审查后定案）：
//   ① G5/G6 不再 NOP（跨天进位放行）——挂机冻结中 A/B/C 已封死、
//      b 不增长、G5/G6 进位条件永不触发，冻结保持不变；
//   ② 跳天检测改为 dayIndex 变化（floor(b/86400) != baseline 同值）
//      ——睡觉是“睡到次日早晨固定时刻”，b 增量通常仅 1~11 小时，
//      旧的 delta>=86400 阈值会漏检大部分睡觉；
//   ③ 检测到 dayIndex 变化 → 立即自动解冻（恢复全部补丁）
//      ——让游戏后续“设早晨时间/天数结算/作物结算”流程完整执行；
//   ④ 写回改为先读后写（同日漂移才拉回）——关闭竞态窗口：
//      若 G5/G6 在读/写之间写入新天值，写回不会覆盖它，下一 tick 必抓到。
// v1.1.1 → v1.1.2（用户反馈修复）：
//   ① toast 文案中文化（"时间冻结中"），菜单项同步改中文
//   ② 多屏修复：不再跟随前台窗口（alt-tab 后 HUD 跑到别的屏幕），
//      改为锚定游戏主窗口所在显示器（枚举本进程可见顶层非工具窗口）
//
// 移植自 TimeFreeze v0.1.2 by gloaming，1.08.1 → 1.09 重新适配：
//   - 开关接入 QoL 快捷菜单（ModManager F1），全天可用（自研设计，
//     与 gloaming v0.1.3 “仅夜间”不同——v1.1.0 已放开夜间限制，
//     本注释 v1.1.1 同步修正，消除文档与代码矛盾）
//   - 冻结时常驻 "TimeFreeze ON" toast（左中，v1.1.1 下移 56px，
//     避免与 AutoPet HUD 同位重叠）
//   - 只冻结主世界时钟，不影响角色、动画、物理、脚本等待
//
// v1.1.0 → v1.1.1（BUG 审查修复）：
//   ① save 对象更换/读链断裂后强制重采样——冻结中读档/换存档槽时，
//     旧档时间不再被写进新档（档损级污染修复）
//   ② TIME_B 读失败不再以 b=0 latch 基准（防冻结把时钟写 0）
//   ③ 日志宏发布版关闭（对齐套件惯例）
//
// 原理（不改任何游戏指令）：
//   [[exe+clock]+0x208]+0x3268（QWORD 累计秒）
//   [[exe+clock]+0x208]+0x3270（QWORD 时钟）
//   worker 每 100ms 读缓存；冻结时把两个字段写回上次缓存值。
//
// 1.09 适配：clock 锚点 RVA 0x19F003（mov rax,[rip+..] -> 0x10D4950，与 MineHelper 同源）

#include <windows.h>
#include <cstdint>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cwchar>

#include "logging.h"
#include "exe_guard.h"   // v-local: 统一游戏版本闸门
#include "game_window.h"   // HUD owner 绑定游戏窗口
#include "hot_config.h"
#include "quickmenu.h"
#include "patch_safety.h"
#include "budget.h"        // P0 帧耗时探针（跨 DLL 共享）
#include "hook.h"          // v1.3.0: x64 trampoline（睡觉自动解冻信号）
#include "obs.h"           // v1.6.x: 统一观测层（AI 可读寄存器 + toast）

// 日志开关：v1.2.17 临时开启（验证 F_A 存储补丁字节匹配 + day-jump 检测；
// 下一轮转正时重新注释）
// #define TIMEFREEZE_LOGGING  // 实测调试开启（诊断后回退）

// [diag] 诊断日志门控（默认关）；调试时 #define DIAG_TIMEFREEZE 1 开启
#ifndef DIAG_TIMEFREEZE
#define DIAG_TIMEFREEZE 0      // v1.2.14 转正：关诊断 watch（500ms 高频监视随宏失效，诊断能力保留源码）
#endif

// 日志运行时开关（2026-10-24）：编译期宏退役，改由 QoL_Shared 运行时门控。
// 开启方式：游戏根 qol_log.cfg 写 level=1（约 1s 生效，免重编免重启）；缺省/level=0 为静默。

using u64 = std::uint64_t;
using i64 = std::int64_t;

// ============================================================
// 常量（1.20 / build 25311578）
// ============================================================
static volatile uintptr_t RVA_CLOCK_LEA    = 0xE2EA3;
static volatile uintptr_t CLOCK_SAVE_0x208 = 0x208;
static volatile uintptr_t TIME_A_OFF       = 0x3268;   // 累计秒
static volatile uintptr_t TIME_B_OFF       = 0x3270;   // 时钟
// v1.2.9: 时钟推进写点（v1.20 逆向——游戏每帧写 save 时间字段，写回方案竞争必败）
//   F_A: 0x476EBE imul qword [r8+0x3270]; mov [r8+0x3268], rsi（r8=save，累计秒驱动）
//        ⚠ v1.2.17：此 imul 参与 GameAddDay 的天数计算链，**只能 NOP 存储指令**；
//        NOP 掉 imul 会让 rdx 变垃圾、被 G7 写回时钟（见文件头 v1.2.17 段）
//   F_B: 0x1562A5 mov [r13+0x3270], rax; mov [r13+0x3278], rax（时钟字段推进）
//   F_C: 0x14177B mov [rdi+0x3268/0x3270/0x3278], rsi（每帧时间同步入口，rsi=新时间）
static volatile uintptr_t RVA_CLOCK_FREEZE_A = 0x48D3DE;
static volatile uintptr_t RVA_CLOCK_FREEZE_B = 0x158E75;
static volatile uintptr_t RVA_CLOCK_FREEZE_C = 0x1442AB;
// v1.2.12: 时钟校准写点（0x2CECxx 校准函数——读外部时间源与 save 时钟比较后拉正写入，
// 切窗/失焦恢复时把真实时间差同步进游戏时钟 = “切窗就失效”真凶；反汇编实锤：
// G1 仅在 [save+0x3270] >= rcx 时拨回 rcx（上限钳制），G2 新算时间 r11 > 当前时写入）
//   G1: 0x2CEC1B mov [rax+0x3270],rcx; mov [rax+0x3278],rcx
//   G2: 0x2CEC8C mov [rbx+0x3270],r11; mov [rbx+0x3278],r11
// 注：同函数 0x2CEBF3 处有经辅助函数(call 0x103db0)写 [save+0x3268] 的路径——
// 0x3268 为派生字段，纯 NOP 化后 MOD 不再写回/校验它，该路径无需拦截。
// v1.2.13: 全拦截——G5/G6(跨天+1)/G7(天边界对齐)/G8(转换写回) 一并 NOP
// （frozen watch 实测：仅拦 A/B/C/G1/G2 时 b 仍持续 +194/+1310s/分钟，保留的
//  G5-G8 中至少一个在推进）。冻结期间跨天/睡觉/剧情时间推进全部封死（用户接受），
// 解冻恢复全部写点。反汇编分类见 .temp\disasm_g3_g9.txt
static volatile uintptr_t RVA_CLOCK_CALIB_G1 = 0x2D9DFB;   // v1.2.12: 校准写点 G1
static volatile uintptr_t RVA_CLOCK_CALIB_G2 = 0x2D9E6C;   // v1.2.12: 校准写点 G2
static volatile uintptr_t RVA_CLOCK_FREEZE_G5 = 0x46CF7A;  // v1.2.13: 跨天+1 推进
static volatile uintptr_t RVA_CLOCK_FREEZE_G6 = 0x46D216;  // v1.2.13: 跨天+1 推进
static volatile uintptr_t RVA_CLOCK_FREEZE_G7 = 0x48D40D;  // v1.2.13: GameAddDay(0x476E20) 天边界对齐写回（v1.2.16 起放行）
static volatile uintptr_t RVA_CLOCK_FREEZE_G8 = 0x491718;  // v1.2.13: 转换写回
static constexpr uintptr_t kFreezePatchLen = 14;
static constexpr uintptr_t kFreezePatchLenC = 21;
static constexpr int DISPLAY_OFFSET         = 7;        // hour = (b/3600 + 7) % 24

// v1.3.0: 睡觉自动解冻 hook 点（入口 48 89 5C 24 08 = mov [rsp+8],rbx，5 字节整指令）
// v1.3.3（build 25640656 适配）：三个地址全部按新 exe 重定位，旧值在 25640656 上
//   并不是这三个函数（旧 0x278A80 处新 exe 已是某函数中段 c1 e8 08…，硬装 hook 会毁代码）。
//   新值取证（Ghidra 双 exe 交叉，非猜测）：
//   ① 0x278A80→0x2812E0：函数体 410B 完全一致；lambda RTTI 名
//      `_Func_impl_no_alloc<lambda_1, ScriptCommand::GameTimeWaitUntil(GameTimeFormat)>`
//      （vftable 0xE58B88→TD 0x10AB3D0）实锤；同签名另两处命中经 RTTI 排除
//      （MsgButtonWait 0x6FAF10 / TalkEndWait 0x6FC170）。
//   ② 0x47B100→0x491620：342B 一致；体内自称 "GameTimeSetTime"、读
//      GAME_ROOT+0x208 取 save、调 TIME_TO_SEC 后写 save+0x3270/0x3278，与旧版逐句对应。
//   ③ 0x1040D0→0x106A90：旧函数 40B 头部签名在新 exe 唯一命中（含 hour-7 换算）。
static volatile uintptr_t RVA_WAIT_UNTIL_IMPL = 0x2812E0;  // GameTimeWaitUntil 实现
static volatile uintptr_t RVA_GAME_TIME_SET   = 0x491620;  // Lua GameTimeSetTime
static volatile uintptr_t RVA_TIME_TO_SEC     = 0x106A90;  // GameTimeFormat -> 原始秒（纯计算）

// ============================================================
// 运行时状态
// ============================================================
static uintptr_t g_exeBase = 0;
static volatile LONG g_running = 0;
static volatile LONG g_paused = 0;
// v1.3.6-diag: G7 机理诊断探针（2026-10-31 加入，**只读不写，零行为改变**）
//   反编译 FUN_14048d340(RVA 0x48D340) 实证该函数体为：
//     *(save+0x3268) = 0;                                  // A 站点 (0x48D3DE) 被 NOP
//     lVar = (*(save+0x3270) / 86400 + 1) * 86400;         // 算下一个整天边界
//     *(save+0x3270) = lVar; *(save+0x3278) = lVar;         // G7 (0x48D40D) 放行
//   即：**该函数每被调用一次，时钟就被推到下一个整天边界**，A 与 G7 无条件顺序执行。
//   所以「AddDay 被调用 N 次」== 「时钟被推 N 天」。冻结期只要 N>0，时钟必被偷写。
//   三个计数器经 obs 每秒发布，AI/人可直接读，用于判定冻结期是否真的"冻住"。
static volatile LONG g_diagAddDayCalls  = 0;   // AddDay 累计调用次数（A 站点进入即计）
static volatile LONG g_diagAddDayFrozen = 0;   // 其中处于冻结状态的次数（>0 即 G7 偷写）
static volatile LONG g_diagAddDayJump   = 0;   // 最近一次 G7 会推掉的秒数（≈86400 = 跳整天）

// v1.3.2: 线程句柄——DETACH 时 join，避免 WndProc/上下文悬空引用已卸载 DLL
static HANDLE g_hToast = nullptr;
static HANDLE g_hWorker = nullptr;
// v1.2.9: 时钟推进点冻结状态 + 备份原始字节
static volatile LONG g_clockFrozen = 0;
static unsigned char g_origFreezeA[kFreezePatchLen];
static unsigned char g_origFreezeB[kFreezePatchLen];
static unsigned char g_origFreezeC[kFreezePatchLenC];
static unsigned char g_origCalibG1[kFreezePatchLen];   // v1.2.12: 校准写点 G1 原始字节
static unsigned char g_origCalibG2[kFreezePatchLen];   // v1.2.12: 校准写点 G2 原始字节
static unsigned char g_origFreezeG5[kFreezePatchLen];   // v1.2.13: G5 跨天+1 原始字节
static unsigned char g_origFreezeG6[kFreezePatchLen];   // v1.2.13: G6 跨天+1 原始字节
static unsigned char g_origFreezeG7[kFreezePatchLen];   // v1.2.13: G7 天边界对齐原始字节
static unsigned char g_origFreezeG8[kFreezePatchLen];   // v1.2.13: G8 转换写回原始字节

// v1.2.14: 写回拉回（冻结兜底）——9 写点 NOP 后仍可能残留低频旁路写入
// （用户实测切窗后时间仍走；watch 采样 60s 太稀无法确认 b 是否被推）。
// 每 100ms 把 0x3270/0x3278 写回冻结基准（无校验无禁用），旁路写在下一
// tick 被拉回。基准在开启时 latch、save 换档时重采样。
static u64  g_freezeBaseline = 0;       // 冻结基准（b 值）
static void* g_freezeSavePtr = nullptr; // 基准对应的 save 对象
// v1.2.18: 0x3278 独立基准（镜像字段拉回，不再被 0x3270 的值覆盖）
static u64  g_freezeBaseline2 = 0;
static void* g_freezeSavePtr2 = nullptr;

// v1.3.0: 睡觉解冻（一次性，不自动复冻）
//   游戏线程（hook）只置信号，.text 补丁恢复统一由 worker 线程做（避免跨线程写代码段）。
//   玩家要求：睡觉解冻后不自动恢复冻结，何时重新冻结由玩家自己决定。
static volatile LONG g_sleepSignal = 0;   // 1 = 检测到睡觉（游戏线程置位）

// 2026-11-07 选 C：睡觉语义保持不变（一次性解冻、不自动复冻、worker 继续跑），
// 只把「到底哪条路径触发了」变成可机读判据。历史日志里零条 freeze released，
// 无法区分「没睡觉」与「触发链断了」，加计数后一次实机即可定性。
// 独立命名空间 tfsleep（timefreeze 自身 8 个 int 槽已满，绝不扩 OBS_KEYS）。
enum { TF_S_WAIT = 0, TF_S_TIMESET, TF_S_GATE, TF_S_DAYJUMP, TF_S_RELEASED };
static volatile LONG g_tfSleepCounters[5] = {};
static const char* const kTfSleepKeys[5] = {
    "waitHit", "timeSetHit", "gatePass", "dayJump", "released"
};
// 2026-11-07 23:25 新增「解冻触发源」判据：实机反馈「还是起床前才取消冻结」，
//   旧判据只看到 released=1，分不清是 waitImpl(躺下) / timeSet / gate /
//   dayJump 哪一路触发。语义一字未改（仍一次性解冻、不自动复冻）。
static volatile LONG g_tfLastSrc = 0;   // 1..4 对应 TF_S_WAIT..TF_S_DAYJUMP
static inline void TfSleepMark(int which) {
    if (which < 0 || which >= 5) return;
    if (which != TF_S_RELEASED)
        InterlockedExchange(&g_tfLastSrc, which + 1);
    qol::obs::SetInt("tfsleep", kTfSleepKeys[which],
                     (int)InterlockedIncrement(&g_tfSleepCounters[which]));
    qol::obs::SetInt("tfsleep", "releaseSrc", (int)g_tfLastSrc);
}

// v1.3.2【P1 硬化】写回（SEH + 值合法性版）
//   背景：v1.2.14 的 WriteBack 是裸调用，存在两类死档/崩溃风险——
//   ① `qol::WritePatchChecked` 内部**没有 SEH**。目标页若在 MOD 与游戏之间
//      被释放或改保护，memcpy 访问违例会沿 worker 线程直接把进程带崩；
//   ② 会把"采样失败得到的 0"写进时钟字段（save+0x3270 / 0x3278）。时钟是
//      派生字段——跨天判定、作物生长、机器 start/duration 全依赖它，写成 0
//      等于把整档的时间基准打没，是最坏形态的存档损坏。
//   现在：空指针/零值一律不写；写入全程 SEH，失败只丢弃本轮（100ms 后再来）。
//   注意（防误判）：v1.2.12 那条"删除后不再读写任何存档内存、死档风险归零"
//   的注释已**与代码不符**——本 MOD 至今仍是套件内唯一会直写存档时钟字段
//   的 MOD，风险品类为"时钟回拉导致依赖时间的存档状态错乱"，不得按零风险
//   处理。跨线程写入是既有架构（.text 补丁恢复同样由 worker 做，见上方
//   注释），本次不改动线程归属，仅做写入加固。
static void WriteBack(void* save, uintptr_t off, u64 v) {
    if (!save || v == 0) return;
    __try {
        qol::WritePatchChecked(
            reinterpret_cast<unsigned char*>(save) + off, &v, sizeof(v));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // 写入失败：丢弃本轮回拉，不重试。
    }
}

// v1.2.12: 写回层（WriteTime + D4 防护三件套 g_lastSavePtr/g_lastSaveVtable/g_writeDisabled
// + 失效 toast + 回读校验）整体移除——纯 NOP 化。写回竞争必败（游戏每帧写 vs MOD
// 100ms 写回），切窗校准拉正使回读漂移 90823s -> 永久禁用 = “切窗就失效”误报根源。
// 删除后 TimeFreeze 不再读/写任何存档内存（套件内唯一直写存档 MOD 的死档风险品类归零）。

static HWND g_toastHwnd = nullptr;
static wchar_t g_toastText[32] = {0};

// v1.2.4: toast 尺寸常量（失效提示需更长文本，200→300 加宽）
static const int TOAST_W = 300;
static const int TOAST_H = 40;
// 前置声明：WriteTime 失效提示需要从 worker 线程直接显示 toast
static void ShowToastHold(const wchar_t* text);

// ============================================================
// 基础工具
// ============================================================
static bool ReadPtr(void* p, void** out) {
    if (!p) return false;
    uintptr_t v = 0;
    __try {
        v = *reinterpret_cast<uintptr_t*>(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (v < 0x10000) return false;
    *out = reinterpret_cast<void*>(v);
    return true;
}

// 获取 save 对象（clock → +0x208 解引用）
static void* GetSave() {
    void* base = nullptr;
    const int32_t* disp = reinterpret_cast<const int32_t*>(
        g_exeBase + RVA_CLOCK_LEA + 3);
    uintptr_t addr = g_exeBase + RVA_CLOCK_LEA + 7 + *disp;
    if (!ReadPtr(reinterpret_cast<void*>(addr), &base)) return nullptr;
    void* save = nullptr;
    if (!ReadPtr(reinterpret_cast<void*>(
            reinterpret_cast<unsigned char*>(base) + CLOCK_SAVE_0x208), &save))
        return nullptr;
    return save;
}

static bool ReadTime(void* save, uintptr_t off, u64* out) {
    if (!save || !out) return false;
    __try {
        *out = *reinterpret_cast<u64*>(
            reinterpret_cast<unsigned char*>(save) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// ============================================================
// v1.3.0: 睡觉解冻 —— 游戏线程信号（hook 只置位，不写代码段）
// ============================================================
static void RequestSleepUnfreeze() {
    if (!g_paused) return;                     // 未冻结：无需处理
    InterlockedExchange(&g_sleepSignal, 1);
}

typedef void (__fastcall *TFnWaitImpl)(int* fmt);
typedef int  (__fastcall *TFnTimeSet)(void* luaState);
typedef long long (__fastcall *TFnTimeToSec)(int* fmt);
static TFnWaitImpl g_origWaitImpl = nullptr;
static TFnTimeSet  g_origTimeSet  = nullptr;

// v1.3.4: 探针计数器（限频日志，最多各 20 条）
static volatile LONG g_waitProbe = 0;
static volatile LONG g_setProbe  = 0;

// v1.3.4: GameTimeFormat → 累计秒（含 __try 的独立函数，避免 detour 主体触发 C2712）
static bool CalcTargetSec(int* fmt, u64* out) {
    if (!fmt || !out || !g_exeBase) return false;
    __try {
        TFnTimeToSec toSec =
            reinterpret_cast<TFnTimeToSec>(g_exeBase + RVA_TIME_TO_SEC);
        *out = static_cast<u64>(toSec(fmt));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// v1.3.4: WaitUntil 被调记录——实测用来确认"睡觉是否真的走这条路"。
// 之前只有"判定成功才打日志"，导致"detour 从未被调用"和"被调用但条件不成立"
// 两种情况在日志里长得一样，白白空转一轮。
static void ProbeWaitLog(int* fmt, bool frozen) {
    LONG n = InterlockedIncrement(&g_waitProbe);
    if (n > 20) return;
    u64 target = 0, b = 0;
    CalcTargetSec(fmt, &target);
    void* save = GetSave();
    if (save) ReadTime(save, TIME_B_OFF, &b);
    Log("[tf][probe] waitImpl#%ld frozen=%d fmt=%p target=%llu now=%llu day %llu->%llu\n",
        n, frozen ? 1 : 0, fmt, (unsigned long long)target,
        (unsigned long long)b, (unsigned long long)(b / 86400),
        (unsigned long long)(target / 86400));
}

// GameTimeWaitUntil 实现（0x2812E0）：rcx = GameTimeFormat（36B：[0]秒 [1]分 [2]时
// [3]12时制 [4]当日 [5]上下午 [6]星期 [7]季 [8]年）
// v1.3.0：只有“等待目标跨到下一天”（睡觉 = 等到次日 daybreak）才算睡觉，
// 普通短时间等待（剧情/交互计时）一律不动冻结状态。
static void __fastcall Detour_WaitImpl(int* fmt) {
    if (fmt && g_exeBase) ProbeWaitLog(fmt, g_paused != 0);
    if (g_paused && fmt && g_exeBase) {
        __try {
            TFnTimeToSec toSec =
                reinterpret_cast<TFnTimeToSec>(g_exeBase + RVA_TIME_TO_SEC);
            u64 target = static_cast<u64>(toSec(fmt));
            void* save = GetSave();
            u64 b = 0;
            if (save && ReadTime(save, TIME_B_OFF, &b) && target > b &&
                (target / 86400) > (b / 86400)) {
                Log("[tf] sleep detected (wait next day): now=%llu target=%llu -> release freeze\n",
                    (unsigned long long)b, (unsigned long long)target);
                TfSleepMark(TF_S_WAIT);
                RequestSleepUnfreeze();
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[tf] wait-until probe exception (ignored)\n");
        }
    }
    if (g_origWaitImpl) g_origWaitImpl(fmt);
}

// Lua GameTimeSetTime（0x491620）：脚本直接设时间。
// v1.3.0：调用后比对 save+0x3270——只有“日期跨天”才判定为睡觉（其它设时间不动）。
static int __fastcall Detour_TimeSet(void* luaState) {
    bool frozen = (g_paused != 0);
    void* save = frozen ? GetSave() : nullptr;
    u64 before = 0;
    if (save) ReadTime(save, TIME_B_OFF, &before);
    if (!g_origTimeSet) return 0;
    int r = g_origTimeSet(luaState);
    u64 after = 0;
    bool haveAfter = (save && ReadTime(save, TIME_B_OFF, &after));
    // v1.3.4 探针：无论是否冻结都记录前 20 次（含 before/after，判"是否真跨天"）
    LONG n = InterlockedIncrement(&g_setProbe);
    if (n <= 20) {
        Log("[tf][probe] timeSet#%ld frozen=%d before=%llu after=%llu day %llu->%llu\n",
            n, frozen ? 1 : 0, (unsigned long long)before,
            (unsigned long long)after,
            (unsigned long long)(before / 86400),
            (unsigned long long)(after / 86400));
    }
    if (frozen && save && before != 0 && haveAfter &&
        after / 86400 > before / 86400) {
        Log("[tf] sleep detected (GameTimeSetTime day-jump): %llu -> %llu -> release freeze\n",
            (unsigned long long)before, (unsigned long long)after);
        TfSleepMark(TF_S_TIMESET);
        RequestSleepUnfreeze();
    }
    return r;
}

// v1.3.4：hook 前强制核对入口序言（48 89 5C 24 08 = mov [rsp+8],rbx）。
// 背景升级事故：v1.3.2 用了 1.20 的旧 RVA，在 1.30 上它们是函数中段，
// qol::HookInstall 只验长度不验语义 → 往中段写 jmp → 睡觉流程卡死。
// 原则：**宁可这个功能静默失效，也不能写坏游戏代码**；失败必留 entry mismatch 日志。
static bool VerifyHookEntry(uintptr_t rva, const char* tag) {
    static const unsigned char kPrologue[5] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
    if (!g_exeBase) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(g_exeBase + rva);
    if (memcmp(p, kPrologue, 5) != 0) {
        Log("[tf] %s entry mismatch @0x%X (%02X %02X %02X %02X %02X) -> hook SKIPPED\n",
            tag, (unsigned)rva, p[0], p[1], p[2], p[3], p[4]);
        return false;
    }
    return true;
}

static void InstallSleepHooks() {
    if (!g_exeBase) return;
    g_origWaitImpl = nullptr;
    g_origTimeSet  = nullptr;
    bool ok1 = false, ok2 = false;
    if (VerifyHookEntry(RVA_WAIT_UNTIL_IMPL, "waitImpl")) {
        ok1 = qol::HookInstall(g_exeBase + RVA_WAIT_UNTIL_IMPL,
                               reinterpret_cast<uintptr_t>(&Detour_WaitImpl),
                               reinterpret_cast<uintptr_t*>(&g_origWaitImpl));
        if (!ok1) g_origWaitImpl = nullptr;
    }
    if (VerifyHookEntry(RVA_GAME_TIME_SET, "timeSet")) {
        ok2 = qol::HookInstall(g_exeBase + RVA_GAME_TIME_SET,
                               reinterpret_cast<uintptr_t>(&Detour_TimeSet),
                               reinterpret_cast<uintptr_t*>(&g_origTimeSet));
        if (!ok2) g_origTimeSet = nullptr;
    }
    Log("[tf] sleep hooks: waitImpl=%d timeSet=%d\n", ok1 ? 1 : 0, ok2 ? 1 : 0);
}

#if DIAG_TIMEFREEZE
// v1.2.12-diag: 冻结期间时钟监视（60s 限频）——实测验证 G1/G2 拦截后
// 切窗不再拉正时间（b 应保持不动）。取代 v1.2.8-diag 的 DiagProbePtr
// （0x3270 已确认为时钟字段非指针，探测完成使命）。
// 独立函数（含 __try 的代码不能放在带 C++ 对象展开的函数里，C2712）
static void DiagFrozenWatch(u64 b, u64 a) {
    Log("[tf][diag] frozen watch: b=%llu a=%llu\n",
        (unsigned long long)b, (unsigned long long)a);
}
#endif
// ============================================================
// v1.2.9: 时钟推进点冻结/恢复（NOP 两处写点，游戏不再覆盖时间字段）
// ============================================================
// v1.3.5: 时钟写点条件放行桩（核心重构，背景见文件头 v1.3.5 段）
// ============================================================
// 站点描述：覆盖 [rva, rva+len) 的 mov [base+disp], src 指令组。
//   len 必须是完整指令边界（stub jmp 只占前 5 字节，其余字节不被执行但保持原值）。
struct TfSiteDesc {
    uintptr_t   rva;
    int         len;
    int         baseReg;    // x64 寄存器编号（0=rax..7=rdi, 8=r8..15=r15）
    int         srcReg;
    uint32_t    disp[3];
    int         nDisp;
    unsigned char expect[21];   // 原始字节（安装前校验）
    unsigned char backup[21];   // 首次安装时备份（恢复用）
    const char* tag;
    bool        installed;
};

// 寄存器 → 快照栈偏移（stub push 顺序：pushfq,rax,rcx,rdx,rbx,rbp,rsi,rdi,r8..r15）
static constexpr int kRegSnapOff[16] = {
    112, 104, 96, 88, -1, 80, 72, 64,   // rax rcx rdx rbx rsp rbp rsi rdi
    56, 48, 40, 32, 24, 16, 8, 0        // r8 r9 r10 r11 r12 r13 r14 r15
};

// 5 个冻结站点（v1.2.12~17 的 A-store/B/C/G1/G2；expect 字节在 Apply 时填入）
static TfSiteDesc g_sites[5] = {
    { 0,  7,  8,  6, { 0x3268 },                 1, {0}, {0}, "A-store", false },
    { 0, 14, 13, 0, { 0x3270, 0x3278 },          2, {0}, {0}, "B",       false },
    { 0, 21,  7, 6, { 0x3268, 0x3270, 0x3278 },  3, {0}, {0}, "C",       false },
    { 0, 14,  0, 1, { 0x3270, 0x3278 },          2, {0}, {0}, "G1",      false },
    { 0, 14,  3, 11, { 0x3270, 0x3278 },         2, {0}, {0}, "G2",      false },
};

// 大跳变阈值（秒）：src-cur 达到此值即视为游戏主动推进（睡觉快进/剧情/设时刻）。
// 自然流逝每帧增量 <= 数秒，300s 有 50 倍以上裕度。热配置可调。
static volatile DWORD g_passMinJump = 300;   // [hot] TF_PASS_MINJUMP (HOT_INT, 秒)

// 判定 + 放行（游戏线程，stub 内 call 进入）。
//   语义：src 是游戏想写进时钟字段的新值；base 是 save 对象指针。
//   返回 1 = 已放行（当场写内存 + 置解冻信号），stub 直接跳回站点尾；
//         0 = 拦截（什么都不写，等效原 NOP），stub 跳回站点尾。
static int __fastcall TfSiteGate(void* snap, int idx) {
    if (idx < 0 || idx >= 5) return 0;
    const TfSiteDesc* d = &g_sites[idx];
    const int bo = kRegSnapOff[d->baseReg], so = kRegSnapOff[d->srcReg];
    if (bo < 0 || so < 0) return 0;
    const uintptr_t base = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<char*>(snap) + bo);
    const u64 src = *reinterpret_cast<u64*>(
        reinterpret_cast<char*>(snap) + so);
    if (base < 0x10000) return 0;

    bool pass = false;
    u64 cur = 0;
    __try {
        cur = *reinterpret_cast<u64*>(base + TIME_B_OFF);
        if (cur != 0 && src != 0) {
            const bool crossDay = (src / 86400) > (cur / 86400);
            const bool bigJump  = (src > cur) &&
                                  (src - cur >= static_cast<u64>(g_passMinJump));
            if (crossDay || bigJump) {
                for (int i = 0; i < d->nDisp; ++i)
                    *reinterpret_cast<u64*>(base + d->disp[i]) = src;
                pass = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        pass = false;   // 读写异常：保守拦截，本轮丢弃
    }

    // v1.3.6-diag: G7 机理探针（只处理 idx==0 即 A 站点；只读 cur，不写游戏内存）
    if (idx == 0 && cur != 0) {
        static volatile LONG s_calls = 0, s_fz = 0;
        const LONG n = InterlockedIncrement(&s_calls);
        if (n > g_diagAddDayCalls) g_diagAddDayCalls = n;
        if (g_paused) {
            const LONG f = InterlockedIncrement(&s_fz);
            if (f > g_diagAddDayFrozen) g_diagAddDayFrozen = f;
        }
        const u64 would = (cur / 86400 + 1) * 86400;   // G7 紧接着会写入的值
        g_diagAddDayJump = static_cast<LONG>(would > cur ? (would - cur) : 0);
    }

    if (pass) {
        static volatile LONG s_gateCnt = 0;
        const LONG n = InterlockedIncrement(&s_gateCnt);
        if (n <= 12) {
            Log("[tf][gate] %s PASS src=%llu cur=%llu -> write-through + unfreeze\n",
                d->tag, (unsigned long long)src, (unsigned long long)cur);
        }
        TfSleepMark(TF_S_GATE);
        RequestSleepUnfreeze();
    }
    return pass ? 1 : 0;
}

static void* g_stubBase = nullptr;   // RWX 页：5 个站点桩（各 74B，0x60 间隔）

// 生成站点桩并写 5 字节 jmp 覆盖站点头。
//   桩体：pushfq + 15 push 保存全寄存器 → rcx=快照, edx=站点号 → call TfSiteGate
//         → 恢复全寄存器+flags → jmp 站点尾（原指令要么被 gate 代写、要么被拦，
//           两种情况都不再执行原指令）。
static bool InstallOneStub(int idx) {
    TfSiteDesc* d = &g_sites[idx];
    unsigned char* p = reinterpret_cast<unsigned char*>(g_exeBase + d->rva);
    if (d->installed || p[0] == 0xE9) { d->installed = true; return true; } // 幂等
    if (memcmp(p, d->expect, static_cast<size_t>(d->len)) != 0) {
        Log("[tf] stub %s sig mismatch @0x%X (%02X %02X %02X) -> skip\n",
            d->tag, (unsigned)d->rva, p[0], p[1], p[2]);
        return false;
    }
    memcpy(d->backup, p, static_cast<size_t>(d->len));

    unsigned char* s = reinterpret_cast<unsigned char*>(g_stubBase) + idx * 0x60;
    unsigned char* w = s;
    *w++ = 0x9C;                                    // pushfq
    { static const unsigned char kPush1[7] = {0x50,0x51,0x52,0x53,0x55,0x56,0x57};
      for (int i = 0; i < 7; ++i) *w++ = kPush1[i]; }     // push rax..rdi
    for (int r = 8; r < 16; ++r) { *w++ = 0x41; *w++ = static_cast<unsigned char>(0x50 + (r - 8)); }
    *w++ = 0x48; *w++ = 0x89; *w++ = 0xE1;          // mov rcx, rsp
    *w++ = 0xBA;                                    // mov edx, idx
    *reinterpret_cast<int32_t*>(w) = idx; w += 4;
    *w++ = 0x48; *w++ = 0x83; *w++ = 0xEC; *w++ = 0x28;   // sub rsp, 0x28
    *w++ = 0xE8;                                    // call TfSiteGate
    *reinterpret_cast<int32_t*>(w) = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&TfSiteGate) - reinterpret_cast<uintptr_t>(w + 4));
    w += 4;
    *w++ = 0x48; *w++ = 0x83; *w++ = 0xC4; *w++ = 0x28;   // add rsp, 0x28
    for (int r = 15; r >= 8; --r) { *w++ = 0x41; *w++ = static_cast<unsigned char>(0x58 + (r - 8)); }
    { static const unsigned char kPop1[7] = {0x5F,0x5E,0x5D,0x5B,0x5A,0x59,0x58};
      for (int i = 0; i < 7; ++i) *w++ = kPop1[i]; }      // pop rdi..rax
    *w++ = 0x9D;                                    // popfq
    *w++ = 0xE9;                                    // jmp 站点尾
    *reinterpret_cast<int32_t*>(w) = static_cast<int32_t>(
        (g_exeBase + d->rva + d->len) - (reinterpret_cast<uintptr_t>(w) + 4));
    w += 4;

    // 覆盖站点头 5 字节 jmp（stub 内已代劳原指令语义，其余字节保持原值）
    DWORD oldProt = 0;
    if (!VirtualProtect(p, static_cast<size_t>(d->len), PAGE_EXECUTE_READWRITE, &oldProt))
        return false;
    p[0] = 0xE9;
    *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(d->len - 5);
    VirtualProtect(p, static_cast<size_t>(d->len), oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), p, static_cast<size_t>(d->len));
    d->installed = true;
    return true;
}

static bool InstallConditionalStubs() {
    if (!g_exeBase) return false;
    if (!g_stubBase) {
        g_stubBase = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_EXECUTE_READWRITE);
        if (!g_stubBase) {
            Log("[tf] stub VirtualAlloc failed\n");
            return false;
        }
    }
    // 每次安装前刷新站点 RVA（热配置改 RVA 后重新开关可生效；已装桩不迁移）
    g_sites[0].rva = RVA_CLOCK_FREEZE_A + 7;
    g_sites[1].rva = RVA_CLOCK_FREEZE_B;
    g_sites[2].rva = RVA_CLOCK_FREEZE_C;
    g_sites[3].rva = RVA_CLOCK_CALIB_G1;
    g_sites[4].rva = RVA_CLOCK_CALIB_G2;
    bool ok = true;
    for (int i = 0; i < 5; ++i) ok &= InstallOneStub(i);
    return ok;
}

// ============================================================
// v1.2.12: 统一补丁点处理——首次 patch 前【备份原始字节】。
// 修复严重潜伏 bug：v1.2.9~11 从未备份 g_origFreezeA/B/C，RestoreClockFreezePatches
// 一直在用 BSS 全零覆写游戏 .text（解冻即破坏代码；未爆发只因“失效”后
// 补丁保持 NOP、解冻路径从未被实测——同 MineHelper v1.0.8 修复过的同型 bug）。
// 已 NOP 视为幂等通过（重入不重复备份，保留首次备份）。
static bool PatchSite(uintptr_t rva, const unsigned char* expect,
                      unsigned char* backup, size_t len, const unsigned char* nop,
                      const char* tag) {
    if (!g_exeBase) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(g_exeBase + rva);
    if (memcmp(p, expect, len) == 0) {
        memcpy(backup, p, len);   // 首次：备份原始字节（Restore 凭此恢复）
        if (!qol::WritePatchChecked(
                reinterpret_cast<void*>(g_exeBase + rva), nop, len)) {
            Log("[tf] %s write failed @0x%X\n", tag, (unsigned)rva);
            return false;
        }
        return true;
    }
    for (size_t i = 0; i < len; ++i)
        if (p[i] != 0x90) {
            Log("[tf] %s sig mismatch @0x%X\n", tag, (unsigned)rva);
            return false;
        }
    return true;  // 已 NOP（重入幂等，不覆盖首次备份）
}

static void ApplyClockFreezePatches() {
    if (g_clockFrozen || !g_exeBase) return;
    // v1.3.5: 期望字节初始化（原 v1.2.12~17 的 kExpect* 挪入站点表）。
    //   A-store 仍只封存储指令（+7 起 7B），imul 保留（v1.2.17 教训）；
    //   G5/G6/G7/G8 保持原生（跨天/对齐/转换放行，v1.2.16 结论），
    //   现在连 A/B/C/G1/G2 也会在"游戏主动推进"时放行（条件放行桩）。
    {
        static const unsigned char kExpectA[7] = {
            0x49,0x89,0xB0,0x68,0x32,0x00,0x00};           // mov [r8+0x3268], rsi
        static const unsigned char kExpectB[14] = {
            0x49,0x89,0x85,0x70,0x32,0x00,0x00,
            0x49,0x89,0x85,0x78,0x32,0x00,0x00};
        static const unsigned char kExpectC[21] = {
            0x48,0x89,0xB7,0x68,0x32,0x00,0x00,
            0x48,0x89,0xB7,0x70,0x32,0x00,0x00,
            0x48,0x89,0xB7,0x78,0x32,0x00,0x00};
        static const unsigned char kExpectG1[14] = {
            0x48,0x89,0x88,0x70,0x32,0x00,0x00,
            0x48,0x89,0x88,0x78,0x32,0x00,0x00};
        static const unsigned char kExpectG2[14] = {
            0x4C,0x89,0x9B,0x70,0x32,0x00,0x00,
            0x4C,0x89,0x9B,0x78,0x32,0x00,0x00};
        memcpy(g_sites[0].expect, kExpectA,  7);
        memcpy(g_sites[1].expect, kExpectB, 14);
        memcpy(g_sites[2].expect, kExpectC, 21);
        memcpy(g_sites[3].expect, kExpectG1, 14);
        memcpy(g_sites[4].expect, kExpectG2, 14);
    }
    const bool ok = InstallConditionalStubs();
    g_clockFrozen = 1;
    if (ok) {
        Log("[tf] conditional freeze stubs applied (A-store B C G1 G2; pass on day-jump/jump>=%lus)\n",
            (unsigned long)g_passMinJump);
    } else {
        Log("[tf] conditional freeze stub PARTIAL (see mismatch above)\n");
    }
}

static void RestoreClockFreezePatches() {
    if (!g_clockFrozen || !g_exeBase) return;
    for (int i = 0; i < 5; ++i) {
        TfSiteDesc* d = &g_sites[i];
        if (!d->installed) continue;
        qol::WritePatchChecked(reinterpret_cast<void*>(g_exeBase + d->rva),
                               d->backup, static_cast<size_t>(d->len));
        d->installed = false;
    }
    g_clockFrozen = 0;
    Log("[tf] conditional freeze stubs restored\n");
}

// v1.2.12: WriteTime 及 D4 写入防护层整体删除（纯 NOP 化）——冻结只通过
// .text 补丁拦截游戏全部时钟写入路径（推进 A/B/C + 切窗校准 G1/G2），
// MOD 不再读写任何存档数据字段；原“写回+回读校验”正是切窗失效误报
// 的根源（游戏校准拉正后回读漂移 90823s → 永久禁用）。

// ============================================================
// toast 提示窗（左中，风格参照 AutoPet HUD）
// ============================================================
// ============================================================
// 游戏主窗口定位（v1.1.2：多屏锚定）
// 旧实现 MonitorFromWindow(GetForegroundWindow())：alt-tab 后前台
// 变成别的程序，toast 跳到其他屏幕。改为枚举本进程可见顶层非
// 工具窗口（游戏主窗口），锚定其所在显示器；找不到再回退前台。
// ============================================================
struct FindGameWindowCtx {
    DWORD pid;
    HWND found;
};

static BOOL CALLBACK FindGameWindowEnumProc(HWND hwnd, LPARAM lParam) {
    FindGameWindowCtx* ctx = reinterpret_cast<FindGameWindowCtx*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) return TRUE;          // 非本进程
    if (!IsWindowVisible(hwnd)) return TRUE;   // 不可见
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;  // 有属主
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;  // 我们自己的 HUD/toast
    ctx->found = hwnd;
    return FALSE;  // 找到即停
}

static HWND FindGameMainWindow() {
    FindGameWindowCtx ctx = { GetCurrentProcessId(), nullptr };
    EnumWindows(FindGameWindowEnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// 计算 toast 位置：**锚定游戏窗口客户区**（左中，下移 56px）
// v1.3.9（2026-11-01）【定位修复·KB-113】原实现用「游戏所在显示器的工作区」定位，
//   窗口化游玩时游戏窗口不在 (0,0)（本机实测 rect=[150,10,1444,768]），
//   toast 就被画到游戏窗口外面的桌面上 —— 玩家看不到，日志却一切正常。
//   现优先贴游戏客户区；拿不到客户区才退回工作区。
static bool ComputeToastPosition(int width, int height, int* x, int* y) {
    HWND game = FindGameMainWindow();
    if (game) {
        RECT gr = {};
        POINT gpt = { 0, 0 };
        if (GetClientRect(game, &gr) && ClientToScreen(game, &gpt)) {
            const int cw = gr.right - gr.left;
            const int ch = gr.bottom - gr.top;
            if (cw > width + 16 && ch > height + 16) {
                *x = gpt.x + 24;
                *y = gpt.y + (ch - height) / 2 + 56;
                if (*y + height > gpt.y + ch - 8)
                    *y = gpt.y + ch - height - 8;
                if (*y < gpt.y + 8) *y = gpt.y + 8;
                return true;
            }
        }
    }
    RECT wa = {};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0))
        return false;
    *x = wa.left + 24;
    *y = wa.top + (wa.bottom - wa.top - height) / 2 + 56;
    return true;
}

// v1.3.x【HUD 抗闪烁】离屏 DIB 双缓冲：渲染到内存 DC 后用 UpdateLayeredWindow 一次性提交，
// 取代 BeginPaint 直绘，消除分层窗口 GDI 重绘闪烁（与 ModManager v1.2.24 同方案）。
static HDC     g_tfMemDC  = nullptr;
static HBITMAP g_tfMemBmp = nullptr;
static HBITMAP g_tfOldBmp = nullptr;  // 内存 DC 初始 1x1 位图，删 DIB 前换回
static int     g_tfBufW   = 0, g_tfBufH = 0;
static void TfEnsureBuffer(int w, int h) {
    if (g_tfMemDC && g_tfBufW == w && g_tfBufH == h) return;
    if (g_tfMemBmp) {
        if (g_tfOldBmp) { SelectObject(g_tfMemDC, g_tfOldBmp); g_tfOldBmp = nullptr; }
        DeleteObject(g_tfMemBmp); g_tfMemBmp = nullptr;
    }
    if (!g_tfMemDC) {
        g_tfMemDC = CreateCompatibleDC(nullptr);
        g_tfOldBmp = (HBITMAP)GetCurrentObject(g_tfMemDC, OBJ_BITMAP);
    }
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;   // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    g_tfMemBmp = CreateDIBSection(g_tfMemDC, &bmi, DIB_RGB_COLORS, nullptr, nullptr, 0);
    if (g_tfMemBmp) {
        SelectObject(g_tfMemDC, g_tfMemBmp);
        g_tfBufW = w; g_tfBufH = h;
    }
}
static void TfFreeBuffer() {
    if (g_tfMemBmp) {
        if (g_tfOldBmp) { SelectObject(g_tfMemDC, g_tfOldBmp); g_tfOldBmp = nullptr; }
        DeleteObject(g_tfMemBmp); g_tfMemBmp = nullptr;
    }
    if (g_tfMemDC)  { DeleteDC(g_tfMemDC); g_tfMemDC = nullptr; }
    g_tfBufW = g_tfBufH = 0;
}
// 把整张 toast 绘制到给定 DC（原 WM_PAINT 绘制体，参数化 w/h 复用离屏缓冲）
static void TfRender(HDC hdc, int w, int h) {
    RECT rc = { 0, 0, w, h };
    // 深色背景
    HBRUSH bg = CreateSolidBrush(RGB(28, 30, 34));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    // 左侧色条（红色=冻结中）
    RECT bar = rc;
    bar.right = bar.left + 6;
    HBRUSH accent = CreateSolidBrush(RGB(220, 80, 80));
    FillRect(hdc, &bar, accent);
    DeleteObject(accent);
    // 文字
    SetBkMode(hdc, TRANSPARENT);
    HFONT font = CreateFontW(22, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                             DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                             0, L"Microsoft YaHei");
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, RGB(220, 225, 230));
    RECT tr = { 22, 8, rc.right - 14, rc.bottom - 8 };
    DrawTextW(hdc, g_toastText, -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, old);
    DeleteObject(font);
}
// 离屏缓冲渲染 + BitBlt 一次性提交（不再用 ULW）
static void TfRedraw() {
    if (!g_toastHwnd) return;
    RECT rc; GetClientRect(g_toastHwnd, &rc);
    int w = rc.right, h = rc.bottom;
    TfEnsureBuffer(w, h);
    if (!g_tfMemDC) return;
    TfRender(g_tfMemDC, w, h);
    // v1.6.12（2026-11-01）【可见性修复·KB-112】放弃 UpdateLayeredWindow，
    //   改走「窗口 DC 一次 BitBlt」。依据是同机对照实验：用
    //   SetLayeredWindowAttributes(LWA_ALPHA) 的 HUD（Sower / PA F5）全部可见，
    //   唯独走 ULW 的三个（AutoFish / EyeFix / TimeFreeze）窗口都建成了却看不见。
    //   → ULW 在本机游戏合成环境下不生效（不只是 alpha 通道的问题）。
    //   LWA_ALPHA 整窗 alpha 已由 InitHud 设定；离屏渲染后只提交一次，
    //   既可见也不闪（与 PA F5 HUD 完全同一条路）。
    HDC target = GetDC(g_toastHwnd);
    if (target) {
        BitBlt(target, 0, 0, w, h, g_tfMemDC, 0, 0, SRCCOPY);
        ReleaseDC(g_toastHwnd, target);
    }
}

static LRESULT CALLBACK ToastWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_PAINT: {
        // v1.3.x：仅 BeginPaint/EndPaint 校验绘制区；实际绘制走 TfRedraw →
        //   UpdateLayeredWindow 一次性提交离屏 DIB（消除分层窗口 GDI 直绘闪烁）。
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        TfRedraw();
        return 0;
    }
    case WM_DESTROY:
        TfFreeBuffer();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        KillTimer(hwnd, 1);
        QolHudMarkHiddenByMod(hwnd);  // 瞬时 toast 自动消失：清守卫标记，防回前台误还原
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void SetToastText(const wchar_t* text) {
    wcsncpy(g_toastText, text, 31);
    g_toastText[31] = 0;
}

static void ShowToastHold(const wchar_t* text) {
    SetToastText(text);
    if (g_toastHwnd) {
        // v1.1.2: 每次显示重算位置（跟随游戏窗口所在显示器）
        int x = 0, y = 0;
        if (ComputeToastPosition(TOAST_W, TOAST_H, &x, &y)) {
            SetWindowPos(g_toastHwnd, HWND_TOPMOST, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
        }
        KillTimer(g_toastHwnd, 1);
        ShowWindow(g_toastHwnd, SW_SHOWNOACTIVATE);
        InvalidateRect(g_toastHwnd, nullptr, TRUE);
    }
}

static void ShowToast1s(const wchar_t* text) {
    SetToastText(text);
    if (g_toastHwnd) {
        KillTimer(g_toastHwnd, 1);
        ShowWindow(g_toastHwnd, SW_SHOWNOACTIVATE);
        SetTimer(g_toastHwnd, 1, 1000, nullptr);
        InvalidateRect(g_toastHwnd, nullptr, TRUE);
    }
}

static void HideToast() {
    if (g_toastHwnd) {
        QolHudMarkHiddenByMod(g_toastHwnd);  // v1.2.4: MOD 主动隐藏：清守卫标记，防回前台误还原
        ShowWindow(g_toastHwnd, SW_HIDE);
    }
}

static DWORD WINAPI ToastThread(LPVOID) {
    WNDCLASSA wc;
    MSG msg;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = ToastWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "TimeFreezeToast";
    if (!RegisterClassA(&wc)) return 0;
    // v1.1.2: 位置计算统一走 ComputeToastPosition（游戏窗口锚定）
    int width = TOAST_W;
    int height = TOAST_H;
    int tx = 0, ty = 0;
    if (!ComputeToastPosition(width, height, &tx, &ty)) {
        tx = 24;
        ty = 100;
    }

    g_toastHwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        "TimeFreezeToast", "TF", WS_POPUP, tx, ty, width, height,
        QolFindGameWindow(), nullptr, wc.hInstance, nullptr);
    if (!g_toastHwnd) return 0;
    SetLayeredWindowAttributes(g_toastHwnd, 0, 228, LWA_ALPHA);
    HRGN rounded = CreateRoundRectRgn(0, 0, width + 1, height + 1, 10, 10);
    if (!SetWindowRgn(g_toastHwnd, rounded, FALSE)) DeleteObject(rounded);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// ============================================================
// 开关切换（v1.1.0：供 QoL 快捷菜单调用，深夜限制保留）
// ============================================================
static void ResetSleepSignal();   // v1.3.0（实现在 worker 区，供手动开关清理状态）

static void ToggleTimeFreeze() {
    // v1.3.0: 玩家手动开关优先——清掉残留的睡觉信号，避免刚开启就被自动解除
    ResetSleepSignal();
    if (g_paused) {
        // 关闭冻结
        InterlockedExchange(&g_paused, 0);
        HideToast();
        Log("[tf] menu TimeFreeze=0 (resumed)\n");
        return;
    }
    // 开启冻结（全天可用）
    // v1.2.12: 移除 g_writeDisabled 拦截分支——写回层已删，不存在失效态
    void* save = GetSave();
    u64 b = 0;
    if (save && ReadTime(save, TIME_B_OFF, &b)) {
        int hour = (int)((b / 3600 + DISPLAY_OFFSET) % 24);
        InterlockedExchange(&g_paused, 1);
        g_freezeBaseline = b;          // v1.2.14: 存冻结基准
        g_freezeSavePtr = save;
        // v1.2.18: 0x3278 镜像字段同步采样冻结基准
        u64 b2 = 0;
        if (ReadTime(save, TIME_B_OFF + 8, &b2) && b2 != 0) {
            g_freezeBaseline2 = b2;
            g_freezeSavePtr2 = save;
        }
        ShowToastHold(L"时间冻结中");
        Log("[tf] menu TimeFreeze=1 (hour=%d b=%llu)\n", hour, (unsigned long long)b);
    } else {
        // v1.2.4: 原此处静默失败——save 读链断裂时明确提示
        ShowToast1s(L"时间冻结开启失败 稍后重试");
        Log("[tf] menu TimeFreeze=1 failed (save chain broken)\n");
    }
}

static bool IsTimeFreezeOn() {
    return g_paused != 0;
}

// ============================================================
// QoL 快捷菜单导出（宿主：ModManager F1）
// ============================================================
extern "C" __declspec(dllexport) int QolQuickMenuItems(
        QolQuickMenuItem* items, int maxItems) {
    int n = 1;  // 时间冻结
    if (items && maxItems >= 1)
        items[0] = { "时间冻结", IsTimeFreezeOn, ToggleTimeFreeze };
    return n;
}

// ============================================================
// v1.3.0: 睡觉解冻（worker 线程）——一次性解除，不自动复冻
// ============================================================
static void ResetSleepSignal() {
    InterlockedExchange(&g_sleepSignal, 0);
}

static void ServiceSleepUnfreeze() {
    if (!InterlockedExchange(&g_sleepSignal, 0)) return;
    if (!g_paused) return;                     // 已经不冻结：忽略
    InterlockedExchange(&g_paused, 0);
    if (g_clockFrozen) RestoreClockFreezePatches();
    void* save = GetSave();
    u64 b = 0;
    if (save) ReadTime(save, TIME_B_OFF, &b);
    ShowToast1s(L"睡觉：时间冻结已解除（需冻结请手动开启）");
    TfSleepMark(TF_S_RELEASED);
    Log("[tf] sleep detected -> freeze released (b=%llu, manual re-enable required)\n",
        (unsigned long long)b);
}

// ============================================================
// worker 线程
// ============================================================
static DWORD WINAPI WorkerThread(LPVOID) {
    Sleep(1500);  // 原 600ms + P1-6 相位偏移 900ms，防多 MOD 同帧共振
    Log("[tf] TimeFreeze v1.3.6-diag loaded; base=%p\n", reinterpret_cast<void*>(g_exeBase));

    while (g_running) {
        Sleep(100);
        HotConfig_Poll();
        // P0 探针（每轮迭代计时上报）
        static int s_bdgSlot = -1;
        if (s_bdgSlot < 0) s_bdgSlot = qol::budget::Slot("TimeFreeze");
        struct BdgGuard {
            int slot; uint64_t t0;
            ~BdgGuard() { qol::budget::Report(slot, qol::budget::NowUs() - t0); }
        } bdgGuard = { s_bdgSlot, qol::budget::NowUs() };

        // v1.3.0: 先处理睡觉解冻（游戏线程置信号后 100ms 内解除冻结）
        ServiceSleepUnfreeze();

        // v1.2.17: 冻结 = 5 处 NOP（A-store/B/C/G1/G2），GameAddDay 只封存储、
        // imul 保留；G5-G8 全放行
        // + 100ms 检测：dayIndex 变化→判定睡觉/强制跳天，直接解除冻结；同日漂移→拉回。
        // 写回先读后写（关闭竞态窗口）。
        if (g_paused) {
            if (!g_clockFrozen) ApplyClockFreezePatches();
            void* save = GetSave();
            u64 b = 0;
            if (save && ReadTime(save, TIME_B_OFF, &b)) {
                if (save != g_freezeSavePtr) {
                    g_freezeSavePtr = save;
                    g_freezeBaseline = b;   // 换档/重建：以新档当前时刻为基准
                    // v1.2.18: 镜像字段基准同步重采样
                    g_freezeSavePtr2 = nullptr;
                    g_freezeBaseline2 = 0;
                    Log("[tf] freeze baseline resampled (b=%llu)\n",
                        (unsigned long long)b);
                }
                // v1.2.15 核心：跳天检测（dayIndex 变化 = 睡觉/强制跳天）。
                // b 是自 day0 起的总秒，dayIndex = b/86400；睡觉睡到次日早晨
                // 固定时刻，b 增量仅 1~11 小时，不能用 delta>=86400 检测。
                if (b / 86400 != g_freezeBaseline / 86400) {
                    Log("[tf] day-jump detected (b=%llu base=%llu) — release freeze\n",
                        (unsigned long long)b,
                        (unsigned long long)g_freezeBaseline);
                    InterlockedExchange(&g_paused, 0);
                    if (g_clockFrozen) RestoreClockFreezePatches();
                    // v1.3.0: 睡觉/强制跳天 = 一次性解除冻结，不自动复冻（玩家自行重新开启）
                    TfSleepMark(TF_S_DAYJUMP);
                    ShowToast1s(L"跨天：时间冻结已解除（需冻结请手动开启）");
                    continue;
                }
                // v1.2.15: 同日漂移才拉回（先读后写——b 已读过，仅当确实
                // 漂移时写回；若此处读到的是 G5/G6 刚写的新天值，上面
                // dayIndex 分支已拦截，不会走到这里覆盖它）
                // v1.3.0: 游戏线程已请求跳时间（睡觉/设时间）时本轮不拉回，
                // 避免把脚本刚写入的早晨时刻抢回基准（下一轮即解除冻结）
                if (b != g_freezeBaseline && !g_sleepSignal) {
                    // v1.2.18 修复：0x3278 是「时钟 b 的镜像」字段，语义上
                    // 与 0x3270 同源（FN-009 时间结构 + G7/G8 对偶写点均
                    // 成对读两个字段）。旧版把 baseline 原样写进 0x3278，
                    // 但 0x3278 的基线从来只有一个采样点——改成"漂移前值
                    // 读到多少写回多少"（即两个字段各自拉回自己的基准），
                    // 并对 0x3278 建立独立基准，避免两个单位不同的字段被
                    // 同一个值覆盖后产生慢性偏移。
                    u64 b2 = 0;
                    if (save == g_freezeSavePtr2) {
                        b2 = g_freezeBaseline2;
                    } else {
                        g_freezeSavePtr2 = save;
                        b2 = 0;  // 新存档：本轮先采样，不写
                    }
                    if (b2 != 0) {
                        u64 b2cur = 0;
                        if (ReadTime(save, TIME_B_OFF + 8, &b2cur) &&
                            b2cur != b2) {
                            WriteBack(save, TIME_B_OFF + 8, b2);
                        } else if (b2cur == 0) {
                            g_freezeBaseline2 = 0;      // 字段变空：重采样
                            g_freezeSavePtr2 = nullptr;
                        }
                    } else {
                        u64 b2cur = 0;
                        if (ReadTime(save, TIME_B_OFF + 8, &b2cur) &&
                            b2cur != 0) {
                            g_freezeBaseline2 = b2cur;  // 采样
                            g_freezeSavePtr2 = save;
                        }
                    }
                    WriteBack(save, TIME_B_OFF, g_freezeBaseline);      // 0x3270
                }
            }
        } else {
            if (g_clockFrozen) RestoreClockFreezePatches();
        }
#if DIAG_TIMEFREEZE
        // v1.2.14-diag: 高频 watch（500ms）——确认 b 是否仍被推/写回是否拉回
        if (g_paused) {
            static ULONGLONG s_lastWatch = 0;
            ULONGLONG now = GetTickCount64();
            if (s_lastWatch == 0 || now - s_lastWatch > 500) {
                s_lastWatch = now;
                void* save = GetSave();
                u64 a = 0, b = 0;
                if (save && ReadTime(save, TIME_A_OFF, &a) && ReadTime(save, TIME_B_OFF, &b))
                    DiagFrozenWatch(b, a);
            }
        }
#endif
        // v1.6.x：冻结状态寄存器（低频发布，1s 一次；AI 可读，避免每 100ms 写共享内存）
        // 语义：frozen=玩家已开启冻结；patches=时钟写点已被 NOP 封住；sleepSignal=已收到解冻信号
        {
            static ULONGLONG s_obsPub = 0;
            ULONGLONG nowMs = GetTickCount64();
            if (s_obsPub == 0 || nowMs - s_obsPub > 1000) {
                s_obsPub = nowMs;
                qol::obs::SetInt("timefreeze", "frozen",  g_paused ? 1 : 0);
                qol::obs::SetInt("timefreeze", "patches", g_clockFrozen ? 1 : 0);
                qol::obs::SetInt("timefreeze", "sleepSignal", g_sleepSignal ? 1 : 0);
                // v1.3.6-diag: G7 机理三计数（obs 容量 8 个 int，本 MOD 用满 8 个）
                qol::obs::SetInt("timefreeze", "adddayCalls",  (int)g_diagAddDayCalls);
                qol::obs::SetInt("timefreeze", "adddayFrozen", (int)g_diagAddDayFrozen);
                qol::obs::SetInt("timefreeze", "adddayJump",   (int)g_diagAddDayJump);
            }
        }
        // v1.2.4: 失焦隐藏 toast（不飘桌面）
        QolHudGuardVisibility(g_toastHwnd);
    }
    return 0;
}

// ============================================================
// preflight：clock 锚点验证
// ============================================================
static bool Preflight() {
    g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_exeBase) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(g_exeBase);
    if (p[RVA_CLOCK_LEA] != 0x48 || p[RVA_CLOCK_LEA + 1] != 0x8B ||
        p[RVA_CLOCK_LEA + 2] != 0x05) {
        Log("[tf] FAIL: clock lea mismatch\n");
        return false;
    }
    return true;
}

// ============================================================
// DLL 入口
// ============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        LogOpen("timefreeze");
        qol::obs::Init(nullptr);  // v1.6.x: 统一观测层接入
        qol::obs::SetInt("timefreeze", "ready", 0);
    // v-local：游戏版本闸门（village.exe 非 v1.20 build 25311578 则不启用）
    if (!qol::ExeBuildSupported()) {
        Log("[TimeFreeze] 游戏版本不匹配（非 v1.20 build 25311578）→ 本 MOD 不启用\n");
        qol::obs::SetInt("timefreeze", "exeOk", 0);
        qol::obs::EventF(3, 2500, L"[TimeFreeze] 游戏版本不匹配→本 MOD 不启用");
        return TRUE;
    }
        // v1.2.12: TIME_A_OFF/TIME_B_OFF 仅用于【只读】（菜单开启 hour 日志 +
        // diag 冻结监视）；写回层已删除，热调偏移无写入风险，读坏仅日志值异常
        HotConfig_Register("timefreeze", (void*)&RVA_CLOCK_LEA,    "RVA_CLOCK_LEA",    HOT_RVA, 0xE2EA3);
        HotConfig_Register("timefreeze", (void*)&CLOCK_SAVE_0x208, "CLOCK_SAVE_0x208", HOT_RVA, 0x208);
        HotConfig_Register("timefreeze", (void*)&TIME_A_OFF,       "TIME_A_OFF",       HOT_RVA, 0x3268);
        HotConfig_Register("timefreeze", (void*)&TIME_B_OFF,       "TIME_B_OFF",       HOT_RVA, 0x3270);
        HotConfig_Register("timefreeze", (void*)&RVA_CLOCK_FREEZE_A, "RVA_CLOCK_FREEZE_A", HOT_RVA, 0x48D3DE);
        HotConfig_Register("timefreeze", (void*)&RVA_CLOCK_FREEZE_B, "RVA_CLOCK_FREEZE_B", HOT_RVA, 0x158E75);
        HotConfig_Register("timefreeze", (void*)&RVA_CLOCK_FREEZE_C, "RVA_CLOCK_FREEZE_C", HOT_RVA, 0x1442AB);
        HotConfig_Register("timefreeze", (void*)&RVA_CLOCK_CALIB_G1, "RVA_CLOCK_CALIB_G1", HOT_RVA, 0x2D9DFB);
        HotConfig_Register("timefreeze", (void*)&RVA_CLOCK_CALIB_G2, "RVA_CLOCK_CALIB_G2", HOT_RVA, 0x2D9E6C);
        HotConfig_Register("timefreeze", (void*)&RVA_CLOCK_FREEZE_G5, "RVA_CLOCK_FREEZE_G5", HOT_RVA, 0x46CF7A);
        HotConfig_Register("timefreeze", (void*)&RVA_CLOCK_FREEZE_G6, "RVA_CLOCK_FREEZE_G6", HOT_RVA, 0x46D216);
        HotConfig_Register("timefreeze", (void*)&RVA_CLOCK_FREEZE_G7, "RVA_CLOCK_FREEZE_G7", HOT_RVA, 0x48D40D);
        HotConfig_Register("timefreeze", (void*)&RVA_CLOCK_FREEZE_G8, "RVA_CLOCK_FREEZE_G8", HOT_RVA, 0x491718);
        // v1.3.5: 条件放行大跳变阈值（秒）。src-cur >= 该值视为游戏主动推进。
        HotConfig_Register("timefreeze", (void*)&g_passMinJump, "TF_PASS_MINJUMP", HOT_INT, 300);
        HotConfig_Poll();
        HotConfig_DumpCE("timefreeze");
        if (!Preflight()) {
            Log("[tf] preflight failed, abort\n");
            LogFlush();  // 2026-10-30：abort 路径禁用 LogClose（同 KB-092 修复）——
                         // DllMain(ATTACH) 持加载器锁时拆除日志后端（杀异步日志线程+
                         // 删临界区）会引发 ~2s 后 write@0x24 进程崩溃。
            return TRUE;
        }
        SetProcessDPIAware();
        // v1.3.0: 睡觉自动解冻 hook（GameTimeWaitUntil 实现 / GameTimeSetTime）
        // 注意：这两处是 trampoline hook，不做 HotConfig 运行时改写（改了也不会移动已装钩子）
        InstallSleepHooks();
        g_running = 1;
        g_hToast  = CreateThread(nullptr, 0, ToastThread, nullptr, 0, nullptr);
        g_hWorker = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        // v1.6.x：观测层就绪态（AI 可读：exeOk=闸门通过 / ready=已装钩子起线程）
        qol::obs::SetInt("timefreeze", "exeOk", 1);
        qol::obs::SetInt("timefreeze", "ready", 1);
        qol::obs::EventF(1, 2500, L"[TimeFreeze] 已加载：观测层接入（睡觉解冻钩子已装）");
        // loaded 日志由 WorkerThread 统一输出（v1.1.0），此处不重复打印
    } else if (reason == DLL_PROCESS_DETACH) {
        g_running = 0;
        // v1.3.2: 卸载清理——信号线程退出并 join（2s 超时，不强制 TerminateThread）
        // 线程退出时会自行销毁其 toast 窗口，避免 WndProc 悬空引用已卸载 DLL
        if (g_hToast)  { WaitForSingleObject(g_hToast, 2000);  CloseHandle(g_hToast);  g_hToast = nullptr; }
        if (g_hWorker) { WaitForSingleObject(g_hWorker, 2000); CloseHandle(g_hWorker); g_hWorker = nullptr; }
        LogClose();
        qol::obs::Shutdown();  // v1.6.x: 解观测层映射（仅本 DLL 视图）
    }
    return TRUE;
}
