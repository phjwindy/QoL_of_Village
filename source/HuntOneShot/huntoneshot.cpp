// huntoneshot.cpp —— 狩猎一击必杀 (HuntOneShot v1.1.15-diag 诊断版 for v1.20, build 25311578)
// v1.1.15-diag: 开启 DIAG_HUNTONESHOT 收集 gauge 形态实例真实分布——
//   目的：确认 v1.20 动物实例真实 vtable + 初始命中白名单是否漂移
//   （KB-059 附带发现：0 命中主嫌疑=v1.20 改了动物初始命中数值）
//
// v1.1.7 → v1.1.8（noescape v1.0.9 异步化+文件级补丁形态识别，2026-10-01 日志+内存实锤）：
//   v1.0.8 同步全扫实测 ~8s 卡顿（6.4GB 私有堆）；v1.20 Lua chunk 懒加载，场景切换
//   触发扫描时 chunk 常未加载（pass1/2 0 命中；0:05 打猎后 chunk 才进内存
//   0x39E33EFB/0x3E2199D8 两处 PRIVATE 实锤，之后无场景切换错过）。
//   改造：①异步分块 3ms/tick + 30s 重试（零卡顿）；②识别 already-patched 形态
//   （data.dat 文件级补丁方案——参考 BigL233 dinput8.cpp 写文件而非扫内存；
//   文件 patched 后内存即 patched 形态，扫描确认后置 done 永不再扫）；
//   ③块间 overlap 24→32B（escape2 28B 签名跨界漏匹配修正）。
//
// v1.1.6 → v1.1.7（夜间省开销，用户实测反馈 2026-09-30）：
//   夜间无野生动物（用户确认狩猎仅白天有效），夜间扫描纯浪费（2% 占空比
//   仍持续触碰内存页）。新增 SCAN_HOUR_FROM/TO 可热调窗口（默认 7-22，
//   与商店营业日窗一致）；夜间不扫描不 clamp，天亮（raw 跨日=显示 7 点）
//   由跨日 rescan 自然恢复。
//
// v1.1.5 → v1.1.6（日志刷屏 + 无效重扫修复，2026-09-30 实测日志分析）：
//   ① !g_enumerated 分支每 tick 记日志 + 置 g_scanRequested：全内存扫描在
//      2% 占空比（2ms/100ms tick）下一轮需 5~15 分钟游戏时长，短会话内扫不完，
//      每 100ms 刷一行 rescan request（实测 5.5 分钟 3013 行），且扫描完成后
//      因 g_scanRequested 残留 1 立刻再跑一整轮浪费预算。修复：跨日才记日志
//      与请求重扫；未枚举完成只静默补启动（不置 g_scanRequested）。
//   ② 扫描激活延迟为设计特性（分块预算），非本版回归：一击必杀在进游戏
//      后约 5~15 分钟才开始生效，长会话正常。
//
// v1.0.6 → v1.0.7（降帧根治）：
//   空场定时退避重扫（30s→300s 封顶）仍残留卡顿：封顶后每 5 分钟仍启动
//   一轮全内存扫描（约 100s，持续触碰内存页），缓存污染导致“日常行动
//   时不时变卡”（2026-09-14 实测）。彻底去除定时重扫，改为场景切换事件
//   触发：读 root→mapOwner→mapInfo→spatialOwner 链（与 AutoPet
//   GetWorldContext 同源实测）取 spatialOwner 指针为场景指纹，指纹变化
//   （进/出矿洞、跨区、进出建筑、矿洞换层）才重扫一次。日常停留零后台
//   扫描，降帧根除；跨日重扫保留。原空场重扫的场景（列表空=换场景后
//   未收录）由场景切换精准覆盖，响应从最长 5 分钟提升到即时。
//
// v1.0.5 → v1.0.6（用户反馈降帧修复）：
//   空列表重扫从固定 5s 改为指数退避（30s→60s→120s→300s 封顶）。
//   v1.0.5 的 5s 固定间隔在分块扫描下等于永续扫描：全内存一轮
//   100s+，扫完只歇 5s 又扫，在农场/矿洞等无野生动物场景持续
//   后台扫描，用户实测为"长时间停留原地慢慢变卡，离开后恢复"
//   （缓存污染 + alt-tab 后页面换入）。扫到动物即重置退避。
//   代价：进入有动物场景后最长延迟 5 分钟收录（正常场景一轮即中）。
//
// v1.0.4 → v1.0.5（BUG 审查 + 吸收 gloaming 防御优点）：
//   ① 扫描收录与逐 tick 处理均增加 +0x20 vt_sub 范围校验（第二 vtable
//      必须落在 exe 映像内）——防内存复用残留误命中，在单 vtable+池标记+
//      白名单之外增加第四道防线（gloaming 为精确匹配发现值，本版用等价
//      的范围校验，无需维护具体发现值）
//   ② 空列表 5s 限频重扫：旧版每日一扫，当天扫到 0 则整天空窗；
//      动物刷新后最多延迟 5s 收录
//
// 功能（移植自 HuntOneShot v0.2.0 by gloaming，1.08.1 → 1.09 重新适配）：
//   - 可狩猎动物（松鼠/狸猫/鼬鼠/兔子/狐狸/绿雉/野猪/鹿/熊）一枪狩猎成功
//   - 不影响掉落物与狩猎结算，动物仍正常逃跑/消失
//
// 机制（静态分析 huntoneshot.dll + 1.09 exe 逆向）：
//   - 锚点：
//       vt_hunt : 可狩猎动物 vtable（v1.20 RVA 0xE27A08，lea 目标双重验证）
//       clock   : 全局对象读链（v1.20 RVA 0x10FCBB0，mov rdx,[rip+disp] 双重验证）
//       vt_sub  : 第二 vtable（+0x20），1.09 改为“指向 exe 映像内”的
//                 范围校验（v1.0.5，等价防御，无需动态发现具体值）
//   - 扫描：遍历动物实例列表 -> 读 [inst+0x10] 高 32 位（初始命中）
//     白名单 {30,40,50,150,200,500}（松鼠30/狸猫·鼬鼠·兔子40/狐狸·绿雉50/野猪150/鹿200/熊500）
//     -> 命中则低 32 位 clamp 到 10
//   - 跨日重扫：天数变化时重新枚举
//
// 待实测项（v1.0.0）：
//   - 动物实例列表的枚举来源（链表/数组），先用候选偏移探测，日志输出实测确认后回填

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
#include "hotkey.h"
#include "budget.h"        // P0 帧耗时探针（跨 DLL 共享）
#include "state.h"         // P1-2 状态感知暂停（载入/菜单静默）
#include "quickmenu.h"     // F2 统一菜单接入（P0）

// 日志开关：发布版关闭（调试时取消注释）
// 日志运行时开关（2026-10-24）：编译期宏 HUNTONESHOT_LOGGING 退役，改由 QoL_Shared 运行时门控。
// 开启方式：游戏根 qol_log.cfg 写 level=1（约 1s 生效，免重编免重启）；缺省/level=0 为静默。

// [diag] 诊断日志门控（正式版关）；调试时 #define DIAG_HUNTONESHOT 1 开启
// v1.1.18 正式版：关闭（同步全扫修复已实测通过，诊断 dump/state 行不再需要）
#ifndef DIAG_HUNTONESHOT
#define DIAG_HUNTONESHOT 0
#endif

using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8  = std::uint8_t;

// ============================================================
// 常量（v1.20, build 25311578）
// ============================================================

// ---- 锚点 RVA ----
static volatile uintptr_t RVA_VT_HUNT_LEA = 0xE9D35;   // 20261028: 0xE7465→0xE9D35 (AOB 64B 通配唯一命中, Δ+0x28D0 与 CLOCK_LEA 同簇)   // lea rax,[rip+..]（引用动物 vtable）
static volatile uintptr_t RVA_CLOCK_LEA    = 0xE2EA3;   // mov rax,[rip+..]（全局时钟指针；20261028: 0xE05D3→0xE2EA3, AOB 唯一命中, 目标=0x111CC90=新GAME_ROOT）
static volatile uintptr_t RVA_VT_HUNT      = 0xE422D8;  // 20261028: 0xE27A08→0xE422D8 (由新锚点 lea disp 实算; 新旧 vtable 同构 [p,p+0xDF0,+0x10,+0x10])

// ---- clock 读链（与 MineHelper/AutoPet 一致）----
static volatile uintptr_t CLOCK_SAVE_0x208 = 0x208;      // save = *(base + 0x208)（解引用）
static volatile uintptr_t SAVE_RAW_SECOND  = 0x3270;     // save + 0x3270 = raw_second (i64)

// ---- v1.0.7 场景指纹链（root→mapOwner→mapInfo→spatialOwner，与 AutoPet 同源实测）----
static volatile uintptr_t RVA_GAME_ROOT = 0x111cc90;  // 全局根指针（clock 对象）
static volatile uintptr_t ROOT_MAP_OWNER    = 0x268;
static volatile uintptr_t MAP_INFO_OFF      = 0x0d8;
static volatile uintptr_t MAP_SPATIAL_OWNER = 0x030;

// ---- 动物实例偏移 ----
static volatile uintptr_t INST_VTABLE      = 0x00;       // inst + 0x0  = vtable 指针
static volatile uintptr_t INST_CURRENT     = 0x08;       // inst + 0x8  = 当前剩余可被击中次数（u32，每次命中 -10）
static volatile uintptr_t INST_INIT        = 0x10;       // inst + 0x10 = 初始命中次数（高 32 位，战斗中不变）
static volatile uintptr_t INST_POOL_MARK   = 0x18;       // inst + 0x18 = 动态池标记（原版要求 == 0）
static volatile uintptr_t INST_VT_SUB      = 0x20;       // inst + 0x20 = 第二 vtable（vt_sub，指向 exe 映像内）

// ---- 白名单（初始命中次数，对应物种） ----
static const u32 kHitWhitelist[6] = {30, 40, 50, 150, 200, 500};
static constexpr u32 kClampValue  = 10;                    // clamp 目标值

// ---- 全内存扫描上限 ----
static constexpr int kMaxAnimals = 2048;   // 对齐原版 MAX_ANIMALS

// v1.1.10: F6 热键切换一击猎杀开关。关闭时动物不钻洞但正常 HP，
//           可以享受正常打猎体验（需 data.dat escape 补丁配合）。
// v1.1.11: 改用 QolHotKeys 框架支持改键（QoL_hot.json / ModManager 面板）。
static QolHotKeys g_hotkeys;                   // 改键热键集合
static volatile LONG g_clampEnabled = 1;       // 1=一击猎杀开（默认）
static volatile LONG g_clampTogglePrev = 0;    // 边沿检测
static ULONGLONG g_clampToggleTime = 0;        // HUD 显示时间戳
static ULONGLONG g_clampToggleLastTime = 0;     // v1.1.14: 防抖时间戳

// ---- 不钻洞补丁（吸收自 BigL233 Village_QoL_Mod v1.6.7）----
// 运行时扫描 Lua 字节码两处 escape 指令签名，替换 index[2] 跳过逃遁路径
static const u32 kHuntOriginal[5] = {
    0x00000108, 0x7fff8181, 0x0009013c, 0x80002538, 0x81ff8201
};
static const u32 kHuntPatched[5] = {
    0x00000108, 0x7fff8181, 0x80001a38, 0x80002538, 0x81ff8201
};
static const u32 kHuntEscape2Original[7] = {
    0x80000181, 0x7fff8201, 0x000202b4, 0x000501bb,
    0x80002138, 0x007f023f, 0x80002a38
};
static const u32 kHuntEscape2Patched[7] = {
    0x80000181, 0x7fff8201, 0x80002c38, 0x000501bb,
    0x80002138, 0x007f023f, 0x80002a38
};

// ============================================================
// 运行时状态
// ============================================================
static uintptr_t g_exeBase = 0;
static volatile LONG g_running = 0;
static HANDLE g_huntThread = nullptr;   // H5: 保存线程句柄，卸载时等待退出
static u64 g_lastDay = 0;
static u64 g_variantHunt = 0;             // 主猎物 vtable（绝对地址，PreflightAndInit 中设置）
static int g_totalClamped = 0;
static int g_processedCount = 0;
static bool g_enumerated = false;
static void* g_animalList[kMaxAnimals] = {nullptr};   // v1.0.2: 扫描到的实例列表
static int  g_animalCount = 0;

// v1.0.4: 分块扫描状态机（跨 tick 扫描，避免全内存扫描卡顿）
static unsigned char* g_scanAddr = nullptr;            // 当前扫描位置
static const unsigned char* g_scanMax = nullptr;      // 扫描上限
static volatile LONG g_scanActive = 0;                // 扫描进行中
static volatile LONG g_scanRequested = 0;            // 请求新一轮扫描
static int  g_scanPass = 0;                           // 扫描轮次（日志）
static uintptr_t g_exeImageEnd = 0;                   // v1.0.5: exe 映像上界（vt_sub 范围校验）
static void* g_lastSceneFingerprint = nullptr;      // v1.0.7: 上次场景指纹（spatialOwner 指针）
static std::atomic<bool> g_noEscapeDone{false};     // 不钻洞补丁是否已完成
// v1.0.8: 自身 DLL 映像范围（noescape 放宽扫描后需排除，防匹配到自身常量）
static uintptr_t g_selfImageBase = 0;
static uintptr_t g_selfImageEnd = 0;

// ---- 吸收自 BigL233 code_patch_safety.inl 的两项防御 ----
// ①写后读回验证：确认补丁实际写入（防 WriteProcessMemory/SEH 吞错误）
// ②VirtualProtect 恢复校验：恢复原保护属性后检查返回值（防恢复失败留 RW 页）
static bool WritePatchChecked(void* dst, const void* src, size_t len) {
    DWORD oldProt = 0;
    if (!VirtualProtect(dst, len, PAGE_READWRITE, &oldProt)) return false;
    memcpy(dst, src, len);
    // 读回验证
    u32 readback = 0;
    memcpy(&readback, dst, 4);  // 检查前 4 字节
    u32 expected = 0;
    memcpy(&expected, src, 4);
    if (readback != expected) return false;
    // 恢复原保护并校验
    DWORD tmpProt = 0;
    bool restored = VirtualProtect(dst, len, oldProt, &tmpProt) != 0;
    return restored;
}

// v1.1.21: 数据字段轻量写入（clamp HP 等普通数据，非代码补丁）——
// 避免每次写入 2 次 VirtualProtect 往返（10Hz × 14 动物的主要开销）。
// 数据字段页已可写则直接写；读回验证保留（防悬垂/错页）。
static bool WriteFieldChecked(void* dst, const void* src, size_t len) {
    if (!dst || !src || len == 0) return false;
    __try {
        memcpy(dst, src, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    u32 readback = 0, expected = 0;
    memcpy(&readback, dst, 4);
    memcpy(&expected, src, 4);
    return readback == expected;
}

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

static bool IsWhitelisted(u32 hit) {
    for (int i = 0; i < 6; ++i)
        if (kHitWhitelist[i] == hit) return true;
    return false;
}

// ============================================================
// 实例处理：白名单 -> clamp +8（u32）
// ============================================================
static void ProcessInstance(void* inst) {
    if (!inst) return;

    // v1.0.3: 扫描已按 vtable == g_variantHunt 过滤，此处不再重复校验 vtable
    // （内存被回收复用时可读但非动物：poolMark + 白名单兜底）

    // +0x18 动态池标记检查（对齐原版 scan_pool 验证）
    u64 poolMark = 0;
    __try {
        poolMark = *reinterpret_cast<u64*>(
            reinterpret_cast<unsigned char*>(inst) + INST_POOL_MARK);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (poolMark != 0) return;

    // v1.0.5: +0x20 第二 vtable（vt_sub）范围校验（防扫描后内存被复用成非动物对象）
    u64 subVt = 0;
    __try {
        subVt = *reinterpret_cast<u64*>(
            reinterpret_cast<unsigned char*>(inst) + INST_VT_SUB);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (subVt < g_exeBase || subVt >= g_exeImageEnd) return;

    // 初始命中次数（+0x10 高 32 位）
    u64 initRaw = 0;
    __try {
        initRaw = *reinterpret_cast<u64*>(
            reinterpret_cast<unsigned char*>(inst) + INST_INIT);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    u32 init = static_cast<u32>(initRaw >> 32);
    if (!IsWhitelisted(init)) return;

    // 当前剩余可被击中次数（+0x8，u32）
    u32 cur = 0;
    __try {
        cur = *reinterpret_cast<u32*>(
            reinterpret_cast<unsigned char*>(inst) + INST_CURRENT);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    if (cur > 0 && cur != kClampValue) {
        // D5: 改用 WritePatchChecked 安全写入（VirtualProtect 改页 + 读回验证），
        //     替代裸指针写入 *(u32*)(inst+0x08)=kClampValue
void* addr = reinterpret_cast<unsigned char*>(inst) + INST_CURRENT;
        __try {
            if (WriteFieldChecked(addr, &kClampValue, sizeof(kClampValue))) {
                ++g_totalClamped;
                Log("[ho] clamp %p: %u -> %u (init=%u)\n", inst, cur, kClampValue, init);
            } else {
                Log("[ho] clamp FAIL %p (write verify)\n", inst);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[ho] clamp FAIL %p (SEH)\n", inst);
        }
    }
}

// v1.1.14: 恢复已 clamp 动物至原始 HP（toggle OFF 时逆向操作）
// 旧版仅停止新 clamp，已 clamp 的动物保持 10 HP，用户感知"关不掉一击必杀"。
// 仅恢复 cur == kClampValue(10) 的动物——未被 clamp 或已被击杀的不动。
static void RestoreInstance(void* inst) {
    if (!inst) return;

    u64 poolMark = 0;
    __try {
        poolMark = *reinterpret_cast<u64*>(
            reinterpret_cast<unsigned char*>(inst) + INST_POOL_MARK);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (poolMark != 0) return;

    u64 subVt = 0;
    __try {
        subVt = *reinterpret_cast<u64*>(
            reinterpret_cast<unsigned char*>(inst) + INST_VT_SUB);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (subVt < g_exeBase || subVt >= g_exeImageEnd) return;

    u64 initRaw = 0;
    __try {
        initRaw = *reinterpret_cast<u64*>(
            reinterpret_cast<unsigned char*>(inst) + INST_INIT);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    u32 init = static_cast<u32>(initRaw >> 32);
    if (!IsWhitelisted(init)) return;

    u32 cur = 0;
    __try {
        cur = *reinterpret_cast<u32*>(
            reinterpret_cast<unsigned char*>(inst) + INST_CURRENT);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    // 仅恢复被我们 clamp 到 10 的动物（cur == kClampValue）
    if (cur == kClampValue) {
        void* addr = reinterpret_cast<unsigned char*>(inst) + INST_CURRENT;
        __try {
            if (WriteFieldChecked(addr, &init, sizeof(init))) {
                Log("[ho] restore %p: %u -> %u (init=%u)\n", inst, kClampValue, init, init);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// ============================================================
// 扫描：全内存扫描 vt_hunt 动态实例（对齐原版 scan_pool）
// ============================================================
static bool ReadMem(void* dst, const void* src, size_t len) {
    __try {
        memcpy(dst, src, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static u64 ReadMemU64(const void* p) {
    u64 v = 0;
    if (!ReadMem(&v, p, sizeof(v))) return 0;
    return v;
}

// 每 tick 扫描时间预算（微秒）：后台线程单次不超过 8ms，游戏无感知
static constexpr DWORD SCAN_BUDGET_US = 20000;  // v1.1.18-diag: 2000→20000 诊断提速（6.4GB堆首扫需5分钟太慢，正式版回调 2000）

// v1.1.7: 扫描/处理窗口——夜间狩猎无效，只白天工作（可热调）
static volatile int SCAN_HOUR_FROM = 7;
static volatile int SCAN_HOUR_TO   = 22;

// 不钻洞：扫描内存中 Lua 签名并补丁（一次性，Lua 按需加载故场景切换时重试）
// ============================================================
// v1.0.9: 不钻洞补丁——异步分块扫描状态机（原 v1.0.8 同步全扫替换）
// 同步全扫 6.4GB 私有堆 ~8s 卡顿不可接受；v1.20 chunk 懒加载使
// "场景切换时扫描"的时机经常早于 chunk 加载而 0 命中。
// 新机制：3ms/tick 增量推进 + 30s 节流重试；匹配时同时识别
// already-patched 形态（data.dat 文件级补丁加载后的内存状态）。
// ============================================================
static unsigned char* g_escAddr = nullptr;        // 扫描游标
static const unsigned char* g_escMax = nullptr;   // 扫描上限
static volatile LONG g_escActive = 0;
static ULONGLONG g_lastEscTry = 0;                 // 上次启动时间（30s 节流）
static int g_escPatched = 0, g_escAlready = 0, g_escWriteFail = 0;
static int g_escRegions = 0, g_escScanned = 0, g_escExcluded = 0;
static unsigned char* g_escBuf = nullptr;          // 1MB 常驻缓冲

static void EscapeScanStart() {
    if (!g_escBuf) {
        g_escBuf = static_cast<unsigned char*>(
            HeapAlloc(GetProcessHeap(), 0, 0x100000));
        if (!g_escBuf) return;
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_escAddr = static_cast<unsigned char*>(si.lpMinimumApplicationAddress);
    g_escMax = static_cast<const unsigned char*>(si.lpMaximumApplicationAddress);
    g_escPatched = 0; g_escAlready = 0; g_escWriteFail = 0;
    g_escRegions = 0; g_escScanned = 0; g_escExcluded = 0;
    g_escActive = 1;
}

static void EscapeScanFinish() {
    g_escActive = 0;
    if (g_escPatched + g_escAlready > 0)
        g_noEscapeDone.store(true, std::memory_order_release);
    if (g_escPatched > 0 || g_escAlready > 0) {
        Log("[ho] noescape done: patched=%d already=%d (regions=%d scanned=%d "
            "exclImg=%d writeFail=%d) [v1.0.9 async]\n",
            g_escPatched, g_escAlready, g_escRegions, g_escScanned,
            g_escExcluded, g_escWriteFail);
    } else {
        Log("[ho] noescape: 0 hits (regions=%d scanned=%d exclImg=%d; "
            "chunk not loaded, retry 30s) [v1.0.9 async]\n",
            g_escRegions, g_escScanned, g_escExcluded);
    }
}

// 单 tick 时间预算内推进；返回 true=本轮完成
static bool EscapeScanStep() {
    if (!g_escActive || !g_escBuf) return true;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    HANDLE proc = GetCurrentProcess();
    const uintptr_t bufAddr = reinterpret_cast<uintptr_t>(g_escBuf);
    constexpr SIZE_T BUF = 0x100000;

    int regionProbe = 0;
    while (g_escAddr < g_escMax) {
        if ((++regionProbe & 0xFF) == 0) {   // 外层节流探针：256 region 一查
            QueryPerformanceCounter(&t1);
            if ((double)(t1.QuadPart - t0.QuadPart) * 1000000.0 /
                (double)freq.QuadPart >= 3000.0)
                return false;
        }
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(g_escAddr, &mbi, sizeof(mbi)) == 0) {
            EscapeScanFinish();
            return true;
        }
        uintptr_t rs = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t re = rs + mbi.RegionSize;
        if (re <= reinterpret_cast<uintptr_t>(g_escAddr)) {
            EscapeScanFinish();
            return true;
        }
        g_escAddr = reinterpret_cast<unsigned char*>(re);
        if (!(mbi.State == MEM_COMMIT &&
              !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
              (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                              PAGE_EXECUTE_WRITECOPY))))
            continue;
        const uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if ((regionStart >= g_exeBase && regionStart < g_exeImageEnd) ||
            (g_selfImageBase != 0 && regionStart >= g_selfImageBase &&
             regionStart < g_selfImageEnd)) {
            ++g_escExcluded;
            continue;
        }
        if (bufAddr >= rs && bufAddr < re)
            continue;   // 跳过自身缓冲区
        ++g_escRegions;
        uintptr_t cur = rs;
        bool budgetOut = false;
        while (cur < re) {
            SIZE_T want = static_cast<SIZE_T>(re - cur);
            if (want > BUF) want = BUF;
            SIZE_T got = 0;
            if (ReadProcessMemory(proc, reinterpret_cast<void*>(cur),
                                  g_escBuf, want, &got) && got >= 20) {
                ++g_escScanned;
                unsigned char* buf = g_escBuf;
                for (SIZE_T off = 0; off + 20 <= got; off += 4) {
                    // escape1: 前缀[0..1]+后缀[3..4]匹配，替换[2]
                    if (memcmp(buf + off, kHuntOriginal, 8) == 0 &&
                        memcmp(buf + off + 12, kHuntOriginal + 3, 8) == 0) {
                        u32 v = 0; memcpy(&v, buf + off + 8, 4);
                        if (v == kHuntOriginal[2]) {
                            void* tgt = reinterpret_cast<void*>(cur + off + 8);
                            if (WritePatchChecked(tgt, &kHuntPatched[2], 4)) {
                                ++g_escPatched;
                                Log("[ho] noescape1 @ %p\n", tgt);
                            } else {
                                ++g_escWriteFail;
                                Log("[ho] noescape1 found @ %p but WRITE FAILED\n", tgt);
                            }
                        } else if (v == kHuntPatched[2]) {
                            ++g_escAlready;   // v1.0.9: 文件级补丁已生效
                        }
                    }
                    // escape2: 前缀[0..1]+后缀[3..6]匹配，替换[2]
                    if (off + 28 > got) continue;
                    if (memcmp(buf + off, kHuntEscape2Original, 8) == 0 &&
                        memcmp(buf + off + 12, kHuntEscape2Original + 3, 16) == 0) {
                        u32 v = 0; memcpy(&v, buf + off + 8, 4);
                        if (v == kHuntEscape2Original[2]) {
                            void* tgt = reinterpret_cast<void*>(cur + off + 8);
                            if (WritePatchChecked(tgt, &kHuntEscape2Patched[2], 4)) {
                                ++g_escPatched;
                                Log("[ho] noescape2 @ %p\n", tgt);
                            } else {
                                ++g_escWriteFail;
                                Log("[ho] noescape2 found @ %p but WRITE FAILED\n", tgt);
                            }
                        } else if (v == kHuntEscape2Patched[2]) {
                            ++g_escAlready;
                        }
                    }
                }
            }
            if (want == 0 || cur + want >= re) break;
            cur += want - 32;   // v1.0.9: overlap 32B（escape2 28B 签名跨界修正）
            QueryPerformanceCounter(&t1);
            if ((double)(t1.QuadPart - t0.QuadPart) * 1000000.0 /
                (double)freq.QuadPart >= 3000.0) {
                budgetOut = true;
                break;
            }
        }
        if (budgetOut) {
            g_escAddr = reinterpret_cast<unsigned char*>(cur);
            return false;
        }
    }
    EscapeScanFinish();
    return true;
}

// 启动一轮分块扫描
static bool ScanStep();   // v1.1.18-diag: 前向声明（ScanStart 尾部同步全扫调用）

static void ScanStart() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_scanAddr = static_cast<unsigned char*>(si.lpMinimumApplicationAddress);
    g_scanMax = static_cast<const unsigned char*>(si.lpMaximumApplicationAddress);
    g_scanActive = 1;
    g_scanRequested = 0;
    g_animalCount = 0;   // 清空旧列表（旧实例可能已失效）
    g_processedCount = 0;
    ++g_scanPass;
    if (!g_noEscapeDone.load(std::memory_order_acquire) && !g_escActive)
        EscapeScanStart();   // v1.0.9: 异步启动不钻洞扫描（分块推进，30s 节流重试见 HuntTick）
    Log("[ho] scan start (pass %d)\n", g_scanPass);
    // v1.1.18-diag: 同步扫完（诊断版一次性）——分块推进在 6.4GB 堆上过慢
    // （2ms/tick 首扫需数分钟）且用户久等 enumerated 恒 0 疑存在推进异常；
    // 同步全扫 ~8s（HuntLoop 后台线程阻塞，游戏主线程无感），一次到位。
    // 正式版恢复分块（回调预算并修复推进）。
    while (!ScanStep()) {}
}

// 执行扫描的一步：时间预算内继续扫，返回 true 表示全部完成
static bool ScanStep() {
    if (!g_scanActive) return false;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    int n = g_animalCount;

    while (g_scanAddr < g_scanMax && n < kMaxAnimals) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(g_scanAddr, &mbi, sizeof(mbi)) == 0) {
            g_scanActive = 0;
            break;
        }
        // 只扫已提交的可读写内存（对齐原版 scan_pool 条件）
        if (mbi.State == MEM_COMMIT && mbi.RegionSize > 64 &&
            (mbi.Protect & PAGE_GUARD) == 0 &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            static unsigned char buf[0x40000];   // M7: 256KB 栈缓冲→static，避免栈溢出
            // v1.1.17 fix: 恢复 region 内进度——原 pos=0 每次 ScanStep 重扫 region 头，
            // 大 region（主堆几百 MB）在 2ms/tick 预算下永远卡死在前几 MB
            // （VirtualQuery 从中间地址仍返回整个 region 起点+完整大小）
            // → g_enumerated 永不置位 → 列表恒空 → 一击必杀失效的根因。
            // 修复：pos 从 g_scanAddr 相对本 region 起点的偏移恢复，续扫不再回头。
            size_t pos = 0;
            unsigned char* regionBase = static_cast<unsigned char*>(mbi.BaseAddress);
            if (g_scanAddr > regionBase &&
                g_scanAddr < regionBase + mbi.RegionSize) {
                pos = static_cast<size_t>(g_scanAddr - regionBase);
                if (pos >= mbi.RegionSize) pos = 0;   // 防御：进度异常从头扫
            }
            while (pos < mbi.RegionSize && n < kMaxAnimals) {
                size_t chunk = mbi.RegionSize - pos;
                if (chunk > sizeof(buf)) chunk = sizeof(buf);
                if (!ReadMem(buf,
                        reinterpret_cast<unsigned char*>(mbi.BaseAddress) + pos, chunk))
                    break;
                for (size_t i = 0; i + 8 <= chunk; i += 8) {
                    u64 vt;
                    memcpy(&vt, buf + i, 8);
                    if (vt != g_variantHunt) continue;   // vtable 过滤
                    // +0x18 == 0（动态池标记）
                    if (i + 0x20 > chunk) {
                        u64 pool = ReadMemU64(reinterpret_cast<unsigned char*>(mbi.BaseAddress) + pos + i + 0x18);
                        if (pool != 0) continue;
                    } else {
                        u64 pool;
                        memcpy(&pool, buf + i + 0x18, 8);
                        if (pool != 0) continue;
                    }
                    // +0x10 高 32 位白名单（初始命中次数）
                    u64 initRaw;
                    if (i + 0x10 + 8 <= chunk) {
                        memcpy(&initRaw, buf + i + 0x10, 8);
                    } else {
                        initRaw = ReadMemU64(reinterpret_cast<unsigned char*>(mbi.BaseAddress) + pos + i + 0x10);
                    }
                    if (!IsWhitelisted(static_cast<u32>(initRaw >> 32))) continue;
                    // v1.0.5: +0x20 vt_sub 范围校验（真实动物的第二 vtable 必在
                    // exe 映像内；堆指针/垃圾值在此被拦下）
                    u64 subVt;
                    if (i + 0x20 + 8 <= chunk) {
                        memcpy(&subVt, buf + i + 0x20, 8);
                    } else {
                        subVt = ReadMemU64(reinterpret_cast<unsigned char*>(mbi.BaseAddress) + pos + i + 0x20);
                    }
                    if (subVt < g_exeBase || subVt >= g_exeImageEnd) continue;
                    // 通过全部过滤：收录实例
                    g_animalList[n++] =
                        reinterpret_cast<void*>(reinterpret_cast<unsigned char*>(mbi.BaseAddress) + pos + i);
                    if (n >= kMaxAnimals) break;
                }
                pos += chunk;

                // 时间预算检查：超时保存进度，下个 tick 继续
                QueryPerformanceCounter(&t1);
                double us = (double)(t1.QuadPart - t0.QuadPart) * 1000000.0 / (double)freq.QuadPart;
                if (us >= SCAN_BUDGET_US) {
                    g_scanAddr = reinterpret_cast<unsigned char*>(mbi.BaseAddress) + pos;
                    g_animalCount = n;
                    return false;   // 未完成，下次继续
                }
            }
        }
        if (mbi.RegionSize == 0) {
            g_scanActive = 0;
            break;
        }
        g_scanAddr = reinterpret_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
    }

    g_animalCount = n;
    g_processedCount = n;
    g_scanActive = 0;
    g_enumerated = true;
    Log("[ho] scan done: %d animal instances (pass %d)\n", n, g_scanPass);
    return true;
}

// ============================================================
// v1.1.11-diag: 候选实例诊断 dump（对照 BigL233 v0.2.0 discover_vtables）
// 现状：scan done 0 instances —— vt_hunt (0xE27A08=CGauge<int>) 或白名单
// 在 v1.20 漂移。此函数扫全内存，收集所有 "gauge 形态" 候选实例
//   (vptr 在 exe .rdata) && (+0x18==0) && (+0x20 在 exe 映像内)
// 按 (vtable_rva, init_hi32) 分组计数，输出 Top 组到日志。
// 一轮即可看到：动物实例的真实 vtable + 真实初始命中次数。
// ============================================================
struct DiagGroup {
    uintptr_t vtRva;
    u32 initHi;
    int count;
    u32 curSample;   // 第一个样本的 +0x8 当前值
};
static void DiagDumpCandidates() {
    // 读 PE section 表拿 .rdata 范围（vptr/vt_sub 应落在其中）
    uintptr_t rdataLo = 0, rdataHi = 0;
    {
        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exeBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            g_exeBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        const IMAGE_SECTION_HEADER* sc = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sc) {
            if (memcmp(sc->Name, ".rdata", 6) == 0) {
                rdataLo = g_exeBase + sc->VirtualAddress;
                rdataHi = rdataLo + sc->Misc.VirtualSize;
                break;
            }
        }
        if (!rdataLo) return;
    }
    static DiagGroup groups[64];
    int nGroups = 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    unsigned char* addr = static_cast<unsigned char*>(si.lpMinimumApplicationAddress);
    const unsigned char* maxAddr = static_cast<const unsigned char*>(si.lpMaximumApplicationAddress);
    int total = 0;
    LARGE_INTEGER freq, t0;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    while (addr < maxAddr) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_COMMIT && mbi.RegionSize > 64 &&
            (mbi.Protect & PAGE_GUARD) == 0 &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            static unsigned char buf[0x40000];
            size_t pos = 0;
            while (pos < mbi.RegionSize) {
                size_t chunk = mbi.RegionSize - pos;
                if (chunk > sizeof(buf)) chunk = sizeof(buf);
                if (!ReadMem(buf,
                        reinterpret_cast<unsigned char*>(mbi.BaseAddress) + pos, chunk))
                    break;
                for (size_t i = 0; i + 0x28 <= chunk; i += 8) {
                    u64 vptr, pool, sub, initRaw;
                    memcpy(&vptr, buf + i, 8);
                    if (vptr < rdataLo || vptr >= rdataHi) continue;   // vptr 必在 .rdata
                    memcpy(&pool, buf + i + 0x18, 8);
                    if (pool != 0) continue;                           // 动态池标记
                    memcpy(&sub, buf + i + 0x20, 8);
                    if (sub < g_exeBase || sub >= g_exeImageEnd) continue;  // vt_sub 在 exe 内
                    memcpy(&initRaw, buf + i + 0x10, 8);
                    u32 initHi = static_cast<u32>(initRaw >> 32);
                    u32 cur = 0;
                    memcpy(&cur, buf + i + 8, 4);
                    total++;
                    // 分组
                    int g = -1;
                    for (int k = 0; k < nGroups; ++k) {
                        if (groups[k].vtRva == (vptr - g_exeBase) &&
                            groups[k].initHi == initHi) { g = k; break; }
                    }
                    if (g < 0 && nGroups < 64) {
                        g = nGroups++;
                        groups[g].vtRva = vptr - g_exeBase;
                        groups[g].initHi = initHi;
                        groups[g].count = 0;
                        groups[g].curSample = cur;
                    }
                    if (g >= 0) groups[g].count++;
                }
                pos += chunk;
                // 诊断模式预算放宽到 50ms/块（一次性，可接受）
                LARGE_INTEGER t1;
                QueryPerformanceCounter(&t1);
                double us = (double)(t1.QuadPart - t0.QuadPart) * 1000000.0 / (double)freq.QuadPart;
                (void)us;   // 诊断一次扫完，不中断（HuntLoop 线程阻塞几秒可接受）
            }
        }
        if (mbi.RegionSize == 0) break;
        addr = reinterpret_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
    }
#if DIAG_HUNTONESHOT
    Log("[ho][diag] gauge 候选实例共 %d 个，分组 %d 组（vt_rva / init_hi / count / cur_sample）\n",
        total, nGroups);
#endif
    // 按计数排序输出前 40 组
    for (int pass = 0; pass < 40 && pass < nGroups; ++pass) {
        int best = -1;
        for (int k = 0; k < nGroups; ++k) {
            if (groups[k].count <= 0) continue;
            if (best < 0 || groups[k].count > groups[best].count) best = k;
        }
        if (best < 0) break;
#if DIAG_HUNTONESHOT
        Log("[ho][diag]   vt=0x%llX init=%u count=%d cur=%u\n",
            (unsigned long long)groups[best].vtRva, groups[best].initHi,
            groups[best].count, groups[best].curSample);
#endif
        groups[best].count = 0;
    }
}

// ============================================================
// v1.0.7: 场景指纹——读 root→mapOwner→mapInfo→spatialOwner 链末指针。
// 链断（标题画面/切换瞬间）返回 false 不更新指纹。
// ============================================================
static bool ReadSceneFingerprint(void** fp) {
    if (!fp) return false;
    *fp = nullptr;
    if (!g_exeBase) return false;
    void* root = nullptr;
    if (!ReadPtr(reinterpret_cast<void*>(g_exeBase + RVA_GAME_ROOT), &root) ||
        !root)
        return false;
    void* mapOwner = nullptr;
    if (!ReadPtr(reinterpret_cast<void*>(
            reinterpret_cast<unsigned char*>(root) + ROOT_MAP_OWNER),
            &mapOwner) || !mapOwner)
        return false;
    void* mapInfo = nullptr;
    if (!ReadPtr(reinterpret_cast<void*>(
            reinterpret_cast<unsigned char*>(mapOwner) + MAP_INFO_OFF),
            &mapInfo) || !mapInfo)
        return false;
    void* spatialOwner = nullptr;
    if (!ReadPtr(reinterpret_cast<void*>(
            reinterpret_cast<unsigned char*>(mapInfo) + MAP_SPATIAL_OWNER),
            &spatialOwner) || !spatialOwner)
        return false;
    *fp = spatialOwner;
    return true;
}

// ============================================================
// clock 循环
// ============================================================
static void HuntTick() {
    // v1.0.9: 不钻洞异步扫描推进（置于 clock 读链前：不依赖昼夜/时钟；
    // 3ms/tick 零卡顿，30s 节流重试直至 patch 完成）
    if (g_escActive) {
        EscapeScanStep();
    } else if (!g_noEscapeDone.load(std::memory_order_acquire)) {
        const ULONGLONG nowEsc = GetTickCount64();
        if (nowEsc - g_lastEscTry >= 30000) {
            g_lastEscTry = nowEsc;
            EscapeScanStart();
        }
    }
    // v1.0.1: 修正 clock 读链——base+0x208 是指针需解引用（原连续偏移错误）
    void* base = nullptr;
    {
        const int32_t* disp = reinterpret_cast<const int32_t*>(
            g_exeBase + RVA_CLOCK_LEA + 3);
        uintptr_t addr = g_exeBase + RVA_CLOCK_LEA + 7 + *disp;
        if (!ReadPtr(reinterpret_cast<void*>(addr), &base)) {
            Log("[ho] clock base FAIL\n");
            return;
        }
    }
    // save = *(base + 0x208)（解引用，与 ProductionAuto/AutoPet 一致）
    void* savePtr = nullptr;
    if (!ReadPtr(reinterpret_cast<void*>(
            reinterpret_cast<unsigned char*>(base) + CLOCK_SAVE_0x208), &savePtr)) {
        Log("[ho] save read FAIL (base=%p)\n", base);
        return;
    }
    unsigned char* save = reinterpret_cast<unsigned char*>(savePtr);
    u64 rawSecond = 0;
    __try {
        rawSecond = *reinterpret_cast<u64*>(save + SAVE_RAW_SECOND);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[ho] save read FAIL (save=%p)\n", (void*)save);
        return;
    }
    u64 day = rawSecond / 86400;
    // v1.1.7: 夜间狩猎无效——夜间不扫描不处理，省内存触碰开销
    // （小时 = raw/3600+7 与 SelfServiceStore 同式；raw 日界 = 显示 7 点，
    //   天亮后跨日逻辑自然触发重扫）
    int hour = (int)((rawSecond / 3600 + 7) % 24);
    if (hour < SCAN_HOUR_FROM || hour > SCAN_HOUR_TO) {
        g_scanActive = 0;
        g_scanRequested = 0;
        return;   // 夜间：保留已收集列表，其余全停
    }
    // v1.0.4: 跨日请求分块重扫；v1.1.6: 未枚举完成只静默补启动（不刷日志、
    // 不置 g_scanRequested——否则扫描完成后立刻多跑一整轮）
    if (day != g_lastDay) {
        g_lastDay = day;
        Log("[ho] day %lld rescan request\n", (long long)day);
        if (!g_scanActive) {
            ScanStart();
        } else {
            g_scanRequested = 1;   // 扫描中又跨日：完成后立即再扫
        }
    } else if (!g_enumerated && !g_scanActive) {
        ScanStart();   // 枚举未完成且扫描不在途（首进存档/扫描中断）：静默补启动
    }
    // 分块扫描进行中：每 tick 扫一步（时间预算内）
    if (g_scanActive) {
        if (ScanStep() && g_scanRequested) {
            ScanStart();   // 一轮完成且期间有新请求：立即开新一轮
        }
    }
    // v1.1.16-diag: 触发条件放宽为定时强制 dump——v1.1.15 的 count==0 条件
    // 在"扫描未完成/列表中途清零"等状态机边缘下从未触发（用户实测野生动物
    // 场景全程 processed=0 仍无 dump）。诊断版直接每 120s 收一次数（正式版
    // 恢复窄条件）；每 5s 打状态行观测 enumerated/scanActive/count 状态机。
#if DIAG_HUNTONESHOT
    // v1.1.16-diag: 触发条件放宽为定时强制 dump——v1.1.15 的 count==0 条件在
    // 扫描状态机边缘下从未触发。正式版（宏=0）整块编译排除。
    static ULONGLONG s_lastDiag = 0;
    {
        static ULONGLONG s_lastTickMs = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_lastTickMs > 5000) {   // 每 5s 评估一次
            s_lastTickMs = now;
            Log("[ho][diag] state enumerated=%d scanActive=%d count=%d clamp=%s addr=0x%llX\n",
                g_enumerated, g_scanActive, g_animalCount,
                g_clampEnabled ? "ON" : "OFF",
                (unsigned long long)(uintptr_t)g_scanAddr);
            if (g_enumerated &&
                (s_lastDiag == 0 || now - s_lastDiag > 120000)) {
                s_lastDiag = now;
                Log("[ho][diag] 定时诊断 dump（120s 限频）...\n");
                DiagDumpCandidates();
            }
        }
    }
#endif
    // v1.0.7: 场景切换触发重扫（替代 v1.0.5/v1.0.6 空场定时退避重扫）。
    // 指纹变化 = 进/出矿洞、跨区、进出建筑、矿洞换层，此时重扫一次；
    // 日常停留零后台扫描。首次记录不触发（进存档首轮由跨日逻辑扫）。
    {
        void* sceneFp = nullptr;
        if (ReadSceneFingerprint(&sceneFp)) {
            if (g_lastSceneFingerprint == nullptr) {
                g_lastSceneFingerprint = sceneFp;
            } else if (sceneFp != g_lastSceneFingerprint) {
                g_lastSceneFingerprint = sceneFp;
                Log("[ho] scene changed; rescan requested\n");
                if (!g_scanActive) {
                    ScanStart();
                } else {
                    g_scanRequested = 1;   // 扫描中切换场景：完成后立即再扫
                }
            }
        }
    }
    // v1.1.13: 热键检查已移至 HuntLoop 循环顶层——原在 HuntTick 末尾，
    //   被夜间 clock/save 读取失败的 early return（L738/746/754/764）阻塞，
    //   导致 F6 热键在夜间或存档未加载时全程不可达。
    // v1.1.14: toggle OFF 时主动恢复已 clamp 动物至原始 HP——
    //   旧版仅停止新 clamp，已 clamp 动物保持 10 HP，用户感知"关不掉"。
    // 遍历已收集实例并 clamp/restore（对齐原版 worker 循环）
    if (g_clampEnabled) {
        for (int i = 0; i < g_animalCount; ++i) {
            if (g_animalList[i]) ProcessInstance(g_animalList[i]);
        }
    } else {
        for (int i = 0; i < g_animalCount; ++i) {
            if (g_animalList[i]) RestoreInstance(g_animalList[i]);
        }
    }
}

static DWORD WINAPI HuntLoop(LPVOID) {
    Sleep(500);  // P1-6: 相位偏移 500ms，防多 MOD 同帧共振
    Log("[ho] loop started\n");
    int tick = 0;
    while (g_running) {
        static int s_bdgSlot = -1;  // P0 探针
        if (s_bdgSlot < 0) s_bdgSlot = qol::budget::Slot("HuntOneShot");
        uint64_t bdgT0 = qol::budget::NowUs();
        HotConfig_Poll();
        if (!QolGameBusy()) HuntTick();  // P1-2: 载入/菜单期间静默
        // v1.1.13: 热键检查移至循环顶层（原在 HuntTick 末尾 L829）
        // v1.1.14: 加 200ms 防抖（边沿检测已正确，防抖仅防御物理按键抖动）
        QolHotkeyCheckReload(&g_hotkeys);
        {
            WORD vk = QolHotKeysVk(&g_hotkeys, 0);
            bool keyDown = (vk != VK_UNDEFINED)
                ? ((GetAsyncKeyState(vk) & 0x8000) != 0)
                : ((GetAsyncKeyState(VK_F6) & 0x8000) != 0);  // 回退默认 F6
            if (keyDown && !g_clampTogglePrev) {
                ULONGLONG now = GetTickCount64();
                if (now - g_clampToggleLastTime >= 200) {
                    g_clampEnabled = !g_clampEnabled;
                    g_clampToggleTime = now;
                    g_clampToggleLastTime = now;
                    Log("[ho] clamp %s (hotkey)\n", g_clampEnabled ? "ENABLED" : "DISABLED");
                    // 2026-11-07: HOS 全文没有自建 HUD 窗口，功能正常但玩家看不到状态；
                    // 统一走 obs 通道补一条按键确认（功能逻辑零改动）。
                    qol::obs::EventF(1, 1800, L"[HuntOneShot] F6 \u4E00\u51FB\u5FC5\u6740\uFF1A%s",
                                     g_clampEnabled ? L"\u5F00" : L"\u5173");
                }
            }
            g_clampTogglePrev = keyDown;
        }
        if ((++tick % 50) == 0)
            Log("[ho] tick=%d clamped=%d processed=%d clamp=%s\n",
                tick, g_totalClamped, g_processedCount,
                g_clampEnabled ? "ON" : "OFF");
        qol::budget::Report(s_bdgSlot, qol::budget::NowUs() - bdgT0);
        Sleep(200);  // v1.1.21: 10Hz→5Hz（clamp 保持型，200ms 延迟感知无感，遍历开销减半）
    }
    return 0;
}

// ============================================================
// preflight：锚点字节验证
// ============================================================
static bool PreflightAndInit() {
    g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_exeBase) return false;

    // v1.0.5: exe 映像范围 [base, base+SizeOfImage)（vt_sub 范围校验用）
    {
        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exeBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            g_exeBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        g_exeImageEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;
    }

    const unsigned char* p = reinterpret_cast<const unsigned char*>(g_exeBase);

    // 1. vt_hunt lea 前缀（A0 02 00 00 4C 8D 83 B0 02 00 00 4C 89 44 24 30 48 8D 05）
    static const unsigned char kLeaPre[11] = {
        0xA0, 0x02, 0x00, 0x00, 0x4C, 0x8D, 0x83, 0xB0, 0x02, 0x00, 0x00};
    if (memcmp(p + RVA_VT_HUNT_LEA, kLeaPre, 11) != 0) {
        Log("[ho] FAIL: vt_hunt lea prefix mismatch\n");
        return false;
    }
    // 2. lea 目标验证（+0x10 处 48 8D 05 disp32 -> RVA_VT_HUNT）
    if (p[RVA_VT_HUNT_LEA + 0x10] != 0x48 || p[RVA_VT_HUNT_LEA + 0x11] != 0x8D ||
        p[RVA_VT_HUNT_LEA + 0x12] != 0x05) {
        Log("[ho] FAIL: vt_hunt lea opcode mismatch\n");
        return false;
    }
    {
        const int32_t* disp = reinterpret_cast<const int32_t*>(
            p + RVA_VT_HUNT_LEA + 0x13);
        uintptr_t target = g_exeBase + RVA_VT_HUNT_LEA + 0x17 + *disp;
        if ((target - g_exeBase) != RVA_VT_HUNT) {
            Log("[ho] FAIL: vt_hunt target mismatch (got %p)\n",
                reinterpret_cast<void*>(target - g_exeBase));
            return false;
        }
    }
    // 3. clock lea（48 8B 05 disp32 @ RVA_CLOCK_LEA）
    if (p[RVA_CLOCK_LEA] != 0x48 || p[RVA_CLOCK_LEA + 1] != 0x8B ||
        p[RVA_CLOCK_LEA + 2] != 0x05) {
        Log("[ho] FAIL: clock lea mismatch\n");
        return false;
    }
    // 设置 vtable 绝对地址（原版 g_ctx.base + g_ctx.vft_hunt）
    g_variantHunt = static_cast<u64>(g_exeBase) + RVA_VT_HUNT;
    Log("[ho] preflight OK; vt_hunt=%016llX (base=%p + RVA %08X)\n",
        (unsigned long long)g_variantHunt, reinterpret_cast<void*>(g_exeBase),
        (unsigned)RVA_VT_HUNT);
    // 不在 DllMain 中调用 EscapeScanStart——Lua 脚本按场景加载，
    // 启动时几乎必空扫。交给 tick 的 30s 节流重试与 ScanStart 触发（v1.0.9 异步）。
    return true;
}

// ============================================================
// DLL 入口
// ============================================================
// ============================================================
// QoL 快捷菜单导出（宿主：ModManager F2 / F1）
//   F6「一击必杀」开关，复用 g_clampEnabled（与热键同义）
// ============================================================
static bool IsHuntOneShotOn() {
    return g_clampEnabled != 0;
}
static void ToggleClampEnabled() {
    g_clampEnabled = g_clampEnabled ? 0 : 1;
    Log("[ho] clamp %s (quickmenu)\n", g_clampEnabled ? "ENABLED" : "DISABLED");
}
extern "C" __declspec(dllexport) int QolQuickMenuItems(
        QolQuickMenuItem* items, int maxItems) {
    int n = 1;  // 一击必杀
    if (items && maxItems >= 1)
        items[0] = { "一击必杀", IsHuntOneShotOn, ToggleClampEnabled };
    return n;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // v1.0.8: 记录自身映像范围（noescape 扫描排除用）
        {
            const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(
                reinterpret_cast<uintptr_t>(hModule));
            if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                    reinterpret_cast<uintptr_t>(hModule) + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE) {
                    g_selfImageBase = reinterpret_cast<uintptr_t>(hModule);
                    g_selfImageEnd = g_selfImageBase + nt->OptionalHeader.SizeOfImage;
                }
            }
        }
        LogOpen("huntoneshot");
        qol::obs::Init(nullptr);  // v1.6.x: 统一观测层接入
        qol::obs::SetInt("huntoneshot", "ready", 1);
        qol::obs::EventF(1, 2500, L"[HuntOneShot] 已加载：观测层接入");
    // v-local：游戏版本闸门（village.exe 非 v1.20 build 25311578 则不启用）
    if (!qol::ExeBuildSupported()) {
        Log("[HuntOneShot] 游戏版本不匹配（非 v1.20 build 25311578）→ 本 MOD 不启用\n");
        return TRUE;
    }
        HotConfig_Register("huntoneshot", (void*)&RVA_VT_HUNT_LEA,      "RVA_VT_HUNT_LEA",      HOT_RVA, 0xE9D35);
        HotConfig_Register("huntoneshot", (void*)&RVA_CLOCK_LEA,        "RVA_CLOCK_LEA",        HOT_RVA, 0xE2EA3);
        HotConfig_Register("huntoneshot", (void*)&RVA_VT_HUNT,          "RVA_VT_HUNT",          HOT_RVA, 0xE422D8);
        HotConfig_Register("huntoneshot", (void*)&CLOCK_SAVE_0x208,     "CLOCK_SAVE_0x208",     HOT_RVA, 0x208);
        HotConfig_Register("huntoneshot", (void*)&SAVE_RAW_SECOND,      "SAVE_RAW_SECOND",      HOT_RVA, 0x3270);
        HotConfig_Register("huntoneshot", (void*)&RVA_GAME_ROOT,        "RVA_GAME_ROOT",        HOT_RVA, 0x111cc90);
        HotConfig_Register("huntoneshot", (void*)&ROOT_MAP_OWNER,      "ROOT_MAP_OWNER",       HOT_RVA, 0x268);
        HotConfig_Register("huntoneshot", (void*)&MAP_INFO_OFF,        "MAP_INFO_OFF",         HOT_RVA, 0x0d8);
        HotConfig_Register("huntoneshot", (void*)&MAP_SPATIAL_OWNER,   "MAP_SPATIAL_OWNER",    HOT_RVA, 0x030);
        HotConfig_Register("huntoneshot", (void*)&INST_VTABLE,         "INST_VTABLE",          HOT_RVA, 0x00);
        HotConfig_Register("huntoneshot", (void*)&INST_CURRENT,        "INST_CURRENT",         HOT_RVA, 0x08);
        HotConfig_Register("huntoneshot", (void*)&INST_INIT,           "INST_INIT",            HOT_RVA, 0x10);
        HotConfig_Register("huntoneshot", (void*)&INST_POOL_MARK,      "INST_POOL_MARK",       HOT_RVA, 0x18);
        HotConfig_Register("huntoneshot", (void*)&INST_VT_SUB,         "INST_VT_SUB",          HOT_RVA, 0x20);
        HotConfig_Register("huntoneshot", (void*)&SCAN_HOUR_FROM,     "SCAN_HOUR_FROM",       HOT_INT, 7);
        HotConfig_Register("huntoneshot", (void*)&SCAN_HOUR_TO,       "SCAN_HOUR_TO",         HOT_INT, 22);
        HotConfig_Poll();
        HotConfig_DumpCE("huntoneshot");
        // v1.1.11: 热键改键注册
        QolHotKeysInit(&g_hotkeys, "huntoneshot");
        QolHotKeysSetDefault(&g_hotkeys, "F6");
        QolRegisterHotKey("huntoneshot", "F6");
        if (!PreflightAndInit()) {
            Log("[ho] preflight failed, abort\n");
            // 2026-10-28：abort 路径禁用 LogClose（同 MineHelper/BirthdayReminder 修复）——
            // DllMain(ATTACH) 持加载器锁时拆除日志后端会引发 ~2s 后 write@0x24 进程崩溃。
            LogFlush();
            return TRUE;
        }
        g_running = 1;
        g_huntThread = CreateThread(nullptr, 0, HuntLoop, nullptr, 0, nullptr);
        Log("[ho] HuntOneShot v1.1.15-diag loaded; base=%p\n", reinterpret_cast<void*>(g_exeBase));
    } else if (reason == DLL_PROCESS_DETACH) {
        qol::obs::Shutdown();  // v1.6.x: 解观测层映射（仅本 DLL 视图）
        g_running = 0;
        // H5: 等待 HuntLoop 线程退出（最多 5s），防止 FreeLibrary 后线程仍访问已释放内存
        if (g_huntThread) {
            WaitForSingleObject(g_huntThread, 5000);
            CloseHandle(g_huntThread);
            g_huntThread = nullptr;
        }
        // M6: 释放 1MB 常驻扫描缓冲，修复内存泄漏
        if (g_escBuf) {
            HeapFree(GetProcessHeap(), 0, g_escBuf);
            g_escBuf = nullptr;
        }
        Log("[ho] unloaded; clamped=%d\n", g_totalClamped);
        LogClose();
    }
    return TRUE;
}
