// sower.cpp —— 范围播种 (1x1 / 3x3 / 5x5) (v1.3.41：连通档下线（用户拍板）——v1.20 连通 Flood-Fill 实测 filled=0 不工作；v1.3.36：每帧最多结算 1 格限速防播种风暴；v1.3.35-diag 计时确认额外格 28us/格)
//
// v1.3.10: detour 入口限频诊断日志（区分调用链断裂 vs 结构漂移）——
//   实测 v1.20：detour 被调用（调用链完好），但 next=0xFFFFFFFF、count=0
//   → 结构偏移漂移实锤（NEXT_PHASE_OFFSET 0x1DC 在 v1.20 已失效）。
// v1.3.11: detour 内 dump state 窗口 0x1C0-0x2BF（8 行×8 值），
// v1.3.12: next_phase(0x1DC) 在 v1.20 始终 FFFFFFFF→回退用 count 增加判断中心结算。
// v1.3.13: 修正 v1.3.12 逻辑反转 bug（else 分支忘了取反"未结算"条件）。
// v1.3.14: SowingCaptureCenterItem 全路径诊断日志（定位 centerItem invalid 原因）。
// v1.3.15: save data 偏移漂移回退模式——当 save data 验证失败时，仅用 sowing state
//          结构（state+0x268 land 指针 + SowingInspectLand），跳过所有 save data
//          依赖的验证。影响 4 个函数：SowingCaptureCenterItem / SettleNativeExtras /
//          SettleTileCore / SettleNativeExtrasImpl。
//   对照 phase 推进序列定位 v1.20 新偏移（定位后热参数直接修复，免编译）。
//
// v1.3.25-diag: mapInfo 偏移热参数化（MANAGER_MAPINFO_OFFSET 默认 0x310）+
//          manager/mapInfo 诊断 dump（validateLand 失败时打印 mapInfo+[mI+0x30]+
//          manager+0x300~0x340 窗口），定位 v1.20 manager 结构 0x310 偏移是否漂移。
//          根因分析：AOB 确认 RVA_SOW_VALIDATE_LAND=0x1E1310 正确（全 exe 唯一命中），
//          函数体从 [mapInfo+0x30]→[+0x560] 解引用；若 0x310 漂移→mapInfo 无效→SEH→false。
// v1.3.16: H10——7 个原生函数调用加 SEH 保护（stateManagerLookup / sowValidateLand /
//          sowValidateState / sowSeedItemLookup / sowSeedItemCheck / sowWorldToKey /
//          sowKeyToWorld），异常时返回失败默认值（nullptr / false / 0）。
//          M4——连通模式预算耗尽 break 前 --idx 回退到未结算格，避免跳过一格。
//
// v1.3.8: v1.20 (build 25311578) 适配——13 个 RVA 全部重定位；4 个 expected 数组从新 exe 实读重生成（STATE_LOOKUP/REFRESH_LAND/SEED_ITEM_LOOKUP 的 rel32 变化 + VALIDATE_STATE 序言重写：r12→r13、栈帧 0xA0→0x90）。22B hook 序言与结构偏移（[state+0x268] 等）经多路交叉验证不变。
// v1.3.5: 连通模式四邻域坐标系修复——旧代码直接用世界轴 ±45 偏移，
//         在 45 度菱形网格上落在格子中缝（非格子中心），Flood-Fill
//         永远只收集到中心一格，连通模式对普通田地/田垄全失效。
//         修复：四邻域改屏幕格子偏移 + 基向量变换（与 3x3/5x5 同坐标系）。
// v1.3.1: 续播队列 state 指针添加 IsReadable 防悬空检查。
//
// 切换键：键盘 5 或 手柄 D-pad Left  循环 1x1(原生) -> 3x3 -> 5x5 -> 1x1（v1.3.41 连通档下线）
// 原则：只使用背包中当前选中的种子，播完即停。
//
// 核心策略（源自 BigL233 dinput8.cpp 3272-5944 行，适配 v1.20 build 25311578）：
//   Hook 播种状态机 Phase 1 更新函数 (RVA 0x24ACE0, 22 字节序言)。
//   中心格子由原生结算，额外格子同步批量结算。
//   播种状态机两阶段：
//     Phase 0 (RVA 0x24ACA0): 刷新 [state+0x268] 选中种子 + 预验证
//     Phase 1 (RVA 0x24ACE0): 重新选目标 -> command 0x3FE -> 验证 -> 0x24B460 结算
//   中心结算成功后 (count+1)，枚举周围格子做同步批量结算。
//
// 安全措施：
//   - 所有指针访问通过 IsReadable 检查（含 VirtualQuery 区域缓存）
//   - 每个额外格子结算前验证中心种子物品未变（索引/物品/head/field 全匹配）
//   - 种子耗尽自动停止
//   - 任何异常立即 fail-closed，不继续操作

#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <cfloat>   // FLT_MAX

// 手柄 D-pad 支持（XInput + HID + joyGetPosEx 三路混合）
#include <mmsystem.h>    // joyGetPosEx (winmm)
#include <setupapi.h>    // SetupAPI 枚举 HID 设备
#include <hidsdi.h>      // HidD_GetHidGuid / HidD_GetAttributes
#pragma comment(lib, "winmm.lib")  // joyGetPosEx
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

#include "logging.h"
#include "budget.h"        // v1.3.19: P0 帧预算/耗时探针（跨 DLL 共享）

static int s_sowerBudgetSlot = -1;  // v1.3.19: P0 探针槽位
#include "hot_config.h"
#include "hotkey.h"
#include "game_window.h"   // HUD owner 绑定游戏窗口
#include "patch_safety.h"

// 日志开关：发布版禁用日志输出（不生成 qol_sower.log）
// 调试时取消注释下行即可开启日志，无需改其他代码
// v1.3.41 转正：日志关闭（qol_sower.log 不再生成）
// #define SOWER_LOGGING
#ifdef SOWER_LOGGING
#else
  #define LogOpen(x)  ((void)0)
  #define Log(...)    ((void)0)
  #define LogV(...)   ((void)0)
  #define LogClose()  ((void)0)
#endif

// [diag] 诊断日志门控（默认关）；调试时 #define DIAG_SOWER 1 开启
#ifndef DIAG_SOWER
#define DIAG_SOWER 0      // v1.3.41 转正：诊断日志关闭（定位时改 1 重编）
#endif

// ============================================================
// 常量（build 25311578 / v1.20）
// ============================================================

// ---- RVA 地址 ----
static volatile uintptr_t RVA_SOWING_UPDATE             = 0x24ACE0;
static volatile uintptr_t RVA_SOWING_PHASE0_UPDATE      = 0x24ACA0;
static volatile uintptr_t RVA_STATE_MANAGER_LOOKUP      = 0x1CA5D0;
static volatile uintptr_t RVA_SOW_SELECT_TARGET         = 0x212020;
static volatile uintptr_t RVA_SOW_VALIDATE_LAND         = 0x1E1310;
static volatile uintptr_t RVA_SOW_REFRESH_LAND          = 0x212110;
static volatile uintptr_t RVA_SOW_VALIDATE_STATE        = 0x24B0B0;
static volatile uintptr_t RVA_SOW_SETTLE                = 0x24B460;
static volatile uintptr_t RVA_SOW_SEED_ITEM_LOOKUP      = 0x17C840;
static volatile uintptr_t RVA_SOW_SEED_ITEM_CHECK       = 0x75F630;
static volatile uintptr_t RVA_GAME_SINGLETON_PTR        = 0x10FCBB0;
static volatile uintptr_t RVA_SOW_WORLD_TO_KEY          = 0x16D460;
static volatile uintptr_t RVA_SOW_KEY_TO_WORLD          = 0x16D540;

// ---- 结构体偏移 ----
static volatile uintptr_t SAVE_DATA_POINTER_OFFSET           = 0x208;
static volatile uintptr_t SAVE_INVENTORY_UNLOCK_FLAGS_OFFSET = 0x388;
static volatile uintptr_t SAVE_SKIP_ITEM_DEDUCTION_OFFSET    = 0x340C;
static volatile uintptr_t SAVE_INVENTORY_SLOTS_OFFSET        = 0x32C0;
static volatile uintptr_t SAVE_SELECTED_ITEM_INDEX_OFFSET    = 0x33B0;
static constexpr size_t    SAVE_INVENTORY_MAX_SLOTS           = 30;

static constexpr uint64_t ACTION_SOW = 0x424;

static volatile uintptr_t SOWING_STATE_PHASE_OFFSET         = 0x1D8;
static volatile uintptr_t SOWING_STATE_NEXT_PHASE_OFFSET    = 0x1DC;
static volatile uintptr_t SOWING_STATE_TARGET_OFFSET        = 0x240;
static volatile uintptr_t SOWING_STATE_QUERY_OFFSET         = 0x248;
static volatile uintptr_t SOWING_STATE_LAND_OFFSET          = 0x268;
static volatile uintptr_t SOWING_STATE_SELECTED_KEY_OFFSET  = 0x270;
static volatile uintptr_t SOWING_STATE_COUNT_OFFSET         = 0x278;
static volatile uintptr_t SOWING_STATE_VALIDATED_OFFSET     = 0x27C;
static volatile uintptr_t SOWING_STATE_RUNNER_OFFSET        = 0x238;

static volatile uintptr_t UNIT_POSITION_OFFSET         = 0x230;
static volatile uintptr_t SOWING_MANAGER_TARGET_OFFSET = 0x460;
static volatile uintptr_t MANAGER_MAPINFO_OFFSET       = 0x310;  // v1.3.24: manager→mapInfo 偏移（热参数化，v1.20 待验证）

// ---- 种子物品内部偏移 ----
static volatile uintptr_t ITEM_DATA_HOLDER_OFFSET  = 0x240;
static volatile uintptr_t ITEM_STACK_COUNT_OFFSET  = 0x260;

// ---- 网格布局 ----
static constexpr float SOW_TILE_STEP = 45.0f;
static constexpr size_t SOWING_MAX_EXTRA_TILES = 24;      // 固定 3x3/5x5 最大额外格子
static constexpr size_t SOWING_MAX_CONNECTED_TILES = 256; // 连通播种 Flood-Fill 上限（保护）
static constexpr size_t SOWING_MAX_TILES = 256;           // 批量结算统一容量（含连通）
static constexpr size_t SOWING_HOOK_LENGTH = 22;

// ---- 防卡顿：每帧结算时间预算（微秒）----
// 连通播种可能一次收集上百格，若同一帧全部结算必然卡顿。
// 方案：每帧最多用掉 SOWING_FRAME_BUDGET_US 微秒做结算，剩余格子存入
//       续播队列，由 mod_tick 的续播泵逐帧完成（每次播种动作后最多约 6ms）。
static constexpr uint64_t SOWING_FRAME_BUDGET_US = 2000;   // v1.3.4: 6000→2000（避免单帧 6ms 脉冲）
static constexpr size_t  SOWING_RESUME_KEEP_TILES = 2;     // 剩余 >= 2 格才启用续播

// ---- DualSense HID 常量 ----
// DualSense USB HID 输入报告布局（源自 Linux 内核 hid-playstation.c）:
//   byte 0 = Report ID (0x01)
//   byte 8 = buttons[0]: bits 0-3 = D-pad Hat
//   D-pad Hat: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8=Released
//   D-pad Left = hat 5(SW) 或 6(W) 或 7(NW)
static constexpr unsigned short DUALSENSE_VID = 0x054C;
static constexpr unsigned short DUALSENSE_PID = 0x0CE6;
static constexpr size_t DUALSENSE_REPORT_SIZE = 64;
static constexpr int DUALSENSE_DPAD_OFFSET = 8;

// ============================================================
// 类型定义
// ============================================================

struct SowingTileOffset { int dx, dy; };

struct SowingQueryResult {
    uint64_t first;
    uint8_t  valid;
    uint16_t score;
    uint32_t flags;
    uint64_t tailA;
    uint64_t tailB;
};
static_assert(sizeof(SowingQueryResult) == 0x20, "sowing query layout");

struct SowingLandInfo {
    void*     land = nullptr;
    void*     head = nullptr;
    uint64_t  action = 0;
    void*     field = nullptr;
    bool      readable = false;
};

struct alignas(16) SowingWorldPoint {
    float x, y, z, w;
};
static_assert(sizeof(SowingWorldPoint) == 0x10, "sowing world point");

struct SowingCenterItemSnapshot {
    void*          save = nullptr;
    uint32_t       selectedIndex = 0;
    size_t         unlockedCapacity = 0;
    void*          item = nullptr;
    SowingLandInfo info = {};
    bool           valid = false;
    bool           saveDataOk = false;  // v1.3.15: save data 验证成功标志，false=偏移漂移回退模式
};

struct SowingPendingCenterItem {
    void*                     state = nullptr;
    void*                     task = nullptr;
    void*                     runner = nullptr;
    uint64_t                  selectedKey = 0;
    SowingCenterItemSnapshot  item = {};
    bool                      armed = false;
};

// ---- 函数指针类型 ----
using SowingUpdateFunction       = bool  (__fastcall*)(void*);
using StateManagerLookupFunction = void* (__fastcall*)(void*);
using SowingSelectTargetFunction = void* (__fastcall*)(void*, SowingQueryResult*, uint64_t);
using SowingValidateLandFunction = bool  (__fastcall*)(void*, uint64_t, void*);
using SowingRefreshLandFunction  = void  (__fastcall*)(void*);
using SowingValidateStateFunction= bool  (__fastcall*)(void*);
using SowingSettleFunction       = void  (__fastcall*)(void*);
using SowingSeedItemLookupFunction = void* (__fastcall*)(void*);
using SowingSeedItemCheckFunction  = bool  (__fastcall*)(void*, unsigned char, unsigned char);
using SowingWorldToKeyFunction   = uint64_t (__fastcall*)(const SowingWorldPoint*);
using SowingKeyToWorldFunction   = SowingWorldPoint* (__fastcall*)(SowingWorldPoint*, uint64_t);
using SowingFacingFunction       = double (__fastcall*)(void*);

// ============================================================
// 全局状态
// ============================================================
namespace G {
    uintptr_t base = 0;
    bool ready = false;

    std::atomic<int> rangeSowingMode{0};       // 0=1x1, 1=3x3, 2=5x5（v1.3.41 连通档 3 下线，Flood-Fill 代码保留不可达）
    bool rangeSowingReady = false;
    bool rangeSowingNativeBatchReady = false;

    // 手柄 D-pad Left 检测（三路混合：XInput 线程 + HID 线程 + joyGetPosEx 回退）
    bool padReady = false;           // joyGetPosEx 路径是否可用
    bool xinputReady = false;        // XInput 线程是否启动
    bool hidReady = false;           // HID 线程是否启动
    bool dpadLeftNeedsRelease = true;  // 边沿触发：需先释放再按下
    DWORD padReconnectTimer = 0;    // joyGetPosEx 热插拔重试计数

    // 函数指针
    SowingUpdateFunction         originalSowingUpdate = nullptr;
    StateManagerLookupFunction   stateManagerLookup = nullptr;
    SowingSelectTargetFunction   sowSelectTarget = nullptr;
    SowingValidateLandFunction   sowValidateLand = nullptr;
    SowingRefreshLandFunction    sowRefreshLand = nullptr;
    SowingValidateStateFunction  sowValidateState = nullptr;
    SowingSettleFunction         sowSettle = nullptr;
    SowingSeedItemLookupFunction sowSeedItemLookup = nullptr;
    SowingSeedItemCheckFunction  sowSeedItemCheck = nullptr;
    SowingWorldToKeyFunction     sowWorldToKey = nullptr;
    SowingKeyToWorldFunction     sowKeyToWorld = nullptr;
}

// TLS
static thread_local bool g_insideRangeSowing = false;
static thread_local SowingPendingCenterItem g_sowingPendingCenterItem;

// ---- 手柄 D-pad Left：XInput 独立线程 + HID 独立线程 + joyGetPosEx 同步回退 ----
// XInput 线程：Xbox 手柄热插拔天然支持，独立线程避免 mod_tick 卡死
// HID 线程：DualSense 直读，断开后重新枚举设备实现热插拔
// joyGetPosEx：其他 DirectInput 手柄，同步调用做最后回退
static volatile LONG g_xinputThreadRunning = 0;
static volatile LONG g_xinputDpadLeft = 0;
static HANDLE g_xinputThread = nullptr;
static DWORD WINAPI XInputPollThread(LPVOID);     // 前向声明
static DWORD (WINAPI *g_XInputGetState)(DWORD, void*) = nullptr;
static void LoadXInput();

static volatile LONG g_hidThreadRunning = 0;
static volatile LONG g_hidDpadLeft = 0;
static volatile LONG g_hidConnected = 0;
static HANDLE g_hidThread = nullptr;
static HANDLE volatile g_hidDevice = nullptr;   // DualSense 设备句柄（unload 时关闭以中断 ReadFile）
static DWORD WINAPI HidPollThread(LPVOID);       // 前向声明

// 防重复加载
static bool g_duplicate = false;   // 本副本是否被判定为重复加载

static QolHotKeys g_hotkeys;  // 运行时热键

// trampoline 地址（ unload 时释放用）
static void* g_sowingTrampoline = nullptr;

// ============================================================
// 内存安全：IsReadable + 区域缓存
// ============================================================
static constexpr size_t SOWING_READ_REGION_CACHE_SIZE = 64;

struct SowingReadRegion {
    uintptr_t begin = 0;
    uintptr_t end = 0;
};

struct SowingReadRegionCache {
    SowingReadRegion regions[SOWING_READ_REGION_CACHE_SIZE] = {};
    size_t count = 0;
    size_t replacement = 0;
};

static thread_local SowingReadRegionCache* g_sowingReadRegionCache = nullptr;

class SowingReadRegionCacheScope {
public:
    explicit SowingReadRegionCacheScope(SowingReadRegionCache* cache)
        : previous_(g_sowingReadRegionCache) {
        g_sowingReadRegionCache = cache;
    }
    ~SowingReadRegionCacheScope() { g_sowingReadRegionCache = previous_; }
    SowingReadRegionCacheScope(const SowingReadRegionCacheScope&) = delete;
    SowingReadRegionCacheScope& operator=(const SowingReadRegionCacheScope&) = delete;
private:
    SowingReadRegionCache* previous_;
};

static bool SowingIsReadable(const void* pointer, size_t size) {
    SowingReadRegionCache* cache = g_sowingReadRegionCache;
    if (!cache) {
        // 无缓存时走 VirtualQuery
        if (!pointer || size == 0) return false;
        uintptr_t start = (uintptr_t)pointer;
        if (start < 0x10000 || start > 0x00007FFFFFFFFFFFULL) return false;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(pointer, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (start + size > regionEnd || start + size < start) return false;
        return true;
    }
    if (!pointer || size == 0) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    if (start < 0x10000 || start > 0x00007FFFFFFFFFFFULL ||
        start + size < start) {
        return false;
    }
    const uintptr_t requestedEnd = start + size;
    for (size_t i = 0; i < cache->count; ++i) {
        if (start >= cache->regions[i].begin && requestedEnd <= cache->regions[i].end)
            return true;
    }
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(pointer, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const uintptr_t regionBegin = (uintptr_t)mbi.BaseAddress;
    const uintptr_t regionEnd = regionBegin + mbi.RegionSize;
    if (regionEnd < regionBegin || start < regionBegin || requestedEnd > regionEnd)
        return false;
    size_t idx = cache->count;
    if (cache->count < SOWING_READ_REGION_CACHE_SIZE) {
        ++cache->count;
    } else {
        idx = cache->replacement;
        cache->replacement = (cache->replacement + 1) % SOWING_READ_REGION_CACHE_SIZE;
    }
    cache->regions[idx] = {regionBegin, regionEnd};
    return true;
}

// ---- 类型化读取辅助 ----
static uint32_t SowingReadU32(void* base, uintptr_t offset) {
    if (!SowingIsReadable(reinterpret_cast<unsigned char*>(base) + offset, sizeof(uint32_t)))
        return 0;
    return *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(base) + offset);
}

static uint8_t SowingReadU8(void* base, uintptr_t offset) {
    if (!SowingIsReadable(reinterpret_cast<unsigned char*>(base) + offset, sizeof(uint8_t)))
        return 0;
    return *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(base) + offset);
}

static uint64_t SowingReadU64(void* base, uintptr_t offset) {
    if (!SowingIsReadable(reinterpret_cast<unsigned char*>(base) + offset, sizeof(uint64_t)))
        return 0;
    return *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(base) + offset);
}

static void* SowingReadPointer(void* base, uintptr_t offset) {
    if (!SowingIsReadable(reinterpret_cast<unsigned char*>(base) + offset, sizeof(void*)))
        return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(base) + offset);
}

static bool SowingTryReadU32(void* base, uintptr_t offset, uint32_t* out) {
    if (!out || !SowingIsReadable(reinterpret_cast<unsigned char*>(base) + offset, sizeof(uint32_t)))
        return false;
    *out = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(base) + offset);
    return true;
}

static bool SowingTryReadU64(void* base, uintptr_t offset, uint64_t* out) {
    if (!out || !SowingIsReadable(reinterpret_cast<unsigned char*>(base) + offset, sizeof(uint64_t)))
        return false;
    *out = *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(base) + offset);
    return true;
}

static bool SowingTryReadU8(void* base, uintptr_t offset, uint8_t* out) {
    if (!out || !SowingIsReadable(reinterpret_cast<unsigned char*>(base) + offset, sizeof(uint8_t)))
        return false;
    *out = *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(base) + offset);
    return true;
}

static bool SowingTryReadPointer(void* base, uintptr_t offset, void** out) {
    if (!out || !SowingIsReadable(reinterpret_cast<unsigned char*>(base) + offset, sizeof(void*)))
        return false;
    *out = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(base) + offset);
    return true;
}

static void SowingWriteU64(void* base, uintptr_t offset, uint64_t value) {
    if (!SowingIsReadable(reinterpret_cast<unsigned char*>(base) + offset, sizeof(uint64_t)))
        return;
    *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(base) + offset) = value;
}

static void SowingWriteU8(void* base, uintptr_t offset, uint8_t value) {
    if (!SowingIsReadable(reinterpret_cast<unsigned char*>(base) + offset, sizeof(uint8_t)))
        return;
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(base) + offset) = value;
}

// ============================================================
// 内存写入工具（用于 hook 安装）
// ============================================================
static bool WriteMem(void* target, const void* data, size_t size) {
    return qol::WritePatchChecked(target, data, size);
}

// v1.3.16: 原生函数 SEH 包装前向声明（定义在 L803+）
__declspec(noinline) static uint64_t SafeSowWorldToKey(const SowingWorldPoint* point);
__declspec(noinline) static SowingWorldPoint* SafeSowKeyToWorld(SowingWorldPoint* point, uint64_t key);
__declspec(noinline) static void* SafeStateManagerLookup(void* owner);
__declspec(noinline) static void* SafeSowSeedItemLookup(void* owner);
__declspec(noinline) static bool SafeSowSeedItemCheck(void* item, unsigned char a, unsigned char b);
__declspec(noinline) static bool SafeSowValidateLand(void* mapInfo, uint64_t key, void* field);

// ============================================================
// 播种几何：WorldToKey / KeyToWorld / OffsetKeyNative
// ============================================================
static constexpr float SOWING_DEGREES_TO_RADIANS =
    3.14159265358979323846f / 180.0f;

static bool SowingWorldToKey(float x, float y, uint64_t* outKey) {
    if (!outKey || !G::sowWorldToKey || !std::isfinite(x) || !std::isfinite(y))
        return false;
    const SowingWorldPoint point = {x, y, 0.0f, 0.0f};
    const uint64_t key = SafeSowWorldToKey(&point);
    if (key == 0 || key == ~uint64_t{0}) return false;
    *outKey = key;
    return true;
}

static bool SowingKeyToWorld(uint64_t key, SowingWorldPoint* outPoint) {
    if (!outPoint || !G::sowKeyToWorld || key == 0 || key == ~uint64_t{0})
        return false;
    SowingWorldPoint point = {};
    if (SafeSowKeyToWorld(&point, key) != &point ||
        !std::isfinite(point.x) || !std::isfinite(point.y) ||
        point.z != 0.0f || point.w != 0.0f)
        return false;
    // 往返校验
    uint64_t roundTrip = 0;
    if (!SowingWorldToKey(point.x, point.y, &roundTrip) || roundTrip != key)
        return false;
    *outPoint = point;
    return true;
}

static bool SowingOffsetKeyNative(uint64_t originKey, long long rowDelta,
                                   long long colDelta, uint64_t* outKey) {
    if (!outKey) return false;
    SowingWorldPoint point = {};
    if (!SowingKeyToWorld(originKey, &point)) return false;
    const double x = (double)point.x + (double)colDelta * SOW_TILE_STEP;
    const double y = (double)point.y + (double)rowDelta * SOW_TILE_STEP;
    if (!std::isfinite(x) || !std::isfinite(y) ||
        x < -(double)FLT_MAX || x > (double)FLT_MAX ||
        y < -(double)FLT_MAX || y > (double)FLT_MAX)
        return false;
    const float cx = (float)x, cy = (float)y;
    uint64_t key = 0;
    if (!SowingWorldToKey(cx, cy, &key)) return false;
    // 要求 key 往返一致
    SowingWorldPoint canonical = {};
    if (!SowingKeyToWorld(key, &canonical) ||
        canonical.x != cx || canonical.y != cy)
        return false;
    *outKey = key;
    return true;
}

// ============================================================
// 播种数据辅助
// ============================================================
static SowingLandInfo SowingInspectLand(void* land) {
    SowingLandInfo info;
    info.land = land;
    if (!land) return info;
    void* record = SowingReadPointer(land, ITEM_DATA_HOLDER_OFFSET);
    if (!record) return info;
    void* head = SowingReadPointer(record, 0);
    // v1.3.18: head 偏移漂移修复——dump 定位 ACTION_SOW 在 head+0x230（v1.09: 0x208）
    // field 指针在 head+0x228（v1.09: 0x210）
    if (!head || !SowingIsReadable(reinterpret_cast<unsigned char*>(head) + 0x228, sizeof(uint64_t) * 2))
        return info;
    info.head = head;
    info.action = SowingReadU64(head, 0x230);   // v1.20: 0x208→0x230 (+0x28)
    info.field = SowingReadPointer(head, 0x238); // v1.20: 0x210→0x238 (+0x28，与 action 同步漂移；2026-10-03 反汇编实锤：原生 validateLand 调用点 field 经 [head+0x238] 解引用)
    info.readable = true;
    return info;
}

// ============================================================
// v1.3.17: head 结构偏移漂移诊断
// inspect fail 时 2s 限频 dump land→record→head 链 +
// head+0x100~0x2FF 扫描 ACTION_SOW(0x424) + head+0x1E0~0x230 窗口 hex dump
// 定位 v1.20 head 内部 action/field 偏移漂移
// ============================================================
static ULONGLONG g_sowingInspectFailDumpTime = 0;

__declspec(noinline)
static void SowingDumpLandChainOnInspectFail(void* land) {
    ULONGLONG now = GetTickCount64();
    if (now - g_sowingInspectFailDumpTime < 2000) return;
    g_sowingInspectFailDumpTime = now;

    void* record = land ? SowingReadPointer(land, ITEM_DATA_HOLDER_OFFSET) : nullptr;
    void* head = record ? SowingReadPointer(record, 0) : nullptr;
#if DIAG_SOWER
    Log("[Sower] [diag] dump: land=%p record=%p(off 0x%llx) head=%p\n",
        land, record,
        (unsigned long long)(uintptr_t)ITEM_DATA_HOLDER_OFFSET,
        head);
#endif
    if (!head) return;

    // head+0x100~0x2FF 扫描 ACTION_SOW (0x424)
    if (!SowingIsReadable(reinterpret_cast<unsigned char*>(head) + 0x100, 0x200)) {
#if DIAG_SOWER
        Log("[Sower] [diag] dump: head+0x100..0x2FF not readable\n");
#endif
        return;
    }
    int hits = 0;
    for (uintptr_t off = 0x100; off + 8 <= 0x300; off += 4) {
        uint64_t v = SowingReadU64(head, off);
        if (v == ACTION_SOW) {
#if DIAG_SOWER
            Log("[Sower] [diag] dump: head+0x%llx = 0x424 (ACTION_SOW candidate)\n",
                (unsigned long long)off);
#endif
            ++hits;
        }
    }
    if (hits == 0) {
#if DIAG_SOWER
        Log("[Sower] [diag] dump: no 0x424 found in head+0x100~0x2FF\n");
#endif
    }

    // 窗口 dump: head+0x1E0~0x230 (预期 action 在 0x208 附近)，每行 4 个 u64
    for (uintptr_t off = 0x1E0; off < 0x230; off += 0x20) {
        uint64_t v0 = SowingReadU64(head, off);
        uint64_t v1 = SowingReadU64(head, off + 8);
        uint64_t v2 = SowingReadU64(head, off + 0x10);
        uint64_t v3 = SowingReadU64(head, off + 0x18);
#if DIAG_SOWER
        Log("[Sower] [diag] dump: head+0x%llx: %016llx %016llx %016llx %016llx\n",
            (unsigned long long)off,
            (unsigned long long)v0, (unsigned long long)v1,
            (unsigned long long)v2, (unsigned long long)v3);
#endif
    }
}

static void* SowingGetSaveData() {
    if (!G::base) return nullptr;
    void* game = SowingReadPointer(reinterpret_cast<void*>(G::base), RVA_GAME_SINGLETON_PTR);
    if (!game || !SowingIsReadable(game, SAVE_DATA_POINTER_OFFSET + sizeof(void*)))
        return nullptr;
    void* save = SowingReadPointer(game, SAVE_DATA_POINTER_OFFSET);
    if (!save ||
        !SowingIsReadable(save, SAVE_SELECTED_ITEM_INDEX_OFFSET + sizeof(uint32_t)) ||
        !SowingIsReadable(save, SAVE_INVENTORY_SLOTS_OFFSET + SAVE_INVENTORY_MAX_SLOTS * sizeof(void*)))
        return nullptr;
    return save;
}

static bool SowingReadUnlockedInventoryCapacity(void* save, size_t* capacity) {
    if (!save || !capacity) return false;
    uint64_t flags = 0;
    if (!SowingTryReadU64(save, SAVE_INVENTORY_UNLOCK_FLAGS_OFFSET, &flags))
        return false;
    *capacity = (flags & (1ULL << 22)) != 0 ? 30u
              : ((flags & (1ULL << 21)) != 0 ? 20u : 10u);
    return true;
}

static SowingCenterItemSnapshot SowingCaptureCenterItem(void* state) {
    SowingCenterItemSnapshot snapshot;
    if (!state) return snapshot;
    // v1.3.15: 先从 sowing state 读 land 指针（不依赖 save data 偏移）
    if (!SowingTryReadPointer(state, SOWING_STATE_LAND_OFFSET, &snapshot.item) ||
        !snapshot.item) {
#if DIAG_SOWER
        Log("[Sower] [diag] capture: land ptr null (offset=0x%llx)\n",
            (unsigned long long)SOWING_STATE_LAND_OFFSET);
#endif
        return snapshot;
    }
    // 尝试 save data 验证（如果偏移正确则增强可信度）
    snapshot.save = SowingGetSaveData();
    snapshot.saveDataOk = false;
    if (snapshot.save &&
        SowingReadUnlockedInventoryCapacity(snapshot.save, &snapshot.unlockedCapacity) &&
        SowingTryReadU32(snapshot.save, SAVE_SELECTED_ITEM_INDEX_OFFSET, &snapshot.selectedIndex) &&
        snapshot.unlockedCapacity > 0 &&
        snapshot.unlockedCapacity <= SAVE_INVENTORY_MAX_SLOTS &&
        snapshot.selectedIndex < snapshot.unlockedCapacity) {
        void* selectedSlotItem = nullptr;
        if (SowingTryReadPointer(snapshot.save,
                SAVE_INVENTORY_SLOTS_OFFSET + (size_t)snapshot.selectedIndex * sizeof(void*),
                &selectedSlotItem) &&
            selectedSlotItem == snapshot.item) {
            snapshot.saveDataOk = true;
        } else {
#if DIAG_SOWER
            Log("[Sower] [diag] capture: save data item mismatch, fallback to state-only mode\n");
#endif
        }
    } else if (snapshot.save) {
#if DIAG_SOWER
        Log("[Sower] [diag] capture: save data validation failed, fallback to state-only mode\n");
#endif
    } else {
#if DIAG_SOWER
        Log("[Sower] [diag] capture: save data null, fallback to state-only mode\n");
#endif
    }
    // v1.3.15: 无论 save data 验证是否成功，只要 land info 有效就标记 valid
    snapshot.info = SowingInspectLand(snapshot.item);
    snapshot.valid = snapshot.info.readable &&
                     snapshot.info.action == ACTION_SOW &&
                     snapshot.info.field != nullptr;
    if (!snapshot.valid) {
#if DIAG_SOWER
        Log("[Sower] [diag] capture: inspect fail readable=%d action=%u field=%p\n",
            (int)snapshot.info.readable, snapshot.info.action, snapshot.info.field);
        SowingDumpLandChainOnInspectFail(snapshot.item);  // v1.3.17: land 链 dump 诊断
#endif
    } else if (!snapshot.saveDataOk) {
#if DIAG_SOWER
        Log("[Sower] [diag] capture: valid via state-only (saveDataOk=0)\n");
#endif
    }
    return snapshot;
}

// ============================================================
// Manager / SeedItem 辅助
// ============================================================
static void* RangeSowingGetManager(void* state) {
    void* owner = SowingReadPointer(state, 8);
    if (!owner || !G::stateManagerLookup) return nullptr;
    return SafeStateManagerLookup(owner);
}

static void* RangeSowingGetSeedItem(void* state) {
    if (!G::sowSeedItemLookup) return nullptr;
    void* owner = SowingReadPointer(state, 8);
    if (!owner) return nullptr;
    return SafeSowSeedItemLookup(owner);
}

static bool RangeSowingCheckSeedItem(void* item) {
    return item && G::sowSeedItemCheck && SafeSowSeedItemCheck(item, 0, 0);
}

// ============================================================
// 格子计划构建
// ============================================================
static bool SowingBuildTilePlan(int mode, SowingTileOffset* offsets, size_t* count) {
    if (!offsets || !count || (mode != 1 && mode != 2)) return false;
    const int size = mode == 2 ? 5 : 3;
    const int half = size / 2;
    size_t index = 0;
    for (int distance = 1; distance <= half; ++distance) {
        for (int dx = -distance; dx <= distance; ++dx) {
            for (int dy = -distance; dy <= distance; ++dy) {
                const int cheb = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                                   ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
                if (cheb != distance) continue;
                if (index < SOWING_MAX_EXTRA_TILES) {
                    offsets[index].dx = dx;
                    offsets[index].dy = dy;
                    ++index;
                }
            }
        }
    }
    // 按 Chebyshev 环 + Manhattan 距离排序（近的先种）
    for (size_t i = 1; i < index; ++i) {
        SowingTileOffset value = offsets[i];
        size_t j = i;
        while (j > 0) {
            int leftCheb = offsets[j-1].dx < 0 ? -offsets[j-1].dx : offsets[j-1].dx;
            int leftOther = offsets[j-1].dy < 0 ? -offsets[j-1].dy : offsets[j-1].dy;
            if (leftOther > leftCheb) { int t = leftCheb; leftCheb = leftOther; leftOther = t; }
            int valCheb = value.dx < 0 ? -value.dx : value.dx;
            int valOther = value.dy < 0 ? -value.dy : value.dy;
            if (valOther > valCheb) { int t = valCheb; valCheb = valOther; valOther = t; }
            int leftMan = leftCheb + leftOther;
            int valMan = valCheb + valOther;
            if (leftCheb > valCheb || (leftCheb == valCheb && leftMan > valMan)) {
                offsets[j] = offsets[j-1];
                --j;
            } else break;
        }
        offsets[j] = value;
    }
    *count = index;
    return index == (mode == 2 ? 24u : 8u);
}

// 45 度斜格基向量变换：rowDelta = dy - dx, colDelta = dy + dx
static bool RangeSowingGridKey(uint64_t centerKey, const SowingTileOffset& offset,
                                uint64_t* outKey) {
    const long long rowDelta = (long long)offset.dy - offset.dx;
    const long long colDelta = (long long)offset.dy + offset.dx;
    return SowingOffsetKeyNative(centerKey, rowDelta, colDelta, outKey);
}

// ============================================================
// 批量播种：RangeSowingSettleNativeExtras
// 中心格子由原生结算后，对周围格子同步批量结算。
// 每个额外格子：
//   1. 验证中心种子物品未变（索引/物品/head/field 全匹配）
//   2. g_sowValidateLand(mapInfo, key, field)  地块验证
//   3. RangeSowingGetSeedItem + g_sowSeedItemCheck  种子可用性
//   4. 写 [state+0x248]=key, [state+0x27C]=1
//   5. g_sowValidateState(state)  状态验证
//   6. g_sowSettle(state)  结算
//   7. 验证 count+1, 种子数量-1
// ============================================================
// 防卡顿：单格结算时间预算 + 续播队列
// 连通播种可能一次收集上百格。若在同一帧内同步结算全部，必定卡顿。
// 方案：每帧最多用掉 SOWING_FRAME_BUDGET_US（6ms）做结算，剩余格子存入
//       续播队列，由 mod_tick 的续播泵逐帧完成，直到播完或种子耗尽。
// ============================================================
struct RangeSowingResumeQueue {
    void*                        state = nullptr;   // 播种状态机指针（续播期间必须有效）
    SowingCenterItemSnapshot     item = {};         // 中心/物品快照
    uint64_t                     centerKey = 0;     // 中心格子 key
    uint64_t                     keys[SOWING_MAX_CONNECTED_TILES] = {};
    size_t                       count = 0;
    size_t                       cursor = 0;
    uint32_t                     delayFrames = 0;   // 入队后延迟 N 帧再续播，避免与播种帧重叠
    bool                         active = false;
};

static thread_local RangeSowingResumeQueue g_sowingResumeQueue;

static inline uint64_t SowingNowUs() {
    LARGE_INTEGER freq = {}, counter = {};
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) return 0;
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000ULL) / freq.QuadPart);
}

// 单格结算结果枚举
enum SowingSettleTileResult : int {
    kSettleOk = 0,          // 结算成功
    kSettleSkip = 1,        // 未结算（不可播/状态验证不通过），继续
    kSettleGateRejected = 2,// 种子不可用，终止
    kSettleItemChanged = 3, // 选中物品变化，终止
    kSettleSeedDepleted = 4,// 种子耗尽，终止
    kSettleMismatch = 5,    // 结算异常/验证不一致，终止
    kSettleBudgetExhausted = 6, // 时间预算耗尽，暂停（剩余格子留给续播）
};

// ============================================================
struct RangeSowingBatchResult {
    size_t attempted = 0;
    size_t accepted = 0;
    size_t settled = 0;
    bool nativeGateRejected = false;
    bool selectedItemChanged = false;
    bool seedStackDepleted = false;
    bool settlementMismatch = false;
};

static RangeSowingBatchResult RangeSowingSettleNativeExtras(
        void* state, int mode, uint64_t centerKey,
        const SowingCenterItemSnapshot& centerItem);

static bool RangeSowingSettleNativeExtrasImpl(
        void* state, int mode, uint64_t centerKey,
        const SowingCenterItemSnapshot& centerItem,
        const uint64_t* planKeys, size_t planCount, size_t startIdx,
        size_t& settledOut, bool& budgetExhaustedOut, size_t& stopIdxOut);

// ============================================================
// 连通播种：Flood-Fill 从中心向四邻域扩散
// 相邻地块通过 g_sowValidateLand(mapInfo, key, field) 判定是否可播。
//   可播  => 同类型耕地，纳入并继续向邻居扩散
//   不可播 => 草地 / 不同类型耕地，作为边界停止
// 上限 SOWING_MAX_CONNECTED_TILES（保护，防止超大田地一次播完）。
// 返回值：收集到的可播相邻地块 key 数量（含中心）。
//
// v1.3.5（用户反馈修复）：四邻域偏移坐标系错误——旧代码直接用世界轴
// 偏移 ±45（rowDeltas={-1,1,0,0}, colDeltas={0,0,-1,1}），但 45 度菱形
// 格子中心坐标是 (45(i+j), 45(j-i))，世界轴 ±45 落点解出 j'=j+0.5
// ——在两格中缝，不是任何格子中心（SowingOffsetKeyNative 往返校验
// 失败不入队 / validateLand 拒绝）→ Flood-Fill 永远只收集到中心一格
// → planCount=0 → settlementMismatch，连通模式对普通田地/田垄全失效。
// 修复：四邻域改为屏幕格子偏移 (dx,dy) ∈ {(±1,0),(0,±1)}，经与
// RangeSowingGridKey（3x3/5x5 已验证）相同的基向量变换
// rowDelta = dy - dx, colDelta = dy + dx 得到正确世界偏移。
// ============================================================
static size_t SowingBuildConnectedPlan(void* mapInfo, void* field,
                                        uint64_t centerKey,
                                        uint64_t* outKeys,
                                        size_t outCapacity) {
    if (!mapInfo || !field || !outKeys || outCapacity == 0 ||
        !G::sowValidateLand ||
        centerKey == 0 || centerKey == ~uint64_t{0})
        return 0;

    // 中心本身必须可播，否则没有扩散基础
    if (!SafeSowValidateLand(mapInfo, centerKey, field))
        return 0;

    // 已收集集合（线性搜索去重，零碰撞）
    // 256 格最多 256*255/2 = 32640 次比较，微秒级，且不会漏地块
    uint64_t seenKeys[SOWING_MAX_CONNECTED_TILES] = {};
    size_t seenCount = 0;

    // BFS 队列
    uint64_t queue[SOWING_MAX_CONNECTED_TILES * 4] = {};
    size_t queueHead = 0, queueTail = 0;
    queue[queueTail++] = centerKey;

    size_t count = 0;
    while (queueHead < queueTail && count < outCapacity) {
        const uint64_t cur = queue[queueHead++];
        if (cur == 0 || cur == ~uint64_t{0}) continue;

        // 线性搜索去重
        bool alreadySeen = false;
        for (size_t k = 0; k < seenCount; ++k) {
            if (seenKeys[k] == cur) { alreadySeen = true; break; }
        }
        if (alreadySeen) continue;
        if (seenCount < SOWING_MAX_CONNECTED_TILES) {
            seenKeys[seenCount++] = cur;
        }

        outKeys[count++] = cur;
        if (count >= outCapacity) break;

        // 四邻域：屏幕格子坐标偏移 → 45 度斜格基向量变换
        // （v1.3.5 修复：与 RangeSowingGridKey 同坐标系，世界轴直偏会落在格子中缝）
        static const int nbDx[4] = {1, -1, 0, 0};
        static const int nbDy[4] = {0, 0, 1, -1};
        for (int dir = 0; dir < 4; ++dir) {
            const long long rowDelta = (long long)nbDy[dir] - nbDx[dir];
            const long long colDelta = (long long)nbDy[dir] + nbDx[dir];
            uint64_t neighbor = 0;
            if (!SowingOffsetKeyNative(cur, rowDelta, colDelta, &neighbor))
                continue;
            // 邻居可播才入队（同类型耕地）
            if (!SafeSowValidateLand(mapInfo, neighbor, field))
                continue;
            // 检查邻居是否已入队或已收集
            bool nbSeen = false;
            for (size_t k = 0; k < seenCount; ++k) {
                if (seenKeys[k] == neighbor) { nbSeen = true; break; }
            }
            if (!nbSeen && queueTail < SOWING_MAX_CONNECTED_TILES * 4)
                queue[queueTail++] = neighbor;
        }
    }
    return count;
}

// SEH 安全包装：单独函数避免 C2712（对象展开与 __try 不兼容）
static bool SafeSowSettle(void* state) {
    __try {
        G::sowSettle(state);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// H10: SEH 安全包装——7 个原生函数调用异常保护
// 单独 noinline 函数避免 C2712（对象展开与 __try 不兼容）
__declspec(noinline) static void* SafeStateManagerLookup(void* owner) {
    __try {
        return G::stateManagerLookup(owner);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

__declspec(noinline) static bool SafeSowValidateLand(void* mapInfo, uint64_t key, void* field) {
    __try {
        return G::sowValidateLand(mapInfo, key, field);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) static bool SafeSowValidateState(void* state) {
    __try {
        return G::sowValidateState(state);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) static void* SafeSowSeedItemLookup(void* owner) {
    __try {
        return G::sowSeedItemLookup(owner);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

__declspec(noinline) static bool SafeSowSeedItemCheck(void* item, unsigned char a, unsigned char b) {
    __try {
        return G::sowSeedItemCheck(item, a, b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) static uint64_t SafeSowWorldToKey(const SowingWorldPoint* point) {
    __try {
        return G::sowWorldToKey(point);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

__declspec(noinline) static SowingWorldPoint* SafeSowKeyToWorld(SowingWorldPoint* point, uint64_t key) {
    __try {
        return G::sowKeyToWorld(point, key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static RangeSowingBatchResult RangeSowingSettleNativeExtras(
        void* state, int mode, uint64_t centerKey,
        const SowingCenterItemSnapshot& centerItem) {
    RangeSowingBatchResult result;
    if (!state || !G::sowValidateState || !G::sowSettle ||
        !G::sowRefreshLand || !G::sowValidateLand ||
        centerKey == 0 || centerKey == ~uint64_t{0}) {
        result.settlementMismatch = true;
        return result;
    }

    SowingReadRegionCache readCache = {};
    SowingReadRegionCacheScope readCacheScope(&readCache);

    void* manager = RangeSowingGetManager(state);
    void* mapInfo = SowingReadPointer(manager, MANAGER_MAPINFO_OFFSET);
    void* save = SowingGetSaveData();
    if (!manager || !mapInfo || !centerItem.valid) {
        result.settlementMismatch = true;
        return result;
    }
    // v1.3.25-diag: manager/mapInfo 健康度诊断（2s 限频）
    {
        static ULONGLONG s_mapInfoDiagTime = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_mapInfoDiagTime >= 2000) {
            s_mapInfoDiagTime = now;
            void* mi30 = (mapInfo && SowingIsReadable(reinterpret_cast<unsigned char*>(mapInfo) + 0x30, sizeof(void*)))
                         ? SowingReadPointer(mapInfo, 0x30) : nullptr;
#if DIAG_SOWER
            Log("[Sower] [diag] mapinfo: manager=%p mapInfo=%p [mI+0x30]=%p off=0x%llX\n",
                manager, mapInfo, mi30, (unsigned long long)MANAGER_MAPINFO_OFFSET);
#endif
            if (manager && SowingIsReadable(reinterpret_cast<unsigned char*>(manager) + 0x2F8, 0x50)) {
                for (uintptr_t off = 0x300; off <= 0x340; off += 8) {
                    uint64_t v = SowingReadU64(manager, off);
#if DIAG_SOWER
                    Log("[Sower] [diag] dump: m+0x%llX=%016llX\n",
                        (unsigned long long)off, (unsigned long long)v);
#endif
                }
            }
        }
    }
    // v1.3.15: save data 验证仅在 saveDataOk 时执行
    if (centerItem.saveDataOk) {
        if (!save || save != centerItem.save) {
            result.settlementMismatch = true;
            return result;
        }
        uint32_t liveSelectedIndex = 0;
        size_t liveUnlockedCapacity = 0;
        void* selectedSlotItem = nullptr;
        // v1.3.22: state+0x268 语义不稳定——模式1 实测为 key、模式2/3 实测为
        // item 指针（sd dump：模式1 0x268==centerKey，模式2/3 0x268=0x44EB37C0）。
        // 精确对比必误杀，放宽为合法值校验（非 0 非 -1），防竞态由
        // count/QUERY 回读/liveCenterInfo/SafeSow* 层承担（回归 v1.09 简洁哲学）
        uint64_t installedLandKey = 0;
        if (!SowingReadUnlockedInventoryCapacity(save, &liveUnlockedCapacity) ||
            !SowingTryReadU32(save, SAVE_SELECTED_ITEM_INDEX_OFFSET, &liveSelectedIndex) ||
            !SowingTryReadPointer(save,
                SAVE_INVENTORY_SLOTS_OFFSET + (size_t)centerItem.selectedIndex * sizeof(void*),
                &selectedSlotItem) ||
            !SowingTryReadU64(state, SOWING_STATE_LAND_OFFSET, &installedLandKey) ||
            installedLandKey == 0 || installedLandKey == ~uint64_t{0} ||
            liveUnlockedCapacity != centerItem.unlockedCapacity ||
            liveSelectedIndex != centerItem.selectedIndex ||
            selectedSlotItem != centerItem.item) {
            result.selectedItemChanged = true;
            return result;
        }
    } else {
        // v1.3.15: state-only 模式——v1.3.22: 0x268 语义不稳定，改合法值校验
        uint64_t installedLandKey = 0;
        if (!SowingTryReadU64(state, SOWING_STATE_LAND_OFFSET, &installedLandKey) ||
            installedLandKey == 0 || installedLandKey == ~uint64_t{0}) {
            result.selectedItemChanged = true;
            return result;
        }
    }
    const SowingLandInfo liveCenterInfo = SowingInspectLand(centerItem.item);
    if (!liveCenterInfo.readable ||
        liveCenterInfo.head != centerItem.info.head ||
        liveCenterInfo.action != ACTION_SOW ||
        liveCenterInfo.field != centerItem.info.field) {
        result.settlementMismatch = true;
        return result;
    }

    // state 保存/恢复由 Impl 负责

    // 构建格子计划：模式 1/2 用方格 offsets，模式 3（连通）用 Flood-Fill
    uint64_t planKeys[SOWING_MAX_TILES] = {};
    size_t planCount = 0;
    if (mode == 3) {
        // 连通播种：Flood-Fill 收集相邻同类型耕地（含中心，首元素即 centerKey）
        const size_t filled = SowingBuildConnectedPlan(
            mapInfo, centerItem.info.field, centerKey,
            planKeys, SOWING_MAX_TILES);
#if DIAG_SOWER
        Log("[Sower] [diag] connected plan: filled=%u center=0x%llX field=%p mapInfo=%p\n",
            (unsigned)filled, (unsigned long long)centerKey,
            centerItem.info.field, mapInfo);
#endif
        if (filled == 0) {
            result.settlementMismatch = true;
            return result;
        }
        // 排除中心（已由原生结算），其余邻居进入批量
        for (size_t i = 1; i < filled; ++i)
            planKeys[planCount++] = planKeys[i];
    } else {
        SowingTileOffset offsets[SOWING_MAX_EXTRA_TILES] = {};
        size_t tileCount = 0;
        if (!SowingBuildTilePlan(mode, offsets, &tileCount)) {
            result.settlementMismatch = true;
            return result;
        }
        for (size_t idx = 0; idx < tileCount; ++idx) {
            uint64_t key = 0;
            if (!RangeSowingGridKey(centerKey, offsets[idx], &key)) {
                result.settlementMismatch = true;
                break;
            }
            if (key != centerKey) planKeys[planCount++] = key;
        }
    }
    if (planCount == 0) {
        result.settlementMismatch = true;
        return result;
    }

    // 调用批量结算 Impl（state 保存/恢复由 Impl 负责）
    size_t settled = 0;
    bool budgetExhausted = false;
    size_t stopIdx = 0;
    RangeSowingSettleNativeExtrasImpl(state, mode, centerKey, centerItem,
        planKeys, planCount, 0,
        settled, budgetExhausted, stopIdx);
    result.settled = settled;
    result.attempted = planCount;
    if (budgetExhausted && stopIdx < planCount) {
        // v1.3.36 修复：限速后所有模式（1/2/3）都需要续播队列接力
        // 原条件 mode==3 只覆盖连通模式，3x3/5x5 每帧 1 格后剩余格被丢弃
        size_t remain = planCount - stopIdx;
        size_t keep = remain < SOWING_MAX_CONNECTED_TILES ? remain : SOWING_MAX_CONNECTED_TILES;
        g_sowingResumeQueue.state = state;
        g_sowingResumeQueue.item = centerItem;
        g_sowingResumeQueue.centerKey = centerKey;
        g_sowingResumeQueue.count = keep;
        g_sowingResumeQueue.cursor = 0;
        g_sowingResumeQueue.delayFrames = 2;
        g_sowingResumeQueue.active = true;
        for (size_t r = 0; r < keep; ++r)
            g_sowingResumeQueue.keys[r] = planKeys[stopIdx + r];
    }
    return result;
}
// 单格结算核心（不含状态恢复，由调用方负责）
static SowingSettleTileResult SowingSettleTileCore(
        void* state, uint64_t key,
        const SowingCenterItemSnapshot& centerItem,
        void* save, void* mapInfo,
        const SowingLandInfo& centerInfo) {
    if (!state || !mapInfo || !centerItem.valid)
        return kSettleMismatch;
    // v1.3.15: save data 验证仅在 saveDataOk 时执行
    // v1.3.21: state+0x268 语义随 phase 变化——phase 0(capture)为 land 指针、
    // phase 1(settle)为地块 key（sd dump 实锤 0x268==centerKey）。旧"item 指针
    // 一致性"对比在 phase 1 必失败 → 静默 return false / kSettleItemChanged，
    // 与日志（attempted=8 settled=0 全标记 0）完全吻合。改为合法 key 校验
    // （非 0 非 -1），竞态防护由 QUERY 回读/count/liveCenterInfo/SafeSow* 层承担。
    uint64_t curLandKey = 0;
    if (centerItem.saveDataOk) {
        if (!save) return kSettleMismatch;
        uint32_t curSelectedIndex = 0;
        size_t curUnlockedCapacity = 0;
        void* curSlotItem = nullptr;
        if (!SowingReadUnlockedInventoryCapacity(save, &curUnlockedCapacity) ||
            !SowingTryReadU32(save, SAVE_SELECTED_ITEM_INDEX_OFFSET, &curSelectedIndex) ||
            !SowingTryReadPointer(save,
                SAVE_INVENTORY_SLOTS_OFFSET + (size_t)centerItem.selectedIndex * sizeof(void*),
                &curSlotItem) ||
            !SowingTryReadU64(state, SOWING_STATE_LAND_OFFSET, &curLandKey) ||
            curLandKey == 0 || curLandKey == ~uint64_t{0}) {
            return kSettleMismatch;
        }
        if (curUnlockedCapacity != centerItem.unlockedCapacity ||
            curSelectedIndex != centerItem.selectedIndex ||
            curSlotItem != centerItem.item) {
            return kSettleItemChanged;
        }
    } else {
        // v1.3.15: state-only 模式——v1.3.21: 0x268 为 key，合法值校验
        if (!SowingTryReadU64(state, SOWING_STATE_LAND_OFFSET, &curLandKey) ||
            curLandKey == 0 || curLandKey == ~uint64_t{0}) {
            return kSettleItemChanged;
        }
    }
    // v1.3.21: 旧代码 SowingInspectLand(curStateItem)——phase 1 的 0x268 是 key
    // 非 item 指针，InspectLand(key) 必 readable=false 误报 mismatch。
    // 改用 centerItem.item（phase 0 捕获的 land 真指针，与 Impl 的 liveCenterInfo 同源）
    const SowingLandInfo curItemInfo = SowingInspectLand(centerItem.item);
    if (!curItemInfo.readable ||
        curItemInfo.head != centerItem.info.head ||
        curItemInfo.action != ACTION_SOW ||
        curItemInfo.field != centerItem.info.field) {
        return kSettleMismatch;
    }

    // 地块验证
    // v1.3.23-diag: kSettleSkip 三点定位诊断（一次实测区分失败点）
    if (!SafeSowValidateLand(mapInfo, key, centerItem.info.field)) {
        static ULONGLONG s_skipLandTime = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_skipLandTime >= 2000) {
            s_skipLandTime = now;
            void* mi30 = (mapInfo && SowingIsReadable(reinterpret_cast<unsigned char*>(mapInfo) + 0x30, sizeof(void*)))
                         ? SowingReadPointer(mapInfo, 0x30) : nullptr;
#if DIAG_SOWER
            Log("[Sower] [diag] tile skip: reason=validate_land key=0x%llX field=%p mapInfo=%p [mI+0x30]=%p\n",
                (unsigned long long)key, centerItem.info.field, mapInfo, mi30);
#endif
        }
        return kSettleSkip;
    }

    // 种子可用性检查
    void* seedItem = RangeSowingGetSeedItem(state);
    if (!RangeSowingCheckSeedItem(seedItem))
        return kSettleGateRejected;

    // 写入 query key 和 validated 标志
    SowingWriteU64(state, SOWING_STATE_QUERY_OFFSET, key);
    SowingWriteU8(state, SOWING_STATE_VALIDATED_OFFSET, 1);
    uint64_t installedQueryKey = 0;
    uint8_t installedValidation = 0;
    if (!SowingTryReadU64(state, SOWING_STATE_QUERY_OFFSET, &installedQueryKey) ||
        !SowingTryReadU8(state, SOWING_STATE_VALIDATED_OFFSET, &installedValidation) ||
        installedQueryKey != key || installedValidation != 1) {
        return kSettleMismatch;
    }

    // 状态验证
    if (!SafeSowValidateState(state)) {
        static ULONGLONG s_skipStateTime = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_skipStateTime >= 2000) {
            s_skipStateTime = now;
#if DIAG_SOWER
            Log("[Sower] [diag] tile skip: reason=validate_state key=0x%llX\n",
                (unsigned long long)key);
#endif
        }
        return kSettleSkip;
    }
    const uint64_t selectedKey = SowingReadU64(state, SOWING_STATE_SELECTED_KEY_OFFSET);
    if (selectedKey != key) {
        static ULONGLONG s_skipSelTime = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_skipSelTime >= 2000) {
            s_skipSelTime = now;
#if DIAG_SOWER
            Log("[Sower] [diag] tile skip: reason=selected_key key=0x%llX selected=0x%llX\n",
                (unsigned long long)key, (unsigned long long)selectedKey);
#endif
        }
        return kSettleSkip;
    }

    // 结算前再次验证物品
    // v1.3.21: 0x268 为 key（phase 1），settleState 指针对比必失败——改合法 key 校验
    if (centerItem.saveDataOk) {
        uint32_t settleSelIdx = 0;
        void* settleSlot = nullptr;
        uint64_t settleLandKey = 0;
        if (!SowingTryReadU32(save, SAVE_SELECTED_ITEM_INDEX_OFFSET, &settleSelIdx) ||
            !SowingTryReadPointer(save,
                SAVE_INVENTORY_SLOTS_OFFSET + (size_t)centerItem.selectedIndex * sizeof(void*),
                &settleSlot) ||
            !SowingTryReadU64(state, SOWING_STATE_LAND_OFFSET, &settleLandKey) ||
            settleLandKey == 0 || settleLandKey == ~uint64_t{0}) {
            return kSettleMismatch;
        }
        if (settleSelIdx != centerItem.selectedIndex ||
            settleSlot != centerItem.item) {
            return kSettleItemChanged;
        }
    } else {
        // v1.3.15: state-only 模式
        uint64_t settleLandKey = 0;
        if (!SowingTryReadU64(state, SOWING_STATE_LAND_OFFSET, &settleLandKey) ||
            settleLandKey == 0 || settleLandKey == ~uint64_t{0}) {
            return kSettleItemChanged;
        }
    }

    // 种子数量检查
    uint32_t seedQtyBefore = 0;
    uint8_t skipDeductBefore = 0;
    if (!SowingTryReadU32(centerItem.item, ITEM_STACK_COUNT_OFFSET, &seedQtyBefore) ||
        seedQtyBefore == 0) {
        return kSettleMismatch;
    }
    if (centerItem.saveDataOk) {
        if (!SowingTryReadU8(save, SAVE_SKIP_ITEM_DEDUCTION_OFFSET, &skipDeductBefore)) {
            return kSettleMismatch;
        }
    }
    // v1.3.15: state-only 模式下 skipDeductBefore=0（正常扣减模式）

    // 执行结算
    uint32_t countBefore = 0;
    if (!SowingTryReadU32(state, SOWING_STATE_COUNT_OFFSET, &countBefore)) {
        return kSettleMismatch;
    }
    if (!SafeSowSettle(state)) {
        return kSettleMismatch;
    }
    uint32_t countAfter = 0;
    if (!SowingTryReadU32(state, SOWING_STATE_COUNT_OFFSET, &countAfter) ||
        countAfter != countBefore + 1) {
        return kSettleMismatch;
    }

    // 结算后验证
    // v1.3.15: state-only 模式下简化验证（不依赖 save data 偏移）
    if (centerItem.saveDataOk) {
        uint32_t postSelIdx = 0;
        void* postSlot = nullptr;
        // v1.3.21: 0x268 在 phase 1 全程为 key，指针对比必失败——改合法 key 校验
        uint64_t postLandKey = 0;
        if (!SowingTryReadU32(save, SAVE_SELECTED_ITEM_INDEX_OFFSET, &postSelIdx) ||
            !SowingTryReadPointer(save,
                SAVE_INVENTORY_SLOTS_OFFSET + (size_t)centerItem.selectedIndex * sizeof(void*),
                &postSlot) ||
            !SowingTryReadU64(state, SOWING_STATE_LAND_OFFSET, &postLandKey) ||
            postLandKey == 0 || postLandKey == ~uint64_t{0}) {
            return kSettleMismatch;
        }
        uint8_t skipDeductAfter = 0;
        if (!SowingTryReadU8(save, SAVE_SKIP_ITEM_DEDUCTION_OFFSET, &skipDeductAfter) ||
            skipDeductAfter != skipDeductBefore) {
            return kSettleMismatch;
        }

        if (skipDeductBefore != 0) {
            // 不扣减模式：种子数量不变
            uint32_t seedQtyAfter = 0;
            if (postSelIdx != centerItem.selectedIndex ||
                postSlot != centerItem.item ||
                !SowingTryReadU32(centerItem.item, ITEM_STACK_COUNT_OFFSET, &seedQtyAfter) ||
                seedQtyAfter != seedQtyBefore) {
                return kSettleMismatch;
            }
            return kSettleOk;
        }

        if (seedQtyBefore == 1) {
            // 种子用完：原生会清空槽位
            if (postSlot == centerItem.item) {
                return kSettleMismatch;
            }
            return kSettleSeedDepleted;
        }

        // 正常扣减：种子数量应 -1
        uint32_t seedQtyAfter = 0;
        if (postSelIdx != centerItem.selectedIndex ||
            postSlot != centerItem.item ||
            !SowingTryReadU32(centerItem.item, ITEM_STACK_COUNT_OFFSET, &seedQtyAfter) ||
            seedQtyAfter != seedQtyBefore - 1) {
            return kSettleMismatch;
        }
        return kSettleOk;
    } else {
        // v1.3.15: state-only 模式——结算后只验证 count+1 和种子数量
        uint32_t seedQtyAfter = 0;
        if (!SowingTryReadU32(centerItem.item, ITEM_STACK_COUNT_OFFSET, &seedQtyAfter)) {
            return kSettleMismatch;
        }
        if (seedQtyBefore == 1) {
            // 种子用完
            return kSettleSeedDepleted;
        }
        // 正常扣减：种子数量应 -1
        if (seedQtyAfter != seedQtyBefore - 1) {
            return kSettleMismatch;
        }
        return kSettleOk;
    }
}

// 批量结算（带时间预算）：返回已结算数量和是否预算耗尽
// 若预算耗尽，剩余格子存入续播队列（仅连通模式）
static bool RangeSowingSettleNativeExtrasImpl(
        void* state, int mode, uint64_t centerKey,
        const SowingCenterItemSnapshot& centerItem,
        const uint64_t* planKeys, size_t planCount, size_t startIdx,
        size_t& settledOut, bool& budgetExhaustedOut, size_t& stopIdxOut) {
    settledOut = 0;
    budgetExhaustedOut = false;
    if (!state || !planKeys || planCount == 0 ||
        !G::sowValidateState || !G::sowSettle ||
        !G::sowRefreshLand || !G::sowValidateLand ||
        centerKey == 0 || centerKey == ~uint64_t{0}) {
        // v1.3.20: 前置检查失败诊断（v1.3.19 的后置诊断未打印说明在此就 return）
        static ULONGLONG s_preFailTime = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_preFailTime >= 2000) {
            s_preFailTime = now;
#if DIAG_SOWER
            Log("[Sower] [diag] settle pre-fail: state=%p planKeys=%p planCount=%zu "
                "validateState=%p settle=%p refreshLand=%p validateLand=%p "
                "centerKey=0x%llX\n",
                state, planKeys, planCount,
                G::sowValidateState, G::sowSettle,
                G::sowRefreshLand, G::sowValidateLand,
                (unsigned long long)centerKey);
#endif
        }
        return false;
    }

    SowingReadRegionCache readCache = {};
    SowingReadRegionCacheScope readCacheScope(&readCache);

    void* manager = RangeSowingGetManager(state);
    void* mapInfo = SowingReadPointer(manager, MANAGER_MAPINFO_OFFSET);
    void* save = SowingGetSaveData();
    if (!manager || !mapInfo || !centerItem.valid) {
        static ULONGLONG s_settleAbortTime = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_settleAbortTime >= 2000) {
            s_settleAbortTime = now;
#if DIAG_SOWER
            Log("[Sower] [diag] settle abort: manager=%p mapInfo=%p valid=%d\n",
                manager, mapInfo, (int)centerItem.valid);
#endif
        }
        return false;
    }
    // v1.3.15: save data 验证仅在 saveDataOk 时执行
    if (centerItem.saveDataOk) {
        // v1.3.21: state+0x268 (SOWING_STATE_LAND) 在 v1.20 存的是地块 key
        // 而非 item 指针（sd dump 实锤 0x268==batch key；capture 侧 0x248 QUERY
        // 同为 key 且已用 TryReadU64）。旧指针读法对 key 值必失败 → settle 在此
        // 静默 return false（v1.3.19/20 三个诊断点均无日志 + settled=0 佐证）。
        // 改为 key 对比 + 子条件诊断（一次实测定位失败子项）。
        const bool saveOk = save && save == centerItem.save;
        uint32_t liveSelectedIndex = 0;
        size_t liveUnlockedCapacity = 0;
        void* selectedSlotItem = nullptr;
        uint64_t installedLandKey = 0;
        const bool capOk =
            SowingReadUnlockedInventoryCapacity(save, &liveUnlockedCapacity) &&
            liveUnlockedCapacity == centerItem.unlockedCapacity;
        const bool selOk =
            SowingTryReadU32(save, SAVE_SELECTED_ITEM_INDEX_OFFSET,
                            &liveSelectedIndex) &&
            liveSelectedIndex == centerItem.selectedIndex;
        const bool slotOk = SowingTryReadPointer(
            save, SAVE_INVENTORY_SLOTS_OFFSET +
                      (size_t)centerItem.selectedIndex * sizeof(void*),
            &selectedSlotItem) && selectedSlotItem == centerItem.item;
        const bool landOk = SowingTryReadU64(state, SOWING_STATE_LAND_OFFSET,
                                             &installedLandKey) &&
                            installedLandKey != 0 &&
                            installedLandKey != ~uint64_t{0};
        if (!saveOk || !capOk || !selOk || !slotOk || !landOk) {
            static ULONGLONG s_settleSaveFailTime = 0;
            ULONGLONG now = GetTickCount64();
            if (now - s_settleSaveFailTime >= 2000) {
                s_settleSaveFailTime = now;
#if DIAG_SOWER
                Log("[Sower] [diag] settle saveData fail: save=%d cap=%d "
                    "sel=%d slot=%d land=%d landKey=0x%llX centerKey=0x%llX\n",
                    (int)saveOk, (int)capOk, (int)selOk, (int)slotOk,
                    (int)landOk, (unsigned long long)installedLandKey,
                    (unsigned long long)centerKey);
#endif
            }
            return false;
        }
    } else {
        // v1.3.15: state-only 模式
        // v1.3.22: 0x268 语义不稳定（模式1=key/模式2-3=item指针），改合法值校验
        uint64_t installedLandKey = 0;
        if (!SowingTryReadU64(state, SOWING_STATE_LAND_OFFSET,
                              &installedLandKey) ||
            installedLandKey == 0 || installedLandKey == ~uint64_t{0}) {
            static ULONGLONG s_settleLandFailTime = 0;
            ULONGLONG now = GetTickCount64();
            if (now - s_settleLandFailTime >= 2000) {
                s_settleLandFailTime = now;
#if DIAG_SOWER
                Log("[Sower] [diag] settle state-only land invalid: "
                    "landKey=0x%llX\n",
                    (unsigned long long)installedLandKey);
#endif
            }
            return false;
        }
    }
    const SowingLandInfo liveCenterInfo = SowingInspectLand(centerItem.item);
    if (!liveCenterInfo.readable ||
        liveCenterInfo.head != centerItem.info.head ||
        liveCenterInfo.action != ACTION_SOW ||
        liveCenterInfo.field != centerItem.info.field) {
        static ULONGLONG s_settleLiveTime = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_settleLiveTime >= 2000) {
            s_settleLiveTime = now;
#if DIAG_SOWER
            Log("[Sower] [diag] settle liveCenter mismatch: readable=%d "
                "liveHead=%p capHead=%p liveAction=0x%llx capField=%p liveField=%p\n",
                (int)liveCenterInfo.readable,
                liveCenterInfo.head, centerItem.info.head,
                (unsigned long long)liveCenterInfo.action,
                centerItem.info.field, liveCenterInfo.field);
#endif
        }
        return false;
    }

    // 保存原值以便恢复
    SowingQueryResult savedQuery = {};
    if (!SowingIsReadable(reinterpret_cast<unsigned char*>(state) + SOWING_STATE_QUERY_OFFSET,
                          sizeof(savedQuery))) {
        return false;
    }
    memcpy(&savedQuery,
           reinterpret_cast<unsigned char*>(state) + SOWING_STATE_QUERY_OFFSET,
           sizeof(savedQuery));
    uint64_t savedSelectedKey = 0;
    uint8_t savedValidation = 0;
    if (!SowingTryReadU64(state, SOWING_STATE_SELECTED_KEY_OFFSET, &savedSelectedKey) ||
        !SowingTryReadU8(state, SOWING_STATE_VALIDATED_OFFSET, &savedValidation)) {
        return false;
    }

    uint64_t seenKeys[SOWING_MAX_TILES] = {};
    size_t seenKeyCount = 0;

    g_insideRangeSowing = true;
    const uint64_t frameStart = SowingNowUs();
    // v1.3.35-diag: 播种结算耗时统计（定位卡顿）
    uint64_t settleSlowTotal = 0;
    uint64_t settleSlowMax = 0;
    size_t settleSlowCount = 0;
    size_t idx = startIdx;
    while (idx < planCount) {
        // v1.3.39: 回退限速——续播与播种状态机生命周期不匹配导致丢格；
        // 3x3/5x5 一次播完 222us 远低于 2ms 预算本就不卡，卡时走预算 break+续播泵
        const uint64_t key = planKeys[idx++];
        if (key == 0 || key == ~uint64_t{0}) continue;
        // 去重（不应与中心 key 重复）
        bool duplicateKey = (key == centerKey);
        for (size_t s = 0; s < seenKeyCount; ++s) {
            if (seenKeys[s] == key) { duplicateKey = true; break; }
        }
        if (duplicateKey || seenKeyCount >= SOWING_MAX_TILES) break;
        seenKeys[seenKeyCount++] = key;

        // 每结算 1 格后检查时间预算（首次不检查，保证至少播 1 格）
        if (idx > startIdx + 1 && frameStart != 0 && SowingNowUs() - frameStart > SOWING_FRAME_BUDGET_US) {
            --idx;  // M4: 回退到未结算的格子，避免跳过一格
            budgetExhaustedOut = true;
            break;
        }

        const uint64_t tTile0 = SowingNowUs();
        const SowingSettleTileResult code = SowingSettleTileCore(state, key, centerItem, save, mapInfo, liveCenterInfo);
        const uint64_t tTile1 = SowingNowUs();
        if (tTile1 - tTile0 > 1000) {
            ++settleSlowCount;
            settleSlowTotal += tTile1 - tTile0;
            if (tTile1 - tTile0 > settleSlowMax) settleSlowMax = tTile1 - tTile0;
        }
        switch (code) {
            case kSettleOk: ++settledOut; break;
            case kSettleSkip: break;
            case kSettleGateRejected: goto done_batch;
            case kSettleItemChanged: goto done_batch;
            case kSettleSeedDepleted: goto done_batch;
            case kSettleMismatch: goto done_batch;
            case kSettleBudgetExhausted: goto done_batch;
        }
    }

done_batch:
#if DIAG_SOWER
    Log("[Sower] [diag] settle batch: %d tiles in %llu us (budgetHit=%d slow>1ms:%u total=%llu max=%llu)\n",
        (int)settledOut, (unsigned long long)(SowingNowUs() - frameStart),
        (int)budgetExhaustedOut, (unsigned)settleSlowCount,
        (unsigned long long)settleSlowTotal, (unsigned long long)settleSlowMax);
#endif
    stopIdxOut = idx;
    // 恢复 state 临时字段
    memcpy(reinterpret_cast<unsigned char*>(state) + SOWING_STATE_QUERY_OFFSET,
           &savedQuery, sizeof(savedQuery));
    SowingWriteU64(state, SOWING_STATE_SELECTED_KEY_OFFSET, savedSelectedKey);
    SowingWriteU8(state, SOWING_STATE_VALIDATED_OFFSET, savedValidation);

    g_insideRangeSowing = false;
    return true;
}

// ============================================================
// 续播泵：mod_tick 中每帧调用，消费续播队列中剩余格子
// ============================================================
static void RangeSowingResumePump() {
    if (!g_sowingResumeQueue.active) return;

    // 延迟帧等待（避免与播种动画帧重叠）
    if (g_sowingResumeQueue.delayFrames > 0) {
        g_sowingResumeQueue.delayFrames--;
        return;
    }

    // ★ state 指针悬空防护：续播期间 state 可能已被游戏释放（如播种完成、场景切换）。
    // 使用前先做 IsReadable 检查，不可读则直接清除队列，避免对已释放内存操作导致崩溃。
    // 检查范围覆盖 Impl 内部读取的关键偏移（SOWING_STATE_QUERY_OFFSET + sizeof(SowingQueryResult)）。
    if (!g_sowingResumeQueue.state ||
        !SowingIsReadable(reinterpret_cast<unsigned char*>(g_sowingResumeQueue.state),
                          SOWING_STATE_QUERY_OFFSET + sizeof(SowingQueryResult))) {
        Log("[Sower] [resume] state 不可读（可能已释放），清除续播队列");
        g_sowingResumeQueue.active = false;
        g_sowingResumeQueue.state = nullptr;
        g_sowingResumeQueue.count = 0;
        g_sowingResumeQueue.cursor = 0;
        return;
    }

    size_t settled = 0;
    bool budgetExhausted = false;
    size_t stopIdx = 0;
    const bool ok = RangeSowingSettleNativeExtrasImpl(
        g_sowingResumeQueue.state, 3, g_sowingResumeQueue.centerKey,
        g_sowingResumeQueue.item,
        g_sowingResumeQueue.keys, g_sowingResumeQueue.count,
        g_sowingResumeQueue.cursor,
        settled, budgetExhausted, stopIdx);

    if (!ok) {
        // Impl 验证失败（state 无效等），清空队列
        g_sowingResumeQueue.active = false;
        g_sowingResumeQueue.count = 0;
        g_sowingResumeQueue.cursor = 0;
        return;
    }

    if (budgetExhausted && stopIdx < g_sowingResumeQueue.count) {
        // 仍有剩余格子，更新游标，下一帧继续
        g_sowingResumeQueue.cursor = stopIdx;
        g_sowingResumeQueue.delayFrames = 1;
        Log("[Sower] [resume] pumped %d, cursor=%d/%d\n", (int)settled,
            (int)stopIdx, (int)g_sowingResumeQueue.count);
    } else {
        // 全部完成（或种子耗尽等终止条件），清空队列
        g_sowingResumeQueue.active = false;
        g_sowingResumeQueue.count = 0;
        g_sowingResumeQueue.cursor = 0;
        Log("[Sower] [resume] done (settled=%d)\n", (int)settled);
    }
}

// ============================================================
// 原生批量更新：RangeSowingNativeBatchUpdate
// 在 hook detour 中调用，包装原生 Phase 0/1 更新：
//   - Phase 0 完成时捕获中心种子快照
//   - Phase 1 完成时（count+1）结算额外格子
// ============================================================
static bool RangeSowingNativeBatchUpdate(void* state, int mode) {
    SowingReadRegionCache updateReadCache = {};
    SowingReadRegionCacheScope updateReadCacheScope(&updateReadCache);

    const uint32_t phaseBefore = SowingReadU32(state, SOWING_STATE_PHASE_OFFSET);
    const uint32_t nextBefore = SowingReadU32(state, SOWING_STATE_NEXT_PHASE_OFFSET);
    const uint32_t countBefore = SowingReadU32(state, SOWING_STATE_COUNT_OFFSET);
    const uint64_t selectedBefore = SowingReadU64(state, SOWING_STATE_SELECTED_KEY_OFFSET);
    const uint8_t validationBefore = SowingReadU8(state, SOWING_STATE_VALIDATED_OFFSET);
    void* runnerBefore = SowingReadPointer(state, SOWING_STATE_RUNNER_OFFSET);
    void* taskBefore = SowingReadPointer(state, SOWING_STATE_TARGET_OFFSET);

    // 如果当前是 phase 1 且 next=-1（pending），尝试匹配已有的中心快照
    SowingCenterItemSnapshot centerItem = {};
    if (phaseBefore == 1 && nextBefore == 0xFFFFFFFFu) {
        const bool pendingMatches = g_sowingPendingCenterItem.armed &&
            g_sowingPendingCenterItem.state == state &&
            g_sowingPendingCenterItem.task == taskBefore &&
            g_sowingPendingCenterItem.runner == runnerBefore &&
            g_sowingPendingCenterItem.selectedKey == selectedBefore &&
            g_sowingPendingCenterItem.item.valid;
        centerItem = pendingMatches
            ? g_sowingPendingCenterItem.item
            : SowingCaptureCenterItem(state);
        if (!pendingMatches) {
            g_sowingPendingCenterItem = {};
            if (centerItem.valid && selectedBefore != 0 && selectedBefore != ~uint64_t{0}) {
                g_sowingPendingCenterItem.state = state;
                g_sowingPendingCenterItem.task = taskBefore;
                g_sowingPendingCenterItem.runner = runnerBefore;
                g_sowingPendingCenterItem.selectedKey = selectedBefore;
                g_sowingPendingCenterItem.item = centerItem;
                g_sowingPendingCenterItem.armed = true;
            }
        }
    }

    // 调用原生更新函数（Phase 0 或 Phase 1 原样执行）
    const bool nativeOk = G::originalSowingUpdate(state);
    updateReadCache.count = 0;
    updateReadCache.replacement = 0;

    // ---- Phase 0 返回 ----
    if (phaseBefore == 0) {
        const uint32_t phaseAfter = SowingReadU32(state, SOWING_STATE_PHASE_OFFSET);
        const uint32_t nextAfter = SowingReadU32(state, SOWING_STATE_NEXT_PHASE_OFFSET);
        const bool publishedToRunner = runnerBefore && phaseAfter == 0 && nextAfter == 1;
        const bool immediatelyCommitted = !runnerBefore && phaseAfter == 1 && nextAfter == 0xFFFFFFFFu;

        g_sowingPendingCenterItem = {};
        if (publishedToRunner || immediatelyCommitted) {
            const SowingCenterItemSnapshot publishedItem = SowingCaptureCenterItem(state);
            const uint64_t publishedKey = SowingReadU64(state, SOWING_STATE_SELECTED_KEY_OFFSET);
            if (publishedItem.valid && publishedKey != 0 && publishedKey != ~uint64_t{0}) {
                g_sowingPendingCenterItem.state = state;
                g_sowingPendingCenterItem.task = SowingReadPointer(state, SOWING_STATE_TARGET_OFFSET);
                g_sowingPendingCenterItem.runner = SowingReadPointer(state, SOWING_STATE_RUNNER_OFFSET);
                g_sowingPendingCenterItem.selectedKey = publishedKey;
                g_sowingPendingCenterItem.item = publishedItem;
                g_sowingPendingCenterItem.armed = true;
            }
        }
        return nativeOk;
    }

    // ---- 非 Phase 0 且非 Phase 1 ----
    if (phaseBefore != 1) {
        if (g_sowingPendingCenterItem.state == state)
            g_sowingPendingCenterItem = {};
        return nativeOk;
    }

    // ---- Phase 1 返回：检查中心是否结算成功 ----
    const uint32_t countAfter = SowingReadU32(state, SOWING_STATE_COUNT_OFFSET);
    const uint32_t nextAfter = SowingReadU32(state, SOWING_STATE_NEXT_PHASE_OFFSET);
    const uint32_t phaseAfter = SowingReadU32(state, SOWING_STATE_PHASE_OFFSET);
    const bool publishedToRunner = runnerBefore && phaseAfter == 1 && nextAfter == 0;
    const bool immediatelyCommitted = !runnerBefore && phaseAfter == 0 && nextAfter == 0xFFFFFFFFu;

    // v1.3.13: v1.20 诊断实锤——next_phase 并非始终 FFFFFFFF：
    // 播种后 nextBefore=FFFFFFFF→nextAfter=0, count 0→1, published=1。
    // v1.3.12 逻辑反转 bug：else 分支忘了取反"未结算"条件→已结算被当未结算。
    // 修正：nextPhaseDead 仅在 before+after 均 FFFFFFFF 时为真（真正失效）；
    // else 分支用原始条件的取反（即"已结算"= nextBefore==pending && count++ && (published||committed)）。
    const bool nextPhaseDead = (nextBefore == 0xFFFFFFFFu && nextAfter == 0xFFFFFFFFu);
    const bool centerSettled = nextPhaseDead
        ? (countAfter == countBefore + 1)
        : (nextBefore == 0xFFFFFFFFu && countAfter == countBefore + 1 &&
           (publishedToRunner || immediatelyCommitted));

    if (!centerSettled) {
        const bool stillWaiting = nextBefore == 0xFFFFFFFFu && countAfter == countBefore &&
                                   phaseAfter == 1 && nextAfter == 0xFFFFFFFFu;
        // v1.3.10-diag: 中心结算未成功的原因（限频 1s）——区分状态机结构漂移
        if (!stillWaiting) {
            static ULONGLONG s_lastFailDiag = 0;
            const ULONGLONG nowFailDiag = GetTickCount64();
            if (nowFailDiag - s_lastFailDiag >= 1000) {
                s_lastFailDiag = nowFailDiag;
#if DIAG_SOWER
                Log("[Sower] [diag] center not settled: phase=%u->%u next=%u->%u "
                    "count=%u->%u runner=%p published=%d committed=%d",
                    phaseBefore, phaseAfter, nextBefore, nextAfter,
                    countBefore, countAfter, runnerBefore,
                    (int)publishedToRunner, (int)immediatelyCommitted);
#endif
            }
        }
        if (!stillWaiting && g_sowingPendingCenterItem.state == state)
            g_sowingPendingCenterItem = {};
        return nativeOk;
    }

    // 中心结算成功，消费快照
    if (g_sowingPendingCenterItem.state == state)
        g_sowingPendingCenterItem = {};

    const uint64_t centerKey = (selectedBefore != 0 && selectedBefore != ~uint64_t{0})
        ? selectedBefore
        : SowingReadU64(state, SOWING_STATE_SELECTED_KEY_OFFSET);
    if (centerKey == 0 || centerKey == ~uint64_t{0}) {
#if DIAG_SOWER
        Log("[Sower] [diag] centerKey invalid=0x%llX\n", (unsigned long long)centerKey);
#endif
        return nativeOk;
    }
    if (!centerItem.valid) {
#if DIAG_SOWER
        Log("[Sower] [diag] centerSettled but centerItem invalid\n");
#endif
        return nativeOk;
    }

    // 执行额外格子批量结算
    RangeSowingBatchResult batch =
        RangeSowingSettleNativeExtras(state, mode, centerKey, centerItem);

    Log("[Sower] batch_end mode=%d key=0x%llX attempted=%zu settled=%zu "
        "gate_rejected=%d item_changed=%d seed_depleted=%d mismatch=%d",
        mode, (unsigned long long)centerKey,
        batch.attempted, batch.settled,
        batch.nativeGateRejected ? 1 : 0,
        batch.selectedItemChanged ? 1 : 0,
        batch.seedStackDepleted ? 1 : 0,
        batch.settlementMismatch ? 1 : 0);

    return nativeOk;
}

// ============================================================
// Detour 函数
// ============================================================
static bool __fastcall SowingUpdateDetour(void* state) {
    // v1.3.10-diag: detour 入口限频日志（≥1s 一条）——验证 hook 是否被调用
    // + 打印状态机现场值（phase/next/count），一轮播种即可区分
    // "调用链断裂"（无此日志）vs "结构偏移漂移"（日志出现但值异常）。
    {
        static ULONGLONG s_lastEntryDiag = 0;
        const ULONGLONG nowEntryDiag = GetTickCount64();
        if (nowEntryDiag - s_lastEntryDiag >= 1000) {
            s_lastEntryDiag = nowEntryDiag;
#if DIAG_SOWER
            Log("[Sower] [diag] detour: mode=%d state=%p phase=%u next=%u count=%u",
                (int)G::rangeSowingMode.load(std::memory_order_relaxed), state,
                state ? SowingReadU32(state, SOWING_STATE_PHASE_OFFSET) : 0u,
                state ? SowingReadU32(state, SOWING_STATE_NEXT_PHASE_OFFSET) : 0u,
                state ? SowingReadU32(state, SOWING_STATE_COUNT_OFFSET) : 0u);
#endif
            // v1.3.11-diag: state 窗口 dump（0x1C0-0x2BF，8 行×8 值）
            if (state) {
                for (uintptr_t row = 0x1C0; row <= 0x2A0; row += 0x20) {
#if DIAG_SOWER
                    Log("[Sower] [diag] sd[%03X]: %08X %08X %08X %08X %08X %08X %08X %08X",
                        (unsigned)row,
                        SowingReadU32(state, row + 0x00), SowingReadU32(state, row + 0x04),
                        SowingReadU32(state, row + 0x08), SowingReadU32(state, row + 0x0C),
                        SowingReadU32(state, row + 0x10), SowingReadU32(state, row + 0x14),
                        SowingReadU32(state, row + 0x18), SowingReadU32(state, row + 0x1C));
#endif
                }
            }
        }
    }
    const int mode = G::rangeSowingMode.load(std::memory_order_relaxed);
    if (g_insideRangeSowing) {
        // v1.3.35-diag: 重入事件计数（递归播种 = 卡顿头号嫌疑）
#if DIAG_SOWER
        static ULONGLONG sReentryDiag = 0;
        ULONGLONG nowR = GetTickCount64();
        if (nowR - sReentryDiag >= 2000) {
            sReentryDiag = nowR;
            Log("[Sower] [diag] REENTRY 递归播种重入被拦截\n");
        }
#endif
        return false;
    }
    if (mode <= 0 || !G::rangeSowingReady || !G::originalSowingUpdate) {
        g_sowingPendingCenterItem = {};
        return G::originalSowingUpdate ? G::originalSowingUpdate(state) : false;
    }
    if (!G::stateManagerLookup || !G::sowSelectTarget ||
        !G::sowValidateLand || !G::sowRefreshLand ||
        !G::sowSeedItemLookup || !G::sowSeedItemCheck ||
        !G::sowWorldToKey || !G::sowKeyToWorld) {
        g_sowingPendingCenterItem = {};
        return G::originalSowingUpdate(state);
    }
    if (G::rangeSowingNativeBatchReady && G::sowValidateState && G::sowSettle) {
        // v1.3.40-diag: detour 主体实时耗时（定位卡顿是否在 batch 结算内）
        const uint64_t dT0 = SowingNowUs();
        const bool dR = RangeSowingNativeBatchUpdate(state, mode);
        const uint64_t dT1 = SowingNowUs();
        Log("[Sower] [diag] detour batch took %llu us\n", (unsigned long long)(dT1 - dT0));
        return dR;
    }
    // v1.3.14-diag: 记录为何未进入 batch update
#if DIAG_SOWER
    Log("[Sower] [diag] detour fallback: batchReady=%d validateState=%p settle=%p\n",
        (int)G::rangeSowingNativeBatchReady, G::sowValidateState, G::sowSettle);
#endif
    g_sowingPendingCenterItem = {};
    return G::originalSowingUpdate(state);
}

// ============================================================
// 手柄 D-pad 支持（三路混合：XInput + HID + joyGetPosEx）
// 复用 ChestSort 成熟方案：XInput 独立线程 + HID 独立线程 + joyGetPosEx 同步回退
// ============================================================

// ---- XInput 定义（不依赖 <xinput.h>，避免 SDK 版本差异）----
#pragma pack(push, 4)
struct XINPUT_GAMEPAD {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};
struct XINPUT_STATE {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
};
#pragma pack(pop)
#define XINPUT_GAMEPAD_DPAD_LEFT 0x0004

static void LoadXInput() {
    if (g_XInputGetState) return;
    HMODULE hLib = LoadLibraryW(L"xinput1_4.dll");
    if (!hLib) hLib = LoadLibraryW(L"xinput1_3.dll");
    if (!hLib) hLib = LoadLibraryW(L"xinput9_1_0.dll");
    if (hLib) {
        g_XInputGetState = reinterpret_cast<DWORD(WINAPI*)(DWORD, void*)>(
            GetProcAddress(hLib, "XInputGetState"));
    }
}

static DWORD WINAPI XInputPollThread(LPVOID) {
    Log("[Sower] [xinput] 线程启动");
    bool wasConnected = false;
    while (InterlockedCompareExchange(&g_xinputThreadRunning, 1, 1) == 1) {
        if (!g_XInputGetState) { Sleep(2000); continue; }
        XINPUT_STATE state = {};
        DWORD result = g_XInputGetState(0, &state);
        if (result == ERROR_SUCCESS) {
            bool dpadLeft = (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
            InterlockedExchange(&g_xinputDpadLeft, dpadLeft ? 1 : 0);
            if (!wasConnected) { wasConnected = true; Log("[Sower] [xinput] 手柄已连接"); }
        } else {
            InterlockedExchange(&g_xinputDpadLeft, 0);
            if (wasConnected) { wasConnected = false; Log("[Sower] [xinput] 手柄断开 (err=%lu)", result); }
        }
        Sleep(8);  // ~120Hz 采样
    }
    Log("[Sower] [xinput] 线程退出");
    return 0;
}

// ---- DualSense HID 直读 ----
static HANDLE OpenDualSense() {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return nullptr;

    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &hidGuid, i, &ifData); i++) {
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, nullptr, 0, &requiredSize, nullptr);
        if (requiredSize == 0) continue;
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(requiredSize);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, requiredSize, nullptr, nullptr)) {
            free(detail); continue;
        }
        HANDLE hDev = CreateFileW(detail->DevicePath,
            GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        free(detail);
        if (hDev == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr = {};
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(hDev, &attr) &&
            attr.VendorID == DUALSENSE_VID && attr.ProductID == DUALSENSE_PID) {
            SetupDiDestroyDeviceInfoList(hDevInfo);
            return hDev;
        }
        CloseHandle(hDev);
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return nullptr;
}

static DWORD WINAPI HidPollThread(LPVOID) {
    Log("[Sower] [hid] 线程启动");
    bool wasConnected = false;
    while (InterlockedCompareExchange(&g_hidThreadRunning, 1, 1) == 1) {
        HANDLE hDev = OpenDualSense();
        InterlockedExchangePointer(&g_hidDevice, hDev);
        if (!hDev) {
            if (wasConnected) {
                wasConnected = false;
                InterlockedExchange(&g_hidConnected, 0);
                InterlockedExchange(&g_hidDpadLeft, 0);
                Log("[Sower] [hid] DualSense 断开，等待重连...");
            }
            Sleep(2000);
            continue;
        }
        if (!wasConnected) {
            wasConnected = true;
            InterlockedExchange(&g_hidConnected, 1);
            Log("[Sower] [hid] DualSense 已连接");
        }
        BYTE report[DUALSENSE_REPORT_SIZE] = {};
        while (InterlockedCompareExchange(&g_hidThreadRunning, 1, 1) == 1) {
            __try {
                DWORD bytesRead = 0;
                BOOL ok = ReadFile(hDev, report, DUALSENSE_REPORT_SIZE, &bytesRead, nullptr);
                if (!ok || bytesRead < 9) break;
                if (report[0] == 0x01 && bytesRead > DUALSENSE_DPAD_OFFSET) {
                    int hat = report[DUALSENSE_DPAD_OFFSET] & 0x0F;
                    // D-pad Left: hat 5(SW) 或 6(W) 或 7(NW)
                    bool dpadLeft = (hat >= 5 && hat <= 7);
                    InterlockedExchange(&g_hidDpadLeft, dpadLeft ? 1 : 0);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedExchange(&g_hidDpadLeft, 0);
                break;
            }
        }
        CloseHandle(hDev);
        InterlockedExchangePointer(&g_hidDevice, nullptr);
        InterlockedExchange(&g_hidConnected, 0);
        InterlockedExchange(&g_hidDpadLeft, 0);
    }
    Log("[Sower] [hid] 线程退出");
    return 0;
}

// ---- joyGetPosEx 辅助 ----
static void (WINAPI *g_joyConfigChanged)(void) = nullptr;
static void LoadJoyConfigChanged() {
    if (g_joyConfigChanged) return;
    HMODULE hWinmm = GetModuleHandleW(L"winmm.dll");
    if (!hWinmm) hWinmm = LoadLibraryW(L"winmm.dll");
    if (hWinmm) {
        g_joyConfigChanged = reinterpret_cast<void(WINAPI*)(void)>(
            GetProcAddress(hWinmm, "joyConfigChanged"));
    }
}

static void InitGamepad() {
    // 1. XInput 独立线程（Xbox 手柄）
    LoadXInput();
    if (g_XInputGetState) {
        InterlockedExchange(&g_xinputThreadRunning, 1);
        g_xinputThread = CreateThread(nullptr, 0, XInputPollThread, nullptr, 0, nullptr);
        if (g_xinputThread) G::xinputReady = true;
    }

    // 2. HID 独立线程（DualSense 直读）
    InterlockedExchange(&g_hidThreadRunning, 1);
    g_hidThread = CreateThread(nullptr, 0, HidPollThread, nullptr, 0, nullptr);
    if (g_hidThread) G::hidReady = true;

    // 3. joyGetPosEx 探测（其他 DirectInput 手柄）
    LoadJoyConfigChanged();
    JOYINFOEX ji = {}; ji.dwSize = sizeof(ji); ji.dwFlags = JOY_RETURNPOV;
    G::padReady = (joyGetPosEx(JOYSTICKID1, &ji) == JOYERR_NOERROR);
}

// D-pad Left 边沿触发（三路聚合）
static bool CheckDpadLeftPressed() {
    bool dpadLeft = false;
    if (G::xinputReady && InterlockedOr(&g_xinputDpadLeft, 0)) dpadLeft = true;
    if (!dpadLeft && G::hidReady && InterlockedOr(&g_hidConnected, 0))
        dpadLeft = InterlockedOr(&g_hidDpadLeft, 0) != 0;
    if (!dpadLeft && !G::xinputReady && !(G::hidReady && InterlockedOr(&g_hidConnected, 0))) {
        JOYINFOEX ji = {}; ji.dwSize = sizeof(ji); ji.dwFlags = JOY_RETURNPOV;
        MMRESULT res = joyGetPosEx(JOYSTICKID1, &ji);
        if (res == JOYERR_NOERROR) {
            if (!G::padReady) { G::padReady = true; G::padReconnectTimer = 0; G::dpadLeftNeedsRelease = true; }
            DWORD pov = ji.dwPOV;
            // 左扇区：POV 中心 22500，±4500
            dpadLeft = (pov != 0xFFFF && pov != JOY_POVCENTERED) &&
                       (pov >= 18000 && pov <= 27000);
        } else {
            if (G::padReady) { G::padReady = false; G::padReconnectTimer = 0; }
            G::padReconnectTimer++;
            if (G::padReconnectTimer % 120 == 0 && g_joyConfigChanged) g_joyConfigChanged();
        }
    }
    bool pressed = false;
    if (G::dpadLeftNeedsRelease) { if (!dpadLeft) G::dpadLeftNeedsRelease = false; }
    else if (dpadLeft) { pressed = true; G::dpadLeftNeedsRelease = true; }
    return pressed;
}

// ============================================================
// 防重复加载检测（复用 Scarecrow v1.0.2 方案）
// ============================================================
static HMODULE g_sowerModuleForDup = nullptr;  // DllMain 记录
static bool IsDuplicateInstance() {
    HMODULE first = GetModuleHandleW(L"sower.dll");
    return (first != nullptr && first != g_sowerModuleForDup);
}

// ============================================================
// Hook 安装
// ============================================================

// 22 字节序言签名
static const unsigned char EXPECTED_UPDATE_PROLOGUE[SOWING_HOOK_LENGTH] = {
    0x48, 0x8B, 0xC4, 0x48,
    0x89, 0x58, 0x10, 0x48,
    0x89, 0x70, 0x18, 0x55,
    0x57, 0x41, 0x56, 0x48,
    0x8D, 0xA8, 0x18, 0xFF,
    0xFF, 0xFF
};

// 关键函数头部签名（轻量验证）
static const unsigned char EXPECTED_STATE_LOOKUP[12] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20,
    0x48, 0x8B, 0xD9, 0xE8, 0x42, 0x43
};
static const unsigned char EXPECTED_SELECT_TARGET[17] = {
    0x48, 0x89, 0x5C, 0x24, 0x18,
    0x55, 0x56, 0x57,
    0x48, 0x83, 0xEC, 0x60,
    0x0F, 0x29, 0x74, 0x24, 0x50
};
static const unsigned char EXPECTED_VALIDATE_LAND[24] = {
    0x48, 0x89, 0x5C, 0x24, 0x20,
    0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0xAC, 0x24, 0xF0, 0xFD, 0xFF, 0xFF
};
static const unsigned char EXPECTED_REFRESH_LAND[32] = {
    0x48, 0x83, 0xEC, 0x28,
    0x4C, 0x8B, 0xC9,
    0x48, 0x8B, 0x05, 0x92, 0xAA, 0xEE, 0x00,
    0x48, 0x8B, 0x90, 0x08, 0x02, 0x00, 0x00,
    0x48, 0x63, 0x82, 0xB0, 0x33, 0x00, 0x00,
    0x4C, 0x8B, 0x84, 0xC2
};
static const unsigned char EXPECTED_SEED_ITEM_LOOKUP[14] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20,
    0x48, 0x8B, 0xD9, 0xE8, 0x02, 0x69, 0x5E, 0x00
};
static const unsigned char EXPECTED_SEED_ITEM_CHECK[24] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20,
    0x48, 0x8B, 0x41, 0x08, 0x0F, 0xB6, 0xDA,
    0x48, 0x85, 0xC0, 0x0F, 0x84, 0x63, 0x01,
    0x00, 0x00, 0x80, 0xB8
};
static const unsigned char EXPECTED_VALIDATE_STATE[24] = {
    0x40, 0x55, 0x57, 0x41, 0x55,
    0x48, 0x8D, 0x6C, 0x24, 0xB9,
    0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00,
    0x45, 0x33, 0xED,
    0x48, 0x8B, 0xF9,
    0x4C
};
static const unsigned char EXPECTED_SETTLE[30] = {
    0x48, 0x8B, 0xC4,
    0x48, 0x89, 0x58, 0x10,
    0x48, 0x89, 0x70, 0x18,
    0x48, 0x89, 0x78, 0x20,
    0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0x6C, 0x24, 0x90,
    0x48
};

static bool VerifyBytes(uintptr_t base, uintptr_t rva,
                        const unsigned char* expected, size_t len) {
    return memcmp(reinterpret_cast<const void*>(base + rva), expected, len) == 0;
}

static bool InstallRangeSowingHook() {
    G::rangeSowingReady = false;
    G::rangeSowingNativeBatchReady = false;
    G::originalSowingUpdate = nullptr;

    const uintptr_t base = G::base;

    // 1. 验证关键函数字节签名
    if (!VerifyBytes(base, RVA_SOWING_UPDATE, EXPECTED_UPDATE_PROLOGUE, SOWING_HOOK_LENGTH) ||
        !VerifyBytes(base, RVA_STATE_MANAGER_LOOKUP, EXPECTED_STATE_LOOKUP, sizeof(EXPECTED_STATE_LOOKUP)) ||
        !VerifyBytes(base, RVA_SOW_SELECT_TARGET, EXPECTED_SELECT_TARGET, sizeof(EXPECTED_SELECT_TARGET)) ||
        !VerifyBytes(base, RVA_SOW_VALIDATE_LAND, EXPECTED_VALIDATE_LAND, sizeof(EXPECTED_VALIDATE_LAND)) ||
        !VerifyBytes(base, RVA_SOW_REFRESH_LAND, EXPECTED_REFRESH_LAND, sizeof(EXPECTED_REFRESH_LAND)) ||
        !VerifyBytes(base, RVA_SOW_SEED_ITEM_LOOKUP, EXPECTED_SEED_ITEM_LOOKUP, sizeof(EXPECTED_SEED_ITEM_LOOKUP)) ||
        !VerifyBytes(base, RVA_SOW_SEED_ITEM_CHECK, EXPECTED_SEED_ITEM_CHECK, sizeof(EXPECTED_SEED_ITEM_CHECK)) ||
        !VerifyBytes(base, RVA_SOW_VALIDATE_STATE, EXPECTED_VALIDATE_STATE, sizeof(EXPECTED_VALIDATE_STATE)) ||
        !VerifyBytes(base, RVA_SOW_SETTLE, EXPECTED_SETTLE, sizeof(EXPECTED_SETTLE))) {
        Log("[Sower] [hook] 字节签名验证失败，hook 未安装");
        return false;
    }
    Log("[Sower] [hook] 所有字节签名验证通过");

    // 2. 分配跳板页（64 字节）
    unsigned char* trampoline = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!trampoline) {
        Log("[Sower] [hook] 跳板分配失败 (err=%lu)", GetLastError());
        return false;
    }
    g_sowingTrampoline = trampoline;  // 保存供 unload 时释放

    // 3. 跳板 = 22 字节序言副本 + 14 字节绝对跳转回 target+22
    memcpy(trampoline, EXPECTED_UPDATE_PROLOGUE, SOWING_HOOK_LENGTH);
    unsigned char jumpBack[14] = {0xFF, 0x25, 0, 0, 0, 0};
    *reinterpret_cast<uint64_t*>(jumpBack + 6) =
        (uint64_t)(base + RVA_SOWING_UPDATE + SOWING_HOOK_LENGTH);
    memcpy(trampoline + SOWING_HOOK_LENGTH, jumpBack, sizeof(jumpBack));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);

    // 4. 跳板改为只读可执行
    DWORD oldProtect = 0;
    if (!VirtualProtect(trampoline, 64, PAGE_EXECUTE_READ, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("[Sower] [hook] 跳板保护失败");
        return false;
    }

    G::originalSowingUpdate = reinterpret_cast<SowingUpdateFunction>(trampoline);

    // 5. 绑定函数指针
    G::stateManagerLookup = reinterpret_cast<StateManagerLookupFunction>(base + RVA_STATE_MANAGER_LOOKUP);
    G::sowSelectTarget   = reinterpret_cast<SowingSelectTargetFunction>(base + RVA_SOW_SELECT_TARGET);
    G::sowValidateLand   = reinterpret_cast<SowingValidateLandFunction>(base + RVA_SOW_VALIDATE_LAND);
    G::sowRefreshLand    = reinterpret_cast<SowingRefreshLandFunction>(base + RVA_SOW_REFRESH_LAND);
    G::sowValidateState  = reinterpret_cast<SowingValidateStateFunction>(base + RVA_SOW_VALIDATE_STATE);
    G::sowSettle         = reinterpret_cast<SowingSettleFunction>(base + RVA_SOW_SETTLE);
    G::sowSeedItemLookup = reinterpret_cast<SowingSeedItemLookupFunction>(base + RVA_SOW_SEED_ITEM_LOOKUP);
    G::sowSeedItemCheck  = reinterpret_cast<SowingSeedItemCheckFunction>(base + RVA_SOW_SEED_ITEM_CHECK);
    G::sowWorldToKey     = reinterpret_cast<SowingWorldToKeyFunction>(base + RVA_SOW_WORLD_TO_KEY);
    G::sowKeyToWorld     = reinterpret_cast<SowingKeyToWorldFunction>(base + RVA_SOW_KEY_TO_WORLD);

    // 6. 写入 hook：14 字节绝对跳转 + NOP 填充到 22 字节
    unsigned char* updateTarget = reinterpret_cast<unsigned char*>(base + RVA_SOWING_UPDATE);
    unsigned char hook[SOWING_HOOK_LENGTH] = {0xFF, 0x25, 0, 0, 0, 0};
    *reinterpret_cast<uint64_t*>(hook + 6) =
        reinterpret_cast<uint64_t>(&SowingUpdateDetour);
    for (size_t i = 14; i < SOWING_HOOK_LENGTH; ++i)
        hook[i] = 0x90;  // NOP

    if (!WriteMem(updateTarget, hook, sizeof(hook))) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        G::originalSowingUpdate = nullptr;
        Log("[Sower] [hook] hook 写入失败");
        return false;
    }

    G::rangeSowingNativeBatchReady = true;
    G::rangeSowingReady = true;
    G::rangeSowingMode.store(0, std::memory_order_relaxed);
    Log("[Sower] [hook] 安装成功，初始模式 1x1（原生）");
    return true;
}

// ============================================================
// HUD 浮现窗口：左上角显示当前播种模式，5秒后自动消退
// ============================================================
static HMODULE g_sowerModule = nullptr;
static constexpr wchar_t SOWER_HUD_CLASS[] = L"SowerHudWindow";
static HWND g_hudWindow = nullptr;
static HFONT g_hudFont = nullptr;
static HFONT g_hudFontSmall = nullptr;
static int g_hudMode = -1;
static ULONGLONG g_hudHideAt = 0;

// HUD 画刷（文件作用域，unload 可清理）
static HBRUSH g_hudBgBrush = nullptr;
static HBRUSH g_hudAccentBrushes[4] = {};

static LRESULT CALLBACK HudWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(window, &paint);
        RECT client = {};
        GetClientRect(window, &client);

        // 背景（缓存画刷，避免每帧创建/销毁）
        if (!g_hudBgBrush) g_hudBgBrush = CreateSolidBrush(RGB(28, 30, 34));
        FillRect(dc, &client, g_hudBgBrush);

        // 左侧强调色条（4 色缓存）
        if (!g_hudAccentBrushes[0]) {
            g_hudAccentBrushes[0] = CreateSolidBrush(RGB(120, 130, 140));  // mode 0
            g_hudAccentBrushes[1] = CreateSolidBrush(RGB(91, 192, 122));    // mode 1
            g_hudAccentBrushes[2] = CreateSolidBrush(RGB(230, 145, 50));    // mode 2
            g_hudAccentBrushes[3] = CreateSolidBrush(RGB(140, 120, 230));   // mode 3
        }
        const int mode = g_hudMode;
        RECT bar = client;
        bar.right = bar.left + 6;
        int accentIdx = (mode >= 0 && mode <= 3) ? mode : 0;
        FillRect(dc, &bar, g_hudAccentBrushes[accentIdx]);

        SetBkMode(dc, TRANSPARENT);

        SetTextColor(dc, RGB(150, 150, 155));
        HFONT oldFont = (HFONT)SelectObject(dc, g_hudFontSmall);
        RECT titleRect = client;
        titleRect.left += 22;
        titleRect.right -= 14;
        titleRect.bottom = titleRect.top + 22;
        DrawTextW(dc, L"播种模式", -1, &titleRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        const wchar_t* modeText;
        COLORREF modeColor;
        switch (mode) {
            case 3:  modeText = L"连通播种"; modeColor = RGB(180, 170, 240); break;
            case 2:  modeText = L"5×5 范围"; modeColor = RGB(230, 180, 80); break;
            case 1:  modeText = L"3×3 范围"; modeColor = RGB(130, 220, 150); break;
            default: modeText = L"1×1 原生"; modeColor = RGB(200, 200, 210); break;
        }
        SelectObject(dc, g_hudFont);
        SetTextColor(dc, modeColor);
        RECT modeRect = client;
        modeRect.left += 22;
        modeRect.right -= 14;
        modeRect.top += 24;
        DrawTextW(dc, modeText, -1, &modeRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        SelectObject(dc, oldFont);
        EndPaint(window, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

static bool InitHud() {
    WNDCLASSEXW cls = {};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = HudWndProc;
    cls.hInstance = g_sowerModule;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    cls.lpszClassName = SOWER_HUD_CLASS;
    if (!RegisterClassExW(&cls)) {
        DWORD err = GetLastError();
        WNDCLASSEXW existing = {};
        existing.cbSize = sizeof(existing);
        if (err != ERROR_CLASS_ALREADY_EXISTS ||
            !GetClassInfoExW(g_sowerModule, SOWER_HUD_CLASS, &existing) ||
            existing.lpfnWndProc != HudWndProc ||
            existing.hInstance != g_sowerModule) {
            Log("[Sower] [HUD] window class register failed (err=%lu)", err);
            return false;
        }
    }

    g_hudFont = CreateFontW(-22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                            L"Microsoft YaHei UI");
    g_hudFontSmall = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 L"Microsoft YaHei UI");
    if (!g_hudFont || !g_hudFontSmall) {
        Log("[Sower] [HUD] font creation failed");
        return false;
    }

    const int width = 180;
    const int height = 62;
    g_hudWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        SOWER_HUD_CLASS, L"", WS_POPUP, 0, 0, width, height,
        nullptr, nullptr, g_sowerModule, nullptr);
    // v1.3.34: 移除游戏窗口 owner 绑定（hWndParent 原传 QolFindGameWindow()）——
    // owned TOPMOST 窗口链干扰 Alt+Tab 前台切换，游戏切不回（切窗修复第四轮
    // 漏网之鱼，其余 5 个 HUD 均已修，详见 game_window.h v1.5 / KB-075；
    // 2026-10-06 用户实测切窗回归定位）
    if (!g_hudWindow) {
        Log("[Sower] [HUD] CreateWindowExW failed (err=%lu)", GetLastError());
        return false;
    }

    SetLayeredWindowAttributes(g_hudWindow, 0, 228, LWA_ALPHA);
    HRGN rounded = CreateRoundRectRgn(0, 0, width + 1, height + 1, 12, 12);
    if (!SetWindowRgn(g_hudWindow, rounded, FALSE)) DeleteObject(rounded);

    Log("[Sower] [HUD] overlay window ready");
    return true;
}

static void UpdateHudPosition() {
    if (!g_hudWindow) return;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(QolFindGameWindow(), MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(mon, &mi)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
    }
    const int width = 180;
    const int height = 62;
    const int x = mi.rcWork.left + 24;
    const int y = mi.rcWork.top + 24;
    SetWindowPos(g_hudWindow, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void RefreshHud(int mode) {
    if (!g_hudWindow && !InitHud()) return;
    g_hudMode = mode;
    UpdateHudPosition();
    InvalidateRect(g_hudWindow, nullptr, TRUE);
    UpdateWindow(g_hudWindow);
    g_hudHideAt = GetTickCount64() + 5000;  // 5秒后消退
}

static void PumpHud() {
    if (!g_hudWindow) return;
    if (!QolGameInForeground()) { QolHudGuardVisibility(g_hudWindow); return; }  // v1.6: 失焦守卫兜底（alpha 渐隐）后 pump 静默
    MSG msg = {};
    while (PeekMessageW(&msg, g_hudWindow, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // 5秒后自动隐藏
    if (IsWindowVisible(g_hudWindow) && g_hudHideAt != 0 && GetTickCount64() >= g_hudHideAt) {
        QolHudMarkHiddenByMod(g_hudWindow);  // MOD 主动隐藏：清守卫标记
        ShowWindow(g_hudWindow, SW_HIDE);
        g_hudHideAt = 0;
    }
}

// ============================================================
// 输入：键盘 5 切换模式
// ============================================================
static const char* ModeName(int mode) {
    switch (mode) {
        case 0: return "1x1 (原生)";
        case 1: return "3x3";
        case 2: return "5x5";
        case 3: return "连通";
        default: return "未知";
    }
}

static void PollModeSwitch() {
    static bool s_needsRelease = true;
    bool down = (GetAsyncKeyState(QolHotKeysVk(&g_hotkeys, 0)) & 0x8000) != 0;

    bool pressed = false;
    if (s_needsRelease) {
        if (!down) s_needsRelease = false;
    } else if (down) {
        pressed = true;
        s_needsRelease = true;
    }

    // 手柄 D-pad Left 也可切换模式（边沿触发，三路混合检测）
    if (!pressed) {
        pressed = CheckDpadLeftPressed();
    }

    if (pressed && G::rangeSowingReady) {
        int oldMode = G::rangeSowingMode.load(std::memory_order_relaxed);
        int newMode = (oldMode + 1) % 3;  // 0(1x1) -> 1(3x3) -> 2(5x5) -> 0（v1.3.41 连通档 3 下线：v1.20 连通 Flood-Fill 实测 filled=0 不工作）
        G::rangeSowingMode.store(newMode, std::memory_order_relaxed);
        Log("[Sower] 模式切换: %s -> %s", ModeName(oldMode), ModeName(newMode));
        RefreshHud(newMode);
    }
}

// ============================================================
// 插件入口
// ============================================================
extern "C" __declspec(dllexport) void mod_init(void) {
    LogOpen("sower");
    Log("[Sower] mod_init 开始");
    HotConfig_Register("sower", (void*)&RVA_SOWING_UPDATE, "RVA_SOWING_UPDATE", HOT_RVA, 0x24ACE0);
    HotConfig_Register("sower", (void*)&RVA_SOWING_PHASE0_UPDATE, "RVA_SOWING_PHASE0_UPDATE", HOT_RVA, 0x24ACA0);
    HotConfig_Register("sower", (void*)&RVA_STATE_MANAGER_LOOKUP, "RVA_STATE_MANAGER_LOOKUP", HOT_RVA, 0x1CA5D0);
    HotConfig_Register("sower", (void*)&RVA_SOW_SELECT_TARGET, "RVA_SOW_SELECT_TARGET", HOT_RVA, 0x212020);
    HotConfig_Register("sower", (void*)&RVA_SOW_VALIDATE_LAND, "RVA_SOW_VALIDATE_LAND", HOT_RVA, 0x1E1310);
    HotConfig_Register("sower", (void*)&RVA_SOW_REFRESH_LAND, "RVA_SOW_REFRESH_LAND", HOT_RVA, 0x212110);
    HotConfig_Register("sower", (void*)&RVA_SOW_VALIDATE_STATE, "RVA_SOW_VALIDATE_STATE", HOT_RVA, 0x24B0B0);
    HotConfig_Register("sower", (void*)&RVA_SOW_SETTLE, "RVA_SOW_SETTLE", HOT_RVA, 0x24B460);
    HotConfig_Register("sower", (void*)&RVA_SOW_SEED_ITEM_LOOKUP, "RVA_SOW_SEED_ITEM_LOOKUP", HOT_RVA, 0x17C840);
    HotConfig_Register("sower", (void*)&RVA_SOW_SEED_ITEM_CHECK, "RVA_SOW_SEED_ITEM_CHECK", HOT_RVA, 0x75F630);
    HotConfig_Register("sower", (void*)&RVA_GAME_SINGLETON_PTR, "RVA_GAME_SINGLETON_PTR", HOT_RVA, 0x10FCBB0);
    HotConfig_Register("sower", (void*)&RVA_SOW_WORLD_TO_KEY, "RVA_SOW_WORLD_TO_KEY", HOT_RVA, 0x16D460);
    HotConfig_Register("sower", (void*)&RVA_SOW_KEY_TO_WORLD, "RVA_SOW_KEY_TO_WORLD", HOT_RVA, 0x16D540);
    HotConfig_Register("sower", (void*)&SAVE_DATA_POINTER_OFFSET, "SAVE_DATA_POINTER_OFFSET", HOT_RVA, 0x208);
    HotConfig_Register("sower", (void*)&SAVE_INVENTORY_UNLOCK_FLAGS_OFFSET, "SAVE_INVENTORY_UNLOCK_FLAGS_OFFSET", HOT_RVA, 0x388);
    HotConfig_Register("sower", (void*)&SAVE_SKIP_ITEM_DEDUCTION_OFFSET, "SAVE_SKIP_ITEM_DEDUCTION_OFFSET", HOT_RVA, 0x340C);
    HotConfig_Register("sower", (void*)&SAVE_INVENTORY_SLOTS_OFFSET, "SAVE_INVENTORY_SLOTS_OFFSET", HOT_RVA, 0x32C0);
    HotConfig_Register("sower", (void*)&SAVE_SELECTED_ITEM_INDEX_OFFSET, "SAVE_SELECTED_ITEM_INDEX_OFFSET", HOT_RVA, 0x33B0);
    HotConfig_Register("sower", (void*)&SOWING_STATE_PHASE_OFFSET, "SOWING_STATE_PHASE_OFFSET", HOT_RVA, 0x1D8);
    HotConfig_Register("sower", (void*)&SOWING_STATE_NEXT_PHASE_OFFSET, "SOWING_STATE_NEXT_PHASE_OFFSET", HOT_RVA, 0x1DC);
    HotConfig_Register("sower", (void*)&SOWING_STATE_TARGET_OFFSET, "SOWING_STATE_TARGET_OFFSET", HOT_RVA, 0x240);
    HotConfig_Register("sower", (void*)&SOWING_STATE_QUERY_OFFSET, "SOWING_STATE_QUERY_OFFSET", HOT_RVA, 0x248);
    HotConfig_Register("sower", (void*)&SOWING_STATE_LAND_OFFSET, "SOWING_STATE_LAND_OFFSET", HOT_RVA, 0x268);
    HotConfig_Register("sower", (void*)&SOWING_STATE_SELECTED_KEY_OFFSET, "SOWING_STATE_SELECTED_KEY_OFFSET", HOT_RVA, 0x270);
    HotConfig_Register("sower", (void*)&SOWING_STATE_COUNT_OFFSET, "SOWING_STATE_COUNT_OFFSET", HOT_RVA, 0x278);
    HotConfig_Register("sower", (void*)&SOWING_STATE_VALIDATED_OFFSET, "SOWING_STATE_VALIDATED_OFFSET", HOT_RVA, 0x27C);
    HotConfig_Register("sower", (void*)&SOWING_STATE_RUNNER_OFFSET, "SOWING_STATE_RUNNER_OFFSET", HOT_RVA, 0x238);
    HotConfig_Register("sower", (void*)&UNIT_POSITION_OFFSET, "UNIT_POSITION_OFFSET", HOT_RVA, 0x230);
    HotConfig_Register("sower", (void*)&SOWING_MANAGER_TARGET_OFFSET, "SOWING_MANAGER_TARGET_OFFSET", HOT_RVA, 0x460);
    HotConfig_Register("sower", (void*)&ITEM_DATA_HOLDER_OFFSET, "ITEM_DATA_HOLDER_OFFSET", HOT_RVA, 0x240);
    HotConfig_Register("sower", (void*)&ITEM_STACK_COUNT_OFFSET, "ITEM_STACK_COUNT_OFFSET", HOT_RVA, 0x260);
    HotConfig_Register("sower", (void*)&MANAGER_MAPINFO_OFFSET, "MANAGER_MAPINFO_OFFSET", HOT_RVA, 0x310);
    HotConfig_Poll();
    HotConfig_DumpCE("sower");
    s_sowerBudgetSlot = qol::budget::Slot("Sower");  // v1.3.19: P0 探针注册

    // 防重复加载检测
    if (IsDuplicateInstance()) {
        g_duplicate = true;
        G::ready = false;
        Log("[Sower] [跳过] 检测到已存在另一份 sower.dll 实例，本副本不安装 hook");
        return;
    }

    G::base = (uintptr_t)GetModuleHandleW(nullptr);
    Log("[Sower] 游戏基址: 0x%llX", (unsigned long long)G::base);

    QolHotKeysInit(&g_hotkeys, "sower");
    QolHotKeysSetDefault(&g_hotkeys, "5, D-pad Left");
    QolRegisterHotKey("sower", "5, D-pad Left");

    // 安装播种 hook
    if (!InstallRangeSowingHook()) {
        Log("[Sower] [警告] hook 安装失败，范围播种不可用（原生播种不受影响）");
    }

    // 初始化手柄（XInput + HID + joyGetPosEx 三路混合）
    InitGamepad();

    G::ready = true;
    Log("[Sower] mod_init 完成 (ready=%d hook=%d xinput=%d hid=%d pad=%d)",
        G::ready ? 1 : 0, G::rangeSowingReady ? 1 : 0,
        G::xinputReady ? 1 : 0, G::hidReady ? 1 : 0, G::padReady ? 1 : 0);

    // 初始化 HUD（延迟到首次按 5 时才创建窗口）
}

extern "C" __declspec(dllexport) void mod_tick(void) {
    if (!G::ready) return;
    // v1.3.40-diag: mod_tick 心跳（10s 限频，证明宿主每帧调用）
    static ULONGLONG s_lastTickLog = 0;
    ULONGLONG nowT = GetTickCount64();
    if (nowT - s_lastTickLog >= 10000) {
        s_lastTickLog = nowT;
        Log("[Sower] [tick] alive queue=%d\n", (int)g_sowingResumeQueue.active);
    }
    const uint64_t budgetT0 = qol::budget::NowUs();
    HotConfig_Poll();
    QolHotkeyCheckReload(&g_hotkeys);
    PollModeSwitch();
    RangeSowingResumePump();
    PumpHud();
    QolHudGuardVisibility(g_hudWindow);  // v1.3.26: 失焦隐藏 HUD（不飘桌面）
    qol::budget::Report(s_sowerBudgetSlot,
                        qol::budget::NowUs() - budgetT0);  // v1.3.19: P0 自报
}

extern "C" __declspec(dllexport) void unload(void) {
    Log("[Sower] unload");

    // 还原 hook（恢复原始 22 字节序言）
    if (G::rangeSowingReady && G::base) {
        unsigned char* target = reinterpret_cast<unsigned char*>(
            G::base + RVA_SOWING_UPDATE);
        WriteMem(target, EXPECTED_UPDATE_PROLOGUE, SOWING_HOOK_LENGTH);
        G::rangeSowingReady = false;
        G::rangeSowingNativeBatchReady = false;
        Log("[Sower] hook 已还原");
    }

    // 释放跳板内存
    if (g_sowingTrampoline) {
        VirtualFree(g_sowingTrampoline, 0, MEM_RELEASE);
        g_sowingTrampoline = nullptr;
    }

    // 停止 XInput 轮询线程
    if (g_xinputThread) {
        InterlockedExchange(&g_xinputThreadRunning, 0);
        WaitForSingleObject(g_xinputThread, 2000);
        CloseHandle(g_xinputThread);
        g_xinputThread = nullptr;
    }

    // 停止 HID 轮询线程
    if (g_hidThread) {
        InterlockedExchange(&g_hidThreadRunning, 0);
        // 关闭设备句柄使阻塞中的 ReadFile 立即失败返回，
        // 而非 CancelIoEx（其参数应为设备句柄而非线程句柄）
        HANDLE dev = InterlockedExchangePointer(&g_hidDevice, nullptr);
        if (dev) CloseHandle(dev);
        WaitForSingleObject(g_hidThread, 2000);
        CloseHandle(g_hidThread);
        g_hidThread = nullptr;
    }

    // 清理 HUD 资源
    if (g_hudWindow) {
        DestroyWindow(g_hudWindow);
        g_hudWindow = nullptr;
    }
    if (g_hudFont) {
        DeleteObject(g_hudFont);
        g_hudFont = nullptr;
    }
    if (g_hudFontSmall) {
        DeleteObject(g_hudFontSmall);
        g_hudFontSmall = nullptr;
    }
    // 清理画刷
    if (g_hudBgBrush) { DeleteObject(g_hudBgBrush); g_hudBgBrush = nullptr; }
    for (int i = 0; i < 4; ++i) {
        if (g_hudAccentBrushes[i]) { DeleteObject(g_hudAccentBrushes[i]); g_hudAccentBrushes[i] = nullptr; }
    }

    LogClose();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)lpReserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_sowerModule = hModule;
        g_sowerModuleForDup = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
