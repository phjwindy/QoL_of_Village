// chestsort.cpp —— 箱子快速归类 (v1.4.3 正式版)
//
// v1.4.2-diag: H9 修复——5 处 itemAdjust 调用改为 SafeItemAdjust SEH 包装（防崩溃）；
//              D3 修复——回滚 itemAdjust 失败时记录日志+设置 rollbackFailed 标志。
// v1.4.0-diag: v1.20 适配——6 个 EXPECTED_PROLOGUE 签名数组从新 exe 实读更新；
//           6 个 RVA 常量更新至 v1.20 映射；回调 vtable 交叉验证通过。
// v1.3.5: 移除 Level 3 CAS 直写箱槽——用户反馈箱内物品消失/分堆异常，
//         根因是 CAS 直写绕过游戏原生物品管理，游戏不认 CAS 写入的物品。
//         回归 README 承诺：只往已有同类堆叠叠加，不创建新槽位。
//
// v1.3.4: 引用计数审计修复——①Level 1 正常完全合并路径补 InterlockedDecrement
//         释放保护性 AddRef（原仅 intrusiveRelease 释放背包引用，泄漏 +1）；②统一所有
//         "从背包移除"路径的释放顺序为先 Decrement(保护性) 再 Release(背包)，确保
//         独占引用时 Release 触发最终析构。
// v1.3.2: RVA prologue 验证——AOB 扫描定位函数后读回首字节确认有效函数序言。
// v1.3.1: 引用计数泄漏修复——部分转移路径(L1387)补 InterlockedDecrement，
//         与 !anyMoved 路径一致，防止保护性 AddRef 引用永不回收。
//         logging.cpp ASCII 快速路径（共享框架更新）。
//
// v1.3.0: CS-H1/H2 死档风险加固——Level 3 引用计数顺序修正（先 AddRef 后 CAS）、
//         写入后完整性验证（读回确认+物品数量验证）、背包槽位修改检测+回滚。
//         原代码先 CAS 后 AddRef，异常窗口致箱子持有无引用物品→双重释放风险。
//
// v1.2.8: 修复背包 UI 残留——batchUIRefresh(0xF1950) 只刷箱子侧 [obj+0x200]，
//        追加调用 batchUIRefresh-alt(0xF1310) 刷背包侧 [[obj+0x200]+0x30]。
// v1.2.7: 修复 batch_dirty AOB 匹配到 BTR(清除位) 而非 BTS(设置位)——AOB 末尾 0F B3→0F AB。
//        收尾三件套加诊断日志确认 batchDirty 正确执行。
// v1.2.6: 修复工作缓存复用导致箱内不合并——转移物品后未使 workCacheValid=false，
//         下次按键复用过期快照不知箱内已有同类堆，走 Level 3 新建堆而非 Level 1 合并。
//         对齐参考实现 nearby_chest_sort.inl：每次有转移即失效缓存，强制下次重建。
//         同时在 movedStacks>0 时先失效缓存再执行 batchRecalc/UIRefresh/batchDirty，
//         确保背包 UI 刷新前箱子侧缓存已标记为过期。
// v1.2.3: 修复 command_capacity 剩余空间被重复扣除（space=cap-target 应为 space=cap），
//         导致接近满的堆无法继续合并、误开新槽；修复 hasFullMatch 跨物品污染导致吸走无关物品
// v1.2.2: 恢复条件性 Level 3——箱子里有同类满堆 + 有空槽时，在空槽新建堆
// v1.2.1: 修复 Level 3 无条件新建堆导致"什么物品都塞进箱子"的问题
//         归类只做合并（Level 1 满堆跳过 + Level 2 拆分塞入），不再新建堆
//
// 触发键：手柄 D-pad Up（游戏内按键路由 command 0x3f8）
//         数字键 4（VK_4 = 0x34）作为键盘备选
// 原则：最近箱子优先。即使附近有多个箱子含同类物品，也只往最近的箱子放。
//
// 核心流程：
//   mod_init: AOB 扫描定位游戏函数 + 安装 main_update hook
//   main_update detour: 从 state+0x1B0 读 input → 检测 D-pad Up → 触发归类
//   mod_tick: 分片扫描注册表 + 数字键4备选触发
//             → 转移 → 刷新 UI
//
// 安全措施：
//   - IsReadable 检查所有指针访问（VirtualQuery 区域缓存加速）
//   - 转移前验证物品 itemId/rank/stackCount 一致性
//   - 转移后验证目标数量正确递增
//   - 任何异常立即停止，不继续操作

#include <windows.h>
#include <mmsystem.h>    // joyGetPosEx (winmm)
#include <setupapi.h>    // SetupAPI 枚举 HID 设备
#include <hidsdi.h>      // HidD_GetHidGuid / HidD_GetAttributes
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdint>
#pragma comment(lib, "winmm.lib")  // joyGetPosEx
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#include <limits.h>
#include <atomic>

// 已放弃 XInput 方案（DualSense 是 DirectInput 设备，XInput 不识别）
// 改用 HID 直读方案：SetupAPI 枚举 + ReadFile 读输入报告

#include "logging.h"
#include "budget.h"        // P0 帧耗时探针（跨 DLL 共享）
#include "state.h"         // P1-2 状态感知暂停（载入/菜单静默）
#include "hot_config.h"
#include "aobscan.h"
#include "hotkey.h"   // 共享热键运行时读取（游戏内改键链路）

// 日志开关：发布版禁用日志输出（不生成 qol_chestsort.log）
// 如需调试，取消下一行注释即可恢复日志输出
// #define CHESTSORT_LOGGING   // v1.4.5 转正：日志关闭（定位时取消注释重编）
// #define CHESTSORT_LOGGING
#ifdef CHESTSORT_LOGGING
#else
  #define LogOpen(x)  ((void)0)
  #define Log(...)    ((void)0)
  #define LogV(...)   ((void)0)
  #define LogClose()  ((void)0)
#endif

// [diag] 诊断日志门控（默认关）；调试时 #define DIAG_CHESTSORT 1 开启
#ifndef DIAG_CHESTSORT
#define DIAG_CHESTSORT 0
#endif

// ============================================================
// 常量（build 24969282 / v1.08.1）
// ============================================================
static volatile uintptr_t RVA_GAME_ROOT = 0x10FCBB0;  // 新 build 重定位

// ---- 手柄 D-pad Up 检测：XInput + HID + joyGetPosEx 三路混合方案 ----
// 路径 1: XInput 独立线程（Xbox 手柄，热插拔天然支持）
// 路径 2: HID 直读 DualSense 独立线程（DirectInput，独立线程 + 重新枚举热插拔）
// 路径 3: joyGetPosEx 同步回退（其他 DirectInput 手柄，无热插拔）
// 优先级: XInput > HID > joyGetPosEx，哪条路有手柄用哪条
//
// DualSense USB HID 输入报告布局（源自 Linux 内核 hid-playstation.c）:
//   byte 0 = Report ID (0x01)
//   byte 1-6 = 左摇杆XY/右摇杆XY/Z/RZ
//   byte 7 = seq_number（序列号，不是 D-pad！）
//   byte 8 = buttons[0]: bits 0-3 = D-pad Hat, bits 4-7 = □○△×
//   D-pad Hat: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8=Released
//   D-pad Up = hat 0 或 7
static constexpr unsigned short DUALSENSE_VID = 0x054C;
static constexpr unsigned short DUALSENSE_PID = 0x0CE6;
static constexpr size_t DUALSENSE_REPORT_SIZE = 64;
static constexpr int DUALSENSE_DPAD_OFFSET = 8;  // buttons[0] 字节偏移

static volatile uintptr_t ROOT_PLAYER_OFFSET = 0x208;
static volatile uintptr_t ROOT_MAP_OWNER_OFFSET = 0x268;
static volatile uintptr_t MAP_INFO_OFFSET = 0x0d8;
static volatile uintptr_t MAP_SPATIAL_OWNER_OFFSET = 0x030;
static volatile uintptr_t MAP_SPATIAL_INDEX_OFFSET = 0x6e0;

static volatile uintptr_t PLAYER_OBJECT_STATUS_OFFSET = 0x32b8;
static volatile uintptr_t PLAYER_OBJECT_LINK_A_OFFSET = 0x410;
static volatile uintptr_t PLAYER_OBJECT_LINK_B_OFFSET = 0x008;
static volatile uintptr_t PLAYER_OBJECT_ID_OFFSET = 0x028;
static volatile uintptr_t PLAYER_ITEMS_OFFSET = 0x32c0;

static volatile uintptr_t WORLD_OBJECT_POSITION_OFFSET = 0x230;
static volatile uintptr_t WORLD_OBJECT_DIRTY_OFFSET = 0x300;

static volatile uintptr_t STATUS_ITEMS_OFFSET = 0x2b8;
static volatile uintptr_t ITEM_DATA_HOLDER_OFFSET = 0x240;
static volatile uintptr_t ITEM_STACK_COUNT_OFFSET = 0x260;
static volatile uintptr_t ITEM_RANK_OFFSET = 0x280;

static volatile uintptr_t GIMMICK_DATA_HOLDER_OFFSET = 0x240;
static volatile uintptr_t GIMMICK_MODULE_NAME_OFFSET = 0x0E0;  // v1.20: 0xD8→0xE0（实测 data+0xE0 才是 moduleName）
static volatile uintptr_t GIMMICK_POSITION_OFFSET = 0x0f0;

static constexpr size_t CHEST_SLOT_COUNT = 30;
static constexpr size_t MAX_CANDIDATES   = 64;
static constexpr size_t MAX_SEARCH_RESULTS = 8192;
static constexpr size_t MAX_SEARCH_CAPACITY = 16384;
static constexpr float  SORT_RADIUS = 720.0f;
static constexpr float  SORT_RADIUS_SQ = SORT_RADIUS * SORT_RADIUS;

// 搜索回调 vtable —— MapSearchStatusInRectTemplate<CGimmickStatus>
// v1.09 (build 25094764): RTTI 链扫描确认 vtable 从 0xE17168 移至 0xE1C168
// 旧 0xE17168 在 v1.09 下 vtable[-1] 不指向有效 COL，vtable[4]=0x4EEB0 ≠ destroy(0x18C290)
// 新 vtable 通过 _Func_impl_no_alloc<lambda_1@MapSearchStatusInRectTemplate<CGimmickStatus>> RTTI 链定位
static volatile uintptr_t WORLD_OBJECT_REGISTRY_RVA = 0x1104C80;  // .data BSS 全局变量（v1.09 未变）
static volatile uintptr_t SEARCH_CALLBACK_VTABLE_RVA = 0xE3E608;   // v1.20 (build 25311578): ALL_OBJECTS vtable
static volatile uintptr_t SEARCH_CALLBACK_COPY_RVA = 0x28E480;   // v1.09: vt[0] (旧 0x280C50)
static volatile uintptr_t SEARCH_CALLBACK_INVOKE_RVA = 0x28E410;   // v1.09: vt[2] (旧 0x280BE0)
static volatile uintptr_t SEARCH_CALLBACK_DESTROY_RVA = 0x1969B0;   // v1.09: vt[4] (旧 0x18C210)

// ============================================================
// AOB 签名表（从 BigL233 源码验证块摘录）
// ============================================================
struct AOBDef {
    const char* name;
    const char* hex;
};

// batch_recalculate / batch_ui_refresh / batch_dirty 的 AOB 签名仅 15/14 字节
// （纯寄存器保存），全模块 455/5+ 匹配，AOB 无法定位。
// 通过 RVA 关系定位（旧 build recalc=dirty+0x660, ui=dirty-0x4C200）
// + 候选函数体特征验证（DL 读取 + 对象偏移模式），硬编码如下 RVA：
static volatile uintptr_t RVA_BATCH_RECALCULATE = 0x1470C0;  // v1.09(build 25094764) 重定位
static volatile uintptr_t RVA_BATCH_UI_REFRESH = 0xFB170;   // v1.09(build 25094764) 重定位
static volatile uintptr_t RVA_BATCH_UI_REFRESH_ALT = 0xFAB30; // v1.2.8: 背包侧 UI 刷新（[[obj+0x200]+0x30]）

// ---- 硬编码 RVA prologue 验证常量（从 v1.09 exe 实读） ----
// 用于 mod_init 阶段验证硬编码 RVA 仍指向正确函数，防止版本更新后地址偏移
static const unsigned char EXPECTED_BATCH_RECALC_PROLOGUE[8] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C
};
static const unsigned char EXPECTED_BATCH_UI_REFRESH_PROLOGUE[8] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74
};
static const unsigned char EXPECTED_BATCH_UI_REFRESH_ALT_PROLOGUE[8] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74
};
static const unsigned char EXPECTED_CB_COPY_PROLOGUE[8] = {
    0x48, 0x8D, 0x05, 0x81, 0x01, 0xBB, 0x00, 0x48
};
static const unsigned char EXPECTED_CB_INVOKE_PROLOGUE[8] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74
};
static const unsigned char EXPECTED_CB_DESTROY_PROLOGUE[8] = {
    0x48, 0x83, 0xEC, 0x28, 0x84, 0xD2, 0x74, 0x61
};

static const AOBDef kAOBs[] = {
    {"spatial_search",       "40 57 41 54 41 55 41 56 41 57 48 83 EC 60"},
    {"resolve_world_object", "48 89 5C 24 18 55 56 57 41 56 41 57 48 83 EC 40"},
    {"refresh_world_object", "4C 8B DC 49 89 5B 20 55 56 57 49 8D AB 48 FF"},
    {"raw_vector_free",      "48 83 EC 38 48 81 FA 00 10 00 00 72 14 48 8B"},
    {"intrusive_release",   "48 83 EC 28 48 8B 09 48 85 C9 74 21 8B 41"},
    {"item_adjust",         "48 89 5C 24 08 57 48 83 EC 20 48 8D B9 58 02"},
    {"player_capacity",     "48 8B 81 88 03 00 00 48 0F BA E0 16 73 06"},
    // batch_recalculate / batch_ui_refresh 的 AOB 短签名（15 字节，455/5+ 匹配）不可用。
    // 已从 AOB 表中移除——改用硬编码 RVA，见常量 RVA_BATCH_RECALCULATE / RVA_BATCH_UI_REFRESH。
    // command_capacity —— 剩余容量校验。v1.09 改 28 字节截断版（避开 E8 rel32，唯一匹配 0x4CB6B0），旧 65 字节完整签名含 E8 rel32 失配。
    // build 24969282 已验证：函数结构完全一致，仅内部 E8 call 偏移不同（正常）。
    {"command_capacity",   "40 53 48 83 EC 20 48 8B D9 48 8B 09 48 85 C9 75 0B B8 E7 03 00 00 48 83 C4 20 5B C3"},
    // batch_dirty —— 设置脏标记位。v1.2.7 修复：AOB 末尾 0F B3(BTR/清除位) → 0F AB(BTS/设置位)。
    // build 25094764: BTR 版 RVA=0x13AB60, BTS 版 RVA=0x13AAF0 (差 0x70, 前缀相同)。
    // BTR 清除位会导致 batchDirty 清除脏标记而非设置，UI 不刷新。BTS=设置位才是正确行为。
    {"batch_dirty",         "4C 8B D2 48 63 C2 48 C1 F8 06 41 83 E2 3F 4C 8B C9 41 B8 01 00 00 00 41 8B CA 49 D3 E0 4D 23 84 C1 90 00 00 00 48 63 C2 48 C1 F8 06 49 8D 0C C1 48 8B 81 90 00 00 00 4C 0F AB D0"},
};

// 索引对应 kAOBs 数组
enum AOBIndex {
    IDX_SPATIAL_SEARCH = 0,
    IDX_RESOLVE_WORLD_OBJECT,
    IDX_REFRESH_WORLD_OBJECT,
    IDX_RAW_VECTOR_FREE,
    IDX_INTRUSIVE_RELEASE,
    IDX_ITEM_ADJUST,
    IDX_PLAYER_CAPACITY,
    IDX_COMMAND_CAPACITY,
    IDX_BATCH_DIRTY,
};
static constexpr size_t kAOBCount = sizeof(kAOBs) / sizeof(kAOBs[0]);

// ============================================================
// 游戏函数指针类型
// ============================================================
struct Rect { float minX, minY, maxX, maxY; };

struct RawPointerVector { void** begin; void** end; void** capacity; };

// 空间搜索回调（仿函数布局）
struct SearchCallback {
    alignas(16) unsigned char storage[0x38];
    void* target;
};
static_assert(sizeof(SearchCallback) == 0x40, "callback size");
using FnSpatialSearch  = bool(__fastcall*)(void*, const Rect*, void*, int, int);
using FnRawVectorFree  = void(__fastcall*)(void*, size_t);
using FnIntrusiveRelease = void(__fastcall*)(void**);
using FnItemAdjust     = void(__fastcall*)(void*, int);
using FnPlayerCapacity = int(__fastcall*)(void*);
using FnBatchRecalc    = void(__fastcall*)(void*, bool);
using FnBatchUIRefresh = void(__fastcall*)(void*, bool);

// 权威 NearbyCommandItem 布局（nearby_chest_sort.inl 117-128 行）
// { void* item; u8 enabled; u8 pad[3]; int field0c; int state; u8 dirty; u8 pad[3]; }
struct CommandItem {
    void* item;
    unsigned char enabled;
    unsigned char padding09[3];
    int field0c;
    int state;
    unsigned char dirty;
    unsigned char padding15[3];
};
static_assert(sizeof(CommandItem) == 0x18, "native CommandItem layout changed");
using FnCommandCapacity = int(__fastcall*)(CommandItem*);
using FnBatchDirty     = void(__fastcall*)(void*, int);

// ---- 手柄 D-pad Up：XInput 独立线程 + HID 独立线程 + joyGetPosEx 同步回退 ----
// XInput 线程：Xbox 手柄热插拔天然支持，独立线程避免 mod_tick 卡死
// HID 线程：DualSense 直读，断开后重新枚举设备实现热插拔
// joyGetPosEx：其他 DirectInput 手柄，同步调用做最后回退
static volatile LONG g_xinputThreadRunning = 0;  // XInput 线程退出标志
static volatile LONG g_xinputDpadUp = 0;          // XInput D-pad Up 共享状态
static HANDLE g_xinputThread = nullptr;           // XInput 线程句柄
static DWORD WINAPI XInputPollThread(LPVOID);     // 前向声明（定义在 G 之后）
// XInput 函数指针（运行时从 xinput1_4.dll 加载）
static DWORD (WINAPI *g_XInputGetState)(DWORD dwUserIndex, void* pState) = nullptr;
static void LoadXInput();

// HID 线程全局变量（DualSense 直读）
static volatile LONG g_hidThreadRunning = 0;      // HID 线程退出标志
static volatile LONG g_hidDpadUp = 0;              // HID D-pad Up 共享状态
static volatile LONG g_hidConnected = 0;           // HID 手柄是否连接（1=连接, 0=断开）
static HANDLE g_hidThread = nullptr;               // HID 线程句柄
static volatile HANDLE g_hidDevice = nullptr;      // HID 设备句柄（unload 时 CloseHandle 迫使 ReadFile 失败）
static DWORD WINAPI HidPollThread(LPVOID);         // 前向声明（定义在 G 之后）

// ---- 共享热键（游戏内改键链路）：默认 "4, D-pad Up"，qol_hotkeys.txt 覆盖 ----
static QolHotKeys g_hotkeys;

// ============================================================
// 全局状态
// ============================================================

// 预缓存结构（必须在 G 命名空间之前定义）
struct CachedItem {
    void* item;       // 物品对象指针
    uint64_t itemId;
    int rank;
    int stackCount;
};

struct CachedChest {
    void* status;          // 箱子 status 指针
    float posX, posY;
    float distSq;
    CachedItem items[CHEST_SLOT_COUNT];
    int itemCount;
    void* itemsBegin;      // 箱子物品数组起始指针（用于新建堆时定位空槽）
    bool hasFullMatch;     // Level 3 标记：箱子里存在同类满堆（需条件性新建堆）
};

// 全局箱子注册表：搜索过的箱子永久保留，按4时只做坐标过滤
struct ChestEntry {
    void* status;
    float posX, posY;
};

static constexpr size_t MAX_CACHED_CHESTS = 64;
static constexpr size_t MAX_REGISTRY = 256;       // 全局箱子注册表上限
static constexpr float  REGISTRY_SEARCH_RADIUS = 720.0f;   // 每次搜索半径（与归类范围一致）
static constexpr float  WORK_CACHE_REUSE_DIST = 50.0f;    // 移动<50单位时复用工作缓存

namespace G {
    uintptr_t base = 0;
    bool ready = false;

    // 动态 RVA（运行时扫描 .data/.rdata 获得）
    uintptr_t rvaGameRoot = 0;
    uintptr_t rvaWorldRegistry = 0;
    uintptr_t rvaCallbackVTable = 0;       // CGimmickStatus vtable (primary)
    uintptr_t rvaCallbackVTableAlt = 0;   // CItemStatus vtable (fallback)

    FnSpatialSearch    spatialSearch    = nullptr;
    FnRawVectorFree    rawVectorFree    = nullptr;
    FnIntrusiveRelease intrusiveRelease = nullptr;
    FnItemAdjust       itemAdjust       = nullptr;
    FnPlayerCapacity   playerCapacity   = nullptr;
    FnBatchRecalc      batchRecalc      = nullptr;
    FnBatchUIRefresh   batchUIRefresh   = nullptr;
    FnCommandCapacity  commandCapacity  = nullptr;
    FnBatchDirty       batchDirty      = nullptr;

    // 手柄 D-pad Up 检测（三路混合：XInput 线程 + HID 线程 + joyGetPosEx 回退）
    bool padReady = false;           // joyGetPosEx 路径是否可用
    bool xinputReady = false;        // XInput 线程是否启动
    bool hidReady = false;           // HID 线程是否启动

    // D-pad Up 边沿触发状态（XInput 和 joyGetPosEx 共用）
    bool dpadUpNeedsRelease = true;  // 需要先释放再按下
    // joyGetPosEx 热插拔：手柄断开后用 joyConfigChanged 定期刷新
    DWORD padReconnectTimer = 0;
    // XInput 热插拔：断开后递增计数，定期重试
    DWORD xinputReconnectTimer = 0;

    // 全局箱子注册表（永久保留搜索过的箱子坐标）
    ChestEntry registry[MAX_REGISTRY] = {};
    size_t registryCount = 0;
    bool registryValid = false;
    float lastSearchX = 0.0f, lastSearchY = 0.0f;  // 上次注册表搜索中心

    // 注册表分片扫描状态（mod_tick 每帧处理一部分，不卡主线程）
    RawPointerVector pendingResults = {};   // spatialSearch 原始结果（待过滤）
    size_t scanIndex = 0;                   // 当前过滤到第几条
    bool scanPending = false;               // 是否还有待处理
    bool initialScanDone = false;           // v1.2.3: 注册表预热标志

    // 当前按4时的临时工作缓存
    CachedChest cachedChests[MAX_CACHED_CHESTS] = {};
    size_t cachedChestCount = 0;
    DWORD lastCacheTime = 0;
    bool workCacheValid = false;        // 工作缓存是否有效
    bool workCacheMoved = false;       // v1.2.6: 上次按键是否转移过物品（转移则缓存过期）
    float workCacheX = 0.0f, workCacheY = 0.0f;  // 工作缓存建立时玩家位置
}

// ============================================================
// 安全工具
// ============================================================

// VirtualQuery 区域缓存：同区域多次访问只查一次系统调用。
// 批量扫描（注册表/工作缓存）时性能提升巨大。
struct MemRegion {
    uintptr_t start;
    uintptr_t end;
};
static MemRegion g_fastRegions[16];
static int g_fastRegionCount = 0;

static void FastRegionReset() {
    g_fastRegionCount = 0;
}

static bool IsReadable(const void* p, size_t sz) {
    if (!p || sz == 0) return false;
    uintptr_t start = (uintptr_t)p;
    if (start < 0x10000 || start > 0x00007FFFFFFFFFFFULL) return false;
    // 命中缓存区域
    for (int i = 0; i < g_fastRegionCount; ++i) {
        if (start >= g_fastRegions[i].start &&
            start + sz <= g_fastRegions[i].end) return true;
    }
    // 未命中 → 真实 VirtualQuery
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    if (start + sz > regionEnd || start + sz < start) return false;
    // 记录该区域
    if (g_fastRegionCount < 16) {
        g_fastRegions[g_fastRegionCount] = {
            (uintptr_t)mbi.BaseAddress, regionEnd };
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

struct ItemInfo {
    uint64_t itemId;
    int stackCount;
    int rank;
};

static bool ReadItem(void* item, ItemInfo* out) {
    if (!item || !out) return false;
    if (!IsReadable(item, ITEM_RANK_OFFSET + sizeof(int))) return false;
    // 引用计数 > 0 才有效
    LONG refs = *(volatile LONG*)((uintptr_t)item + sizeof(void*));
    if (refs <= 0) return false;
    void* holder = *(void**)((uintptr_t)item + ITEM_DATA_HOLDER_OFFSET);
    void* data = nullptr;
    if (!holder || !ReadPtr(holder, 0, &data) ||
        !IsReadable(data, sizeof(uint64_t))) return false;
    out->itemId = *(const uint64_t*)data;
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

// 读取背包原始列表（RawPointerVector）
// 对照 BigL233 nearby_chest_sort.inl 第 624-641 行（权威实现）：
//   status+0x2b8 直接内嵌 RawPointerVector（begin/end/capacity 三个指针）
//   要求 count == CHEST_SLOT_COUNT（30），capacity <= 64
static bool ReadRawInventory(void* status, RawPointerVector* out) {
    if (!status || !out) return false;
    if (!IsReadable((void*)((uintptr_t)status + STATUS_ITEMS_OFFSET),
                    sizeof(RawPointerVector))) return false;
    memcpy(out, (const void*)((uintptr_t)status + STATUS_ITEMS_OFFSET),
           sizeof(RawPointerVector));
    const uintptr_t begin = (uintptr_t)out->begin;
    const uintptr_t end = (uintptr_t)out->end;
    const uintptr_t capacity = (uintptr_t)out->capacity;
    if (!begin || end < begin || capacity < end ||
        (end - begin) % sizeof(void*) != 0 ||
        (capacity - begin) % sizeof(void*) != 0) return false;
    const size_t count = (end - begin) / sizeof(void*);
    const size_t capacityCount = (capacity - begin) / sizeof(void*);
    return count == CHEST_SLOT_COUNT &&
           capacityCount >= count && capacityCount <= 64 &&
           IsReadable(out->begin, count * sizeof(void*));
}

// ============================================================
// 世界上下文
// ============================================================
struct WorldContext {
    void* player;
    float position[4];
    void* spatialIndex;
};

static bool GetWorldContext(WorldContext* ctx) {
    if (!ctx) return false;
    if (!G::rvaGameRoot || !G::rvaWorldRegistry) {
        Log("[ChestSort] [ctx] FAIL: 动态 RVA 未初始化 (gameRoot=0x%X registry=0x%X)",
            (unsigned)G::rvaGameRoot, (unsigned)G::rvaWorldRegistry);
        return false;
    }
    memset(ctx, 0, sizeof(*ctx));

    // 第 1 步：读取 game root 全局指针
    void** rootSlot = (void**)(G::base + G::rvaGameRoot);
    if (!IsReadable(rootSlot, sizeof(void*))) return false;
    void* root = *rootSlot;
    if (!root) return false;

    // 第 2 步：root+0x208 → player
    void* player = nullptr;
    if (!ReadPtr(root, ROOT_PLAYER_OFFSET, &player)) return false;

    // 第 3 步：player → objectStatus（= playerStatus，背包与位置读取通道）
    void* objectStatus = nullptr;
    if (!ReadPtr(player, PLAYER_OBJECT_STATUS_OFFSET, &objectStatus)) return false;

    // 第 4 步：玩家位置读取（修正为 BigL233 权威链路）
    //   night_map_markers.inl: root+0x208→save→save+0x32b8→playerStatus→+0xf0→position
    //   objectStatus 即 playerStatus，位置在 +0xf0（X/Y 平面，pos[1] 为 Y）
    const float* pos = (const float*)((uintptr_t)objectStatus + GIMMICK_POSITION_OFFSET);
    if (!IsReadable(pos, sizeof(float) * 4)) return false;
    memcpy(ctx->position, pos, sizeof(float) * 4);
    if (!std::isfinite(ctx->position[0]) || !std::isfinite(ctx->position[1])) return false;
    // 零位置保护：位置接近零时视为游戏世界未加载完成
    if (fabsf(ctx->position[0]) < 1.0f && fabsf(ctx->position[1]) < 1.0f) return false;

    // 第 5 步：root+0x268 → mapOwner → mapInfo → spatialOwner → spatialIndex
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
// 空间搜索附近箱子
// ============================================================
struct ChestCandidate {
    void* status;
    float posX, posY;  // X and Y (map plane in build 24969282)
    float distSq;
};

static void ReleaseSearchVector(RawPointerVector* v) {
    if (!v || !v->begin || !G::rawVectorFree) return;
    uintptr_t b = (uintptr_t)v->begin, c = (uintptr_t)v->capacity;
    size_t allocBytes = c >= b ? c - b : 0;
    if (allocBytes % sizeof(void*) == 0 && allocBytes <= MAX_SEARCH_CAPACITY * sizeof(void*)) {
        G::rawVectorFree(v->begin, allocBytes);
    }
    *v = {};
}

// 执行一次空间搜索，返回原始结果
// 对照 BigL233 nearby_chest_sort.inl 第 753-776 行（权威实现）：
//   storage+0x00 = vtable 地址
//   storage+0x08 = &filterId（u64 指针）
//   storage+0x10 = &results（结果向量指针）
//   target = storage（指向自身）
//   搜索完成后调用 vtable[4] destroy 释放
static bool DoSpatialSearch(void* spatialIndex, const Rect* bounds, uint64_t filterId,
                            RawPointerVector* results) {
    if (!spatialIndex || !bounds || !results) return false;
    memset(results, 0, sizeof(*results));
    if (!G::spatialSearch || !G::rvaCallbackVTable) {
        Log("[ChestSort] [search] callback vtable 未定位");
        return false;
    }
    Log("[ChestSort] [search] spatialSearch...", (unsigned)G::rvaCallbackVTable);

    SearchCallback callback = {};
    // BigL233 权威（nearby_chest_sort.inl 753-761 行）：
    //   storage+0x00 = vtable 地址
    //   storage+0x08 = &filterId（u64 指针，回调解引用读取/写入过滤值）
    //   storage+0x10 = &results（结果向量指针的地址）
    //   target = storage（指向自身）
    uint64_t localFilterId = filterId;   // 回调需要 filterId 的地址，必须用局部变量
    *reinterpret_cast<void**>(callback.storage + 0x00) =
        reinterpret_cast<void*>(G::base + G::rvaCallbackVTable);
    *reinterpret_cast<uint64_t**>(callback.storage + 0x08) = &localFilterId;
    // BigL233 权威中 results 是局部变量，storage+0x10 存 &results（向量对象地址）；
    // 此处 results 是参数（RawPointerVector*），直接存 results 即指向向量对象。
    *reinterpret_cast<RawPointerVector**>(callback.storage + 0x10) = results;
    callback.target = callback.storage;

    // 注意：与 BigL233 权威实现一致，spatialSearch 的返回值不可信（类型不符），
    // 直接调用并依赖 results 向量是否被填充来判断成功。
    __try {
        G::spatialSearch(spatialIndex, bounds, &callback, -1, -1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[ChestSort] [search] spatialSearch 调用异常 (SEH)");
        return false;
    }

    // 搜索完成，必须销毁回调（vtable[4] destroy），否则可能泄漏/崩溃
    if (callback.target) {
        __try {
            void** vtable = *reinterpret_cast<void***>(callback.target);
            using DestroyFunction = void(__fastcall*)(void*, bool);
            DestroyFunction destroy = reinterpret_cast<DestroyFunction>(vtable[4]);
            destroy(callback.target, callback.target != callback.storage);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[ChestSort] [search] destroy 调用异常 (SEH)");
        }
        callback.target = nullptr;
    }

    // 结果合理性校验（参照权威 778-790 行）
    if (!results->begin || !results->end || results->end < results->begin ||
        ((uintptr_t)results->end - (uintptr_t)results->begin) % sizeof(void*) != 0) {
        Log("[ChestSort] [search] spatial result rejected");
        return false;
    }
    return true;
}

// 判断状态是否为箱子（module name 精确匹配 "gimmick_chest"）
// 读取链参照 BigL233 权威实现 NearbyReadStatusType：
//   status+0x240 → holder → holder+0 → data → data+0xd8 → moduleName
static bool IsChestStatus(void* status) {
    if (!status) return false;
    // [diag] 记录每个环节失败原因，仅打印前 12 条，避免刷屏
    static int s_diagLogged = 0;
    const bool diag = (s_diagLogged < 12);
    auto Diag = [&](const char* reason) {
        if (diag) {
            ++s_diagLogged;
#if DIAG_CHESTSORT
            Log("[ChestSort][diag-ischest] status=%p REJECT: %s", status, reason);
#endif
        }
    };
    // 记录对象 vtable 前两个槽，帮助判断对象类型（CGimmickStatus 预期）
    if (diag) {
        uintptr_t vt0 = 0, vt16 = 0;
        if (IsReadable(status, 0x18)) {
            vt0 = *(const uintptr_t*)status;
            vt16 = *(const uintptr_t*)((uintptr_t)status + 0x10);
        }
#if DIAG_CHESTSORT
        Log("[ChestSort][diag-ischest] status=%p vtable0=0x%llX vtable+0x10=0x%llX",
            status, (unsigned long long)vt0, (unsigned long long)vt16);
#endif
    }
    void* holder = nullptr;
    if (!ReadPtr(status, GIMMICK_DATA_HOLDER_OFFSET, &holder)) {
        Diag("status+0x240 holder 读取失败");
        return false;
    }
    void* data = nullptr;
    if (!ReadPtr(holder, 0, &data)) {
        Diag("holder+0 data 读取失败");
        return false;
    }
    void* moduleNamePtr = nullptr;
    if (!ReadPtr(data, GIMMICK_MODULE_NAME_OFFSET, &moduleNamePtr)) {
        Diag("data+0xD8 moduleName 指针读取失败");
        return false;
    }
    if (!IsReadable(moduleNamePtr, 64)) {
        Diag("moduleName 内存不可读");
        return false;
    }
    // 有界读取 + 终止符校验（与 BigL233 权威实现一致，避免 strcmp 越界）
    char buf[64];
    size_t len = 0;
    while (len + 1 < sizeof(buf) && ((const char*)moduleNamePtr)[len] != '\0') ++len;
    if (((const char*)moduleNamePtr)[len] != '\0') {
        Diag("moduleName 64字节内未终止");
        return false;
    }
    memcpy(buf, moduleNamePtr, len + 1);
    if (strcmp(buf, "gimmick_chest") != 0) {
        Diag("moduleName 不匹配");
#if DIAG_CHESTSORT
        if (diag) Log("[ChestSort][diag-ischest]   moduleName='%s'", buf);
#endif
        return false;
    }
    return true;
}

// 搜索附近箱子并填充候选列表（按距离排序）
static size_t SearchChests(void* spatialIndex, const float* playerPos,
                           ChestCandidate* candidates) {
    if (!spatialIndex || !playerPos || !candidates) return 0;
    if (!G::rvaCallbackVTable) {
        Log("[ChestSort] [search] callback vtable 未定位");
        return 0;
    }

    Rect bounds = {
        playerPos[0] - SORT_RADIUS,
        playerPos[1] - SORT_RADIUS,
        playerPos[0] + SORT_RADIUS,
        playerPos[1] + SORT_RADIUS,
    };
    Log("[ChestSort] [search] spatialIndex=%p bounds=(%.1f,%.1f)-(%.1f,%.1f) (XY plane)",
        spatialIndex, bounds.minX, bounds.minY, bounds.maxX, bounds.maxY);

    RawPointerVector results = {};
    bool ok = DoSpatialSearch(spatialIndex, &bounds, 0, &results);
    if (!ok || !results.begin || !results.end) {
        Log("[ChestSort] [search] CGimmickStatus 无结果");
        ReleaseSearchVector(&results);  // 提前返回前释放，避免泄漏
        return 0;
    }
    size_t rawCount = results.end - results.begin;

    // 尝试 CItemStatus 回退（同 vtable 组）
    if (rawCount == 0 && G::rvaCallbackVTableAlt) {
        G::rvaCallbackVTable = G::rvaCallbackVTableAlt;
        ReleaseSearchVector(&results);
        ok = DoSpatialSearch(spatialIndex, &bounds, 0, &results);
        if (ok && results.begin && results.end) {
            rawCount = results.end - results.begin;
        }
    }

    size_t count = 0;
    for (size_t i = 0; i < rawCount && count < MAX_CANDIDATES; ++i) {
        void* status = results.begin[i];
        if (!status) continue;
        if (!IsChestStatus(status)) continue;
        const float* pos = (const float*)((uintptr_t)status + GIMMICK_POSITION_OFFSET);
        if (!IsReadable(pos, sizeof(float) * 4)) continue;
        if (!std::isfinite(pos[0]) || !std::isfinite(pos[1])) continue;
        float dx = pos[0] - playerPos[0];
        float dy = pos[1] - playerPos[1];
        float distSq = dx * dx + dy * dy;
        if (distSq > SORT_RADIUS_SQ) continue;
        candidates[count] = { status, pos[0], pos[1], distSq };
        ++count;
    }

    // 按距离排序（插入排序，count 很小）
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
// 全局箱子注册表：一次性大范围搜索，永久保留坐标
// ============================================================

// 用 720 半径搜索当前区域，将新发现的箱子增量加入注册表
// 玩家走到新区域时调用，之前搜过的区域不会重复搜索
// 开始一次注册表扫描：只调 spatialSearch（~0ms），结果存入 pendingResults
// 后续由 ProcessRegistryChunk 分片过滤，避免阻塞主线程
static void StartRegistryScan(const WorldContext* ctx) {
    if (!ctx || !ctx->spatialIndex) return;
    FastRegionReset();  // 批量扫描前重置区域缓存

    // 清理上一次未完成的扫描
    if (G::scanPending) {
        ReleaseSearchVector(&G::pendingResults);
        G::scanPending = false;
    }

    Rect bounds = {
        ctx->position[0] - REGISTRY_SEARCH_RADIUS,
        ctx->position[1] - REGISTRY_SEARCH_RADIUS,
        ctx->position[0] + REGISTRY_SEARCH_RADIUS,
        ctx->position[1] + REGISTRY_SEARCH_RADIUS,
    };
    Log("[ChestSort] [registry] 搜索 bounds=(%.0f,%.0f)-(%.0f,%.0f)",
        bounds.minX, bounds.minY, bounds.maxX, bounds.maxY);

    RawPointerVector results = {};
    bool ok = DoSpatialSearch(ctx->spatialIndex, &bounds, 0, &results);
    if (!ok || !results.begin || !results.end) {
        Log("[ChestSort] [registry] 搜索无结果");
        ReleaseSearchVector(&results);
        G::registryValid = true;
        G::lastSearchX = ctx->position[0];
        G::lastSearchY = ctx->position[1];
        return;
    }
    size_t rawCount = results.end - results.begin;
    Log("[ChestSort] [registry] 原始结果 %zu 条", rawCount);

    // 保存到 pending，等待分片过滤
    G::pendingResults = results;
    G::scanIndex = 0;
    G::scanPending = true;
    G::lastSearchX = ctx->position[0];
    G::lastSearchY = ctx->position[1];
}

// 分片过滤：每帧只处理 SCAN_CHUNK 条结果，处理完释放向量
static constexpr size_t SCAN_CHUNK = 64;  // v1.2.4: 12→64（615条 → ~10帧，约0.17秒无感完成）
static void ProcessRegistryChunk() {
    if (!G::scanPending) return;

    RawPointerVector& results = G::pendingResults;
    size_t rawCount = (uintptr_t)results.end - (uintptr_t)results.begin;
    rawCount /= sizeof(void*);

    size_t stop = G::scanIndex + SCAN_CHUNK;
    if (stop > rawCount) stop = rawCount;

    for (; G::scanIndex < stop && G::registryCount < MAX_REGISTRY; ++G::scanIndex) {
        void* status = results.begin[G::scanIndex];
        if (!status) continue;
        if (!IsChestStatus(status)) continue;
        const float* pos = (const float*)((uintptr_t)status + GIMMICK_POSITION_OFFSET);
        if (!IsReadable(pos, sizeof(float) * 4)) continue;
        if (!std::isfinite(pos[0]) || !std::isfinite(pos[1])) continue;

        // 去重
        bool dup = false;
        for (size_t j = 0; j < G::registryCount; ++j) {
            if (G::registry[j].status == status) { dup = true; break; }
        }
        if (dup) continue;

        G::registry[G::registryCount] = { status, pos[0], pos[1] };
        G::registryCount++;
    }

    if (G::scanIndex >= rawCount || G::registryCount >= MAX_REGISTRY) {
        // 扫描完成
        G::registryValid = true;
        ReleaseSearchVector(&results);
        G::scanPending = false;
        Log("[ChestSort] [registry] 扫描完成: 注册表共 %zu 个箱子", G::registryCount);
    }
}

// 从注册表中按坐标过滤附近的箱子，实时读取物品
static void BuildWorkCache(const WorldContext* ctx) {
    FastRegionReset();  // 批量扫描前重置区域缓存
    G::cachedChestCount = 0;
    for (size_t i = 0; i < G::registryCount && G::cachedChestCount < MAX_CACHED_CHESTS; ++i) {
        float dx = G::registry[i].posX - ctx->position[0];
        float dy = G::registry[i].posY - ctx->position[1];
        float distSq = dx * dx + dy * dy;
        if (distSq > SORT_RADIUS_SQ) continue;

        CachedChest& cc = G::cachedChests[G::cachedChestCount];
        cc.status = G::registry[i].status;
        cc.posX = G::registry[i].posX;
        cc.posY = G::registry[i].posY;
        cc.distSq = distSq;
        cc.itemCount = 0;
        cc.hasFullMatch = false;

        // 实时读取箱子物品
        RawPointerVector inv = {};
        cc.itemsBegin = nullptr;
        if (ReadRawInventory(cc.status, &inv)) {
            cc.itemsBegin = inv.begin;  // 记录物品数组起始指针，供新建堆用
            size_t slots = inv.end - inv.begin;
            for (size_t s = 0; s < slots && cc.itemCount < CHEST_SLOT_COUNT; ++s) {
                void* ci = inv.begin[s];
                if (!ci) continue;
                ItemInfo info = {};
                if (!ReadItem(ci, &info)) continue;
                cc.items[cc.itemCount] = { ci, info.itemId, info.rank, info.stackCount };
                cc.itemCount++;
            }
        }
        G::cachedChestCount++;
    }

    G::workCacheValid = true;
    G::workCacheX = ctx->position[0];
    G::workCacheY = ctx->position[1];
    G::lastCacheTime = GetTickCount();
    Log("[ChestSort] [cache] 工作缓存重建: %zu 个箱子（注册表共 %zu）",
        G::cachedChestCount, G::registryCount);
}

// ============================================================
// input 系统：通过游戏内按键路由检测 D-pad Up
// ============================================================

// 前向声明（DoSort 定义在后面）
static void DoSort(WorldContext* ctx);

// ============================================================
// 手柄 D-pad Up 检测（XInput + joyGetPosEx 混合方案）
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
#define XINPUT_GAMEPAD_DPAD_UP 0x0001

// 动态加载 xinput1_4.dll（Windows 8+），回退 xinput1_3.dll（Windows 7）
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

// XInput 独立轮询线程：与 Teleport MOD 相同架构，避免 mod_tick 卡死
// Xbox 手柄热插拔天然支持——断开返回 ERROR_DEVICE_NOT_CONNECTED，
// 重连后自动恢复，无需特殊处理
static DWORD WINAPI XInputPollThread(LPVOID) {
    Log("[ChestSort] [xinput] 线程启动");
    bool wasConnected = false;
    DWORD emptyCount = 0;

    while (InterlockedCompareExchange(&g_xinputThreadRunning, 1, 1) == 1) {
        if (!g_XInputGetState) { Sleep(2000); continue; }

        XINPUT_STATE state = {};
        DWORD result = g_XInputGetState(0, &state);  // UserIndex 0

        if (result == ERROR_SUCCESS) {
            bool dpadUp = (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
            InterlockedExchange(&g_xinputDpadUp, dpadUp ? 1 : 0);

            if (!wasConnected) {
                wasConnected = true;
                Log("[ChestSort] [xinput] 手柄已连接");
            }
            emptyCount = 0;
        } else {
            // ERROR_DEVICE_NOT_CONNECTED (1167) = 手柄断开
            InterlockedExchange(&g_xinputDpadUp, 0);
            if (wasConnected) {
                wasConnected = false;
                Log("[ChestSort] [xinput] 手柄断开 (err=%lu)", result);
            }
        }
        Sleep(8);  // ~120Hz 采样，足够检测 D-pad 按键
    }

    Log("[ChestSort] [xinput] 线程退出");
    return 0;
}

// ============================================================
// DualSense HID 直读（独立线程，热插拔重连）
// ============================================================

// 枚举 HID 设备找到 DualSense 的设备路径并打开
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
            free(detail);
            continue;
        }

        HANDLE hDev = CreateFileW(detail->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        free(detail);

        if (hDev == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attr = {};
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(hDev, &attr) &&
            attr.VendorID == DUALSENSE_VID &&
            attr.ProductID == DUALSENSE_PID) {
            SetupDiDestroyDeviceInfoList(hDevInfo);
            return hDev;
        }
        CloseHandle(hDev);
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return nullptr;
}

// DualSense HID 轮询线程
// 独立线程 ReadFile 阻塞读取输入报告，解析 D-pad Hat
// 热插拔：ReadFile 失败后关闭句柄，重新枚举设备（OpenDualSense）实现重连
static DWORD WINAPI HidPollThread(LPVOID) {
    Log("[ChestSort] [hid] 线程启动");
    bool wasConnected = false;

    while (InterlockedCompareExchange(&g_hidThreadRunning, 1, 1) == 1) {
        // 枚举并打开 DualSense
        HANDLE hDev = OpenDualSense();
        if (!hDev) {
            if (wasConnected) {
                wasConnected = false;
                InterlockedExchange(&g_hidConnected, 0);
                InterlockedExchange(&g_hidDpadUp, 0);
                Log("[ChestSort] [hid] DualSense 断开，等待重连...");
            }
            Sleep(2000);  // 每 2 秒重新枚举一次
            continue;
        }

        // 设备打开成功
        if (!wasConnected) {
            wasConnected = true;
            InterlockedExchange(&g_hidConnected, 1);
            Log("[ChestSort] [hid] DualSense 已连接");
        }
        InterlockedExchangePointer(&g_hidDevice, hDev);

        // 读取输入报告循环
        BYTE report[DUALSENSE_REPORT_SIZE] = {};
        while (InterlockedCompareExchange(&g_hidThreadRunning, 1, 1) == 1) {
            __try {
                DWORD bytesRead = 0;
                BOOL ok = ReadFile(hDev, report, DUALSENSE_REPORT_SIZE, &bytesRead, nullptr);
                if (!ok || bytesRead < 9) {
                    // ReadFile 失败 = 设备断开
                    Log("[ChestSort] [hid] ReadFile 失败 (ok=%d bytesRead=%lu)", ok ? 1 : 0, bytesRead);
                    break;  // 跳出内循环，回到外循环重新枚举
                }

                // DualSense USB 报告: report[0]=0x01 (Report ID)
                // report[8] = buttons[0] 低 4 位 = D-pad Hat
                // (源自 Linux 内核 hid-playstation.c: struct dualsense_input_report)
                if (report[0] == 0x01 && bytesRead > DUALSENSE_DPAD_OFFSET) {
                    int hat = report[DUALSENSE_DPAD_OFFSET] & 0x0F;
                    // hat: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8=Released
                    // D-pad Up = hat 0(N) 或 1(NE) 或 7(NW)
                    bool dpadUp = (hat <= 1 || hat == 7);
                    InterlockedExchange(&g_hidDpadUp, dpadUp ? 1 : 0);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedExchange(&g_hidDpadUp, 0);
                break;
            }
        }

        CloseHandle(hDev);
        InterlockedExchangePointer(&g_hidDevice, nullptr);
        InterlockedExchange(&g_hidConnected, 0);
        InterlockedExchange(&g_hidDpadUp, 0);
        // 回到外循环重新枚举设备（热插拔重连）
    }

    Log("[ChestSort] [hid] 线程退出");
    return 0;
}

// ---- joyGetPosEx 辅助 ----
// joyConfigChanged 强制 WinMM 重新枚举设备（解决热插拔后 WinMM 不刷新）
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

// 初始化手柄检测：启动 XInput 线程 + HID 线程 + joyGetPosEx 探测
static void InitGamepad() {
    // 1. XInput 独立线程（Xbox 手柄，热插拔天然支持）
    LoadXInput();
    if (g_XInputGetState) {
        InterlockedExchange(&g_xinputThreadRunning, 1);
        g_xinputThread = CreateThread(nullptr, 0, XInputPollThread, nullptr, 0, nullptr);
        if (g_xinputThread) {
            G::xinputReady = true;
            Log("[ChestSort] [xinput] 线程已创建");
        }
    } else {
        Log("[ChestSort] [xinput] xinput DLL 未找到，Xbox 手柄检测不可用");
    }

    // 2. HID 独立线程（DualSense 直读，热插拔重连）
    InterlockedExchange(&g_hidThreadRunning, 1);
    g_hidThread = CreateThread(nullptr, 0, HidPollThread, nullptr, 0, nullptr);
    if (g_hidThread) {
        G::hidReady = true;
        Log("[ChestSort] [hid] 线程已创建");
    }

    // 3. joyGetPosEx 探测（其他 DirectInput 手柄最后回退）
    LoadJoyConfigChanged();
    JOYINFOEX ji = {};
    ji.dwSize = sizeof(ji);
    ji.dwFlags = JOY_RETURNPOV;
    MMRESULT res = joyGetPosEx(JOYSTICKID1, &ji);
    if (res == JOYERR_NOERROR) {
        G::padReady = true;
        Log("[ChestSort] [pad] joyGetPosEx 探测成功，DirectInput 手柄已连接");
    } else {
        G::padReady = false;
        Log("[ChestSort] [pad] joyGetPosEx 未检测到手柄 (err=%d)，运行时自动重试", res);
    }
}

// 检测 D-pad Up 是否被按下（边沿触发）
// 三路优先级: XInput 共享变量 > HID 共享变量 > joyGetPosEx 同步调用
static bool CheckDpadUpPressed() {
    bool dpadUp = false;

    // ---- 路径 1: XInput 共享变量读（<0.01ms）----
    if (G::xinputReady && InterlockedOr(&g_xinputDpadUp, 0)) {
        dpadUp = true;
    }

    // ---- 路径 2: HID 共享变量读（<0.01ms，DualSense 直读）----
    if (!dpadUp && G::hidReady) {
        // HID 线程在连接手柄时会设 g_hidConnected=1
        if (InterlockedOr(&g_hidConnected, 0)) {
            dpadUp = InterlockedOr(&g_hidDpadUp, 0) != 0;
        }
    }

    // ---- 路径 3: joyGetPosEx 同步回退（<1ms，其他 DirectInput 手柄）----
    // 仅当 XInput 和 HID 都没有手柄时才调用，避免重复检测
    if (!dpadUp && !G::xinputReady && !(G::hidReady && InterlockedOr(&g_hidConnected, 0))) {
        JOYINFOEX ji = {};
        ji.dwSize = sizeof(ji);
        ji.dwFlags = JOY_RETURNPOV;
        MMRESULT res = joyGetPosEx(JOYSTICKID1, &ji);

        if (res == JOYERR_NOERROR) {
            if (!G::padReady) {
                G::padReady = true;
                G::padReconnectTimer = 0;
                G::dpadUpNeedsRelease = true;
                Log("[ChestSort] [pad] DirectInput 手柄已连接");
            }
            DWORD pov = ji.dwPOV;
            dpadUp = (pov != 0xFFFF && pov != JOY_POVCENTERED) &&
                     (pov <= 4500 || pov >= 31500);
        } else {
            if (G::padReady) {
                G::padReady = false;
                G::padReconnectTimer = 0;
                Log("[ChestSort] [pad] DirectInput 手柄断开 (err=%d)", res);
            }
            G::padReconnectTimer++;
            if (G::padReconnectTimer % 120 == 0) {
                if (g_joyConfigChanged) g_joyConfigChanged();
            }
        }
    }

    // ---- 边沿触发 ----
    bool pressed = false;
    if (G::dpadUpNeedsRelease) {
        if (!dpadUp) G::dpadUpNeedsRelease = false;
    } else if (dpadUp) {
        pressed = true;
        G::dpadUpNeedsRelease = true;
    }
    return pressed;
}

// ============================================================
// H9/D3: itemAdjust SEH 安全包装
// 所有 G::itemAdjust 调用改为 SafeItemAdjust，防止原生函数异常导致崩溃。
// 回滚路径返回 false 时记录日志并设置 rollbackFailed 标志。
// ============================================================
__declspec(noinline) static bool SafeItemAdjust(void* item, int delta) {
    if (!item || !G::itemAdjust) return false;
    __try {
        G::itemAdjust(item, delta);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ============================================================
// 主归类逻辑
// ============================================================
static void DoSort(WorldContext* ctx) {
    if (!ctx || !ctx->player) return;

    // === 第一步：注册表增量更新 ===
    // 走到新区域（离上次搜索中心>720）才补搜，已搜过的不重复。
    // 搜索立即返回（spatialSearch ~0ms），过滤在 mod_tick 分片完成。
    if (!G::scanPending) {
        if (!G::registryValid) {
            StartRegistryScan(ctx);
        } else {
            float sdx = ctx->position[0] - G::lastSearchX;
            float sdy = ctx->position[1] - G::lastSearchY;
            if (sdx*sdx + sdy*sdy > SORT_RADIUS_SQ) {
                StartRegistryScan(ctx);
            }
        }
    }

    // v1.2.4: 同步排空改为限时处理——最多处理 MAX_REGISTRY 条，
    // 避免极端情况下一帧内处理 615 条造成长时间卡顿。
    // 正常情况下预热已在 ~10 帧内完成，此分支几乎不会触发。
    size_t syncBudget = MAX_REGISTRY;
    while (G::scanPending && syncBudget > 0) {
        size_t before = G::scanIndex;
        ProcessRegistryChunk();
        size_t processed = G::scanIndex - before;
        if (processed == 0) break;  // 防死循环
        syncBudget -= (processed < syncBudget) ? processed : syncBudget;
    }

    // === 第二步：工作缓存复用 ===
    // v1.2.6: 仅当上次没有转移物品（workCacheMoved==false）且没走远（<50单位）时复用缓存。
    //         有转移则缓存中箱子物品快照已过期（数量/槽位变化），必须重建。
    if (G::workCacheValid && !G::workCacheMoved) {
        float wdx = ctx->position[0] - G::workCacheX;
        float wdy = ctx->position[1] - G::workCacheY;
        if (wdx*wdx + wdy*wdy <= WORK_CACHE_REUSE_DIST * WORK_CACHE_REUSE_DIST) {
            Log("[ChestSort] [cache] 复用工作缓存（移动<%d）",
                (int)sqrtf(wdx*wdx+wdy*wdy));
        } else {
            G::workCacheValid = false;
        }
    } else {
        G::workCacheValid = false;
    }
    G::workCacheMoved = false;  // 重置标记，本轮重新统计
    if (!G::workCacheValid) {
        BuildWorkCache(ctx);
    }
    if (G::cachedChestCount == 0) {
        Log("[ChestSort] 附近无可归类箱子");
        return;
    }

    // 读取玩家背包
    int capacity = -1;
    if (G::playerCapacity) capacity = G::playerCapacity(ctx->player);
    if (capacity != 10 && capacity != 20 && capacity != 30) {
        Log("[ChestSort] 无效玩家容量: %d", capacity);
        return;
    }
    void** slots = reinterpret_cast<void**>(
        (uintptr_t)ctx->player + PLAYER_ITEMS_OFFSET);
    if (!IsReadable(slots, (size_t)capacity * sizeof(void*))) return;

    size_t slotCount = (size_t)capacity;
    size_t movedStacks = 0;
    size_t movedItems = 0;
    bool rollbackFailed = false;  // D3: 回滚失败标志

    for (size_t i = 0; i < slotCount; ++i) {
        void* item = slots[i];
        if (!item) continue;
        ItemInfo info = {};
        if (!ReadItem(item, &info)) continue;

        if (!G::itemAdjust) continue;

        // 重置 Level 3 标记：每个背包物品独立判断，避免上一物品的满堆标记污染当前物品
        for (size_t c = 0; c < G::cachedChestCount; ++c) {
            G::cachedChests[c].hasFullMatch = false;
        }

        // 持有引用保护
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(
            reinterpret_cast<uintptr_t>(item) + sizeof(void*)));

        int remaining = info.stackCount;  // 本物品还剩多少要转移
        bool anyMoved = false;

        // ================================================================
        // 归类策略：合并到已有同类堆 + 条件性新建堆
        // Level 1: 找能装下的同类堆合并
        // Level 2: 同类堆都满了就拆分塞入
        // Level 3: 箱子里有同类满堆 + 有空槽 → 在空槽新建堆
        //          （仅对有同类满堆的箱子新建，不会把无同类物品塞进去）
        // ================================================================
        for (size_t c = 0; c < G::cachedChestCount && remaining > 0; ++c) {
            CachedChest& cc = G::cachedChests[c];
            bool chestMatched = false;
            bool diagLoggedRank = false;   // [diag] 每箱只记一次 同ID异Rank
            bool diagLoggedFull = false;   // [diag] 每箱只记一次 满堆
            for (int s = 0; s < cc.itemCount && remaining > 0; ++s) {
                if (cc.items[s].itemId != info.itemId ||
                    cc.items[s].rank != info.rank) {
                    // [diag] 排查"杂草不堆叠"：同 itemId 但 rank 不同
                    if (!diagLoggedRank && cc.items[s].itemId == info.itemId) {
                        diagLoggedRank = true;
#if DIAG_CHESTSORT
                        Log("[ChestSort] [diag] 同ID异rank: itemId=%llu 背包rank=%d 箱内rank=%d",
                            (unsigned long long)info.itemId, info.rank, cc.items[s].rank);
#endif
                    }
                    continue;
                }
                chestMatched = true;

                void* targetItem = cc.items[s].item;
                int targetBefore = 0;
                if (!ReadStackCount(targetItem, &targetBefore)) continue;

                // 容量校验
                // command_capacity 返回"剩余可放空间"（堆上限 - 当前数量），
                // 例如 target=885 时返回 114。直接用 cap 作为剩余空间，
                // 不要再用 cap - targetBefore 重复扣除（会得到负数误判满堆）。
                int cap = 999;
                if (G::commandCapacity) {
                    CommandItem descriptor = {};
                    descriptor.item = targetItem;
                    cap = G::commandCapacity(&descriptor);
                }
                int space = cap;  // 剩余可放空间
                if (space <= 0) {
                    continue;  // 此堆已满，找下一个
                }

                int moveCount = (remaining < space) ? remaining : space;

                // H9: forward itemAdjust — SafeItemAdjust SEH 包装
                if (!SafeItemAdjust(targetItem, moveCount)) {
                    Log("[ChestSort] forward itemAdjust failed: item=%p delta=%d (target)",
                        targetItem, moveCount);
                    continue;
                }
                int targetAfter = 0;
                if (!ReadStackCount(targetItem, &targetAfter) ||
                    targetAfter != targetBefore + moveCount) {
                    // D3: rollback 失败时记录日志+设置标志
                    if (!SafeItemAdjust(targetItem, -moveCount)) {
                        int curCount = -1;
                        ReadStackCount(targetItem, &curCount);
                        Log("[ChestSort] rollback itemAdjust failed: item=%p delta=%d currentCount=%d",
                            targetItem, -moveCount, curCount);
                        rollbackFailed = true;
                    }
                    continue;
                }
                // H9: forward itemAdjust — SafeItemAdjust SEH 包装
                if (!SafeItemAdjust(item, -moveCount)) {
                    Log("[ChestSort] forward itemAdjust failed: item=%p delta=%d (source)",
                        item, -moveCount);
                    // 回滚已添加到 target 的数量
                    if (!SafeItemAdjust(targetItem, -moveCount)) {
                        int curCount = -1;
                        ReadStackCount(targetItem, &curCount);
                        Log("[ChestSort] rollback itemAdjust failed: item=%p delta=%d currentCount=%d",
                            targetItem, -moveCount, curCount);
                        rollbackFailed = true;
                    }
                    continue;
                }
                int sourceAfter = 0;
                if (!ReadStackCount(item, &sourceAfter) ||
                    sourceAfter != remaining - moveCount) {
                    // D3: rollback 失败时记录日志+设置标志
                    if (!SafeItemAdjust(targetItem, -moveCount)) {
                        int curCount = -1;
                        ReadStackCount(targetItem, &curCount);
                        Log("[ChestSort] rollback itemAdjust failed: item=%p delta=%d currentCount=%d",
                            targetItem, -moveCount, curCount);
                        rollbackFailed = true;
                    }
                    if (!SafeItemAdjust(item, moveCount)) {
                        int curCount = -1;
                        ReadStackCount(item, &curCount);
                        Log("[ChestSort] rollback itemAdjust failed: item=%p delta=%d currentCount=%d",
                            item, moveCount, curCount);
                        rollbackFailed = true;
                    }
                    continue;
                }

                remaining -= moveCount;
                anyMoved = true;
                movedItems += moveCount;

                // 更新缓存中目标堆的数量
                cc.items[s].stackCount += moveCount;

                Log("[ChestSort] 合并: itemId=%llu rank=%d x%d (堆 %zu/%d)",
                    (unsigned long long)info.itemId, info.rank, moveCount, c, s);
            }
            // 如果这个箱子有同类堆但都满了（chestMatched && remaining > 0），
            // 标记需要 Level 3（条件性新建堆）
            if (chestMatched && remaining > 0) {
                cc.hasFullMatch = true;
            }
        }

        // ================================================================
        // v1.3.5: Level 3 已移除——CAS 直写箱槽绕过游戏原生物品管理，
        // 导致箱内物品不被游戏识别，取出时消失/分堆异常。
        // 回归 README 承诺：只往已有同类堆叠叠加，不创建新槽位。
        // 满堆+空位时背包同类物品无法放入是预期行为（安全优先）。
        // ================================================================

        // 合并完成后处理背包槽位
        if (anyMoved && remaining == 0) {
            // 物品已全部转移（Level 1 合并到 0）
            // v1.3.5: Level 3 已移除，此分支仅处理 Level 1 完全合并
            //   此时 ReadStackCount 返回原始 stackCount（≠0），不走 CAS 清空分支。
            //   但保护性 AddRef（L1217）未释放 → 需在此补释放。
            //
            // v1.3.1-bugfix: Level 1 完全合并路径，itemAdjust(item, -moveCount)
            //   将 stackCount 减到 0。但如果 itemAdjust 有内部自动释放逻辑
            //   （stack→0 时 release），或 stackCount 变为负值/越界值，
            //   ReadStackCount 会返回 false → 原代码跳过 CAS 清空 + 释放，
            //   导致背包槽位悬空（指向已耗尽物品）+ 保护性 AddRef 泄漏。
            //   修复：无论 ReadStackCount 是否成功，都必须清空背包槽位 + 释放保护性 AddRef。
            int sourceAfter = -1;
            bool countOk = ReadStackCount(item, &sourceAfter);
            if (countOk && sourceAfter == 0) {
                // 正常路径：stackCount=0，清空背包槽
                // v1.3.4/P0-4+P1-7: 先 Decrement(保护性AddRef) 再 Release(背包引用)，
                //   统一释放顺序——Decrement 先减保护引用，Release 再减背包引用
                //   若 refcount→0 则 Release 内部触发析构
                void* observed = InterlockedCompareExchangePointer(
                    reinterpret_cast<void* volatile*>(&slots[i]), nullptr, item);
                if (observed == item) {
                    InterlockedDecrement(reinterpret_cast<volatile LONG*>(
                        reinterpret_cast<uintptr_t>(item) + sizeof(void*)));
                    void* slotOwnedRef = item;
                    if (G::intrusiveRelease) G::intrusiveRelease(&slotOwnedRef);
                }
                movedStacks++;
            } else if (!countOk || sourceAfter != 0) {
                // v1.3.5: Level 3 已移除，此路径仅处理 itemAdjust 内部释放情况
                // 物品已不在背包有效状态，强制清空背包槽位 + 释放保护性 AddRef
                // v1.3.5: Level 3 已移除，此分支仅处理 itemAdjust 内部释放
                // Level 1 异常路径：背包槽位仍指向物品，此处 CAS 清空 + 释放保护性 AddRef
                void* observed = InterlockedCompareExchangePointer(
                    reinterpret_cast<void* volatile*>(&slots[i]), nullptr, item);
                if (observed == item) {
                    // v1.3.4/P1-7: 先 Decrement(保护性) 再 Release(背包引用)，统一顺序
                    InterlockedDecrement(reinterpret_cast<volatile LONG*>(
                        reinterpret_cast<uintptr_t>(item) + sizeof(void*)));
                    void* slotOwnedRef = item;
                    if (G::intrusiveRelease) G::intrusiveRelease(&slotOwnedRef);
                }
                // v1.3.5: Level 3 已移除，此处 CAS 通常成功（observed == item）
                if (observed != item) {
                    InterlockedDecrement(reinterpret_cast<volatile LONG*>(
                        reinterpret_cast<uintptr_t>(item) + sizeof(void*)));
                }
                if (!countOk) {
                    Log("[ChestSort] [bugfix] ReadStackCount 失败，强制清空背包槽: itemId=%llu (箱内已合并)",
                        (unsigned long long)info.itemId);
                } else {
                    // sourceAfter != 0：Level 3 整物品已移入箱子，保护性 AddRef 需释放
                    // 不记日志（正常 Level 3 路径）
                }
                movedStacks++;
            }
        } else if (anyMoved && remaining > 0) {
            // 只转移了一部分（拆分转移后还有剩余），背包槽位不清空
            // v1.3.1: 释放 L1190 的保护性 AddRef，防止引用泄漏
            InterlockedDecrement(reinterpret_cast<volatile LONG*>(
                reinterpret_cast<uintptr_t>(item) + sizeof(void*)));
            Log("[ChestSort] 部分转移: itemId=%llu 剩余 %d 未转移",
                (unsigned long long)info.itemId, remaining);
            movedStacks++;
        }

        if (!anyMoved) {
            // 释放引用保护
            InterlockedDecrement(reinterpret_cast<volatile LONG*>(
                reinterpret_cast<uintptr_t>(item) + sizeof(void*)));
            Log("[ChestSort] 跳过: itemId=%llu rank=%d x%d (无可用堆/空槽)",
                (unsigned long long)info.itemId, info.rank, info.stackCount);
        }
    }

    // D3: 回滚失败汇总
    if (rollbackFailed) {
        Log("[ChestSort] [WARNING] rollbackFailed=true: 本次归类有回滚操作失败，可能存在物品数量不一致");
    }

    // 收尾三件套
    // v1.2.6: 转移后必须失效工作缓存——箱内物品数量/槽位已变，下次按键必须重建快照
    // v1.2.7: batch_dirty AOB 修复（BTR→BTS），加诊断日志
    if (movedStacks > 0) {
        G::workCacheValid = false;
        G::workCacheMoved = true;
#if DIAG_CHESTSORT
        Log("[ChestSort][diag-v127] 收尾开始: movedStacks=%d movedItems=%d batchDirty=0x%p",
            (int)movedStacks, (int)movedItems, (void*)G::batchDirty);
#endif
        if (G::batchRecalc) {
            __try { G::batchRecalc(ctx->player, false);
#if DIAG_CHESTSORT
            Log("[ChestSort][diag-v127] batchRecalc OK");
#endif
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("[ChestSort] batchRecalc 异常 0x%X", GetExceptionCode());
            }
        } else {
#if DIAG_CHESTSORT
            Log("[ChestSort][diag-v127] batchRecalc NULL, 跳过");
#endif
        }
        if (G::batchUIRefresh) {
            __try { G::batchUIRefresh(ctx->player, true);
#if DIAG_CHESTSORT
            Log("[ChestSort][diag-v128] batchUIRefresh OK (箱子侧)");
#endif
            } __except (EXCEPTION_EXECUTE_HANDLER) {
#if DIAG_CHESTSORT
                Log("[ChestSort][diag-v128] batchUIRefresh 异常 0x%X", GetExceptionCode());
#endif
            }
        } else {
#if DIAG_CHESTSORT
            Log("[ChestSort][diag-v128] batchUIRefresh NULL, 跳过");
#endif
        }
        // v1.2.8: 追加调用背包侧 UI 刷新（0xF1310 访问 [[obj+0x200]+0x30]）
        // v1.4.2-diag: 加背包对象链诊断——v1.20 下 UI 刷新疑似失效（[[player+0x200]+0x30] 结构漂移）
        {
            __try {
                const uintptr_t pp = reinterpret_cast<uintptr_t>(ctx->player);
                void* v200 = nullptr;
                void* v30 = nullptr;
                if (IsReadable(reinterpret_cast<void*>(pp), 0x208)) {
                    v200 = *reinterpret_cast<void**>(pp + 0x200);
                }
                if (v200 && IsReadable(v200, 0x38)) {
                    v30 = *reinterpret_cast<void**>(
                        reinterpret_cast<uintptr_t>(v200) + 0x30);
                }
#if DIAG_CHESTSORT
                Log("[ChestSort][diag-ui] player=%p +0x200=%p +0x30=%p "
                    "(readable200=%d readable30=%d)",
                    reinterpret_cast<void*>(pp), v200, v30,
                    IsReadable(reinterpret_cast<void*>(pp), 0x208) ? 1 : 0,
                    (v200 && IsReadable(v200, 0x38)) ? 1 : 0);
#endif
            } __except (EXCEPTION_EXECUTE_HANDLER) {
#if DIAG_CHESTSORT
                Log("[ChestSort][diag-ui] 背包链读取异常");
#endif
            }
            auto fnAlt = (FnBatchUIRefresh)(G::base + RVA_BATCH_UI_REFRESH_ALT);
            __try { fnAlt(ctx->player, true);
#if DIAG_CHESTSORT
            Log("[ChestSort][diag-v128] batchUIRefresh-alt OK (背包侧)");
#endif
            } __except (EXCEPTION_EXECUTE_HANDLER) {
#if DIAG_CHESTSORT
                Log("[ChestSort][diag-v128] batchUIRefresh-alt 异常 0x%X", GetExceptionCode());
#endif
            }
        }
        if (G::batchDirty) {
            __try { G::batchDirty(ctx->player, 0x2bc);
#if DIAG_CHESTSORT
            Log("[ChestSort][diag-v127] batchDirty OK (BTS=设置脏标记)");
#endif
            } __except (EXCEPTION_EXECUTE_HANDLER) {
#if DIAG_CHESTSORT
                Log("[ChestSort][diag-v127] batchDirty 异常 0x%X", GetExceptionCode());
#endif
            }
        } else {
#if DIAG_CHESTSORT
            Log("[ChestSort][diag-v127] batchDirty NULL, 跳过");
#endif
        }
        Log("[ChestSort] 完成: 转移 %d 堆, %d 个物品", (int)movedStacks, (int)movedItems);
    } else {
        Log("[ChestSort] 无可转移物品");
    }
}

// ============================================================
// 输入轮询（mod_tick 中调用）
// ============================================================
static void PollInput() {
    // 触发方式：
    //   1. 手柄 D-pad Up (XInput > HID > joyGetPosEx 三路混合)
    //   2. 键盘热键（默认数字键 4，由 qol_hotkeys.txt 覆盖，游戏内可改键）

    // ---- 手柄 D-pad Up ----
    // 手柄三路混合方案，CheckDpadUpPressed 内部自动选择路径
    if (CheckDpadUpPressed()) {
        Log("[ChestSort] ★ D-pad Up 触发归类");
        WorldContext ctx = {};
        if (GetWorldContext(&ctx)) {
            DoSort(&ctx);
        } else {
            Log("[ChestSort] D-pad Up: 无法获取世界上下文");
        }
    }

    // ---- 键盘热键（默认 4，qol_hotkeys.txt 可覆盖，ModManager 游戏内改键）----
    // 文件 mtime 变化才重读（每 tick 仅一次廉价检查）
    if (QolHotkeyCheckReload(&g_hotkeys)) {
        Log("[ChestSort] 热键配置已重载: count=%d keyboard=%d",
            g_hotkeys.count, g_hotkeys.keyboardCount);
        for (int k = 0; k < g_hotkeys.count && k < 4; ++k)
            Log("[ChestSort]   键%d: raw='%s' vk=0x%X",
                k, g_hotkeys.raw[k], (unsigned)QolHotKeysVk(&g_hotkeys, k));
    }

    static bool s_needsRelease = true;
    bool pressed = false;
    for (int i = 0; i < g_hotkeys.keyboardCount && i < 4; ++i) {
        WORD vk = QolHotKeysVk(&g_hotkeys, i);
        if (vk == VK_UNDEFINED) continue;
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (s_needsRelease) {
            if (!down) s_needsRelease = false;
        } else if (down) {
            pressed = true;
            s_needsRelease = true;
            Log("[ChestSort] ★ 热键 VK=0x%X 触发（备选）", (unsigned)vk);
            break;
        }
    }

    if (pressed) {
        WorldContext ctx = {};
        if (GetWorldContext(&ctx)) {
            DoSort(&ctx);
        } else {
            Log("[ChestSort] 无法获取世界上下文");
        }
    }
}

// ============================================================
// 插件入口
// ============================================================
extern "C" __declspec(dllexport) void mod_init(void) {
    LogOpen("chestsort");
    Log("[ChestSort] mod_init 开始");

    // ---- HotConfig 热调参注册 ----
    HotConfig_Register("chestsort", (void*)&RVA_GAME_ROOT, "RVA_GAME_ROOT", HOT_RVA, 0x10FCBB0);
    HotConfig_Register("chestsort", (void*)&ROOT_PLAYER_OFFSET, "ROOT_PLAYER_OFFSET", HOT_RVA, 0x208);
    HotConfig_Register("chestsort", (void*)&ROOT_MAP_OWNER_OFFSET, "ROOT_MAP_OWNER_OFFSET", HOT_RVA, 0x268);
    HotConfig_Register("chestsort", (void*)&MAP_INFO_OFFSET, "MAP_INFO_OFFSET", HOT_RVA, 0x0d8);
    HotConfig_Register("chestsort", (void*)&MAP_SPATIAL_OWNER_OFFSET, "MAP_SPATIAL_OWNER_OFFSET", HOT_RVA, 0x030);
    HotConfig_Register("chestsort", (void*)&MAP_SPATIAL_INDEX_OFFSET, "MAP_SPATIAL_INDEX_OFFSET", HOT_RVA, 0x6e0);
    HotConfig_Register("chestsort", (void*)&PLAYER_OBJECT_STATUS_OFFSET, "PLAYER_OBJECT_STATUS_OFFSET", HOT_RVA, 0x32b8);
    HotConfig_Register("chestsort", (void*)&PLAYER_OBJECT_LINK_A_OFFSET, "PLAYER_OBJECT_LINK_A_OFFSET", HOT_RVA, 0x410);
    HotConfig_Register("chestsort", (void*)&PLAYER_OBJECT_LINK_B_OFFSET, "PLAYER_OBJECT_LINK_B_OFFSET", HOT_RVA, 0x008);
    HotConfig_Register("chestsort", (void*)&PLAYER_OBJECT_ID_OFFSET, "PLAYER_OBJECT_ID_OFFSET", HOT_RVA, 0x028);
    HotConfig_Register("chestsort", (void*)&PLAYER_ITEMS_OFFSET, "PLAYER_ITEMS_OFFSET", HOT_RVA, 0x32c0);
    HotConfig_Register("chestsort", (void*)&WORLD_OBJECT_POSITION_OFFSET, "WORLD_OBJECT_POSITION_OFFSET", HOT_RVA, 0x230);
    HotConfig_Register("chestsort", (void*)&WORLD_OBJECT_DIRTY_OFFSET, "WORLD_OBJECT_DIRTY_OFFSET", HOT_RVA, 0x300);
    HotConfig_Register("chestsort", (void*)&STATUS_ITEMS_OFFSET, "STATUS_ITEMS_OFFSET", HOT_RVA, 0x2b8);
    HotConfig_Register("chestsort", (void*)&ITEM_DATA_HOLDER_OFFSET, "ITEM_DATA_HOLDER_OFFSET", HOT_RVA, 0x240);
    HotConfig_Register("chestsort", (void*)&ITEM_STACK_COUNT_OFFSET, "ITEM_STACK_COUNT_OFFSET", HOT_RVA, 0x260);
    HotConfig_Register("chestsort", (void*)&ITEM_RANK_OFFSET, "ITEM_RANK_OFFSET", HOT_RVA, 0x280);
    HotConfig_Register("chestsort", (void*)&GIMMICK_DATA_HOLDER_OFFSET, "GIMMICK_DATA_HOLDER_OFFSET", HOT_RVA, 0x240);
    HotConfig_Register("chestsort", (void*)&GIMMICK_MODULE_NAME_OFFSET, "GIMMICK_MODULE_NAME_OFFSET", HOT_RVA, 0x0E0);
    HotConfig_Register("chestsort", (void*)&GIMMICK_POSITION_OFFSET, "GIMMICK_POSITION_OFFSET", HOT_RVA, 0x0f0);
    HotConfig_Register("chestsort", (void*)&WORLD_OBJECT_REGISTRY_RVA, "WORLD_OBJECT_REGISTRY_RVA", HOT_RVA, 0x1104C80);
    HotConfig_Register("chestsort", (void*)&SEARCH_CALLBACK_VTABLE_RVA, "SEARCH_CALLBACK_VTABLE_RVA", HOT_RVA, 0xE3E608);
    HotConfig_Register("chestsort", (void*)&SEARCH_CALLBACK_COPY_RVA, "SEARCH_CALLBACK_COPY_RVA", HOT_RVA, 0x28E480);
    HotConfig_Register("chestsort", (void*)&SEARCH_CALLBACK_INVOKE_RVA, "SEARCH_CALLBACK_INVOKE_RVA", HOT_RVA, 0x28E410);
    HotConfig_Register("chestsort", (void*)&SEARCH_CALLBACK_DESTROY_RVA, "SEARCH_CALLBACK_DESTROY_RVA", HOT_RVA, 0x1969B0);
    HotConfig_Register("chestsort", (void*)&RVA_BATCH_RECALCULATE, "RVA_BATCH_RECALCULATE", HOT_RVA, 0x1470C0);
    HotConfig_Register("chestsort", (void*)&RVA_BATCH_UI_REFRESH, "RVA_BATCH_UI_REFRESH", HOT_RVA, 0xFB170);
    HotConfig_Register("chestsort", (void*)&RVA_BATCH_UI_REFRESH_ALT, "RVA_BATCH_UI_REFRESH_ALT", HOT_RVA, 0xFAB30);
    HotConfig_Poll();
    HotConfig_DumpCE("chestsort");

    // 共享热键初始化：读 qol_hotkeys.txt 中 chestsort 行的键盘键。
    QolHotKeysInit(&g_hotkeys, "chestsort");
    // 只有文件没有该 MOD 行时才注册默认键（避免覆盖用户在游戏内改过的键）
    if (!g_hotkeys.hasEntry) {
        QolRegisterHotKey("chestsort", "4, D-pad Up");
        QolHotKeysSetDefault(&g_hotkeys, "4, D-pad Up");
    }
    Log("[ChestSort] 热键已初始化: %d 个按键, %d 个键盘键, hasEntry=%d",
        g_hotkeys.count, g_hotkeys.keyboardCount, g_hotkeys.hasEntry);

    G::base = (uintptr_t)GetModuleHandleW(nullptr);
    Log("[ChestSort] 游戏基址: 0x%llX", (unsigned long long)G::base);

    // AOB 扫描定位游戏函数
    uintptr_t addrs[kAOBCount] = {};
    for (size_t i = 0; i < kAOBCount; ++i) {
        addrs[i] = qol::ScanModuleAOB(nullptr, kAOBs[i].hex);
        Log("[ChestSort] AOB %-20s => 0x%p %s",
            kAOBs[i].name, (void*)addrs[i],
            addrs[i] ? "(OK)" : "(MISS)");
    }

    // 绑定函数指针
    if (addrs[IDX_SPATIAL_SEARCH])       G::spatialSearch    = (FnSpatialSearch)addrs[IDX_SPATIAL_SEARCH];
    if (addrs[IDX_RAW_VECTOR_FREE])      G::rawVectorFree    = (FnRawVectorFree)addrs[IDX_RAW_VECTOR_FREE];
    if (addrs[IDX_INTRUSIVE_RELEASE])    G::intrusiveRelease = (FnIntrusiveRelease)addrs[IDX_INTRUSIVE_RELEASE];
    if (addrs[IDX_ITEM_ADJUST])          G::itemAdjust       = (FnItemAdjust)addrs[IDX_ITEM_ADJUST];
    if (addrs[IDX_PLAYER_CAPACITY])      G::playerCapacity   = (FnPlayerCapacity)addrs[IDX_PLAYER_CAPACITY];
    if (addrs[IDX_COMMAND_CAPACITY])     G::commandCapacity  = (FnCommandCapacity)addrs[IDX_COMMAND_CAPACITY];
    if (addrs[IDX_BATCH_DIRTY])           G::batchDirty        = (FnBatchDirty)addrs[IDX_BATCH_DIRTY];

    // 检查关键函数是否就绪
    int missing = 0;
    if (!G::spatialSearch)    missing++;
    if (!G::rawVectorFree)    missing++;
    if (!G::itemAdjust)       missing++;

    if (missing > 0) {
        Log("[ChestSort] [警告] %d 个关键函数未定位", missing);
    }

    // 直接使用硬编码 RVA（已通过 vtable_scan_tool + RTTI 确认，build 24969282）
    // 不再运行时扫描 .data 段——扫描 439 万个槽位耗时 20 秒会冻结游戏，
    // 且 resolveWorld 对错误 registry 可能死锁（SEH 无法拦截）
    // ★ v1.3.1: 绑定前先做 prologue 字节验证，防止版本更新后 RVA 漂移导致崩溃
    auto VerifyPrologue = [](uintptr_t base, uintptr_t rva,
                             const unsigned char* expected, size_t len,
                             const char* name) -> bool {
        const unsigned char* addr = reinterpret_cast<const unsigned char*>(base + rva);
        if (memcmp(addr, expected, len) != 0) {
            Log("[ChestSort] [FATAL] prologue mismatch %s RVA=0x%X: got ", name, (unsigned)rva);
            for (size_t i = 0; i < len; ++i) Log("%02X", addr[i]);
            Log(" expected ");
            for (size_t i = 0; i < len; ++i) Log("%02X", expected[i]);
            Log("\n");
            return false;
        }
        return true;
    };

    bool prologueOk = true;
    prologueOk &= VerifyPrologue(G::base, RVA_BATCH_RECALCULATE,
        EXPECTED_BATCH_RECALC_PROLOGUE, sizeof(EXPECTED_BATCH_RECALC_PROLOGUE), "batch_recalc");
    prologueOk &= VerifyPrologue(G::base, RVA_BATCH_UI_REFRESH,
        EXPECTED_BATCH_UI_REFRESH_PROLOGUE, sizeof(EXPECTED_BATCH_UI_REFRESH_PROLOGUE), "batch_ui_refresh");
    prologueOk &= VerifyPrologue(G::base, RVA_BATCH_UI_REFRESH_ALT,
        EXPECTED_BATCH_UI_REFRESH_ALT_PROLOGUE, sizeof(EXPECTED_BATCH_UI_REFRESH_ALT_PROLOGUE), "batch_ui_refresh_alt");
    prologueOk &= VerifyPrologue(G::base, SEARCH_CALLBACK_COPY_RVA,
        EXPECTED_CB_COPY_PROLOGUE, sizeof(EXPECTED_CB_COPY_PROLOGUE), "cb_copy");
    prologueOk &= VerifyPrologue(G::base, SEARCH_CALLBACK_INVOKE_RVA,
        EXPECTED_CB_INVOKE_PROLOGUE, sizeof(EXPECTED_CB_INVOKE_PROLOGUE), "cb_invoke");
    prologueOk &= VerifyPrologue(G::base, SEARCH_CALLBACK_DESTROY_RVA,
        EXPECTED_CB_DESTROY_PROLOGUE, sizeof(EXPECTED_CB_DESTROY_PROLOGUE), "cb_destroy");
    if (!prologueOk) {
        Log("[ChestSort] [FATAL] 硬编码 RVA prologue 验证失败，MOD 不可用");
        G::ready = false;
        return;
    }
    Log("[ChestSort] 硬编码 RVA prologue 验证全部通过");

    // 验证 vtable 条目：vt[0]=copy, vt[2]=invoke, vt[4]=destroy
    {
        const uintptr_t* vt = reinterpret_cast<const uintptr_t*>(G::base + SEARCH_CALLBACK_VTABLE_RVA);
        const uintptr_t expectedCopy   = G::base + SEARCH_CALLBACK_COPY_RVA;
        const uintptr_t expectedInvoke = G::base + SEARCH_CALLBACK_INVOKE_RVA;
        const uintptr_t expectedDestroy = G::base + SEARCH_CALLBACK_DESTROY_RVA;
        if (vt[0] != expectedCopy || vt[2] != expectedInvoke || vt[4] != expectedDestroy) {
            Log("[ChestSort] [FATAL] vtable 条目不匹配: vt[0]=0x%llX(exp 0x%llX) vt[2]=0x%llX(exp 0x%llX) vt[4]=0x%llX(exp 0x%llX)\n",
                (unsigned long long)vt[0], (unsigned long long)expectedCopy,
                (unsigned long long)vt[2], (unsigned long long)expectedInvoke,
                (unsigned long long)vt[4], (unsigned long long)expectedDestroy);
            G::ready = false;
            return;
        }
        Log("[ChestSort] vtable 条目验证通过 (copy/invoke/destroy)");
    }
    G::rvaGameRoot = RVA_GAME_ROOT;
    G::rvaWorldRegistry = WORLD_OBJECT_REGISTRY_RVA;
    Log("[ChestSort] gameRoot RVA=0x%X  worldRegistry RVA=0x%X (硬编码)",
        (unsigned)G::rvaGameRoot, (unsigned)G::rvaWorldRegistry);

    // batch_recalculate / batch_ui_refresh 的 AOB 短签名有 455/5+ 匹配，不可用。
    // 改用 RVA 关系定位（旧 build recalc=dirty+0x660, ui=dirty-0x4C200）
    // + 候选函数体特征验证（DL 读取 + 对象偏移模式），无条件硬编码如下 RVA：
    // （不再用 if(!G::batchRecalc) 条件——AOB 表已移除这两项，不会被覆盖）
    G::batchRecalc = (FnBatchRecalc)(G::base + RVA_BATCH_RECALCULATE);
    G::batchUIRefresh = (FnBatchUIRefresh)(G::base + RVA_BATCH_UI_REFRESH);
    Log("[ChestSort] batchRecalc RVA=0x%X  batchUIRefresh RVA=0x%X  batchDirty=0x%p (硬编码)",
        (unsigned)RVA_BATCH_RECALCULATE, (unsigned)RVA_BATCH_UI_REFRESH, (void*)G::batchDirty);

    // vtable 直接使用硬编码 RVA（v1.09 build 25094764 通过 RTTI 链扫描验证）。
    // 不再运行时扫描 .rdata——遍历 439 万个槽位耗时 16 秒会冻结游戏。
    G::rvaCallbackVTable = SEARCH_CALLBACK_VTABLE_RVA;
    Log("[ChestSort] [vtable] 硬编码 RVA=0x%X (build 25311578, v1.20 vtable 已验证)",
        (unsigned)G::rvaCallbackVTable);

    // ---- 手柄初始化 (XInput + HID + joyGetPosEx 三路混合) ----
    // XInput 独立线程检测 Xbox 手柄（热插拔天然支持）
    // HID 独立线程直读 DualSense（热插拔重连）
    // joyGetPosEx 同步回退检测其他 DirectInput 手柄
    InitGamepad();

    G::ready = true;
    Log("[ChestSort] mod_init 完成 (ready=%d xinput=%d hid=%d pad=%d)",
        G::ready ? 1 : 0, G::xinputReady ? 1 : 0, G::hidReady ? 1 : 0, G::padReady ? 1 : 0);
}

extern "C" __declspec(dllexport) void mod_tick(void) {
    qol::budget::BeginFrame();  // P0: 帧锚定（幂等，多 DLL 安全）
    static int s_bdgSlot = -1;  // P0 探针（惰性注册，析构自动上报）
    if (s_bdgSlot < 0) s_bdgSlot = qol::budget::Slot("ChestSort");
    struct BdgGuard {
        int slot; uint64_t t0;
        ~BdgGuard() { qol::budget::Report(slot, qol::budget::NowUs() - t0); }
    } bdgGuard = { s_bdgSlot, qol::budget::NowUs() };

    if (!G::ready) return;
    HotConfig_Poll();

    // v1.2.3: 注册表预热 — 游戏启动后首帧自动发起一次空间搜索，
    // 让分片扫描在玩家走路时后台完成。首次按键时注册表已就绪，
    // 跳过 DoSort 中的 while(scanPending) 同步循环，消除 ~500ms 卡顿。
    if (!G::initialScanDone && !G::scanPending && !G::registryValid && !QolGameBusy()) {
        WorldContext ctx = {};
        if (GetWorldContext(&ctx)) {
            G::initialScanDone = true;
            Log("[ChestSort] [warmup] 首帧注册表预热开始");
            StartRegistryScan(&ctx);
        }
    }

    // 分片处理注册表扫描（每帧最多 64 条=SCAN_CHUNK，玩家无感）
    if (G::scanPending && !QolGameBusy()) {
        ProcessRegistryChunk();
    }
    PollInput();
}

extern "C" __declspec(dllexport) void unload(void) {
    Log("[ChestSort] unload");

    // 停止 XInput 轮询线程
    if (g_xinputThread) {
        InterlockedExchange(&g_xinputThreadRunning, 0);
        WaitForSingleObject(g_xinputThread, 2000);
        CloseHandle(g_xinputThread);
        g_xinputThread = nullptr;
        Log("[ChestSort] [xinput] 线程已停止");
    }

    // 停止 HID 轮询线程
    if (g_hidThread) {
        InterlockedExchange(&g_hidThreadRunning, 0);
        // CloseHandle 设备句柄迫使阻塞中的 ReadFile 立即失败返回
        HANDLE dev = InterlockedExchangePointer(&g_hidDevice, nullptr);
        if (dev) CloseHandle(dev);
        WaitForSingleObject(g_hidThread, 2000);
        CloseHandle(g_hidThread);
        g_hidThread = nullptr;
        Log("[ChestSort] [hid] 线程已停止");
    }

    LogClose();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)hModule;
    (void)lpReserved;
    if (reason == DLL_PROCESS_ATTACH) {
        Log("[ChestSort] DllMain ATTACH (hModule=%p)", hModule);
        DisableThreadLibraryCalls(hModule);
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("[ChestSort] DllMain DETACH");
    }
    return TRUE;
}