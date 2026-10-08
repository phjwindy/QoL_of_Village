// budget.h —— 全局帧预算框架 + 帧耗时诊断 + 每 MOD 耗时探针（跨 DLL 共享，v1.1）
//
// 目标：防止多个 MOD 同帧叠加批量工作造成单帧尖峰。
// 原理：
//   - 命名内存映射 Local\QoL_Budget_v1 提供同进程所有 MOD DLL 的共享状态
//   - 每帧首个调用 BeginFrame() 的 MOD 锚定本帧（不依赖加载顺序）
//   - Ok() 检查从锚点到当前的总消耗是否超预算——先跑的 MOD 吃预算，
//     后跑的自然 defer，实现"全局"预算
//   - Consume() 供各 MOD 自报耗时，帧结束时超阈值打诊断日志
//
// 接入方式（MOD 的 mod_tick）：
//   void mod_tick(void) {
//       qol::budget::BeginFrame();               // 帧管理
//       uint64_t t0 = qol::budget::NowUs();      // 本 MOD tick 开始
//
//       // ... HUD/输入等必须逐帧的逻辑（不受预算约束）...
//
//       for (auto& item : scanQueue) {            // 批量/扫描类工作
//           ProcessItem(item);
//           if (!qol::budget::Ok()) break;       // 超预算 → defer 剩余项
//       }
//
//       qol::budget::Consume(qol::budget::NowUs() - t0);  // 自报耗时
//   }
//
// 线程约定：仅游戏主线程（mod_tick 上下文）调用；x64 对齐 64 位读写原子。
//
// 作者：PHJ&消失的清风，转载或分享时请注明出处。

#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "logging.h"

namespace qol {
namespace budget {

// ============================================================
// 共享状态（命名内存映射 Local\QoL_Budget_v2）
// v1.1: 新增每 MOD 耗时探针槽位（P0 路线图：先量化再优化）
// ============================================================
// 每 MOD 探针条目（F2 面板显示用）
struct BudgetModEntry {
    char name[24];              // MOD 名（Slot() 注册时写入）
    uint64_t last_us;           // 最近一帧自报耗时
    uint64_t ema_us;            // 指数滑动平均（α=1/16）
    uint64_t max_us;            // 历史最大单帧
    uint64_t frames;            // 累计自报次数
};

struct BudgetState {
    uint32_t magic;               // 'QBB2' 版本校验（v1.1 bump：结构扩展）
    uint32_t budget_us;           // 每帧总预算（默认 2000）
    uint64_t anchor_us;           // 本帧锚点（QPC 微秒）
    uint64_t frame_id;            // 帧计数（诊断用）
    uint64_t consumed_us;         // 本帧接入 MOD 累计自报耗时（诊断用）
    uint64_t spike_count;          // 累计尖峰帧数
    uint32_t spike_threshold_us;  // 尖峰判定阈值（默认 3000，诊断用）
    uint32_t log_cooldown;         // 诊断日志限流（帧数）
    uint32_t mod_count;            // v1.1: 已注册 MOD 数
    uint32_t reserved;            // 对齐保留
    BudgetModEntry mods[16];       // v1.1: 每 MOD 探针槽位
};

static constexpr uint32_t kMagic = 0x32424251;  // 'QBB2'（v1.1 结构扩展 bump）
static constexpr uint32_t kDefaultBudgetUs = 2000;
static constexpr uint32_t kDefaultSpikeThresholdUs = 3000;
static constexpr uint32_t kLogCooldownFrames = 300;     // 约 5 秒@60fps
static constexpr uint64_t kFrameGapUs = 4000;           // >4ms 无 tick = 新帧
static constexpr uint32_t kMaxMods = 16;               // v1.1: 探针槽位上限

// ============================================================
// 进程内单例（每 DLL 一份视图，指向同一块共享内存）
// ============================================================
inline BudgetState* g_state = nullptr;
inline HANDLE g_mapping = nullptr;
inline LARGE_INTEGER g_qpcFreq = {};

inline bool EnsureInit() {
    if (g_state) return true;
    if (g_qpcFreq.QuadPart == 0) {
        if (!QueryPerformanceFrequency(&g_qpcFreq) || g_qpcFreq.QuadPart == 0)
            return false;
    }
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                   PAGE_READWRITE, 0, sizeof(BudgetState),
                                   L"Local\\QoL_Budget_v2");
    if (!g_mapping) return false;
    g_state = (BudgetState*)MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS,
                                          0, 0, sizeof(BudgetState));
    if (!g_state) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return false;
    }
    // 首个创建者初始化（幂等：magic 校验避免重复初始化覆盖运行中状态）
    if (g_state->magic != kMagic) {
        memset(g_state, 0, sizeof(BudgetState));
        g_state->magic = kMagic;
        g_state->budget_us = kDefaultBudgetUs;
        g_state->spike_threshold_us = kDefaultSpikeThresholdUs;
    }
    return true;
}

// ============================================================
// 时间源（QPC 微秒）
// ============================================================
inline uint64_t NowUs() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000ULL) / g_qpcFreq.QuadPart);
}

// ============================================================
// API：帧管理
// ============================================================
// mod_tick 开头调用。距锚点超过 kFrameGapUs 视为新帧：
// 先诊断上一帧（consumed_us 超阈值 → 打日志），再重置锚点与累计器。
inline void BeginFrame() {
    if (!EnsureInit()) return;
    uint64_t now = NowUs();
    uint64_t anchor = g_state->anchor_us;
    if (anchor == 0 || now - anchor > kFrameGapUs) {
        // 上一帧尖峰诊断（限流：kLogCooldownFrames 帧内最多一条）
        if (anchor != 0 && g_state->consumed_us > g_state->spike_threshold_us) {
            g_state->spike_count++;
            if (g_state->log_cooldown == 0) {
                g_state->log_cooldown = kLogCooldownFrames;
                Log("[budget] spike: frame=%llu consumed=%lluus "
                    "threshold=%uus spikes=%llu",
                    (unsigned long long)g_state->frame_id,
                    (unsigned long long)g_state->consumed_us,
                    (unsigned)g_state->spike_threshold_us,
                    (unsigned long long)g_state->spike_count);
            }
        }
        if (g_state->log_cooldown > 0) g_state->log_cooldown--;
        g_state->anchor_us = now;
        g_state->frame_id++;
        g_state->consumed_us = 0;
    }
}

// ============================================================
// API：预算查询（热循环用）
// ============================================================
// 剩余预算（微秒）。<=0 表示已超。未初始化时返回大值（不约束）。
inline int64_t Remaining() {
    if (!EnsureInit()) return (int64_t)1 << 30;
    int64_t elapsed = (int64_t)(NowUs() - g_state->anchor_us);
    return (int64_t)g_state->budget_us - elapsed;
}

// 便捷检查：true = 预算还够，继续干活；false = defer 到下帧
inline bool Ok() { return Remaining() > 0; }

// ============================================================
// API：耗时自报（诊断统计）
// ============================================================
// MOD 在 mod_tick 结尾申报本帧耗时（us）。多 MOD 累加到 consumed_us。
inline void Consume(uint64_t us) {
    if (!EnsureInit()) return;
    g_state->consumed_us += us;
}

// ============================================================
// API：配置（可选）
// ============================================================
inline void SetBudgetUs(uint32_t us) {
    if (!EnsureInit()) return;
    if (us >= 100 && us <= 10000) g_state->budget_us = us;
}

inline uint32_t BudgetUs() {
    return EnsureInit() ? g_state->budget_us : kDefaultBudgetUs;
}

// ============================================================
// v1.1 P0 探针：每 MOD 耗时槽位（mod_tick 计时 → F2 面板显示）
// ============================================================
// 注册/获取槽位。mod_init 里调用一次，返回槽位号；重名返回已有槽。
// 槽位满或初始化失败返回 -1（Report 静默忽略）。
inline int Slot(const char* name) {
    if (!EnsureInit() || !name || !name[0]) return -1;
    for (uint32_t i = 0; i < g_state->mod_count && i < kMaxMods; ++i) {
        if (strncmp(g_state->mods[i].name, name, sizeof(BudgetModEntry::name)) == 0)
            return (int)i;
    }
    if (g_state->mod_count >= kMaxMods) return -1;
    uint32_t slot = g_state->mod_count++;
    BudgetModEntry& e = g_state->mods[slot];
    memset(&e, 0, sizeof(e));
    strncpy_s(e.name, sizeof(e.name), name, _TRUNCATE);
    return (int)slot;
}

// 自报本帧耗时（mod_tick 结尾调用）。EMA α=1/16 平滑，供面板稳定显示。
inline void Report(int slot, uint64_t us) {
    if (!EnsureInit() || slot < 0 ||
        slot >= (int)kMaxMods || (uint32_t)slot >= g_state->mod_count) return;
    BudgetModEntry& e = g_state->mods[slot];
    e.last_us = us;
    if (us > e.ema_us) e.ema_us = e.ema_us + ((us - e.ema_us) >> 4);
    else if (us < e.ema_us) e.ema_us = e.ema_us - ((e.ema_us - us) >> 4);
    if (us > e.max_us) e.max_us = us;
    ++e.frames;
}

// 读取端（ModManager F2 面板）：返回共享槽位表视图
inline const BudgetModEntry* ModEntries(uint32_t* count) {
    if (!EnsureInit()) { if (count) *count = 0; return nullptr; }
    if (count) *count = g_state->mod_count < kMaxMods ? g_state->mod_count : kMaxMods;
    return g_state->mods;
}

} // namespace budget
} // namespace qol
