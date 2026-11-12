// selfservice.cpp -- SelfServiceStore v1.3.0 (v1.30 / build 25640656)
//   对外口径：延长开店时间（不再承诺「24 小时营业」）。
//   v1.3.0 把读取侧 hook 迁到 v1.30（getter 0x290DC0 / Lua 绑定内
//   callsite 0x49BCA5），并把深夜强制关店窗口默认改为恒不成立（24..23），
//   写入侧对 shopOpen 写 1。效果是把商店可进时段延长到脚本允许的范围；
//   FN-019 已定性：深夜 00:00~05:59 的门禁由脚本自算小时，状态键与
//   getter 都绕不过，故不宣称「全天可进」。热参可随时回退。
// selfservice.cpp -- SelfServiceStore v1.2.5 (v1.30 / build 25640656)
//   v1.2.5 修复「牛棚、鸡棚门口常开」：写入侧 hook 的 0x290BE0 是全游戏
//   通用 MapSetAreaStatusValue(areaId, key, value)，跳板只按 key 判定、
//   不校验 areaId。data.dat 实证只有 shopOpen 是商店专用状态键；
//   Open / doorOpen / shutOpen 是跨建筑通用键（门 / 卷帘窗 / 畜棚），
//   旧白名单把它们一并强制成 1 → 游戏每次关门写 0 都被改成写 1 → 门常开。
//   现收窄为仅 shopOpen，并留 LEGACY_ALL_KEYS 热参应急回退。
// ---- 以下为历史版本说明 ----
// selfservice.cpp -- SelfServiceStore v1.2.0 正式版 (v1.20 / build 25311578)
//   v1.2.0：**读取侧 hook 上线**——拦绑定 0x485660 内 call 0x288550 的站点，
//   商店键（Open/shopOpen/shutOpen/doorOpen）返回值强制为 1 → 脚本读到
//   “营业中” → 全天可进（含原版 README 自述失效的 00:00~05:59 时段）。
//   不改 data.dat、不碰存档；写入侧 hook（0x288370）保留为二级保险。
//
// v1.1.7（2026-10-17）：加钩子活性心跳（heartbeat hookCalls/shopHits/hour）
//         ——实测钩子确实在调用（开关写入拦截生效），但玩家仍进不去店。
// v1.1.8-diag（同上）：日志加 caller=0x…（栈扫描猜测的游戏侧调用者 RVA）
//         ——据此反汇编“是谁在写这些开关”，定位真正的营业/门禁判定。
// v1.1.9-diag（同上）：caller 改为「返址形态校验」（E8/FF-call）——
//         v1.1.8 的粗版实测全部返回 0x1000（栈上无关常量，无效）
// v1.1.2（2026-09-30 实测反馈）：门开了但进不去——入口判定疑似走其他
//         键/查询路径。新增全键名限频诊断（ALL_DIAG_LIMIT），下轮采集
//         玩家靠近入口时到底查询了哪些键，再决定补 force 哪个键/函数。
// v1.1.1：营业窗口 7-22 → 0-23（全天开启，用户需求①“不设 7-22 时段限制”，
//         v1.1.0 适配时误沿用大佬原版默认窗口，实测存档午夜 pass(orig) 现形）
// Ported from gloaming's SelfServiceStore v0.1.8 (C/zig), rewritten for QoL DevKit.
//
// v1.20 addresses (confirmed by signature search + runtime log):
//   Hook target: RVA 0x288370 (48-byte addrsig unique match, prologue 14B verified)
//   Clock LEA:   RVA 0xE2EA3 (mov rax,[rip+disp] -> GAME_ROOT 0x10FCBB0)
//   Clock chain: [GAME_ROOT] -> +0x208 -> save -> +0x3270 (QWORD time)
//
// Hook mechanism:
//   12-byte absolute jump (mov rax,imm64; jmp rax) at function entry
//   Stub replays 12 bytes, saves params, calls KeyIsShop(r8),
//   forces r9d=1 if shop key + business hours, restores params, jmp back
//   Back address = hook + 12 (push r12 and beyond intact)
//
// 作者：PHJ&消失的清风

#include <windows.h>
#include "obs.h"              // v1.6.x: 统一观测层（AI 可读寄存器 + toast）
#include <cstdint>
#include <cstring>
#include "logging.h"
#include "exe_guard.h"   // v-local: 统一游戏版本闸门
#include "budget.h"        // P0 帧耗时探针（跨 DLL 共享）
#include "state.h"         // P1-2 状态感知暂停（载入/菜单静默）
#include "hot_config.h"

// ---- 诊断日志开关（发布版注释掉）----
// v1.1.6-diag（2026-10-17）：临时开启——用户反馈夜间无法进店购买，
// 需全键名诊断（ALL_DIAG_LIMIT 扩到 400）定位购买判定走哪个键/函数。
// 定位后转正时重新注释。
// #define STORE_LOGGING  // 实测调试开启（诊断后回退）

// 日志运行时开关（2026-10-24）：编译期宏退役，改由 QoL_Shared 运行时门控。
// 开启方式：游戏根 qol_log.cfg 写 level=1（约 1s 生效，免重编免重启）；缺省/level=0 为静默。

using u64 = std::uint64_t;

// ============================================================
// 常量（HotConfig 可热调）
// ============================================================
static volatile uintptr_t RVA_HOOK       = 0x290be0;
static volatile uintptr_t RVA_CLOCK_LEA  = 0xE2EA3;
static volatile uintptr_t CLOCK_SAVE_OFF = 0x208;
static volatile uintptr_t TIME_B_OFF     = 0x3270;
static constexpr int DISPLAY_OFFSET     = 7;   // hour = (b/3600 + 7) % 24
static volatile int OPEN_HOUR_FROM      = 0;    // v1.1.1：全天开启（用户需求）
static volatile int OPEN_HOUR_TO        = 23;   // v1.1.1：同上
// v1.3.0：默认延长营业。24..23 是恒不成立的窗口 → closeWindow 恒 false，
//   写入侧与读取侧都不再主动关店。深夜门禁在脚本侧（FN-019），本 MOD
//   无法绕过；若需恢复深夜降级，用热参改回 0 / 5。
static volatile int CLOSE_HOUR_FROM     = 24;
static volatile int CLOSE_HOUR_TO       = 23;

// v1.2.5：应急回退开关。默认 0 = 严格模式（只拦 shopOpen，不误伤其它建筑）。
//   设为 1 恢复旧 4 键行为，仅用于对照实验，正常游玩请勿开启。
static volatile int LEGACY_ALL_KEYS      = 0;
static volatile int g_enabled           = 1;

static constexpr int PATCH_LEN    = 12;  // mov rax,imm64(10) + jmp rax(2)
static constexpr int PROLOGUE_LEN = 14;  // verify 14 bytes

// 入口序言（14 字节，安装前校验）
static const unsigned char kPrologue[14] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,  // mov [rsp+8],rbx
    0x48, 0x89, 0x6C, 0x24, 0x20,  // mov [rsp+20],rbp
    0x56,                            // push rsi
    0x57,                            // push rdi
    0x41, 0x54                       // push r12
};

static constexpr int DIAG_LIMIT = 20;
static constexpr int ALL_DIAG_LIMIT = 400;  // v1.1.6-diag: 60→400（覆盖夜间完整购物流程）

// ============================================================
// 运行时状态
// ============================================================
static uintptr_t g_exeBase  = 0;
static uintptr_t g_hookAddr = 0;
static uintptr_t g_backAddr = 0;
static void* g_stub = nullptr;

// ============================================================
// v1.3.0 读取侧 hook（v1.30 重新定位，用于延长开店时间）
//   dsh 实证：Lua 命令 MapGetAreaStatusValue 的绑定函数 FUN_14049bb80
//   在 RVA 0x49BCA5 处 E8 call 内层读取 FUN_140290dc0（序言与 v1.20
//   0x288550 逐字节相同、size 均 469）；setter 0x290BE0→getter 0x290DC0
//   差值 0x1E0 也与 v1.20 的 0x288370→0x288550 一致（整块平移 0x8870）。
//   把该 call 换成 GetterStub：先调原函数，键名 shopOpen 则强制返回 1
//   → 脚本读到「营业中」→ 在脚本允许时段内可进店。写入侧 hook 保留为二级保险。
//   安全：只改这 5 字节调用点（E8 校验 + 目标校验 + 可逆），不动存档。
// ============================================================
static volatile uintptr_t RVA_GETTER       = 0x290dc0;   // v1.3.0: v1.30 MapGetAreaStatusValue 实现
static volatile uintptr_t RVA_GET_CALLSITE = 0x49bca5;   // v1.3.0: Lua 绑定 FUN_14049bb80 内 E8 call
static unsigned char  g_origGetCall[5] = {};
static unsigned char* g_getStubPage = nullptr;
static bool           g_getHooked = false;
static volatile LONG  g_getForceCount = 0;
static constexpr LONG GET_FORCE_LOG_LIMIT = 24;
static bool g_ready = false;
static unsigned char g_origBytes[PATCH_LEN] = {};
static volatile LONG g_diagCount = 0;
static volatile LONG g_allDiagCount = 0;    // v1.1.2: 全键名计数
static volatile LONG g_hookCalls = 0;      // v1.1.7-diag: 钩子调用总计数（活性心跳）

// ============================================================
// 内存安全读取
// ============================================================
static bool ReadPtr(void* p, void** out) {
    if (!p) return false;
    __try { *out = *(void* volatile*)p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool ReadU64(const void* p, u64* out) {
    if (!p) return false;
    __try { *out = *(u64 volatile*)p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// M14: 内存可读性检查——VirtualQuery 确认地址已提交且可读，
// 在 memcpy 备份/probe 前调用，防止 RVA_HOOK 热调到非法地址导致崩溃。
static bool IsReadable(const void* addr, size_t size) {
    if (!addr || size == 0) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_NOACCESS) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end = start + size;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    if (end > regionEnd) return false;
    return true;
}

// ============================================================
// 时钟读取（CLOCK_LEA -> GAME_ROOT -> +0x208 -> save -> +0x3270）
// ============================================================
static void* GetSave() {
    if (!g_exeBase) return nullptr;
    const unsigned char* lea = (const unsigned char*)(g_exeBase + RVA_CLOCK_LEA);
    if (lea[0] != 0x48 || lea[1] != 0x8B || lea[2] != 0x05) return nullptr;
    int32_t disp = *(const int32_t*)(lea + 3);
    uintptr_t clockGlobal = g_exeBase + RVA_CLOCK_LEA + 7 + disp;
    void* clock = nullptr;
    if (!ReadPtr((void*)clockGlobal, &clock) || !clock) return nullptr;
    void* save = nullptr;
    if (!ReadPtr((char*)clock + CLOCK_SAVE_OFF, &save) || !save) return nullptr;
    return save;
}

static int GetHour() {
    void* save = GetSave();
    if (!save) return -1;
    u64 b = 0;
    if (!ReadU64((const char*)save + TIME_B_OFF, &b)) return -1;
    return (int)((b / 3600 + DISPLAY_OFFSET) % 24);
}

// v1.1.9-diag: 精确定位游戏侧调用者——栈扫描 + **返址形态校验**：
//   v1.1.8 的"首个落在 .text 的值"太粗（实测全部返回 0x1000 = 栈上无关常量，
//   我自己会话即被误导）。本版对每个命中 .text 的栈槽 v，检查 v-2..v-7 处是否
//   是 call 指令结尾（E8 rel32 / FF /2 间接调用）→ 才是真实返址。
static bool LooksLikeCallReturn(uintptr_t v) {
    for (int back = 2; back <= 7; ++back) {
        const unsigned char* s = reinterpret_cast<const unsigned char*>(v - back);
        __try {
            if (s[0] == 0xE8) return true;                 // call rel32
            if (s[0] == 0xFF) {
                const unsigned char m = s[1];
                if (m == 0x15 || m == 0x25) return true;   // call [rip+disp32]
                if (m >= 0x10 && m <= 0x17) return true;   // call [reg]
                if (m >= 0x50 && m <= 0x57) return true;   // call [reg+disp8]
                if (m >= 0x90 && m <= 0x97) return true;   // call [reg+disp32]
            }
            if (s[0] == 0x41 && s[1] == 0xFF) return true;  // REX.B 前缀 call
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    return false;
}

static unsigned long long GuessGameCaller() {
    if (!g_exeBase) return 0;
    const uintptr_t lo = g_exeBase + 0x1000;      // village.exe .text 起
    const uintptr_t hi = g_exeBase + 0xA12567;    // .text 尾
    uintptr_t anchor = reinterpret_cast<uintptr_t>(&anchor);
    for (int i = 0; i < 512; ++i) {
        uintptr_t v = 0;
        __try { v = *reinterpret_cast<uintptr_t*>(anchor + i * 8); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        if (v < lo || v >= hi) continue;
        if (LooksLikeCallReturn(v)) return (unsigned long long)(v - g_exeBase);
    }
    return 0;
}

// ============================================================
// v1.2.5 共用键名判定：写入侧与读取侧必须走同一份白名单，避免再次分叉。
//   shopOpen —— 仅商店脚本使用（CMD_INTERACTION_GENERALSTORE /
//               seqUpdate_Shop / shop_Closed），data.dat 共 14 处。
//   Open     —— 通用门状态键（建筑工棚 MapGetAreaStatusValue .. Open）。
//   doorOpen —— 19 项连续通用门键表，data.dat 共 137 处，跨牛棚/鸡棚等。
//   shutOpen —— MAP_ID_CONSTRUCTION_SHED_01 卷帘窗（SEQ_OPEN_SHUTTER）。
// ============================================================
static bool IsShopKeyName(const char* buf) {
    if (strcmp(buf, "shopOpen") == 0) return true;
    if (!LEGACY_ALL_KEYS) return false;
    return (strcmp(buf, "Open") == 0 || strcmp(buf, "doorOpen") == 0 ||
            strcmp(buf, "shutOpen") == 0);
}

// ============================================================
// 商店键判定（从跳板调用，必须遵循 x64 调用约定）
// r8 = 键名指针，检查是否商店类且处于营业时段
// 返回 1 = 强制 r9d=1，返回 0 = 放行原值
// ============================================================
static int KeyIsShop(const char* k) {
    char buf[9];
    int n, shop = 0, hour;

    InterlockedIncrement(&g_hookCalls);   // v1.1.7-diag: 钩子活性计数

    if ((uintptr_t)k < 0x10000) return 0;
    __try {
        for (n = 0; n < 8; n++) {
            buf[n] = k[n];
            if (buf[n] == 0) break;
        }
        buf[n] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    // v1.2.5：只认商店专用键，避免误伤牛棚/鸡棚/工棚的通用门键
    shop = IsShopKeyName(buf) ? 1 : 0;
    if (!shop) {
        // v1.1.2: 全键名限频诊断——找出“入口”判定实际查询的键
        if (InterlockedIncrement(&g_allDiagCount) <= ALL_DIAG_LIMIT)
            Log("[store] key='%s' hour=%d caller=0x%llX (non-shop, no force)",
                buf, GetHour(), GuessGameCaller());
        return 0;
    }

    hour = GetHour();
    if (hour < 0) return 0;  // 读不到时间：放行不干预

    // v1.3.0：默认 CLOSE_HOUR 24..23（恒不成立）→ 全天写 1。
    //   若实机遇到「门开却进不去」，用热参把窗口设回 0..5 恢复降级行为。
    const bool closeWindow = (hour >= CLOSE_HOUR_FROM && hour <= CLOSE_HOUR_TO);
    const bool openWindow  = (hour >= OPEN_HOUR_FROM && hour <= OPEN_HOUR_TO);

    if (InterlockedIncrement(&g_diagCount) <= DIAG_LIMIT) {
        Log("[store] diag key='%s' hour=%d caller=0x%llX -> %s", buf, hour,
            GuessGameCaller(),
            closeWindow ? "force-closed(0)"
                        : (openWindow ? "open(1)" : "pass(orig)"));
    }

    if (closeWindow) return 2;   // 写 0：关店
    if (openWindow) return 1;    // 写 1：营业
    return 0;
}

// ============================================================
// 跳板构建：重放 12B 序言 -> 保存参数 -> 调用 KeyIsShop ->
//           营业时段强制 [rsp]=1 -> 恢复参数 -> 跳回原函数+12
// ============================================================
static bool BuildStub() {
    g_stub = VirtualAlloc(nullptr, 0x200, MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE);
    if (!g_stub) return false;

    unsigned char* m = (unsigned char*)g_stub;
    size_t o = 0;

    // 1. 重放被覆盖的 12 字节序言
    m[o++] = 0x48; m[o++] = 0x89; m[o++] = 0x5C; m[o++] = 0x24; m[o++] = 0x08; // mov [rsp+8],rbx
    m[o++] = 0x48; m[o++] = 0x89; m[o++] = 0x6C; m[o++] = 0x24; m[o++] = 0x20; // mov [rsp+20],rbp
    m[o++] = 0x56;  // push rsi
    m[o++] = 0x57;  // push rdi

    // 2. 保存 4 个参数寄存器
    m[o++] = 0x51;              // push rcx
    m[o++] = 0x52;              // push rdx
    m[o++] = 0x41; m[o++] = 0x50;  // push r8
    m[o++] = 0x41; m[o++] = 0x51;  // push r9

    // 3. 调用 KeyIsShop(r8=键名)
    m[o++] = 0x49; m[o++] = 0x8B; m[o++] = 0xC8;  // mov rcx, r8
    m[o++] = 0x48; m[o++] = 0x83; m[o++] = 0xEC; m[o++] = 0x28;  // sub rsp,0x28
    m[o++] = 0x48; m[o++] = 0xB8;  // mov rax, KeyIsShop
    size_t fnpos = o; o += 8;
    m[o++] = 0xFF; m[o++] = 0xD0;  // call rax
    m[o++] = 0x48; m[o++] = 0x83; m[o++] = 0xC4; m[o++] = 0x28;  // add rsp,0x28
    m[o++] = 0x85; m[o++] = 0xC0;  // test eax,eax
    // v1.2.3：返回 1 = 写 1（营业），返回 2 = 写 0（深夜关店）
    m[o++] = 0x74; m[o++] = 0x15;  // je +0x15（跳过两段写入）
    m[o++] = 0x83; m[o++] = 0xF8; m[o++] = 0x02;  // cmp eax,2
    m[o++] = 0x74; m[o++] = 0x09;  // je +9（跳到写 0）
    m[o++] = 0xC7; m[o++] = 0x04; m[o++] = 0x24;
    m[o++] = 0x01; m[o++] = 0x00; m[o++] = 0x00; m[o++] = 0x00;  // mov dword [rsp],1
    m[o++] = 0xEB; m[o++] = 0x07;  // jmp +7（跳过写 0）
    m[o++] = 0xC7; m[o++] = 0x04; m[o++] = 0x24;
    m[o++] = 0x00; m[o++] = 0x00; m[o++] = 0x00; m[o++] = 0x00;  // mov dword [rsp],0

    // 4. 恢复参数寄存器
    m[o++] = 0x41; m[o++] = 0x59;  // pop r9
    m[o++] = 0x41; m[o++] = 0x58;  // pop r8
    m[o++] = 0x5A;                  // pop rdx
    m[o++] = 0x59;                  // pop rcx

    // 5. 跳回原函数 +12
    m[o++] = 0x48; m[o++] = 0xB8;  // mov rax, imm64
    size_t backpos = o; o += 8;
    m[o++] = 0xFF; m[o++] = 0xE0;  // jmp rax

    // 填入地址
    u64 fn = (u64)(uintptr_t)&KeyIsShop;
    memcpy(m + fnpos, &fn, 8);
    u64 back = (u64)g_backAddr;
    memcpy(m + backpos, &back, 8);

    FlushInstructionCache(GetCurrentProcess(), m, o);
    return true;
}

// ============================================================
// Hook 安装
// ============================================================
// v1.2.0: 纯键名分类（不做时段判断——读取侧要的就是“全天视为营业”）
static int KeyIsShopName(const char* k) {
    if (reinterpret_cast<uintptr_t>(k) < 0x10000) return 0;
    char buf[9] = {};
    int n = 0;
    __try {
        for (n = 0; n < 8; ++n) {
            buf[n] = k[n];
            if (!buf[n]) break;
        }
        buf[n] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return IsShopKeyName(buf) ? 1 : 0;
}

typedef int (*GetterFn)(void*, int, const char*);

// 存根：替调用点被 call（rcx=this, rdx=idx, r8=name → eax=值）
static int __declspec(noinline) GetterStub(void* self, int idx,
                                           const char* name) {
    static GetterFn real = nullptr;
    if (!real) real = reinterpret_cast<GetterFn>(g_exeBase + RVA_GETTER);
    const int ret = real ? real(self, idx, name) : 0;
    if (KeyIsShopName(name)) {
        // v1.3.0：读取侧与写入侧一致——默认全天返回 1（营业中）；
        //   仅当热参把 CLOSE_HOUR 窗口设回有效区间时才返回 0。
        const int hour = GetHour();
        int forced = 1;
        if (hour >= 0 && hour >= CLOSE_HOUR_FROM && hour <= CLOSE_HOUR_TO)
            forced = 0;
        if (InterlockedIncrement(&g_getForceCount) <= GET_FORCE_LOG_LIMIT)
            Log("[store] getter force: key='%.8s' orig=%d -> %d (hour=%d)",
                name, ret, forced, hour);
        return forced;
    }
    return ret;
}

static bool InstallGetterHook() {
    if (g_getHooked) return true;
    if (!g_exeBase) return false;
    unsigned char* site = reinterpret_cast<unsigned char*>(
        g_exeBase + RVA_GET_CALLSITE);
    __try {
        if (site[0] != 0xE8) {
            Log("[store] getter site 0x%llX not call (byte=%02X), skip",
                static_cast<unsigned long long>(RVA_GET_CALLSITE), site[0]);
            return false;
        }
        const int32_t rel = *reinterpret_cast<int32_t*>(site + 1);
        const uintptr_t target = reinterpret_cast<uintptr_t>(site) + 5 + rel;
        if (target != g_exeBase + RVA_GETTER) {
            Log("[store] getter site target 0x%llX != 0x%llX, skip",
                static_cast<unsigned long long>(target - g_exeBase),
                static_cast<unsigned long long>(RVA_GETTER));
            return false;
        }
        memcpy(g_origGetCall, site, 5);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    // v1.2.2: 目标地址选择——**优先直接跳进 DLL 里的 GetterStub**
    //   （v1.2.0 实测：从调用点向上 64 次试探分配全部落到 ±2GB 外而放弃；
    //    DLL 与 exe 通常同在 ±2GB → 直接跳最稳且不需额外可执行页）
    unsigned char* target = reinterpret_cast<unsigned char*>(&GetterStub);
    const int64_t kRelLimit = 0x70000000LL;   // 保守留余量（E8 极限 ±2GB）
    const int64_t dist0 = static_cast<int64_t>(
        reinterpret_cast<uintptr_t>(target) -
        (reinterpret_cast<uintptr_t>(site) + 5));
    bool useStubPage = false;
    if (dist0 <= -kRelLimit || dist0 >= kRelLimit) {
        // 太远 → 双向（上/下）1MB 步进试探分配存根页
        const uintptr_t want = reinterpret_cast<uintptr_t>(site);
        for (int i = 1; i <= 128 && !g_getStubPage; ++i) {
            const int64_t step = static_cast<int64_t>(i) * 0x100000LL;
            uintptr_t hints[2] = {
                static_cast<uintptr_t>(static_cast<int64_t>(want) + step),
                static_cast<uintptr_t>(static_cast<int64_t>(want) - step)};
            for (int h = 0; h < 2 && !g_getStubPage; ++h) {
                void* p = VirtualAlloc(reinterpret_cast<void*>(hints[h]), 0x1000,
                                       MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
                if (!p) continue;
                const int64_t d = static_cast<int64_t>(
                    reinterpret_cast<uintptr_t>(p)) -
                    static_cast<int64_t>(reinterpret_cast<uintptr_t>(site) + 5);
                if (d > -kRelLimit && d < kRelLimit)
                    g_getStubPage = reinterpret_cast<unsigned char*>(p);
                else
                    VirtualFree(p, 0, MEM_RELEASE);
            }
        }
        if (!g_getStubPage) {
            Log("[store] getter hook skip: target too far "
                "(stubDist=%lld, no page in +-1GB)",
                static_cast<long long>(dist0));
            return false;
        }
        unsigned char* sp = g_getStubPage;
        sp[0] = 0x48;
        sp[1] = 0xB8;   // mov rax, imm64
        *reinterpret_cast<uint64_t*>(sp + 2) =
            reinterpret_cast<uint64_t>(&GetterStub);
        sp[10] = 0xFF;
        sp[11] = 0xE0;  // jmp rax
        target = sp;
        useStubPage = true;
    }
    DWORD oldProt = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
        Log("[store] getter site VirtualProtect failed");
        return false;
    }
    const int32_t nrel = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(target) -
        (reinterpret_cast<uintptr_t>(site) + 5));
    site[0] = 0xE8;
    *reinterpret_cast<int32_t*>(site + 1) = nrel;
    VirtualProtect(site, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    g_getHooked = true;
    Log("[store] getter hooked: site=0x%llX -> %s %p (getter rva 0x%X, "
        "directDist=%lld)",
        static_cast<unsigned long long>(RVA_GET_CALLSITE),
        useStubPage ? "stubPage" : "direct", target,
        static_cast<unsigned>(RVA_GETTER),
        static_cast<long long>(dist0));
    return true;
}

static bool InstallHook() {
    g_hookAddr = g_exeBase + RVA_HOOK;

    // M14: memcpy 前先验证 hook 地址可读，RVA_HOOK 是 HotConfig 可热调变量
    if (!IsReadable((const void*)g_hookAddr, PROLOGUE_LEN)) {
        Log("[store] hook addr %p not readable (PROLOGUE_LEN=%d), abort\n",
            (void*)g_hookAddr, PROLOGUE_LEN);
        return false;
    }

    // 保存原始字节
    memcpy(g_origBytes, (const void*)g_hookAddr, PATCH_LEN);

    // 校验 14 字节序言
    unsigned char probe[PROLOGUE_LEN];
    memcpy(probe, (const void*)g_hookAddr, PROLOGUE_LEN);
    if (memcmp(probe, kPrologue, PROLOGUE_LEN) != 0) {
        Log("[store] FAIL: prologue mismatch at %p"
            " (got %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X)",
            (void*)g_hookAddr,
            probe[0], probe[1], probe[2], probe[3], probe[4],
            probe[5], probe[6], probe[7], probe[8], probe[9],
            probe[10], probe[11], probe[12], probe[13]);
        return false;
    }

    g_backAddr = g_hookAddr + PATCH_LEN;

    if (!BuildStub()) {
        Log("[store] FAIL: stub alloc failed");
        return false;
    }

    // 写入 12 字节绝对跳转
    unsigned char patch[PATCH_LEN];
    patch[0] = 0x48; patch[1] = 0xB8;  // mov rax, imm64
    memcpy(patch + 2, &g_stub, 8);
    patch[10] = 0xFF; patch[11] = 0xE0;  // jmp rax

    DWORD oldProtect;
    if (!VirtualProtect((void*)g_hookAddr, 16,
                        PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("[store] FAIL: VirtualProtect");
        return false;
    }
    memcpy((void*)g_hookAddr, patch, PATCH_LEN);
    VirtualProtect((void*)g_hookAddr, 16, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)g_hookAddr, PATCH_LEN);

    return true;
}

// ============================================================
// Preflight：clock 锚点验证
// ============================================================
static bool Preflight() {
    g_exeBase = (uintptr_t)GetModuleHandleW(nullptr);
    if (!g_exeBase) return false;
    const unsigned char* p = (const unsigned char*)g_exeBase;
    if (p[RVA_CLOCK_LEA] != 0x48 || p[RVA_CLOCK_LEA + 1] != 0x8B ||
        p[RVA_CLOCK_LEA + 2] != 0x05) {
        Log("[store] FAIL: clock LEA mismatch at RVA 0x%llX",
            (unsigned long long)RVA_CLOCK_LEA);
        return false;
    }
    // 解算 clock global 目标，确认 == GAME_ROOT
    int32_t disp = *(const int32_t*)(p + RVA_CLOCK_LEA + 3);
    uintptr_t target = RVA_CLOCK_LEA + 7 + disp;
    Log("[store] clock LEA -> target RVA 0x%llX (GAME_ROOT=0x111CC90)",
        (unsigned long long)target);
    return true;
}

// ============================================================
// 插件入口
// ============================================================
extern "C" __declspec(dllexport) void mod_init(void) {
    LogOpen("selfservice");
    qol::obs::Init(nullptr);  // v1.6.x: 统一观测层接入
    qol::obs::SetInt("selfservice", "ready", 1);
    qol::obs::EventF(1, 2500, L"[SelfServiceStore] 已加载：观测层接入");
    // v-local：游戏版本闸门（village.exe 非 v1.30 build 25640656 则不启用）
    if (!qol::ExeBuildSupported()) {
        Log("[SelfServiceStore] 游戏版本不匹配（非 v1.30 build 25640656）→ 本 MOD 不启用\n");
        return;
    }
    Log("[store] SelfServiceStore v1.3.0 mod_init (v1.30, extended shop hours, read+write)");

    // HotConfig 注册
    HotConfig_Register("selfservice", (void*)&RVA_HOOK,       "RVA_HOOK",       HOT_RVA, 0x290be0);
    HotConfig_Register("selfservice", (void*)&RVA_GETTER,     "RVA_GETTER",     HOT_RVA, 0x290dc0);   // v1.3.0 读取侧
    HotConfig_Register("selfservice", (void*)&RVA_GET_CALLSITE, "RVA_GET_CALLSITE", HOT_RVA, 0x49bca5);  // v1.3.0 读取调用点
    HotConfig_Register("selfservice", (void*)&RVA_CLOCK_LEA,  "RVA_CLOCK_LEA",  HOT_RVA, 0xE2EA3);
    HotConfig_Register("selfservice", (void*)&CLOCK_SAVE_OFF,  "CLOCK_SAVE_OFF", HOT_RVA, 0x208);
    HotConfig_Register("selfservice", (void*)&TIME_B_OFF,      "TIME_B_OFF",     HOT_RVA, 0x3270);
    HotConfig_Register("selfservice", (void*)&OPEN_HOUR_FROM,  "OPEN_HOUR_FROM", HOT_INT, 0);
    HotConfig_Register("selfservice", (void*)&OPEN_HOUR_TO,   "OPEN_HOUR_TO",   HOT_INT, 23);
    // v1.3.0：默认延长营业（24..23 恒不成立）；热参改回 0/5 可恢复深夜降级
    HotConfig_Register("selfservice", (void*)&CLOSE_HOUR_FROM, "CLOSE_HOUR_FROM", HOT_INT, 24);
    HotConfig_Register("selfservice", (void*)&CLOSE_HOUR_TO,   "CLOSE_HOUR_TO",   HOT_INT, 23);
    HotConfig_Register("selfservice", (void*)&g_enabled,      "enabled",        HOT_INT, 1);
    HotConfig_Register("selfservice", (void*)&LEGACY_ALL_KEYS, "LEGACY_ALL_KEYS", HOT_INT, 0);
    HotConfig_Poll();
    HotConfig_DumpCE("selfservice");

    if (!g_enabled) {
        Log("[store] disabled by config");
        LogClose();
        return;
    }

    if (!Preflight()) {
        Log("[store] preflight failed, mod disabled");
        LogClose();
        return;
    }

    if (!InstallHook()) {
        Log("[store] hook install failed, mod disabled");
        LogClose();
        return;
    }

    // v1.2.0：读取侧 hook（全天开放）——失败不影响写入侧（只打日志）
    if (!InstallGetterHook())
        Log("[store] getter hook not installed (read-side fix inactive)");

    g_ready = true;
    int hour = GetHour();
    Log("[store] hooked %p -> stub %p (back %p), hour=%d"
        ", keys: shopOpen (v1.3.0 strict, extended hours)",
        (void*)g_hookAddr, g_stub, (void*)g_backAddr, hour);
}

extern "C" __declspec(dllexport) void mod_tick(void) {
    qol::budget::BeginFrame();  // P0: 帧锚定（幂等，多 DLL 安全）
    static int s_bdgSlot = -1;  // P0 探针（惰性注册，析构自动上报）
    if (s_bdgSlot < 0) s_bdgSlot = qol::budget::Slot("SelfServiceStore");
    struct BdgGuard {
        int slot; uint64_t t0;
        ~BdgGuard() { qol::budget::Report(slot, qol::budget::NowUs() - t0); }
    } bdgGuard = { s_bdgSlot, qol::budget::NowUs() };

    if (!g_ready) return;
    if (QolGameBusy()) return;  // P1-2: 载入/菜单期间静默
    HotConfig_Poll();

    // v1.1.7-diag: 钩子活性心跳（每 ≥10s 且计数变化时才写）——
    // 若日志里始终只有 calls=0，则 0x288370 根本不在购物/进店路径上，
    // 需重新定位真正的营业判定函数
    static ULONGLONG s_lastBeat = 0;
    static LONG s_lastReported = -1;
    ULONGLONG now = GetTickCount64();
    if (s_lastBeat == 0 || now - s_lastBeat >= 10000) {
        s_lastBeat = now;
        LONG c = g_hookCalls;
        if (c != s_lastReported) {
            s_lastReported = c;
            Log("[store] heartbeat: hookCalls=%d shopHits=%d hour=%d",
                (int)c, (int)g_diagCount, GetHour());
        }
    }
}

extern "C" __declspec(dllexport) void unload(void) {
    if (g_ready && g_hookAddr) {
        DWORD oldProtect;
        if (VirtualProtect((void*)g_hookAddr, 16,
                           PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy((void*)g_hookAddr, g_origBytes, PATCH_LEN);
            VirtualProtect((void*)g_hookAddr, 16, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(),
                                  (void*)g_hookAddr, PATCH_LEN);
        }
    }
    if (g_stub) {
        VirtualFree(g_stub, 0, MEM_RELEASE);
        g_stub = nullptr;
    }
    Log("[store] unloaded (diag calls=%d)", (int)g_diagCount);
    LogClose();
    g_ready = false;
}

// ============================================================
// DllMain
// ============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        qol::obs::Shutdown();  // v1.6.x: 解观测层映射（仅本 DLL 视图）
    }
    return TRUE;
}
