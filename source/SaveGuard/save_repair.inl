// save_repair.inl —— SaveGuard v1.1.0: 存档运行时净化模块（L1 运行时净化）
//
// 背景（2026-10-13 用户拍板）：多名玩家反馈存档被污染，症状 = 进得去游戏、
// 操作时崩（处理加工机械崩 / 靠近树崩）。产品形态并入 SaveGuard。
//
// 三层检测面：
//   A 类·机器槽净化（确定性修复）：
//     PA 已实锤病灶——机器槽 timer 挂着（timeValue/createTime 非 0）但产出
//     map 键 itemID{slot} 为 0/缺失，原生 outputHelper 在此状态被调用即崩
//     （v1.5.8 肥料桶取料崩溃实证，production_automation_v2.inl L1042-1049）。
//     修复 = CAS 清 timer + 幂等写 0 map（PA_ROLLBACK_TIMERS 同款序列）。
//     双帧 500ms 复验防投料瞬态窗口误伤（原生投料为同步序列，无秒级窗口）。
//     backing 有残留 clone 的病灶槽只报告不动（无实锤结论，保守）。
//   B 类·gadget 链健康巡检（只报告）：
//     "靠近树崩"等场景——gimmick 链（save+0x3440）上 status 悬空/vtable
//     失效（AutoHarvest 树引用 / MM v1.0.60 marker 等嫌疑，无实锤不修复）。
//     检测：链断裂点 / status 不可读 / vtable 指针悬空，样本记入日志。
//   C 类·环境取证（归因基础设施）：
//     启动时枚举 mods 目录清单 + exe 指纹日志，玩家回传日志即可跨 MOD 归因
//     （版本适配问题：exe SHA 门禁不过则修复整体禁用，仅保留容器校验）。
//
// 常量与调用姿势全部来自 ProductionAuto v1.5.10 已验证序列：
//   production_automation_v2.inl（枚举器 L1370 / timer CAS L849-868 / map+notify
//   L296-368 / 释放 L615）+ version_manifest.h（v1.20 RVA 表）。
// 线程上下文：mod_tick = 游戏主线程（加载器契约，steam_api64_README L52），
// 同主线程直调原生函数安全（FN-010 TLS 评估背书）。
//
// 死档红线合规：所有状态写入走原生 map+event notify 链（禁手写清槽即指
// 非 notify 裸改，本模块 map 写即 PA 每次转账使用的同款已验证序列）。
//
// 作者：PHJ&消失的清风  编写：2026-10-13

// ============================================================
// 常量（version_manifest.h v1.20 + PA v1.5.10 已验证值）
// ============================================================
static constexpr uintptr_t SR_GAME_ROOT_RVA = 0x111CC90;        // v1.20 GAME_ROOT
// TODO(2026-10-28 build25640656)：以下 4 项尚未随版本适配，**本 MOD 暂不部署**。
//   SR_GIMMICK_VTABLE_RVA / SR_GIMMICK_VTABLE2_RVA：数据表，需按 RTTI 链（类 CGimmickStatus）定址
//   SR_REGISTRY_LOOKUP_RVA：accessor 族 0x2A430/0x2A500/0x2A5D0/0x2A6A0，需按函数内 hash 立即数消歧
//   SR_SAVE_OFFSET 0x208 / SR_GIMMICK_LIST 0x3440：存档结构偏移，PA 日志已证 v25640656 布局漂移
//   风险：本 MOD 会写存档，地址/偏移错可能损坏存档，必须专轮验证后再 deploy。
static constexpr uintptr_t SR_GIMMICK_VTABLE_RVA = 0xE43870;    // CGimmickStatus off 0x0（RTTI 唯一）
static constexpr uintptr_t SR_GIMMICK_VTABLE2_RVA = 0xE43700;   // CGimmickStatus off 0x10（RTTI 唯一）
static constexpr uintptr_t SR_STATUS_INT_LOOKUP_RVA = 0x15DB60;
static constexpr uintptr_t SR_STATUS_U64_LOOKUP_RVA = 0x15DD20;
static constexpr uintptr_t SR_EVENT_LOOKUP_RVA = 0x15D740;
static constexpr uintptr_t SR_EVENT_NOTIFY_RVA = 0x15D560;
static constexpr uintptr_t SR_INTRUSIVE_RELEASE_RVA = 0xACC20;  // Nearby 侵入式引用释放
// v1.2.1（FN-015/FN-016）：全局注册表 [exe+0x1104C10] → 0x29C60(R)=上下文
// 对象 ctx（key=运行时 id，主菜单与进档后是不同 ctx）→ ctx+0x58 = uid→对象
// 缓存 map（哨兵=[ctx+0x60]，与 0x3FD80 哈希 map 同构：
// buckets+0x18/mask+0x30、桶 16B/链头+8/尾哨兵+0、节点 next+0x8/key+0x10/value+0x18）；
// 玩家/本机复现崩溃点 RVA 0x5D9022 = 读该 map 悬空节点 value（mov rax,[rdi]，
// node+0x18 区不可读）——本巡检的检测目标。
// 地址校验：0x5D8FE7 `mov rcx,[rip+0xb2bc22]` rip=0x5D8FEE → 0x5D8FEE+0xB2BC22
// = 0x1104C10（本次当场 capstone 交叉验证；v1.2.0 曾误诊此地址为错，实为
// 正确——慢的根因是裸 IsReadable，已于 v1.1.4 缓存化根治）
static constexpr uintptr_t SR_REGISTRY_GLOBAL_RVA = 0x1124CF0;
// v1.3.18（2026-10-29 Ghidra 实锤修正）：旧值 0x2A430 是 accessor 克隆族里
//   的【另一个成员】（内部取 key 子函数 FUN_1400bd4e0），在 build 25640656
//   会从注册表查到【别的子系统】的 ctx → ctx+0x58 map 恒 0 键 →
//   SrCtxHasId 对一切 id 恒 miss → 所有"已完成"机器被判 0x5F01A2 崩源，
//   v1.3.17 实测误清 9 台树液提取器（日志 17:05:03"已清理（崩溃源）"）。
//   正确值：游戏崩路 FUN_1405effa0 反编译实锤调用 FUN_14002c720
//   （[exe+0x1124CF0] → FUN_14002c720 → ctx；两克隆仅取 key 子函数不同：
//   0x2C720→FUN_1400b4d90，0x2A430→FUN_1400bd4e0，本体完全同构）。
//   ⚠ MonsterMark(night_map_markers.inl RVA_NIGHT_SAVE_DATA_ACCESSOR) 与
//   PA(g_rva_production_recipe_mgr) 源码中同持 0x2A430，夜间标记/配方管理
//   路径是否同样失配待各自验证（它们的目标 ctx 是哪个成员须另行反编译）。
static constexpr uintptr_t SR_REGISTRY_LOOKUP_RVA = 0x2C720;
static constexpr uintptr_t SR_SAVE_OFFSET = 0x208;              // root+0x208 = save
static constexpr uintptr_t SR_GIMMICK_LIST = 0x3448;            // save+0x3440 链 sentinel
static constexpr uintptr_t SR_UNIQUE_ID = 0x0E0;
static constexpr uintptr_t SR_GIMMICK_DATA = 0x240;             // status+0x240→holder→data
static constexpr uintptr_t SR_TIME_VALUE = 0x288;               // timer 直读区
static constexpr uintptr_t SR_CREATE_TIME = 0x2A0;
static constexpr uintptr_t SR_ITEM_LIST = 0x2B8;                // backing 裸数组指针
// v1.2.9 E 类物品健康体检：玩家背包链（v1.3.1 修正为 ChestSort/AutoHarvest 权威链路）
//   [exe+GAME_ROOT]+0x208 = player 对象
//   player+0x32C0 = 背包 30 槽（**内嵌数组，非指针——勿解引用**）
//   注：player+0x32B8 = playerStatus（位置读取通道），v1.2.9~v1.3.0 误在此层再取
//   +0x32C0 → 落在无关内存（实测日志 status(+0x32B8)不可读 = 病根）
static constexpr uintptr_t SR_PLAYER_STATUS_CHAIN_1 = 0x208;    // root → player
static constexpr uintptr_t SR_PLAYER_STATUS_CHAIN_2 = 0x32B8;   // player → status
static constexpr uintptr_t SR_PLAYER_ITEMS = 0x32C0;            // status → 背包数组
static constexpr size_t   SR_PLAYER_CAPACITY_MAX = 30;
// 物品结构偏移（chestsort/autoharvest 通用）
static constexpr uintptr_t SR_ITEM_REFCOUNT = 0x08;             // +8 引用计数（AddRef 计数）
static constexpr uintptr_t SR_ITEM_STACK_COUNT = 0x260;         // 堆叠数
// v1.3.15：堆叠数合法上限。与 PA / ChestSort / AutoHarvest 三处
//   ReadStackCount 的 c > 999 判定同源，勿各自改。
static constexpr int SR_ITEM_STACK_MAX = 999;
// v1.3.15：时钟倒退告警阈值。3600 秒 = 游戏内 1 小时，低于此不报
//   （跨天/睡觉/读档的正常波动会触发小幅跳变，避免噪音淹没真问题）。
static constexpr u64 SR_CLOCK_DROP_ALERT_SEC = 3600;
static constexpr size_t SR_MACHINE_SLOTS = 3;
static constexpr size_t SR_SLOT_INGREDIENTS = 8;
static constexpr size_t SR_MAX_GIMMICKS = 16384;
// v1.3.0：树液提取器 gimmickId（240250000）——产出走 count 键体系、天然无
// itemID 键（FN-014），必须在 offlist 清理路径上显式豁免，否则 v1.1.0 的
// "误清提取器 timer"事故会从 v1.2.8 新增的 offlist 路径复发
static constexpr u64 SR_SAP_EXTRACTOR_ID = 240250000ULL;
// v1.3.7：存档累计游戏秒（save+0x3270，复用 ProductionAuto PA_SAVE_GAME_SECOND，
// v1 旧版 L8194 已验证、v1.5.45 仍直读）。区分"真损坏型 KEY-MISSING（卡完成后
// 无 itemID）"vs"运行中合法态"的安全门。
static constexpr uintptr_t SR_SAVE_GAME_SECOND = 0x3270;

// 机器 ID 白名单（processing_recipe_table.inl kProcessingRecipeMachineIndex
// 29 项中剔除了 240250000/0xE51EC90 —— 树液提取器（gimmick_sap_extractor，
// AutoHarvest L1831 实锤）。它是 CGimmickStatus 同类对象（无独立 RTTI 子类）且
// 时间戳同样在 0x288/0x2A0 区，但产出用 count 键体系而非 itemID%d/itemVal%d
// （AutoHarvest L1035-1049 实证），A 类病灶判定对它不适用且会误伤其生产时间戳
// ——v1.1.2 已实锤此缺陷，v1.1.1 剔除。避免引入 147KB 配方表本体。）
static const u64 SR_MACHINE_IDS[] = {
    0xE4E4310ULL, 0xE4E4374ULL, 0xE4E6A20ULL, 0xE4E6A84ULL,
    0xE4E9130ULL, 0xE4E9194ULL, 0xE4EB840ULL, 0xE4EB8A4ULL,
    0xE4EDF50ULL, 0xE4EDFB4ULL, 0xE4F0660ULL, 0xE4F06C4ULL,
    0xE4F2D70ULL, 0xE4F2DD4ULL, 0xE4F5480ULL, 0xE4F54E4ULL,
    0xE4F7B90ULL, 0xE4F7BF4ULL, 0xE4FA2A0ULL, 0xE4FA304ULL,
    0xE4FC9B0ULL, 0xE4FCA14ULL, 0xE4FF0C0ULL, 0xE4FF124ULL,
    0xE5065F0ULL, 0xE506654ULL, 0xE51C580ULL, 0xE523AB0ULL,
};
static const size_t SR_MACHINE_ID_COUNT =
    sizeof(SR_MACHINE_IDS) / sizeof(SR_MACHINE_IDS[0]);

// 时序参数
static constexpr ULONGLONG SR_WORLD_PROBE_MS = 500;     // 世界就绪探测间隔
static constexpr ULONGLONG SR_LOAD_SETTLE_MS = 3000;    // save 链有效后等待世界加载
static constexpr ULONGLONG SR_CONFIRM_MS = 500;         // A 类双帧复验间隔
static constexpr ULONGLONG SR_CRUISE_MS = 15000;        // 巡检周期
static constexpr unsigned SR_PATROL_MAX_MS = 3000;      // v1.1.4：单轮巡检超时自禁用阈值

// 候选容量上限
static constexpr size_t SR_MAX_CANDIDATES = 512;

// ============================================================
// 原生函数类型（PA L62-75 同款签名）
// ============================================================
struct SrMapLookupResult {
    void* node;
    unsigned char inserted;
    unsigned char padding[7];
};
using SrMapLookupFunction = SrMapLookupResult* (__fastcall *)(
    void*, SrMapLookupResult*, const u64*);
using SrEventNotifyFunction = void (__fastcall *)(void*);
using SrEventOwnerFunction = void* (__fastcall *)(void*);
using SrIntrusiveReleaseFunction = void (__fastcall*)(void**);
using SrRegistryLookupFunction = void* (__fastcall*)(void*);  // v1.2.1: 0x29C60 注册表→ctx

// ============================================================
// 状态机
// ============================================================
enum class SrPhase : unsigned {
    Disabled = 0,   // 绑定/校验失败或 exe 不支持
    WaitWorld,      // 等待 save 链就绪
    Settle,         // 等 3s 世界加载完成
    FirstPass,      // 收集候选
    Confirm,        // 500ms 后复验
    Repair,         // 执行清理
    Cruise,         // 周期巡检
};

struct SrCandidate {
    void* status;
    u64 uid;
    size_t slot;
    u64 start;
    u64 duration;
    u64 itemID;           // v1.2.3 D 类：悬垂引用的 itemID 原始值
    bool backingResidue;  // backing 有残留 → 仅报告
    bool isDangling;      // v1.2.3 D 类：itemID 有值但 ctx 表查不到（悬垂引用）
    bool keyMissing;      // v1.2.6 D2：timer 挂着但 itemID 键不存在（22B8B 型）
    bool valueBad;        // v1.3.12：键在但 id 不在全局注册表（itemID 有效位=否）
    u64  sentinelVal;     // v1.3.12：保留字段（旧版比对的 ctx 哨兵，已弃用）
    bool alive;           // v1.1.2：修复前重验仍在当前链上（防幽灵修复）
};

static SrPhase sr_phase = SrPhase::WaitWorld;
// v1.3.15：时钟健康巡检状态（SrHealthCheckClock，只报告）
static u64   sr_lastGameSecond = 0;
static bool  sr_clockZeroLogged = false;
static bool  sr_clockDropLogged = false;
static bool sr_repairReady = false;
static uintptr_t sr_exeBase = 0;
static ULONGLONG sr_nextActionAt = 0;
static void* sr_lastSavePtr = nullptr;
static SrCandidate sr_candidates[SR_MAX_CANDIDATES];
static size_t sr_candidateCount = 0;

// B 类巡检状态（每 save 会话；变化才打日志）
static size_t sr_brokenSeen = 0;
static void* sr_brokenSample[4] = {};
static size_t sr_chainBreakLoggedFor = (size_t)-1;

// 统计（会话累计）
static unsigned sr_totalRepaired = 0;
static unsigned sr_totalResidueReported = 0;

// 函数指针
static SrMapLookupFunction sr_statusIntLookup = nullptr;
static SrMapLookupFunction sr_statusU64Lookup = nullptr;
static SrMapLookupFunction sr_eventLookup = nullptr;
static SrEventNotifyFunction sr_eventNotify = nullptr;
static SrIntrusiveReleaseFunction sr_intrusiveRelease = nullptr;
static SrRegistryLookupFunction sr_registryLookup = nullptr;  // v1.2.1

// v1.2.1 B 类 ctx-map 巡检状态（变化才报，防抖）
static size_t sr_ctxBrokenSeen = (size_t)-1;
static void* sr_ctxBrokenSample = nullptr;

// v1.2.3 D 类机器槽全景 dump 状态（每档只打一轮，防刷屏）
static bool sr_slotDumpDone = false;
// v1.2.9 E 类物品健康体检状态（只报告；计数变化才报，防刷屏）
static size_t sr_itemBadSeen = (size_t)-1;
static size_t sr_itemDistrustSeen = (size_t)-1;
static bool sr_itemRealLogged = false;    // v1.3.0：是否已打过"扫到真实槽"的体检结果
static bool sr_itemChainLogged = false;   // v1.3.0：是否已提示过背包链未就绪

// ============================================================
// v1.2.9 E 类：物品健康体检（report-only）
// 背景（2026-10-16 玩家反馈定案）：旧档存在"烂账物品"——历史版本 MOD
// （ChestSort v1.3.0 前先 CAS 后 AddRef / v1.3.5 移除的 CAS 直写箱槽；
// AutoHarvest v1.8.6 前失败路径少释放）留下的引用计数错乱 / 未注册物品。
// 症状：挖矿拾取合并/关箱提交验证失败时游戏静默删除（"取出后消失"）。
// 新档不复现（定案为旧档污染）——本体检给老档玩家定位烂账物品用。
// 检测项（三重）：
//   ① 悬垂：物品 itemId 不在全局 ctx 表（802 键，find-or-insert 会造死键）
//   ② refcount 异常：[item+8] <= 0（无引用=随时会被析构的孤儿物品）
//   ③ 重复对象：同一 item 指针出现在多个槽（引用数学错乱的直接证据）
// 扫描面：玩家背包 30 槽 + gimmick 链上全部 0x2B8 backing（箱子等容器）
// 只报告不动（烂账物品的修复需要逐案语义判断，防止误删玩家财产）
// ============================================================

// ============================================================
// SEH 安全包装（PA L152-179 同款；注意 __try 函数内禁 C++ 析构对象）
// ============================================================
__declspec(noinline) static SrMapLookupResult* SrSafeMapLookup(
    SrMapLookupFunction fn, void* map, SrMapLookupResult* result,
    const u64* key) {
    SrMapLookupResult* found = nullptr;
    __try { found = fn(map, result, key); }
    __except (EXCEPTION_EXECUTE_HANDLER) { found = nullptr; }
    return found;
}

__declspec(noinline) static void SrSafeEventNotify(
    SrEventNotifyFunction fn, void* node) {
    __try { fn(node); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static void SrSafeIntrusiveRelease(
    SrIntrusiveReleaseFunction fn, void* item) {
    if (!item) return;
    void* owned = item;
    __try { fn(&owned); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// v1.2.1 SEH 包装：注册表查询（0x29C60）
__declspec(noinline) static void* SrSafeRegistryLookup(
    SrRegistryLookupFunction fn, void* registry) {
    void* result = nullptr;
    __try { result = fn(registry); }
    __except (EXCEPTION_EXECUTE_HANDLER) { result = nullptr; }
    return result;
}

// ============================================================
// 指针读取工具（PA L184-190 同款）
// ============================================================
static bool SrReadPtr(const void* base, uintptr_t off, void** out) {
    void* const* slot = reinterpret_cast<void* const*>(
        reinterpret_cast<uintptr_t>(base) + off);
    if (!IsReadable(slot, sizeof(void*))) return false;
    *out = *slot;
    return true;
}

// map 读 + inserted 标志（v1.1.1：inserted=true 表示键不存在、由本次 find-or-insert
// 刚建零值节点——不是病灶，是“该对象根本没有这个键”的信号）
static bool SrReadStatusU64Inserted(void* status, const char* key, u64* valueOut,
                                    bool* insertedOut) {
    if (!status || !key || !valueOut || strlen(key) > sizeof(u64))
        return false;
    u64 packedKey = 0;
    memcpy(&packedKey, key, strlen(key));
    SrMapLookupResult result = {};
    SrMapLookupResult* found = SrSafeMapLookup(
        sr_statusU64Lookup,
        reinterpret_cast<unsigned char*>(status) + 0x58, &result,
        &packedKey);
    if (!found || !found->node ||
        !IsReadable(reinterpret_cast<unsigned char*>(found->node) + 0x18,
                    sizeof(u64))) return false;
    *valueOut = *reinterpret_cast<const u64*>(
        reinterpret_cast<unsigned char*>(found->node) + 0x18);
    // v1.1.2（DeepSeek 审查加固）：读原生返回的 found->inserted 而非本地
    // result.inserted——若原生返回指针≠我们的 out param，本地副本恒 0。
    // 失败退化方向无害：恒 0 仅使真病灶提前一周期修复，白名单已挡非机器。
    if (insertedOut) *insertedOut = (found->inserted != 0);
    return true;
}

static bool SrReadStatusU64(void* status, const char* key, u64* valueOut) {
    if (!status || !key || !valueOut || strlen(key) > sizeof(u64))
        return false;
    u64 packedKey = 0;
    memcpy(&packedKey, key, strlen(key));
    SrMapLookupResult result = {};
    SrMapLookupResult* found = SrSafeMapLookup(
        sr_statusU64Lookup,
        reinterpret_cast<unsigned char*>(status) + 0x58, &result,
        &packedKey);
    if (!found || !found->node ||
        !IsReadable(reinterpret_cast<unsigned char*>(found->node) + 0x18,
                    sizeof(u64))) return false;
    *valueOut = *reinterpret_cast<const u64*>(
        reinterpret_cast<unsigned char*>(found->node) + 0x18);
    return true;
}

static bool SrReadStatusInt(void* status, const char* key, int* valueOut) {
    if (!status || !key || !valueOut || strlen(key) > sizeof(u64))
        return false;
    u64 packedKey = 0;
    memcpy(&packedKey, key, strlen(key));
    SrMapLookupResult result = {};
    SrMapLookupResult* found = SrSafeMapLookup(
        sr_statusIntLookup,
        reinterpret_cast<unsigned char*>(status) + 0x18, &result,
        &packedKey);
    if (!found || !found->node ||
        !IsReadable(reinterpret_cast<unsigned char*>(found->node) + 0x18,
                    sizeof(int))) return false;
    *valueOut = *reinterpret_cast<const int*>(
        reinterpret_cast<unsigned char*>(found->node) + 0x18);
    return true;
}

// v1.2.3 D 类：\u7eaf\u8bfb map \u67e5\u8be2\uff08\u5168\u6876\u626b\u63cf\uff0c\u4e0d\u8d70 find-or-insert \u65e0\u63d2\u5165\u526f\u4f5c\u7528\uff09
// map \u5e03\u5c40\uff08AutoHarvest SapReadStatusMapValue + FN-015 \u5bf9\u62cd\uff09：
//   map+0x8=sentinel, map+0x10=size, map+0x18=buckets, map+0x30=mask
//   \u6876 16B\uff08+0 \u94fe\u5c3e\uff0c+8 \u94fe\u5934\uff09\uff1b\u8282\u70b9 +8=next, +0x10=key, +0x18=value
// \u5168\u6876\u626b\u63cf\u4e0d\u4f9d\u8d56 hash \u5bfb\u5740\u516c\u5f0f\uff08\u514d\u75ab map1 \u76f4\u63a5 hash \u8bed\u4e49\u8bef\u5224\uff09\uff0c\u7edd\u5bf9\u4e0d\u9508\u6f0f
static bool SrMapValuePure(void* status, uintptr_t mapOffset, const char* key,
                           u64* valueOut) {
    if (!status || !key || !valueOut || strlen(key) > sizeof(u64))
        return false;
    unsigned char* map =
        reinterpret_cast<unsigned char*>(status) + mapOffset;
    if (!IsReadable(map, 0x38)) return false;
    void* sentinel = *reinterpret_cast<void**>(map + 0x08);
    void* buckets = *reinterpret_cast<void**>(map + 0x18);
    const u64 mask = *reinterpret_cast<const u64*>(map + 0x30);
    if (!sentinel || !buckets || mask == 0 || mask > 0x100000ULL) return false;
    const size_t bucketCount = static_cast<size_t>(mask) + 1;
    u64 packedKey = 0;
    memcpy(&packedKey, key, strlen(key));
    size_t hops = 0;
    for (size_t b = 0; b < bucketCount; ++b) {
        unsigned char* bkt =
            reinterpret_cast<unsigned char*>(buckets) + b * 16;
        if (!IsReadable(bkt, 16)) continue;
        void* tail = *reinterpret_cast<void**>(bkt);
        void* node = *reinterpret_cast<void**>(bkt + 8);
        if (!node || node == tail) continue;
        // v1.3.3：原条件排除 node == tail，导致"链尾节点"永远不被检查——
        //   itemID 键恰好落在链尾时被误判 KEY-MISSING → 误清有效加工槽
        //   （2026-10-18 实测：uid=469BC 反复被清）。改为循环内检查后再跳出。
        while (node && node != sentinel && hops < 65536) {
            ++hops;
            if (!IsReadable(node, 0x20)) break;
            if (*reinterpret_cast<const u64*>(
                    reinterpret_cast<unsigned char*>(node) + 0x10) ==
                packedKey) {
                *valueOut = *reinterpret_cast<const u64*>(
                    reinterpret_cast<unsigned char*>(node) + 0x18);
                return true;
            }
            if (node == tail) break;
            node = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(node) + 8);
        }
        if (hops >= 65536) break;
    }
    return false;
}

// v1.3.8：PA↔SG 协调标记读取。PA 接管机器时往 status 的 0x58 地图写
// pa_own=1（键名 ≤8 字节，ProductionSetStatusU64 要求），释放时写 0；
// SG 据此整台跳过 PA 托管机器，避免误清 PA 功能槽。
static bool SrIsPaManaged(void* status) {
    if (!status) return false;
    u64 v = 0;
    if (SrMapValuePure(status, 0x58, "pa_own", &v))
        return v != 0;
    return false;
}

// v1.3.12：通用 map 查找（key 直接是 u64）。与 SrMapValuePure 同构，只是键
//   从「≤8 字节字符串打包」换成原生 u64——【全局产出注册表】ctx+0x58 的键是
//   item id（u64），绝不是字符串，因此绝不能用 SrMapValuePure 去查它
//   （v1.3.11 就错在这里：拿 "itemID0" 查 id→物品对象 表，恒不命中）。
static bool SrMapValueU64(void* obj, uintptr_t mapOffset, u64 key,
                          u64* valueOut) {
    if (!obj || !valueOut) return false;
    unsigned char* map = reinterpret_cast<unsigned char*>(obj) + mapOffset;
    if (!IsReadable(map, 0x38)) return false;
    void* sentinel = *reinterpret_cast<void**>(map + 0x08);
    void* buckets = *reinterpret_cast<void**>(map + 0x18);
    const u64 mask = *reinterpret_cast<const u64*>(map + 0x30);
    if (!sentinel || !buckets || mask == 0 || mask > 0x100000ULL) return false;
    const size_t bucketCount = static_cast<size_t>(mask) + 1;
    size_t hops = 0;
    for (size_t b = 0; b < bucketCount; ++b) {
        unsigned char* bkt =
            reinterpret_cast<unsigned char*>(buckets) + b * 16;
        if (!IsReadable(bkt, 16)) continue;
        void* tail = *reinterpret_cast<void**>(bkt);
        void* node = *reinterpret_cast<void**>(bkt + 8);
        if (!node || node == tail) continue;
        while (node && node != sentinel && hops < 65536) {
            ++hops;
            if (!IsReadable(node, 0x20)) break;
            if (*reinterpret_cast<const u64*>(
                    reinterpret_cast<unsigned char*>(node) + 0x10) == key) {
                *valueOut = *reinterpret_cast<const u64*>(
                    reinterpret_cast<unsigned char*>(node) + 0x18);
                return true;
            }
            if (node == tail) break;
            node = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(node) + 8);
        }
        if (hops >= 65536) break;
    }
    return false;
}

// v1.3.12：全局加工管理器 ctx = getter(*0x141104c10)（与崩路 L95 同一 getter）
static void* SrCtxGet() {
    void* registry = nullptr;
    if (!SrReadPtr(reinterpret_cast<void*>(sr_exeBase + SR_REGISTRY_GLOBAL_RVA),
                   0, &registry) ||
        !registry || !IsReadable(registry, 0x78))
        return nullptr;
    void* ctx = SrSafeRegistryLookup(sr_registryLookup, registry);
    if (!ctx || !IsReadable(ctx, 0x90)) return nullptr;
    return ctx;
}

// v1.3.12：id 是否在【全局产出注册表】ctx->map[0x58]（id → 物品对象）里。
//   返回 1=命中（合法，游戏不会崩） 0=未命中（0x5D9022 崩源） -1=不可读。
//   不可读时返回 -1，调用方必须保守（视为合法、不动任何内存）。
// v1.3.18 保险丝：ctx map 为空（0 键）⇒ 查找链必失配，而非"所有 id 都非法"。
//   正常进档后 ctx 表恒有键（slotdump ctxKeys 日志可证）；0 键时 SrCtxHasId
//   恒 0 会把一切已完成机器误判崩源（v1.3.17 误清 9 台树液提取器事故根因）。
//   判定来源：扫描层 SrCollectCtxKeys 结果刷新 s_ctxKeyVerdict；负值=保守。
//   SrCtxHasId 的全部调用点都在扫描层收集（slotdump L1304 同函数更早处）之后，
//   时序保证新鲜。误保守的代价=少修一次（安全方向），误放行的代价=误清玩家机器。
static int s_ctxKeyVerdict = -1;  // -1=未收集/空表/不可读 → 不动内存
static int SrCtxHasId(u64 id) {
    if (s_ctxKeyVerdict < 0) return -1;
    void* ctx = SrCtxGet();
    if (!ctx) return -1;
    u64 v = 0;
    return SrMapValueU64(ctx, 0x58, id, &v) ? 1 : 0;
}

// v1.3.9：二次确认门 ledger（首次报告登记、二次/跨boot才修，防误删正常机器）
// 持久化于 SaveGuard_backup/sg_slot_ledger.txt（每行 "uid slot"）。
static constexpr wchar_t SR_LEDGER_NAME[] = L"sg_slot_ledger.txt";
struct SrLedgerRec { uint64_t uid; uint32_t slot; };
static SrLedgerRec s_ledger[1024] = {};
static int s_ledgerCount = 0;
static bool s_ledgerLoaded = false;
static void SrLoadLedger() {
    if (s_ledgerLoaded) return;
    s_ledgerLoaded = true;
    s_ledgerCount = 0;
    wchar_t path[512] = {};
    if (!SaveGuardJoinPath(path, _countof(path), g_guardBackupRoot, SR_LEDGER_NAME))
        return;
    FILE* f = _wfopen(path, L"r");
    if (!f) return;
    uint64_t u = 0; uint32_t s = 0;
    while (s_ledgerCount < 1024 && fscanf(f, "%llu %u\n", &u, &s) == 2) {
        s_ledger[s_ledgerCount].uid = u;
        s_ledger[s_ledgerCount].slot = s;
        ++s_ledgerCount;
    }
    fclose(f);
}
static bool SrLedgerHas(uint64_t uid, uint32_t slot) {
    SrLoadLedger();
    for (int i = 0; i < s_ledgerCount; ++i)
        if (s_ledger[i].uid == uid && s_ledger[i].slot == slot) return true;
    return false;
}
static void SrLedgerAdd(uint64_t uid, uint32_t slot) {
    SrLoadLedger();
    for (int i = 0; i < s_ledgerCount; ++i)
        if (s_ledger[i].uid == uid && s_ledger[i].slot == slot) return;
    if (s_ledgerCount >= 1024) return;
    s_ledger[s_ledgerCount].uid = uid;
    s_ledger[s_ledgerCount].slot = slot;
    ++s_ledgerCount;
    wchar_t path[512] = {};
    if (!SaveGuardJoinPath(path, _countof(path), g_guardBackupRoot, SR_LEDGER_NAME))
        return;
    FILE* f = _wfopen(path, L"w");
    if (!f) return;
    for (int i = 0; i < s_ledgerCount; ++i)
        fwprintf(f, L"%llu %u\n", s_ledger[i].uid, s_ledger[i].slot);
    fclose(f);
}

// eventOwner 读取（PA L274-293 同款：status+0x10→vtable[1] getter）
static bool SrGetEventOwner(void* status, void** ownerOut) {
    if (!status || !ownerOut) return false;
    unsigned char* interfaceObject =
        reinterpret_cast<unsigned char*>(status) + 0x10;
    if (!IsReadable(interfaceObject, sizeof(void*)) ||
        !IsReadable(*reinterpret_cast<void**>(interfaceObject),
                    sizeof(void*) * 2)) return false;
    void** vtable = *reinterpret_cast<void***>(interfaceObject);
    SrEventOwnerFunction getter =
        reinterpret_cast<SrEventOwnerFunction>(vtable[1]);
    if (!getter || !IsReadable(reinterpret_cast<void*>(getter), 1))
        return false;
    void* eventOwner = getter(interfaceObject);
    if (!eventOwner ||
        !IsReadable(reinterpret_cast<unsigned char*>(eventOwner) + 0x08, 0x38))
        return false;
    *ownerOut = eventOwner;
    return true;
}

// int map 写 + notify（PA L296-329 同款）
static bool SrSetStatusInt(void* status, const char* key, int value) {
    if (!status || !key || strlen(key) > sizeof(u64)) return false;
    u64 packedKey = 0;
    memcpy(&packedKey, key, strlen(key));
    SrMapLookupResult result = {};
    SrMapLookupResult* found = SrSafeMapLookup(
        sr_statusIntLookup,
        reinterpret_cast<unsigned char*>(status) + 0x18, &result,
        &packedKey);
    if (!found || !found->node ||
        !IsReadable(reinterpret_cast<unsigned char*>(found->node) + 0x18,
                    sizeof(int))) return false;
    int* nativeValue = reinterpret_cast<int*>(
        reinterpret_cast<unsigned char*>(found->node) + 0x18);
    if (*nativeValue == value) return true;  // 幂等短路（不触发 notify）
    *nativeValue = value;

    void* eventOwner = nullptr;
    if (!SrGetEventOwner(status, &eventOwner)) return false;
    SrMapLookupResult eventResult = {};
    SrMapLookupResult* eventFound = SrSafeMapLookup(
        sr_eventLookup,
        reinterpret_cast<unsigned char*>(eventOwner) + 0x08, &eventResult,
        &packedKey);
    if (!eventFound || !eventFound->node ||
        !IsReadable(reinterpret_cast<unsigned char*>(eventFound->node) + 0x18,
                    0x20)) return false;
    SrSafeEventNotify(
        sr_eventNotify,
        reinterpret_cast<unsigned char*>(eventFound->node) + 0x18);
    return *nativeValue == value;
}

// u64 map 写 + notify（PA L332-368 同款）
static bool SrSetStatusU64(void* status, const char* key, u64 value) {
    if (!status || !key || strlen(key) > sizeof(u64)) return false;
    u64 packedKey = 0;
    memcpy(&packedKey, key, strlen(key));
    SrMapLookupResult result = {};
    SrMapLookupResult* found = SrSafeMapLookup(
        sr_statusU64Lookup,
        reinterpret_cast<unsigned char*>(status) + 0x58, &result,
        &packedKey);
    if (!found || !found->node ||
        !IsReadable(reinterpret_cast<unsigned char*>(found->node) + 0x18,
                    sizeof(u64))) return false;
    u64* nativeValue = reinterpret_cast<u64*>(
        reinterpret_cast<unsigned char*>(found->node) + 0x18);
    if (*nativeValue == value) return true;  // 幂等短路
    *nativeValue = value;

    void* eventOwner = nullptr;
    if (!SrGetEventOwner(status, &eventOwner) ||
        !IsReadable(reinterpret_cast<unsigned char*>(eventOwner) + 0x48, 0x38))
        return false;
    SrMapLookupResult eventResult = {};
    SrMapLookupResult* eventFound = SrSafeMapLookup(
        sr_eventLookup,
        reinterpret_cast<unsigned char*>(eventOwner) + 0x48, &eventResult,
        &packedKey);
    if (!eventFound || !eventFound->node ||
        !IsReadable(reinterpret_cast<unsigned char*>(eventFound->node) + 0x18,
                    0x20)) return false;
    SrSafeEventNotify(
        sr_eventNotify,
        reinterpret_cast<unsigned char*>(eventFound->node) + 0x18);
    return *nativeValue == value;
}

// ============================================================
// 结构读取（PA 同款）
// ============================================================
// gimmickId：status+0x240→holder→data→[0]（PA L253-264）
static bool SrReadGimmickId(void* status, u64* gimmickId) {
    void* holder = nullptr;
    void* data = nullptr;
    if (!SrReadPtr(status, SR_GIMMICK_DATA, &holder) ||
        !SrReadPtr(holder, 0, &data) ||
        !IsReadable(data, sizeof(u64))) return false;
    const u64 id = *reinterpret_cast<const u64*>(data);
    if (!id) return false;
    *gimmickId = id;
    return true;
}

// slotLimit：data+0x128 level == "2" ? 3 : 1（PA L374-391）
static bool SrReadSlotLimit(void* machineStatus, size_t* limitOut) {
    void* holder = nullptr;
    void* data = nullptr;
    if (!SrReadPtr(machineStatus, SR_GIMMICK_DATA, &holder) ||
        !SrReadPtr(holder, 0, &data) ||
        !IsReadable(reinterpret_cast<unsigned char*>(data) + 0x128,
                    sizeof(void*) + sizeof(u64))) return false;
    const char* level = *reinterpret_cast<const char* const*>(
        reinterpret_cast<unsigned char*>(data) + 0x128);
    const u64 levelLength = *reinterpret_cast<const u64*>(
        reinterpret_cast<unsigned char*>(data) + 0x130);
    const bool levelTwo = levelLength == 1 && level &&
        IsReadable(level, 1) && level[0] == '2';
    *limitOut = levelTwo ? SR_MACHINE_SLOTS : 1;
    return true;
}

// timer 直读（PA L411-424）
static bool SrReadSlotTimers(void* machineStatus, size_t slot,
                             u64* startOut, u64* durationOut) {
    const unsigned char* bytes =
        reinterpret_cast<const unsigned char*>(machineStatus);
    if (!IsReadable(bytes + SR_TIME_VALUE + slot * sizeof(u64), sizeof(u64)) ||
        !IsReadable(bytes + SR_CREATE_TIME + slot * sizeof(u64), sizeof(u64)))
        return false;
    *startOut = *reinterpret_cast<const u64*>(
        bytes + SR_TIME_VALUE + slot * sizeof(u64));
    *durationOut = *reinterpret_cast<const u64*>(
        bytes + SR_CREATE_TIME + slot * sizeof(u64));
    return true;
}

// backing 指针（PA L394-408；只读不做修改）
static bool SrReadBacking(void* machineStatus, size_t slot,
                          void*** backingOut) {
    if (!machineStatus || !backingOut ||
        !IsReadable(reinterpret_cast<unsigned char*>(machineStatus) +
                        SR_ITEM_LIST, sizeof(void*))) return false;
    void** backing = *reinterpret_cast<void***>(
        reinterpret_cast<unsigned char*>(machineStatus) + SR_ITEM_LIST);
    const size_t first = slot * SR_SLOT_INGREDIENTS;
    if (!backing || !IsReadable(backing, first * sizeof(void*)))
        return false;
    *backingOut = backing;
    return true;
}

// 机器白名单判定
static bool SrIsMachineId(u64 gimmickId) {
    for (size_t i = 0; i < SR_MACHINE_ID_COUNT; ++i)
        if (SR_MACHINE_IDS[i] == gimmickId) return true;
    return false;
}

// status 是否为 PA 加工机器（双 vtable 匹配 + gimmickId 白名单）
static bool SrIsMachineStatus(void* status, u64* uidOut, u64* gidOut) {
    if (!IsReadable(status, 0x300)) return false;
    if (*reinterpret_cast<const uintptr_t*>(status) !=
            sr_exeBase + SR_GIMMICK_VTABLE_RVA ||
        *reinterpret_cast<const uintptr_t*>(
            reinterpret_cast<unsigned char*>(status) + 0x10) !=
            sr_exeBase + SR_GIMMICK_VTABLE2_RVA) return false;
    const u64 uid = *reinterpret_cast<const u64*>(
        reinterpret_cast<const unsigned char*>(status) + SR_UNIQUE_ID);
    u64 gid = 0;
    if (!SrReadGimmickId(status, &gid) || !SrIsMachineId(gid)) return false;
    if (uidOut) *uidOut = uid;
    if (gidOut) *gidOut = gid;
    return true;
}

// v1.2.8：仅 vtable 类判定（CGimmickStatus 同类，RTTI 实证树液提取器
// 等无独立子类）——白名单外病灶（offlist，如触发器 uid=22B8B）修复用：
// gimmickId 不在 29 项白名单 ≠ 不是机器类对象，vtable 双验仍必须过
static bool SrIsGimmickStatusClass(void* status) {
    if (!IsReadable(status, 0x300)) return false;
    return *reinterpret_cast<const uintptr_t*>(status) ==
               sr_exeBase + SR_GIMMICK_VTABLE_RVA &&
           *reinterpret_cast<const uintptr_t*>(
               reinterpret_cast<const unsigned char*>(status) + 0x10) ==
               sr_exeBase + SR_GIMMICK_VTABLE2_RVA;
}

// ============================================================
// 世界上下文
// ============================================================
static bool SrGetSaveObject(void** saveOut) {
    if (!sr_exeBase) return false;
    void* root = nullptr;
    void* save = nullptr;
    if (!SrReadPtr(reinterpret_cast<void*>(sr_exeBase + SR_GAME_ROOT_RVA),
                   0, &root) || !root) return false;
    if (!SrReadPtr(root, SR_SAVE_OFFSET, &save) || !save) return false;
    *saveOut = save;
    return true;
}

// ============================================================
// B 类：gadget 链健康巡检（只报告）+ A 类候选收集
// 枚举器骨架 = PA L1370-1440（sentinel/declared/prev 链验证同款）
// ============================================================
static void SrInspectSave(void* save) {
    sr_candidateCount = 0;
    size_t broken = 0;
    size_t visited = 0;
    size_t machines = 0;
    size_t residueSlots = 0;
    size_t chainBreakAt = 0;
    bool chainBroken = false;

    void* sentinel = nullptr;
    if (!SrReadPtr(save, SR_GIMMICK_LIST, &sentinel) || !sentinel ||
        !IsReadable(reinterpret_cast<unsigned char*>(save) +
                        SR_GIMMICK_LIST + sizeof(void*), sizeof(u64))) {
        Log("[SaveGuard/Repair] patrol: gimmick list unreadable, skip\n");
        return;
    }
    const u64 declared = *reinterpret_cast<const u64*>(
        reinterpret_cast<unsigned char*>(save) + SR_GIMMICK_LIST +
        sizeof(void*));
    if (declared == 0 || declared > SR_MAX_GIMMICKS) {
        Log("[SaveGuard/Repair] patrol: declared=%llu invalid, skip\n",
            static_cast<unsigned long long>(declared));
        return;
    }

    void* node = *reinterpret_cast<void**>(sentinel);
    void* previous = sentinel;
    for (u64 i = 0; i < declared; ++i) {
        // 节点基本健康：next/prev/status 三指针可读（PA L1410-1411）
        if (!node || node == sentinel ||
            !IsReadable(node, sizeof(void*) * 3)) {
            chainBroken = true;
            chainBreakAt = static_cast<size_t>(i);
            break;
        }
        // prev 链验证（PA L1419-1421）——断裂即链结构损坏
        if (*reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(node) + sizeof(void*)) !=
            previous) {
            chainBroken = true;
            chainBreakAt = static_cast<size_t>(i);
            break;
        }
        void* status = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(node) + sizeof(void*) * 2);
        visited++;

        if (status) {
            // B 类健康度：status 页可读 + vtable 指针可读（悬空 vtable = 释放对象）
            const bool statusOk = IsReadable(status, 0x300) &&
                IsReadable(*reinterpret_cast<void**>(status),
                           sizeof(void*) * 4);
            if (!statusOk) {
                if (broken < 4) sr_brokenSample[broken] = status;
                broken++;
            } else if (SrIsMachineStatus(status, nullptr, nullptr)) {
                // A 类候选收集（仅加工机器）
                machines++;
                size_t slotLimit = 1;
                if (!SrReadSlotLimit(status, &slotLimit))
                    slotLimit = 1;  // 读取失败保守只扫槽 0
                for (size_t slot = 0;
                     slot < slotLimit && slot < SR_MACHINE_SLOTS; ++slot) {
                    u64 start = 0, duration = 0;
                    if (!SrReadSlotTimers(status, slot, &start, &duration))
                        continue;
                    if (start == 0 && duration == 0) continue;
                    char idKey[8] = {};
                    _snprintf_s(idKey, sizeof(idKey), _TRUNCATE,
                                "itemID%zu", slot);
                    u64 itemID = 0;
                    bool mapInserted = true;
                    const bool mapRead = SrReadStatusU64Inserted(
                        status, idKey, &itemID, &mapInserted);
                    // 病灶 = timer 挂着 且 itemID 键原本存在但值为 0。
                    // inserted=true（键不存在刚被建零值节点）说明该对象不是
                    // 加工机器槽语义（如树液提取器用 count 键体系）——不判病灶
                    // （v1.1.1 修正：此前会把正常生产的提取器误判并清其时间戳）。
                    //
                    // v1.1.2 语义文档（DeepSeek 点审）：find-or-insert 探测是改
                    // 变性操作——真病灶若为 key-missing 型（崩溃发生在 timer 写
                    // 后 itemID 写前，键从未存在），本探测会替它建出 itemID=0
                    // 键，首周期跳过，第二周期（+15s）按 key-exists-value-0
                    // 正常修复。收敛方向正确（修复终态本就要写 0），代价仅
                    // key-missing 型病灶延迟一个巡检周期；非机器对象已被
                    // 白名单+双 vtable 挡住，探测无受害者。
                    if (!mapRead || mapInserted || itemID != 0) continue;
                    // backing 残留检查（report-only 路径）
                    bool residue = false;
                    void** backing = nullptr;
                    if (SrReadBacking(status, slot, &backing)) {
                        const size_t first = slot * SR_SLOT_INGREDIENTS;
                        for (size_t k = 0; k < SR_SLOT_INGREDIENTS; ++k) {
                            if (backing[first + k]) { residue = true; break; }
                        }
                    }
                    if (residue) residueSlots++;
                    if (sr_candidateCount < SR_MAX_CANDIDATES) {
                        SrCandidate* c =
                            &sr_candidates[sr_candidateCount++];
                        c->status = status;
                        c->uid = *reinterpret_cast<const u64*>(
                            reinterpret_cast<const unsigned char*>(status) +
                            SR_UNIQUE_ID);
                        c->slot = slot;
                        c->start = start;
                        c->duration = duration;
                        c->itemID = 0;
                        c->backingResidue = residue;
                        c->isDangling = false;  // A 类病灶
                        c->keyMissing = false;
                        c->alive = false;
                    }
                }
            }
        }
        previous = node;
        node = *reinterpret_cast<void**>(node);
    }

    // B 类日志（变化才打，防刷屏）
    if (chainBroken && sr_chainBreakLoggedFor != chainBreakAt) {
        sr_chainBreakLoggedFor = chainBreakAt;
        LogE("[SaveGuard/Repair] patrol: gimmick chain BREAK at index %zu "
             "(node=%p sentinel=%p) — B 类损伤（靠近对应物体可能崩溃），"
             "建议回传日志取证\n", chainBreakAt, node, sentinel);
    }
    if (broken != sr_brokenSeen) {
        sr_brokenSeen = broken;
        if (broken > 0) {
            LogE("[SaveGuard/Repair] patrol: %zu gadget status 损坏/悬空"
                 "（首样本 %p）— B 类损伤，靠近对应物体可能崩溃\n",
                 broken, sr_brokenSample[0]);
        }
    }
    (void)visited; (void)machines; (void)residueSlots;
    Log("[SaveGuard/Repair] patrol: gadgets=%llu machines=%zu "
        "candidates=%zu broken=%zu%s\n",
        static_cast<unsigned long long>(declared), machines,
        sr_candidateCount, broken, chainBroken ? " chain-break" : "");
}

// ============================================================
// v1.2.1 B 类 v1.2：全局上下文缓存 map 巡检（玩家崩溃精确签名，FN-015）
// 链路：[exe+0x1104C10]=注册表 → 0x29C60(R)=上下文对象 ctx（仅进档后
// 有效——FirstPass 链路保证）→ ctx+0x58 map（uid→对象缓存）逐桶逐
// 节点验可读性。
// 本机/玩家双实证崩溃签名：RVA 0x5D9022 `mov rax,[rdi]`（rdi=node+0x18）
// 读悬空节点——本巡检用 IsReadable(node,0x20) 抓同型节点，无需复现即可
// 自动检出（v1.1.4 缓存化 IsReadable 后遍历速度恢复；QPC 护栏 3s 免 v1.1.3
// 卡死事故重演）
// map 布局（0x3FD80 反汇编实证）：+0x8 哨兵 / +0x18 桶数组 / +0x30 掩码；
// 桶 16B（+0 尾哨兵、+8 链头）；节点 +0x8 next / +0x10 key / +0x18 value。
// 返回 false=超时护栏触发（调用方应自禁用巡检链）。
// ============================================================
// SEH：实测读 8 字节（VirtualQuery 盲区检测——VQ 报可读但实测读崩=盲区，
// 即游戏崩溃点真实不可访问）。纯内存读不调游戏函数、无递归风险。
__declspec(noinline) static bool SrSehRead8(const void* p, u64* out) {
    // 2026-10-28【关键修正】：SEH 在本游戏里靠不住 —— 游戏自带 VEH 会在访问违例时
    //   抢先 TerminateProcess，__except 根本轮不到执行（实测崩溃：
    //   saveguard.dll+0xAB64 = 本函数 +0x14，读地址 0x324B0，进程直接消失、
    //   连 QoL_crash 目录都没有）。
    //   改为「先判可读性，再读」：VirtualQuery 不触发访问违例，坏指针直接返回 false。
    if (!p || !out) return false;
    if (!IsReadable(p, sizeof(u64))) return false;
    __try {
        *out = *reinterpret_cast<const u64*>(p);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SrPatrolContextMap() {
    void* registry = nullptr;
    if (!SrReadPtr(reinterpret_cast<void*>(
                       sr_exeBase + SR_REGISTRY_GLOBAL_RVA), 0,
                   &registry) ||
        !registry || !IsReadable(registry, 0x78)) return true;
    void* ctx = SrSafeRegistryLookup(sr_registryLookup, registry);
    if (!ctx || !IsReadable(ctx, 0x90)) return true;

    unsigned char* base = reinterpret_cast<unsigned char*>(ctx) + 0x58;
    void* sentinel = nullptr;
    void* buckets = nullptr;
    if (!SrReadPtr(base, 0x8, &sentinel) || !sentinel) return true;
    if (!SrReadPtr(base, 0x18, &buckets) || !buckets) return true;
    if (!IsReadable(base + 0x30, sizeof(u64))) return true;
    const u64 mask = *reinterpret_cast<const u64*>(base + 0x30);
    if (mask == 0 || mask > 0x100000ULL) return true;  // 荒谬掩码跳出
    const size_t bucketCount = static_cast<size_t>(mask) + 1;

    LARGE_INTEGER t0 = {}, freq = {};
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&freq);

    size_t nodes = 0;
    size_t brokenNodes = 0;    // VQ 报不可读：节点内存悬空
    size_t brokenValues = 0;   // VQ 报不可读：value 悬空
    size_t blindNodes = 0;     // VQ 报可读但 SEH 实测读 node+0x18 崩=盲区
    size_t blindValues = 0;    // VQ 报可读但 SEH 实测读 value+0x3C0 崩=盲区
    void* firstBrokenNode = nullptr;
    unsigned char* cursor = reinterpret_cast<unsigned char*>(buckets);
    for (size_t b = 0; b < bucketCount; ++b, cursor += 16) {
        // 每 256 桶查一次预算（缓存化 IsReadable 后正常档全程 <<3s）
        if ((b & 0xFF) == 0) {
            LARGE_INTEGER t1 = {};
            QueryPerformanceCounter(&t1);
            if ((t1.QuadPart - t0.QuadPart) * 1000 >=
                static_cast<LONGLONG>(freq.QuadPart) *
                    static_cast<LONGLONG>(SR_PATROL_MAX_MS)) {
                LogE("[SaveGuard/Repair] ctx-map timeout >%ums — "
                     "本轮放弃（结构异常或环境慢），护栏生效\n",
                     SR_PATROL_MAX_MS);
                return false;
            }
        }
        if (!IsReadable(cursor, 16)) break;
        void* tail = *reinterpret_cast<void**>(cursor);
        void* node = *reinterpret_cast<void**>(cursor + 8);
        if (!node || node == sentinel || node == tail) continue;
        size_t hops = 0;
        while (node && node != sentinel && node != tail &&
               hops < 65536 && nodes < 0x100000) {
            if (!IsReadable(node, 0x20)) {
                ++brokenNodes;   // VQ 报告不可读：节点内存悬空
                if (!firstBrokenNode) firstBrokenNode = node;
                break;
            }
            // v1.2.2：VQ 可读≠真实可读——SEH 实测读崩溃点 node+0x18（mov rax,[rdi]）
            // 区分 A VirtualQuery 盲区（VQ 报可读但实测读崩=游戏崩溃点）vs B 真竞态
            u64 sehNodeVal = 0;
            if (!SrSehRead8(reinterpret_cast<unsigned char*>(node) + 0x18,
                            &sehNodeVal)) {
                ++blindNodes;   // 崩溃点：VQ 报可读但实测读崩（盲区=A 实锤）
                if (!firstBrokenNode) firstBrokenNode = node;
            }
            void* value = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(node) + 0x18);
            if (value && !IsReadable(value, 0x400)) {
                ++brokenValues;  // VQ 报告不可读：value 悬空
            } else if (value) {
                u64 sehVal = 0;
                if (!SrSehRead8(reinterpret_cast<const void*>(
                                    reinterpret_cast<uintptr_t>(value) + 0x3C0),
                                    &sehVal)) {
                    ++blindValues;  // value+0x3C0：VQ 报可读但测试读崩（盲区）
                }
            }
            ++nodes;
            node = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(node) + 8);
            ++hops;
        }
        if (nodes >= 0x100000) break;
    }

    const size_t brokenTotal = brokenNodes + brokenValues + blindNodes + blindValues;
    if (brokenTotal != sr_ctxBrokenSeen) {
        sr_ctxBrokenSeen = brokenTotal;
        sr_ctxBrokenSample = firstBrokenNode;
        if (brokenTotal > 0) {
            LogE("[SaveGuard/Repair] ctx-map patrol: VQ悬空 node=%zu value=%zu "
                 "| SEH盲区 node=%zu value=%zu / 共%zu节点（首%p）— %s，请回传日志\n",
                 brokenNodes, brokenValues, blindNodes, blindValues, nodes,
                 firstBrokenNode,
                 (blindNodes > 0 || blindValues > 0)
                 ? "VirtualQuery盲区实锤（VQ报可读但实测读崩=游戏崩溃点，A）"
                 : "VQ可读且SEH实测全正常=排除A，指向B懒释放竞态");
        } else {
            Log("[SaveGuard/Repair] ctx-map patrol: healthy nodes=%zu\n", nodes);
        }
    }
    return true;
}

// ============================================================
// D 类：机器槽 itemID 引用全景 dump（v1.2.3，report-only）
// 背景（2026-10-16 实锤）：无 MOD 用同一坏档也崩=游戏 v1.20 原生 bug，
//   污染源=早期 MOD 写入的坏数据固化进档。崩溃链（FN-015）：
//   树机 map1["itemID%d"] → value1 → 全局 ctx+0x58 map(0x3FD80 find-or-insert)
//   → 节点+0x18 读崩。A 类只查"timer 挂着但 itemID==0"（缺失态）覆盖不到
//   "itemID 有值但引用悬垂"形态（candidates=0 实证）。
// 本函数：进档后一次性 dump 全部机器槽全景——
//   每槽 itemID 原始值 / 该值是否存在于 ctx map（数值对照全表 key）
//   / timer 状态 / itemID 作指针 SEH 试探读。
//   不做任何修复，输出数据供定位污染形态（下一版再放开净化）。
// 纯读（全桶扫描 map 不走 find-or-insert，收集 ctx keys 只读遍历）。
// ============================================================

// 收集 ctx+0x58 map 全部节点 key（只读遍历，同 SrPatrolContextMap 结构）
static bool SrCollectCtxKeys(u64* keys, size_t maxKeys, size_t* countOut) {
    if (!keys || !countOut) return false;
    *countOut = 0;
    void* registry = nullptr;
    if (!SrReadPtr(reinterpret_cast<void*>(
                       sr_exeBase + SR_REGISTRY_GLOBAL_RVA), 0,
                   &registry) ||
        !registry || !IsReadable(registry, 0x78)) return false;
    void* ctx = SrSafeRegistryLookup(sr_registryLookup, registry);
    if (!ctx || !IsReadable(ctx, 0x90)) return false;

    unsigned char* base = reinterpret_cast<unsigned char*>(ctx) + 0x58;
    void* sentinel = nullptr;
    void* buckets = nullptr;
    if (!SrReadPtr(base, 0x8, &sentinel) || !sentinel) return false;
    if (!SrReadPtr(base, 0x18, &buckets) || !buckets) return false;
    if (!IsReadable(base + 0x30, sizeof(u64))) return false;
    const u64 mask = *reinterpret_cast<const u64*>(base + 0x30);
    if (mask == 0 || mask > 0x100000ULL) return false;
    const size_t bucketCount = static_cast<size_t>(mask) + 1;

    size_t n = 0;
    size_t hops = 0;
    for (size_t b = 0; b < bucketCount && n < maxKeys; ++b) {
        unsigned char* cursor =
            reinterpret_cast<unsigned char*>(buckets) + b * 16;
        if (!IsReadable(cursor, 16)) continue;
        void* tail = *reinterpret_cast<void**>(cursor);
        void* node = *reinterpret_cast<void**>(cursor + 8);
        if (!node || node == tail) continue;
        while (node && node != sentinel && node != tail &&
               hops < 65536 && n < maxKeys) {
            ++hops;
            if (!IsReadable(node, 0x20)) break;
            keys[n++] = *reinterpret_cast<const u64*>(
                reinterpret_cast<unsigned char*>(node) + 0x10);
            node = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(node) + 8);
        }
        if (hops >= 65536) break;
    }
    *countOut = n;
    return true;
}

// ---- E 类体检辅助：读物品 itemId（item+0 vtable，data 链 fn/ItemInfo 通用：
// holder=*(item+0x240)? 不——物品结构直接 itemId 在对象头附近。
// 权威：chestsort ReadItem：itemId=*(u64*)(**((item)+0x240)→data)? 查证：
// chestsort.cpp ReadItem 走 ITEM_DATA_HOLDER_OFFSET=0x240 → data → itemId。
// 与 PA/SaveGuard GIMMICK_DATA_HOLDER 同构（对象+0x240=holder→0=data→u64 id）
static bool SrReadItemId(void* item, u64* idOut) {
    if (!item || !idOut) return false;
    void* holder = nullptr;
    if (!SrReadPtr(item, 0x240, &holder)) return false;
    void* data = nullptr;
    if (!SrReadPtr(holder, 0, &data)) return false;
    if (!IsReadable(data, sizeof(u64))) return false;
    const u64 id = *reinterpret_cast<const u64*>(data);
    if (!id) return false;
    *idOut = id;
    return true;
}

// E 类体检辅助：校验单个物品，返回 bitflag（0=健康）
//   bit0=悬垂（id 不在 ctx 表）  bit1=refcount<=0  bit2=栈结构坏（0x260 不可读）
static unsigned SrItemHealthFlags(void* item, const u64* ctxKeys,
                                  size_t ctxKeyCount) {
    unsigned flags = 0;
    if (!item) return 0;
    u64 id = 0;
    if (SrReadItemId(item, &id)) {
        bool inCtx = false;
        for (size_t k = 0; k < ctxKeyCount; ++k) {
            if (ctxKeys[k] == id) { inCtx = true; break; }
        }
        if (!inCtx) flags |= 1u;  // 悬垂（未注册全局物品表）
    } else {
        flags |= 4u;  // 结构链坏（读不出 id——烂账对象典型特征）
    }
    if (IsReadable(reinterpret_cast<unsigned char*>(item) + SR_ITEM_REFCOUNT,
                   sizeof(LONG))) {
        const LONG refc = *reinterpret_cast<const LONG*>(
            reinterpret_cast<const unsigned char*>(item) + SR_ITEM_REFCOUNT);
        if (refc <= 0) flags |= 2u;  // 无引用孤儿
    }
    // v1.3.15：堆叠数越界（bit3 = 8）。
    //   偏移 0x260 与上限 999 均为三处同源实证一致：PA
    //   NEARBY_ITEM_STACK_COUNT_OFFSET、ChestSort / AutoHarvest 的
    //   ReadStackCount 共同口径（c < 0 || c > 999 即拒不认账）。
    //   来源：2026-10-25 全仓死档风险审计 P2-3 —— AutoHarvest 物化路径把
    //   u64 窄化为 int 时未 clamp 到 999，可造出超堆叠脏物品落盘；这类
    //   物品在拾取/关箱时会被游戏静默删除（玩家感知为"东西凭空没了"）。
    //   沿用本函数"只报不动"的 E 类定位：脏堆叠能定位到雷物品即可，
    //   擅自改写数量会与游戏侧记账打架。
    if (IsReadable(reinterpret_cast<unsigned char*>(item) + SR_ITEM_STACK_COUNT,
                   sizeof(int))) {
        const int sc = *reinterpret_cast<const int*>(
            reinterpret_cast<const unsigned char*>(item) + SR_ITEM_STACK_COUNT);
        if (sc < 0 || sc > SR_ITEM_STACK_MAX) flags |= 8u;
    }
    return flags;
}

// ============================================================
// v1.3.15：存档时钟健康巡检（只报告，绝不改写）
//
// 来源：2026-10-25 全仓死档风险审计 P1-4。TimeFreeze 的 WorkerThread 每
//   100ms 把采样基准写回 save+0x3270 / 0x3278 —— 这是套件内唯一会直写
//   存档时钟字段的路径。一旦把 0 写进去，跨天判定 / 作物生长 / 机器
//   start+duration 的时间基准全部丢失，是最坏形态的存档损坏。
//   （TimeFreeze v1.3.2 起已在写入侧加"零值不写 + SEH"，这里是第二道
//   独立观测，不依赖它自己报告。）
//
// 为什么只报不改：时钟是派生量，SG 没有任何手段推导出"正确应该是多少"，
//   写回任何猜测值都比留着更糟。观测的价值在于——玩家/开发者能在作物
//   倒退、机器计时错乱之前就看到"时钟被写坏了"，并据此回退存档。
//   这与 SG 一贯的 E 类定位（backing 残留只报不动）保持一致。
// ============================================================
static void SrHealthCheckClock(void* save) {
    if (!save) return;
    const unsigned char* gp =
        reinterpret_cast<const unsigned char*>(save) + SR_SAVE_GAME_SECOND;
    if (!IsReadable(gp, sizeof(u64))) return;
    const u64 gs = *reinterpret_cast<const u64*>(gp);

    if (gs == 0) {
        if (!sr_clockZeroLogged) {
            sr_clockZeroLogged = true;
            LogE("[SaveGuard/Repair] 时钟体检: save+0x3270 累计游戏秒为 0"
                 " —— 时间基准丢失（跨天/作物/机器计时全依赖它）。"
                 "最常见成因是 TimeFreeze 在采样完成前写回。"
                 "仅报告不改写（时钟无法凭空推导）：请检查存档，"
                 "必要时用 Village_Save_Backups 回退\n");
        }
        return;
    }
    // 倒退检测：只认"大幅倒退"，避开跨天/睡觉的正常波动，防误报。
    if (sr_lastGameSecond != 0 && gs < sr_lastGameSecond) {
        const u64 drop = sr_lastGameSecond - gs;
        if (drop > SR_CLOCK_DROP_ALERT_SEC && !sr_clockDropLogged) {
            sr_clockDropLogged = true;
            LogE("[SaveGuard/Repair] 时钟体检: 累计游戏秒倒退 %llu 秒"
                 "（%llu -> %llu）。若伴随作物倒退/机器计时错乱，"
                 "疑似时钟被回拉。仅报告不改写\n",
                 static_cast<unsigned long long>(drop),
                 static_cast<unsigned long long>(sr_lastGameSecond),
                 static_cast<unsigned long long>(gs));
        }
    }
    sr_lastGameSecond = gs;
}

static void SrHealthCheckItems(void* save) {
    // ctx 全表（对照悬垂）
    static u64 ctxKeys[4096];
    static size_t ctxKeyCount = 0;
    ctxKeyCount = 0;
    const bool ctxReady = SrCollectCtxKeys(
        ctxKeys, sizeof(ctxKeys) / sizeof(ctxKeys[0]), &ctxKeyCount);

    size_t scanned = 0;     // 扫过的物品槽总数
    size_t badDangling = 0, badRefcount = 0, badStruct = 0, badDup = 0;
    size_t badStack = 0;    // v1.3.15：堆叠数越界（<0 或 >999）
    void* firstBad = nullptr;

    // 视图：item 指针 → 出现槽位（重复检测，容量 512 够用）
    struct Seen { void* item; size_t count; };
    static Seen seen[512];
    size_t seenCount = 0;

    // ---- 扫描面 1：玩家背包 30 槽 ----
    // v1.3.1 链修正（权威=ChestSort GetWorldContext + AutoHarvest 同源实证）：
    //   [exe+GAME_ROOT]+0x208 = player；背包数组 = **player+0x32C0（内嵌，非指针）**
    //   v1.2.9~v1.3.0 多走了一层 player+0x32B8→status 再取 +0x32C0
    //   （那是“位置读取通道”，+0x32C0 落在无关内存）
    //   → 实测日志 `背包链未就绪（status(+0x32B8)不可读）` 即此病根
    void* playerRoot = nullptr;
    const char* chainFail = "";
    if (!SrReadPtr(reinterpret_cast<void*>(sr_exeBase + SR_GAME_ROOT_RVA),
                   SR_PLAYER_STATUS_CHAIN_1, &playerRoot) || !playerRoot) {
        chainFail = "player(root+0x208)不可读";
    } else if (!IsReadable(reinterpret_cast<unsigned char*>(playerRoot) +
                               SR_PLAYER_ITEMS,
                           SR_PLAYER_CAPACITY_MAX * sizeof(void*))) {
        chainFail = "items(player+0x32C0)不可读";
    } else {
        {
            void** inv = reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(playerRoot) +
                SR_PLAYER_ITEMS);
            for (size_t i = 0; i < SR_PLAYER_CAPACITY_MAX; ++i) {
                void* item = inv[i];
                if (!item) continue;
                ++scanned;
                if (!firstBad) firstBad = item;
                unsigned f = ctxReady ? SrItemHealthFlags(item, ctxKeys,
                                                          ctxKeyCount) : 0;
                if (f & 1u) ++badDangling;
                if (f & 2u) ++badRefcount;
                if (f & 4u) ++badStruct;
                // 重复检测
                bool found = false;
                for (size_t k = 0; k < seenCount; ++k) {
                    if (seen[k].item == item) { seen[k].count++; found = true;
                        ++badDup; break; }
                }
                if (!found && seenCount < 512)
                    seen[seenCount++] = { item, 1 };
            }
        }
    }
    if (chainFail[0] && !sr_itemChainLogged) {
        sr_itemChainLogged = true;
        Log("[SaveGuard/Repair] 物品体检: 背包链未就绪（%s）"
            "——档加载后自动重试\n", chainFail);
    }

    // ---- 扫描面 2：gimmick 链全部容器 backing（箱子等）----
    // 复用 dump 的链遍历骨架：save+0x3440 sentinel 链；容器判定简单化——
    // 读得出 0x2B8 backing 且内容为物品指针列表即扫（宽进严报，只报告）
    void* sentinel = nullptr;
    if (SrReadPtr(save, SR_GIMMICK_LIST, &sentinel) && sentinel &&
        IsReadable(reinterpret_cast<unsigned char*>(save) +
                       SR_GIMMICK_LIST + sizeof(void*), sizeof(u64))) {
        const u64 declared = *reinterpret_cast<const u64*>(
            reinterpret_cast<unsigned char*>(save) + SR_GIMMICK_LIST +
            sizeof(void*));
        if (declared > 0 && declared <= SR_MAX_GIMMICKS) {
            void* node = *reinterpret_cast<void**>(sentinel);
            void* previous = sentinel;
            for (u64 i = 0; i < declared; ++i) {
                if (!node || node == sentinel ||
                    !IsReadable(node, sizeof(void*) * 3)) break;
                if (*reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(node) +
                        sizeof(void*)) != previous) break;
                void* status = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(node) +
                    sizeof(void*) * 2);
                if (status && SrIsGimmickStatusClass(status)) {
                    void** backing = nullptr;
                    if (SrReadBacking(status, 0, &backing) && backing) {
                        // 保守扫 30 槽（箱子容量）；backing 每槽 8 间隔
                        for (size_t s = 0; s < SR_PLAYER_CAPACITY_MAX; ++s) {
                            if (!IsReadable(backing + s, sizeof(void*))) break;
                            void* item = backing[s];
                            if (!item) continue;
                            ++scanned;
                            if (!firstBad) firstBad = item;
                            unsigned f = ctxReady
                                ? SrItemHealthFlags(item, ctxKeys, ctxKeyCount)
                                : 0;
                            if (f & 1u) ++badDangling;
                            if (f & 2u) ++badRefcount;
                            if (f & 4u) ++badStruct;
                            if (f & 8u) ++badStack;
                            bool found = false;
                            for (size_t k = 0; k < seenCount; ++k) {
                                if (seen[k].item == item) {
                                    seen[k].count++; found = true;
                                    ++badDup; break; }
                            }
                            if (!found && seenCount < 512)
                                seen[seenCount++] = { item, 1 };
                        }
                    }
                }
                previous = node;
                node = *reinterpret_cast<void**>(node);
            }
        }
    }

    // ---- 汇总（v1.3.0：计数变化 或 首次扫到真实槽 才打，防刷屏又可见证）----
    const size_t badTotal =
        badDangling + badRefcount + badStruct + badDup + badStack;
    const bool verdictChanged = (badTotal != sr_itemBadSeen);
    const bool firstRealScan = (scanned > 0 && !sr_itemRealLogged);
    if (verdictChanged || firstRealScan) {
        sr_itemBadSeen = badTotal;
        if (scanned > 0) sr_itemRealLogged = true;
        if (badTotal > 0) {
            LogE("[SaveGuard/Repair] 物品体检: 烂账物品 %zu 项"
                 "（悬垂%zu 无引用%zu 结构坏%zu 重复挂载%zu 堆叠越界%zu / 扫描槽%zu "
                 "首例 %p）— E 类旧档遗留（历史 MOD 缺陷产物，拾取/关箱时"
                 "会被游戏静默删除），仅报告不动；若频繁拾取消失建议整理"
                 "该物品\n",
                 badTotal, badDangling, badRefcount, badStruct, badDup,
                 badStack, scanned, firstBad);
        } else if (scanned > 0) {
            Log("[SaveGuard/Repair] 物品体检: healthy（扫描 %zu 槽，"
                "ctx键%zu）\n", scanned, ctxKeyCount);
        }
        // scanned==0 且链未就绪：已由上方单次提示覆盖，不重复刷日志
    }
}

static void SrDumpMachineSlots(void* save, bool verbose) {
    // v1.2.5：verbose=详细 dump（每档首轮）；之后每轮静默收集 D 候选
    // （覆盖晚加载机器/新污染），悬垂线索行仍每轮打印
    // 先收集 ctx map 全部 key（数值对照：槽 itemID 是否存在于全局表）
    static u64 ctxKeys[4096];
    static size_t ctxKeyCount = 0;
    ctxKeyCount = 0;
    const bool ctxKeysReady = SrCollectCtxKeys(
        ctxKeys, sizeof(ctxKeys) / sizeof(ctxKeys[0]), &ctxKeyCount);
    // v1.3.18：刷新 ctx 健康判定（保险丝，见 s_ctxKeyVerdict 处注释）。
    //   空 ctx 表=查找链失配（v1.3.17 accessor 0x2A430 误用事故），此时
    //   SrCtxHasId 恒 miss 会把所有已完成机器误判崩源 → 强制保守不动。
    s_ctxKeyVerdict = (ctxKeysReady && ctxKeyCount > 0) ? 1 : -1;

    void* sentinel = nullptr;
    if (!SrReadPtr(save, SR_GIMMICK_LIST, &sentinel) || !sentinel ||
        !IsReadable(reinterpret_cast<unsigned char*>(save) +
                        SR_GIMMICK_LIST + sizeof(void*), sizeof(u64)))
        return;
    const u64 declared = *reinterpret_cast<const u64*>(
        reinterpret_cast<unsigned char*>(save) + SR_GIMMICK_LIST +
        sizeof(void*));
    if (declared == 0 || declared > SR_MAX_GIMMICKS) return;

    // v1.3.7：读存档累计游戏秒（gameSecond），作为 KEY-MISSING 损坏型判定安全门
    std::int64_t gsNow = -1;
    {
        const unsigned char* gp = reinterpret_cast<const unsigned char*>(save) +
            SR_SAVE_GAME_SECOND;
        if (IsReadable(gp, sizeof(u64)))
            gsNow = static_cast<std::int64_t>(*reinterpret_cast<const u64*>(gp));
    }

    if (verbose)
        Log("[SaveGuard/Repair] slotdump: ctxKeys=%zu(ready=%d) begin\n",
            ctxKeyCount, ctxKeysReady ? 1 : 0);
    void* node = *reinterpret_cast<void**>(sentinel);
    void* previous = sentinel;
    size_t machineIdx = 0;
    size_t slotIssues = 0;      // v1.3.0：仅计 KEY-MISSING（timer 挂着无 itemID 键）
    size_t danglingIssues = 0;  // v1.3.0：悬垂疑似（id 不在 ctx，只报告型）
    size_t offWhitelist = 0;
    size_t offSapExempt = 0;    // v1.3.0：树液提取器型豁免（count 键体系）
    size_t slotPhantom = 0;     // v2.1：幻影槽（0x338==0 未点火，PA 直写残留，清空候选）
    for (u64 i = 0; i < declared; ++i) {
        if (!node || node == sentinel ||
            !IsReadable(node, sizeof(void*) * 3)) break;
        if (*reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(node) + sizeof(void*)) !=
            previous) break;
        void* status = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(node) + sizeof(void*) * 2);

        // v1.3.8：PA↔SG 协调标记——PA 接管机器写 pa_own=1。
        // v1.3.11【放宽 pa_own】崩源检测（含 phantom 清崩源槽）对 PA 机器也执行；
        //   KEY-MISSING/dangling 仍整台跳过（防误清 PA 功能槽）。故此处不再整台
        //   continue，改为记录 paManaged 并在下方分支门控。
        bool paManaged = SrIsPaManaged(status);
        if (paManaged) {
            Log("[SaveGuard/Repair] slotdump: machine PA 自有（pa_own）——"
                "崩源检测仍执行（清 phantom 崩源槽），KEY-MISSING/dangling 跳过\n");
        }

        if (status && SrIsMachineStatus(status, nullptr, nullptr)) {
            u64 uid = *reinterpret_cast<const u64*>(
                reinterpret_cast<const unsigned char*>(status) +
                SR_UNIQUE_ID);
            size_t slotLimit = 1;
            if (!SrReadSlotLimit(status, &slotLimit)) slotLimit = 1;
            // v1.2.6：任一槽有非零 itemID 或 timer 挂着才进（key-missing 病灶
            // （22B8B 型：timer 挂着但 map1 从未有 itemID 键）全空 ID，旧的
            // 仅非零 ID 短路把它们全跳过 = 巡检盲区实锺）
            // v1.2.7：槽收集/检测固定 0..2（slotLimit 读失败保守 1 会漏检
            // slot1/2 病灶——游戏 level-2 槽循环固定 0..2；slotLimit 仅控制
            // 显示循环）
            bool anyNonZero = false;
            struct SlotInfo { u64 id; bool hit; u64 start; u64 duration; };
            SlotInfo slots[SR_MACHINE_SLOTS] = {};
            for (size_t slot = 0; slot < SR_MACHINE_SLOTS; ++slot) {
                char idKey[8] = {};
                _snprintf_s(idKey, sizeof(idKey), _TRUNCATE,
                            "itemID%zu", slot);
                slots[slot].hit = SrMapValuePure(
                    status, 0x58, idKey, &slots[slot].id);
                SrReadSlotTimers(status, slot, &slots[slot].start,
                                 &slots[slot].duration);
                if ((slots[slot].hit && slots[slot].id != 0) ||
                    slots[slot].start != 0 || slots[slot].duration != 0)
                    anyNonZero = true;
            }
            if (!anyNonZero) {
                previous = node;
                node = *reinterpret_cast<void**>(node);
                continue;
            }
            ++machineIdx;
            if (verbose)
                Log("[SaveGuard/Repair] slotdump: machine#%zu uid=%llX status=%p\n",
                    machineIdx, static_cast<unsigned long long>(uid), status);
            // v1.3.11：崩源哨兵不再读本机 [status+0x60]（v1.3.10 比对错表）。
            //   改在下方 per-slot 用 SrCtxHasId 查【全局产出注册表】(id→物品)。
            for (size_t slot = 0;
                 slot < slotLimit && slot < SR_MACHINE_SLOTS; ++slot) {
            // v1.2.6 D2 / v1.2.7 证据升级：key-missing 病灶（timer 挂着
            // 但 map1 无 itemID 键）——0x5D9022 崩溃取证实锺 fault=0x0
            // read / rdi=0 / rsi=0：map1 未命中→rsi/rdi 必为 0→读 [0]
            // 【v2 修正 2026-10-22】上述判定对「加工中」机器误报：PA v2
            // （production_automation_v2.inl；production_automation.inl 为
            // v1 已过滤，不作分析依据）PaClassifyMachine 定义——start!=0 &&
            // duration!=0 && gameSecond<start+duration 即 PaState::Running
            // （产出尚未生成，map1 本就无 itemID 键）。清之会丢失玩家进行中
            // 的生产（实测 25 台运行中机器被清空）。故白名单机器 KEY-MISSING
            // 改为【只报告、不清】：运行中机器由游戏在 timer 到点时自身正常
            // 产出，无需 MOD 干预；真正「卡在完成后仍无 itemID」的损坏型
            // （0x5D9022 同签名）后续用 gameSecond 门控清理（读存档内累计
            // 游戏秒与 start+duration 比较）或 offlist Path 2 处理。
            // v1.3.11：崩源精确判定（Ghidra 0x5D9022 全函数实锤）
            //   崩路 = 加工已完成(elapsed>=duration) 且【全局 ctx 表】itemID{slot}
            //   查表值 == 哨兵（缺键→返回哨兵；或 hit 但值==哨兵 均为崩源）。
            //   比对的是全局表而非逐机器 status（v1.3.10 错比对 status+0x60）。
            //   仅"已完成"才进崩路，故要求 completed，避免误清运行中机器。
            {
                // v1.3.12：按 Ghidra FUN_1405d8e20（rva 0x5D8E20）反编译实锤重写
                //   崩源判定。游戏逻辑（param_1=machine, param_2=slot）：
                //     status   = *(machine + 0xb0);
                //     duration = status[0x2a0 + slot*8];
                //     if (duration == 0) return 0;                  // 非崩
                //     if (duration <= gameSecond - status[0x288+slot*8]) {
                //         V   = status->map[0x58]["itemID{slot}"];   // 本机取 id
                //         hit = ctx->map[0x58].find(V);              // 全局注册表
                //         return **(hit ? hit->val : NULL) + 0x3c0;  // miss→读[0]崩
                //     }
                //   ⇒ 崩源 ⇔ 已完成 && (缺 itemID 键 || id==0 || id 不在全局表)
                //   全局表是 id→物品对象（u64 键）；v1.3.11 用字符串 "itemID0"
                //   去查它 → 恒不命中 → 判据退化成"所有已完成槽都是崩源"，
                //   会误清正常机器（2026-10-24 实测 10 台无差别被清）。此处更正。
                const u64 sNow = slots[slot].start;
                const u64 dNow = slots[slot].duration;
                const bool completed =
                    (gsNow >= 0) &&
                    (dNow >= 60 && dNow <= 2592000ULL) &&
                    (sNow <= 0x8000000ULL) &&
                    (gsNow >= static_cast<std::int64_t>(sNow)) &&
                    (gsNow - static_cast<std::int64_t>(sNow) >=
                        static_cast<std::int64_t>(dNow));
                // 本机 map 里的 id 是否有效（存在且非 0）
                const bool haveId = slots[slot].hit && slots[slot].id != 0;
                // id 是否登记在全局产出注册表（1命中 0未命中 -1不可读）
                const int idInCtx = haveId ? SrCtxHasId(slots[slot].id) : 0;
                // 不可读时保守视为合法（idInCtx==-1 → idOk=true → 不动内存）
                const bool idOk = haveId && (idInCtx != 0);
                const bool crashSrc = (dNow != 0) && completed && !idOk;
                if (dNow != 0 && completed) {
                    Log("[SaveGuard/Repair] DIAG uid=%llX slot%zu completed: "
                        "hit=%d id=%llX inCtx=%d → crashSrc=%d\n",
                        static_cast<unsigned long long>(uid), slot,
                        slots[slot].hit ? 1 : 0,
                        static_cast<unsigned long long>(slots[slot].id),
                        idInCtx, crashSrc ? 1 : 0);
                }
                if (crashSrc) {
                // v1.3.9：二次确认门——!hit(0x58缺itemID键)+timer挂 即 0x5D9022
                // 崩源。首次只报告登记，二次(或跨boot)才进入下方候选逻辑，避免
                // 误删正常/瞬态机器（其他MOD直写合法机器等）。
                if (!SrLedgerHas(uid, static_cast<uint32_t>(slot))) {
                    SrLedgerAdd(uid, static_cast<uint32_t>(slot));
                    ++slotIssues;
                    Log("[SaveGuard/Repair] slotdump: KEYLESS uid=%llX "
                        "slot%zu t=%llu/%llu（0x58缺itemID键=崩源；"
                        "首次登记待二次确认→只报告不动）\n",
                        static_cast<unsigned long long>(uid), slot,
                        static_cast<unsigned long long>(slots[slot].start),
                        static_cast<unsigned long long>(slots[slot].duration));
                    continue;
                }
                // v2.1（2026-10-23）幻影槽判别：status+0x338 = 加工组件指针
                //   （FN-026/029 实锤：仅玩家点火时游戏才构造 CCom_Gimmick）。
                //   ==0 = 从未点火 = PA 直写未点火机器留下的幻影槽（timer 挂着
                //   但游戏永不加工，每日处理时当垃圾清掉=「夜间材料消失」根源）
                //   → 加入清空候选。!=0 = 已点火 = 加工中/完成合法态 → 保持只
                //   报告不清（FN-028 教训：曾误清 25 台运行中机器）。修复复用
                //   offlist 同款链：活性校验 + 双帧复验键仍不存在 + CAS 清 timer
                //   + 白名单 inProc 复位。
                void* compX = nullptr;
                if (IsReadable(reinterpret_cast<const unsigned char*>(status) +
                                   0x338,
                               sizeof(void*))) {
                    compX = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(status) + 0x338);
                }
                const u64 phS = slots[slot].start;
                const u64 phD = slots[slot].duration;
                // 安全门（对齐 offlist）：双非零 + 配方时长 60s~30天 + start 合理
                if (compX == nullptr && phS != 0 && phD != 0 &&
                    phD >= 60 && phD <= 2592000ULL &&
                    phS <= 0x8000000ULL) {
                    // backing 残留检查（防误毁玩家原料）：有残留只报告不动
                    bool bResidue = false;
                    void** backing = nullptr;
                    if (SrReadBacking(status, slot, &backing) && backing) {
                        const size_t first = slot * SR_SLOT_INGREDIENTS;
                        for (size_t k = 0; k < SR_SLOT_INGREDIENTS; ++k) {
                            if (backing[first + k]) { bResidue = true; break; }
                        }
                    }
                    if (sr_candidateCount < SR_MAX_CANDIDATES) {
                        SrCandidate* c = &sr_candidates[sr_candidateCount++];
                        c->status = status;
                        c->uid = uid;
                        c->slot = slot;
                        c->start = phS;
                        c->duration = phD;
                        c->itemID = haveId ? slots[slot].id : 0;
                        c->backingResidue = bResidue;
                        c->isDangling = false;
                        // v1.3.12：崩源两种形态——keyMissing=缺键/id 为 0；
                        //   valueBad=键在但 id 不在全局注册表（悬垂/垃圾 id）。
                        c->keyMissing = !haveId;
                        c->valueBad = haveId;
                        c->sentinelVal = 0;
                        c->alive = false;
                    }
                    ++slotPhantom;
                    LogE("[SaveGuard/Repair] slotdump: PHANTOM uid=%llX "
                         "slot%zu t=%llu/%llu（0x338==0 未点火=PA直写幻影槽；"
                         "backing残留=%d→%s）\n",
                         static_cast<unsigned long long>(uid), slot,
                         static_cast<unsigned long long>(phS),
                         static_cast<unsigned long long>(phD),
                         bResidue ? 1 : 0,
                         bResidue ? "只报告不动" : "清空候选");
                } else if (!paManaged) {
                    ++slotIssues;
                    // v1.3.7：恢复损坏型修复（gameSecond 门控）。
                    // v1.3.11：pa_own 机器不进 KEY-MISSING 清理（防误清 PA 功能槽）
                    // v2 修正(10-22)为防误清运行中机器把白名单 KEY-MISSING 改只报告，
                    // 但崩毁档的"卡在完成后仍无 itemID"（0x5D9022 同签名）也落此支，
                    // 不再被修 → 崩毁档不救。此处用 gameSecond 门控恢复：
                    //   已过期完成（gameSecond >= start+duration）= 真损坏 → 收集候选修复
                    //   运行中（gameSecond < start+duration 或不可读）= 合法中间态 → 只报告
                    bool gsExpired = (gsNow >= 0) &&
                        (phS <= 0x8000000ULL) &&
                        (phD >= 60 && phD <= 2592000ULL) &&
                        (static_cast<std::int64_t>(phS) <=
                            static_cast<std::int64_t>(INT64_MAX)) &&
                        (gsNow >= static_cast<std::int64_t>(phS)) &&
                        (gsNow - static_cast<std::int64_t>(phS) >=
                            static_cast<std::int64_t>(phD));
                    if (gsExpired && sr_candidateCount < SR_MAX_CANDIDATES) {
                        // 复用 PHANTOM 同款 backing 残留保护（防误毁玩家原料）
                        bool bResidue = false;
                        void** backing = nullptr;
                        if (SrReadBacking(status, slot, &backing) && backing) {
                            const size_t first = slot * SR_SLOT_INGREDIENTS;
                            for (size_t k = 0; k < SR_SLOT_INGREDIENTS; ++k) {
                                if (backing[first + k]) { bResidue = true; break; }
                            }
                        }
                        SrCandidate* c = &sr_candidates[sr_candidateCount++];
                        c->status = status;
                        c->uid = uid;
                        c->slot = slot;
                        c->start = phS;
                        c->duration = phD;
                        c->itemID = haveId ? slots[slot].id : 0;
                        c->backingResidue = bResidue;
                        c->isDangling = false;
                        // v1.3.12：崩源两种形态——keyMissing=缺键/id 为 0；
                        //   valueBad=键在但 id 不在全局注册表（悬垂/垃圾 id）。
                        c->keyMissing = !haveId;
                        c->valueBad = haveId;
                        c->sentinelVal = 0;
                        c->alive = false;
                        Log("[SaveGuard/Repair] slotdump: KEY-MISSING uid=%llX "
                            "slot%zu t=%llu/%llu（gameSecond=%lld 已过期完成无itemID="
                            "真损坏→修复候选%s）\n",
                            static_cast<unsigned long long>(uid), slot,
                            static_cast<unsigned long long>(phS),
                            static_cast<unsigned long long>(phD),
                            static_cast<long long>(gsNow),
                            bResidue ? "（backing残留，复验保护）" : "");
                    } else {
                        // v1.3.9：已二次确认→运行中也收候选（!hit 即崩源，
                        // 与是否过期无关；清理时双帧复验键仍缺才清，防瞬态误清）
                        bool bResidue = false;
                        void** backing = nullptr;
                        if (SrReadBacking(status, slot, &backing) && backing) {
                            const size_t first = slot * SR_SLOT_INGREDIENTS;
                            for (size_t k = 0; k < SR_SLOT_INGREDIENTS; ++k) {
                                if (backing[first + k]) { bResidue = true; break; }
                            }
                        }
                        if (sr_candidateCount < SR_MAX_CANDIDATES) {
                            SrCandidate* c = &sr_candidates[sr_candidateCount++];
                            c->status = status;
                            c->uid = uid;
                            c->slot = slot;
                            c->start = phS;
                            c->duration = phD;
                            c->itemID = haveId ? slots[slot].id : 0;
                            c->backingResidue = bResidue;
                            c->isDangling = false;
                            c->keyMissing = !haveId;
                            c->valueBad = haveId;
                            c->sentinelVal = 0;
                            c->alive = false;
                        }
                        ++slotPhantom;
                        LogE("[SaveGuard/Repair] slotdump: KEYLESS uid=%llX "
                            "slot%zu t=%llu/%llu（0x58缺itemID键=崩源；"
                            "二次确认(运行中)→修复候选%s）\n",
                            static_cast<unsigned long long>(uid), slot,
                            static_cast<unsigned long long>(phS),
                            static_cast<unsigned long long>(phD),
                            bResidue ? "（backing残留，复验保护）" : "");
                    }
                } else {
                    // v1.3.11：pa_own 机器崩源命中但非 phantom（已点火+完成但全局
                    //   表缺 itemID{slot} 的异常态）→ 只报告不动，防误清 PA 功能槽
                    ++slotIssues;
                    LogE("[SaveGuard/Repair] slotdump: KEYLESS(PA) uid=%llX "
                        "slot%zu t=%llu/%llu（pa_own 非 phantom 崩源→只报告）\n",
                        static_cast<unsigned long long>(uid), slot,
                        static_cast<unsigned long long>(phS),
                        static_cast<unsigned long long>(phD));
                }
            }
            }
                if (!slots[slot].hit || slots[slot].id == 0) continue;
                // itemID 是否存在于 ctx map（数值对照）
                bool inCtx = false;
                for (size_t k = 0; k < ctxKeyCount; ++k) {
                    if (ctxKeys[k] == slots[slot].id) { inCtx = true; break; }
                }
                // itemID 作指针 SEH 试探读（揭示 key 语义：字符串指针/数字）
                u64 probe = 0;
                const bool ptrReadable = SrSehRead8(
                    reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(slots[slot].id)), &probe);
                // 异常槽 = itemID 有值但 ctx 查不到（find-or-insert 会新建节点）
                const bool danglingLike = ctxKeysReady && !inCtx;
                if (danglingLike) ++danglingIssues;
                if (verbose || danglingLike)
                Log("[SaveGuard/Repair] slotdump:   slot%zu id=%llX "
                    "inCtx=%d ptrReadable=%d probe=%llX t=%llu/%llu%s\n",
                    slot,
                    static_cast<unsigned long long>(slots[slot].id),
                    inCtx ? 1 : 0, ptrReadable ? 1 : 0,
                    static_cast<unsigned long long>(probe),
                    static_cast<unsigned long long>(slots[slot].start),
                    static_cast<unsigned long long>(slots[slot].duration),
                    danglingLike ? "  <== 链dangling疑似" : "");
                // v1.2.7：D1 dangling 撤销自动清理——“完成待领取”机器的产出
                // itemID 未注册 ctx 属正常懒注册态（20:29:19 实证：21AF1 组
                // 4 台完成机器被误清产出，教训）；只报告（上方 dump 行）
            }
        } else if (status && IsReadable(status, 0x300)) {
            // v1.2.7→v1.2.8：白名单外病灶（offlist）——游戏 0x5D8E20 崩溃
            // 查询不查我们的 gimmickId 白名单，v1.2.7 实测同型病灶
            // uid=22B8B（t=11165497/259200 挂 3 天配方）只报告未清→仍崩
            // （fault=0/rdi=0/rsi=0 同签名）。v1.2.8 放开清理，安全门：
            // ①双 vtable 类判定（CGimmickStatus 同类，防误伤非 gadget 对象）
            // ②start/duration 双非零 + duration 在配方合理区间 60s~30天
            // ③纯读 map1 无 itemID 键（修复时再复验）
            if (!SrIsGimmickStatusClass(status)) {
                // 非机器类对象：其 0x288/0x2A0 语义未知，只报告绝不动
                for (size_t slot = 0; slot < SR_MACHINE_SLOTS; ++slot) {
                    u64 s = 0, d = 0;
                    if (!SrReadSlotTimers(status, slot, &s, &d)) continue;
                    if (s == 0 && d == 0) continue;
                    if (s > 0x8000000ULL || d > 0x8000000ULL) continue;
                    char idKey[8] = {};
                    _snprintf_s(idKey, sizeof(idKey), _TRUNCATE,
                                "itemID%zu", slot);
                    u64 id = 0;
                    if (SrMapValuePure(status, 0x58, idKey, &id)) continue;
                    const unsigned char* sc =
                        reinterpret_cast<const unsigned char*>(status);
                    u64 uid = 0;
                    if (IsReadable(sc + SR_UNIQUE_ID, sizeof(u64)))
                        uid = *reinterpret_cast<const u64*>(sc + SR_UNIQUE_ID);
                    ++offWhitelist;
                    LogE("[SaveGuard/Repair] non-class-keymissing uid=%llX "
                         "status=%p slot%zu t=%llu/%llu（非机器类对象字段"
                         "语义未知，只报告不动）\n",
                         static_cast<unsigned long long>(uid), status, slot,
                         static_cast<unsigned long long>(s),
                         static_cast<unsigned long long>(d));
                }
            } else {
                // CGimmickStatus 类 + 白名单外 gimmickId（如 22B8B）：
                // 同型 NULL 崩病灶，收集候选走修复链（双帧复验+活性校验）
                const unsigned char* sc =
                    reinterpret_cast<const unsigned char*>(status);
                u64 uid = 0;
                if (IsReadable(sc + SR_UNIQUE_ID, sizeof(u64)))
                    uid = *reinterpret_cast<const u64*>(sc + SR_UNIQUE_ID);
                // v1.3.0 安全门：树液提取器（gimmickId 240250000）产出走 count 键
                // 体系、天然无 itemID 键（FN-014）——KEY-MISSING 清理按 gimmickId
                // 显式豁免，防 v1.1.0"误清提取器 timer"事故从 offlist 路径复发。
                // v1.3.12：但【崩源(0x5D9022)检测对树液提取器执行】——PA 旧版直写
                // 会在树液机造「未点火+全局 ctx 表缺 itemID」幻影槽（已完成即崩），
                // 此前整台 sapExempt 把它漏了→用户去树液采集点必崩。崩源判定只认
                // 全局表值==哨兵（与 machine 分支一致）；completed 或 未点火
                // (compX==nullptr)即清；其他 sap 情况仍跳过 KEY-MISSING，防误清
                // count 键机。
                u64 gidOff = 0;
                const bool sapExempt =
                    SrReadGimmickId(status, &gidOff) &&
                    gidOff == SR_SAP_EXTRACTOR_ID;
                if (sapExempt) ++offSapExempt;
                for (size_t slot = 0; slot < SR_MACHINE_SLOTS; ++slot) {
                    u64 s = 0, d = 0;
                    if (!SrReadSlotTimers(status, slot, &s, &d)) continue;
                    // 双非零（真实配方 timer 特征；only-one-nonzero 属
                    // 半写状态同样危险但 v1.2.7 实测未见，保守只抓双非零）
                    if (s == 0 || d == 0) continue;
                    // 配方时长合理区间：1 分钟 ~ 30 天
                    if (d < 60 || d > 2592000ULL) continue;
                    if (s > 0x8000000ULL) continue;
                    if (sapExempt) {
                        // v1.3.12：树液提取器用【同一份反汇编判据】检测崩源。
                        //   硬约束：v1.1.0 事故就是"运行中提取器 timer 被清"，
                        //   故这里只认【已完成】(elapsed>=duration)——运行中的
                        //   提取器（实测 s0=11165497/259200 三天周期）绝不动。
                        //   完成态若 (缺 itemID 键 || id 不在全局注册表) → 游戏
                        //   必进 0x5D9022 崩路 → 确定性崩源 → 清 timer（等价于
                        //   游戏 duration==0 的早退语义，且不动 itemID/count 键
                        //   ——count 键体系不插陌生键）。
                        char sidKey[8] = {};
                        _snprintf_s(sidKey, sizeof(sidKey), _TRUNCATE,
                                    "itemID%zu", slot);
                        u64 sapId = 0;
                        const bool sapHaveId =
                            SrMapValuePure(status, 0x58, sidKey, &sapId) &&
                            sapId != 0;
                        const int sapInCtx = sapHaveId ? SrCtxHasId(sapId) : 0;
                        const bool sapIdOk = sapHaveId && (sapInCtx != 0);
                        const bool sapCompleted =
                            (gsNow >= 0) && (s <= 0x8000000ULL) &&
                            (static_cast<std::int64_t>(gsNow) >=
                                static_cast<std::int64_t>(s)) &&
                            (static_cast<std::int64_t>(gsNow) -
                                static_cast<std::int64_t>(s) >=
                                static_cast<std::int64_t>(d));
                        Log("[SaveGuard/Repair] DIAG-SAP uid=%llX slot%zu "
                            "t=%llu/%llu gs=%lld haveId=%d id=%llX inCtx=%d "
                            "completed=%d crashSrc=%d\n",
                            static_cast<unsigned long long>(uid), slot,
                            static_cast<unsigned long long>(s),
                            static_cast<unsigned long long>(d),
                            static_cast<long long>(gsNow),
                            sapHaveId ? 1 : 0,
                            static_cast<unsigned long long>(sapId),
                            sapInCtx, sapCompleted ? 1 : 0,
                            (sapCompleted && !sapIdOk) ? 1 : 0);
                        if (sapCompleted && !sapIdOk) {
                            // backing 残留检查（防误毁玩家原料）：有残留只报告不动
                            bool bResidue = false;
                            void** backing = nullptr;
                            if (SrReadBacking(status, slot, &backing) && backing) {
                                const size_t first = slot * SR_SLOT_INGREDIENTS;
                                for (size_t k = 0; k < SR_SLOT_INGREDIENTS; ++k)
                                    if (backing[first + k]) { bResidue = true; break; }
                            }
                            if (sr_candidateCount < SR_MAX_CANDIDATES) {
                                SrCandidate* c = &sr_candidates[sr_candidateCount++];
                                c->status = status;
                                c->uid = uid;
                                c->slot = slot;
                                c->start = s;
                                c->duration = d;
                                c->itemID = sapHaveId ? sapId : 0;
                                c->backingResidue = bResidue;
                                c->isDangling = false;
                                c->keyMissing = !sapHaveId;
                                c->valueBad = sapHaveId;
                                c->sentinelVal = 0;
                                c->alive = false;
                            }
                            LogE("[SaveGuard/Repair] slotdump: SAP-崩源 uid=%llX "
                                 "status=%p slot%zu t=%llu/%llu（已完成且%s→"
                                 "0x5D9022 崩源→清空候选%s）\n",
                                 static_cast<unsigned long long>(uid), status, slot,
                                 static_cast<unsigned long long>(s),
                                 static_cast<unsigned long long>(d),
                                 sapHaveId ? "id 不在全局注册表"
                                           : "缺 itemID 键",
                                 bResidue ? "（backing残留→复验保护）" : "");
                        }
                        continue; // 树液机其余情况一律不动（count 键体系）
                    }
                    // 白名单外同型 KEY-MISSING（非 sap）
                    char idKey[8] = {};
                    _snprintf_s(idKey, sizeof(idKey), _TRUNCATE,
                                "itemID%zu", slot);
                    u64 id = 0;
                    if (SrMapValuePure(status, 0x58, idKey, &id)) continue;
                    ++offWhitelist;
                    LogE("[SaveGuard/Repair] offlist-keymissing uid=%llX "
                         "status=%p slot%zu t=%llu/%llu（白名单外同型 NULL "
                         "崩病灶，v1.2.8 起自动清理）\n",
                         static_cast<unsigned long long>(uid), status, slot,
                         static_cast<unsigned long long>(s),
                         static_cast<unsigned long long>(d));
                    if (sr_candidateCount < SR_MAX_CANDIDATES) {
                        SrCandidate* c = &sr_candidates[sr_candidateCount++];
                        c->status = status;
                        c->uid = uid;
                        c->slot = slot;
                        c->start = s;
                        c->duration = d;
                        c->itemID = 0;
                        c->backingResidue = false;
                        c->isDangling = false;
                        // 本路径 line1535 已对命中键 continue，只处理缺键型
                        c->keyMissing = true;
                        c->alive = false;
                    }
                }
            }
        }
        previous = node;
        node = *reinterpret_cast<void**>(node);
    }
    Log("[SaveGuard/Repair] slotdump: end 机器(有产出)=%zu KEY-MISSING=%zu "
        "幻影槽=%zu 悬垂疑似=%zu 白名单外病灶=%zu"
        "%s%zu%s\n",
        machineIdx, slotIssues, slotPhantom, danglingIssues, offWhitelist,
        offSapExempt ? " 提取器豁免=" : "",
        offSapExempt, offSapExempt ? "（count 键体系，不动）" : "");
}

// ============================================================
// A 类：修复（双帧复验通过者）
// v1.1.2（DeepSeek 点审）新增活性校验：候选 status 必须仍在当前链上。
// 防幽灵修复——FirstPass→Repair 窗口（500ms+1帧）内机器被拆除时 status
// 已 free，但堆内存保留旧字节可骗过全部复验（vtable/timer/map 全匹配），
// 对已释放堆块 CAS 写零，若堆块已被复用则破坏无关对象。
// ============================================================

// v1.3.2：只报不清模式（游戏更新后的安全网）
//   游戏版本更新 → 结构偏移可能漂移 → 白名单/键名判定可能误伤正常槽。
//   在 QuickAccess 目录放 saveguard_dryrun.txt（内容任意）即进入本模式：
//   走完整识别/复验/统计流程，但【只打日志不动任何内存】。适配完成后
//   删除该文件即恢复清理能力。
static bool SrDryRunMode() {
    static ULONGLONG s_lastCheckTick = 0;
    static bool s_lastResult = false;
    const ULONGLONG nowTick = GetTickCount64();
    if (s_lastCheckTick != 0 && nowTick - s_lastCheckTick < 10000)
        return s_lastResult;   // 10s 缓存，避免高频文件探测
    s_lastCheckTick = nowTick;
    const char* dirs[] = {
        "saveguard_dryrun.txt",
        nullptr,  // 占位：QuickSearch 根由下方手动拼
    };
    (void)dirs;  // 简化：逐候选路径探测
    const char* roots[] = {
        "C:\\Users\\Public\\Documents",          // Public Documents
    };
    s_lastResult = false;
    for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); ++r) {
        char path[MAX_PATH];
        if (_snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s",
                        roots[r], "saveguard_dryrun.txt") <= 0)
            continue;
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
            s_lastResult = true;
            break;
        }
    }
    return s_lastResult;
}
static void SrRepairConfirmed(void* save) {
    unsigned repaired = 0;
    unsigned residueReported = 0;
    unsigned skipped = 0;
    // v1.3.0：跳过原因细分（原版只有总数且"全跳轮"不打印=黑盒，
    // 导致"7 台一直不清"无法归因）
    unsigned skipNotAlive = 0, skipStruct = 0, skipTimerGone = 0;
    unsigned skipChanged = 0, skipKeyPresent = 0;

    // 活性校验：单次链遍历标记候选归属（16384×512 紧凑比较，仅候选>0 时
    // 执行；健康档零开销）
    for (size_t idx = 0; idx < sr_candidateCount; ++idx)
        sr_candidates[idx].alive = false;
    {
        void* sentinel = nullptr;
        if (SrReadPtr(save, SR_GIMMICK_LIST, &sentinel) && sentinel &&
            IsReadable(reinterpret_cast<unsigned char*>(save) +
                           SR_GIMMICK_LIST + sizeof(void*), sizeof(u64))) {
            const u64 declared = *reinterpret_cast<const u64*>(
                reinterpret_cast<unsigned char*>(save) + SR_GIMMICK_LIST +
                sizeof(void*));
            if (declared > 0 && declared <= SR_MAX_GIMMICKS) {
                void* node = *reinterpret_cast<void**>(sentinel);
                void* previous = sentinel;
                for (u64 i = 0; i < declared; ++i) {
                    if (!node || node == sentinel ||
                        !IsReadable(node, sizeof(void*) * 3)) break;
                    if (*reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(node) +
                            sizeof(void*)) != previous) break;
                    void* status = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(node) +
                        sizeof(void*) * 2);
                    if (status) {
                        for (size_t idx = 0; idx < sr_candidateCount; ++idx)
                            if (sr_candidates[idx].status == status)
                                sr_candidates[idx].alive = true;
                    }
                    previous = node;
                    node = *reinterpret_cast<void**>(node);
                }
            }
        }
    }

    for (size_t idx = 0; idx < sr_candidateCount; ++idx) {
        SrCandidate* c = &sr_candidates[idx];
        if (!c->alive) {
            skipped++; skipNotAlive++;  // 已不在链上（拆除/切档后堆残留旧值）——不动
            continue;
        }
        // 复验 1：结构和 timer 仍存在
        // v1.2.8：offlist 候选（白名单外 gadgetId，如触发器 22B8B）
        // 不过 SrIsMachineStatus 白名单——用 vtable 类判定替代（RTTI 实证
        // 同为 CGimmickStatus 类；非机器类对象在扫描端已被挡住不进候选）
        u64 start = 0, duration = 0;
        const bool whitelisted =
            SrIsMachineStatus(c->status, nullptr, nullptr);
        // v1.3.12：offlist 崩源候选（树液提取器等白名单外 gimmick）走
        // valueBad 路径，必须一并放行——否则会被 skipStruct 挡掉、崩源
        // 永远清不掉（用户去树液采集点必崩的直接原因）。
        const bool structureOk =
            whitelisted ||
            ((c->keyMissing || c->valueBad) &&
             SrIsGimmickStatusClass(c->status));
        if (!structureOk ||
            !SrReadSlotTimers(c->status, c->slot, &start, &duration)) {
            skipped++; skipStruct++;
            continue;
        }
        if (start == 0 && duration == 0) { skipped++; skipTimerGone++; continue; }
        if (start != c->start || duration != c->duration) {
            skipped++; skipChanged++;  // 状态变化（重新投料或部分自愈），不动
            continue;
        }
        char idKey[8] = {};
        char countKey[9] = {};
        _snprintf_s(idKey, sizeof(idKey), _TRUNCATE, "itemID%zu", c->slot);
        _snprintf_s(countKey, sizeof(countKey), _TRUNCATE,
                    "itemVal%zu", c->slot);
        u64 itemID = 1;
        bool mapInserted = true;
        if (c->keyMissing) {
            // v1.3.14：复验口径修正——keyMissing 现在表示「id 无效」，包含
            //   ①键不存在 ②键存在但值==0（清槽收尾写 0 留下的"键存在=0"）。
            //   旧判据「键存在即视为自愈」会让 ② 被 skipKeyPresent 永久跳过
            //   （2026-10-24 实测：树液机 uid=22B8B 每轮 crashSrc=1 却清不掉）。
            //   正确判据：只有出现了【非零 id】（真自愈）才不动。
            u64 nowID = 0;
            const bool present = SrMapValuePure(c->status, 0x58, idKey, &nowID);
            if (present && nowID != 0) {
                skipped++; skipKeyPresent++;  // 已被补写有效 id——状态恢复，不动
                continue;
            }
        } else if (c->isDangling) {
            // v1.2.4 D 类复验：纯读 map 再取 itemID，且必须仍 ctx 查不到
            // （双帧 500ms 窗口内：玩家可能已换料修复/游戏可能已注册 ID）
            u64 nowID = 0;
            if (!SrMapValuePure(c->status, 0x58, idKey, &nowID) ||
                nowID != c->itemID) {
                skipped++;  // 槽状态已变化——不动
                continue;
            }
            // 重验 ctx（ID 仍查不到才算真悬垂）
            static u64 recheckKeys[4096];
            static size_t recheckCount = 0;
            recheckCount = 0;
            if (!SrCollectCtxKeys(recheckKeys, 4096, &recheckCount)) {
                skipped++;  // ctx 不可读——保守不动
                continue;
            }
            bool inCtx = false;
            for (size_t k = 0; k < recheckCount; ++k) {
                if (recheckKeys[k] == c->itemID) { inCtx = true; break; }
            }
            if (inCtx) {
                skipped++;  // 双帧窗口内 ID 已被注册（误判保护）——不动
                continue;
            }
        } else if (c->valueBad) {
            // v1.3.12：键在但 id 不在全局注册表型——双帧复验：
            //   ① 本机键仍在且值未变（否则状态已变→不动）
            //   ② 该 id 仍不在全局注册表（SrCtxHasId==0）才清；
            //      ==1 已自愈登记 → 不动；==-1 全局表不可读 → 保守不动
            u64 nowID = 0;
            if (!SrMapValuePure(c->status, 0x58, idKey, &nowID) ||
                nowID != c->itemID) {
                skipped++;
                continue;
            }
            if (SrCtxHasId(c->itemID) != 0) {
                skipped++;
                continue;
            }
        } else if (SrReadStatusU64Inserted(c->status, idKey, &itemID,
                                           &mapInserted) &&
                   (mapInserted || itemID != 0)) {
            skipped++;  // 键已不在/重建/已恢复合法——不动
            continue;
        }

        if (c->backingResidue) {
            // backing 有残留 clone：无实锤语义，只报告不动（防误毁原料）
            residueReported++;
            LogE("[SaveGuard/Repair] machine uid=0x%llx slot %zu %s"
                 "且 backing 有残留，仅报告不动；"
                 "若稳定复现请回传日志\n",
                 static_cast<unsigned long long>(c->uid), c->slot,
                 c->isDangling ? "悬垂引用（id 不在全局表）"
                              : "卡死（timer 挂着无产出）");
            continue;
        }
        // v1.2.4 D 类追加：backing 原料检查（非空即报告不动——丢原料比留着
        // 悬垂更伤玩家；需人工确认后再处理）
        if (c->isDangling) {
            void** backing = nullptr;
            bool dResidue = false;
            if (SrReadBacking(c->status, c->slot, &backing) && backing) {
                const size_t first = c->slot * SR_SLOT_INGREDIENTS;
                for (size_t k = 0; k < SR_SLOT_INGREDIENTS; ++k) {
                    if (backing[first + k]) { dResidue = true; break; }
                }
            }
            if (dResidue) {
                residueReported++;
                LogE("[SaveGuard/Repair] machine uid=0x%llx slot %zu "
                     "悬垂引用 id=0x%llX 但槽内还有原料 backing，"
                     "仅报告不动（防误毁）；若稳定复现请回传日志\n",
                     static_cast<unsigned long long>(c->uid),
                     c->slot,
                     static_cast<unsigned long long>(c->itemID));
                continue;
            }
        }

        // v1.3.2: 只报不清模式——识别到确定性病灶也不动内存（游戏更新后
        //   的安全网；适配期间观察日志足够），走完统计后直接返回。
        static bool s_dryRunAnnounced = false;
        if (SrDryRunMode()) {
            if (!s_dryRunAnnounced) {
                s_dryRunAnnounced = true;
                Log("[SaveGuard/Repair] DRY-RUN 模式（saveguard_dryrun.txt "
                    "存在）：只报告不清理\n");
            }
            Log("[SaveGuard/Repair] 本轮（dry-run）：确定性病灶 %zu 台槽"
                "已识别但未清理（删 saveguard_dryrun.txt 恢复清理）\n",
                sr_candidateCount);
            sr_nextActionAt = GetTickCount64() + SR_CRUISE_MS;
            sr_phase = SrPhase::Cruise;
            return;
        }

        // 确定性清理（PA_ROLLBACK_TIMERS 同款 CAS 序列，L866-868）
        volatile LONG64* startField =
            reinterpret_cast<volatile LONG64*>(
                reinterpret_cast<unsigned char*>(c->status) +
                SR_TIME_VALUE + c->slot * sizeof(u64));
        volatile LONG64* durationField =
            reinterpret_cast<volatile LONG64*>(
                reinterpret_cast<unsigned char*>(c->status) +
                SR_CREATE_TIME + c->slot * sizeof(u64));
        InterlockedCompareExchange64(
            startField, 0, static_cast<LONG64>(c->start));
        InterlockedCompareExchange64(
            durationField, 0, static_cast<LONG64>(c->duration));
        // v1.3.12：写 map 键的前提 = 本机 map 里确实存在该键。
        //   keyMissing 型本无键（写 0 会造出"键存在=0"新状态）；
        //   树液提取器型（count 键体系）本机同样可能无 itemID{slot} 键——
        //   此时只清 timer，绝不插入陌生键（防 v1.1.0 事故复发）。
        u64 localProbe = 0;
        const bool localKeyPresent =
            SrMapValuePure(c->status, 0x58, idKey, &localProbe);
        if (!c->keyMissing && localKeyPresent) {
            SrSetStatusU64(c->status, idKey, 0);
            SrSetStatusInt(c->status, countKey, 0);
        }

        // inProc 复位（PA L1119-1130 同款判定；v1.2.8：offlist 候选跳过
        // ——"inProc" 键对白名单外对象语义未验证，不写陌生键）
        size_t slotLimit = 1;
        if (!SrReadSlotLimit(c->status, &slotLimit)) slotLimit = 1;
        bool anyRunning = false;
        for (size_t slot = 0;
             slot < slotLimit && slot < SR_MACHINE_SLOTS; ++slot) {
            u64 s = 0, d = 0;
            if (!SrReadSlotTimers(c->status, slot, &s, &d)) continue;
            if (s != 0 || d != 0) { anyRunning = true; break; }
        }
        if (!anyRunning && whitelisted)
            SrSetStatusInt(c->status, "inProc", 0);

        repaired++;
        Log("[SaveGuard/Repair] machine uid=0x%llx slot %zu 已清理"
            "（%s：崩溃源）\n",
            static_cast<unsigned long long>(c->uid), c->slot,
            c->keyMissing
                ? "KEY-MISSING timer→空槽（timer 挂着但无 itemID 键，"
                  "游戏空槽查询 NULL 崩）"
                : c->isDangling
                      ? "悬垂引用 itemID→空槽（早期 MOD 污染，已复位"
                        "为未加工状态）"
                      : "卡死 timer：无产出状态");
    }

    sr_totalRepaired += repaired;
    sr_totalResidueReported += residueReported;
    // v1.3.0：只要进入修复阶段就打印（含"全跳轮"，附跳过原因细分）
    if (sr_candidateCount > 0) {
        Log("[SaveGuard/Repair] 本轮：修复 %u 台槽，残留报告 %u，跳过 %u"
            "（离链%u 结构/timer读%u 已清%u 值变%u 键已补%u）"
            "（会话累计修复 %u）\n",
            repaired, residueReported, skipped,
            skipNotAlive, skipStruct, skipTimerGone, skipChanged,
            skipKeyPresent, sr_totalRepaired);
    }
}

// ============================================================
// C 类：环境取证（mods 目录清单，玩家回传日志即可归因）
// ============================================================
static void SrLogEnvironment() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (!lastSlash) return;
    *lastSlash = L'\0';

    wchar_t modsDir[MAX_PATH] = {};
    _snwprintf_s(modsDir, MAX_PATH, _TRUNCATE, L"%s\\mods", exePath);

    WIN32_FIND_DATAW fd = {};
    wchar_t findPath[MAX_PATH] = {};
    _snwprintf_s(findPath, MAX_PATH, _TRUNCATE, L"%s\\*", modsDir);
    HANDLE find = FindFirstFileW(findPath, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        Log("[SaveGuard/Env] mods 目录不可读: %ls\n", modsDir);
        return;
    }
    char names[256] = {};
    char line[1100] = {};
    size_t count = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 ||
            wcscmp(fd.cFileName, L"..") == 0) continue;
        char name[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                            name, sizeof(name), nullptr, nullptr);
        if (strlen(names) + strlen(name) + 2 >= sizeof(names)) {
            snprintf(line, sizeof(line),
                     "[SaveGuard/Env] mods[%zu+]: %s", count, names);
            Log("%s\n", line);
            names[0] = '\0';
        }
        if (names[0]) strcat_s(names, ",");
        strcat_s(names, name);
        count++;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (count == 0) {
        Log("[SaveGuard/Env] mods 目录为空\n");
    } else {
        snprintf(line, sizeof(line), "[SaveGuard/Env] mods 目录 %zu 项: %s",
                 count, names);
        Log("%s\n", line);
    }
}

// ============================================================
// v1.3.12：0x5D9022 空崩点硬化（2 字节运行时补丁，只改内存不动磁盘）
//   反汇编 FUN_1405d8e20（rva 0x5D8E20）尾部：
//     0x5D9013  MOV R8,[RSP+0x20]    ; 全局注册表查表结果（未命中=哨兵）
//     0x5D9018  CMP R8,[R11+0x60]    ; 与 ctx 哨兵比较
//     0x5D901C  JZ  0x5D9022         ; 未命中 → 跳过 LEA，RDI 仍为 NULL
//     0x5D901E  LEA RDI,[R8+0x18]
//     0x5D9022  MOV RAX,[RDI]        ; ← NULL 解引用＝崩点（fault addr=0x0）
//     0x5D9025  MOV RAX,[RAX+0x3C0]
//     0x5D902C  JMP 0x5D905D         ; 函数出口
//     ...
//     0x5D905B  XOR EAX,EAX          ; 现成的 return 0 路径
//   补丁：0x5D901C 的 74 04 → 74 3D，即"未命中"时改跳 return 0。
//   语义与游戏自身 duration==0 时的早退分支完全一致（调用方 FUN_1405d9080
//   拿到 0 后走无产出路径），正常命中路径一个字节都不改。
//   门禁：18 字节上下文签名必须完全匹配，且目标跳转点必须是 XOR EAX,EAX；
//   任一不符即跳过（游戏更新后的安全网）。
// ============================================================
// v1.3.13：崩溃点硬化表（表驱动，逐条目签名校验后才落补丁）
//   每条 = 一处「查表未命中 → 解引用 NULL」同型崩点。字段：
//     siteRva : 待改字节位置（跳过赋值的那个 Jcc）
//     sigRva  : 上下文签名起点；sig/sigLen：期望字节
//     patch0/1: 改写字节（跳向该函数自带的 return 0 路径）
//     ret0Rva : 必须指向 XOR EAX,EAX（return 0 语义）
//   游戏更新后签名不符 → 该条目自动跳过并记日志，互不影响。
struct SrPatchEntry {
    const char* name;
    uintptr_t siteRva;
    uintptr_t sigRva;
    size_t sigLen;
    const unsigned char* sig;
    unsigned char patch0;
    unsigned char patch1;
    uintptr_t ret0Rva;
};

// 条目 1 — 0x5F01A2（旧 0x5D9022）：FUN_1405effa0 加工完成 → 全局产出注册表查不到 id → 读 [0]
//   build 25640656 实测：崩溃点与旧版**字节完全相同**、函数 size 均 594、偏移均 +0x202，
//   平移量 0x17180；sig/site/ret0 全部按平移后地址重新落位（JZ 相对位移仍 0x3D）。
static const unsigned char kSig_5D9022[18] = {
    0x4C, 0x8B, 0x44, 0x24, 0x20,   // MOV R8,[RSP+0x20]
    0x4D, 0x3B, 0x43, 0x60,         // CMP R8,[R11+0x60]
    0x74, 0x04,                     // JZ 0x5D9022   ← 待改
    0x49, 0x8D, 0x78, 0x18,         // LEA RDI,[R8+0x18]
    0x48, 0x8B, 0x07,               // MOV RAX,[RDI]
};
static const SrPatchEntry kSrPatchTable[] = {
    {"0x5D9022 加工完成+全局id注册表未命中→NULL解引用",
     0x5F019C, 0x5F0193, 18, kSig_5D9022, 0x74, 0x3D, 0x5F01DB},
};

static bool SrInstallOnePatch(const SrPatchEntry& e) {
    if (e.siteRva < e.sigRva) return false;
    const size_t siteOff = static_cast<size_t>(e.siteRva - e.sigRva);
    if (siteOff + 2 > e.sigLen) return false;
    unsigned char* sigBase =
        reinterpret_cast<unsigned char*>(sr_exeBase + e.sigRva);
    unsigned char* site =
        reinterpret_cast<unsigned char*>(sr_exeBase + e.siteRva);
    unsigned char* ret0 =
        reinterpret_cast<unsigned char*>(sr_exeBase + e.ret0Rva);
    if (!IsReadable(sigBase, e.sigLen) || !IsReadable(ret0, 2)) {
        Log("[SaveGuard/Guard] %s：地址不可读→跳过\n", e.name);
        return false;
    }
    if (site[0] == e.patch0 && site[1] == e.patch1) {
        Log("[SaveGuard/Guard] %s：已硬化（补丁在位）\n", e.name);
        return true;
    }
    for (size_t i = 0; i < e.sigLen; ++i) {
        if (i == siteOff || i == siteOff + 1) continue;
        if (sigBase[i] != e.sig[i]) {
            Log("[SaveGuard/Guard] %s：签名不符（+%zu 期望 %02X 实得 %02X）"
                "→跳过（构建可能已变更）\n",
                e.name, i, e.sig[i], sigBase[i]);
            return false;
        }
    }
    if (site[0] != e.sig[siteOff] || site[1] != e.sig[siteOff + 1]) {
        Log("[SaveGuard/Guard] %s：原字节 %02X %02X 非预期→跳过\n",
            e.name, site[0], site[1]);
        return false;
    }
    if (ret0[0] != 0x33 || ret0[1] != 0xC0) {
        Log("[SaveGuard/Guard] %s：跳转目标不是 XOR EAX,EAX（实得 %02X %02X）"
            "→跳过\n", e.name, ret0[0], ret0[1]);
        return false;
    }
    DWORD oldProt = 0;
    if (!VirtualProtect(site, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
        Log("[SaveGuard/Guard] %s：VirtualProtect 失败 err=%lu\n",
            e.name, GetLastError());
        return false;
    }
    site[0] = e.patch0;
    site[1] = e.patch1;
    DWORD restored = 0;
    VirtualProtect(site, 2, oldProt, &restored);
    FlushInstructionCache(GetCurrentProcess(), site, 2);
    if (site[0] != e.patch0 || site[1] != e.patch1) {
        Log("[SaveGuard/Guard] %s：写入后复验失败\n", e.name);
        return false;
    }
    Log("[SaveGuard/Guard] %s：硬化完成（未命中改走 return 0）\n", e.name);
    return true;
}

static bool SrInstallCrashGuard() {
    if (!sr_exeBase) return false;
    const int total = static_cast<int>(
        sizeof(kSrPatchTable) / sizeof(kSrPatchTable[0]));
    int ok = 0;
    for (int i = 0; i < total; ++i)
        if (SrInstallOnePatch(kSrPatchTable[i])) ++ok;
    Log("[SaveGuard/Guard] 崩溃点硬化表：%d/%d 已硬化\n", ok, total);
    return ok == total;
}

// ============================================================
// 初始化（mod_init 时机，主线程）
// ============================================================
static void InitSaveRepair() {
    sr_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!sr_exeBase) {
        sr_phase = SrPhase::Disabled;
        return;
    }

    // C 类：环境取证（mods 清单一行，exe 指纹已由 VerifyExeBuild 记录）
    SrLogEnvironment();

    // 函数指针绑定（PA L1656-1678 同款）
    sr_statusIntLookup = reinterpret_cast<SrMapLookupFunction>(
        sr_exeBase + SR_STATUS_INT_LOOKUP_RVA);
    sr_statusU64Lookup = reinterpret_cast<SrMapLookupFunction>(
        sr_exeBase + SR_STATUS_U64_LOOKUP_RVA);
    sr_eventLookup = reinterpret_cast<SrMapLookupFunction>(
        sr_exeBase + SR_EVENT_LOOKUP_RVA);
    sr_eventNotify = reinterpret_cast<SrEventNotifyFunction>(
        sr_exeBase + SR_EVENT_NOTIFY_RVA);
    sr_intrusiveRelease = reinterpret_cast<SrIntrusiveReleaseFunction>(
        sr_exeBase + SR_INTRUSIVE_RELEASE_RVA);
    sr_registryLookup = reinterpret_cast<SrRegistryLookupFunction>(
        sr_exeBase + SR_REGISTRY_LOOKUP_RVA);  // v1.2.1

    // v1.3.12：先装 0x5D9022 空崩点硬化补丁（与巡检是否 ready 无关——
    //   即使后续探针失败、巡检禁用，这一层也要挡住空崩；游戏不该崩）
    const bool guardOk = SrInstallCrashGuard();
    Log("[SaveGuard/Guard] crash_guard=%d\n", guardOk ? 1 : 0);

    // 入口可读性抽查（exe SHA 已门禁 v1.20，此处仅防异常布局）
    const uintptr_t probes[] = {
        SR_STATUS_INT_LOOKUP_RVA, SR_STATUS_U64_LOOKUP_RVA,
        SR_EVENT_LOOKUP_RVA, SR_EVENT_NOTIFY_RVA,
        SR_INTRUSIVE_RELEASE_RVA, SR_GIMMICK_VTABLE_RVA,
        SR_REGISTRY_LOOKUP_RVA,
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        if (!IsReadable(reinterpret_cast<void*>(sr_exeBase + probes[i]), 16)) {
            sr_phase = SrPhase::Disabled;
            LogE("[SaveGuard/Repair] init FAILED: RVA 0x%llX 不可读，"
                 "净化禁用\n",
                 static_cast<unsigned long long>(probes[i]));
            return;
        }
    }

    sr_repairReady = true;
    sr_lastSavePtr = nullptr;
    sr_ctxBrokenSeen = (size_t)-1;  // v1.2.1：新会话首次巡检必报
    sr_slotDumpDone = false;         // v1.2.3：D 类全景 dump 待打
    sr_itemBadSeen = (size_t)-1;     // v1.2.9：E 类体检首报
    sr_itemRealLogged = false;       // v1.3.0：真实槽体检结果重臂
    sr_itemChainLogged = false;      // v1.3.0：背包链提示重臂
    sr_nextActionAt = GetTickCount64() + SR_WORLD_PROBE_MS;
    sr_phase = SrPhase::WaitWorld;
    Log("[SaveGuard/Repair] init OK（A 类机器槽净化 + B 类 gadget/ctx-map 巡检"
        " + D 类周期化槽检 + 快照兜底）\n");

}

// ============================================================
// 主泵（mod_tick 每帧调用，内部节流）
// ============================================================
static void PumpSaveRepair() {
    if (sr_phase == SrPhase::Disabled || !sr_repairReady) return;
    const ULONGLONG now = GetTickCount64();
    if (now < sr_nextActionAt) return;

    switch (sr_phase) {
    case SrPhase::WaitWorld: {
        void* save = nullptr;
        if (SrGetSaveObject(&save) && save) {
            sr_lastSavePtr = save;
            sr_nextActionAt = now + SR_LOAD_SETTLE_MS;
            sr_phase = SrPhase::Settle;
        } else {
            sr_nextActionAt = now + SR_WORLD_PROBE_MS;
        }
        break;
    }
    case SrPhase::Settle: {
        void* save = nullptr;
        if (!SrGetSaveObject(&save) || !save) {
            // 主菜单/切档中，回等待
            sr_phase = SrPhase::WaitWorld;
            sr_nextActionAt = now + SR_WORLD_PROBE_MS;
            break;
        }
        sr_lastSavePtr = save;
        sr_nextActionAt = now + 1;
        sr_phase = SrPhase::FirstPass;
        break;
    }
    case SrPhase::FirstPass: {
        void* save = nullptr;
        if (!SrGetSaveObject(&save) || !save) {
            sr_phase = SrPhase::WaitWorld;
            sr_nextActionAt = now + SR_WORLD_PROBE_MS;
            break;
        }
        // v1.1.4：QPC 计时护栏——单轮巡检超 SR_PATROL_MAX_MS 告警并自禁用
        // （v1.1.3 事故实证：裸 VirtualQuery×安全软件内核 hook 可拖到 21~50s/轮；
        // 巡检是取证增强，任何时候不得超过游戏可玩性——超时自禁用保底）
        LARGE_INTEGER t0 = {}, t1 = {}, freq = {};
        QueryPerformanceCounter(&t0);
        SrInspectSave(save);
        // v1.2.5：换为周期化（原 v1.2.3 仅每档一轮——漏晚加载机器）
        SrDumpMachineSlots(save, !sr_slotDumpDone);
        sr_slotDumpDone = true;
        // v1.2.9 E 类：物品健康体检（背包 30 槽 + 容器 backing，只报告；
        // 烂账物品=旧档历史 MOD 缺陷遗留，给玩家定位"取出会消失"的雷物品）
        SrHealthCheckItems(save);
        // v1.3.15：存档时钟健康（save+0x3270 归零/倒退，TimeFreeze 直写
        //   风险的独立观测点；只报告不改写）
        SrHealthCheckClock(save);
        // v1.2.1：ctx-map 巡检（进档后 ctx；超时护栏触发则连同 gadget 巡检
        // 一起自禁用——两者共享 SR_PATROL_MAX_MS 预算理念）
        const bool ctxOk = SrPatrolContextMap();
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&freq);
        const double elapsedMs =
            (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        if (elapsedMs > static_cast<double>(SR_PATROL_MAX_MS) || !ctxOk) {
            LogE("[SaveGuard/Repair] patrol_timeout %.0fms > %ums — "
                 "净化巡检自禁用（机器槽/快照/文件校验不受影响），"
                 "请回传日志\n", elapsedMs, SR_PATROL_MAX_MS);
            sr_phase = SrPhase::Disabled;
            break;
        }
        // v1.1.4 关键修复：出口用新鲜时间算节流——旧版用巡检前抓的 now，
        // 巡检耗时 > SR_CRUISE_MS 时 nextActionAt 已在过去 = 死循环连环轰炸
        // （打点实锤：21s 巡检 + 15s 旧基准 = 巡检一结束下一轮立即开跑）
        const ULONGLONG finishedAt = GetTickCount64();
        if (sr_candidateCount > 0) {
            sr_nextActionAt = finishedAt + SR_CONFIRM_MS;
            sr_phase = SrPhase::Confirm;
        } else {
            sr_nextActionAt = finishedAt + SR_CRUISE_MS;
            sr_phase = SrPhase::Cruise;
        }
        break;
    }
    case SrPhase::Confirm: {
        void* save = nullptr;
        if (!SrGetSaveObject(&save) || !save || save != sr_lastSavePtr) {
            // 双帧窗口内切档 → 回到探测
            sr_phase = SrPhase::WaitWorld;
            sr_nextActionAt = now + SR_WORLD_PROBE_MS;
            break;
        }
        sr_nextActionAt = now + 1;
        sr_phase = SrPhase::Repair;
        break;
    }
    case SrPhase::Repair: {
        // v1.1.2（DeepSeek 点审）：执行前重验 save 指针（Confirm 只隔 1 帧，
        // 但防止理论上的跨帧切档路径；活性校验在 SrRepairConfirmed 内完成）
        void* save = nullptr;
        if (!SrGetSaveObject(&save) || !save || save != sr_lastSavePtr) {
            sr_phase = SrPhase::WaitWorld;
            sr_nextActionAt = now + SR_WORLD_PROBE_MS;
            break;
        }
        SrRepairConfirmed(save);
        // v1.1.4：同型修复——修复出口用新鲜时间（旧版旧 now 同样可致连环轰炸）
        sr_nextActionAt = GetTickCount64() + SR_CRUISE_MS;
        sr_phase = SrPhase::Cruise;
        break;
    }
    case SrPhase::Cruise: {
        void* save = nullptr;
        if (!SrGetSaveObject(&save) || !save) {
            sr_phase = SrPhase::WaitWorld;
            sr_nextActionAt = now + SR_WORLD_PROBE_MS;
            break;
        }
        if (save != sr_lastSavePtr) {
            // 新读档：重新进入首次扫描（等世界加载）
            sr_lastSavePtr = save;
            sr_brokenSeen = 0;            // B 类统计重置（新世界）
            sr_chainBreakLoggedFor = (size_t)-1;
            sr_ctxBrokenSeen = (size_t)-1;  // v1.2.1：ctx-map 同步重置
            sr_slotDumpDone = false;         // v1.2.3：D 类 dump 重打（新档）
            sr_itemBadSeen = (size_t)-1;     // v1.2.9：E 类体检重报（新档）
            sr_itemRealLogged = false;       // v1.3.0：新档重臂
            sr_itemChainLogged = false;      // v1.3.0：新档重臂
            sr_nextActionAt = now + SR_LOAD_SETTLE_MS;
            sr_phase = SrPhase::Settle;
            break;
        }
        sr_nextActionAt = now + 1;
        sr_phase = SrPhase::FirstPass;
        break;
    }
    default:
        sr_phase = SrPhase::Disabled;
        break;
    }
}
