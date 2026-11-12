// debug_hud.cpp —— QoL 调试 HUD（列出当前加载的 MOD 及状态；默认隐藏，异常自动弹出，F8 手动切换）v1.0.1
//
// 独立 MOD：基座加载器扫 mods\ 子目录，LoadLibrary 本 DLL 进游戏进程。
// 不依赖 ModManager；如需面板集成，后续可在 ModManager g_mods[] 登记。
//
// 浮层约定（沿用 QoL_Shared/game_window.h）：
//   - WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW
//   - 在游戏线程（首个 mod_tick）建窗，由游戏消息循环泵 WM_PAINT
//   - 失焦用 QolHudGuardVisibility 渐隐（alpha 方案，不干扰 Alt+Tab）
//
// 默认隐藏（不打扰）；当检测到预期 MOD 未正常加载时自动弹出。F8 可随时手动切换显示/隐藏。

#include <windows.h>
#include <string>
#include "logging.h"
#include "feature.h"
#include "game_window.h"
#include "exe_guard.h"   // 统一游戏版本闸门：非适配 build 不启用

namespace {

// ---- 配置 ----
static const wchar_t* const HUD_CLASS       = L"QolDebugHudWnd";
static const int          TOGGLE_VK         = VK_F8;   // 切换显示/隐藏
static const int          PAINT_INTERVAL_MS = 200;     // 刷新节流
static const int          HUD_X = 10, HUD_Y = 10;      // 左上角（避让游戏 UI 改这里）

// ---- MOD 清单（显示名 + dll 名，源自 ModManager g_mods[]）----
struct ModInfo { const wchar_t* name; const wchar_t* dll; };
static const ModInfo kMods[] = {
    { L"ChestSort 箱子自动归类",     L"chestsort.dll" },
    { L"Sower 范围播种",             L"sower.dll" },
    { L"EyeFix 正常眼",              L"eyefix.dll" },
    { L"MonsterMark 怪物标记",       L"monstermark.dll" },
    { L"AutoFish 自动钓鱼",          L"autofish.dll" },
    { L"ProductionAuto 链式自动化",  L"productionauto.dll" },
    { L"Scarecrow 稻草人重叠",       L"scarecrow.dll" },
    { L"AutoHarvest 自动采集",       L"autoharvest.dll" },
    { L"SickleHarvest 镰刀范围收割", L"sickleharvest.dll" },
    { L"AutoPet 自动摸摸",           L"autopet.dll" },
    { L"BirthdayReminder 生日提醒",  L"birthdayreminder.dll" },
    { L"CameraZoom 镜头变焦",        L"camerazoom.dll" },
    { L"HuntOneShot 一击猎杀",       L"huntoneshot.dll" },
    { L"MineHelper 采矿助手",        L"minehelper.dll" },
    { L"SelfServiceStore 自助商店",   L"selfservice.dll" },
    { L"TimeFreeze 时间冻结",        L"timefreeze.dll" },
    { L"SaveGuard 存档守护",         L"saveguard.dll" },
    { L"ModManager 模块管理器",       L"modmanager.dll" },
};
static const int kModCount = (int)(sizeof(kMods) / sizeof(kMods[0]));

// ---- 全局状态 ----
static HWND    g_hWnd     = nullptr;
static HMODULE g_hMod     = nullptr;
static bool    g_visible  = false;            // 当前是否显示（默认隐藏，有问题时才自动弹出）
static bool    g_f8Prev   = false;
static bool    g_prevProblem = false;         // 上一帧是否检测到问题（用于"新问题"触发自动弹出）
static bool    g_classReg = false;
static DWORD   g_lastPaint= 0;
static HFONT   g_hFont    = nullptr;

enum class ManualMode { None, ForceHide, ForceShow };
static ManualMode g_manual = ManualMode::None; // F8 手动覆盖：None=随自动，ForceHide=强制隐藏，ForceShow=强制显示

static bool IsModLoaded(const wchar_t* dll) {
    return GetModuleHandleW(dll) != nullptr;
}

// 检测"问题"：任一预期 MOD 未加载（GetModuleHandle 命中失败）即视为问题 → 自动弹出
static bool DetectProblem() {
    int loaded = 0;
    for (int i = 0; i < kModCount; ++i) if (IsModLoaded(kMods[i].dll)) ++loaded;
    return loaded < kModCount;
}

static void DrawLine(HDC hdc, int x, int y, const std::wstring& t, COLORREF c) {
    SetTextColor(hdc, c);
    TextOutW(hdc, x, y, t.c_str(), (int)t.size());
}

static LRESULT CALLBACK HudWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        SetBkMode(hdc, TRANSPARENT);

        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(18, 20, 28));
        FillRect(hdc, &rc, bg); DeleteObject(bg);

        if (g_hFont) SelectObject(hdc, g_hFont);

        const int lineH = 20;
        int x = 10, y = 8;

        DrawLine(hdc, x, y, L"QoL 调试 HUD   (F8 手动切换 · 异常自动弹出)", RGB(120, 200, 255));
        y += lineH + 4;

        int loaded = 0;
        for (int i = 0; i < kModCount; ++i) if (IsModLoaded(kMods[i].dll)) ++loaded;
        bool prob = loaded < kModCount;
        std::wstring stat = (prob ? L"⚠ 已加载 " : L"已加载 ") + std::to_wstring(loaded) + L"/" +
                            std::to_wstring(kModCount) + L"   前台:" +
                            (QolGameInForeground() ? L"是" : L"否");
        DrawLine(hdc, x, y, stat, prob ? RGB(255, 120, 120) : RGB(180, 180, 180));
        y += lineH;

        HPEN pen = CreatePen(PS_SOLID, 1, RGB(60, 64, 80));
        SelectObject(hdc, pen);
        MoveToEx(hdc, x, y, nullptr);
        LineTo(hdc, rc.right - 10, y);
        DeleteObject(pen);
        y += 4;

        for (int i = 0; i < kModCount; ++i) {
            bool ok = IsModLoaded(kMods[i].dll);
            COLORREF c = ok ? RGB(90, 230, 120) : RGB(230, 180, 70);
            std::wstring line = (ok ? L"[●] " : L"[○] ") + std::wstring(kMods[i].name);
            DrawLine(hdc, x, y, line, c);
            y += lineH;
        }
        EndPaint(hwnd, &ps);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool CreateHudWindow() {
    if (!g_classReg) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = HudWndProc;
        wc.hInstance     = g_hMod;
        wc.lpszClassName = HUD_CLASS;
        if (!RegisterClassExW(&wc)) {
            qol::LogE("[debughud] RegisterClassExW failed");
            return false;
        }
        g_classReg = true;
    }

    int h = 8 + 24 + 20 + 4 + kModCount * 20 + 8;
    int w = 380;

    // v1.0.2【KB-113】原来用固定屏幕坐标 (10,10)，窗口化/4K DPI 下整个面板
    //   落在游戏窗口外的桌面左上角（实测重叠率 0）。改为锚游戏客户区。
    int ax = HUD_X, ay = HUD_Y;
    QolAnchorToClient(HUD_X, HUD_Y, w, h, &ax, &ay);
    g_hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        HUD_CLASS, L"",
        WS_POPUP,                 // 默认不带 WS_VISIBLE，由 Tick 的可见性逻辑控制
        ax, ay, w, h,
        nullptr, nullptr, g_hMod, nullptr);
    if (!g_hWnd) { qol::LogE("[debughud] CreateWindowExW failed"); return false; }

    SetLayeredWindowAttributes(g_hWnd, 0, 205, LWA_ALPHA);
    g_hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, FF_DONTCARE, L"Microsoft YaHei");
    qol::Log("[debughud] window created (%dx%d)", w, h);
    return true;
}

static void ApplyVisibility(bool show) {
    if (!g_hWnd) return;
    if (show) {
        // 每次显示重算位置：游戏窗口可能被移动/改尺寸，固定坐标会再次飞出框外。
        RECT rc = {};
        GetClientRect(g_hWnd, &rc);
        int ax = HUD_X, ay = HUD_Y;
        QolAnchorToClient(HUD_X, HUD_Y, rc.right, rc.bottom, &ax, &ay);
        SetWindowPos(g_hWnd, HWND_TOPMOST, ax, ay, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(g_hWnd, SW_SHOW);
        InvalidateRect(g_hWnd, nullptr, FALSE);
    }
    else       { ShowWindow(g_hWnd, SW_HIDE); }
}

class DebugHudFeature : public qol::Feature {
public:
    DebugHudFeature() : Feature("debughud") {}
    bool Init() override { return true; }   // 窗口延迟到首个 Tick 创建（避开 DllMain 加载锁）
    void Tick() override {
        if (!g_hWnd && !CreateHudWindow()) return;

        bool hasProblem = DetectProblem();
        // 新问题出现时清除"强制隐藏"覆盖，使自动弹出重新生效
        if (hasProblem && !g_prevProblem) g_manual = ManualMode::None;
        g_prevProblem = hasProblem;

        // F8：手动切换显示/隐藏（覆盖自动逻辑）
        SHORT s = GetAsyncKeyState(TOGGLE_VK);
        bool down = (s & 0x8000) != 0;
        if (down && !g_f8Prev) {
            g_manual = g_visible ? ManualMode::ForceHide : ManualMode::ForceShow;
            bool desired = (g_manual == ManualMode::ForceShow);
            if (desired != g_visible) { g_visible = desired; ApplyVisibility(desired); }
            qol::Log("[debughud] 手动切换 -> %s", desired ? "显示" : "隐藏");
        }
        g_f8Prev = down;

        // 自动逻辑：无手动覆盖时，有问题才显示
        bool desired;
        if (g_manual == ManualMode::ForceShow)       desired = true;
        else if (g_manual == ManualMode::ForceHide)  desired = false;
        else                                          desired = hasProblem;

        if (desired != g_visible) { g_visible = desired; ApplyVisibility(desired); }

        if (g_visible) {
            QolHudGuardVisibility(g_hWnd);        // 失焦渐隐（game_window.h 约定）
            DWORD now = GetTickCount();
            if (now - g_lastPaint >= (DWORD)PAINT_INTERVAL_MS) {
                g_lastPaint = now;
                InvalidateRect(g_hWnd, nullptr, FALSE);
            }
        }
    }
};

static DebugHudFeature g_feature;

} // namespace

extern "C" __declspec(dllexport) void mod_init(void) { qol::Log("[debughud] mod_init()"); }
extern "C" __declspec(dllexport) void mod_tick(void) { g_feature.SafeTick(); }
extern "C" __declspec(dllexport) void unload(void) {
    qol::Log("[debughud] unload()");
    if (g_hFont) { DeleteObject(g_hFont); g_hFont = nullptr; }
    if (g_hWnd)  { DestroyWindow(g_hWnd); g_hWnd = nullptr; }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hMod = h;
        qol::SetLogName("debughud");
        if (!qol::ExeBuildSupported()) {
            qol::Log("[debughud] 游戏版本不匹配（非 build 25640656）→ 不启用\n");
            return TRUE;
        }
        qol::Log("[debughud] attach");
        g_feature.SafeInit();
    } else if (reason == DLL_PROCESS_DETACH) {
        qol::Log("[debughud] detach");
    }
    return TRUE;
}
