// production_automation_v2.inl -- ProductionAuto v2: 链式自动化（五槽框架重写）
//
// KB-080 五槽模型：
//   1. 触发器：NearbyMainUpdate hook 回调（嵌套保护，顶层才执行）
//   2. 枚举器：gimmickList_ 遍历 + 位置配对（机器↔箱子 95 半径）
//   3. 过滤器：三态状态机（Idle→Running→Ready，纯内存读）
//   4. 结算器：原生函数（投料序列/取产出 outputHelper/入箱 chestCommit）
//   5. 幂等标记：binding 状态字段 + 机器槽位字段本身
//
// 修正路线（2026-10-09 定）：投料 = AutoHarvest 物化模式 + 旧版已验证的
// 字段写入序列。走原生 event notify 链，不死档。所有写法抄自旧
// production_automation.inl 已验证代码（标注行号），砍掉防御性验证链。
//
// 关键机制（全部旧版已验证）：
//   - status map: int map(status+0x18) / u64 map(status+0x58)，lookup
//     find-or-insert，node+0x18 是值，写后必须 event notify
//   - 机器槽位: timeValue(+0x288+slot*8)=开始秒, createTime(+0x2a0+slot*8)
//     =时长秒, itemID{slot}=产出id, itemVal{slot}=产出数, inProc=加工中
//   - backing: *(void***)(status+0x2b8) 裸数组, 每槽 8 格
//   - slotLimit: data+0x128 level=="2" ? 3 : 1
//   - outputHelper 崩溃防护: [exe+0x3150768] 全局 NULL 时调用必崩（3 次实锤）
//
// 替换 production_automation.inl（9190行），保留基础设施层直接复用。
// 重写日期：2026-10-09  作者：PHJ&消失的清风

#pragma once
#include "processing_recipe_table.inl"

using namespace village_qol::production_automation;

// g_productionMainWorldGameSecond / g_productionMainWorldGameClockSourceReady
// 在 production_auto.cpp 中 static 定义，#include 时同 TU 可见

// ============================================================
// 常量（全部来自旧 .inl L40-82 已验证值）
// ============================================================
static constexpr uintptr_t PA_SAVE_OFFSET = 0x208;
static constexpr uintptr_t PA_PLAYER_STATUS = 0x32b8;
static constexpr uintptr_t PA_GIMMICK_LIST = 0x3448;   // build 25640656: 存档结构 +8 平移（旧 0x3440）
static constexpr uintptr_t PA_UNIQUE_ID = 0x0e0;
static constexpr uintptr_t PA_MAP_HOLDER = 0x0e8;
static constexpr uintptr_t PA_GIMMICK_DATA = 0x240;
static constexpr uintptr_t PA_TIME_VALUE = 0x288;
static constexpr uintptr_t PA_CREATE_TIME = 0x2a0;
// v1.5.45：save 内"当前累计游戏秒"字段。v1 旧版（production_automation.inl
//   L8194）长期直接用 save+0x3270 作为 gameSecond；v2 改为走时钟发布路径后
//   这条兜底被丢掉，一旦发布原子没就绪 gameSecond 恒 0 → 把槽 start 写成 0
//   → 游戏不认 → 幻影槽清理 + 白吞箱子材料。
static constexpr uintptr_t PA_SAVE_GAME_SECOND = 0x3270;
static constexpr uintptr_t PA_ITEM_LIST = 0x2b8;
static constexpr uintptr_t PA_NAME_OFFSET = 0x2e0;
static constexpr uintptr_t PA_UNIQUE_NAME = 0x2e8;
static constexpr size_t PA_MACHINE_SLOTS = 3;
static constexpr size_t PA_SLOT_INGREDIENTS = 8;
static constexpr size_t PA_MAX_GIMMICKS = 16384;
static constexpr float PA_AUTOLINK_RADIUS_SQ = 95.0f * 95.0f;
static constexpr ULONGLONG PA_SCAN_INTERVAL_MS = 5000;
static constexpr ULONGLONG PA_FIRST_SCAN_DELAY_MS = 3000;
// v1.5.90：游戏加载存档后约 30~45 秒才批量建立加工组件 comp(status+0x338)
//   （2026-10-25 16:19 日志实证：同一机器 5 槽 16:19:39 全 comp=0 →
//   16:19:44 全部同时变为非空）。此窗口内判"未点火"会误报"需玩家手动投料"。
//   故 PA 启动后该时长内静默等待，超期仍无 comp 才提示。
static constexpr ULONGLONG PA_BOOT_GRACE_MS = 90000;
// v2: position 偏移热调参（旧版 L60 同源）
static volatile uintptr_t PA_POSITION_OFFSET = 0x0f0;
// v2: outputHelper 内部 memcpy dst 全局（旧版 L65，NULL 检查必须）
static volatile uintptr_t PA_OUTPUT_TARGET_GLOBAL_RVA = 0x3150768;

// ============================================================
// 原生函数类型（旧版 L2031-2085 已验证签名）
// ============================================================
struct PaMapLookupResult {
    void* node;
    unsigned char inserted;
    unsigned char padding[7];
};
using PaMapLookupFunction = PaMapLookupResult* (__fastcall *)(
    void*, PaMapLookupResult*, const u64*);
using PaEventNotifyFunction = void (__fastcall *)(void*);
using PaEventOwnerFunction = void* (__fastcall *)(void*);
using PaItemAllocateFunction = void* (__fastcall *)(size_t);
using PaItemCtorFunction = void* (__fastcall *)(void*, void*, u64);
using PaItemSetCountFunction = void (__fastcall *)(void*, int);
using PaOutputHelperFunction = void* (__fastcall *)(void**, void*, int);
using PaChestCommitRawFunction = void (__fastcall *)(void* chest, void* item, int rank);

// ============================================================
// 简化状态机（3 态）
// ============================================================
enum class PaState : unsigned char {
    Idle = 0,       // 有空槽，可投料
    Running,        // 加工中
    Ready,           // 有到期槽，待取产出
};

// ============================================================
// Binding：机器↔箱子配对（幂等 key = machine+input+output）
// ============================================================
struct PaStableId {
    u64 mapId;
    u64 uniqueId;
};

static bool PaIdEqual(const PaStableId& a, const PaStableId& b) {
    return a.mapId == b.mapId && a.uniqueId == b.uniqueId;
}

// 连通组（旧版 flood-fill L266-283 机制）：机器+箱子距离半径内全图连通
// 上限与旧版一致：128 组 / 16 箱每组 / 64 机每组
static constexpr size_t PA_MAX_GROUPS = 128;
static constexpr size_t PA_MAX_CHESTS_PER_GROUP = 16;
static constexpr size_t PA_MAX_MACHINES_PER_GROUP = 64;
struct PaGroup {
    PaStableId machines[PA_MAX_MACHINES_PER_GROUP];
    size_t machineCount;
    PaStableId chests[PA_MAX_CHESTS_PER_GROUP];
    size_t chestCount;
};

static volatile int g_paNativePushEnabled = 0;   // v1.5.37: 投料后调用原生 push
static volatile int g_paAutomationPaused = 0;   // v1.5.41: 取证用，暂停一切自动投取
// v1.5.46-diag：原生 push 验证 + 加工时长压缩（仅诊断，默认关）
//   pushprobe：投料完成后调用一次原生 push(status,0)，记录返回值与前后 diff，
//              用于判断"游戏是否认可直写状态"（面板空槽的定性关键）。
//   fasttest ：把写入的 duration 压到 120 秒，2 分钟即可看到完成/取料，避免
//              等 23 小时。两者都由游戏根目录 flag 文件开启，测完即删。
static volatile int g_paPushProbe = 0;
static volatile int g_paFastTest = 0;
static volatile int g_paNativePush = 0;
// v1.5.49-diag：dump 机器 status 两个 map 的全部键值（对比"游戏写"与"PA 写"）
static volatile int g_paDumpKeys = 0;
// v1.5.53-diag：投料后触碰 UI 同步键（UIManpu+1），触发面板重绘。
// v1.5.78 尝试：g_paTouchUi=1（作者原意"默认开"），但 2026-10-22 用户实测：
//   手动投料能触发动画、PA 自动投料不能 → 该键仅触发面板重绘，动画/进度条由
//   游戏原生投料/event-notify 链驱动，PA 直写字段无法复现（FN-029 边界：远处/
//   首次需玩家 UI 交互路径，PA 无法复现）。故回退到 0 = 原始基线"每台手动点火
//   一次 + 之后全自动"，不再追自动动画。UIPros/UIPrsFar 写入仍省略（v1.5.55
//   实证整数 +1 被判非法进度）。逆向结论：status+0x58 为 u64 map，三键均为游戏
//   字符串字面量（UIPros@0xFABDD29 / UIManpu@0xFABDD6D / UIPrsFar@0x27D31F）。
// v1.5.84【修正误置】恢复 1：旧档 进度文件\UI键修复_20261022.md 已定论——作者原
//   注释「默认开」，被误置 0 导致"投料后 +1 UIManpu 触发面板重绘"从未生效，
//   表现=PA 投料后机器面板空白/小进度条不显示。该键是**纯触发器**（值大小不参与
//   显示，UI 只看事件），写入安全；v1.5.55 之所以更坏是因为同时写了 UIPros，
//   任务书已明确 UIPros/UIPrsFar 编码未知、禁止盲写（此处仍不写）。
//   注意：2026-10-22 的 1→0 回退是为放弃"自动点火/自动动画"，与重绘无关，
//   属误伤；且当时 PA 是**直写**、绕不过原生链；现在走原生 push（v1.5.81+），
//   前提已变，值得重测。
// v1.5.95【订正】v1.5.84 的"该键是纯触发器、值大小不参与显示"结论**错误**，已被
//   同文件 1068 行订正推翻：UIPros/UIManpu 存的是**对象指针**（UIPros =
//   *(taskObj[2]+0x28)，加工完成时由 GimmickDeleteProcessingUI 置 0）。对指针 +1
//   会把有效 UI 对象改坏 → UI 系统取不到对象 → 加工动画永不渲染。v1.5.55 在 UIPros
//   上已踩过同一个坑（现象："三键 +1 后面板反而消失"）。
//   v1.5.94 取证佐证：v1.5.93/94 日志里 UIManpu 键被 PA 自增成 5781→5782 这类
//   小整数（游戏写的应是对象指针），且引擎动画表 t32c0 全程**无新增实例**、
//   t33b8 的 8 个 owner 恒定不变 → PA 加工过程从未在引擎动画表注册过实例。
//   故默认关闭触碰：宁可不触发重绘，也绝不破坏指针。
// v1.5.112【重新开启，但改用"重建"语义】v1.5.95 关掉它是对的（对 UIManpu 指针 +1
//   会毁掉对象），但它同时把 PaMaintainProcessingUi 整条维护链一起冻死 —— 那是
//   "靠近再离开小进度条消失"修复、manpu 重建、动画实例取证的全部入口，等于
//   为了躲一个 +1 把整个 UI 维护关掉。现在直写路径上的 +1 已删除（见 PaAttemptInput
//   内的 v1.5.112 注记），维护链只做「键为空则重建对象」，不碰任何指针值 → 安全。
//   开启后每 10 秒对"加工中(inProc)"机器走一遍 PaMaintainProcessingUi。
static volatile int g_paTouchUi = 1;
// v1.5.60-diag：原生 push 可行性验证（零副作用，只改选中索引再还原）
static volatile int g_paNativePushTest = 0;
// v1.5.71：组件已点火时用原生 push 投料（箱子→临时入包→push→清理）
// v1.5.81【停直写·防 phantom 崩源】默认走原生 push（=1）。直写仅改机器
//   status 的 timer+itemID，但不登记游戏运行时产出表（全局 ctx），会造出
//   "已完成 + 全局 itemID{slot}=哨兵"的 phantom 崩源槽，游戏加工完成时
//   在 0x5D9022 读全局表得哨兵 → JZ → RDI=0 → MOV RAX,[0] 空崩。故默认
//   关闭直写；非 push 路径一律跳过（见下方直写入口护栏）。
static volatile int g_paUsePushFeed = 1;

// v1.5.74：makecomp 实验相关定义已移除（FN-029：直接构造组件会崩游戏）
static void* g_paCurrentSave = nullptr;   // v1.5.29: 被动机器自建产出用
static PaGroup g_groups[PA_MAX_GROUPS];
static size_t g_groupCount = 0;
static std::atomic<bool> g_paReady{false};

// ============================================================
// 原生函数指针（install 时绑定）
// ============================================================
static PaMapLookupFunction g_paStatusIntLookup = nullptr;
static PaMapLookupFunction g_paStatusU64Lookup = nullptr;
static PaMapLookupFunction g_paEventLookup = nullptr;
static PaEventNotifyFunction g_paEventNotify = nullptr;
static PaItemAllocateFunction g_paItemAllocate = nullptr;
static PaItemCtorFunction g_paItemCtor = nullptr;
static PaItemSetCountFunction g_paItemSetCount = nullptr;
static PaOutputHelperFunction g_paOutputHelper = nullptr;
// v1.5.46-diag：原生 production_push 直调（4 参，rcx=status, rdx=0）
typedef uintptr_t (*PaNative4Raw)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static PaNative4Raw g_paPushNative = nullptr;
// v1.5.85：加工 UI 创建三条（签名统一为 (baseID, gimmickStatus)）
typedef uintptr_t (*PaUiCreateFn)(uintptr_t baseId, uintptr_t status);
// 地址在 production_auto.cpp 的 g_rva_production_ui_*（build 25640656 值）
//   near 0x285CE0 近处大 UI / far 0x286540 远处小进度条 / manpu 0x286CC0 加工动画
//   （旧版 0x27D480/0x27DCE0/0x27E460，2026-10-29 用内联立即数+整簇对齐重新定址）
static PaUiCreateFn g_paUiNear = nullptr;
static PaUiCreateFn g_paUiFar = nullptr;
static PaUiCreateFn g_paUiManpu = nullptr;

// ============================================================
// v1.6.16【HUD 统一守卫·方案A】所有 PA 侧加工 UI 创建（far 进度条 / manpu 动画）
//   必须经 PaUiCreateGuarded()，禁止再裸调 PaSafeUiCreate()。
//   起因（2026-10-31 22:56/22:59 两次 0xC0000005）：
//     PaMaintainProcessingUi 的 `st = (uintptr_t)machineStatus` 在 machineStatus
//     为空时为 0，而游戏 far UI 创建函数内部对 status+0x10 **不判空**（仅 +0x8 判），
//     0x2866E0 处 `mov rcx,[rcx+0x10]` 直接摸地址 0x10 → 崩。PaSafeUiCreate 的 SEH
//     没能拦下（经 inline hook trampoline 的异常分发链不在 PA 的 __try 覆盖范围内），
//     因此**唯一可靠手段是在调用前把非法输入挡掉**。
//   本守卫同时解决三件事：
//     ① 崩溃：fn / baseId / status 任一为 0 就跳过，绝不把 0 交给游戏（方案A）；
//     ② 闪烁：per-(gid,kind) 成功即封顶(done)，不再反复销毁重建；
//            失败按 5→10→20→40→60 秒退避，避免 1 秒一轮的创建风暴显现为忽明忽暗；
//     ③ 可观测：每次拦截/跳过的理由写入 obs 寄存器，AI 可零截图判定。
// ============================================================
enum class PaUiKind { kFar = 0, kManpu = 1, kNear = 2, kCount = 3 };
static inline const char* PaUiKindName(PaUiKind k) {
    switch (k) {
        case PaUiKind::kFar:   return "far";
        case PaUiKind::kManpu: return "manpu";
        case PaUiKind::kNear:  return "near";
        default:               return "?";
    }
}
static uintptr_t PaUiCreateGuarded(PaUiCreateFn fn, u64 baseId, uintptr_t st,
                                   u64 gid, PaUiKind kind, bool allowCreate);
// v1.5.85：baseID 合法性校验 FUN_140186930(table, baseId) -> 非 0 合法
typedef uintptr_t (*PaBaseIdResolveFn)(uintptr_t table, uintptr_t baseId);
static PaBaseIdResolveFn g_paBaseIdResolve = nullptr;
// v1.5.96【baseID 分离】脚本里三条加工 UI 指令对应**三个不同的 UI 预制体**
//   （ProcessingMachineRoot / ProcessingMachineRoot_Far / ProcessingManpuRoot），
//   各自 baseID 很可能不同；而 PA 此前只捕获一个（g_paCapturedBaseId，来自
//   collect=manpu 探针）并拿它调三条 → 校验函数 FUN_14025d0e0 查表失败会抛
//   "invalid baseID"，被 SEH 吞掉 → 静默不建 UI。
//   证据（v1.5.95 日志）：24 次 pushfeed 但 uifix 仅 1 条（base=1612/0x64C），
//   而历史 apiprobe 捕到的 manpu baseID 是 0x648=1608 —— 两者不同。
//   故 v1.5.96 分别捕获 near / far / manpu 三个 baseID，并分别使用。
static std::atomic<u64> g_paBaseIdNear{0};
static std::atomic<u64> g_paBaseIdFar{0};
// v1.5.96：PA 自己调用 UI 创建函数时抑制探针捕获，避免"自己传的 baseID 又被
//   当成游戏的值"而自污染（探针装在这三个函数入口上）。
static std::atomic<int> g_paUiCallDepth{0};
// v1.5.86：DAT_141104C80 是注册表的**地址本身**（脚本调用 FUN_14025d0e0 时
//   MOV RCX,[0x141104C80] 取指针后传给 FUN_140186930 校验）。该全局在游戏
//   启动早期为 NULL（PA mod_init 太早读到 NULL → 早退），故只存 slot 地址，
//   每次 PaCreateProcessingUi 调用时再解引用，避开初始化时序。
static void** g_paBaseIdTableSlot = nullptr;
// v1.5.86：玩家手动投料（点火）时游戏脚本调三条 UI 指令，其第一参数 baseID
//   即加工 UI 真值。PA 因自动点火是硬边界、必然先手动一轮，故挂 collect(manpu)
//   探针捕获该 rcx，缓存给自动投料轮复用（自动轮 PA 不跑脚本、拿不到 baseID）。
static std::atomic<uintptr_t> g_paCapturedBaseId{0};
// v1.5.110：任意一条"合法 baseID 表项"即可对全部机器通用（prefab 查询结果从未
//   被 UI 创建函数用于构造，仅过 FUN_14025d0e0 的表项存在性校验）。由 near/far
//   探针首次捕获、或 bootstrap 扫描得到，使 PaCreateProcessingUi 对"玩家从未靠近"
//   的自动投料机器也能成功建 UI / 加工动画。
static std::atomic<u64> g_paValidBaseId{0};
static PaChestCommitRawFunction g_paChestCommitRaw = nullptr;

// ============================================================
// SEH 安全调用包装（旧版 L2087-2149 模式：noinline + 无析构对象）
// ============================================================
__declspec(noinline) static void* PaSafeItemAllocate(
    PaItemAllocateFunction fn, size_t size) {
    void* memory = nullptr;
    __try { memory = fn(size); }
    __except (EXCEPTION_EXECUTE_HANDLER) { memory = nullptr; }
    return memory;
}

__declspec(noinline) static void* PaSafeItemCtor(
    PaItemCtorFunction fn, void* memory, void* save, u64 itemId) {
    void* item = nullptr;
    __try { item = fn(memory, save, itemId); }
    __except (EXCEPTION_EXECUTE_HANDLER) { item = nullptr; }
    return item;
}

__declspec(noinline) static void PaSafeItemSetCount(
    PaItemSetCountFunction fn, void* item, int count) {
    __try { fn(item, count); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static PaMapLookupResult* PaSafeMapLookup(
    PaMapLookupFunction fn, void* map, PaMapLookupResult* result,
    const u64* key) {
    PaMapLookupResult* found = nullptr;
    __try { found = fn(map, result, key); }
    __except (EXCEPTION_EXECUTE_HANDLER) { found = nullptr; }
    return found;
}

__declspec(noinline) static void PaSafeEventNotify(
    PaEventNotifyFunction fn, void* node) {
    __try { fn(node); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static void PaSafeItemAdjust(
    NearbyItemAdjustFunction fn, void* item, int delta) {
    __try { fn(item, delta); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static void* PaSafeOutputHelper(
    PaOutputHelperFunction fn, void* machineStatus, int slot) {
    void* item = nullptr;
    __try { fn(&item, machineStatus, slot); }
    __except (EXCEPTION_EXECUTE_HANDLER) { item = nullptr; }
    return item;
}

// ============================================================
// 工具函数
// ============================================================
static bool PaReadPtr(const void* base, uintptr_t off, void** out) {
    void* const* slot = reinterpret_cast<void* const*>(
        reinterpret_cast<uintptr_t>(base) + off);
    if (!NearbyIsReadable(slot, sizeof(void*))) return false;
    *out = *slot;
    return true;
}

static bool PaReadFloat3(const void* base, uintptr_t off,
                         float* x, float* y, float* z) {
    const float* p = reinterpret_cast<const float*>(
        reinterpret_cast<uintptr_t>(base) + off);
    if (!NearbyIsReadable(p, sizeof(float) * 3)) return false;
    *x = p[0]; *y = p[1]; *z = p[2];
    return true;
}

static float PaDist2D(float ax, float ay, float bx, float by) {
    float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

// rank 强制 0 特例（旧版 L2608-2616）
static bool PaForcesRankZero(u64 itemId) {
    if (itemId >= 0x330ccULL && itemId - 0x330ccULL <= 0x14ULL &&
        ((0x100401ULL >> (itemId - 0x330ccULL)) & 1ULL) != 0)
        return true;
    if (itemId >= 0x330eaULL && itemId - 0x330eaULL <= 0x3cULL &&
        ((0x1004010040100401ULL >> (itemId - 0x330eaULL)) & 1ULL) != 0)
        return true;
    return false;
}

// ============================================================
// 身份读取（旧版 L1173-1205 已验证）
// ============================================================
static bool PaReadMapId(void* status, u64* mapId) {
    if (!status || !mapId) return false;
    void* holder = nullptr;
    void* group = nullptr;
    if (!PaReadPtr(status, PA_MAP_HOLDER, &holder) ||
        !PaReadPtr(holder, 0, &group) ||
        !NearbyIsReadable(group, sizeof(u64))) return false;
    const u64 value = *reinterpret_cast<const u64*>(group);
    if (value == 0) return false;
    *mapId = value;
    return true;
}

static bool PaReadIdentity(void* status, PaStableId* identity) {
    const uintptr_t base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!status || !identity ||
        !NearbyIsReadable(status, PA_UNIQUE_NAME + sizeof(void*) * 2) ||
        *reinterpret_cast<const uintptr_t*>(status) !=
            base + RVA_PRODUCTION_GIMMICK_VTABLE ||
        *reinterpret_cast<const uintptr_t*>(
            reinterpret_cast<const unsigned char*>(status) + 0x10) !=
            base + RVA_PRODUCTION_GIMMICK_SECONDARY_VTABLE) return false;
    const u64 uniqueId = *reinterpret_cast<const u64*>(
        reinterpret_cast<const unsigned char*>(status) + PA_UNIQUE_ID);
    u64 mapId = 0;
    if (!uniqueId || !PaReadMapId(status, &mapId)) return false;
    identity->mapId = mapId;
    identity->uniqueId = uniqueId;
    return true;
}

// gimmickId 快读（旧版 L1224-1235：+0x240→holder→data→+0）
static bool PaReadGimmickId(void* status, u64* gimmickId) {
    void* holder = nullptr;
    void* data = nullptr;
    if (!status || !gimmickId ||
        !PaReadPtr(status, PA_GIMMICK_DATA, &holder) ||
        !PaReadPtr(holder, 0, &data) ||
        !NearbyIsReadable(data, sizeof(u64))) return false;
    const u64 id = *reinterpret_cast<const u64*>(data);
    if (!id) return false;
    *gimmickId = id;
    return true;
}

static bool PaIsMachineId(u64 gimmickId) {
    if (gimmickId == 0) return false;
    return ProductionRecipeMachineIndexLookup(gimmickId) != nullptr;
}

// ============================================================
// v1.5.106：PA 机器 → 动画实例 owner 登记表
//
// 2026-10-26 实测（打点对齐）：原版在「加工完成」时刻挂新动画实例，owner 是
//   机器的 event owner 对象（*(*(inst+0x240))），与 near 调用传的第 2 参数
//   不是同一个对象。而 PA 侧同样能通过 PaGetEventOwner 取到该对象 → 本表把
//   「PA 的机器」与「动画实例」直接对表，采样时逐实例比对 owner，输出
//   「PA机 gid=X 实例 c260=N」并把 c260 的 0↔>0 变化落盘。
//   这直接回答：PA 的机器到底有没有拿到活着的动画实例（c260>0 才判活）。
// ============================================================
struct PaMachineReg {
    void* status;
    void* owner;
    u64 gid;
    ULONGLONG seen;
};
#define PA_MACHREG_N 16
static PaMachineReg g_paMachReg[PA_MACHREG_N];
static int g_paMachRegN = 0;

static u64 PaMachineRegGidByOwner(void* owner) {
    if (!owner) return 0;
    for (int i = 0; i < g_paMachRegN; ++i) {
        if (g_paMachReg[i].owner == owner)
            return g_paMachReg[i].gid;
    }
    return 0;
}

// ============================================================
// status map 写 + event notify（旧版 L2369-2496 已验证）
// ============================================================
static bool PaGetEventOwner(void* status, void** ownerOut) {
    if (!status || !ownerOut) return false;
    unsigned char* interfaceObject =
        reinterpret_cast<unsigned char*>(status) + 0x10;
    if (!NearbyIsReadable(interfaceObject, sizeof(void*)) ||
        !NearbyIsReadable(*reinterpret_cast<void**>(interfaceObject),
                          sizeof(void*) * 2)) return false;
    void** vtable = *reinterpret_cast<void***>(interfaceObject);
    PaEventOwnerFunction getter =
        reinterpret_cast<PaEventOwnerFunction>(vtable[1]);
    if (!getter || !NearbyIsReadable(reinterpret_cast<void*>(getter), 1))
        return false;
    void* eventOwner = getter(interfaceObject);
    if (!eventOwner ||
        !NearbyIsReadable(
            reinterpret_cast<unsigned char*>(eventOwner) + 0x08, 0x38))
        return false;
    *ownerOut = eventOwner;
    return true;
}
// ============================================================
// v1.5.108：本机 baseID 收获表（status → baseID 正确配对捕获）
//
// 2026-10-26 14:43~14:44 v1.5.107 日志实证：
//   - 游戏自身 near/far 调用携带 (baseID, status) 正确配对：
//       far base=b5a st=21c3995ed40 / far base=e14 st=21c39962500 /
//       far base=1357 st=21c3988b040 / near base=b48 st=21c39962c00
//   - PA 借用他机 baseID(4951) 调 far(本机 st=398DD720) → 返回 0：
//     baseID 与 status 必须一一配对；全局表解析能过但配对不过。
//   - PA 机器 status map 内 baseID=0 objID=0：自身无 baseID 可读。
//   ⇒ 修复：游戏自身 near/far 调用（非 PA 来源）把 (status, baseID)
//     收获进本表；PaResolveBaseIdEx 以「本机收获 baseID」为最优先候选。
//     玩家靠近过一次的机器即被收获，此后 PA 三条 UI 全部可正确创建。
// ============================================================
struct PaBaseHarvest {
    void* status;
    u64 baseId;      // 兼容旧字段（保留：最后一次捕获值）
    u64 nearBase;    // v1.6.57：near UI 的 baseId（游戏调 near 时传入）
    u64 farBase;     // v1.6.57：far  UI 的 baseId（游戏调 far  时传入）
    ULONGLONG seen;
};
#define PA_BASEHARVEST_N 32
static PaBaseHarvest g_paBaseHarvest[PA_BASEHARVEST_N];
static int g_paBaseHarvestN = 0;

// v1.5.109 诊断：转储 status 结构体前 0x300 字节，离线定位 baseID 存储偏移。
//   背景：baseID 不在 status+0x58 字符串表（mapdump 见 baseID=0 objID=0），
//   必须从结构字段直接读；但 PA 无法依赖"玩家靠近"采集（v1.5.108 实测：
//   玩家路过的机器 ≠ PA 投料的机器 → harv=0 全失败）。故转储靠近机器的
//   status 字节，跨多台机型求交集定位偏移，之后 PaResolveBaseIdEx 直接
//   *(status+offset) 读 baseID，覆盖全部机器，不再依赖 proximity。
static void PaBaseHarvestDiagDump(void* status, uintptr_t baseId) {
    static struct { void* st; ULONGLONG t; } s_bd[16] = {};
    const ULONGLONG now = GetTickCount64();
    bool known = false, doDump = true;
    for (int k = 0; k < 16; ++k) {
        if (s_bd[k].st == status) {
            known = true;
            if (now - s_bd[k].t < 60000ULL) doDump = false;
            s_bd[k].t = now;
            break;
        }
    }
    if (!known) {
        for (int k = 0; k < 16; ++k)
            if (!s_bd[k].st) { s_bd[k].st = status; s_bd[k].t = now; break; }
    }
    if (!doDump) return;
    Log("[PA][harvest-bytes] st=%p base=%llx\n", status,
        static_cast<unsigned long long>(baseId));
    unsigned char* p = reinterpret_cast<unsigned char*>(status);
    for (uintptr_t off = 0; off < 0x300; off += 16) {
        if (!NearbyIsReadable(p + off, 16)) {
            Log("  [unreadable @%04x]\n", static_cast<unsigned>(off));
            break;
        }
        unsigned char b[16];
        memcpy(b, p + off, 16);
        char line[64] = {};
        int o = 0;
        for (int i = 0; i < 16; ++i)
            o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE,
                             "%02x ", b[i]);
        Log("  +%04x: %s\n", static_cast<unsigned>(off), line);
    }
}

// 收获/刷新一条配对（同 status 且 baseID 未变则静默刷新）
static void PaBaseHarvestStore(void* status, uintptr_t baseId, int kind = 1) {
    if (!status || !baseId) return;
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < g_paBaseHarvestN; ++i) {
        if (g_paBaseHarvest[i].status == status) {
            if (g_paBaseHarvest[i].baseId != static_cast<u64>(baseId)) {
                Log("[PA][harvest] 更新 st=%p base=%llx (旧 %llx)\n",
                    status, static_cast<unsigned long long>(baseId),
                    static_cast<unsigned long long>(
                        g_paBaseHarvest[i].baseId));
                g_paBaseHarvest[i].baseId = baseId;
                // v1.5.110：偏移定位转储已无用（baseID 仅过校验），关闭以减日志
            }
            if (kind == 0) g_paBaseHarvest[i].nearBase = baseId;
            else          g_paBaseHarvest[i].farBase  = baseId;
            g_paBaseHarvest[i].seen = now;
            return;
        }
    }
    if (g_paBaseHarvestN < PA_BASEHARVEST_N) {
        g_paBaseHarvest[g_paBaseHarvestN].status = status;
        g_paBaseHarvest[g_paBaseHarvestN].baseId = baseId;
        if (kind == 0) g_paBaseHarvest[g_paBaseHarvestN].nearBase = baseId;
        else          g_paBaseHarvest[g_paBaseHarvestN].farBase  = baseId;
        g_paBaseHarvest[g_paBaseHarvestN].seen = now;
        ++g_paBaseHarvestN;
        Log("[PA][harvest] +st=%p base=%llx\n", status,
            static_cast<unsigned long long>(baseId));
        // v1.5.110：偏移定位转储已无用（baseID 仅过校验），关闭以减日志
    }
}

// 查本机收获的 baseID（未收获返回 0）
// v1.6.57：kind 0=near / 1=far —— 两套 baseId 完全不同（实测 near=0x2022、far=0xdaf，
//   混用会让 PA 拿 near 的 base 去建 far，产出的对象 handle 与键里现值相同 →
//   游戏判定值没变不写键、不发 notify → 小 UI 不刷新（靠近再走开就消失）。
static u64 PaBaseHarvestGet(void* status, int kind = 1) {
    if (!status) return 0;
    for (int i = 0; i < g_paBaseHarvestN; ++i) {
        if (g_paBaseHarvest[i].status != status) continue;
        const u64 v = (kind == 0) ? g_paBaseHarvest[i].nearBase
                                  : g_paBaseHarvest[i].farBase;
        return v ? v : g_paBaseHarvest[i].baseId;
    }
    return 0;
}

// ============================================================
// v1.6.44【玩家位置】游戏 1.30 靠近建 near 面板时**不写 UIPros 非 0**（实测
//   uimaint uiPros=0 恒成立 → v1.6.38 的"玩家在旁跳过补料"守卫永久失效 →
//   PA 在玩家眼皮底下补料改状态 → 面板闪）。改用玩家实际坐标判"在旁"。
//   链路与 far UI 函数开头一致：root→+0x208 playerObj→+0x32b8 status→+0xF0 pos。
// （定义放在 PaReadPtr 之后供全文件使用；PaGetWorldContext 负责每 tick 刷新）
static float g_paPlayerX = 0.f, g_paPlayerY = 0.f;
static volatile long g_paPlayerValid = 0;

// 距离阈值：1 格≈45px，玩家站机器旁一般 <50px；取 100px 宁大勿小
// （代价只是玩家在旁时少补一轮料，绝无副作用）。
static bool PaPlayerNearMachine(float mx, float my) {
    if (!g_paPlayerValid) return false;
    const float dx = g_paPlayerX - mx;
    const float dy = g_paPlayerY - my;
    return (dx * dx + dy * dy) < 100.0f * 100.0f;
}

// v1.6.47：far/manpu 早退点探针（定义在文件后部，此处前向声明供 guarded 调用）
static void PaCondProbe(const char* who, u64 gid);
// v1.6.53：UI 黄金快照（定义在文件后部）
static void PaGoldenCapture(void* status);
static void PaGoldenRestore(void* status, u64 gid);

// v1.6.40【far/manpu UI 真根因：三函数读的是线程上下文，不是 status】
//
// 反编译对照（build 25640656，服务台实拉，非旧笔记）：
//   push  FUN_140284670(status,itemId)        —— unaff_GS 出现 0 次
//   near  FUN_140285ce0(baseId,status)        —— unaff_GS 7 次
//   far   FUN_140286540(baseId,status)        —— unaff_GS 7 次
//   manpu FUN_140286cc0(baseId,status)        —— unaff_GS 7 次
// 三个 UI 函数开头同构：
//   iVar2  = *(int *)(**(GS+0x58) + 0x56c8);                    // 上下文深度
//   actor  = iVar2 ? *(void **)(**(GS+0x58)+0x5a18+iVar2*8) : 0; // 当前 gimmick
//   manager= *(FUN_14025fac0(actor[2]) + 0x250);                // far 的挂载点
// 最后把 UI 对象挂到 manager+0x278 / manager+0x2b8。
//   ⇒ **UI 挂到哪台机器，由线程上下文决定，调用方传的 status 只用于写键**。
// PA 在主线程 tick 里调用时该上下文不是本机（甚至为空）→ UI 挂到别人身上，
// 而 status 上的 UIPrsFar/UIManpu 键照样被写入 → 「键有值、画面没有」的脏键。
// KB-104 记的 `mov rcx,[rcx+0x10]` 崩溃 = actor 为空时的直接后果（actor[2]）。
// 而玩家手动投料时是 Lua 在该机器上下文里执行 → 三个函数都正确 → UI 正常。
//
// 修法（本段实现）：
//   ① 在 near/far 的入口 hook 里把「游戏自己调用时」的 (status → actor) 配对
//      收下来（此刻 actor 必然正确）；
//   ② PA 建 far/manpu 前用 PaTlsScope 临时把上下文换成本机 actor，调用后立刻
//      还原（含 depth==0 时压一层的兜底）；
//   ③ 没收到配对的机器一律不建 —— 宁缺 UI，不造脏 UI。
// ============================================================
#include <intrin.h>

namespace {

struct PaTlsSnapshot {
    unsigned char* ctx      = nullptr;
    int            depth    = 0;
    void*          slotOld  = nullptr;
    bool           active   = false;
};

// 只读当前线程上下文里的 actor（失败一律返回 nullptr，不抛不崩）
//   ★ 解引用层数（v1.6.41 修正）：反编译是 `**(longlong**)(GS+0x58)`，
//     即 ctx = *(GS:[0x58])，**只解一层**。v1.6.40 多解了一层（当成指针数组再取[0]）
//     → 读到垃圾 → 永远返回 nullptr → harvest 表全空。这就是当次实测
//     `want=0000000000000000` 的来源。
static void* PaTlsActorPeek() {
    __try {
        void* p = reinterpret_cast<void*>(
            static_cast<unsigned long long>(__readgsqword(0x58)));
        if (!p) return nullptr;
        unsigned char* ctx = *reinterpret_cast<unsigned char**>(p);
        if (!ctx) return nullptr;
        const int depth = *reinterpret_cast<int*>(ctx + 0x56c8);
        if (depth <= 0) return nullptr;
        return *reinterpret_cast<void**>(
            ctx + 0x5a18 + static_cast<long long>(depth) * 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// 把当前线程上下文临时切换成 want；返回 true 表示确实做过替换（需 End 还原）
static bool PaTlsScopeBegin(PaTlsSnapshot* s, void* want) {
    if (!s || !want) return false;
    s->active = false;
    __try {
        void* p = reinterpret_cast<void*>(
            static_cast<unsigned long long>(__readgsqword(0x58)));
        if (!p) return false;
        unsigned char* ctx = *reinterpret_cast<unsigned char**>(p);
        if (!ctx) return false;
        const int depth = *reinterpret_cast<int*>(ctx + 0x56c8);
        s->ctx = ctx;
        s->depth = depth;
        if (depth > 0) {
            void** slot = reinterpret_cast<void**>(
                ctx + 0x5a18 + static_cast<long long>(depth) * 8);
            if (*slot == want) return false;   // 已在本机上下文，无需替换
            s->slotOld = *slot;
            *slot = want;
        } else {
            // depth==0：上下文栈为空。压一层（深度1 + 槽1），调用后原样还原。
            //   只影响这一个函数调用窗口，且写的是数组内（+0x5a20）不越界。
            void** slot = reinterpret_cast<void**>(ctx + 0x5a18 + 8);
            s->slotOld = *slot;
            *slot = want;
            *reinterpret_cast<int*>(ctx + 0x56c8) = 1;
        }
        s->active = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s->active = false;
    }
    return s->active;
}

static void PaTlsScopeEnd(PaTlsSnapshot* s) {
    if (!s || !s->active) return;
    __try {
        if (s->depth > 0) {
            *reinterpret_cast<void**>(
                s->ctx + 0x5a18 + static_cast<long long>(s->depth) * 8) =
                s->slotOld;
        } else {
            *reinterpret_cast<void**>(s->ctx + 0x5a18 + 8) = s->slotOld;
            *reinterpret_cast<int*>(s->ctx + 0x56c8) = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    s->active = false;
}

// status → 正确的 actor（由游戏自身调用路径收获）
struct PaActorHarvestEntry {
    void*     status = nullptr;
    void*     actor  = nullptr;
    ULONGLONG seen   = 0;
};
PaActorHarvestEntry g_paActorHarvest[64];
int                 g_paActorHarvestN = 0;

static void PaActorHarvestStore(void* status, void* actor) {
    if (!status || !actor) return;
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < g_paActorHarvestN; ++i) {
        if (g_paActorHarvest[i].status == status) {
            g_paActorHarvest[i].actor = actor;
            g_paActorHarvest[i].seen = now;
            return;
        }
    }
    if (g_paActorHarvestN < 64) {
        g_paActorHarvest[g_paActorHarvestN].status = status;
        g_paActorHarvest[g_paActorHarvestN].actor = actor;
        g_paActorHarvest[g_paActorHarvestN].seen = now;
        ++g_paActorHarvestN;
        Log("[PA][tls] harvest + status=%p actor=%p\n", status, actor);
    }
}

static void* PaActorHarvestGet(void* status) {
    if (!status) return nullptr;
    for (int i = 0; i < g_paActorHarvestN; ++i)
        if (g_paActorHarvest[i].status == status)
            return g_paActorHarvest[i].actor;
    return nullptr;
}

}  // namespace

// 热开关（HOT_BOOL）：0=只观察不替换，1=启用上下文切换（默认开）
static int g_paTlsScopeFix = 1;

// v1.5.50-diag：前向声明（PaAttemptInput 里要立即 dump 一次）
static void PaDumpStatusMap(void* status, uintptr_t mapOffset, bool isInt,
                            u64 gid);

// int map 写（status+0x18）+ notify（旧版 L2414-2450）
static bool PaSetStatusInt(void* status, const char* key, int value) {
    if (!status || !key || strlen(key) > sizeof(u64)) return false;
    u64 packedKey = 0;
    memcpy(&packedKey, key, strlen(key));
    PaMapLookupResult result = {};
    PaMapLookupResult* found = PaSafeMapLookup(
        g_paStatusIntLookup,
        reinterpret_cast<unsigned char*>(status) + 0x18, &result,
        &packedKey);
    if (!found || !found->node ||
        !NearbyIsReadable(
            reinterpret_cast<unsigned char*>(found->node) + 0x18,
            sizeof(int))) return false;
    int* nativeValue = reinterpret_cast<int*>(
        reinterpret_cast<unsigned char*>(found->node) + 0x18);
    if (*nativeValue == value) return true;
    *nativeValue = value;

    void* eventOwner = nullptr;
    if (!PaGetEventOwner(status, &eventOwner)) return false;
    PaMapLookupResult eventResult = {};
    PaMapLookupResult* eventFound = PaSafeMapLookup(
        g_paEventLookup,
        reinterpret_cast<unsigned char*>(eventOwner) + 0x08, &eventResult,
        &packedKey);
    if (!eventFound || !eventFound->node ||
        !NearbyIsReadable(
            reinterpret_cast<unsigned char*>(eventFound->node) + 0x18,
            0x20)) return false;
    PaSafeEventNotify(
        g_paEventNotify,
        reinterpret_cast<unsigned char*>(eventFound->node) + 0x18);
    return *nativeValue == value;
}

// v1.5.55-diag：纯读检查 status+0x58 里某个键是否存在（全桶扫描，绝不插入节点）。
//   原生 lookup 是 find-or-insert，用它探测不存在的键会凭空建出节点，
//   这里必须只读，避免污染状态表。
static bool PaStatusKeyPresent(void* status, const char* key, u64* valueOut) {
    if (!status || !key) return false;
    unsigned char* map = reinterpret_cast<unsigned char*>(status) + 0x58;
    if (!NearbyIsReadable(map, 0x38)) return false;
    void* sentinel = *reinterpret_cast<void**>(map + 0x08);
    void* buckets = *reinterpret_cast<void**>(map + 0x18);
    const u64 mask = *reinterpret_cast<const u64*>(map + 0x30);
    if (!sentinel || !buckets || mask == 0 || mask > 0x10000ULL) return false;
    const size_t bucketCount = static_cast<size_t>(mask) + 1;
    u64 packedKey = 0;
    memcpy(&packedKey, key, strlen(key));
    for (size_t b = 0; b < bucketCount; ++b) {
        unsigned char* bkt = reinterpret_cast<unsigned char*>(buckets) + b * 16;
        if (!NearbyIsReadable(bkt, 16)) continue;
        void* tail = *reinterpret_cast<void**>(bkt);
        void* node = *reinterpret_cast<void**>(bkt + 8);
        size_t hops = 0;
        // v1.5.72：原条件排除链尾节点，会把落在链尾的 itemID 键漏掉
        //   （与 SaveGuard 同源 bug）。改为循环内检查后再跳出。
        while (node && node != sentinel && hops++ < 256) {
            if (!NearbyIsReadable(node, 0x20)) break;
            if (*reinterpret_cast<const u64*>(
                    reinterpret_cast<unsigned char*>(node) + 0x10) ==
                packedKey) {
                if (valueOut) {
                    *valueOut = *reinterpret_cast<const u64*>(
                        reinterpret_cast<unsigned char*>(node) + 0x18);
                }
                return true;
            }
            if (node == tail) break;
            node = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(node) + 0x08);
        }
    }
    return false;
}

// u64 map 写（status+0x58）+ notify（旧版 L2456-2496）
static bool PaSetStatusU64(void* status, const char* key, u64 value) {
    if (!status || !key || strlen(key) > sizeof(u64)) return false;
    u64 packedKey = 0;
    memcpy(&packedKey, key, strlen(key));
    PaMapLookupResult result = {};
    PaMapLookupResult* found = PaSafeMapLookup(
        g_paStatusU64Lookup,
        reinterpret_cast<unsigned char*>(status) + 0x58, &result,
        &packedKey);
    if (!found || !found->node ||
        !NearbyIsReadable(
            reinterpret_cast<unsigned char*>(found->node) + 0x18,
            sizeof(u64))) return false;
    u64* nativeValue = reinterpret_cast<u64*>(
        reinterpret_cast<unsigned char*>(found->node) + 0x18);
    if (*nativeValue == value) return true;
    *nativeValue = value;

    void* eventOwner = nullptr;
    if (!PaGetEventOwner(status, &eventOwner)) {
        // v1.5.59-diag：区分失败原因（UI 通知 ok=0 的定性）
        if (strcmp(key, "UIManpu") == 0) {
            static std::atomic<int> s_noOwner{0};
            if (s_noOwner.fetch_add(1, std::memory_order_relaxed) < 8) {
                Log("[PA] ui-notify FAIL: no eventOwner key=%s status=%p\n",
                    key, status);
            }
        }
        return false;
    }
    if (!NearbyIsReadable(
            reinterpret_cast<unsigned char*>(eventOwner) + 0x48, 0x38)) {
        if (strcmp(key, "UIManpu") == 0) {
            static std::atomic<int> s_noMap{0};
            if (s_noMap.fetch_add(1, std::memory_order_relaxed) < 8) {
                Log("[PA] ui-notify FAIL: eventOwner+0x48 unreadable key=%s "
                    "owner=%p status=%p\n", key, eventOwner, status);
            }
        }
        return false;
    }
    PaMapLookupResult eventResult = {};
    PaMapLookupResult* eventFound = PaSafeMapLookup(
        g_paEventLookup,
        reinterpret_cast<unsigned char*>(eventOwner) + 0x48, &eventResult,
        &packedKey);
    if (!eventFound || !eventFound->node ||
        !NearbyIsReadable(
            reinterpret_cast<unsigned char*>(eventFound->node) + 0x18,
            0x20)) {
        if (strcmp(key, "UIManpu") == 0) {
            static std::atomic<int> s_noNode{0};
            if (s_noNode.fetch_add(1, std::memory_order_relaxed) < 8) {
                Log("[PA] ui-notify FAIL: event node missing key=%s "
                    "node=%p status=%p\n",
                    key, eventFound ? eventFound->node : nullptr, status);
            }
        }
        return false;
    }
    PaSafeEventNotify(
        g_paEventNotify,
        reinterpret_cast<unsigned char*>(eventFound->node) + 0x18);
    return *nativeValue == value;
}

// ============================================================
// 机器工具（旧版 L2564-2606 已验证）
// ============================================================
// slotLimit: 升级机（模块名 _Lv2）=3 槽，普通机 =1 槽
//   v1.5.12 修正：旧判定读 data+0x128 的 {ptr,len} 等级串，v1.20 实测该
//   偏移已经变了（读到的 len = 0x391658AF 垃圾 → 所有机器恒 1 槽，
//   玩家三槽机只处理 slot0）。改用模块名判定：data.dat 实证生产机械
//   Lv2 变体（blast_furnace_Lv2 / storage_jar_Lv2 / brewing_barrel_Lv2…）
//   三槽定时器全有值，非 Lv2 机 slot1/2 恒 0；生产机械无 Lv3 变体。
static bool PaReadGimmickModule(void* status, char* module,
                                size_t moduleSize);

static bool PaReadSlotLimit(void* machineStatus, size_t* limitOut) {
    if (!machineStatus || !limitOut) return false;

    char module[64] = {};
    if (PaReadGimmickModule(machineStatus, module, sizeof(module))) {
        const size_t len = strlen(module);
        const bool upgraded =
            len > 4 && strcmp(module + len - 4, "_Lv2") == 0;
        *limitOut = upgraded ? PA_MACHINE_SLOTS : 1;
        return true;
    }

    // 兜底：模块名读不到时退回旧等级串判定（v1.20 下基本恒 1，仅防崩）
    void* holder = nullptr;
    void* data = nullptr;
    if (!PaReadPtr(machineStatus, PA_GIMMICK_DATA, &holder) ||
        !PaReadPtr(holder, 0, &data) ||
        !NearbyIsReadable(
            reinterpret_cast<unsigned char*>(data) + 0x128,
            sizeof(void*) + sizeof(u64))) return false;
    const char* level = *reinterpret_cast<const char* const*>(
        reinterpret_cast<unsigned char*>(data) + 0x128);
    const u64 levelLength = *reinterpret_cast<const u64*>(
        reinterpret_cast<unsigned char*>(data) + 0x130);
    const bool levelTwo = levelLength == 1 && level &&
        NearbyIsReadable(level, 1) && level[0] == '2';
    *limitOut = levelTwo ? PA_MACHINE_SLOTS : 1;
    return true;
}

// backing: *(void***)(status+0x2b8)（旧版 L2586-2606）
static bool PaReadBacking(void* machineStatus, size_t requiredEntries,
                          void*** backingOut) {
    if (!machineStatus || !backingOut || requiredEntries == 0 ||
        requiredEntries > PA_MACHINE_SLOTS * PA_SLOT_INGREDIENTS ||
        !NearbyIsReadable(
            reinterpret_cast<unsigned char*>(machineStatus) + PA_ITEM_LIST,
            sizeof(void*))) return false;
    void** backing = *reinterpret_cast<void***>(
        reinterpret_cast<unsigned char*>(machineStatus) + PA_ITEM_LIST);
    if (!backing ||
        !NearbyIsReadable(backing, requiredEntries * sizeof(void*)))
        return false;
    *backingOut = backing;
    return true;
}

/// ============================================================
/// v1.6.6【炭窑死锁修复】槽 backing 空判定（三态）
///
/// 起因（2026-10-29 实测）：炭窑 gid=240100000 与高炉、面粉机同组
///   （groupdiag #0 machines=5 chests=1），箱里有 230000/218 木头、
///   配方 211000←230000×5 且 fuel=0，条件全满足，但日志里**连一条
///   input 记录都没有**（既无 OK 也无 fail），slotdiag 也从不打印
///   （它 ready=0 running=0，原 slotdiag 只在 ready/running 时打）。
///
///   链路：PaTick 里投料入口是 `else if (hasIdle)`，hasIdle 由
///   PaClassifyMachine 给出；而它判空槽的唯一条件是
///   `PaReadBacking(status, 8, &backing)` 成功 + backing 里 8 项全空。
///   `PaReadBacking` 在 `*(void***)(status+0x2b8) == nullptr` 时返回
///   false —— 于是 hasIdle 恒 false → PaAttemptInput 一次都不调 →
///   这台机器永远不投料 → 永远保持"从未投过料"→ 物品列表永远不分配
///   → **死锁**。凡是玩家没手动投过料的机器（炭窑、新放下的机器）
///   都被这条挡住，而日志里没有任何痕迹。
///
/// 修法：把"backing 指针本身为空"与"backing 非空但不可读"分开：
///   · 指针为空 → 该机器没有物品列表 = 槽内确定无物品 → 判为空槽；
///   · 非空但不可读 → 无法判定，返回 false（保持旧的保守行为）。
/// 安全性：即便误判为空槽，后续仍要过 native push，游戏认为槽不空
///   会直接拒绝（al=0），clone 原样退回，不会覆盖既有物品。
///
/// 返回值：true = 判定完成（*emptyOut 有效）；false = 无法判定。
/// ============================================================
static bool PaSlotBackingEmpty(void* machineStatus, size_t slot,
                               bool* emptyOut, void** backingRawOut) {
    if (!machineStatus || !emptyOut) return false;
    *emptyOut = false;
    if (backingRawOut) *backingRawOut = nullptr;
    if (slot >= PA_MACHINE_SLOTS) return false;

    unsigned char* bytes = reinterpret_cast<unsigned char*>(machineStatus);
    if (!NearbyIsReadable(bytes + PA_ITEM_LIST, sizeof(void*))) return false;
    void** backing = *reinterpret_cast<void***>(bytes + PA_ITEM_LIST);
    if (backingRawOut) *backingRawOut = backing;

    if (!backing) {
        // 无物品列表 —— 从未投过料的机器就是这样，确定无物品。
        *emptyOut = true;
        return true;
    }

    const size_t required = (slot + 1) * PA_SLOT_INGREDIENTS;
    if (required == 0 || required > PA_MACHINE_SLOTS * PA_SLOT_INGREDIENTS)
        return false;
    if (!NearbyIsReadable(backing, required * sizeof(void*))) return false;

    for (size_t i = slot * PA_SLOT_INGREDIENTS; i < required; ++i) {
        if (backing[i]) return true;   // 槽内有物品 → 非空
    }
    *emptyOut = true;
    return true;
}

// 槽位直接内存读（不走 map）
static bool PaReadSlotTimers(void* machineStatus, size_t slot,
                             u64* startOut, u64* durationOut) {
    const unsigned char* bytes =
        reinterpret_cast<const unsigned char*>(machineStatus);
    if (!NearbyIsReadable(bytes + PA_TIME_VALUE + slot * sizeof(u64),
                          sizeof(u64)) ||
        !NearbyIsReadable(bytes + PA_CREATE_TIME + slot * sizeof(u64),
                          sizeof(u64))) return false;
    *startOut = *reinterpret_cast<const u64*>(
        bytes + PA_TIME_VALUE + slot * sizeof(u64));
    *durationOut = *reinterpret_cast<const u64*>(
        bytes + PA_CREATE_TIME + slot * sizeof(u64));
    return true;
}

/// ============================================================
/// v1.5.122【far UI 修复·核心】判断机器是否处于「加工中」
///
/// 起因（本轮实测 + Ghidra 反编译双向确证）：
///   维护链原用 status+0x18 的 int map 键 `inProc` 判加工中，但——
///     · 09:04:24 游戏自己投的料 → inProc=1 → 维护链正常执行 ✓
///     · 09:04:44 PA 原生 push 成功（pushfeed al=1）→ **inProc 读不到**，
///       维护链 0 次执行；而同刻 slotdiag 显示 running=3（槽确实在加工）
///   Ghidra 反编译（probe_inproc.py）确证：PA 调的原生 push
///   `FUN_14027be10`(RVA 0x27BE10) 只写 `itemID%d`(0x58 map) 与
///   `itemVal%d`(0x18 map)，**不写 inProc**；4 条 gamescript 包装函数
///   （0x497F30/0x498320/0x498610/0x498120）也都不写 inProc。
///   ⇒ inProc 由**游戏自己的点火流程**写入，PA 走「已点火组件 + 原生 push」
///     不经过点火那一步 → inProc 恒 0 → 维护链被门禁挡死 → far UI 永不重建
///     → 「玩家走远后小 UI 进度消失」。
///
/// 修法：门禁改用**槽 timer**（status 固定偏移），与 PA 自己的 slotdiag
///   判据完全一致，且已被本次日志证明正确（running=3 而 inProc 读不到）。
///
/// 纯读：PaReadSlotTimers 只读固定偏移，不写任何东西。
/// ============================================================
static bool PaMachineHasRunningSlot(void* machineStatus) {
    size_t slotLimit = 0;
    if (!PaReadSlotLimit(machineStatus, &slotLimit)) return false;
    for (size_t slot = 0; slot < slotLimit && slot < PA_MACHINE_SLOTS; ++slot) {
        u64 start = 0, duration = 0;
        if (!PaReadSlotTimers(machineStatus, slot, &start, &duration))
            continue;
        if (start != 0 && duration != 0) return true;   // 槽在计时 = 加工中
    }
    return false;
}

// status map 只读（find-or-insert 读 node+0x18，插入零值节点无害——旧版 L2452 注释）
static bool PaReadStatusU64(void* status, const char* key, u64* valueOut) {
    if (!status || !key || !valueOut || strlen(key) > sizeof(u64))
        return false;
    u64 packedKey = 0;
    memcpy(&packedKey, key, strlen(key));
    PaMapLookupResult result = {};
    PaMapLookupResult* found = PaSafeMapLookup(
        g_paStatusU64Lookup,
        reinterpret_cast<unsigned char*>(status) + 0x58, &result,
        &packedKey);
    if (!found || !found->node ||
        !NearbyIsReadable(
            reinterpret_cast<unsigned char*>(found->node) + 0x18,
            sizeof(u64))) return false;
    *valueOut = *reinterpret_cast<const u64*>(
        reinterpret_cast<unsigned char*>(found->node) + 0x18);
    return true;
}

static bool PaReadStatusInt(void* status, const char* key, int* valueOut) {
    if (!status || !key || !valueOut || strlen(key) > sizeof(u64))
        return false;
    u64 packedKey = 0;
    memcpy(&packedKey, key, strlen(key));
    PaMapLookupResult result = {};
    PaMapLookupResult* found = PaSafeMapLookup(
        g_paStatusIntLookup,
        reinterpret_cast<unsigned char*>(status) + 0x18, &result,
        &packedKey);
    if (!found || !found->node ||
        !NearbyIsReadable(
            reinterpret_cast<unsigned char*>(found->node) + 0x18,
            sizeof(int))) return false;
    *valueOut = *reinterpret_cast<const int*>(
        reinterpret_cast<unsigned char*>(found->node) + 0x18);
    return true;
}

// ============================================================
// v1.5.26：被动机器（蜂箱 / 采汁器 / 蚕盒）——定时器恒 0，产出靠
//   itemID/itemVal 键，不走 timer 路径。v2 重写时丢了这段旧版逻辑
//   （KB-017 / ARCHIVE v1.1.2），导致蚕盒产出永远不转移。
// ============================================================
static bool PaIsPassiveMachine(u64 /*gimmickId*/) {
    // v1.5.51：整段退役。v1.5.42 探针实证——蚕盒手动投料后
    //   s0 id=221500 v=1 t=11466928/169200 bk[100430/1]
    //   它有 timer（duration=169200 ≈ 47 游戏小时），与普通加工机同构。
    //   历史"被动机器 timer 恒 0"是 PA 把 start 写成 0 的残留态误判，
    //   由此衍生的 skip / 无 timer 产出判定反而会复制物品（v1.5.34 事故）。
    //   现在时钟与 start 已修正，蚕盒/蜂箱/采汁器一律按普通 timer 机处理。
    (void)0;
    return false;
}

static bool PaSlotHasOutputNoTimer(void* machineStatus, size_t slot) {
    if (!machineStatus || slot >= PA_MACHINE_SLOTS) return false;
    char idKey[8] = {};
    char countKey[9] = {};
    _snprintf_s(idKey, sizeof(idKey), _TRUNCATE, "itemID%zu", slot);
    _snprintf_s(countKey, sizeof(countKey), _TRUNCATE, "itemVal%zu", slot);
    u64 itemId = 0;
    int count = 0;
    if (!PaReadStatusU64(machineStatus, idKey, &itemId)) return false;
    if (!PaReadStatusInt(machineStatus, countKey, &count)) return false;
    return itemId != 0 && count > 0;
}

// ============================================================
// 槽 3 过滤器：三态判定（纯内存读，timer 字段驱动）
// ============================================================
static bool PaClassifyMachine(void* machineStatus, std::int64_t gameSecond,
                              PaState* stateOut, size_t* readySlotOut,
                              bool* hasIdleSlotOut) {
    if (!machineStatus || !stateOut) return false;
    *stateOut = PaState::Idle;
    if (readySlotOut) *readySlotOut = SIZE_MAX;
    if (hasIdleSlotOut) *hasIdleSlotOut = false;

    size_t slotLimit = 0;
    if (!PaReadSlotLimit(machineStatus, &slotLimit)) return false;

    u64 selfGid = 0;
    PaReadGimmickId(machineStatus, &selfGid);
    const bool passive = PaIsPassiveMachine(selfGid);
    bool hasRunning = false;
    bool hasReady = false;
    size_t readySlot = SIZE_MAX;
    bool hasIdle = false;

    for (size_t slot = 0; slot < slotLimit && slot < PA_MACHINE_SLOTS;
         ++slot) {
        u64 start = 0, duration = 0;
        if (!PaReadSlotTimers(machineStatus, slot, &start, &duration))
            return false;
        if (start == 0 || duration == 0) {
            // v1.5.26：被动机器 timer 恒 0，但有产出键 → 直接算 ready
            if (passive && PaSlotHasOutputNoTimer(machineStatus, slot)) {
                hasReady = true;
                if (readySlot == SIZE_MAX) readySlot = slot;
                continue;
            }
            // 空槽：再验 backing 空（旧版 L4187-4194）
            // v1.6.6【炭窑死锁修复】改走 PaSlotBackingEmpty（见其注释）：
            //   旧写法 PaReadBacking 在 `status+0x2b8 == nullptr` 时返回
            //   false → hasIdle 恒 false → 该机器永远进不了 PaAttemptInput
            //   （零日志零投料）→ 永远保持"从未投料"→ 物品列表永不分配。
            //   炭窑 240100000 就是此死锁：组内有箱、配方满足，却零 input。
            bool slotEmpty = false;
            if (PaSlotBackingEmpty(machineStatus, slot, &slotEmpty, nullptr)) {
                if (slotEmpty) hasIdle = true;
            }
            continue;
        }
        if (gameSecond >= 0 &&
            static_cast<std::int64_t>(start) <= INT64_MAX &&
            static_cast<std::int64_t>(duration) <= INT64_MAX &&
            gameSecond >= static_cast<std::int64_t>(start) &&
            gameSecond - static_cast<std::int64_t>(start) >=
                static_cast<std::int64_t>(duration)) {
            hasReady = true;
            if (readySlot == SIZE_MAX) readySlot = slot;
        } else {
            hasRunning = true;
        }
    }

    if (hasReady) {
        *stateOut = PaState::Ready;
        if (readySlotOut) *readySlotOut = readySlot;
    } else if (hasRunning) {
        *stateOut = PaState::Running;
    } else {
        *stateOut = PaState::Idle;
    }
    if (hasIdleSlotOut) *hasIdleSlotOut = hasIdle;
    return true;
}

// ============================================================
// 配方匹配：箱子组原料检查（组内任意箱有料即可，选第一条满足配方）
// ============================================================
struct PaRecipePlan {
    const ProcessingRecipeTableEntry* recipe;
    // 每种原料：源物品指针（所在箱子）+ 扣减量
    void* sourceItems[8];
    void* sourceChests[8];
    int sourceAmounts[8];
    size_t ingredientCount;
};

// v1.5.113：规划失败原因提示。
//   过去"主料不够"和"燃料不够"都只打一句 "no recipe (chests lack ingredients?)"，
//   排查高炉弹窗时被误导过一轮：主料（石英×5）箱子里明明够，缺的是燃料
//   （高炉燃料只有 木炭211000 / 煤炭230100 两种）。这里把"缺哪个燃料 ID"
//   带出来，日志直接写明。
static std::atomic<u64> g_paPlanFailFuelId{0};

static bool PaBuildRecipePlan(void* chestStatuses[],
                              size_t chestCount, u64 machineId,
                              PaRecipePlan* plan) {
    if (!chestStatuses || chestCount == 0 || !plan || machineId == 0)
        return false;
    plan->recipe = nullptr;
    plan->ingredientCount = 0;
    g_paPlanFailFuelId.store(0, std::memory_order_relaxed);

    const ProcessingRecipeMachineIndex* index =
        ProductionRecipeMachineIndexLookup(machineId);
    if (!index) return false;

    // 收集组内全部箱子物品（合并 inventory）
    struct PaSlot { void* item; int chestIdx; };
    PaSlot slotPool[16 * 64] = {};  // 16 箱 x 64 槽上限
    size_t poolCount = 0;
    for (size_t c = 0; c < chestCount && c < PA_MAX_CHESTS_PER_GROUP;
         ++c) {
        NearbyRawPointerVector inventory = {};
        if (!NearbyReadRawInventoryRelaxed(chestStatuses[c], &inventory))
            continue;
        const size_t slots =
            (reinterpret_cast<uintptr_t>(inventory.end) -
             reinterpret_cast<uintptr_t>(inventory.begin)) / sizeof(void*);
        for (size_t s = 0; s < slots && poolCount < 1024; ++s) {
            if (!inventory.begin[s]) continue;
            slotPool[poolCount].item = inventory.begin[s];
            slotPool[poolCount].chestIdx = static_cast<int>(c);
            poolCount++;
        }
    }
    if (poolCount == 0) return false;

    for (u32 r = 0; r < index->recipeCount; ++r) {
        const u32 recipeIndex = index->recipeIndices[r];
        if (recipeIndex >= kProcessingRecipeTableCount) continue;
        const ProcessingRecipeTableEntry* recipe =
            &kProcessingRecipeTable[recipeIndex];
        // 跳过无输入配方（采集型机器不投料）
        bool hasInput = false;
        for (size_t i = 0; i < 8; ++i) {
            if (recipe->inputItemIds[i] != 0) { hasInput = true; break; }
        }
        if (!hasInput) continue;

        // 检查箱子组能提供全部原料（旧版 L3890-3960 rank 语义）
        PaRecipePlan candidate = {};
        candidate.recipe = recipe;
        // 锚定主原料 rank（旧版 L3900-3928：inputItemIds[0] 的 rank 作为锚）
        int anchorRank = 0;
        bool anchorFound = false;
        if (recipe->inputItemIds[0] != 0) {
            for (size_t s = 0; s < poolCount; ++s) {
                void* item = slotPool[s].item;
                if (!item) continue;
                NearbyItemInfo info = {};
                if (!NearbyReadItem(item, &info) ||
                    info.itemId != recipe->inputItemIds[0]) continue;
                anchorRank = info.rank;
                anchorFound = true;
                break;
            }
            if (!anchorFound) continue;  // 主料都没有，此配方不满足
        }
        // rank 优先序（旧版 L3838-3845）：anchorRank 降序到 0，再升序到 4
        int rankOrder[5] = {};
        size_t rankCount = 0;
        for (int rk = anchorRank; rk >= 0; --rk) rankOrder[rankCount++] = rk;
        for (int rk = anchorRank + 1; rk <= 4; ++rk) rankOrder[rankCount++] = rk;

        bool satisfied = true;
        for (size_t i = 0; i < 8; ++i) {
            const u64 inputId = recipe->inputItemIds[i];
            const u32 inputCount = recipe->inputCounts[i];
            if (inputId == 0 || inputCount == 0) continue;
            // 在箱子组里找该原料（按 rank 优先序）
            int remaining = static_cast<int>(inputCount);
            void* source = nullptr;
            int sourceChestIdx = -1;
            for (size_t order = 0; order < rankCount && remaining > 0;
                 ++order) {
                const int wantRank = rankOrder[order];
                for (size_t s = 0; s < poolCount && remaining > 0; ++s) {
                    void* item = slotPool[s].item;
                    if (!item) continue;
                    NearbyItemInfo info = {};
                    if (!NearbyReadItem(item, &info) ||
                        info.itemId != inputId ||
                        info.rank != wantRank) continue;
                    if (info.stackCount >= remaining) {
                        source = item;
                        sourceChestIdx = slotPool[s].chestIdx;
                        remaining = 0;
                        break;
                    }
                }
            }
            if (!source || remaining > 0) { satisfied = false; break; }
            candidate.sourceItems[candidate.ingredientCount] = source;
            candidate.sourceChests[candidate.ingredientCount] =
                chestStatuses[sourceChestIdx];
            candidate.sourceAmounts[candidate.ingredientCount] =
                static_cast<int>(inputCount);
            candidate.ingredientCount++;
        }
        // v1.5.25：燃料/第二材料（熔炉等）。旧配方表丢了这个字段，
        //   导致"主料+燃料"的机器永远只进一半材料而无法开工。
        if (satisfied && recipe->fuelItemId != 0 &&
            candidate.ingredientCount < 8) {
            const u32 needFuel = recipe->fuelCount ? recipe->fuelCount : 1;
            int remainingFuel = static_cast<int>(needFuel);
            for (size_t s = 0; s < poolCount && remainingFuel > 0; ++s) {
                void* item = slotPool[s].item;
                if (!item) continue;
                NearbyItemInfo info = {};
                if (!NearbyReadItem(item, &info) ||
                    info.itemId != recipe->fuelItemId) continue;
                const int take = info.stackCount >= remainingFuel
                                     ? remainingFuel : info.stackCount;
                if (take <= 0) continue;
                candidate.sourceItems[candidate.ingredientCount] = item;
                candidate.sourceChests[candidate.ingredientCount] =
                    chestStatuses[slotPool[s].chestIdx];
                candidate.sourceAmounts[candidate.ingredientCount] = take;
                candidate.ingredientCount++;
                remainingFuel -= take;
                if (candidate.ingredientCount >= 8) break;
            }
            if (remainingFuel > 0) {
                // v1.5.113：记录"只差燃料"的配方，供调用方打明确日志
                if (g_paPlanFailFuelId.load(std::memory_order_relaxed) == 0)
                    g_paPlanFailFuelId.store(recipe->fuelItemId,
                                             std::memory_order_relaxed);
                satisfied = false;
            }
        }
        if (satisfied && candidate.ingredientCount > 0) {
            *plan = candidate;
            return true;
        }
    }
    // 诊断：dump 箱子组首箱前 10 格物品（首次触发）
    static bool s_chestDumpDone = false;
    if (!s_chestDumpDone) {
        s_chestDumpDone = true;
        char buf[256] = {};
        int off = 0;
        NearbyRawPointerVector inv = {};
        if (NearbyReadRawInventoryRelaxed(chestStatuses[0], &inv)) {
            const size_t cnt =
                (reinterpret_cast<uintptr_t>(inv.end) -
                 reinterpret_cast<uintptr_t>(inv.begin)) / sizeof(void*);
            for (size_t s = 0; s < 10 && s < cnt; ++s) {
                void* item = inv.begin[s];
                if (!item) {
                    off += _snprintf_s(buf + off, sizeof(buf) - off, _TRUNCATE,
                                      " %zu:null", s);
                    continue;
                }
                NearbyItemInfo info = {};
                if (NearbyReadItem(item, &info)) {
                    off += _snprintf_s(buf + off, sizeof(buf) - off, _TRUNCATE,
                                      " %zu:%llx/%d", s,
                                      static_cast<unsigned long long>(info.itemId),
                                      info.stackCount);
                } else {
                    off += _snprintf_s(buf + off, sizeof(buf) - off, _TRUNCATE,
                                      " %zu:bad", s);
                }
            }
        }
        Log("[PA] recipe diag: machine=%llu no-match chests=%zu:%hs\n",
            static_cast<unsigned long long>(machineId), chestCount, buf);
    }
    return false;
}

// chestCommit 安全包装（v1.2.5 崩溃修复：裸调用无 SEH → 崩在 village.exe）
__declspec(noinline) static void PaSafeChestCommit(
    PaChestCommitRawFunction fn, void* chest, void* item, int rank) {
    __try { fn(chest, item, rank); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[PA] chestCommit SEH caught\n");
    }
}

// ============================================================
// 槽 4 结算器：投料（修正路线：物化 + 字段写 + notify + 扣箱）
// 旧版 L5008-5392 核心序列（砍防御链）
// ============================================================
// ============================================================
// v1.5.14-diag：投料失败明细（机器要什么 vs 箱子里有什么）
// 玩家反馈"生丝没有自动投料"——需要区分是配方表缺条目、机器不在组内，
// 还是箱子里根本没有那个原料（或 id 对不上）。只读。
// ============================================================
static void PaLogInputDiag(void* machineStatus, void* chestStatuses[],
                           size_t chestCount, u64 gimmickId) {
    char recipeBuf[160] = {};
    int ro = 0;
    const ProcessingRecipeMachineIndex* index =
        ProductionRecipeMachineIndexLookup(gimmickId);
    if (!index) {
        ro += _snprintf_s(recipeBuf + ro, sizeof(recipeBuf) - ro, _TRUNCATE,
                          "NO_RECIPE_INDEX");
    } else {
        size_t shown = 0;
        for (u32 r = 0; r < index->recipeCount && shown < 4; ++r) {
            const u32 ri = index->recipeIndices[r];
            if (ri >= kProcessingRecipeTableCount) continue;
            const ProcessingRecipeTableEntry* rc = &kProcessingRecipeTable[ri];
            u64 in0 = rc->inputItemIds[0];
            if (in0 == 0) continue;
            ro += _snprintf_s(recipeBuf + ro, sizeof(recipeBuf) - ro, _TRUNCATE,
                              "%s r%u:in=%llu out=%llu", shown ? " |" : "",
                              static_cast<unsigned>(rc->recipeId),
                              static_cast<unsigned long long>(in0),
                              static_cast<unsigned long long>(
                                  rc->outputItemId));
            ++shown;
        }
        if (shown == 0)
            ro += _snprintf_s(recipeBuf + ro, sizeof(recipeBuf) - ro,
                              _TRUNCATE, "ALL_NO_INPUT recipes=%u",
                              static_cast<unsigned>(index->recipeCount));
    }

    // v1.6.7-diag：改为「全量去重 itemId」，原来只列前 10 个槽 —— 玩家
    //   坚持箱里有木炭/煤炭、而清单里前 10 项全是木材和矿石时，无法判断
    //   到底是"燃料排在后面"还是"真的没有"，上一轮就被这一点卡住。
    //   同时统计 totalSlots（实际扫了多少槽）与 fuelHit（是否命中燃料），
    //   一行即可定案。
    char chestBuf[960] = {};
    int co = 0;
    u64 seenIds[48] = {};
    size_t seenCount = 0;
    size_t totalSlots = 0;
    bool fuelHit = false;
    for (size_t c = 0; c < chestCount; ++c) {
        NearbyRawPointerVector inv = {};
        if (!NearbyReadRawInventoryRelaxed(chestStatuses[c], &inv)) continue;
        const size_t cnt =
            (reinterpret_cast<uintptr_t>(inv.end) -
             reinterpret_cast<uintptr_t>(inv.begin)) / sizeof(void*);
        for (size_t s = 0; s < cnt; ++s) {
            void* item = inv.begin[s];
            if (!item) continue;
            ++totalSlots;
            NearbyItemInfo info = {};
            if (!NearbyReadItem(item, &info) || info.itemId == 0) continue;
            if (info.itemId == 211000ULL || info.itemId == 230100ULL)
                fuelHit = true;
            bool dup = false;
            for (size_t k = 0; k < seenCount; ++k) {
                if (seenIds[k] == info.itemId) { dup = true; break; }
            }
            if (dup) continue;
            if (seenCount < 48) seenIds[seenCount++] = info.itemId;
            if (co >= 0 && co < static_cast<int>(sizeof(chestBuf)) - 24) {
                co += _snprintf_s(chestBuf + co, sizeof(chestBuf) - co,
                                  _TRUNCATE, " %llu",
                                  static_cast<unsigned long long>(info.itemId));
            }
        }
    }
    Log("[PA] input diag gid=%llu slotLimit=%zu chests=%zu slots=%zu "
        "fuelHit=%d ids=%zu recipes[%s] chestIds[%s]\n",
        static_cast<unsigned long long>(gimmickId), [&] {
            size_t lim = 1;
            PaReadSlotLimit(machineStatus, &lim);
            return lim;
        }(), chestCount, totalSlots, fuelHit ? 1 : 0, seenCount,
        recipeBuf, seenCount ? chestBuf : " empty/unreadable");
}

// ============================================================
// v1.5.71：原生 push 投料（组件已由玩家点火时使用）
//   push 前置：status+0x338 != 0（CCom_Gimmick 组件）+ save+0x33B0 选中的
//   背包槽里有原料。这里把原料 clone 临时放进背包空槽、指向它、调 push、
//   再把残留清掉；箱子扣减由调用方负责（失败时回补）。
// ============================================================
static bool PaPushFeed(void* save, void* machineStatus, void* ingredient,
                       u64 gid) {
    if (!save || !machineStatus || !ingredient || !g_paPushNative) return false;
    unsigned char* sb = reinterpret_cast<unsigned char*>(save);
    void** slots = reinterpret_cast<void**>(sb + 0x32C0);
    std::int32_t* selectIndex = reinterpret_cast<std::int32_t*>(sb + 0x33B0);
    if (!NearbyIsReadable(slots, 30 * sizeof(void*)) ||
        !NearbyIsReadable(selectIndex, sizeof(std::int32_t)))
        return false;

    unsigned char* statusBytes =
        reinterpret_cast<unsigned char*>(machineStatus);
    if (!NearbyIsReadable(statusBytes + 0x338, sizeof(void*))) return false;
    void* component = *reinterpret_cast<void**>(statusBytes + 0x338);
    // v1.6.4：不再因「无组件」硬拒。实测（build 25640656）原生 push 在无组件时
    //   是**拒绝**而非半吞：返回 al=0、物品原样留在背包槽（left==item），
    //   调用方已把主料 +1 补回 → 探测不会丢材料。
    //   硬拒的副作用是高炉死锁：没燃料→不点火→无组件→PA 不敢投燃料。
    if (!component) {
        Log("[PA] pushfeed gid=%llu comp=NULL，仍尝试原生 push（探测）\n",
            static_cast<unsigned long long>(gid));
    }

    int bagSlot = -1;
    for (int i = 0; i < 30; ++i) {
        if (!slots[i]) { bagSlot = i; break; }
    }
    if (bagSlot < 0) return false;

    NearbyAddReference(ingredient);  // 背包持有一份
    if (InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(&slots[bagSlot]), ingredient,
            nullptr) != nullptr) {
        NearbyReleaseOwnedReference(ingredient);
        return false;
    }

    const std::int32_t savedIndex = *selectIndex;
    *selectIndex = bagSlot;
    uintptr_t ret = 0;
    int faulted = 0;
    __try {
        ret = g_paPushNative(reinterpret_cast<uintptr_t>(machineStatus), 0, 0,
                             0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = 1;
    }
    *selectIndex = savedIndex;

    void* leftover = slots[bagSlot];

    // v1.6.22-diag：双料闸门现场量（高炉 vs 炭窑 对照）
    //   push 函数 FUN_140284670 的两个判定：
    //     (A) 机器 0x240-substruct 的 +0x138(int)==1 且 +0x130(char)=='2'
    //     (B) 物品内部 item+0x240 == item+0xa8（克隆料须"注定属于该机器"）
    //   把这两项 + 机器 substruct 指针一起打出来，炉/窑对照即可定位卡点。
    {
        u64 subStructPtr = 0, itemA8 = 0, item240 = 0;
        int  sub138 = -1; char sub130 = '?';
        if (NearbyIsReadable(statusBytes + 0x240, sizeof(void*))) {
            subStructPtr = reinterpret_cast<u64>(
                *reinterpret_cast<void* const*>(statusBytes + 0x240));
        }
        if (subStructPtr) {
            if (NearbyIsReadable(reinterpret_cast<void*>(subStructPtr + 0x130), 1))
                sub130 = *reinterpret_cast<char*>(
                    reinterpret_cast<void*>(subStructPtr + 0x130));
            if (NearbyIsReadable(reinterpret_cast<void*>(subStructPtr + 0x138),
                                 sizeof(int)))
                sub138 = *reinterpret_cast<int*>(
                    reinterpret_cast<void*>(subStructPtr + 0x138));
        }
        if (NearbyIsReadable(
                reinterpret_cast<const unsigned char*>(ingredient) + 0xa8,
                sizeof(void*))) {
            itemA8 = reinterpret_cast<u64>(*reinterpret_cast<void* const*>(
                reinterpret_cast<const unsigned char*>(ingredient) + 0xa8));
        }
        if (NearbyIsReadable(
                reinterpret_cast<const unsigned char*>(ingredient) + 0x240,
                sizeof(void*))) {
            item240 = reinterpret_cast<u64>(*reinterpret_cast<void* const*>(
                reinterpret_cast<const unsigned char*>(ingredient) + 0x240));
        }
        Log("[PA] pushdiag gid=%llu bag=%d subStruct=%p sub138=%d sub130=%c "
            "itemA8=%p item240=%p match(B)=%d slotItem=%p\n",
            static_cast<unsigned long long>(gid), bagSlot,
            reinterpret_cast<void*>(subStructPtr), sub138, sub130,
            reinterpret_cast<void*>(itemA8),
            reinterpret_cast<void*>(item240),
            (item240 == itemA8) ? 1 : 0, slots[bagSlot]);
    }

    // v1.6.8【判据修正 · 关键】反编译 FUN_140284670（RVA 0x284670，
    //   RVA_PRODUCTION_PUSH）实锤其签名为
    //       void FUN_140284670(longlong param_1, undefined8 param_2)
    //   —— 它**没有任何返回值**。所以旧判据 `(ret & 0xFF) != 0` 读的是
    //   函数返回后 RAX 里剩下的垃圾：
    //     · 炭窑那次 RAX 残留 0x1        → 判"成功"（蒙对）
    //     · 高炉 RAX 残留 0xEC5419C700   → 判"失败"（同样是蒙的）
    //   两个结论都不可信（KB-088：判据必须能区分真伪）。
    //   唯一真判据：**游戏有没有把背包选中槽里的物品拿走**——
    //     拿走（槽变空）= 真投进去了；还在 = 游戏拒绝，物品原样退回。
    if (leftover) {
        InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(&slots[bagSlot]), nullptr,
            leftover);
        NearbyReleaseOwnedReference(leftover);
    }

    const bool ok = !faulted && (leftover == nullptr);

    // v1.6.8-diag：push 失败时必须能回答"为什么"。void 返回值已经没用，
    //   于是把真正决定投料成败的现场量全打出来——
    //     comp   = status+0x338 加工组件。反编译：`if (*(param_1+0x338)!=0)`
    //              不成立时整个函数什么都不做，物品必然留在背包。
    //     limit  = 槽数（游戏侧读 gimmick+0x240 -> +0x138/+0x130 判 Lv2）
    //     what   = 实际推的物品 id/数量（确认推的是不是配方要的那一项）
    //     t[]    = 3 个槽的 (start/duration)。游戏侧用 `*(param_1+0x2a0)==0`
    //              判空槽，全非 0 就找不到空槽 → 不投。
    NearbyItemInfo ingInfo = {};
    NearbyReadItem(ingredient, &ingInfo);
    size_t limD = 0;
    PaReadSlotLimit(machineStatus, &limD);
    u64 ts0 = 0, td0 = 0, ts1 = 0, td1 = 0, ts2 = 0, td2 = 0;
    PaReadSlotTimers(machineStatus, 0, &ts0, &td0);
    PaReadSlotTimers(machineStatus, 1, &ts1, &td1);
    PaReadSlotTimers(machineStatus, 2, &ts2, &td2);
    Log("[PA] pushfeed gid=%llu slot=%d item=%p ret=%p fault=%d left=%p "
        "ok=%d comp=%p limit=%zu what=%llu/%d "
        "t=[%llu/%llu,%llu/%llu,%llu/%llu]\n",
        static_cast<unsigned long long>(gid), bagSlot, ingredient,
        reinterpret_cast<void*>(ret), faulted, leftover, ok ? 1 : 0,
        component, limD, static_cast<unsigned long long>(ingInfo.itemId),
        ingInfo.stackCount, static_cast<unsigned long long>(ts0),
        static_cast<unsigned long long>(td0),
        static_cast<unsigned long long>(ts1),
        static_cast<unsigned long long>(td1),
        static_cast<unsigned long long>(ts2),
        static_cast<unsigned long long>(td2));
    return ok;
}

// ============================================================
// v1.6.x【多料机器修复 · 核心】一次把配方所需全部原料 staged 进玩家背包槽
//   （save+0x32C0[0..29]），再调一次原生 push，让引擎自己的 auto-select
//   分支（反编译 FUN_140284670 @ RVA 0x284670 已实锤）去选配方并整体转入。
//
// 旧 PaPushFeed 每次只 staged 1 种料 → 双料机器（高炉=矿石+燃料）永远凑不齐
//   配方 → 引擎 auto-select 失败 → 物品原样退回 → 死锁。这正是"炭窑能成、
//   高炉死锁"的根因。
//
// 实现要点（与 PaPushFeed 同款引用计数纪律）：
//   · 每种 clone 先 NearbyAddReference（staging 引用），再 InterlockedCAS 入槽。
//   · 轮流把 selectIndex(save+0x33B0) 指向每种 clone 调原生 push。auto-select
//     要求「被推物品 == 配方 input[0]」才匹配，故哪种料是 input[0] 未知时逐个试；
//     命中后引擎把所有 input 一次性转入机器、清空全部 staged 槽。
//   · 成功判据 = 所有 staged 槽被清空（引擎 auto-select 命中后把整批原料转入机器）。
//     v1.6.37 修正：曾改用 gate A(status+0x240 子结构) 判成功，但实测高炉 leftover=0
//     （料已被吞、running 正常推进）却 gateA 判假 → 误判失败 → 每 5 秒重试 pushall
//     → 每轮 stage/清玩家背包槽 → near 面板跟着重建 → 大进度 UI 闪。已改回"槽全空"。
//     成功槽不释放 staging 引用（引擎已接管），残留槽 null+释放 staging 引用。最终统一
//     由调用方 PaReleaseClones 释放事务引用，引用计数闭合。

// v1.6.x【诊断】把多料机器的 gid 映射成中文名，实测日志可直接区分三台机器。
static const char* PaMachineLabel(u64 gid) {
    switch (gid) {
        case 240090000ULL: case 240090100ULL: return "高炉";
        case 240080000ULL: case 240080100ULL: return "熔炼炉";
        case 240150000ULL: case 240150100ULL: return "熔炉";
        default:                                   return "?";
    }
}

static bool PaPushFeedAll(void* save, void* machineStatus, void** clones,
                          size_t cloneCount, u64 gid) {
    if (!save || !machineStatus || !g_paPushNative || cloneCount == 0 ||
        cloneCount > 30)
        return false;
    unsigned char* sb = reinterpret_cast<unsigned char*>(save);
    void** slots = reinterpret_cast<void**>(sb + 0x32C0);
    std::int32_t* selectIndex = reinterpret_cast<std::int32_t*>(sb + 0x33B0);
    if (!NearbyIsReadable(slots, 30 * sizeof(void*)) ||
        !NearbyIsReadable(selectIndex, sizeof(std::int32_t)))
        return false;
    unsigned char* statusBytes =
        reinterpret_cast<unsigned char*>(machineStatus);
    if (!NearbyIsReadable(statusBytes + 0x338, sizeof(void*))) return false;

    // 1) 找 cloneCount 个空背包槽
    int slotIdx[30];
    size_t n = 0;
    for (int i = 0; i < 30 && n < cloneCount; ++i) {
        if (!slots[i]) slotIdx[n++] = i;
    }
    if (n < cloneCount) {
        Log("[PA] pushall %s gid=%llu: 背包槽不足 (%zu/%zu)\n",
            PaMachineLabel(gid), static_cast<unsigned long long>(gid), n, cloneCount);
        return false;
    }

    // 2) staged 全部 clone（顺序入槽，遇并发占用则回滚已入槽者）
    for (size_t j = 0; j < cloneCount; ++j) {
        NearbyAddReference(clones[j]);  // staging 引用
        if (InterlockedCompareExchangePointer(
                reinterpret_cast<void* volatile*>(&slots[slotIdx[j]]),
                clones[j], nullptr) != nullptr) {
            NearbyReleaseOwnedReference(clones[j]);
            for (size_t k = 0; k < j; ++k) {
                InterlockedCompareExchangePointer(
                    reinterpret_cast<void* volatile*>(&slots[slotIdx[k]]),
                    nullptr, clones[k]);
                NearbyReleaseOwnedReference(clones[k]);
            }
            return false;
        }
    }

    // 3) 轮流以每种 clone 作 selectIndex 调原生 push。引擎 auto-select 命中后会把
    //    全部 staged 原料一次性转入机器并清空这些槽，故「所有 staged 槽被清空」
    //    即成功。
    //    【v1.6.37 判据修正·关键·修闪】不再用 gate A(status+0x240 子结构)判成功：
    //    实测高炉 leftover=0（料已被吞、slotdiag running 正常推进）却 gateA 判假
    //    → ok=false 误判失败 → PA 每 5 秒重试 pushall → 每轮往玩家背包槽
    //    (save+0x32C0)/选中索引(0x33B0) stage+清 → 游戏 near 面板跟着重建 →
    //    大进度 UI 一闪一闪。leftover=0 = 引擎已接管原料，是可靠成功信号
    //    （v1.6.8 同款判据）。手动投料不走这套 stage，所以手动不闪。
    bool allConsumed = false;
    int faulted = 0;
    for (size_t c = 0; c < cloneCount; ++c) {
        const std::int32_t savedIndex = *selectIndex;
        *selectIndex = slotIdx[c];
        __try {
            g_paPushNative(reinterpret_cast<uintptr_t>(machineStatus), 0, 0, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            faulted = 1;
        }
        *selectIndex = savedIndex;
        if (faulted) break;

        allConsumed = true;
        for (size_t j = 0; j < cloneCount; ++j) {
            if (slots[slotIdx[j]] != nullptr) { allConsumed = false; break; }
        }
        if (allConsumed) break;   // 全吞完 = 成功，停止试其它 selectIndex
    }

    // 4) 收尾：已空槽不碰引用（引擎接管）；残留槽 null + 释放 staging 引用。
    bool anyLeftover = false;
    for (size_t j = 0; j < cloneCount; ++j) {
        if (slots[slotIdx[j]] != nullptr) {
            InterlockedCompareExchangePointer(
                reinterpret_cast<void* volatile*>(&slots[slotIdx[j]]),
                nullptr, clones[j]);
            NearbyReleaseOwnedReference(clones[j]);
            anyLeftover = true;
        }
    }
    const bool ok = !faulted && !anyLeftover;
    if (!ok) {
        Log("[PA] pushall %s gid=%llu: 未触发配方自动选择 (fault=%d leftover=%d)\n",
            PaMachineLabel(gid), static_cast<unsigned long long>(gid), faulted, anyLeftover ? 1 : 0);
    } else {
        Log("[PA] pushall OK %s gid=%llu (all staged consumed)\n",
            PaMachineLabel(gid), static_cast<unsigned long long>(gid));
    }
    return ok;
}

// ============================================================
// v1.5.85【加工 UI 回归修复】补齐游戏脚本的 UI 创建指令
//
// 取证（Ghidra 反编译 + data.dat 内 Lua 字节码）：玩家投料时游戏脚本
//   依次执行 4 条 ScriptCommand 指令。PA 从 v1.5.71 起只做了第 1 条
//   （push，0x27BE10），缺后三条 —— 这正是"面板空白 / 走远小进度条消失 /
//   自动投料没有加工动画"的根因（三现象同源）：
//     [已有] GimmickProcessPushItem        -> 0x27BE10
//     [补]   GimmickCreateProcessingUI     -> 0x27D480  近处大 UI（同时写 UIPros）
//     [补]   GimmickCreateProcessingUI_Far -> 0x27DCE0  远处小进度条
//     [补]   GimmickCreateProcessingManpuUI-> 0x27E460  加工动画
//   三条签名统一为 (baseID, gimmickStatus)。
//
// 历史误区订正：v1.5.53~v1.5.55 以为 UIPros/UIManpu 是"进度计数"并做 +1，
//   实为**对象指针**——UIPros = *(taskObj[2]+0x28)，加工完成时由
//   GimmickDeleteProcessingUI 置 0。对指针做加法必然把 UI 弄坏，这也解释
//   了当年"三键 +1 后面板反而消失"。
//
// 安全性：三条函数的第一件事都是 FUN_14025d0e0(ctx, baseID) 查 baseID 表，
//   查不到即 throw "invalid baseID %d"（此时尚未做任何状态修改）。故这里
//   先用同一张表（FUN_140186930）预检候选 baseID，只把合法的传进去；外层
//   再套 SEH。任何意外都原地降级，绝不影响已经成功的投料。
// ============================================================
__declspec(noinline) static uintptr_t PaSafeBaseIdResolve(
    PaBaseIdResolveFn fn, uintptr_t table, uintptr_t baseId) {
    uintptr_t ret = 0;
    __try { ret = fn(table, baseId); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ret = 0; }
    return ret;
}

__declspec(noinline) static uintptr_t PaSafeUiCreate(
    PaUiCreateFn fn, uintptr_t baseId, uintptr_t status) {
    uintptr_t ret = 0;
    __try { ret = fn(baseId, status); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ret = 0; }
    return ret;
}

// v1.5.87：baseID 解析抽成独立函数，供 push 时创建与每轮维护共用
// v1.5.110：重写。Ghidra 进程内反编译实锤：near/far/manpu 内部对 baseID 的
//   prefab 查询结果（FUN_140186930 返回值）**从未用于 UI 对象构造**——UI 完全
//   由 status 驱动构建（FUN_1405d70b0(alloc,status) / FUN_1405db870(alloc,status)）；
//   baseID 唯一作用是过 FUN_14025d0e0 的"表项存在性"校验（不存在即 throw →
//   PA 的 SEH 吞掉返回 0）。故：任意一条合法 baseID 表项对全部机器通用，无需按
//   机器配对。优先用探针捕获/ bootstrap 扫描得到的任意合法 baseID，使
//   PaCreateProcessingUi 对"玩家从未靠近"的自动投料机器也能成功建 UI / 动画。
// ============================================================
// v1.5.121【far UI 消失·只读诊断】零逻辑变更，一次运行定案
//
// 背景：用户实测「加工动画已出现（v1.5.112 的 near 交还游戏生效），但走远后
//   远处小 UI 进度（far UI）消失」。
// 本诊断要回答的唯一问题：**玩家走远后 UIPrsFar 键里到底是什么？**
//   · 键空(0)         → PA 维护链本应重建 far → 查维护链为何没跑/没建成功
//   · 键非零但对象已死 → 残留句柄，PA 永远判定"far 已存在"不重建 → 需改判定
//   · 键非零且对象有效 → 问题不在键，在别处
//
// 关键背景（v1.5.95 同源教训）：UIManpu / UIPros / UIPrsFar 存的是**对象指针/
//   每帧刷新的句柄**，不是稳定值。**键非零 ≠ 对象还活着** —— 键存在只说明
//   "曾经建过"，不代表"现在可用"。这正是本诊断要验证的点。
//
// 全部纯读：不写任何 status 键、不建/删任何 UI 对象。
// ============================================================

static u64 PaResolveBaseIdEx(void* machineStatus, u64 gid, u64 preferred) {
    if (!g_paBaseIdResolve || !g_paBaseIdTableSlot) return 0;
    void* table = NearbyIsReadable(g_paBaseIdTableSlot, sizeof(void*))
                      ? *g_paBaseIdTableSlot
                      : nullptr;
    if (!table) return 0;

    // 1) v1.5.110：任意已知合法 baseID（prefab 未被使用，仅过校验）即通用兜底
    const u64 vb = g_paValidBaseId.load(std::memory_order_relaxed);
    if (vb && PaSafeBaseIdResolve(g_paBaseIdResolve,
                                  reinterpret_cast<uintptr_t>(table),
                                  static_cast<uintptr_t>(vb)) != 0) {
        return vb;
    }

    // 2) 原有候选（preferred=本条指令同名捕获值；其次 全局捕获 / 字段兜底）
    u64 cands[6] = {};
    size_t n = 0;
    if (preferred) cands[n++] = preferred;
    {
        const uintptr_t cap = g_paCapturedBaseId.load(std::memory_order_relaxed);
        if (cap && n < 6) cands[n++] = cap;
    }
    for (size_t slot = 0; slot < 3; ++slot) {
        char key[8] = {};
        _snprintf_s(key, sizeof(key), _TRUNCATE, "itemID%zu", slot);
        u64 v = 0;
        if (PaReadStatusU64(machineStatus, key, &v) && v) {
            if (n < 6) cands[n++] = v;
            break;
        }
    }
    if (gid && n < 6) cands[n++] = gid;
    {
        u64 v = 0;
        if (PaReadStatusU64(machineStatus, "objID", &v) && v && n < 6) cands[n++] = v;
    }
    {
        u64 v = 0;
        if (PaReadStatusU64(machineStatus, "baseID", &v) && v && n < 6) cands[n++] = v;
    }
    for (size_t i = 0; i < n; ++i) {
        if (PaSafeBaseIdResolve(g_paBaseIdResolve,
                                reinterpret_cast<uintptr_t>(table),
                                static_cast<uintptr_t>(cands[i])) != 0) {
            g_paValidBaseId.store(cands[i], std::memory_order_relaxed);
            return cands[i];
        }
    }

    // 3) v1.5.110 bootstrap：探针从未捕获过时（玩家从未靠近任何机器），扫描任意
    //   合法表项作兜底。结果缓存到 g_paValidBaseId，仅首次执行一次（之后走分支1）。
    for (u64 c = 1; c <= 0x10000; ++c) {
        if (PaSafeBaseIdResolve(g_paBaseIdResolve,
                                reinterpret_cast<uintptr_t>(table), c) != 0) {
            g_paValidBaseId.store(c, std::memory_order_relaxed);
            Log("[PA][bid] v1.5.110 bootstrap 找到合法 baseID=%llu\n",
                static_cast<unsigned long long>(c));
            return c;
        }
    }
    return 0;
}

// v1.5.96：保留原调用点语义（用 manpu 捕获的 baseID 作首选）
static u64 PaResolveBaseId(void* machineStatus, u64 gid) {
    return PaResolveBaseIdEx(machineStatus, gid,
                             g_paCapturedBaseId.load(std::memory_order_relaxed));
}

// v1.5.111-diag：前置声明（判活计数取数辅助，纯读；动画专项已于 v1.5.118 收口）
static int PaAnimAliveCount(const void* owner);

// v1.5.85：push 成功后补齐 near/far/manpu 三条 UI 指令（玩家投料脚本链）。
// v1.5.89【回归修复】恢复 push 当帧建 manpu（v1.5.87 行为）：manpu 函数
//   (0x27E460) 会把 UIManpu 键写入 status+0x58；游戏据此判定机器 UI/加工已
//   正确装配，完工时保留组件(status+0x338)。v1.5.88 把 manpu 挪到 inProc 维护
//   后，玩家投料后靠近机器 → PaMaintainProcessingUi 提前 return(UIPros 存在)
//   → UIManpu 键始终未写 → 游戏完工清组件 → 下一轮 not ignited → 自动投料断。
//   故 push 当帧建 manpu 不可省（保自动投料循环）。动画渲染所需的"计数>0 活跃
//   对象"改由 PaMaintainProcessingUi 在 inProc(远离)时重建一次补足（见下）。
// 2026-11-07 23:25 自动投料 UI 判据（定义在 PaCreateProcessingUi 之前，
//   因为下面的函数体会引用它们；PaTxObsPublish 本体在 2239 行，先声明）。
static std::atomic<long> s_patxUiForced{0};   // 自动投料绕过 playerNear 早退次数
static std::atomic<long> s_patxUiSkip{0};     // 非自动投料仍被 playerNear 拦下次数
static void PaTxObsPublish();
// 2026-11-09【UI 链可观测性】独立命名空间 paui（patx 已占 7/8 槽，不再扩）。
// 只记数、不参与任何判定分支，零行为变更；用于一次实机定位「小 UI / 动画不出来」
// 究竟卡在 PaCreateProcessingUi 的哪个早退点。
static std::atomic<long> s_pauiEnter{0};
static std::atomic<long> s_pauiNoStatus{0};
static std::atomic<long> s_pauiCool{0};
static std::atomic<long> s_pauiNearSkip{0};
static std::atomic<long> s_pauiNoUiObj{0};
static std::atomic<long> s_pauiBuilt{0};
static std::atomic<long> s_pauiAutoFar{0};

// fromAutoFeed: 本次调用来自「原生 push 自动投料成功」。自动投料没有玩家
//   交互，游戏不会为它跑「开 near 面板 + 注册动画实例」脚本；旧版一律用
//   playerNear 早退，等于把 near 交给一个永远不会来的游戏流程 —— 于是
//   far 小进度条与 manpu 加工动画两边都不建（2026-11-07 23:16 实机复现）。
static void PaCreateProcessingUi(void* machineStatus, u64 gid, bool fromAutoFeed) {
    qol::obs::SetInt("paui", "enter", (int)s_pauiEnter.fetch_add(1) + 1);
    if (!machineStatus) {
        qol::obs::SetInt("paui", "noStatus", (int)s_pauiNoStatus.fetch_add(1) + 1);
        return;
    }
    // v1.6.42【闪的第四来源：逐槽装配】本函数在**每次投料成功后**都会被调，
    //   而 PaAttemptInput 外层是"逐槽循环（最多 3 次/轮）"——实测 20:56:13 一轮里
    //   对两台高炉各投 3 槽 → 本函数被连调 6 次 → far+manpu 各建 6 次，
    //   每次尾部都 event-notify → 游戏面板被连轰 12 次重建通知 = 疯狂闪。
    //   push 当帧建 manpu 的真实目的只有一个：把 UIManpu 键写上（保加工组件
    //   存活，KB-087/v1.5.89）——**写一次就够**，与投几个槽无关。
    //   故加 per-gid 30 秒装配冷却：同机 30 秒内只装配一次。
    {
        struct PaAsmCool { u64 gid; u64 tick; };
        static PaAsmCool s_cool[16] = {};
        static size_t s_cur = 0;
        const u64 now = GetTickCount64();
        PaAsmCool* e = nullptr;
        for (size_t k = 0; k < 16; ++k)
            if (s_cool[k].tick && s_cool[k].gid == gid) { e = &s_cool[k]; break; }
        if (!e) {
            e = &s_cool[s_cur % 16];
            ++s_cur;
            e->gid = gid;
            e->tick = now;
        } else if (now - e->tick < 30000ULL) {
            qol::obs::SetInt("paui", "cool", (int)s_pauiCool.fetch_add(1) + 1);
            return;   // 30 秒内已装配过，本次投料不再重复建 UI
        } else {
            e->tick = now;
        }
    }
    // v1.6.37【闪修复·关键】玩家在旁（UIPros 非 0）时游戏自己管 near 面板；PA 若在
    //   push 成功后于此处再建 far/manpu，会与 near 面板抢 machine+0x2b8 同一槽并触发
    //   manpu 末尾 FUN_14015d560 event-notify 重绘 → 大进度 UI 一闪一闪。
    //   故靠近时直接跳过本次创建；far/manpu 等玩家走远后由 PaMaintainProcessingUi 的
    //   !nearNow 分支补建（那条路径已验证可用）。
    //   v1.6.44：UIPros 判据在 1.30 失效（恒 0），补距离守卫（与投料守卫同源）。
    {
        u64 uiProsVal = 0;
        bool playerNear =
            PaStatusKeyPresent(machineStatus, "UIPros", &uiProsVal) &&
            uiProsVal != 0;
        if (!playerNear) {
            float mx = 0.f, my = 0.f, mz = 0.f;
            if (PaReadFloat3(machineStatus, PA_POSITION_OFFSET, &mx, &my, &mz) &&
                PaPlayerNearMachine(mx, my))
                playerNear = true;
        }
        if (fromAutoFeed && !playerNear)
            qol::obs::SetInt("paui", "autoFar", (int)s_pauiAutoFar.fetch_add(1) + 1);
        if (playerNear && fromAutoFeed) {
            // 自动投料：即使玩家站在旁边也由 PA 补建 far/manpu（near 仍交游戏）。
            // 30 秒装配冷却在上面的块里，不会造成逐槽投料时的重建风暴。
            s_patxUiForced.fetch_add(1, std::memory_order_relaxed);
            PaTxObsPublish();
        } else if (playerNear) {
            s_patxUiSkip.fetch_add(1, std::memory_order_relaxed);
            qol::obs::SetInt("paui", "nearSkip", (int)s_pauiNearSkip.fetch_add(1) + 1);
            return;
        }
    }
    // v1.5.106：登记本机 event owner（供动画实例对表）
    if (!g_paUiNear && !g_paUiFar && !g_paUiManpu) {
        qol::obs::SetInt("paui", "noUiObj", (int)s_pauiNoUiObj.fetch_add(1) + 1);
        return;
    }
    qol::obs::SetInt("paui", "built", (int)s_pauiBuilt.fetch_add(1) + 1);
    // v1.5.112【死锁解锁】不再抢建 near UI（UIPros）。
    //   历史：v1.5.85 起 push 当帧把 near+far+manpu 三件套一起建，理由是"自动投料
    //   没有玩家交互，脚本不会跑"。但实测与反编译给出的是另一幅图景：
    //     ① PaProbeNear 的 `!byPa` 分支确实捕获到**游戏自身**调 0x27D480 —— 玩家
    //        靠近时游戏会自己建 near，PA 不必代劳；
    //     ② 一旦 PA 把 UIPros 写成非 0，PaMaintainProcessingUi 首行
    //        `UIPros 非 0 → return` 就永久成立 → far/manpu 维护、动画重建、诊断
    //        全部变成死代码（v1.5.87 的"靠近再离开小进度条消失"修复因此从未生效）；
    //     ③ 更关键的是引擎侧：v1.5.94 取证已证「PA 加工过程从未在动画实例表
    //        (mgr+0x33b8/+0x32c0) 注册过实例」，而 manpu(0x27E460) 末尾只有
    //        FUN_1401569c0(mgr,owner)>0 才写 transform —— 抢建 near 会让游戏以为
    //        面板已装配，从而跳过它自己的「开面板 + 注册动画实例」流程。
    //   故 near 交还游戏；PA 只保留 far（远处进度条）与 manpu（写 UIManpu 键，
    //   维持加工组件存活 → 自动投料循环不断，v1.5.89 血泪教训）。
    const u64 baseNear = 0;   // v1.5.112：near 由游戏自建，PA 不再解析其 baseID
    // v1.6.50：push 后装配同样必须用本机自己的 baseId（见 PaMaintainProcessingUi
    //   里的实测对照：全局兜底值 0xB10 ≠ 本机真实 base）。
    // v1.6.56：harvest 到的本机 baseId **直接使用**（它来自游戏自己的调用，
    //   必然合法）；不再走 PaResolveBaseIdEx —— 那个函数第一步就用全局兜底
    //   2820 直接 return，preferred 永远轮不到（这就是 v1.6.50 无效的原因）。
    const u64 hbAsm = PaBaseHarvestGet(machineStatus);
    const u64 baseFar = hbAsm ? hbAsm
        : PaResolveBaseIdEx(machineStatus, gid,
                            g_paBaseIdFar.load(std::memory_order_relaxed));
    const u64 baseManpu = hbAsm ? hbAsm
        : PaResolveBaseIdEx(machineStatus, gid,
                            g_paCapturedBaseId.load(std::memory_order_relaxed));
    if (!baseFar && !baseManpu) {  // 无合法 baseID：静默降级
        qol::obs::SetInt("pa", "uiDegrade", 1);
        qol::obs::EventF(3, 2500,
            L"[PA] 自动投料机无合法 baseID：far/manpu UI 未建（动画回归风险）");
        return;
    }
    const uintptr_t st = reinterpret_cast<uintptr_t>(machineStatus);
    // 抑制探针自捕获：PA 自己也调这三个函数，会命中入口 hook
    g_paUiCallDepth.fetch_add(1, std::memory_order_relaxed);
    const uintptr_t r0 = 0;   // v1.5.112：near 不再由 PA 创建
    const uintptr_t r1 = PaUiCreateGuarded(g_paUiFar, baseFar, st, gid,
                                           PaUiKind::kFar, baseFar != 0);
    const uintptr_t r2 = PaUiCreateGuarded(g_paUiManpu, baseManpu, st, gid,
                                           PaUiKind::kManpu, baseManpu != 0);
    g_paUiCallDepth.fetch_sub(1, std::memory_order_relaxed);

    // v1.6.x: 观测层——把 PA 加工 UI/动画装配结果发布成机器可读寄存器。
    // 这是"自动投料无加工动画"回归的根因点：版本更新后 AI 只需断言
    //   pa.uiFar / pa.uiManpu 是否随 pa.uiCreateCount 增长，即可零截图判定。
    {
        qol::obs::SetInt("pa", "uiDegrade", 0);  // 走到这里说明有合法 baseID
        const int farOk = (r1 != 0) ? 1 : 0;
        const int manOk = (r2 != 0) ? 1 : 0;
        static int s_uiCreate = 0, s_farOk = 0, s_manOk = 0;
        ++s_uiCreate;
        if (farOk) ++s_farOk;
        if (manOk) ++s_manOk;
        qol::obs::SetInt("pa", "uiFar",         farOk);
        qol::obs::SetInt("pa", "uiManpu",       manOk);
        qol::obs::SetInt("pa", "uiCreateCount", s_uiCreate);
        qol::obs::SetInt("pa", "uiFarCount",    s_farOk);
        qol::obs::SetInt("pa", "uiManpuCount",  s_manOk);
        if (!manOk) {
            qol::obs::EventF(2, 2500,
                L"[PA] manpu UI 建失败（加工动画可能缺失）");
        }
    }

}

// v1.5.87：每轮对加工中机器维护 far/manpu。
//   near UI 由游戏接管（玩家靠近时创建/销毁），PA 不维护；
//   但游戏 near/far 切换会清理 PA 创建的 far UI（UIPrsFar 键清空），
//   玩家离开后游戏原生 far 恢复依赖"玩家手动投料"标记（PA 机器无此标记）→ 不恢复。
//   故 PA 在"远离（UIPros 空）且 far 键空"时重建 far，修复"靠近再离开小进度条消失"。
//   manpu 动画：v1.5.88 起由本函数（inProc 确认后、远离开）创建，避开 push
//   当帧动画实例未激活(+0x260=0)导致 FUN_1401569c0 永不判活的问题；创建时加工
//   已激活则动画相位注册后立即可渲染。键存在时仅 +1 发重绘信号。
// ============================================================
// v1.5.92-diag【加工动画取证·纯读版】
//
// v1.5.91 曾用 inline hook 观测判活函数（本 build FUN_1401569c0），但漏了"只装一次"保护，
//   PaTick 每 tick 重复 HookInstall 同一地址 → 目标指令被反复改写 → 靠近加工
//   机械即崩溃（日志可见 probe installed=1 在 16ms 内刷了几十条）。该做法废弃。
//
// 本版改为**纯读取**：直接遍历 manager+0x33b8(10) / +0x32c0(30) 两张实例表，
//   打印每个实例的指针、owner(**(inst+0x240))、+0x260 值，并标记 owner 是否
//   等于本机 status。零写入、零 hook；任一步不可读即跳过，不触碰游戏状态。
//
// manager 推导（必须两层解引用，v1.5.93 修正）：
//   root = *(void**)(base + RVA_GAME_ROOT)      ← PA 既有用法同源（PaReadPtr 0 偏移）
//   mgr  = *(void**)(root + 0x208)
//   v1.5.92 曾误写成 *(void**)(base + RVA_GAME_ROOT + 0x208)（少一层），读到哈希样
//   垃圾值 0x3F7CC727...，可读性校验把后续读取挡住才没崩，但那批数据无效。
// 开关：qol_pa_aliveprobe.txt（沿用），预算 600 条，每台机器独立 6 秒节流。
// v1.5.94 增补：本机侧链路（UIManpu 键 / comp / comp+0x340 激活对象 / 其 vtbl+0x50）
//   + t33b8 空槽打印，用于验证「新建 manpu 是否占槽」这一推断。
// ============================================================
static bool PaSafeReadPtrRaw(const void* addr, void** out) {
    if (!addr || !out) return false;
    if (!NearbyIsReadable(const_cast<void*>(addr), sizeof(void*))) return false;
    *out = *reinterpret_cast<void* const*>(addr);
    return true;
}

// v1.5.111-diag / v1.6.19 订正：调游戏的「实例判活计数」FUN_1401569c0(mgr, owner, filter=0)，纯读。
//   语义（反编译实锤，KB-107/KB-108）：遍历 mgr+0x33b8(11 槽) 与 mgr+0x32c0(30 槽)
//   两张动画/UI 实例表，累加「owner(**(inst+0x240)) == 传入 owner」的实例的
//   *(int*)(inst+0x260)。manpu 创建函数末尾正是用它挑候选 owner：只有计数 >0
//   才写入 transform，动画才真正出现在场景里。
//   返回 -1 = root/mgr 不可读；-2 = 调用抛异常（地址错位时就是这个值，旧版
//   0x153EE0 落在别人函数中段，恒为 -2 且被 SEH 静默吞掉）。
//   ⚠ 地址走 RVA_MANPU_ALIVE 宏（HotConfig 可热改），不要再硬编码常量。
static int PaAnimAliveCount(const void* owner) {
    const uintptr_t base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    void* root = nullptr;
    void* mgr = nullptr;
    if (!PaSafeReadPtrRaw(reinterpret_cast<void*>(base + RVA_GAME_ROOT),
                          &root) ||
        !root) return -1;
    if (!PaSafeReadPtrRaw(reinterpret_cast<char*>(root) + 0x208, &mgr) ||
        !mgr) return -1;
    typedef int (*PaAliveFn)(void*, void*, void*);
    PaAliveFn fn = reinterpret_cast<PaAliveFn>(base + RVA_MANPU_ALIVE);
    int r = -2;
    __try { r = fn(mgr, const_cast<void*>(owner), nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        r = -2;
        // v1.6.19：地址错位必须留下痕迹，不再静默。只报一次，避免刷日志。
        static std::atomic<int> s_excWarned{0};
        if (s_excWarned.fetch_add(1, std::memory_order_relaxed) < 1) {
            Log("[PA] manpu alive probe 异常：RVA_MANPU_ALIVE=0x%llX 不像函数入口"
                "（mgr=%p owner=%p）\n",
                static_cast<unsigned long long>(RVA_MANPU_ALIVE), mgr,
                const_cast<void*>(owner));
        }
    }
    return r;
}

// v1.6.20【动画实例表全局采样】纯读 mgr+0x33b8(11 槽) / +0x32c0(30 槽) 两张实例表，
//   统计：实例总数 / 活跃数（*(int*)(inst+0x260) > 0）/ 与给定 owner 匹配数。
//   这条比"按 owner 判活"更强，因为它不依赖 owner 是否对得上 —— 直接回答
//   「游戏到底有没有注册动画实例」，从而把「动画缺失」劈成两半：
//     · animInst == 0            → 游戏压根没注册实例：问题在创建/装配侧（KB-107 那条线）
//     · animInst > 0, active > 0 → 实例活着却不显示：问题在渲染侧（transform/相机/裁剪）
//     · animInst > 0, hit == 0   → 实例属于别的 owner：PA 的机器没被注册进去
//   零 hook、零写入，只读；不可读即返回 0，绝不拖累维护链。
static void PaAnimTableSample(void* owner, int* instN, int* activeN,
                              int* hitN) {
    *instN = 0;
    *activeN = 0;
    *hitN = 0;
    const uintptr_t base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    void* root = nullptr;
    void* mgr = nullptr;
    if (!PaSafeReadPtrRaw(reinterpret_cast<void*>(base + RVA_GAME_ROOT),
                          &root) ||
        !root) return;
    if (!PaSafeReadPtrRaw(reinterpret_cast<char*>(root) + 0x208, &mgr) ||
        !mgr) return;
    const uintptr_t offs[2] = { 0x33B8, 0x32C0 };
    const int cnts[2] = { 11, 30 };
    for (int t = 0; t < 2; ++t) {
        for (int i = 0; i < cnts[t]; ++i) {
            void* inst = nullptr;
            if (!PaSafeReadPtrRaw(reinterpret_cast<char*>(mgr) + offs[t] +
                                      i * sizeof(void*),
                                  &inst) ||
                !inst) continue;
            ++(*instN);
            unsigned char* ib = reinterpret_cast<unsigned char*>(inst);
            if (NearbyIsReadable(ib + 0x260, sizeof(int)) &&
                *reinterpret_cast<int*>(ib + 0x260) > 0)
                ++(*activeN);
            if (owner) {
                void* p1 = nullptr;
                void* ow = nullptr;
                if (PaSafeReadPtrRaw(ib + 0x240, &p1) && p1 &&
                    PaSafeReadPtrRaw(p1, &ow) && ow && ow == owner)
                    ++(*hitN);
            }
        }
    }
}
// ============================================================
// v1.6.10-diag【小 UI 消失取证】维护链决策日志（per-gid 30 秒一条）
//   背景（2026-10-29 用户实测）：靠近加工机械后小进度条消失、离开也不恢复，
//   且 1.20 时代同样如此。而这条链在日志里**完全隐形** —— 三条出口全是静默：
//     ① 玩家在旁（UIPros 非 0）→ 直接 return，不维护 far/manpu
//     ② baseFar/baseManpu 都解析失败 → 直接 return
//     ③ UIPrsFar 键仍存在 → 跳过重建（但 UI 可能已被游戏 side 销毁，键是脏的）
//   另：PaSafeUiCreate 的返回值此前从不检查，创建失败也无痕。
//   本日志把 uiPros / baseFar / baseManpu / farKey / farVal / rFar 一次打全，
//   复现一次即可判定卡在哪一步，不再靠猜。
// ============================================================
static void PaUiDiagLog(u64 gid, const char* stage, u64 uiPros, u64 baseFar,
                        u64 baseManpu, bool farPresent, u64 farVal,
                        uintptr_t rFar) {
    struct PaUiDiagEntry { u64 gid; u64 tick; };
    static PaUiDiagEntry s_entries[16] = {};
    static size_t s_cursor = 0;
    const u64 now = GetTickCount64();
    PaUiDiagEntry* entry = nullptr;
    for (size_t k = 0; k < 16; ++k) {
        if (s_entries[k].tick != 0 && s_entries[k].gid == gid) {
            entry = &s_entries[k];
            break;
        }
    }
    if (!entry) {
        entry = &s_entries[s_cursor % 16];
        ++s_cursor;
        entry->gid = gid;
        entry->tick = 0;
    }
    if (now - entry->tick < 10000ULL) return;
    entry->tick = now;
    Log("[PA] uimaint gid=%llu stage=%s uiPros=%llu baseFar=%llu "
        "baseManpu=%llu farKey=%d farVal=%llu rFar=%p\n",
        static_cast<unsigned long long>(gid), stage,
        static_cast<unsigned long long>(uiPros),
        static_cast<unsigned long long>(baseFar),
        static_cast<unsigned long long>(baseManpu), farPresent ? 1 : 0,
        static_cast<unsigned long long>(farVal),
        reinterpret_cast<void*>(rFar));
}

// v1.6.16【HUD 统一守卫·方案A】唯一允许的 PA 侧加工 UI 创建出口。
//   needNow 语义：调用方**已独立判定**本机本轮确实需要这个 UI（如 far 键丢失、
//   manpu 键缺失、push 后首次装配）。false = 例行巡检，一律不创建。
//   冷却：成功后 8 秒（期间即便再判定需要也不重复建 → 压住闪烁）；
//         失败后按 5→10→20→40→60 秒退避（原 1 秒一轮的重试风暴是忽明忽暗的主因）。
// v1.6.52【最小干预】PA 是否允许自己创建加工 UI（near/far/manpu）。
//   默认 0 = 全不建。依据：
//     · 用户实际用法是"每台机器都手投一次启动"，手投时游戏会建**有效且能显示**的
//       UI（实测手投碳窑走远有小进度条，far 调用 0 次 → 小 UI 来自 near 对象）；
//     · PA 的机器 UIPros 键根本不存在（present=0），说明游戏从没给它建过；
//     · PA 每次重建都改写 UIPrsFar/UIManpu 键，但渲染列表里仍是老对象 →
//       键指向新对象、列表没有 → 不显示；更糟的是会把"手投建立的好对象"覆盖掉。
//   ⇒ 不造对象，只投料/取料，UI 生命周期完全交还游戏。
//   需要回到"PA 自建 UI"实验时把此值改 1 即可。
static volatile int g_paUiAutoCreate = 1;  // v1.6.56: 重新启用（baseId 修好后必须建，才能产生新 handle 触发写键+notify）

// v1.6.59【Case B 脏键恢复开关】0=关（默认，不碰脏键，保持 v1.6.58 行为）；
//   1=启用：玩家走远后 PA 自建的 UI 对象被游戏销毁但键没清（脏键残留）→ 清键强制重建，
//   补偿"PA 投料机走远无小 UI"。只在 PA 自己成功建过的机器上生效，避免误伤手投机。
static volatile long g_paUiCaseBFix = 0;

// v1.6.63【A/B 热开关】两个互斥的诊断方案，默认都关（保持 v1.6.62 行为）。
//   UI_NATIVE_ONLY=1：PA 完全不自建 near/far/manpu，UI 生命周期交还游戏，
//                     用于验证「PA 的 no-op 重建/清脏键是否在打掉游戏建的 near」。
//   UI_PIGGYBACK=1 ：在游戏自己调 near 的 hook 里（TLS 上下文天然正确）用同一
//                     baseId/status 补建 far/manpu，绕开 harvest status 配对。
// 两者均由 QoL_hot.json 的 productionauto.* 约 1s 热生效，无需重编/重启。
static volatile int g_paUiNativeOnly = 0;
static volatile int g_paUiPiggyback = 0;

// v1.6.60【Case B 纯观测计数器 · 零行为变更】
//   pa 命名空间的 obs int 槽只有 8 个（QoL_Shared/obs.h OBS_KEYS=8），已被
//   clock/nearby/production/uiNear*/manpuAlive/animInst 占满；SetInt 超限会
//   静默丢弃 —— 这正是 2026-11-05 首轮观测中 manpuAliveSeq 恒 None 的真因
//   （当时被误读成「维护链没跑」）。扩 OBS_KEYS 会改 QolObsShm 布局、必须
//   重编全部 MOD，风险不可接受。故启用独立命名空间 "pacaseb"：只占 mods[]
//   一个新槽（上限 32），不动布局、不动其他 DLL、不参与任何判定分支。
//   纪律：仅做原子自增 + SetInt，AI 可零截图/零日志地读到「修复是否真执行」。
static std::atomic<long> s_caseBNearHits{0};
static std::atomic<long> s_caseBFarHits{0};
// v1.6.62：修复是否真正生效的机器可读判据（pacaseb 原 6 键 + 这 2 键 = 8，不扩 OBS_KEYS）
static std::atomic<long> s_paGuardResets{0};   // harvest 到位后重置失败退避的次数
static std::atomic<long> s_paTlsScoped{0};    // 真正做过 TLS 上下文切换的创建次数
static void PaCaseBObsPublish() {
    qol::obs::SetInt("pacaseb", "dllBuild",       163);
    qol::obs::SetInt("pacaseb", "caseBEnabled",   (int)g_paUiCaseBFix);
    qol::obs::SetInt("pacaseb", "caseBNearHits",  (int)s_caseBNearHits.load());
    qol::obs::SetInt("pacaseb", "caseBFarHits",   (int)s_caseBFarHits.load());
    qol::obs::SetInt("pacaseb", "guardResets",    (int)s_paGuardResets.load());
    qol::obs::SetInt("pacaseb", "tlsScoped",      (int)s_paTlsScoped.load());
}

// v1.6.61【重塑阶段 0 · 投料事务观测，零行为变更】
//   2026-11-07 定位：16:32 实机日志 pushfeed=0，而声明默认 =1 —— 首次 PaTick
//   被「qol_pa_pushfeed.txt 是否存在」覆盖成 0，随后 PaAttemptInput 在
//   `if (!g_paUsePushFeed) return false` 静默返回，整条 push 投料链不可达。
//   历次补丁都堆在这条不可达链里，所以「炭窑/高炉不投料」反复修不好。
//   本命名空间把结论变成机器可读：
//     pushfeed=0            → 开关被关（配置问题，不是逻辑问题）
//     attempts=0            → 主链压根没被调用（扫描/分组/状态机门控）
//     attempts>0 success=0  → 投料被拒（点火/组件/燃料，lastFail 给 reason）
//     success>0             → 投料真的发生
//   沿用 pacaseb 的纪律：独立命名空间占 mods[] 一个新槽（上限 32），
//   不动 QolObsShm 布局、不重编其它 MOD、不参与任何判定分支。
static std::atomic<long> s_patxAttempts{0};   // PaAttemptInput 被调用次数
static std::atomic<long> s_patxSuccess{0};    // 原生 push 成功次数
static std::atomic<long> s_patxLastFail{0};   // 最近一次失败 reason code
static void PaTxObsPublish() {
    qol::obs::SetInt("patx", "dllBuild", 161);
    qol::obs::SetInt("patx", "pushfeed", (int)g_paUsePushFeed);
    qol::obs::SetInt("patx", "attempts", (int)s_patxAttempts.load());
    qol::obs::SetInt("patx", "success",  (int)s_patxSuccess.load());
    qol::obs::SetInt("patx", "lastFail", (int)s_patxLastFail.load());
    qol::obs::SetInt("patx", "uiForced", (int)s_patxUiForced.load());
    qol::obs::SetInt("patx", "uiSkip",   (int)s_patxUiSkip.load());
}

// v1.6.59【Case B 追踪】记录 PA 已成功为哪些 (gid,kind) 建过 UI，供脏键恢复只在
//   "PA 自己建过"的机器上触发（手投机器 PA 从没建过 → 不触发 → 不会闪）。
namespace {
struct PaUiCreatedRec { u64 key; bool created; };
static PaUiCreatedRec s_uiCreated[64] = {};
static void PaUiMarkCreated(u64 gid, PaUiKind kind) {
    const u64 key = gid * 3ULL + static_cast<u64>(kind);
    for (int i = 0; i < 64; ++i) {
        if (s_uiCreated[i].key == key || !s_uiCreated[i].created) {
            s_uiCreated[i].key = key;
            s_uiCreated[i].created = true;
            return;
        }
    }
}
static bool PaUiCreatedQ(u64 gid, PaUiKind kind) {
    const u64 key = gid * 3ULL + static_cast<u64>(kind);
    for (int i = 0; i < 64; ++i)
        if (s_uiCreated[i].key == key && s_uiCreated[i].created) return true;
    return false;
}
// v1.6.59【Case B 幂等】记录每台机每种 UI「上次我们成功建立的键值」；当前键值若与之
//   相同=对象仍活，不再清键重建（避免每 5s 反复清活对象导致闪烁）；仅当游戏把对象重新
//   弄脏（键值变化）才再触发。key = gid*3 + kind。
struct PaCaseBEst { u64 key; u64 estVal; };
static PaCaseBEst s_caseBEst[64] = {};
static u64* PaCaseBEstGet(u64 gid, PaUiKind kind) {
    const u64 key = gid * 3ULL + static_cast<u64>(kind);
    for (int i = 0; i < 64; ++i)
        if (s_caseBEst[i].key == key) return &s_caseBEst[i].estVal;
    for (int i = 0; i < 64; ++i)
        if (s_caseBEst[i].key == 0) { s_caseBEst[i].key = key; s_caseBEst[i].estVal = 0; return &s_caseBEst[i].estVal; }
    return &s_caseBEst[key % 64].estVal;  // 满槽退化为哈希槽
}
}

static uintptr_t PaUiCreateGuarded(PaUiCreateFn fn, u64 baseId, uintptr_t st,
                                   u64 gid, PaUiKind kind, bool needNow) {
    // v1.6.63【入口诊断】限 20 条，一次定位 early-return 点。
    //   22:2x 实机反常：新 DLL 的 [PA][guard]/[PA][tls] 一条都没有，
    //   但 [PA][uifix]/[PA][nearui] 有 40+ 条 —— 说明在到达创建代码前就 return。
    {
        static int s_entryLog = 0;
        if (s_entryLog < 20) {
            ++s_entryLog;
            Log("[PA][entry] gid=%llu kind=%s auto=%d need=%d fn=%p base=%llu "
                "st=%p harvestN=%d want=%p\n",
                static_cast<unsigned long long>(gid), PaUiKindName(kind),
                (int)g_paUiAutoCreate, needNow ? 1 : 0,
                reinterpret_cast<void*>(fn),
                static_cast<unsigned long long>(baseId),
                reinterpret_cast<void*>(st), g_paActorHarvestN,
                PaActorHarvestGet(reinterpret_cast<void*>(st)));
        }
    }
    if (!g_paUiAutoCreate) return 0;   // v1.6.52：默认不创建任何加工 UI
    if (!needNow) return 0;
    // ① 方案A：三要素任一为空即跳过 —— 这是唯一可挡住 0x2866E0 空指针的手段
    if (!fn) { qol::obs::SetInt("pa", "uiGuardNoFn", 1); return 0; }
    if (!baseId) { qol::obs::SetInt("pa", "uiGuardNoBaseId", 1); return 0; }
    if (!st) {
        if (kind == PaUiKind::kFar) {
            qol::obs::SetInt("pa", "farNullStatus", 1);
            PaUiDiagLog(gid, "far-null-status", 0, 0, 0, false, 0, 0);
        } else {
            qol::obs::SetInt("pa", "manpuNullStatus", 1);
            PaUiDiagLog(gid, "manpu-null-status", 0, 0, 0, false, 0, 0);
        }
        {
            static int s_hits = 0;
            if (s_hits < 5) {
                ++s_hits;
                Log("[PA] uiguard skip: status=0 kind=%s gid=%llu "
                    "(空指针守卫，避免游戏 status+0x10 解引用崩溃)\n",
                    PaUiKindName(kind),
                    static_cast<unsigned long long>(gid));
            }
        }
        return 0;
    }
    // ② per-(gid,kind) 冷却/退避/失败限次
    struct PaUiGuardSlot {
        u64 key;
        u64 lastTry;
        u64 gapMs;
        int fails;      // v1.6.41：连续失败计数（>=3 进入 60s 退避上限）
        bool used;
        void* seenActor;  // v1.6.62：上次创建时该机收获到的 actor（用于解锁退避）
    };
    static PaUiGuardSlot s_slots[32] = {};
    static size_t s_cur = 0;
    const u64 key = gid * 3ULL + static_cast<u64>(kind);  // v1.6.51: 3 种 kind
    PaUiGuardSlot* s = nullptr;
    for (size_t k = 0; k < 32; ++k) {
        if (s_slots[k].used && s_slots[k].key == key) { s = &s_slots[k]; break; }
    }
    if (!s) {
        s = &s_slots[s_cur % 32];
        ++s_cur;
        s->key = key;
        s->used = true;
        s->lastTry = 0;
        s->gapMs = 0;
        s->fails = 0;
        s->seenActor = nullptr;
    }
    // v1.6.62【解锁 harvest 前的误锁死】2026-11-11 实机日志铁证：
    //   20:51 游戏刚启动、玩家从未靠近任何机器 → PaActorHarvestGet 全 0 →
    //   PA 用 bootstrap 通用 baseID=13 连建 3 轮全部 ret=0 / 键未变(函数早退)
    //   → guard `fails>=3` 把每台机器锁死 1 小时；21:03 玩家走到机器群、
    //   游戏自己调 near 并 harvest 到真 actor+baseId 时 guard 早已 give-up，
    //   收获永远用不上。这才是「小 UI/漫符全阴性」的直接可修真因。
    //   修法：actor 检测提到节流之前；该机收获配对出现/变化时立刻清空
    //   fails/gapMs/lastTry，让本轮就用真 actor 重试。
    void* wantActor = PaActorHarvestGet(reinterpret_cast<void*>(st));
    if (s->seenActor != wantActor) {
        void* seenOld = s->seenActor;
        s->seenActor = wantActor;
        s->fails = 0;
        s->gapMs = 0;
        s->lastTry = 0;
        s_paGuardResets.fetch_add(1, std::memory_order_relaxed);
        static int s_resetLog = 0;
        if (s_resetLog < 30) {
            ++s_resetLog;
            Log("[PA][guard] reset gid=%llu kind=%s actor %p -> %p "
                "(harvest 到位，解除历史失败退避)\n",
                static_cast<unsigned long long>(gid), PaUiKindName(kind),
                seenOld, wantActor);
        }
    }
    const u64 now = GetTickCount64();
    if (s->lastTry && s->gapMs && (now - s->lastTry) < s->gapMs) {
        qol::obs::SetInt("pa", "uiGuardThrottled", 1);
        return 0;
    }
    s->lastTry = now;
    void* curActor = PaTlsActorPeek();
    // v1.6.62【无本机 actor 一律不建】逆向取证 v1 结论③：near/far/manpu
    //   三个底层函数都用 TLS 上下文里的 actor 取 manager；PA 在 mod_tick
    //   线程直调时该上下文不是本机 actor，建出的对象挂错 manager、键却被
    //   写脏，还触发 event-notify 闪屏。旧逻辑「收获表为空就照建」正是
    //   20:51 那批 base=13 / ret=0 / 键未变 的来源。
    //   ⇒ 只有拿到该机自己的 actor 才创建；其余机器宁可不建，交游戏自己。
    if (!wantActor) {
        qol::obs::SetInt("pa", (kind == PaUiKind::kFar) ? "tlsFarNoActor"
                                                       : "tlsMpuNoActor", 1);
        static int s_noActorLog = 0;
        if (s_noActorLog < 12) {
            ++s_noActorLog;
            Log("[PA][guard] skip %s gid=%llu reason=no-harvest-actor"
                "(等游戏自己为这台机器调一次 near/far)\n",
                PaUiKindName(kind),
                static_cast<unsigned long long>(gid));
        }
        return 0;
    }
    PaTlsSnapshot tlsSnap;
    bool scoped = false;
    if (g_paTlsScopeFix && wantActor)
        scoped = PaTlsScopeBegin(&tlsSnap, wantActor);
    if (scoped) s_paTlsScoped.fetch_add(1, std::memory_order_relaxed);
    // v1.6.47：PA 调用侧同一份条件快照（对照用）
    if (kind == PaUiKind::kFar) {
        static int s_c2 = 0;
        if (s_c2 < 8) {
            ++s_c2;
            PaCondProbe("pa-far", gid);
        }
    }
    const uintptr_t r = PaSafeUiCreate(fn, static_cast<uintptr_t>(baseId), st);
    if (scoped) PaTlsScopeEnd(&tlsSnap);
    {
        static int s_tlsLog = 0;
        if (s_tlsLog < 60) {
            ++s_tlsLog;
            Log("[PA][tls] %s gid=%llu cur=%p want=%p scoped=%d ret=%p\n",
                PaUiKindName(kind),
                static_cast<unsigned long long>(gid),
                curActor, wantActor, scoped ? 1 : 0,
                reinterpret_cast<void*>(r));
        }
        qol::obs::SetInt("pa", (kind == PaUiKind::kFar) ? "tlsFarWant"
                                                       : "tlsMpuWant",
                         wantActor ? 1 : 0);
    }
    if (r) {
        s->fails = 0;
        PaUiMarkCreated(gid, kind);  // v1.6.59：标记 PA 已成功建过该机该型 UI（供 Case B 脏键恢复判定）
        s->gapMs = 8000ULL;  // 成功后 8 秒冷静期：远 UI 不会又被打掉重建
    } else {
        // v1.6.41【失败限次】失败的 far/manpu 创建**同样**会让游戏走到
        //   FUN_14015dd20(写键) + FUN_14015d560(event-notify)，所以"建不成还一直重试"
        //   等于每 5~10 秒给 near 面板打一次重建通知 —— 用户看到的就是隔几秒闪一下。
        //   v1.6.62：连续失败 3 次只进入 60 秒退避（不再永久放弃），
        //   保证不会再有周期性 notify，同时留给后续 harvest/上下文修复重试机会。
        //   fails 会被清零，不影响正常机器。
        ++s->fails;
        if (s->fails >= 3) {
            s->gapMs = 60000ULL;
            static int s_giveUpLog = 0;
            if (s_giveUpLog < 40) {
                ++s_giveUpLog;
                Log("[PA][tls] give-up %s gid=%llu 连续失败 %d 次，退避 60s"
                    "（每次失败也会触发游戏 event-notify → 面板闪）\n",
                    PaUiKindName(kind),
                    static_cast<unsigned long long>(gid), s->fails);
            }
            qol::obs::SetInt("pa", (kind == PaUiKind::kFar) ? "uiGiveUpFar"
                                                           : "uiGiveUpMpu", 1);
        } else {
            s->gapMs = s->gapMs ? ((s->gapMs >= 60000ULL) ? 60000ULL
                                                          : s->gapMs * 2ULL)
                                : 5000ULL;
        }
    }
    return r;
}

// v1.6.54【撤掉"玩家在旁不投料"守卫】
//   v1.6.38 当时加它是为了修"面板闪烁"（怀疑 PA 在玩家眼前补料导致重建）。
//   但后续实测已排除："空的高炉不加工也在闪、60fps 抓 5993 帧画面无变化"→
//   闪属显示链路，与 PA 投料无关。而该守卫造成严重后果：
//     · 用户站在机器旁观察时 PA 永远不投料 → 看起来像"PA 不工作"；
//     · 用户在旁时看不到"投料→UI 出现"的过程，无法验证 UI 链路。
//   ⇒ 默认 0 = 不因玩家在旁而跳过投料。需要恢复旧行为时置 1。
static volatile int g_paSkipWhenPlayerNear = 0;

static void PaMaintainProcessingUi(void* machineStatus, u64 gid) {

    // v1.6.63【配对诊断】harvest 表里的 status 与 PA 维护的 machineStatus
    //   是否为同一指针？若不同，则 PaBaseHarvestGet/PaActorHarvestGet 永远
    //   查不到，PA 会退回 bootstrap base=13 —— 这正是 22:2x 实机现象。
    {
        static int s_pairLog = 0;
        if (s_pairLog < 12) {
            ++s_pairLog;
            Log("[PA][pair] gid=%llu st=%p want=%p harvestN=%d "
                "hv0=%p baseN=%d bv0=%p b0=%llu\n",
                static_cast<unsigned long long>(gid), machineStatus,
                PaActorHarvestGet(machineStatus), g_paActorHarvestN,
                g_paActorHarvestN ? g_paActorHarvest[0].actor : nullptr,
                g_paBaseHarvestN,
                g_paBaseHarvestN ? g_paBaseHarvest[0].status : nullptr,
                static_cast<unsigned long long>(
                    g_paBaseHarvestN ? g_paBaseHarvest[0].baseId : 0));
        }
    }

    // v1.6.53【黄金快照写回】游戏建的 UI 键值一旦被清/被换，就把快照值写回去
    //   （只写键，不造对象）。放在最前面，先于任何 near/far 判断。
    PaGoldenRestore(machineStatus, gid);

    // v1.5.106：登记本机 event owner（供动画实例对表）
    u64 nearVal = 0;
    const bool nearKey = PaStatusKeyPresent(machineStatus, "UIPros", &nearVal);
    // v1.6.44【距离守卫】nearNow 判定同样被 UIPros=0 骗过：玩家站旁边时
    //   PA 会误走"远离"分支去建 far/manpu → 与游戏 near 面板抢 0x2b8 + event-
    //   notify → 闪。补一条距离判定：玩家在本机 100px 内一律视为 nearNow。
    bool nearNow = nearKey && nearVal != 0;
    if (!nearNow) {
        float mx = 0.f, my = 0.f, mz = 0.f;
        if (PaReadFloat3(machineStatus, PA_POSITION_OFFSET, &mx, &my, &mz) &&
            PaPlayerNearMachine(mx, my)) {
            nearNow = true;
        }
    }

    // v1.6.18【near UI 观察·不改行为】near UI（GimmickCreateProcessingUI / 0x285CE0）
    //   由游戏自建，PA 自 v1.5.112 起不创建它。此处只把它的存在/激活与 near↔far
    //   切换次数发布成机器可读 obs 寄存器，供 AI 自主 playtest 判定 near/far 探测
    //   是否正常——**不改动任何创建/维护/动画逻辑**。
    //   诊断判据：walk-up/walk-away 测试中 uiNearTransitions 应 >0 且 uiNearActive
    //   随玩家靠近变 1、离开变 0；若恒为 0 即 near 探测断链（游戏没建 near UI 或
    //   PA 读键失效）。多机器并发时该计数器会合并统计，聚焦单机测试即可。
    {
        static std::atomic<long long> s_nearTransitions{0};
        static int s_prevNear = -1;
        if (s_prevNear != -1 && (int)nearNow != s_prevNear)
            s_nearTransitions.fetch_add(1);
        s_prevNear = (int)nearNow;
        qol::obs::SetInt("pa", "uiNearActive",      nearNow ? 1 : 0);
        qol::obs::SetInt("pa", "uiNearPresent",     nearKey  ? 1 : 0);
        qol::obs::SetInt("pa", "uiNearTransitions", (int)s_nearTransitions.load());
        // v1.6.60：pa 8 槽已满，把 Case B 判据镜像到独立命名空间 pacaseb
        qol::obs::SetInt("pacaseb", "uiNearTransitions", (int)s_nearTransitions.load());
        PaCaseBObsPublish();
    }

    // v1.6.19【KB-108 收口 · 判活实证】把 manpu 判活计数真正接上（此前
    //   PaAnimAliveCount 只有声明和定义、从未被调用；且它硬编码的 0x153EE0
    //   在本 build 落在别人函数中段 → 一旦接上也会恒被 SEH 吞成 -2）。
    //   此处每台机器 3 秒一次读 FUN_1401569c0(mgr, eventOwner, 0)，发布成
    //   obs 寄存器，让两条长期挂着"等实测"的链路变成机器可判定：
    //     · pa.manpuAlive  近 / 远 都会刷新 —— 走近走远加工机时若数值随加工
    //                      进度跳动，即证明近 UI 探测 + 动画实例注册都健康；
    //     · 自动投料的机器 pa.manpuAlive > 0 ⇒ 存在活着的动画实例（必然被渲染）。
    //   取值语义：-1 = root/mgr 不可读或 owner 取不到；-2 = 调用异常（地址错位，
    //   日志会留一次告警）；>=0 = 累加计数。
    //   纪律：纯读 + SEH + per-gid 3 秒节流，零行为变更。
    {
        struct PaAliveTick { u64 gid; u64 tick; };
        static PaAliveTick s_aliveTick[16] = {};
        static size_t s_aliveCur = 0;
        static std::atomic<int> s_aliveSeq{0};
        const u64 aliveNow = GetTickCount64();
        PaAliveTick* ae = nullptr;
        for (size_t k = 0; k < 16; ++k) {
            if (s_aliveTick[k].tick != 0 && s_aliveTick[k].gid == gid) {
                ae = &s_aliveTick[k];
                break;
            }
        }
        if (!ae) {
            ae = &s_aliveTick[s_aliveCur % 16];
            ++s_aliveCur;
            ae->gid = gid;
            ae->tick = 0;
        }
        if (ae->tick == 0 || aliveNow - ae->tick >= 3000ULL) {
            ae->tick = aliveNow;
            void* evOwner = nullptr;
            const bool ownerOk = PaGetEventOwner(machineStatus, &evOwner);
            const int alive =
                (ownerOk && evOwner) ? PaAnimAliveCount(evOwner) : -1;
            qol::obs::SetInt("pa", "manpuAlive",    alive);
            // v1.6.20：实例表全局采样（把"动画缺失"劈成 创建侧 / 渲染侧 两半）
            int instN = 0, activeN = 0, hitN = 0;
            PaAnimTableSample((ownerOk && evOwner) ? evOwner : nullptr,
                              &instN, &activeN, &hitN);
            qol::obs::SetInt("pa", "animInst",     instN);
            qol::obs::SetInt("pa", "animActive",   activeN);
            qol::obs::SetInt("pa", "animOwnerHit", hitN);
            qol::obs::SetInt("pa", "manpuOwnerOk",  (ownerOk && evOwner) ? 1 : 0);
            qol::obs::SetInt("pa", "manpuAliveNear", nearNow ? 1 : 0);
            qol::obs::SetInt("pa", "manpuAliveGid",
                             static_cast<int>(gid & 0x7fffffffULL));
            qol::obs::SetInt("pa", "manpuAliveSeq",
                             s_aliveSeq.fetch_add(1, std::memory_order_relaxed) + 1);
            // v1.6.60：原 pa 槽满会被丢弃，镜像一份供 AI 判定维护链是否在跑
            qol::obs::SetInt("pacaseb", "manpuAliveSeq", (int)s_aliveSeq.load());
        }
    }

    // v1.6.11【即时恢复】per-gid 记住上一轮"玩家是否在旁"。
    //   从「在旁」变成「离开」的那一轮就立刻重建 far，不再等周期兜底 ——
    //   配合维护链缩到 1 秒一轮，玩家离开机器后小进度条应在 ~1 秒内出现。
    // v1.6.46【致命 bug 修复】原判定用 `lastNear(UIPros 值) != 0 && now==0`，
    //   但 1.30 的 UIPros 恒为 0 → 条件永不成立 → **justLeft 从未触发过** →
    //   PA 一次都没建过 far/manpu（日志里完全没有 `[PA][tls] manpu` 行）→
    //   小进度条和加工动画永远出不来。这正是"补了 TLS 上下文也看不到 UI"的
    //   真正原因（TLS 只是前置条件，触发器本身是死的）。
    //   改用 nearNow（含距离守卫）的前后跳变来判「刚走开」。
    bool justLeft = false;
    {
        struct PaNearState { u64 gid; int lastNearNow; bool used; };
        static PaNearState s_near[16] = {};
        static size_t s_cursor = 0;
        PaNearState* e = nullptr;
        for (size_t k = 0; k < 16; ++k) {
            if (s_near[k].used && s_near[k].gid == gid) {
                e = &s_near[k];
                break;
            }
        }
        if (!e) {
            e = &s_near[s_cursor % 16];
            ++s_cursor;
            e->gid = gid;
            e->used = true;
            e->lastNearNow = nearNow ? 1 : 0;
            // 首见即视为"刚走开"：保证每台加工机至少拿到一次 far/manpu 装配机会。
            //   否则玩家一开始就站在远处时，该机器的 justLeft 永远等不到。
            justLeft = true;
        } else {
            const int nowFlag = nearNow ? 1 : 0;
            justLeft = (e->lastNearNow == 1 && nowFlag == 0);
            e->lastNearNow = nowFlag;
        }
        e->lastNearNow = nearNow ? 1 : 0;
    }
    (void)nearVal;

    if (nearNow) {
        // v1.6.10-diag：玩家在旁 → 游戏 near UI 接管，PA 不碰 far/manpu。
        // v1.6.36【回退 v1.6.35】v1.6.35 曾在此处补建 manpu（想修近距动画），实测：
        //   ① manpu 末尾写 UIManpu 键 + FUN_14015d560 event-notify → 触发游戏反复重建
        //      near 面板 → **大进度 UI 闪烁**（用户实测）；② near 面板先占 machine+0x2b8
        //      同一 key，manpu 对象注册不进 → 动画仍不出现。→ 净负面，已回退。
        //   结论：近距动画不能靠"近处直接调 manpu"实现（与 near 面板抢同一 0x2b8 槽），
        //   需另寻机制。详见 FN-029 修订三。
        PaUiDiagLog(gid, "near-takeover", nearVal, 0, 0, false, 0, 0);
        return;
    }
    // v1.6.12【按需解析 baseID】原代码每轮无条件解析两次 baseID（每次要多次
    //   调 PaSafeBaseIdResolve），而维护链现在是 1 秒一轮 × 每台加工中机器
    //   —— 纯浪费，也是卡顿的潜在来源。改为只在"确实要建 UI"时才解析。
    const uintptr_t st = reinterpret_cast<uintptr_t>(machineStatus);
    u64 baseFar = 0;
    u64 baseManpu = 0;
    u64 farVal = 0;
    g_paUiCallDepth.fetch_add(1, std::memory_order_relaxed);
    uintptr_t rFar = 0;
    const bool farKey = PaStatusKeyPresent(machineStatus, "UIPrsFar", &farVal);
    // v1.6.12【只在需要时建一次】反悔 v1.6.10 的"每 30 秒无条件重建"——
    //   那样每台加工中机器都会被反复调 PaSafeUiCreate，持续产生开销（卡顿源），
    //   而且没必要。改为**事件驱动**：每台机器记一份 memo（上次创建后的
    //   UIPrsFar 键值），只有下面三种情况才建一次：
    //     ① 键不存在 / 值为 0                → UI 确实没了
    //     ② 玩家刚走开（UIPros 由非 0 变 0） → 游戏 near→far 切换后必然丢，
    //                                          立刻补一次（justLeft 每轮只 true 一次）
    //     ③ 键值 ≠ memo 记录值               → 游戏把 far 换过/清过（脏键也能捕获）
    //   其余情况一律不碰：不重复创建、不产生额外开销。
    bool refreshFar = false;
    {
        struct PaFarMemo { u64 gid; u64 seenKey; bool used; };
        static PaFarMemo s_memo[16] = {};
        static size_t s_cursor = 0;
        PaFarMemo* m = nullptr;
        for (size_t k = 0; k < 16; ++k) {
            if (s_memo[k].used && s_memo[k].gid == gid) {
                m = &s_memo[k];
                break;
            }
        }
        if (!m) {
            m = &s_memo[s_cursor % 16];
            ++s_cursor;
            m->gid = gid;
            m->seenKey = 0;
            m->used = true;
        }
        const bool keyEmpty = (!farKey || !farVal);
        // v1.6.41【闪的第二来源，本条是根因之一】
        //   原先 keyChanged(=farVal != memo) 也算"需要重建"。但实测 UIPrsFar 的值
        //   是**每 tick 变的句柄**（4013→4523→3492，见 20:54:47 的 uimaint 三台机
        //   各不相同，KB-087 也早写过 UIManpu 每帧刷新）→ 该条件**恒真** →
        //   PA 每轮为每台加工机调一次 far，而 far 尾部必然执行
        //   FUN_14015dd20(写 UIPrsFar) + FUN_14015d560(event-notify)
        //   → 游戏 near 面板被反复通知重建 = 用户看到的「隔几秒闪一下」。
        //   即使 far 创建返回 0（失败）也会先走到 notify，所以"没建成"不等于"没副作用"。
        //   ⇒ 删除该触发条件；值抖动只留痕，不再驱动创建。
        const bool keyChanged =
            (m->seenKey != 0 && farVal != 0 && farVal != m->seenKey);
        if (keyChanged) {
            static int s_churnLog = 0;
            if (s_churnLog < 20) {
                ++s_churnLog;
                Log("[PA] farmemo gid=%llu 句柄抖动 %llu -> %llu "
                    "(每 tick 变，已不再作为重建依据)\n",
                    static_cast<unsigned long long>(gid),
                    static_cast<unsigned long long>(m->seenKey),
                    static_cast<unsigned long long>(farVal));
            }
        }
        // v1.6.13【验证脏句柄】实测（16:32~16:33 日志）：
        //   farKey=1 farVal=3405 之类的小句柄一直存在，玩家却看不到小 UI，
        //   且 uiPros 恒为 0 → "刚走开"这个触发点从未出现（justLeft 永不成立）。
        //   说明句柄是**脏的**（对象已销毁、map 键没清），而小整数句柄无法
        //   解引用验活。这里加一个 per-gid 15 秒的验证性重建：
        //     · 若重建后玩家能看到 UI      → 确认脏句柄，周期重建即修复方案；
        //     · 若 rFar 恒为 0 且仍看不到  → 说明创建函数本身没生效，另找入口；
        //     · 若出现多个进度条          → 说明重复创建不幂等，立刻回退。
        //   15 秒/台的开销可忽略（每台每 15 秒一次调用 + 一次 baseID 解析）。
        {
            struct PaFarRefresh { u64 gid; u64 tick; };
            static PaFarRefresh s_refresh[16] = {};
            static size_t s_cursor2 = 0;
            const u64 nowTick = GetTickCount64();
            PaFarRefresh* e = nullptr;
            for (size_t k = 0; k < 16; ++k) {
                if (s_refresh[k].tick != 0 && s_refresh[k].gid == gid) {
                    e = &s_refresh[k];
                    break;
                }
            }
            // v1.6.49【恢复保活重建】v1.6.41 曾把它关掉（以为周期重建引起面板闪），
            //   但闪已证实与 PA 无关（空高炉也闪、60fps 抓不到任何画面变化）。
            //   而 v1.5.122 的结论是：**far UI 必须靠周期性重建保活**，否则键里
            //   留的是游戏早已作废的句柄 → 走远什么都看不到。故恢复为开。
            static volatile long s_farPeriodicRefresh = 1;  // 1=开（15s/台保活重建）
            if (!e) {
                e = &s_refresh[s_cursor2 % 16];
                ++s_cursor2;
                e->gid = gid;
                e->tick = nowTick;
                refreshFar = true;
            // v1.6.15【闪烁修复】原逻辑每到 15 秒就把 far UI 无条件重建一次
            //   （refreshFar=true），远处小进度条被反复销毁重建 → 玩家看到一闪一闪。
            //   v1.6.13 的「脏句柄」验证性实验已完成取证（结论：句柄脏但不影响创建成败），
            //   此项周期重建关停；需要对比时把下面的 0 改成 1 即可恢复。
            } else if (s_farPeriodicRefresh &&
                       nowTick - e->tick >= 15000ULL) {
                e->tick = nowTick;
                refreshFar = true;
            }
        }
        // v1.6.49：恢复 keyChanged 作为"保活"信号之一。
        //   UIPrsFar 的值是每 tick 刷新的句柄，值一变往往意味着游戏那边已经把
        //   旧对象换掉/作废了——此时不重建，键里就是死句柄（v1.5.122 的教训）。
        //   为避免每轮都建，用 memo 记"上次真正重建时的值"，且叠加下面的 15s
        //   周期兜底；guard 的 8 秒成功冷静期保证不会每轮刷。
        // v1.6.58【残留修复·"靠近再走开小 UI 消失"】
        //   根因：PA 重建 far 用的 harvested baseId 与游戏手柄写进 UIPrsFar 的 handle
        //   恒定相差固定值 → 同一 handle → 游戏判"值没变"→ 不写键、不发 event-notify
        //   → far 对象始终进不了渲染列表。原 justLeft/refreshFar/keyChanged 三路触发都带
        //   着同一个 baseId 重建 → 全是 no-op（且周期重建还会搅 near 面板）。
        //   故改为**只在键为空（游戏确把 far 对象销毁并清键）时建一次**；键非空一律
        //   不碰，交游戏自己维护。守卫 8s 成功冷静期兜底，不狂建。
        // v1.6.59【Case B 脏键恢复】游戏销毁 PA 自建的 far 对象后不清 UIPrsFar 键
        //   （脏键残留）→ keyEmpty 恒假 → far 永不重建 → 走远无小 UI。补偿：玩家走远
        //   (!nearNow) 且 PA 曾成功建过该机 far（g_paUiCaseBFix 开）时，清掉脏键并
        //   触发重建（节流 5s/台，避免每 tick 轰）。只在 PA 自己建过的机器生效，
        //   手投机器 PA 从没建过 → 不触发 → 不会闪。
        bool caseBFar = false;
        if (g_paUiCaseBFix && !nearNow && farKey && farVal &&
            PaUiCreatedQ(gid, PaUiKind::kFar)) {
            struct PaCaseB { u64 gid; u64 tick; };
            static PaCaseB s_cb[16] = {};
            static size_t s_cbc = 0;
            const u64 nowCb = GetTickCount64();
            PaCaseB* e = nullptr;
            for (size_t k = 0; k < 16; ++k)
                if (s_cb[k].gid == gid) { e = &s_cb[k]; break; }
            if (!e) { e = &s_cb[s_cbc % 16]; ++s_cbc; e->gid = gid; e->tick = 0; }
            u64* est = PaCaseBEstGet(gid, PaUiKind::kFar);
            // 幂等：当前键值==上次建立的键值（对象仍活）→ 不清；键值变化（游戏重新弄脏）才清。
            if (farVal != *est && nowCb - e->tick >= 5000ULL) {
                e->tick = nowCb;
                PaSetStatusU64(machineStatus, "UIPrsFar", 0);
                caseBFar = true;
                s_caseBFarHits.fetch_add(1, std::memory_order_relaxed);  // v1.6.60 纯观测
                PaCaseBObsPublish();
                static int s_cblog = 0;
                if (s_cblog < 30) {
                    ++s_cblog;
                    Log("[PA][caseB] 清脏键 far gid=%llu val=%llu\n",
                        static_cast<unsigned long long>(gid),
                        static_cast<unsigned long long>(farVal));
                }
            }
        }

        const bool needFarCreate = keyEmpty;
        if ((needFarCreate || caseBFar) && g_paUiFar) {
            // v1.6.50【关键修复：baseId 必须用本机自己的】
            //   实测对照（23:15 日志）：
            //     游戏自己调 far/near 时 base = 0x2022 / 0x20c8 / 0x205c …（每台不同）
            //     PA 调时 base 恒为 2832(0xB10) —— 一个"能过校验"的通用值
            //   baseID 不是通用令牌，而是**每台机器 UI 资源的身份**。用通用值建出的
            //   对象是空壳（UIPrsFar 被写成 19130 这种怪 handle、且永远不渲染）。
            //   本 MOD 的 near/far 入口 hook 早已把 (status → baseId) 正确配对收进
            //   PaBaseHarvestGet，这里优先用它；没有才退回全局兜底。
            const u64 harvested = PaBaseHarvestGet(machineStatus);
            baseFar = harvested ? harvested
                : PaResolveBaseIdEx(machineStatus, gid,
                                    g_paBaseIdFar.load(std::memory_order_relaxed));
            if (harvested) {
                static int s_bidLog = 0;
                if (s_bidLog < 10) {
                    ++s_bidLog;
                    Log("[PA][bid] far gid=%llu 使用本机 base=%llu(0x%llx)\n",
                        static_cast<unsigned long long>(gid),
                        static_cast<unsigned long long>(harvested),
                        static_cast<unsigned long long>(harvested));
                }
            }
        }
        // v1.6.16【全部 HUD 统一守卫】不变量与 v1.6.15 相同（st 空不能进游戏），
        //   但判定不再写死在本调用点，而是收进 PaUiCreateGuarded —— 三件套装同时
        //   获得：① 空指针守卫（方案A）② 成功 8 秒冷静期（压闪烁）
        //   ③ 失败 5→10→20→40→60 秒退避（压 1 秒一轮的创建风暴，另一路闪烁源）。
        if (!g_paUiNativeOnly && (needFarCreate || caseBFar) && baseFar) {
            rFar = PaUiCreateGuarded(g_paUiFar, baseFar, st, gid,
                                     PaUiKind::kFar, true);
            u64 afterVal = 0;
            const bool afterOk =
                PaStatusKeyPresent(machineStatus, "UIPrsFar", &afterVal);
            m->seenKey = afterOk ? afterVal : 0;
            // v1.6.46【成败判据·KB-087】Create UI 的返回值不是错误码（obj[5]），
            //   真正的成功信号是**游戏函数把 UIPrsFar 键改写成新对象指针**
            //   （函数尾部 FUN_14015dd20 写键）。故比对创建前后的键值。
            Log("[PA][uifix] far gid=%llu base=%llu before=%llu after=%llu "
                "ret=%p -> %s\n",
                static_cast<unsigned long long>(gid),
                static_cast<unsigned long long>(baseFar),
                static_cast<unsigned long long>(farVal),
                static_cast<unsigned long long>(afterVal),
                reinterpret_cast<void*>(rFar),
                (afterOk && afterVal != farVal) ? "键已更新(成功)"
                                                : "键未变(函数早退)");
            if (caseBFar)
                *PaCaseBEstGet(gid, PaUiKind::kFar) = afterOk ? afterVal : 0;
        } else if (needFarCreate) {
            PaUiDiagLog(gid, "no-baseid", nearVal, 0, 0, farKey, farVal, 0);
        }
    }
    // v1.6.17【动画回归修复】manpu 是"远离"特效(FN-029)：玩家靠近时 near UI 接管，
    //   PA 不碰 manpu；仅当远离(nearNow=false)才经守卫建 manpu。守卫的 8 秒成功冷静
    //   期保证加工激活(+0x260>0)后必有一次重建触发渲染，且不每秒刷屏。
    //   v1.5.121 把"inProc 后重建 manpu"移除 → manpu 只在 push 帧建一次(激活=0)→
    //   永不渲染，动画只能靠走远回来游戏原生重注册才出现。本改动恢复"远离即建"。
    u64 manpuVal = 0;
    if (nearNow) {
// ============================================================
// v1.5.121【重大更正】加工动画已修复 —— v1.5.118 的「已知限制」结论作废
//
// 【事实】2026-10-27 08:44 用户实测：**加工动画已出现**。
//   ⇒ v1.5.118 写的「最终根因 = 引擎配对 ID 未成立 / 记为已知限制」**是错的**。
//
// 【正解】v1.5.112 的注释（PaCreateProcessingUi 处）假说③ 已写明：
//   "抢建 near 会让游戏以为面板已装配，从而跳过它自己的「开面板 + 注册动画
//    实例」流程" → v1.5.112 把 near 交还游戏 → 游戏自己走完注册流程
//   → 引擎注册动画实例 → FUN_1401569c0(mgr,owner) > 0 → manpu 写 transform
//   → **动画出现**。
//   ⇒ **动画的正解就是 v1.5.112 那一行"near 交还游戏"**，与 baseID 配对、
//     与"引擎注册-查询配对 ID"都无关。
//
// 【为什么绕了 5 个版本】v1.5.113~117 连续追查 baseID 配对与"配对 ID"机制
//   （两个都是**假想根因**），把真正的解放一行就在眼前却没认出来。
//   v1.5.117 的 achain hit=0 也是**判据缺陷造成的假象**（拿"查表键"比
//   "实例侧键"），据此断定"引擎没注册实例"是错的 —— 引擎**注册了**。
//
// 【保留这段的原因】记录一次完整的教训链，供后续同类问题参考：
//   ① 正解常常就写在**自己上一版的注释里**，只是没和当前代码对上；
//   ② 连续的"反例假设"会把注意力锁死在一个错误方向（baseID / 配对 ID）；
//   ③ 判据必须能区分真假（本案的 achain 判据就不合格）。
//
// 【现在的状态】
//   · 动画：✅ 已生效（v1.5.112 起），本函数**不再需要重建 manpu**。
//   · manpu 键语义：v1.5.111 实测其值每 tick 都在变（9498→9609→9352→…）
//     ⇒ 是**每帧刷新的句柄**，不是稳定指针。**绝不碰它的值**（+1 会毁 UI）。
//   · 远端小进度条（far UI）：已定案并修复（v1.5.122）。
//     **真根因不在 far 键，而在维护链的门禁**：原门禁用 status+0x18 的
//     `inProc` 键判「加工中」，但 PA 走的原生 push 路径**不写 inProc**
//     （Ghidra 实锤：FUN_14027be10 只写 itemID%d/itemVal%d；4 条 gamescript
//     包装函数也都不写）→ 门禁永不成立 → 维护链整条不执行 → far UI 永不重建。
//     修法：门禁改用**槽 timer**（与 slotdiag 同源判据，本次日志已证明正确：
//     slotdiag running=3 而 inProc 读不到）。
//     详见 进度文件/DEV_PROGRESS.md 的 v1.5.122 条目。
//
// ★ 若游戏更新后要重启此专题：先读
//   进度文件/PA加工动画专题_逆向地图与收口结论_20261027.md
//   ——但注意该文档 §六 的"最终根因"部分同样已被本次实测更正，
//     真实根因是 v1.5.112 的 near 交还游戏，不是"配对 ID"。
// ============================================================
        (void)manpuVal;
    } else {
        // v1.6.41【闪的第三来源：每轮无条件试建 manpu】
        //   v1.6.16 起这段是"每轮都调一次 PaUiCreateGuarded(needNow = baseManpu!=0)"，
        //   只靠守卫的 8 秒冷静期/失败退避来限流 → 实际是「成功也每 8 秒重调一次、
        //   失败每 5~60 秒重调一次」。而 manpu 尾部必然执行
        //   FUN_14015dd20(写 UIManpu 键) + FUN_14015d560(event-notify)，
        //   **每调一次就给 near 面板发一次重建通知** = 隔几秒闪一下。
        //   现在改成与 far 同样的事件驱动：只有「键真的没了」或「玩家刚走开」
        //   （near→far 切换必然丢）才建一次，其余轮次完全不动。
        u64 manpuKeyVal = 0;
        const bool manpuPresent =
            PaStatusKeyPresent(machineStatus, "UIManpu", &manpuKeyVal) &&
            manpuKeyVal != 0;
        // v1.6.49【恢复动画保活】KB-107 的两段式解：动画实例只有在
        //   "机器真正进入加工(+0x260>0)"之后再建一次 manpu 才会注册相位→渲染。
        //   v1.6.42 改成纯事件驱动后 manpu 键一直有值 → 一次都不建 → 动画永不出现。
        //   故加 per-gid 20 秒周期重建（配合 guard 的 8 秒冷静期，实际约 20s/台）。
        bool manpuPeriodic = false;
        {
            struct PaMpTick { u64 gid; u64 tick; };
            static PaMpTick s_mp[16] = {};
            static size_t s_mpCur = 0;
            const u64 nowM = GetTickCount64();
            PaMpTick* e = nullptr;
            for (size_t k = 0; k < 16; ++k)
                if (s_mp[k].tick && s_mp[k].gid == gid) { e = &s_mp[k]; break; }
            if (!e) {
                e = &s_mp[s_mpCur % 16];
                ++s_mpCur;
                e->gid = gid;
                e->tick = nowM;
                manpuPeriodic = true;
            } else if (nowM - e->tick >= 20000ULL) {
                e->tick = nowM;
                manpuPeriodic = true;
            }
        }
        const bool needManpuCreate = (!manpuPresent || justLeft || manpuPeriodic);
        if (needManpuCreate && !g_paUiNativeOnly) {
            // 只在确实要建时才解析 baseID（按需解析，避免每轮白跑）
            // v1.6.50：同样优先用本机 harvest 到的 baseId（见 far 分支的说明）
            const u64 hbM = PaBaseHarvestGet(machineStatus);
            baseManpu = hbM ? hbM
                : PaResolveBaseIdEx(machineStatus, gid,
                                    g_paCapturedBaseId.load(std::memory_order_relaxed));
            const uintptr_t rM = PaUiCreateGuarded(
                g_paUiManpu, baseManpu, st, gid, PaUiKind::kManpu,
                baseManpu != 0);
            // v1.6.46：manpu 的成败同样只看「游戏有没有把 UIManpu 键改写」。
            u64 mpAfter = 0;
            const bool mpOk =
                PaStatusKeyPresent(machineStatus, "UIManpu", &mpAfter);
            Log("[PA][uifix] manpu gid=%llu base=%llu before=%llu after=%llu "
                "ret=%p -> %s\n",
                static_cast<unsigned long long>(gid),
                static_cast<unsigned long long>(baseManpu),
                static_cast<unsigned long long>(manpuKeyVal),
                static_cast<unsigned long long>(mpAfter),
                reinterpret_cast<void*>(rM),
                (mpOk && mpAfter != manpuKeyVal) ? "键已更新(成功)"
                                                 : "键未变(函数早退)");
        }
    }
    // ============================================================
    // v1.6.51【小 UI 正解：小进度条来自 near 对象，不是 far】
    //   用户实测硬事实：手投的碳窑走远**能看到小进度条**；而那台机器的日志显示
    //   游戏在投料时**只调 near（5 次）、far 0 次**。
    //   ⇒ 小进度条是 near 对象自带的"远距自适应显示"（同一个对象，近了是大面板、
    //     远了是小进度条），far 函数只是历史遗留的批量路径。
    //   PA 自 v1.5.112 "near 交还游戏"→ 从不创建 near 对象 → 走远自然什么都没有。
    //   故：**玩家远离时**补建 near（玩家在旁时游戏自己建，不抢，避免 KB-087
    //   记录的"抢建 near 会让游戏跳过它自己的动画注册流程"）。
    //   冷却沿用 guard（成功 8s / 失败 5→60s 退避），另加 per-gid 20s 节流。
    // ============================================================
    if (nearNow) {
        // 玩家在旁：near 由游戏维护，PA 不碰（保持 v1.5.112 结论）
    } else if (g_paUiNear) {
        u64 paOwn = 0;
        const bool paManaged =
            PaStatusKeyPresent(machineStatus, "pa_own", &paOwn) && paOwn != 0;
        u64 upNow = 0;
        bool nearPresent =
            PaStatusKeyPresent(machineStatus, "UIPros", &upNow) && upNow != 0;
        // v1.6.59【Case B 脏键恢复·near 路】小 UI 实为 near 对象（v1.6.51）。
        //   玩家靠近→游戏建 near→走开→游戏销毁 near 但**未清 UIPros 键**（脏键，与
        //   v1.6.13 的 far 脏键同性质）→ PA 见键非空→永不重建→小 UI 消失不回。
        //   v1.6.58 只在"键空"时建（Case A），覆盖不到"键脏"的 Case B。补偿：开
        //   g_paUiCaseBFix 且玩家走远(!nearNow)且 UIPros 键非空（脏）且本机是 PA 托管
        //   (pa_own=1，避免误伤手投机) 时，清脏键并强制重建（节流 5s/台，幂等于 estVal）。
        bool caseBNear = false;
        if (g_paUiCaseBFix && !nearNow && nearPresent && paManaged) {
            struct PaCaseBN { u64 gid; u64 tick; };
            static PaCaseBN s_cbn[16] = {};
            static size_t s_cbnCur = 0;
            const u64 nowC = GetTickCount64();
            PaCaseBN* e = nullptr;
            for (size_t k = 0; k < 16; ++k)
                if (s_cbn[k].gid == gid) { e = &s_cbn[k]; break; }
            if (!e) { e = &s_cbn[s_cbnCur % 16]; ++s_cbnCur; e->gid = gid; e->tick = 0; }
            u64* est = PaCaseBEstGet(gid, PaUiKind::kNear);
            // 幂等：当前键值==上次建立的键值（对象仍活）→ 不清；键值变化（游戏重新弄脏）才清。
            if (upNow != *est && nowC - e->tick >= 5000ULL) {
                e->tick = nowC;
                PaSetStatusU64(machineStatus, "UIPros", 0);  // 清脏键
                nearPresent = false;  // 反映刚清掉，供下方日志准确
                caseBNear = true;
                s_caseBNearHits.fetch_add(1, std::memory_order_relaxed);  // v1.6.60 纯观测
                PaCaseBObsPublish();
                static int s_cblog = 0;
                if (s_cblog < 30) {
                    ++s_cblog;
                    Log("[PA][caseB] 清脏键 near gid=%llu val=%llu\n",
                        static_cast<unsigned long long>(gid),
                        static_cast<unsigned long long>(upNow));
                }
            }
        }
        // v1.6.58【残留修复·"靠近再走开小 UI 消失"】
        //   小 UI 实际是 near 对象（远离时自适应成小进度条，见 v1.6.51）。原逻辑每 20s
        //   无条件重建一次，但 PA 重建用的 baseId 与游戏手柄写进 UIPros 的 handle 相差
        //   固定 → 重建=同一 handle → 游戏判"值没变"→ 不写键、不 notify → 小 UI 在玩家
        //   靠近再走开被销毁后就再也回不来（全是 no-op 重建）。
        //   改为**键为空才建**（游戏确把 near 对象销毁并清键）→ 重建必产生新 handle →
        //   游戏写键+notify → 小 UI 重现；键非空交游戏自己维护，PA 不搅动。
        bool buildNear = false;
        if (!nearPresent) {
            bool due = false;
            {
                struct PaNearTick { u64 gid; u64 tick; };
                static PaNearTick s_nt[16] = {};
                static size_t s_ntCur = 0;
                const u64 nowN = GetTickCount64();
                PaNearTick* e = nullptr;
                for (size_t k = 0; k < 16; ++k)
                    if (s_nt[k].tick && s_nt[k].gid == gid) { e = &s_nt[k]; break; }
                if (!e) {
                    e = &s_nt[s_ntCur % 16];
                    ++s_ntCur;
                    e->gid = gid;
                    e->tick = nowN;
                    due = true;
                } else if (nowN - e->tick >= 2000ULL) {  // 键空时尽快补（2s 节流，别每 tick 轰）
                    e->tick = nowN;
                    due = true;
                }
            }
            buildNear = due;
        }
        if ((buildNear || caseBNear) && !g_paUiNativeOnly) {
            const u64 hbN = PaBaseHarvestGet(machineStatus, 0);  // near 专用
            const u64 baseNearUi = hbN ? hbN
                : PaResolveBaseIdEx(machineStatus, gid,
                                    g_paBaseIdNear.load(std::memory_order_relaxed));
            const uintptr_t rN = PaUiCreateGuarded(
                g_paUiNear, baseNearUi, st, gid, PaUiKind::kNear,
                baseNearUi != 0);
            u64 afterN = 0;
            const bool afterNOk =
                PaStatusKeyPresent(machineStatus, "UIPros", &afterN);
            if (caseBNear)
                *PaCaseBEstGet(gid, PaUiKind::kNear) = afterNOk ? afterN : 0;
            Log("[PA][nearui] gid=%llu 远离补建 near(脏键恢复) base=%llu present=%d "
                "ret=%p\n",
                static_cast<unsigned long long>(gid),
                static_cast<unsigned long long>(baseNearUi),
                nearPresent ? 1 : 0,
                reinterpret_cast<void*>(rN));
        }
    }

    // v1.6.10-diag：维护链决策留痕（per-gid 30 秒一条）。放在 manpu 之后，
    //   这样 baseManpu 若本轮解析过也能一并看到。
    PaUiDiagLog(gid,
                justLeft ? "just-left"
                         : (refreshFar ? "refresh" : "maintain"),
                nearVal, baseFar, baseManpu, farKey, farVal, rFar);
    g_paUiCallDepth.fetch_sub(1, std::memory_order_relaxed);

}

static bool PaAttemptInput(PaGroup* b, void* machineStatus,
                           void* chestStatuses[], size_t chestCount,
                           void* save, std::int64_t gameSecond) {
    const u64 machineUid =
        b && b->machineCount > 0 ? b->machines[0].uniqueId : 0;
    if (!b || !machineStatus || !chestStatuses || chestCount == 0 ||
        !save) return false;
    // v1.6.61 阶段 0：进到投料入口就计一次，并立即发布，保证 attempts=0
    // 能被读成「主链没被调用」而不是「obs 没数据」。
    s_patxAttempts.fetch_add(1, std::memory_order_relaxed);
    PaTxObsPublish();
    // v1.5.45：时钟无效时禁止投料。旧判断是 gameSecond < 0，而拿不到时钟时
    //   值是 0（不小于 0）→ 照常投料，把 start 写成 0，游戏不认 → 槽被
    //   幻影清理擦掉、箱子材料已被扣 = 净损失。宁可不投也不能白吞材料。
    if (gameSecond <= 0) {
        static std::atomic<int> s_noClock{0};
        if (s_noClock.fetch_add(1, std::memory_order_relaxed) < 5) {
            Log("[PA] input skipped: game clock unavailable "
                "gameSecond=%lld gid=%llu\n",
                static_cast<long long>(gameSecond),
                static_cast<unsigned long long>(machineUid));
        }
        return false;
    }
    if (!g_paItemAllocate || !g_paItemCtor || !g_paItemSetCount ||
        !g_paStatusIntLookup || !g_paStatusU64Lookup ||
        !g_nearbyItemAdjust) {
        Log("[PA] input fail: natives missing machine=%llu\n",
            static_cast<unsigned long long>(machineUid));
        return false;
    }

    // 1. 找空槽
    PaState state = PaState::Idle;
    size_t readySlot = SIZE_MAX;
    bool hasIdle = false;
    if (!PaClassifyMachine(machineStatus, gameSecond, &state, &readySlot,
                           &hasIdle)) {
        Log("[PA] input fail: classify failed machine=%llu\n",
            static_cast<unsigned long long>(machineUid));
        return false;
    }
    if (!hasIdle) return false;  // 全槽忙，正常沉默

    // v1.5.60-diag：原生 push 可行性验证（零副作用）。
    //   push 入口要求：status+0x338 非空 + save+0x33B0(选中索引, int32)
    //   指向 save+0x32C0 数组里一个有物品的槽。这里只临时把索引指向
    //   玩家背包里第一个非空槽、调一次 push、立刻还原索引；不搬物品、
    //   不扣箱子、不改背包。用 al 判断游戏是否接受。
    if (g_paNativePushTest && g_paPushNative) {
        static std::atomic<int> s_pushTries{0};
        if (s_pushTries.fetch_add(1, std::memory_order_relaxed) < 40) {
            unsigned char* saveBytes = reinterpret_cast<unsigned char*>(save);
            std::int32_t* selectIndex =
                reinterpret_cast<std::int32_t*>(saveBytes + 0x33B0);
            void** bagSlots = reinterpret_cast<void**>(saveBytes + 0x32C0);
            if (NearbyIsReadable(selectIndex, sizeof(std::int32_t)) &&
                NearbyIsReadable(bagSlots, 30 * sizeof(void*))) {
                const std::int32_t savedIndex = *selectIndex;
                // v1.5.61：必须挑"这台机器的配方原料"，随便拿第一个物品
                //   push 会因配方不匹配直接拒绝（实测 picked=0 230000 → al=0）。
                u64 npGid = 0;
                PaReadGimmickId(machineStatus, &npGid);
                const ProcessingRecipeMachineIndex* npIdx =
                    ProductionRecipeMachineIndexLookup(npGid);
                u64 allowedIds[32] = {};
                size_t allowedN = 0;
                if (npIdx) {
                    for (u32 r = 0; r < npIdx->recipeCount && r < 128; ++r) {
                        const ProcessingRecipeTableEntry& rec =
                            kProcessingRecipeTable[npIdx->recipeIndices[r]];
                        for (int k = 0; k < 8; ++k) {
                            const u64 id = rec.inputItemIds[k];
                            if (!id) continue;
                            bool dup = false;
                            for (size_t q = 0; q < allowedN; ++q)
                                if (allowedIds[q] == id) { dup = true; break; }
                            if (!dup && allowedN < 32) allowedIds[allowedN++] = id;
                        }
                        if (rec.fuelItemId) {
                            bool dup = false;
                            for (size_t q = 0; q < allowedN; ++q)
                                if (allowedIds[q] == rec.fuelItemId) { dup = true; break; }
                            if (!dup && allowedN < 32)
                                allowedIds[allowedN++] = rec.fuelItemId;
                        }
                    }
                }
                int picked = -1;
                u64 pickedId = 0;
                int pickedCount = 0;
                for (int i = 0; i < 30 && picked < 0; ++i) {
                    if (!bagSlots[i]) continue;
                    NearbyItemInfo bagInfo = {};
                    if (!NearbyReadItem(bagSlots[i], &bagInfo)) continue;
                    for (size_t q = 0; q < allowedN; ++q) {
                        if (bagInfo.itemId == allowedIds[q]) {
                            picked = i;
                            pickedId = bagInfo.itemId;
                            pickedCount = bagInfo.stackCount;
                            break;
                        }
                    }
                }
                if (picked >= 0) {
                    *selectIndex = picked;
                    // v1.5.62：push 第一道门是 [status+0x338] != 0，打印它
                    // v1.5.63：该字段在 push 内只做"非零检查"、后续不再读取
                    //   （0x27BE64 唯一引用），临时置 1 让门通过，调完还原。
                    void* owner338 = nullptr;
                    bool owner338Set = false;
                    unsigned char* owner338Addr =
                        reinterpret_cast<unsigned char*>(machineStatus) + 0x338;
                    if (NearbyIsReadable(owner338Addr, sizeof(void*))) {
                        owner338 = *reinterpret_cast<void**>(owner338Addr);
                        if (!owner338) {
                            *reinterpret_cast<void**>(owner338Addr) =
                                reinterpret_cast<void*>(1);
                            owner338Set = true;
                        }
                    }
                    uintptr_t ret = 0;
                    int faulted = 0;
                    __try {
                        ret = g_paPushNative(
                            reinterpret_cast<uintptr_t>(machineStatus), 0, 0, 0);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        faulted = 1;
                    }
                    // v1.5.85【P1 修正】原实现无条件把 0x338 写回 nullptr ——
                    //   若 push 成功并顺手把该字段填成了**真实组件指针**，这
                    //   一行会把它抹成 nullptr，等于把机器的组件链接打断
                    //   （后续 push/取料全部失效，且该状态会随存档落盘）。
                    //   现在：只在"该字段仍是我们塞进去的 1"时才还原；
                    //   若已被改成别的值，说明游戏写入了真东西，一律保留。
                    if (owner338Set) {
                        int restoreFaulted = 0;
                        __try {
                            void* cur = *reinterpret_cast<void**>(owner338Addr);
                            if (cur == reinterpret_cast<void*>(1)) {
                                *reinterpret_cast<void**>(owner338Addr) = nullptr;
                            } else {
                                Log("[PA] nativepush: 0x338 changed by game "
                                    "(%p), keep it\n", cur);
                            }
                        } __except (EXCEPTION_EXECUTE_HANDLER) {
                            restoreFaulted = 1;
                        }
                        if (restoreFaulted) {
                            Log("[PA] nativepush: 0x338 restore faulted\n");
                        }
                    }
                    *selectIndex = savedIndex;
                    Log("[PA] nativepush test: gid=%llu picked=%d item=%llu "
                        "x%d owner338=%p faked=%d ret=%p al=%d fault=%d\n",
                        static_cast<unsigned long long>(machineUid), picked,
                        static_cast<unsigned long long>(pickedId), pickedCount,
                        owner338, static_cast<int>(owner338Set),
                        reinterpret_cast<void*>(ret),
                        static_cast<int>(ret & 0xFF), faulted);
                    // v1.5.64：push 认可（al!=0）时跳过直写，让 push 成为
                    //   唯一写入者——排除"push 投完又被直写覆盖"的互相踩。
                    if (!faulted && (ret & 0xFF) != 0) {
                        Log("[PA] nativepush ACCEPTED: gid=%llu slot picked=%d "
                            "→ skip direct write\n",
                            static_cast<unsigned long long>(machineUid), picked);
                        return true;
                    }
                } else {
                    Log("[PA] nativepush test: gid=%llu bag empty\n",
                        static_cast<unsigned long long>(machineUid));
                }
            }
        }
    }

    // v1.5.74：makecomp 实验已移除（FN-029 证伪：直接构造会崩游戏且无法
    //   满足前置初始化）。首次点火只能由玩家手动完成。

    size_t slotLimit = 0;
    if (!PaReadSlotLimit(machineStatus, &slotLimit)) {
        Log("[PA] input fail: slotLimit unreadable machine=%llu\n",
            static_cast<unsigned long long>(machineUid));
        return false;
    }
    size_t machineSlot = SIZE_MAX;
    // v1.6.6：backing 仍是函数级变量——第 6 步「backing 插入 clone」要用它。
    //   找空槽时顺带回填；若机器从未分配过物品列表，这里会是 nullptr，
    //   第 6 步之前有专门的防护（不能拿空指针取槽地址）。
    void** backing = nullptr;
    for (size_t slot = 0; slot < slotLimit && slot < PA_MACHINE_SLOTS;
         ++slot) {
        u64 start = 0, duration = 0;
        if (!PaReadSlotTimers(machineStatus, slot, &start, &duration))
            return false;
        if (start != 0 || duration != 0) continue;
        // v1.6.6：与 PaClassifyMachine 用同一判据（backing 指针为空=空槽），
        //   否则 hasIdle 已放行、这里仍会 return false，等于没修。
        bool slotEmpty = false;
        void* backingRaw = nullptr;
        if (!PaSlotBackingEmpty(machineStatus, slot, &slotEmpty, &backingRaw))
            return false;
        if (!slotEmpty) continue;
        machineSlot = slot;
        backing = reinterpret_cast<void**>(backingRaw);
        break;
    }
    if (machineSlot >= PA_MACHINE_SLOTS) {
        Log("[PA] input fail: no empty slot machine=%llu\n",
            static_cast<unsigned long long>(machineUid));
        return false;
    }

    // 2. 配方匹配
    u64 machineGimmickId = 0;
    if (!PaReadGimmickId(machineStatus, &machineGimmickId)) {
        Log("[PA] input fail: gid unreadable machine=%llu\n",
            static_cast<unsigned long long>(machineUid));
        return false;
    }
    PaRecipePlan plan = {};
    if (!PaBuildRecipePlan(chestStatuses, chestCount, machineGimmickId,
                           &plan)) {
        // 节流：同机器 60 秒只打一条 no recipe（防挂机日志刷屏）
        static u64 s_lastNoRecipeTick = 0;
        static u64 s_lastNoRecipeMachine = 0;
        const u64 nowTick = GetTickCount64();
        if (nowTick - s_lastNoRecipeTick >= 60000 ||
            s_lastNoRecipeMachine != machineUid) {
            s_lastNoRecipeTick = nowTick;
            s_lastNoRecipeMachine = machineUid;
            const u64 failFuel =
                g_paPlanFailFuelId.load(std::memory_order_relaxed);
            if (failFuel) {
                // v1.5.113：主料够、只缺燃料——高炉最常见的"弹窗但不加工"就是这种
                const char* fuelHint =
                    (failFuel == 211000ULL) ? "木炭"
                  : (failFuel == 230100ULL) ? "煤炭" : "未知燃料";
                Log("[PA] input fail: 缺燃料 %s(%llu) machine=%llu gid=%llu "
                    "(主料够，箱子组里没有该燃料)\n",
                    fuelHint, static_cast<unsigned long long>(failFuel),
                    static_cast<unsigned long long>(machineUid),
                    static_cast<unsigned long long>(machineGimmickId));
            } else {
                Log("[PA] input fail: no recipe (chests lack ingredients?) machine=%llu gid=%llu\n",
                    static_cast<unsigned long long>(machineUid),
                    static_cast<unsigned long long>(machineGimmickId));
            }
            PaLogInputDiag(machineStatus, chestStatuses, chestCount,
                           machineGimmickId);
        }
        return false;
    }

    // 3. 物化原料 clone（旧版 L4324-4359）
    void* clones[8] = {};
    const size_t cloneCount = plan.ingredientCount;
    // 防御（v1.5.9）：清理已分配 clone 的局部 lambda——中途任一失败必须
    // 释放前面已物化的 clone，否则引用泄漏（对象悬空 → 拆炉/开面板闪退）。
    auto PaReleaseClones = [&](size_t upto) {
        for (size_t k = 0; k < upto; ++k) {
            if (clones[k]) NearbyReleaseOwnedReference(clones[k]);
        }
    };
    for (size_t i = 0; i < cloneCount; ++i) {
        NearbyItemInfo info = {};
        if (!NearbyReadItem(plan.sourceItems[i], &info)) {
            Log("[PA] input fail: source item unreadable i=%zu machine=%llu\n",
                i, static_cast<unsigned long long>(machineUid));
            PaReleaseClones(i);
            return false;
        }
        void* memory = PaSafeItemAllocate(g_paItemAllocate, 0x2c0);
        if (!memory || !NearbyIsReadable(memory, 0x2c0)) {
            Log("[PA] input fail: allocate failed machine=%llu\n",
                static_cast<unsigned long long>(machineUid));
            PaReleaseClones(i);
            return false;
        }
        memset(memory, 0, 0x2c0);
        void* item = PaSafeItemCtor(g_paItemCtor, memory, save,
                                    info.itemId);
        if (!item || item != memory) {
            Log("[PA] input fail: ctor failed item=%llu machine=%llu\n",
                static_cast<unsigned long long>(info.itemId),
                static_cast<unsigned long long>(machineUid));
            PaReleaseClones(i);
            return false;
        }
        NearbyAddReference(item);  // 事务持有
        int rank = info.rank;
        if (PaForcesRankZero(info.itemId)) rank = 0;
        *reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(item) +
            NEARBY_ITEM_RANK_OFFSET) = rank;
        PaSafeItemSetCount(g_paItemSetCount, item,
                           plan.sourceAmounts[i]);
        NearbyItemInfo verify = {};
        if (!NearbyReadItem(item, &verify) ||
            verify.itemId != info.itemId ||
            verify.stackCount != plan.sourceAmounts[i]) {
            Log("[PA] input fail: clone verify failed item=%llu machine=%llu\n",
                static_cast<unsigned long long>(info.itemId),
                static_cast<unsigned long long>(machineUid));
            NearbyReleaseOwnedReference(item);
            PaReleaseClones(i);
            return false;
        }
        clones[i] = item;
    }

    // 4. 写 createTime{slot}=duration*60（CAS 0→值，旧版 L5281）
    u64 duration = plan.recipe->duration * 60ULL;
    if (g_paFastTest) duration = 400;  // v1.5.69-diag：快速验证（400 秒，投料+取料全链路）
    // v1.5.71：组件已点火（玩家投过至少一次）→ 走原生 push。
    //   直写虽然能改 status，但游戏加工组件不认；push 才是游戏自己的
    //   投料入口，能把"数据 + 组件"一次做全。
    // v1.5.78【关键】pushfeed 开启时禁止回退直写：
    //   未点火机器（status+0x338==0）直写只会得到"timer 挂着但游戏侧没有
    //   物品"的幻影槽 —— 箱子材料已被扣，跨天/每日处理时游戏把这些无效槽
    //   清掉，玩家看到的就是"夜间慢速材料消失"。现在直接跳过（不扣料），
    //   提示玩家手动投料一次点火。push 失败同样不再回退直写。
    if (g_paUsePushFeed) {
        unsigned char* compBytes =
            reinterpret_cast<unsigned char*>(machineStatus);
        void* comp = NearbyIsReadable(compBytes + 0x338, sizeof(void*))
                         ? *reinterpret_cast<void**>(compBytes + 0x338)
                         : nullptr;
        if (!comp) {
            // v1.5.90【启动宽限期】游戏加载存档后需 30~45 秒才批量建立加工
            //   组件（实证见 PA_BOOT_GRACE_MS 定义处）。此窗口内 comp 恒 0，
            //   属引擎延迟初始化、玩家无需任何操作 —— 旧文案"需玩家手动投料
            //   一次"在该窗口内是误导。故宽限期内静默等待，超期才提示。
            static ULONGLONG s_paBootAt = 0;
            if (s_paBootAt == 0) s_paBootAt = GetTickCount64();
            const bool bootGrace =
                (GetTickCount64() - s_paBootAt) < PA_BOOT_GRACE_MS;
            if (!bootGrace) {
                static std::atomic<int> s_notIgnited{0};
                if (s_notIgnited.fetch_add(1, std::memory_order_relaxed) < 20) {
                    Log("[PA] input skipped: not ignited (no component) gid=%llu "
                        "recipe=%llu — 需玩家手动投料一次\n",
                        static_cast<unsigned long long>(machineGimmickId),
                        static_cast<unsigned long long>(plan.recipe->recipeId));
                }
            }
            // v1.6.4【ignite-probe】死锁解除尝试（见 PaPushFeed 处注释）：
            //   高炉"缺煤炭/木炭"的本质是循环依赖 —— 无燃料→不点火→无组件→
            //   PA 不敢投燃料。这里放行**最多 6 次**探测 push，让引擎自己决定
            //   是否建组件：成功 → 死锁解除；被拒 → 物品原样退回，预算耗尽后
            //   恢复旧的静默跳过行为（不会无限刷日志/反复试探）。
            static std::atomic<int> s_igniteProbe{0};
            const int probeNo =
                s_igniteProbe.fetch_add(1, std::memory_order_relaxed);
            if (probeNo < 6) {
                Log("[PA] ignite-probe #%d: gid=%llu recipe=%llu comp=NULL，"
                    "尝试原生 push 让引擎自建组件（被拒则原样退回）\n",
                    probeNo + 1,
                    static_cast<unsigned long long>(machineGimmickId),
                    static_cast<unsigned long long>(plan.recipe->recipeId));
            } else {
                PaReleaseClones(cloneCount);
                return false;
            }
        }
        if (cloneCount > 0) {
            // v1.6.x【多料机器修复】双料/多料机器（高炉=矿石+燃料）原生 push 的
            //   auto-select 分支（反编译 FUN_140284670 @ RVA 0x284670 实锤）要求
            //   "所有原料同时 staged 在 save+0x32C0 背包槽" 才会建立机器配方上下文。
            //   PA 旧逻辑每次只推 1 种料 → 永远凑不齐配方 → 物品原样退回 → 死锁。
            //   现改为：多料时一次性全部 staged，轮流以每种料作 selectIndex 触发
            //   引擎 auto-select 整体转入。单料仍走下方原 PaPushFeed（已验证炭窑）。
            if (cloneCount > 1) {
                for (size_t i = 0; i < cloneCount; ++i)
                    PaSafeItemAdjust(g_nearbyItemAdjust, plan.sourceItems[i], -1);
                const bool pushed = PaPushFeedAll(save, machineStatus, clones,
                                                 cloneCount, machineGimmickId);
                if (pushed) {
                    s_patxSuccess.fetch_add(1, std::memory_order_relaxed);
                    PaTxObsPublish();
                    Log("[PA] input OK (native push-all): %s gid=%llu recipe=%llu "
                        "inputs=%zu\n",
                        PaMachineLabel(machineGimmickId),
                        static_cast<unsigned long long>(machineGimmickId),
                        static_cast<unsigned long long>(plan.recipe->recipeId),
                        cloneCount);
                    PaCreateProcessingUi(machineStatus, machineGimmickId, true);
                    PaSetStatusU64(machineStatus, "pa_own", 1);
                    if (!comp) {
                        void* comp2 =
                            NearbyIsReadable(compBytes + 0x338, sizeof(void*))
                                ? *reinterpret_cast<void**>(compBytes + 0x338)
                                : nullptr;
                        Log("[PA] ignite-probe result: push-all comp %p -> %p %s\n",
                            comp, comp2,
                            comp2 ? "（引擎已建组件 → 未点火死锁解除）"
                                  : "（仍无组件）");
                    }
                    return true;
                }
                for (size_t i = 0; i < cloneCount; ++i)
                    PaSafeItemAdjust(g_nearbyItemAdjust, plan.sourceItems[i], +1);
                PaReleaseClones(cloneCount);
                return false;
            }
            // v1.6.9【先投燃料】高炉/熔炉类是「主料 + 燃料」双材料，而旧代码
            //   永远只 push clones[0]（主料），燃料从头到尾没被 push 过。
            //   实测证据（2026-10-29 16:03:07，同一 push 函数、同一背包槽）：
            //     · 炭窑 240100000（单材料）→ 物品被拿走 → 成功
            //     · 高炉 240090100（主料+燃料）→ 物品留在背包 → 被拒
            //   反编译 FUN_140284670 显示它对每个候选物品都要查配方表，
            //   机器里没有燃料时投矿石会被判不匹配而直接退回。
            //   所以这里先读机器 3 个槽的 itemID%d：机器内还没有燃料 →
            //   本次先推燃料；已有燃料 → 照旧推主料。
            //   （物品读不到/键缺失一律按"没有燃料"处理：push 若被拒会原样
            //     退回并 +1 补回箱子，不会真丢料。）
            size_t pushIdx = 0;
            const u64 fuelId = plan.recipe ? plan.recipe->fuelItemId : 0ULL;
            if (fuelId != 0 && cloneCount > 1) {
                bool machineHasFuel = false;
                for (size_t s = 0; s < PA_MACHINE_SLOTS; ++s) {
                    char idKey[16] = {};
                    _snprintf_s(idKey, sizeof(idKey), _TRUNCATE, "itemID%zu",
                                s);
                    u64 slotId = 0;
                    if (PaReadStatusU64(machineStatus, idKey, &slotId) &&
                        slotId == fuelId) {
                        machineHasFuel = true;
                        break;
                    }
                }
                if (!machineHasFuel) {
                    for (size_t k = 0; k < cloneCount; ++k) {
                        NearbyItemInfo ii = {};
                        if (NearbyReadItem(clones[k], &ii) &&
                            ii.itemId == fuelId) {
                            pushIdx = k;
                            break;
                        }
                    }
                }
            }
            PaSafeItemAdjust(g_nearbyItemAdjust, plan.sourceItems[pushIdx], -1);
            const bool pushed = PaPushFeed(save, machineStatus, clones[pushIdx],
                                           machineGimmickId);
            if (pushed) {
                s_patxSuccess.fetch_add(1, std::memory_order_relaxed);
                PaTxObsPublish();
                PaReleaseClones(cloneCount);
                Log("[PA] input OK (native push): gid=%llu recipe=%llu "
                    "pushIdx=%zu fuelFirst=%d\n",
                    static_cast<unsigned long long>(machineGimmickId),
                    static_cast<unsigned long long>(plan.recipe->recipeId),
                    pushIdx, pushIdx != 0 ? 1 : 0);
                // v1.5.85：push 只完成"投料逻辑"，UI 三件事（近处面板 /
                //   远处小进度条 / 加工动画）由游戏脚本另外三条指令负责。
                //   自动投料没有玩家交互，脚本不会跑 → 这里主动补齐。
                PaCreateProcessingUi(machineStatus, machineGimmickId, true);
                // v1.5.90【恢复 PA↔SG 协调标记】SG 的 SrIsPaManaged
                //   (save_repair.inl:381) 读 status+0x58 的 pa_own 键，用于整台
                //   跳过 PA 托管机器、避免误清 PA 功能槽。该标记原只在旧
                //   production_automation.inl 的直写路径写，而当前构建只编
                //   v2.inl（cpp:1020，旧 .inl 已在 cpp:1022 注释掉）→ 标记恒缺
                //   → SG 的跳过形同虚设。故在 push 成功（PA 已接管此机器）处
                //   置 1；加工周期结束复位 inProc 处清 0（见下方 anyRunning）。
                PaSetStatusU64(machineStatus, "pa_own", 1);
                if (!comp) {
                    void* comp2 =
                        NearbyIsReadable(compBytes + 0x338, sizeof(void*))
                            ? *reinterpret_cast<void**>(compBytes + 0x338)
                            : nullptr;
                    Log("[PA] ignite-probe result: comp %p -> %p %s\n", comp,
                        comp2,
                        comp2 ? "（引擎已建组件 → 未点火死锁解除）"
                              : "（仍无组件：push 只写了数据，需玩家点火一次）");
                }
                return true;
            }
            PaSafeItemAdjust(g_nearbyItemAdjust, plan.sourceItems[pushIdx], +1);
            PaReleaseClones(cloneCount);
            return false;   // v1.5.78：push 失败不回退直写（防幻影槽吞料）
        }
        PaReleaseClones(cloneCount);
        return false;
    }

    // ============================================================
    // 2026-11-07 23:25【旧路线已归档】原 L3502-3760（259 行「直写机器状态」
    //   投料实现）已整体移至：弃用MOD源码/PA_旧直写投料路线_20261107/
    //   含逐行原文 + 改前完整 .inl 副本，可原样恢复。
    // 归档原因：该块入口就有 `if (!g_paUsePushFeed) return false`，而到达这里
    //   必然是 pushfeed=0；阶段 0 把开关收敛为默认启用后更是恒不可达。留着
    //   只会让扫描/分析把死代码当成活跃实现（与 F3 假撞车同类问题）。
    // 唯一生效路径：上方原生 push；失败即返回，绝不回退直写。
    // ============================================================
    return false;
}

// ============================================================
// 槽 4 结算器：取产出（outputHelper + 清槽收尾 + chestCommit）
// 旧版 L3464-3499 + FinalizeClearedMachineSlot
// ============================================================
static bool PaTransferOutput(PaGroup* b, void* machineStatus,
                             size_t readySlot, void* chestStatuses[],
                             size_t chestCount) {
    if (!b || !machineStatus || !chestStatuses || chestCount == 0 ||
        readySlot >= PA_MACHINE_SLOTS) return false;
    if (!g_paOutputHelper || !g_paStatusIntLookup ||
        !g_paStatusU64Lookup) return false;

    // v1.5.32：旧版这里打的是"组内第一台机器"的 uid → 日志把别的机器
    //   的取料记到蚕盒头上，误导排查。改为读取本机 gid。
    u64 transferGid = 0;
    PaReadGimmickId(machineStatus, &transferGid);
    Log("[PA] transfer begin: gid=%llu slot=%zu\n",
        static_cast<unsigned long long>(transferGid), readySlot);

    // v1.6.3【fail-open 化】原第 61-65 行的崩溃防护在 v1.20 之后已失去依据：
    //   · 该防护针对「outputHelper 内部 memcpy 的 dst 来自全局 [exe+0x3150768]，
    //     全局 NULL → memcpy(NULL) 崩溃」；
    //   · build 25640656 实扫 outputHelper（RVA 0x173290, 1882B）全部 rip-rel 引用，
    //     只有 0x3170EF0 / 0x3170EF8 / 0x3170EE8（.data）与 GAME_ROOT，
    //     **没有任何一处引用 0x3150768**——即该 memcpy-全局-dst 路径已不存在；
    //   · 而 0x3150768 落在 .data 的 bss 区（.data vsize 0x21889A0，虚拟地址可达
    //     0x31C59A0），恒为 0，于是这个"防护"变成永久拦截：PA 每台机器都
    //     `transfer skipped: output target global NULL`，一台都投不出去。
    // 处理：改为 fail-open —— 只做一次性诊断记录，然后继续。若真出现空崩，
    //   SaveGuard 的空崩点硬化 + QoL VEH 留痕会直接给出证据。
    // exeBase 在该函数后半段（收尾三件套）仍要用，保留声明
    const uintptr_t exeBase =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    {
        static bool s_outputTargetDiagDone = false;
        if (!s_outputTargetDiagDone) {
            s_outputTargetDiagDone = true;
            const uintptr_t eb = exeBase;
            void* v = nullptr;
            if (PA_OUTPUT_TARGET_GLOBAL_RVA &&
                NearbyIsReadable(reinterpret_cast<void*>(
                                     eb + PA_OUTPUT_TARGET_GLOBAL_RVA),
                                 sizeof(void*))) {
                v = *reinterpret_cast<void**>(eb + PA_OUTPUT_TARGET_GLOBAL_RVA);
            }
            Log("[PA] output-target guard fail-open: rva=0x%llX value=%p "
                "(旧版防护前提已失效，继续执行)\n",
                (unsigned long long)PA_OUTPUT_TARGET_GLOBAL_RVA, v);
        }
    }

    // 收尾审查①：pop 前先读预期产出（itemID{slot}/itemVal{slot} 从机器 map）
    char idKey[8] = {};
    char countKey[9] = {};
    _snprintf_s(idKey, sizeof(idKey), _TRUNCATE, "itemID%zu", readySlot);
    _snprintf_s(countKey, sizeof(countKey), _TRUNCATE, "itemVal%zu",
                readySlot);
    u64 expectedItemId = 0;
    int expectedCount = 0;
    PaReadStatusU64(machineStatus, idKey, &expectedItemId);
    PaReadStatusInt(machineStatus, countKey, &expectedCount);

    // 防御（v1.5.8 玩家反馈"肥料桶取料崩溃"）：槽无实际产出（itemID==0）
    // 时绝不调用 outputHelper——状态机可能误判 Ready（timer 到期但槽空），
    // outputHelper 在无效槽上内部崩溃。安全跳过，等下一轮扫描。
    if (expectedItemId == 0) {
        Log("[PA] transfer skipped: slot %zu has no output (itemID=0)\n",
            readySlot);
        return false;
    }

    // 收口②：pop 前只读预检箱子组——必须有空槽或可 merge 的同 id 堆，
    // 否则不 pop（防物品丢失：pop 后无处安放 = 凭空消失）
    bool capacityOk = false;
    {
        // 若有已读 itemId 则按 id 找同堆；否则按任何堆
        for (size_t c = 0; c < chestCount && !capacityOk; ++c) {
            NearbyRawPointerVector inv = {};
            if (!NearbyReadRawInventoryRelaxed(chestStatuses[c], &inv))
                continue;
            const size_t cnt =
                (reinterpret_cast<uintptr_t>(inv.end) -
                 reinterpret_cast<uintptr_t>(inv.begin)) / sizeof(void*);
            for (size_t s = 0; s < cnt; ++s) {
                void* t = inv.begin[s];
                if (!t) {
                    capacityOk = true;  // 有空槽
                    break;
                }
                if (!expectedItemId) continue;
                NearbyItemInfo ti = {};
                if (NearbyReadItem(t, &ti) &&
                    ti.itemId == expectedItemId) {
                    capacityOk = true;  // 有可 merge 的同 id 堆（rank 合并时再判）
                    break;
                }
            }
        }
    }
    if (!capacityOk) {
        Log("[PA] transfer: no chest capacity (pop skipped, item preserved)\n");
        return false;  // 不 pop，物品留在机器
    }

    // outputHelper pop（内部清 timeValue/createTime 槽字段）
    // v1.5.29：被动机器（timer 恒 0）调 outputHelper 会返回 null（实测日志
    //   `outputHelper returned null` 刷屏），改为自建物品：读 itemID/itemVal
    //   → allocate+ctor+setCount → 清键，再走原有入箱流程。
    u64 selfGid = 0;
    PaReadGimmickId(machineStatus, &selfGid);
    const bool passiveMachine = PaIsPassiveMachine(selfGid);

    void* item = nullptr;
    if (passiveMachine) {
        if (expectedItemId == 0 || expectedCount <= 0) {
            Log("[PA] transfer: passive slot %zu no output key\n", readySlot);
            return false;
        }
        void* memory = PaSafeItemAllocate(g_paItemAllocate, 0x2c0);
        if (!memory || !NearbyIsReadable(memory, 0x2c0)) {
            Log("[PA] transfer: passive allocate failed slot=%zu\n", readySlot);
            return false;
        }
        memset(memory, 0, 0x2c0);
        item = PaSafeItemCtor(g_paItemCtor, memory, g_paCurrentSave,
                              static_cast<u64>(expectedItemId));
        if (!item || item != memory) {
            Log("[PA] transfer: passive ctor failed item=%llu\n",
                static_cast<unsigned long long>(expectedItemId));
            return false;
        }
        NearbyAddReference(item);
        PaSafeItemSetCount(g_paItemSetCount, item, expectedCount);
        Log("[PA] transfer: passive popped item=%p slot=%zu item=%llu x%d\n",
            item, readySlot,
            static_cast<unsigned long long>(expectedItemId), expectedCount);
    } else {
        item = PaSafeOutputHelper(g_paOutputHelper, machineStatus,
                                  static_cast<int>(readySlot));
        if (!item) {
            Log("[PA] transfer: outputHelper returned null slot=%zu\n",
                readySlot);
            return false;
        }
        Log("[PA] transfer: popped item=%p slot=%zu\n", item, readySlot);
    }

    // 收口③：pop 后物品验证（itemId 与预期一致，旧版 L3517 三重验证）
    NearbyItemInfo popInfo = {};
    if (!NearbyReadItem(item, &popInfo)) {
        Log("[PA] transfer: popped item unreadable\n");
        NearbyReleaseOwnedReference(item);  // 防御：pop 后已脱离炉子，须释放引用
        return false;
    }
    if (expectedItemId != 0 && popInfo.itemId != expectedItemId) {
        Log("[PA] transfer: item mismatch expected=%llu got=%llu\n",
            static_cast<unsigned long long>(expectedItemId),
            static_cast<unsigned long long>(popInfo.itemId));
        NearbyReleaseOwnedReference(item);  // 防御：pop 后已脱离炉子，须释放
        return false;
    }
    if (expectedCount > 0 && popInfo.stackCount != expectedCount) {
        Log("[PA] transfer: count mismatch expected=%d got=%d\n",
            expectedCount, popInfo.stackCount);
        NearbyReleaseOwnedReference(item);  // 防御：同上
        return false;
    }

    // 清槽收尾：itemID{slot}=0、itemVal{slot}=0（旧版 FinalizeClearedSlot）
    PaSetStatusU64(machineStatus, idKey, 0);
    PaSetStatusInt(machineStatus, countKey, 0);

    // 全槽空则复位 inProc
    bool anyRunning = false;
    size_t slotLimit = 0;
    if (PaReadSlotLimit(machineStatus, &slotLimit)) {
        for (size_t slot = 0; slot < slotLimit && slot < PA_MACHINE_SLOTS;
             ++slot) {
            u64 start = 0, duration = 0;
            if (PaReadSlotTimers(machineStatus, slot, &start, &duration))
                if (start != 0 && duration != 0) { anyRunning = true; break; }
        }
    }
    if (!anyRunning) {
        PaSetStatusInt(machineStatus, "inProc", 0);
        // v1.5.89：本加工周期结束，清动画重建标记，下轮 inProc 重建新鲜动画对象
        PaSetStatusU64(machineStatus, "UIManpu2", 0);
        // v1.5.90：加工周期结束 → 释放 PA↔SG 协调标记，让 SG 恢复正常巡检该机器
        //   （仅加工期间 PA 托管，空闲期交还游戏/SG，避免长期误跳过）。
        PaSetStatusU64(machineStatus, "pa_own", 0);
    }

    // 产出入箱（旧版 L3630-3697 验证方式：直接写箱子 items 数组空槽，
    // CAS + 引用计数。chestCommit 从未被旧版调用，签名不可靠）
    NearbyItemInfo info = {};
    u64 itemId = 0;
    int rank = 0;
    if (NearbyReadItem(item, &info)) {
        itemId = info.itemId;
        rank = info.rank;
    }
    // v1.5.83：闭环汇总行——一次启动即可同时判定两件事：
    //   ① 取料闭环：gid/slot → 弹出物品 → 入箱（chests 数量），链路完整成环；
    //   ② HUD 显示一致性：hud=itemID/itemVal（面板显示值）vs actual=游戏真实
    //      弹出物品的 id/数量。两者相同 = HUD 与实际一致；不同则上方
    //      "item mismatch"/"count mismatch" 已拦下并留证。
    Log("[PA] LOOP gid=%llu slot=%zu hud=%llu x%d actual=%llu x%d chests=%zu\n",
        static_cast<unsigned long long>(selfGid), readySlot,
        static_cast<unsigned long long>(expectedItemId), expectedCount,
        static_cast<unsigned long long>(itemId), info.stackCount, chestCount);

    // 入箱：优先 merge 到组内同 id/rank 已有堆叠（旧版 L2784 语义：
    // distribute over same-id/rank stack, remainder to empty slot）
    // 堆叠上限 999（旧版注释）
    int remaining = info.stackCount > 0 ? info.stackCount : 1;
    void* mergedTarget = nullptr;
    void* mergedChest = nullptr;
    for (size_t c = 0; c < chestCount && remaining > 0; ++c) {
        NearbyRawPointerVector inv = {};
        if (!NearbyReadRawInventoryRelaxed(chestStatuses[c], &inv)) continue;
        const size_t cnt =
            (reinterpret_cast<uintptr_t>(inv.end) -
             reinterpret_cast<uintptr_t>(inv.begin)) / sizeof(void*);
        for (size_t s = 0; s < cnt && remaining > 0; ++s) {
            void* t = inv.begin[s];
            if (!t) continue;
            NearbyItemInfo ti = {};
            if (!NearbyReadItem(t, &ti) || ti.itemId != itemId ||
                ti.rank != rank) continue;
            // 容量上限用 commandCapacity（AutoHarvest L959-964 同款，替代硬编码 999）
            int capLimit = 999;
            if (g_nearbyCommandCapacity) {
                NearbyCommandItem descriptor = {};
                descriptor.item = t;
                const int remainingCap = g_nearbyCommandCapacity(&descriptor);
                if (remainingCap <= 0) continue;  // 无容量
                capLimit = ti.stackCount + remainingCap;
            }
            if (ti.stackCount >= capLimit) continue;  // 堆满
            const int cap = capLimit - ti.stackCount;
            const int move = cap >= remaining ? remaining : cap;
            PaSafeItemAdjust(g_nearbyItemAdjust, t, move);
            remaining -= move;
            mergedTarget = t;
            mergedChest = chestStatuses[c];
        }
    }
    if (remaining > 0) {
        // merge 后仍有剩余 → 找箱子组内第一个空槽位
        void** freeSlot = nullptr;
        size_t targetChest = SIZE_MAX;
        for (size_t c = 0; c < chestCount && !freeSlot; ++c) {
            NearbyRawPointerVector inventory = {};
            if (!NearbyReadRawInventoryRelaxed(chestStatuses[c], &inventory))
                continue;
            const size_t cnt =
                (reinterpret_cast<uintptr_t>(inventory.end) -
                 reinterpret_cast<uintptr_t>(inventory.begin)) / sizeof(void*);
            for (size_t s = 0; s < cnt; ++s) {
                if (inventory.begin[s] == nullptr) {
                    freeSlot = &inventory.begin[s];
                    targetChest = c;
                    break;
                }
            }
        }
        if (!freeSlot) {
            Log("[PA] transfer: all chests full (%zu chests), releasing item %p\n",
                chestCount, item);
            // 防御（v1.5.9）：物品已从炉子弹出但无箱可入 → 必须释放 PA 临时
            // 引用，否则悬空对象 + 引用泄漏 → 拆炉/开面板闪退（玩家实锤，
            // 删 MOD 依旧）。释放后物品引用归零，游戏可正常回收。
            NearbyReleaseOwnedReference(item);
            return false;
        }
        // 写入空槽：先把弹出对象数量调整为剩余部分（部分 merge 时，
        // 已并入箱子堆的数量不能重复计入弹出对象）
        if (info.stackCount != remaining) {
            PaSafeItemAdjust(g_nearbyItemAdjust, item,
                             remaining - info.stackCount);
        }
        // 诊断：打印目标箱子 uid + gimmickId（确认写入哪个箱子）
        PaStableId cid = {};
        u64 cgid = 0;
        if (PaReadIdentity(chestStatuses[targetChest], &cid) &&
            PaReadGimmickId(chestStatuses[targetChest], &cgid))
            Log("[PA] transfer: target chest uid=%llu gid=%llu\n",
                static_cast<unsigned long long>(cid.uniqueId),
                static_cast<unsigned long long>(cgid));
        NearbyAddReference(item);  // 箱子槽所有权
        void* observed = InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(freeSlot), item, nullptr);
        if (observed != nullptr) {
            NearbyReleaseOwnedReference(item);  // 撤销 AddRef 槽所有权
            // 防御（v1.5.9）：槽被占（竞态），物品未入箱。outputHelper 弹出
            // 的临时引用也必须释放，否则悬空对象 + 引用泄漏 → 拆炉/开面板闪退。
            NearbyReleaseOwnedReference(item);
            Log("[PA] transfer: chest slot race, released item %p\n", item);
            return false;
        }
        NearbyReleaseOwnedReference(item);  // 事务/临时引用释放（槽已持有）
        Log("[PA] transfer: committed to chest slot idx=%zu (remainder=%d)\n",
            targetChest, remaining);
    } else {
        // 全部 merge 进已有堆：弹出对象不再入槽，释放调用方引用
        NearbyReleaseOwnedReference(item);
        Log("[PA] transfer: merged to existing stack (count=%d)\n",
            info.stackCount);
    }

    // 收尾三件套（AutoHarvest L1013-1025 已验证：让游戏感知箱子变化，
    // 否则箱子 UI/存档/逻辑都看不到新物品 = "物品不见了"）
    void* root = nullptr;
    void* player = nullptr;
    if (PaReadPtr(reinterpret_cast<void*>(exeBase + RVA_GAME_ROOT), 0, &root) &&
        root && PaReadPtr(root, NEARBY_ROOT_PLAYER_OFFSET, &player) &&
        player) {
        if (g_nearbyBatchRecalculate) {
            __try { g_nearbyBatchRecalculate(player, false); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (g_nearbyBatchUiRefresh) {
            __try { g_nearbyBatchUiRefresh(player, true); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (g_nearbyBatchDirty) {
            __try { g_nearbyBatchDirty(player, 0x2bc); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        Log("[PA] transfer: batch refresh done\n");
    } else {
        Log("[PA] transfer: player unreadable, batch refresh skipped\n");
    }

    Log("[PA] output OK: machine=%llu slot=%zu item=%llu -> chests=%zu\n",
        static_cast<unsigned long long>(
            b->machineCount > 0 ? b->machines[0].uniqueId : 0),
        readySlot,
        static_cast<unsigned long long>(itemId), chestCount);
    return true;
}

// ============================================================
// 世界上下文（从 GAME_ROOT 链读取）
// ============================================================
struct PaWorldContext {
    void* save;
    std::int64_t gameSecond;
};

// v1.6.44：读玩家位置（读不到就置 invalid，退回旧行为）
//   定义/判定函数在文件前部（PaReadPtr 之后）；这里只做每 tick 刷新。
static bool PaGetWorldContext(PaWorldContext* ctx) {
    if (!ctx) return false;
    ctx->save = nullptr;
    ctx->gameSecond = 0;

    const uintptr_t base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) return false;

    void* root = nullptr;
    if (!PaReadPtr(reinterpret_cast<void*>(base + RVA_GAME_ROOT), 0, &root) ||
        !root) return false;
    void* save = nullptr;
    if (!PaReadPtr(root, PA_SAVE_OFFSET, &save) || !save) return false;
    ctx->save = save;

    // v1.6.44：读玩家位置（读不到就置 invalid，退回旧行为）
    {
        void* playerObj = nullptr;
        float px = 0.f, py = 0.f, pz = 0.f;
        if (PaReadPtr(root, 0x208, &playerObj) && playerObj) {
            void* playerStatus = nullptr;
            if (PaReadPtr(playerObj, 0x32b8, &playerStatus) && playerStatus &&
                PaReadFloat3(playerStatus, PA_POSITION_OFFSET, &px, &py, &pz)) {
                g_paPlayerX = px;
                g_paPlayerY = py;
                g_paPlayerValid = 1;
            } else {
                g_paPlayerValid = 0;
            }
        } else {
            g_paPlayerValid = 0;
        }
    }

    if (g_productionMainWorldGameClockSourceReady.load(
            std::memory_order_acquire)) {
        // epoch 校验（旧版 L462-489 语义）：WorldEpoch 必须与 schedule epoch
        // 一致，LoadGeneration 必须已就绪，否则 gameSecond 是过期值
        const u64 worldEpoch = g_productionWorldScheduleEpoch.load(
            std::memory_order_relaxed);
        const u64 pubEpoch = g_productionMainWorldGameClockWorldEpoch.load(
            std::memory_order_relaxed);
        const u64 loadGen = g_productionMainWorldGameClockLoadGeneration.load(
            std::memory_order_relaxed);
        const u64 readyGen = g_autoPetLoadReadyGeneration.load(
            std::memory_order_relaxed);
        if (worldEpoch == pubEpoch &&
            loadGen != UINT64_MAX && loadGen == readyGen) {
            ctx->gameSecond = g_productionMainWorldGameSecond.load(
                std::memory_order_relaxed);
        }
    }

    // v1.5.45 主路径：直接从 save 读当前累计秒（v1 旧版 L8194 已验证）。
    //   发布路径依赖 AutoPet 的 loadGeneration 就绪标记，AutoPet 未加载/
    //   尚未完成加载时恒不满足 → gameSecond 一直是 0。直读 save 不依赖任何
    //   跨 MOD 状态，是更可靠的来源。
    std::int64_t directSecond = 0;
    unsigned char* const saveBytes = reinterpret_cast<unsigned char*>(save);
    if (NearbyIsReadable(saveBytes + PA_SAVE_GAME_SECOND,
                         sizeof(std::int64_t))) {
        directSecond = *reinterpret_cast<const std::int64_t*>(
            saveBytes + PA_SAVE_GAME_SECOND);
    }
    if (directSecond > 0) {
        ctx->gameSecond = directSecond;
    } else if (ctx->gameSecond > 0) {
        // 直读失败时才退回发布值（保持旧行为）
    } else {
        static std::atomic<int> s_clockDiag{0};
        if (s_clockDiag.fetch_add(1, std::memory_order_relaxed) < 5) {
            Log("[PA] clock unavailable: direct=%lld published=%lld "
                "ready=%d seq=%llu pubEpoch=%llu worldEpoch=%llu "
                "loadGen=%llu readyGen=%llu\n",
                static_cast<long long>(directSecond),
                static_cast<long long>(ctx->gameSecond),
                static_cast<int>(g_productionMainWorldGameClockSourceReady.load(
                    std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    g_productionMainWorldGameClockSequence.load(
                        std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    g_productionMainWorldGameClockWorldEpoch.load(
                        std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    g_productionWorldScheduleEpoch.load(
                        std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    g_productionMainWorldGameClockLoadGeneration.load(
                        std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    g_autoPetLoadReadyGeneration.load(
                        std::memory_order_relaxed)));
        }
    }
    return true;
}

// ============================================================
// 槽 2 枚举器：遍历 gimmickList_ 收集机器和箱子
// ============================================================
struct PaGimmickEntry {
    PaStableId id;
    void* status;
    float x, y, z;
    u64 gimmickId;
    bool isMachine;
    bool isChest;
};

static constexpr size_t PA_MAX_ENTRIES = 1024;
static PaGimmickEntry g_entries[PA_MAX_ENTRIES];

// ============================================================
// v1.6.7【燃料借用】组内箱子可能有主料却没有燃料
//   背景（2026-10-29 实测）：高炉 240090100 与面粉机/炭窑同组，
//   groupdiag 显示该组只有 1 个箱子；高炉报「缺燃料 木炭(211000)」，
//   而玩家坚持箱子里有木炭和煤炭。两种可能：
//     ①燃料被放在另一个箱子（不属本组，flood-fill 只认四斜角邻接）；
//     ②物品 ID 与配方表不符（待诊断日志全量清单确认）。
//   燃料是通用消耗品、不参与产出归属，所以对①做"就近借用"：
//   本组箱子不含燃料时，把距本组任一台机器 ≤ 4 格、且确实含
//   211000/230100 的组外箱子并入本次投料的箱子集合。
//   只并入"含燃料"的箱子 → 不会把无关材料吸进来。
// ============================================================
static constexpr size_t PA_FUEL_BORROW_MAX = 2;
static constexpr float PA_FUEL_BORROW_RADIUS2 = 180.0f * 180.0f;  // 4 格

static bool PaChestHasFuel(void* chestStatus) {
    if (!chestStatus) return false;
    NearbyRawPointerVector inv = {};
    if (!NearbyReadRawInventoryRelaxed(chestStatus, &inv)) return false;
    const size_t cnt =
        (reinterpret_cast<uintptr_t>(inv.end) -
         reinterpret_cast<uintptr_t>(inv.begin)) / sizeof(void*);
    for (size_t s = 0; s < cnt; ++s) {
        void* item = inv.begin[s];
        if (!item) continue;
        NearbyItemInfo info = {};
        if (!NearbyReadItem(item, &info)) continue;
        if (info.itemId == 211000ULL || info.itemId == 230100ULL) return true;
    }
    return false;
}

static bool PaReadCString(const char* source, char* output,
                          size_t outputSize) {
    if (!source || !output || outputSize < 2) return false;
    for (size_t length = 0; length + 1 < outputSize; ++length) {
        if (!NearbyIsReadable(source + length, 1)) return false;
        const char value = source[length];
        output[length] = value;
        if (value == '\0') return true;
    }
    output[0] = '\0';
    return false;
}

// 模块名读取（旧版 L1240-1253：status+0x240→holder→data→data+0xE0 字符串指针）
static bool PaReadGimmickModule(void* status, char* module, size_t moduleSize) {
    void* holder = nullptr;
    void* data = nullptr;
    if (!status || !module || moduleSize < 2 ||
        !PaReadPtr(status, PA_GIMMICK_DATA, &holder) ||
        !PaReadPtr(holder, 0, &data) ||
        !NearbyIsReadable(
            reinterpret_cast<unsigned char*>(data) +
                GIMMICK_MODULE_NAME_OFFSET,
            sizeof(void*)))
        return false;
    const char* modulePointer = *reinterpret_cast<const char* const*>(
        reinterpret_cast<unsigned char*>(data) +
        GIMMICK_MODULE_NAME_OFFSET);
    return PaReadCString(modulePointer, module, moduleSize);
}

static bool PaIsChestModule(const char* module) {
    return module && strcmp(module, "gimmick_chest") == 0;
}

static size_t PaEnumerateGimmicks(void* save) {
    if (!save) return 0;
    size_t count = 0;
    static bool s_diagDone = false;  // 首次扫描诊断（防刷屏）

    // registry head（旧版 L1654-1671: sentinel 在 save+0x3440，
    // declared 在 save+0x3448——是 save 的相邻字段，不是 sentinel 内部！）
    void* sentinel = nullptr;
    if (!PaReadPtr(save, PA_GIMMICK_LIST, &sentinel) || !sentinel ||
        !NearbyIsReadable(reinterpret_cast<unsigned char*>(save) +
                             PA_GIMMICK_LIST + sizeof(void*),
                         sizeof(u64))) {
        if (!s_diagDone) {
            s_diagDone = true;
            void* rawList = nullptr;
            PaReadPtr(save, PA_GIMMICK_LIST, &rawList);
            Log("[PA] enum diag: sentinel INVALID save=%p raw@0x3440=%p\n",
                save, rawList);
        }
        return 0;
    }
    const u64 declared = *reinterpret_cast<const u64*>(
        reinterpret_cast<unsigned char*>(save) + PA_GIMMICK_LIST +
        sizeof(void*));
    if (declared == 0 || declared > PA_MAX_GIMMICKS ||
        !NearbyIsReadable(sentinel, sizeof(void*))) {
        if (!s_diagDone) {
            s_diagDone = true;
            Log("[PA] enum diag: declared=%llu sentinel=%p\n",
                static_cast<unsigned long long>(declared), sentinel);
            // v1.5.99【25640656 结构漂移取证】save 布局变更后 0x3440 不再是
            //   gimmickList 头（实测 sentinel=0x9 / declared=堆指针）。扫描
            //   save+0x3200..0x3700 的 qword，打印「像堆指针 + 紧随其后是
            //   合理 count(1..16384)」的候选，用来定位新偏移。一次性，只读。
            unsigned char* sbScan = reinterpret_cast<unsigned char*>(save);
            if (NearbyIsReadable(sbScan + 0x3200, 0x500)) {
                Log("[PA] save scan 0x3200..0x3700 (ptr,count) candidates:\n");
                for (uintptr_t off = 0x3200; off < 0x3700; off += 8) {
                    void* candPtr = *reinterpret_cast<void**>(sbScan + off);
                    const uintptr_t pv =
                        reinterpret_cast<uintptr_t>(candPtr);
                    const u64 candCnt =
                        *reinterpret_cast<u64*>(sbScan + off + 8);
                    if (pv > 0x10000 && candCnt >= 1 &&
                        candCnt <= PA_MAX_GIMMICKS) {
                        Log("  +0x%llX ptr=%p count=%llu\n",
                            static_cast<unsigned long long>(off), candPtr,
                            static_cast<unsigned long long>(candCnt));
                    }
                }
            }
        }
        return 0;
    }

    // 遍历（旧版 L6247-6282：链表节点 ≠ status，node+0x10 是 status 指针）
    void* node = *reinterpret_cast<void**>(sentinel);
    void* previous = sentinel;
    size_t identityFails = 0;
    size_t gidFails = 0;
    size_t nodesVisited = 0;
    for (size_t visited = 0; visited < declared; ++visited) {
        if (!node || node == sentinel ||
            !NearbyIsReadable(node, sizeof(void*) * 3)) {
            if (!s_diagDone) {
                s_diagDone = true;
                Log("[PA] enum diag: walk break at %zu node=%p (sentinel=%p)\n",
                    visited, node, sentinel);
            }
            break;
        }
        if (*reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(node) + sizeof(void*)) !=
            previous) break;  // prev 链验证
        void* status = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(node) + sizeof(void*) * 2);
        nodesVisited++;

        if (status && count < PA_MAX_ENTRIES) {
            PaStableId id = {};
            u64 gimmickId = 0;
            float x = 0, y = 0, z = 0;
            const bool idOk = PaReadIdentity(status, &id);
            const bool gidOk = idOk && PaReadGimmickId(status, &gimmickId);
            if (!idOk) identityFails++;
            if (idOk && !gidOk) gidFails++;
            if (idOk && gidOk) {
                PaReadFloat3(status, PA_POSITION_OFFSET, &x, &y, &z);
                const bool isMachine = PaIsMachineId(gimmickId);
                // 箱子识别：按模块名 gimmick_chest（旧版 L1385-1387）
                // 不能用"有 items vector"判断——储物罐等容器会误判！
                bool isChest = false;
                char module[48] = {};
                if (!isMachine &&
                    PaReadGimmickModule(status, module, sizeof(module))) {
                    isChest = PaIsChestModule(module);
                }
                if (isMachine || isChest) {
                    g_entries[count].id = id;
                    g_entries[count].status = status;
                    g_entries[count].x = x;
                    g_entries[count].y = y;
                    g_entries[count].z = z;
                    g_entries[count].gimmickId = gimmickId;
                    g_entries[count].isMachine = isMachine;
                    g_entries[count].isChest = isChest;
                    count++;
                }
                // 首次诊断：dump 第一个通过身份验证的 status 头部
                if (!s_diagDone) {
                    s_diagDone = true;
                    const unsigned char* p =
                        reinterpret_cast<const unsigned char*>(status);
                    if (NearbyIsReadable(p, 0x20)) {
                        Log("[PA] enum diag: first ok status=%p vt0=%llx vt10=%llx "
                            "uid=%llx gid=%llx pos=(%.0f,%.0f)\n",
                            status,
                            static_cast<unsigned long long>(
                                *reinterpret_cast<const u64*>(p)),
                            static_cast<unsigned long long>(
                                *reinterpret_cast<const u64*>(p + 0x10)),
                            static_cast<unsigned long long>(id.uniqueId),
                            static_cast<unsigned long long>(gimmickId),
                            x, y);
                    }
                }
            }
        }
        previous = node;
        node = *reinterpret_cast<void**>(node);
    }
    // 首次扫描汇总（无论成功失败）
    if (!s_diagDone) {
        s_diagDone = true;
        Log("[PA] enum diag: visited=%zu declared=%llu id_fails=%zu gid_fails=%zu count=%zu\n",
            nodesVisited, static_cast<unsigned long long>(declared),
            identityFails, gidFails, count);
        // 诊断：列出每个箱子的 gid + module（确认识别正确）
        for (size_t i = 0; i < count; ++i) {
            if (!g_entries[i].isChest) continue;
            char mod[48] = {};
            PaReadGimmickModule(g_entries[i].status, mod, sizeof(mod));
            Log("[PA] enum diag: chest[%zu] uid=%llu gid=%llu module=%hs\n",
                i, static_cast<unsigned long long>(g_entries[i].id.uniqueId),
                static_cast<unsigned long long>(g_entries[i].gimmickId), mod);
        }
    }
    return count;
}

// ============================================================
// v1.5.19：联动判定 = 「相邻 1 格」（含斜角），禁止隔格
//
// 玩家实测日志（v1.5.18 严格正交 45）：35/35 台机器全部 orphan —— 因为
// 场上所有相邻对都是斜角：dist2=4050 = (45,45) = 63.6，正交的只有极少数
// （而且箱子那对是 dist2=8100 = 90 = 隔一格）。
// 说明玩家的摆法就是「一格一格斜着接」，视觉上相邻但严格正交不成立。
//
// 因此改为格邻接（Chebyshev，dx≤1格 且 dy≤1格）：
//   上下左右 45 ✓ / 斜角 63.6 ✓ / 隔一格 90 ✗
// 既不做远距挂接（不会到处飞），也不会把正常摆法切断。
// ============================================================
static constexpr float PA_TILE = 45.0f;
static constexpr size_t PA_LINK_MAX_TILES = 1;
// 判定阈值严格取 1 格（45）：坐标是 45 的整数倍，留 1.0 只为吸收浮点误差
static constexpr float PA_LINK_TOL = 1.0f;

// v1.5.23：撤回 v1.5.22 的"机器↔箱子放宽到 4 格"。
//   设计要求：只要链上有一台机器挨着箱子，整条链就都算连通（机器↔机器 1 格
//   传递），不需要也不应该单独放大箱子半径——那会让远处箱子把不相干的机器
//   拉进同一组，材料和产品乱飞。故机器↔箱子与机器↔机器统一 1 格。
// v1.6.48【修复"必须对角贴箱"】v1.5.31 把这格判定改成了"只认四个对角方位"
//   （dx>0 && dy>0），于是正上/正下/正左/正右的箱子**完全连不上** ——
//   用户实测"加工机械必须贴得很近才有自动投料，加一台机器就没反应"即此。
//   现改为「1 格内任意方位」（上下左右 + 四个斜角都算），不放宽格数。
static volatile int PA_CHEST_LINK_TILES = 1;

static bool PaIsNeighborLinkedTiles(float x1, float y1, float x2, float y2,
                                    float tiles) {
    const float dx = fabsf(x1 - x2);
    const float dy = fabsf(y1 - y2);
    const float span = PA_TILE * tiles + PA_LINK_TOL;
    if (dx > span || dy > span) return false;
    return dx > PA_LINK_TOL || dy > PA_LINK_TOL;      // 同一位置不算
}

static bool PaIsNeighborLinked(float x1, float y1, float x2, float y2) {
    return PaIsNeighborLinkedTiles(
        x1, y1, x2, y2, static_cast<float>(PA_LINK_MAX_TILES));
}
// ══════════════════════════════════════════════════════════════════
// ⚠️⚠️ 死代码 —— 全项目零调用，请勿误改此处 ═══════════════════════════════
//
// PaIsOrthoNeighbor 是 v1.5.30（2026-10-17）的「正交口径」实现：
//   箱子↔箱子 / 机器↔箱子 只认上下左右（单轴偏移），斜角不算相邻。
//
// 但它**从未被调用**。实际生效的是 PaBuildGroups（v1.5.31）里的
// 「四斜角」规则：
//       linked = dx > PA_LINK_TOL && dy > PA_LINK_TOL
//                && dx <= span && dy <= span;      // |dx|=|dy|=45
//   即**只认斜角**（上下左右单轴偏移不算相邻）。
//
// ★ 两条规则互相矛盾，且本文件上方 PaBuildGroups 的头注释还写着
//   「按上下左右相邻连通」（v1.5.17 口径），三处说法都不一样。
//
// 【为什么还留着这段死代码】
//   游戏的「箱子↔机器」连接判定与 PA 不是同一套机制：
//     · 游戏的存储菜单链接走 CMD_STORAGE_MENU_LINK_CHANGE_LEFT/RIGHT（data.dat）
//     · FUN_14072e760（空间索引注册）有 52 个调用者，全是各类 gimmick 注册，
//       **没有箱子专用的** → 箱子的邻接判定不在该路径上
//   ⇒ 游戏的真实规则尚未定位，贸然统一到某一套可能让 23 台孤儿子机
//     反而全部断链。故保留本函数，等确认后再二选一。
//
// 【已知的实测证据（2026-10-26）】
//   · 用户把箱子搬到高炉**斜角**位置（(12645,21060)，与石磨 dx45/dy45）后，
//     PA 立刻接管该组 → **斜角确实有效**（游戏侧同位置也能取到燃料）
//   · 高炉在存档布局里距最近箱子 6~7 格 → 游戏与 PA 都取不到 → 反复弹窗
//
// 【下一步（需实测，一次即可定案）】
//   在高炉**正上方/正下方/正左/正右**各摆一只箱子，看游戏是否认。
//   认 → 游戏是 8 方向，PA 应放宽 PaBuildGroups 的 chest 分支为 8 方向；
//   不认 → 游戏只认斜角，现行规则正确，把上方三处矛盾注释统一即可。
// ══════════════════════════════════════════════════════════════════
static bool PaIsOrthoNeighbor(float x1, float y1, float x2, float y2) {
    const float dx = fabsf(x1 - x2);
    const float dy = fabsf(y1 - y2);
    const float span = PA_TILE + PA_LINK_TOL;
    if (dx > span || dy > span) return false;
    const bool sameRow = dy <= PA_LINK_TOL && dx > PA_LINK_TOL;
    const bool sameCol = dx <= PA_LINK_TOL && dy > PA_LINK_TOL;
    return sameRow || sameCol;
}

// ============================================================
// flood-fill 建组：机器+箱子按**相邻**连通（v1.5.17 起不用半径）
// 一组 = 连通网络内全部机器 + 全部箱子，任意机器可用组内任意箱子
//
// 【邻接口径（唯一定义处，勿凭注释推断，去下面读代码）】
//   · 涉及箱子的任意一方（chestInvolved）时：**只认四斜角**
//       linked = dx > PA_LINK_TOL && dy > PA_LINK_TOL && dx<=span && dy<=span
//       即 |dx|≈|dy|≈45（坐标同时偏移一格）；正上下左右单轴偏移**不算**相邻。
//   · 机器↔机器（都不涉及箱子）：走 PaIsNeighborLinked(PA_LINK_MAX_TILES)。
//   · v1.5.17 那句「上下左右相邻」是**旧口径，已废弃**，勿再引用；
//     v1.5.30 的正交实现 PaIsOrthoNeighbor 见上方，是零调用死代码。
// ============================================================
static void PaBuildGroups(size_t entryCount) {
    g_groupCount = 0;
    if (entryCount == 0) return;

    bool visited[PA_MAX_ENTRIES] = {};
    // BFS 队列（entries 下标）
    size_t queue[PA_MAX_ENTRIES];
    size_t qHead = 0, qTail = 0;

    for (size_t start = 0; start < entryCount &&
         g_groupCount < PA_MAX_GROUPS; ++start) {
        if (visited[start]) continue;
        // 孤立节点跳过（非机器非箱子无意义）
        if (!g_entries[start].isMachine && !g_entries[start].isChest)
            continue;

        visited[start] = true;
        queue[qTail++] = start;
        PaGroup* g = &g_groups[g_groupCount];
        g->machineCount = 0;
        g->chestCount = 0;

        while (qHead < qTail) {
            const size_t cur = queue[qHead++];
            if (g_entries[cur].isMachine &&
                g->machineCount < PA_MAX_MACHINES_PER_GROUP)
                g->machines[g->machineCount++] = g_entries[cur].id;
            if (g_entries[cur].isChest &&
                g->chestCount < PA_MAX_CHESTS_PER_GROUP)
                g->chests[g->chestCount++] = g_entries[cur].id;
            // 找相邻未访问节点
            for (size_t j = 0; j < entryCount; ++j) {
                if (visited[j]) continue;
                if (!g_entries[j].isMachine && !g_entries[j].isChest)
                    continue;
                const float d = PaDist2D(g_entries[cur].x, g_entries[cur].y,
                                         g_entries[j].x, g_entries[j].y);
                (void)d;
                // v1.5.31：按玩家口径，箱子只认「左上/右上/左下/右下」四个方位
                //   —— 即坐标同时偏移一格（|dx|=45 且 |dy|=45）。
                //   正上下左右（单轴偏移）不算相邻。机器↔机器保持原判定。
                const bool chestInvolved =
                    g_entries[cur].isChest || g_entries[j].isChest;
                bool linked = false;
                if (chestInvolved) {
                    // v1.6.48：见 PA_CHEST_LINK_TILES 注释——恢复"1 格内任意方位"，
                    //   原来只认对角，正方向全断。
                    linked = PaIsNeighborLinkedTiles(
                        g_entries[cur].x, g_entries[cur].y,
                        g_entries[j].x, g_entries[j].y,
                        static_cast<float>(PA_CHEST_LINK_TILES));
                } else {
                    linked = PaIsNeighborLinked(g_entries[cur].x,
                                                g_entries[cur].y,
                                                g_entries[j].x,
                                                g_entries[j].y);
                }
                if (linked) {
                    visited[j] = true;
                    queue[qTail++] = j;
                }
            }
        }
        // 有效组：至少一台机器且至少一个箱子
        if (g->machineCount > 0 && g->chestCount > 0)
            g_groupCount++;
    }

    // v1.6.48-diag【分组结果留痕】60 秒一条：每组 机器数/箱子数。
    //   用户反馈"加工机械离箱子近才有反应，加一台机器就没反应"——此日志可
    //   直接看出新机器是没进组（变成孤立）还是进了组但没箱子。
    {
        static ULONGLONG s_lastGroupLog = 0;
        const ULONGLONG nowG = GetTickCount64();
        if (nowG - s_lastGroupLog >= 60000ULL) {
            s_lastGroupLog = nowG;
            char buf[512] = {};
            int off = 0;
            for (size_t gi = 0; gi < g_groupCount && gi < 8; ++gi) {
                off += _snprintf_s(buf + off, sizeof(buf) - off, _TRUNCATE,
                                   " [组%zu m=%zu c=%zu]", gi,
                                   g_groups[gi].machineCount,
                                   g_groups[gi].chestCount);
                if (off >= (int)sizeof(buf) - 40) break;
            }
            Log("[PA] groups: total=%zu chestLinkTiles=%d%s\n",
                g_groupCount, (int)PA_CHEST_LINK_TILES, buf);
        }
    }

}

// ============================================================
// v1.5.16-diag：孤立机器「周围到底放了什么」一次性取证
//
// 玩家反馈：材料箱就放在机器隔壁，却仍不生效。组内无箱 → 永不投料，
// 所以要看清机器 300 范围内实际存在的对象（gid / 模块名 / 距离）。
// 只读；不改变联动规则（保持"靠近才算生产链"）。
// ============================================================
struct PaNbObj {
    float x, y;
    u64 gid;
    char mod[40];
};
static PaNbObj g_paNbObjects[1024];

static void PaDiagOrphanNeighbors(void* save, size_t entryCount) {
    // v1.5.21：改为按拓扑变化重复调用（原来一次性 → 中途挪箱子看不到结果）
    if (!save) return;

    void* sentinel = nullptr;
    if (!PaReadPtr(save, PA_GIMMICK_LIST, &sentinel) || !sentinel ||
        !NearbyIsReadable(reinterpret_cast<unsigned char*>(save) +
                              PA_GIMMICK_LIST + sizeof(void*),
                          sizeof(u64))) {
        Log("[PA] nbr: list unreadable\n");
        return;
    }
    const u64 declared = *reinterpret_cast<const u64*>(
        reinterpret_cast<unsigned char*>(save) + PA_GIMMICK_LIST +
        sizeof(void*));
    // v1.5.99【崩溃修复 25640656】与 PaEnumerateGimmicks 同款保护。
    //   save 结构漂移后 sentinel 可能读到小整数（实测 0x9），此前直接
    //   `*sentinel` 解引用 → 访问违规 addr=0x9 直接踢出游戏。必须先验
    //   declared 范围与 sentinel 可读性，任一不满足就放弃（只诊断不崩）。
    if (declared == 0 || declared > PA_MAX_GIMMICKS ||
        !NearbyIsReadable(sentinel, sizeof(void*))) {
        Log("[PA] nbr: list invalid (declared=%llu sentinel=%p)\n",
            static_cast<unsigned long long>(declared), sentinel);
        return;
    }
    void* node = *reinterpret_cast<void**>(sentinel);
    void* previous = sentinel;
    size_t count = 0;
    for (size_t i = 0; i < declared && i < 8192 && count < 1024; ++i) {
        if (!node || node == sentinel ||
            !NearbyIsReadable(node, sizeof(void*) * 3))
            break;
        if (*reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(node) + sizeof(void*)) !=
            previous)
            break;
        void* status = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(node) + sizeof(void*) * 2);
        if (status && NearbyIsReadable(status, 0x2f0)) {
            float x = 0, y = 0, z = 0;
            char mod[40] = {};
            u64 gid = 0;
            PaReadGimmickModule(status, mod, sizeof(mod));
            PaReadGimmickId(status, &gid);
            if (PaReadFloat3(status, PA_POSITION_OFFSET, &x, &y, &z)) {
                g_paNbObjects[count].x = x;
                g_paNbObjects[count].y = y;
                g_paNbObjects[count].gid = gid;
                strncpy_s(g_paNbObjects[count].mod,
                          sizeof(g_paNbObjects[count].mod), mod, _TRUNCATE);
                ++count;
            }
        }
        previous = node;
        node = *reinterpret_cast<void**>(node);
    }
    Log("[PA] nbr: objects=%zu declared=%llu\n", count,
        static_cast<unsigned long long>(declared));

    // 逐台孤立机器：找 300 范围内最近的 5 个对象
    const float radiusSq = 300.0f * 300.0f;
    for (size_t j = 0; j < entryCount; ++j) {
        if (!g_entries[j].isMachine) continue;
        bool inGroup = false;
        for (size_t gi = 0; gi < g_groupCount && !inGroup; ++gi) {
            for (size_t m = 0; m < g_groups[gi].machineCount; ++m) {
                if (PaIdEqual(g_groups[gi].machines[m], g_entries[j].id)) {
                    inGroup = true;
                    break;
                }
            }
        }
        if (inGroup) continue;

        struct Hit {
            float dist;
            size_t idx;
        };
        Hit hits[5] = {};
        size_t hitCount = 0;
        for (size_t k = 0; k < count; ++k) {
            const float d = PaDist2D(g_entries[j].x, g_entries[j].y,
                                     g_paNbObjects[k].x, g_paNbObjects[k].y);
            if (d <= 0.0f || d > radiusSq) continue;
            if (hitCount < 5) {
                hits[hitCount].dist = d;
                hits[hitCount].idx = k;
                ++hitCount;
            } else {
                size_t worst = 0;
                for (size_t h = 1; h < 5; ++h)
                    if (hits[h].dist > hits[worst].dist) worst = h;
                if (d < hits[worst].dist) {
                    hits[worst].dist = d;
                    hits[worst].idx = k;
                }
            }
        }
        for (size_t h = 0; h < hitCount; ++h) {
            const PaNbObj* o = &g_paNbObjects[hits[h].idx];
            Log("[PA] nbr machine gid=%llu pos=(%.0f,%.0f) <- gid=%llu "
                "mod=%s pos=(%.0f,%.0f) dist2=%.0f\n",
                static_cast<unsigned long long>(g_entries[j].gimmickId),
                g_entries[j].x, g_entries[j].y,
                static_cast<unsigned long long>(o->gid),
                o->mod[0] ? o->mod : "?", o->x, o->y, hits[h].dist);
        }
    }
    Log("[PA] nbr: end\n");
}
// ============================================================
// v1.5.35-diag：原生生产接口探针（默认关闭）
//
// 目的：一次上手就把 push / pop / native_collect 的真实参数与返回值抓全，
//   不用分步试错。开启方式：在游戏根目录创建空文件
//       qol_pa_apiprobe.txt
//   然后正常手动投料/取料一次，日志会写出每次调用的
//   rcx / rdx / r8 / r9 与返回值。
// 关闭（默认）：文件不存在 → 完全不挂钩，行为与 v1.5.34 一致。
// ============================================================
typedef uintptr_t (*PaNative4)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static PaNative4 g_probePushOrig = nullptr;
static PaNative4 g_probePopOrig = nullptr;
// v1.5.96：near / far 两条 UI 指令的探针（各捕获自己的 baseID）
static PaNative4 g_probeNearOrig = nullptr;
static PaNative4 g_probeFarOrig = nullptr;
// ============================================================
// v1.5.101【调用链观测 — 安全 hook 子集 + 纯读表观测】
//
// 目的：把"玩家手动投料/取料"时游戏自身走的关键调用实时显示到游戏内浮窗
//   （F6 切换，production_chainhud.inl），直接拿到原版调用链，不必在 PA 里反复猜。
//
// 实测结论（2026-10-25，崩溃 dump QoL_crash/20261025_231703 实证，rva 0x27E90D）：
//   * near(0x27D480) / far(0x27DCE0) 入口首条指令恰好 5 字节 → HookInstall 跳板
//     字节对齐，安全（v1.5.96 实测靠近机械不崩，沿用）。
//   * manpu(0x27E460) 入口首条虽也 5 字节，但 trampoline 改写调用栈帧的 RSP 基准，
//     函数体内调别人时栈错位 → 深处 MOV RDI,[RBX+0x28] 解野指针崩溃。故禁用其 hook。
//
// 2026-10-29 复查（build 25640656，新址 near 0x285CE0 / far 0x286540 / manpu 0x286CC0）：
//   新址与旧址**头部 48 字节逐字节相同**，首条指令仍是 `48 89 5C 24 18`
//   （mov [rsp+0x18],rbx，恰好 5 字节）→ 上述"near/far 可安全 hook、manpu 不可"
//   的结论在新版依然成立，无需改动。

//   * alive(0x1569C0) 首条 SUB RSP,0x8(4B) + 次条 MOV [RSP+0x10],RBX(5B) → 5 字节
//     拷贝截断次条指令，跳板必然损坏 → 禁用。
//   因此只保留 near/far 两个安全 hook；manpu 是否真的创建了动画实例，改用
//   PaChainSampleAnim 纯读动画管理器实例表（零 hook、零崩）在浮窗显示，等效观测。
//
// 注：移除 manpu hook 后，PA 自动投料轮原先靠 collect 探针捕获的 g_paCapturedBaseId
//   暂不再更新；PA 恢复自动化时需改为在 PaMaintainProcessingUi 自身调用处取 baseID。
// ============================================================
static std::atomic<int> g_origProbeOn{0};  // 仅供 near/far 的 [ORIG] 日志开关（默认关）
static PaNative4 g_probeLoadOrig = nullptr;   // v1.5.42: native_load
// v1.5.86：默认 0（静默）。仅 qol_pa_apiprobe.txt 诊断时由 PaInstallApiProbe
//   置正数，避免无条件 collect 探针刷日志（捕获 baseID 本身不受 budget 限制）。
static std::atomic<int> g_probeBudget{0};

// v1.5.38：机器状态紧凑快照（探针用）——用来做"手动投料前后 diff"
static void PaProbeDumpState(const char* tag, void* status) {
    if (!status) return;
    char buf[320] = {};
    int off = 0;
    for (size_t slot = 0; slot < 3 && off >= 0; ++slot) {
        char idKey[8] = {};
        char valKey[9] = {};
        _snprintf_s(idKey, sizeof(idKey), _TRUNCATE, "itemID%zu", slot);
        _snprintf_s(valKey, sizeof(valKey), _TRUNCATE, "itemVal%zu", slot);
        u64 itemId = 0;
        int count = 0;
        u64 st = 0, du = 0;
        PaReadStatusU64(status, idKey, &itemId);
        PaReadStatusInt(status, valKey, &count);
        PaReadSlotTimers(status, slot, &st, &du);

        char backing[256] = {};
        int bo = 0;
        void** arr = nullptr;
        if (PaReadBacking(status, PA_MACHINE_SLOTS * PA_SLOT_INGREDIENTS,
                          &arr) && arr) {
            for (size_t k = 0; k < PA_SLOT_INGREDIENTS && bo >= 0; ++k) {
                void* it = arr[slot * PA_SLOT_INGREDIENTS + k];
                if (!it) continue;
                NearbyItemInfo inf = {};
                if (NearbyReadItem(it, &inf)) {
                    bo += _snprintf_s(backing + bo, sizeof(backing) - bo,
                                      _TRUNCATE, "%llu/%d@%p ",
                                      (unsigned long long)inf.itemId,
                                      inf.stackCount, it);
                }
                if (bo < 0) break;
            }
        }
        off += _snprintf_s(buf + off, sizeof(buf) - off, _TRUNCATE,
                           "|s%zu id=%llu v=%d t=%llu/%llu bk[%s]",
                           slot, (unsigned long long)itemId, count,
                           (unsigned long long)st, (unsigned long long)du,
                           backing);
    }
    if (off > 0) Log("[PA][api] %s state %s\n", tag, buf);
}

// v1.5.44：dump 槽内首个 backing 物品的内存头部（对比游戏插入 vs PA 克隆）
static void PaProbeDumpItem(void* status) {
    void** arr = nullptr;
    if (!status ||
        !PaReadBacking(status, PA_MACHINE_SLOTS * PA_SLOT_INGREDIENTS, &arr) ||
        !arr)
        return;
    void* item = arr[0];
    if (!item || !NearbyIsReadable(item, 0x40)) return;
    const unsigned int* d = reinterpret_cast<const unsigned int*>(item);
    Log("[PA][api] item %p dwords: %08X %08X %08X %08X %08X %08X %08X %08X\n",
        item, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
}

static uintptr_t PaProbeForward(const char* tag, uintptr_t a, uintptr_t b,
                                uintptr_t c, uintptr_t d, PaNative4 orig) {
    const bool logIt = g_probeBudget.fetch_sub(1, std::memory_order_relaxed) > 0;
    // v1.5.38：机器类函数（rcx 指向 status）记录调用前后快照
    void* status = nullptr;
    if (a > 0x10000 && a < 0x7FFFFFFFFFFFULL) status = (void*)a;
    if (logIt && status) PaProbeDumpState("before", status);
    const uintptr_t ret = orig ? orig(a, b, c, d) : 0;
    if (logIt && status) {
        PaProbeDumpState("after ", status);
        PaProbeDumpItem(status);
    }
    if (logIt) {
        Log("[PA][api] %s rcx=%p rdx=%p r8=%p r9=%p ret=%p\n", tag,
            reinterpret_cast<void*>(a), reinterpret_cast<void*>(b),
            reinterpret_cast<void*>(c), reinterpret_cast<void*>(d),
            reinterpret_cast<void*>(ret));
    }
    return ret;
}

static uintptr_t PaProbePush(uintptr_t a, uintptr_t b, uintptr_t c,
                             uintptr_t d) {
    return PaProbeForward("push", a, b, c, d, g_probePushOrig);
}
static uintptr_t PaProbePop(uintptr_t a, uintptr_t b, uintptr_t c,
                            uintptr_t d) {
    return PaProbeForward("pop", a, b, c, d, g_probePopOrig);
}
// v1.5.101：manpu(0x27E460) 的 hook 已禁用 —— 其实测在 trampoline 下崩于
//   rva 0x27E90D（RSP/栈帧错位）。manpu 是否创建动画实例改由 PaChainSampleAnim
//   纯读动画管理器实例表观测（零 hook、零崩）。

// v1.5.105：near/far 事件节流落盘 —— 区分来源（玩家 near / PA 自己 nearPA）并带
//   第 2 参数（status 指针，用于与动画实例 owner 对表）。
//   2026-10-26 实测时间线：原版在 near UI 调用后 1.4~6 秒挂新动画实例；
//   本版要验证「PA 自己调 near 是否也能挂上实例」——若 PA 调用后不挂，即定位到
//   PA 侧缺环；若挂上，则动画管理器这侧 PA 本就通了，问题在别处。
static void PaUiEvtLogThrottled(const char* tag, uintptr_t base,
                                 uintptr_t status, uintptr_t ret) {
    struct Ent { uintptr_t id; uintptr_t tagKey; ULONGLONG t; };
    static Ent s_tab[32] = {};
    const ULONGLONG now = GetTickCount64();
    const uintptr_t tagKey = reinterpret_cast<uintptr_t>(tag);  // 字面量地址做区分
    for (int i = 0; i < 32; ++i) {
        if (s_tab[i].id == base && s_tab[i].tagKey == tagKey) {
            if (now - s_tab[i].t < 3000ULL) return;
            s_tab[i].t = now;
            Log("[PA][uievt] %s base=%llx st=%llx ret=%llx\n", tag,
                static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(status),
                static_cast<unsigned long long>(ret));
            return;
        }
    }
    for (int i = 0; i < 32; ++i) {
        if (!s_tab[i].id) {
            s_tab[i].id = base;
            s_tab[i].tagKey = tagKey;
            s_tab[i].t = now;
            break;
        }
    }
    Log("[PA][uievt] %s base=%llx st=%llx ret=%llx\n", tag,
        static_cast<unsigned long long>(base),
        static_cast<unsigned long long>(status),
        static_cast<unsigned long long>(ret));
}

static uintptr_t PaProbeNear(uintptr_t a, uintptr_t b, uintptr_t c,
                             uintptr_t d) {
    // v1.6.40：必须在**调用原函数之前**抓线程上下文的 actor —— 原函数返回后
    //   上下文可能已被游戏换走，那时抓到的就不是这台机器的 actor 了。
    void* tlsActor = PaTlsActorPeek();
    // v1.5.100：同上，先 orig 后记录（register-safe）。
    const uintptr_t ret = g_probeNearOrig ? g_probeNearOrig(a, b, c, d) : 0;
    // v1.5.96：near UI(0x27D480) 的 rcx 即它自己的 baseID（与 manpu 的不同）
    const bool byPa = (g_paUiCallDepth.load(std::memory_order_relaxed) != 0);
    if (!byPa) {
        g_paBaseIdNear.store(a, std::memory_order_relaxed);
        // v1.6.40：游戏自己调用 = actor 必然正确 → 收下 (status → actor) 配对
        PaActorHarvestStore(reinterpret_cast<void*>(b), tlsActor);
        // v1.5.110：任意合法 baseID 对全部机器通用，首捕即作为全局兜底
        if (!g_paValidBaseId.load(std::memory_order_relaxed))
            g_paValidBaseId.store(a, std::memory_order_relaxed);
        // v1.5.108：收获 (status, baseID) 正确配对（游戏自身调用，b=status）
        PaBaseHarvestStore(reinterpret_cast<void*>(b), a);

        if (g_paChainActive.load(std::memory_order_relaxed)) {
            wchar_t cb[96];
            _snwprintf_s(cb, _TRUNCATE, L"near base=%p",
                         reinterpret_cast<void*>(a));
            PaChainAdd(1, cb);
        }
        // v1.6.63【piggyback】游戏自己调 near 时 TLS 上下文天然正确，
        //   用同一 baseId/status 顺带补建 far/manpu，绕开「PA 线程直调挂错
        //   manager」与「harvest status 配对不上」两个死结。
        //   用 g_paUiCallDepth 包住，防止 hook 内层再次 harvest / 递归。
        //   默认关，靠 QoL_hot.json 的 UI_PIGGYBACK 热开启做 A/B。
        if (g_paUiPiggyback && g_paUiFar && g_paUiManpu && a && b) {
            g_paUiCallDepth.fetch_add(1, std::memory_order_relaxed);
            const uintptr_t rf = PaSafeUiCreate(g_paUiFar, a, b);
            const uintptr_t rm = PaSafeUiCreate(g_paUiManpu, a, b);
            g_paUiCallDepth.fetch_sub(1, std::memory_order_relaxed);
            static int s_pigLog = 0;
            if (s_pigLog < 20) {
                ++s_pigLog;
                Log("[PA][piggy] base=%llu st=%p far=%p manpu=%p "
                    "nearRet=%p\n",
                    static_cast<unsigned long long>(a),
                    reinterpret_cast<void*>(b),
                    reinterpret_cast<void*>(rf), reinterpret_cast<void*>(rm),
                    reinterpret_cast<void*>(ret));
            }
        }
    }
    // v1.5.105：来源标记 + status 指针（与实例 owner 对表）
    PaUiEvtLogThrottled(byPa ? "nearPA" : "near", a, b, ret);
    if (g_origProbeOn.load(std::memory_order_relaxed)) {
        Log("[ORIG] near base=%p status=%p ret=%p\n",
            reinterpret_cast<void*>(a), reinterpret_cast<void*>(b),
            reinterpret_cast<void*>(ret));
    }
    return ret;
}

// v1.6.47-diag【far/manpu 早退点探针】
//   反编译 FUN_140286540（far）与 FUN_140286cc0（manpu）主体之前有两道门：
//     ① lVar4 = *(*(GAME_ROOT+0x208)+0x32b8)  // = 玩家 status
//        再由它取 +0x158 字符串 / +0x150 指针算出场景标识，
//        **等于 0xbfb2b51 就 goto end**（疑似"屋内/非世界场景"）。
//     ② if ((GAME_ROOT+0x268 == 0 ||
//            *(*(*(GAME_ROOT+0x268)+0xd8)+0x3ab30) == 0) && farUI计数 < 0xC9)
//        才进主体 → 计数 >= 201 时静默跳过（全局上限）。
//   实测 PA 调用时 before==after（键没被写）= 函数在写键前就早退，但不知道卡哪条。
//   本探针纯读上述三个量，在「游戏自己调用」与「PA 调用」两个时机各记一份，
//   一次对照即可定位是哪道门。零副作用。
static void PaCondProbe(const char* who, u64 gid) {
    const uintptr_t base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) return;
    void* root = nullptr;
    if (!PaReadPtr(reinterpret_cast<void*>(base + RVA_GAME_ROOT), 0, &root) ||
        !root) return;
    void* playerObj = nullptr;
    PaReadPtr(root, 0x208, &playerObj);
    void* playerStatus = nullptr;
    if (playerObj) PaReadPtr(playerObj, 0x32b8, &playerStatus);
    void* p150 = nullptr;
    void* p158 = nullptr;
    if (playerStatus) {
        PaReadPtr(playerStatus, 0x150, &p150);
        PaReadPtr(playerStatus, 0x158, &p158);
    }
    void* g268 = nullptr;
    PaReadPtr(root, 0x268, &g268);
    void* g268d8 = nullptr;
    if (g268) PaReadPtr(g268, 0xd8, &g268d8);
    unsigned char flag = 0;
    if (g268d8 && NearbyIsReadable(
            reinterpret_cast<unsigned char*>(g268d8) + 0x3ab30, 1))
        flag = *(reinterpret_cast<unsigned char*>(g268d8) + 0x3ab30);
    int farCount = -1;
    if (NearbyIsReadable(reinterpret_cast<void*>(base + 0x319F8D8), 4))
        farCount = *reinterpret_cast<const int*>(base + 0x319F8D8);
    Log("[PA][cond] %s gid=%llu playerStatus=%p p150=%p p158=%p "
        "g268=%p g268d8=%p flag=%u farCount=%d\n",
        who, static_cast<unsigned long long>(gid), playerStatus, p150, p158,
        g268, g268d8, static_cast<unsigned>(flag), farCount);
}

// ============================================================
// v1.6.53【黄金快照 + 写回】—— 用户的思路：不造对象，借游戏造好的。
//
//   已确认的事实（2026-11-01 实测）：
//     · 玩家手动投料 → 游戏自己建 near 对象 → 走远有**小进度条**（有效、能渲染）
//     · PA 自动投料一轮后 → 小 UI 消失且不再出现
//     · PA 直接调 near/far 创建函数 6 个版本都不渲染（TLS/baseId/时机都试过）
//     · status 上只有 5+3 个键；PA 从不直接写 UIManpu/UIPrsFar
//   ⇒ 游戏建的那个对象是"唯一有效"的。PA 能做的是**记住它的键值，被清/被换后
//     把原值写回去**（只写键，不造对象）。
//
//   快照时机：near/far 入口 hook 里 byPa=false（游戏自己调用）时采——此刻对象刚建好。
//   写回时机：维护链里发现键值偏离快照 → 写回（per-status 3 秒节流）。
//   风险控制：只写快照中非 0 的值；写前后留日志；SEH 保护；可全局关闭。
// ============================================================
struct PaUiGolden {
    void*     status = nullptr;
    u64       gid    = 0;      // v1.6.55：status 指针会被游戏复用，必须配 gid 校验
    u64       pros   = 0;      // UIPros 快照
    u64       mpu    = 0;      // UIManpu 快照
    u64       farV    = 0;      // UIPrsFar 快照
    ULONGLONG tick   = 0;      // 快照时刻
    ULONGLONG lastWB = 0;      // 上次写回时刻
};

static PaUiGolden g_paGolden[32];
static volatile long g_paGoldenWriteBack = 0;   // 0=只观察不写回（v1.6.55 紧急关闭）

static PaUiGolden* PaGoldenSlot(void* status, u64 gid) {
    if (!status) return nullptr;
    for (int i = 0; i < 32; ++i)
        if (g_paGolden[i].status == status && g_paGolden[i].gid == gid)
            return &g_paGolden[i];
    for (int i = 0; i < 32; ++i) {
        if (!g_paGolden[i].status) {
            g_paGolden[i].status = status;
            g_paGolden[i].gid = gid;
            return &g_paGolden[i];
        }
    }
    // 满了就替换最旧的
    int oldest = 0;
    for (int i = 1; i < 32; ++i)
        if (g_paGolden[i].tick < g_paGolden[oldest].tick) oldest = i;
    g_paGolden[oldest].status = status;
    g_paGolden[oldest].gid = gid;
    return &g_paGolden[oldest];
}

// 游戏自己建 UI 之后采集（byPa=false 时调用）
static void PaGoldenCapture(void* status) {
    if (!status) return;
    u64 pros = 0, mpu = 0, farV = 0;
    const bool hp = PaStatusKeyPresent(status, "UIPros", &pros);
    const bool hm = PaStatusKeyPresent(status, "UIManpu", &mpu);
    const bool hf = PaStatusKeyPresent(status, "UIPrsFar", &farV);
    if (!hp && !hm && !hf) return;
    u64 gid = 0;
    PaReadGimmickId(status, &gid);
    PaUiGolden* e = PaGoldenSlot(status, gid);
    if (!e) return;
    const bool changed = (e->pros != pros) || (e->mpu != mpu) || (e->farV != farV);
    e->pros = pros;
    e->mpu = mpu;
    e->farV = farV;
    e->tick = GetTickCount64();
    if (changed) {
        static int s_capLog = 0;
        if (s_capLog < 40) {
            ++s_capLog;
            Log("[PA][golden] 采集 st=%p pros=%llu mpu=%llu farV=%llu\n",
                status, static_cast<unsigned long long>(pros),
                static_cast<unsigned long long>(mpu),
                static_cast<unsigned long long>(farV));
        }
    }
}

// 键值偏离快照时写回（维护链每轮调用，per-status 3 秒节流）
static void PaGoldenRestore(void* status, u64 gid) {
    if (!status) return;
    PaUiGolden* e = PaGoldenSlot(status, gid);
    if (!e || e->tick == 0) return;
    const ULONGLONG now = GetTickCount64();
    if (e->lastWB && now - e->lastWB < 3000ULL) return;

    u64 pros = 0, mpu = 0, farV = 0;
    const bool hp = PaStatusKeyPresent(status, "UIPros", &pros);
    const bool hm = PaStatusKeyPresent(status, "UIManpu", &mpu);
    const bool hf = PaStatusKeyPresent(status, "UIPrsFar", &farV);

    const bool diffP = (e->pros != 0) && (!hp || pros != e->pros);
    const bool diffM = (e->mpu != 0) && (!hm || mpu != e->mpu);
    const bool diffF = (e->farV != 0) && (!hf || farV != e->farV);
    if (!diffP && !diffM && !diffF) return;

    static int s_wbLog = 0;
    if (s_wbLog < 60) {
        ++s_wbLog;
        Log("[PA][golden] 偏离 gid=%llu pros %llu->%llu(%d) mpu %llu->%llu(%d) "
            "farV %llu->%llu(%d) writeBack=%d\n",
            static_cast<unsigned long long>(gid),
            static_cast<unsigned long long>(pros),
            static_cast<unsigned long long>(e->pros), hp ? 1 : 0,
            static_cast<unsigned long long>(mpu),
            static_cast<unsigned long long>(e->mpu), hm ? 1 : 0,
            static_cast<unsigned long long>(farV),
            static_cast<unsigned long long>(e->farV), hf ? 1 : 0,
            (int)g_paGoldenWriteBack);
    }
    if (!g_paGoldenWriteBack) return;
    e->lastWB = now;
    if (diffF) PaSetStatusU64(status, "UIPrsFar", e->farV);
    if (diffM) PaSetStatusU64(status, "UIManpu",  e->mpu);
    if (diffP) PaSetStatusU64(status, "UIPros",   e->pros);
}

static uintptr_t PaProbeFar(uintptr_t a, uintptr_t b, uintptr_t c,
                            uintptr_t d) {
    // v1.6.40：调用前抓 actor（同 near，理由见 PaTlsActorPeek 注释块）
    void* tlsActor = PaTlsActorPeek();
    // v1.6.47：游戏自己调用 far 时（进档批量建/手投料）记录早退条件快照
    {
        static int s_c = 0;
        if (s_c < 8 && g_paUiCallDepth.load(std::memory_order_relaxed) == 0) {
            ++s_c;
            PaCondProbe("game-far", 0);
        }
    }
    // v1.5.100：同上，先 orig 后记录（register-safe）。
    const uintptr_t ret = g_probeFarOrig ? g_probeFarOrig(a, b, c, d) : 0;
    // v1.5.96：far UI(0x27DCE0) 的 rcx 即它自己的 baseID
    const bool byPa = (g_paUiCallDepth.load(std::memory_order_relaxed) != 0);
    if (!byPa) {
        g_paBaseIdFar.store(a, std::memory_order_relaxed);
        // v1.6.40：收下 (status → actor) 配对
        PaActorHarvestStore(reinterpret_cast<void*>(b), tlsActor);
        // v1.6.57：kind=1 → far 专用表位（注意与 near 的 baseId 不同）
        PaBaseHarvestStore(reinterpret_cast<void*>(b), a, 1);
        // v1.6.53：游戏刚建完 far UI → 采集黄金快照
        PaGoldenCapture(reinterpret_cast<void*>(b));
        // v1.5.110：任意合法 baseID 对全部机器通用，首捕即作为全局兜底
        if (!g_paValidBaseId.load(std::memory_order_relaxed))
            g_paValidBaseId.store(a, std::memory_order_relaxed);
        // v1.5.108：收获 (status, baseID) 正确配对（游戏自身调用，b=status）
        // v1.6.57：kind=0 → near 专用表位（near/far 的 baseId 完全不同）
        PaBaseHarvestStore(reinterpret_cast<void*>(b), a, 0);
        // v1.6.53：游戏刚建完 near UI → 立刻采集黄金快照（此刻键值有效）
        PaGoldenCapture(reinterpret_cast<void*>(b));
        if (g_paChainActive.load(std::memory_order_relaxed)) {
            wchar_t cb[96];
            _snwprintf_s(cb, _TRUNCATE, L"far base=%p",
                         reinterpret_cast<void*>(a));
            PaChainAdd(2, cb);
        }
    }
    // v1.5.105：来源标记 + status 指针（与实例 owner 对表）
    PaUiEvtLogThrottled(byPa ? "farPA" : "far", a, b, ret);
    if (g_origProbeOn.load(std::memory_order_relaxed)) {
        Log("[ORIG] far base=%p status=%p ret=%p\n",
            reinterpret_cast<void*>(a), reinterpret_cast<void*>(b),
            reinterpret_cast<void*>(ret));
    }
    return ret;
}
static uintptr_t PaProbeLoad(uintptr_t a, uintptr_t b, uintptr_t c,
                             uintptr_t d) {
    return PaProbeForward("load", a, b, c, d, g_probeLoadOrig);
}

static void PaInstallApiProbe() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;

    const uintptr_t base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    // v1.5.101：只安装 near/far 两个【入口首条指令恰好 5 字节】的安全 hook。
    //   - manpu(0x27E460) 与 alive(0x1569C0) 的 hook 已禁用：实测前者在 trampoline
    //     下因 RSP/栈帧错位崩于 rva 0x27E90D，后者 5 字节拷贝会截断其 2 指令 prologue。
    //     （崩溃 dump QoL_crash/20261025_231703 实证。）manpu 的动画实例改由
    //     PaChainSampleAnim 纯读实例表观测。
    //   幂等：本函数开头 static s_done 保证整个安装过程只执行一次。
    const bool okNear = qol::HookInstall(
        base + RVA_PRODUCTION_UI_NEAR,
        reinterpret_cast<uintptr_t>(&PaProbeNear),
        reinterpret_cast<uintptr_t*>(&g_probeNearOrig));
    const bool okFar = qol::HookInstall(
        base + RVA_PRODUCTION_UI_FAR,
        reinterpret_cast<uintptr_t>(&PaProbeFar),
        reinterpret_cast<uintptr_t*>(&g_probeFarOrig));
    Log("[PA][api] v1.5.112: uiprobe near=%d far=%d (manpu/alive hooks "
        "disabled: crash-prone; PA 不再抢建 near UI（还给游戏），只建 far+manpu；"
        "维护链已开启；动画专项已于 v1.5.118 收口归档)\n",
        static_cast<int>(okNear), static_cast<int>(okFar));

    // 诊断用 push/pop/load 仅在 qol_pa_apiprobe.txt 存在时安装
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    *(slash + 1) = 0;
    wcscat_s(path, MAX_PATH, L"qol_pa_apiprobe.txt");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;

    const bool okPush = qol::HookInstall(
        base + RVA_PRODUCTION_PUSH, reinterpret_cast<uintptr_t>(&PaProbePush),
        reinterpret_cast<uintptr_t*>(&g_probePushOrig));
    const bool okPop = qol::HookInstall(
        base + RVA_PRODUCTION_POP, reinterpret_cast<uintptr_t>(&PaProbePop),
        reinterpret_cast<uintptr_t*>(&g_probePopOrig));
    const bool okLoad = qol::HookInstall(
        base + RVA_PRODUCTION_NATIVE_LOAD,
        reinterpret_cast<uintptr_t>(&PaProbeLoad),
        reinterpret_cast<uintptr_t*>(&g_probeLoadOrig));
    g_probeBudget.store(600, std::memory_order_relaxed);  // 诊断时放开打印预算
    Log("[PA][api] probe installed: push=%d pop=%d collect=DISABLED load=%d\n",
        static_cast<int>(okPush), static_cast<int>(okPop),
        static_cast<int>(okLoad));
}

// v1.5.91 的判活 inline hook 已移除：PaTick 每 tick 重复 HookInstall 同一地址，
//   目标指令被反复改写，靠近加工机械即崩溃。动画专项已于 v1.5.118 收口归档。

// ============================================================
// v1.5.39：清理"幻影槽"——PA 直写状态被游戏拒绝后残留的脏数据
//
// 现场：织布机被写入 itemID=212000 / duration=82800，但 start 始终 0，
//   游戏认为该槽"有产出却永不完成"→ 面板拒绝再投料/取料。
// 判定：非被动机器 + itemID!=0 + start==0 + duration!=0 → 清空该槽。
//   （被动机器本来就是 itemID!=0 且两个 timer 都为 0，不在此列）
// ============================================================
static void PaSanitizePhantomSlots(void* status, u64 gid) {
    if (!status || PaIsPassiveMachine(gid)) return;
    size_t slotLimit = 1;
    PaReadSlotLimit(status, &slotLimit);
    if (slotLimit > PA_MACHINE_SLOTS) slotLimit = PA_MACHINE_SLOTS;
    for (size_t slot = 0; slot < slotLimit; ++slot) {
        char idKey[8] = {};
        char valKey[9] = {};
        _snprintf_s(idKey, sizeof(idKey), _TRUNCATE, "itemID%zu", slot);
        _snprintf_s(valKey, sizeof(valKey), _TRUNCATE, "itemVal%zu", slot);
        u64 itemId = 0;
        int count = 0;
        u64 st = 0, du = 0;
        PaReadStatusU64(status, idKey, &itemId);
        PaReadStatusInt(status, valKey, &count);
        PaReadSlotTimers(status, slot, &st, &du);
        if (itemId == 0 || st != 0 || du == 0) continue;

        Log("[PA] sanitize phantom slot: gid=%llu slot=%zu id=%llu "
            "start=0 dur=%llu -> cleared\n",
            static_cast<unsigned long long>(gid), slot,
            static_cast<unsigned long long>(itemId),
            static_cast<unsigned long long>(du));
        volatile LONG64* startField = reinterpret_cast<volatile LONG64*>(
            reinterpret_cast<unsigned char*>(status) + PA_TIME_VALUE +
            slot * sizeof(u64));
        volatile LONG64* durField = reinterpret_cast<volatile LONG64*>(
            reinterpret_cast<unsigned char*>(status) + PA_CREATE_TIME +
            slot * sizeof(u64));
        InterlockedExchange64(durField, 0);
        InterlockedExchange64(startField, 0);
        PaSetStatusU64(status, idKey, 0);
        PaSetStatusInt(status, valKey, 0);
    }
}

// ============================================================
// v1.5.79-diag：空间索引 map 只读查询（FN-029 补充二决定性诊断）
//   查某 key(组件指针)是否已在 machine+0x2B8 的哈希表里。
//   完全复用 PaDumpStatusMap 的安全遍历姿势（sentinel/buckets/mask +
//   NearbyIsReadable + hop 上限）——只读、不写入、不调用任何原生函数。
//   背景：Ghidra 证实 FUN_14072e760(machine+0x2B8,&out,&comp) 在组件创建
//   （手动点火）时就把组件注册进该 map；若 push 投料的机器组件早已在 map 里，
//   → PA 再注册=空操作，空间索引不是"加工没动画"的缺失环节。
//   map 布局：+0x8 sentinel / +0x18 buckets / +0x30 mask；桶16B(链尾/链头)；
//   节点 +8=next / +0x10=key。
// ============================================================
static bool PaSpatialMapHasKey(void* status, uintptr_t mapOffset, u64 key) {
    if (!status) return false;
    unsigned char* map = reinterpret_cast<unsigned char*>(status) + mapOffset;
    if (!NearbyIsReadable(map, 0x38)) return false;
    void* sentinel = *reinterpret_cast<void**>(map + 0x08);
    void* buckets = *reinterpret_cast<void**>(map + 0x18);
    const u64 mask = *reinterpret_cast<const u64*>(map + 0x30);
    if (!sentinel || !buckets || mask == 0 || mask > 0x10000ULL) return false;
    const size_t bucketCount = static_cast<size_t>(mask) + 1;
    for (size_t b = 0; b < bucketCount; ++b) {
        unsigned char* bkt = reinterpret_cast<unsigned char*>(buckets) + b * 16;
        if (!NearbyIsReadable(bkt, 16)) continue;
        void* tail = *reinterpret_cast<void**>(bkt);
        void* node = *reinterpret_cast<void**>(bkt + 8);
        size_t hops = 0;
        while (node && node != sentinel && hops++ < 256) {
            if (!NearbyIsReadable(node, 0x20)) return false;
            const u64 k = *reinterpret_cast<const u64*>(
                reinterpret_cast<unsigned char*>(node) + 0x10);
            if (k == key) return true;
            if (node == tail) break;
            node = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(node) + 0x08);
        }
    }
    return false;
}

// 诊断：对单台机器查组件是否已注册进 +0x2B8 空间索引 map（只读）
static void PaDiagSpatialRegistration(void* status, u64 gid) {
    if (!status) return;
    void* comp = nullptr;
    if (NearbyIsReadable(reinterpret_cast<unsigned char*>(status) + 0x338,
                         sizeof(void*)))
        comp = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(status) + 0x338);
    int inProc = 0;
    PaReadStatusInt(status, "inProc", &inProc);
    bool registered = comp
        ? PaSpatialMapHasKey(status, 0x2B8,
                             static_cast<u64>(reinterpret_cast<uintptr_t>(comp)))
        : false;
    Log("[PA] spatialdiag gid=%llu status=%p comp=%p inProc=%d "
        "comp_in_spatial_map=%d\n",
        static_cast<unsigned long long>(gid), status, comp, inProc,
        static_cast<int>(registered));
}

// ============================================================
// v1.5.49-diag：status map 全键 dump（不依赖 hash 寻址，全桶扫描）
//   map 布局：+0x8 sentinel / +0x18 buckets / +0x30 mask
//   桶 16B（+0 链尾, +8 链头）；节点 +8=next, +0x10=key(8B packed), +0x18=value
//   用途：把"游戏手动投料写入的键"与"PA 直写后的键"逐键对比，
//   找出 UI 显示所需但 PA 没写的键。
// ============================================================
static void PaDumpStatusMap(void* status, uintptr_t mapOffset, bool isInt,
                            u64 gid) {
    if (!status) return;
    unsigned char* map = reinterpret_cast<unsigned char*>(status) + mapOffset;
    if (!NearbyIsReadable(map, 0x38)) return;
    void* sentinel = *reinterpret_cast<void**>(map + 0x08);
    void* buckets = *reinterpret_cast<void**>(map + 0x18);
    const u64 mask = *reinterpret_cast<const u64*>(map + 0x30);
    if (!sentinel || !buckets || mask == 0 || mask > 0x10000ULL) return;
    const size_t bucketCount = static_cast<size_t>(mask) + 1;

    char line[768] = {};
    int off = 0;
    size_t count = 0;
    for (size_t b = 0; b < bucketCount && count < 64; ++b) {
        unsigned char* bkt = reinterpret_cast<unsigned char*>(buckets) + b * 16;
        if (!NearbyIsReadable(bkt, 16)) continue;
        void* tail = *reinterpret_cast<void**>(bkt);
        void* node = *reinterpret_cast<void**>(bkt + 8);
        size_t hops = 0;
        // v1.5.72：链尾节点也要检查（原条件 node != tail 会漏掉最后一个节点）
        while (node && node != sentinel && hops++ < 256 && count < 64) {
            if (!NearbyIsReadable(node, 0x20)) break;
            const u64 key =
                *reinterpret_cast<const u64*>(
                    reinterpret_cast<unsigned char*>(node) + 0x10);
            const u64 val =
                *reinterpret_cast<const u64*>(
                    reinterpret_cast<unsigned char*>(node) + 0x18);
            char k[9] = {};
            memcpy(k, &key, 8);
            for (int i = 0; i < 8; ++i) {
                if (k[i] == 0) { k[i] = 0; continue; }
                if (k[i] < 0x20 || k[i] > 0x7e) k[i] = '?';
            }
            int written = isInt
                ? _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE,
                              "%s=%d ", k, static_cast<int>(val))
                : _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE,
                              "%s=%llu ", k,
                              static_cast<unsigned long long>(val));
            if (written < 0) break;
            off += written;
            ++count;
            if (node == tail) break;
            node = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(node) + 0x08);
        }
        if (off > 700) break;
    }
    Log("[PA] mapdump gid=%llu off=0x%X n=%zu %s\n",
        static_cast<unsigned long long>(gid), static_cast<unsigned>(mapOffset),
        count, line);

    // v1.5.50-diag：同一时刻用 PA 的读取函数对照，区分"遍历漏节点"与
    //   "写入根本没进这个 map"
    {
        u64 probeId = 0, probeId1 = 0;
        int probeVal = 0, probeProc = 0;
        PaReadStatusU64(status, "itemID0", &probeId);
        PaReadStatusU64(status, "itemID1", &probeId1);
        PaReadStatusInt(status, "itemVal0", &probeVal);
        PaReadStatusInt(status, "inProc", &probeProc);
        Log("[PA] mapread gid=%llu itemID0=%llu itemID1=%llu itemVal0=%d "
            "inProc=%d\n",
            static_cast<unsigned long long>(gid),
            static_cast<unsigned long long>(probeId),
            static_cast<unsigned long long>(probeId1), probeVal, probeProc);
    }
}

// ============================================================
// 主调度（NearbyMainUpdate 顶层回调触发，5s 扫描）
// ============================================================
static ULONGLONG g_lastScanTick = 0;
static ULONGLONG g_firstScanAt = 0;
static bool g_firstScanDone = false;

static void PaTick(const PaWorldContext* ctx) {
    if (!ctx || !ctx->save) return;
    g_paCurrentSave = ctx->save;

    // ============================================================
    // v1.6.45-diag【面板存在性探针】—— 屏幕取证对不上时，把「面板在不在」
    //   变成可读日志。玩家 100px 内每台机器，每 100ms 采样一次 UI 三键的
    //   「存在性 + 是否非 0」，签名变化即打一条带毫秒的日志：
    //     [PA][panel] gid=.. t=.. UIPros=1/0 UIManpu=1/125613 UIPrsFar=1/4388 sig 7->15
    //   若日志里 sig 在快速翻转 → 面板确实在反复开关（闪），翻转时刻可直接
    //   对齐到当时 PA/游戏在做什么；若 sig 恒定 → 「闪」不在键状态层面，
    //   是纯渲染问题，结论立刻明确，不再猜。
    // ============================================================
    {
        static ULONGLONG s_lastProbe = 0;
        const ULONGLONG nowP = GetTickCount64();
        if (nowP - s_lastProbe >= 100ULL && g_paPlayerValid) {
            s_lastProbe = nowP;
            struct PaPanelPrev { u64 gid; u64 sig; };
            static PaPanelPrev s_prev[8] = {};
            for (size_t i = 0; i < PA_MAX_ENTRIES; ++i) {
                PaGimmickEntry* e = &g_entries[i];
                if (!e->gimmickId || !e->isMachine || !e->status) continue;
                const float dx = g_paPlayerX - e->x;
                const float dy = g_paPlayerY - e->y;
                if (dx * dx + dy * dy > 100.0f * 100.0f) continue;

                u64 up = 0, um = 0, uf = 0;
                const bool hp = PaStatusKeyPresent(e->status, "UIPros", &up);
                const bool hm = PaStatusKeyPresent(e->status, "UIManpu", &um);
                const bool hf = PaStatusKeyPresent(e->status, "UIPrsFar", &uf);
                const u64 sig = (hp ? 1ULL : 0) | (hm ? 2ULL : 0) |
                                (hf ? 4ULL : 0) | (up ? 8ULL : 0) |
                                (um ? 16ULL : 0) | (uf ? 32ULL : 0);
                PaPanelPrev* pv = nullptr;
                for (int k = 0; k < 8; ++k)
                    if (s_prev[k].gid == e->gimmickId) { pv = &s_prev[k]; break; }
                if (!pv) {
                    for (int k = 0; k < 8; ++k)
                        if (s_prev[k].gid == 0) { pv = &s_prev[k]; break; }
                    if (!pv) continue;
                    pv->gid = e->gimmickId;
                    pv->sig = sig;
                    Log("[PA][panel] gid=%llu 初始化 sig=%llu "
                        "UIPros=%d/%llu UIManpu=%d/%llu UIPrsFar=%d/%llu\n",
                        static_cast<unsigned long long>(e->gimmickId),
                        static_cast<unsigned long long>(sig),
                        hp ? 1 : 0, static_cast<unsigned long long>(up),
                        hm ? 1 : 0, static_cast<unsigned long long>(um),
                        hf ? 1 : 0, static_cast<unsigned long long>(uf));
                    continue;
                }
                if (pv->sig != sig) {
                    Log("[PA][panel] gid=%llu t=%llu sig %llu->%llu "
                        "UIPros=%d/%llu UIManpu=%d/%llu UIPrsFar=%d/%llu\n",
                        static_cast<unsigned long long>(e->gimmickId),
                        static_cast<unsigned long long>(nowP),
                        static_cast<unsigned long long>(pv->sig),
                        static_cast<unsigned long long>(sig),
                        hp ? 1 : 0, static_cast<unsigned long long>(up),
                        hm ? 1 : 0, static_cast<unsigned long long>(um),
                        hf ? 1 : 0, static_cast<unsigned long long>(uf));
                    pv->sig = sig;
                }

                // v1.6.45-diag 之二【进度值探针】每 500ms 把 3 个槽的
                //   (start, duration, 实时进度%) 打出来。目的是把「面板上画的
                //   进度条」变成可读数字：
                //   · 进度单调递增 → 面板内容正常，闪是纯渲染问题；
                //   · 进度抖动/回退/负数/超界 → 槽数据本身在变，直接就是闪的根因。
                {
                    static ULONGLONG s_lastProg = 0;
                    const ULONGLONG nowQ = GetTickCount64();
                    if (nowQ - s_lastProg >= 500ULL) {
                        s_lastProg = nowQ;
                        char buf[220] = {};
                        int off = 0;
                        for (size_t sl = 0; sl < 3; ++sl) {
                            u64 st = 0, du = 0;
                            PaReadSlotTimers(e->status, sl, &st, &du);
                            const long long gs = static_cast<long long>(ctx->gameSecond);
                            long long prog = -1;
                            if (du) prog = (gs - static_cast<long long>(st)) * 100 /
                                           static_cast<long long>(du);
                            off += _snprintf_s(buf + off, sizeof(buf) - off,
                                               _TRUNCATE, " s%zu=%llu/%llu/%lld",
                                               sl,
                                               static_cast<unsigned long long>(st),
                                               static_cast<unsigned long long>(du),
                                               prog);
                            if (off >= (int)sizeof(buf) - 8) break;
                        }
                        Log("[PA][prog] gid=%llu now=%lld%s\n",
                            static_cast<unsigned long long>(e->gimmickId),
                            static_cast<long long>(ctx->gameSecond), buf);
                    }
                }
            }
        }
    }

    // v1.5.41：存在 qol_pa_inputoff.txt 时暂停一切自动投/取，
    //   便于用探针抓到"玩家手动投料"时游戏自己写的干净前后状态。
    {
        static int s_checked = 0;
        static int s_paused = 0;
        if (s_checked == 0) {
            s_checked = 1;
            wchar_t path[MAX_PATH] = {};
            if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
                wchar_t* slash = wcsrchr(path, L'\\');
                if (slash) {
                    *(slash + 1) = 0;
                    wcscat_s(path, MAX_PATH, L"qol_pa_inputoff.txt");
                    s_paused = (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES);
                }
            }
            Log("[PA] automation %s (qol_pa_inputoff.txt)\n",
                s_paused ? "PAUSED for capture" : "active");
        }
        g_paAutomationPaused = s_paused;
    }
    // v1.5.46-diag：两个诊断 flag（启动后首次 PaTick 读取一次）
    {
        static int s_diagChecked = 0;
        if (s_diagChecked == 0) {
            s_diagChecked = 1;
            wchar_t dir[MAX_PATH] = {};
            if (GetModuleFileNameW(nullptr, dir, MAX_PATH)) {
                wchar_t* slash = wcsrchr(dir, L'\\');
                if (slash) {
                    *(slash + 1) = 0;
                    wchar_t p1[MAX_PATH] = {};
                    wcscpy_s(p1, MAX_PATH, dir);
                    wcscat_s(p1, MAX_PATH, L"qol_pa_pushprobe.txt");
                    g_paPushProbe =
                        (GetFileAttributesW(p1) != INVALID_FILE_ATTRIBUTES);
                    wchar_t p2[MAX_PATH] = {};
                    wcscpy_s(p2, MAX_PATH, dir);
                    wcscat_s(p2, MAX_PATH, L"qol_pa_fasttest.txt");
                    g_paFastTest =
                        (GetFileAttributesW(p2) != INVALID_FILE_ATTRIBUTES);
                    wchar_t p3[MAX_PATH] = {};
                    wcscpy_s(p3, MAX_PATH, dir);
                    wcscat_s(p3, MAX_PATH, L"qol_pa_dumpkeys.txt");
                    g_paDumpKeys =
                        (GetFileAttributesW(p3) != INVALID_FILE_ATTRIBUTES);
                    wchar_t p4[MAX_PATH] = {};
                    wcscpy_s(p4, MAX_PATH, dir);
                    wcscat_s(p4, MAX_PATH, L"qol_pa_nativepush.txt");
                    g_paNativePushTest =
                        (GetFileAttributesW(p4) != INVALID_FILE_ATTRIBUTES);
                    // v1.6.61【单一事实源】旧实现把「qol_pa_pushfeed.txt 是否存在」
                    // 当作唯一开关，而声明默认值是 1；发布包/实机里该文件通常
                    // 不存在 -> 首次 PaTick 覆盖成 0 -> 整块 push 投料链不可达
                    // （2026-11-07 16:32 实机日志 pushfeed=0，炭窑/高炉零投料）。
                    // 现改为：默认启用；只有显式放 qol_pa_pushfeed_off.txt 才关闭。
                    // 回滚无需重编：放这个 off 文件即恢复旧行为。
                    wchar_t p6[MAX_PATH] = {};
                    wcscpy_s(p6, MAX_PATH, dir);
                    wcscat_s(p6, MAX_PATH, L"qol_pa_pushfeed_off.txt");
                    g_paUsePushFeed =
                        (GetFileAttributesW(p6) == INVALID_FILE_ATTRIBUTES) ? 1 : 0;
                    PaTxObsPublish();
                }
            }
            Log("[PA] diag flags: pushprobe=%d fasttest=%d dumpkeys=%d "
                "nativepush=%d pushfeed=%d%s touchui=%d\n",
                g_paPushProbe, g_paFastTest, g_paDumpKeys,
                g_paNativePushTest, g_paUsePushFeed,
                g_paUsePushFeed
                    ? " (feed chain ENABLED)"
                    : " (DISABLED by qol_pa_pushfeed_off.txt)",
                g_paTouchUi);
        }
    }
    // ⚠ 2026-10-29 修正：PaInstallApiProbe() **是无条件调用的**，其中 near/far
    //   两个 hook 也**无条件安装**（首条指令 5 字节，实测安全）；函数内部只有
    //   末尾的 push/pop/load 诊断探针才受 qol_pa_apiprobe.txt 控制。
    //   旧注释写"仅当存在 flag 文件时挂钩"是错的，会误导排查（以为没 hook）。
    //   ⇒ 这三个 UI RVA 一旦为 0，HookInstall(base+0) 会失败 → near/far 探针
    //     不装 → g_paBaseIdFar 捕获不到 → 远处小进度条彻底失效。
    PaInstallApiProbe();
    // v1.5.92：已移除 v1.5.91 的判活 inline hook（重复安装导致崩溃）。
    //   动画取证已于 v1.5.118 收口归档（见进度文件/PA加工动画专题_逆向地图与收口结论_20261027.md）。
    const ULONGLONG now = GetTickCount64();

    if (g_firstScanAt == 0) {
        g_firstScanAt = now;
        Log("[PA] first scan armed (3s delay)\n");
        return;
    }
    if (!g_firstScanDone &&
        now - g_firstScanAt < PA_FIRST_SCAN_DELAY_MS) return;
    if (!g_firstScanDone) {
        g_firstScanDone = true;
        Log("[PA] first scan fired\n");
    }
    if (now - g_lastScanTick < PA_SCAN_INTERVAL_MS) return;
    g_lastScanTick = now;

    // v1.5.49-diag：dumpkeys 开启时每 10 秒全量 dump 一轮所有机器的 status map
    bool dumpThisScan = false;
    {
        static ULONGLONG s_lastDump = 0;
        if (g_paDumpKeys && now - s_lastDump >= 5000) {
            s_lastDump = now;
            dumpThisScan = true;
        }
    }

    // v1.5.73：UI 触碰节流。
    // v1.6.14【错位节流修复】v1.6.11 把这里改成 1 秒，但 PaTick 的扫描本身
    //   是 **5 秒一次**（日志 scan: entries 每 5 秒一条）——1 秒节流与 5 秒扫描
    //   错位：5 秒里只有 1 秒为 true，那一次又不一定落在扫描上 → 大部分扫描
    //   直接跳过维护链 → 实测 16:42 整轮 uimaint **一条都没打**，
    //   「离开后重建 far」与「周期刷新」全部变成死代码。
    //   现改为：跟随扫描节奏，每次扫描都维护（5 秒一次 × 每台加工中机器，
    //   开销就是几次 status map 查找，可忽略）。
    const bool uiTouchThisScan = true;

    // 槽 1+2：枚举 + flood-fill 建组
    const size_t entryCount = PaEnumerateGimmicks(ctx->save);
    PaBuildGroups(entryCount);
    Log("[PA] scan: entries=%zu groups=%zu\n", entryCount, g_groupCount);

    // v1.5.21：组构成 + 孤立机器，**按拓扑变化重打**（箱子/机器动了就重出）
    //   旧版一次性打印 → 玩家中途挪箱子看不到新结果，误以为"箱子在隔壁却没连"。
    {
        static u64 s_lastTopoHash = 0;
        u64 h = 1469598103934665603ull;
        for (size_t j = 0; j < entryCount; ++j) {
            const u64 mix = (g_entries[j].gimmickId * 1000003ull) ^
                            (static_cast<u64>(static_cast<int>(g_entries[j].x)) << 20) ^
                            static_cast<u64>(static_cast<int>(g_entries[j].y)) ^
                            (g_entries[j].isChest ? 0x9E3779B9ull : 0ull);
            h = (h ^ mix) * 1099511628211ull;
        }
        h ^= static_cast<u64>(g_groupCount);
        if (h != s_lastTopoHash) {
            s_lastTopoHash = h;
            Log("[PA] topology changed: entries=%zu groups=%zu\n", entryCount,
                g_groupCount);
            for (size_t gi = 0; gi < g_groupCount; ++gi) {
                Log("[PA] groupdiag #%zu machines=%zu chests=%zu\n", gi,
                    g_groups[gi].machineCount, g_groups[gi].chestCount);
                for (size_t m = 0; m < g_groups[gi].machineCount; ++m) {
                    for (size_t j = 0; j < entryCount; ++j) {
                        if (g_entries[j].isMachine &&
                            PaIdEqual(g_groups[gi].machines[m],
                                      g_entries[j].id)) {
                            Log("[PA] groupdiag machine gid=%llu pos=(%.0f,%.0f)\n",
                                static_cast<unsigned long long>(
                                    g_entries[j].gimmickId),
                                g_entries[j].x, g_entries[j].y);
                            break;
                        }
                    }
                }
                for (size_t c = 0; c < g_groups[gi].chestCount; ++c) {
                    for (size_t j = 0; j < entryCount; ++j) {
                        if (g_entries[j].isChest &&
                            PaIdEqual(g_groups[gi].chests[c],
                                      g_entries[j].id)) {
                            Log("[PA] groupdiag chest gid=%llu pos=(%.0f,%.0f)\n",
                                static_cast<unsigned long long>(
                                    g_entries[j].gimmickId),
                                g_entries[j].x, g_entries[j].y);
                            break;
                        }
                    }
                }
            }
            size_t orphanCount = 0;
            for (size_t j = 0; j < entryCount; ++j) {
                if (!g_entries[j].isMachine) continue;
                bool inGroup = false;
                for (size_t gi = 0; gi < g_groupCount && !inGroup; ++gi) {
                    for (size_t m = 0; m < g_groups[gi].machineCount; ++m) {
                        if (PaIdEqual(g_groups[gi].machines[m],
                                      g_entries[j].id)) {
                            inGroup = true;
                            break;
                        }
                    }
                }
                if (inGroup) continue;
                char mod[48] = {};
                PaReadGimmickModule(g_entries[j].status, mod, sizeof(mod));
                ++orphanCount;
                Log("[PA] orphan machine gid=%llu mod=%s pos=(%.0f,%.0f)\n",
                    static_cast<unsigned long long>(g_entries[j].gimmickId),
                    mod[0] ? mod : "?", g_entries[j].x, g_entries[j].y);
            }
            Log("[PA] groupdiag orphan_machines=%zu\n", orphanCount);

            PaDiagOrphanNeighbors(ctx->save, entryCount);
            // v1.5.24：机器槽内材料 dump；v1.5.30 蚕盒/织布机优先
            {
                size_t dumped = 0;
                const u64 kPriority[] = {240270000ULL, 240110000ULL,
                                         240110100ULL};
                for (size_t pi = 0;
                     pi < sizeof(kPriority) / sizeof(u64) && dumped < 12; ++pi) {
                    for (size_t j = 0; j < entryCount && dumped < 12; ++j) {
                        if (!g_entries[j].isMachine || !g_entries[j].status)
                            continue;
                        ++dumped;
                    }
                }
                for (size_t j = 0; j < entryCount && dumped < 12; ++j) {
                    if (!g_entries[j].isMachine || !g_entries[j].status)
                        continue;
                    const u64 gid = g_entries[j].gimmickId;
                    if (gid == 240270000ULL || gid == 240110000ULL ||
                        gid == 240110100ULL)
                        continue;
                    ++dumped;
                }
            }
        }
        // v1.5.119【孤儿子机可见化】原实现只在拓扑哈希变化时打印孤儿列表，
        //   箱子布局不变时 23 台孤儿机器长期静默 → 排查时完全瞎。
        //   这里每 60 秒打一次汇总：孤儿台数 + 每台到最近箱子的距离与方位
        //   （距离能直接区分「差 1 格」与「差 6 格」，方位能区分斜角/正交）。
        //   纯读，不改任何状态。
        {
            static ULONGLONG s_lastOrphanReport = 0;
            const ULONGLONG nowO = GetTickCount64();
            if (nowO - s_lastOrphanReport >= 60000ULL) {
                s_lastOrphanReport = nowO;
                size_t nOrphan = 0;
                for (size_t j = 0; j < entryCount; ++j) {
                    if (!g_entries[j].isMachine) continue;
                    bool inGroup = false;
                    for (size_t gi = 0; gi < g_groupCount && !inGroup; ++gi) {
                        for (size_t m = 0; m < g_groups[gi].machineCount; ++m) {
                            if (PaIdEqual(g_groups[gi].machines[m],
                                          g_entries[j].id)) {
                                inGroup = true;
                                break;
                            }
                        }
                    }
                    if (inGroup) continue;
                    // 找最近箱子
                    float bestD = 1e30f;
                    float bdx = 0.0f, bdy = 0.0f;
                    for (size_t c = 0; c < entryCount; ++c) {
                        if (!g_entries[c].isChest) continue;
                        const float dx = fabsf(g_entries[c].x - g_entries[j].x);
                        const float dy = fabsf(g_entries[c].y - g_entries[j].y);
                        const float d = dx + dy;
                        if (d < bestD) {
                            bestD = d;
                            bdx = dx;
                            bdy = dy;
                        }
                    }
                    ++nOrphan;
                    Log("[PA] orphan gid=%llu pos=(%.0f,%.0f) nearestChest"
                        " dist=(%.0f,%.0f) sum=%.0f%s%s\n",
                        static_cast<unsigned long long>(
                            g_entries[j].gimmickId),
                        g_entries[j].x, g_entries[j].y, bdx, bdy, bestD,
                        (bdx < 2.0f && bdy < 2.0f) ? " [ORTHO]"
                                                    : ((bdx < 46.0f && bdy < 46.0f)
                                                       ? " [DIAG]" : " [FAR]"),
                        (bestD > 1e29f) ? " (无箱)" : "");
                }
                Log("[PA] orphan summary: %zu/%zu 台机器无箱接管\n",
                    nOrphan, entryCount);
            }
        }
    }
    if (g_groupCount == 0) return;
    // v1.5.52-diag：暂停取证模式下也要 dump——用来抓"玩家手动投料时游戏
    //   自己写了哪些键"，再与 PA 直写后的键值逐项对比（UI 面板空槽的定位）。
    if (g_paAutomationPaused && dumpThisScan) {
        for (size_t j = 0; j < entryCount; ++j) {
            if (!g_entries[j].isMachine || !g_entries[j].status) continue;
            PaDumpStatusMap(g_entries[j].status, 0x18, true,
                            g_entries[j].gimmickId);
            PaDumpStatusMap(g_entries[j].status, 0x58, false,
                            g_entries[j].gimmickId);
            // v1.5.66：组件字段快照（手动投料后哪些字段变非零）
            unsigned char* sb =
                reinterpret_cast<unsigned char*>(g_entries[j].status);
            if (NearbyIsReadable(sb + 0x320, 0x50)) {
                Log("[PA] compdump gid=%llu "
                    "f320=%p f328=%p f330=%p f338=%p f340=%p "
                    "f348=%p f350=%p f358=%p f360=%p\n",
                    static_cast<unsigned long long>(g_entries[j].gimmickId),
                    *reinterpret_cast<void**>(sb + 0x320),
                    *reinterpret_cast<void**>(sb + 0x328),
                    *reinterpret_cast<void**>(sb + 0x330),
                    *reinterpret_cast<void**>(sb + 0x338),
                    *reinterpret_cast<void**>(sb + 0x340),
                    *reinterpret_cast<void**>(sb + 0x348),
                    *reinterpret_cast<void**>(sb + 0x350),
                    *reinterpret_cast<void**>(sb + 0x358),
                    *reinterpret_cast<void**>(sb + 0x360));
                // v1.5.67：看 0x338 指向对象的结构（是否反向指向 status）
                void* f338 = *reinterpret_cast<void**>(sb + 0x338);
                if (f338 && NearbyIsReadable(f338, 0x48)) {
                    void** obj = reinterpret_cast<void**>(f338);
                    // v1.5.82：同时打印 vtable 的【静态 RVA】——组件对象只在
                    //   玩家手工点火后出现（KB-082），静态库里查不到构造点，
                    //   必须靠运行时抓 vtable 再去反查引用。绝对地址无基址
                    //   无法换算，故这里直接算好 RVA 打出来。
                    const uintptr_t modBase = reinterpret_cast<uintptr_t>(
                        GetModuleHandleW(nullptr));
                    const uintptr_t vtAbs =
                        reinterpret_cast<uintptr_t>(obj[0]);
                    Log("[PA] compobj gid=%llu status=%p obj=%p vtable=%p "
                        "vtableRva=%#llX o8=%p o10=%p o18=%p o20=%p o28=%p "
                        "o30=%p o38=%p o40=%p\n",
                        static_cast<unsigned long long>(g_entries[j].gimmickId),
                        g_entries[j].status, f338, obj[0],
                        static_cast<unsigned long long>(
                            modBase ? (vtAbs - modBase) : 0),
                        obj[1], obj[2], obj[3], obj[4], obj[5], obj[6],
                        obj[7], obj[8]);
                }
            }
        }
    }
    if (g_paAutomationPaused) return;   // v1.5.41 取证模式：不做投取，只留探针

    // 槽 3+4+5：每组内逐台机器 过滤 → 结算 → 幂等
    for (size_t i = 0; i < g_groupCount; ++i) {
        PaGroup* g = &g_groups[i];
        // 解析组内箱子 status 数组（预留 PA_FUEL_BORROW_MAX 个借位）
        void* chestStatuses[PA_MAX_CHESTS_PER_GROUP + PA_FUEL_BORROW_MAX] = {};
        size_t resolvedChests = 0;
        for (size_t c = 0; c < g->chestCount; ++c) {
            for (size_t j = 0; j < entryCount; ++j) {
                if (PaIdEqual(g_entries[j].id, g->chests[c])) {
                    chestStatuses[resolvedChests++] = g_entries[j].status;
                    break;
                }
            }
        }

        // v1.6.7【燃料借用】见 PA_FUEL_BORROW_MAX 处注释。放在
        //   resolvedChests==0 的提前返回之前 —— 孤立机器（本组无箱）
        //   恰恰最需要从旁边借燃料。
        {
            size_t borrowed = 0;
            for (size_t j = 0; j < entryCount &&
                 borrowed < PA_FUEL_BORROW_MAX; ++j) {
                PaGimmickEntry* cand = &g_entries[j];
                if (!cand->isChest || !cand->status) continue;
                bool inGroup = false;
                for (size_t c = 0; c < g->chestCount; ++c) {
                    if (PaIdEqual(cand->id, g->chests[c])) {
                        inGroup = true;
                        break;
                    }
                }
                if (inGroup) continue;
                // 到本组最近机器的平方距离（网格步长 45，4 格 = 180）
                float best2 = 1.0e30f;
                for (size_t mm = 0; mm < g->machineCount; ++mm) {
                    for (size_t e = 0; e < entryCount; ++e) {
                        if (!g_entries[e].isMachine) continue;
                        if (!PaIdEqual(g_entries[e].id, g->machines[mm]))
                            continue;
                        const float dx = cand->x - g_entries[e].x;
                        const float dy = cand->y - g_entries[e].y;
                        const float d2 = dx * dx + dy * dy;
                        if (d2 < best2) best2 = d2;
                        break;
                    }
                }
                if (best2 > PA_FUEL_BORROW_RADIUS2) continue;
                if (!PaChestHasFuel(cand->status)) continue;
                chestStatuses[resolvedChests++] = cand->status;
                ++borrowed;
                Log("[PA] fuel-borrow: 组%zu 借用箱 gid=%llu pos=(%.0f,%.0f) "
                    "dist2=%.0f（本组箱子不含燃料）\n",
                    i, static_cast<unsigned long long>(cand->gimmickId),
                    static_cast<double>(cand->x), static_cast<double>(cand->y),
                    static_cast<double>(best2));
            }
        }
        if (resolvedChests == 0) continue;

        for (size_t m = 0; m < g->machineCount; ++m) {
            PaGimmickEntry* machine = nullptr;
            for (size_t j = 0; j < entryCount; ++j) {
                if (PaIdEqual(g_entries[j].id, g->machines[m])) {
                    machine = &g_entries[j];
                    break;
                }
            }
            if (!machine) continue;

            // v1.5.28：撤回 v1.5.27 的"每台机器必须自己贴箱子"。
            // 玩家澄清的正确语义 = 泛洪（flood-fill）：
            //   · 机器/箱子按 1 格邻接连成连通块；中间隔一格（2 格）即断开
            //   · 连通块内有 ≥1 个箱子 → 整块机器都由该块箱子供料
            //   · 一组上限 64 台机器（PA_MAX_MACHINES_PER_GROUP）
            // 因此这里不做单机邻箱校验，连通性完全交给 PaBuildGroups。

            // 防御（v1.5.8）：机器 gimmickId 必须在本 MOD 配方表中有索引，
            // 否则安全跳过（不投料/不取料）。玩家反馈"肥料桶取料崩溃"——
            // 某些 composter 变体/无配方机器进入取料会触 outputHelper 崩溃。
            if (!PaIsMachineId(machine->gimmickId)) {
                Log("[PA] skip: machine gid=%llu no recipe index\n",
                    static_cast<unsigned long long>(machine->gimmickId));
                continue;
            }

            // v1.5.34【紧急】被动机器（蚕盒/蜂箱/采汁器）暂停自动化：
            //   它们的投料/产出语义与 timer 机器不同：写入 itemID/itemVal 后
            //   会被自身的"无 timer 产出"判定读回 → 凭空造出物品（复制）。
            //   在确认原生接口与 backing 语义之前一律跳过，防止污染存档。
            if (PaIsPassiveMachine(machine->gimmickId)) {
                static std::atomic<int> s_passiveLog{0};
                if (s_passiveLog.fetch_add(1, std::memory_order_relaxed) < 8) {
                    Log("[PA] skip passive machine gid=%llu pos=(%.0f,%.0f) "
                        "（被动机器自动化暂停，防物品复制）\n",
                        static_cast<unsigned long long>(machine->gimmickId),
                        machine->x, machine->y);
                }
                continue;
            }

            // v1.5.39：先清掉历史遗留的幻影槽（否则该机器会卡死）
            PaSanitizePhantomSlots(machine->status, machine->gimmickId);

            // v1.5.73：加工中的机器定期触碰 UI 同步键（独立 10s 节流，
            //   不再依赖 dumpkeys）。push 是离屏调用，游戏侧 UI 未必收到
            //   刷新；这里按扫描轮次把 UIManpu +1 作为重绘信号。
            // v1.5.87：加工中机器每轮维护 far/manpu（修复"靠近再离开小进度条消失"）
            if (uiTouchThisScan && g_paTouchUi) {
                // v1.5.122：门禁放宽 —— 原用 inProc 键判加工中，但 PA 原生 push
                //   路径**不写 inProc**（Ghidra 实锤：FUN_14027be10 只写
                //   itemID%d/itemVal%d；4 条 gamescript 包装也都不写）。
                //   实测证据：PA 投料成功后 inProc 恒读不到 → 维护链整条被挡死
                //   → far UI 永不重建 → 走远后小 UI 进度消失。
                //   改用**槽 timer** 判加工中（与 slotdiag 同源判据，本次日志
                //   已证明其正确：running=3 而 inProc=0）。
                //   保留 inProc 作为「或」条件：游戏自己投的机器两条路都通。
                int inProc = 0;
                const bool inProcOk =
                    PaReadStatusInt(machine->status, "inProc", &inProc) &&
                    inProc != 0;
                if (inProcOk || PaMachineHasRunningSlot(machine->status)) {
                    PaMaintainProcessingUi(machine->status, machine->gimmickId);
                }
            }

            // v1.6.21【动画实例表·tick 级采样】不依赖「加工中」门禁。
            //   维护链只在 inProc / 槽 timer 成立时才跑，机器空闲时 obs 里读不到任何
            //   动画数据 —— 而「游戏到底有没有注册动画实例」正是排查"无动画"最需要的
            //   事实。这里做 5 秒一次的全局采样（与具体机器无关，owner 传 null）：
            //     animInstGlobal == 0   → 引擎层一个动画实例都没有（创建/装配侧问题）
            //     animInstGlobal > 0    → 实例存在，问题在渲染或 owner 绑定
            {
                static ULONGLONG s_lastAnimSample = 0;
                const ULONGLONG nowA = GetTickCount64();
                if (nowA - s_lastAnimSample >= 5000ULL) {
                    s_lastAnimSample = nowA;
                    int gi = 0, ga = 0, gh = 0;
                    PaAnimTableSample(nullptr, &gi, &ga, &gh);
                    qol::obs::SetInt("pa", "animInstGlobal", gi);
                    qol::obs::SetInt("pa", "animActiveGlobal", ga);
                }
            }

            // v1.5.107：PA 机器 status 全键 dump（节流 90s，最多 8 台）
            //   目的：确认 baseID/objID/UIPros/UIManpu/UIPrsFar 等键的真实存在与
            //   取值 —— 当前 uifix 三条 UI 全返回 0，说明 PA 拿不到本机 baseID。
            //   拿到键清单即可确定「该用哪个字段」，无需再猜。
            {
                static struct { void* st; ULONGLONG t; } s_th[8] = {};
                static int s_cnt = 0;
                const ULONGLONG now = GetTickCount64();
                void* mst = machine->status;
                int idx = -1;
                for (int k = 0; k < s_cnt; ++k) {
                    if (s_th[k].st == mst) { idx = k; break; }
                }
                if (idx < 0 && s_cnt < 8) {
                    idx = s_cnt++;
                    s_th[idx].st = mst;
                    s_th[idx].t = 0;
                }
                if (idx >= 0 && (now - s_th[idx].t) >= 90000ULL) {
                    s_th[idx].t = now;
                    PaDumpStatusMap(mst, 0x18, true, machine->gimmickId);
                    PaDumpStatusMap(mst, 0x58, false, machine->gimmickId);
                }
            }

            // v1.5.49-diag：全键 dump（PA 写入了什么，一目了然）
            if (dumpThisScan) {
                PaDumpStatusMap(machine->status, 0x18, true,
                                machine->gimmickId);
                PaDumpStatusMap(machine->status, 0x58, false,
                                machine->gimmickId);
                // v1.5.65-diag：组件相关字段（手动投料 vs PA 投料对比用）
                unsigned char* sb =
                    reinterpret_cast<unsigned char*>(machine->status);
                if (NearbyIsReadable(sb + 0x320, 0x50)) {
                    Log("[PA] compdump gid=%llu "
                        "f320=%p f328=%p f330=%p f338=%p f340=%p "
                        "f348=%p f350=%p f358=%p f360=%p\n",
                        static_cast<unsigned long long>(machine->gimmickId),
                        *reinterpret_cast<void**>(sb + 0x320),
                        *reinterpret_cast<void**>(sb + 0x328),
                        *reinterpret_cast<void**>(sb + 0x330),
                        *reinterpret_cast<void**>(sb + 0x338),
                        *reinterpret_cast<void**>(sb + 0x340),
                        *reinterpret_cast<void**>(sb + 0x348),
                        *reinterpret_cast<void**>(sb + 0x350),
                        *reinterpret_cast<void**>(sb + 0x358),
                        *reinterpret_cast<void**>(sb + 0x360));
                }
            }

            PaState state = PaState::Idle;
            size_t readySlot = SIZE_MAX;
            bool hasIdle = false;
            if (!PaClassifyMachine(machine->status, ctx->gameSecond,
                                   &state, &readySlot, &hasIdle))
                continue;

            // v1.5.11-diag: 三槽按槽诊断（玩家反馈"只处理第一个就绪槽 +
            //   某些机器仍需手动投料"）。目的：区分两种成因——
            //   ①PaReadSlotLimit 读出来只有 1（PA 根本看不见 slot1/2）
            //   ②看得到 3 槽但每轮只取第一个 → 后续槽永不回头
            {
                // v1.6.6-diag：槽统计从"预算门内"提到门内（只多做几次
                //   内存读，成本可忽略），因为下面新增的 idle 诊断也要用它。
                size_t slotLim = 0;
                PaReadSlotLimit(machine->status, &slotLim);
                size_t readyCount = 0, runningCount = 0, idleCount = 0;
                for (size_t s2 = 0; s2 < slotLim && s2 < PA_MACHINE_SLOTS;
                     ++s2) {
                    u64 st = 0, du = 0;
                    if (!PaReadSlotTimers(machine->status, s2, &st, &du))
                        continue;
                    if (st == 0 || du == 0) { ++idleCount; continue; }
                    const std::int64_t gs = ctx->gameSecond;
                    if (gs >= static_cast<std::int64_t>(st) &&
                        gs - static_cast<std::int64_t>(st) >=
                            static_cast<std::int64_t>(du))
                        ++readyCount;
                    else
                        ++runningCount;
                }
                static std::atomic<int> s_slotDiag{0};
                if ((readyCount > 0 || runningCount > 0) &&
                    s_slotDiag.fetch_add(1, std::memory_order_relaxed) < 240) {
                    Log("[PA] slotdiag gid=%llu slotLimit=%zu "
                        "ready=%zu running=%zu idle=%zu pickedSlot=%zu "
                        "state=%d hasIdle=%d\n",
                        static_cast<unsigned long long>(machine->gimmickId),
                        slotLim, readyCount, runningCount, idleCount,
                        readySlot, static_cast<int>(state),
                        static_cast<int>(hasIdle));
                }
                // v1.6.6-diag【炭窑案】：纯 idle（ready/running 全 0）却
                //   hasIdle=0 的机器，在原日志里**完全不可见** —— 上面那条
                //   slotdiag 只在 ready/running 时打印，所以"有空槽却从不
                //   投料"的机器一条痕迹都没有。炭窑 240100000 就是这样被
                //   漏掉一整轮排查的。这里 per-gid 60 秒一条，并直接给出
                //   status+0x2b8（物品列表）与 +0x338（加工组件）指针，
                //   一眼可辨"列表未分配"还是"列表非空但不可读"。
                if (readyCount == 0 && runningCount == 0 && idleCount > 0 &&
                    !hasIdle) {
                    struct PaIdleDiag { u64 gid; u64 tick; bool used; };
                    static PaIdleDiag s_idleDiag[16] = {};
                    static size_t s_idleCursor = 0;
                    const u64 nowTick = GetTickCount64();
                    PaIdleDiag* entry = nullptr;
                    for (size_t k = 0; k < 16; ++k) {
                        if (s_idleDiag[k].used &&
                            s_idleDiag[k].gid == machine->gimmickId) {
                            entry = &s_idleDiag[k];
                            break;
                        }
                    }
                    if (!entry) {
                        entry = &s_idleDiag[s_idleCursor % 16];
                        ++s_idleCursor;
                        entry->used = true;
                        entry->gid = machine->gimmickId;
                        entry->tick = 0;
                    }
                    if (nowTick - entry->tick >= 60000ULL) {
                        entry->tick = nowTick;
                        unsigned char* sbI =
                            reinterpret_cast<unsigned char*>(machine->status);
                        const bool listReadable =
                            NearbyIsReadable(sbI + PA_ITEM_LIST, sizeof(void*));
                        void* backingPtr =
                            listReadable
                                ? *reinterpret_cast<void**>(sbI + PA_ITEM_LIST)
                                : nullptr;
                        void* compPtr =
                            NearbyIsReadable(sbI + 0x338, sizeof(void*))
                                ? *reinterpret_cast<void**>(sbI + 0x338)
                                : nullptr;
                        Log("[PA] slotdiag-idle gid=%llu slotLimit=%zu "
                            "idle=%zu hasIdle=0 listReadable=%d backing=%p "
                            "comp=%p <- 有空槽却未投料\n",
                            static_cast<unsigned long long>(machine->gimmickId),
                            slotLim, idleCount,
                            listReadable ? 1 : 0, backingPtr, compPtr);
                    }
                }
            }

            // v1.5.75-diag：UI 注册诊断——对比"有/无小进度条"的机器，
            //   看 status / 组件 / player+0x250（工厂里的 machineState）/
            //   各自 +0x2B8 容器的关系。dumpkeys 开启时每轮打印一次。
            if (dumpThisScan) {
                unsigned char* sbD = reinterpret_cast<unsigned char*>(
                    machine->status);
                void* comp338 = NearbyIsReadable(sbD + 0x338, sizeof(void*))
                                    ? *reinterpret_cast<void**>(sbD + 0x338)
                                    : nullptr;
                void* p250 = nullptr;
                PaReadPtr(ctx->save, 0x250, &p250);
                void* p250_2b8 = nullptr;
                if (p250) PaReadPtr(p250, 0x2B8, &p250_2b8);
                void* status2b8 = nullptr;
                PaReadPtr(machine->status, 0x2B8, &status2b8);
                // v1.5.77：组件"工作状态"字段对比（手动投料 vs push 投料）
                void* c0 = nullptr, *c8 = nullptr, *c10 = nullptr;
                void* c18 = nullptr, *c20 = nullptr, *c28 = nullptr;
                if (comp338 && NearbyIsReadable(comp338, 0x30)) {
                    void** cc = reinterpret_cast<void**>(comp338);
                    c0 = cc[0]; c8 = cc[1]; c10 = cc[2];
                    c18 = cc[3]; c20 = cc[4]; c28 = cc[5];
                }
                Log("[PA] uidbg gid=%llu status=%p comp=%p p250=%p "
                    "p250_2b8=%p status_2b8=%p | comp0=%p c8=%p c10=%p "
                    "c18=%p c20=%p c28=%p\n",
                    static_cast<unsigned long long>(machine->gimmickId),
                    machine->status, comp338, p250, p250_2b8, status2b8,
                    c0, c8, c10, c18, c20, c28);

                // v1.5.79-diag：只读查组件是否已注册进 +0x2B8 空间索引 map
                //   （FN-029 补充二决定性诊断：在→空操作；不在→才值得实现注册）
                PaDiagSpatialRegistration(machine->status,
                                          machine->gimmickId);
            }

            // v1.5.12：逐槽结算（原先每轮只处理 readySlot 一个槽 → 三槽机
            //   要三轮 5s 才轮完，且新就绪槽若排在后面会被反复跳过）。
            size_t slotLimit = 1;
            PaReadSlotLimit(machine->status, &slotLimit);
            if (slotLimit > PA_MACHINE_SLOTS) slotLimit = PA_MACHINE_SLOTS;

            if (state == PaState::Ready) {
                const bool passive = PaIsPassiveMachine(machine->gimmickId);
                for (size_t s = 0; s < slotLimit; ++s) {
                    if (passive) {
                        // v1.5.26：被动机器无 timer，按产出键判定
                        if (!PaSlotHasOutputNoTimer(machine->status, s))
                            continue;
                        PaTransferOutput(g, machine->status, s, chestStatuses,
                                         resolvedChests);
                        continue;
                    }
                    u64 st = 0, du = 0;
                    if (!PaReadSlotTimers(machine->status, s, &st, &du))
                        continue;
                    if (st == 0 || du == 0) continue;
                    const std::int64_t gs = ctx->gameSecond;
                    if (gs < static_cast<std::int64_t>(st)) continue;
                    if (gs - static_cast<std::int64_t>(st) <
                        static_cast<std::int64_t>(du)) continue;
                    PaTransferOutput(g, machine->status, s, chestStatuses,
                                     resolvedChests);
                }
            } else if (hasIdle) {
                // v1.6.38【闪修复·根因】玩家站在本机旁（UIPros 非 0）时不投料。
                //   取证：用户实测"每隔几秒闪一下"= 正好是 PA 补料周期；且闪的是
                //   PA 持续补料的高炉。PA 往玩家正在看的机器里塞料会改机器状态 →
                //   游戏 near 面板重建 → 大进度 UI 闪。手动投料只投一次（看到的是
                //   "料进去"而非常驻闪），PA 反复补料就反复闪。
                //   故：玩家在旁时跳过本机投料，走开（UIPros 归 0）后自动恢复补料。
                //   多台机器时只影响玩家正站的那一台，其余照常投料。
                u64 uiProsVal = 0;
                // v1.6.44【距离守卫】UIPros 在 1.30 靠近建面板时恒读 0（键缺失），
                //   原守卫永久失效。改为「UIPros 非 0 **或** 玩家实际距离 <100px」
                //   任一成立即视为在旁，跳过投料——在玩家眼皮底下改机器状态 =
                //   面板重建 = 闪（用户实测"靠近才闪、远离再靠近一会儿又开始"，
                //   正好是加工完成→取料→补料的周期）。
                const bool playerAtThisMachine =
                    g_paSkipWhenPlayerNear && (
                        (PaStatusKeyPresent(machine->status, "UIPros", &uiProsVal) &&
                         uiProsVal != 0) ||
                        PaPlayerNearMachine(machine->x, machine->y));
                if (!playerAtThisMachine) {
                    // v1.6.43【闪修复·最后一个周期源：失败投料冷却】
                    //   缺料机器（no recipe / push 失败）每 5 秒重试：每次都把物品
                    //   stage 进玩家背包槽→调 push→失败→清槽。背包被反复搅动，
                    //   玩家正开着的面板跟着刷新 → 闪。且多台缺料机的 5s 节奏错开
                    //   叠加，用户看到"先隔得久、然后越闪越快"。
                    //   失败 60 秒内不再试该机器（料不会自己长出来；放料后冷却
                    //   自然过期恢复）。
                    struct PaInputCool { u64 gid; u64 tick; };
                    static PaInputCool s_cool[32] = {};
                    static size_t s_cur = 0;
                    const u64 nowTick = GetTickCount64();
                    PaInputCool* e = nullptr;
                    for (size_t k = 0; k < 32; ++k)
                        if (s_cool[k].tick && s_cool[k].gid == machine->gimmickId) {
                            e = &s_cool[k]; break;
                        }
                    if (!e) {
                        e = &s_cool[s_cur % 32];
                        ++s_cur;
                        e->gid = machine->gimmickId;
                        e->tick = 0;
                    }
                    const bool cooled = e->tick && (nowTick - e->tick < 60000ULL);
                    if (!cooled) {
                        const bool hadFail = [&]() {
                            for (size_t attempt = 0; attempt < PA_MACHINE_SLOTS;
                                 ++attempt) {
                                if (!PaAttemptInput(g, machine->status,
                                                    chestStatuses, resolvedChests,
                                                    ctx->save, ctx->gameSecond))
                                    return true;
                            }
                            return false;
                        }();
                        e->tick = hadFail ? nowTick : 0;
                    }
                }
            }
        }
    }
}

// ============================================================
// NearbyMainUpdate 顶层回调入口（cpp 桩转发到这里）
// ============================================================
static void PaOnMainUpdate() {
    if (!g_paReady.load(std::memory_order_acquire)) return;
    PaWorldContext ctx = {};
    if (!PaGetWorldContext(&ctx)) return;
    PaTick(&ctx);
}

// ============================================================
// 安装入口
// ============================================================
// R12 guard 前向声明（定义在文件尾）
static bool InstallR12GuardHook();

static bool InstallProductionAutomation() {
    const uintptr_t base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) return false;

    g_paStatusIntLookup = reinterpret_cast<PaMapLookupFunction>(
        base + RVA_PRODUCTION_STATUS_INT_LOOKUP);
    g_paStatusU64Lookup = reinterpret_cast<PaMapLookupFunction>(
        base + RVA_PRODUCTION_STATUS_U64_LOOKUP);
    g_paEventLookup = reinterpret_cast<PaMapLookupFunction>(
        base + RVA_PRODUCTION_STATUS_EVENT_LOOKUP);
    g_paEventNotify = reinterpret_cast<PaEventNotifyFunction>(
        base + RVA_PRODUCTION_STATUS_EVENT_NOTIFY);
    g_paItemAllocate = reinterpret_cast<PaItemAllocateFunction>(
        base + RVA_PRODUCTION_ITEM_ALLOCATE);
    g_paItemCtor = reinterpret_cast<PaItemCtorFunction>(
        base + RVA_PRODUCTION_ITEM_CTOR);
    g_paItemSetCount = reinterpret_cast<PaItemSetCountFunction>(
        base + RVA_PRODUCTION_ITEM_SET_COUNT);
    // v1.5.46-diag：原生 production_push（仅 pushprobe flag 开启时使用）
    g_paPushNative =
        reinterpret_cast<PaNative4Raw>(base + RVA_PRODUCTION_PUSH);
    // v1.5.74：CCom_Gimmick 构造函数指针已移除（makecomp 实验证伪）
    g_paOutputHelper = reinterpret_cast<PaOutputHelperFunction>(
        base + RVA_PRODUCTION_OUTPUT_HELPER);
    g_paChestCommitRaw = reinterpret_cast<PaChestCommitRawFunction>(
        base + RVA_PRODUCTION_CHEST_COMMIT);
    // v1.5.85：加工 UI 三条 + baseID 校验（fail-open：绑不到就只是没有 UI 修复）
    g_paUiNear = reinterpret_cast<PaUiCreateFn>(base + RVA_PRODUCTION_UI_NEAR);
    g_paUiFar = reinterpret_cast<PaUiCreateFn>(base + RVA_PRODUCTION_UI_FAR);
    g_paUiManpu = reinterpret_cast<PaUiCreateFn>(base + RVA_PRODUCTION_UI_MANPU);
    g_paBaseIdResolve = reinterpret_cast<PaBaseIdResolveFn>(
        base + RVA_PRODUCTION_BASEID_RESOLVE);
    {
        // 只存 slot 地址；注册表指针在游戏早期为 NULL，运行时再解引用
        g_paBaseIdTableSlot = reinterpret_cast<void**>(
            base + RVA_PRODUCTION_BASEID_TABLE);
    }
    Log("[PA] install: uifix near=%p far=%p manpu=%p resolve=%p tableSlot=%p\n",
        reinterpret_cast<void*>(g_paUiNear), reinterpret_cast<void*>(g_paUiFar),
        reinterpret_cast<void*>(g_paUiManpu),
        reinterpret_cast<void*>(g_paBaseIdResolve), g_paBaseIdTableSlot);

    if (!g_paStatusIntLookup || !g_paStatusU64Lookup ||
        !g_paEventLookup || !g_paEventNotify || !g_paItemAllocate ||
        !g_paItemCtor || !g_paItemSetCount || !g_paOutputHelper ||
        !g_paChestCommitRaw) {
        Log("[PA] install: native function binding FAILED\n");
        return false;
    }

    // RTTI 校验（机器双 vtable）
    if (!CodeMsvcTypeMatches(base, RVA_PRODUCTION_GIMMICK_VTABLE,
                             RTTI_GIMMICK_COL, RTTI_GIMMICK_TYPE_DESCRIPTOR,
                             ".?AVCGimmickStatus@@")) {
        Log("[PA] install: RTTI gimmick type mismatch\n");
        return false;
    }

    // R12 guard hook（outputHelper 内部 R12=NULL 崩溃防护，fail-open）
    InstallR12GuardHook();

    g_paReady.store(true, std::memory_order_release);
    Log("[PA] install OK (v2 rewrite, no HUD)\n");
    return true;
}

// ============================================================
// R12 guard（方案B inline hook，旧版 v1.1.60 移植）
// output_helper 内部 RVA 0x1703C7 mov [rsp+0x30],r12 处注入 NULL 检查：
//   R12=0 → 跳 0x1709B9（游戏自身 NULL 退出路径，*outItem=0 + return）
//   R12≠0 → 原样执行。三次崩溃 dump 实锤 R12=0 → memcpy(NULL+off) 读崩
// ============================================================
static void* g_r12GuardStub = nullptr;
static uintptr_t g_r12GuardHookAddr = 0;
static ULONGLONG g_r12GuardLastCheckTick = 0;

static bool InstallR12GuardHook() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(
        GetModuleHandleW(nullptr));
    const uintptr_t hookAddr = base + RVA_PRODUCTION_R12_GUARD;

    // 签名校验（fail-open）
    unsigned char expectedSig[] = SIG_PRODUCTION_R12_GUARD;
    if (!IsReadable(reinterpret_cast<void*>(hookAddr),
                    SIG_PRODUCTION_R12_GUARD_LEN) ||
        memcmp(reinterpret_cast<void*>(hookAddr), expectedSig,
               SIG_PRODUCTION_R12_GUARD_LEN) != 0) {
        Log("[PA] R12 guard: signature mismatch at RVA 0x%llX (fail-open)\n",
            static_cast<unsigned long long>(RVA_PRODUCTION_R12_GUARD));
        return false;
    }

    unsigned char* stub = static_cast<unsigned char*>(
        AllocateExecutableNear(hookAddr, 32));
    if (!stub) {
        Log("[PA] R12 guard: stub alloc failed (fail-open)\n");
        return false;
    }

    const uintptr_t stubAddr = reinterpret_cast<uintptr_t>(stub);
    const uintptr_t resumeAddr = hookAddr + 5;
    const uintptr_t nullExitAddr = base + RVA_PRODUCTION_R12_NULL_EXIT;
    const std::int64_t dispBack =
        static_cast<std::int64_t>(resumeAddr) -
        static_cast<std::int64_t>(stubAddr + 0xF);
    const std::int64_t dispExit =
        static_cast<std::int64_t>(nullExitAddr) -
        static_cast<std::int64_t>(stubAddr + 0x14);
    if (dispBack < INT32_MIN || dispBack > INT32_MAX ||
        dispExit < INT32_MIN || dispExit > INT32_MAX) {
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[PA] R12 guard: stub displacement out of rel32 (fail-open)\n");
        return false;
    }

    stub[0] = 0x4D; stub[1] = 0x85; stub[2] = 0xE4;   // test r12, r12
    stub[3] = 0x74; stub[4] = 0x0A;                    // jz +0F
    memcpy(stub + 5, expectedSig, SIG_PRODUCTION_R12_GUARD_LEN);  // mov [rsp+0x30], r12
    stub[0xA] = 0xE9;
    *reinterpret_cast<std::int32_t*>(stub + 0xB) =
        static_cast<std::int32_t>(dispBack);
    stub[0xF] = 0xE9;
    *reinterpret_cast<std::int32_t*>(stub + 0x10) =
        static_cast<std::int32_t>(dispExit);

    DWORD oldProt = 0;
    if (!VirtualProtect(stub, 32, PAGE_EXECUTE_READ, &oldProt) ||
        !FlushInstructionCache(GetCurrentProcess(), stub, 32)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[PA] R12 guard: stub protect failed (fail-open)\n");
        return false;
    }

    unsigned char patch[5];
    patch[0] = 0xE9;
    *reinterpret_cast<std::int32_t*>(patch + 1) =
        static_cast<std::int32_t>(
            static_cast<std::int64_t>(stubAddr) -
            static_cast<std::int64_t>(hookAddr + 5));
    if (!WriteCodePatchChecked(reinterpret_cast<void*>(hookAddr),
                               patch, 5)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        Log("[PA] R12 guard: patch write failed (fail-open)\n");
        return false;
    }

    g_r12GuardStub = stub;
    g_r12GuardHookAddr = hookAddr;
    g_r12GuardLastCheckTick = GetTickCount64();
    Log("[PA] R12 guard: hook installed at RVA 0x%llX\n",
        static_cast<unsigned long long>(RVA_PRODUCTION_R12_GUARD));
    return true;
}

// 周期完整性检查：游戏可能反篡改恢复 .text → 检测到自动重打
//
// v1.5.20 修复（日志每 2 秒刷 `patch RE-APPLIED` 的元凶）：
//   旧实现拿 hook 点 5 字节去和**原始指令签名**比，但装上 hook 后那里
//   已经是我们的 E9 rel32，永远不相等 → 每 2 秒都判定"被还原"并重写一次
//   （白白写代码页 + 刷日志）。现在改成和"我们自己写的补丁字节"比，
//   只有真的被还原（字节回到原始签名）才重打，未知状态单独报。
static void VerifyR12GuardPatch() {
    if (!g_r12GuardStub || !g_r12GuardHookAddr) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_r12GuardLastCheckTick < 2000) return;
    g_r12GuardLastCheckTick = now;

    unsigned char expectedPatch[5];
    expectedPatch[0] = 0xE9;
    *reinterpret_cast<std::int32_t*>(expectedPatch + 1) =
        static_cast<std::int32_t>(
            static_cast<std::int64_t>(
                reinterpret_cast<uintptr_t>(g_r12GuardStub)) -
            static_cast<std::int64_t>(g_r12GuardHookAddr + 5));

    unsigned char expectedSig[] = SIG_PRODUCTION_R12_GUARD;
    void* cur = reinterpret_cast<void*>(g_r12GuardHookAddr);
    if (!IsReadable(cur, 5)) return;
    if (memcmp(cur, expectedPatch, 5) == 0) return;  // 补丁在位，正常

    if (memcmp(cur, expectedSig, 5) == 0) {
        // 真的被还原成原指令 → 重打
        if (WriteCodePatchChecked(cur, expectedPatch, 5))
            Log("[PA] R12 guard: patch RE-APPLIED\n");
        else
            Log("[PA] R12 guard: re-patch FAILED\n");
        return;
    }
    // 既不是我们的补丁也不是原指令：可能被其它 MOD 占用/游戏改写
    static ULONGLONG s_lastUnknownLog = 0;
    if (now - s_lastUnknownLog >= 10000) {
        s_lastUnknownLog = now;
        Log("[PA] R12 guard: patch state UNKNOWN (cur=%02X %02X %02X %02X %02X)\n",
            reinterpret_cast<unsigned char*>(cur)[0],
            reinterpret_cast<unsigned char*>(cur)[1],
            reinterpret_cast<unsigned char*>(cur)[2],
            reinterpret_cast<unsigned char*>(cur)[3],
            reinterpret_cast<unsigned char*>(cur)[4]);
    }
}

// 卸载还原（unload 调用）
static void PaUninstallR12Guard() {
    if (!g_r12GuardStub || !g_r12GuardHookAddr) return;
    unsigned char expectedSig[] = SIG_PRODUCTION_R12_GUARD;
    void* cur = reinterpret_cast<void*>(g_r12GuardHookAddr);
    if (IsReadable(cur, 5) && memcmp(cur, expectedSig, 5) != 0) {
        WriteCodePatchChecked(cur, expectedSig,
                              SIG_PRODUCTION_R12_GUARD_LEN);
    }
    VirtualFree(g_r12GuardStub, 0, MEM_RELEASE);
    g_r12GuardStub = nullptr;
    g_r12GuardHookAddr = 0;
    Log("[PA] R12 guard: restored\n");
}
