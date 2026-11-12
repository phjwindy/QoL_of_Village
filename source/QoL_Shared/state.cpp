// state.cpp -- Game State Awareness implementation (P1-2 共享机制层)
//
// 仅依赖 <windows.h> + 自带 SEH 读助手，不 include 其他 QoL_Shared 头文件，
// 最大限度降低耦合（另一个代理正在并行改 14 个 MOD 源码，避免依赖波动）。
//
// 编译：各 MOD 项目将本文件加入编译列表即可（与 logging.cpp / hot_config.cpp 同模式）。
// 未加入编译前不参与构建，天然安全。
//
// 作者：PHJ&消失的清风
#include "state.h"

// ============================================================
// 常量（v1.20 / build 25311578）
// ============================================================
// clock LEA 指令地址（mov rax,[rip+disp32]）
static constexpr uintptr_t RVA_CLOCK_LEA    = 0xE2EA3;   // 20261028 纠偏: 0x1A9C3B 系 10-27 迁移误配(旧exe该址=GAME_ROOT加载lea); AOB 唯一命中, 目标=0x111CC90(新GAME_ROOT)铁证
// 游戏根对象全局指针（来自 ProductionAuto/version_manifest.h v1.20）
static constexpr uintptr_t RVA_GAME_ROOT = 0x111cc90;
// clock_mgr → save 对象偏移（TimeFreeze 实测）
static constexpr uintptr_t CLOCK_SAVE_OFF   = 0x208;
// save 对象内时钟值偏移（TimeFreeze TIME_B_OFF）
static constexpr uintptr_t CLOCK_TIME_B_OFF = 0x3270;
// 时钟停走判定阈值（毫秒）—— 超过此时间时钟未变 → 判定暂停/菜单
static constexpr ULONGLONG  CLOCK_STALL_MS  = 1000;
// 指针最小有效值（同 TimeFreeze ReadPtr 约定）
static constexpr uintptr_t  PTR_MIN_VALID   = 0x10000;

// ============================================================
// 运行时状态（per-DLL 静态，仅游戏主线程调用）
// ============================================================
static uintptr_t s_exeBase = 0;
static bool s_preflightOk = false;

// 时钟停走检测状态
static uint64_t  s_lastClockValue    = 0;
static ULONGLONG s_lastClockChangeMs  = 0;   // 时钟值上次发生变化的时间戳
static bool      s_clockBaselineSet   = false;

// ============================================================
// SEH 安全读取（同 TimeFreeze 模式：纯 POD 函数，无 C++ 析构对象）
// ============================================================

// 读取一个指针值，带 SEH 保护
static bool SafeReadPtr(void* p, void** out) {
    if (!p) return false;
    uintptr_t v = 0;
    __try {
        v = *reinterpret_cast<uintptr_t*>(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (v < PTR_MIN_VALID) return false;
    *out = reinterpret_cast<void*>(v);
    return true;
}

// 读取一个 uint64 值，带 SEH 保护
static bool SafeReadU64(void* p, uint64_t* out) {
    if (!p || !out) return false;
    __try {
        *out = *reinterpret_cast<uint64_t*>(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// ============================================================
// Preflight：验证 exe 基址 + clock LEA 指令签名
// 提炼自 TimeFreeze::Preflight()
// ============================================================
static bool EnsureInit() {
    if (s_preflightOk) return true;
    s_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!s_exeBase) return false;
    // 验证 clock LEA 处为 48 8B 05（mov rax,[rip+disp32]）
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s_exeBase);
    __try {
        if (p[RVA_CLOCK_LEA] != 0x48 || p[RVA_CLOCK_LEA + 1] != 0x8B ||
            p[RVA_CLOCK_LEA + 2] != 0x05) {
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    s_preflightOk = true;
    return true;
}

// ============================================================
// 获取 save 对象（提炼自 TimeFreeze::GetSave()）
// 读链：[exe+RVA_CLOCK_LEA] → mov rax,[rip+disp32] → clock_mgr
//       [clock_mgr + 0x208] → save
// 返回 nullptr = 链断裂（载入/场景切换/读档窗口）
// ============================================================
static void* GetSave() {
    if (!EnsureInit()) return nullptr;

    // 解析 mov rax,[rip+disp32] 的目标地址
    const int32_t* disp = reinterpret_cast<const int32_t*>(
        s_exeBase + RVA_CLOCK_LEA + 3);
    uintptr_t addr;
    __try {
        addr = s_exeBase + RVA_CLOCK_LEA + 7 + *disp;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }

    void* clockMgr = nullptr;
    if (!SafeReadPtr(reinterpret_cast<void*>(addr), &clockMgr))
        return nullptr;

    void* save = nullptr;
    if (!SafeReadPtr(reinterpret_cast<void*>(
            reinterpret_cast<unsigned char*>(clockMgr) + CLOCK_SAVE_OFF), &save))
        return nullptr;

    return save;
}

// ============================================================
// 检测 A：QolLoading —— 载入/场景切换
// 机制 A1：save 链断裂（TimeFreeze 同源）
// 机制 A2：game root 全局指针空（HuntOneShot 同源）
// 可靠性：实测可靠（HIGH）
// ============================================================
bool QolLoading(void) {
    if (!EnsureInit()) return false;  // fail-open

    // A1: save 链断裂 → 载入/场景切换
    void* save = GetSave();
    if (!save) return true;

    // A2: game root 指针空 → 标题画面/载入
    // 来源：HuntOneShot::ReadSceneFingerprint 第一步
    //       ProductionAuto/version_manifest.h RVA_GAME_ROOT
    void* gameRoot = nullptr;
    if (!SafeReadPtr(reinterpret_cast<void*>(s_exeBase + RVA_GAME_ROOT),
                     &gameRoot)) {
        // 全局指针地址本身不可读 → 极端情况（exe 映射未完成）
        return true;  // 判定为载入中
    }
    if (!gameRoot) return true;  // game root 未初始化 → 标题/载入

    return false;
}

// ============================================================
// 检测 B：QolInMenu —— 菜单/游戏暂停（启发式）
// 机制：时钟值（TIME_B）连续 > 1s 未变化 → 判定暂停
// 可靠性：高概率（MEDIUM）—— 需实测确认时钟在菜单时确实停走
// fail-open：读失败/未初始化 → false
// ============================================================
bool QolInMenu(void) {
    if (!EnsureInit()) return false;  // fail-open

    // 载入中不判定菜单（避免误报）
    if (QolLoading()) return false;

    void* save = GetSave();
    if (!save) return false;  // 链断 → QolLoading 已覆盖，此处 fail-open

    uint64_t clockNow = 0;
    if (!SafeReadU64(reinterpret_cast<void*>(
            reinterpret_cast<unsigned char*>(save) + CLOCK_TIME_B_OFF),
            &clockNow)) {
        return false;  // 读失败 → fail-open
    }

    ULONGLONG nowMs = GetTickCount64();

    if (!s_clockBaselineSet) {
        // 首次采样：建立基线，返回 false
        s_lastClockValue   = clockNow;
        s_lastClockChangeMs = nowMs;
        s_clockBaselineSet  = true;
        return false;
    }

    if (clockNow != s_lastClockValue) {
        // 时钟在走 → 更新基线，非暂停
        s_lastClockValue    = clockNow;
        s_lastClockChangeMs = nowMs;
        return false;
    }

    // 时钟未变 → 检查持续时间
    ULONGLONG elapsed = nowMs - s_lastClockChangeMs;
    if (elapsed >= CLOCK_STALL_MS) {
        // 超过阈值 → 判定菜单/暂停
        // 已知限制：TimeFreeze 激活时也会触发（可接受：冻结时 MOD 静默合理）
        return true;
    }

    return false;
}

// ============================================================
// 检测 C：QolInCutscene —— 剧情演出
// 待定位：现有代码无可靠检测机制
// 潜在方向：GIMMICK_MGR(0x1104C80) 事件标志 / game_mode 字段
// 当前：fail-open 返回 false
// ============================================================
bool QolInCutscene(void) {
    // TODO: 待定位
    // 候选方向：
    //   1. GIMMICK_MGR (RVA 0x1104C80) 内事件/演出状态标志
    //   2. 游戏根对象内 game_mode / scene_type 字段
    //   3. 需 IDA/CE 逆向定位后补充
    return false;  // fail-open
}

// ============================================================
// 检测 D：QolSleeping —— 睡觉
// 待定位：睡觉时时钟快速前进，与时钟停走正交
// 当前：fail-open 返回 false
// ============================================================
bool QolSleeping(void) {
    // TODO: 待定位
    // 候选方向：
    //   1. 床交互标志 / 睡眠动画状态机
    //   2. 事件标志位
    //   3. 需 IDA/CE 逆向定位后补充
    return false;  // fail-open
}

// ============================================================
// 主 API：QolGameBusy —— 任意忙状态
// = QolLoading() || QolInMenu()
// （QolInCutscene / QolSleeping 待定位，暂不纳入 → fail-open）
// ============================================================
bool QolGameBusy(void) {
    // 短路求值：先查最可靠的 Loading，再查启发式的 Menu
    if (QolLoading()) return true;
    if (QolInMenu()) return true;
    // QolInCutscene() / QolSleeping() 待定位，不纳入
    return false;
}
