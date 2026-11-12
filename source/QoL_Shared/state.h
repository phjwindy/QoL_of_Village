// state.h -- Game State Awareness: detect loading / menu / cutscene / sleeping
//
// P1-2 状态感知暂停（共享机制层）—— 供 14 个 MOD 在 mod_tick 开头一行接入静默。
//
// 用法（MOD 侧极简）：
//   #include "state.h"
//   extern "C" void mod_tick(void) {
//       if (QolGameBusy()) return;   // 一行静默
//       // ... 正常逻辑 ...
//   }
//
// API 清单（一行一个函数）：
//   bool QolGameBusy(void);     // 剧情/菜单/睡觉/载入任一为真 → true（MOD 侧用这个）
//   bool QolLoading(void);      // 载入中/场景切换中（save 链断裂或 game root 空）
//   bool QolInMenu(void);       // 菜单打开中/游戏暂停（时钟停走 > 1s 启发式）
//   bool QolInCutscene(void);   // 剧情演出中（待定位，fail-open 返回 false）
//   bool QolSleeping(void);     // 睡觉中（待定位，fail-open 返回 false）
//
// ============================================================
// 设计说明：每检测项的机制 / 地址或来源 / 可靠性标注
// ============================================================
//
// [QolLoading] —— 可靠性：实测可靠（HIGH）
//   机制 A：clock save 链断裂检测（提炼自 TimeFreeze::GetSave()）
//     读链：[exe+RVA_CLOCK_LEA(0xE2EA3)] → mov rax,[rip+disp32] → clock_mgr
//           [clock_mgr+0x208] → save 对象
//     指令签名：48 8B 05（mov rax,[rip+disp32]），Preflight 校验
//     当 save 对象读不出（null / SEH 异常 / 值 < 0x10000）→ 链断裂
//     触发场景：载入画面、场景切换、读档/换存档槽
//     证据：TimeFreeze v1.2.1 实测可用；HuntOneShot v1.0.9 同源读链
//
//   机制 B：game root 全局指针空检测（提炼自 HuntOneShot::ReadSceneFingerprint）
//     地址：[exe+RVA_GAME_ROOT(0x111CC90)] → 游戏根对象指针
//     当指针为 null 或不可读 → 标题画面/载入中
//     证据：HuntOneShot v1.0.7 场景指纹用此链路实测可用
//     来源：ProductionAuto/version_manifest.h（v1.20 适配值）
//
// [QolInMenu] —— 可靠性：高概率（MEDIUM，启发式，需实测确认）
//   机制：时钟停走检测
//     读取 save 对象内 TIME_B（[save+0x3270]，QWORD 时钟值）
//     连续采样，若值 > 1s 未变化且 save 链完整 → 判定游戏暂停（菜单/暂停菜单）
//     采样间隔：跟随 mod_tick 频率（~60fps），用 GetTickCount64 计时
//   已知限制：
//     ① TimeFreeze 激活时时钟被冻结 → 会误报为"菜单中"（可接受：冻结时 MOD 静默合理）
//     ② 睡觉时时钟快速前进 → 不会被检测为暂停（QolSleeping 需独立机制，待定位）
//     ③ 游戏卡顿/低帧时可能短暂误报（1s 阈值已留足余量）
//   验证方式：游戏内打开菜单 → 日志版 MOD 打印 QolInMenu() 值 → 应为 true
//
// [QolInCutscene] —— 可靠性：待定位（TODO）
//   现有代码中无剧情/演出检测机制。潜在方向：
//     ① GIMMICK_MGR (RVA 0x1104C80, 即 RVA_NEARBY_WORLD_OBJECT_REGISTRY) 可能有事件状态标志
//     ② 游戏根对象内可能有 game_mode / scene_type 字段
//     ③ 需 IDA/CE 逆向定位具体标志位后补充
//   当前：fail-open 返回 false（不误伤）
//
// [QolSleeping] —— 可靠性：待定位（TODO）
//   睡觉时时钟快速前进，与时钟停走检测正交，需独立标志位。
//   潜在方向：动画状态机/事件标志/床交互标志
//   当前：fail-open 返回 false（不误伤）
//
// [QolGameBusy] = QolLoading() || QolInMenu()
//   QolInCutscene/QolSleeping 待定位，暂不纳入（fail-open）。
//   后续定位后可追加 || QolInCutscene() || QolSleeping()
//
// ============================================================
// 失败模式（强制设计）：fail-open
//   任何检测读失败 / 未初始化 / Preflight 失败 → QolGameBusy() 返回 false
//   （维持现状行为，绝不误伤游戏）
//
// ============================================================
// 线程约定：仅游戏主线程（mod_tick 上下文）调用。
//   静态变量 per-DLL（与 budget.h / hot_config.cpp 同模式）。
//   x64 对齐 64 位读写原子；clock 采样无锁可接受（最坏读半新半旧 → 延迟一帧）。
//
// ============================================================
// 版本：v1.20 (build 25311578)
//   RVA_CLOCK_LEA  = 0xE2EA3  (TimeFreeze v1.2.1 实测)
//   RVA_GAME_ROOT = 0x111cc90 (version_manifest.h v1.20)
//   CLOCK_SAVE     = 0x208     (TimeFreeze save 链偏移)
//   TIME_B_OFF     = 0x3270    (TimeFreeze 时钟值偏移)
//   游戏更新后需重新验证 Preflight 签名（48 8B 05）并更新 RVA。
//
// 作者：PHJ&消失的清风
#pragma once
#include <windows.h>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// 主 API：MOD 侧在 mod_tick 开头一行接入
//   if (QolGameBusy()) return;
// 返回 true → 游戏处于忙状态（载入/菜单），MOD 应静默
// 返回 false → 游戏正常，MOD 可执行逻辑
// fail-open：未初始化/检测失败时返回 false（不误伤）
bool QolGameBusy(void);

// 细分查询（备用）
bool QolLoading(void);      // 载入中/场景切换（save 链断裂或 game root 空）
bool QolInMenu(void);       // 菜单中/游戏暂停（时钟停走 > 1s 启发式）
bool QolInCutscene(void);   // 剧情演出中（待定位，fail-open）
bool QolSleeping(void);     // 睡觉中（待定位，fail-open）

#ifdef __cplusplus
}
#endif
