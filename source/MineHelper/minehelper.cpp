// minehelper.cpp —— 下矿助手 (MineHelper v1.0.17-diag for v1.20：诊断版——lvl dump + 禁 patch；v1.0.14 正式版基础上)
//
// v1.0.7 → v1.0.8（BUG 审查修复）：
//   ① 触发点 X 上限 0x7F：cmp imm8 按“有符号”符号扩展参与比较，
//      X≥128 时（u8 截断后 ≥0x80 → 负数）正数剩余 b 永不相等 →
//      洞口永不生成（软锁）。上限 127 与原版 gloaming cap 一致。
//      深层单层石头 ≥133 时必触发，v1.0.7 “X 不 cap”把保护丢了。
//   ② RestoreMinePatches 重置 g_lastFloor：修复出洞后同层号重进
//      （续进度/电梯）不重算 cmp imm → 需砸完全部石头才出洞的静默失效。
//   ③ DETACH 恢复加 g_patched 门禁：preflight 失败时 g_origXxx 仍是
//      BSS 全零，旧实现会用零覆写游戏代码 6+6+1 字节（潜伏于热卸载场景）。
//   ④ CreateThread 句柄关闭（防御性清理）。
//
// 功能（移植自 MineHelper v1.1.1 by gloaming，1.08.1 → 1.09 重新适配）：
//   - 矿洞中保证最多敲 5 块石头就找到矿洞
//
// 机制（静态分析 minehelper.dll + 1.20 exe 逆向）：
//   游戏"挖矿判定"函数（RVA 0x1A9ACA 附近）：
//     cmp dword [rdi+0x3C], imm8   <- 该函数是否进入主体逻辑的判定
//     jne +0x35D                    <- jne1（跳到 rdi 引用计数收尾 + 返回）
//     cmp byte  [rdi+0x38], 0
//     jne +0x33F                    <- jne2（同样跳到收尾返回）
//     主体路径 = 在矿洞地图上生成对象（分配 0x370 字节 + 坐标计算 + 链表加入）
//   - jne2 恒 NOP×6：主体逻辑每次必走
//   - jne1 保持原样：由 cmp 阈值控制进入主体的频率
//   - cmp 立即数 imm8 动态改写为 dil（上限 5）：按楼层数据 b 调整判定基准，
//     使"进入主体逻辑"的频率与楼层关联，达成"最多 5 块出洞"
//
// 1.20 适配地址（v1.09→v1.20 消歧：16B 签名在新 exe 有 2 匹配，0x1A9ACA 与
//   CLOCK_LEA 0xE2EA3 距离 0x171 与旧版一致；0x1AA2AB 距离为负属模板误匹配）：
//   mine_cmp sig   : 0x1A9ACA（唯一）
//   cmp            : RVA 0x1A9ACA
//   cmp+3 (imm8)   : RVA 0x1A9ACD
//   jne1           : RVA 0x1A9ACE
//   jne2           : RVA 0x1A9AD8
//   clock 锚点     : RVA 0xE2EA3（mov rax,[rip+..] 引全局指针），目标 RVA 0x1104C80
//   clock 读链     : [clock] -> +0x208 -> +0x34E0 -> b=[+0x3C], X=[+0x34]
//
// 简化说明（相对原版 1.1.1）：
//   - 不做 Win32 HUD 窗口
//   - 日志走共享框架（发布版关闭）
//   - 去掉 F9 热键 / FirstHole / minehelper.txt 配置
//   - dil 上限 5（原版上限 0x7F=127，约 15 块出洞）
//   - dil 只在值变化时写入（原版每 100ms 无条件写）

#include <windows.h>
#include "obs.h"              // v1.6.x: 统一观测层（AI 可读寄存器 + toast）
#include <cstdint>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "logging.h"
#include "exe_guard.h"   // v-local: 统一游戏版本闸门
#include "hot_config.h"
#include "patch_safety.h"
#include "budget.h"        // P0 帧耗时探针（跨 DLL 共享）
#include "state.h"         // P1-2 状态感知暂停（载入/菜单静默）

// 日志开关：发布版关闭（调试时取消注释）
// v1.0.21 转正：日志关闭（qol_minehelper.log 不再生成，定位时取消下行注释重编）
// 日志运行时开关（2026-10-24）：编译期宏 MINEHELPER_LOGGING 退役，改由 QoL_Shared 运行时门控。
// 开启方式：游戏根 qol_log.cfg 写 level=1（约 1s 生效，免重编免重启）；缺省/level=0 为静默。

// 性能止血 v1.0.14：base FAIL 日志门控（默认关）
// 不在矿洞时每 100ms 触发、日志 819KB；调试时 #define DIAG_MINE_BASE_FAIL 1 开启
#ifndef DIAG_MINE_BASE_FAIL
#define DIAG_MINE_BASE_FAIL 0   // v1.0.21 转正：关 base FAIL 日志
#endif

// [diag] 诊断日志门控（默认关）；调试时 #define DIAG_MINEHELPER 1 开启
#ifndef DIAG_MINEHELPER
#define DIAG_MINEHELPER 0       // v1.0.21 转正：关 lvl 对象 dump
#endif

// v1.0.17-diag 曾禁 patch；v1.0.18 验证版恢复 patch + 写后回读验证（DIAG 宏保留门控，转正删）
// v1.0.19: 错过触发点保底（软锁修复，用户实测“全部石头打完都不出洞”的机制性根因）——
//   触发点 X 由 100ms tick 写入，若玩家在写入前敲过头（进层载入静默期/
//   瞬间连敲），b 跌破 imm8 后 cmp [rdi+0x3C], imm8 永不相等 → 全部打完
//   也不出洞（b=0 后挖矿判定不再运行 = 永久错过）。
//   保底：tick 检测 b-1 < 当前 imm8 时拉 imm8=b-1（下一块敲完必出洞）；
//   每层只拉一次（防出洞后继续敲石误触发重复生成洞口对象）；
//   [lvl+0x38]!=0（jne2 原生门字段，疑似“洞口已生成”标志）时不拉。
// v1.0.21: 洞口生成即封洞（用户实测“出现两个洞”根治）——gate38 0→1 沿检测
//   （=洞口对象已生成）。jne2 被 NOP 后游戏无原生防重，玩家继续敲石时
//   保底/再触发可使 cmp 再次相等 → 二次生成洞口=双洞。沿上升沿把 imm8
//   写为 0xFF（不可达值），此后任意 b 的 cmp 均不等 → 每层洞口至多生成
//   一次；同时原生 b=0 洞也被封死。进层/restore 时沿基准复位。
#ifndef DIAG_MINE_NO_PATCH
#define DIAG_MINE_NO_PATCH 0
#endif

using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8  = std::uint8_t;

// ============================================================
// 常量（build 25311578 / v1.20）
// ============================================================

// ---- patch 目标 RVA ----
static volatile uintptr_t RVA_MINE_CMP_IMM = 0x1ACACD;  // cmp [rdi+0x3C], imm8 的 imm8
static volatile uintptr_t RVA_MINE_JNE1    = 0x1ACACE;  // jne1（6B：0F 85 rel32）
static volatile uintptr_t RVA_MINE_JNE2    = 0x1ACAD8;  // jne2（6B：0F 85 rel32）
static volatile uintptr_t RVA_CLOCK_LEA    = 0xE2EA3;  // mov rax,[rip+..]（引全局指针）

// ---- clock 读链偏移（三级解引用，与原版 get_level_obj 一致）----
//   g   = *[clock]               // 第一级
//   d   = *(g + 0x208)           // 第二级（save 对象）
//   lvl = *(d + 0x34E8)          // 第三级（level 对象）← 移植版曾遗漏此级（25640656 已 +8）
//   b   = [lvl + 0x3C]           // 剩余石头（原版 F_REMAIN）
//   x   = [lvl + 0x34]           // 楼层数（原版 F_FLOOR）
static volatile uintptr_t CLOCK_OBJ_0x208    = 0x208;
static volatile uintptr_t CLOCK_FLOOR_0x34E0 = 0x34E8;  // 第三级解引用偏移（build 25640656：0x3410 后整体 +8，旧 0x34E0→0x34E8；exe 实测 [0x34E8]=40 处 vs 旧 0x34E0=2）
static volatile uintptr_t CLOCK_B_OFF        = 0x3C;    // b = 剩余石头（F_REMAIN）
static volatile uintptr_t CLOCK_X_OFF        = 0x34;    // x = 楼层数（F_FLOOR）

// ---- 签名（preflight 验证） ----
static const unsigned char kSexpCmpSeq[16] = {
    0x83, 0x7F, 0x3C, 0x00, 0x0F, 0x85, 0x5D, 0x03,
    0x00, 0x00, 0x80, 0x7F, 0x38, 0x00, 0x0F, 0x85
};

// ============================================================
// 运行时状态
// ============================================================
static uintptr_t g_exeBase = 0;
static volatile LONG g_running = 0;
static volatile LONG g_patched = 0;    // v1.0.7: 是否已应用矿洞 patch（出洞恢复）
static constexpr u32 kFloorSentinel = 0xFFFFFFFFu;  // v1.0.8: 非法楼层哨兵
static u32 g_lastFloor = kFloorSentinel;  // v1.0.6/1.0.8: 楼层跟踪（变化才重算；出洞重置）
static int  g_lastTrigger = -1;      // v1.0.19: 当前已写入的 cmp imm8 触发点（-1=未写）
static bool g_overshotArmed = true; // v1.0.19: 保底每层只拉一次（防出洞后继续敲石误触发）
static bool g_lastGate38 = true;    // v1.0.21: 洞口标志 gate38 上轮采样（0->1 沿=洞口已生成，防双洞）

// 备份的原始字节
static unsigned char g_origJne1[6];
static unsigned char g_origJne2[6];
static unsigned char g_origCmpImm;

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
    if (v < 0x10000) return false;  // 无效指针
    *out = reinterpret_cast<void*>(v);
    return true;
}

static bool WritePatchBytes(void* addr, const void* data, size_t len) {
    return qol::WritePatchChecked(addr, data, len);
}

// 计算触发点 X（移植自 MineHelper 1.1.1，块数上限改为 5）
// 原版：a=剩余石头，b=随机块数（1~15），X = a - b（触发点，剩余降到 X 时出洞）
// v1.0.6 修正：r=随机块数限定 [1,5]，X = rem - r
//   （此前把 X cap 5 导致 cmp 恒 5，要砸到剩 5 块才出洞 ≈ 79 块，非“砸 5 块出洞”）
// v1.0.8 修正：X 上限 0x7F——写入目标是 cmp r/m32, imm8 的 imm8，
//   CPU 会将其“符号扩展”成 32 位参与比较；X≥128 时 static_cast<u8> 后
//   ≥0x80 → 负数 → 正数 b 永不相等 → 主体（洞口生成）永不执行（软锁）。
//   X≥256 时还有 mod 256 截断落点随机问题。cap 127 后深层大楼层
//   退化为“砸 rem-127 块出洞”（仍远优于全清）。
static int ComputeDil(u32 rem, u32 floor) {
    if (rem == 0) return 0;
    const u32 tick = GetTickCount();
    const u32 seed = tick ^ static_cast<u32>(g_exeBase) ^ floor;
    int r;   // 要砸的块数
    if (rem <= 5) {
        r = static_cast<int>(seed % rem) + 1;   // 剩余很少：砸到完
    } else {
        r = static_cast<int>(seed % 5) + 1;     // [1,5] 最多砸 5 块
    }
    int X = static_cast<int>(rem) - r;          // 触发点（绝对剩余值）
    if (X < 0) X = 0;                            // clamp 0：砸完最后一块必出洞
    if (X > 0x7F) X = 0x7F;                      // v1.0.8: imm8 上限（防符号扩展软锁）
    return X;
}

// ============================================================
// patch 应用
// ============================================================
static void ApplyMineJne2() {
    if (!g_exeBase) return;
    unsigned char nop6[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    void* addr = reinterpret_cast<void*>(g_exeBase + RVA_MINE_JNE2);
    if (WritePatchBytes(addr, nop6, 6))
        Log("[mine] jne2 nop6 OK @%p\n", addr);
}

static void ApplyCmpImm(u8 dil) {
    if (!g_exeBase) return;
    void* addr = reinterpret_cast<void*>(g_exeBase + RVA_MINE_CMP_IMM);
    if (WritePatchBytes(addr, &dil, 1)) {
        // v1.0.18: 写后回读验证（确认 imm8 真写入）
        u8 rb = 0xFF;
        __try { rb = *reinterpret_cast<u8*>(addr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { rb = 0xFF; }
        Log("[mine] cmp+3 dil=%u (readback=%u %s)\n", dil, rb, rb == dil ? "OK" : "MISMATCH");
    }
}

// v1.0.7: 进入矿洞时应用 patch（jne2 NOP）
static void ApplyMinePatches() {
    if (g_patched) return;
    ApplyMineJne2();
    g_patched = 1;
}

// v1.0.7: 离开矿洞恢复原始字节（对齐原版：避免影响通用破坏/收割结算路径）
static void RestoreMinePatches() {
    if (!g_patched) return;
    if (g_exeBase) {
        WritePatchBytes(reinterpret_cast<void*>(g_exeBase + RVA_MINE_JNE2),
                        g_origJne2, 6);
        WritePatchBytes(reinterpret_cast<void*>(g_exeBase + RVA_MINE_CMP_IMM),
                       &g_origCmpImm, 1);
    }
    g_patched = 0;
    g_lastFloor = kFloorSentinel;  // v1.0.8: 重置楼层跟踪（同层号重进时强制重算 cmp imm）
    g_lastTrigger = -1;            // v1.0.19: 同步重置触发点跟踪
    g_overshotArmed = true;         // v1.0.19: 保底重新武装
    g_lastGate38 = true;            // v1.0.21: 出洞复位沿基准（防同层重进残留误封）
    Log("[mine] patches restored (out of mine)\n");
}

// ============================================================
// 主线程：读楼层 -> 计算 dil -> 写 cmp
// ============================================================
static void MineHelperTick() {
    // v1.0.5: clock 读链修正——三级解引用（对齐原版 get_level_obj）
    //   g   = *[clock]               // 第一级：读全局指针
    //   d   = *(g + 0x208)           // 第二级：save 对象
    //   lvl = *(d + 0x34E8)          // 第三级：level 对象 ← v1.0.4 遗漏此级（25640656 已 +8）
    //   b   = [lvl + 0x3C]           // 剩余石头
    //   x   = [lvl + 0x34]           // 楼层数
    void* base = nullptr;
    {
        const int32_t* disp = reinterpret_cast<const int32_t*>(
            g_exeBase + RVA_CLOCK_LEA + 3);
        uintptr_t addr = g_exeBase + RVA_CLOCK_LEA + 7 + *disp;
        if (!ReadPtr(reinterpret_cast<void*>(addr), &base)) {
#if DIAG_MINE_BASE_FAIL
            Log("[mine] base FAIL\n");
#endif
            return;
        }
    }
    // d = *(base + 0x208)（第二级解引用）
    void* save = nullptr;
    if (!ReadPtr(reinterpret_cast<void*>(
            reinterpret_cast<unsigned char*>(base) + CLOCK_OBJ_0x208), &save)) {
        Log("[mine] save FAIL (base=%p)\n", base);
        return;
    }
    // lvl = *(save + 0x34E8)（第三级解引用 ← v1.0.5 修复；25640656 已由 0x34E0 +8）
    void* levelObj = nullptr;
    if (!ReadPtr(reinterpret_cast<void*>(
            reinterpret_cast<unsigned char*>(save) + CLOCK_FLOOR_0x34E0), &levelObj)) {
        // v1.0.7: 不在矿洞（lvl 无效）→ 恢复原始字节
        RestoreMinePatches();
        Log("[mine] level_obj FAIL (save=%p), restore\n", save);
        return;
    }
    unsigned char* fb = reinterpret_cast<unsigned char*>(levelObj);
    u32 b = 0, x = 0;
    __try {
        b = *reinterpret_cast<u32*>(fb + CLOCK_B_OFF);
        x = *reinterpret_cast<u32*>(fb + CLOCK_X_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RestoreMinePatches();
        Log("[mine] fb SEH (fb=%p), restore\n", (void*)fb);
        return;
    }
    Log("[mine] base=%p save=%p lvl=%p b=%u x=%u\n", base, save, levelObj, b, x);
    // v1.0.12-diag: b==0 时 dump lvl 前 0x80 字节（60s 限频）——
    // 验证 v1.20 下 +0x3C/+0x34 偏移是否漂移（用户在矿洞时此 dump 可定位真实字段）
    if (b == 0) {
        static ULONGLONG s_lastDump = 0;
        ULONGLONG now = GetTickCount64();
        if (s_lastDump == 0 || now - s_lastDump > 60000) {
            s_lastDump = now;
#if DIAG_MINEHELPER
            Log("[mine][diag] lvl dump (fb=%p):\n", (void*)fb);
#endif
            for (int row = 0; row < 8; ++row) {
                u64 vals[2] = {0, 0};
                __try {
                    memcpy(vals, fb + row * 16, 16);
                } __except (EXCEPTION_EXECUTE_HANDLER) { vals[0] = vals[1] = 0; }
#if DIAG_MINEHELPER
                Log("[mine][diag]   +%02X: %016llX %016llX\n",
                    row * 16, (unsigned long long)vals[0], (unsigned long long)vals[1]);
#endif
            }
        }
    }
    if (b == 0 || b >= 10000) {
        // v1.0.7: 楼层/石头数据无效 = 不在矿洞 → 恢复
        RestoreMinePatches();
        return;
    }

    // v1.0.7: 确认在矿洞 → 应用 patch
#if DIAG_MINE_NO_PATCH
    // v1.0.17-diag: 只识别不写入（防 b 漂移误触发 imm8 软锁；转正版恢复此段）
#else
    ApplyMinePatches();
#endif
    // v1.0.6: 楼层变化才重算触发点（对齐原版 last_floor 逻辑，避免每 tick 抖动）
    if (x != g_lastFloor) {
        g_lastFloor = x;
#if DIAG_MINE_NO_PATCH
        Log("[mine][diag] floor %u: rem=%u (no-patch)\n", x, b);
#else
        int X = ComputeDil(b, x);
        ApplyCmpImm(static_cast<u8>(X));
        g_lastTrigger = X;
        g_overshotArmed = true;
        g_lastGate38 = true;         // v1.0.21: 新层先把沿基准同步（防残留洞误封）
        Log("[mine] new floor %u: rem=%u trigger=%d\n", x, b, X);
#endif
    }
#if !DIAG_MINE_NO_PATCH
    // v1.0.19: 错过触发点保底（软锁修复，详见头部 v1.0.19 注释）。
    // b-1 < 当前 imm8 = 已敲过触发点（X 写入晚于玩家敲击）→ 拉 imm8=b-1，
    // 下一块敲完必出洞；每层只拉一次。[lvl+0x38]!=0 时疑似洞口已生成，不拉。
    else if (g_overshotArmed && g_lastTrigger >= 0 && (int)b - 1 < g_lastTrigger) {
        u8 gate = 0;
        __try { gate = *reinterpret_cast<u8*>(fb + 0x38); }
        __except (EXCEPTION_EXECUTE_HANDLER) { gate = 0xFF; }  // 读不到 fail-closed
        if (gate == 0) {
            u8 newImm = (b <= 1) ? 0 : static_cast<u8>(b - 1);
            ApplyCmpImm(newImm);
            g_lastTrigger = newImm;
            g_overshotArmed = false;
            Log("[mine] overshot guard: b=%u -> imm8=%u (gate38=0, next block opens)\n",
                b, newImm);
        } else {
            g_overshotArmed = false;  // 洞口疑似已生成，本层不再保底
            Log("[mine] overshot but gate38=%u (hole likely spawned), guard skipped\n",
                gate);
        }
    }
    // v1.0.21: 洞口生成即封死（双洞根治，详见头部 v1.0.21 注释）
    // gate38 0->1 沿：洞口已生成 → imm8 写 0xFF（不可达）→ 二次 cmp 永不相等。
    {
        u8 gate = 0;
        __try { gate = *reinterpret_cast<u8*>(fb + 0x38); }
        __except (EXCEPTION_EXECUTE_HANDLER) { gate = 0xFF; }
        if (gate == 1 && !g_lastGate38 && g_lastTrigger >= 0 && g_lastTrigger != 0xFF) {
            ApplyCmpImm(0xFF);
            g_lastTrigger = 0xFF;
            g_overshotArmed = false;   // 已出洞：本层不再保底
            Log("[mine] hole spawned (gate38 0->1): imm8 sealed 0xFF, no second hole\n");
        }
        g_lastGate38 = (gate == 1);
    }
#endif
}

static DWORD WINAPI MineHelperLoop(LPVOID) {
    Sleep(700);  // P1-6: 相位偏移 700ms，防多 MOD 同帧共振
    Log("[mine] loop started\n");
    while (g_running) {
        static int s_bdgSlot = -1;  // P0 探针
        if (s_bdgSlot < 0) s_bdgSlot = qol::budget::Slot("MineHelper");
        uint64_t bdgT0 = qol::budget::NowUs();
        HotConfig_Poll();
        if (!QolGameBusy()) MineHelperTick();  // P1-2: 载入/菜单期间静默
        qol::budget::Report(s_bdgSlot, qol::budget::NowUs() - bdgT0);
        Sleep(100);
    }
    return 0;
}

// ============================================================
// preflight：验证 RVA 处字节 + 备份原始字节
// ============================================================
static bool PreflightAndBackup() {
    g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_exeBase) return false;

    const unsigned char* p = reinterpret_cast<const unsigned char*>(g_exeBase);

    // 1. 验证 cmp 序列（0x1A9ACA 处 14 字节）
    if (memcmp(p + RVA_MINE_CMP_IMM - 3, kSexpCmpSeq, 14) != 0) {
        Log("[mine] FAIL: cmp seq mismatch\n");
        return false;
    }
    // 2. 验证 jne1 / jne2 是 0F 85
    if (p[RVA_MINE_JNE1] != 0x0F || p[RVA_MINE_JNE1 + 1] != 0x85 ||
        p[RVA_MINE_JNE2] != 0x0F || p[RVA_MINE_JNE2 + 1] != 0x85) {
        Log("[mine] FAIL: jne sig mismatch\n");
        return false;
    }
    // 3. 验证 clock lea（48 8B 05 disp32）
    if (p[RVA_CLOCK_LEA] != 0x48 || p[RVA_CLOCK_LEA + 1] != 0x8B ||
        p[RVA_CLOCK_LEA + 2] != 0x05) {
        Log("[mine] FAIL: clock lea mismatch\n");
        return false;
    }

    // 备份原始字节
    memcpy(g_origJne1, p + RVA_MINE_JNE1, 6);
    memcpy(g_origJne2, p + RVA_MINE_JNE2, 6);
    g_origCmpImm = p[RVA_MINE_CMP_IMM];
    Log("[mine] backup: jne1=%02X%02X%02X%02X%02X%02X jne2=%02X%02X%02X%02X%02X%02X cmp=%02X\n",
        g_origJne1[0], g_origJne1[1], g_origJne1[2], g_origJne1[3],
        g_origJne1[4], g_origJne1[5],
        g_origJne2[0], g_origJne2[1], g_origJne2[2], g_origJne2[3],
        g_origJne2[4], g_origJne2[5], g_origCmpImm);
    return true;
}

// ============================================================
// DLL 入口
// ============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        LogOpen("minehelper");
        qol::obs::Init(nullptr);  // v1.6.x: 统一观测层接入
        qol::obs::SetInt("minehelper", "ready", 1);
        qol::obs::EventF(1, 2500, L"[MineHelper] 已加载：观测层接入");
    // v-local：游戏版本闸门（village.exe 非 v1.20 build 25311578 则不启用）
    if (!qol::ExeBuildSupported()) {
        Log("[MineHelper] 游戏版本不匹配（非 v1.20 build 25311578）→ 本 MOD 不启用\n");
        return TRUE;
    }
        HotConfig_Register("minehelper", (void*)&RVA_MINE_CMP_IMM, "RVA_MINE_CMP_IMM", HOT_RVA, 0x1ACACD);
        HotConfig_Register("minehelper", (void*)&RVA_MINE_JNE1,    "RVA_MINE_JNE1",    HOT_RVA, 0x1ACACE);
        HotConfig_Register("minehelper", (void*)&RVA_MINE_JNE2,    "RVA_MINE_JNE2",    HOT_RVA, 0x1ACAD8);
        HotConfig_Register("minehelper", (void*)&RVA_CLOCK_LEA,    "RVA_CLOCK_LEA",    HOT_RVA, 0xE2EA3);
        HotConfig_Register("minehelper", (void*)&CLOCK_OBJ_0x208,    "CLOCK_OBJ_0x208",    HOT_RVA, 0x208);
        HotConfig_Register("minehelper", (void*)&CLOCK_FLOOR_0x34E0, "CLOCK_FLOOR_0x34E0", HOT_RVA, 0x34E8);
        HotConfig_Register("minehelper", (void*)&CLOCK_B_OFF,        "CLOCK_B_OFF",        HOT_RVA, 0x3C);
        HotConfig_Register("minehelper", (void*)&CLOCK_X_OFF,        "CLOCK_X_OFF",        HOT_RVA, 0x34);
        HotConfig_Poll();
        HotConfig_DumpCE("minehelper");
        if (!PreflightAndBackup()) {
            Log("[mine] preflight failed, abort\n");
            // 2026-10-28：abort 路径禁用 LogClose——DllMain(ATTACH) 持加载器锁时
            // 拆除日志后端（杀线程+删临界区+关文件）会在约 2s 后引发
            // write@0x24 进程崩溃（五场二分实测定位：MineHelper/BirthdayReminder
            // 的 preflight abort 是启动崩溃唯一触发源）。改 LogFlush 只刷盘，
            // 资源交由 EnsureInit 注册的 atexit(LogFlushAndShutdown) 收尾。
            LogFlush();
            return TRUE;
        }
        g_running = 1;
        // v1.0.7: 不再启动时恒 NOP jne2，改为进矿洞时才 patch、出洞恢复
        HANDLE hThread = CreateThread(nullptr, 0, MineHelperLoop, nullptr, 0, nullptr);
        if (hThread) CloseHandle(hThread);  // v1.0.8: 句柄清理
        Log("[mine] loaded v1.0.21; base=%p\n", reinterpret_cast<void*>(g_exeBase));
    } else if (reason == DLL_PROCESS_DETACH) {
        qol::obs::Shutdown();  // v1.6.x: 解观测层映射（仅本 DLL 视图）
        g_running = 0;
        // v1.0.8: 仅在实际应用过 patch 时恢复——preflight 失败路径下
        // g_origJne1/Jne2/CmpImm 仍是 BSS 全零，旧实现（只判 g_exeBase）
        // 会拿全零覆写游戏代码。g_patched 只有在 preflight 成功 + 进矿
        // 应用后才会置 1，天然排除失败路径。jne1 从未被 patch，无需恢复。
        if (g_exeBase && g_patched) {
            WritePatchBytes(reinterpret_cast<void*>(g_exeBase + RVA_MINE_JNE2),
                            g_origJne2, 6);
            WritePatchBytes(reinterpret_cast<void*>(g_exeBase + RVA_MINE_CMP_IMM),
                            &g_origCmpImm, 1);
        }
        LogClose();
    }
    return TRUE;
}
