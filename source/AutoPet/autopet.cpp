// autopet.cpp —— 自动抚摸动物（AutoPet v0.6.3 正式版 for v1.20）
//
// v0.6.9: 补扫时机提前为“每天 17:15 单次”（用户拍板 2026-10-05，原 19:00）——
//   白天新买/新孵化动物当天 caress=0，仅跨日触发摸不到；v0.6.0 的
//   rescan_minutes 周期补扫整体移除，改为 17:15 后首个 5s tick 补摸一次
//   （幂等：caress=1 跳过，只摸新增），错过（睡觉跨过 17:15）由次日
//   跨日全量兑底。
//
// v0.6.0（两位大佬源码对照后的实际增强）：
//   1. gain_mult 好感倍率热参数（灵感：VillageInTheShade_Mods AutoPet v0.2.5
//      的 GainMultiplier 配置）——原生结算 delta 乘倍率，默认 1=原值，
//      范围 1..100，热生效（QoL_hot.json autopet 组）。注意倍率只放大
//      本 MOD 摸抚加的好感，不改变原生公式（神社技能倍率仍生效）。
// v0.6.1: 新增外置配置文件（大佬风格）——MOD 文件夹下 config.txt：
//   gain_mult=1 一行 key=value（# 注释行忽略）。
//   启动时在 HotConfig 之后加载（TXT 优先于 QoL_hot.json）；
//   运行中每 ~10 秒查 mtime，改完自动生效无需重启。
//   澄清：跨日自动全量抚摸 v0.5.1 起已有（dayId 变化+链表全量+原生
//   caress 防重），大佬"早晨调度"核心已具备，无需移植 BigL233 版。
//
// v0.5.8：适配 v1.20 (build 25311578) —— 3 组 vtable + GAME_ROOT + 3 个原生函数
//          全部 RVA 更新（KB-029 三层更新 + KB-011 RTTI 链 + KB-032 模板消歧）。
// v0.5.7：HUD 多屏锚定修复——不再跟随前台窗口（alt-tab 后跳屏），
//          改为锚定游戏主窗口所在显示器（与 TimeFreeze v1.1.2 同机制）。
// v0.5.6（回归修复）：恢复地图对象链校验作为“已在游戏内”门禁。
//
// v0.5.6（回归修复）：恢复地图对象链校验作为“已在游戏内”门禁。
//   v0.5.5 删除空间索引校验后，标题画面/存档选择时 root→player 链
//   已可读（游戏预建默认数据），导致未进存档就执行抚摸扫描
//   （HUD 弹出“已抚摸 N/M”）。地图对象在标题画面未加载 → 链不可达
//   → 不扫描。门禁仅作校验，spatialIndex 不存储（死代码不复活）。
//
// v0.5.5（BUG 审查修复 + 吸收 gloaming 防御优点）：
//   - 删除空间搜索死代码路径（v0.4.6 起走 livestockList_ 链表，
//     spatialSearch/rawVectorFree 无调用点）——其 AOB 失配曾导致整个
//     抚摸功能被假性禁用（确认 BUG#7）
//   - 三个原生结算函数（LOVE/RATE_MODIFIER/STATUS_SETTER）增加
//     prologue 字节验证（确认 BUG#8：裸 base+RVA 零校验，游戏更新
//     挪函数会调到任意代码，SEH 拦不住错误逻辑执行）
//   - dayId 回退重置基准：TimeFreeze 冻结点在 0 点边界抖动时不再
//     提前 latch 吞掉真实跨日（漏摸一整天修复）
//   - 跨日扫描瞬态失败（列表不可读）不 latch lastDayId，5s 后重试
//   - GetModuleHandleA("village.exe") → GetModuleHandleW(nullptr)
//     （对齐套件其余 MOD，exe 改名不失效）
//
// v0.5.1: 改为跨日触发——读 save+0x3270 (raw_second) 判断天数变化，
//         睡觉跨日后 caress 被原生重置为 0，此时执行一次性抚摸。
//         同一天内不重复扫描。进游戏后延迟 10 秒首次扫描。
// v0.5.0: HUD 左中显示已抚摸/总数 + 扫描周期优化。
// v0.4.9: FEATURE 过滤修正（&0x03 而非 !=0），狗子终于可摸。
// v0.4.8: 去掉距离过滤，保留 LIVE_VALID + FEATURE + caress 过滤。
// v0.4.6: 改用 livestockList_ 链表遍历（非空间搜索），覆盖牛/马/狗等所有动物。
// v0.4.6: 改用 livestockList_ 链表遍历（非空间搜索），覆盖牛/马/狗等所有动物。——打印空间搜索结果里所有对象的
//             vtable，确认狗是否在空间索引内、其 vtable 是什么，为支持
//             狗（动物类型 200，参考实现 RVA_AUTO_PET_LIVESTOCK_VTABLE
//             =0xE047A8 vs 我们的 0xE07850）提供依据。
//
// v0.4.3: 性能优化——v0.4.2 DirectRead 实测反劣（搜索 58~85ms），根因是
//         每次字段读都触发 IsReadable→VirtualQuery，比 RPM 还多一次系统调用。
//         改为移植 SickleHarvest 已验证的 FastRegion 区域缓存（16 槽
//         MemRegion，VirtualQuery 结果缓存命中），每轮扫描前 FastRegionReset()，
//         200+ 次字段读仅 ~16 次 VirtualQuery，目标降到 ~1ms。
//         （v0.4.3 实测：搜索 8~17ms，掉帧已明显缓解；但狗子未被扫描到）
//
// v0.4.2: 性能优化——热路径 RPM 读取（ReadProcessMemory）改为 DirectRead
//         （IsReadable 校验 + 直接解引用），消除每 2 秒扫描 ~50ms 卡顿
//         （v0.4.1-diag 实测：扫描 39~54ms/次，RPM 系统调用为主因）
//
// v0.4.1-diag: FPS 掉帧排查诊断版——扫描/结算阶段帧内耗时计时日志
//
// v0.4.0 正式版（相对 v0.3.3 诊断版）：
//   - 移除诊断刷屏日志（rawCount/candidate 每秒 4~5 条），仅保留 [Pet] settled 关键行
//   - 降低空间搜索频率（60 tick → 120 tick，约 2 秒一次），减轻每帧开销
//   - 核心逻辑不变（v0.3.3 已实测：4 只家畜 love +6、caress 每日一次、不碰 foodNum_）
//
// 功能：靠近家畜时自动抚摸，每日每只限一次。
// 设计原则：
//   - 纯位置驱动，无面板
//   - 结算走原生函数（love setter + caress key-value），不触碰游戏进食/喂养状态位
//   - 规避第三方 AutoPet v0.2.5 "抚摸后进食状态也被标记完成"的 bug
//
// v0.3.0 正式版（相对 v0.2.0-diag 的改动）：
//   1. 接入 3 个原生函数 RVA（经 AOB 扫描 + 脚本锚点交叉验证）：
//      - LOVE_SETTER   = 0x2928D0  （被 ScriptCommand::LivestockSetLoveRate/GetLoveRate 调用）
//      - RATE_MODIFIER = 0x130FE0  （被 ScriptCommand::LivestockGetLoveRate 调用）
//      - STATUS_SETTER = 0x14E9D0  （map 写入语义：写 "caress"=1 每日标记）
//   2. 靠近检测（距离 <= PET_RADIUS 220.0f）→ 原生结算好感 + 每日标记
//   3. 不触碰 foodNum_/进食状态位，每日重置由原生日落清 caress
//
// 逆向基础（v1.20 build 25311578, ImageBase=0x140000000）：
//   - CLivestockStatus 主 vtable RVA=0xE29CD8（24 槽，已验证 v1.20）
//   - 空间搜索模板 + 回调 vtable 0xE1C210（ChestSort 同款，v1.20 已弃用）
//   - LoveSetter 经脚本命令 LivestockSetLoveRate(v1.20: 0x4AE587 字符串引用) 交叉验证
//
// 参考实现：Village_QoL_Mod-main（BigL233 开源）source\auto_pet.inl
//   字段偏移：AUTO_PET_STATUS_* 系列（0xE0/0x150/0x158/0x240/0x290/0x2C0/0x300/0x328/0x330）
//   caress key：AUTO_PET_CARESS_KEY = 0x0000737365726163ULL（ASCII "caress"）
//   结算链（v0.3.0 已接入）：
//     rateModifier(playerStatus, 11) → (mod+100)*5/100  [v1.20 RVA=0x13B2C0]
//     loveSetter(status, love+delta, false)
//     statusSetter(status+0x10, &caressKey, 1)
//
// 作者：PHJ&消失的清风，转载或分享时请注明出处。

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <unordered_set>
#include <vector>
#include <cmath>

#include "logging.h"
#include "budget.h"        // P0 帧耗时探针（跨 DLL 共享）
#include "state.h"         // P1-2 状态感知暂停（载入/菜单静默）
#include "hot_config.h"
#include "aobscan.h"
#include "hook.h"
#include "feature.h"
#include "hotkey.h"
#include "game_window.h"   // HUD owner 绑定游戏窗口

// ============================================================
// 日志开关（正式版保留关键日志，方便实测验证）
// ============================================================
// #define AUTOPET_LOGGING   // v0.6.10 转正：日志关闭（定位时取消注释重编）
#ifdef AUTOPET_LOGGING
  // 使用共享框架日志
#else
  #define LogOpen(x)  ((void)0)
  #define Log(...)    ((void)0)
  #define LogV(...)   ((void)0)
  #define LogClose()  ((void)0)
#endif

// [diag] 诊断日志门控（默认关）；调试时 #define DIAG_AUTOPET 1 开启
#ifndef DIAG_AUTOPET
#define DIAG_AUTOPET 0
#endif

// ============================================================
// 版本常量（build 25311578 / v1.20）
// ============================================================
static constexpr const char* AUTOPET_VERSION = "v0.6.3";

// 逆向定位的 RVA（build 25311578）
namespace rva {
    // CLivestockStatus vtable（主继承层级，有最多槽位）
    static volatile uintptr_t LIVESTOCK_VTABLE = 0xE29CD8;
    // CCreatureStatus vtable
    static volatile uintptr_t CREATURE_VTABLE = 0xE27A58;
    // CCom_Unit_Livestock vtable
    static volatile uintptr_t COM_UNIT_LIVESTOCK_VTABLE = 0xE37088;

    // ---- 世界上下文（复用 ChestSort 链路；v0.5.6 恢复地图链作为游戏内门禁） ----
    static volatile uintptr_t GAME_ROOT = 0x10FCBB0;  // 全局根指针
    static volatile uintptr_t ROOT_PLAYER_OFFSET = 0x208;
    static volatile uintptr_t ROOT_MAP_OWNER_OFFSET = 0x268;
    static volatile uintptr_t MAP_INFO_OFFSET = 0x0d8;
    static volatile uintptr_t MAP_SPATIAL_OWNER_OFFSET = 0x030;
    static volatile uintptr_t MAP_SPATIAL_INDEX_OFFSET = 0x6e0;
    static volatile uintptr_t PLAYER_OBJECT_STATUS_OFFSET = 0x32b8;
    static volatile uintptr_t OBJECT_POSITION_OFFSET = 0x0f0;

    // v0.4.6: livestockList_ 链表偏移（CSaveData + 0x3430）
    // 参考实现 BigL233 auto_pet.inl L147: AUTO_PET_LIVESTOCK_LIST_OFFSET = 0x3430
    // save = root+0x208（与 ChestSort 的 player 同一指针，因为 player = save 对象）
    static volatile uintptr_t LIVESTOCK_LIST_OFFSET = 0x3430;
    static constexpr size_t MAX_LIVESTOCK_LIST = 256;  // 链表节点上限（游戏限制 ~128 只）
    static constexpr int MAX_PET_CANDIDATES = 256;     // v0.5.4: 候选数组容量 32→128（用户上百只动物只摸 32 只）

    // v0.5.1: 游戏时间（save+0x3270 = raw_second，int64）
    // dayId = rawSecond / 86400，跨日时 caress 被原生重置为 0
    static volatile uintptr_t RAW_SECOND_OFFSET = 0x3270;
    static constexpr int64_t SECONDS_PER_DAY = 86400;

    // v0.5.9: 白天门控（与 HuntOneShot 同款）——只在白天执行抚摸与 HUD 弹出，
    // 夜间跳过（省开销 + 不打扰）。小时 = (raw/3600+7) % 24，raw 日界 = 显示 7 点。
    static volatile int SCAN_HOUR_FROM = 7;
    static volatile int SCAN_HOUR_TO = 22;

    // v0.6.0: 好感倍率（gain_mult，默认 1=原生结算原值，1..100 钳制）
    static volatile int GAIN_MULT = 1;

    // ---- v0.3.0 新增：原生结算函数（AOB + 脚本锚点交叉验证） ----
    // LOVE_SETTER：ScriptCommand::LivestockSetLoveRate 交叉验证（v1.20: 0x4AE4E7 调用 -> 0x2A0D50）
    static volatile uintptr_t LOVE_SETTER = 0x2A0D50;
    // RATE_MODIFIER：ScriptCommand::LivestockGetLoveRate 交叉验证（v1.20: 0x4AE4BE 调用 -> 0x13B2C0）
    static volatile uintptr_t RATE_MODIFIER = 0x13B2C0;
    // STATUS_SETTER：map 写入语义（写 status+0x10 的 "caress" 键），16 字节签名 + prologue 验证
    static volatile uintptr_t STATUS_SETTER = 0x158C50;
}

// ============================================================
// BigL233 真实字段偏移（build 24646798，经 v0.1.3 日志部分验证）
// ============================================================
namespace field {
    // CLivestockStatus 内嵌 map 的基址偏移（"caress" key-value 序列化表）
    static volatile uintptr_t CA_MAP_BASE = 0x10;   // status+0x10 = map 对象
    static volatile uintptr_t MAP_SENTINEL = 0x10;   // map+0x10 = sentinel 指针
    static volatile uintptr_t MAP_SIZE = 0x18;   // map+0x18 = size (u64)
    static volatile uintptr_t MAP_BUCKETS = 0x20;   // map+0x20 = bucket 向量
    static volatile uintptr_t MAP_MASK = 0x38;   // map+0x38 = mask (u64)

    static volatile uintptr_t UNIQUE_ID = 0x0E0;  // 唯一 ID（u64）
    static volatile uintptr_t LIVE_OBJECT = 0x150;  // 场景实时对象指针
    static volatile uintptr_t LIVE_VALID = 0x158;  // 实时对象有效标记（byte*）
    static volatile uintptr_t GIMMICK_HOLDER = 0x240;  // GimmickData holder 指针
    static volatile uintptr_t ANIMAL_HOLDER = 0x290;  // 动物 holder 指针
    static volatile uintptr_t PLACEMENT_HOLDER = 0x2C0;  // 放置 holder 指针
    static volatile uintptr_t LOVE = 0x300;  // 好感度（int）
    static volatile uintptr_t FEATURE_LOW = 0x328;  // 特征低位（u8）
    static volatile uintptr_t FEATURE_HIGH = 0x330;  // 特征高位（u8）
}

// "caress" key：ASCII 小端 = 0x0000737365726163
static constexpr uint64_t CARESS_KEY = 0x0000737365726163ULL;

// ============================================================
// 全局状态
// ============================================================
namespace G {
    uintptr_t base = 0;                    // village.exe 模块基址
    bool ready = false;                    // 初始化完成
    bool vtableVerified = false;           // vtable 地址验证通过

    // vtable 绝对地址（运行时计算）
    uintptr_t livestockVtable = 0;
    uintptr_t creatureVtable = 0;
    uintptr_t comUnitLivestockVtable = 0;

    // 扫描节流
    std::atomic<uint64_t> lastScanTick{0};
    static constexpr uint64_t SCAN_INTERVAL_TICKS = 300; // ~5秒@60fps，检查天数变化用
    int64_t lastDayId = -1;  // v0.5.1: 上次扫描的游戏天数，-1=首次
    int64_t firstScanDelayTicks = 0;  // v0.5.1: 进游戏后延迟首次扫描（等场景加载）
    int64_t eveningRescanDoneDay = -1;  // v0.6.9: 已完成 17:15 补扫的 dayId（-1=从未）

    // ---- v0.3.0 原生结算函数 ----
    // int64_t (__fastcall*)(void* playerStatus, int mode)
    using FnRateModifier = int64_t(__fastcall*)(void*, int);
    // void (__fastcall*)(void* status, int love, bool flag)
    using FnLoveSetter = void(__fastcall*)(void*, int, bool);
    // void (__fastcall*)(void* mapBase, const uint64_t* key, int value)
    using FnStatusSetter = void(__fastcall*)(void*, const uint64_t*, int);
    FnRateModifier rateModifier = nullptr;
    FnLoveSetter loveSetter = nullptr;
    FnStatusSetter statusSetter = nullptr;
}

// ============================================================
// 安全内存读取（只读诊断，不写入任何游戏内存）
// ============================================================
static bool SafeRead(void* addr, void* buf, size_t len) {
    if (!addr || !buf || len == 0) return false;
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, buf, len, &bytesRead)
           && bytesRead == len;
}

// 读取指针（验证可读）
static uintptr_t SafeReadPtr(void* addr) {
    uintptr_t val = 0;
    if (SafeRead(addr, &val, sizeof(val)))
        return val;
    return 0;
}

// ============================================================
// FastRegion 区域缓存（移植 SickleHarvest 已验证方案）
// 同区域多次访问只查一次系统调用。v0.4.2 实测每次字段读
// 都触发 VirtualQuery 反而比 RPM 慢；缓存后 200+ 次字段读
// 仅 ~16 次 VirtualQuery。
// ============================================================
struct MemRegion {
    uintptr_t start;
    uintptr_t end;
};
static MemRegion g_fastRegions[16];
static int g_fastRegionCount = 0;

// 每轮扫描开始前必须调用（清空缓存，防止跨帧区域失效）
static void FastRegionReset() {
    g_fastRegionCount = 0;
}

// 内存可读性检查（VirtualQuery + 16 槽区域缓存）
static bool IsReadable(const void* addr, size_t len) {
    if (!addr || len == 0) return false;
    uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    if (start < 0x10000 || start > 0x00007FFFFFFFFFFFULL) return false;
    // 命中缓存区域
    for (int i = 0; i < g_fastRegionCount; ++i) {
        if (start >= g_fastRegions[i].start &&
            start + len <= g_fastRegions[i].end) return true;
    }
    // 未命中 → 真实 VirtualQuery
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t regionEnd = regionStart + mbi.RegionSize;
    if (start + len < start || start + len > regionEnd) return false;
    // 记录该区域
    if (g_fastRegionCount < 16) {
        g_fastRegions[g_fastRegionCount] = { regionStart, regionEnd };
        g_fastRegionCount++;
    }
    return true;
}

// ============================================================
// 直接内存读取（热路径优化：FastRegion IsReadable 校验 + 直接解引用）
// 同进程内不必要用 ReadProcessMemory（每次跨内核态~2us），
// 直接读内存快 ~100 倍。读取前 VirtualQuery 保证安全，
// 且 v0.4.3 起 IsReadable 走 16 槽区域缓存，仅首访触发系统调用。
// ============================================================
static bool DirectRead(const void* addr, void* buf, size_t len) {
    if (!addr || !buf || len == 0) return false;
    if (!IsReadable(addr, len)) return false;
    memcpy(buf, addr, len);
    return true;
}

// 读取指针字段
static bool ReadPtr(const void* obj, uintptr_t off, void** out) {
    if (!obj || !out) return false;
    const void* field = (const void*)((uintptr_t)obj + off);
    if (!IsReadable(field, sizeof(void*))) return false;
    *out = *(void* const*)field;
    return *out != nullptr;
}

// ============================================================
// vtable 地址验证：读取 vtable[-1] 处的 COL，检查 signature=1
// ============================================================
static bool VerifyVtable(uintptr_t vtableRVA, const char* name) {
    if (!G::base) return false;
    uintptr_t vtAddr = G::base + vtableRVA;

    // vtable[-1] = 指向 COL 的指针
    uintptr_t colAbs = SafeReadPtr(reinterpret_cast<void*>(vtAddr - 8));
    if (!colAbs) {
        Log("[autopet] %s vtable[-1] read FAILED at 0x%llx", name,
            static_cast<unsigned long long>(vtAddr - 8));
        return false;
    }

    // COL signature = 1 (x64)
    uint32_t sig = 0;
    if (!SafeRead(reinterpret_cast<void*>(colAbs), &sig, sizeof(sig))) {
        Log("[autopet] %s COL read FAILED at 0x%llx", name,
            static_cast<unsigned long long>(colAbs));
        return false;
    }
    if (sig != 1) {
        Log("[autopet] %s COL sig=%u (expected 1) at 0x%llx", name, sig,
            static_cast<unsigned long long>(colAbs));
        return false;
    }

    Log("[autopet] %s vtable OK: vt=0x%llx COL=0x%llx sig=1",
        name, static_cast<unsigned long long>(vtAddr),
        static_cast<unsigned long long>(colAbs));
    return true;
}

// ============================================================
// 世界上下文（复用 ChestSort 链路；v0.5.5 删除空间索引部分）
// ============================================================
struct WorldContext {
    void* player;
    void* save;          // v0.4.6: save 对象（= root+0x208），用于 livestockList_ 遍历
    float position[4];
};

static bool GetWorldContext(WorldContext* ctx) {
    if (!ctx) return false;
    memset(ctx, 0, sizeof(*ctx));

    // 第 1 步：读取 game root 全局指针
    void** rootSlot = (void**)(G::base + rva::GAME_ROOT);
    if (!IsReadable(rootSlot, sizeof(void*))) return false;
    void* root = *rootSlot;
    if (!root) return false;

    // 第 2 步：root+0x208 → player (save对象)
    void* player = nullptr;
    if (!ReadPtr(root, rva::ROOT_PLAYER_OFFSET, &player)) return false;

    // v0.4.6: player 即 save 对象（root+0x208），livestockList_ 在 save+0x3430
    ctx->save = player;

    // 第 3 步：player → objectStatus（= playerStatus）
    void* objectStatus = nullptr;
    if (!ReadPtr(player, rva::PLAYER_OBJECT_STATUS_OFFSET, &objectStatus)) return false;

    // 第 4 步：玩家位置（+0xf0）
    const float* pos = (const float*)((uintptr_t)objectStatus + rva::OBJECT_POSITION_OFFSET);
    if (!IsReadable(pos, sizeof(float) * 4)) return false;
    memcpy(ctx->position, pos, sizeof(float) * 4);
    if (!std::isfinite(ctx->position[0]) || !std::isfinite(ctx->position[1])) return false;
    if (fabsf(ctx->position[0]) < 1.0f && fabsf(ctx->position[1]) < 1.0f) return false;

    // 第 5 步（v0.5.6 恢复，回归修复）：地图对象链校验——“已在游戏内”门禁。
    // v0.5.5 删除此校验后，标题画面/存档选择时 root→player 链已可读
    // （游戏预建默认数据），导致未进存档就执行抚摸扫描（HUD 弹出
    // “已抚摸 N/M”）。地图对象在标题画面未加载 → 链不可达 → 不扫描。
    // 仅作校验，spatialIndex 不存储（死代码不复活，仅恢复验证语义）。
    void* mapOwner = nullptr;
    void* mapInfo = nullptr;
    void* spatialOwner = nullptr;
    if (!ReadPtr(root, rva::ROOT_MAP_OWNER_OFFSET, &mapOwner)) return false;
    if (!ReadPtr(mapOwner, rva::MAP_INFO_OFFSET, &mapInfo)) return false;
    if (!ReadPtr(mapInfo, rva::MAP_SPATIAL_OWNER_OFFSET, &spatialOwner)) return false;
    void* spatialIndex = (void*)((uintptr_t)spatialOwner + rva::MAP_SPATIAL_INDEX_OFFSET);
    if (!IsReadable(spatialIndex, 0x48)) return false;

    ctx->player = player;
    return true;
}

// ============================================================
// caress key-value map 只读读取（移植 BigL233 AutoPetReadCaressState）
// ============================================================
static bool ReadCaressState(void* status, int* valueOut) {
    if (!status || !valueOut) return false;
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(status);
    const uintptr_t mapBase = reinterpret_cast<uintptr_t>(status) + field::CA_MAP_BASE;

    if (!IsReadable(reinterpret_cast<const void*>(mapBase + field::MAP_SENTINEL), 0x38))
        return false;

    void* sentinel = *reinterpret_cast<void* const*>(mapBase + field::MAP_SENTINEL);
    const uint64_t size = *reinterpret_cast<const uint64_t*>(mapBase + field::MAP_SIZE);
    void* buckets = *reinterpret_cast<void* const*>(mapBase + field::MAP_BUCKETS);
    const uint64_t mask = *reinterpret_cast<const uint64_t*>(mapBase + field::MAP_MASK);

    // 合理性校验（BigL233 同款）
    if (!sentinel || !IsReadable(sentinel, 0x20) || size > 1024 ||
        mask > 1023 || ((mask + 1) & mask) != 0 || !buckets ||
        !IsReadable(buckets, static_cast<size_t>(mask + 1) * 16)) {
        Log("[autopet] [map] 布局异常 sentinel=0x%llx size=%llu buckets=0x%llx mask=0x%llx",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(sentinel)),
            static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(buckets)),
            static_cast<unsigned long long>(mask));
        return false;
    }

    const uint64_t bucket = CARESS_KEY & mask;
    void** pair = reinterpret_cast<void**>(
        reinterpret_cast<unsigned char*>(buckets) + bucket * 16);
    void* stop = pair[0];
    void* node = pair[1];
    if (size == 0 && node != sentinel) return false;
    if ((node == sentinel) != (stop == sentinel)) return false;
    if (node == sentinel) {
        *valueOut = 0;
        return true;
    }
    size_t visited = 0;
    bool reachedStop = false;
    while (node != sentinel && visited <= size) {
        if (!node || !IsReadable(node, 0x20)) return false;
        if (*reinterpret_cast<const uint64_t*>(
                reinterpret_cast<unsigned char*>(node) + 0x10) == CARESS_KEY) {
            const int value = *reinterpret_cast<const int*>(
                reinterpret_cast<unsigned char*>(node) + 0x18);
            if (value != 0 && value != 1) return false;
            *valueOut = value;
            return true;
        }
        ++visited;
        if (node == stop) { reachedStop = true; break; }
        node = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(node) + sizeof(void*));
    }
    if (!reachedStop || visited > size) return false;
    *valueOut = 0; // 原生 getter 对缺失 key 返回 0
    return true;
}

// ============================================================
// 靠近检测 + 原生结算（v0.3.0 核心）
// ============================================================
struct PetCandidate {
    void* status;        // CLivestockStatus 对象
    uint64_t uniqueId;   // 唯一 ID
    float dist;          // 距离
    int love;            // 当前好感
    int caress;          // caress 标记（0/1）
};

// v0.4.6: 收集附近可抚摸家畜——改用 livestockList_ 链表遍历（非空间搜索）
// 参考实现 BigL233 auto_pet.inl:2664 AutoPetSnapshotResidents
// v0.5.5: 返回 -1 = 列表瞬态不可读（调用方应稍后重试）；>=0 = 成功收集数
static int CollectNearbyLivestock(void* save, const float* playerPos,
                                  PetCandidate* out, int maxOut) {
    if (!save || !playerPos || !out || maxOut <= 0) return 0;

    // 每轮扫描开始前重置区域缓存
    FastRegionReset();

    // 读取 livestockList_ 链表（save + 0x3430）
    // 布局：[void* sentinel] [u64 declared_count]
    uintptr_t listAddr = (uintptr_t)save + rva::LIVESTOCK_LIST_OFFSET;
    if (!IsReadable((void*)listAddr, sizeof(void*) + sizeof(uint64_t))) return -1;

    void* sentinel = *reinterpret_cast<void**>(listAddr);
    uint64_t declared = *reinterpret_cast<const uint64_t*>(listAddr + sizeof(void*));
    // v0.5.5: -1=瞬态不可读（重试）；declared==0=无动物（合法，返回 0）
    if (!sentinel || declared > rva::MAX_LIVESTOCK_LIST) return -1;
    if (declared == 0) return 0;
    if (!IsReadable(sentinel, sizeof(void*) * 2)) return -1;

    // 链表第一个节点
    void* node = *reinterpret_cast<void**>(sentinel);
    void* previous = sentinel;

    int n = 0;
    uint64_t vtLivestock = 0, vtOther = 0;
    int diagLogged = 0;
    size_t visited = 0;

    while (visited < (size_t)declared && n < maxOut) {
        if (!node || node == sentinel) break;
        if (!IsReadable(node, sizeof(void*) * 3)) break;
        // back-pointer 一致性校验
        if (*reinterpret_cast<void**>((uintptr_t)node + sizeof(void*)) != previous) break;

        // node+0x10 = CLivestockStatus* status
        void* status = *reinterpret_cast<void**>((uintptr_t)node + sizeof(void*) * 2);
        if (!status) { previous = node; node = *reinterpret_cast<void**>(node); ++visited; continue; }

        // vtable 过滤（放宽：接受三个 vtable 覆盖所有动物类型）
        uint64_t objVtable = 0;
        if (!DirectRead(status, &objVtable, sizeof(objVtable))) {
            previous = node; node = *reinterpret_cast<void**>(node); ++visited; continue;
        }
        if (objVtable != G::livestockVtable &&
            objVtable != G::creatureVtable &&
            objVtable != G::comUnitLivestockVtable) {
            vtOther++;
            if (diagLogged < 5) {
#if DIAG_AUTOPET
                Log("[autopet] [diag] 非家畜对象[%zu] status=0x%llx vtable=0x%llx",
                    visited, (unsigned long long)(uintptr_t)status,
                    (unsigned long long)objVtable);
#endif
                diagLogged++;
            }
            previous = node; node = *reinterpret_cast<void**>(node); ++visited; continue;
        }
        vtLivestock++;

        uintptr_t addr = (uintptr_t)status;

        // LIVE 有效
        uintptr_t liveValidPtr = 0;
        DirectRead(reinterpret_cast<void*>(addr + field::LIVE_VALID), &liveValidPtr, sizeof(liveValidPtr));
        uint8_t liveValid = 1;
        if (liveValidPtr && IsReadable(reinterpret_cast<void*>(liveValidPtr), 1)) {
            DirectRead(reinterpret_cast<void*>(liveValidPtr), &liveValid, sizeof(liveValid));
        }

        // FEATURE 位
        uint8_t featureLow = 0, featureHigh = 0;
        DirectRead(reinterpret_cast<void*>(addr + field::FEATURE_LOW), &featureLow, sizeof(featureLow));
        DirectRead(reinterpret_cast<void*>(addr + field::FEATURE_HIGH), &featureHigh, sizeof(featureHigh));

        // UID + caress
        uint64_t uniqueId = 0;
        DirectRead(reinterpret_cast<void*>(addr + field::UNIQUE_ID), &uniqueId, sizeof(uniqueId));
        int caress = -1;
        ReadCaressState(status, &caress);

        // LIVE 有效过滤
        if (liveValidPtr && IsReadable(reinterpret_cast<void*>(liveValidPtr), 1)) {
            if (!liveValid) { previous = node; node = *reinterpret_cast<void**>(node); ++visited; continue; }
        }

        // FEATURE 位过滤——参考实现只检查最低 2 位 (& 0x03)：
        // (featureLow & 0x03) != 0 或 (featureHigh & 0x03) != 0 才是 HORROR/PIRO 事件动物
        // 狗的 featureLow=0x70，0x70 & 0x03 = 0，不是事件动物，应通过
        if ((featureLow & 0x03) != 0 || (featureHigh & 0x03) != 0) {
            previous = node; node = *reinterpret_cast<void**>(node); ++visited; continue;
        }

        // UID + LOVE
        int love = 0;
        DirectRead(reinterpret_cast<void*>(addr + field::LOVE), &love, sizeof(love));

        out[n++] = {status, uniqueId, 0.0f, love, caress};

        previous = node;
        node = *reinterpret_cast<void**>(node);
        ++visited;
    }

    // 诊断统计（降频）
    static uint64_t diagSeq = 0;
    if (++diagSeq % 5 == 0) {
#if DIAG_AUTOPET
        Log("[autopet] [diag] livestockList: 遍历=%zu 家畜=%llu 其他=%llu 已收集=%d",
            visited, (unsigned long long)vtLivestock,
            (unsigned long long)vtOther, n);
#endif
    }

    return n;
}

// 原生结算：love setter + caress 标记（全程不碰 foodNum_）
static bool SettlePet(PetCandidate* c) {
    if (!c || !c->status) return false;
    if (!G::loveSetter || !G::statusSetter || !G::rateModifier) {
        Log("[Pet] 原生结算函数未就绪 (love=%p status=%p rate=%p)",
            (void*)G::loveSetter, (void*)G::statusSetter, (void*)G::rateModifier);
        return false;
    }

    // 玩家 status（计算好感倍率用）
    void* playerStatus = nullptr;
    {
        void** rootSlot = (void**)(G::base + rva::GAME_ROOT);
        if (!IsReadable(rootSlot, sizeof(void*))) return false;
        void* root = *rootSlot;
        if (!root) return false;
        void* player = nullptr;
        if (!ReadPtr(root, rva::ROOT_PLAYER_OFFSET, &player)) return false;
        if (!ReadPtr(player, rva::PLAYER_OBJECT_STATUS_OFFSET, &playerStatus)) return false;
    }
    if (!playerStatus) return false;

    // 倍率（BigL233 同款：RateModifier(playerStatus, 11)）
    int64_t modifierResult = 0;
    __try {
        modifierResult = G::rateModifier(playerStatus, 11);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[Pet] rateModifier 调用异常 (SEH)");
        return false;
    }
    int32_t modifier = static_cast<int32_t>(modifierResult);
    int64_t scaledDelta = (static_cast<int64_t>(modifier) + 100) * 5;
    if (scaledDelta < -2147483647LL - 1 || scaledDelta > 2147483647LL) return false;
    int effectiveDelta = static_cast<int>(scaledDelta) / 100;

    // v0.6.0: gain_mult 好感倍率（热参数，1..100 钳制；1=原生原值）
    int gainMult = rva::GAIN_MULT;
    if (gainMult < 1) gainMult = 1;
    if (gainMult > 100) gainMult = 100;
    if (gainMult != 1) {
        int64_t boosted = static_cast<int64_t>(effectiveDelta) * gainMult;
        if (boosted < -2147483647LL - 1 || boosted > 2147483647LL) return false;
        effectiveDelta = static_cast<int>(boosted);
    }

    int loveBefore = c->love;
    int64_t requestedLove = static_cast<int64_t>(loveBefore) + effectiveDelta;
    if (requestedLove < -2147483647LL - 1 || requestedLove > 2147483647LL) return false;

    // 写好感（原生 love setter）
    __try {
        G::loveSetter(c->status, static_cast<int>(requestedLove), false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[Pet] loveSetter 调用异常 (SEH)");
        return false;
    }

    // 写 caress 每日标记（原生 status setter）
    const uint64_t key = CARESS_KEY;
    __try {
        G::statusSetter(reinterpret_cast<unsigned char*>(c->status) + 0x10, &key, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[Pet] statusSetter 调用异常 (SEH)");
        return false;
    }

    // 回读验证
    int after = -1;
    bool afterOk = ReadCaressState(c->status, &after);
    Log("[Pet] settled uid=0x%llX dist=%.1f love %d -> %d (delta=%d x%d mult) mod=%d caress=%d(ok=%d)",
        static_cast<unsigned long long>(c->uniqueId),
        c->dist, loveBefore, static_cast<int>(requestedLove), effectiveDelta,
        gainMult, modifier, after, afterOk ? 1 : 0);
    return true;
}

// ============================================================
// HUD：左中显示已抚摸/总数（v0.5.0）
// ============================================================
static HMODULE g_autopetModule = nullptr;
static constexpr wchar_t PET_HUD_CLASS[] = L"AutoPetHudWindow";
static HWND g_petHudWindow = nullptr;
static HFONT g_petHudFont = nullptr;
static int g_petHudTotal = 0;
static int g_petHudPetted = 0;
static ULONGLONG g_petHudHideAt = 0;  // v0.5.4: 自动隐藏计时

static LRESULT CALLBACK PetHudWndProc(HWND window, UINT message,
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

        // 左侧强调色条（绿色=已全摸，灰色=未完成）
        RECT bar = client;
        bar.right = bar.left + 6;
        COLORREF accent = (g_petHudTotal > 0 && g_petHudPetted == g_petHudTotal)
            ? RGB(127, 176, 105) : RGB(200, 165, 70);
        HBRUSH accentBrush = CreateSolidBrush(accent);
        FillRect(dc, &bar, accentBrush);
        DeleteObject(accentBrush);

        SetBkMode(dc, TRANSPARENT);
        HFONT oldFont = (HFONT)SelectObject(dc, g_petHudFont);
        SetTextColor(dc, RGB(220, 225, 230));

        // "已抚摸 N/M 只动物" (v0.5.4: 修正 抚=U+629A)
        wchar_t buf[64];
        int len = swprintf_s(buf, 64, L"\x5DF2\x629A\x6478 %d/%d \x53EA\x52A8\x7269",
                             g_petHudPetted, g_petHudTotal);
        RECT r = client;
        r.left += 22; r.right -= 14; r.top += 6;
        r.bottom = r.top + 28;
        DrawTextW(dc, buf, len, &r,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        SelectObject(dc, oldFont);
        EndPaint(window, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

static bool PetHudInit() {
    WNDCLASSEXW cls = {};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = PetHudWndProc;
    cls.hInstance = g_autopetModule;
    cls.lpszClassName = PET_HUD_CLASS;
    if (!RegisterClassExW(&cls)) {
        DWORD err = GetLastError();
        WNDCLASSEXW existing = {};
        existing.cbSize = sizeof(existing);
        if (err != ERROR_CLASS_ALREADY_EXISTS ||
            !GetClassInfoExW(g_autopetModule, PET_HUD_CLASS, &existing) ||
            existing.lpfnWndProc != PetHudWndProc) return false;
    }

    g_petHudFont = CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (!g_petHudFont) return false;

    const int width = 200;
    const int height = 40;
    g_petHudWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        PET_HUD_CLASS, L"", WS_POPUP, 0, 0, width, height,
        nullptr, nullptr, g_autopetModule, nullptr);
    if (!g_petHudWindow) return false;
    // v0.6.7: 移除游戏窗口 owner 绑定（GWLP_HWNDPARENT）——owned TOPMOST 窗口链
    // 干扰 Alt+Tab 前台切换，游戏窗口切不回（切窗修复第四轮，详见 game_window.h v1.5）

    SetLayeredWindowAttributes(g_petHudWindow, 0, 228, LWA_ALPHA);
    HRGN rounded = CreateRoundRectRgn(0, 0, width + 1, height + 1, 10, 10);
    if (!SetWindowRgn(g_petHudWindow, rounded, FALSE)) DeleteObject(rounded);
    return true;
}

static void PetHudUpdate(DWORD durationMs = 5000) {
    if (!g_petHudWindow && !PetHudInit()) return;
    if (!g_petHudWindow) return;

    // v0.5.7: 锚定游戏主窗口所在显示器（旧实现跟随前台窗口，
    // alt-tab 后 HUD 跳到其他屏幕）
    struct FindGameCtx { DWORD pid; HWND found; };
    FindGameCtx fgc = { GetCurrentProcessId(), nullptr };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* ctx = reinterpret_cast<FindGameCtx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != ctx->pid) return TRUE;
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW) return TRUE;  // 我们自己的 HUD
        ctx->found = hwnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&fgc));

    HMONITOR mon = fgc.found
        ? MonitorFromWindow(fgc.found, MONITOR_DEFAULTTONEAREST)
        : MonitorFromWindow(QolFindGameWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
    }
    const int width = 200;
    const int height = 40;
    const int x = mi.rcWork.left + 24;
    const int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - height) / 2;
    SetWindowPos(g_petHudWindow, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_petHudWindow, nullptr, TRUE);
    UpdateWindow(g_petHudWindow);
    g_petHudHideAt = GetTickCount64() + durationMs;  // v0.5.4: 设定隐藏时间
}

static void PetHudPump() {
    if (!g_petHudWindow) return;
    if (!QolGameInForeground()) { QolHudGuardVisibility(g_petHudWindow); return; }  // v1.6: 失焦守卫兜底（alpha 渐隐）后 pump 静默
    MSG msg = {};
    while (PeekMessageW(&msg, g_petHudWindow, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // v0.5.4: 超时自动隐藏
    if (IsWindowVisible(g_petHudWindow) && g_petHudHideAt != 0 &&
        GetTickCount64() >= g_petHudHideAt) {
        QolHudMarkHiddenByMod(g_petHudWindow);  // MOD 主动隐藏：清守卫标记
        ShowWindow(g_petHudWindow, SW_HIDE);
        g_petHudHideAt = 0;
    }
}

// ============================================================
// 每帧逻辑
// ============================================================
// v0.5.5: 返回 false = 列表瞬态不可读（调用方应稍后重试，勿 latch 天数）
static bool AutoPetTickImpl() {
    if (!G::vtableVerified || !G::livestockVtable) return false;

    WorldContext ctx = {};
    if (!GetWorldContext(&ctx)) {
        return false; // 游戏加载中属正常，不刷日志
    }

    // v0.4.6: 改用 livestockList_ 链表遍历（非空间搜索），覆盖牛/马/狗等所有动物
    PetCandidate candidates[rva::MAX_PET_CANDIDATES];
    int n = CollectNearbyLivestock(ctx.save, ctx.position, candidates, rva::MAX_PET_CANDIDATES);
    if (n < 0) {
        return false;  // v0.5.5: 列表瞬态不可读，重试
    }
    if (n == 0) {
        g_petHudTotal = 0;
        g_petHudPetted = 0;
        PetHudUpdate();
        return true;   // 无动物（合法状态）
    }

    for (int i = 0; i < n; i++) {
        // 已抚摸过（caress=1）跳过
        if (candidates[i].caress == 1) continue;
        SettlePet(&candidates[i]);
    }

    // v0.5.4: HUD 统计——结算后重新读 caress 状态，统计实际已摸数量
    g_petHudTotal = n;
    g_petHudPetted = 0;
    for (int i = 0; i < n; i++) {
        int caress = -1;
        ReadCaressState(candidates[i].status, &caress);
        if (caress == 1) g_petHudPetted++;
    }
    PetHudUpdate();
    return true;
}

// ============================================================
// Feature 实现
// ============================================================
class AutoPetFeature : public qol::Feature {
public:
    AutoPetFeature() : Feature("autopet") {}

    bool Init() override {
        G::base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!G::base) {
            Log("[autopet] FATAL: exe module not found");
            return false;
        }
        Log("[autopet] mod_init() base=0x%llx %s",
            static_cast<unsigned long long>(G::base), AUTOPET_VERSION);

        // 计算 vtable 绝对地址
        G::livestockVtable = G::base + rva::LIVESTOCK_VTABLE;
        G::creatureVtable = G::base + rva::CREATURE_VTABLE;
        G::comUnitLivestockVtable = G::base + rva::COM_UNIT_LIVESTOCK_VTABLE;

        // 验证 vtable（读 vtable[-1] 确认 COL sig=1）
        bool ok1 = VerifyVtable(rva::LIVESTOCK_VTABLE, "CLivestockStatus");
        bool ok2 = VerifyVtable(rva::CREATURE_VTABLE, "CCreatureStatus");
        bool ok3 = VerifyVtable(rva::COM_UNIT_LIVESTOCK_VTABLE, "CCom_Unit_Livestock");

        G::vtableVerified = ok1; // 只需主 vtable 验证通过即可

        if (!G::vtableVerified) {
            Log("[autopet] vtable verification FAILED — feature disabled");
            return false;
        }

        // v0.5.5: 空间搜索死代码路径已整体删除（v0.4.6 起走 livestockList_
        // 链表遍历，spatialSearch/rawVectorFree 无调用点）；其 AOB 失配
        // 曾导致整个抚摸功能被假性禁用（BUG 审查确认 BUG#7）

        // v0.5.5: 原生结算函数 prologue 字节验证（吸收 gloaming addrsig 防御，
        // 修复 BUG#8：裸 base+RVA 零校验，游戏更新挪函数会调到任意代码）
        static const unsigned char kLoveSig[]   =
            {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x55,0x57};
        static const unsigned char kRateSig[]   =
            {0x40,0x53,0x48,0x81,0xEC,0x80,0x00,0x00,0x00};
        static const unsigned char kStatusSig[] =
            {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC};
        struct NativeSig { const char* name; uintptr_t rva;
                           const unsigned char* sig; size_t len; };
        const NativeSig nativeSigs[3] = {
            {"LOVE_SETTER",   rva::LOVE_SETTER,   kLoveSig,   sizeof(kLoveSig)},
            {"RATE_MODIFIER", rva::RATE_MODIFIER, kRateSig,   sizeof(kRateSig)},
            {"STATUS_SETTER", rva::STATUS_SETTER, kStatusSig, sizeof(kStatusSig)},
        };
        for (int i = 0; i < 3; ++i) {
            if (memcmp(reinterpret_cast<const void*>(G::base + nativeSigs[i].rva),
                       nativeSigs[i].sig, nativeSigs[i].len) != 0) {
                Log("[autopet] FATAL: %s prologue mismatch @RVA 0x%llX — feature disabled",
                    nativeSigs[i].name,
                    static_cast<unsigned long long>(nativeSigs[i].rva));
                return false;
            }
        }
        G::rateModifier = reinterpret_cast<G::FnRateModifier>(G::base + rva::RATE_MODIFIER);
        G::loveSetter = reinterpret_cast<G::FnLoveSetter>(G::base + rva::LOVE_SETTER);
        G::statusSetter = reinterpret_cast<G::FnStatusSetter>(G::base + rva::STATUS_SETTER);
        Log("[autopet] native funcs verified: rate=0x%llx love=0x%llx status=0x%llx",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(G::rateModifier)),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(G::loveSetter)),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(G::statusSetter)));

        Log("[autopet] init OK: livestockVt=0x%llx creatureVt=0x%llx comUnitVt=0x%llx",
            static_cast<unsigned long long>(G::livestockVtable),
            static_cast<unsigned long long>(G::creatureVtable),
            static_cast<unsigned long long>(G::comUnitLivestockVtable));
        return true;
    }

    void Tick() override {
        if (!G::vtableVerified) return;

        PetHudPump();

        // 节流：每 SCAN_INTERVAL_TICKS 检查一次天数变化
        uint64_t now = ++G::lastScanTick;
        if (now % G::SCAN_INTERVAL_TICKS != 0) return;

        // v0.5.1: 读游戏时间判断是否跨日
        WorldContext ctx = {};
        if (!GetWorldContext(&ctx)) return;

        // 读 raw_second
        int64_t rawSecond = 0;
        if (!IsReadable(reinterpret_cast<void*>((uintptr_t)ctx.save + rva::RAW_SECOND_OFFSET), sizeof(int64_t))) return;
        DirectRead(reinterpret_cast<void*>((uintptr_t)ctx.save + rva::RAW_SECOND_OFFSET), &rawSecond, sizeof(rawSecond));
        if (rawSecond < 0) return;

        // v0.5.9: 白天门控——夜间跳过抚摸与 HUD（省开销 + 不打扰）
        // 小时 = (raw/3600+7) % 24（raw 日界 = 显示 7 点，与 HuntOneShot/SelfServiceStore 同式）
        int hour = (int)((rawSecond / 3600 + 7) % 24);
        if (hour < rva::SCAN_HOUR_FROM || hour > rva::SCAN_HOUR_TO) return;
        int minute = (int)((rawSecond / 60) % 60);  // v0.6.9: 17:15 触发需分钟位

        int64_t dayId = rawSecond / rva::SECONDS_PER_DAY;

        // 首次扫描：延迟 ~10秒（等场景加载）后执行一次
        if (G::lastDayId < 0) {
            if (G::firstScanDelayTicks < 600) {  // ~10秒@60fps
                G::firstScanDelayTicks += G::SCAN_INTERVAL_TICKS;
                return;
            }
            // v0.5.5: 瞬态失败不 latch，下轮（5s 后）重试
            if (!AutoPetTickImpl()) return;
            G::lastDayId = dayId;
            Log("[autopet] 首次扫描 dayId=%lld", (long long)dayId);
            return;
        }

        // 同一天：默认不扫描；v0.6.9 补扫时机=每天 17:15 单次（用户拍板 2026-10-05，
        // 原 19:00）。白天新买/新孵化动物当天 caress=0，仅跨日触发摸不到；
        // 17:15 后首个 5s tick 补摸一次，幂等：caress=1 跳过，只摸新增。
        if (dayId == G::lastDayId) {
            if (hour < 17 || (hour == 17 && minute < 15)) return;
            if (G::eveningRescanDoneDay == dayId) return;  // 今天已补扫
            if (!AutoPetTickImpl()) return;  // 幂等：caress=1 跳过，只摸新动物
            G::eveningRescanDoneDay = dayId;
            Log("[autopet] 17:15 补扫 dayId=%lld（幂等，只摸新增）",
                (long long)dayId);
            return;
        }

        // v0.5.5: dayId 回退（TimeFreeze 冻结点在 0 点边界抖动 / 读档回跳）
        // → 重置基准，防止抖动窗口提前 latch 吞掉真实跨日（漏摸一整天）
        if (dayId < G::lastDayId) {
            Log("[autopet] dayId 回退 %lld -> %lld，重置基准",
                (long long)G::lastDayId, (long long)dayId);
            G::lastDayId = dayId;
            return;
        }

        // 跨日了！执行抚摸（caress 已被原生重置为 0）
        // v0.5.5: 扫描瞬态失败不 latch lastDayId，5s 后重试（防漏摸整天）
        if (!AutoPetTickImpl()) return;
        G::lastDayId = dayId;
        Log("[autopet] 检测到跨日 dayId %lld -> %lld，执行抚摸",
            (long long)(dayId - 1), (long long)dayId);
    }
};

AutoPetFeature g_feature;

// ============================================================
// ============================================================
// v0.6.1: 外置配置文件（MOD 文件夹下 config.txt，大佬风格）
//   gain_mult=1        好感倍率（1..100，1=原版数值）
//   （v0.6.3 移除 rescan_minutes：补扫固定为每天 19:00 单次，不可配置）
//   # 开头为注释行。启动时在 HotConfig 后加载（TXT 优先）；
//   运行中每 ~10 秒查 mtime，改完自动生效。
// ============================================================
static wchar_t g_cfgPath[MAX_PATH] = L"";
static ULONGLONG g_cfgMtime = 0;

static ULONGLONG FileMtimeFromFad(const WIN32_FILE_ATTRIBUTE_DATA& fad) {
    return (static_cast<ULONGLONG>(fad.ftLastWriteTime.dwHighDateTime) << 32)
         | fad.ftLastWriteTime.dwLowDateTime;
}

static bool AutoPetLoadConfig(bool verbose) {
    if (g_cfgPath[0] == L'\0') {
        wchar_t dllPath[MAX_PATH] = {};
        if (!GetModuleFileNameW(g_autopetModule, dllPath, MAX_PATH)) return false;
        wchar_t* slash = wcsrchr(dllPath, L'\\');
        if (!slash) return false;
        *slash = L'\0';
        swprintf(g_cfgPath, MAX_PATH, L"%s\\config.txt", dllPath);
    }
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(g_cfgPath, GetFileExInfoStandard, &fad)) {
        if (verbose) Log("[autopet] config.txt 不存在，使用默认值");
        g_cfgMtime = 0;
        return false;  // 无配置文件 → 保持当前值（默认/JSON）
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_cfgPath, L"rb") != 0 || !f) return false;
    char line[256];
    int applied = 0;
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') continue;
        int v = 0;
        if (sscanf(p, "gain_mult=%d", &v) == 1) {
            if (v < 1) v = 1;
            if (v > 100) v = 100;
            rva::GAIN_MULT = v;
            ++applied;
        }
    }
    fclose(f);
    g_cfgMtime = FileMtimeFromFad(fad);
    if (verbose) Log("[autopet] config.txt 应用 %d 项 (gain_mult=%d)",
                     applied, rva::GAIN_MULT);
    return true;
}

// 插件入口（加载器要求：mod_init / mod_tick / unload）
// ============================================================

extern "C" __declspec(dllexport) void mod_init(void) {
    LogOpen("autopet");
    Log("[autopet] mod_init() called");

    // ---- HotConfig 热调参注册 ----
    HotConfig_Register("autopet", (void*)&rva::LIVESTOCK_VTABLE, "rva_LIVESTOCK_VTABLE", HOT_RVA, 0xE29CD8);
    HotConfig_Register("autopet", (void*)&rva::CREATURE_VTABLE, "rva_CREATURE_VTABLE", HOT_RVA, 0xE27A58);
    HotConfig_Register("autopet", (void*)&rva::COM_UNIT_LIVESTOCK_VTABLE, "rva_COM_UNIT_LIVESTOCK_VTABLE", HOT_RVA, 0xE37088);
    HotConfig_Register("autopet", (void*)&rva::GAME_ROOT, "rva_GAME_ROOT", HOT_RVA, 0x10FCBB0);
    HotConfig_Register("autopet", (void*)&rva::ROOT_PLAYER_OFFSET, "rva_ROOT_PLAYER_OFFSET", HOT_RVA, 0x208);
    HotConfig_Register("autopet", (void*)&rva::ROOT_MAP_OWNER_OFFSET, "rva_ROOT_MAP_OWNER_OFFSET", HOT_RVA, 0x268);
    HotConfig_Register("autopet", (void*)&rva::MAP_INFO_OFFSET, "rva_MAP_INFO_OFFSET", HOT_RVA, 0x0d8);
    HotConfig_Register("autopet", (void*)&rva::MAP_SPATIAL_OWNER_OFFSET, "rva_MAP_SPATIAL_OWNER_OFFSET", HOT_RVA, 0x030);
    HotConfig_Register("autopet", (void*)&rva::MAP_SPATIAL_INDEX_OFFSET, "rva_MAP_SPATIAL_INDEX_OFFSET", HOT_RVA, 0x6e0);
    HotConfig_Register("autopet", (void*)&rva::PLAYER_OBJECT_STATUS_OFFSET, "rva_PLAYER_OBJECT_STATUS_OFFSET", HOT_RVA, 0x32b8);
    HotConfig_Register("autopet", (void*)&rva::OBJECT_POSITION_OFFSET, "rva_OBJECT_POSITION_OFFSET", HOT_RVA, 0x0f0);
    HotConfig_Register("autopet", (void*)&rva::LIVESTOCK_LIST_OFFSET, "rva_LIVESTOCK_LIST_OFFSET", HOT_RVA, 0x3430);
    HotConfig_Register("autopet", (void*)&rva::RAW_SECOND_OFFSET, "rva_RAW_SECOND_OFFSET", HOT_RVA, 0x3270);
    HotConfig_Register("autopet", (void*)&rva::SCAN_HOUR_FROM, "rva_SCAN_HOUR_FROM", HOT_INT, 7);
    HotConfig_Register("autopet", (void*)&rva::SCAN_HOUR_TO, "rva_SCAN_HOUR_TO", HOT_INT, 22);
    HotConfig_Register("autopet", (void*)&rva::GAIN_MULT, "gain_mult", HOT_INT, 1);        // v0.6.0: 好感倍率 1..100
    HotConfig_Register("autopet", (void*)&rva::LOVE_SETTER, "rva_LOVE_SETTER", HOT_RVA, 0x2A0D50);
    HotConfig_Register("autopet", (void*)&rva::RATE_MODIFIER, "rva_RATE_MODIFIER", HOT_RVA, 0x13B2C0);
    HotConfig_Register("autopet", (void*)&rva::STATUS_SETTER, "rva_STATUS_SETTER", HOT_RVA, 0x158C50);
    HotConfig_Register("autopet", (void*)&field::CA_MAP_BASE, "field_CA_MAP_BASE", HOT_RVA, 0x10);
    HotConfig_Register("autopet", (void*)&field::MAP_SENTINEL, "field_MAP_SENTINEL", HOT_RVA, 0x10);
    HotConfig_Register("autopet", (void*)&field::MAP_SIZE, "field_MAP_SIZE", HOT_RVA, 0x18);
    HotConfig_Register("autopet", (void*)&field::MAP_BUCKETS, "field_MAP_BUCKETS", HOT_RVA, 0x20);
    HotConfig_Register("autopet", (void*)&field::MAP_MASK, "field_MAP_MASK", HOT_RVA, 0x38);
    HotConfig_Register("autopet", (void*)&field::UNIQUE_ID, "field_UNIQUE_ID", HOT_RVA, 0x0E0);
    HotConfig_Register("autopet", (void*)&field::LIVE_OBJECT, "field_LIVE_OBJECT", HOT_RVA, 0x150);
    HotConfig_Register("autopet", (void*)&field::LIVE_VALID, "field_LIVE_VALID", HOT_RVA, 0x158);
    HotConfig_Register("autopet", (void*)&field::GIMMICK_HOLDER, "field_GIMMICK_HOLDER", HOT_RVA, 0x240);
    HotConfig_Register("autopet", (void*)&field::ANIMAL_HOLDER, "field_ANIMAL_HOLDER", HOT_RVA, 0x290);
    HotConfig_Register("autopet", (void*)&field::PLACEMENT_HOLDER, "field_PLACEMENT_HOLDER", HOT_RVA, 0x2C0);
    HotConfig_Register("autopet", (void*)&field::LOVE, "field_LOVE", HOT_RVA, 0x300);
    HotConfig_Register("autopet", (void*)&field::FEATURE_LOW, "field_FEATURE_LOW", HOT_RVA, 0x328);
    HotConfig_Register("autopet", (void*)&field::FEATURE_HIGH, "field_FEATURE_HIGH", HOT_RVA, 0x330);
    HotConfig_Poll();
    HotConfig_DumpCE("autopet");
    AutoPetLoadConfig(true);  // v0.6.1: 外置配置在 HotConfig 后加载（TXT 优先）
}

extern "C" __declspec(dllexport) void mod_tick(void) {
    qol::budget::BeginFrame();  // P0: 帧锚定（幂等，多 DLL 安全）
    static int s_bdgSlot = -1;  // P0 探针（惰性注册，析构自动上报）
    if (s_bdgSlot < 0) s_bdgSlot = qol::budget::Slot("AutoPet");
    struct BdgGuard {
        int slot; uint64_t t0;
        ~BdgGuard() { qol::budget::Report(slot, qol::budget::NowUs() - t0); }
    } bdgGuard = { s_bdgSlot, qol::budget::NowUs() };

    HotConfig_Poll();
    if (!QolGameBusy()) g_feature.SafeTick();  // P1-2: 载入/菜单期间静默
    // v0.6.1: config.txt 热改检测（每 ~10 秒查一次 mtime）
    static DWORD s_cfgTick = 0;
    if (++s_cfgTick >= 600) {
        s_cfgTick = 0;
        if (g_cfgPath[0]) {
            WIN32_FILE_ATTRIBUTE_DATA fad = {};
            if (GetFileAttributesExW(g_cfgPath, GetFileExInfoStandard, &fad)) {
                ULONGLONG mt = FileMtimeFromFad(fad);
                if (mt != g_cfgMtime) AutoPetLoadConfig(true);
            }
        }
    }
    QolHudGuardVisibility(g_petHudWindow);  // v0.5.11: 失焦隐藏 HUD（不飘桌面）
}

extern "C" __declspec(dllexport) void unload(void) {
    Log("[autopet] unload() called");
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID /*lpReserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_autopetModule = h;  // v0.5.0: HUD 需要模块句柄
        qol::SetLogName("autopet");
        Log("[autopet] DllMain attach \u2014 %s build 25311578", AUTOPET_VERSION);
        g_feature.SafeInit();
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("[autopet] detach");
    }
    return TRUE;
}