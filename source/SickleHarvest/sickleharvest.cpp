// sickleharvest.cpp — SickleHarvest MOD for Village in the Shade v1.20 (v2.3.5 正式版)
// 镰刀范围收割：挥镰刀时一次性收割范围内所有成熟田地作物 + 果树
// v2.3.3: v1.09→v1.20 (build 25311578) 适配——16 个 RVA 全部重定位。
//         SHAKE_TREE_SETTLE 序言变化（新增 mov [rax+0x10],rdx 影子栈保存，
//         0x19D320→0x1A7A40，经 0x514 参数锚点投票 6/6 + REFRESH 邻接 0x4740 互证）；
//         CROPS_ACTION_CALLSITE rel32 实读更新（目标 0xEEEB0=新 ACTION_CHECK，
//         CALLSITE 链语义验证）；其余 10 个函数序言新旧完全一致。
// v2.3.0: 代码审计修复——①HarvestSettle 循环加 SEH 保护（崩溃时恢复 savedTarget）；
//         ②FastRegionReset 提前到 native 调用之前（detour 期间用干净缓存）；
//         ③HarvestSettle 加 8ms 帧预算（大农场 100+ 作物时不卡顿，超出部分跳过）。
// v2.2.2: 去重从 O(N²) 线性扫描改为 unordered_set 哈希集合
//         约 10M 次比较 → O(1) 查找）；移除 item_ctor / HarvestSettle 诊断
//         被动记录 hook（v2.2.0 已验证转正，收割逻辑不再需要诊断捕获）。
// v1.1.1: 修正 getTargetCrops RVA 0x23B110→0x23B710（原地址为辅助函数，真正函数在 +0x600 处）
// v1.1.2: 修正 ShakeTreeSettle 调用参数——arg5 从 0.0f 改为从 [comp+0x260] 读取 float，
//         arg7 从 3 改为 2，对齐 v1.09 原生 callsite (0x1A3C37 / 0x1A4A81)
// v1.2.0-diag: 增强 CropsActionCheckDetour 日志 + unresolved 结构 dump
// v1.2.1-diag: 新增 GimmickStatus 侦查扫描——挥镰刀时用 spatialSearch 扫描周围
//              所有 GimmickStatus 对象，dump moduleName + action 接受矩阵（纯只读）
// v1.2.2-diag: 深度诊断增强——对空名 gimmick 读取 RTTI 类类型名（vtable[-1]→COL→
//              TypeDescriptor→name）、dump vtable 前 16 个函数指针、hex dump
//              对象前 0x400 字节 + data holder 前 0x100 字节（纯只读，不改游戏状态）
// v1.2.3-diag: 修复空字符串未触发深度dump + 多偏移vtable探测(0x00~0x30) +
//              指针扫描全范围 + 有名gimmick对比dump
// v1.2.4-diag: 在 ScoutGimmicks 中对每个 gimmick 调用原生函数
//              GimmickIsHarvestItem(RVA 0x47E4B0) 和 GimmickGetHarvestItemID(RVA 0x47E680)
//              测试木耳是否被原生判定为可收获，记录返回值（纯只读，__try包裹防崩溃）
// v1.2.5-diag: spatialSearch SEH 修复 A/B——恢复 v1.2.3 内联 __try 形式（v1.2.4 独立 NOINLINE
//              wrapper 下 4 次挥刀全部 spatialSearch SEH 异常；v1.2.3 同位置成功）。
//              同时 dump GetExceptionCode 便于定位崩溃点。
// v1.2.6-diag: A1+A2 被动 hook 监听——命令处理器全部 0 命中（已证死代码）
// v1.2.7-diag: hook CState_Player_Retrieve::searchNextTarget(slot[26]=0x2364C0)
//              手动采集木耳实测 0 命中——该函数无直接调用者，非采集主路径
// v1.2.8-diag: hook CState_Player_Retrieve::update(slot[27]=0x2365D0) 主循环
//              用 RtlCaptureStackBackTrace 抓调用链，定位手动采集的真实入口
//              假设：update 是状态机主循环，玩家处于 Retrieve 状态必然每帧调用
//              实测结果：0 命中——手动采集不走 Retrieve 状态机
// v1.3.0-diag: 移除 Retrieve hook（诊断完成）。增强 ScoutGimmicks——
//              对每个 gimmick 读取 baseID(data+0x00)、CCom_Crops 组件(+0x250)是否存在、
//              status map(0x18/0x58) 内容、dump 前 0x400 字节 + data holder 0x200 字节。
//              目标：定位木耳为何未出现在 GetSickleTargetCrops 返回列表中、
//              确认 gimmick 数据结构（产出物 ID、可收获标志位）。
//              移除 thread_local dump 限制，每次挥镰刀 dump 前 8 个非作物 gimmick。
//              实测结论：三次挥刀（家附近23 gimmick/树液区4-5个）均无木耳——
//              木耳不在 spatialSearch 空间索引中，ScoutGimmicks 路径对木耳无效；
//              +0x250 的 CCom_Crops 检查不可靠（所有 gimmick 返回同一共享 vtable 指针）。
// v1.4.0-diag: hook native_collect（RVA 0x271560）入口被动记录——
//              记录 baseID(rcx) + status(rdx) + status 结构摘要，
//              确认手动采集木耳是否走 native_collect 路径（TLS 依赖，不可直接调用）。
//              移除 ScoutGimmicks 调用（木耳不在 spatialSearch 索引，该路径无效）。
//              实测结论：手动采集木耳 native_collect 0 命中——该函数是生产机器收集
//              函数（AutoHarvest 树液提取器用），非玩家手动采集路径。
// v1.5.0-diag: hook CItemStatus 构造（RVA 0xFF870）——任何物品产生（含手动采集
// 木耳入包）必经此构造，r8=itemId 直接给出物品 ID。配合
//              RtlCaptureStackBackTrace 抓调用栈转 RVA，一次性还原"采集木耳
//              到底调用了什么函数链"。与 AutoHarvest 已验证的物化链同源，风险最低。
//              实测结论：手动采集木耳 itemId=0x187E0 命中，调用链
//              HarvestSettle(0x211DD0)->ShakeTreeSettle(0x19D320)->item_ctor(0xFF870)，
//              与反汇编一致。另：挥镰刀时原生 getTargetCrops 返回空列表
//              （begin=end=cap=0），但内部 action_check 被调用 2805+ 次，
//              全部 SICKLE 失败——说明候选对象大量存在但被原生过滤，且
//              fallback 机制因 result 为空而无法生效（候选永远进不了 result）。
// v1.6.0-diag: action_check 候选 baseID 诊断——在 CropsActionCheckDetour 中打印
//              status 的 base 字段（+0x00/+0x18 landId），确认挥镰刀候选对象里
//              是否有木耳（0x187E0）；统计 SHAKE_TREE/HARVEST fallback 接受数，
//              并在 getTargetCrops result 为空时打印 FALLBACK-BUT-EMPTY 警告，
//              验证"候选被过滤 + fallback 无法写入空 result"这一核心机制。
//              实测结论：挥镰刀时原生 getTargetCrops 返回 5~6 个候选（成熟作物），
//              SICKLE 接受 5 个（native=5），其余被拒。发现一个反复出现的
//              SICKLE-failed 对象 cropsStatus=0x3BFB16A0（每帧都在候选列表，
//              三种 action 全拒）——极可能是木耳。另确认 status+0x00 是 vtable
//              指针不是 baseID（所有对象相同），baseID 在 0xE5980 栈参数里。
// v1.7.0-diag: 针对 SICKLE-failed 对象 dump RTTI 类名（确认是否木耳），
//              SHAKE_TREE 失败时也打印；action_check 常规日志改为每 128 次
//              打印一次防刷屏。目标：确认 0x3BFB16A0 身份 + 木耳类名。
//              实测结论：RTTI 链读取失败（rtti unavailable）——MSVC RTTI 链
//              vtable[-1] 处不是 COL 或该对象无 RTTI，改用重指纹识别。
// v1.8.0-diag: RTTI 链不可用，改 dump vtable RVA + vtable 前 6 个函数指针
//              RVA（函数指纹）+ landId + hexdump 前 0x40 字节，与原生接受
//              对象对比（是否同 vtable）。按 vtable 去重防刷屏。
//              实测结论：指纹识别成功（原生接受 vtable RVA=0xE05C00），
//              但用户在木耳旁挥刀 2 次，候选列表始终只有 5 个成熟作物，
//              无任何 SICKLE-failed——木耳根本不在原生镰刀候选列表里。
//              过滤层放行思路前提不成立，需换方向。
// v1.9.0-diag: hook HarvestSettle(0x211DD0) 入口被动记录参数+调用栈。
//              目标：手动采集木耳时拿到 HarvestSettle 的 landID/身份参数，
//              挥镰刀时主动把该 landID 加入收割列表（现有架构已支持：
//              设置 actionTarget=landID 后调用 g_harvestSettle）。
// v2.2.0-diag: 修复 v2.0.0 重复产出问题——MOD 收割的目标不写回列表，避免原生重复处理。
//              v2.1.0 实测确认：纯原生放行时木耳收割不了，证明木耳必须由 MOD 执行 HarvestSettle。
//              逻辑：IsHarvestableNow 通过 → MOD 收割不写回；未通过/拿不到 landID → 写回交原生。
// v2.2.2: 修复已验证转正，item_ctor / HarvestSettle 诊断 hook 已移除。

// 参考权威源码 dinput8.cpp L2582-3270，RVA 已按 v1.20 (build 25311578) 重新定位验证
// 作者：PHJ&消失的清风，转载或分享时请注明出处。

#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <unordered_set>
#include <vector>

#include "logging.h"
#include "budget.h"        // P0 帧耗时探针（跨 DLL 共享）
#include "state.h"         // P1-2 状态感知暂停（载入/菜单静默）
#include "hot_config.h"
#include "safe_call.h"
#include "patch_safety.h"

// 日志开关：发布版禁用日志输出
// 调试时取消注释下行即可开启日志
// #define SICKLEHARVEST_LOGGING   // v2.3.7 转正：日志关闭（定位时取消注释重编）
// #define SICKLEHARVEST_LOGGING
#ifdef SICKLEHARVEST_LOGGING
  // 使用 QoL_Shared 的日志系统
#else
  #define LogOpen(x)  ((void)0)
  #define Log(...)    ((void)0)
  #define LogV(...)   ((void)0)
  #define LogClose()  ((void)0)
#endif

// [diag] 诊断日志门控（默认关）；调试时 #define DIAG_SICKLEHARVEST 1 开启
#ifndef DIAG_SICKLEHARVEST
#define DIAG_SICKLEHARVEST 0
#endif

// ---- 版本与 RVA 常量（v1.20, build 25311578）----------------------------
// exe size=18305544, SHA-256=AC7D955BAA915929B185DF85CFA13F7E81AD57A9C68F308DA769739775869A05 (v1.20 build 25311578)

static volatile uintptr_t RVA_SICKLE_GET_TARGET_CROPS   = 0x247F10;
static volatile uintptr_t RVA_CROPS_ACTION_CHECK        = 0x0EEEB0;
static volatile uintptr_t RVA_CROPS_GET_CURRENT_FORM    = 0x0EE790;
static volatile uintptr_t RVA_HARVEST_SEARCH            = 0x21D960;
static volatile uintptr_t RVA_HARVEST_SETTLE            = 0x21D380;
static volatile uintptr_t RVA_CROPS_ACTION_CALLSITE     = 0x2481C2;
static volatile uintptr_t RVA_SHAKE_TREE_SETTLE         = 0x1A7A40;
static volatile uintptr_t RVA_SHAKE_TREE_REMAINING      = 0x0EF470;
static volatile uintptr_t RVA_SHAKE_RAW_VECTOR_FREE     = 0x0B8720;
static volatile uintptr_t RVA_SHAKE_TREE_REFRESH        = 0x1A3300;
static volatile uintptr_t RVA_SHAKE_TREE_EVENT          = 0x7600C0;
// v1.8.0-diag: RTTI 链不可用，改用模块基址 + vtable RVA 指纹识别
// v2.2.2: RVA_ITEM_CTOR 已随 item_ctor 诊断 hook 一并移除
static volatile uintptr_t MODULE_IMAGE_SIZE             = 0x5000000; // 模块映射大小（vtable 指纹识别用）
// v1.3.0-diag: Retrieve hook 已移除（诊断完成，0 命中已证手动采集不走该状态机）

// 结构偏移
static volatile uintptr_t COM_CROPS_STATUS_OFFSET       = 0x250;
static volatile uintptr_t CROPS_LAND_ID_OFFSET          = 0x18;
static volatile uintptr_t CROPS_IS_HARVESTABLE_OFFSET   = 0x78;
static volatile uintptr_t CROPS_CHANGE_FORM_OFFSET      = 0x80;
static volatile uintptr_t CROPS_IS_BIG_FORM_OFFSET      = 0x98;
static volatile uintptr_t PLAYER_ACTION_TARGET_OFFSET   = 0x248;

// hook 长度
static constexpr size_t SICKLE_HOOK_LENGTH         = 16;
static constexpr size_t ACTION_CHECK_HOOK_LENGTH   = 14;
// v1.2.6-diag: A1+A2 hook 长度（均为完整指令边界，不拆分指令）


// action 常量
static constexpr uint64_t ACTION_SICKLE       = 0x410;
static constexpr uint64_t ACTION_HARVEST      = 0x42E;
static constexpr uint64_t ACTION_SHAKE_TREE   = 0x514;

// 摇树上限
static constexpr size_t SICKLE_SHAKE_TREE_MAX_TARGETS = 256;
static constexpr size_t SICKLE_SHAKE_TREE_MAX_SHAKES  = 64;

// ---- 空间搜索常量（从 ChestSort/AutoHarvest 移植，v1.09 已验证）----------------
static volatile uintptr_t RVA_GAME_ROOT              = 0x10FCBB0;
static volatile uintptr_t WORLD_OBJECT_REGISTRY_RVA  = 0x1104C80;
static volatile uintptr_t SEARCH_CALLBACK_VTABLE_RVA = 0xE3E608;
static volatile uintptr_t RVA_SPATIAL_SEARCH          = 0x194060;
static volatile uintptr_t RVA_RAW_VECTOR_FREE_GEN    = 0x0B8720;

// 指针链偏移（与 ChestSort / AutoHarvest 一致）
static volatile uintptr_t ROOT_PLAYER_OFFSET         = 0x208;
static volatile uintptr_t ROOT_MAP_OWNER_OFFSET       = 0x268;
static volatile uintptr_t MAP_INFO_OFFSET             = 0x0d8;
static volatile uintptr_t MAP_SPATIAL_OWNER_OFFSET    = 0x030;
static volatile uintptr_t MAP_SPATIAL_INDEX_OFFSET    = 0x6e0;
static volatile uintptr_t PLAYER_OBJECT_STATUS_OFFSET = 0x32b8;

// Gimmick 偏移（与 ChestSort / AutoHarvest 一致）
static volatile uintptr_t GIMMICK_DATA_HOLDER_OFFSET  = 0x240;
static volatile uintptr_t GIMMICK_MODULE_NAME_OFFSET  = 0x0E0;  // v1.20: 0xD8→0xE0
static volatile uintptr_t GIMMICK_POSITION_OFFSET     = 0x0f0;

// 侦查搜索半径
static constexpr float SCOUT_RADIUS = 500.0f;

// ---- 类型定义 ---------------------------------------------------------------

using u8  = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

struct RawPointerVector {
    void** begin;
    void** end;
    void** capacity;
};

using GetSickleTargetCropsFunction   = RawPointerVector* (__fastcall *)(void*, RawPointerVector*);
using CropsActionCheckFunction       = bool (__fastcall *)(void*, void*, u64);
using GetCurrentCropsFormFunction    = void* (__fastcall *)(void*);
using HarvestSearchFunction          = void** (__fastcall *)(void*, void**, u64);
using HarvestSettleFunction          = void (__fastcall *)(void*);
using ShakeTreeRemainingFunction    = unsigned short (__fastcall *)(void*);
using ShakeTreeSettleFunction       = void (__fastcall *)(void*, RawPointerVector*, u64, void*, float, int, unsigned short);
using ShakeRawVectorFreeFunction    = void (__fastcall *)(void*, size_t);
using ShakeTreeRefreshFunction      = void (__fastcall *)(void*);
using ShakeTreeEventFunction        = void (__fastcall *)(void*, int, unsigned short);

// v1.3.0-diag: Retrieve hook 已移除
// ---- 空间搜索类型（从 ChestSort 移植）---------------------------------------
struct Rect { float minX, minY, maxX, maxY; };

struct SearchCallback {
    alignas(16) unsigned char storage[0x38];
    void* target;
};

using FnSpatialSearch = bool(__fastcall*)(void*, const Rect*, void*, int, int);
using FnRawVectorFreeGen = void(__fastcall*)(void*, size_t);

// ---- 全局状态 ---------------------------------------------------------------

static GetSickleTargetCropsFunction   g_originalGetSickleTargetCrops = nullptr;
static CropsActionCheckFunction       g_originalCropsActionCheck = nullptr;
static GetCurrentCropsFormFunction    g_getCurrentCropsForm = nullptr;
static HarvestSearchFunction          g_harvestSearch = nullptr;
static HarvestSettleFunction          g_harvestSettle = nullptr;
static ShakeTreeRemainingFunction    g_shakeTreeRemaining = nullptr;
static ShakeTreeSettleFunction       g_shakeTreeSettle = nullptr;
static ShakeRawVectorFreeFunction    g_shakeRawVectorFree = nullptr;
static ShakeTreeRefreshFunction      g_shakeTreeRefresh = nullptr;
static ShakeTreeEventFunction       g_shakeTreeEvent = nullptr;


// v1.3.0-diag: Retrieve hook 已移除

static std::atomic<bool> g_sickleHarvestEnabled{false};
static bool g_sickleHarvestHookReady = false;
static bool g_shakeTreeHarvestReady = false;
static bool g_shakeTreeVisualReady = false;
// H3: 保存安装时的原始字节，供 unload 还原 hook
static unsigned char g_savedActionCheckBytes[ACTION_CHECK_HOOK_LENGTH] = {};
static unsigned char g_savedSickleHookBytes[SICKLE_HOOK_LENGTH] = {};
// ---- 空间搜索全局状态（侦查诊断用）-------------------------------------------
static FnSpatialSearch     g_spatialSearch = nullptr;
static FnRawVectorFreeGen  g_rawVectorFreeGen = nullptr;
static uintptr_t           g_searchCallbackVTable = 0;

static thread_local bool g_insideSickleHarvest = false;
static thread_local bool g_insideSickleTargetQuery = false;
static thread_local bool g_insideShakeTreeSettlement = false;

static thread_local unsigned g_addedHarvestTargets = 0;
static thread_local unsigned g_addedShakeTreeTargets = 0;
static thread_local void* g_harvestFallbackStatuses[256] = {};
static thread_local size_t g_harvestFallbackStatusCount = 0;
static thread_local void* g_shakeTreeFallbackStatuses[256] = {};
static thread_local size_t g_shakeTreeFallbackStatusCount = 0;

// ---- 内存工具 ---------------------------------------------------------------

// VirtualQuery 区域缓存：同区域多次访问只查一次系统调用。
// 挥镰刀时 3250+ 目标每个 3+ 次 VirtualQuery，缓存后降至 ~16 次。
struct MemRegion {
    uintptr_t start;
    uintptr_t end;
};
static MemRegion g_fastRegions[16];
static int g_fastRegionCount = 0;

static void FastRegionReset() {
    g_fastRegionCount = 0;
}

static bool IsReadable(const void* pointer, size_t size) {
    if (!pointer || size == 0) return false;
    uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    if (start < 0x10000 || start > 0x00007FFFFFFFFFFFULL) return false;
    // 命中缓存区域
    for (int i = 0; i < g_fastRegionCount; ++i) {
        if (start >= g_fastRegions[i].start &&
            start + size <= g_fastRegions[i].end) return true;
    }
    // 未命中 → 真实 VirtualQuery
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(pointer, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t regionEnd = regionStart + mbi.RegionSize;
    if (start + size < start || start + size > regionEnd) return false;
    // 记录该区域
    if (g_fastRegionCount < 16) {
        g_fastRegions[g_fastRegionCount] = { regionStart, regionEnd };
        g_fastRegionCount++;
    }
    return true;
}

static bool WriteMem(void* target, const void* data, size_t size) {
    return qol::WritePatchChecked(target, data, size);
}

static bool SealExecutableMemory(void* address, size_t size) {
    DWORD previousProtection = 0;
    if (!address || size == 0) return false;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READ, &previousProtection)) return false;
    return FlushInstructionCache(GetCurrentProcess(), address, size) != 0;
}

// ---- RTTI 辅助（v1.2.2-diag 深度诊断用）----------------------------------
// MSVC RTTI 链: object -> vtable[0] -> vtable[-1] (COL*) -> TypeDescriptor -> name

static void* GetVTable(void* obj) {
    if (!obj || !IsReadable(obj, sizeof(void*))) return nullptr;
    return *reinterpret_cast<void**>(obj);
}

static void* GetCOL(void* vtable) {
    if (!vtable) return nullptr;
    void** vt = reinterpret_cast<void**>(vtable);
    if (!IsReadable(vt - 1, sizeof(void*))) return nullptr;
    return *(vt - 1);
}

static void* GetTypeDescriptor(void* col) {
    if (!col || !IsReadable(col, 0x18)) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(col) + 0x10);
}

static bool ReadRTTIName(void* obj, char* buf, size_t bufSize) {
    if (!obj || !buf || bufSize < 16) return false;
    void* vtable = GetVTable(obj);
    if (!vtable) return false;
    void* col = GetCOL(vtable);
    if (!col) return false;
    void* td = GetTypeDescriptor(col);
    if (!td || !IsReadable(td, 0x18)) return false;
    const char* name = reinterpret_cast<const char*>(
        reinterpret_cast<uintptr_t>(td) + 0x10);
    if (!IsReadable(name, 16)) return false;
    size_t len = 0;
    while (len + 1 < bufSize && name[len] != '\0') ++len;
    if (name[len] != '\0') return false;
    memcpy(buf, name, len + 1);
    return true;
}

static void DumpVTable(void* obj) {
    if (!obj) return;
    void* vtable = GetVTable(obj);
    if (!vtable) {
        Log("[SickleHarvest][deep] vtable=NULL\n");
        return;
    }
    void** vt = reinterpret_cast<void**>(vtable);
    Log("[SickleHarvest][deep] vtable=%p\n", vtable);
    for (int i = 0; i < 16; ++i) {
        if (!IsReadable(vt + i, sizeof(void*))) break;
        Log("[SickleHarvest][deep] vt[%d]=%p\n", i, vt[i]);
    }
}

static void HexDump(const char* tag, void* addr, size_t size) {
    if (!addr || size == 0) return;
    uintptr_t base = reinterpret_cast<uintptr_t>(addr);
    for (size_t off = 0; off < size; off += 16) {
        if (!IsReadable(reinterpret_cast<void*>(base + off),
                        (off + 16 <= size) ? 16 : (size - off))) break;
        char hex[128] = {};
        char ascii[32] = {};
        size_t hexPos = 0;
        size_t maxCol = (off + 16 <= size) ? 16 : (size - off);
        for (size_t j = 0; j < maxCol; ++j) {
            unsigned char b = *reinterpret_cast<unsigned char*>(base + off + j);
            hexPos += snprintf(hex + hexPos, sizeof(hex) - hexPos, "%02X ", b);
            ascii[j] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
        }
        ascii[maxCol] = '\0';
        Log("[SickleHarvest][deep] %s +0x%03zX: %-48s |%s|\n", tag, off, hex, ascii);
    }
}

// ---- v1.8.0-diag: 重指纹识别辅助（RTTI 不可用时的替代方案）------------------
// 原理：每个 C++ 类实例的第一个成员是 vtable 指针，vtable 位于模块 .rdata，
//       因此 vtable 的模块内 RVA 唯一标识一个类（同类共享同一 vtable）。
//       再 dump vtable 前几个函数指针 RVA 作为指纹，与原生接受的作物对比。

static bool ModuleRva(void* address, uintptr_t* outRva) {
    if (!outRva) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
    if (addr < base || addr >= base + MODULE_IMAGE_SIZE) return false;
    *outRva = addr - base;
    return true;
}

// 打印对象身份指纹：vtable RVA + 前 6 个函数 RVA + 字符串 + hexdump
static void DumpIdentityObject(void* cropsStatus, const char* tag) {
    if (!cropsStatus) return;
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    Log("[SickleHarvest] IDENTITY %s cropsStatus=%p\n", tag, cropsStatus);

    void* vtable = GetVTable(cropsStatus);
    if (!vtable) {
        Log("[SickleHarvest] ID %s vtable=NULL\n", tag);
    } else {
        uintptr_t vtRva = 0;
        if (ModuleRva(vtable, &vtRva)) {
            Log("[SickleHarvest] ID %s vtable=%p (RVA 0x%zX)\n", tag, vtable, (size_t)vtRva);
        } else {
            Log("[SickleHarvest] ID %s vtable=%p (outside module)\n", tag, vtable);
        }
        // vtable 前 6 个函数指针 RVA（指纹）
        void** vt = reinterpret_cast<void**>(vtable);
        for (int i = 0; i < 6; ++i) {
            if (!IsReadable(vt + i, sizeof(void*))) break;
            uintptr_t fnRva = 0;
            if (ModuleRva(vt[i], &fnRva)) {
                Log("[SickleHarvest] ID %s vt[%d]=RVA 0x%zX\n", tag, i, (size_t)fnRva);
            } else {
                Log("[SickleHarvest] ID %s vt[%d]=0x%p (non-module)\n", tag, i, vt[i]);
            }
        }
    }
    // landId + moduleId（+0x0d8 若可读）
    if (IsReadable(cropsStatus, CROPS_LAND_ID_OFFSET + sizeof(u64))) {
        u64 landId = *reinterpret_cast<const u64*>(
            reinterpret_cast<uintptr_t>(cropsStatus) + CROPS_LAND_ID_OFFSET);
        Log("[SickleHarvest] ID %s landId=0x%llX\n", tag, (unsigned long long)landId);
    }
    void* moduleName = nullptr;
    if (IsReadable(cropsStatus, GIMMICK_MODULE_NAME_OFFSET + sizeof(void*))) {
        moduleName = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(cropsStatus) + GIMMICK_MODULE_NAME_OFFSET);
        if (moduleName && IsReadable(moduleName, 8)) {
            const char* name = reinterpret_cast<const char*>(moduleName);
            if (IsReadable(name, 32)) {
                char buf[32] = {};
                memcpy(buf, name, 31);
                Log("[SickleHarvest] ID %s moduleName='%s'\n", tag, buf);
            }
        }
    }
    // hexdump 前 0x40 字节（单行）
    if (IsReadable(cropsStatus, 0x40)) {
        unsigned char* p = reinterpret_cast<unsigned char*>(cropsStatus);
        char hexBuf[256] = {};
        size_t pos = 0;
        for (int i = 0; i < 0x40; ++i) {
            pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02X ", p[i]);
        }
        Log("[SickleHarvest] ID %s hex: %s\n", tag, hexBuf);
    }
}

// 记录一个已 dump 过的 vtable RVA（按 vtable 去重，防每帧重复刷屏）
static thread_local uintptr_t s_dumpedVtableRvas[64] = {};
static thread_local int s_dumpedVtableCount = 0;

static bool AlreadyDumpedVtable(uintptr_t vtRva) {
    if (vtRva == 0) return false;
    for (int i = 0; i < s_dumpedVtableCount; ++i) {
        if (s_dumpedVtableRvas[i] == vtRva) return true;
    }
    return false;
}

static void RememberDumpedVtable(uintptr_t vtRva) {
    if (vtRva == 0 || s_dumpedVtableCount >= 64) return;
    if (AlreadyDumpedVtable(vtRva)) return;
    s_dumpedVtableRvas[s_dumpedVtableCount++] = vtRva;
}

// ---- 作物状态辅助 -----------------------------------------------------------

static void* TryGetCropsStatus(void* cropsComponent) {
    if (!IsReadable(cropsComponent, COM_CROPS_STATUS_OFFSET + sizeof(void*))) return nullptr;
    void* status = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(cropsComponent) + COM_CROPS_STATUS_OFFSET);
    return IsReadable(status, CROPS_LAND_ID_OFFSET + sizeof(u64)) ? status : nullptr;
}

static void RememberHarvestFallbackStatus(void* cropsStatus) {
    if (!cropsStatus) return;
    for (size_t i = 0; i < g_harvestFallbackStatusCount; ++i) {
        if (g_harvestFallbackStatuses[i] == cropsStatus) return;
    }
    if (g_harvestFallbackStatusCount < 256) {
        g_harvestFallbackStatuses[g_harvestFallbackStatusCount++] = cropsStatus;
    }
}

static bool WasAddedByHarvestFallback(void* cropsComponent) {
    void* status = TryGetCropsStatus(cropsComponent);
    if (!status) return false;
    for (size_t i = 0; i < g_harvestFallbackStatusCount; ++i) {
        if (g_harvestFallbackStatuses[i] == status) return true;
    }
    return false;
}

static void RememberShakeTreeFallbackStatus(void* cropsStatus) {
    if (!cropsStatus) return;
    for (size_t i = 0; i < g_shakeTreeFallbackStatusCount; ++i) {
        if (g_shakeTreeFallbackStatuses[i] == cropsStatus) return;
    }
    if (g_shakeTreeFallbackStatusCount < 256) {
        g_shakeTreeFallbackStatuses[g_shakeTreeFallbackStatusCount++] = cropsStatus;
    }
}

static bool WasAddedByShakeTreeFallback(void* cropsComponent) {
    void* status = TryGetCropsStatus(cropsComponent);
    if (!status) return false;
    for (size_t i = 0; i < g_shakeTreeFallbackStatusCount; ++i) {
        if (g_shakeTreeFallbackStatuses[i] == status) return true;
    }
    return false;
}

static bool TryGetHarvestCropLandID(
    void* cropsComponent, u64* outLandID, bool* outHasSpecialForm) {
    if (!g_getCurrentCropsForm) return false;
    void* status = TryGetCropsStatus(cropsComponent);
    if (!status) return false;

    void* currentForm = g_getCurrentCropsForm(status);
    if (!IsReadable(currentForm, CROPS_IS_BIG_FORM_OFFSET + sizeof(u32)) ||
        *reinterpret_cast<const u32*>(
            reinterpret_cast<uintptr_t>(currentForm) + CROPS_IS_HARVESTABLE_OFFSET) == 0) {
        return false;
    }

    *outLandID = *reinterpret_cast<const u64*>(
        reinterpret_cast<uintptr_t>(status) + CROPS_LAND_ID_OFFSET);
    if (outHasSpecialForm) {
        const u32 changeForm = *reinterpret_cast<const u32*>(
            reinterpret_cast<uintptr_t>(currentForm) + CROPS_CHANGE_FORM_OFFSET);
        const u32 isBigForm = *reinterpret_cast<const u32*>(
            reinterpret_cast<uintptr_t>(currentForm) + CROPS_IS_BIG_FORM_OFFSET);
        *outHasSpecialForm = changeForm != 0 || isBigForm != 0;
    }
    return *outLandID != ~u64{0};
}

static void ReleaseIntrusivePointer(void* object) {
    if (!IsReadable(object, sizeof(void*) + sizeof(LONG))) return;
    volatile LONG* references = reinterpret_cast<volatile LONG*>(
        reinterpret_cast<uintptr_t>(object) + sizeof(void*));
    if (*references == 0 || InterlockedDecrement(references) != 0) return;
    void** vtable = *reinterpret_cast<void***>(object);
    if (!IsReadable(vtable, sizeof(void*)) || !vtable[0]) return;
    using DestroyFunction = void (__fastcall *)(void*, int);
    reinterpret_cast<DestroyFunction>(vtable[0])(object, 1);
}

static bool IsHarvestableNow(void* playerState, u64 landID) {
    if (!g_harvestSearch) return false;
    void* result = nullptr;
    g_harvestSearch(playerState, &result, landID);
    const bool harvestable = result != nullptr;
    if (result) ReleaseIntrusivePointer(result);
    return harvestable;
}


// ---- 空间搜索辅助（从 ChestSort/AutoHarvest 移植）---------------------------

static bool ReadPtr(const void* base, uintptr_t offset, void** out) {
    if (!base) return false;
    void* p = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(base) + offset);
    if (!IsReadable(p, sizeof(void*))) return false;
    if (out) *out = p;
    return true;
}

static bool GetWorldContext(void** outPlayer, float* outPos, void** outSpatialIndex) {
    if (!outPlayer || !outPos || !outSpatialIndex) return false;
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    void** rootSlot = reinterpret_cast<void**>(base + RVA_GAME_ROOT);
    if (!IsReadable(rootSlot, sizeof(void*))) return false;
    void* root = *rootSlot;
    if (!root) return false;

    void* player = nullptr;
    if (!ReadPtr(root, ROOT_PLAYER_OFFSET, &player)) return false;

    void* objectStatus = nullptr;
    if (!ReadPtr(player, PLAYER_OBJECT_STATUS_OFFSET, &objectStatus)) return false;

    const float* pos = reinterpret_cast<const float*>(
        reinterpret_cast<uintptr_t>(objectStatus) + GIMMICK_POSITION_OFFSET);
    if (!IsReadable(pos, sizeof(float) * 4)) return false;
    memcpy(outPos, pos, sizeof(float) * 4);
    if (!(_finite(outPos[0]) && _finite(outPos[1]))) return false;
    if (fabsf(outPos[0]) < 1.0f && fabsf(outPos[1]) < 1.0f) return false;

    void* mapOwner = nullptr;
    void* mapInfo = nullptr;
    void* spatialOwner = nullptr;
    if (!ReadPtr(root, ROOT_MAP_OWNER_OFFSET, &mapOwner)) return false;
    if (!ReadPtr(mapOwner, MAP_INFO_OFFSET, &mapInfo)) return false;
    if (!ReadPtr(mapInfo, MAP_SPATIAL_OWNER_OFFSET, &spatialOwner)) return false;

    void* spatialIndex = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(spatialOwner) + MAP_SPATIAL_INDEX_OFFSET);
    if (!IsReadable(spatialIndex, 0x48)) return false;

    *outPlayer = player;
    *outSpatialIndex = spatialIndex;
    return true;
}

// v1.2.5-diag: 恢复 v1.2.3 内联 __try 形式（v1.2.4 独立 NOINLINE wrapper 下 spatialSearch 全部 SEH 异常）
// 内联 __try 在调用点直接捕获，参数直传，与 v1.2.3 成功版本完全一致
// g_spatialSearch 签名: bool(void*, const Rect*, void*, int, int)

static bool DoSpatialSearch(void* spatialIndex, const Rect* bounds,
                            RawPointerVector* results) {
    if (!spatialIndex || !bounds || !results) return false;
    memset(results, 0, sizeof(*results));
    if (!g_spatialSearch || !g_searchCallbackVTable) return false;

    SearchCallback callback = {};
    uint64_t localFilterId = 0;
    *reinterpret_cast<void**>(callback.storage + 0x00) =
        reinterpret_cast<void*>(g_searchCallbackVTable);
    *reinterpret_cast<uint64_t**>(callback.storage + 0x08) = &localFilterId;
    *reinterpret_cast<RawPointerVector**>(callback.storage + 0x10) = results;
    callback.target = callback.storage;

    __try {
        g_spatialSearch(spatialIndex, bounds, &callback, -1, -1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[SickleHarvest][scout] spatialSearch SEH exception code=0x%08X\n",
            GetExceptionCode());
        return false;
    }

    // 销毁回调（内联 __try，与 v1.2.3 一致）
    if (callback.target) {
        __try {
            void** vtable = *reinterpret_cast<void***>(callback.target);
            using DestroyFunction = void(__fastcall*)(void*, bool);
            DestroyFunction destroy = reinterpret_cast<DestroyFunction>(vtable[4]);
            destroy(callback.target, false);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[SickleHarvest][scout] destroy SEH exception code=0x%08X\n",
                GetExceptionCode());
        }
        callback.target = nullptr;
    }

    if (!results->begin || !results->end || results->end < results->begin ||
        ((uintptr_t)results->end - (uintptr_t)results->begin) % sizeof(void*) != 0) {
        return false;
    }
    return true;
}

static void ReleaseSearchVectorGen(RawPointerVector* v) {
    if (!v || !v->begin || !g_rawVectorFreeGen) return;
    uintptr_t b = (uintptr_t)v->begin, c = (uintptr_t)v->capacity;
    size_t allocBytes = c >= b ? c - b : 0;
    if (allocBytes % sizeof(void*) == 0 && allocBytes <= 16384 * sizeof(void*)) {
        g_rawVectorFreeGen(v->begin, allocBytes);
    }
    *v = {};
}

// 读取 GimmickStatus 的 moduleName（尝试标准路径）
static bool ReadStatusType(void* status, char* buf, size_t bufSize) {
    if (!status || !buf || bufSize < 64) return false;
    void* holder = nullptr;
    if (!ReadPtr(status, GIMMICK_DATA_HOLDER_OFFSET, &holder)) return false;
    void* data = nullptr;
    if (!ReadPtr(holder, 0, &data)) return false;
    void* moduleNamePtr = nullptr;
    if (!ReadPtr(data, GIMMICK_MODULE_NAME_OFFSET, &moduleNamePtr)) return false;
    if (!IsReadable(moduleNamePtr, 64)) return false;
    size_t len = 0;
    while (len + 1 < bufSize && reinterpret_cast<const char*>(moduleNamePtr)[len] != '\0')
        ++len;
    if (reinterpret_cast<const char*>(moduleNamePtr)[len] != '\0') return false;
    memcpy(buf, moduleNamePtr, len + 1);
    return true;
}

// 尝试备用路径读取 moduleName（data holder 偏移可能不同）
static bool ReadStatusTypeAlt(void* status, char* buf, size_t bufSize) {
    if (!status || !buf || bufSize < 64) return false;
    // 尝试从 status 直接读取各种偏移
    // 路径 1: status -> [+0x240] holder -> [+0x00] data -> [+0x0D8] name (标准)
    // 路径 2: status -> [+0x250] holder -> [+0x00] data -> [+0x0D8] name
    // 路径 3: status -> [+0x230] holder
    static const uintptr_t altOffsets[] = {0x250, 0x230, 0x248, 0x238, 0x260, 0x258};
    for (size_t i = 0; i < sizeof(altOffsets)/sizeof(altOffsets[0]); ++i) {
        void* holder = nullptr;
        if (!ReadPtr(status, altOffsets[i], &holder)) continue;
        void* data = nullptr;
        if (!ReadPtr(holder, 0, &data)) continue;
        void* moduleNamePtr = nullptr;
        if (!ReadPtr(data, GIMMICK_MODULE_NAME_OFFSET, &moduleNamePtr)) continue;
        if (!IsReadable(moduleNamePtr, 64)) continue;
        size_t len = 0;
        while (len + 1 < bufSize && reinterpret_cast<const char*>(moduleNamePtr)[len] != '\0')
            ++len;
        if (len == 0) continue;
        if (reinterpret_cast<const char*>(moduleNamePtr)[len] != '\0') continue;
        memcpy(buf, moduleNamePtr, len + 1);
        return true;
    }
    return false;
}

// v1.2.4-diag: 原生函数 SEH 包装已迁移至共享框架 safe_call.h
// 调用处直接使用 qol::SafeCallOutBool1 / qol::SafeCallOutInt1

// GimmickStatus 侦查扫描：扫描附近所有 Gimmick 对象，dump moduleName + position + RTTI
// v1.2.2-diag: 对空名 gimmick 增加深度诊断——RTTI 类名、vtable dump、hex dump
// v1.2.3-diag: 修复空字符串未触发深度dump + 多偏移vtable探测 + 对比dump
static void ScoutGimmicks(void* sickleState) {
    void* player = nullptr;
    float playerPos[4] = {};
    void* spatialIndex = nullptr;
    if (!GetWorldContext(&player, playerPos, &spatialIndex)) {
        Log("[SickleHarvest][scout] GetWorldContext FAILED\n");
        return;
    }

    Log("[SickleHarvest][scout] === SCOUT START pos=(%.1f,%.1f) spatialIndex=%p ===\n",
        playerPos[0], playerPos[1], spatialIndex);

    Rect bounds = {
        playerPos[0] - SCOUT_RADIUS, playerPos[1] - SCOUT_RADIUS,
        playerPos[0] + SCOUT_RADIUS, playerPos[1] + SCOUT_RADIUS,
    };

    RawPointerVector results = {};
    if (!DoSpatialSearch(spatialIndex, &bounds, &results)) {
        Log("[SickleHarvest][scout] DoSpatialSearch FAILED\n");
        return;
    }

    size_t rawCount = 0;
    if (results.begin && results.end) {
        rawCount = static_cast<size_t>(results.end - results.begin);
    }
    Log("[SickleHarvest][scout] spatialSearch returned %zu gimmicks\n", rawCount);

    if (rawCount > 0 && !IsReadable(results.begin, rawCount * sizeof(void*))) {
        Log("[SickleHarvest][scout] results begin not readable\n");
        ReleaseSearchVectorGen(&results);
        return;
    }

    // 遍历所有 Gimmick，dump moduleName + position + RTTI
    size_t mushroomCount = 0;
    size_t unnamedCount = 0;
    size_t uniqueTypes = 0;
    char seenTypes[64][64] = {};
    size_t seenTypeCount = 0;

    for (size_t i = 0; i < rawCount && i < 512; ++i) {
        void* status = results.begin[i];
        if (!status) continue;
        if (!IsReadable(status, GIMMICK_POSITION_OFFSET + sizeof(float) * 4)) continue;

        char moduleName[64] = {};
        bool hasName = ReadStatusType(status, moduleName, sizeof(moduleName));
        if (!hasName) {
            // 尝试备用路径
            hasName = ReadStatusTypeAlt(status, moduleName, sizeof(moduleName));
        }

        // 读取位置
        const float* pos = reinterpret_cast<const float*>(
            reinterpret_cast<uintptr_t>(status) + GIMMICK_POSITION_OFFSET);
        float dx = pos[0] - playerPos[0];
        float dy = pos[1] - playerPos[1];
        float distSq = dx * dx + dy * dy;

        // RTTI 类名（v1.2.2-diag 深度诊断）
        char rttiName[128] = {};
        bool hasRTTI = ReadRTTIName(status, rttiName, sizeof(rttiName));

        // 检查是否新类型（按 moduleName 分组）
        const char* typeKey = hasName ? moduleName : "<empty>";
        bool isNew = true;
        for (size_t j = 0; j < seenTypeCount; ++j) {
            if (strcmp(seenTypes[j], typeKey) == 0) {
                isNew = false;
                break;
            }
        }
        if (isNew && seenTypeCount < 64) {
            memcpy(seenTypes[seenTypeCount], typeKey, strlen(typeKey) + 1);
            ++seenTypeCount;
        }

        // 检查是否 mushroom 相关
        bool isMushroom = (hasName && (
                          strstr(moduleName, "mushroom") != nullptr ||
                          strstr(moduleName, "tree_ear") != nullptr ||
                          strstr(moduleName, "wood_ear") != nullptr ||
                          strstr(moduleName, "fungus") != nullptr));
        if (isMushroom) ++mushroomCount;

        // 只 dump 非作物对象，或 mushroom 相关，或空名（木耳疑似对象）
        bool isCrops = (hasName && (
                        strstr(moduleName, "crops") != nullptr ||
                        strstr(moduleName, "crop") != nullptr ||
                        strstr(moduleName, "tree") != nullptr));

        bool isUnnamed = !hasName || (moduleName[0] == '\0');
        if (isUnnamed) ++unnamedCount;

        if (isMushroom || isUnnamed || (!isCrops && distSq < SCOUT_RADIUS * SCOUT_RADIUS)) {
            Log("[SickleHarvest][scout] gimmick[%zu] name='%s' rtti='%s' pos=(%.1f,%.1f) dist=%.1f %s\n",
                i,
                hasName ? moduleName : "<empty>",
                hasRTTI ? rttiName : "<none>",
                pos[0], pos[1], sqrtf(distSq),
                isMushroom ? "[MUSHROOM?]" : (isUnnamed ? "[UNNAMED]" : ""));
        }

        // v1.2.7-diag: 原生收获判定函数已证实为死代码，移除调用

    // v1.3.0-diag: 对所有非作物 gimmick 做结构 dump（不再限制只 dump 第一个）
        if (isUnnamed || (!isCrops && distSq < SCOUT_RADIUS * SCOUT_RADIUS)) {
            static thread_local int s_dumpCount = 0;
            if (s_dumpCount < 8) {
                ++s_dumpCount;
            Log("[SickleHarvest][deep] === DEEP DUMP gimmick[%zu] status=%p name='%s' rtti='%s' ===\n",
                i, status, hasName ? moduleName : "<empty>", hasRTTI ? rttiName : "<none>");

            // v1.3.0-diag: 读取 baseID (data→+0x00, u64)
            void* holder = nullptr;
            if (ReadPtr(status, GIMMICK_DATA_HOLDER_OFFSET, &holder)) {
                void* data = nullptr;
                if (ReadPtr(holder, 0, &data) && IsReadable(data, sizeof(u64))) {
                    u64 baseID = *reinterpret_cast<const u64*>(data);
                    Log("[SickleHarvest][deep] baseID=%llu (0x%llX) data=%p\n",
                        (unsigned long long)baseID, (unsigned long long)baseID, data);
                    // dump data 前 0x200 字节
                    HexDump("data", data, 0x200);
                } else {
                    Log("[SickleHarvest][deep] data read failed holder=%p\n", holder);
                }
            } else {
                Log("[SickleHarvest][deep] dataHolder NOT at +0x240, scanning\n");
                for (size_t off = 0; off < 0x400; off += 8) {
                    void* ptr = nullptr;
                    if (ReadPtr(status, off, &ptr) && ptr && IsReadable(ptr, 0x40)) {
                        Log("[SickleHarvest][deep] ptr@+0x%03zX = %p (readable)\n", off, ptr);
                    }
                }
            }

            // v1.3.0-diag: 检查 CCom_Crops 组件 (status+0x250)
            void* cropsComp = nullptr;
            if (ReadPtr(status, COM_CROPS_STATUS_OFFSET, &cropsComp)) {
                Log("[SickleHarvest][deep] CCom_Crops at +0x250 = %p (IS CROPS!)\n", cropsComp);
            } else {
                Log("[SickleHarvest][deep] CCom_Crops at +0x250 = NULL (not crops component)\n");
            }

            // v1.3.0-diag: 尝试读 status map (offset 0x18 和 0x58, 与 sap extractor 同路径)
            // status+0x18: 可能有 count 键
            // status+0x58: 可能有 itemID 键
            for (uintptr_t mapOff : {(uintptr_t)0x18, (uintptr_t)0x58, (uintptr_t)0x68,
                                     (uintptr_t)0x78, (uintptr_t)0x88}) {
                if (IsReadable(reinterpret_cast<unsigned char*>(status) + mapOff, sizeof(void*) * 4)) {
                    void* mapPtr = *reinterpret_cast<void**>(
                        reinterpret_cast<uintptr_t>(status) + mapOff);
                    if (mapPtr && IsReadable(mapPtr, 0x20)) {
                        Log("[SickleHarvest][deep] status+0x%03zX map ptr = %p\n", mapOff, mapPtr);
                    }
                }
            }

            // dump vtable (尝试从多个偏移读取 vtable 指针)
            static const uintptr_t vtableOffsets[] = {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30};
            for (size_t vi = 0; vi < sizeof(vtableOffsets)/sizeof(vtableOffsets[0]); ++vi) {
                void* maybeVt = nullptr;
                if (ReadPtr(status, vtableOffsets[vi], &maybeVt)) {
                    char altRtti[128] = {};
                    if (ReadRTTIName(reinterpret_cast<void*>(
                            reinterpret_cast<uintptr_t>(status) + vtableOffsets[vi]),
                            altRtti, sizeof(altRtti))) {
                        Log("[SickleHarvest][deep] RTTI found at +0x%03zX: '%s' vtable=%p\n",
                            vtableOffsets[vi], altRtti, maybeVt);
                    }
                }
            }
            DumpVTable(status);

            // hex dump status 前 0x400 字节
            HexDump("status", status, 0x400);

            Log("[SickleHarvest][deep] === DEEP DUMP END ===\n");
            } // end if (s_dumpCount < 8)
        }

        // v1.2.3-diag: 对第一个有名字的 gimmick 也做对比 dump（只做一次）
        if (hasName && moduleName[0] != '\0' && unnamedCount == 0) {
            static thread_local bool s_dumpedNamed = false;
            if (!s_dumpedNamed) {
                s_dumpedNamed = true;
                Log("[SickleHarvest][deep] === NAMED REFERENCE DUMP gimmick[%zu] name='%s' status=%p ===\n",
                    i, moduleName, status);
                Log("[SickleHarvest][deep] rtti='%s'\n", hasRTTI ? rttiName : "<none>");
                DumpVTable(status);
                HexDump("status_ref", status, 0x100);
                Log("[SickleHarvest][deep] === NAMED REFERENCE DUMP END ===\n");
            }
        }
    }

    Log("[SickleHarvest][scout] unique types: %zu, mushrooms: %zu, unnamed: %zu\n",
        seenTypeCount, mushroomCount, unnamedCount);
    for (size_t j = 0; j < seenTypeCount; ++j) {
        Log("[SickleHarvest][scout] type[%zu] = '%s'\n", j, seenTypes[j]);
    }

    ReleaseSearchVectorGen(&results);
    Log("[SickleHarvest][scout] === SCOUT END ===\n");
}

// ---- 摇树结算 ---------------------------------------------------------------

static void ReleaseShakeTreeSettleOutput(RawPointerVector* output) {
    if (!output || !output->begin) return;
    if (output->end >= output->begin && output->capacity >= output->end) {
        for (void** item = output->begin; item != output->end; ++item) {
            if (*item) ReleaseIntrusivePointer(*item);
        }
    }
    if (g_shakeRawVectorFree) {
        const size_t bytes = static_cast<size_t>(output->end - output->begin) *
                             sizeof(void*);
        g_shakeRawVectorFree(output->begin, bytes);
    }
    output->begin = nullptr;
    output->end = nullptr;
    output->capacity = nullptr;
}

static bool TrySettleShakeTreeOnce(void* cropsComponent, u64* outDelta) {
    if (!cropsComponent || g_insideShakeTreeSettlement ||
        !g_shakeTreeSettle || !g_shakeTreeRemaining) {
        return false;
    }
    void* status = TryGetCropsStatus(cropsComponent);
    if (!status) return false;
    const unsigned short before = g_shakeTreeRemaining(status);
    if (before == 0) return false;

    // v1.1.2: 参数修正——对齐 v1.09 原生 callsite (0x1A3C37 / 0x1A4A81)
    //   arg5 (float):  原生从 [parent_obj+0x260] 加载，我们 0.0f 会导致
    //                  函数内部除零或无效坐标计算 → 崩溃
    //   arg7 (ushort): 原生 callsite 明确用 2（我们原来用 3）
    //   arg6 (int):    保持 0（与 callsite 0x1A3C37 一致）
    float settleParam5 = 0.0f;
    if (IsReadable(reinterpret_cast<unsigned char*>(cropsComponent) + 0x260,
                   sizeof(float))) {
        settleParam5 = *reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(cropsComponent) + 0x260);
    }
    Log("[SickleHarvest] SETTLE comp=%p settleParam5=%.4f before=%u\n",
        cropsComponent, settleParam5, (unsigned)before);

    float position[4] = {};
    RawPointerVector output = {};
    g_insideShakeTreeSettlement = true;
    __try {
        g_shakeTreeSettle(cropsComponent, &output, ACTION_SHAKE_TREE, position,
                          settleParam5, 0, 2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_insideShakeTreeSettlement = false;
        Log("[SickleHarvest] ShakeTreeSettle SEH caught (safe skip)\n");
        return false;
    }
    g_insideShakeTreeSettlement = false;
    ReleaseShakeTreeSettleOutput(&output);

    const unsigned short after = g_shakeTreeRemaining(status);
    if (outDelta) *outDelta = before > after ? before - after : 0;
    return after < before;
}

static void* ShakeTreeVisualObject(void* component) {
    if (!IsReadable(component, 0x260)) return nullptr;
    return *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(component) + 0x258);
}

static bool TryPlayShakeTreeEvent(void* component, int eventId,
                                  unsigned short eventValue) {
    if (!g_shakeTreeEvent) return false;
    void* visual = ShakeTreeVisualObject(component);
    if (!visual || !IsReadable(visual, 0x840)) return false;
    if (!IsReadable(reinterpret_cast<unsigned char*>(visual) + 0x810,
                    sizeof(void*) * 4) ||
        !IsReadable(reinterpret_cast<unsigned char*>(visual) + 0x830,
                    sizeof(void*) * 2)) {
        return false;
    }
    __try {
        g_shakeTreeEvent(visual, eventId, eventValue);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

static bool TryRefreshShakeTreeVisual(void* component, void* status) {
    if (!g_shakeTreeRefresh || !component || !status) return false;
    if (!IsReadable(component, 0x290)) return false;
    if (TryGetCropsStatus(component) != status) return false;
    void* visual = ShakeTreeVisualObject(component);
    if (!visual || !IsReadable(visual, 0x840)) return false;
    void* owner = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(component) + 8);
    if (!owner || !IsReadable(owner, sizeof(void*))) return false;
    void* mapInfo = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(component) + 0x248);
    if (!mapInfo || !IsReadable(mapInfo, sizeof(void*))) return false;
    __try {
        g_shakeTreeRefresh(component);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// ---- v1.3.0-diag: Retrieve hook 已移除（诊断完成）---------------------------

// ---- Hook detour ------------------------------------------------------------

static bool __fastcall CropsActionCheckDetour(
    void* cropsStatus, void* landMap, u64 actionID) {
    // v1.2.0-diag: 记录所有 action ID 和返回值（诊断木耳等可收获物）
    static thread_local int s_checkCalls = 0;
    ++s_checkCalls;
    bool nativeResult = g_originalCropsActionCheck(cropsStatus, landMap, actionID);
    if (g_insideSickleTargetQuery) {
        // v1.6.0-diag: 提取 base 字段（+0x00/+0x18 landId），确认候选对象身份
        // v1.7.0-diag: +0x00 是 vtable 指针不是 baseID（实测全部相同），
        //              仅保留 landId + 增加 SICKLE-failed 对象的 RTTI 类名 dump
        u64 landId = 0;
        if (IsReadable(cropsStatus, 0x20)) {
            landId = *reinterpret_cast<const u64*>(
                reinterpret_cast<uintptr_t>(cropsStatus) + CROPS_LAND_ID_OFFSET);
        }
        // 每 128 次打印一次（防刷屏），SICKLE-failed 对象每次挥刀 dump 类名
        if ((s_checkCalls % 128) == 0) {
#if DIAG_SICKLEHARVEST
            Log("[SickleHarvest][diag] action_check#%d action=0x%llX native=%d cropsStatus=%p landId=0x%llX\n",
                s_checkCalls, (unsigned long long)actionID, nativeResult ? 1 : 0, cropsStatus,
                (unsigned long long)landId);
#endif
        }
    }

    if (nativeResult || actionID != ACTION_SICKLE || !g_insideSickleTargetQuery ||
        !g_sickleHarvestEnabled.load(std::memory_order_relaxed) || g_insideSickleHarvest) {
        return nativeResult;
    }
    // 原生 SICKLE 不接受的作物，尝试果树检查
    if (g_shakeTreeHarvestReady && g_shakeTreeSettle && g_shakeTreeRemaining) {
        // v1.8.0-diag: SICKLE-failed 对象 dump 重指纹（vtable RVA + 函数指纹 + hexdump）
        //              与原生接受对象对比确认是否木耳。按 vtable 去重防刷屏。
        void* vtable = GetVTable(cropsStatus);
        uintptr_t vtRva = 0;
        if (vtable && ModuleRva(vtable, &vtRva) && !AlreadyDumpedVtable(vtRva)) {
            DumpIdentityObject(cropsStatus, "SICKLE-failed");
            RememberDumpedVtable(vtRva);
        }
        Log("[SickleHarvest] action_check SICKLE-failed, trying SHAKE_TREE cropsStatus=%p\n",
            cropsStatus);
        const bool shakeResult =
            g_originalCropsActionCheck(cropsStatus, landMap, ACTION_SHAKE_TREE);
        if (shakeResult) {
            RememberShakeTreeFallbackStatus(cropsStatus);
            ++g_addedShakeTreeTargets;
            Log("[SickleHarvest] SHAKE_TREE accepted -> tree fallback #%u\n",
                g_addedShakeTreeTargets);
            return true;
        }
        // v1.8.0-diag: SHAKE_TREE 失败时打印（确认是否三 action 全拒）
        Log("[SickleHarvest] SHAKE_TREE failed too cropsStatus=%p\n", cropsStatus);
    }
    const bool harvestResult =
        g_originalCropsActionCheck(cropsStatus, landMap, ACTION_HARVEST);
    if (harvestResult) {
        RememberHarvestFallbackStatus(cropsStatus);
        ++g_addedHarvestTargets;
    }
    return harvestResult;

}


// v2.3.0/P1-8+P2-10: HarvestSettle 批处理辅助函数（独立函数避免 C2712 对象展开限制）
// SEH 包裹防崩溃 + 8ms 帧预算防卡顿
static void RunHarvestSettleBatch(const u64* harvestLandIDs, size_t harvestCount,
                                   u64* actionTarget, void* sickleState) {
    LARGE_INTEGER budgetStart, budgetFreq;
    QueryPerformanceCounter(&budgetStart);
    QueryPerformanceFrequency(&budgetFreq);
    const LONGLONG budgetLimit = budgetFreq.QuadPart / 125;  // 8ms
    size_t harvestedActual = 0;
    __try {
        for (size_t i = 0; i < harvestCount; ++i) {
            if ((i & 15) == 15) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                if (now.QuadPart - budgetStart.QuadPart > budgetLimit) {
                    Log("[SickleHarvest] HarvestSettle 超 8ms 预算，已处理 %zu/%zu",
                        harvestedActual, harvestCount);
                    break;
                }
            }
            *actionTarget = harvestLandIDs[i];
            g_harvestSettle(sickleState);
            ++harvestedActual;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[SickleHarvest] HarvestSettle 异常 0x%X at index %zu",
            GetExceptionCode(), harvestedActual);
    }
}

static RawPointerVector* __fastcall GetSickleTargetCropsDetour(
    void* sickleState, RawPointerVector* targets) {
    // v1.0.5 诊断入口日志：确认 hook 是否触发
    Log("[SickleHarvest] ENTER getTargetCrops sickleState=%p targets=%p enabled=%d hookReady=%d\n",
        sickleState, targets, g_sickleHarvestEnabled.load(std::memory_order_relaxed) ? 1 : 0,
        g_sickleHarvestHookReady ? 1 : 0);
    g_addedHarvestTargets = 0;
    g_addedShakeTreeTargets = 0;
    g_harvestFallbackStatusCount = 0;
    g_shakeTreeFallbackStatusCount = 0;
    g_insideSickleTargetQuery = true;
    FastRegionReset();  // v2.3.0/P2-11: 提前到 native 调用前，detour 期间用干净缓存
    Log("[SickleHarvest] CALL originalGetTargetCrops\n");
    RawPointerVector* result = g_originalGetSickleTargetCrops(sickleState, targets);
    g_insideSickleTargetQuery = false;
    Log("[SickleHarvest] RET originalGetTargetCrops result=%p begin=%p end=%p cap=%p\n",
        (void*)result, result ? (void*)result->begin : nullptr,
        result ? (void*)result->end : nullptr,
        result ? (void*)result->capacity : nullptr);
if (!result) {
        Log("[SickleHarvest] early-return reason=result_null\n");
        return result;
    }
    if (!IsReadable(result, sizeof(*result)) || !result->begin ||
        result->end < result->begin || result->capacity < result->end) {
        // v1.6.0-diag: 原生返回空列表时，打印 fallback 接受统计
        Log("[SickleHarvest] early-return reason=result_unreadable "
            "fallback_harvest=%u fallback_shake=%u\n",
            g_addedHarvestTargets, g_addedShakeTreeTargets);
        return result;
    }
    if (!g_sickleHarvestEnabled.load(std::memory_order_relaxed)) {
        Log("[SickleHarvest] early-return reason=not_enabled targets=%zu\n",
            (size_t)(result->end - result->begin));
        return result;
    }
    if (g_insideSickleHarvest) {
        Log("[SickleHarvest] early-return reason=inside_sickle_harvest\n");
        return result;
    }
    if (!g_getCurrentCropsForm || !g_harvestSearch || !g_harvestSettle) {
        Log("[SickleHarvest] early-return reason=func_null form=%p search=%p settle=%p\n",
            (void*)g_getCurrentCropsForm, (void*)g_harvestSearch, (void*)g_harvestSettle);
        return result;
    }
    if (!IsReadable(result, sizeof(*result)) || !result->begin ||
        result->end < result->begin || result->capacity < result->end) {
        Log("[SickleHarvest] early-return reason=result_unreadable\n");
        return result;
    }

    const size_t targetCount = static_cast<size_t>(result->end - result->begin);
    if (targetCount == 0) {
        Log("[SickleHarvest] early-return reason=zero_targets\n");
        return result;
    }
    // v1.1.0: 移除 256 上限——v1.09 原生 getTargetCrops 返回 3250+ 个目标
    //         固定数组已改为 vector，不再有溢出风险
    if (!IsReadable(result->begin, targetCount * sizeof(void*))) {
        Log("[SickleHarvest] early-return reason=begin_unreadable\n");
        return result;
    }


    // v1.4.0-diag: ScoutGimmicks 已移除——v1.3.0 实测木耳不在 spatialSearch 索引中

    // 重置区域缓存：每次挥镰刀处理前清空（v2.3.0 已提前到 native 调用前，此处保留冗余重置确保干净）
    FastRegionReset();

    Log("[SickleHarvest] PROCESS START targets=%zu\n", targetCount);
    // v1.8.0-diag: dump 原生接受对象（SICKLE 通过）的 vtable 指纹，与 SICKLE-failed 对比
    //              仅打印第一个接受对象的完整指纹 + 全部接受对象的 vtable RVA 列表
    {
        uintptr_t vtRva = 0;
        void* firstStatus = targetCount > 0 ? TryGetCropsStatus(result->begin[0]) : nullptr;
        void* firstVt = firstStatus ? GetVTable(firstStatus) : nullptr;
        if (firstVt && ModuleRva(firstVt, &vtRva)) {
            Log("[SickleHarvest] NATIVE-ACCEPTED first vtable RVA=0x%zX (%zu targets)\n",
                (size_t)vtRva, targetCount);
            // 与 SICKLE-failed 对比：dump 前 6 个函数 RVA
            void** vt = reinterpret_cast<void**>(firstVt);
            for (int i = 0; i < 6; ++i) {
                if (!IsReadable(vt + i, sizeof(void*))) break;
                uintptr_t fnRva = 0;
                if (ModuleRva(vt[i], &fnRva)) {
                    Log("[SickleHarvest] ACCEPTED vt[%d]=RVA 0x%zX\n", i, (size_t)fnRva);
                } else {
                    Log("[SickleHarvest] ACCEPTED vt[%d]=0x%p (non-module)\n", i, vt[i]);
                }
            }
        } else {
            Log("[SickleHarvest] ACCEPTED first vtable unavailable\n");
        }
        // 记录所有接受对象的 vtable RVA（去重）
        for (size_t i = 0; i < targetCount && i < 16; ++i) {
            void* s = TryGetCropsStatus(result->begin[i]);
            if (!s) continue;
            void* vt = GetVTable(s);
            if (!vt || !ModuleRva(vt, &vtRva)) continue;
            if (!AlreadyDumpedVtable(vtRva)) {
                Log("[SickleHarvest] ACCEPTED vtable=%p RVA=0x%zX\n", vt, (size_t)vtRva);
                RememberDumpedVtable(vtRva);
            }
        }
    }
    // v1.1.0: 固定数组 → vector（v1.09 原生返回 3250+ 目标）
    // v2.2.2: 去重改用 unordered_set 哈希集合（O(1) 查找），避免 3250+ 目标
    //         时 O(N²) 线性扫描（约 10M 次比较）
    std::vector<u64> harvestLandIDs;
    std::unordered_set<void*> seenHarvestComponents;
    std::unordered_set<u64> seenHarvestLandIDs;
    std::unordered_set<void*> seenShakeTreeComponents;
    std::unordered_set<void*> seenShakeTreeStatuses;
    // shakeTreeComponents / shakeTreeStatuses 保留 vector（结算循环按索引访问）
    std::vector<void*> shakeTreeComponents;
    std::vector<void*> shakeTreeStatuses;
    size_t uniqueHarvestTargetCount = 0;
    size_t shakeTreeTargetCount = 0;
    size_t harvestCount = 0;
    size_t nativeSickleCount = 0;
    size_t duplicateCount = 0;
    size_t shakeDuplicateCount = 0;
    size_t specialFormCount = 0;
    size_t protectedImmatureCount = 0;
    size_t unresolvedFallbackCount = 0;
    size_t shakeUnresolvedCount = 0;
    void** write = result->begin;
    for (void** read = result->begin; read != result->end; ++read) {
        if (WasAddedByShakeTreeFallback(*read) &&
            !WasAddedByHarvestFallback(*read)) {
            void* status = TryGetCropsStatus(*read);
            if (!status) {
                ++shakeUnresolvedCount;
                continue;
            }
            // v2.2.2: O(1) 哈希去重（component 指针 + status 指针双维度）
            if (seenShakeTreeComponents.count(*read) != 0 ||
                seenShakeTreeStatuses.count(status) != 0) {
                ++shakeDuplicateCount;
                continue;
            }
            seenShakeTreeComponents.insert(*read);
            seenShakeTreeStatuses.insert(status);
            shakeTreeComponents.push_back(*read);
            shakeTreeStatuses.push_back(status);
            ++shakeTreeTargetCount;
            continue;
        }

        // v2.2.0-diag: 对原生接受的目标也尝试收割（修复木耳被接受但不收割）
        // 关键改进（vs v2.0.0）：MOD 收割的目标不写回列表（不 *write++=*read），
        // 避免原生重复处理导致双倍产出；未成熟或拿不到 landID 的照常写回。
        if (!WasAddedByHarvestFallback(*read)) {
            // 尝试获取 landID 并执行 HarvestSettle
            u64 nativeLandID = 0;
            bool nativeHasSpecialForm = false;
            bool gotLandID = TryGetHarvestCropLandID(*read, &nativeLandID, &nativeHasSpecialForm);

            // Fallback：标准 form 路径失败时，直接读 status+0x18 拿 landID
            // 木耳可能不走标准作物 form 结构（TryGetHarvestCropLandID 返回 false）
            if (!gotLandID) {
                void* nativeStatus = TryGetCropsStatus(*read);
                if (nativeStatus && IsReadable(nativeStatus, CROPS_LAND_ID_OFFSET + sizeof(u64))) {
                    nativeLandID = *reinterpret_cast<const u64*>(
                        reinterpret_cast<uintptr_t>(nativeStatus) + CROPS_LAND_ID_OFFSET);
                    if (nativeLandID != 0 && nativeLandID != ~u64{0}) {
                        gotLandID = true;
                    }
                }
            }

            bool nativeHarvested = false;
            if (gotLandID) {
                // v2.2.2: O(1) 哈希去重（component 指针 + landID 双维度）
                const bool nativeDup =
                    seenHarvestComponents.count(*read) != 0 ||
                    seenHarvestLandIDs.count(nativeLandID) != 0;
                if (!nativeDup) {
                    seenHarvestComponents.insert(*read);
                    seenHarvestLandIDs.insert(nativeLandID);
                    ++uniqueHarvestTargetCount;
                    if (nativeHasSpecialForm) ++specialFormCount;
                    if (IsHarvestableNow(sickleState, nativeLandID)) {
                        // MOD 收割：不写回列表，避免原生重复产出
                        harvestLandIDs.push_back(nativeLandID);
                        ++harvestCount;
                        nativeHarvested = true;
                        Log("[SickleHarvest] NATIVE-HARVEST comp=%p landID=0x%llX harvestable=YES (not written back)\n",
                            *read, (unsigned long long)nativeLandID);
                    } else {
                        ++protectedImmatureCount;
                        Log("[SickleHarvest] NATIVE-PROTECT comp=%p landID=0x%llX harvestable=NO\n",
                            *read, (unsigned long long)nativeLandID);
                    }
                }
            }
            // 未被 MOD 收割的（未成熟/拿不到 landID/重复）写回列表交原生处理
            if (!nativeHarvested) {
                *write++ = *read;
                ++nativeSickleCount;
            }
            continue;
        }

        u64 landID = 0;
        bool hasSpecialForm = false;
        if (TryGetHarvestCropLandID(*read, &landID, &hasSpecialForm)) {
            // v2.2.2: O(1) 哈希去重（component 指针 + landID 双维度）
            if (seenHarvestComponents.count(*read) != 0 ||
                seenHarvestLandIDs.count(landID) != 0) {
                ++duplicateCount;
                continue;
            }
            seenHarvestComponents.insert(*read);
            seenHarvestLandIDs.insert(landID);
            ++uniqueHarvestTargetCount;
            if (hasSpecialForm) ++specialFormCount;
            if (IsHarvestableNow(sickleState, landID)) {
                harvestLandIDs.push_back(landID);
                ++harvestCount;
            } else {
                ++protectedImmatureCount;
            }
        } else {
            ++unresolvedFallbackCount;
            // v1.2.0-diag: dump unresolved fallback 目标的结构信息
            void* diagStatus = TryGetCropsStatus(*read);
            if (diagStatus && g_getCurrentCropsForm && IsReadable(diagStatus, 0x20)) {
                void* diagForm = g_getCurrentCropsForm(diagStatus);
                u64 diagLandID = *reinterpret_cast<const u64*>(
                    reinterpret_cast<uintptr_t>(diagStatus) + CROPS_LAND_ID_OFFSET);
                u32 diagHarvestable = 0;
                if (diagForm && IsReadable(diagForm, CROPS_IS_HARVESTABLE_OFFSET + sizeof(u32))) {
                    diagHarvestable = *reinterpret_cast<const u32*>(
                        reinterpret_cast<uintptr_t>(diagForm) + CROPS_IS_HARVESTABLE_OFFSET);
                }
#if DIAG_SICKLEHARVEST
                Log("[SickleHarvest][diag] unresolved comp=%p status=%p form=%p harvestable=0x%X landID=0x%llX\n",
                    *read, diagStatus, diagForm, diagHarvestable,
                    (unsigned long long)diagLandID);
#endif
            } else {
#if DIAG_SICKLEHARVEST
                Log("[SickleHarvest][diag] unresolved comp=%p status=%p (no status/form)\n",
                    *read, diagStatus);
#endif
            }
        }
    }
    result->end = write;

    u64* actionTarget = nullptr;
    bool actionTargetReadable = IsReadable(
        reinterpret_cast<unsigned char*>(sickleState) +
            PLAYER_ACTION_TARGET_OFFSET,
        sizeof(u64));
    if (actionTargetReadable) {
        actionTarget = reinterpret_cast<u64*>(
            reinterpret_cast<uintptr_t>(sickleState) + PLAYER_ACTION_TARGET_OFFSET);
    }

    g_insideSickleHarvest = true;
    if (harvestCount > 0 && actionTarget) {
        const u64 savedTarget = *actionTarget;
        // v2.3.0/P1-8+P2-10: SEH 包裹 + 帧预算（提取到辅助函数避免 C2712）
        RunHarvestSettleBatch(harvestLandIDs.data(), harvestCount, actionTarget, sickleState);
        *actionTarget = savedTarget;
    }

    Log("[SickleHarvest] HARVEST DONE harvested=%zu trees=%zu native=%zu\n",
        harvestCount, shakeTreeTargetCount, nativeSickleCount);
    size_t shakeTreeSettledCount = 0;
    size_t shakeTotalCount = 0;
    size_t shakeTreeRemainingAfter = 0;
    size_t shakeEventCount = 0;
    size_t shakeRefreshCount = 0;
    Log("[SickleHarvest] SHAKE START treeCount=%zu\n", shakeTreeTargetCount);
    for (size_t i = 0; i < shakeTreeTargetCount; ++i) {
        void* status = shakeTreeStatuses[i];
        if (!status || !g_shakeTreeRemaining) continue;
        unsigned short remaining = g_shakeTreeRemaining(status);
        if (g_shakeTreeVisualReady && remaining > 0) {
            const int eventId = remaining > 2 ? -3 : (remaining > 1 ? -2 : -1);
            const unsigned short eventValue = remaining > 2
                ? 0x3a2 : (remaining > 1 ? 0x398 : 0x38e);
            if (TryPlayShakeTreeEvent(shakeTreeComponents[i], eventId,
                                      eventValue)) {
                ++shakeEventCount;
            }
        }
        Log("[SickleHarvest] SHAKE tree[%zu] comp=%p status=%p remaining=%u\n",
            i, shakeTreeComponents[i], status, (unsigned)remaining);
        size_t settled = 0;
        while (remaining > 0 && settled < SICKLE_SHAKE_TREE_MAX_SHAKES) {
            u64 delta = 0;
            if (!TrySettleShakeTreeOnce(shakeTreeComponents[i], &delta)) break;
            if (delta == 0) break;
            ++settled;
            remaining = g_shakeTreeRemaining(status);
        }
        shakeTotalCount += settled;
        if (settled > 0) {
            ++shakeTreeSettledCount;
            shakeTreeRemainingAfter += remaining;
            if (g_shakeTreeVisualReady &&
                TryRefreshShakeTreeVisual(shakeTreeComponents[i], status)) {
                ++shakeRefreshCount;
            }
        } else if (remaining > 0) {
            shakeTreeRemainingAfter += remaining;
        }
    }
    g_insideSickleHarvest = false;

    Log("[SickleHarvest] swing targets=%zu fallback_checks=%u fallback_status=%zu "
        "tree_checks=%u tree_status=%zu native=%zu unique=%zu duplicates=%zu "
        "special=%zu harvested=%zu protected=%zu unresolved=%zu normal=%zu "
        "trees=%zu tree_dedup=%zu tree_unresolved=%zu tree_settled=%zu "
        "shakes=%zu tree_remaining=%zu shake_events=%zu tree_refreshed=%zu\n",
        targetCount, g_addedHarvestTargets, g_harvestFallbackStatusCount,
        g_addedShakeTreeTargets, g_shakeTreeFallbackStatusCount,
        nativeSickleCount, uniqueHarvestTargetCount, duplicateCount,
        specialFormCount, harvestCount, protectedImmatureCount,
        unresolvedFallbackCount, static_cast<size_t>(result->end - result->begin),
        shakeTreeTargetCount, shakeDuplicateCount, shakeUnresolvedCount,
        shakeTreeSettledCount, shakeTotalCount, shakeTreeRemainingAfter,
shakeEventCount, shakeRefreshCount);
    return result;
}

// ---- 安装 -------------------------------------------------------------------

static bool InstallShakeTreeSickleSupport() {
    g_shakeTreeHarvestReady = false;
    g_shakeTreeVisualReady = false;
    g_shakeTreeSettle = nullptr;
    g_shakeTreeRemaining = nullptr;
    g_shakeRawVectorFree = nullptr;
    g_shakeTreeRefresh = nullptr;
    g_shakeTreeEvent = nullptr;
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    static const unsigned char expectedShakeTreeSettle[16] = {
        0x48, 0x8b, 0xc4, 0x48, 0x89, 0x50, 0x10, 0x55,
        0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41
    };
    static const unsigned char expectedShakeTreeRemaining[12] = {
        0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83,
        0xec, 0x20, 0x48, 0x8b
    };
    static const unsigned char expectedShakeRawVectorFree[12] = {
        0x48, 0x83, 0xec, 0x38, 0x48, 0x81, 0xfa, 0x00,
        0x10, 0x00, 0x00, 0x72
    };
    unsigned char* shakeSettle = reinterpret_cast<unsigned char*>(
        base + RVA_SHAKE_TREE_SETTLE);
    unsigned char* shakeRemaining = reinterpret_cast<unsigned char*>(
        base + RVA_SHAKE_TREE_REMAINING);
    unsigned char* shakeRawVectorFree = reinterpret_cast<unsigned char*>(
        base + RVA_SHAKE_RAW_VECTOR_FREE);
    if (memcmp(shakeSettle, expectedShakeTreeSettle,
               sizeof(expectedShakeTreeSettle)) != 0 ||
        memcmp(shakeRemaining, expectedShakeTreeRemaining,
               sizeof(expectedShakeTreeRemaining)) != 0 ||
        memcmp(shakeRawVectorFree, expectedShakeRawVectorFree,
               sizeof(expectedShakeRawVectorFree)) != 0) {
        Log("[SickleHarvest] shake-tree settlement byte check FAILED; "
            "tree harvest disabled safely\n");
        return false;
    }
    g_shakeTreeSettle = reinterpret_cast<ShakeTreeSettleFunction>(
        base + RVA_SHAKE_TREE_SETTLE);
    g_shakeTreeRemaining = reinterpret_cast<ShakeTreeRemainingFunction>(
        base + RVA_SHAKE_TREE_REMAINING);
    g_shakeRawVectorFree = reinterpret_cast<ShakeRawVectorFreeFunction>(
        base + RVA_SHAKE_RAW_VECTOR_FREE);
    g_shakeTreeHarvestReady = true;
    Log("[SickleHarvest] shake-tree settlement + remaining-count + "
        "raw-vector-free verified; one-swing tree harvest ENABLED\n");

    static const unsigned char expectedShakeTreeRefresh[12] = {
        0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c,
        0x24, 0x18, 0x56, 0x57
    };
    static const unsigned char expectedShakeTreeEvent[12] = {
        0x48, 0x89, 0x5c, 0x24, 0x18, 0x57, 0x48, 0x83,
        0xec, 0x40, 0x48, 0x8b
    };
    if (memcmp(reinterpret_cast<void*>(base + RVA_SHAKE_TREE_REFRESH),
               expectedShakeTreeRefresh, sizeof(expectedShakeTreeRefresh)) != 0 ||
        memcmp(reinterpret_cast<void*>(base + RVA_SHAKE_TREE_EVENT),
               expectedShakeTreeEvent, sizeof(expectedShakeTreeEvent)) != 0) {
        Log("[SickleHarvest] shake-tree visual refresh byte check FAILED; "
            "trees are harvested but stale visuals may remain\n");
        return true;
    }
    g_shakeTreeRefresh = reinterpret_cast<ShakeTreeRefreshFunction>(
        base + RVA_SHAKE_TREE_REFRESH);
    g_shakeTreeEvent = reinterpret_cast<ShakeTreeEventFunction>(
        base + RVA_SHAKE_TREE_EVENT);
    g_shakeTreeVisualReady = true;
    Log("[SickleHarvest] shake-tree visual refresh + shake event verified; "
        "tree shake animation refresh ENABLED\n");
    return true;
}

static bool InstallSickleHarvestHook() {
    g_sickleHarvestEnabled.store(false, std::memory_order_relaxed);
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    unsigned char* target = reinterpret_cast<unsigned char*>(
        base + RVA_SICKLE_GET_TARGET_CROPS);

    // v1.09 字节签名
    static const unsigned char expected[SICKLE_HOOK_LENGTH] = {
        0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57,
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
    };
    static const unsigned char expectedHarvestSettle[12] = {
        0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74,
        0x24, 0x18, 0x48, 0x89
    };
    static const unsigned char expectedHarvestSearch[12] = {
        0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x30,
        0x48, 0x89, 0x54, 0x24
    };
    static const unsigned char expectedGetCurrentForm[12] = {
        0x48, 0x8b, 0x41, 0x10, 0x45, 0x33, 0xc0, 0x4c,
        0x8b, 0xd9, 0x48, 0x8b
    };
    static const unsigned char expectedActionCallsite[21] = {
        0x41, 0xb8, 0x10, 0x04, 0x00, 0x00, 0x48, 0x8b,
        0xd7, 0x49, 0x8b, 0x8e, 0x50, 0x02, 0x00, 0x00,
        0xe8, 0xd9, 0x6c, 0xea, 0xff
    };
    unsigned char* actionCheck = reinterpret_cast<unsigned char*>(
        base + RVA_CROPS_ACTION_CHECK);
    static const unsigned char expectedActionCheck[ACTION_CHECK_HOOK_LENGTH] = {
        0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
        0x24, 0x10, 0x56, 0x57, 0x41, 0x56
    };
    if (memcmp(target, expected, sizeof(expected)) != 0 ||
        memcmp(actionCheck, expectedActionCheck, sizeof(expectedActionCheck)) != 0 ||
        memcmp(reinterpret_cast<void*>(base + RVA_CROPS_GET_CURRENT_FORM),
               expectedGetCurrentForm, sizeof(expectedGetCurrentForm)) != 0 ||
        memcmp(reinterpret_cast<void*>(base + RVA_HARVEST_SEARCH),
               expectedHarvestSearch, sizeof(expectedHarvestSearch)) != 0 ||
        memcmp(reinterpret_cast<void*>(base + RVA_HARVEST_SETTLE),
               expectedHarvestSettle, sizeof(expectedHarvestSettle)) != 0 ||
        memcmp(reinterpret_cast<void*>(base + RVA_CROPS_ACTION_CALLSITE),
               expectedActionCallsite, sizeof(expectedActionCallsite)) != 0) {
        Log("[SickleHarvest] function byte check FAILED; hook disabled safely\n");
        return false;
    }

    // H3: 保存原始字节供 unload 还原
    memcpy(g_savedActionCheckBytes, actionCheck, ACTION_CHECK_HOOK_LENGTH);
    memcpy(g_savedSickleHookBytes, target, SICKLE_HOOK_LENGTH);

    // CropsActionCheck trampoline
    unsigned char* actionTrampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!actionTrampoline) {
        Log("[SickleHarvest] action trampoline allocation failed (error %lu)\n",
            GetLastError());
        return false;
    }
    memcpy(actionTrampoline, expectedActionCheck, sizeof(expectedActionCheck));
    unsigned char actionJumpBack[14] = {0xff, 0x25, 0, 0, 0, 0};
    *reinterpret_cast<u64*>(actionJumpBack + 6) =
        reinterpret_cast<u64>(actionCheck + ACTION_CHECK_HOOK_LENGTH);
    memcpy(actionTrampoline + ACTION_CHECK_HOOK_LENGTH,
           actionJumpBack, sizeof(actionJumpBack));
    FlushInstructionCache(GetCurrentProcess(), actionTrampoline, 64);
    if (!SealExecutableMemory(actionTrampoline, 64)) {
        VirtualFree(actionTrampoline, 0, MEM_RELEASE);
        Log("[SickleHarvest] action trampoline sealing failed\n");
        return false;
    }
    g_originalCropsActionCheck =
        reinterpret_cast<CropsActionCheckFunction>(actionTrampoline);

    unsigned char actionHook[ACTION_CHECK_HOOK_LENGTH] = {0xff, 0x25, 0, 0, 0, 0};
    *reinterpret_cast<u64*>(actionHook + 6) =
        reinterpret_cast<u64>(&CropsActionCheckDetour);
    if (!WriteMem(actionCheck, actionHook, sizeof(actionHook))) {
        VirtualFree(actionTrampoline, 0, MEM_RELEASE);
        g_originalCropsActionCheck = nullptr;
        Log("[SickleHarvest] action hook write failed\n");
        return false;
    }

    // getTargetCorps trampoline
    unsigned char* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!trampoline) {
        Log("[SickleHarvest] trampoline allocation failed (error %lu)\n", GetLastError());
        // H4: 回滚已安装的 actionCheck hook
        WriteMem(actionCheck, g_savedActionCheckBytes, ACTION_CHECK_HOOK_LENGTH);
        VirtualFree(actionTrampoline, 0, MEM_RELEASE);
        g_originalCropsActionCheck = nullptr;
        return false;
    }
    memcpy(trampoline, expected, sizeof(expected));
    unsigned char jumpBack[14] = {0xff, 0x25, 0, 0, 0, 0};
    *reinterpret_cast<u64*>(jumpBack + 6) =
        reinterpret_cast<u64>(target + SICKLE_HOOK_LENGTH);
    memcpy(trampoline + SICKLE_HOOK_LENGTH, jumpBack, sizeof(jumpBack));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    if (!SealExecutableMemory(trampoline, 64)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("[SickleHarvest] trampoline sealing failed\n");
        // H4: 回滚已安装的 actionCheck hook
        WriteMem(actionCheck, g_savedActionCheckBytes, ACTION_CHECK_HOOK_LENGTH);
        VirtualFree(actionTrampoline, 0, MEM_RELEASE);
        g_originalCropsActionCheck = nullptr;
        return false;
    }
    g_originalGetSickleTargetCrops =
        reinterpret_cast<GetSickleTargetCropsFunction>(trampoline);
    g_getCurrentCropsForm =
        reinterpret_cast<GetCurrentCropsFormFunction>(base + RVA_CROPS_GET_CURRENT_FORM);
    g_harvestSearch = reinterpret_cast<HarvestSearchFunction>(base + RVA_HARVEST_SEARCH);
    g_harvestSettle = reinterpret_cast<HarvestSettleFunction>(base + RVA_HARVEST_SETTLE);

    unsigned char hook[SICKLE_HOOK_LENGTH] = {0xff, 0x25, 0, 0, 0, 0};
    *reinterpret_cast<u64*>(hook + 6) =
        reinterpret_cast<u64>(&GetSickleTargetCropsDetour);
    hook[14] = 0x90;
    hook[15] = 0x90;
    if (!WriteMem(target, hook, sizeof(hook))) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        g_originalGetSickleTargetCrops = nullptr;
        g_getCurrentCropsForm = nullptr;
        g_harvestSettle = nullptr;
        // H4: 回滚已安装的 actionCheck hook
        WriteMem(actionCheck, g_savedActionCheckBytes, ACTION_CHECK_HOOK_LENGTH);
        VirtualFree(actionTrampoline, 0, MEM_RELEASE);
        g_originalCropsActionCheck = nullptr;
        Log("[SickleHarvest] hook write failed\n");
        return false;
    }
    g_sickleHarvestHookReady = true;
    g_sickleHarvestEnabled.store(true, std::memory_order_relaxed);
    Log("[SickleHarvest] native-range + current-form + fallback-origin + "
        "deduplicated harvest hooks installed; starts ENABLED\n");

    // v2.2.2: HarvestSettle 被动捕获 hook 已移除（诊断完成，g_harvestSettle 直接指向原函数）
    if (!InstallShakeTreeSickleSupport()) {
        Log("[SickleHarvest] one-swing tree harvest unavailable; field crop "
            "harvest remains enabled\n");
    }

    // v1.2.1-diag: 初始化空间搜索（侦查用）
    {
        g_spatialSearch = reinterpret_cast<FnSpatialSearch>(base + RVA_SPATIAL_SEARCH);
        g_rawVectorFreeGen = reinterpret_cast<FnRawVectorFreeGen>(base + RVA_RAW_VECTOR_FREE_GEN);
        g_searchCallbackVTable = base + SEARCH_CALLBACK_VTABLE_RVA;
        // 字节签名验证 spatialSearch
        static const unsigned char expectedSpatialSearch[14] = {
        0x40, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
        0x41, 0x57, 0x48, 0x83, 0xec, 0x60
    };
        if (memcmp(reinterpret_cast<void*>(base + RVA_SPATIAL_SEARCH),
                   expectedSpatialSearch, sizeof(expectedSpatialSearch)) != 0) {
            Log("[SickleHarvest][scout] spatialSearch byte check FAILED\n");
            g_spatialSearch = nullptr;
        } else {
            Log("[SickleHarvest][scout] spatialSearch + rawVectorFree + callbackVTable verified\n");
        }
    }

    // v1.2.7-diag: 旧 GimmickIsHarvestItem/GimmickGetHarvestItemID 初始化已移除（死代码）

    // v1.3.0-diag: Retrieve hook 已移除（诊断完成）

    // v2.2.2: item_ctor 被动捕获 hook 已移除（诊断完成，RVA_ITEM_CTOR 常量一并删除）

    return true;
}

// ---- DLL 入口 ---------------------------------------------------------------

extern "C" __declspec(dllexport) void mod_init(void) {
    LogOpen("sickleharvest");
    Log("[SickleHarvest] mod_init -- v2.3.5 v1.20 build 25311578\n");
    HotConfig_Register("sickleharvest", (void*)&RVA_SICKLE_GET_TARGET_CROPS, "RVA_SICKLE_GET_TARGET_CROPS", HOT_RVA, 0x247F10);
    HotConfig_Register("sickleharvest", (void*)&RVA_CROPS_ACTION_CHECK, "RVA_CROPS_ACTION_CHECK", HOT_RVA, 0x0EEEB0);
    HotConfig_Register("sickleharvest", (void*)&RVA_CROPS_GET_CURRENT_FORM, "RVA_CROPS_GET_CURRENT_FORM", HOT_RVA, 0x0EE790);
    HotConfig_Register("sickleharvest", (void*)&RVA_HARVEST_SEARCH, "RVA_HARVEST_SEARCH", HOT_RVA, 0x21D960);
    HotConfig_Register("sickleharvest", (void*)&RVA_HARVEST_SETTLE, "RVA_HARVEST_SETTLE", HOT_RVA, 0x21D380);
    HotConfig_Register("sickleharvest", (void*)&RVA_CROPS_ACTION_CALLSITE, "RVA_CROPS_ACTION_CALLSITE", HOT_RVA, 0x2481C2);
    HotConfig_Register("sickleharvest", (void*)&RVA_SHAKE_TREE_SETTLE, "RVA_SHAKE_TREE_SETTLE", HOT_RVA, 0x1A7A40);
    HotConfig_Register("sickleharvest", (void*)&RVA_SHAKE_TREE_REMAINING, "RVA_SHAKE_TREE_REMAINING", HOT_RVA, 0x0EF470);
    HotConfig_Register("sickleharvest", (void*)&RVA_SHAKE_RAW_VECTOR_FREE, "RVA_SHAKE_RAW_VECTOR_FREE", HOT_RVA, 0x0B8720);
    HotConfig_Register("sickleharvest", (void*)&RVA_SHAKE_TREE_REFRESH, "RVA_SHAKE_TREE_REFRESH", HOT_RVA, 0x1A3300);
    HotConfig_Register("sickleharvest", (void*)&RVA_SHAKE_TREE_EVENT, "RVA_SHAKE_TREE_EVENT", HOT_RVA, 0x7600C0);
    // MODULE_IMAGE_SIZE 是扫描区域大小（非模块内偏移），用 HOT_INT64
    // 避免被 IsRvaSane 误拒（D6/D7 的 RVA 合法性检查只应管真正的 RVA）
    HotConfig_Register("sickleharvest", (void*)&MODULE_IMAGE_SIZE, "MODULE_IMAGE_SIZE", HOT_INT64, 0x5000000);
    HotConfig_Register("sickleharvest", (void*)&COM_CROPS_STATUS_OFFSET, "COM_CROPS_STATUS_OFFSET", HOT_RVA, 0x250);
    HotConfig_Register("sickleharvest", (void*)&CROPS_LAND_ID_OFFSET, "CROPS_LAND_ID_OFFSET", HOT_RVA, 0x18);
    HotConfig_Register("sickleharvest", (void*)&CROPS_IS_HARVESTABLE_OFFSET, "CROPS_IS_HARVESTABLE_OFFSET", HOT_RVA, 0x78);
    HotConfig_Register("sickleharvest", (void*)&CROPS_CHANGE_FORM_OFFSET, "CROPS_CHANGE_FORM_OFFSET", HOT_RVA, 0x80);
    HotConfig_Register("sickleharvest", (void*)&CROPS_IS_BIG_FORM_OFFSET, "CROPS_IS_BIG_FORM_OFFSET", HOT_RVA, 0x98);
    HotConfig_Register("sickleharvest", (void*)&PLAYER_ACTION_TARGET_OFFSET, "PLAYER_ACTION_TARGET_OFFSET", HOT_RVA, 0x248);
    HotConfig_Register("sickleharvest", (void*)&RVA_GAME_ROOT, "RVA_GAME_ROOT", HOT_RVA, 0x10FCBB0);
    HotConfig_Register("sickleharvest", (void*)&WORLD_OBJECT_REGISTRY_RVA, "WORLD_OBJECT_REGISTRY_RVA", HOT_RVA, 0x1104C80);
    HotConfig_Register("sickleharvest", (void*)&SEARCH_CALLBACK_VTABLE_RVA, "SEARCH_CALLBACK_VTABLE_RVA", HOT_RVA, 0xE3E608);
    HotConfig_Register("sickleharvest", (void*)&RVA_SPATIAL_SEARCH, "RVA_SPATIAL_SEARCH", HOT_RVA, 0x194060);
    HotConfig_Register("sickleharvest", (void*)&RVA_RAW_VECTOR_FREE_GEN, "RVA_RAW_VECTOR_FREE_GEN", HOT_RVA, 0x0B8720);
    HotConfig_Register("sickleharvest", (void*)&ROOT_PLAYER_OFFSET, "ROOT_PLAYER_OFFSET", HOT_RVA, 0x208);
    HotConfig_Register("sickleharvest", (void*)&ROOT_MAP_OWNER_OFFSET, "ROOT_MAP_OWNER_OFFSET", HOT_RVA, 0x268);
    HotConfig_Register("sickleharvest", (void*)&MAP_INFO_OFFSET, "MAP_INFO_OFFSET", HOT_RVA, 0x0d8);
    HotConfig_Register("sickleharvest", (void*)&MAP_SPATIAL_OWNER_OFFSET, "MAP_SPATIAL_OWNER_OFFSET", HOT_RVA, 0x030);
    HotConfig_Register("sickleharvest", (void*)&MAP_SPATIAL_INDEX_OFFSET, "MAP_SPATIAL_INDEX_OFFSET", HOT_RVA, 0x6e0);
    HotConfig_Register("sickleharvest", (void*)&PLAYER_OBJECT_STATUS_OFFSET, "PLAYER_OBJECT_STATUS_OFFSET", HOT_RVA, 0x32b8);
    HotConfig_Register("sickleharvest", (void*)&GIMMICK_DATA_HOLDER_OFFSET, "GIMMICK_DATA_HOLDER_OFFSET", HOT_RVA, 0x240);
    HotConfig_Register("sickleharvest", (void*)&GIMMICK_MODULE_NAME_OFFSET, "GIMMICK_MODULE_NAME_OFFSET", HOT_RVA, 0x0E0);
    HotConfig_Register("sickleharvest", (void*)&GIMMICK_POSITION_OFFSET, "GIMMICK_POSITION_OFFSET", HOT_RVA, 0x0f0);
    HotConfig_Poll();
    HotConfig_DumpCE("sickleharvest");
    InstallSickleHarvestHook();
}

extern "C" __declspec(dllexport) void mod_tick(void) {
    qol::budget::BeginFrame();  // P0: 帧锚定（幂等，多 DLL 安全）
    static int s_bdgSlot = -1;  // P0 探针（惰性注册，析构自动上报）
    if (s_bdgSlot < 0) s_bdgSlot = qol::budget::Slot("SickleHarvest");
    struct BdgGuard {
        int slot; uint64_t t0;
        ~BdgGuard() { qol::budget::Report(slot, qol::budget::NowUs() - t0); }
    } bdgGuard = { s_bdgSlot, qol::budget::NowUs() };

    if (QolGameBusy()) return;  // P1-2: 载入/菜单期间静默
    HotConfig_Poll();
    // Hook-based：安装后由游戏原生挥镰刀驱动，tick 无需额外逻辑
}

extern "C" __declspec(dllexport) void unload(void) {
    Log("[SickleHarvest] unload\n");
    g_sickleHarvestEnabled.store(false, std::memory_order_relaxed);
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    // H3: 还原 actionCheck hook
    if (g_originalCropsActionCheck) {
        WriteMem(reinterpret_cast<void*>(base + RVA_CROPS_ACTION_CHECK),
                 g_savedActionCheckBytes, ACTION_CHECK_HOOK_LENGTH);
        VirtualFree(g_originalCropsActionCheck, 0, MEM_RELEASE);
        g_originalCropsActionCheck = nullptr;
    }

    // H3: 还原 getTargetCrops hook
    if (g_originalGetSickleTargetCrops) {
        WriteMem(reinterpret_cast<void*>(base + RVA_SICKLE_GET_TARGET_CROPS),
                 g_savedSickleHookBytes, SICKLE_HOOK_LENGTH);
        VirtualFree(g_originalGetSickleTargetCrops, 0, MEM_RELEASE);
        g_originalGetSickleTargetCrops = nullptr;
    }

    g_sickleHarvestHookReady = false;
    LogClose();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
