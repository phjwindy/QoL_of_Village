// birthdayreminder.cpp —— 生日提醒 (BirthdayReminder v1.0.10 正式版转正 for v1.20：审查通过——LEA时间链正确/三重读保护/无自建窗口/无诊断代码)
// v1.1.2-diag → v1.2.0（定版）：① 移除 HUD 测试文字（HUD 效果用户已实测确认）
//   ② 日志恢复关闭（正式版）。HUD 效果确认项：自适应小框/贴边/防闪/10 分钟消失/送礼隐藏
// v1.1.1-diag → v1.1.2：① 右对齐贴边（内缩 1cm→8px）② 移除礼物行（识别常出错）
// ③ 修复闪动（TimeFreeze 冻结点抖动引发 隐藏↔显示 循环：跳时改为只重算不隐藏 +
// 文字无变化不重绘 + 删 250ms 重绘 timer）④ 显示满 10 分钟（真实时间）自动隐藏。
//
// v1.0.0 → v1.1.0 变更：
//   1. HUD 自适应尺寸：按文字行数/最长行像素宽度动态调整窗口宽高，
//      不再固定 320x120（单行时不再出现大黑框大片空白）
//   2. 字号 26→20px，色条 6→4px，内边距收紧，整体更小巧
//   3. 修正右对齐边距方向（原 +dx 导致窗口右缘出屏，改 -dx 内缩 1cm）
//   4. 移除"生日：测试NPC"测试文字（无生日时隐藏）
//   5. 新增送礼检测：hook NpcSendPresent Lua 绑定函数（RVA 0x4C88E0（v1.30 正确值；旧注释 0x495760 实为 MapObjectSetBodyHitEnabled，物体身体碰撞启用，非送礼函数）），
//      当天送出礼物后隐藏生日提示
//   6. WM_PAINT 字体改为静态句柄复用（原每帧 Create/Delete）
//
// 送礼 hook 说明（RVA 0x4C88E0（v1.30 正确值；旧注释 0x495760 实为 MapObjectSetBodyHitEnabled，物体身体碰撞启用，非送礼函数） NpcSendPresent 绑定函数）：
//   prologue 4 条 mov（20 字节，恰好指令边界）：mov [rsp+8],rbx / +10,rbp /
//   +18,rsi / +20,rdi；之后 push r14。back = +20。
//   函数在 +0x36 处 movsxd rax,[rdx+r9*2] 直接使用 r9/rdx ——
//   stub 必须完整保存 rcx/rdx/r8/r9（OnGiftSent 编译代码会破坏 volatile 寄存器）。
//   栈对齐：入口 rsp≡8 (mod16) → 5 push(40B) ≡0 → sub 0x20 ≡0 → call 前≡0 ✓
//
// 原理：游戏时间 [[exe+clock]+0x208]+0x3270（QWORD 累计秒），
//   1 天 = 86400 秒，4 季 × 28 天 = 112 天/年，day0 = 春 1。
//
// 1.09 适配：
//   clock 锚点 RVA 0x19F003（mov rax,[rip+0xF35946] → 0x10D4950，与 MineHelper 同源）
//   clock 读链：clock → +0x208 → save → +0x3270（raw_second，1.09 已验证）
//
// 作者：PHJ&消失的清风，转载或分享时请注明出处。

#include <windows.h>
#include "obs.h"              // v1.6.x: 统一观测层（AI 可读寄存器 + toast）
#include <cstdint>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cwchar>

#include "logging.h"
#include "exe_guard.h"   // v-local: 统一游戏版本闸门
#include "game_window.h"   // HUD owner 绑定游戏窗口
#include "hot_config.h"
#include "patch_safety.h"
#include "budget.h"        // P0 帧耗时探针（跨 DLL 共享）
#include "state.h"         // P1-2 状态感知暂停（载入/菜单静默）

// 日志开关：正式版关闭（v1.2.0）
// 日志运行时开关（2026-10-24）：编译期宏 BIRTHDAYREMINDER_LOGGING 退役，改由 QoL_Shared 运行时门控。
// 开启方式：游戏根 qol_log.cfg 写 level=1（约 1s 生效，免重编免重启）；缺省/level=0 为静默。

using u64 = std::uint64_t;
using u32 = std::uint32_t;
using i64 = std::int64_t;

// ============================================================
// 常量（v1.09 build 25094764）
// ============================================================
static volatile uintptr_t RVA_CLOCK_LEA    = 0xE2EA3;  // mov rax,[rip+..] -> 0x10D4950
static volatile uintptr_t CLOCK_SAVE_0x208 = 0x208;     // save = *(base + 0x208)
static volatile uintptr_t SAVE_RAW_SECOND  = 0x3270;    // save + 0x3270 = raw_second (i64)

static constexpr i64 SECONDS_PER_DAY = 86400;
static constexpr i64 DAYS_PER_SEASON = 28;
static constexpr int SEASON_COUNT    = 4;
static constexpr i64 SLEEP_DELTA_SEC = 600;   // 时间前进超过 10 游戏分钟视为睡觉/跳时

// 送礼 hook（NpcSendPresent Lua 绑定函数）
static volatile uintptr_t RVA_NPC_SEND_PRESENT = 0x4C88E0;
static constexpr int GIFT_PATCH_LEN    = 12;   // mov rax,imm64(10) + jmp rax(2)
static constexpr int GIFT_PROLOGUE_LEN = 20;   // 4 条 mov（5 字节 each），恰为指令边界

// ============================================================
// HUD 布局常量（v1.1.0 自适应）
// ============================================================
static const int BR_FONT_H = 20;   // 字号（原 26，缩小）
static const int BR_LINE_H = 26;   // 行高
static const int BR_PAD_X  = 10;   // 文字左内边距（色条右侧）
static const int BR_PAD_Y  = 4;    // 上下内边距
static const int BR_BAR_W  = 4;    // 左侧色条宽（原 6）
static const int BR_MIN_W  = 120;  // 最小宽度
static const int BR_ROUND  = 8;    // 圆角

// ============================================================
// 生日数据表（0=春 1=夏 2=秋 3=冬，day = 1..28）
// ============================================================
struct Birthday {
    const wchar_t* name;
    const wchar_t* fav;
    int season;
    int day;
};

static const Birthday BIRTHDAYS[] = {
    { L"帷",     L"铝箔纸烤大蒜、樱桃萝卜、乌龙茶套组、黄宝石", 1, 17 },
    { L"木助",   L"芋头、麦烧酎、地瓜烧酎、牛肉干", 0, 18 },
    { L"四郎治", L"玛格丽特披萨、薯条、豆腐汉堡排、波斯菊", 1,  7 },
    { L"林",     L"大头菜、玫瑰果酱、金矿石、蓝宝石", 0,  4 },
    { L"驹子",   L"焗烤马铃薯、暖呼呼火锅、向日葵、青金石", 2,  1 },
    { L"茶梅",   L"柿子蜜饯、古典巧克力蛋糕、巧克力香蕉、琥珀", 3, 20 },
    { L"六角",   L"梅子茶泡饭、姜、松露、梅酒", 3, 15 },
    { L"今野",   L"豆皮乌龙面、野生小动物的肉、日本酒、金矿石", 1, 11 },
    { L"名护",   L"家乡味定食、酸梅干、葡萄酒、碧玺", 2, 26 },
    { L"洋",     L"儿童咖喱、草莓蛋糕、牛奶寒天冻、郁金香", 2, 12 },
    { L"裕太",   L"炖牛肉、夏日圣品西瓜、菠菜、独角仙", 2,  8 },
    { L"堇怜",   L"烤鱼、盐烤香鱼、香鱼、鲑鱼", 1, 21 },
    { L"莲实",   L"茶香冰淇淋、焙茶套组、玫瑰、紫水晶", 3,  3 },
    { L"琪娜娜", L"自创拉面、木槿、啤酒、粉水晶", 1, 24 },
};
static constexpr int BIRTHDAY_COUNT =
    static_cast<int>(sizeof(BIRTHDAYS) / sizeof(BIRTHDAYS[0]));

// ============================================================
// 运行时状态
// ============================================================
static uintptr_t g_exeBase = 0;
static volatile LONG g_running = 0;
// v1.2.1: 线程句柄——DETACH 时 join，避免 HUD WndProc 悬空引用已卸载 DLL
static HANDLE g_hHud = nullptr;
static HANDLE g_hWorker = nullptr;

// HUD
static HWND g_hudHwnd = nullptr;
static wchar_t g_display[512] = {0};
static wchar_t g_fontPath[MAX_PATH] = {0};
static HFONT g_hudFont = nullptr;
static int g_hudW = BR_MIN_W;                    // 当前 HUD 宽（自适应）
static int g_hudH = BR_PAD_Y * 2 + BR_LINE_H;    // 当前 HUD 高

// 送礼 hook 状态
static uintptr_t g_giftHookAddr = 0;
static uintptr_t g_giftBackAddr = 0;
static void* g_giftStub = nullptr;
static bool g_giftHooked = false;
static unsigned char g_giftOrig[12] = {};
static volatile LONG64 g_giftDay = -1;   // 最后一次送礼的游戏 day

// ============================================================
// 基础工具
// ============================================================
static bool ReadPtr(void* p, void** out) {
    if (!p) return false;
    uintptr_t v = 0;
    __try {
        v = *reinterpret_cast<uintptr_t*>(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (v < 0x10000) return false;
    *out = reinterpret_cast<void*>(v);
    return true;
}

// M15: 内存可读性检查——VirtualQuery 确认地址已提交且可读，
// 在 memcpy 备份/probe 前调用，防止 RVA_NPC_SEND_PRESENT 热调到非法地址导致崩溃。
static bool IsReadable(const void* addr, size_t size) {
    if (!addr || size == 0) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_NOACCESS) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end = start + size;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    if (end > regionEnd) return false;
    return true;
}

// 游戏累计秒（i64）；读不到返回 -1
static i64 ReadGameSeconds() {
    void* base = nullptr;
    const int32_t* disp = reinterpret_cast<const int32_t*>(
        g_exeBase + RVA_CLOCK_LEA + 3);
    uintptr_t addr = g_exeBase + RVA_CLOCK_LEA + 7 + *disp;
    if (!ReadPtr(reinterpret_cast<void*>(addr), &base)) return -1;
    void* save = nullptr;
    if (!ReadPtr(reinterpret_cast<void*>(
            reinterpret_cast<unsigned char*>(base) + CLOCK_SAVE_0x208), &save))
        return -1;
    i64 sec = -1;
    __try {
        sec = *reinterpret_cast<i64*>(
            reinterpret_cast<unsigned char*>(save) + SAVE_RAW_SECOND);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return sec;
}

// ============================================================
// 字体：优先加载游戏同款字体（FR_PRIVATE），失败用系统回退
// ============================================================
static void LoadGameFont() {
    GetModuleFileNameW(nullptr, g_fontPath, MAX_PATH);
    wchar_t* slash = wcsrchr(g_fontPath, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat(g_fontPath, L"ResourceHanRoundedTC-Regular.ttf");
    if (AddFontResourceExW(g_fontPath, FR_PRIVATE, 0) > 0) {
        Log("[br] game font loaded: %ls\n", g_fontPath);
    } else {
        g_fontPath[0] = 0;
        Log("[br] font load failed (fallback font used)\n");
    }
}

static void UnloadGameFont() {
    if (g_fontPath[0])
        RemoveFontResourceExW(g_fontPath, FR_PRIVATE, 0);
}

// ============================================================
// HUD 布局：锚点（右缘内缩 1cm + 时间显示下方）与自适应测量
// ============================================================
static int CmToPx(double cm) {
    HDC hdc = GetDC(nullptr);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(nullptr, hdc);
    return (int)(cm * dpi / 2.54);
}

static HFONT GetHudFont() {
    if (!g_hudFont) {
        g_hudFont = CreateFontW(BR_FONT_H, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                           DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                           0, L"Microsoft YaHei");
    }
    return g_hudFont;
}

// HUD 右上锚点：ax = 右缘 X（内缩 8px 贴边），ay = 顶 Y（时间显示下方）
static void HudAnchor(HWND game, int* ax, int* ay) {
    const int dx = 8;                               // 右边距 8px（靠边）
    int dy = 8 + 13 * 30 + (int)(CmToPx(0.5) * 2);  // 时间显示下方
    RECT wr;
    if (game && GetWindowRect(game, &wr)) {
        *ax = wr.right - dx;
        *ay = wr.top + dy;
    } else {
        int sx = GetSystemMetrics(SM_CXSCREEN);
        *ax = sx - dx;
        *ay = dy;
    }
}

// 测量 g_display 的自适应尺寸
static void MeasureDisplay(int* w, int* h) {
    *w = BR_MIN_W;
    *h = BR_PAD_Y * 2 + BR_LINE_H;
    if (!g_display[0]) return;

    HDC hdc = GetDC(nullptr);
    HFONT font = GetHudFont();
    HFONT old = (HFONT)SelectObject(hdc, font);

    int lines = 1, maxW = 0;
    const wchar_t* p = g_display;
    while (*p) {
        const wchar_t* eol = wcschr(p, L'\n');
        int len = eol ? (int)(eol - p) : (int)wcslen(p);
        if (len > 0 && p[len - 1] == L'\r') len--;
        SIZE sz = {0, 0};
        GetTextExtentPoint32W(hdc, p, len, &sz);
        if (sz.cx > maxW) maxW = sz.cx;
        if (!eol) break;
        lines++;
        p = eol + 1;
    }
    SelectObject(hdc, old);
    ReleaseDC(nullptr, hdc);

    *w = maxW + BR_BAR_W + BR_PAD_X * 2 + 8;   // +8 像素余量
    *h = BR_PAD_Y * 2 + lines * BR_LINE_H;
    if (*w < BR_MIN_W) *w = BR_MIN_W;
}

// 设置文字并自适应窗口（右对齐锚定）。
// v1.1.2 防闪：文字与显示状态均无变化时直接返回，不触碰窗口——
// 杜绝 TimeFreeze 时间抖动导致的 隐藏↔显示 循环闪烁。
static bool g_hudShown = false;
static void SetDisplayText(const wchar_t* text) {
    if (!g_hudHwnd) {
        wcsncpy(g_display, text, 255);
        g_display[255] = 0;
        return;
    }
    // 无变化早退（同文字 + 同显示状态）
    if (g_hudShown && text[0] && wcscmp(g_display, text) == 0) return;
    if (!g_hudShown && !text[0]) return;

    wcsncpy(g_display, text, 255);
    g_display[255] = 0;
    if (!text[0]) {
        g_hudShown = false;
        QolHudMarkHiddenByMod(g_hudHwnd);  // MOD 主动隐藏：清守卫标记，防回前台误还原
        ShowWindow(g_hudHwnd, SW_HIDE);
        return;
    }

    int w, h;
    MeasureDisplay(&w, &h);
    g_hudW = w;
    g_hudH = h;

    HWND game = FindWindowA("Honogurashinoniwa", nullptr);
    int ax, ay;
    HudAnchor(game, &ax, &ay);
    MoveWindow(g_hudHwnd, ax - w, ay, w, h, FALSE);

    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, BR_ROUND, BR_ROUND);
    SetWindowRgn(g_hudHwnd, rgn, FALSE);

    if (!g_hudShown) {
        g_hudShown = true;
        ShowWindow(g_hudHwnd, SW_SHOWNOACTIVATE);
    }
    InvalidateRect(g_hudHwnd, nullptr, FALSE);
}

static LRESULT CALLBACK HudWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        // 深色背景
        HBRUSH bg = CreateSolidBrush(RGB(28, 30, 34));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        // 左侧色条（粉色=生日）
        RECT bar = rc;
        bar.right = bar.left + BR_BAR_W;
        HBRUSH accent = CreateSolidBrush(RGB(220, 120, 180));
        FillRect(hdc, &bar, accent);
        DeleteObject(accent);
        // 文字（静态字体复用）
        HFONT font = GetHudFont();
        HFONT old = (HFONT)SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(220, 225, 230));
        int y = BR_PAD_Y;
        const wchar_t* text = g_display;
        while (*text) {
            const wchar_t* eol = wcschr(text, L'\n');
            int len = eol ? (int)(eol - text) : (int)wcslen(text);
            wchar_t tmp[256];
            if (len > 0 && text[len - 1] == L'\r') len--;
            if (len >= 256) len = 255;
            wcsncpy(tmp, text, len);
            tmp[len] = 0;
            RECT lr = { BR_BAR_W + BR_PAD_X, y, rc.right - 4, y + BR_LINE_H };
            DrawTextW(hdc, tmp, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            y += BR_LINE_H;
            if (!eol) break;
            text = eol + 1;
        }
        SelectObject(hdc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    // v1.1.2 防闪：删除 250ms WM_TIMER 重绘循环，仅内容变化时主动重绘
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI HudThread(LPVOID) {
    WNDCLASSA wc;
    HWND game;
    int ax, ay;
    MSG msg;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = HudWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "BirthdayReminderHud";
    if (!RegisterClassA(&wc)) return 0;
    game = FindWindowA("Honogurashinoniwa", nullptr);
    HudAnchor(game, &ax, &ay);
    // 初始小尺寸隐藏，等 SetDisplayText 自适应后再显示
    g_hudHwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        "BirthdayReminderHud", "BR", WS_POPUP, ax - BR_MIN_W, ay,
        BR_MIN_W, BR_PAD_Y * 2 + BR_LINE_H,
        QolFindGameWindow(), nullptr, wc.hInstance, nullptr);
    if (!g_hudHwnd) return 0;
    SetLayeredWindowAttributes(g_hudHwnd, 0, 228, LWA_ALPHA);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// ============================================================
// 送礼 hook（NpcSendPresent 绑定函数 0x4C88E0（v1.30 正确值；旧注释 0x495760 实为 MapObjectSetBodyHitEnabled，物体身体碰撞启用，非送礼函数））
// ============================================================
static void OnGiftSent() {
    i64 sec = ReadGameSeconds();
    if (sec < 0) return;
    i64 day = sec / SECONDS_PER_DAY;
    InterlockedExchange64(&g_giftDay, day);
    Log("[br] gift sent (day=%lld)\n", (long long)day);
}

// stub：
//   保存 rcx/rdx/r8/r9/rax（OnGiftSent 编译代码会破坏 volatile 寄存器；
//   原函数 +0x36 处 movsxd rax,[rdx+r9*2] 直接使用 rdx/r9）
//   栈对齐：入口 rsp≡8 → 5 push(40B)≡0 → sub 0x20≡0 → call 前≡0 ✓
static bool BuildGiftStub() {
    g_giftStub = VirtualAlloc(nullptr, 0x200, MEM_COMMIT | MEM_RESERVE,
                              PAGE_EXECUTE_READWRITE);
    if (!g_giftStub) return false;

    unsigned char* m = (unsigned char*)g_giftStub;
    size_t o = 0;

    // --- 保存参数寄存器（5 push = 40 字节，rsp: 8→0 mod 16）---
    m[o++] = 0x51;          // push rcx
    m[o++] = 0x52;          // push rdx
    m[o++] = 0x41; m[o++] = 0x50;  // push r8
    m[o++] = 0x41; m[o++] = 0x51;  // push r9
    m[o++] = 0x50;          // push rax

    // --- 影子空间 ---
    m[o++] = 0x48; m[o++] = 0x83; m[o++] = 0xEC; m[o++] = 0x20;  // sub rsp,0x20

    // --- 调用 OnGiftSent ---
    m[o++] = 0x48; m[o++] = 0xB8;  // mov rax, imm64
    size_t fnpos = o; o += 8;
    m[o++] = 0xFF; m[o++] = 0xD0;  // call rax

    // --- 恢复 ---
    m[o++] = 0x48; m[o++] = 0x83; m[o++] = 0xC4; m[o++] = 0x20;  // add rsp,0x20
    m[o++] = 0x58;          // pop rax
    m[o++] = 0x41; m[o++] = 0x59;  // pop r9
    m[o++] = 0x41; m[o++] = 0x58;  // pop r8
    m[o++] = 0x5A;          // pop rdx
    m[o++] = 0x59;          // pop rcx

    // --- 重放 20 字节 prologue（4 条 mov，不触碰 rsp）---
    // mov [rsp+0x08], rbx
    m[o++] = 0x48; m[o++] = 0x89; m[o++] = 0x5C; m[o++] = 0x24; m[o++] = 0x08;
    // mov [rsp+0x10], rbp
    m[o++] = 0x48; m[o++] = 0x89; m[o++] = 0x6C; m[o++] = 0x24; m[o++] = 0x10;
    // mov [rsp+0x18], rsi
    m[o++] = 0x48; m[o++] = 0x89; m[o++] = 0x74; m[o++] = 0x24; m[o++] = 0x18;
    // mov [rsp+0x20], rdi
    m[o++] = 0x48; m[o++] = 0x89; m[o++] = 0x7C; m[o++] = 0x24; m[o++] = 0x20;

    // --- 跳回原函数 + 20 ---
    m[o++] = 0x48; m[o++] = 0xB8;  // mov rax, imm64
    size_t backpos = o; o += 8;
    m[o++] = 0xFF; m[o++] = 0xE0;  // jmp rax

    // 填入地址
    u64 fn = (u64)(uintptr_t)&OnGiftSent;
    memcpy(m + fnpos, &fn, 8);
    u64 back = (u64)g_giftBackAddr;
    memcpy(m + backpos, &back, 8);

    FlushInstructionCache(GetCurrentProcess(), m, o);
    return true;
}

static bool InstallGiftHook() {
    g_giftHookAddr = g_exeBase + RVA_NPC_SEND_PRESENT;

    // M15: memcpy 前先验证 hook 地址可读，RVA_NPC_SEND_PRESENT 是 HotConfig 可热调变量
    if (!IsReadable((const void*)g_giftHookAddr, GIFT_PROLOGUE_LEN)) {
        Log("[br] gift hook addr %p not readable (GIFT_PROLOGUE_LEN=%d), abort\n",
            (void*)g_giftHookAddr, GIFT_PROLOGUE_LEN);
        return false;
    }

    memcpy(g_giftOrig, (const void*)g_giftHookAddr, GIFT_PATCH_LEN);

    // 校验 20 字节 prologue（4 条 mov）
    unsigned char probe[20];
    memcpy(probe, (const void*)g_giftHookAddr, 20);
    if (probe[0]  != 0x48 || probe[1]  != 0x89 || probe[2]  != 0x5C ||
        probe[3]  != 0x24 || probe[4]  != 0x08 ||
        probe[5]  != 0x48 || probe[6]  != 0x89 || probe[7]  != 0x6C ||
        probe[8]  != 0x24 || probe[9]  != 0x10 ||
        probe[10] != 0x48 || probe[11] != 0x89 || probe[12] != 0x74 ||
        probe[13] != 0x24 || probe[14] != 0x18 ||
        probe[15] != 0x48 || probe[16] != 0x89 || probe[17] != 0x7C ||
        probe[18] != 0x24 || probe[19] != 0x20) {
        Log("[br] FAIL: NpcSendPresent prologue mismatch\n");
        return false;
    }

    g_giftBackAddr = g_giftHookAddr + GIFT_PROLOGUE_LEN;

    if (!BuildGiftStub()) {
        Log("[br] FAIL: gift stub alloc\n");
        return false;
    }

    unsigned char patch[12];
    patch[0] = 0x48; patch[1] = 0xB8;             // mov rax, imm64
    memcpy(patch + 2, &g_giftStub, 8);
    patch[10] = 0xFF; patch[11] = 0xE0;            // jmp rax

    if (!qol::WritePatchChecked((void*)g_giftHookAddr, patch, GIFT_PATCH_LEN)) {
        Log("[br] FAIL: WritePatchChecked\n");
        return false;
    }

    g_giftHooked = true;
    Log("[br] NpcSendPresent hooked %p -> stub %p (back %p)\n",
        (void*)g_giftHookAddr, g_giftStub, (void*)g_giftBackAddr);
    return true;
}

static void UninstallGiftHook() {
    if (!g_giftHooked) return;
    DWORD oldProtect;
    if (VirtualProtect((void*)g_giftHookAddr, GIFT_PATCH_LEN,
                       PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy((void*)g_giftHookAddr, g_giftOrig, GIFT_PATCH_LEN);
        VirtualProtect((void*)g_giftHookAddr, GIFT_PATCH_LEN, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(),
                              (void*)g_giftHookAddr, GIFT_PATCH_LEN);
    }
    g_giftHooked = false;
    if (g_giftStub) {
        VirtualFree(g_giftStub, 0, MEM_RELEASE);
        g_giftStub = nullptr;
    }
}

// ============================================================
// 生日计算与显示
// ============================================================
static void RefreshBirthday(i64 sec) {
    i64 day;
    int season, doy;
    wchar_t buf[512];
    int n, found = 0;

    if (sec < 0) { SetDisplayText(L""); return; }
    day = sec / SECONDS_PER_DAY;
    if (day < 0) { SetDisplayText(L""); return; }
    season = (int)((day / DAYS_PER_SEASON) % SEASON_COUNT);
    doy = (int)(day % DAYS_PER_SEASON) + 1;

    n = swprintf(buf, 512, L"生日：");
    for (int i = 0; i < BIRTHDAY_COUNT; i++) {
        if (BIRTHDAYS[i].season != season || BIRTHDAYS[i].day != doy)
            continue;
        n += swprintf(buf + n, 512 - n, found ? L"、%s" : L"%s",
                      BIRTHDAYS[i].name);
        found = 1;
    }
    // v1.1.2：不再显示礼物行（用户反馈识别经常出错）
    // v1.2.0：测试文字已移除（HUD 效果用户确认），无生日时隐藏
    if (!found) { SetDisplayText(L""); return; }
    SetDisplayText(buf);
}

// ============================================================
// worker 线程
// ============================================================
static DWORD WINAPI WorkerThread(LPVOID) {
    i64 last_day = -1;
    i64 last_sec = -1;
    int follow_tick = 0;
    ULONGLONG showStart = 0;   // 当前显示窗口的起始 tick

    Sleep(1700);  // 原 600ms + P1-6 相位偏移 1100ms，防多 MOD 同帧共振
    // 送礼 hook 在线程中安装（避开 DllMain loader lock）
    if (!InstallGiftHook()) {
        Log("[br] gift hook install failed, continue without\n");
    }

    while (g_running) {
        i64 sec, day;
        Sleep(500);
        HotConfig_Poll();
        // P0 探针（每轮迭代计时上报）
        static int s_bdgSlot = -1;
        if (s_bdgSlot < 0) s_bdgSlot = qol::budget::Slot("BirthdayReminder");
        struct BdgGuard {
            int slot; uint64_t t0;
            ~BdgGuard() { qol::budget::Report(slot, qol::budget::NowUs() - t0); }
        } bdgGuard = { s_bdgSlot, qol::budget::NowUs() };
        // 每 5 秒跟随游戏窗口位置（用当前自适应宽高，不强制重绘防闪）
        if ((++follow_tick % 10) == 0 && g_hudHwnd) {
            HWND game = FindWindowA("Honogurashinoniwa", nullptr);
            int ax, ay;
            HudAnchor(game, &ax, &ay);
            RECT cr;
            if (GetWindowRect(g_hudHwnd, &cr)) {
                int tx = ax - g_hudW, ty = ay;
                if (cr.left != tx || cr.top != ty) {
                    MoveWindow(g_hudHwnd, tx, ty, g_hudW, g_hudH, FALSE);
                    if (g_hudShown) InvalidateRect(g_hudHwnd, nullptr, FALSE);
                }
            }
        }
        if (QolGameBusy()) { QolHudGuardVisibility(g_hudHwnd); continue; }  // P1-2
        sec = ReadGameSeconds();
        if (sec < 0) continue;
        // 跳时/睡觉（含 TimeFreeze 冻结点抖动）：不直接隐藏，仅强制按新时间重算。
        // v1.1.2 修复：原先直接隐藏+跨日重显在时间抖动下会形成隐藏↔显示闪烁循环。
        if (last_sec >= 0 && (sec - last_sec) > SLEEP_DELTA_SEC) {
            last_day = -1;
        }
        last_sec = sec;
        day = sec / SECONDS_PER_DAY;
        if (day != last_day) {
            last_day = day;
            RefreshBirthday(sec);
            showStart = GetTickCount64();   // 新的 10 分钟显示窗口
        }
        // 送礼检测：当天已送出礼物且提示显示中 → 隐藏
        if (g_giftDay >= 0 && g_giftDay == day && g_display[0]) {
            SetDisplayText(L"");
            Log("[br] birthday reminder hidden (gift sent, day=%lld)\n",
                (long long)day);
        }
        // v1.1.2：显示满 10 分钟自动隐藏（真实时间，减少常驻负担）
        if (g_display[0] && showStart &&
            GetTickCount64() - showStart > 600000ULL) {
            SetDisplayText(L"");
            Log("[br] hidden (10-min display window elapsed)\n");
        }
        // v1.0.3: 失焦隐藏 HUD（不飘桌面）
        QolHudGuardVisibility(g_hudHwnd);
    }
    return 0;
}

// ============================================================
// preflight：clock 锚点验证
// ============================================================
static bool Preflight() {
    g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_exeBase) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(g_exeBase);
    // clock lea（48 8B 05 disp32 @ RVA_CLOCK_LEA）
    if (p[RVA_CLOCK_LEA] != 0x48 || p[RVA_CLOCK_LEA + 1] != 0x8B ||
        p[RVA_CLOCK_LEA + 2] != 0x05) {
        Log("[br] FAIL: clock lea mismatch\n");
        return false;
    }
    return true;
}

// ============================================================
// DLL 入口
// ============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        LogOpen("birthdayreminder");
        qol::obs::Init(nullptr);  // v1.6.x: 统一观测层接入
        qol::obs::SetInt("birthdayreminder", "ready", 1);
        qol::obs::EventF(1, 2500, L"[BirthdayReminder] \u5DF2\u542F\u7528 \u00B7 \u72B6\u6001\u63D0\u793A\u5DF2\u63A5\u5165");
    // v-local：游戏版本闸门（village.exe 非 v1.20 build 25311578 则不启用）
    if (!qol::ExeBuildSupported()) {
        Log("[BirthdayReminder] 游戏版本不匹配（非 v1.20 build 25311578）→ 本 MOD 不启用\n");
        return TRUE;
    }
        HotConfig_Register("birthdayreminder", (void*)&RVA_CLOCK_LEA,        "RVA_CLOCK_LEA",        HOT_RVA, 0xE2EA3);
        HotConfig_Register("birthdayreminder", (void*)&CLOCK_SAVE_0x208,     "CLOCK_SAVE_0x208",     HOT_RVA, 0x208);
        HotConfig_Register("birthdayreminder", (void*)&SAVE_RAW_SECOND,      "SAVE_RAW_SECOND",      HOT_RVA, 0x3270);
        HotConfig_Register("birthdayreminder", (void*)&RVA_NPC_SEND_PRESENT, "RVA_NPC_SEND_PRESENT", HOT_RVA, 0x4C88E0);
        HotConfig_Poll();
        HotConfig_DumpCE("birthdayreminder");
        if (!Preflight()) {
            Log("[br] preflight failed, abort\n");
            // 2026-10-28：abort 路径禁用 LogClose（同 MineHelper 修复）——
            // DllMain(ATTACH) 持加载器锁时拆除日志后端会引发 ~2s 后 write@0x24
            // 进程崩溃。改 LogFlush 只刷盘，资源交由 atexit 收尾。
            LogFlush();
            return TRUE;
        }
        SetProcessDPIAware();
        LoadGameFont();
        g_running = 1;
        g_hHud    = CreateThread(nullptr, 0, HudThread, nullptr, 0, nullptr);
        g_hWorker = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        Log("[br] BirthdayReminder v1.2.0 loaded; base=%p\n",
            reinterpret_cast<void*>(g_exeBase));
    } else if (reason == DLL_PROCESS_DETACH) {
        qol::obs::Shutdown();  // v1.6.x: 解观测层映射（仅本 DLL 视图）
        g_running = 0;
        // v1.2.1: 卸载清理——信号线程退出并 join（2s 超时），线程退出时会自行销毁 HUD 窗口
        if (g_hHud)    { WaitForSingleObject(g_hHud, 2000);    CloseHandle(g_hHud);    g_hHud = nullptr; }
        if (g_hWorker) { WaitForSingleObject(g_hWorker, 2000); CloseHandle(g_hWorker); g_hWorker = nullptr; }
        UninstallGiftHook();
        if (g_hudFont) { DeleteObject(g_hudFont); g_hudFont = nullptr; }
        UnloadGameFont();
        LogClose();
    }
    return TRUE;
}
