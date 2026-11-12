// production_chainhud.inl -- PA 原生调用链实时浮窗（v1.5.102）
//
// 目的：在游戏内把"玩家手动投料/取料"时游戏真正走过的函数调用链实时显示出来，
//   直接回答【原版手动版本走了哪些函数 / 哪条指令被调 / 参数是什么】。
//   这是调试观测工具，不是给玩家看的生产状态（生产状态是 F5 那个 HUD）。
//
// v1.5.102 新增：动画实例 owner 新增追踪 —— 每次采样把各实例的 owner
//   （*(*(inst+0x240))，与 PaDumpAnimTables 同一解法，owner=机器状态对象指针）
//   与上一次采样对比，新出现的 owner 标「NEW」+ 相对秒数。手动投料后若浮窗
//   出现 NEW owner 行，即为原版给该机器挂上动画实例的确凿证据。
//
// 2026-11-07：F6/F8 硬编码热键已按用户指令移除，浮窗默认隐藏；2Hz 采样保留。
//   频谱：只在 F6 开启时，2 个安全原生探针（near/far，入口首条指令恰好 5 字节）
//   才会把命中事件写进本浮窗；manpu 是否创建动画实例由本浮窗纯读动画管理器实例表
//   观测（零 hook、零崩）。manpu(0x27E460) 与 alive(0x1569C0) 的 hook 因 trampoline
//   崩溃已禁用（详见 v2.inl）。
//
// 显示内容（从上到下）：
//   1) 标题 + PA 状态（运行 / 暂停·观测原版）
//   2) 调用链事件（near / far，最新在上，最多 10 条）
//   3) 动画管理器实例表快照（t33b8 / t32c0 的实例数与激活数，确认动画实例是否挂上）
//
// 寄存器安全：所有探针在 production_automation_v2.inl 里一律「先 orig() 再记录」，
//   因此本浮窗的显示逻辑绝不参与原调用链（不会像 v1.5.97/98 那样污染寄存器崩溃）。
//
// 作者：PHJ&消失的清风  2026-10-25

#pragma once
#include "game_window.h"  // QolFindGameWindow / FindSettingsOwner（hud 已 include，幂等）

#define PA_CHAIN_EVT_N 10
#define PA_CHAIN_EVT_W 128
static wchar_t g_paChainEvt[PA_CHAIN_EVT_N][PA_CHAIN_EVT_W];
static int g_paChainEvtColor[PA_CHAIN_EVT_N];
static int g_paChainEvtCount = 0;
static int g_paChainAnimLines = 0;
static wchar_t g_paChainAnim[12][96];
static wchar_t g_paChainTitle[96];

// v1.5.103：按【实例指针】判新（不依赖 owner 是否可读）+ 粘性 NEW 记录
//   v1.5.102 的两个缺陷：①owner 解不出(=0)时新实例被跳过 ②NEW 只显示当前采样、
//   200ms 即消失 → 肉眼必漏。本版改为：槽位里出现未见过的新指针即记 NEW，
//   粘在列表里直到 F6 重开才清，并同步 Log 落盘（可事后精确核对）。
struct PaChainNewRec {
    int tab; int slot; uintptr_t inst; uintptr_t owner; int c260; double t;
};
static uintptr_t g_paChainPrevInst[64];
static int g_paChainPrevInstN = 0;
static bool g_paChainHasPrev = false;
static ULONGLONG g_paChainSampleT0 = 0;
static PaChainNewRec g_paChainNews[8];
static int g_paChainNewsN = 0;

std::atomic<bool> g_paChainActive{false};   // F6 切换：是否显示并采集事件（v2.inl 探针也引用）
static HWND g_paChainWindow = nullptr;
static HFONT g_paChainFont = nullptr;
static HBRUSH g_paChainBg = nullptr;
static bool g_paChainVisible = false;

// v1.5.105 曾提供 F8 标记键，2026-11-07 已按用户指令移除。
//   时间戳，与 [PA][uievt]/[PA][chain] NEW 对时间线，彻底消除「哪个动作触发」的歧义。

// 事件入口（被 v2.inl 的探针函数调用）。仅在 F6 开启时入屏。
void PaChainAdd(int color, const wchar_t* line) {
    if (!g_paChainActive.load(std::memory_order_relaxed)) return;
    if (!line) return;
    // 环形前插：整体下移一格，新事件放 [0]
    for (int i = PA_CHAIN_EVT_N - 1; i > 0; --i) {
        wcsncpy_s(g_paChainEvt[i], PA_CHAIN_EVT_W, g_paChainEvt[i - 1], _TRUNCATE);
        g_paChainEvtColor[i] = g_paChainEvtColor[i - 1];
    }
    wcsncpy_s(g_paChainEvt[0], PA_CHAIN_EVT_W, line, _TRUNCATE);
    g_paChainEvtColor[0] = color;
    if (g_paChainEvtCount < PA_CHAIN_EVT_N) ++g_paChainEvtCount;
}

// ---- 纯读：全局动画管理器实例表（确认动画实例是否挂上，零 hook） ----
// v1.5.103：改按【实例指针】判新 —— 槽位里出现上一采样没见过的指针即算新实例，
//   不再依赖 owner 能否解出（v1.5.102 在 owner=0 时会漏判）。owner 仍按
//   inst+0x240 -> *p1 两段解引用读取并显示（同 PaDumpAnimTables），解不出则显示 0。
//   NEW 记录为粘性列表（直到 F6 重开才清）并同步 Log 落盘，避免肉眼错过。
static void PaChainSampleAnim() {
    const uintptr_t base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    void* root = nullptr;
    void* mgr = nullptr;
    g_paChainAnimLines = 0;
    if (!PaSafeReadPtrRaw(reinterpret_cast<void*>(base + RVA_GAME_ROOT),
                          &root) ||
        !root ||
        !PaSafeReadPtrRaw(reinterpret_cast<char*>(root) + 0x208, &mgr) ||
        !mgr) {
        wcscpy_s(g_paChainAnim[g_paChainAnimLines++],
                 L"[动画管理器] root/mgr 不可读（游戏未就绪）");
        return;
    }
    const bool paused = (g_paAutomationPaused != 0);
    _snwprintf_s(g_paChainAnim[g_paChainAnimLines++], 96, _TRUNCATE,
                 L"PA: %s", paused ? L"暂停·观测原版" : L"运行");
    // 本次采样收集：实例指针集合 + 新增计数
    int newCnt[2] = { 0, 0 };
    uintptr_t curInst[64];
    int curN = 0;
    const double dt = g_paChainSampleT0
        ? static_cast<double>(GetTickCount64() - g_paChainSampleT0) / 1000.0
        : 0.0;

    struct Tab { const char* nm; uintptr_t off; int cnt; };
    const Tab tabs[2] = { {"t33b8", 0x33B8, 10}, {"t32c0", 0x32C0, 30} };
    int totalInst = 0, totalActive = 0;
    // v1.5.106：动画实例 owner → PA 机器 对表（owner 命中 PA 登记表的即 PA 的机器）
    struct PaHit { u64 gid; int c260; int tab; int slot; };
    PaHit hits[PA_MACHREG_N];
    int hitN = 0;
    for (int t = 0; t < 2; ++t) {
        int nonnull = 0, active = 0;
        for (int i = 0; i < tabs[t].cnt; ++i) {
            void* inst = nullptr;
            if (!PaSafeReadPtrRaw(reinterpret_cast<void*>(
                    reinterpret_cast<char*>(mgr) + tabs[t].off +
                    i * sizeof(void*)),
                    &inst) ||
                !inst)
                continue;
            ++nonnull;
            int c260 = -1;
            if (NearbyIsReadable(
                    reinterpret_cast<unsigned char*>(inst) + 0x260,
                    sizeof(int)))
                c260 = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(inst) + 0x260);
            if (c260 > 0) ++active;
            // owner 两段解引用（同 PaDumpAnimTables v1.5.94 写法）
            uintptr_t owner = 0;
            void* p1 = nullptr;
            if (PaSafeReadPtrRaw(
                    reinterpret_cast<unsigned char*>(inst) + 0x240, &p1) &&
                p1) {
                void* ow = nullptr;
                if (PaSafeReadPtrRaw(p1, &ow) && ow)
                    owner = reinterpret_cast<uintptr_t>(ow);
            }
            const uintptr_t ip = reinterpret_cast<uintptr_t>(inst);
            if (curN < 64) curInst[curN++] = ip;
            // v1.5.106：owner 对表 —— 命中 PA 登记机器则记录 gid+c260
            const u64 pgid = PaMachineRegGidByOwner(
                reinterpret_cast<void*>(owner));
            if (pgid && hitN < PA_MACHREG_N) {
                hits[hitN].gid = pgid;
                hits[hitN].c260 = c260;
                hits[hitN].tab = t;
                hits[hitN].slot = i;
                ++hitN;
            }
            bool seen = false;
            for (int j = 0; j < g_paChainPrevInstN; ++j) {
                if (g_paChainPrevInst[j] == ip) { seen = true; break; }
            }
            if (!seen && g_paChainHasPrev) {
                ++newCnt[t];
                const PaChainNewRec rec = { t, i, ip, owner, c260, dt };
                if (g_paChainNewsN < 8) {
                    g_paChainNews[g_paChainNewsN++] = rec;
                } else {  // 满了丢弃最旧
                    for (int k = 1; k < 8; ++k)
                        g_paChainNews[k - 1] = g_paChainNews[k];
                    g_paChainNews[7] = rec;
                }
                Log("[PA][chain] NEW %hs[%d] inst=%p owner=%p c260=%d "
                    "(+%.1fs)\n",
                    tabs[t].nm, i, reinterpret_cast<void*>(ip),
                    reinterpret_cast<void*>(owner), c260, dt);
            }
        }
        totalInst += nonnull;
        totalActive += active;
        _snwprintf_s(g_paChainAnim[g_paChainAnimLines++], 96, _TRUNCATE,
                     L"  %hs  实例 %d/%d   激活 %d   新+%d",
                     tabs[t].nm, nonnull, tabs[t].cnt, active, newCnt[t]);
    }
    _snwprintf_s(g_paChainAnim[g_paChainAnimLines++], 96, _TRUNCATE,
                 L"动画实例合计: %d   激活: %d", totalInst, totalActive);
    // v1.5.104：总数变化落盘（时间线锚点，与 [PA][uievt] 对时刻）
    {
        static int s_lastTotal = -1;
        if (s_lastTotal >= 0 && s_lastTotal != totalInst)
            Log("[PA][chain] total %d -> %d\n", s_lastTotal, totalInst);
        s_lastTotal = totalInst;
    }
    // 粘性 NEW 展示（最多 6 行；记录一直保留到 F6 重开才清，不会一闪而过）
    if (g_paChainNewsN > 0) {
        for (int k = 0; k < g_paChainNewsN && k < 6 &&
                        g_paChainAnimLines < 11; ++k) {
            _snwprintf_s(g_paChainAnim[g_paChainAnimLines++], 96, _TRUNCATE,
                         L"  NEW %hs[%d] i=%p o=%p c=%d +%.1fs",
                         tabs[g_paChainNews[k].tab].nm,
                         g_paChainNews[k].slot,
                         reinterpret_cast<void*>(g_paChainNews[k].inst),
                         reinterpret_cast<void*>(g_paChainNews[k].owner),
                         g_paChainNews[k].c260, g_paChainNews[k].t);
        }
    } else if (g_paChainHasPrev) {
        wcscpy_s(g_paChainAnim[g_paChainAnimLines++], 96,
                 L"  (无新增实例)");
    }
    // v1.5.106：PA 机器的动画实例状态（c260>0 才判活）+ 变化落盘
    if (hitN > 0) {
        for (int k = 0; k < hitN && g_paChainAnimLines < 11; ++k) {
            _snwprintf_s(g_paChainAnim[g_paChainAnimLines++], 96, _TRUNCATE,
                         L"  PA机 gid=%llu %hs[%d] c260=%d%s",
                         static_cast<unsigned long long>(hits[k].gid),
                         tabs[hits[k].tab].nm, hits[k].slot, hits[k].c260,
                         hits[k].c260 > 0 ? L" 活跃" : L" 未激活");
        }
        // 状态变化落盘（每台 PA 机器记最后一次 c260，变了才记）
        static PaHit s_last[PA_MACHREG_N];
        static int s_lastN = 0;
        for (int k = 0; k < hitN; ++k) {
            int idx = -1;
            for (int j = 0; j < s_lastN; ++j) {
                if (s_last[j].gid == hits[k].gid) { idx = j; break; }
            }
            if (idx < 0) {
                if (s_lastN >= PA_MACHREG_N) continue;
                idx = s_lastN++;
                s_last[idx].gid = hits[k].gid;
                s_last[idx].c260 = hits[k].c260;
                s_last[idx].tab = hits[k].tab;
                s_last[idx].slot = hits[k].slot;
            } else if (s_last[idx].c260 == hits[k].c260) {
                continue;   // 未变化
            } else {
                s_last[idx].c260 = hits[k].c260;
                s_last[idx].tab = hits[k].tab;
                s_last[idx].slot = hits[k].slot;
            }
            Log("[PA][reg] machine gid=%llu inst=%hs[%d] c260=%d %s\n",
                static_cast<unsigned long long>(hits[k].gid),
                tabs[hits[k].tab].nm, hits[k].slot, hits[k].c260,
                hits[k].c260 > 0 ? "ALIVE" : "DEAD");
        }
    }
    // 本次指针集合转为上次集合
    g_paChainPrevInstN = 0;
    for (int j = 0; j < curN && j < 64; ++j)
        g_paChainPrevInst[g_paChainPrevInstN++] = curInst[j];
    g_paChainHasPrev = true;
    if (!g_paChainSampleT0) g_paChainSampleT0 = GetTickCount64();
}

static LRESULT CALLBACK PaChainWndProc(HWND hwnd, UINT msg,
                                       WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        // KB-111 (2026-11-01)：LWA 半透明窗口里逐笔 GDI 直绘（FillRect+多次
        //   DrawText）会被合成器逐笔呈现 → 经典闪烁。改为离屏双缓冲、一次
        //   BitBlt 提交；分配失败自动降级为直绘，不影响功能。
        HDC paChainMemDC = nullptr;
        HBITMAP paChainMemBmp = nullptr;
        HGDIOBJ paChainOldBmp = nullptr;
        HDC paChainSplash = dc;
        if (rc.right > 0 && rc.bottom > 0) {
            paChainMemDC = CreateCompatibleDC(dc);
            if (paChainMemDC) {
                paChainMemBmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
                if (paChainMemBmp) {
                    paChainOldBmp = SelectObject(paChainMemDC, paChainMemBmp);
                    dc = paChainMemDC;
                } else {
                    DeleteDC(paChainMemDC);
                    paChainMemDC = nullptr;
                }
            }
        }
        if (!g_paChainBg) g_paChainBg = CreateSolidBrush(RGB(18, 20, 26));
        FillRect(dc, &rc, g_paChainBg);
        RECT accent = rc;
        accent.right = accent.left + PaHudScale(4);
        HBRUSH ab = CreateSolidBrush(RGB(120, 200, 160));
        FillRect(dc, &accent, ab);
        DeleteObject(ab);
        if (g_paChainFont) {
            HFONT old = reinterpret_cast<HFONT>(SelectObject(dc, g_paChainFont));
            SetBkMode(dc, TRANSPARENT);
            int y = PaHudScale(6);
            const int lh = PaHudScale(17);
            SetTextColor(dc, RGB(255, 220, 140));
            RECT tr = rc; tr.left += PaHudScale(14); tr.top += y;
            DrawTextW(dc, g_paChainTitle, -1, &tr, DT_LEFT | DT_TOP | DT_NOPREFIX);
            y += lh + PaHudScale(2);
            // 调用链事件
            SetTextColor(dc, RGB(150, 200, 255));
            RECT h1 = rc; h1.left += PaHudScale(14); h1.top += y;
            DrawTextW(dc, L"— 调用链事件（最新在上）—", -1, &h1,
                      DT_LEFT | DT_TOP | DT_NOPREFIX);
            y += lh;
            for (int k = 0; k < g_paChainEvtCount; ++k) {
                COLORREF c = RGB(205, 230, 210);
                switch (g_paChainEvtColor[k]) {
                case 0: c = RGB(130, 240, 150); break;  // manpu 绿
                case 1: c = RGB(130, 180, 255); break;  // near 蓝
                case 2: c = RGB(210, 150, 255); break;  // far 紫
                case 3: c = RGB(255, 230, 120); break;  // alive 黄
                }
                SetTextColor(dc, c);
                RECT er = rc; er.left += PaHudScale(14); er.top += y;
                DrawTextW(dc, g_paChainEvt[k], -1, &er,
                          DT_LEFT | DT_TOP | DT_NOPREFIX);
                y += lh;
            }
            if (g_paChainEvtCount == 0) {
                SetTextColor(dc, RGB(150, 150, 150));
                RECT er = rc; er.left += PaHudScale(14); er.top += y;
                DrawTextW(dc, L"(无；手动投料一次以触发)", -1, &er,
                          DT_LEFT | DT_TOP | DT_NOPREFIX);
                y += lh;
            }
            // 动画管理器快照
            SetTextColor(dc, RGB(150, 200, 255));
            RECT h2 = rc; h2.left += PaHudScale(14); h2.top += y;
            DrawTextW(dc, L"— 动画管理器快照 —", -1, &h2,
                      DT_LEFT | DT_TOP | DT_NOPREFIX);
            y += lh;
            for (int k = 0; k < g_paChainAnimLines; ++k) {
                SetTextColor(dc, (k == 0) ? RGB(255, 220, 140)
                                          : RGB(205, 230, 210));
                RECT ar = rc; ar.left += PaHudScale(14); ar.top += y;
                DrawTextW(dc, g_paChainAnim[k], -1, &ar,
                          DT_LEFT | DT_TOP | DT_NOPREFIX);
                y += lh;
            }
            SelectObject(dc, old);
        }
        if (paChainMemDC && dc == paChainMemDC)
            BitBlt(paChainSplash, 0, 0, rc.right, rc.bottom, paChainMemDC, 0, 0, SRCCOPY);
        if (paChainMemDC) {
            if (paChainOldBmp) SelectObject(paChainMemDC, paChainOldBmp);
            if (paChainMemBmp) DeleteObject(paChainMemBmp);
            DeleteDC(paChainMemDC);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;  // 点击穿透
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int PaChainWinW() { return PaHudScale(470); }
static int PaChainWinH() {
    // 标题 + 事件头 + 10事件(+空行) + 动画头 + 最多11动画行（含 NEW 行）
    return PaHudScale(6 + 17 * 26 + 8);
}

static bool PaChainInitWindow() {
    if (g_paChainWindow) return true;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = PaChainWndProc;
    wc.hInstance = g_module;
    wc.lpszClassName = L"VillageQoLPAChain";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("[PA][chain] RegisterClass failed err=%lu\n", GetLastError());
        return false;
    }
    g_paChainWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        L"VillageQoLPAChain", L"", WS_POPUP, 0, 0, PaChainWinW(),
        PaChainWinH(), nullptr, nullptr, g_module, nullptr);
    if (!g_paChainWindow) {
        Log("[PA][chain] create failed err=%lu\n", GetLastError());
        return false;
    }
    SetLayeredWindowAttributes(g_paChainWindow, RGB(0, 0, 0), 215, LWA_ALPHA);
    g_paChainFont = reinterpret_cast<HFONT>(CreateFontW(
        -PaHudScale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei UI"));
    return true;
}

static void PaChainPosition() {
    if (!g_paChainWindow) return;
    HWND game = QolFindGameWindow();
    if (!game) return;
    RECT gr = {};
    if (!GetWindowRect(game, &gr)) return;
    SetWindowPos(g_paChainWindow, HWND_TOPMOST,
                 gr.right - PaChainWinW() - PaHudScale(12),
                 gr.top + PaHudScale(12),
                 PaChainWinW(), PaChainWinH(),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void PaChainShow() {
    if (!PaChainInitWindow()) return;
    // 清空事件，给一个干净的起点
    g_paChainEvtCount = 0;
    for (int i = 0; i < PA_CHAIN_EVT_N; ++i) g_paChainEvt[i][0] = 0;
    // v1.5.103：重置实例追踪（首个采样不标 NEW，之后对比）
    g_paChainPrevInstN = 0;
    g_paChainHasPrev = false;
    g_paChainNewsN = 0;
    g_paChainSampleT0 = GetTickCount64();
    g_paChainActive.store(true, std::memory_order_relaxed);
    const bool paused = (g_paAutomationPaused != 0);
    _snwprintf_s(g_paChainTitle, 96, _TRUNCATE,
                 L"[PA链路] 调用链实测  %s",
                 paused ? L"PA暂停·观测原版" : L"PA运行");
    PaChainSampleAnim();
    PaChainPosition();
    g_paChainVisible = true;
    ShowWindow(g_paChainWindow, SW_SHOW);
    InvalidateRect(g_paChainWindow, nullptr, TRUE);
    UpdateWindow(g_paChainWindow);
    Log("[PA][chain] shown\n");
}

static void PaChainHide() {
    if (g_paChainWindow && IsWindowVisible(g_paChainWindow))
        ShowWindow(g_paChainWindow, SW_HIDE);
    g_paChainVisible = false;
    g_paChainActive.store(false, std::memory_order_relaxed);
    Log("[PA][chain] hidden\n");
}

static void PaChainInit() {
    // 2026-11-07 用户指令：F6/F8 硬编码热键已移除，浮窗默认不显示。
    // 2Hz 纯读采样仍保留；如需重新显示，改走 QolHotKeys 可配置项。
    g_paChainActive.store(false, std::memory_order_relaxed);
    g_paChainEvtCount = 0;
    Log("[PA][chain] init ready (v1.5.106, hotkeys removed, "
        "always-on 2Hz sampling)\n");
}

// mod_tick 调用：消息泵 + 周期刷新（F6/F8 热键已移除）
static void PaChainPoll() {
    if (g_paChainWindow) {
        MSG msg = {};
        while (PeekMessageW(&msg, g_paChainWindow, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    // 2026-11-07：F6 切换 / F8 打点的硬编码边沿检测已按用户指令移除。
    // v1.5.104：采样与显示解耦 —— 恒以 2Hz 纯读采样并落盘
    //   （NEW 实例 / 总数变化都有日志时间戳），时间线不依赖浮窗是否打开；
    //   绘制仍仅在浮窗可见时进行。
    {
        static ULONGLONG s_last = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - s_last >= 500) {
            s_last = now;
            PaChainSampleAnim();
            if (g_paChainVisible && g_paChainWindow)
                InvalidateRect(g_paChainWindow, nullptr, TRUE);
        }
    }
}

static void PaChainCleanup() {
    if (g_paChainWindow) {
        DestroyWindow(g_paChainWindow);
        g_paChainWindow = nullptr;
    }
    if (g_paChainFont) { DeleteObject(g_paChainFont); g_paChainFont = nullptr; }
    if (g_paChainBg) { DeleteObject(g_paChainBg); g_paChainBg = nullptr; }
    g_paChainVisible = false;
    g_paChainActive.store(false, std::memory_order_relaxed);
}
