// eyefix.cpp —— 结局后保持正常眼 (EyeFix v1.2.7 正式版 for v1.20)
//
// 原理（源自 BigL233 dinput8.cpp 1123-1170 行）：
//   PlayerSetupAnimeColor 函数中有一条条件跳转：
//     bt  rax, 0x0D        ; 测试 flag bit 13（是否进入"坏眼"分支）
//     jae normal_eye       ; 如果 bit=0 跳到 normal_eye（正常眼）
//   游戏在结局后会设置该 flag，导致 jae 不跳转，进入坏眼渲染。
//   补丁：将 jae(0F 83) 替换为 nop+jmp(90 E9)，使用相同偏移，
//   无条件跳转到 normal_eye 分支，强制使用正常眼资源。
//
// 补丁点：RVA 0x12FD2D（build 25094764 / v1.09）
//   原始：0F 83 EF 00 00 00  (jae +0xEF)
//   补丁：90 E9 EF 00 00 00  (nop; jmp +0xEF)
//
// 切换键：键盘 0  循环 结局眼(原生) <-> 正常眼(补丁)
// 默认：不应用补丁（结局眼），按 0 后启用正常眼。
// 左上角 HUD 显示当前状态，5 秒后消退。

#include <windows.h>
#include <cstdint>
#include <atomic>

#include "logging.h"
#include "game_window.h"   // HUD owner 绑定游戏窗口
#include "budget.h"        // P0 帧耗时探针（跨 DLL 共享）
#include "state.h"         // P1-2 状态感知暂停（载入/菜单静默）
#include "hot_config.h"
#include "quickmenu.h"
#include "patch_safety.h"

// 日志开关：发布版禁用日志输出
// 调试时取消注释下行即可开启日志
// #define EYEFIX_LOGGING   // v1.2.13 转正：日志关闭（定位时取消注释重编）
#ifdef EYEFIX_LOGGING
#else
  #define LogOpen(x)  ((void)0)
  #define Log(...)    ((void)0)
  #define LogV(...)   ((void)0)
  #define LogClose()  ((void)0)
#endif

// ============================================================
// 常量（build 25094764 / v1.09）
// ============================================================
static volatile uintptr_t RVA_EYE_BRANCH = 0x13A00D;

static const unsigned char kEyeOriginal[6] = { 0x0F, 0x83, 0xEF, 0x00, 0x00, 0x00 };
static const unsigned char kEyeNormal[6]   = { 0x90, 0xE9, 0xEF, 0x00, 0x00, 0x00 };

// ============================================================
// 全局状态
// ============================================================
namespace G {
    uintptr_t base = 0;
    bool ready = false;
    bool available = false;   // 字节签名验证通过
    std::atomic<bool> patched{false};  // 当前是否已打补丁（正常眼）
}

// v1.2.0: 独立热键已移除，开关由 QoL 快捷菜单（F1）驱动

// ============================================================
// 内存写入
// ============================================================
static bool WriteMem(void* target, const void* data, size_t size) {
    return qol::WritePatchChecked(target, data, size);
}

// ============================================================
// 补丁切换
// ============================================================
static bool ToggleEyePatch(bool enable) {
    if (!G::available) return false;
    unsigned char* target = reinterpret_cast<unsigned char*>(G::base + RVA_EYE_BRANCH);
    const unsigned char* source = enable ? kEyeNormal : kEyeOriginal;

    if (!WriteMem(target, source, sizeof(kEyeOriginal)) ||
        memcmp(target, source, sizeof(kEyeOriginal)) != 0) {
        Log("[EyeFix] 补丁写入失败 enable=%d", enable ? 1 : 0);
        return false;
    }

    G::patched.store(enable, std::memory_order_relaxed);
    Log("[EyeFix] 补丁%s", enable ? "已启用 (normal-eye)" : "已禁用 (original)");
    return true;
}

// ============================================================
// 验证字节签名
// ============================================================
static bool VerifyEyeBranch() {
    const uintptr_t base = G::base;
    unsigned char* target = reinterpret_cast<unsigned char*>(base + RVA_EYE_BRANCH);

    const bool isOriginal = memcmp(target, kEyeOriginal, sizeof(kEyeOriginal)) == 0;
    const bool isPatched  = memcmp(target, kEyeNormal, sizeof(kEyeNormal)) == 0;
    if (!isOriginal && !isPatched) {
        Log("[EyeFix] 字节验证失败 rva=0x%llX: 得到 ",
            (unsigned long long)RVA_EYE_BRANCH);
        for (size_t i = 0; i < sizeof(kEyeOriginal); ++i)
            Log("%02X", target[i]);
        Log("; 补丁不可用");
        return false;
    }

    Log("[EyeFix] 字节验证通过 (状态: %s)", isPatched ? "已补丁" : "原始");
    return true;
}

// ============================================================
// HUD 浮现窗口：左上角显示当前眼睛状态，5秒后消退
// ============================================================
static HMODULE g_eyefixModule = nullptr;
static constexpr wchar_t EYEFIX_HUD_CLASS[] = L"EyeFixHudWindow";
static HWND g_hudWindow = nullptr;
static HFONT g_hudFont = nullptr;
static HFONT g_hudFontSmall = nullptr;
static int g_hudState = -1;  // 0=结局眼, 1=正常眼
static ULONGLONG g_hudHideAt = 0;

// HUD 画刷（文件作用域，unload 可清理）
static HBRUSH g_hudBgBrush = nullptr;
static HBRUSH g_hudAccentBrushes[2] = {};

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

        // 背景（缓存画刷，避免每帧创建/销毁）
        if (!g_hudBgBrush) g_hudBgBrush = CreateSolidBrush(RGB(28, 30, 34));
        FillRect(dc, &client, g_hudBgBrush);

        // 左侧强调色条（2 色缓存）
        if (!g_hudAccentBrushes[0]) {
            g_hudAccentBrushes[0] = CreateSolidBrush(RGB(91, 192, 122));   // 绿色=正常眼
            g_hudAccentBrushes[1] = CreateSolidBrush(RGB(220, 90, 80));     // 红色=结局眼
        }
        const int state = g_hudState;
        RECT bar = client;
        bar.right = bar.left + 6;
        FillRect(dc, &bar, g_hudAccentBrushes[state == 1 ? 0 : 1]);

        SetBkMode(dc, TRANSPARENT);

        // 标题
        SetTextColor(dc, RGB(150, 150, 155));
        HFONT oldFont = (HFONT)SelectObject(dc, g_hudFontSmall);
        RECT titleRect = client;
        titleRect.left += 22;
        titleRect.right -= 14;
        titleRect.bottom = titleRect.top + 22;
        DrawTextW(dc, L"\x773C\x775B\x72B6\x6001", -1, &titleRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // 状态文字
        const wchar_t* stateText;
        COLORREF stateColor;
        if (state == 1) {
            stateText = L"\x6B63\x5E38\x773C";  // 正常眼
            stateColor = RGB(130, 220, 150);
        } else {
            stateText = L"\x7ED3\x5C40\x773C";  // 结局眼
            stateColor = RGB(220, 130, 120);
        }
        SelectObject(dc, g_hudFont);
        SetTextColor(dc, stateColor);
        RECT stateRect = client;
        stateRect.left += 22;
        stateRect.right -= 14;
        stateRect.top += 24;
        DrawTextW(dc, stateText, -1, &stateRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

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
    cls.hInstance = g_eyefixModule;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    cls.lpszClassName = EYEFIX_HUD_CLASS;
    if (!RegisterClassExW(&cls)) {
        DWORD err = GetLastError();
        WNDCLASSEXW existing = {};
        existing.cbSize = sizeof(existing);
        if (err != ERROR_CLASS_ALREADY_EXISTS ||
            !GetClassInfoExW(g_eyefixModule, EYEFIX_HUD_CLASS, &existing) ||
            existing.lpfnWndProc != HudWndProc ||
            existing.hInstance != g_eyefixModule) {
            Log("[EyeFix] [HUD] window class register failed (err=%lu)", err);
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
        Log("[EyeFix] [HUD] font creation failed");
        return false;
    }

    const int width = 180;
    const int height = 62;
    g_hudWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        EYEFIX_HUD_CLASS, L"", WS_POPUP, 0, 0, width, height,
        nullptr, nullptr, g_eyefixModule, nullptr);
    if (!g_hudWindow) {
        Log("[EyeFix] [HUD] CreateWindowExW failed (err=%lu)", GetLastError());
        return false;
    }
    // v1.2.11: 移除游戏窗口 owner 绑定（GWLP_HWNDPARENT）——owned TOPMOST 窗口链
    // 干扰 Alt+Tab 前台切换，游戏窗口切不回（切窗修复第四轮，详见 game_window.h v1.5）

    SetLayeredWindowAttributes(g_hudWindow, 0, 228, LWA_ALPHA);
    HRGN rounded = CreateRoundRectRgn(0, 0, width + 1, height + 1, 12, 12);
    if (!SetWindowRgn(g_hudWindow, rounded, FALSE)) DeleteObject(rounded);

    Log("[EyeFix] [HUD] overlay window ready");
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
    const int height = 62;
    const int x = mi.rcWork.left + 24;
    const int y = mi.rcWork.top + 24;
    SetWindowPos(g_hudWindow, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void RefreshHud(bool patched) {
    if (!g_hudWindow && !InitHud()) return;
    g_hudState = patched ? 1 : 0;
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
// 开关切换（v1.2.0：供 QoL 快捷菜单调用，原独立热键 0 已移除）
// ============================================================
static void ToggleEyeFix() {
    if (!G::available) return;
    bool current = G::patched.load(std::memory_order_relaxed);
    bool next = !current;
    if (ToggleEyePatch(next)) {
        Log("[EyeFix] 切换: %s -> %s",
            current ? "正常眼" : "结局眼",
            next ? "正常眼" : "结局眼");
        RefreshHud(next);
    }
}

static bool IsEyeFixOn() {
    return G::patched.load(std::memory_order_relaxed);
}

// ============================================================
// QoL 快捷菜单导出（宿主：ModManager F1）
// ============================================================
extern "C" __declspec(dllexport) int QolQuickMenuItems(
        QolQuickMenuItem* items, int maxItems) {
    int n = 1;  // 正常眼/结局眼切换
    if (items && maxItems >= 1)
        items[0] = { "正常眼 (EyeFix)", IsEyeFixOn, ToggleEyeFix };
    return n;
}

// ============================================================
// 插件入口
// ============================================================
extern "C" __declspec(dllexport) void mod_init(void) {
    LogOpen("eyefix");
    Log("[EyeFix] mod_init 开始");
    HotConfig_Register("eyefix", (void*)&RVA_EYE_BRANCH, "RVA_EYE_BRANCH", HOT_RVA, 0x13A00D);
    HotConfig_Poll();
    HotConfig_DumpCE("eyefix");
    G::base = (uintptr_t)GetModuleHandleW(nullptr);
    Log("[EyeFix] 游戏基址: 0x%llX", (unsigned long long)G::base);

    // 验证字节签名（不打补丁，默认结局眼）
    G::available = VerifyEyeBranch();
    if (!G::available) {
        Log("[EyeFix] [警告] 字节验证失败，补丁不可用（不影响游戏正常运行）");
    } else {
        Log("[EyeFix] 就绪，默认结局眼（F1 快捷菜单可切换正常眼）");
    }

    G::ready = true;
    Log("[EyeFix] mod_init 完成 (ready=%d available=%d)",
        G::ready ? 1 : 0, G::available ? 1 : 0);
}

extern "C" __declspec(dllexport) void mod_tick(void) {
    qol::budget::BeginFrame();  // P0: 帧锚定（幂等，多 DLL 安全）
    static int s_bdgSlot = -1;  // P0 探针（惰性注册，析构自动上报）
    if (s_bdgSlot < 0) s_bdgSlot = qol::budget::Slot("EyeFix");
    struct BdgGuard {
        int slot; uint64_t t0;
        ~BdgGuard() { qol::budget::Report(slot, qol::budget::NowUs() - t0); }
    } bdgGuard = { s_bdgSlot, qol::budget::NowUs() };

    if (!G::ready) return;
    if (!QolGameBusy()) HotConfig_Poll();  // P1-2: 载入/菜单期间跳过配置轮询
    PumpHud();
    QolHudGuardVisibility(g_hudWindow);  // v1.2.6: 失焦隐藏 HUD（不飘桌面）
}

extern "C" __declspec(dllexport) void unload(void) {
    Log("[EyeFix] unload");
    // 清理 HUD 资源
    if (g_hudWindow) { DestroyWindow(g_hudWindow); g_hudWindow = nullptr; }
    if (g_hudFont) { DeleteObject(g_hudFont); g_hudFont = nullptr; }
    if (g_hudFontSmall) { DeleteObject(g_hudFontSmall); g_hudFontSmall = nullptr; }
    // 清理画刷
    if (g_hudBgBrush) { DeleteObject(g_hudBgBrush); g_hudBgBrush = nullptr; }
    for (int i = 0; i < 2; ++i) {
        if (g_hudAccentBrushes[i]) { DeleteObject(g_hudAccentBrushes[i]); g_hudAccentBrushes[i] = nullptr; }
    }
    // 补丁不还原（进程退出即失效）
    LogClose();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)lpReserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_eyefixModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
