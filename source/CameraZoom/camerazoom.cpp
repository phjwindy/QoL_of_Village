// camerazoom.cpp —— 相机视野 (CameraZoom v1.2.0 for v1.20, build 25311578)
//
// v1.2.0（2026-10-24 睡觉复位根治：hook 三大缩放应用入口）：
//   ★根因定案（Ghidra + 实测日志）：进图/睡觉唤醒时游戏从 exe 文件重映射
//   .rdata，倍率表（RVA 0xE95C00）瞬时回原版；同帧的一次性 apply（滚轮循环
//   FUN_140185A00 / SetZoomMode FUN_1401865A0 / 脚本命令 MapSetCameraZoomMode
//   FUN_14048A590——ghidra 见 reverse_camerazoom*.log）读到刚回原版的表 →
//   相机拿原版值 → 视角复位。v1.1.0 的 1s diag 全程 cur==expect 佐证表在
//   采样粒度下从未偏离（瞬时窗口采样不到）；v1.0.9 时代"表被改写立即重应用"
//   日志佐证重映射确实发生。轮询永远赢不了同帧竞态 →
//   ★修法：hook 三个应用入口，入口处先重断言倍率表（force 写当前档位），
//   再调原函数——把"读表"时刻的表值锁死为我们的档位，竞态窗口归零。
//   mode=0（原版档）时断言为幂等空操作，行为与原版完全一致。
//
// v1.1.0（2026-10-23 修睡觉/过渡期缩放丢失 + 诊断）：
//   ① 持续保持重应用【移到 QolGameBusy 之前】——睡觉/进出房间/剧情过渡期游戏
//      可能处于 busy 态，旧逻辑 busy 时 return 导致过渡期倍率被重置后无法重应用。
//      重应用=读表+条件写（SEH+写校验），busy 期也安全执行。
//   ② 1s 节流 diag 快照：追踪睡觉过渡全程（mode/busy/cur/expect），定位根因。
//
// v1.0.9（2026-10-22 修复睡觉/进出房间缩放丢失）：
//   ① 持续保持——mode≠0 时每帧检测倍率表，被游戏改回原版（睡觉/进出房间/
//      剧情每帧重置）立即重应用当前档位（force 无条件写），不再延迟 2 秒。
//      旧逻辑延迟 2 秒导致「游戏每帧盖回原版、2x 仅生效一瞬」，表现为
//      「视角回原版但 HUD 仍显示 2x」。
//   ② 重应用失败（加载中表暂不可写）不回退 mode，下一帧继续尝试。
//
// v1.0.8 → v1.0.8（玩家反馈功能增强，2026-10-16）：
//   ① HUD 倍率显示：切到 2 倍时常驻左下角小窗显示"视野 2.0x"，
//      切回原版显示"已恢复原版"2 秒后隐藏——玩家随时知道当前档位
//   ② 出入房间缩放保持：旧版剧情保护把"表被游戏改写"一律重置 mode=0
//      （进出房间游戏重写倍率表 = 缩放丢失）；新版改为延迟 2 秒【重应用】
//      当前档位（等房间加载完成，QolGameBusy 期间顺延）——出入房间
//      缩放自动恢复；剧情强制 FOV 的场景仍会被改回（罕见，玩家可 F7 回原版）
//
// 功能：F7 切换视野——原版 ↔ 视野 2 倍。
// 平时完全不写入；切到 2 倍时游戏画面视野整体等比拉远，
// 滚轮三档手感不变；切回原版时恢复。只改内存不改文件，重启自然还原。
//
// 定位方式：在 exe 镜像内扫描原版三连浮点（1.2/1.35/1.5）作锚点，
// 校验紧随其后的第 4 个浮点在 1.5~3.0 之间（实测 v1.20 = 2.0），
// 命中即原地改写前三个 float。重启游戏自然恢复原值（只改内存不改文件）。
//
// 热键：默认 F7，走 QolHotKeys 框架支持改键（QoL_hot.json / ModManager 面板）。

#include <windows.h>
#include "obs.h"              // v1.6.x: 统一观测层（AI 可读寄存器 + toast）
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <atomic>
#include <cwchar>

#include "logging.h"
#include "exe_guard.h"   // v-local: 统一游戏版本闸门
#include "hook.h"        // v1.2.0: 三大缩放应用入口 trampoline hook
#include "budget.h"        // P0 帧耗时探针（跨 DLL 共享）
#include "state.h"         // P1-2 状态感知暂停（载入/菜单静默）
#include "game_window.h"   // v1.0.8: HUD owner 绑定游戏窗口 + 失焦守卫
#include "hot_config.h"
#include "hotkey.h"
#include "quickmenu.h"     // F2 统一菜单接入（P0）

// CameraZoom 日志开关：v1.1.0 睡觉复位诊断临时开启（定位后回退注释）
// 日志运行时开关（2026-10-24）：编译期宏 CAMERAZOOM_LOGGING 退役，改由 QoL_Shared 运行时门控。
// 开启方式：游戏根 qol_log.cfg 写 level=1（约 1s 生效，免重编免重启）；缺省/level=0 为静默。

// ---- 原版三档倍率（扫描锚点）+ 第 4 个值的验证范围 ----
static const float ORIG_ZOOM[3] = { 1.2f, 1.35f, 1.5f };
static constexpr float ZOOM4_MIN = 1.5f;
static constexpr float ZOOM4_MAX = 3.0f;
// v1.20 exe 镜像扫描范围（base .. base+span）
static constexpr uintptr_t ZOOM_TABLE_SPAN = 0x2000000u;  // 32MB，覆盖 .data/.rdata

// ---- 视野档位（F7 循环）----
// 0 = 原版；1 = 视野2倍
static constexpr int ZOOM_MODE_COUNT = 2;
static const char* ZOOM_MODE_NAMES[ZOOM_MODE_COUNT] = {
    "原版", "视野2倍"
};

// ---- 运行时状态 ----
static uintptr_t g_exeBase = 0;
static uintptr_t g_exeImageEnd = 0;
static uintptr_t g_tableRva = 0;          // 找到的倍率表 RVA（0=未找到）
static std::atomic<int> g_zoomMode{0};    // 当前档位（默认 0=原版不写）
static volatile LONG g_hotkeyPrev = 0;   // 热键边沿检测
static QolHotKeys g_hotkeys;
static volatile LONG g_running = 0;
// v1.1.0: 线程句柄——unload 时投递 WM_QUIT + join，避免 HUD WndProc 悬空引用已卸载 DLL
static HANDLE g_hHudThread = nullptr;
static ULONGLONG g_lastToggleTime = 0;    // HUD 显示时间戳

// 热调参：视野倍率（存10倍值，20=2倍）
static volatile int ZOOM_ENABLED = 1;
static volatile int VIEW_MULT_X10 = 20;

// ============================================================
// v1.2.0: 三大缩放应用入口 hook —— 入口先重断言倍率表再放行
// 原函数（v1.20 build 25311578，Ghidra 反查 reverse_camerazoom*.log）：
//   FUN_140185A00 滚轮循环切档（mode=(mode+1)%3 → apply table[mode]）
//   FUN_1401865A0 SetZoomMode(cam, idx, anim)（睡觉唤醒序列 case2 调用）
//   FUN_14048A590 脚本命令 MapSetCameraZoomMode（进图/换图应用）
// ============================================================
typedef void (*TZoomCycle)(void*, void*, void*, void*);
typedef void (*TSetZoomMode)(void*, int, void*);
typedef unsigned (*TMapSetZoom)(void*);
static TZoomCycle  g_orig_ZoomCycle  = nullptr;
static TSetZoomMode g_orig_SetZoomMode = nullptr;
static TMapSetZoom g_orig_MapSetZoom = nullptr;
static volatile LONG g_hookAssertBusy = 0;

// 前向声明（定义在本文件后段）
static void ModeTarget(int mode, float out[3]);
static bool WritePatchChecked(void* dst, const void* src, size_t len);

// 强制把当前档位写回倍率表（静默版：不刷切换日志；写失败无副作用）
static void AssertZoomTableSilent(void) {
    int mode = g_zoomMode.load();
    if (!g_tableRva || !g_exeBase || mode == 0) return;
    if (InterlockedExchange(&g_hookAssertBusy, 1) != 0) return;  // 重入防护
    float* table = reinterpret_cast<float*>(g_exeBase + g_tableRva);
    float target[3];
    ModeTarget(mode, target);
    __try {
        if (memcmp(table, target, sizeof(target)) != 0)
            WritePatchChecked(table, target, sizeof(target));
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    InterlockedExchange(&g_hookAssertBusy, 0);
}

static void Detour_ZoomCycle(void* a, void* b, void* c, void* d) {
    AssertZoomTableSilent();
    g_orig_ZoomCycle(a, b, c, d);
}
static void Detour_SetZoomMode(void* cam, int idx, void* anim) {
    AssertZoomTableSilent();
    g_orig_SetZoomMode(cam, idx, anim);
}
static unsigned Detour_MapSetZoom(void* vm) {
    AssertZoomTableSilent();
    return g_orig_MapSetZoom(vm);
}

// 安装三入口 hook（版本闸门后调用；失败仅降级为 v1.1.0 轮询行为）
static void InstallZoomApplyHooks(void) {
    struct Entry { const char* name; uintptr_t rva; uintptr_t detour; uintptr_t* orig; };
    const Entry entries[] = {
        { "ZoomCycle",  0x188A90, (uintptr_t)&Detour_ZoomCycle,  (uintptr_t*)&g_orig_ZoomCycle },   // 20261028: 0x185A00 -> 0x188A90 (build 25640656, 0/96 通配差异)
        { "SetZoomMode", 0x189630, (uintptr_t)&Detour_SetZoomMode, (uintptr_t*)&g_orig_SetZoomMode }, // 0x1865A0 -> 0x189630 (2/96)
        { "MapSetZoom", 0x4A0AB0, (uintptr_t)&Detour_MapSetZoom,  (uintptr_t*)&g_orig_MapSetZoom },   // 0x48A590 -> 0x4A0AB0 (0/240)
    };
    int ok = 0;
    for (const Entry& e : entries) {
        uintptr_t target = g_exeBase + e.rva;
        if (qol::HookInstall(target, e.detour, e.orig)) {
            ++ok;
            Log("[camerazoom] hook %s OK @ RVA %08X", e.name,
                static_cast<unsigned>(e.rva));
        } else {
            Log("[camerazoom] hook %s FAILED @ RVA %08X（降级为轮询保持）",
                e.name, static_cast<unsigned>(e.rva));
        }
    }
    Log("[camerazoom] apply hooks installed %d/3", ok);
}

// ============================================================
// 写入验证（VirtualProtect + 读回）
// ============================================================
static bool WritePatchChecked(void* dst, const void* src, size_t len) {
    DWORD oldProt = 0;
    if (!VirtualProtect(dst, len, PAGE_READWRITE, &oldProt)) return false;
    memcpy(dst, src, len);
    // 读回验证
    uint32_t rb = 0, exp = 0;
    memcpy(&rb, dst, 4);
    memcpy(&exp, src, 4);
    if (rb != exp) return false;
    DWORD tmpProt = 0;
    return VirtualProtect(dst, len, oldProt, &tmpProt) != 0;
}

// ============================================================
// 扫描 exe 镜像内三连浮点锚点
// ============================================================
static uintptr_t FindZoomTable() {
    if (!g_exeBase) return 0;
    const unsigned char* addr = reinterpret_cast<const unsigned char*>(g_exeBase);
    const unsigned char* end = addr + ZOOM_TABLE_SPAN;
    unsigned char pat[12];
    memcpy(pat, ORIG_ZOOM, sizeof(pat));

    MEMORY_BASIC_INFORMATION mbi;
    while (reinterpret_cast<uintptr_t>(addr) < reinterpret_cast<uintptr_t>(end)) {
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_COMMIT && mbi.RegionSize >= 16 &&
            (mbi.Protect & PAGE_GUARD) == 0 &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY))) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(mbi.BaseAddress);
            const unsigned char* pend = p + mbi.RegionSize;
            unsigned char buf[0x4000];
            while (p + 16 <= pend) {
                size_t chunk = static_cast<size_t>(pend - p);
                if (chunk > sizeof(buf)) chunk = sizeof(buf);
                __try {
                    memcpy(buf, p, chunk);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    break;
                }
                for (size_t i = 0; i + 16 <= chunk; i += 4) {  // float 对齐步进
                    if (memcmp(buf + i, pat, 12) == 0) {
                        float v4;
                        memcpy(&v4, buf + i + 12, 4);
                        if (v4 >= ZOOM4_MIN && v4 <= ZOOM4_MAX) {
                            uintptr_t rva = reinterpret_cast<uintptr_t>(p + i) - g_exeBase;
                            Log("[camerazoom] table found @ RVA %08X (v4=%.2f)",
                                static_cast<unsigned>(rva), v4);
                            return rva;
                        }
                    }
                }
                p += chunk;
            }
        }
        if (mbi.RegionSize == 0) break;
        addr = reinterpret_cast<const unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return 0;
}

// ---- v1.0.2: 计算指定档位的目标倍率值 ----
static void ModeTarget(int mode, float out[3]) {
    if (mode == 0) {
        memcpy(out, ORIG_ZOOM, sizeof(ORIG_ZOOM));
        return;
    }
    float mult = static_cast<float>(VIEW_MULT_X10) / 10.0f;
    if (mult < 0.1f) mult = 0.1f;  // 防除零/防过小
    for (int i = 0; i < 3; ++i) {
        out[i] = ORIG_ZOOM[i] / mult;
    }
}

// ---- v1.0.2: 判断当前表值是否是“已知状态”（ORIG 或本 MOD 任一档位） ----
static bool IsKnownState(const float cur[3]) {
    for (int m = 0; m < ZOOM_MODE_COUNT; ++m) {
        float t[3];
        ModeTarget(m, t);
        if (memcmp(cur, t, sizeof(ORIG_ZOOM)) == 0) return true;
    }
    return false;
}

// ---- v1.0.2: 安全读取倍率表（noinline + SEH，KB-067） ----
__declspec(noinline)
static bool ReadZoomTableSafe(float out[3]) {
    if (!g_tableRva || !g_exeBase) return false;
    __try {
        memcpy(out, reinterpret_cast<void*>(g_exeBase + g_tableRva),
               sizeof(float) * 3);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[camerazoom] read table FAILED");
        return false;
    }
    return true;
}

// ============================================================
// 写入/恢复倍率表（v1.0.2 重写：档位互切不再卡死）
// v1.0.8: force=true 无条件写入（房间切换重应用专用——表已被游戏改写
// 不再是已知状态，但重写目标值正是目的；玩家仍可靠 F7 回 mode 0）
// ============================================================
static bool ApplyZoomMode(int mode, bool force = false) {
    if (!g_tableRva || !g_exeBase) return false;
    float* table = reinterpret_cast<float*>(g_exeBase + g_tableRva);

    float target[3];
    ModeTarget(mode, target);

    // 读当前值，如果已经是目标值则跳过
    float cur[3];
    if (!ReadZoomTableSafe(cur)) return false;
    if (memcmp(cur, target, sizeof(cur)) == 0) {
        return true;  // 已在目标档位
    }

    // v1.0.2: mode 0（恢复原版）无条件写入——这是玩家的“逃生舱”，
    // 即使表被剧情改写也能按一次 F7 回到 mode 0 恢复原版。
    // 非 0 档位要求当前是“已知状态”（ORIG 或本 MOD 档位），
    // 避免在剧情改写的表上叠加写入（v1.0.8: force 时跳过此检查——
    // 重应用场景就是要覆盖被房间改写的表）。
    if (mode != 0 && !force && !IsKnownState(cur)) {
        Log("[camerazoom] skip: current %.2f/%.2f/%.2f not known (ORIG/zoom levels), may be story-controlled",
            cur[0], cur[1], cur[2]);
        return false;
    }

    if (!WritePatchChecked(table, target, sizeof(target))) {
        Log("[camerazoom] patch FAILED (write verify)");
        return false;
    }
    if (mode == 0) {
        Log("[camerazoom] 视野恢复原版");
    } else {
        Log("[camerazoom] 视野切换到 %s", ZOOM_MODE_NAMES[mode]);
    }
    return true;
}

// ============================================================
// v1.0.9: HUD 倍率指示窗（左中偏下，与 TimeFreeze/AutoPet 垂直错开）
// 布局：AutoPet=左中上 / TimeFreeze=左中(+56) / CameraZoom=左中下方
// 再+一档（toast 高 40+间距 12）——用户要求三者分开、放大。
// ============================================================
static HWND g_hudHwnd = nullptr;
static wchar_t g_hudText[32] = {0};
static ULONGLONG g_hudHideAt = 0;    // 0=常驻；非 0=到达时刻自动隐藏
static const int HUD_W = 220;       // v1.0.9: 170→220 放大
static const int HUD_H = 46;        // v1.0.9: 36→46 放大

static LRESULT CALLBACK HudWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(28, 30, 34));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        RECT bar = rc;
        bar.right = bar.left + 6;
        HBRUSH accent = CreateSolidBrush(RGB(74, 144, 217));  // 蓝色=CameraZoom
        FillRect(hdc, &bar, accent);
        DeleteObject(accent);
        SetBkMode(hdc, TRANSPARENT);
        HFONT font = CreateFontW(26, 0, 0, 0, FW_NORMAL, 0, 0, 0,     // v1.0.9: 20→26 放大
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                 0, L"Microsoft YaHei");
        HFONT old = (HFONT)SelectObject(hdc, font);
        SetTextColor(hdc, RGB(220, 225, 230));
        RECT tr = { 24, 8, rc.right - 10, rc.bottom - 8 };
        DrawTextW(hdc, g_hudText, -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, old);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (g_hudHideAt != 0 && GetTickCount64() >= g_hudHideAt) {
            KillTimer(hwnd, 1);
            g_hudHideAt = 0;
            QolHudMarkHiddenByMod(hwnd);
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI HudThread(LPVOID) {
    WNDCLASSA wc;
    MSG msg;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = HudWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "CameraZoomHud";
    if (!RegisterClassA(&wc)) return 0;
    // v1.0.9: 左中偏下（TimeFreeze toast=中线+56；CameraZoom=中线+56+40+12
    // —与 AutoPet（左中上）/TimeFreeze（左中）三档垂直错开互不重叠）
    // v1.2.1【KB-113】原实现按「屏幕工作区」左中偏下定位，窗口化/4K DPI 下
    //   HUD 会飞到游戏框外。改为锚定游戏客户区，找不到游戏窗口才回退工作区。
    int cx = 0, cy = 0, cw = 0, ch = 0;
    int x = 24, y = 0;
    if (QolGetClientBox(&cx, &cy, &cw, &ch)) {
        x = cx + 24;
        y = cy + (ch - HUD_H) / 2 + 56 + 40 + 12;
        if (y + HUD_H > cy + ch - 8) y = cy + ch - HUD_H - 8;
        if (y < cy + 8) y = cy + 8;
    } else {
        RECT work = {};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        x = work.left + 24;
        y = work.top + (work.bottom - work.top - HUD_H) / 2 + 56 + 40 + 12;
    }
    g_hudHwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        "CameraZoomHud", "CZ", WS_POPUP, x, y, HUD_W, HUD_H,
        QolFindGameWindow(), nullptr, wc.hInstance, nullptr);
    if (!g_hudHwnd) return 0;
    SetLayeredWindowAttributes(g_hudHwnd, 0, 228, LWA_ALPHA);
    HRGN rounded = CreateRoundRectRgn(0, 0, HUD_W + 1, HUD_H + 1, 10, 10);
    if (!SetWindowRgn(g_hudHwnd, rounded, FALSE)) DeleteObject(rounded);
    SetTimer(g_hudHwnd, 1, 500, nullptr);  // 自动隐藏检查节拍
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// 更新 HUD：holdMs=0 常驻；>0 到时自动隐藏
static void UpdateHud(int mode, ULONGLONG holdMs) {
    if (!g_hudHwnd) return;
    if (mode == 0) {
        _snwprintf_s(g_hudText, 32, _TRUNCATE, L"视野已恢复原版");
    } else {
        float mult = static_cast<float>(VIEW_MULT_X10) / 10.0f;
        _snwprintf_s(g_hudText, 32, _TRUNCATE,
                     L"视野 %.1fx", static_cast<double>(mult));
    }
    // v1.2.1【KB-113】窗口在 HudThread 启动时只创建一次；那一刻游戏主窗口
    //   可能尚未就绪而回退到工作区坐标，之后显示不会再重定位 → HUD 永久留在框外。
    //   故每次 UpdateHud 都重新锚定客户区（左中偏下，与 TF/其它 toast 错开）。
    int ax = 24, ay = 0;
    int cx = 0, cy = 0, cw = 0, ch = 0;
    if (QolGetClientBox(&cx, &cy, &cw, &ch)) {
        ax = cx + 24;
        ay = cy + (ch - HUD_H) / 2 + 56 + 40 + 12;
        if (ay + HUD_H > cy + ch - 8) ay = cy + ch - HUD_H - 8;
        if (ay < cy + 8) ay = cy + 8;
    } else {
        RECT work = {};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        ax = work.left + 24;
        ay = work.top + (work.bottom - work.top - HUD_H) / 2 + 56 + 40 + 12;
    }
    SetWindowPos(g_hudHwnd, HWND_TOPMOST, ax, ay, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(g_hudHwnd, nullptr, TRUE);
    ShowWindow(g_hudHwnd, SW_SHOWNOACTIVATE);
    g_hudHideAt = holdMs ? (GetTickCount64() + holdMs) : 0;
    if (holdMs) SetTimer(g_hudHwnd, 1, 500, nullptr);
}

// ============================================================
// mod_tick：热键 + 重试扫描
// ============================================================
static ULONGLONG s_lastScanTry = 0;

extern "C" __declspec(dllexport) void mod_tick(void) {
    qol::budget::BeginFrame();  // P0: 帧锚定（幂等，多 DLL 安全）
    static int s_bdgSlot = -1;  // P0 探针（惰性注册，析构自动上报）
    if (s_bdgSlot < 0) s_bdgSlot = qol::budget::Slot("CameraZoom");
    struct BdgGuard {
        int slot; uint64_t t0;
        ~BdgGuard() { qol::budget::Report(slot, qol::budget::NowUs() - t0); }
    } bdgGuard = { s_bdgSlot, qol::budget::NowUs() };

    // v1.0.8: HUD 失焦守卫（不飘桌面）
    QolHudGuardVisibility(g_hudHwnd);

    if (!ZOOM_ENABLED) return;

    // 热键改键刷新
    QolHotkeyCheckReload(&g_hotkeys);

    // 热键切换（边沿触发）
    {
        WORD vk = QolHotKeysVk(&g_hotkeys, 0);
        bool keyDown = (vk != VK_UNDEFINED)
            ? ((GetAsyncKeyState(vk) & 0x8000) != 0)
            : ((GetAsyncKeyState(VK_F7) & 0x8000) != 0);  // 回退默认 F7
        if (keyDown && !g_hotkeyPrev) {
            int newMode = (g_zoomMode.load() + 1) % ZOOM_MODE_COUNT;
            if (ApplyZoomMode(newMode)) {
                g_zoomMode.store(newMode);
                g_lastToggleTime = GetTickCount64();
                // v1.0.8: HUD 更新（2x 常驻 / 原版显示 2s）
                UpdateHud(newMode, newMode == 0 ? 2000 : 0);
            }
        }
        g_hotkeyPrev = keyDown;
    }

    // v1.1.0: 持续保持重应用【移到 QolGameBusy 之前】——睡觉/进出房间/剧情过渡期
    //   游戏可能处于 busy 态；旧逻辑在 busy 时直接 return，导致过渡期倍率被游戏
    //   重置后无法重应用（表现为"睡觉后视角回原版但 HUD 仍显示 2x"）。重应用=
    //   读表+条件写（SEH + 写回校验），busy 期执行也安全。force=true 无条件写
    //   目标值；mode==0 时本 MOD 完全不写，F7 回原版后保持静默。
    if (g_tableRva && g_zoomMode.load() != 0) {
        float cur[3], expect[3];
        if (ReadZoomTableSafe(cur)) {
            ModeTarget(g_zoomMode.load(), expect);
            bool differs = memcmp(cur, expect, sizeof(cur)) != 0;
            if (differs) {
                int mode = g_zoomMode.load();
                bool ok = ApplyZoomMode(mode, true);
                Log("[camerazoom] 倍率被游戏改写，重应用 mode=%d ok=%d busy=%d "
                    "cur=%.3f/%.3f/%.3f", mode, (int)ok, (int)QolGameBusy(),
                    cur[0], cur[1], cur[2]);
            }
            // v1.1.0-diag: 1s 节流状态快照——睡觉过渡全程追踪，定位"压不住"根因
            //   （重应用没跑? busy 挡住? 写了但下一帧又被盖? 一看便知）
            {
                static ULONGLONG s_lastDiag = 0;
                ULONGLONG now = GetTickCount64();
                if (now - s_lastDiag >= 1000) {
                    s_lastDiag = now;
                    Log("[camerazoom] diag mode=%d busy=%d cur=%.3f/%.3f/%.3f "
                        "expect=%.3f/%.3f/%.3f", g_zoomMode.load(),
                        (int)QolGameBusy(), cur[0], cur[1], cur[2],
                        expect[0], expect[1], expect[2]);
                }
            }
        }
    }

    if (QolGameBusy()) return;  // P1-2: 载入/菜单期间跳过表扫描

    // 首次/重试扫描表位置（5s 节流）
    if (!g_tableRva) {
        ULONGLONG now = GetTickCount64();
        if (now - s_lastScanTry >= 5000) {
            s_lastScanTry = now;
                g_tableRva = FindZoomTable();
            if (g_tableRva) {
                Log("[camerazoom] zoom table locked @ RVA %08X", static_cast<unsigned>(g_tableRva));
                // 如果当前档位不是 0（重启恢复场景），重新应用
                int mode = g_zoomMode.load();
                if (mode != 0) {
                    if (ApplyZoomMode(mode, true)) UpdateHud(mode, 0);
                }
            }
        }
    }
}

// ============================================================
// QoL 快捷菜单导出（宿主：ModManager F2 / F1）
//   F7「相机缩放」开关，复用 ApplyZoomMode()（与热键同义）
// ============================================================
static bool IsCameraZoomOn() {
    return g_zoomMode.load() != 0;
}
static void ToggleCameraZoom() {
    int newMode = (g_zoomMode.load() + 1) % ZOOM_MODE_COUNT;
    if (ApplyZoomMode(newMode)) {
        g_zoomMode.store(newMode);
        g_lastToggleTime = GetTickCount64();
        UpdateHud(newMode, newMode == 0 ? 2000 : 0);
    }
}
extern "C" __declspec(dllexport) int QolQuickMenuItems(
        QolQuickMenuItem* items, int maxItems) {
    int n = 1;  // 相机2倍缩放
    if (items && maxItems >= 1)
        items[0] = { "相机2倍缩放", IsCameraZoomOn, ToggleCameraZoom };
    return n;
}

// ============================================================
// mod_init
// ============================================================
extern "C" __declspec(dllexport) int mod_init(void) {
    LogOpen("camerazoom");
    qol::obs::Init(nullptr);  // v1.6.x: 统一观测层接入
    qol::obs::SetInt("camerazoom", "ready", 1);
    qol::obs::EventF(1, 2500, L"[CameraZoom] 已加载：观测层接入");
    // v-local：游戏版本闸门（village.exe 非 v1.20 build 25311578 则不启用）
    if (!qol::ExeBuildSupported()) {
        Log("[CameraZoom] 游戏版本不匹配（非 v1.20 build 25311578）→ 本 MOD 不启用\n");
        return 0;
    }

    g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_exeBase) {
        Log("[camerazoom] GetModuleHandle failed");
        return 0;
    }
    // exe 映像范围
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exeBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { Log("[camerazoom] DOS header mismatch"); return 0; }
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_exeBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { Log("[camerazoom] NT header mismatch"); return 0; }
    g_exeImageEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;

    Log("[camerazoom] mod_init begin; base=%p imageEnd=%p",
        reinterpret_cast<void*>(g_exeBase), reinterpret_cast<void*>(g_exeImageEnd));

    // HotConfig 注册
    HotConfig_Register("camerazoom", (void*)&ZOOM_ENABLED,   "ZOOM_ENABLED",   HOT_INT, 1);
    HotConfig_Register("camerazoom", (void*)&VIEW_MULT_X10,   "VIEW_MULT_X10",   HOT_INT, 20);
    HotConfig_Poll();
    HotConfig_DumpCE("camerazoom");

    // 热键注册
    QolHotKeysInit(&g_hotkeys, "camerazoom");
    QolHotKeysSetDefault(&g_hotkeys, "F7");
    QolRegisterHotKey("camerazoom", "F7");

    // v1.2.0: 三大缩放应用入口 hook（根治睡觉/进图复位——入口断言表值）
    InstallZoomApplyHooks();

    // 首次扫描（mod_init 在游戏主线程，此时 exe 已加载但游戏数据可能未就绪，失败则 tick 重试）
    g_tableRva = FindZoomTable();
    if (g_tableRva) {
        Log("[camerazoom] table found immediately @ RVA %08X", static_cast<unsigned>(g_tableRva));
    } else {
        Log("[camerazoom] table not found yet, will retry in mod_tick");
    }

    g_running = 1;
    g_hHudThread = CreateThread(nullptr, 0, HudThread, nullptr, 0, nullptr);  // v1.0.8: HUD 倍率指示
    Log("[camerazoom] mod_init done (ready=1 hook=3 xinput=0 hid=0 pad=0)");
    return 1;
}

// ============================================================
// unload
// ============================================================
extern "C" __declspec(dllexport) void unload(void) {
    g_running = 0;
    // v1.2.0: 摘除三入口 hook
    qol::HookRemove(g_exeBase + 0x188A90);   // 20261028: 0x185A00 -> 0x188A90 (build 25640656)
    qol::HookRemove(g_exeBase + 0x189630);   // 20261028: 0x1865A0 -> 0x189630
    qol::HookRemove(g_exeBase + 0x4A0AB0);   // 20261028: 0x48A590 -> 0x4A0AB0
    // 恢复原版倍率
    if (g_tableRva && g_zoomMode.load() != 0) {
        ApplyZoomMode(0);
    }
    // v1.1.0: 卸载清理——销毁 HUD 窗口 + join HUD 线程，避免 WndProc 悬空引用已卸载 DLL
    if (g_hudHwnd) {
        QolHudMarkHiddenByMod(g_hudHwnd);
        DestroyWindow(g_hudHwnd);   // 触发 WM_DESTROY，窗口销毁（线程仍在消息循环）
        g_hudHwnd = nullptr;
    }
    if (g_hHudThread) {
        // 消息循环仅在 WM_QUIT 时返回（HudWndProc 不 PostQuitMessage），主动投递确保退出
        PostThreadMessageW(GetThreadId(g_hHudThread), WM_QUIT, 0, 0);
        WaitForSingleObject(g_hHudThread, 2000);
        CloseHandle(g_hHudThread);
        g_hHudThread = nullptr;
    }
    Log("[camerazoom] unloaded (mode was %d)", g_zoomMode.load());
    LogClose();
}

// ============================================================
// DllMain
// ============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        SetProcessDPIAware();  // 高 DPI 下 HUD 坐标对齐（避免模糊/偏移）
    }
    else if (reason == DLL_PROCESS_DETACH) {
        qol::obs::Shutdown();  // v1.6.x: 解观测层映射（仅本 DLL 视图）
    }
    return TRUE;
}
