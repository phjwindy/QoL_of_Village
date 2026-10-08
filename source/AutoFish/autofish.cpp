// autofish.cpp —— 自动钓鱼（v1.5.7 正式版：v1.20 (build 25311578) 适配；v1.5.0 起开关接入 QoL 快捷菜单）
//
// v1.3.4: AF-H2 修复——ReadFishingState 中添加 vtable 软校验（warn-only），
//         记录运行时实际 vtable RVA 供后续精确回填。原 v1.09 适配时
//         vtable 校验被完全移除，hook 被错误对象触发时等于向随机对象写零。
//         AF-H1（直接写字段+跳过 action finish）暂保留——参考源码也是此模式，
//         改造需理解 action_finish 副作用，列为后续 P2 工作。
//
// 原理（源自 BigL233 dinput8.cpp 421-1121 行）：
//   1. 钓鱼小游戏自动完成 + 自动收竿（3 个 NOP 补丁）
//   2. 连续抛竿循环（4 个代码注入 + 1 个 Monitor trampoline）
//   3. 垃圾排除（1 个代码注入 stub，过滤 4 种垃圾物品 ID）
//
// 钓鱼状态机（CState_Player_FishingRod::update）8 个 Phase：
//   0=待命/重抛  4=钓到鱼/收线动画  6=成功展示  7=玩家取消/收竿
//
// 补丁点（build 25311578 / v1.20）：
//   minigame_settle_gate  RVA 0x392BAD  0F 84 81 01 00 00 -> 6 NOP
//   note_hit_check       RVA 0x392BE4  0F 85 77 01 00 00 -> 6 NOP
//   auto_reel_in         RVA 0x218DEE  74 1D               -> 2 NOP
//
// 代码注入点（build 25311578 / v1.20）：
//   common_exit          RVA 0x219F46  Phase 6/7 公共退出点
//   success_gate         RVA 0x219DF7  Phase 6 完成门
//   cancel_request       RVA 0x218963  玩家取消请求
//   phase4_finish        RVA 0x219C75  Phase 4 动画结束
//   monitor              RVA 0x218430  trampoline hook 监控钓鱼状态机
//
// 垃圾排除注入点（build 25311578 / v1.20）：
//   no_trash_hook        RVA 0x21B0C5  钓到鱼后物品判定分支
//   anchor: 45 85 FF 7E 4C (test r15d,r15d; jle +0x4C)
//   垃圾物品 ID: 0x927C0 / 0x927CA / 0x927D4 / 0x927DE
//
// v1.3.0: 自动检测钓鱼状态——Monitor trampoline 检测钓鱼状态机是否活跃，
//         钓鱼时自动开启 NOP 补丁，停止钓鱼时自动关闭，过剧情无需手动操作。
// v1.3.1: HUD 修复——自动检测模式下的状态变化不再弹窗，仅按键切换时显示 HUD。
// v1.3.2: HUD 布局调整——加大窗口高度，垃圾排除行下移避免与自动钓鱼文字重叠。
// v1.3.3: HUD 微调——状态文字和垃圾排除文字限定 rect 高度，避免重叠。
//
// 切换键：v1.5.0 起热键 9/F10 已移除，开关改由 QoL 快捷菜单（F2）驱动
// 默认：自动检测开启，垃圾排除关闭。
// 左上角 HUD 仅在按键切换时显示，5 秒后消退（自动检测状态变化不弹窗）。

#include <windows.h>
#include <cstdint>
#include <atomic>

#include "logging.h"
#include "game_window.h"   // HUD owner 绑定游戏窗口
#include "budget.h"        // P0 帧耗时探针（跨 DLL 共享）
#include "state.h"         // P1-2 状态感知暂停（载入/菜单静默）
#include "hot_config.h"
#include "quickmenu.h"
#include "memory_cache.h"
#include "patch_safety.h"  // v1.4.1: FastRegion 区域缓存

// 日志开关：发布版禁用日志输出
// 调试时取消注释下行即可开启日志
// #define AUTOFISH_LOGGING
#ifdef AUTOFISH_LOGGING
#else
  #define LogOpen(x)  ((void)0)
  #define Log(...)    ((void)0)
  #define LogV(...)   ((void)0)
  #define LogClose()  ((void)0)
#endif

using u64 = std::uint64_t;
using u32 = std::uint32_t;

// ============================================================
// 常量（build 25094764 / v1.09）
// ============================================================

// ---- 3 个 NOP 补丁点 ----
struct PatchPoint {
    uintptr_t rva;
    size_t len;
    unsigned char original[8];
    const char* name;
};

static PatchPoint g_fishingPoints[3] = {
    { 0x392BAD, 6, { 0x0F, 0x84, 0x81, 0x01, 0x00, 0x00 }, "minigame settle-gate" },
    { 0x392BE4, 6, { 0x0F, 0x85, 0x77, 0x01, 0x00, 0x00 }, "note-hit check"      },
    { 0x218DEE, 2, { 0x74, 0x1D },                         "auto reel-in"         },
};

static const unsigned char kNops[8] = {
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};

// ---- 代码注入 RVA ----
static volatile uintptr_t RVA_FISHING_COMMON_EXIT          = 0x219F46;
static volatile uintptr_t RVA_FISHING_COMMON_EXIT_CONTINUE  = 0x219F4D;
static volatile uintptr_t RVA_FISHING_UPDATE_EPILOGUE       = 0x219FC1;

static volatile uintptr_t RVA_FISHING_SUCCESS_COMPLETION_GATE = 0x219DF7;
static volatile uintptr_t RVA_FISHING_SUCCESS_GATE_CONTINUE   = 0x219DFE;

static volatile uintptr_t RVA_FISHING_CANCEL_REQUEST         = 0x218963;
static volatile uintptr_t RVA_FISHING_CANCEL_REQUEST_CONTINUE = 0x21896E;

static volatile uintptr_t RVA_FISHING_PHASE4_FINISH          = 0x219C75;
static volatile uintptr_t RVA_FISHING_PHASE4_CLEANUP         = 0x219C7E;
static volatile uintptr_t RVA_FISHING_PHASE4_NATIVE_CONTINUE = 0x219C7D;
static volatile uintptr_t RVA_FISHING_NATIVE_ACTION_FINISH   = 0x211DE0;

// v1.20: vtable RVA 通过 RTTI 链定位（.?AVCState_Player_FishingRod@@），槽位镜像 64/64 字节验证
static volatile uintptr_t RVA_FISHING_VTABLE          = 0xE387F8; // v1.5.3: v1.20 RTTI 定位
static volatile uintptr_t RVA_FISHING_GAME_ROOT_SLOT  = 0x10FCBB0;

// v1.20: save_data vtable = CSaveData vtable 已知映射（0xE0B3D8 -> 0xE2D838）
//         保留 warn-only 模式，不影响钓鱼功能（仅时钟读取用）
static volatile uintptr_t RVA_FISHING_SAVE_DATA_VTABLE = 0xE2D838; // v1.5.3: v1.20 已知映射，非钓鱼路径

static volatile uintptr_t RVA_FISHING_MONITOR_TARGET  = 0x218430;
// ---- InstallFishingLoopHook 内部验证用 RVA（移至文件作用域以支持 HotConfig 注册）----
static volatile uintptr_t RVA_SUCCESS_EXIT_JUMP   = 0x219E50;
static volatile uintptr_t RVA_DIRECT_NULL_EXIT    = 0x219E01;
static volatile uintptr_t RVA_STATE_DISPATCH       = 0x218480;
static volatile uintptr_t RVA_SUCCESS_PHASE        = 0x219154;

// ---- 垃圾排除 ----
// Hook 点在钓鱼 update 函数内部（update RVA + 0x2C95，v1.20）
// 5 字节 anchor: test r15d, r15d; jle +0x4C
static volatile uintptr_t RVA_NO_TRASH_HOOK = 0x21B0C5;
static volatile uintptr_t RVA_NO_TRASH_CONTINUE = 0x21B0CA;   // hook + 5（非垃圾路径跳回点）
static volatile uintptr_t RVA_NO_TRASH_SKIP_TARGET = 0x21B116; // hook + 5 + 0x4C（垃圾路径跳过点）
static const unsigned char NO_TRASH_ANCHOR[5] = {
    0x45, 0x85, 0xFF, 0x7E, 0x4C  // test r15d, r15d; jle +0x4C
};
// 4 种垃圾物品 ID（空罐 / 塑料袋 / 水草 / 树枝）
static constexpr u32 JUNK_ITEM_IDS[4] = {
    0x0927C0,  // 600000
    0x0927CA,  // 600010
    0x0927D4,  // 600020
    0x0927DE   // 600030
};
static constexpr size_t NO_TRASH_STUB_SIZE = 0xC0;  // stub 大小 192 字节

// ---- 钓鱼状态机偏移 ----
static volatile uintptr_t FISHING_CURRENT_PHASE_OFFSET  = 0x1D8;
static volatile uintptr_t FISHING_REQUESTED_PHASE_OFFSET = 0x1DC;
static volatile uintptr_t FISHING_VALID_SPOT_OFFSET      = 0x270;
static volatile uintptr_t FISHING_TASK_OFFSET            = 0x290;
static volatile uintptr_t FISHING_SPECIAL_RESULT_OFFSET = 0x2B8;
static volatile uintptr_t FISHING_WAIT_TICKS_OFFSET      = 0x2F8;

// ---- 游戏时钟读取（源自 BigL233 AutoPetReadClock + GetGameHour） ----
static volatile uintptr_t SAVE_DATA_OFFSET          = 0x208;   // game_root -> save data
static volatile uintptr_t SAVE_RAW_SECOND_OFFSET     = 0x3270;  // save -> raw second
static constexpr u64 RAW_SECONDS_PER_DAY   = 86400;
static constexpr u64 RAW_SECONDS_PER_HOUR  = 3600;
static constexpr u32 RAW_DAY_START_HOUR    = 7;                 // 游戏一天从早上 7 点开始
// 宵禁窗口：晚上 23:00 ~ 早上 6:00（系统通知回家睡觉）

// ============================================================
// 全局状态
// ============================================================
namespace G {
    uintptr_t base = 0;
    bool ready = false;
}

static bool g_fishingAvailable = false;
static std::atomic<bool> g_fishingEnabled{false};

// hook 就绪标志
static bool g_fishingLoopReady = false;
static bool g_fishingPostCatchReady = false;
static bool g_fishingMonitorReady = false;
static bool g_noTrashReady = false;          // 垃圾排除 hook 是否已安装

// stub 内存指针
static void* g_fishingLoopStub = nullptr;
static void* g_fishingSuccessStub = nullptr;
static void* g_fishingCancelStub = nullptr;
static void* g_fishingPostCatchStub = nullptr;
static void* g_noTrashStub = nullptr;

// 垃圾排除状态
static std::atomic<bool> g_noTrashEnabled{false};  // 默认关闭
static std::atomic<u32> g_noTrashExecCount{0};     // 执行计数
static std::atomic<u32> g_noTrashSkipCount{0};     // 跳过计数
// stub 内部计数器地址（由 stub 通过 lock inc 原子递增）
// 注意：stub 使用 lock inc [rcx] 直接操作这两个变量
// 由于 stub 在 mod_init 时构建，地址固定，生命周期与 DLL 相同

// 循环计数
static std::atomic<u64> g_fishingLoopCount{0};

// 取消请求记录（线程局部）
static thread_local void* g_fishingCancelRequestedState = nullptr;

// Monitor trampoline
using FishingUpdateFunction = bool (__fastcall *)(void*, void*);
static FishingUpdateFunction g_originalFishingUpdate = nullptr;

// Monitor 状态追踪（线程局部）
static thread_local void* g_lastFishingMonitorState = nullptr;
static thread_local u32 g_lastFishingMonitorCurrent = ~u32{0};
static thread_local u32 g_lastFishingMonitorRequested = ~u32{0};

// v1.3.0: 自动检测钓鱼状态
static std::atomic<bool>      g_autoDetectMode{true};        // 自动检测模式（默认开）
static std::atomic<ULONGLONG> g_lastMonitorTick{0};          // Monitor 最后被调用的 tick
static bool                   g_fishingInitFailed = false;   // hook 安装失败（不重试）
static constexpr ULONGLONG    MONITOR_FISHING_TIMEOUT    = 1000; // <1s = 钓鱼中
static constexpr ULONGLONG    MONITOR_NOTFISHING_TIMEOUT = 2000; // >2s = 已停止

// ============================================================
// 内存工具
// ============================================================
static bool WriteMem(void* target, const void* data, size_t size) {
    return qol::WritePatchChecked(target, data, size);
}

static bool IsReadable(const void* pointer, size_t size) {
    return qol_mem::IsReadable(pointer, size);
}

static bool SealExecutableMemory(void* address, size_t size) {
    DWORD previousProtection = 0;
    if (!address || size == 0) return false;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READ, &previousProtection))
        return false;
    return FlushInstructionCache(GetCurrentProcess(), address, size) != 0;
}

// ============================================================
// 钓鱼状态读取
// ============================================================
static bool ReadFishingState(void* state, uintptr_t* object, uintptr_t* vtable,
                              u32* currentPhase, u32* requestedPhase,
                              bool* validSpot, void** task) {
    if (!state || !IsReadable(state, FISHING_WAIT_TICKS_OFFSET + sizeof(u64)))
        return false;
    *object = reinterpret_cast<uintptr_t>(state);
    *vtable = *reinterpret_cast<const uintptr_t*>(*object);
    // vtable 硬校验（v1.4.0 恢复）：RVA v1.20 通过 RTTI 链定位确认 0xE387F8。
    // 不匹配则拒绝对象，防止 hook 被错误状态对象触发导致向随机内存写零。
    if (G::base && *vtable >= G::base) {
        const uintptr_t vtableRva = *vtable - G::base;
        if (vtableRva != RVA_FISHING_VTABLE) {
            return false;
        }
    }
    *currentPhase = *reinterpret_cast<const u32*>(
        *object + FISHING_CURRENT_PHASE_OFFSET);
    *requestedPhase = *reinterpret_cast<const u32*>(
        *object + FISHING_REQUESTED_PHASE_OFFSET);
    *validSpot = *reinterpret_cast<const unsigned char*>(
        *object + FISHING_VALID_SPOT_OFFSET) != 0;
    *task = *reinterpret_cast<void* const*>(*object + FISHING_TASK_OFFSET);
    return true;
}

// ============================================================
// 游戏时钟读取
// 读取 CSaveData 的 raw second，转换为游戏小时（0-23）
// 路径：game_root(+0x10FCBB0) → +0x208 → save_data → +0x3270 → raw_second
// 换算：hour = (rawSecond % 86400) / 3600 + 7) % 24
// ============================================================
static bool ReadGameHour(u32* outHour) {
    if (!outHour || !G::base) return false;
    void* root = *reinterpret_cast<void**>(G::base + RVA_FISHING_GAME_ROOT_SLOT);
    if (!root || !IsReadable(root, SAVE_DATA_OFFSET + sizeof(void*)))
        return false;
    void* save = *reinterpret_cast<void**>(
        reinterpret_cast<unsigned char*>(root) + SAVE_DATA_OFFSET);
    if (!save || !IsReadable(save, SAVE_RAW_SECOND_OFFSET + sizeof(std::int64_t)))
        return false;

    // 打印 save vtable RVA 供调试（不做硬校验，v1.08.1 vtable RVA 待确认）
    const uintptr_t saveVtable = *reinterpret_cast<const uintptr_t*>(save);
    const uintptr_t saveVtableRva = saveVtable - G::base;

    const std::int64_t rawSecond = *reinterpret_cast<const std::int64_t*>(
        reinterpret_cast<unsigned char*>(save) + SAVE_RAW_SECOND_OFFSET);
    if (rawSecond < 0) {
        Log("[AutoFish] ReadGameHour: raw_second=%lld (INVALID, negative) save_vtable_rva=0x%X\n",
            rawSecond, saveVtableRva);
        return false;
    }
    const u64 withinRawDay = static_cast<u64>(rawSecond) % RAW_SECONDS_PER_DAY;
    *outHour = static_cast<u32>((withinRawDay / RAW_SECONDS_PER_HOUR +
                                 RAW_DAY_START_HOUR) % 24);
    Log("[AutoFish] ReadGameHour: raw_second=%lld hour=%u save_vtable_rva=0x%X\n",
        rawSecond, *outHour, saveVtableRva);
    return true;
}

// 宵禁检测已移除：原始源码无此逻辑，游戏自身的强制睡觉机制会自然中断钓鱼。
// 时钟读取功能保留供其他用途，但不再拦截自动重抛。
static bool InCurfewWindow() {
    return false;
}

// ============================================================
// 辅助函数（从 BigL233 源码直接移植）
// ============================================================

// 记录玩家主动取消请求
static void __fastcall MarkFishingCancelRequest(void* state) {
    g_fishingCancelRequestedState = state;
    uintptr_t object = 0, vtable = 0;
    u32 current = ~u32{0}, requested = ~u32{0};
    bool valid = false;
    void* task = nullptr;
    ReadFishingState(state, &object, &vtable, &current, &requested, &valid, &task);
    Log("[AutoFish] stage=cancel-request marked object=%p current=%u requested=%u "
        "valid_spot=%d task=%p\n", state, current, requested, valid ? 1 : 0, task);
}

// 观察成功门（不做重抛，让原生动画播放）
static bool __fastcall ObserveFishingSuccessGate(void* state) {
    uintptr_t object = 0, vtable = 0;
    u32 current = ~u32{0}, requested = ~u32{0};
    bool valid = false;
    void* task = nullptr;
    ReadFishingState(state, &object, &vtable, &current, &requested, &valid, &task);
    Log("[AutoFish] stage=pre-animation gate observed object=%p current=%u "
        "requested=%u valid_spot=%d task=%p; native animation preserved\n",
        state, current, requested, valid ? 1 : 0, task);
    return false;
}

// 公共退出点：检测成功退出后重置状态并重新抛竿
static bool __fastcall RearmFishingAtCommonExit(void* state) {
    if (!g_fishingEnabled.load(std::memory_order_relaxed)) return false;
    uintptr_t object = 0, vtable = 0;
    u32 currentPhase = ~u32{0}, requestedPhase = ~u32{0};
    bool validSpot = false;
    void* task = nullptr;
    if (!ReadFishingState(state, &object, &vtable, &currentPhase,
                          &requestedPhase, &validSpot, &task)) {
        Log("[AutoFish] stage=post-animation rejected reason=invalid_state object=%p\n",
            state);
        return false;
    }

    const bool explicitCancel = g_fishingCancelRequestedState == state;
    Log("[AutoFish] stage=post-animation exit object=%p cancel_mark=%d current=%u "
        "requested=%u valid_spot=%d task=%p\n", state, explicitCancel ? 1 : 0,
        currentPhase, requestedPhase, validSpot ? 1 : 0, task);
    if (explicitCancel) {
        g_fishingCancelRequestedState = nullptr;
        Log("[AutoFish] stage=cancel native exit preserved; continuous loop stopped\n");
        return false;
    }

    const uintptr_t base = G::base;
    if (!base || !validSpot || task != nullptr) {
        Log("[AutoFish] stage=post-animation rejected reason=state_mismatch object=%p "
            "vtable_rva=0x%llx current=%u requested=%u valid_spot=%d task=%p\n", state,
            static_cast<unsigned long long>(base && vtable >= base ? vtable - base : 0),
            currentPhase, requestedPhase, validSpot ? 1 : 0, task);
        return false;
    }

    // 宵禁窗口（23:00-06:00）：不自动重抛，循环自然停止，等待玩家收竿回屋
    if (InCurfewWindow()) {
        Log("[AutoFish] stage=post-animation rejected reason=curfew object=%p "
            "current=%u requested=%u valid_spot=%d\n",
            state, currentPhase, requestedPhase, validSpot ? 1 : 0);
        return false;
    }

    // 钓后动画已完成，无主动取消请求 -> 重置状态并重新抛竿
    *reinterpret_cast<u32*>(object + FISHING_REQUESTED_PHASE_OFFSET) = 0;
    *reinterpret_cast<unsigned char*>(object + FISHING_SPECIAL_RESULT_OFFSET) = 0;
    *reinterpret_cast<u64*>(object + FISHING_WAIT_TICKS_OFFSET) = 0;
    const u64 cycle = g_fishingLoopCount.fetch_add(1, std::memory_order_relaxed) + 1;
    Log("[AutoFish] stage=loop rearmed cycle=%llu animation_complete=1 "
        "requested=0 observed_current=%u observed_requested=%u task=null\n",
        static_cast<unsigned long long>(cycle), currentPhase, requestedPhase);
    return true;
}

// Phase 4 动画结束：跳过原生 action destroy 并重新抛竿
static bool __fastcall RearmFishingAfterCatchAnimation(void* state) {
    if (!g_fishingEnabled.load(std::memory_order_relaxed)) return false;
    uintptr_t object = 0, vtable = 0;
    u32 current = ~u32{0}, requested = ~u32{0};
    bool valid = false;
    void* task = nullptr;
    if (!ReadFishingState(state, &object, &vtable, &current, &requested,
                          &valid, &task)) {
        Log("[AutoFish] stage=catch-animation-finish rejected reason=invalid_state\n");
        return false;
    }
    const uintptr_t base = G::base;
    Log("[AutoFish] stage=catch-animation-finish object=%p current=%u requested=%u "
        "valid_spot=%d task=%p\n", state, current, requested, valid ? 1 : 0, task);
    if (!base || !valid || task != nullptr ||
        current != 4) {
        Log("[AutoFish] stage=catch-animation-finish rejected reason=state_mismatch "
            "vtable_rva=0x%llx current=%u requested=%u valid_spot=%d task=%p\n",
            static_cast<unsigned long long>(base && vtable >= base ? vtable - base : 0),
            current, requested, valid ? 1 : 0, task);
        return false;
    }

    // 宵禁窗口（23:00-06:00）：不自动重抛，循环自然停止
    if (InCurfewWindow()) {
        Log("[AutoFish] stage=catch-animation-finish rejected reason=curfew object=%p "
            "current=%u requested=%u valid_spot=%d\n",
            state, current, requested, valid ? 1 : 0);
        return false;
    }

    *reinterpret_cast<u32*>(object + FISHING_REQUESTED_PHASE_OFFSET) = 0;
    *reinterpret_cast<unsigned char*>(object + FISHING_SPECIAL_RESULT_OFFSET) = 0;
    *reinterpret_cast<u64*>(object + FISHING_WAIT_TICKS_OFFSET) = 0;
    g_fishingCancelRequestedState = nullptr;
    const u64 cycle = g_fishingLoopCount.fetch_add(1, std::memory_order_relaxed) + 1;
    Log("[AutoFish] stage=loop rearmed cycle=%llu source=phase4-animation-finish "
        "current=4 requested=0 task=null; native action finish skipped\n",
        static_cast<unsigned long long>(cycle));
    return true;
}

// ============================================================
// 就近分配可执行内存（在 target ±2GB 范围内搜索空闲页）
// ============================================================
static void* AllocateExecutableNear(uintptr_t target, size_t size) {
    SYSTEM_INFO info = {};
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t range = 0x7fff0000ull;
    const uintptr_t minimum = target > range ? target - range :
        reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t maximumByRange = target + range;
    const uintptr_t maximumSystem =
        reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t maximum = maximumByRange < maximumSystem ?
        maximumByRange : maximumSystem;

    uintptr_t address = minimum & ~(granularity - 1);
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION region = {};
        if (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) == 0) {
            break;
        }
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(region.BaseAddress);
        const uintptr_t regionEnd = regionBase + region.RegionSize;
        if (region.State == MEM_FREE) {
            const uintptr_t candidate =
                (regionBase + granularity - 1) & ~(granularity - 1);
            if (candidate >= minimum && candidate + size <= regionEnd &&
                candidate + size <= maximum) {
                void* allocated = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                                               MEM_COMMIT | MEM_RESERVE,
                                               PAGE_READWRITE);
                if (allocated) {
                    const std::int64_t displacement =
                        static_cast<std::int64_t>(reinterpret_cast<uintptr_t>(allocated)) -
                        static_cast<std::int64_t>(target + 5);
                    if (displacement >= INT32_MIN && displacement <= INT32_MAX) {
                        return allocated;
                    }
                    VirtualFree(allocated, 0, MEM_RELEASE);
                }
            }
        }
        if (regionEnd <= address) break;
        address = regionEnd;
    }
    return nullptr;
}

// ============================================================
// Hook 安装：InstallFishingLoopHook
// 3 个代码注入：common_exit / success_gate / cancel_request
// ============================================================
static bool InstallFishingLoopHook() {
    if (g_fishingLoopReady) return true;
    const uintptr_t base = G::base;

    unsigned char* hookPoint = reinterpret_cast<unsigned char*>(
        base + RVA_FISHING_COMMON_EXIT);
    unsigned char* successGate = reinterpret_cast<unsigned char*>(
        base + RVA_FISHING_SUCCESS_COMPLETION_GATE);
    unsigned char* cancelPoint = reinterpret_cast<unsigned char*>(
        base + RVA_FISHING_CANCEL_REQUEST);

    // ---- 字节签名验证 ----
    static const unsigned char expectedSuccessGate[7] = {
        0x49, 0x8B, 0x8E, 0x90, 0x02, 0x00, 0x00
    };
    static const unsigned char expectedCommonExit[7] = {
        0x48, 0x8B, 0x05, 0x63, 0x2C, 0xEE, 0x00
    };
    static const unsigned char expectedCancelPhase[11] = {
        0x41, 0xC7, 0x86, 0xDC, 0x01, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00
    };
    // 额外验证：state_dispatch, success_phase, direct_null_exit, success_exit_jump
    static const unsigned char expectedStateDispatch[12] = {
        0x48, 0x63, 0x81, 0xD8, 0x01, 0x00, 0x00, 0x83,
        0xF8, 0x07, 0x0F, 0x87
    };
    static const unsigned char expectedSuccessPhase[10] = {
        0x41, 0xC7, 0x86, 0xDC, 0x01, 0x00, 0x00, 0x06, 0x00, 0x00
    };
    static const unsigned char expectedDirectNullExit[6] = {
        0x0F, 0x84, 0x3F, 0x01, 0x00, 0x00
    };
    static const unsigned char expectedSuccessJump[5] = {
        0xE9, 0xF1, 0x00, 0x00, 0x00
    };

    // 新 RVA 对应的地址（已移至文件作用域）

    const bool successGateOk = memcmp(successGate, expectedSuccessGate,
                                      sizeof(expectedSuccessGate)) == 0;
    const bool commonExitOk = memcmp(hookPoint, expectedCommonExit,
                                     sizeof(expectedCommonExit)) == 0;
    const bool cancelOk = memcmp(cancelPoint, expectedCancelPhase,
                                  sizeof(expectedCancelPhase)) == 0;
    const bool dispatchOk = memcmp(reinterpret_cast<void*>(base + RVA_STATE_DISPATCH),
                                    expectedStateDispatch,
                                    sizeof(expectedStateDispatch)) == 0;
    const bool successPhaseOk = memcmp(reinterpret_cast<void*>(base + RVA_SUCCESS_PHASE),
                                        expectedSuccessPhase,
                                        sizeof(expectedSuccessPhase)) == 0;
    const bool directNullOk = memcmp(reinterpret_cast<void*>(base + RVA_DIRECT_NULL_EXIT),
                                     expectedDirectNullExit,
                                     sizeof(expectedDirectNullExit)) == 0;
    const bool successJumpOk = memcmp(reinterpret_cast<void*>(base + RVA_SUCCESS_EXIT_JUMP),
                                      expectedSuccessJump,
                                      sizeof(expectedSuccessJump)) == 0;

    Log("[AutoFish] stage=loop-signature success_jump=%s success_gate=%s "
        "direct_null_exit=%s common_exit=%s dispatch=%s success_phase=%s "
        "cancel_phase=%s\n",
        successJumpOk ? "OK" : "FAILED", successGateOk ? "OK" : "FAILED",
        directNullOk ? "OK" : "FAILED", commonExitOk ? "OK" : "FAILED",
        dispatchOk ? "OK" : "FAILED",
        successPhaseOk ? "OK" : "FAILED", cancelOk ? "OK" : "FAILED");

    if (!successJumpOk || !successGateOk || !directNullOk || !commonExitOk ||
        !dispatchOk || !successPhaseOk || !cancelOk) {
        Log("[AutoFish] continuous recast disabled safely; original F1 automation retained\n");
        return false;
    }

    // ---- 分配 3 个 stub ----
    unsigned char* stub = static_cast<unsigned char*>(
        AllocateExecutableNear(reinterpret_cast<uintptr_t>(hookPoint), 96));
    if (!stub) {
        Log("[AutoFish] stage=loop-hook near allocation failed (error %lu)\n", GetLastError());
        return false;
    }
    unsigned char* directStub = static_cast<unsigned char*>(
        AllocateExecutableNear(reinterpret_cast<uintptr_t>(successGate), 96));
    if (!directStub) {
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[AutoFish] stage=success-hook near allocation failed (error %lu)\n", GetLastError());
        return false;
    }
    unsigned char* cancelStub = static_cast<unsigned char*>(
        AllocateExecutableNear(reinterpret_cast<uintptr_t>(cancelPoint), 96));
    if (!cancelStub) {
        VirtualFree(directStub, 0, MEM_RELEASE);
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[AutoFish] stage=cancel-hook near allocation failed (error %lu)\n", GetLastError());
        return false;
    }

    // ---- Common Exit stub (68 字节) ----
    // mov rcx,r14; sub rsp,0x20; mov rax,&RearmFishingAtCommonExit; call rax;
    // add rsp,0x20; test al,al; je native;
    // [native] jmp update_epilogue;
    // [native_reconstruct] mov rax,[game_root_slot]; jmp common_exit_continue
    unsigned char code[68] = {
        0x4C, 0x89, 0xF1,                                         // mov rcx, r14
        0x48, 0x83, 0xEC, 0x20,                                   // sub rsp, 0x20
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                       // mov rax, &helper
        0xFF, 0xD0,                                               // call rax
        0x48, 0x83, 0xC4, 0x20,                                   // add rsp, 0x20
        0x84, 0xC0,                                               // test al, al
        0x74, 0x0E,                                               // je +14 (to native reconstruct)
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,           // jmp [rip+0] -> update_epilogue
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                       // mov rax, game_root_slot
        0x48, 0x8B, 0x00,                                         // mov rax, [rax]
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0            // jmp [rip+0] -> common_exit_continue
    };
    *reinterpret_cast<u64*>(code + 9) =
        reinterpret_cast<u64>(&RearmFishingAtCommonExit);
    *reinterpret_cast<u64*>(code + 33) = base + RVA_FISHING_UPDATE_EPILOGUE;
    *reinterpret_cast<u64*>(code + 43) = base + RVA_FISHING_GAME_ROOT_SLOT;
    *reinterpret_cast<u64*>(code + 60) = base + RVA_FISHING_COMMON_EXIT_CONTINUE;
    memcpy(stub, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof(code));

    // ---- Success Gate stub (62 字节) ----
    // mov rcx,r14; sub rsp,0x20; mov rax,&ObserveFishingSuccessGate; call rax;
    // add rsp,0x20; test al,al; je native;
    // [native] jmp update_epilogue;
    // [native_reconstruct] mov rcx,[r14+0x290]; jmp success_gate_continue
    unsigned char directCode[62] = {
        0x4C, 0x89, 0xF1,                                         // mov rcx, r14
        0x48, 0x83, 0xEC, 0x20,                                   // sub rsp, 0x20
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                       // mov rax, &helper
        0xFF, 0xD0,                                               // call rax
        0x48, 0x83, 0xC4, 0x20,                                   // add rsp, 0x20
        0x84, 0xC0,                                               // test al, al
        0x74, 0x0E,                                               // je +14 (to native reconstruct)
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,           // jmp [rip+0] -> update_epilogue
        0x49, 0x8B, 0x8E, 0x90, 0x02, 0x00, 0x00,                 // mov rcx, [r14+0x290]
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0            // jmp [rip+0] -> success_gate_continue
    };
    *reinterpret_cast<u64*>(directCode + 9) =
        reinterpret_cast<u64>(&ObserveFishingSuccessGate);
    *reinterpret_cast<u64*>(directCode + 33) = base + RVA_FISHING_UPDATE_EPILOGUE;
    *reinterpret_cast<u64*>(directCode + 54) = base + RVA_FISHING_SUCCESS_GATE_CONTINUE;
    memcpy(directStub, directCode, sizeof(directCode));
    FlushInstructionCache(GetCurrentProcess(), directStub, sizeof(directCode));

    // ---- Cancel Request stub (48 字节) ----
    // mov rcx,r14; sub rsp,0x20; mov rax,&MarkFishingCancelRequest; call rax;
    // add rsp,0x20;
    // replay 11-byte cancel phase write;
    // jmp cancel_request_continue
    unsigned char cancelCode[48] = {
        0x4C, 0x89, 0xF1,                                         // mov rcx, r14
        0x48, 0x83, 0xEC, 0x20,                                   // sub rsp, 0x20
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                       // mov rax, &helper
        0xFF, 0xD0,                                               // call rax
        0x48, 0x83, 0xC4, 0x20,                                   // add rsp, 0x20
        0x41, 0xC7, 0x86, 0xDC, 0x01, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, // replay cancel phase write
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0            // jmp [rip+0] -> cancel_request_continue
    };
    *reinterpret_cast<u64*>(cancelCode + 9) =
        reinterpret_cast<u64>(&MarkFishingCancelRequest);
    *reinterpret_cast<u64*>(cancelCode + 40) =
        base + RVA_FISHING_CANCEL_REQUEST_CONTINUE;
    memcpy(cancelStub, cancelCode, sizeof(cancelCode));
    FlushInstructionCache(GetCurrentProcess(), cancelStub, sizeof(cancelCode));

    // ---- 封存 stub 内存 ----
    if (!SealExecutableMemory(stub, 96) ||
        !SealExecutableMemory(directStub, 96) ||
        !SealExecutableMemory(cancelStub, 96)) {
        VirtualFree(cancelStub, 0, MEM_RELEASE);
        VirtualFree(directStub, 0, MEM_RELEASE);
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[AutoFish] executable stub sealing failed\n");
        return false;
    }

    // ---- 写入跳转：cancel_request (11 字节: E9 rel32 + 6 NOP) ----
    const std::int64_t cancelDisplacement =
        reinterpret_cast<std::int64_t>(cancelStub) -
        static_cast<std::int64_t>(reinterpret_cast<uintptr_t>(cancelPoint) + 5);
    unsigned char cancelRedirect[11] = {
        0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };
    *reinterpret_cast<std::int32_t*>(cancelRedirect + 1) =
        static_cast<std::int32_t>(cancelDisplacement);
    if (!WriteMem(cancelPoint, cancelRedirect, sizeof(cancelRedirect))) {
        VirtualFree(cancelStub, 0, MEM_RELEASE);
        VirtualFree(directStub, 0, MEM_RELEASE);
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[AutoFish] stage=cancel-hook write failed\n");
        return false;
    }

    // ---- 写入跳转：success_gate (7 字节: E9 rel32 + 2 NOP) ----
    const std::int64_t directDisplacement =
        reinterpret_cast<std::int64_t>(directStub) -
        static_cast<std::int64_t>(reinterpret_cast<uintptr_t>(successGate) + 5);
    unsigned char directRedirect[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    *reinterpret_cast<std::int32_t*>(directRedirect + 1) =
        static_cast<std::int32_t>(directDisplacement);
    if (!WriteMem(successGate, directRedirect, sizeof(directRedirect))) {
        WriteMem(cancelPoint, expectedCancelPhase, sizeof(expectedCancelPhase));
        VirtualFree(cancelStub, 0, MEM_RELEASE);
        VirtualFree(directStub, 0, MEM_RELEASE);
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[AutoFish] stage=success-hook write failed\n");
        return false;
    }

    // ---- 写入跳转：common_exit (7 字节: E9 rel32 + 2 NOP) ----
    const std::int64_t displacement =
        reinterpret_cast<std::int64_t>(stub) -
        static_cast<std::int64_t>(reinterpret_cast<uintptr_t>(hookPoint) + 5);
    unsigned char redirect[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    *reinterpret_cast<std::int32_t*>(redirect + 1) =
        static_cast<std::int32_t>(displacement);
    if (!WriteMem(hookPoint, redirect, sizeof(redirect))) {
        WriteMem(cancelPoint, expectedCancelPhase, sizeof(expectedCancelPhase));
        WriteMem(successGate, expectedSuccessGate, sizeof(expectedSuccessGate));
        VirtualFree(cancelStub, 0, MEM_RELEASE);
        VirtualFree(directStub, 0, MEM_RELEASE);
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[AutoFish] stage=loop-hook write failed\n");
        return false;
    }

    // ---- 回读验证 ----
    const bool directReadback = memcmp(successGate, directRedirect,
                                       sizeof(directRedirect)) == 0;
    const bool cancelReadback = memcmp(cancelPoint, cancelRedirect,
                                       sizeof(cancelRedirect)) == 0;
    const bool commonReadback = memcmp(hookPoint, redirect, sizeof(redirect)) == 0;
    Log("[AutoFish] stage=hook-readback success_gate=%s cancel_request=%s "
        "common_exit=%s\n", directReadback ? "OK" : "FAILED",
        cancelReadback ? "OK" : "FAILED", commonReadback ? "OK" : "FAILED");
    if (!directReadback || !cancelReadback || !commonReadback) {
        WriteMem(cancelPoint, expectedCancelPhase, sizeof(expectedCancelPhase));
        WriteMem(successGate, expectedSuccessGate, sizeof(expectedSuccessGate));
        WriteMem(hookPoint, expectedCommonExit, sizeof(expectedCommonExit));
        VirtualFree(cancelStub, 0, MEM_RELEASE);
        VirtualFree(directStub, 0, MEM_RELEASE);
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[AutoFish] hook readback failed; continuous recast disabled safely\n");
        return false;
    }

    g_fishingLoopStub = stub;
    g_fishingSuccessStub = directStub;
    g_fishingCancelStub = cancelStub;
    g_fishingLoopReady = true;
    Log("[AutoFish] stage=loop-hook installed success_rva=0x%llx success_stub=%p "
        "cancel_rva=0x%llx cancel_stub=%p common_rva=0x%llx common_stub=%p\n",
        static_cast<unsigned long long>(RVA_FISHING_SUCCESS_COMPLETION_GATE), directStub,
        static_cast<unsigned long long>(RVA_FISHING_CANCEL_REQUEST), cancelStub,
        static_cast<unsigned long long>(RVA_FISHING_COMMON_EXIT), stub);
    return true;
}

// ============================================================
// Hook 安装：InstallFishingPostCatchHook
// Phase 4 动画结束 hook
// ============================================================
static bool InstallFishingPostCatchHook() {
    if (g_fishingPostCatchReady) return true;
    const uintptr_t base = G::base;
    unsigned char* target = reinterpret_cast<unsigned char*>(
        base + RVA_FISHING_PHASE4_FINISH);

    static const unsigned char expected[8] = {
        0x49, 0x8B, 0xCE, 0xE8, 0x63, 0x81, 0xFF, 0xFF
    };
    if (memcmp(target, expected, sizeof(expected)) != 0) {
        Log("[AutoFish] stage=post-catch-hook signature FAILED\n");
        return false;
    }

    unsigned char* stub = static_cast<unsigned char*>(
        AllocateExecutableNear(reinterpret_cast<uintptr_t>(target), 96));
    if (!stub) return false;

    // Call RearmFishingAfterCatchAnimation. On success, skip native action-destroy
    // call but continue at local reference cleanup. On failure, replay
    // mov rcx,r14 + call native finish, then continue at NOP.
    unsigned char code[70] = {
        0x4C, 0x89, 0xF1,                                         // mov rcx, r14
        0x48, 0x83, 0xEC, 0x20,                                   // sub rsp, 0x20
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                       // mov rax, &helper
        0xFF, 0xD0,                                               // call rax
        0x48, 0x83, 0xC4, 0x20,                                   // add rsp, 0x20
        0x84, 0xC0,                                               // test al, al
        0x74, 0x0E,                                               // je +14 (to native replay)
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,           // jmp [rip+0] -> phase4_cleanup
        0x49, 0x8B, 0xCE,                                         // mov rcx, r14
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                       // mov rax, &native_action_finish
        0xFF, 0xD0,                                               // call rax
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0            // jmp [rip+0] -> phase4_native_continue
    };
    *reinterpret_cast<u64*>(code + 9) =
        reinterpret_cast<u64>(&RearmFishingAfterCatchAnimation);
    *reinterpret_cast<u64*>(code + 33) = base + RVA_FISHING_PHASE4_CLEANUP;
    *reinterpret_cast<u64*>(code + 46) = base + RVA_FISHING_NATIVE_ACTION_FINISH;
    *reinterpret_cast<u64*>(code + 62) = base + RVA_FISHING_PHASE4_NATIVE_CONTINUE;
    memcpy(stub, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof(code));

    if (!SealExecutableMemory(stub, 96)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[AutoFish] post-catch stub sealing failed\n");
        return false;
    }

    const std::int64_t displacement = reinterpret_cast<std::int64_t>(stub) -
        static_cast<std::int64_t>(reinterpret_cast<uintptr_t>(target) + 5);
    unsigned char redirect[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    *reinterpret_cast<std::int32_t*>(redirect + 1) =
        static_cast<std::int32_t>(displacement);
    if (!WriteMem(target, redirect, sizeof(redirect)) ||
        memcmp(target, redirect, sizeof(redirect)) != 0) {
        WriteMem(target, expected, sizeof(expected));
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[AutoFish] stage=post-catch-hook write/readback FAILED\n");
        return false;
    }

    g_fishingPostCatchStub = stub;
    g_fishingPostCatchReady = true;
    Log("[AutoFish] stage=post-catch-hook installed rva=0x219C75 stub=%p\n", stub);
    return true;
}

// ============================================================
// Hook 安装：InstallFishingMonitorHook
// Monitor trampoline hook 监控钓鱼状态机
// ============================================================
// v1.3.5: 剧情/过场场景检测——在记录时间戳前先读取钓鱼状态，
// 若 state 无效或处于"非钓鱼上下文"（current=0 && requested=0 &&
// task=null），判定为剧情/对话/过场场景，不刷新 g_lastMonitorTick，
// 避免 PollAutoDetect 误判"钓鱼中"而自动开启 NOP 补丁干扰剧情流程。
static bool __fastcall FishingUpdateMonitorDetour(void* state, void* frameContext) {
    // v1.3.0: 记录调用时间用于自动检测
    // v1.3.5: 仅当处于有效钓鱼上下文时才刷新时间戳
    // v1.3.7: 合并 before 读取与剧情检测，减少 ReadFishingState 调用 3→2
    uintptr_t object = 0, vtable = 0;
    u32 beforeCurrent = ~u32{0}, beforeRequested = ~u32{0};
    bool beforeValid = false;
    void* beforeTask = nullptr;
    const bool beforeReadable = ReadFishingState(
        state, &object, &vtable, &beforeCurrent, &beforeRequested,
        &beforeValid, &beforeTask);

    // 剧情场景检测 + 时间戳刷新（复用 before 读取结果）
    if (beforeReadable && beforeCurrent == 0 && beforeRequested == 0 && beforeTask == nullptr) {
        g_lastMonitorTick.store(0, std::memory_order_relaxed);
    } else {
        g_lastMonitorTick.store(GetTickCount64(), std::memory_order_relaxed);
    }

    // state-before 日志（仅状态变化时打印）
    if (beforeReadable &&
        (state != g_lastFishingMonitorState ||
         beforeCurrent != g_lastFishingMonitorCurrent ||
         beforeRequested != g_lastFishingMonitorRequested)) {
        Log("[AutoFish] stage=state-before object=%p current=%u requested=%u "
            "valid_spot=%d task=%p\n", state, beforeCurrent, beforeRequested,
            beforeValid ? 1 : 0, beforeTask);
        g_lastFishingMonitorState = state;
        g_lastFishingMonitorCurrent = beforeCurrent;
        g_lastFishingMonitorRequested = beforeRequested;
    }

    const bool result = g_originalFishingUpdate(state, frameContext);

    // state-after 日志（仅变化时打印）
    uintptr_t afterObject = 0, afterVtable = 0;
    u32 afterCurrent = ~u32{0}, afterRequested = ~u32{0};
    bool afterValid = false;
    void* afterTask = nullptr;
    if (ReadFishingState(state, &afterObject, &afterVtable, &afterCurrent,
                         &afterRequested, &afterValid, &afterTask) &&
        (afterCurrent != beforeCurrent || afterRequested != beforeRequested ||
         afterTask != beforeTask)) {
        Log("[AutoFish] stage=state-after object=%p before=%u/%u after=%u/%u "
            "valid_spot=%d task_before=%p task_after=%p result=%d\n", state,
            beforeCurrent, beforeRequested, afterCurrent, afterRequested,
            afterValid ? 1 : 0, beforeTask, afterTask, result ? 1 : 0);
    }
    return result;
}

static bool InstallFishingMonitorHook() {
    if (g_fishingMonitorReady) return true;
    const uintptr_t base = G::base;
    unsigned char* target = reinterpret_cast<unsigned char*>(
        base + RVA_FISHING_MONITOR_TARGET);
    static constexpr size_t HOOK_LENGTH = 15;
    static const unsigned char expected[HOOK_LENGTH] = {
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48,
        0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20
    };
    if (memcmp(target, expected, sizeof(expected)) != 0) {
        Log("[AutoFish] stage=monitor signature FAILED\n");
        return false;
    }

    unsigned char* trampoline = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!trampoline) return false;

    // trampoline = 15 字节序言副本 + 14 字节绝对跳转回 target+15
    memcpy(trampoline, expected, HOOK_LENGTH);
    unsigned char jumpBack[14] = {0xFF, 0x25, 0, 0, 0, 0};
    *reinterpret_cast<u64*>(jumpBack + 6) =
        base + RVA_FISHING_MONITOR_TARGET + HOOK_LENGTH;
    memcpy(trampoline + HOOK_LENGTH, jumpBack, sizeof(jumpBack));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);

    if (!SealExecutableMemory(trampoline, 64)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("[AutoFish] monitor trampoline sealing failed\n");
        return false;
    }
    g_originalFishingUpdate = reinterpret_cast<FishingUpdateFunction>(trampoline);

    // hook = 14 字节绝对跳转 + 1 NOP
    unsigned char hook[HOOK_LENGTH] = {0xFF, 0x25, 0, 0, 0, 0};
    *reinterpret_cast<u64*>(hook + 6) =
        reinterpret_cast<u64>(&FishingUpdateMonitorDetour);
    hook[14] = 0x90;
    if (!WriteMem(target, hook, sizeof(hook))) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        g_originalFishingUpdate = nullptr;
        Log("[AutoFish] stage=monitor write FAILED\n");
        return false;
    }

    g_fishingMonitorReady = true;
    Log("[AutoFish] stage=monitor installed update_rva=0x218430\n");
    return true;
}

// ============================================================
// Hook 安装：InstallNoTrashHook
// 垃圾排除：在钓鱼 update 函数内 hook 物品判定分支
// stub 逻辑（逆向自 gloaming AutoFish v0.1.5）：
//   1. 从 [rbx]→[rax] 读取即将生成的物品 ID 到 r11d
//   2. 原子递增执行计数器
//   3. 逐一比较 r11d 是否为 4 种垃圾 ID
//   4. 若匹配（垃圾）: 递增跳过计数器，跳到原始 jle 目标（跳过物品生成）
//   5. 若不匹配（鱼）: 执行原始 test r15d,r15d; jle，然后跳回 hook+5 继续原流程
//
// stub 布局（0xC0 = 192 字节）：
//   [0x00] mov rax,[rbx]; mov r11d,[rax]; movabs rcx,&exec_counter
//   [0x10] lock inc [rcx]; cmp r11d,junk1; je skip; ×4
//   [0x37] test r15d,r15d; jle +rel → skip_target（非垃圾但 r15d<=0 路径）
//   [0x3C] jmp [rip+0] → continue（非垃圾且 r15d>0，跳回 hook+5）
//   [0x4A] sub r14,r15; movabs rcx,&skip_counter; lock inc [rcx]（垃圾路径）
//   [0x5A] jmp [rip+0] → skip_target（跳过物品生成）
//   [0x68..] NOP 填充
// ============================================================
static bool InstallNoTrashHook() {
    if (g_noTrashReady) return true;
    const uintptr_t base = G::base;
    unsigned char* target = reinterpret_cast<unsigned char*>(
        base + RVA_NO_TRASH_HOOK);

    // 验证 anchor 签名
    if (memcmp(target, NO_TRASH_ANCHOR, sizeof(NO_TRASH_ANCHOR)) != 0) {
        Log("[AutoFish] stage=no-trash anchor FAILED at rva=0x%llX: got ",
            static_cast<unsigned long long>(RVA_NO_TRASH_HOOK));
        for (size_t i = 0; i < 5; ++i) Log("%02x", target[i]);
        Log("\n");
        return false;
    }

    // 分配 stub 内存（用 AllocateExecutableNear 确保在 hook 点 ±2GB 范围内，
    // 因为 hook 点写入的是 E9 rel32 跳转，需要 32 位范围内的跳转距离）
    unsigned char* stub = static_cast<unsigned char*>(
        AllocateExecutableNear(reinterpret_cast<uintptr_t>(target),
                               NO_TRASH_STUB_SIZE));
    if (!stub) {
        Log("[AutoFish] stage=no-trash stub near-alloc failed\n");
        return false;
    }

    // 原始 5 字节: 45 85 FF 7E 4C
    // test r15d, r15d (3 bytes: 45 85 FF)
    // jle +0x4C (2 bytes: 7E 4C)
    // 原始 jle 目标: hook + 5 + 0x4C = RVA_NO_TRASH_SKIP_TARGET

    // ---- 构建 stub ----
    // [0x00-0x02] mov rax, [rbx]           ; 从 rbx 读取物品结构指针
    stub[0x00] = 0x48; stub[0x01] = 0x8B; stub[0x02] = 0x03;

    // [0x03-0x05] mov r11d, [rax]          ; 从 [rax] 读取物品 ID
    stub[0x03] = 0x44; stub[0x04] = 0x8B; stub[0x05] = 0x18;

    // [0x06-0x0F] movabs rcx, &g_noTrashExecCount
    stub[0x06] = 0x48; stub[0x07] = 0xB9;
    *reinterpret_cast<u64*>(stub + 0x08) = reinterpret_cast<u64>(&g_noTrashExecCount);

    // [0x10-0x12] lock inc dword ptr [rcx]  ; exec++
    stub[0x10] = 0xF0; stub[0x11] = 0xFF; stub[0x12] = 0x01;

    // [0x13..] 4 组 cmp r11d, <junk_id>; je skip
    // 每组 10 字节（7 字节 cmp + 2 字节 je + 1 字节 padding 不需要）
    // 实际: cmp r11d, imm32 = 41 81 FB <4 bytes> = 7 bytes
    //        je rel8 = 74 <offset> = 2 bytes
    //        共 9 bytes per comparison
    // je 目标 = 0x4A（垃圾路径）
    size_t offset = 0x13;
    for (int i = 0; i < 4; ++i) {
        // cmp r11d, imm32
        stub[offset++] = 0x41; stub[offset++] = 0x81; stub[offset++] = 0xFB;
        *reinterpret_cast<u32*>(stub + offset) = JUNK_ITEM_IDS[i];
        offset += 4;
        // je +rel8 → 0x4A（垃圾路径）
        // rel8 相对于 je 的下一条指令（je_addr + 2），此时 offset = je_addr + 1
        stub[offset++] = 0x74;
        stub[offset++] = static_cast<unsigned char>(0x4A - offset - 1);
    }
    // offset 现在应该 = 0x13 + 4*9 = 0x13 + 36 = 0x37

    // [0x37-0x39] test r15d, r15d          ; 原始指令前 3 字节
    stub[0x37] = 0x45; stub[0x38] = 0x85; stub[0x39] = 0xFF;

    // [0x3A-0x3B] jle +rel8 → 0x5A        ; 原始 jle（目标改为 stub 内的 0x5A）
    stub[0x3A] = 0x7E;
    stub[0x3B] = static_cast<unsigned char>(0x5A - 0x3C);  // = 0x1E

    // [0x3C-0x3F] jmp [rip+0]              ; 非垃圾且 r15d>0 → 跳回 continue
    stub[0x3C] = 0xFF; stub[0x3D] = 0x25;
    *reinterpret_cast<u32*>(stub + 0x3E) = 0;  // RIP+0
    // [0x42-0x49] = continue 地址
    *reinterpret_cast<u64*>(stub + 0x42) = base + RVA_NO_TRASH_CONTINUE;

    // [0x4A-0x4C] sub r14, r15             ; 垃圾路径：调整状态
    stub[0x4A] = 0x44; stub[0x4B] = 0x29; stub[0x4C] = 0xFE;

    // [0x4D-0x56] movabs rcx, &g_noTrashSkipCount
    stub[0x4D] = 0x48; stub[0x4E] = 0xB9;
    *reinterpret_cast<u64*>(stub + 0x4F) = reinterpret_cast<u64>(&g_noTrashSkipCount);

    // [0x57-0x59] lock inc dword ptr [rcx]  ; skip++
    stub[0x57] = 0xF0; stub[0x58] = 0xFF; stub[0x59] = 0x01;

    // [0x5A-0x5D] jmp [rip+0]              ; 跳到 skip_target
    stub[0x5A] = 0xFF; stub[0x5B] = 0x25;
    *reinterpret_cast<u32*>(stub + 0x5C) = 0;  // RIP+0
    // [0x60-0x67] = skip_target 地址
    *reinterpret_cast<u64*>(stub + 0x60) = base + RVA_NO_TRASH_SKIP_TARGET;

    // [0x68..] NOP 填充
    memset(stub + 0x68, 0x90, NO_TRASH_STUB_SIZE - 0x68);

    FlushInstructionCache(GetCurrentProcess(), stub, NO_TRASH_STUB_SIZE);

    // 封存 stub
    if (!SealExecutableMemory(stub, NO_TRASH_STUB_SIZE)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[AutoFish] stage=no-trash stub sealing failed\n");
        return false;
    }

    // ---- 写入 hook：5 字节 E9 rel32 ----
    // AllocateExecutableNear 保证 stub 在 ±2GB 范围内，rel32 不会溢出
    const std::int64_t displacement =
        reinterpret_cast<std::int64_t>(stub) -
        static_cast<std::int64_t>(reinterpret_cast<uintptr_t>(target) + 5);

    unsigned char redirect[5] = {0xE9, 0, 0, 0, 0};
    *reinterpret_cast<std::int32_t*>(redirect + 1) =
        static_cast<std::int32_t>(displacement);
    if (!WriteMem(target, redirect, sizeof(redirect)) ||
        memcmp(target, redirect, sizeof(redirect)) != 0) {
        // 回滚
        WriteMem(target, NO_TRASH_ANCHOR, sizeof(NO_TRASH_ANCHOR));
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[AutoFish] stage=no-trash write/readback FAILED\n");
        return false;
    }

    g_noTrashStub = stub;
    g_noTrashReady = true;
    Log("[AutoFish] stage=no-trash hook installed rva=0x%llX stub=%p\n",
        static_cast<unsigned long long>(RVA_NO_TRASH_HOOK), stub);
    return true;
}

// ============================================================
// SetNoTrash：切换垃圾排除开关
// hook 始终在，只是 stub 内部根据 g_noTrashEnabled 决定是否过滤
// 但我们的 stub 实现方式是：hook 始终过滤垃圾
// 如果 g_noTrashEnabled=false，则卸载 hook（恢复原始字节）
// 如果 g_noTrashEnabled=true，则安装 hook
// ============================================================
static bool SetNoTrash(bool enable) {
    if (enable && !g_noTrashReady) {
        if (!InstallNoTrashHook()) return false;
    }
    if (!enable && g_noTrashReady) {
        // 恢复原始字节
        const uintptr_t base = G::base;
        unsigned char* target = reinterpret_cast<unsigned char*>(
            base + RVA_NO_TRASH_HOOK);
        if (!WriteMem(target, NO_TRASH_ANCHOR, sizeof(NO_TRASH_ANCHOR))) {
            Log("[AutoFish] stage=no-trash restore FAILED\n");
            return false;
        }
        g_noTrashReady = false;
        // stub 内存不释放（安全起见，避免悬空指针）
    }
    g_noTrashEnabled.store(enable, std::memory_order_relaxed);
    Log("[AutoFish] no-trash %s (exec=%u skip=%u)\n",
        enable ? "ON" : "OFF",
        g_noTrashExecCount.load(std::memory_order_relaxed),
        g_noTrashSkipCount.load(std::memory_order_relaxed));
    return true;
}

// ============================================================
// InitFishingDetection（v1.3.0）：mod_init 阶段安装 Monitor hook
// Monitor hook 只观察钓鱼状态机，不修改行为，用于自动检测钓鱼状态
// ============================================================
static bool InitFishingDetection() {
    return InstallFishingMonitorHook();
}

// ============================================================
// SetFishing：启用/禁用自动钓鱼
// ============================================================
static bool SetFishing(bool enable) {
    uintptr_t base = G::base;
    if (!base) return false;

    if (!g_fishingAvailable) {
        // 首次启用：验证字节签名 + 安装所有 hook
        bool ok = true;
        for (PatchPoint& point : g_fishingPoints) {
            unsigned char* target = reinterpret_cast<unsigned char*>(base + point.rva);
            bool matchOriginal = memcmp(target, point.original, point.len) == 0;
            bool matchPatched = memcmp(target, kNops, point.len) == 0;
            if (!matchOriginal && !matchPatched) {
                Log("[AutoFish] byte check FAILED rva=0x%llx (%s): got ",
                    static_cast<unsigned long long>(point.rva), point.name);
                for (size_t i = 0; i < point.len; ++i) Log("%02x", target[i]);
                Log("\n");
                ok = false;
            }
        }
        if (!ok) {
            Log("[AutoFish] version mismatch; fishing patch disabled safely\n");
            return false;
        }
        Log("[AutoFish] byte checks passed\n");

        const bool loopReady = InstallFishingLoopHook();
        const bool postCatchReady = InstallFishingPostCatchHook();
        // v1.3.0: Monitor hook 已在 mod_init 中通过 InitFishingDetection() 安装
        if (!loopReady || !postCatchReady || !g_fishingMonitorReady) {
            g_fishingAvailable = false;
            g_fishingInitFailed = true;
            g_fishingEnabled.store(false, std::memory_order_relaxed);
            Log("[AutoFish] required hook chain incomplete; feature disabled safely\n");
            return false;
        }
        g_fishingAvailable = true;
    }

    if (!g_fishingLoopReady || !g_fishingPostCatchReady ||
        !g_fishingMonitorReady) return false;

    // 切换 3 个 NOP 补丁
    unsigned char previous[3][8] = {};
    bool attempted[3] = {};
    for (size_t index = 0; index < 3; ++index) {
        memcpy(previous[index], reinterpret_cast<void*>(base + g_fishingPoints[index].rva),
               g_fishingPoints[index].len);
    }
    bool writeOk = true;
    size_t failedIndex = 3;
    for (size_t index = 0; index < 3; ++index) {
        PatchPoint& point = g_fishingPoints[index];
        const unsigned char* source = enable ? kNops : point.original;
        if (memcmp(previous[index], source, point.len) == 0) continue;
        attempted[index] = true;
        if (!WriteMem(reinterpret_cast<void*>(base + point.rva), source, point.len) ||
            memcmp(reinterpret_cast<void*>(base + point.rva), source, point.len) != 0) {
            Log("[AutoFish] write failed at rva=0x%llx\n",
                static_cast<unsigned long long>(point.rva));
            writeOk = false;
            failedIndex = index;
            break;
        }
    }
    if (!writeOk) {
        bool rollbackOk = true;
        for (size_t index = 0; index < 3; ++index) {
            if (!attempted[index]) continue;
            PatchPoint& point = g_fishingPoints[index];
            const bool restored = WriteMem(
                reinterpret_cast<void*>(base + point.rva), previous[index], point.len) &&
                memcmp(reinterpret_cast<void*>(base + point.rva), previous[index],
                       point.len) == 0;
            rollbackOk = rollbackOk && restored;
        }
        g_fishingAvailable = false;
        Log("[AutoFish] base-patch transaction failed index=%zu rollback=%s\n",
            failedIndex, rollbackOk ? "OK" : "FAILED");
        return false;
    }

    g_fishingEnabled.store(enable, std::memory_order_relaxed);
    Log("[AutoFish] %s continuous_recast=%s\n", enable ? "ENABLED" : "disabled",
        (g_fishingLoopReady && g_fishingPostCatchReady) ? "ready" : "unavailable");
    return true;
}

// ============================================================
// HUD 浮现窗口：左上角显示自动钓鱼状态，5秒后消退
// ============================================================
static HMODULE g_autofishModule = nullptr;
static constexpr wchar_t AUTOFISH_HUD_CLASS[] = L"AutoFishHudWindow";
static HWND g_hudWindow = nullptr;
static HFONT g_hudFont = nullptr;
static HFONT g_hudFontSmall = nullptr;
static int g_hudState = -1;      // 0=关闭, 1=开启, 2=待机（自动钓鱼）
static int g_hudNoTrashState = -1; // 0=关闭, 1=开启（垃圾排除）
static ULONGLONG g_hudHideAt = 0;

static LRESULT CALLBACK HudWndProc(HWND window, UINT message,
                                    WPARAM wParam, LPARAM lParam) {
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

        // 背景
        HBRUSH bg = CreateSolidBrush(RGB(28, 30, 34));
        FillRect(dc, &client, bg);
        DeleteObject(bg);

        // 左侧强调色条
        COLORREF accent;
        const int state = g_hudState;
        if (state == 1) accent = RGB(64, 160, 224);    // 蓝色=自动钓鱼
        else if (state == 2) accent = RGB(200, 170, 60); // 黄色=待机
        else accent = RGB(120, 130, 140);              // 灰色=关闭
        RECT bar = client;
        bar.right = bar.left + 6;
        HBRUSH accentBrush = CreateSolidBrush(accent);
        FillRect(dc, &bar, accentBrush);
        DeleteObject(accentBrush);

        SetBkMode(dc, TRANSPARENT);

        // 标题
        SetTextColor(dc, RGB(150, 150, 155));
        HFONT oldFont = (HFONT)SelectObject(dc, g_hudFontSmall);
        RECT titleRect = client;
        titleRect.left += 22;
        titleRect.right -= 14;
        titleRect.bottom = titleRect.top + 22;
        DrawTextW(dc, L"\x81EA\x52A8\x9493\x9C7C", -1, &titleRect,  // 自动钓鱼
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // 状态文字
        const wchar_t* stateText;
        COLORREF stateColor;
        if (state == 1) {
            stateText = L"\x5F00\x542F";  // 开启
            stateColor = RGB(100, 200, 240);
        } else if (state == 2) {
            stateText = L"\x5F85\x673A";  // 待机
            stateColor = RGB(220, 190, 100);
        } else {
            stateText = L"\x5173\x95ED";  // 关闭
            stateColor = RGB(200, 200, 210);
        }
        SelectObject(dc, g_hudFont);
        SetTextColor(dc, stateColor);
        RECT stateRect = client;
        stateRect.left += 22;
        stateRect.right -= 14;
        stateRect.top += 22;
        stateRect.bottom = stateRect.top + 32;  // 限定高度，避免 VCENTER 居中过低
        DrawTextW(dc, stateText, -1, &stateRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // 垃圾排除状态行
        const int ntState = g_hudNoTrashState;
        if (ntState >= 0) {
            const wchar_t* ntText;
            COLORREF ntColor;
            if (ntState == 1) {
                ntText = L"\x5783\x573E\x6392\x9664: \x5F00\x542F";  // 垃圾排除: 开启
                ntColor = RGB(120, 220, 120);  // 绿色
            } else {
                ntText = L"\x5783\x573E\x6392\x9664: \x5173\x95ED";  // 垃圾排除: 关闭
                ntColor = RGB(160, 160, 170);
            }
            SetTextColor(dc, ntColor);
            RECT ntRect = client;
            ntRect.left += 22;
            ntRect.right -= 14;
            ntRect.top += 58;
            ntRect.bottom = ntRect.top + 30;  // 限定高度
            DrawTextW(dc, ntText, -1, &ntRect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

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
    cls.hInstance = g_autofishModule;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    cls.lpszClassName = AUTOFISH_HUD_CLASS;
    if (!RegisterClassExW(&cls)) {
        DWORD err = GetLastError();
        WNDCLASSEXW existing = {};
        existing.cbSize = sizeof(existing);
        if (err != ERROR_CLASS_ALREADY_EXISTS ||
            !GetClassInfoExW(g_autofishModule, AUTOFISH_HUD_CLASS, &existing) ||
            existing.lpfnWndProc != HudWndProc ||
            existing.hInstance != g_autofishModule) {
            Log("[AutoFish] [HUD] window class register failed (err=%lu)", err);
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
        Log("[AutoFish] [HUD] font creation failed");
        return false;
    }

    const int width = 180;
    const int height = 108;
    g_hudWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        AUTOFISH_HUD_CLASS, L"", WS_POPUP, 0, 0, width, height,
        nullptr, nullptr, g_autofishModule, nullptr);
    if (!g_hudWindow) {
        Log("[AutoFish] [HUD] CreateWindowExW failed (err=%lu)", GetLastError());
        return false;
    }
    // v1.5.11: 移除游戏窗口 owner 绑定（GWLP_HWNDPARENT）——owned TOPMOST 窗口链
    // 干扰 Alt+Tab 前台切换，游戏窗口切不回（切窗修复第四轮，详见 game_window.h v1.5）

    SetLayeredWindowAttributes(g_hudWindow, 0, 228, LWA_ALPHA);
    HRGN rounded = CreateRoundRectRgn(0, 0, width + 1, height + 1, 12, 12);
    if (!SetWindowRgn(g_hudWindow, rounded, FALSE)) DeleteObject(rounded);

    Log("[AutoFish] [HUD] overlay window ready");
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
    const int height = 108;
    const int x = mi.rcWork.left + 24;
    const int y = mi.rcWork.top + 24;
    SetWindowPos(g_hudWindow, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void RefreshHud(bool enabled, bool noTrashEnabled = false,
                       bool autoDetect = true) {
    if (!g_hudWindow && !InitHud()) return;
    if (!autoDetect)       g_hudState = 0;  // 关闭
    else if (enabled)      g_hudState = 1;  // 开启
    else                   g_hudState = 2;  // 待机
    g_hudNoTrashState = noTrashEnabled ? 1 : 0;
    UpdateHudPosition();
    InvalidateRect(g_hudWindow, nullptr, TRUE);
    UpdateWindow(g_hudWindow);
    g_hudHideAt = GetTickCount64() + 5000;
}

static void PumpHud() {
    if (!g_hudWindow) return;
    if (!QolGameInForeground()) { QolHudGuardVisibility(g_hudWindow); return; }  // v1.6: 失焦守卫兜底（alpha 渐隐）后 pump 静默
    MSG msg = {};
    while (PeekMessageW(&msg, g_hudWindow, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (IsWindowVisible(g_hudWindow) && g_hudHideAt != 0 &&
        GetTickCount64() >= g_hudHideAt) {
        QolHudMarkHiddenByMod(g_hudWindow);  // MOD 主动隐藏：清守卫标记
        ShowWindow(g_hudWindow, SW_HIDE);
        g_hudHideAt = 0;
    }
}

// ============================================================
// 开关切换（v1.5.0：供 QoL 快捷菜单调用，原独立热键已移除）
// ============================================================
static void ToggleAutoFish() {
    bool currentAuto = g_autoDetectMode.load(std::memory_order_relaxed);
    if (currentAuto) {
        // 关闭自动检测 + 禁用钓鱼
        g_autoDetectMode.store(false, std::memory_order_relaxed);
        if (g_fishingEnabled.load(std::memory_order_relaxed)) {
            SetFishing(false);
        }
        Log("[AutoFish] 切换: 自动检测 关闭，自动钓鱼已禁用");
        RefreshHud(false, g_noTrashEnabled.load(std::memory_order_relaxed), false);
    } else {
        // 开启自动检测
        g_autoDetectMode.store(true, std::memory_order_relaxed);
        g_fishingInitFailed = false;  // 重置失败标志，允许重试
        Log("[AutoFish] 切换: 自动检测 开启");
        RefreshHud(g_fishingEnabled.load(std::memory_order_relaxed),
                   g_noTrashEnabled.load(std::memory_order_relaxed), true);
    }
}

static void ToggleNoTrash() {
    bool current = g_noTrashEnabled.load(std::memory_order_relaxed);
    bool next = !current;
    if (SetNoTrash(next)) {
        Log("[AutoFish] 垃圾排除: %s -> %s (exec=%u skip=%u)",
            current ? "开启" : "关闭",
            next ? "开启" : "关闭",
            g_noTrashExecCount.load(std::memory_order_relaxed),
            g_noTrashSkipCount.load(std::memory_order_relaxed));
        RefreshHud(g_fishingEnabled.load(std::memory_order_relaxed), next,
                   g_autoDetectMode.load(std::memory_order_relaxed));
    }
}

static bool IsAutoFishOn() {
    return g_autoDetectMode.load(std::memory_order_relaxed);
}

static bool IsNoTrashOn() {
    return g_noTrashEnabled.load(std::memory_order_relaxed);
}

// ============================================================
// QoL 快捷菜单导出（宿主：ModManager F1）
// ============================================================
extern "C" __declspec(dllexport) int QolQuickMenuItems(
        QolQuickMenuItem* items, int maxItems) {
    int n = 2;  // 自动钓鱼 + 垃圾排除
    if (items && maxItems >= 1)
        items[0] = { "自动钓鱼", IsAutoFishOn, ToggleAutoFish };
    if (items && maxItems >= 2)
        items[1] = { "垃圾排除", IsNoTrashOn, ToggleNoTrash };
    return n;
}

// ============================================================
// v1.3.0: 自动检测钓鱼状态，动态开关 NOP 补丁
// ============================================================
static void PollAutoDetect() {
    if (!g_autoDetectMode.load(std::memory_order_relaxed)) return;
    if (!g_fishingMonitorReady) return;

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG lastMonitor = g_lastMonitorTick.load(std::memory_order_relaxed);
    const ULONGLONG sinceMonitor = (lastMonitor > 0) ? (now - lastMonitor) : 0xFFFFFFFF;

    const bool enabled = g_fishingEnabled.load(std::memory_order_relaxed);

    if (!enabled && sinceMonitor < MONITOR_FISHING_TIMEOUT) {
        if (g_fishingInitFailed) return;  // hook 安装失败，不重试
        // 钓鱼中，自动开启
        if (SetFishing(true)) {
            Log("[AutoFish] 自动检测: 钓鱼中 -> 自动开启");
            // 不弹 HUD：自动检测模式下的状态变化不干扰玩家
        }
    } else if (enabled && sinceMonitor > MONITOR_NOTFISHING_TIMEOUT) {
        // 已停止钓鱼，自动关闭
        if (SetFishing(false)) {
            Log("[AutoFish] 自动检测: 停止钓鱼 -> 自动关闭");
            // 不弹 HUD：自动检测模式下的状态变化不干扰玩家
        }
    }
}

// ============================================================
// Unload：还原所有已安装 hook + 释放 stub 内存
// 1) Monitor trampoline hook：还原原始 15 字节序言 + 释放 trampoline
// 2) Loop hook：还原 3 个代码注入点的原始字节 + 释放 3 个 stub
// 3) PostCatch hook：还原原始 8 字节 + 释放 stub
// 4) NoTrash hook：还原原始 5 字节 anchor（stub 不释放，避免悬空）
// 5) NOP 补丁：还原 3 个补丁点原始字节
// ============================================================
static void RestoreAllHooks() {
    const uintptr_t base = G::base;
    if (!base) return;

    // ---- Monitor hook ----
    if (g_fishingMonitorReady) {
        unsigned char* target = reinterpret_cast<unsigned char*>(
            base + RVA_FISHING_MONITOR_TARGET);
        static const unsigned char monitorOriginal[15] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48,
            0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20
        };
        WriteMem(target, monitorOriginal, sizeof(monitorOriginal));
        g_fishingMonitorReady = false;
        Log("[AutoFish] unload: monitor hook restored\n");
    }

    // ---- Loop hook（3 个代码注入点）----
    if (g_fishingLoopReady) {
        // common_exit: 7 字节原始字节
        static const unsigned char loopCommonExit[7] = {
            0x48, 0x8B, 0x05, 0x63, 0x2C, 0xEE, 0x00
        };
        WriteMem(reinterpret_cast<void*>(base + RVA_FISHING_COMMON_EXIT),
                 loopCommonExit, sizeof(loopCommonExit));
        // success_gate: 7 字节原始字节
        static const unsigned char loopSuccessGate[7] = {
            0x49, 0x8B, 0x8E, 0x90, 0x02, 0x00, 0x00
        };
        WriteMem(reinterpret_cast<void*>(base + RVA_FISHING_SUCCESS_COMPLETION_GATE),
                 loopSuccessGate, sizeof(loopSuccessGate));
        // cancel_request: 11 字节原始字节
        static const unsigned char loopCancelPhase[11] = {
            0x41, 0xC7, 0x86, 0xDC, 0x01, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00
        };
        WriteMem(reinterpret_cast<void*>(base + RVA_FISHING_CANCEL_REQUEST),
                 loopCancelPhase, sizeof(loopCancelPhase));
        g_fishingLoopReady = false;
        Log("[AutoFish] unload: loop hook restored\n");
    }

    // ---- PostCatch hook ----
    if (g_fishingPostCatchReady) {
        unsigned char* target = reinterpret_cast<unsigned char*>(
            base + RVA_FISHING_PHASE4_FINISH);
        static const unsigned char postCatchOriginal[8] = {
            0x49, 0x8B, 0xCE, 0xE8, 0x63, 0x81, 0xFF, 0xFF
        };
        WriteMem(target, postCatchOriginal, sizeof(postCatchOriginal));
        g_fishingPostCatchReady = false;
        Log("[AutoFish] unload: post-catch hook restored\n");
    }

    // ---- NoTrash hook ----
    if (g_noTrashReady) {
        unsigned char* target = reinterpret_cast<unsigned char*>(
            base + RVA_NO_TRASH_HOOK);
        WriteMem(target, NO_TRASH_ANCHOR, sizeof(NO_TRASH_ANCHOR));
        g_noTrashReady = false;
        Log("[AutoFish] unload: no-trash hook restored\n");
    }

    // ---- NOP 补丁 ----
    if (g_fishingAvailable) {
        for (size_t i = 0; i < 3; ++i) {
            WriteMem(reinterpret_cast<void*>(base + g_fishingPoints[i].rva),
                     g_fishingPoints[i].original, g_fishingPoints[i].len);
        }
        g_fishingAvailable = false;
        g_fishingEnabled.store(false, std::memory_order_relaxed);
        Log("[AutoFish] unload: NOP patches restored\n");
    }
}
extern "C" __declspec(dllexport) void mod_init(void) {
    LogOpen("autofish");
    Log("[AutoFish] mod_init 开始 (v1.5.5-diag v1.20)");
    HotConfig_Register("autofish", (void*)&RVA_FISHING_COMMON_EXIT, "RVA_FISHING_COMMON_EXIT", HOT_RVA, 0x219F46);
    HotConfig_Register("autofish", (void*)&RVA_FISHING_COMMON_EXIT_CONTINUE, "RVA_FISHING_COMMON_EXIT_CONTINUE", HOT_RVA, 0x219F4D);
    HotConfig_Register("autofish", (void*)&RVA_FISHING_UPDATE_EPILOGUE, "RVA_FISHING_UPDATE_EPILOGUE", HOT_RVA, 0x219FC1);
    HotConfig_Register("autofish", (void*)&RVA_FISHING_SUCCESS_COMPLETION_GATE, "RVA_FISHING_SUCCESS_COMPLETION_GATE", HOT_RVA, 0x219DF7);
    HotConfig_Register("autofish", (void*)&RVA_FISHING_SUCCESS_GATE_CONTINUE, "RVA_FISHING_SUCCESS_GATE_CONTINUE", HOT_RVA, 0x219DFE);
    HotConfig_Register("autofish", (void*)&RVA_FISHING_CANCEL_REQUEST, "RVA_FISHING_CANCEL_REQUEST", HOT_RVA, 0x218963);
    HotConfig_Register("autofish", (void*)&RVA_FISHING_CANCEL_REQUEST_CONTINUE, "RVA_FISHING_CANCEL_REQUEST_CONTINUE", HOT_RVA, 0x21896E);
    HotConfig_Register("autofish", (void*)&RVA_FISHING_PHASE4_FINISH, "RVA_FISHING_PHASE4_FINISH", HOT_RVA, 0x219C75);
    HotConfig_Register("autofish", (void*)&RVA_FISHING_PHASE4_CLEANUP, "RVA_FISHING_PHASE4_CLEANUP", HOT_RVA, 0x219C7E);
    HotConfig_Register("autofish", (void*)&RVA_FISHING_PHASE4_NATIVE_CONTINUE, "RVA_FISHING_PHASE4_NATIVE_CONTINUE", HOT_RVA, 0x219C7D);
    HotConfig_Register("autofish", (void*)&RVA_FISHING_NATIVE_ACTION_FINISH, "RVA_FISHING_NATIVE_ACTION_FINISH", HOT_RVA, 0x211DE0);
    HotConfig_Register("autofish", (void*)&RVA_FISHING_VTABLE, "RVA_FISHING_VTABLE", HOT_RVA, 0xE387F8);
    HotConfig_Register("autofish", (void*)&RVA_FISHING_GAME_ROOT_SLOT, "RVA_FISHING_GAME_ROOT_SLOT", HOT_RVA, 0x10FCBB0);
    HotConfig_Register("autofish", (void*)&RVA_FISHING_SAVE_DATA_VTABLE, "RVA_FISHING_SAVE_DATA_VTABLE", HOT_RVA, 0xE2D838);
    HotConfig_Register("autofish", (void*)&RVA_FISHING_MONITOR_TARGET, "RVA_FISHING_MONITOR_TARGET", HOT_RVA, 0x218430);
    HotConfig_Register("autofish", (void*)&RVA_NO_TRASH_HOOK, "RVA_NO_TRASH_HOOK", HOT_RVA, 0x21B0C5);
    HotConfig_Register("autofish", (void*)&RVA_NO_TRASH_CONTINUE, "RVA_NO_TRASH_CONTINUE", HOT_RVA, 0x21B0CA);
    HotConfig_Register("autofish", (void*)&RVA_NO_TRASH_SKIP_TARGET, "RVA_NO_TRASH_SKIP_TARGET", HOT_RVA, 0x21B116);
    HotConfig_Register("autofish", (void*)&FISHING_CURRENT_PHASE_OFFSET, "FISHING_CURRENT_PHASE_OFFSET", HOT_RVA, 0x1D8);
    HotConfig_Register("autofish", (void*)&FISHING_REQUESTED_PHASE_OFFSET, "FISHING_REQUESTED_PHASE_OFFSET", HOT_RVA, 0x1DC);
    HotConfig_Register("autofish", (void*)&FISHING_VALID_SPOT_OFFSET, "FISHING_VALID_SPOT_OFFSET", HOT_RVA, 0x270);
    HotConfig_Register("autofish", (void*)&FISHING_TASK_OFFSET, "FISHING_TASK_OFFSET", HOT_RVA, 0x290);
    HotConfig_Register("autofish", (void*)&FISHING_SPECIAL_RESULT_OFFSET, "FISHING_SPECIAL_RESULT_OFFSET", HOT_RVA, 0x2B8);
    HotConfig_Register("autofish", (void*)&FISHING_WAIT_TICKS_OFFSET, "FISHING_WAIT_TICKS_OFFSET", HOT_RVA, 0x2F8);
    HotConfig_Register("autofish", (void*)&SAVE_DATA_OFFSET, "SAVE_DATA_OFFSET", HOT_RVA, 0x208);
    HotConfig_Register("autofish", (void*)&SAVE_RAW_SECOND_OFFSET, "SAVE_RAW_SECOND_OFFSET", HOT_RVA, 0x3270);
    HotConfig_Register("autofish", (void*)&RVA_SUCCESS_EXIT_JUMP, "RVA_SUCCESS_EXIT_JUMP", HOT_RVA, 0x219E50);
    HotConfig_Register("autofish", (void*)&RVA_DIRECT_NULL_EXIT, "RVA_DIRECT_NULL_EXIT", HOT_RVA, 0x219E01);
    HotConfig_Register("autofish", (void*)&RVA_STATE_DISPATCH, "RVA_STATE_DISPATCH", HOT_RVA, 0x218480);
    HotConfig_Register("autofish", (void*)&RVA_SUCCESS_PHASE, "RVA_SUCCESS_PHASE", HOT_RVA, 0x219154);
    HotConfig_Poll();
    HotConfig_DumpCE("autofish");
    // v1.5.0: 热键 9/F10 移除，开关改由 QoL 快捷菜单（F1）驱动

    G::base = (uintptr_t)GetModuleHandleW(nullptr);
    Log("[AutoFish] 游戏基址: 0x%llX", (unsigned long long)G::base);

    // v1.3.0: mod_init 阶段安装 Monitor trampoline（只观察，不修改行为）
    // 用于自动检测玩家是否在钓鱼状态
    if (InitFishingDetection()) {
        Log("[AutoFish] Monitor hook 已安装，自动检测就绪");
    } else {
        Log("[AutoFish] Monitor hook 安装失败，自动检测不可用");
    }

    // v1.3.0: 自动检测模式默认开启
    // 钓鱼时自动开启 NOP 补丁，停止钓鱼时自动关闭，过剧情无需手动操作
    Log("[AutoFish] 自动检测模式默认开启（F1 快捷菜单可切换）");

    // 垃圾排除默认关闭
    Log("[AutoFish] 垃圾排除默认关闭（F1 快捷菜单可切换）");

    G::ready = true;
    Log("[AutoFish] mod_init 完成 (ready=%d auto_detect=%d monitor=%d fishing=%d no_trash=%d)",
        G::ready ? 1 : 0,
        g_autoDetectMode.load(std::memory_order_relaxed) ? 1 : 0,
        g_fishingMonitorReady ? 1 : 0,
        g_fishingEnabled.load(std::memory_order_relaxed) ? 1 : 0,
        g_noTrashEnabled.load(std::memory_order_relaxed) ? 1 : 0);
}

extern "C" __declspec(dllexport) void mod_tick(void) {
    qol::budget::BeginFrame();  // P0: 帧锚定（幂等，多 DLL 安全）
    static int s_bdgSlot = -1;  // P0 探针（惰性注册，析构自动上报）
    if (s_bdgSlot < 0) s_bdgSlot = qol::budget::Slot("AutoFish");
    struct BdgGuard {
        int slot; uint64_t t0;
        ~BdgGuard() { qol::budget::Report(slot, qol::budget::NowUs() - t0); }
    } bdgGuard = { s_bdgSlot, qol::budget::NowUs() };

    if (!G::ready) return;
    HotConfig_Poll();
    if (!QolGameBusy()) PollAutoDetect();  // P1-2: 载入/菜单期间静默
    PumpHud();
    QolHudGuardVisibility(g_hudWindow);  // v1.5.6: 失焦隐藏 HUD（不飘桌面）
}

extern "C" __declspec(dllexport) void unload(void) {
    Log("[AutoFish] unload begin");
    RestoreAllHooks();
    Log("[AutoFish] unload complete");
    LogClose();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)lpReserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_autofishModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

