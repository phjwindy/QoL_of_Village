// game_window.h —— 查找游戏主窗口（HUD 定位/前台判断用）
//
// v1.5 变更：废弃 owner 绑定模式（2026-10-05 切窗修复第四轮）。
// v1.0~v1.4 曾推荐 HUD 绑定游戏窗口为 owner（CreateWindowExW 第 8 参传
// game，或 SetWindowLongPtrW GWLP_HWNDPARENT）让 HUD 随游戏最小化隐藏。
// 实测 owned TOPMOST 窗口链干扰 Alt+Tab 前台切换——游戏窗口切不回，
// 全部 MOD 的 owner 绑定已移除。新 HUD 一律：
//   - CreateWindowExW 第 8 参传 nullptr（不设 parent/owner）
//   - HUD 定位/显示器解析用 QolFindGameWindow()
//   - 失焦静默用 QolGameInForeground() 门控 pump，勿用 Hide/Show
//     （Hide/Show 干扰切换流程）；失焦隐藏用 v1.6 守卫 alpha 透明度方案，
//     门控处须兜底调用守卫：if (!QolGameInForeground()) { QolHudGuardVisibility(g_hud); return; }
//
// 作者：PHJ&消失的清风
#pragma once
#include <windows.h>

// 查找游戏主窗口（当前进程的可见顶层窗口，排除工具窗口和有 owner 的窗口）
// 返回：找到返回 HWND，未找到返回 nullptr
// v1.1: 进程内静态缓存（游戏窗口在会话期间不变，失效时重枚举），
//       避免每 tick EnumWindows 开销（守卫每帧调用）
inline HWND QolFindGameWindow() {
    static HWND cached = nullptr;
    if (cached && !IsWindow(cached)) cached = nullptr;
    if (cached) return cached;
    struct FindCtx { DWORD pid; HWND found; };
    FindCtx ctx = { GetCurrentProcessId(), nullptr };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        FindCtx* ctx = reinterpret_cast<FindCtx*>(lp);
        if (!IsWindowVisible(hwnd)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != ctx->pid) return TRUE;
        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW) return TRUE;  // 跳过工具窗口（我们自己创建的 HUD）
        if (GetWindow(hwnd, GW_OWNER)) return TRUE;   // 跳过有 owner 的窗口
        ctx->found = hwnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&ctx));

    cached = ctx.found;
    return cached;
}

// ============================================================
// HUD 可见性守卫（v1.6：透明度方案）
// 问题：WS_EX_TOPMOST 的 HUD 在游戏失焦/切桌面时仍飘在最顶层。
// 历史：v1.2 用 Hide/Show —— 干扰 Windows 切换流程（Alt+Tab 切不回，
//   KB-075），v1.3 关闭守卫（HUD 失焦可见，切窗正常优先）。
// v1.6 方案：SetLayeredWindowAttributes 调 alpha —— 游戏失前台 HUD 全透明，
//   回前台恢复原 alpha。纯渲染层变化（DWM 合成），不产生窗口消息、
//   不触发窗口管理器重评估，对 Alt+Tab 切换零干扰。
// 防抖 200ms：前台观察持续稳定才执行（切窗抖动期不动作防闪烁）。
// per-HUD 状态存窗口属性（禁 static 共享 —— ModManager 双窗口互扰教训）。
// fail-open：游戏窗口找不到时不动作。
// ⚠ 用法约定：守卫须在游戏失焦时仍被调用 —— pump 门控处不能裸 return，
//   须写成 if (!QolGameInForeground()) { QolHudGuardVisibility(g_hud); return; }
//   （守卫在 pump 末尾调用时失焦不可达；门控处兜底调用保证失焦渐隐生效）
// ============================================================
// MOD 主动隐藏自己的 HUD 时调用：清除守卫状态，防守卫恢复错误 alpha
inline void QolHudMarkHiddenByMod(HWND hud) {
    if (hud) {
        RemovePropW(hud, L"QolHudDim");
        RemovePropW(hud, L"QolHudAlpha");
    }
}

// v1.4: 游戏非前台时 HUD pump 完全静默（fix: 每 tick 的 SetWindowPos/PeekMessage/
//       重绘操作在切窗期干扰 Windows 窗口切换流程 → 游戏切不回。非前台时跳过
//       整个 HUD pump，切窗期所有 HUD 窗口操作停止，不触发窗口管理器任何动作。
inline bool QolGameInForeground() {
    HWND game = QolFindGameWindow();
    return game && IsWindowVisible(game) && !IsIconic(game) &&
           GetForegroundWindow() == game;
}

// v1.6 守卫：游戏失焦 → HUD alpha=0 全透明；回前台 → 恢复原 alpha（200ms 防抖）
inline void QolHudGuardVisibility(HWND hud) {
    if (!hud) return;
    HWND game = QolFindGameWindow();
    if (!game) return;                                   // fail-open
    // 仅对 LAYERED 窗口有效（非 layered 读不到属性，自然跳过）
    COLORREF key = 0; BYTE alpha = 0; DWORD lwaFlags = 0;
    if (!GetLayeredWindowAttributes(hud, &key, &alpha, &lwaFlags)) return;

    bool fg = (GetForegroundWindow() == game);
    INT_PTR pendVal = (INT_PTR)GetPropW(hud, L"QolHudPend");
    INT_PTR pendAt  = (INT_PTR)GetPropW(hud, L"QolHudPendAt");

    if (pendVal != (INT_PTR)(fg ? 2 : 1)) {               // 前台观察变化 → 重置计时
        SetPropW(hud, L"QolHudPend", (HANDLE)(INT_PTR)(fg ? 2 : 1));
        SetPropW(hud, L"QolHudPendAt", (HANDLE)(INT_PTR)GetTickCount());
        return;
    }
    if (GetTickCount() - (DWORD)pendAt < 200) return;    // 未稳定 200ms → 不动作

    if (fg) {
        if (!GetPropW(hud, L"QolHudDim")) return;         // 本来就正常
        INT_PTR saved = (INT_PTR)GetPropW(hud, L"QolHudAlpha");
        BYTE restore = saved ? (BYTE)(saved > 255 ? 255 : saved) : 228;
        SetLayeredWindowAttributes(hud, 0, restore, LWA_ALPHA);
        RemovePropW(hud, L"QolHudDim");
        RemovePropW(hud, L"QolHudAlpha");
    } else {
        if (GetPropW(hud, L"QolHudDim")) return;          // 已变暗
        SetPropW(hud, L"QolHudAlpha", (HANDLE)(INT_PTR)alpha);  // 保存当前 alpha
        SetPropW(hud, L"QolHudDim", (HANDLE)(INT_PTR)1);
        SetLayeredWindowAttributes(hud, 0, 0, LWA_ALPHA);
    }
}

