// autoharvest.cpp —— AutoHarvest: 自动收集树液提取器中的树液 (v1.9.14 正式版转正：审查通过——DIAG宏0/时间链正确/F4可改键/8键v1.6.2已移除/F9诊断#if0)
//
// v1.9.2: F4 设箱触发收紧（用户反馈：菜单里按 F4 也弹“自动采集已开启”，
//         与“对着箱子按才启动”的设计预期不符）——设箱搜索半径 300→95
//         （一格范围，参照链式自动化的格距标准），只有站在箱子旁一格内
//         才能设上，远处/菜单内误触大幅减少；设箱失败增加 HUD 提示
//         （“附近没有箱子/请靠近箱子再按”），避免按了没反应的困惑。
// v1.9.1: 引用计数泄漏修复——SapTransferSlot 中 AddRef(4e) 之后的 8 个失败
//         路径均未释放 owned 引用（每次失败泄漏一个 0x2C0 僵尸物品）。
//         引用计数语义经反汇编实锤：memset 归零后 ctor(0xFF870) 不写 [item+8]
//         （引用从 0 起）；4e 的 AddRef 即唯一 owned ref；intrusiveRelease
//         归零走 vtable[0] 销毁，与游戏原生释放（0x166510）一致；ctor 内
//         0x13B4F0(save,item) 为 FNV 哈希 ID 登记表不持引用 → 失败路径
//         单次释放无双释放风险（v1.9.0 前的“宁可泄漏”系对注册语义误判）。
//         成功路径账目原本平衡（入槽 +2-1=槽位持有；全合并 +1-1=销毁）。
// v1.9.0: 空/失败 60s 重试、玩家移动 1 格立即重搜（搜索半径 1200）。
// v1.8.9: 启动卡顿优化——①搜索半径 5000→1200（首搜候选减 90%+）；②AutoRun 首搜延迟 1 秒（避开 F4 帧峰值）；③aobscan 共享库 memchr+.text 限定提速（游戏加载期 AOB 扫描几百 ms→几十 ms）
//
// v1.8.7: 代码审计修复——①增量触发缓存提取器列表（保底路径复用指针不跳过采集）；
//         ②DWORD 无符号溢出修复首次延迟；③TransferToChest AddRef/Release 对称化；
//         ④诊断数据采集移除（发布版不再执行哈希表查找）；⑤日志先赋值再打印修复；
//         ⑥多槽提取器 count 键 fallback 修复。
// v1.8.6: 掉帧优化——①搜索半径 10000→5000；②增量触发+帧内预算；③移除诊断计时。
// v1.8.5-diag: FPS 掉帧排查诊断版
// v1.8.1: ① F9 诊断触发器移除（发布版不应有诊断代码）；
// v1.8.0: 死档风险修复（3 项高危）：
//          ① AH-H1: 删除 SapClearExtractorSlot 死代码（含手写引用计数+vtable 析构+nullptr 写 backing）
//          ② AH-H3: 合并失败假回滚→真正回滚（反向 itemAdjust 撤销已成功合并）
//          ③ AH-H2: SapTransferSlot 清槽前先读游戏时间，失败则 abort 整个清槽，
//                    避免"map 已清零但时间戳未重置"的不一致状态（会导致空指针崩溃）
//
// v1.7.0: ① 恢复时间判断——参考 production_automation.inl, count>0 不代表完成,
//             gameSecond-start>=duration 才算完成。v1.6.2 去掉时间判断导致收集了未完成的树液。
//          ② 移除 DoHarvestCycle 中的 RefreshHud——采集时不应反复弹 HUD,
//             HUD 只在 F4 按下时显示。
//          ③ 保留 v1.6.2 的 F4 单键设计（去掉 8 键）
//
// 功能：
//   1. 自动收集树液提取器中的树液（转移到存储箱子）
//   2. 地上的果实/物品不收集
//
// 快捷键：
//   F4: 设置/取消存储箱子（设箱=自动开始采集，再按=取消+停止）
//
// 架构：
//   - 空间搜索框架（来自 ChestSort）：搜索附近的 CGimmickStatus 对象
//   - 物品转移（来自 ChestSort）：itemAdjust + intrusiveRelease
//   - 树液提取器产出物化：自建链路（allocate→ctor→setCount→入箱）
//   - 切换模式（来自 AutoFish）：GetAsyncKeyState 边沿触发 + HUD
//
// 安全措施：
//   - IsReadable 检查所有指针访问
//   - SEH 包裹所有原生函数调用
//   - 字节签名验证后才能调用游戏函数
//   - 未设置存储箱子时不执行采集

#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <atomic>

#include "logging.h"
#include "budget.h"        // P0 帧耗时探针（跨 DLL 共享）
#include "state.h"         // P1-2 状态感知暂停（载入/菜单静默）
#include "hotkey.h"
#include "game_window.h"   // HUD owner 绑定游戏窗口
#include "patch_safety.h"

// 日志开关：发布版禁用日志输出
// 调试时取消注释下行即可开启日志
// #define AUTOHARVEST_LOGGING   // v1.9.14 转正：日志关闭（定位时取消注释重编）
#ifdef AUTOHARVEST_LOGGING
#else
  #define LogOpen(x)  ((void)0)
  #define Log(...)    ((void)0)
  #define LogV(...)   ((void)0)
  #define LogClose()  ((void)0)
#endif

// [diag] 诊断日志门控（默认关）；调试时 #define DIAG_AUTOHARVEST 1 开启
#ifndef DIAG_AUTOHARVEST
#define DIAG_AUTOHARVEST 0
#endif

using u64 = std::uint64_t;
using u32 = std::uint32_t;

// ============================================================
// 常量（build 25311578 / v1.20，2026-09-30 由 ChestSort/ProductionAuto 实测迁移）
// ============================================================
static constexpr uintptr_t RVA_GAME_ROOT = 0x10FCBB0;

// ---- 指针链偏移（与 ChestSort / AutoFish 一致）----
static constexpr uintptr_t ROOT_PLAYER_OFFSET       = 0x208;
static constexpr uintptr_t PLAYER_OBJECT_STATUS_OFFSET = 0x32b8;
static constexpr uintptr_t PLAYER_ITEMS_OFFSET         = 0x32c0;

// ---- 世界对象注册表 / 空间搜索（与 ChestSort 一致）----
// v1.09 (build 25094764): RTTI 链扫描确认 vtable 从 0xE17168 移至 0xE1C168
static constexpr uintptr_t WORLD_OBJECT_REGISTRY_RVA     = 0x1104C80;
static constexpr uintptr_t SEARCH_CALLBACK_VTABLE_RVA    = 0xE3E608;   // v1.20 ALL_OBJECTS vtable（与 ChestSort 一致）
static constexpr uintptr_t ROOT_MAP_OWNER_OFFSET    = 0x268;
static constexpr uintptr_t MAP_INFO_OFFSET          = 0x0d8;
static constexpr uintptr_t MAP_SPATIAL_OWNER_OFFSET = 0x030;
static constexpr uintptr_t MAP_SPATIAL_INDEX_OFFSET = 0x6e0;

// ---- Gimmick / 物品偏移（与 ChestSort 一致）----
static constexpr uintptr_t GIMMICK_DATA_HOLDER_OFFSET  = 0x240;
static constexpr uintptr_t GIMMICK_MODULE_NAME_OFFSET = 0x0E0;  // v1.20: 0xD8→0xE0
static constexpr uintptr_t GIMMICK_POSITION_OFFSET    = 0x0f0;
static constexpr uintptr_t STATUS_ITEMS_OFFSET        = 0x2b8;
static constexpr uintptr_t ITEM_DATA_HOLDER_OFFSET    = 0x240;
static constexpr uintptr_t ITEM_STACK_COUNT_OFFSET    = 0x260;
static constexpr uintptr_t ITEM_RANK_OFFSET           = 0x280;
static constexpr uintptr_t WORLD_OBJECT_POSITION_OFFSET = 0x230;



// ---- 空间搜索/物品（与 ChestSort 一致）----
static constexpr uintptr_t RVA_SPATIAL_SEARCH     = 0;  // AOB 扫描获得
static constexpr uintptr_t RVA_RAW_VECTOR_FREE    = 0;  // AOB 扫描获得
static constexpr uintptr_t RVA_INTRUSIVE_RELEASE   = 0;  // AOB 扫描获得
static constexpr uintptr_t RVA_ITEM_ADJUST         = 0;  // AOB 扫描获得
static constexpr uintptr_t RVA_PLAYER_CAPACITY     = 0;  // AOB 扫描获得
static constexpr uintptr_t RVA_BATCH_RECALCULATE   = 0x1470C0;
static constexpr uintptr_t RVA_BATCH_UI_REFRESH    = 0x0FB170;  // v1.20（与 ChestSort 一致）
static constexpr uintptr_t RVA_BATCH_DIRTY         = 0;  // AOB 扫描获得

// ---- 生产机状态结构偏移（build 25094764 / v1.09，从 ProductionAuto 适配）----
// 树液提取器（被动机器）产出存储在 status map（哈希表）而不是 0x2b8 数组：
//   status+0x58 存键值：键 itemID0/1/2 → 值 = 物品 ID（u64）
//   status+0x18 存键值：键 count → 值 = 产出数量（int，2026-09-02 实测）
//   0x2b8 是固定 3x8 backing 指针数组（不是 RawPointerVector）
static constexpr uintptr_t SAP_DATA_LEVEL_POINTER_OFFSET = 0x128;  // gimmick data + 0x128 = level CString 指针
static constexpr uintptr_t SAP_DATA_LEVEL_LENGTH_OFFSET  = 0x130;  // gimmick data + 0x130 = level 字符串长度
static constexpr uintptr_t SAP_STATUS_ITEMID_MAP_OFFSET  = 0x58;   // status 哈希表（itemID{slot} → u64）
static constexpr uintptr_t SAP_STATUS_ITEMVAL_MAP_OFFSET  = 0x18;  // status 哈希（count → int，产出数量）
static constexpr uintptr_t SAP_STATUS_TIME_VALUE_OFFSET   = 0x288; // 槽位 start 时间（u64 × 3）
static constexpr uintptr_t SAP_STATUS_CREATE_TIME_OFFSET = 0x2a0;  // 槽位 duration（u64 × 3）
static constexpr uintptr_t SAP_STATUS_ITEM_LIST_OFFSET   = 0x2b8;  // 固定 3x8 backing 数组指针
static constexpr size_t    SAP_MACHINE_SLOTS            = 3;
static constexpr size_t    SAP_SLOT_INGREDIENTS         = 8;   // 每槽 8 个条目（level1 只提交前 8 个）
static constexpr size_t    SAP_CHEST_SLOTS              = 30;  // 存储箱子 30 格

// ---- 生产函数 RVA（build 25311578 / v1.20，ProductionAuto v1.1.43-diag 实测值）----
static constexpr uintptr_t RVA_SAP_OUTPUT_HELPER        = 0x170280;  // GimmickProcessPopItem 内部完成辅助
static constexpr uintptr_t RVA_SAP_STATUS_INT_LOOKUP    = 0x15AE10;  // status+0x18 map 查找（find-or-insert）
static constexpr uintptr_t RVA_SAP_STATUS_U64_LOOKUP    = 0x15AFD0;  // status+0x58 map 查找（find-or-insert）
static constexpr uintptr_t RVA_SAP_STATUS_EVENT_LOOKUP  = 0x15A9F0;  // 事件 owner map 查找
static constexpr uintptr_t RVA_SAP_STATUS_EVENT_NOTIFY  = 0x15A810;  // 事件通知
static constexpr uintptr_t RVA_SAP_ITEM_ALLOCATE        = 0x8D2980;  // 物品内存分配（size→ptr）
static constexpr uintptr_t RVA_SAP_ITEM_CTOR            = 0x109100;  // CItemStatus 构造（memory, save, itemId）
static constexpr uintptr_t RVA_SAP_ITEM_SET_COUNT       = 0x108E60;  // 设置堆叠数（item, count）
static constexpr uintptr_t RVA_SAP_ITEM_IS_STACKABLE    = 0x1096C0;  // 查询物品可否堆叠（item）→ bool
static constexpr uintptr_t RVA_SAP_CHEST_PUSH           = 0x35E4E0;  // 入箱（GimmickProcessPushItem 内部）
static constexpr uintptr_t RVA_SAP_CHEST_COMMIT         = 0x35F1E0;  // 入箱提交
static constexpr uintptr_t RVA_SAP_SAVE_DATA_GLOBAL     = 0x10FCBB0; // 全局 save data 指针（与 RVA_GAME_ROOT 同）
static constexpr uintptr_t RVA_SAP_NATIVE_COLLECT       = 0x27E460;  // native_collect（游戏原生收集函数）
static constexpr uintptr_t SAP_ITEM_ALLOC_SIZE           = 0x2C0;    // CItemStatus 分配大小（反汇编 0x16614C 确认）
static constexpr uintptr_t SAP_ITEM_REF_COUNT_OFFSET    = 0x08;     // CItemStatus 引用计数（AddRef/Release 用）
static constexpr uintptr_t SAP_ITEM_RANK_OFFSET         = 0x280;    // CItemStatus rank 字段（反汇编 0x1664EC 确认）
static constexpr uintptr_t SAP_SAVE_DATA_OFFSET           = 0x208;    // game_root -> save data（ctor 第 2 参数）

// ---- AOB 签名表（从 ChestSort 复制关键签名）----
struct AOBDef { const char* name; const char* hex; };
static const AOBDef kAOBs[] = {
    {"spatial_search",       "40 57 41 54 41 55 41 56 41 57 48 83 EC 60"},
    {"raw_vector_free",      "48 83 EC 38 48 81 FA 00 10 00 00 72 14 48 8B"},
    {"intrusive_release",   "48 83 EC 28 48 8B 09 48 85 C9 74 21 8B 41"},
    {"item_adjust",         "48 89 5C 24 08 57 48 83 EC 20 48 8D B9 58 02"},
    {"player_capacity",     "48 8B 81 88 03 00 00 48 0F BA E0 16 73 06"},
    {"batch_dirty",         "4C 8B D2 48 63 C2 48 C1 F8 06 41 83 E2 3F 4C 8B C9 41 B8 01 00 00 00 41 8B CA 49 D3 E0 4D 23 84 C1 90 00 00 00 48 63 C2 48 C1 F8 06 49 8D 0C C1 48 8B 81 90 00 00 00 4C 0F B3 D0"},
};
enum AOBIndex {
    IDX_SPATIAL_SEARCH = 0,
    IDX_RAW_VECTOR_FREE,
    IDX_INTRUSIVE_RELEASE,
    IDX_ITEM_ADJUST,
    IDX_PLAYER_CAPACITY,
    IDX_BATCH_DIRTY,
};
static constexpr size_t kAOBCount = sizeof(kAOBs) / sizeof(kAOBs[0]);

// ---- 采集参数 ----
static constexpr float  HARVEST_RADIUS = 2000.0f;      // v1.8.9: 搜索半径 5000→2000（4 格世界网格，首搜候选减 80%+，已注册提取器不受影响）
static constexpr float  HARVEST_RADIUS_SQ = HARVEST_RADIUS * HARVEST_RADIUS;
static constexpr float  CHEST_SET_RADIUS = 95.0f;    // v1.9.2: F4 设箱搜索半径（一格范围，参照链式自动化格距；旧值 300 导致菜单/远处误触）
static constexpr float  CHEST_SET_RADIUS_SQ = CHEST_SET_RADIUS * CHEST_SET_RADIUS;
static constexpr size_t MAX_SEARCH_RESULTS = 512;     // 搜索半径缩小后候选量大减
static constexpr size_t MAX_SEARCH_CAPACITY = 16384;
static constexpr size_t MAX_CHESTS = 32;
static constexpr size_t MAX_EXTRACTORS = 64;
static constexpr size_t CHEST_SLOT_COUNT = 30;
static constexpr size_t EXTRACTORS_PER_CYCLE = 4;        // 每周期最多处理4个提取器

// ---- Action ID ----

// ============================================================
// 游戏函数指针类型
// ============================================================
struct Rect { float minX, minY, maxX, maxY; };
struct RawPointerVector { void** begin; void** end; void** capacity; };

struct SearchCallback {
    alignas(16) unsigned char storage[0x38];
    void* target;
};
static_assert(sizeof(SearchCallback) == 0x40, "callback size");

using FnSpatialSearch    = bool(__fastcall*)(void*, const Rect*, void*, int, int);
using FnRawVectorFree    = void(__fastcall*)(void*, size_t);
using FnIntrusiveRelease = void(__fastcall*)(void**);
using FnItemAdjust       = void(__fastcall*)(void*, int);
using FnPlayerCapacity   = int(__fastcall*)(void*);
using FnBatchRecalc      = void(__fastcall*)(void*, bool);
using FnBatchUIRefresh   = void(__fastcall*)(void*, bool);
using FnBatchDirty       = void(__fastcall*)(void*, int);


struct CommandItem {
    void* item;
    unsigned char enabled;
    unsigned char padding09[3];
    int field0c;
    int state;
    unsigned char dirty;
    unsigned char padding15[3];
};
static_assert(sizeof(CommandItem) == 0x18, "native CommandItem layout");

// ---- 生产机函数类型（从 ProductionAuto 移植）----
using FnSapOutputHelper = void*(__fastcall*)(void**, void*, int);
//     GimmickProcessPopItem 内部完成辅助：
//     rcx=out(CItemStatus**), rdx=machineStatus, r8d=slot
//     物化产出物品 → 清空槽位(0x288/0x2a0)与 0x2b8 backing 条目 → 返回带 1 个 owned ref 的 CItemStatus*
using FnSapStatusMapLookup = struct SapMapLookupResult*(__fastcall*)(void*, struct SapMapLookupResult*, const u64*);
//     status map 查找（find-or-insert），返回节点
using FnSapStatusEventNotify = void(__fastcall*)(void*);
using FnSapItemAllocate     = void*(__fastcall*)(size_t);
using FnSapItemCtor         = void*(__fastcall*)(void*, void*, u64);
using FnSapItemSetCount     = void(__fastcall*)(void*, int);
using FnSapItemIsStackable  = bool(__fastcall*)(void*);
struct SapMapLookupResult {
    void* node;
    unsigned char inserted;
    unsigned char padding[7];
};
using FnCommandCapacity = int(__fastcall*)(CommandItem*);
// native_collect（游戏原生收集函数）：rcx=baseID(u64), rdx=machineStatus(void*), r8/r9=未用
// 反汇编确认：内部经 StatusSearchByBaseID(0x250690) 按第一参数(baseID)搜索 g_gimmickMgr 映射表
// 0x250690 内部格式串 '%s invalid baseID %d' 证实 key 为整数
// 第二参数 status 的 +0x58 map 被查 'UIPrsF' 键、+0x2b8 backing 数组被操作
using FnSapNativeCollect = void*(__fastcall*)(u64, void*, void*, void*);

// ============================================================
// 全局状态
// ============================================================
namespace G {
    uintptr_t base = 0;
    bool ready = false;

    // AOB 扫描结果
    uintptr_t rvaGameRoot = 0;
    uintptr_t rvaWorldRegistry = 0;
    uintptr_t rvaCallbackVTable = 0;

    // 函数指针
    FnSpatialSearch    spatialSearch    = nullptr;
    FnRawVectorFree    rawVectorFree    = nullptr;
    FnIntrusiveRelease intrusiveRelease = nullptr;
    FnItemAdjust       itemAdjust       = nullptr;
    FnPlayerCapacity   playerCapacity   = nullptr;
    FnBatchRecalc      batchRecalc      = nullptr;
    FnBatchUIRefresh   batchUIRefresh   = nullptr;
    FnBatchDirty       batchDirty       = nullptr;
    FnCommandCapacity  commandCapacity  = nullptr;

    // 生产机函数指针（树液提取器收集用，RVA 绑定）
    FnSapOutputHelper       sapOutputHelper       = nullptr;
    FnSapStatusMapLookup    sapStatusIntLookup    = nullptr;
    FnSapStatusMapLookup    sapStatusU64Lookup    = nullptr;
    FnSapStatusMapLookup    sapStatusEventLookup  = nullptr;
    FnSapStatusEventNotify  sapStatusEventNotify  = nullptr;
    FnSapItemAllocate       sapItemAllocate       = nullptr;
    FnSapItemCtor           sapItemCtor           = nullptr;
    FnSapItemSetCount       sapItemSetCount       = nullptr;
    FnSapNativeCollect      sapNativeCollect      = nullptr;   // 游戏原生收集函数（诊断用）
    FnSapItemIsStackable    sapItemIsStackable    = nullptr;
    bool sapFunctionsReady  = false;


    // 状态
    void* storageChest = nullptr;            // 存储箱子 status 指针
    float storageChestX = 0.0f, storageChestY = 0.0f;
    bool chestSet = false;

    // ---- 行为状态机（F4 设箱即自动开始 / 按 8 手动暂停 / 收完自动停 / 跨日自动恢复）----
    enum class RunMode : int {
        Idle,        // 未设置箱子 / 手动暂停后重设前的空闲态
        AutoRun,     // F4 设箱后自动运行（收集树液）
        Paused,      // 按 8 手动暂停（跨日也不自动恢复）
        Done,        // 已收集完全部树液，当天不再运行（跨日自动恢复）
    };
    RunMode runMode = RunMode::Idle;

    // 跨日判定：游戏 raw second（CSaveData.raw_second，路径 game_root->+0x208->+0x3270）
    std::int64_t lastRunDay = -1;            // 上一次运行周期所在游戏日（-1=尚未运行）
    bool pendingDayReset = false;          // 标记跨日，等待安全时机重置轮转/恢复

    // 采集周期控制
    DWORD lastHarvestTick = 0;
    static constexpr DWORD HARVEST_INTERVAL_MS = 5000;  // 每 5 秒一个周期（背包转移+采集）
    static constexpr DWORD HARVEST_BACKSTOP_MS = 30000;  // v1.8.8: 增量触发保底 30 秒（树液生成慢，无需频繁全量搜索）
    size_t extractorRotateOffset = 0;
    size_t lastExtractorCount = (size_t)-1;
    DWORD lastFullScanTick = 0;

}

static QolHotKeys g_hotkeys;  // 运行时热键

// ============================================================
// 内存安全工具（与 ChestSort 一致）
// ============================================================
struct MemRegion { uintptr_t start; uintptr_t end; };
static MemRegion g_fastRegions[16];
static int g_fastRegionCount = 0;

static void FastRegionReset() { g_fastRegionCount = 0; }

static bool IsReadable(const void* p, size_t sz) {
    if (!p || sz == 0) return false;
    uintptr_t start = (uintptr_t)p;
    if (start < 0x10000 || start > 0x00007FFFFFFFFFFFULL) return false;
    for (int i = 0; i < g_fastRegionCount; ++i) {
        if (start >= g_fastRegions[i].start && start + sz <= g_fastRegions[i].end) return true;
    }
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    if (start + sz > regionEnd || start + sz < start) return false;
    if (g_fastRegionCount < 16) {
        g_fastRegions[g_fastRegionCount] = { (uintptr_t)mbi.BaseAddress, regionEnd };
        g_fastRegionCount++;
    }
    return true;
}

static bool ReadPtr(const void* obj, uintptr_t off, void** out) {
    if (!obj || !out) return false;
    const void* field = (const void*)((uintptr_t)obj + off);
    if (!IsReadable(field, sizeof(void*))) return false;
    *out = *(void* const*)field;
    return *out != nullptr;
}

// ============================================================
// 内存写入工具（与 AutoFish 一致）
// ============================================================
static bool WriteMem(void* target, const void* data, size_t size) {
    return qol::WritePatchChecked(target, data, size);
}

static bool SealExecutableMemory(void* address, size_t size) {
    DWORD previousProtection = 0;
    if (!address || size == 0) return false;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READ, &previousProtection)) return false;
    return FlushInstructionCache(GetCurrentProcess(), address, size) != 0;
}

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
    const uintptr_t maximum = maximumByRange < maximumSystem ? maximumByRange : maximumSystem;

    uintptr_t address = minimum & ~(granularity - 1);
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION region = {};
        if (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) == 0) break;
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(region.BaseAddress);
        const uintptr_t regionEnd = regionBase + region.RegionSize;
        if (region.State == MEM_FREE) {
            const uintptr_t candidate = (regionBase + granularity - 1) & ~(granularity - 1);
            if (candidate >= minimum && candidate + size <= regionEnd && candidate + size <= maximum) {
                void* allocated = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                                               MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (allocated) {
                    const std::int64_t displacement =
                        static_cast<std::int64_t>(reinterpret_cast<uintptr_t>(allocated)) -
                        static_cast<std::int64_t>(target + 5);
                    if (displacement >= INT32_MIN && displacement <= INT32_MAX) return allocated;
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
// 物品读取（与 ChestSort 一致）
// ============================================================
struct ItemInfo { u64 itemId; int stackCount; int rank; };

static bool ReadItem(void* item, ItemInfo* out) {
    if (!item || !out) return false;
    if (!IsReadable(item, ITEM_RANK_OFFSET + sizeof(int))) return false;
    LONG refs = *(volatile LONG*)((uintptr_t)item + sizeof(void*));
    if (refs <= 0) return false;
    void* holder = *(void**)((uintptr_t)item + ITEM_DATA_HOLDER_OFFSET);
    void* data = nullptr;
    if (!holder || !ReadPtr(holder, 0, &data) || !IsReadable(data, sizeof(u64))) return false;
    out->itemId = *(const u64*)data;
    out->stackCount = *(const int*)((uintptr_t)item + ITEM_STACK_COUNT_OFFSET);
    out->rank = *(const int*)((uintptr_t)item + ITEM_RANK_OFFSET);
    return out->itemId != 0 && out->stackCount > 0 && out->stackCount <= 999;
}

static bool ReadStackCount(void* item, int* out) {
    if (!item || !out) return false;
    if (!IsReadable((void*)((uintptr_t)item + ITEM_STACK_COUNT_OFFSET), sizeof(int))) return false;
    int c = *(const int*)((uintptr_t)item + ITEM_STACK_COUNT_OFFSET);
    if (c < 0 || c > 999) return false;
    *out = c;
    return true;
}

static bool ReadRawInventory(void* status, RawPointerVector* out) {
    if (!status || !out) return false;
    if (!IsReadable((void*)((uintptr_t)status + STATUS_ITEMS_OFFSET), sizeof(RawPointerVector))) return false;
    memcpy(out, (const void*)((uintptr_t)status + STATUS_ITEMS_OFFSET), sizeof(RawPointerVector));
    const uintptr_t begin = (uintptr_t)out->begin;
    const uintptr_t end = (uintptr_t)out->end;
    const uintptr_t capacity = (uintptr_t)out->capacity;
    if (!begin || end < begin || capacity < end ||
        (end - begin) % sizeof(void*) != 0 ||
        (capacity - begin) % sizeof(void*) != 0) return false;
    const size_t count = (end - begin) / sizeof(void*);
    const size_t capacityCount = (capacity - begin) / sizeof(void*);
    return count > 0 && count <= 64 && capacityCount >= count && capacityCount <= 64 &&
           IsReadable(out->begin, count * sizeof(void*));
}

// ============================================================
// 世界上下文（与 ChestSort 一致）
// ============================================================
struct WorldContext {
    void* player;
    float position[4];
    void* spatialIndex;
};

static bool GetWorldContext(WorldContext* ctx) {
    if (!ctx) return false;
    if (!G::rvaGameRoot || !G::rvaWorldRegistry) return false;
    memset(ctx, 0, sizeof(*ctx));

    void** rootSlot = (void**)(G::base + G::rvaGameRoot);
    if (!IsReadable(rootSlot, sizeof(void*))) return false;
    void* root = *rootSlot;
    if (!root) return false;

    void* player = nullptr;
    if (!ReadPtr(root, ROOT_PLAYER_OFFSET, &player)) return false;

    void* objectStatus = nullptr;
    if (!ReadPtr(player, PLAYER_OBJECT_STATUS_OFFSET, &objectStatus)) return false;

    const float* pos = (const float*)((uintptr_t)objectStatus + GIMMICK_POSITION_OFFSET);
    if (!IsReadable(pos, sizeof(float) * 4)) return false;
    memcpy(ctx->position, pos, sizeof(float) * 4);
    if (!std::isfinite(ctx->position[0]) || !std::isfinite(ctx->position[1])) return false;
    if (fabsf(ctx->position[0]) < 1.0f && fabsf(ctx->position[1]) < 1.0f) return false;

    void* mapOwner = nullptr;
    void* mapInfo = nullptr;
    void* spatialOwner = nullptr;
    if (!ReadPtr(root, ROOT_MAP_OWNER_OFFSET, &mapOwner)) return false;
    if (!ReadPtr(mapOwner, MAP_INFO_OFFSET, &mapInfo)) return false;
    if (!ReadPtr(mapInfo, MAP_SPATIAL_OWNER_OFFSET, &spatialOwner)) return false;

    void* spatialIndex = (void*)((uintptr_t)spatialOwner + MAP_SPATIAL_INDEX_OFFSET);
    if (!IsReadable(spatialIndex, 0x48)) return false;

    ctx->player = player;
    ctx->spatialIndex = spatialIndex;
    return true;
}

// ============================================================
// 空间搜索（与 ChestSort 一致）
// ============================================================
static void ReleaseSearchVector(RawPointerVector* v) {
    if (!v || !v->begin || !G::rawVectorFree) return;
    uintptr_t b = (uintptr_t)v->begin, c = (uintptr_t)v->capacity;
    size_t allocBytes = c >= b ? c - b : 0;
    if (allocBytes % sizeof(void*) == 0 && allocBytes <= MAX_SEARCH_CAPACITY * sizeof(void*)) {
        G::rawVectorFree(v->begin, allocBytes);
    }
    *v = {};
}

static bool DoSpatialSearch(void* spatialIndex, const Rect* bounds, uint64_t filterId,
                            RawPointerVector* results) {
    if (!spatialIndex || !bounds || !results) return false;
    memset(results, 0, sizeof(*results));
    if (!G::spatialSearch || !G::rvaCallbackVTable) return false;

    SearchCallback callback = {};
    u64 localFilterId = filterId;
    *reinterpret_cast<void**>(callback.storage + 0x00) =
        reinterpret_cast<void*>(G::base + G::rvaCallbackVTable);
    *reinterpret_cast<u64**>(callback.storage + 0x08) = &localFilterId;
    *reinterpret_cast<RawPointerVector**>(callback.storage + 0x10) = results;
    callback.target = callback.storage;

    __try {
        G::spatialSearch(spatialIndex, bounds, &callback, -1, -1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[AutoHarvest] [search] spatialSearch 调用异常 (SEH)");
        return false;
    }

    if (callback.target) {
        __try {
            void** vtable = *reinterpret_cast<void***>(callback.target);
            using DestroyFunction = void(__fastcall*)(void*, bool);
            DestroyFunction destroy = reinterpret_cast<DestroyFunction>(vtable[4]);
            destroy(callback.target, callback.target != callback.storage);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[AutoHarvest] [search] destroy 调用异常 (SEH)");
        }
        callback.target = nullptr;
    }

    if (!results->begin || !results->end || results->end < results->begin ||
        ((uintptr_t)results->end - (uintptr_t)results->begin) % sizeof(void*) != 0) return false;
    return true;
}

// ============================================================
// Gimmick 类型判断（来自 ChestSort 的 IsChestStatus）
// ============================================================

// 判断 status 是否为箱子
static bool IsChestStatus(void* status) {
    if (!status) return false;
    void* holder = nullptr;
    if (!ReadPtr(status, GIMMICK_DATA_HOLDER_OFFSET, &holder)) return false;
    void* data = nullptr;
    if (!ReadPtr(holder, 0, &data)) return false;
    void* moduleNamePtr = nullptr;
    if (!ReadPtr(data, GIMMICK_MODULE_NAME_OFFSET, &moduleNamePtr)) return false;
    if (!IsReadable(moduleNamePtr, 64)) return false;
    char buf[64];
    size_t len = 0;
    while (len + 1 < sizeof(buf) && ((const char*)moduleNamePtr)[len] != '\0') ++len;
    if (((const char*)moduleNamePtr)[len] != '\0') return false;
    memcpy(buf, moduleNamePtr, len + 1);
    return strcmp(buf, "gimmick_chest") == 0;
}


// 前向声明：游戏原始秒数读取（定义在文件后部）
static bool ReadGameRawSecond(std::int64_t* outSec);

// ============================================================
// 诊断：dump 树液提取器 status 内全部疑似 map 的键（定位 itemVal 存储位置）
// ============================================================
static void SapDumpExtractorMaps(void* status) {
    if (!status) return;
    Log("[AutoHarvest] [sap-dbg] ===== dump maps status=%p =====", status);
    // 0x00~0x200 逐 0x8 对齐扫描疑似 map：sentinel=*(p+8) 可读 && size<=4096 && mask 为 2^N-1
    for (uintptr_t off = 0; off <= 0x1F8; off += 8) {
        void* p = (unsigned char*)status + off;
        if (!IsReadable(p, 0x38)) continue;
        void* sentinel = *(void**)((unsigned char*)p + 0x08);
        const u64 size = *(const u64*)((unsigned char*)p + 0x10);
        void* buckets = *(void**)((unsigned char*)p + 0x18);
        const u64 mask = *(const u64*)((unsigned char*)p + 0x30);
        if (!sentinel || !IsReadable(sentinel, 0x20)) continue;
        if (size > 4096 || mask > 4095 || ((mask + 1) & mask) != 0) continue;
        if (!buckets || !IsReadable(buckets, (size_t)(mask + 1) * 16)) continue;
        Log("[AutoHarvest] [sap-dbg] map @ +0x%03llX sentinel=%p size=%llu buckets=%p mask=0x%llX",
            (unsigned long long)off, sentinel, (unsigned long long)size, buckets,
            (unsigned long long)mask);
        // 遍历前 16 个非空节点，dump 键（key=node+0x10 的 8 字节）
        size_t visited = 0;
        for (u64 bi = 0; bi <= mask && visited < 16; ++bi) {
            void** pair = (void**)((unsigned char*)buckets + bi * 16);
            void* stop = pair[0];
            void* node = pair[1];
            size_t chain = 0;
            while (node && node != sentinel && IsReadable(node, 0x20) && chain < 32) {
                u64 key = *(const u64*)((unsigned char*)node + 0x10);
                char kb[9] = {};
                memcpy(kb, &key, 8);
                bool ascii = true;
                for (int c = 0; c < 8; ++c) if (kb[c] != 0 && (kb[c] < 32 || kb[c] > 126)) { ascii = false; break; }
                u64 val = *(const u64*)((unsigned char*)node + 0x18);
                if (ascii) {
                    Log("[AutoHarvest] [sap-dbg]   +0x%03llX bucket=%llu key=\"%s\" val=%llu",
                        (unsigned long long)off, (unsigned long long)bi, kb, (unsigned long long)val);
                } else {
                    Log("[AutoHarvest] [sap-dbg]   +0x%03llX bucket=%llu key=0x%llX val=%llu",
                        (unsigned long long)off, (unsigned long long)bi,
                        (unsigned long long)key, (unsigned long long)val);
                }
                ++visited;
                if (node == stop) break;
                node = *(void**)((unsigned char*)node + sizeof(void*));
                ++chain;
            }
        }
    }
    // ---- 扩展：dump 0x2b8 物品数组指针 + data 对象内部的 map ----
    // 0x2b8 是固定 3x8 backing 数组指针
    if (IsReadable((void*)((uintptr_t)status + SAP_STATUS_ITEM_LIST_OFFSET), sizeof(void*))) {
        void** items = *(void***)((unsigned char*)status + SAP_STATUS_ITEM_LIST_OFFSET);
        Log("[AutoHarvest] [sap-dbg] 0x2b8 items=%p", items);
        if (items && IsReadable(items, sizeof(void*) * SAP_SLOT_INGREDIENTS)) {
            for (size_t si = 0; si < SAP_SLOT_INGREDIENTS; ++si) {
                void* it = items[si];
                if (!it) continue;
                Log("[AutoHarvest] [sap-dbg]   items[%zu]=%p", si, it);
                if (IsReadable(it, ITEM_RANK_OFFSET + sizeof(int))) {
                    Log("[AutoHarvest] [sap-dbg]     itemId=%llu rank=%d count=%d",
                        (unsigned long long)(*(const u64*)it), *(const int*)((unsigned char*)it + ITEM_RANK_OFFSET),
                        *(const int*)((unsigned char*)it + ITEM_STACK_COUNT_OFFSET));
                }
            }
        }
    }
    // 时间戳 dump (0x288 start + 0x2a0 duration, 3 slots)
    if (IsReadable((void*)((uintptr_t)status + SAP_STATUS_TIME_VALUE_OFFSET), 3 * sizeof(u64)) &&
        IsReadable((void*)((uintptr_t)status + SAP_STATUS_CREATE_TIME_OFFSET), 3 * sizeof(u64))) {
        for (int si = 0; si < 3; ++si) {
            u64 st = *(const u64*)((unsigned char*)status + SAP_STATUS_TIME_VALUE_OFFSET + si * sizeof(u64));
            u64 du = *(const u64*)((unsigned char*)status + SAP_STATUS_CREATE_TIME_OFFSET + si * sizeof(u64));
            Log("[AutoHarvest] [sap-dbg] ts slot%d start=%llu dur=%llu end=%llu",
                si, (unsigned long long)st, (unsigned long long)du, (unsigned long long)(st + du));
        }
    }
    { std::int64_t rs = 0; if (ReadGameRawSecond(&rs)) {
        Log("[AutoHarvest] [sap-dbg] gameNow rawSec=%lld day=%lld", (long long)rs, (long long)(rs / 86400));
    }}

    // 读 data 对象（status+0x240 -> +0）内部 0x00~0x100 的 map
    void* holder = nullptr;
    if (ReadPtr(status, GIMMICK_DATA_HOLDER_OFFSET, &holder)) {
        void* data = nullptr;
        if (ReadPtr(holder, 0, &data) && data) {
            Log("[AutoHarvest] [sap-dbg] data=%p", data);
            for (uintptr_t off = 0; off <= 0xF8; off += 8) {
                void* p = (unsigned char*)data + off;
                if (!IsReadable(p, 0x38)) continue;
                void* sentinel = *(void**)((unsigned char*)p + 0x08);
                const u64 size = *(const u64*)((unsigned char*)p + 0x10);
                void* buckets = *(void**)((unsigned char*)p + 0x18);
                const u64 mask = *(const u64*)((unsigned char*)p + 0x30);
                if (!sentinel || !IsReadable(sentinel, 0x20)) continue;
                if (size > 4096 || mask > 4095 || ((mask + 1) & mask) != 0) continue;
                if (!buckets || !IsReadable(buckets, (size_t)(mask + 1) * 16)) continue;
                Log("[AutoHarvest] [sap-dbg] data map @ +0x%03llX sentinel=%p size=%llu buckets=%p mask=0x%llX",
                    (unsigned long long)off, sentinel, (unsigned long long)size, buckets,
                    (unsigned long long)mask);
                size_t visited = 0;
                for (u64 bi = 0; bi <= mask && visited < 16; ++bi) {
                    void** pair = (void**)((unsigned char*)buckets + bi * 16);
                    void* stop = pair[0];
                    void* node = pair[1];
                    size_t chain = 0;
                    while (node && node != sentinel && IsReadable(node, 0x20) && chain < 32) {
                        u64 key = *(const u64*)((unsigned char*)node + 0x10);
                        char kb[9] = {};
                        memcpy(kb, &key, 8);
                        bool ascii = true;
                        for (int c = 0; c < 8; ++c) if (kb[c] != 0 && (kb[c] < 32 || kb[c] > 126)) { ascii = false; break; }
                        u64 val = *(const u64*)((unsigned char*)node + 0x18);
                        if (ascii) {
                            Log("[AutoHarvest] [sap-dbg]   data+0x%03llX bucket=%llu key=\"%s\" val=%llu",
                                (unsigned long long)off, (unsigned long long)bi, kb, (unsigned long long)val);
                        } else {
                            Log("[AutoHarvest] [sap-dbg]   data+0x%03llX bucket=%llu key=0x%llX val=%llu",
                                (unsigned long long)off, (unsigned long long)bi,
                                (unsigned long long)key, (unsigned long long)val);
                        }
                        ++visited;
                        if (node == stop) break;
                        node = *(void**)((unsigned char*)node + sizeof(void*));
                        ++chain;
                    }
                }
            }
        }
    }
    Log("[AutoHarvest] [sap-dbg] ===== dump end =====");
}

// 判断 status 是否为树液提取器
static bool IsSapExtractorStatus(void* status) {
    if (!status) return false;
    void* holder = nullptr;
    if (!ReadPtr(status, GIMMICK_DATA_HOLDER_OFFSET, &holder)) return false;
    void* data = nullptr;
    if (!ReadPtr(holder, 0, &data)) return false;
    void* moduleNamePtr = nullptr;
    if (!ReadPtr(data, GIMMICK_MODULE_NAME_OFFSET, &moduleNamePtr)) return false;
    if (!IsReadable(moduleNamePtr, 64)) return false;
    char buf[64];
    size_t len = 0;
    while (len + 1 < sizeof(buf) && ((const char*)moduleNamePtr)[len] != '\0') ++len;
    if (((const char*)moduleNamePtr)[len] != '\0') return false;
    memcpy(buf, moduleNamePtr, len + 1);
    bool match = (strcmp(buf, "gimmick_sap_extractor") == 0);
    if (!match) {
        static int s_logCnt = 0;
#if DIAG_AUTOHARVEST
        if (s_logCnt < 5) { s_logCnt++; Log("[AutoHarvest] [diag-sap] name='%s' match=0", buf); }
#endif
    }
    return match;
}

// 读取 status 的坐标
static bool ReadPosition(void* status, float* x, float* y) {
    const float* pos = (const float*)((uintptr_t)status + GIMMICK_POSITION_OFFSET);
    if (!IsReadable(pos, sizeof(float) * 4)) return false;
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1])) return false;
    *x = pos[0];
    *y = pos[1];
    return true;
}

// ============================================================
// 搜索附近箱子
// ============================================================
struct ChestCandidate {
    void* status;
    float posX, posY;
    float distSq;
};

static size_t SearchChests(void* spatialIndex, const float* playerPos,
                           ChestCandidate* candidates, size_t maxCount, float radiusSq) {
    if (!spatialIndex || !playerPos || !candidates) return 0;
    if (!G::rvaCallbackVTable) return 0;

    float radius = sqrtf(radiusSq);
    Rect bounds = {
        playerPos[0] - radius, playerPos[1] - radius,
        playerPos[0] + radius, playerPos[1] + radius,
    };

    RawPointerVector results = {};
    bool ok = DoSpatialSearch(spatialIndex, &bounds, 0, &results);
    if (!ok || !results.begin || !results.end) {
        ReleaseSearchVector(&results);
        return 0;
    }
    size_t rawCount = results.end - results.begin;

    size_t count = 0;
    for (size_t i = 0; i < rawCount && count < maxCount; ++i) {
        void* status = results.begin[i];
        if (!status) continue;
        if (!IsChestStatus(status)) continue;
        float px, py;
        if (!ReadPosition(status, &px, &py)) continue;
        float dx = px - playerPos[0];
        float dy = py - playerPos[1];
        float distSq = dx * dx + dy * dy;
        if (distSq > radiusSq) continue;
        candidates[count] = { status, px, py, distSq };
        ++count;
    }

    // 按距离排序
    for (size_t i = 1; i < count; ++i) {
        ChestCandidate key = candidates[i];
        size_t j = i;
        while (j > 0 && candidates[j - 1].distSq > key.distSq) {
            candidates[j] = candidates[j - 1];
            --j;
        }
        candidates[j] = key;
    }

    ReleaseSearchVector(&results);
    return count;
}

// ============================================================
// 搜索附近树液提取器
// ============================================================
struct SapExtractorCandidate {
    void* status;
    float posX, posY;
    float distSq;
};

// v1.8.7: 持久化提取器缓存——保底路径复用此列表跳过搜索但仍采集
static SapExtractorCandidate g_cachedExtractors[64] = {};
static size_t g_cachedExtractorCount = 0;

// v1.8.9: 分块搜索状态机——把 2000 半径矩形拆成 SHARD_GRID×SHARD_GRID 小块，
// 每帧只做一个小块的空间搜索（单帧 DoSpatialSearch 开销恒定，彻底消除启动卡顿）
static constexpr int    SHARD_GRID = 4;                     // 4×4 = 16 小块
static constexpr int    SHARD_GRID_TOTAL = SHARD_GRID * SHARD_GRID;
static constexpr DWORD  SHARD_SCAN_INTERVAL_MS = 30000;     // 成功搜索：30 秒后重搜
static constexpr DWORD  SHARD_RETRY_INTERVAL_MS = 60000;    // v1.9.0: 空/失败：60 秒后重试（无采集物区域降耗，玩家移动会立即重搜）
static constexpr DWORD  SHARD_MOVE_RETRY_MIN_MS = 2000;   // v1.9.5: 移动触发重搜最小间隔（原每 0.5s 移动即全城 16 块重搜 ~5.7ms/帧，实测持续卡顿）
struct ShardSearchState {
    bool active = false;            // 一轮分块搜索进行中
    int gridIndex = 0;              // 当前块索引（0..SHARD_GRID_TOTAL-1）
    float originX = 0, originY = 0; // 本轮搜索区域左下角（玩家为中心 ±HARVEST_RADIUS）
    RawPointerVector results = {};  // 当前块的搜索结果（本帧用完即释放）
    SapExtractorCandidate found[64]; // 本轮内累计找到的提取器
    size_t foundCount = 0;
    DWORD lastScanTick = 0;         // 上次开始一轮搜索的时间
    DWORD firstSearchDeferUntil = 0; // v1.8.9: 首轮延迟截止（避开 F4 按下帧）
    float lastScanX = 0, lastScanY = 0; // v1.9.0: 上次搜索时玩家位置（移动超阈值立即重搜）
    bool lastResultEmpty = true;    // 上一轮是否空/失败（决定 10s vs 30s 重搜间隔）
};
static ShardSearchState g_shardSearch;

// v1.8.6: 搜索结果过滤加帧内预算——超过 2ms 自动截断（候选量受缩半径已大减，此处是安全网）
static size_t SearchSapExtractors(void* spatialIndex, const float* playerPos,
                                  SapExtractorCandidate* candidates, size_t maxCount) {
    if (!spatialIndex || !playerPos || !candidates) return 0;
    if (!G::rvaCallbackVTable) return 0;

    Rect bounds = {
        playerPos[0] - HARVEST_RADIUS, playerPos[1] - HARVEST_RADIUS,
        playerPos[0] + HARVEST_RADIUS, playerPos[1] + HARVEST_RADIUS,
    };

    RawPointerVector results = {};
    bool ok = DoSpatialSearch(spatialIndex, &bounds, 0, &results);
    if (!ok || !results.begin || !results.end) {
        ReleaseSearchVector(&results);
        return 0;
    }
    size_t rawCount = results.end - results.begin;

    // v1.8.6: 帧内预算——过滤循环超过 2ms 自动截断
    LARGE_INTEGER budgetStart, budgetFreq;
    QueryPerformanceCounter(&budgetStart);
    QueryPerformanceFrequency(&budgetFreq);
    const LONGLONG budgetLimit = budgetFreq.QuadPart / 500;  // 2ms = 1/500 秒

    size_t count = 0;
    for (size_t i = 0; i < rawCount && count < maxCount; ++i) {
        // v1.8.6: 每 64 个候选检查一次预算
        if ((i & 63) == 63) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (now.QuadPart - budgetStart.QuadPart > budgetLimit) {
                Log("[AutoHarvest] 搜索过滤超 2ms 预算，截断于 %zu/%zu", i, rawCount);
                break;
            }
        }
        void* status = results.begin[i];
        if (!status) continue;
        if (!IsSapExtractorStatus(status)) continue;
        float px, py;
        if (!ReadPosition(status, &px, &py)) continue;
        float dx = px - playerPos[0];
        float dy = py - playerPos[1];
        float distSq = dx * dx + dy * dy;
        if (distSq > HARVEST_RADIUS_SQ) continue;
        candidates[count] = { status, px, py, distSq };
        ++count;
    }

    ReleaseSearchVector(&results);
    return count;
}



// v1.9.6: SEH 包装的 itemAdjust 调用——防止原生函数异常导致崩溃（H8+D1）
// 返回 false 表示调用异常（SEH 捕获），调用方应执行回滚或中止
__declspec(noinline) static bool SafeItemAdjust(void* item, int delta) {
    if (!item || !G::itemAdjust) return false;
    __try {
        G::itemAdjust(item, delta);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[AutoHarvest] [SEH] itemAdjust 异常 item=%p delta=%d", item, delta);
        return false;
    }
}

// ============================================================
// 背包→存储箱子转移（来自 ChestSort DoSort 的核心逻辑）
// ============================================================
static size_t TransferToChest(void* player, void* chestStatus) {
    if (!player || !chestStatus) return 0;

    // 读取玩家背包容量
    int capacity = -1;
    if (G::playerCapacity) {
        __try { capacity = G::playerCapacity(player); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    if (capacity != 10 && capacity != 20 && capacity != 30) return 0;

    // 读取背包物品列表
    void** slots = reinterpret_cast<void**>((uintptr_t)player + PLAYER_ITEMS_OFFSET);
    if (!IsReadable(slots, (size_t)capacity * sizeof(void*))) return 0;

    // 读取存储箱子物品列表
    RawPointerVector chestInv = {};
    if (!ReadRawInventory(chestStatus, &chestInv)) return 0;
    size_t chestSlots = (chestInv.end - chestInv.begin);

    size_t movedStacks = 0;
    size_t movedItems = 0;

    for (int i = 0; i < capacity; ++i) {
        void* item = slots[i];
        if (!item) continue;
        ItemInfo info = {};
        if (!ReadItem(item, &info)) continue;

        // 在存储箱子中找同类堆
        void* targetItem = nullptr;
        for (size_t s = 0; s < chestSlots; ++s) {
            void* ci = chestInv.begin[s];
            if (!ci) continue;
            ItemInfo ciInfo = {};
            if (!ReadItem(ci, &ciInfo)) continue;
            if (ciInfo.itemId == info.itemId && ciInfo.rank == info.rank) {
                targetItem = ci;
                break;
            }
        }
        // 找不到同类堆时跳过——与 ChestSort 一致，不尝试放入空槽位
        // （itemAdjust 只能合并到已有堆，不能创建新堆）
        if (!targetItem) continue;

        // 转移前持有引用保护
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(
            reinterpret_cast<uintptr_t>(item) + sizeof(void*)));

        if (!G::itemAdjust) goto skip_item;
        const int moveCount = info.stackCount;
        int targetBefore = 0;
        if (!ReadStackCount(targetItem, &targetBefore)) goto skip_item;

        // 容量校验
        if (G::commandCapacity) {
            CommandItem descriptor = {};
            descriptor.item = targetItem;
            const int remainingCap = G::commandCapacity(&descriptor);
            if (remainingCap < moveCount) goto skip_item;
        }

        // v1.9.6 H8+D1: itemAdjust 调用全部改用 SafeItemAdjust（SEH 保护）
        if (!SafeItemAdjust(targetItem, moveCount)) goto skip_item;
        int targetAfter = 0;
        if (!ReadStackCount(targetItem, &targetAfter) || targetAfter != targetBefore + moveCount) {
            if (!SafeItemAdjust(targetItem, -moveCount))
                Log("[AutoHarvest] [SEH] 回滚 targetItem 失败! itemId=%llu", info.itemId);
            goto skip_item;
        }
        if (!SafeItemAdjust(item, -moveCount)) {
            if (!SafeItemAdjust(targetItem, -moveCount))
                Log("[AutoHarvest] [SEH] 回滚 targetItem 失败! itemId=%llu", info.itemId);
            goto skip_item;
        }
        int sourceAfter = 0;
        if (!ReadStackCount(item, &sourceAfter) || sourceAfter != 0) {
            if (!SafeItemAdjust(targetItem, -moveCount))
                Log("[AutoHarvest] [SEH] 回滚 targetItem 失败! itemId=%llu", info.itemId);
            if (!SafeItemAdjust(item, moveCount))
                Log("[AutoHarvest] [SEH] 恢复 sourceItem 失败! itemId=%llu", info.itemId);
            goto skip_item;
        }

        // v1.8.7: 先释放保护性 AddRef，再 intrusiveRelease 背包引用（释放顺序统一）
        // 清空背包槽位指针并释放引用
        {
            void* observed = InterlockedCompareExchangePointer(
                reinterpret_cast<void* volatile*>(&slots[i]), nullptr, item);
            if (observed == item) {
                InterlockedDecrement(reinterpret_cast<volatile LONG*>(
                    reinterpret_cast<uintptr_t>(item) + sizeof(void*)));  // 释放保护性 AddRef
                void* slotOwnedRef = item;
                if (G::intrusiveRelease) G::intrusiveRelease(&slotOwnedRef);  // 释放背包引用
            }
        }

        movedStacks++;
        movedItems += moveCount;
        Log("[AutoHarvest] [transfer] itemId=%llu rank=%d x%d", info.itemId, info.rank, moveCount);
        continue;

    skip_item:
        // v1.8.7: 所有失败路径统一在此释放保护性 AddRef
        InterlockedDecrement(reinterpret_cast<volatile LONG*>(
            reinterpret_cast<uintptr_t>(item) + sizeof(void*)));
    }

    // 收尾三件套
    if (movedStacks > 0) {
        if (G::batchRecalc) {
            __try { G::batchRecalc(player, false); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (G::batchUIRefresh) {
            __try { G::batchUIRefresh(player, true); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (G::batchDirty) {
            __try { G::batchDirty(player, 0x2bc); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        Log("[AutoHarvest] [transfer] 转移 %d 堆, %d 个物品到存储箱", (int)movedStacks, (int)movedItems);
    }

    return movedStacks;
}

// ============================================================
// 树液提取器收集（从 ProductionAuto 移植，build 24969282 适配）
// ============================================================
// 树液提取器（gimmick_sap_extractor）是"被动机器"：
//   - 产出不在 0x2b8 指针数组（那是固定 3x8 backing，不是 RawPointerVector）
//   - 而是存储在 status map（哈希表）：
//       status+0x58：键 "itemID0/1/2" → 值 = 物品 ID（u64）
//       status+0x18：键 "count" → 值 = 产出数量（int，2026-09-02 实测；无 itemVal{slot} 键）
//   转移流程（方案 B：自建物化路径，绕过 outputHelper）：
//     1. 读 status map 拿到 itemID + count（2026-09-02 反汇编确认：outputHelper 内部
//        用 itemVal%d 查 0x18 map，但树液提取器只有 count 键，导致返回 null）
//     2. 读 0x2b8 backing 前缀确定 rank（level1 只用前 8 条目）
//     3. 自建物化：allocate(0x2C0) → ctor(mem, savePtr, itemId) → setCount(count)
//        复刻 outputHelper 内部逻辑但绕过其 itemVal%d 检查
//     4. 在存储箱子中找同类堆合并（AddReference + itemAdjust）或写入空闲槽
//     5. 清空提取器槽位（count→0, itemID→0, 时间戳→0, 0x2b8 backing→release+置零）
//     6. 转移完 G::intrusiveRelease 释放物化物品的 owned 引用
//        （v1.9.1：所有失败路径同样释放——引用计数语义见 SapReleaseOwnedRef 注释）

// ---- 移植辅助：读 status map 值（哈希表遍历，键为短字符串）----
static bool SapReadStatusMapValue(void* status, uintptr_t mapOffset,
                                  const char* key, u64* output,
                                  bool valueIsInt) {
    if (!status || !key || !output || strlen(key) > 8 ||
        !IsReadable((void*)((uintptr_t)status + mapOffset), 0x38))
        return false;
    u64 packedKey = 0;
    memcpy(&packedKey, key, strlen(key));
    unsigned char* map = (unsigned char*)status + mapOffset;
    void* sentinel = *(void**)(map + 0x08);
    const u64 size = *(const u64*)(map + 0x10);
    void* buckets = *(void**)(map + 0x18);
    const u64 mask = *(const u64*)(map + 0x30);
    // ---- 诊断：一次性打印 map 结构（前 3 次失败/结构异常）----
    static int s_sapMapDiag = 0;
    const bool structBad = (!sentinel || !IsReadable(sentinel, 0x20) || size > 4096 ||
                            mask > 4095 || ((mask + 1) & mask) != 0 || !buckets ||
                            !IsReadable(buckets, (size_t)(mask + 1) * 16));
    if (structBad && s_sapMapDiag < 3) {
        ++s_sapMapDiag;
        Log("[AutoHarvest] [sap-dbg] map结构异常 status=%p mapOff=0x%llX key='%s' "
            "sentinel=%p size=%llu buckets=%p mask=0x%llX",
            status, (unsigned long long)mapOffset, key, sentinel,
            (unsigned long long)size, buckets, (unsigned long long)mask);
    }
    if (structBad) return false;
    void** pair = (void**)((unsigned char*)buckets + (packedKey & mask) * 16);
    void* stop = pair[0];
    void* node = pair[1];
    if ((node == sentinel) != (stop == sentinel)) return false;
    size_t visited = 0;
    while (node != sentinel && visited++ <= (size_t)size) {
        if (!node || !IsReadable(node, 0x20)) return false;
        if (*(const u64*)((unsigned char*)node + 0x10) == packedKey) {
            *output = valueIsInt
                ? (u64)(*(const int*)((unsigned char*)node + 0x18))
                : *(const u64*)((unsigned char*)node + 0x18);
            return true;
        }
        if (node == stop) break;
        node = *(void**)((unsigned char*)node + sizeof(void*));
    }
    if (s_sapMapDiag < 6) {
        ++s_sapMapDiag;
        Log("[AutoHarvest] [sap-dbg] map未命中 status=%p mapOff=0x%llX key='%s' packedKey=0x%llX "
            "sentinel=%p size=%llu buckets=%p mask=0x%llX stop=%p node=%p",
            status, (unsigned long long)mapOffset, key,
            (unsigned long long)packedKey, sentinel,
            (unsigned long long)size, buckets, (unsigned long long)mask,
            stop, node);
    }
    *output = 0;
    return false; // key 不存在时返回 false，让调用者区分“键不存在”和“键存在值=0”
                  // 反汇编确认：map 命中时 inserted=false，未命中时 inserted=true
                  // 之前返回 true 导致 SapIsProductionComplete 永远认为 procEnd 存在→提前收割
}

// ---- 写回 status map 值（找到节点后写入，不插入新节点）----
static bool SapWriteStatusMapValue(void* status, uintptr_t mapOffset,
                                    const char* key, u64 value,
                                    bool valueIsInt) {
    if (!status || !key || strlen(key) > 8 ||
        !IsReadable((void*)((uintptr_t)status + mapOffset), 0x38))
        return false;
    u64 packedKey = 0;
    memcpy(&packedKey, key, strlen(key));
    unsigned char* map = (unsigned char*)status + mapOffset;
    void* sentinel = *(void**)(map + 0x08);
    const u64 size = *(const u64*)(map + 0x10);
    void* buckets = *(void**)(map + 0x18);
    const u64 mask = *(const u64*)(map + 0x30);
    if (!sentinel || !IsReadable(sentinel, 0x20) || size > 4096 ||
        mask > 4095 || ((mask + 1) & mask) != 0 || !buckets ||
        !IsReadable(buckets, (size_t)(mask + 1) * 16))
        return false;
    void** pair = (void**)((unsigned char*)buckets + (packedKey & mask) * 16);
    void* stop = pair[0];
    void* node = pair[1];
    if ((node == sentinel) != (stop == sentinel)) return false;
    size_t visited = 0;
    while (node != sentinel && visited++ <= (size_t)size) {
        if (!node || !IsReadable(node, 0x20)) return false;
        if (*(const u64*)((unsigned char*)node + 0x10) == packedKey) {
            if (valueIsInt)
                *(int*)((unsigned char*)node + 0x18) = (int)value;
            else
                *(u64*)((unsigned char*)node + 0x18) = value;
            return true;
        }
        if (node == stop) break;
        node = *(void**)((unsigned char*)node + sizeof(void*));
    }
    return false; // key not found, no insert
}

// ---- 清空提取器槽位状态（已删除，v1.8.0 死档风险修复）----
// v1.5.6 SapClearExtractorSlot 为死代码（从未被调用），含手写引用计数递减
// +vtable[0] 析构 + 直接写 nullptr 到 backing 数组。一旦被误调用即死档。
// 已删除，不再保留。

// ---- 读取机器槽位上限（level2=3 槽，否则 1 槽）----
static bool SapReadSlotLimit(void* machineStatus, size_t* limitOut) {
    if (!machineStatus || !limitOut) return false;
    void* holder = nullptr;
    if (!ReadPtr(machineStatus, GIMMICK_DATA_HOLDER_OFFSET, &holder)) return false;
    void* data = nullptr;
    if (!ReadPtr(holder, 0, &data)) return false;
    if (!IsReadable((void*)((uintptr_t)data + SAP_DATA_LEVEL_POINTER_OFFSET),
                    sizeof(void*) + sizeof(u64)))
        return false;
    const char* level = *(const char* const*)((unsigned char*)data + SAP_DATA_LEVEL_POINTER_OFFSET);
    const u64 levelLength = *(const u64*)((unsigned char*)data + SAP_DATA_LEVEL_LENGTH_OFFSET);
    const bool levelTwo = levelLength == 1 && level && IsReadable(level, 1) && level[0] == '2';
    *limitOut = levelTwo ? SAP_MACHINE_SLOTS : 1;
    return true;
}

// ---- 读取机器产出 rank（从 0x2b8 backing 槽位前缀）----
static bool SapReadExpectedOutputRank(void* machineStatus, size_t slot,
                                      u64 itemId, int* rankOut) {
    if (!machineStatus || !rankOut || slot >= SAP_MACHINE_SLOTS ||
        !IsReadable((void*)((uintptr_t)machineStatus + SAP_STATUS_ITEM_LIST_OFFSET),
                    sizeof(void*)))
        return false;
    void** items = *(void***)((unsigned char*)machineStatus + SAP_STATUS_ITEM_LIST_OFFSET);
    if (!items || !IsReadable(items, (slot + 1) * SAP_SLOT_INGREDIENTS * sizeof(void*)))
        return false;
    int rank = 0;
    const size_t first = slot * SAP_SLOT_INGREDIENTS;
    for (size_t index = first; index < first + SAP_SLOT_INGREDIENTS; ++index) {
        if (!items[index]) continue;
        const unsigned char* item = (const unsigned char*)items[index];
        if (!IsReadable((void*)(item + ITEM_RANK_OFFSET), sizeof(int))) return false;
        const int itemRank = *(const int*)(item + ITEM_RANK_OFFSET);
        if (itemRank > rank) rank = itemRank;
    }
    *rankOut = rank;
    return true;
}

// ---- 读取存储箱子（只读 RawPointerVector，校验容量 30 格）----
static bool SapReadChestInventory(void* chestStatus, RawPointerVector* out) {
    if (!chestStatus || !out) return false;
    if (!IsReadable((void*)((uintptr_t)chestStatus + STATUS_ITEMS_OFFSET),
                    sizeof(RawPointerVector)))
        return false;
    memcpy(out, (const void*)((uintptr_t)chestStatus + STATUS_ITEMS_OFFSET),
           sizeof(RawPointerVector));
    const uintptr_t begin = (uintptr_t)out->begin;
    const uintptr_t end = (uintptr_t)out->end;
    const uintptr_t capacity = (uintptr_t)out->capacity;
    if (!begin || end < begin || capacity < end ||
        (end - begin) % sizeof(void*) != 0 ||
        (capacity - begin) % sizeof(void*) != 0) return false;
    const size_t count = (end - begin) / sizeof(void*);
    const size_t capacityCount = (capacity - begin) / sizeof(void*);
    return count == SAP_CHEST_SLOTS && capacityCount >= count &&
           capacityCount <= 64 && IsReadable(out->begin, count * sizeof(void*));
}

// ---- 转移计划（合并到同类堆 + 剩余入空槽）----
struct SapMergeTarget { void* stack; size_t slot; int before; int amount; };
struct SapTransferPlan {
    void** vectorBegin;
    SapMergeTarget merges[SAP_CHEST_SLOTS];
    size_t mergeCount;
    size_t freeSlot;
    int rank;
    int remaining;
    bool usesFreeSlot;
};
enum class SapPlanResult : unsigned char { Ready, CapacityInsufficient, Invalid };

static SapPlanResult SapPlanChestDelivery(void* chestStatus, u64 itemId, int rank,
                                          int amount,
                                          SapTransferPlan* plan) {
    if (!chestStatus || !plan || amount <= 0) return SapPlanResult::Invalid;
    *plan = {};
    plan->freeSlot = SAP_CHEST_SLOTS;
    plan->rank = rank;
    plan->remaining = amount;
    RawPointerVector items = {};
    if (!SapReadChestInventory(chestStatus, &items)) return SapPlanResult::Invalid;
    plan->vectorBegin = items.begin;
    size_t firstFree = SAP_CHEST_SLOTS;
    for (size_t slot = 0; slot < SAP_CHEST_SLOTS; ++slot) {
        void* candidate = items.begin[slot];
        if (!candidate) {
            if (firstFree == SAP_CHEST_SLOTS) firstFree = slot;
            continue;
        }
        ItemInfo info = {};
        if (!ReadItem(candidate, &info)) return SapPlanResult::Invalid;
        if (info.itemId != itemId || info.rank != rank) continue;
        if (info.stackCount >= 999) continue;
        const int capacity = 999 - info.stackCount;
        const int move = capacity < plan->remaining ? capacity : plan->remaining;
        SapMergeTarget& target = plan->merges[plan->mergeCount++];
        target.stack = candidate;
        target.slot = slot;
        target.before = info.stackCount;
        target.amount = move;
        plan->remaining -= move;
    }
    if (plan->remaining > 0) {
        if (firstFree == SAP_CHEST_SLOTS) return SapPlanResult::CapacityInsufficient;
        plan->usesFreeSlot = true;
        plan->freeSlot = firstFree;
    }
    return SapPlanResult::Ready;
}

static bool SapPlanUnchanged(const SapTransferPlan& before,
                             const SapTransferPlan& after) {
    if (before.vectorBegin != after.vectorBegin ||
        before.mergeCount != after.mergeCount ||
        before.freeSlot != after.freeSlot || before.rank != after.rank ||
        before.remaining != after.remaining ||
        before.usesFreeSlot != after.usesFreeSlot) return false;
    for (size_t index = 0; index < before.mergeCount; ++index) {
        const SapMergeTarget& left = before.merges[index];
        const SapMergeTarget& right = after.merges[index];
        if (left.stack != right.stack || left.slot != right.slot ||
            left.before != right.before || left.amount != right.amount)
            return false;
    }
    return true;
}

// v1.7.0: 恢复时间判断——参考 production_automation.inl L2646-2668
// 诊断日志确认 count=1 在生产开始时就设置（预期产出数量），不是完成后才设置。
// v1.6.2 去掉时间判断改为 count>0 即收 → 收集了未完成的树液（进度仅 10%）。
// 正确判断：gameSecond - start[slot] >= duration[slot] 才算完成。
// 时间判断内联到 SapTransferSlot 和 HasSapToCollect 中（下方）。
// 清空后重置 start 时间戳（见 SapTransferSlot L9 区块）保证下一轮生产周期正常。

// ---- v1.9.1: owned 引用释放辅助 ----
// 引用计数语义（2026-09-14 反汇编实锤）：
//   - memset(0x2C0) 归零后 ctor(0xFF870) 不写 [item+8]，引用计数从 0 起
//   - SapTransferSlot 4e 的 AddRef 即唯一 owned ref（对应原生 0x166223
//     的 lock inc）；槽位插入的 AddRef 是槽位自己的引用
//   - intrusiveRelease 递减，归零走 vtable[0] 销毁——与游戏原生释放路径
//     （0x166510 lock xadd + old==1 → vtable[0]）一致
//   - ctor 内 0x13B4F0(save,item) 是 FNV 哈希 ID 登记表，不持引用
//     （原生对已登记物品同样 ref=0 即销毁）→ 单次释放无双释放风险，
//     修复 v1.9.0 前“宁可泄漏也不双重释放”的误判
static void SapReleaseOwnedRef(void* item) {
    if (!item) return;
    void* ref = item;
    if (G::intrusiveRelease) G::intrusiveRelease(&ref);
}

// ---- 转移一个槽位的产出到存储箱 ----
// 返回 true = 已转移（可能部分转移）
static bool SapTransferSlot(void* player, void* extractorStatus, void* chestStatus,
                            size_t slot, size_t* movedStacks, size_t* movedItems) {
    if (!player || !extractorStatus || !chestStatus || !movedStacks || !movedItems)
        return false;

// 1) 读 status map：itemID{slot} / count
    // v1.5.9: 改回 count 键——树液提取器 0x18 map 只有 count 键（2026-09-02 反汇编实测），
    // 无 itemVal{slot} 键；v1.5.8 误用 itemVal{slot} 导致实测 64 次 map 未命中、树液完全不入箱。
    // 与 DumpExtractorSlotState / HasSapToCollect 的 count 键保持一致。
    // 多槽提取器第 1 槽以上用 count{slot} 后缀键。
    char idKey[8] = {};
    char countKey[12] = {};
    _snprintf_s(idKey, sizeof(idKey), _TRUNCATE, "itemID%zu", slot);
    _snprintf_s(countKey, sizeof(countKey), _TRUNCATE, "count");
    u64 itemId = 0;
    u64 countValue = 0;
    if (!SapReadStatusMapValue(extractorStatus, SAP_STATUS_ITEMID_MAP_OFFSET, idKey, &itemId, false)) {
        Log("[AutoHarvest] [sap-dbg] 槽位map读取失败 status=%p slot=%zu key=%s",
            extractorStatus, slot, idKey);
        return false;
    }
    if (!SapReadStatusMapValue(extractorStatus, SAP_STATUS_ITEMVAL_MAP_OFFSET, countKey, &countValue, true)) {
        // count 主键不存在：多槽提取器尝试 count{slot} 后缀键
        if (slot == 0) {
            Log("[AutoHarvest] [sap-dbg] 槽位map读取失败 status=%p slot=%zu key=%s",
                extractorStatus, slot, countKey);
            return false;
        }
        _snprintf_s(countKey, sizeof(countKey), _TRUNCATE, "count%zu", slot);
        if (!SapReadStatusMapValue(extractorStatus, SAP_STATUS_ITEMVAL_MAP_OFFSET, countKey, &countValue, true)) {
            Log("[AutoHarvest] [sap-dbg] 槽位map读取失败 status=%p slot=%zu key=%s",
                extractorStatus, slot, countKey);
            return false;
        }
    } else if (countValue == 0 && slot > 0) {
        // count 主键存在但为 0，尝试后缀键
        _snprintf_s(countKey, sizeof(countKey), _TRUNCATE, "count%zu", slot);
        if (!SapReadStatusMapValue(extractorStatus, SAP_STATUS_ITEMVAL_MAP_OFFSET, countKey, &countValue, true)) {
            Log("[AutoHarvest] [sap-dbg] 槽位map读取失败 status=%p slot=%zu key=%s",
                extractorStatus, slot, countKey);
            return false;
        }
    } else if (slot > 0) {
        // v1.8.7: count 主键存在且非 0，但 slot>0 时不能直接用主键值——
        // 先尝试 count{slot} 后缀键，失败则回退到主键值
        char slotKey[12] = {};
        _snprintf_s(slotKey, sizeof(slotKey), _TRUNCATE, "count%zu", slot);
        u64 slotValue = 0;
        if (SapReadStatusMapValue(extractorStatus, SAP_STATUS_ITEMVAL_MAP_OFFSET, slotKey, &slotValue, true)) {
            countValue = slotValue;  // 后缀键存在，用专用值
        }
        // 后缀键不存在则保持主键的 countValue（单槽提取器兼容行为）
    }
    const int amount = (int)countValue;

    // v1.6.1-diag: 打印每槽位完整数据（itemId/count/amount），不限次数
    Log("[AutoHarvest] [sap-diag] status=%p slot=%zu itemId=%llu count=%llu amount=%d",
        extractorStatus, slot,
        (unsigned long long)itemId, (unsigned long long)countValue, amount);

    // v1.7.0: 恢复时间判断——参考 production_automation.inl L2646-2668
    // count>0 不代表完成：游戏在生产开始时就设置 count=1（预期产出数量）
    // 正确判断：gameSecond - start[slot] >= duration[slot]
    if (amount <= 0) {
        Log("[AutoHarvest] [sap-dbg] count=0, 跳过 status=%p slot=%zu",
            extractorStatus, slot);
        return false;
    }
    // 时间完成判断
    {
        uintptr_t startOff = (uintptr_t)extractorStatus +
                             SAP_STATUS_TIME_VALUE_OFFSET + slot * sizeof(u64);
        uintptr_t durOff   = (uintptr_t)extractorStatus +
                             SAP_STATUS_CREATE_TIME_OFFSET + slot * sizeof(u64);
        u64 startVal = 0, durVal = 0;
        if (IsReadable((void*)startOff, sizeof(u64)) &&
            IsReadable((void*)durOff, sizeof(u64))) {
            startVal = *(const u64*)startOff;
            durVal   = *(const u64*)durOff;
        }
        if (!startVal || !durVal) {
            Log("[AutoHarvest] [sap-dbg] 无生产任务 start=%llu dur=%llu, 跳过 status=%p slot=%zu",
                (unsigned long long)startVal, (unsigned long long)durVal,
                extractorStatus, slot);
            return false;
        }
        std::int64_t nowSec = 0;
        if (!ReadGameRawSecond(&nowSec) || nowSec < 0) {
            Log("[AutoHarvest] [sap-dbg] 游戏时间读取失败, 跳过 status=%p slot=%zu",
                extractorStatus, slot);
            return false;
        }
        std::int64_t elapsed = nowSec - (std::int64_t)startVal;
        if (elapsed < (std::int64_t)durVal) {
            Log("[AutoHarvest] [sap-dbg] 生产未完成 start=%llu dur=%llu now=%lld elapsed=%lld need=%llu, 跳过 status=%p slot=%zu",
                (unsigned long long)startVal, (unsigned long long)durVal,
                (long long)nowSec, (long long)elapsed,
                (unsigned long long)durVal,
                extractorStatus, slot);
            return false;
        }
        Log("[AutoHarvest] [sap-dbg] 生产完成 start=%llu dur=%llu now=%lld elapsed=%lld, 收集 status=%p slot=%zu",
            (unsigned long long)startVal, (unsigned long long)durVal,
            (long long)nowSec, (long long)elapsed,
            extractorStatus, slot);
    }

    // 2. 读期望 rank（0x2b8 backing 槽位前缀）
    int expectedRank = 0;
    if (!SapReadExpectedOutputRank(extractorStatus, slot, itemId, &expectedRank)) {
        Log("[AutoHarvest] [sap-dbg] rank读取失败 status=%p slot=%zu itemId=%llu",
            extractorStatus, slot, (unsigned long long)itemId);
        return false;
    }

    // 3. 计划：合并同类堆 + 剩余入空槽
    SapTransferPlan plan = {};
    SapPlanResult result = SapPlanChestDelivery(chestStatus, itemId, expectedRank, amount, &plan);
    if (result != SapPlanResult::Ready) {
        if (result == SapPlanResult::CapacityInsufficient) {
            Log("[AutoHarvest] [sap] 存储箱满，等待空间 (itemId=%llu x%d)",
                (unsigned long long)itemId, amount);
        } else {
            Log("[AutoHarvest] [sap] 存储箱计划失败 (itemId=%llu x%d)",
                (unsigned long long)itemId, amount);
        }
        return false;
    }

    // 4. 自建物化产出物品（方案 B，v1.5.6 验证过的路径）
    //    v1.5.9 实测确认：outputHelper 内部用 itemVal{slot} 查 0x18 map，
    //    但树液提取器 0x18 map 只有 count 键 → outputHelper 返回空壳 item（itemId=0）
    //    （日志 16:41:49/16:42:34 "弹出物品与计划不符 itemId=0 期望 140370"）
    //    因此恢复 v1.5.6 自建物化：allocate(0x2C0) → memset → ctor → setCount
    void* item = nullptr;
    void* savePtr = nullptr;
    // 4a. 读取 savePtr 全局指针（ctor 第 2 参数）
    if (IsReadable((void*)(G::base + RVA_SAP_SAVE_DATA_GLOBAL), sizeof(void*))) {
        void* gameRoot = *(void**)(G::base + RVA_SAP_SAVE_DATA_GLOBAL);
        if (gameRoot && IsReadable((void*)((uintptr_t)gameRoot + SAP_SAVE_DATA_OFFSET), sizeof(void*))) {
            savePtr = *(void**)((unsigned char*)gameRoot + SAP_SAVE_DATA_OFFSET);
        }
    }
    if (!savePtr) {
        Log("[AutoHarvest] [sap] savePtr 读取失败 (slot=%zu)", slot);
        return false;
    }
    // 4b. allocate
    if (!G::sapItemAllocate || !G::sapItemCtor || !G::sapItemSetCount) {
        Log("[AutoHarvest] [sap] 物化函数未绑定 (slot=%zu)", slot);
        return false;
    }
    __try {
        item = G::sapItemAllocate(SAP_ITEM_ALLOC_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[AutoHarvest] [sap] sapItemAllocate SEH 异常 (slot=%zu)", slot);
        return false;
    }
    if (!item || !IsReadable(item, SAP_ITEM_ALLOC_SIZE)) {
        Log("[AutoHarvest] [sap] sapItemAllocate 返回空 (slot=%zu)", slot);
        return false;
    }
    // 4c. memset(item, 0, 0x2C0) —— 反汇编 0x166163 确认
    __try { memset(item, 0, SAP_ITEM_ALLOC_SIZE); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // 4d. ctor(item, savePtr, itemId) —— 反汇编 0x166189 确认
    void* ctorResult = nullptr;
    __try {
        ctorResult = G::sapItemCtor(item, savePtr, itemId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[AutoHarvest] [sap] sapItemCtor SEH 异常 (slot=%zu)", slot);
        return false;
    }
    if (ctorResult) item = ctorResult;
    if (!IsReadable((void*)((uintptr_t)item + ITEM_RANK_OFFSET), sizeof(int))) {
        Log("[AutoHarvest] [sap] sapItemCtor 物化失败 (slot=%zu)", slot);
        return false;
    }
    // 4e. AddRef (lock inc [item+8]) —— 反汇编 0x1661A3 确认
    __try {
        InterlockedIncrement((volatile LONG*)((unsigned char*)item + SAP_ITEM_REF_COUNT_OFFSET));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // 4f. setCount(item, count) —— 反汇编 0x1661AC 确认
    __try {
        G::sapItemSetCount(item, amount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[AutoHarvest] [sap] sapItemSetCount SEH 异常 (slot=%zu)", slot);
        SapReleaseOwnedRef(item);   // v1.9.1: 失败路径释放 owned 引用（防泄漏）
        return false;
    }
    // 4g. 设置 rank (item+0x280 = expectedRank) —— 反汇编 0x1664EC 确认
    if (expectedRank > 0 && IsReadable((void*)((uintptr_t)item + SAP_ITEM_RANK_OFFSET), sizeof(int))) {
        *(int*)((unsigned char*)item + SAP_ITEM_RANK_OFFSET) = expectedRank;
    }

    // 5. 验证弹出物品与计划一致
    ItemInfo popped = {};
    if (!ReadItem(item, &popped) || popped.itemId != itemId ||
        popped.stackCount != amount || popped.rank != expectedRank) {
        Log("[AutoHarvest] [sap] 弹出物品与计划不符 itemId=%llu x%d rank=%d (期望 %llu x%d rank=%d)",
            (unsigned long long)popped.itemId, popped.stackCount, popped.rank,
            (unsigned long long)itemId, amount, expectedRank);
        // v1.9.1: 释放 owned 引用——反汇编实锤 ctor 的 ID 登记表不持引用，
        // 且此时尚未入槽/未合并，我们是唯一持有者，单次释放无双释放风险
        // （v1.9.0 的“宁可泄漏”判断源于对 0x13B4F0 注册语义的误判）
        SapReleaseOwnedRef(item);
        return false;
    }

    // 6. 合并到同类堆
    size_t appliedMerges = 0;
    bool commitFailed = false;
    for (; appliedMerges < plan.mergeCount; ++appliedMerges) {
        const SapMergeTarget& target = plan.merges[appliedMerges];
        // v1.9.6 H8+D1: SEH 保护 itemAdjust
        if (!SafeItemAdjust(target.stack, target.amount)) {
            ++appliedMerges;
            commitFailed = true;
            break;
        }
        int afterCount = 0;
        if (!ReadStackCount(target.stack, &afterCount) || afterCount != target.before + target.amount) {
            ++appliedMerges;
            commitFailed = true;
            break;
        }
    }
    if (commitFailed) {
        // v1.8.0: 真正回滚——撤销已成功的合并（反向 itemAdjust）
        for (size_t i = 0; i + 1 < appliedMerges; ++i) {
            const SapMergeTarget& target = plan.merges[i];
            if (!SafeItemAdjust(target.stack, -target.amount))
                Log("[AutoHarvest] [sap] SEH: 回滚合并失败 slot=%zu i=%zu", slot, i);
        }
        Log("[AutoHarvest] [sap] 合并失败，已回滚 %zu 项合并 (slot=%zu)",
            appliedMerges > 0 ? appliedMerges - 1 : 0, slot);
        SapReleaseOwnedRef(item);   // v1.9.1: 失败路径释放 owned 引用（防泄漏）
        return false;
    }

    // v1.8.1: 合并成功后后续步骤失败的回滚辅助——撤销全部已成功的 itemAdjust 合并
    auto rollbackMerges = [&]() {
        for (size_t i = 0; i < appliedMerges; ++i) {
            const SapMergeTarget& t = plan.merges[i];
            if (!SafeItemAdjust(t.stack, -t.amount))
                Log("[AutoHarvest] [sap] SEH: 回滚合并失败 slot=%zu i=%zu", slot, i);
        }
        Log("[AutoHarvest] [sap] 后续步骤失败，已回滚 %zu 项合并 (slot=%zu)",
            appliedMerges, slot);
    };

    // 7. 弹出物品剩余数量调整（amount - 已合并）
    const int sourceDelta = plan.remaining - amount;
    if (sourceDelta != 0) SafeItemAdjust(item, sourceDelta);
    int sourceCount = 0;
    if (!ReadStackCount(item, &sourceCount) || sourceCount != plan.remaining) {
        Log("[AutoHarvest] [sap] 剩余数量调整失败 (slot=%zu)", slot);
        rollbackMerges();
        SapReleaseOwnedRef(item);   // v1.9.1: 失败路径释放 owned 引用（防泄漏）
        return false;
    }

    // 8. 入空闲槽
    if (plan.usesFreeSlot) {
        if (!plan.vectorBegin || plan.freeSlot >= SAP_CHEST_SLOTS) {
            Log("[AutoHarvest] [sap] 空槽越界 (slot=%zu)", slot);
            rollbackMerges();
            SapReleaseOwnedRef(item);   // v1.9.1: 失败路径释放 owned 引用（防泄漏）
            return false;
        }
        void** freeSlot = &plan.vectorBegin[plan.freeSlot];
        if (!IsReadable(freeSlot, sizeof(void*)) || *freeSlot != nullptr) {
            Log("[AutoHarvest] [sap] 空槽被占用 (slot=%zu)", slot);
            rollbackMerges();
            SapReleaseOwnedRef(item);   // v1.9.1: 失败路径释放 owned 引用（防泄漏）
            return false;
        }
        // 持有引用后写入
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(
            reinterpret_cast<uintptr_t>(item) + sizeof(void*)));
        void* observed = InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(freeSlot), item, nullptr);
        if (observed != nullptr) {
            // 槽位已被占用：撤销槽位 AddRef（不覆盖）+ 释放 owned 引用（v1.9.1）
            // ref=2 → 两次释放归零销毁（槽位未写入，我们是唯一持有者）
            void* ownedRef = item;
            if (G::intrusiveRelease) G::intrusiveRelease(&ownedRef);
            SapReleaseOwnedRef(item);
            Log("[AutoHarvest] [sap] 空槽写入冲突 (slot=%zu)", slot);
            rollbackMerges();
            return false;
        }
        if (!IsReadable(freeSlot, sizeof(void*)) || *freeSlot != item) {
            Log("[AutoHarvest] [sap] 空槽写入未验证 (slot=%zu)", slot);
            rollbackMerges();
            SapReleaseOwnedRef(item);   // v1.9.1: 释放 owned（槽位引用随物品留在槽内）
            return false;
        }
    }

    // 9. 清空提取器槽位产出 map（自建物化后需手动清，outputHelper 已被替换）
    //    v1.5.10: 只清产出 map 键（count/itemID）+ 重置 start 时间戳，
    //    不触碰 0x2b8 backing —— 那是输入成分槽，误释放会破坏游戏状态
    //    （v1.5.6 手写清 backing 导致死档的教训），此处为安全清槽。
    //    v1.8.0: AH-H2 修复——先读取游戏时间，失败则 abort 整个清槽，
    //    避免出现"map 已清零但时间戳未重置"的不一致状态（会致空指针崩溃）。
    {
        // 9a. 先读取游戏时间，失败则 abort（此时 map 尚未被修改，状态一致）
        std::int64_t nowSec = 0;
        if (!ReadGameRawSecond(&nowSec) || nowSec <= 0) {
            Log("[AutoHarvest] [sap] 游戏时间读取失败，中止清槽避免不一致 (slot=%zu)", slot);
            // v1.9.6 D2: 撤销已写入的箱子空闲槽位，防止物品复制
            // （CAS 写箱成功 + 清槽失败 = 物品既在箱中又在提取器中 → 下次收集复制）
            if (plan.usesFreeSlot) {
                void** freeSlot = &plan.vectorBegin[plan.freeSlot];
                if (IsReadable(freeSlot, sizeof(void*)) && *freeSlot == item) {
                    InterlockedCompareExchangePointer(
                        reinterpret_cast<void* volatile*>(freeSlot), nullptr, item);
                    Log("[AutoHarvest] [sap] D2 修复：已撤销空闲槽写入 (slot=%zu freeSlot=%zu)",
                        slot, plan.freeSlot);
                }
            }
            // 释放物化物品引用（owned ref + slot AddRef 两次释放，ref 2→0 销毁）
            void* ownedRef = item;
            if (G::intrusiveRelease) G::intrusiveRelease(&ownedRef);
            SapReleaseOwnedRef(item);
            rollbackMerges();
            return false;
        }

        // 9b. 时间戳读取成功，先重置 start 时间戳
        uintptr_t startOff = (uintptr_t)extractorStatus +
                             SAP_STATUS_TIME_VALUE_OFFSET + slot * sizeof(u64);
        if (IsReadable((void*)startOff, sizeof(u64))) {
            *(u64*)startOff = (u64)nowSec;
        }

        // 9c. 再清产出 map 键（此时时间戳已重置，即使后续操作异常也不会不一致）
        char cntKey[9] = {};
        _snprintf_s(cntKey, sizeof(cntKey), _TRUNCATE, "count");
        SapWriteStatusMapValue(extractorStatus, SAP_STATUS_ITEMVAL_MAP_OFFSET,
                               cntKey, 0, true);
        if (slot > 0) {
            _snprintf_s(cntKey, sizeof(cntKey), _TRUNCATE, "count%zu", slot);
            SapWriteStatusMapValue(extractorStatus, SAP_STATUS_ITEMVAL_MAP_OFFSET,
                                   cntKey, 0, true);
        }
        char idKey[8] = {};
        _snprintf_s(idKey, sizeof(idKey), _TRUNCATE, "itemID%zu", slot);
        SapWriteStatusMapValue(extractorStatus, SAP_STATUS_ITEMID_MAP_OFFSET,
                               idKey, 0, false);
    }

    // 10. 调用原生 sapStatusEventNotify 触发游戏自身的状态转换
    //     通知游戏状态已变更（inProc 重置/事件链），替代手写状态重置
    if (G::sapStatusEventNotify) {
        __try {
            G::sapStatusEventNotify(extractorStatus);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[AutoHarvest] [sap] sapStatusEventNotify SEH 异常 (slot=%zu)", slot);
        }
    }

    // 11. 收尾：释放物化物品的 owned 引用
    void* ownedRef = item;
    if (G::intrusiveRelease) G::intrusiveRelease(&ownedRef);

    if (movedStacks) (*movedStacks)++;
    if (movedItems) (*movedItems) += amount;
    Log("[AutoHarvest] [sap] 转移树液 itemId=%llu x%d rank=%d",
        (unsigned long long)itemId, amount, expectedRank);
    return true;
}

// ============================================================
// 树液提取器收集（重写版）
// ============================================================
// 树液提取器是一个 Gimmick，其功能由 ProductionAuto 管理。
// AutoHarvest 中读取树液提取器的存储槽位，
// 如果有树液物品，就转移到存储箱子。
// 树液提取器的产出存储在 status map（哈希表）而非 0x2b8 数组。
static size_t CollectFromSapExtractor(void* player, void* extractorStatus, void* chestStatus) {
    if (!player || !extractorStatus || !chestStatus) return 0;
    if (!G::sapItemAllocate || !G::sapItemCtor || !G::sapItemSetCount ||
        !G::sapStatusEventNotify || !G::sapStatusIntLookup || !G::sapStatusU64Lookup) {
#if DIAG_AUTOHARVEST
        Log("[AutoHarvest] [diag-sap] sap 函数未绑定，跳过");
#endif
        return 0;
    }

    // 读取槽位上限（level2=3，否则 1）
    size_t slotLimit = 1;
    if (!SapReadSlotLimit(extractorStatus, &slotLimit) || slotLimit == 0 ||
        slotLimit > SAP_MACHINE_SLOTS) {
#if DIAG_AUTOHARVEST
        Log("[AutoHarvest] [diag-sap] 槽位上限读取失败 status=%p", extractorStatus);
#endif
        return 0;
    }

    // v1.8.7: 移除诊断数据采集（原 L1574-1608 的 slotLimit 遍历 + 哈希表查找 + ReadGameRawSecond）
    // 发布版 Log() 为空操作，但那些代码是独立语句不是 Log 参数内，仍会执行 ~48 次/周期的哈希表查找

    size_t movedStacks = 0;
    size_t totalItems = 0;
    for (size_t slot = 0; slot < slotLimit; ++slot) {
        SapTransferSlot(player, extractorStatus, chestStatus, slot,
                        &movedStacks, &totalItems);
    }

    if (movedStacks > 0) {
        Log("[AutoHarvest] [sap] 从树液提取器转移 %d 堆到存储箱", (int)movedStacks);
    }

    return movedStacks;
}

    // 判断提取器是否有树液可采（读取 status map，不依赖 0x2b8 数组）

// ---- 诊断：实测调用 native_collect（F9 触发）----
// 目的：确认 native_collect 的真实参数约定与行为。
// 调用前 dump 提取器槽位与背包，调用后再次 dump，对比差异。
// v1.8.1: 发布版移除 F9 诊断链（#if 0 包裹，不参与编译）
#if 0
static void DumpExtractorSlotState(void* status, const char* tag) {
    if (!status) return;
    // 0x58 map: itemID0/1/2
    for (size_t s = 0; s < 3; ++s) {
        char idKey[8] = {}; _snprintf_s(idKey, sizeof(idKey), _TRUNCATE, "itemID%zu", s);
        u64 idv = 0;
        SapReadStatusMapValue(status, SAP_STATUS_ITEMID_MAP_OFFSET, idKey, &idv, false);
        char cntKey[9] = {}; _snprintf_s(cntKey, sizeof(cntKey), _TRUNCATE, "count");
        u64 cv = 0;
        SapReadStatusMapValue(status, SAP_STATUS_ITEMVAL_MAP_OFFSET, cntKey, &cv, true);
        // count{slot} 后缀
        char cntKey2[9] = {}; _snprintf_s(cntKey2, sizeof(cntKey2), _TRUNCATE, "count%zu", s);
        u64 cv2 = 0;
        SapReadStatusMapValue(status, SAP_STATUS_ITEMVAL_MAP_OFFSET, cntKey2, &cv2, true);
        Log("[AutoHarvest] [nc] %s slot%zu itemID=%llu count=%llu count%zu=%llu",
            tag, s, (unsigned long long)idv,
            (unsigned long long)cv, s, (unsigned long long)cv2);
    }
    // 时间戳
    for (int si = 0; si < 3; ++si) {
        if (IsReadable((void*)((uintptr_t)status + SAP_STATUS_TIME_VALUE_OFFSET + si * sizeof(u64)), sizeof(u64)) &&
            IsReadable((void*)((uintptr_t)status + SAP_STATUS_CREATE_TIME_OFFSET + si * sizeof(u64)), sizeof(u64))) {
            u64 st = *(const u64*)((unsigned char*)status + SAP_STATUS_TIME_VALUE_OFFSET + si * sizeof(u64));
            u64 du = *(const u64*)((unsigned char*)status + SAP_STATUS_CREATE_TIME_OFFSET + si * sizeof(u64));
            Log("[AutoHarvest] [nc] %s ts slot%d start=%llu dur=%llu end=%llu",
                tag, si, (unsigned long long)st, (unsigned long long)du, (unsigned long long)(st + du));
        }
    }
    { std::int64_t rs = 0; if (ReadGameRawSecond(&rs)) {
        Log("[AutoHarvest] [nc] %s gameNow rawSec=%lld", tag, (long long)rs);
    }}

    // 0x2b8 backing
    void** items = nullptr;
    if (IsReadable((void*)((uintptr_t)status + SAP_STATUS_ITEM_LIST_OFFSET), sizeof(void*))) {
        items = *(void***)((unsigned char*)status + SAP_STATUS_ITEM_LIST_OFFSET);
        if (items && IsReadable(items, 3 * SAP_SLOT_INGREDIENTS * sizeof(void*))) {
            for (int i = 0; i < 3 * SAP_SLOT_INGREDIENTS; ++i) {
                if (items[i]) {
                    u64 obj0 = 0;
                    if (IsReadable(items[i], 0x10)) obj0 = *(const u64*)items[i];
                    Log("[AutoHarvest] [nc] %s items[%d]=%p obj0=0x%llX",
                        tag, i, items[i], (unsigned long long)obj0);
                }
            }
        }
    }
    // 背包：root(+0x208)->player_obj(+0x32B8)->items(+0x32C0)
    void* player = nullptr;
    {
        void* root = *(void**)(G::base + G::rvaGameRoot);
        if (root && IsReadable((void*)((uintptr_t)root + ROOT_PLAYER_OFFSET), sizeof(void*))) {
            void* playerObj = *(void**)((unsigned char*)root + ROOT_PLAYER_OFFSET);
            if (playerObj && IsReadable((void*)((uintptr_t)playerObj + PLAYER_OBJECT_STATUS_OFFSET), sizeof(void*))) {
                void* playerStatus = *(void**)((unsigned char*)playerObj + PLAYER_OBJECT_STATUS_OFFSET);
                player = playerStatus;
                if (playerStatus && IsReadable((void*)((uintptr_t)playerStatus + PLAYER_ITEMS_OFFSET), 30 * sizeof(void*))) {
                    void** slots = *(void***)((unsigned char*)playerStatus + PLAYER_ITEMS_OFFSET);
                    if (slots && IsReadable(slots, 30 * sizeof(void*))) {
                        int n = 0;
                        for (int i = 0; i < 30; ++i) {
                            if (slots[i]) {
                                u64 idv = 0, cntv = 0;
                                if (IsReadable(slots[i], 0x10)) idv = *(const u64*)slots[i];
                                if (IsReadable((void*)((uintptr_t)slots[i] + ITEM_STACK_COUNT_OFFSET), sizeof(int)))
                                    cntv = (u64)(*(const int*)((unsigned char*)slots[i] + ITEM_STACK_COUNT_OFFSET));
                                Log("[AutoHarvest] [nc] %s inv[%d]=%p id=0x%llX cnt=%llu",
                                    tag, i, slots[i], (unsigned long long)idv, (unsigned long long)cntv);
                                if (++n >= 10) break;
                            }
                        }
                        if (n == 0) Log("[AutoHarvest] [nc] %s inv empty", tag);
                    } else {
                        Log("[AutoHarvest] [nc] %s slots null/unreadable", tag);
                    }
                } else {
                    Log("[AutoHarvest] [nc] %s playerStatus items unreadable", tag);
                }
            } else {
                Log("[AutoHarvest] [nc] %s playerObj unreadable", tag);
            }
        }
    }
}

static void ProbeNativeCollect() {
    Log("[AutoHarvest] [nc] ===== F9 诊断：native_collect 实测 =====");
    if (!G::sapNativeCollect) { Log("[AutoHarvest] [nc] native_collect 未绑定"); return; }

    // 找最近的提取器
    WorldContext ctx = {};
    if (!GetWorldContext(&ctx)) { Log("[AutoHarvest] [nc] 无法获取世界上下文"); return; }
    SapExtractorCandidate extractors[MAX_EXTRACTORS] = {};
    size_t n = SearchSapExtractors(ctx.spatialIndex, ctx.position, extractors, MAX_EXTRACTORS);
    if (n == 0) { Log("[AutoHarvest] [nc] 未找到提取器"); return; }
    // 取最近
    void* status = extractors[0].status;
    Log("[AutoHarvest] [nc] 目标提取器 status=%p dist=%.2f", status, sqrtf(extractors[0].distSq));

    // 读提取器 baseID：status->holder(+0x240)->data(+0)->u64 baseID
    // 反汇编确认：ProductionReadGimmickType 从 data+0 读 u64 gimmickId
    // 已知 gimmick_sap_extractor = 240250000
    u64 baseId = 0;
    {
        void* holder = nullptr;
        if (ReadPtr(status, GIMMICK_DATA_HOLDER_OFFSET, &holder)) {
            void* data = nullptr;
            if (ReadPtr(holder, 0, &data) && data && IsReadable(data, sizeof(u64))) {
                baseId = *(const u64*)((unsigned char*)data);
                Log("[AutoHarvest] [nc] baseID=%llu (0x%llX) from data=%p", (unsigned long long)baseId, (unsigned long long)baseId, data);
            } else {
                Log("[AutoHarvest] [nc] read data failed holder=%p", holder);
            }
        } else {
            Log("[AutoHarvest] [nc] read holder failed status=%p", status);
        }
    }
    if (baseId == 0) { Log("[AutoHarvest] [nc] baseID=0, skip"); return; }
    Log("[AutoHarvest] [nc] expected=240250000 actual=%llu %s", (unsigned long long)baseId, baseId == 240250000ULL ? "(match)" : "(MISMATCH!)");

    Log("[AutoHarvest] [nc] === before ===");
    DumpExtractorSlotState(status, "before");

    // 调用 native_collect(baseID, status, nullptr, nullptr)
    // 约定：rcx=baseID(u64), rdx=machineStatus(void*)
    void* ret = nullptr;
    __try {
        ret = G::sapNativeCollect(baseId, nullptr, nullptr, reinterpret_cast<void*>(2));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[AutoHarvest] [nc] native_collect SEH! baseID=%llu", (unsigned long long)baseId);
        ret = nullptr;
    }
    Log("[AutoHarvest] [nc] ret=%p (baseID=%llu)", ret, (unsigned long long)baseId);

    Log("[AutoHarvest] [nc] === 后调用状态 ===");
    DumpExtractorSlotState(status, "after");

    // 刷新背包/箱子 UI
    if (G::batchRecalc) { __try { G::batchRecalc(ctx.player, false); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (G::batchUIRefresh) { __try { G::batchUIRefresh(ctx.player, true); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (G::batchDirty) { __try { G::batchDirty(ctx.player, 0x2bc); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    Log("[AutoHarvest] [nc] ===== 诊断结束 =====");
}
#endif // #if 0 F9 诊断链（发布版不编译）

    static bool HasSapToCollect(void* extractorStatus) {
        if (!extractorStatus) return false;
        if (!G::sapStatusIntLookup || !G::sapStatusU64Lookup) return false;
        size_t slotLimit = 1;
        if (!SapReadSlotLimit(extractorStatus, &slotLimit) || slotLimit == 0 ||
            slotLimit > SAP_MACHINE_SLOTS) return false;
        for (size_t slot = 0; slot < slotLimit; ++slot) {
            char idKey[8] = {};
            char countKey[9] = {};
            _snprintf_s(idKey, sizeof(idKey), _TRUNCATE, "itemID%zu", slot);
            _snprintf_s(countKey, sizeof(countKey), _TRUNCATE, "count");
            u64 itemId = 0;
            u64 countValue = 0;
            if (!SapReadStatusMapValue(extractorStatus, SAP_STATUS_ITEMID_MAP_OFFSET,
                                       idKey, &itemId, false) ||
                !SapReadStatusMapValue(extractorStatus, SAP_STATUS_ITEMVAL_MAP_OFFSET,
                                       countKey, &countValue, true))
                return false;
            if (countValue == 0 && slot > 0) {
                // 兼容多槽提取器：尝试带后缀 count{slot} 键
                _snprintf_s(countKey, sizeof(countKey), _TRUNCATE, "count%zu", slot);
                if (!SapReadStatusMapValue(extractorStatus, SAP_STATUS_ITEMVAL_MAP_OFFSET,
                                           countKey, &countValue, true))
                    return false;
            }
            // v1.7.0: 恢复时间判断——count>0 不代表完成
            if (itemId != 0 && countValue > 0 && countValue <= 64) {
                // 检查时间是否到期
                uintptr_t startOff = (uintptr_t)extractorStatus +
                                     SAP_STATUS_TIME_VALUE_OFFSET + slot * sizeof(u64);
                uintptr_t durOff   = (uintptr_t)extractorStatus +
                                     SAP_STATUS_CREATE_TIME_OFFSET + slot * sizeof(u64);
                u64 startVal = 0, durVal = 0;
                if (IsReadable((void*)startOff, sizeof(u64)) &&
                    IsReadable((void*)durOff, sizeof(u64))) {
                    startVal = *(const u64*)startOff;
                    durVal   = *(const u64*)durOff;
                }
                if (startVal > 0 && durVal > 0) {
                    std::int64_t nowSec = 0;
                    if (ReadGameRawSecond(&nowSec) && nowSec >= 0 &&
                        nowSec - (std::int64_t)startVal >= (std::int64_t)durVal) {
                        return true; // 生产完成，可以收
                    }
                    // 生产未完成，继续检查下一个槽位
                } else {
                    return true; // 无时间信息，保守收取
                }
            }
        }
        return false;
    }

// ============================================================
// 前向声明
// ============================================================
static void RefreshHud(bool enabled, DWORD durationMs = 2000);
static void ShowHudMessage(const wchar_t* line1, const wchar_t* line2,
                           COLORREF accent, DWORD durationMs);

// ============================================================
// 游戏时钟读取（源自 AutoFish 已验证路径）
// 路径：game_root(+0x10FCBB0) -> +0x208 (save data) -> +0x3270 (raw second)
// 换算：dayIndex = rawSecond / 86400
// ============================================================
static constexpr uintptr_t SAVE_DATA_OFFSET      = 0x208;    // game_root -> save data
static constexpr uintptr_t SAVE_RAW_SECOND_OFFSET = 0x3270;  // save -> raw second
static constexpr std::int64_t RAW_SECONDS_PER_DAY = 86400;

static bool ReadGameDayIndex(std::int64_t* outDay) {
    if (!outDay || !G::base) return false;
    void* root = *(void**)(G::base + G::rvaGameRoot);
    if (!root || !IsReadable(root, SAVE_DATA_OFFSET + sizeof(void*))) return false;
    void* save = *(void**)((unsigned char*)root + SAVE_DATA_OFFSET);
    if (!save || !IsReadable(save, SAVE_RAW_SECOND_OFFSET + sizeof(std::int64_t)))
        return false;
    std::int64_t rawSecond = *(const std::int64_t*)((unsigned char*)save + SAVE_RAW_SECOND_OFFSET);
    if (rawSecond < 0) return false;
    *outDay = rawSecond / RAW_SECONDS_PER_DAY;
    return true;
}

// 读取游戏原始秒数（不除以 86400，用于时间戳比较）
static bool ReadGameRawSecond(std::int64_t* outSec) {
    if (!outSec || !G::base) return false;
    void* root = *(void**)(G::base + G::rvaGameRoot);
    if (!root || !IsReadable(root, SAVE_DATA_OFFSET + sizeof(void*))) return false;
    void* save = *(void**)((unsigned char*)root + SAVE_DATA_OFFSET);
    if (!save || !IsReadable(save, SAVE_RAW_SECOND_OFFSET + sizeof(std::int64_t)))
        return false;
    *outSec = *(const std::int64_t*)((unsigned char*)save + SAVE_RAW_SECOND_OFFSET);
    return *outSec >= 0;
}

// 跨日重置：新的一天到来，重置轮转起点并恢复运行（手动暂停除外）
static void HandleDayRolloverLocked() {
    // 手动暂停不因跨日而恢复（用户明确停止意图）
    if (G::runMode == G::RunMode::Paused) {
        Log("[AutoHarvest] 跨日：手动暂停中，保持暂停（重设 F4 恢复）");
        return;
    }
    // 收完自动停：跨日自动恢复继续收集
    if (G::runMode == G::RunMode::Done) {
        Log("[AutoHarvest] 跨日：前一天收集完成，自动恢复采集");
    }
    G::extractorRotateOffset = 0;
    G::runMode = G::RunMode::AutoRun;
}

// 跨日检查：每次采集周期前调用。返回 true 表示检测到跨日并已处理
static bool CheckDayRollover() {
    if (G::runMode == G::RunMode::Idle) return false;   // 未设置箱子，不检测
    std::int64_t day = 0;
    if (!ReadGameDayIndex(&day)) {
        // 读不到时钟：保守策略——不清除暂停/完成标记，避免误恢复
        return false;
    }
    if (G::lastRunDay >= 0 && day != G::lastRunDay) {
        Log("[AutoHarvest] 跨日检测: day %lld -> %lld", (long long)G::lastRunDay, (long long)day);
        G::lastRunDay = day;
        HandleDayRolloverLocked();
        return true;
    }
    G::lastRunDay = day;   // 首次运行也记录当前日
    return false;
}

// ============================================================
// 执行一次采集周期（分批轮转，防卡顿）
// ============================================================
static void DoHarvestCycle(WorldContext* ctx) {
    if (!ctx || !ctx->player || !ctx->spatialIndex) return;
    if (!G::chestSet || !G::storageChest) {
        Log("[AutoHarvest] 未设置存储箱子，跳过采集");
        return;
    }

    // ---- 运行模式过滤 ----
    if (G::runMode != G::RunMode::AutoRun) {
        return;  // Idle / Paused / Done 均不执行采集
    }

    // 验证存储箱子仍然有效
    if (!IsReadable(G::storageChest, STATUS_ITEMS_OFFSET + sizeof(RawPointerVector))) {
        Log("[AutoHarvest] 存储箱子指针失效，请重新设置");
        G::chestSet = false;
        G::storageChest = nullptr;
        G::runMode = G::RunMode::Idle;
        return;
    }

    // 不清空 FastRegion 缓存——让 IsReadable 的 VirtualQuery 结果跨周期复用
    size_t totalHarvested = 0;

    // ---- 第一步：转移到存储箱子 ----
    size_t moved = TransferToChest(ctx->player, G::storageChest);
    if (moved > 0) {
        Log("[AutoHarvest] 转移 %zu 堆到存储箱", moved);
        totalHarvested += moved;
    }

    // ---- 第二步：采集已注册的树液提取器 ----
    // v1.8.8: 空间搜索已拆到 mod_tick 分片执行（ShardSearchTick），这里只做采集
    // 验证已注册提取器指针有效性，失效的移除
    if (g_cachedExtractorCount > 0) {
        size_t validCount = 0;
        for (size_t k = 0; k < g_cachedExtractorCount; ++k) {
            float px, py;
            if (IsReadable(g_cachedExtractors[k].status, 64) &&
                ReadPosition(g_cachedExtractors[k].status, &px, &py)) {
                g_cachedExtractors[k].posX = px;
                g_cachedExtractors[k].posY = py;
                g_cachedExtractors[validCount++] = g_cachedExtractors[k];
            }
        }
        g_cachedExtractorCount = validCount;
    }

    SapExtractorCandidate extractors[MAX_EXTRACTORS] = {};
    size_t extractorCount = g_cachedExtractorCount < MAX_EXTRACTORS ? g_cachedExtractorCount : MAX_EXTRACTORS;
    for (size_t k = 0; k < extractorCount; ++k) {
        extractors[k] = g_cachedExtractors[k];
    }

    if (extractorCount > 0) {
        size_t start = G::extractorRotateOffset % extractorCount;
        size_t toProcess = extractorCount < EXTRACTORS_PER_CYCLE ? extractorCount : EXTRACTORS_PER_CYCLE;

        size_t sapMoved = 0;
        for (size_t k = 0; k < toProcess; ++k) {
            size_t idx = (start + k) % extractorCount;
            // 直接试转移，CollectFromSapExtractor 内部会检查 itemID/count/时间戳
            sapMoved += CollectFromSapExtractor(ctx->player, extractors[idx].status, G::storageChest);
        }

        G::extractorRotateOffset = (start + toProcess) % extractorCount;

        if (sapMoved > 0) {
            Log("[AutoHarvest] 树液提取器转移 %zu 堆 (处理 %zu/%zu 个)",
                sapMoved, toProcess, extractorCount);
            totalHarvested += sapMoved;
        }
    }

    // 有采集时统一刷新 UI（一个周期只调一次，减少卡顿）
    // v1.7.0: 移除 RefreshHud——采集时不应反复弹 HUD，HUD 只在 F4 按下时显示
    if (totalHarvested > 0) {
        if (G::batchRecalc) { __try { G::batchRecalc(ctx->player, false); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        if (G::batchUIRefresh) { __try { G::batchUIRefresh(ctx->player, true); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        if (G::batchDirty) { __try { G::batchDirty(ctx->player, 0x2bc); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        Log("[AutoHarvest] 采集周期完成: 转移 %d 个树液", (int)totalHarvested);
    }
}

// 前向声明：HUD 按键名辅助函数（定义在 ShowHudMessage 之后）
static const wchar_t* HudKeyName(int idx, const wchar_t* fallback);

// ============================================================
// F4：设置/取消存储箱子
// ============================================================
static void HandleSetStorageChest() {
    WorldContext ctx = {};
    if (!GetWorldContext(&ctx)) {
        Log("[AutoHarvest] 无法获取世界上下文");
        return;
    }

    // 如果已设置，取消设置
    if (G::chestSet) {
        G::chestSet = false;
        G::storageChest = nullptr;
        G::runMode = G::RunMode::Idle;
        G::lastRunDay = -1;
        Log("[AutoHarvest] 存储箱子已取消，运行状态重置为 Idle");
        RefreshHud(false, 2000);
        return;
    }

    // 搜索最近的箱子
    FastRegionReset();
    ChestCandidate chests[MAX_CHESTS] = {};
    size_t chestCount = SearchChests(ctx.spatialIndex, ctx.position, chests, MAX_CHESTS, CHEST_SET_RADIUS_SQ);

    if (chestCount == 0) {
        Log("[AutoHarvest] 附近 %d 范围内没有箱子", (int)CHEST_SET_RADIUS);
        // v1.9.2: 半径收紧到一格后“没对准”概率上升——加 HUD 提示避免按了没反应
        {
            wchar_t line2[64];
            _snwprintf_s(line2, 64, _TRUNCATE,
                L"\x8BF7\x9760\x8FD1\x7BB1\x5B50\x518D\x6309\x0020%s",
                HudKeyName(0, L"F4"));
            ShowHudMessage(L"\x9644\x8FD1\x6CA1\x6709\x7BB1\x5B50",
                           line2, RGB(200, 120, 100), 2000);
        }
        return;
    }

    // 取最近的箱子
    G::storageChest = chests[0].status;
    G::storageChestX = chests[0].posX;
    G::storageChestY = chests[0].posY;
    G::chestSet = true;

    // F4 设箱后自动开始运行（AutoRun），并记录当前游戏日
    G::runMode = G::RunMode::AutoRun;
    G::extractorRotateOffset = 0;
    std::int64_t day = 0;
    if (ReadGameDayIndex(&day)) {
        G::lastRunDay = day;
    } else {
        G::lastRunDay = -1;   // 读不到时钟：跨日检测退化为不检测
    }
    Log("[AutoHarvest] 存储箱子已设置: %p (%.0f, %.0f)，自动开始采集 (day=%lld)",
        G::storageChest, G::storageChestX, G::storageChestY, (long long)G::lastRunDay);
    // 提示：设箱即自动开始
    {
        wchar_t line2[64];
        _snwprintf_s(line2, 64, _TRUNCATE,
            L"\x518D\x6309\x0020%s\x0020\x53D6\x6D88",
            HudKeyName(0, L"F4"));
        ShowHudMessage(L"\x81EA\x52A8\x91C7\x96C6\x0020\x5DF2\x5F00\x542F",
                       line2, RGB(127, 176, 105), 2500);
    }

}

// ============================================================
// HUD 瞬时提示窗口（简化版：只显示一行文字，按时消退）
// ============================================================
static HMODULE g_autoharvestModule = nullptr;
static constexpr wchar_t HARVEST_HUD_CLASS[] = L"AutoHarvestHudWindow";
static HWND g_hudWindow = nullptr;
static HFONT g_hudFont = nullptr;
static HFONT g_hudFontSmall = nullptr;

// HUD 显示内容（由 RefreshHud 设定）
static wchar_t g_hudLine1[64] = {};   // 第一行（主信息）
static wchar_t g_hudLine2[64] = {};   // 第二行（辅助信息，可为空）
static COLORREF g_hudAccent = RGB(120, 130, 140);
static ULONGLONG g_hudHideAt = 0;

static LRESULT CALLBACK HudWndProc(HWND window, UINT message,
                                    WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(window, &paint);
        RECT client = {};
        GetClientRect(window, &client);

        HBRUSH bg = CreateSolidBrush(RGB(28, 30, 34));
        FillRect(dc, &client, bg);
        DeleteObject(bg);

        // 左侧强调色条
        RECT bar = client;
        bar.right = bar.left + 6;
        HBRUSH accentBrush = CreateSolidBrush(g_hudAccent);
        FillRect(dc, &bar, accentBrush);
        DeleteObject(accentBrush);

        SetBkMode(dc, TRANSPARENT);

        // 第一行（大字）
        HFONT oldFont = (HFONT)SelectObject(dc, g_hudFont);
        SetTextColor(dc, RGB(220, 225, 230));
        RECT r1 = client;
        r1.left += 22; r1.right -= 14;
        r1.top += 6;
        r1.bottom = r1.top + 30;
        DrawTextW(dc, g_hudLine1, -1, &r1,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // 第二行（小字，如有）
        if (g_hudLine2[0] != L'\0') {
            SelectObject(dc, g_hudFontSmall);
            SetTextColor(dc, RGB(160, 165, 170));
            RECT r2 = client;
            r2.left += 22; r2.right -= 14;
            r2.top += 34;
            r2.bottom = r2.top + 20;
            DrawTextW(dc, g_hudLine2, -1, &r2,
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
    cls.hInstance = g_autoharvestModule;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    cls.lpszClassName = HARVEST_HUD_CLASS;
    if (!RegisterClassExW(&cls)) {
        DWORD err = GetLastError();
        WNDCLASSEXW existing = {};
        existing.cbSize = sizeof(existing);
        if (err != ERROR_CLASS_ALREADY_EXISTS ||
            !GetClassInfoExW(g_autoharvestModule, HARVEST_HUD_CLASS, &existing) ||
            existing.lpfnWndProc != HudWndProc ||
            existing.hInstance != g_autoharvestModule) {
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
    if (!g_hudFont || !g_hudFontSmall) return false;

    const int width = 200;
    const int height = 60;
    g_hudWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        HARVEST_HUD_CLASS, L"", WS_POPUP, 0, 0, width, height,
        nullptr, nullptr, g_autoharvestModule, nullptr);
    if (!g_hudWindow) return false;
    // v1.9.12: 移除游戏窗口 owner 绑定（GWLP_HWNDPARENT）——owned TOPMOST 窗口链
    // 干扰 Alt+Tab 前台切换，游戏窗口切不回（切窗修复第四轮，详见 game_window.h v1.5）

    SetLayeredWindowAttributes(g_hudWindow, 0, 228, LWA_ALPHA);
    HRGN rounded = CreateRoundRectRgn(0, 0, width + 1, height + 1, 12, 12);
    if (!SetWindowRgn(g_hudWindow, rounded, FALSE)) DeleteObject(rounded);
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
    const int width = 200;
    const int height = 60;
    const int x = mi.rcWork.left + 24;
    const int y = mi.rcWork.top + 94;
    SetWindowPos(g_hudWindow, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// 显示瞬时提示，durationMs 后自动隐藏
static void RefreshHud(bool enabled, DWORD durationMs) {
    if (!g_hudWindow && !InitHud()) return;

    // 构造显示内容
    if (enabled) {
        // L"自动采集 已开启"
        wcscpy_s(g_hudLine1, L"\x81EA\x52A8\x91C7\x96C6\x0020\x5DF2\x5F00\x542F");
        g_hudAccent = RGB(127, 176, 105);  // 绿色
    } else {
        // L"自动采集 已关闭"
        wcscpy_s(g_hudLine1, L"\x81EA\x52A8\x91C7\x96C6\x0020\x5DF2\x5173\x95ED");
        g_hudAccent = RGB(120, 130, 140);  // 灰色
    }

    if (G::chestSet) {
        // L"存储箱已设置"
        wcscpy_s(g_hudLine2, L"\x5B58\x50A8\x7BB1\x5DF2\x8BBE\x7F6E");
    } else {
        // L"存储箱未设置"
        wcscpy_s(g_hudLine2, L"\x5B58\x50A8\x7BB1\x672A\x8BBE\x7F6E");
    }

    UpdateHudPosition();
    InvalidateRect(g_hudWindow, nullptr, TRUE);
    UpdateWindow(g_hudWindow);
    g_hudHideAt = GetTickCount64() + durationMs;
}

// 显示自定义 HUD 提示（两行文字 + 强调色）
static void ShowHudMessage(const wchar_t* line1, const wchar_t* line2,
                           COLORREF accent, DWORD durationMs) {
    if (!g_hudWindow && !InitHud()) return;
    if (line1) wcscpy_s(g_hudLine1, 64, line1); else g_hudLine1[0] = L'\0';
    if (line2) wcscpy_s(g_hudLine2, 64, line2); else g_hudLine2[0] = L'\0';
    g_hudAccent = accent;
    UpdateHudPosition();
    InvalidateRect(g_hudWindow, nullptr, TRUE);
    UpdateWindow(g_hudWindow);
    g_hudHideAt = GetTickCount64() + durationMs;
}

// 将热键 raw 描述转为宽字符串（用于 HUD 动态显示按键名）
// idx=0 → 第1个键（默认 8），idx=1 → 第2个键（默认 F4）
static const wchar_t* HudKeyName(int idx, const wchar_t* fallback) {
    static wchar_t buf[4][32];
    if (idx < 0 || idx > 3) return fallback;
    const char* raw = g_hotkeys.raw[idx];
    if (!raw || raw[0] == '\0') return fallback;
    // D-pad 等手柄键保留原样显示
    int n = MultiByteToWideChar(CP_UTF8, 0, raw, -1, buf[idx], 31);
    if (n > 0) return buf[idx];
    return fallback;
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
// AOB 扫描（从 aobscan.h 引入）
// ============================================================
#include "aobscan.h"


// ============================================================
// 输入处理
// ============================================================
static void PollInput() {
    // ---- 跨日检查（每 tick 执行，确保 Done/Paused 状态也能跨日恢复或保持）----
    if (G::chestSet) {
        CheckDayRollover();
    }

    // ---- F4: 设置/取消存储箱子（边沿触发）----
    static bool s_f4NeedsRelease = true;
    bool f4Down = (GetAsyncKeyState(QolHotKeysVk(&g_hotkeys, 0)) & 0x8000) != 0;
    bool f4Pressed = false;
    if (s_f4NeedsRelease) {
        if (!f4Down) s_f4NeedsRelease = false;
    } else if (f4Down) {
        f4Pressed = true;
        s_f4NeedsRelease = true;
    }
    if (f4Pressed) {
        HandleSetStorageChest();
    }

    // v1.6.2: 8 键已移除——F4 设箱=自动开始, F4 再按=取消+停止

    // ---- F9: 诊断触发（native_collect 实测，仅诊断版）----
    // v1.8.1: 发布版移除 F9 诊断触发器（#if 0 包裹，不参与编译）
#if 0
    static bool s_f9NeedsRelease = true;
    bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    bool f9Pressed = false;
    if (s_f9NeedsRelease) {
        if (!f9Down) s_f9NeedsRelease = false;
    } else if (f9Down) {
        f9Pressed = true;
        s_f9NeedsRelease = true;
    }
    if (f9Pressed) {
        Log("[AutoHarvest] [nc] F9 按下，触发 native_collect 诊断");
        ProbeNativeCollect();
    }
#endif

    // ---- 分块空间搜索（每帧只搜一个小块，单帧 DoSpatialSearch 开销恒定）----
    // v1.8.9: 由「单次全范围搜索+过滤分片」改为「搜索本身分块」——每帧执行一个
    //         小块的 DoSpatialSearch，块内候选少，单帧不卡；16 帧完成一轮全范围覆盖
    if (G::runMode == G::RunMode::AutoRun && !QolGameBusy()) {
        WorldContext ctx = {};
        if (GetWorldContext(&ctx)) {
            // 本轮未开始：判断是否到了启动新一轮的时间
            if (!g_shardSearch.active) {
                DWORD now = GetTickCount();
                bool shouldSearch = false;
                // v1.9.0: 玩家移动超过 1 格（200 世界单位）→ 立即重搜（进入可能有采集物的新区域）
                float dx = ctx.position[0] - g_shardSearch.lastScanX;
                float dy = ctx.position[1] - g_shardSearch.lastScanY;
                bool movedFar = g_shardSearch.lastScanTick != 0 &&
                    (dx * dx + dy * dy) > 200.0f * 200.0f;
                if (g_shardSearch.lastScanTick == 0) {
                    // v1.8.9: 首轮延迟 1 秒——避开 F4 按下帧
                    if (g_shardSearch.firstSearchDeferUntil == 0) {
                        g_shardSearch.firstSearchDeferUntil = now + 1000;
                    }
                    shouldSearch = now >= g_shardSearch.firstSearchDeferUntil;
                } else if (movedFar) {
                    // v1.9.5: 移动触发重搜限流 2000ms（玩家快速移动
                    // 时不再每帧触发全城 16 块搜索，卡顿消除）
                    shouldSearch = now - g_shardSearch.lastScanTick >= SHARD_MOVE_RETRY_MIN_MS;
                } else {
                    // 空/失败 60 秒重试，成功 30 秒常规间隔
                    const DWORD interval = g_shardSearch.lastResultEmpty
                        ? SHARD_RETRY_INTERVAL_MS
                        : SHARD_SCAN_INTERVAL_MS;
                    shouldSearch = now - g_shardSearch.lastScanTick >= interval;
                }
                if (shouldSearch) {
                    g_shardSearch.active = true;
                    g_shardSearch.gridIndex = 0;
                    g_shardSearch.foundCount = 0;
                    g_shardSearch.originX = ctx.position[0] - HARVEST_RADIUS;
                    g_shardSearch.originY = ctx.position[1] - HARVEST_RADIUS;
                    g_shardSearch.lastScanTick = now;
                    g_shardSearch.lastScanX = ctx.position[0];
                    g_shardSearch.lastScanY = ctx.position[1];
                    g_shardSearch.lastResultEmpty = true;  // 本轮找到任何提取器后置 false
                    Log("[AutoHarvest] 分块搜索启动: %d 块", SHARD_GRID_TOTAL);
                }
            }

            // 每帧只处理一个块
            if (g_shardSearch.active) {
                const int gi = g_shardSearch.gridIndex;
                if (gi < SHARD_GRID_TOTAL) {
                    const int col = gi % SHARD_GRID;
                    const int row = gi / SHARD_GRID;
                    const float cell = (2.0f * HARVEST_RADIUS) / (float)SHARD_GRID;
                    Rect bounds = {
                        g_shardSearch.originX + col * cell,
                        g_shardSearch.originY + row * cell,
                        g_shardSearch.originX + (col + 1) * cell,
                        g_shardSearch.originY + (row + 1) * cell,
                    };
                    g_shardSearch.results = {};
                    if (DoSpatialSearch(ctx.spatialIndex, &bounds, 0, &g_shardSearch.results) &&
                        g_shardSearch.results.begin && g_shardSearch.results.end) {
                        const size_t rawCount =
                            g_shardSearch.results.end - g_shardSearch.results.begin;
                        for (size_t i = 0;
                             i < rawCount && g_shardSearch.foundCount < 64; ++i) {
                            void* status = g_shardSearch.results.begin[i];
                            if (!status) continue;
                            if (!IsSapExtractorStatus(status)) continue;
                            float px, py;
                            if (!ReadPosition(status, &px, &py)) continue;
                            float dx = px - ctx.position[0];
                            float dy = py - ctx.position[1];
                            float distSq = dx * dx + dy * dy;
                            if (distSq > HARVEST_RADIUS_SQ) continue;
                            g_shardSearch.found[g_shardSearch.foundCount++] =
                                { status, px, py, distSq };
                        }
                        g_shardSearch.lastResultEmpty = false;
                    }
                    ReleaseSearchVector(&g_shardSearch.results);
                    g_shardSearch.results = {};
                    g_shardSearch.gridIndex++;
                }

                // 一轮 16 块完成：合并到注册表
                if (g_shardSearch.gridIndex >= SHARD_GRID_TOTAL) {
                    g_shardSearch.active = false;
                    Log("[AutoHarvest] 分块搜索完成: 找到 %zu 个提取器",
                        g_shardSearch.foundCount);

                    if (g_cachedExtractorCount == 0) {
                        // 首次注册
                        g_cachedExtractorCount = g_shardSearch.foundCount;
                        for (size_t k = 0; k < g_cachedExtractorCount; ++k) {
                            g_cachedExtractors[k] = g_shardSearch.found[k];
                        }
                        Log("[AutoHarvest] 首次注册 %zu 个提取器", g_cachedExtractorCount);
                    } else {
                        // 增量合并
                        size_t added = 0;
                        for (size_t i = 0; i < g_shardSearch.foundCount && g_cachedExtractorCount < 64; ++i) {
                            bool exists = false;
                            for (size_t j = 0; j < g_cachedExtractorCount; ++j) {
                                if (g_cachedExtractors[j].status == g_shardSearch.found[i].status) {
                                    exists = true; break;
                                }
                            }
                            if (!exists) {
                                g_cachedExtractors[g_cachedExtractorCount++] = g_shardSearch.found[i];
                                ++added;
                            }
                        }
                        if (added > 0) {
                            Log("[AutoHarvest] 增量检测：新增 %zu 个提取器（总 %zu）", added, g_cachedExtractorCount);
                        }
                    }
                }
            }
        }
    }

    // ---- 采集周期 ----
    // v1.8.7: 修复 DWORD 无符号溢出——原 now+2500 方案使 now-(now+2500) 下溢为巨大值立即触发
    if (G::runMode == G::RunMode::AutoRun && !QolGameBusy()) {
        DWORD now = GetTickCount();
        if (G::lastHarvestTick == 0) {
            // 首次：设置当前时间，本轮跳过（延迟到下个 5s 周期），与 ProductionAuto 错开
            G::lastHarvestTick = now;
        } else if (now - G::lastHarvestTick >= G::HARVEST_INTERVAL_MS) {
            G::lastHarvestTick = now;
            WorldContext ctx = {};
            if (GetWorldContext(&ctx)) {
                DoHarvestCycle(&ctx);
            }
        }
    }

    PumpHud();
}

// ============================================================
// 插件入口
// ============================================================
extern "C" __declspec(dllexport) void mod_init(void) {
    LogOpen("autoharvest");
    Log("[AutoHarvest] mod_init 开始");
    QolHotKeysInit(&g_hotkeys, "autoharvest");
    QolHotKeysSetDefault(&g_hotkeys, "F4");
    QolRegisterHotKey("autoharvest", "F4");

    G::base = (uintptr_t)GetModuleHandleW(nullptr);
    Log("[AutoHarvest] 游戏基址: 0x%llX", (unsigned long long)G::base);

    // AOB 扫描定位游戏函数
    uintptr_t addrs[kAOBCount] = {};
    for (size_t i = 0; i < kAOBCount; ++i) {
        addrs[i] = qol::ScanModuleAOB(nullptr, kAOBs[i].hex);
        Log("[AutoHarvest] AOB %-20s => 0x%p %s",
            kAOBs[i].name, (void*)addrs[i], addrs[i] ? "(OK)" : "(MISS)");
    }

    // 绑定函数指针
    if (addrs[IDX_SPATIAL_SEARCH])    G::spatialSearch    = (FnSpatialSearch)addrs[IDX_SPATIAL_SEARCH];
    if (addrs[IDX_RAW_VECTOR_FREE])    G::rawVectorFree    = (FnRawVectorFree)addrs[IDX_RAW_VECTOR_FREE];
    if (addrs[IDX_INTRUSIVE_RELEASE])  G::intrusiveRelease = (FnIntrusiveRelease)addrs[IDX_INTRUSIVE_RELEASE];
    if (addrs[IDX_ITEM_ADJUST])        G::itemAdjust       = (FnItemAdjust)addrs[IDX_ITEM_ADJUST];
    if (addrs[IDX_PLAYER_CAPACITY])    G::playerCapacity   = (FnPlayerCapacity)addrs[IDX_PLAYER_CAPACITY];
    if (addrs[IDX_BATCH_DIRTY])        G::batchDirty       = (FnBatchDirty)addrs[IDX_BATCH_DIRTY];

    // 硬编码 RVA（与 ChestSort 一致）
    G::rvaGameRoot = RVA_GAME_ROOT;
    G::rvaWorldRegistry = WORLD_OBJECT_REGISTRY_RVA;
    G::rvaCallbackVTable = SEARCH_CALLBACK_VTABLE_RVA;
    G::batchRecalc = (FnBatchRecalc)(G::base + RVA_BATCH_RECALCULATE);
    G::batchUIRefresh = (FnBatchUIRefresh)(G::base + RVA_BATCH_UI_REFRESH);

    // 加载 command_capacity AOB（从 ChestSort 复制）
    // v1.09 改 28 字节截断版（避开 E8 rel32，唯一匹配 0x4CB6B0），旧 65 字节完整签名含 E8 rel32 失配。
    static const AOBDef commandCapacityAOB = {
        "command_capacity",
        "40 53 48 83 EC 20 48 8B D9 48 8B 09 48 85 C9 75 0B B8 E7 03 00 00 48 83 C4 20 5B C3"
    };
    uintptr_t ccAddr = qol::ScanModuleAOB(nullptr, commandCapacityAOB.hex);
    if (ccAddr) {
        G::commandCapacity = (FnCommandCapacity)ccAddr;
        Log("[AutoHarvest] AOB command_capacity => 0x%p (OK)", (void*)ccAddr);
    }

    // ---- 树液提取器生产函数绑定（RVA 直连，v1.20 build 25311578 实测值）----
    // v1.9.6 M8: 绑定前 IsReadable 验证——防止版本更新后 RVA 漂移导致崩溃
    // 验证失败的 RVA 置 nullptr（禁用对应功能），不崩溃
    auto SapBindIfReadable = [](uintptr_t rva, const char* name) -> uintptr_t {
        uintptr_t addr = G::base + rva;
        if (!IsReadable((const void*)addr, 16)) {
            Log("[AutoHarvest] [FATAL] sap RVA %s=0x%X 不可读，置 nullptr 禁用", name, (unsigned)rva);
            return 0;
        }
        return addr;
    };

    G::sapOutputHelper      = (FnSapOutputHelper)(SapBindIfReadable(RVA_SAP_OUTPUT_HELPER, "sapOutputHelper"));
    G::sapStatusIntLookup   = (FnSapStatusMapLookup)(SapBindIfReadable(RVA_SAP_STATUS_INT_LOOKUP, "sapStatusIntLookup"));
    G::sapStatusU64Lookup   = (FnSapStatusMapLookup)(SapBindIfReadable(RVA_SAP_STATUS_U64_LOOKUP, "sapStatusU64Lookup"));
    G::sapStatusEventLookup = (FnSapStatusMapLookup)(SapBindIfReadable(RVA_SAP_STATUS_EVENT_LOOKUP, "sapStatusEventLookup"));
    G::sapStatusEventNotify = (FnSapStatusEventNotify)(SapBindIfReadable(RVA_SAP_STATUS_EVENT_NOTIFY, "sapStatusEventNotify"));
    G::sapItemAllocate      = (FnSapItemAllocate)(SapBindIfReadable(RVA_SAP_ITEM_ALLOCATE, "sapItemAllocate"));
    G::sapItemCtor          = (FnSapItemCtor)(SapBindIfReadable(RVA_SAP_ITEM_CTOR, "sapItemCtor"));
    G::sapItemSetCount      = (FnSapItemSetCount)(SapBindIfReadable(RVA_SAP_ITEM_SET_COUNT, "sapItemSetCount"));
    G::sapItemIsStackable   = (FnSapItemIsStackable)(SapBindIfReadable(RVA_SAP_ITEM_IS_STACKABLE, "sapItemIsStackable"));
    G::sapNativeCollect     = (FnSapNativeCollect)(SapBindIfReadable(RVA_SAP_NATIVE_COLLECT, "sapNativeCollect"));
#if DIAG_AUTOHARVEST
    Log("[AutoHarvest] [diag] native_collect 绑定 %p", (void*)G::sapNativeCollect);
#endif
        G::sapFunctionsReady = G::sapItemAllocate && G::sapItemCtor &&
                               G::sapItemSetCount && G::sapStatusEventNotify &&
                               G::sapStatusIntLookup && G::sapStatusU64Lookup;
    if (G::sapFunctionsReady) {
        Log("[AutoHarvest] sap 生产函数绑定成功 (itemAllocate=%p itemCtor=%p itemSetCount=%p statusEventNotify=%p statusLookup=%p/%p)",
            (void*)G::sapItemAllocate, (void*)G::sapItemCtor,
            (void*)G::sapItemSetCount, (void*)G::sapStatusEventNotify,
            (void*)G::sapStatusIntLookup, (void*)G::sapStatusU64Lookup);
    } else {
        Log("[AutoHarvest] [警告] sap 生产函数绑定失败，树液收集不可用");
    }

    // 检查关键函数
    int missing = 0;
    if (!G::spatialSearch)    missing++;
    if (!G::rawVectorFree)    missing++;
    if (!G::itemAdjust)       missing++;
    if (missing > 0) {
        Log("[AutoHarvest] [警告] %d 个关键函数未定位，MOD 不可用", missing);
    } else {
        G::ready = true;
    }
    Log("[AutoHarvest] mod_init 完成 (ready=%d sap=%d)",
        G::ready ? 1 : 0, G::sapFunctionsReady ? 1 : 0);

}

extern "C" __declspec(dllexport) void mod_tick(void) {
    qol::budget::BeginFrame();  // P0: 帧锚定（幂等，多 DLL 安全）
    static int s_bdgSlot = -1;  // P0 探针（惰性注册，析构自动上报）
    if (s_bdgSlot < 0) s_bdgSlot = qol::budget::Slot("AutoHarvest");
    struct BdgGuard {
        int slot; uint64_t t0;
        ~BdgGuard() { qol::budget::Report(slot, qol::budget::NowUs() - t0); }
    } bdgGuard = { s_bdgSlot, qol::budget::NowUs() };

    if (!G::ready) return;
    QolHotkeyCheckReload(&g_hotkeys);
    PollInput();
    QolHudGuardVisibility(g_hudWindow);  // v1.9.8: 失焦隐藏 HUD（不飘桌面）
}

extern "C" __declspec(dllexport) void unload(void) {
    Log("[AutoHarvest] unload");
    LogClose();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)lpReserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_autoharvestModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
