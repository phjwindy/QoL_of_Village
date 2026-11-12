// obs.h —— 统一 HUD 观测层（toast 是其事件半边）
//
// 设计：见 规范文件/HUD优化方案.md、P2_toast设计.md。
// 目标：让**人和 AI 都能用最低成本观察每个 MOD 的实时状态**——
//   人看屏（toast 浮层 + 可选状态面板），AI 读结构化寄存器（命名文件映射）。
//
// 关键设计点（务必理解，避免重造轮子 / 踩坑）：
//   1. QoL_Shared 是**每个 DLL 各自编译一份**（无共享运行时），跨 DLL static 不共享。
//      所以真理必须放在**进程级共享体**里 —— 这里用命名文件映射 "QolObsShm"。
//   2. 外部读取（state_probe.py / 服务台 server.py 是独立进程）用
//      OpenFileMappingW("QolObsShm") + MapViewOfFile 按**名字**读，
//      **不依赖任何游戏内地址、不受版本地址漂移影响**。
//   3. 窗口类注册用 self-resolved HMODULE（GetModuleHandleExW(FROM_ADDRESS,&Init)），
//      因此本 header 编译进任意 MOD 都能正确归属窗口类到自己的 DLL。
//   4. 首个 Init 的 DLL 创建映射 + 浮层窗口；任何 DLL 发 toast 时若发现
//      ownerWnd 无效，由 obsEnsureWindow() 在锁内兜底认领重建。
//   5. owner 窗口 HWND 写进共享内存，所有 DLL 都能 PostMessage 它触发重绘。
//   6. 【KB-112 可见性铁律，2026-11-09 修复】本机游戏合成环境下只有
//      LWA_ALPHA 分层窗口可见（2026-11-01 同机对照实验：Sower / PA F5 用
//      LWA_ALPHA 全部可见，走 ULW 的 AutoFish / EyeFix / TimeFreeze 窗口
//      建成了却看不见）。共享 toast 必须用 LWA_ALPHA(228) + 深色不透明底
//      + 离屏 BitBlt 一次提交，**禁用 LWA_COLORKEY / UpdateLayeredWindow**。
//      旧版误用 color-key，是 5 个 MOD HUD「窗口/事件全绿但肉眼看不到」的真因。
//
// 作者：PHJ&消失的清风
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include "game_window.h"   // KB-113: toast 必须锚游戏客户区，禁用全屏坐标

// _countof 兜底：部分 MOD 的编译单元未间接引入 <stdlib.h>（_countof 定义处），
// 这里自行定义，保证 obs.h 自包含、不依赖调用方的 include 顺序。
#ifndef _countof
#define _countof(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

namespace qol { namespace obs {

// ---------------- 共享内存布局（C 兼容，Python 端按此对齐解析） ----------------
static const UINT32 OBS_MAGIC     = 0x514F4C31;  // 'QOL1'
static const UINT32 OBS_VER       = 1;
static const int    OBS_MODS      = 32;
static const int    OBS_KEYS      = 8;
static const int    OBS_KLEN      = 20;
static const int    OBS_SLEN      = 48;
static const int    OBS_EVTS      = 64;
static const int    OBS_ETXT      = 96;

struct ObsMod {
    char          name[16];
    int           nInt;
    char          ikey[OBS_KEYS][OBS_KLEN];
    int           ival[OBS_KEYS];
    int           nStr;
    char          skey[OBS_KEYS][OBS_KLEN];
    wchar_t       sval[OBS_KEYS][OBS_SLEN];
};
struct ObsEvent {
    wchar_t       text[OBS_ETXT];
    int           severity;   // 0信息 1成功 2警告 3错误
    DWORD         t;          // GetTickCount()
    int           used;       // 1 = 已写入
};
struct QolObsShm {
    UINT32        magic;
    UINT32        ver;
    LONG          tail;       // Interlocked 事件追加游标
    HWND          ownerWnd;   // 进程级窗口句柄（所有 DLL 可 PostMessage）
    ObsMod        mods[OBS_MODS];
    ObsEvent      events[OBS_EVTS];
};

// ---------------- 每 DLL 自己的句柄（不跨 DLL 共享） ----------------
static HANDLE   g_obsMap   = nullptr;
static QolObsShm* g_obs    = nullptr;
static bool     g_obsOwner = false;
static HWND     g_obsWnd   = nullptr;
static HFONT    g_obsFont  = nullptr;
static UINT     g_obsTimer = 0;
static const wchar_t* OBS_MAP_NAME = L"QolObsShm";
static const wchar_t* OBS_MUTEX    = L"QolObsInit";
static const wchar_t* OBS_CLS      = L"QolObsToastCls";

// 2026-11-07 23:25：以下三个函数互相引用，先给前向声明。
inline LRESULT CALLBACK ObsWndProc(HWND, UINT, WPARAM, LPARAM);
inline HWND obsEnsureWindow();
inline void obsPaintWindow(HWND, HDC);
inline void obsRedraw(HWND);
// 自动隐藏：不依赖窗口线程的消息循环（历史失败根因之一）。
static volatile LONG g_obsHideAt     = 0;   // GetTickCount 过期时刻，0=无待隐藏
static volatile LONG g_obsHideThread = 0;   // 1=已有隐藏线程在跑

// ---------------- 内部：找/建 MOD 槽 ----------------
inline ObsMod* obsFindMod(const char* mod) {
    if (!g_obs) return nullptr;
    for (int i = 0; i < OBS_MODS; ++i) {
        if (g_obs->mods[i].name[0] && _stricmp(g_obs->mods[i].name, mod) == 0)
            return &g_obs->mods[i];
    }
    for (int i = 0; i < OBS_MODS; ++i) {
        if (g_obs->mods[i].name[0] == 0) {
            memset(&g_obs->mods[i], 0, sizeof(ObsMod));
            strncpy_s(g_obs->mods[i].name, sizeof(g_obs->mods[i].name), mod, _TRUNCATE);
            return &g_obs->mods[i];
        }
    }
    return nullptr;  // 槽满（极少见）
}

// ---------------- 当前状态（AI 可读） ----------------
inline void SetInt(const char* mod, const char* key, int v) {
    ObsMod* m = obsFindMod(mod); if (!m) return;
    for (int i = 0; i < m->nInt; ++i)
        if (_stricmp(m->ikey[i], key) == 0) { m->ival[i] = v; return; }
    if (m->nInt < OBS_KEYS) {
        strncpy_s(m->ikey[m->nInt], sizeof(m->ikey[m->nInt]), key, _TRUNCATE);
        m->ival[m->nInt] = v; m->nInt++;
    }
}
inline void SetStr(const char* mod, const char* key, const wchar_t* v) {
    ObsMod* m = obsFindMod(mod); if (!m) return;
    for (int i = 0; i < m->nStr; ++i)
        if (_stricmp(m->skey[i], key) == 0) { wcsncpy_s(m->sval[i], v, _TRUNCATE); return; }
    if (m->nStr < OBS_KEYS) {
        strncpy_s(m->skey[m->nStr], sizeof(m->skey[m->nStr]), key, _TRUNCATE);
        wcsncpy_s(m->sval[m->nStr], v, _TRUNCATE); m->nStr++;
    }
}

// ---------------- 事件 / toast（写 events[] 环形，既画浮层也可机读） ----------------
inline DWORD WINAPI obsHideThread(LPVOID) {
    for (;;) {
        Sleep(100);
        const DWORD now = GetTickCount();
        const DWORD exp = (DWORD)InterlockedCompareExchange(&g_obsHideAt, 0, 0);
        if (!exp || (LONG)(now - exp) < 0) continue;   // 本 DLL 的到期时刻未到

        // 2026-11-09 22:10 修复：g_obsHideAt / g_obsHideThread 是 header 内的
        // static，每个 DLL 各有一份。A DLL 的隐藏线程到期时看不到 B DLL 刚发的
        // 新事件，会把刚显示的窗口立刻藏掉 —— 21:02 实机 visible 在
        // True/False 间跳变、判据最终落在 HIDDEN 就是这个竞态。
        // 隐藏决策改以「共享 events[] 里最新事件时刻」为准：所有 DLL 读同一份
        // 真相，仍有 2500ms 内的事件就顺延，绝不抢在别人的 toast 前面隐藏。
        DWORD latest = 0;
        if (g_obs) {
            for (int i = 0; i < OBS_EVTS; ++i) {
                const ObsEvent* ev = &g_obs->events[i];
                if (ev->used && ev->t && now >= ev->t &&
                    (now - ev->t) < 2500 && ev->t > latest)
                    latest = ev->t;
            }
        }
        if (latest) {
            const DWORD nextExp = latest + 2500;
            if ((LONG)(nextExp - now) > 0) {
                InterlockedExchange(&g_obsHideAt, (LONG)nextExp);
                continue;
            }
        }

        HWND w = g_obs ? g_obs->ownerWnd : nullptr;
        if (w && IsWindow(w)) ShowWindow(w, SW_HIDE);
        InterlockedExchange(&g_obsHideAt, 0);
        InterlockedExchange(&g_obsHideThread, 0);
        return 0;
    }
}
inline void obsScheduleHide() {
    InterlockedExchange(&g_obsHideAt, (LONG)(GetTickCount() + 2500));
    if (InterlockedCompareExchange(&g_obsHideThread, 1, 0) == 0) {
        HANDLE th = CreateThread(nullptr, 0, obsHideThread, nullptr, 0, nullptr);
        if (th) CloseHandle(th);
        else InterlockedExchange(&g_obsHideThread, 0);
    }
}

inline void obsPush(const wchar_t* text, int severity, int durationMs) {
    (void)durationMs;  // 停留时长由 2500ms 事件窗口 + 隐藏线程控制
    if (!g_obs) return;

    // 关键：不再假设「首个 Init 的 DLL 一定建过窗口」。任何 DLL 发 toast 时，
    // ownerWnd 无效就由本 DLL 在锁内认领创建（见 obsEnsureWindow）。
    HWND w = obsEnsureWindow();

    // 无条件重锚：QolAnchorToClient 即使返回 false 也已填好 workarea 坐标，
    // 旧版 `if (QolAnchorToClient(...))` 会让窗口永远停在 (-20000,-20000)。
    if (w && IsWindow(w)) {
        int ax = 0, ay = 0;
        const int tw = 360, th = 4 + 4 * 28;
        QolAnchorToClient(16, 16, tw, th, &ax, &ay);
        SetWindowPos(w, HWND_TOPMOST, ax, ay, tw, th, SWP_NOACTIVATE);
    }

    LONG idx = InterlockedIncrement(&g_obs->tail) - 1;
    ObsEvent* e = &g_obs->events[idx % OBS_EVTS];
    wcsncpy_s(e->text, text, _TRUNCATE);
    e->severity = severity;
    e->t = GetTickCount();
    e->used = 1;

    if (w && IsWindow(w)) {
        // 兜底纠正：窗口可能由未重编的旧 DLL 首建（仍是 LWA_COLORKEY），
        // 就地改成 LWA_ALPHA，保证任何 DLL 发 toast 都能把显示模式拉回可见路径。
        COLORREF ck = 0; BYTE alpha = 0; DWORD flags = 0;
        if (!GetLayeredWindowAttributes(w, &ck, &alpha, &flags) ||
            !(flags & LWA_ALPHA)) {
            SetLayeredWindowAttributes(w, 0, 228, LWA_ALPHA);
        }
        ShowWindow(w, SW_SHOWNA);
        obsRedraw(w);                         // 离屏渲染 + BitBlt 一次提交
        PostMessageW(w, WM_USER + 1, 0, 0);   // 消息循环正常时让 WndProc 再刷/隐藏
        obsScheduleHide();                    // 消息循环不正常时的隐藏兜底
    }
}
inline void Event(const wchar_t* text, int severity = 0, int durationMs = 2000) {
    obsPush(text, severity, durationMs);
}
inline void __cdecl EventF(int severity, int durationMs, const wchar_t* fmt, ...) {
    wchar_t buf[OBS_ETXT];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    obsPush(buf, severity, durationMs);
}

// ---------------- 浮层窗口绘制（LWA_ALPHA 分层，KB-112 已验证范式） ----------------
// 2026-11-09 修复：旧注释错称「与 Sower/EyeFix/TimeFreeze 范式一致」，实际那些
// MOD 用的是 LWA_ALPHA；这里曾用 LWA_COLORKEY(品红抠透明) → 本机合成环境下窗口
// 建成、events 写满、句柄与位置全绿，但玩家肉眼完全看不到。现改为
// LWA_ALPHA(228) + 深色不透明底 + 离屏 memDC 渲染后 BitBlt 一次提交，
// 与 CameraZoom / TimeFreeze::TfRedraw / Sower 完全同一条路。
inline const wchar_t* obsSevColor(int sev) {
    switch (sev) {
        case 1: return L"#82DC96";  // 成功 绿
        case 2: return L"#E6B450";  // 警告 橙
        case 3: return L"#EB6E6E";  // 错误 红
        default: return L"#DCDCE1"; // 信息 浅灰白
    }
}
inline COLORREF obsParseColor(const wchar_t* hex) {
    // 期望 #RRGGBB
    if (hex && hex[0] == L'#' && wcslen(hex) >= 7) {
        int r = 0, g = 0, b = 0;
        swscanf_s(hex, L"#%2x%2x%2x", &r, &g, &b);
        return RGB((BYTE)r, (BYTE)g, (BYTE)b);
    }
    return RGB(220, 220, 225);
}

// 绘制体抽成公共函数：WM_PAINT 与 obsPush 的同步绘制共用同一份逻辑。
inline void obsPaintWindow(HWND w, HDC dc) {
    if (!w || !dc || !g_obs) return;
    __try {
        RECT rc; GetClientRect(w, &rc);
        // LWA_ALPHA 是整窗 alpha，品红不会变透明 → 必须画深色不透明底。
        HBRUSH bg = CreateSolidBrush(RGB(18, 18, 24));
        FillRect(dc, &rc, bg); DeleteObject(bg);
        if (g_obsFont) SelectObject(dc, g_obsFont);
        SetBkMode(dc, TRANSPARENT);
        DWORD now = GetTickCount();
        int lineH = 26;
        int y = 4;
        int shown = 0;
        for (int i = OBS_EVTS - 1; i >= 0 && shown < 4; --i) {
            ObsEvent* e = &g_obs->events[i];
            if (!e->used || now - e->t >= 2500) continue;
            SetTextColor(dc, obsParseColor(obsSevColor(e->severity)));
            RECT tr = {8, y, rc.right - 8, y + lineH};
            DrawTextW(dc, e->text, -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += lineH + 2;
            shown++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // 绘制异常绝不波及游戏主循环
    }
}

// 与 TimeFreeze::TfRedraw 同构：离屏渲染 → 窗口 DC 一次 BitBlt 提交。
// LWA_ALPHA 分层窗口多步 GDI 直绘易闪/不刷新，一次性提交最稳。
inline void obsRedraw(HWND w) {
    if (!w || !IsWindow(w) || !g_obs) return;
    RECT rc;
    if (!GetClientRect(w, &rc)) return;
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;
    HDC screen = GetDC(nullptr);
    if (!screen) return;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, cw, ch);
    if (mem && bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        obsPaintWindow(w, mem);
        HDC target = GetDC(w);
        if (target) {
            BitBlt(target, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
            ReleaseDC(w, target);
        }
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

inline LRESULT CALLBACK ObsWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { return 0; }
    if (msg == WM_TIMER || msg == WM_USER + 1) {
        // 轮询共享内存：有未过期事件就显示并重绘，否则隐藏
        bool any = false;
        if (g_obs) {
            DWORD now = GetTickCount();
            for (int i = 0; i < OBS_EVTS; ++i) {
                ObsEvent* e = &g_obs->events[i];
                if (e->used && now - e->t < 2500) { any = true; break; }
            }
        }
        if (any) {
            if (!IsWindowVisible(w)) ShowWindow(w, SW_SHOWNA);
            InvalidateRect(w, nullptr, FALSE);
        } else if (IsWindowVisible(w)) {
            ShowWindow(w, SW_HIDE);
        }
        return 0;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps);
        obsPaintWindow(w, dc);
        EndPaint(w, &ps);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

// ---------------- 初始化 / 退出 ----------------
// ---------------- 兜底认领：任何 DLL 都能保证进程内有一个浮层窗口 ----------------
// 旧设计只让「首个 CreateFileMapping 的 DLL」建窗口，且 created 判定读的是
// 未清零的 GetLastError()，一旦判错就全库 ownerWnd 恒空 → toast 只写共享内存、
// 永远不显示（2026-11-07 23:16 实机：仅自建窗口的 TF/PA/CameraZoom 有 HUD）。
inline HWND obsEnsureWindow() {
    if (!g_obs) return nullptr;
    if (g_obs->ownerWnd && IsWindow(g_obs->ownerWnd)) return g_obs->ownerWnd;

    HANDLE mx = CreateMutexW(nullptr, FALSE, L"QolObsWndLock");
    if (mx) WaitForSingleObject(mx, 3000);
    HWND w = nullptr;
    if (g_obs->ownerWnd && IsWindow(g_obs->ownerWnd)) {
        w = g_obs->ownerWnd;
    } else {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT |
                           GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCWSTR)&obsEnsureWindow, &self);
        if (!self) { if (mx) { ReleaseMutex(mx); CloseHandle(mx); } return nullptr; }
        WNDCLASSEXW wc = {};
        bool clsOk = (self && GetClassInfoExW(self, OBS_CLS, &wc) != 0);
        if (!clsOk) {
            memset(&wc, 0, sizeof(wc));
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = ObsWndProc;
            wc.hInstance = self;
            wc.lpszClassName = OBS_CLS;
            wc.hbrBackground = (HBRUSH)CreateSolidBrush(RGB(18, 18, 24)); // LWA_ALPHA 深色底
            clsOk = (RegisterClassExW(&wc) != 0);
            // 同名类可能由别的 DLL 注册过；类已存在照样可以建窗口。
            if (!clsOk && GetLastError() == ERROR_CLASS_ALREADY_EXISTS) clsOk = true;
        }
        if (clsOk) {
            w = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
                OBS_CLS, nullptr, WS_POPUP,
                0, 0, 360, 4 + 4 * 28,
                nullptr, nullptr, self, nullptr);
            if (w) {
                SetLayeredWindowAttributes(w, 0, 228, LWA_ALPHA);
                if (!g_obsFont) {
                    int dpi = GetDpiForSystem();
                    int px = MulDiv(18, dpi, 96);
                    g_obsFont = CreateFontW(px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                            DEFAULT_PITCH, L"Microsoft YaHei UI");
                    if (!g_obsFont) g_obsFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                }
                g_obs->ownerWnd = w;
                g_obsWnd = w;
                g_obsOwner = true;
                SetTimer(w, 1, 250, nullptr);
                ShowWindow(w, SW_HIDE);
            }
        }
    }
    if (mx) { ReleaseMutex(mx); CloseHandle(mx); }
    return w;
}

inline void Init(HMODULE hModule) {
    static bool inited = false;
    if (inited) return;
    inited = true;

    HMODULE self = hModule;
    if (!self) {
        // self-resolve：取包含本函数实例的 DLL 模块
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT |
                           GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCWSTR)&Init, &self);
    }

    HANDLE mx = CreateMutexW(nullptr, FALSE, OBS_MUTEX);
    if (mx) WaitForSingleObject(mx, INFINITE);

    // CreateFileMappingW 只在「对象已存在」时保证设置 ERROR_ALREADY_EXISTS；
    // 新建成功时 last error 未定义。旧代码直接读它判 created，可能全库判成
    // false → 没有任何 DLL 初始化共享内存 / 建浮层窗口。必须先清零。
    SetLastError(0);
    g_obsMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  0, sizeof(QolObsShm), OBS_MAP_NAME);
    bool created = (GetLastError() != ERROR_ALREADY_EXISTS);
    if (g_obsMap) {
        g_obs = (QolObsShm*)MapViewOfFile(g_obsMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(QolObsShm));
        if (g_obs && created) {
            memset(g_obs, 0, sizeof(QolObsShm));
            g_obs->magic = OBS_MAGIC;
            g_obs->ver   = OBS_VER;
            g_obs->tail  = 0;
            g_obs->ownerWnd = nullptr;
        }
    }

    // 仅首个 Init 的 DLL 创建浮层窗口
    if (g_obs && created && g_obs->magic == OBS_MAGIC) {
        g_obsOwner = true;
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ObsWndProc;
        wc.hInstance = self;
        wc.lpszClassName = OBS_CLS;
        wc.hbrBackground = (HBRUSH)CreateSolidBrush(RGB(18, 18, 24)); // LWA_ALPHA 深色底
        if (RegisterClassExW(&wc)) {
            g_obsWnd = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
                OBS_CLS, nullptr, WS_POPUP,
                -20000, -20000, 360, 4 + 4 * 28,  // 初始藏屏幕外，显示时定位
                nullptr, nullptr, self, nullptr);
            if (g_obsWnd) {
                SetLayeredWindowAttributes(g_obsWnd, 0, 228, LWA_ALPHA);
                g_obs->ownerWnd = g_obsWnd;
                int dpi = GetDpiForSystem();
                int px = MulDiv(18, dpi, 96);
                g_obsFont = CreateFontW(px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        DEFAULT_PITCH, L"Microsoft YaHei UI");
                if (!g_obsFont) g_obsFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                // KB-113：锚游戏客户区，不用 SM_CXSCREEN / 固定屏幕坐标。
                // 显示时 obsPush() 会再重锚一次（窗口尺寸变化自愈）。
                int ax = 0, ay = 0;
                if (QolAnchorToClient(16, 16, 360, 4 + 4 * 28, &ax, &ay))
                    SetWindowPos(g_obsWnd, HWND_TOPMOST, ax, ay, 360, 4 + 4 * 28,
                                 SWP_NOACTIVATE);
                ShowWindow(g_obsWnd, SW_HIDE);
                g_obsTimer = SetTimer(g_obsWnd, 1, 250, nullptr);
            }
        }
    }

    if (mx) { ReleaseMutex(mx); CloseHandle(mx); }
}

inline void Shutdown() {
    if (g_obsWnd) {
        if (g_obsTimer) KillTimer(g_obsWnd, g_obsTimer);
        DestroyWindow(g_obsWnd);
        g_obsWnd = nullptr;
    }
    if (g_obsFont) { DeleteObject(g_obsFont); g_obsFont = nullptr; }
    if (g_obs) { UnmapViewOfFile(g_obs); g_obs = nullptr; }
    if (g_obsMap) { CloseHandle(g_obsMap); g_obsMap = nullptr; }
    g_obsOwner = false;
    // 注：映射由首建 DLL 持有；非 owner DLL 的 Shutdown 只解映射视图，不删映射。
}

}}  // namespace qol::obs

// ---------------- 兼容别名（便于渐进迁移既有 Show 调用） ----------------
namespace qol { namespace toast {
    inline void Show(const wchar_t* text, int severity = 0, int durationMs = 2000) {
        qol::obs::Event(text, severity, durationMs);
    }
    inline void __cdecl ShowF(int severity, int durationMs, const wchar_t* fmt, ...) {
        wchar_t buf[96];
        va_list ap; va_start(ap, fmt);
        _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        qol::obs::Event(buf, severity, durationMs);
    }
}}
