// modmanager.cpp —— ModManager v1.2.6: 游戏内 MOD 开关管理器 + F2 快捷菜单
//
// v1.1.2: static 缓冲区扩容（16→64 槽）+ TerminateProcess 前存档检测。
// v1.1.1 修复：注册表清理——移除退役 SaveBackup/LangHelper、去重第三方 MOD 重复条目。
//              logging.cpp ASCII 快速路径（共享框架更新）。
// v1.1.0 新增：第三方 MOD 热键自动扫描（README + qol_meta.txt 标准）、热键缓存、删除快捷键
// v1.0.2 新增：第三方 MOD 热键只读显示（从 README 提取，面板可见但不支持改键）
// v1.0.1 发布版：修复版本号排序、目录清理安全、存档检测节流、热键冲突预计算、RemoveDirectoryRecursive 替代 _wsystem
// v0.5.0 新增：手柄 L1+R1 触发面板、手柄改键录制、滑动开关还原、改键按钮、
//             顶部警示栏、自绘确认弹窗（支持手柄）、面板手柄导航
// v0.4.0 重构：注册表改为模板 + 运行时自动扫描，新增/改版本不再需要重编译
//
// 独立 DLL 插件，由 steam_api64 桥接加载器加载。
// 功能：扫描 Mods / 待用MOD 目录，在游戏内 F2 面板展示并管理
//       全部 MOD 的启用 / 停用状态。
//
// 开关机制：移动 MOD 整个文件夹
//   启用目录：<游戏根>\Mods\<MOD文件夹>
//   停用目录：<游戏根>\待用MOD\<MOD文件夹>
//
// 行为约定：
//   - 进存档后锁定开关（避免运行时卸载 DLL 导致崩溃）
//   - 修改开关后【关闭面板时】统一提示"需要重启游戏生效"，并自动关闭游戏
//   - 不允许停用 ModManager 自身（面板会关闭无法恢复）
//   - 列表排序：已启用在前、停用在后；支持鼠标滚轮滚动
//   - MOD 间依赖：被依赖 MOD 未启用时显示"依赖缺失"警示
//
// 设计参考：settings_panel.inl（权威源码）+ prototype-v2.html（视觉风格）

#include <windows.h>
#include <shlobj.h>   // SHGetFolderPathW
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include <algorithm>
#include <mmsystem.h>    // joyGetPosEx (winmm)
#include <setupapi.h>    // SetupAPI 枚举 HID 设备
#include "quickmenu.h"   // QoL 快捷菜单跨 DLL 约定
#include <hidsdi.h>      // HidD_GetHidGuid / HidD_GetAttributes
#include <atomic>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "msimg32.lib")  // AlphaBlend

// ============================================================
// 日志（QoL_Shared）
// ============================================================
#include "logging.h"
#include "budget.h"    // v1.2.3: P0 性能探针读取（共享内存跨 DLL）
#include "game_window.h"  // v1.2.6: HUD 可见性守卫（失焦隐藏，不飘桌面）

// 日志开关：发布版禁用日志
// #define MODMANAGER_LOGGING   // v1.2.17 转正：日志关闭（定位时取消注释重编）
#ifdef MODMANAGER_LOGGING
#else
  #define LogOpen(x)  ((void)0)
  #define Log(...)    ((void)0)
  #define LogV(...)   ((void)0)
  #define LogClose()  ((void)0)
#endif

// 版本号
#define MODMANAGER_VERSION L"v1.2.7"

// ============================================================
// 手柄输入（三路混合：XInput 线程 + HID 直读 DualSense 线程 + joyGetPosEx 回退）
// 参考 ChestSort 的成熟方案，扩展为全按键状态共享（L1/R1 + 改键录制需要）
// ============================================================

// ---- 统一手柄按键状态（所有路径映射到这个结构体）----
// 通过原子变量在后台线程和主线程间共享
enum GamepadBtn : int {
    GP_NONE = 0,
    GP_DPAD_UP, GP_DPAD_DOWN, GP_DPAD_LEFT, GP_DPAD_RIGHT,
    GP_L1, GP_R1, GP_L2, GP_R2,
    GP_CROSS, GP_CIRCLE, GP_SQUARE, GP_TRIANGLE,
    GP_L3, GP_R3, GP_SHARE, GP_OPTIONS,
};

// 单个按键的按下状态（原子变量，线程间共享）
struct GamepadState {
    std::atomic<bool> connected{false};
    std::atomic<bool> dpadUp{false};
    std::atomic<bool> dpadDown{false};
    std::atomic<bool> dpadLeft{false};
    std::atomic<bool> dpadRight{false};
    std::atomic<bool> l1{false};
    std::atomic<bool> r1{false};
    std::atomic<bool> l2{false};
    std::atomic<bool> r2{false};
    std::atomic<bool> cross{false};
    std::atomic<bool> circle{false};
    std::atomic<bool> square{false};
    std::atomic<bool> triangle{false};
    std::atomic<bool> l3{false};
    std::atomic<bool> r3{false};
    std::atomic<bool> share{false};
    std::atomic<bool> options{false};
};

static GamepadState g_gpXInput;   // XInput 路径共享状态
static GamepadState g_gpHID;     // HID 直读路径共享状态
static bool g_xinputReady = false;
static bool g_hidReady = false;

// 按键名 → GamepadBtn 映射表（用于改键录制）
struct GpNameMap { const wchar_t* name; GamepadBtn btn; };
static const GpNameMap kGpNames[] = {
    { L"D-pad Up",    GP_DPAD_UP },
    { L"D-pad Down",  GP_DPAD_DOWN },
    { L"D-pad Left",  GP_DPAD_LEFT },
    { L"D-pad Right", GP_DPAD_RIGHT },
    { L"L1",         GP_L1 },
    { L"R1",         GP_R1 },
    { L"L2",         GP_L2 },
    { L"R2",         GP_R2 },
    { L"Cross",      GP_CROSS },
    { L"Circle",     GP_CIRCLE },
    { L"Square",     GP_SQUARE },
    { L"Triangle",   GP_TRIANGLE },
    { L"L3",        GP_L3 },
    { L"R3",        GP_R3 },
    { L"Share",      GP_SHARE },
    { L"Options",    GP_OPTIONS },
};
static constexpr int kGpNameCount = sizeof(kGpNames) / sizeof(kGpNames[0]);

// 从 GamepadState 读取某个按键的按下状态
static bool GpIsPressed(const GamepadState& gp, GamepadBtn btn) {
    switch (btn) {
    case GP_DPAD_UP:    return gp.dpadUp.load(std::memory_order_relaxed);
    case GP_DPAD_DOWN:  return gp.dpadDown.load(std::memory_order_relaxed);
    case GP_DPAD_LEFT:  return gp.dpadLeft.load(std::memory_order_relaxed);
    case GP_DPAD_RIGHT: return gp.dpadRight.load(std::memory_order_relaxed);
    case GP_L1:         return gp.l1.load(std::memory_order_relaxed);
    case GP_R1:         return gp.r1.load(std::memory_order_relaxed);
    case GP_L2:         return gp.l2.load(std::memory_order_relaxed);
    case GP_R2:         return gp.r2.load(std::memory_order_relaxed);
    case GP_CROSS:      return gp.cross.load(std::memory_order_relaxed);
    case GP_CIRCLE:     return gp.circle.load(std::memory_order_relaxed);
    case GP_SQUARE:     return gp.square.load(std::memory_order_relaxed);
    case GP_TRIANGLE:   return gp.triangle.load(std::memory_order_relaxed);
    case GP_L3:        return gp.l3.load(std::memory_order_relaxed);
    case GP_R3:        return gp.r3.load(std::memory_order_relaxed);
    case GP_SHARE:      return gp.share.load(std::memory_order_relaxed);
    case GP_OPTIONS:    return gp.options.load(std::memory_order_relaxed);
    default: return false;
    }
}

// 检查任一路径的某个按键是否被按下（XInput > HID > joyGetPosEx）
static bool AnyGpPressed(GamepadBtn btn) {
    if (g_xinputReady && g_gpXInput.connected.load(std::memory_order_relaxed) &&
        GpIsPressed(g_gpXInput, btn)) return true;
    if (g_hidReady && g_gpHID.connected.load(std::memory_order_relaxed) &&
        GpIsPressed(g_gpHID, btn)) return true;
    return false;
}

// ---- XInput 定义（不依赖 <xinput.h>）----
struct XINPUT_GAMEPAD_EX {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    short sThumbLX;
    short sThumbLY;
    short sThumbRX;
    short sThumbRY;
};
struct XINPUT_STATE_EX {
    DWORD           dwPacketNumber;
    XINPUT_GAMEPAD_EX Gamepad;
};

#define XI_DPAD_UP      0x0001
#define XI_DPAD_DOWN    0x0002
#define XI_DPAD_LEFT    0x0004
#define XI_DPAD_RIGHT   0x0008
#define XI_START        0x0010
#define XI_BACK         0x0020
#define XI_L3           0x0040
#define XI_R3           0x0080
#define XI_L1           0x0100
#define XI_R1           0x0200
#define XI_A            0x1000
#define XI_B            0x2000
#define XI_X            0x4000
#define XI_Y            0x8000

static DWORD (WINAPI *g_XInputGetState)(DWORD, void*) = nullptr;
static volatile LONG g_xinputThreadRunning = 0;
static HANDLE g_xinputThread = nullptr;

static void LoadXInput() {
    if (g_XInputGetState) return;
    HMODULE h = LoadLibraryW(L"xinput1_4.dll");
    if (!h) h = LoadLibraryW(L"xinput1_3.dll");
    if (!h) h = LoadLibraryW(L"xinput9_1_0.dll");
    if (h) g_XInputGetState = reinterpret_cast<DWORD(WINAPI*)(DWORD, void*)>(
        GetProcAddress(h, "XInputGetState"));
}

// XInput 独立轮询线程
static DWORD WINAPI XInputPollThread(LPVOID) {
    Log("[ModManager] [xinput] 线程启动\n");
    bool wasConnected = false;
    while (InterlockedCompareExchange(&g_xinputThreadRunning, 1, 1) == 1) {
        if (!g_XInputGetState) { Sleep(2000); continue; }
        XINPUT_STATE_EX state = {};
        DWORD result = g_XInputGetState(0, &state);
        if (result == 0) {  // ERROR_SUCCESS
            if (!wasConnected) { wasConnected = true; Log("[ModManager] [xinput] 手柄已连接\n"); }
            g_gpXInput.connected.store(true, std::memory_order_relaxed);
            WORD b = state.Gamepad.wButtons;
            g_gpXInput.dpadUp.store((b & XI_DPAD_UP) != 0, std::memory_order_relaxed);
            g_gpXInput.dpadDown.store((b & XI_DPAD_DOWN) != 0, std::memory_order_relaxed);
            g_gpXInput.dpadLeft.store((b & XI_DPAD_LEFT) != 0, std::memory_order_relaxed);
            g_gpXInput.dpadRight.store((b & XI_DPAD_RIGHT) != 0, std::memory_order_relaxed);
            g_gpXInput.l1.store((b & XI_L1) != 0, std::memory_order_relaxed);
            g_gpXInput.r1.store((b & XI_R1) != 0, std::memory_order_relaxed);
            g_gpXInput.l2.store(state.Gamepad.bLeftTrigger > 128, std::memory_order_relaxed);
            g_gpXInput.r2.store(state.Gamepad.bRightTrigger > 128, std::memory_order_relaxed);
            g_gpXInput.cross.store((b & XI_A) != 0, std::memory_order_relaxed);
            g_gpXInput.circle.store((b & XI_B) != 0, std::memory_order_relaxed);
            g_gpXInput.square.store((b & XI_X) != 0, std::memory_order_relaxed);
            g_gpXInput.triangle.store((b & XI_Y) != 0, std::memory_order_relaxed);
            g_gpXInput.l3.store((b & XI_L3) != 0, std::memory_order_relaxed);
            g_gpXInput.r3.store((b & XI_R3) != 0, std::memory_order_relaxed);
            g_gpXInput.share.store((b & XI_BACK) != 0, std::memory_order_relaxed);
            g_gpXInput.options.store((b & XI_START) != 0, std::memory_order_relaxed);
        } else {
            g_gpXInput.connected.store(false, std::memory_order_relaxed);
            if (wasConnected) { wasConnected = false; Log("[ModManager] [xinput] 手柄断开 (err=%lu)\n", result); }
        }
        Sleep(16);  // ~60Hz（与游戏帧率对齐，足够检测按键边沿）
    }
    Log("[ModManager] [xinput] 线程退出\n");
    return 0;
}

// ---- DualSense HID 直读（独立线程，热插拔重连）----
static constexpr unsigned short DUALSENSE_VID = 0x054C;
static constexpr unsigned short DUALSENSE_PID = 0x0CE6;
static constexpr size_t DUALSENSE_REPORT_SIZE = 64;
static constexpr int DUALSENSE_BTN_OFFSET = 8;  // buttons[0] 字节偏移

static volatile LONG g_hidThreadRunning = 0;
static HANDLE g_hidThread = nullptr;
static HANDLE g_hidDev = nullptr;  // H6: 保存当前 HID 设备句柄，供 ShutdownGamepad 取消阻塞 ReadFile

// 枚举 HID 设备找到 DualSense 并打开
static HANDLE OpenDualSense() {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return nullptr;

    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &hidGuid, i, &ifData); i++) {
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, nullptr, 0, &requiredSize, nullptr);
        if (requiredSize == 0) continue;
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(requiredSize);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, requiredSize, nullptr, nullptr)) {
            free(detail); continue;
        }
        HANDLE hDev = CreateFileW(detail->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        free(detail);
        if (hDev == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr = {};
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(hDev, &attr) &&
            attr.VendorID == DUALSENSE_VID && attr.ProductID == DUALSENSE_PID) {
            SetupDiDestroyDeviceInfoList(hDevInfo);
            return hDev;
        }
        CloseHandle(hDev);
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return nullptr;
}

// DualSense USB HID 报告布局（源自 Linux hid-playstation.c）:
//   byte 0 = Report ID (0x01)
//   byte 7 = seq_number
//   byte 8 = buttons[0]: bits 0-3 = D-pad Hat, bits 4-7 = □○△×
//   byte 9 = buttons[1]: bits 0-3 = L1/R1/L2/R2, bits 4-7 = L3/R3/Share/Options
//   D-pad Hat: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8=Released
static DWORD WINAPI HidPollThread(LPVOID) {
    Log("[ModManager] [hid] 线程启动\n");
    bool wasConnected = false;
    while (InterlockedCompareExchange(&g_hidThreadRunning, 1, 1) == 1) {
        HANDLE hDev = OpenDualSense();
        if (!hDev) {
            if (wasConnected) {
                wasConnected = false;
                g_gpHID.connected.store(false, std::memory_order_relaxed);
                Log("[ModManager] [hid] DualSense 断开，等待重连...\n");
            }
            Sleep(2000);
            continue;
        }
        if (!wasConnected) { wasConnected = true; g_gpHID.connected.store(true, std::memory_order_relaxed); Log("[ModManager] [hid] DualSense 已连接\n"); }
        g_hidDev = hDev;  // H6: 保存设备句柄，供 ShutdownGamepad 关闭取消 ReadFile
        BYTE report[DUALSENSE_REPORT_SIZE] = {};
        while (InterlockedCompareExchange(&g_hidThreadRunning, 1, 1) == 1) {
            __try {
                DWORD bytesRead = 0;
                BOOL ok = ReadFile(hDev, report, DUALSENSE_REPORT_SIZE, &bytesRead, nullptr);
                if (!ok || bytesRead < 10) break;  // 断开
                if (report[0] == 0x01 && bytesRead > DUALSENSE_BTN_OFFSET + 1) {
                    BYTE b0 = report[DUALSENSE_BTN_OFFSET];     // buttons[0]
                    BYTE b1 = report[DUALSENSE_BTN_OFFSET + 1]; // buttons[1]
                    // D-pad Hat (低4位)
                    int hat = b0 & 0x0F;
                    g_gpHID.dpadUp.store(hat == 0 || hat == 1 || hat == 7, std::memory_order_relaxed);
                    g_gpHID.dpadDown.store(hat == 3 || hat == 4 || hat == 5, std::memory_order_relaxed);
                    g_gpHID.dpadLeft.store(hat == 5 || hat == 6 || hat == 7, std::memory_order_relaxed);
                    g_gpHID.dpadRight.store(hat == 1 || hat == 2 || hat == 3, std::memory_order_relaxed);
                    // □○△× (b0 高4位: bit4=□, bit5=○, bit6=△, bit7=×)
                    g_gpHID.square.store((b0 & 0x10) != 0, std::memory_order_relaxed);
                    g_gpHID.circle.store((b0 & 0x20) != 0, std::memory_order_relaxed);
                    g_gpHID.triangle.store((b0 & 0x40) != 0, std::memory_order_relaxed);
                    g_gpHID.cross.store((b0 & 0x80) != 0, std::memory_order_relaxed);
                    // L1/R1/L2/R2 (b1 低4位: bit0=L1, bit1=R1, bit2=L2, bit3=R2)
                    g_gpHID.l1.store((b1 & 0x01) != 0, std::memory_order_relaxed);
                    g_gpHID.r1.store((b1 & 0x02) != 0, std::memory_order_relaxed);
                    g_gpHID.l2.store((b1 & 0x04) != 0, std::memory_order_relaxed);
                    g_gpHID.r2.store((b1 & 0x08) != 0, std::memory_order_relaxed);
                    // L3/R3/Share/Options (b1 高4位: bit4=L3, bit5=R3, bit6=Share, bit7=Options)
                    g_gpHID.l3.store((b1 & 0x10) != 0, std::memory_order_relaxed);
                    g_gpHID.r3.store((b1 & 0x20) != 0, std::memory_order_relaxed);
                    g_gpHID.share.store((b1 & 0x40) != 0, std::memory_order_relaxed);
                    g_gpHID.options.store((b1 & 0x80) != 0, std::memory_order_relaxed);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
        }
        g_hidDev = nullptr;  // H6: 清除全局引用，避免 ShutdownGamepad 重复关闭
        CloseHandle(hDev);
        g_gpHID.connected.store(false, std::memory_order_relaxed);
    }
    Log("[ModManager] [hid] 线程退出\n");
    return 0;
}

// ---- joyGetPosEx 辅助 ----
static void (WINAPI *g_joyConfigChanged)(void) = nullptr;
static void LoadJoyConfigChanged() {
    if (g_joyConfigChanged) return;
    HMODULE hWinmm = GetModuleHandleW(L"winmm.dll");
    if (!hWinmm) hWinmm = LoadLibraryW(L"winmm.dll");
    if (hWinmm) g_joyConfigChanged = reinterpret_cast<void(WINAPI*)(void)>(
        GetProcAddress(hWinmm, "joyConfigChanged"));
}

// 初始化手柄：启动 XInput 线程 + HID 线程
static void InitGamepad() {
    LoadXInput();
    if (g_XInputGetState) {
        InterlockedExchange(&g_xinputThreadRunning, 1);
        g_xinputThread = CreateThread(nullptr, 0, XInputPollThread, nullptr, 0, nullptr);
        if (g_xinputThread) { g_xinputReady = true; Log("[ModManager] [xinput] 线程已创建\n"); }
    } else {
        Log("[ModManager] [xinput] xinput DLL 未找到\n");
    }
    InterlockedExchange(&g_hidThreadRunning, 1);
    g_hidThread = CreateThread(nullptr, 0, HidPollThread, nullptr, 0, nullptr);
    if (g_hidThread) { g_hidReady = true; Log("[ModManager] [hid] 线程已创建\n"); }
    LoadJoyConfigChanged();
}

// 停止手柄线程
static void ShutdownGamepad() {
    InterlockedExchange(&g_xinputThreadRunning, 0);
    InterlockedExchange(&g_hidThreadRunning, 0);
    // XInput 线程无阻塞调用，Sleep(50) 足够
    if (g_xinputThread) { Sleep(50); CloseHandle(g_xinputThread); g_xinputThread = nullptr; }
    // H6: HID 线程 ReadFile 阻塞，先关闭设备句柄取消 ReadFile，再等待线程退出
    if (g_hidThread) {
        HANDLE dev = g_hidDev;
        if (dev) { CloseHandle(dev); g_hidDev = nullptr; }
        WaitForSingleObject(g_hidThread, 2000);
        CloseHandle(g_hidThread);
        g_hidThread = nullptr;
    }
}

// 手柄 L1+R1 同时按下检测（用于打开面板）
static bool PollGamepadL1R1() {
    if (AnyGpPressed(GP_L1) && AnyGpPressed(GP_R1)) return true;
    // 路径 3: joyGetPosEx 同步回退（XInput 和 HID 都没手柄时）
    if (!g_xinputReady && !(g_hidReady && g_gpHID.connected.load(std::memory_order_relaxed))) {
        JOYINFOEX ji = {};
        ji.dwSize = sizeof(ji);
        ji.dwFlags = JOY_RETURNBUTTONS;
        if (joyGetPosEx(JOYSTICKID1, &ji) == JOYERR_NOERROR) {
            return (ji.dwButtons & 0x10) && (ji.dwButtons & 0x20);
        }
    }
    return false;
}

// 返回当前按下的第一个手柄按键名称（用于改键录制），nullptr = 无按键
static const wchar_t* GetPressedGamepadButton() {
    // 路径 1+2: 从共享变量读（XInput + HID）
    for (int n = 0; n < kGpNameCount; ++n) {
        if (AnyGpPressed(kGpNames[n].btn)) return kGpNames[n].name;
    }
    // 路径 3: joyGetPosEx 同步回退
    if (!g_xinputReady && !(g_hidReady && g_gpHID.connected.load(std::memory_order_relaxed))) {
        JOYINFOEX ji = {};
        ji.dwSize = sizeof(ji);
        ji.dwFlags = JOY_RETURNBUTTONS | JOY_RETURNPOV;
        if (joyGetPosEx(JOYSTICKID1, &ji) == JOYERR_NOERROR) {
            DWORD pov = ji.dwPOV;
            if (pov != 0xFFFF && pov != JOY_POVCENTERED) {
                if (pov < 4500 || pov > 31500) return L"D-pad Up";
                if (pov < 13500) return L"D-pad Right";
                if (pov < 22500) return L"D-pad Down";
                return L"D-pad Left";
            }
            DWORD b = ji.dwButtons;
            if (b & 0x001) return L"Cross";
            if (b & 0x002) return L"Circle";
            if (b & 0x004) return L"Square";
            if (b & 0x008) return L"Triangle";
            if (b & 0x010) return L"L1";
            if (b & 0x020) return L"R1";
            if (b & 0x040) return L"L2";
            if (b & 0x080) return L"R2";
            if (b & 0x100) return L"Share";
            if (b & 0x200) return L"Options";
            if (b & 0x400) return L"L3";
            if (b & 0x800) return L"R3";
        }
    }
    return nullptr;
}

// 手柄改键录制状态
static const wchar_t* g_gamepadCaptured = nullptr;  // 已捕获的手柄按键名

// 前向声明（PollGamepadForRecording 需要用到，定义移至全局变量之后）
static void PollGamepadForRecording();
static void WriteHotkeysFile(int index);
static void CancelRecordKey();

// ============================================================
// 颜色常量（v2 原型配色：深色底、暖金强调）
// ============================================================
namespace Colors {
    constexpr COLORREF Bg        = RGB(20, 24, 28);    // #14181c
    constexpr COLORREF Panel     = RGB(30, 34, 42);    // #1e222a
    constexpr COLORREF Panel2    = RGB(38, 46, 54);    // #262e36
    constexpr COLORREF Panel3    = RGB(46, 56, 66);    // #2e3842
    constexpr COLORREF Border    = RGB(60, 74, 80);    // #3c4a50
    constexpr COLORREF Text      = RGB(232, 230, 227); // #e8e6e3
    constexpr COLORREF TextDim   = RGB(154, 165, 176); // #9aa5b0
    constexpr COLORREF Accent    = RGB(217, 164, 65);  // #d9a441 暖金
    constexpr COLORREF Accent2   = RGB(127, 176, 105); // #7fb069 绿
    constexpr COLORREF Danger    = RGB(201, 107, 107); // #c96b6b 红
    constexpr COLORREF Warn      = RGB(224, 176, 96);  // #e0b060 警示黄
    constexpr COLORREF WarnBg    = RGB(52, 44, 30);    // 深黄底
}

// ============================================================
// 动态缩放（完整自适应布局：面板尺寸随游戏窗口分辨率等比缩放）
// ============================================================
static float g_scale = 1.0f;    // 当前缩放因子（1.0 = 基准 760 布局）

// 基准面板尺寸（在 ShowPanel 按游戏窗口计算实际尺寸，存到 g_panelW/H）
static int g_panelW = 760;
static int g_panelH = 760;

// 缩放像素：任意基准尺寸 × 当前 scale，四舍五入
static inline int S(int v) { return (int)(v * g_scale + 0.5f); }

// 常量：全部改为宏，引用处自动按 g_scale 缩放（VISIBLE_ROWS 是行数非像素，不缩放）
#define PANEL_WIDTH  g_panelW
#define PANEL_HEIGHT g_panelH
#define ROW_HEIGHT   S(62)
#define ROW_GAP      S(6)
#define ROW_START_Y  S(100)   // 顶部标题+警示栏后开始
#define VISIBLE_ROWS 9        // 一屏最多显示行数（不随缩放变化）
#define LEFT_MARGIN  S(24)
#define RIGHT_MARGIN S(24)

// 快捷键格子（行中间固定一格显示一个按键）
#define KEYCELL_W   S(92)   // 格子宽（容纳 "D-pad Left" 等长文本）
#define KEYCELL_H   S(32)   // 格子高
#define KEYCELL_GAP S(6)    // 格子间距

static constexpr wchar_t PANEL_CLASS[] = L"VillageModManagerPanel";

// ============================================================
// MOD 元数据
// ============================================================
enum class ModStatus : int {
    Unknown,
    Loaded,        // 已加载且正常
    Unsupported,   // unsupported exe（功能未生效）
    Error,         // 日志有 error/failed
    NotLoaded,     // 未加载（停用或加载失败）
};

// 开关状态（与文件夹位置解耦，由目录扫描实时确定）
enum class ToggleState : int {
    Enabled,   // 文件夹在 Mods\ 下
    Disabled,  // 文件夹在 待用MOD\ 下
    Missing,   // 两个目录都找不到（异常）
    Self,      // 是 ModManager 自身（不可停用）
};

struct ModEntry {
    // ---- 静态模板数据（编译期固定）----
    const wchar_t* name;          // 显示名（中文）
    const wchar_t* folderPrefix;  // 文件夹名前缀（运行时匹配，如 L"ChestSort"）
    const wchar_t* dllName;       // DLL 文件名（小写，数据 MOD 为 nullptr）
    const wchar_t* hotkey;        // 默认热键描述（nullptr = 无，可能含逗号分隔多键）
    const wchar_t* logFile;       // 日志文件名（nullptr = 无日志）
    bool          isSelf;         // 是否 ModManager 自身（不可停用）
    const wchar_t* dependsOn;     // 依赖的 MOD 显示名（nullptr = 无依赖）
    bool          thirdParty;    // 第三方 MOD（热键由 README/qol_meta.txt 扫描填充）
    bool          rekeyable;     // 热键是否可改（qol_meta.txt 声明则可改）

    // ---- 运行时填充（ScanModFolders）----
    wchar_t       folder[MAX_PATH]; // 文件夹名（含版本，运行时扫描填充）
    wchar_t       version[64];      // 版本号（从文件夹名提取）

    // ---- 运行时状态 ----
    ModStatus     status;
    ToggleState   toggle;
    ToggleState   startToggle;  // 面板打开时的初始开关（用于关闭时撤销）
    int           loadOrder;   // 加载顺序（-1 = 未加载）
    int           displayIdx;  // 当前显示顺序（用于重排后绘制）

    // 运行时热键列表（从 qol_hotkeys.txt 或默认值解析）
    wchar_t       hotkeys[6][24]; // 最多 6 个按键，每个最长 24 字符
    int           hotkeyCount;     // 实际按键数（0 = 无热键）
    wchar_t       hotkeyNames[6][32]; // 各热键的功能名（qol_meta.txt 声明）
};

// MOD 模板注册表（只含静态元数据，folder/version 运行时自动填充）
static ModEntry g_mods[] = {
    // ---- QoL 自有 MOD ----
    { L"ChestSort 箱子自动归类", L"ChestSort", L"chestsort.dll", L"4",
      L"qol_chestsort.log", false, nullptr },

    { L"Sower 范围播种", L"Sower", L"sower.dll", L"5, D-pad Left",
      L"qol_sower.log", false, nullptr },

    { L"EyeFix 正常眼", L"EyeFix", L"eyefix.dll", nullptr,
      L"qol_eyefix.log", false, nullptr },

    { L"MonsterMark 怪物标记", L"MonsterMark", L"monstermark.dll", nullptr,
      L"qol_monstermark.log", false, nullptr },

    { L"AutoFish 自动钓鱼", L"AutoFish", L"autofish.dll", nullptr,
      nullptr, false, nullptr },

    { L"ProductionAuto 链式自动化", L"ProductionAuto", L"productionauto.dll", L"F5",
      L"qol_productionauto.log", false, nullptr },

    { L"Scarecrow 稻草人重叠", L"Scarecrow", L"scarecrow.dll", nullptr,
      L"qol_scarecrow.log", false, nullptr },

    { L"AutoHarvest 自动采集", L"AutoHarvest", L"autoharvest.dll", L"F4",
      L"qol_autoharvest.log", false, nullptr },

    { L"SickleHarvest 镰刀范围收割", L"SickleHarvest", L"sickleharvest.dll", nullptr,
      L"qol_sickleharvest.log", false, nullptr },

    // ---- 第三方 MOD（thirdParty=true，热键只读显示，不支持改键）----
    // v1.2.14: AutoPet/BirthdayReminder/CameraZoom/HuntOneShot/MineHelper/
    // SelfServiceStore/TimeFreeze 7 个实为 QoL 自有 MOD，从本区移回正式区
    // （thirdParty=true 导致 ApplyHotkeysFile 跳过覆盖 → huntoneshot=F6 /
    // camerazoom=F7 在面板不显示；2026-10-06 用户实锤）
    { L"Teleport \x4F20\x9001", L"Teleport", L"teleport.dll", nullptr,
      nullptr, false, nullptr, true, false },

    { L"SCPatch \x5168\x666F\x91C7\x96C6", L"SCPatch", nullptr, nullptr,
      nullptr, false, nullptr, true, false },

    { L"AutoPet 自动摸摸", L"AutoPet", L"autopet.dll", nullptr,
      L"qol_autopet.log", false, nullptr },

    { L"BirthdayReminder 生日提醒", L"BirthdayReminder", L"birthdayreminder.dll", nullptr,
      L"qol_birthdayreminder.log", false, nullptr },

    { L"CameraZoom 镜头变焦", L"CameraZoom", L"camerazoom.dll", L"F7",
      L"qol_camerazoom.log", false, nullptr },

    { L"HuntOneShot 一击猎杀", L"HuntOneShot", L"huntoneshot.dll", L"F6",
      L"qol_huntoneshot.log", false, nullptr },

    { L"MineHelper 采矿助手", L"MineHelper", L"minehelper.dll", nullptr,
      L"qol_minehelper.log", false, nullptr },

    { L"SelfServiceStore 自助商店", L"SelfServiceStore", L"selfservice.dll", nullptr,
      L"qol_selfservice.log", false, nullptr },

    { L"TimeFreeze 时间冻结", L"TimeFreeze", L"timefreeze.dll", nullptr,
      L"qol_timefreeze.log", false, nullptr },

    // ---- ModManager 自身（最后显示，可停用保护）----
    { L"ModManager 模块管理器", L"ModManager", L"modmanager.dll", nullptr,
      L"qol_modmanager.log", true, nullptr },
};

static constexpr int g_modCount = sizeof(g_mods) / sizeof(g_mods[0]);

// ============================================================
// 全局状态
// ============================================================
static HMODULE g_module = nullptr;
static HWND g_panel = nullptr;
static HWND g_owner = nullptr;
static HFONT g_titleFont = nullptr;
static HFONT g_bodyFont = nullptr;
static HFONT g_smallFont = nullptr;

// 按当前 g_scale 重建三档字体（先删旧，再按缩放后字号创建）
static void RebuildScaledFonts() {
    if (g_titleFont) { DeleteObject(g_titleFont); g_titleFont = nullptr; }
    if (g_bodyFont)  { DeleteObject(g_bodyFont);  g_bodyFont = nullptr; }
    if (g_smallFont) { DeleteObject(g_smallFont); g_smallFont = nullptr; }
    g_titleFont = CreateFontW(-S(23), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_bodyFont = CreateFontW(-S(17), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_smallFont = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}
static bool g_panelVisible = false;
static bool g_needsRelease = true;

static bool g_inSave = false;           // 是否已进入存档（锁定开关）
static bool g_restartRequested = false; // 本轮是否修改过开关（关闭面板时统一提示）
static int  g_changedCount = 0;         // 本轮修改过的 MOD 数量
static std::wstring g_changedNames;     // 本轮修改过的 MOD 名称（顿号分隔）
static int  g_scrollTop = 0;            // 列表滚动偏移（单位：行）
static int  g_visibleCount = 0;         // v1.2.7: 可见条目数（Missing 已隐藏不占行）
static int  g_hoverRow = -1;            // 当前鼠标悬停行（-1 = 无）

static std::wstring g_gameDir;
static std::wstring g_modsDir;       // Mods
static std::wstring g_storageDir;    // 待用MOD
static std::wstring g_abandonDir;    // 弃用MOD

// ---- 按键录制状态（游戏内改键原型）----
static int  g_recModIdx = -1;    // 正在录制热键的 MOD 数组下标（-1 = 无）
static int  g_recKeyIdx = -1;    // 正在录制的按键下标（对应 hotkeys[] 槽位）
static int  g_capturedVk = -1;   // 已捕获、等待松开的 VK（-1 = 未捕获）
static int  g_recStage = 0;      // 录制阶段：0=捕获按键 1=已录完一个键，选择下一步(下一个/结束)
static int  g_recNextSel = 0;    // 选择阶段：0=下一个 1=删除 2=结束
static bool g_recNextRelease = true;  // 选择阶段手柄边沿触发
static bool g_recGpWait = false;     // 捕获阶段手柄防抖：进入捕获后先等待所有按键松开
static RECT g_keyRects[32][7];   // 各 MOD 各按键标签 + 改键按钮 的命中矩形（绘制时填充）

// 手柄状态
static bool g_gamepadNeedsRelease = true;  // L1+R1 边沿触发（需先释放再按下）

// ---- 面板手柄导航状态 ----
static int  g_gpCursor = -1;   // 手柄光标所在行（displayIdx，-1=未选中）
static int  g_gpFocus  = 0;    // 手柄行内焦点：0=改键区（无热键则直接开关） 1=开关
static bool g_gpNeedsRelease = true;     // 手柄导航键边沿触发（D-pad/R1/Circle）

// ---- 自绘确认弹窗状态 ----
static bool g_confirmDlg = false;   // 是否正在显示确认弹窗
static int  g_confirmSel = 1;       // 0=是(重启)，1=否(保持运行)，默认否
static bool g_confirmDlgRelease = true;  // 弹窗内手柄边沿触发

// 热键冲突缓存（在 ScanModStatus 时预计算，-1=未计算, -2=无冲突, >=0=冲突键索引）
static int g_conflictCache[32] = {};
static bool g_conflictCacheValid = false;

// 前向声明（手柄改键录制轮询/导航用）
static void ToggleMod(int index);
static void BeginRecordKey(int modIdx, int keyIdx);
static void RecordKeyWritten();
static void RecordNext();
static void RecordDeleteKey();
static int  ArrayIdxOfDisplay(int displayIdx);
static void ConfirmApply();
static void ConfirmCancel();
static void HidePanel();

// ============================================================
// 手柄改键录制轮询（在 mod_tick 中调用）
// ============================================================
static void PollGamepadForRecording() {
    if (g_recModIdx < 0) return;       // 未在录制
    if (g_recStage != 0) return;       // 选择阶段不捕获
    if (g_capturedVk >= 0) return;     // 已捕获键盘键，等键盘完成

    // 手柄防抖：进入捕获阶段后，先等待所有按键完全松开，避免把"进入录制时的旧按键"录进去
    if (g_recGpWait) {
        for (int n = 0; n < kGpNameCount; ++n) {
            if (AnyGpPressed(kGpNames[n].btn)) return;  // 还有键按住，继续等
        }
        g_recGpWait = false;   // 全部松开，开始接受输入
    }

    // 手柄取消：捕获阶段按下 Circle 直接取消录制（不录成键）
    if (AnyGpPressed(GP_CIRCLE)) {
        Log("[ModManager] gamepad record cancelled by Circle\n");
        CancelRecordKey();
        return;
    }

    const wchar_t* pressed = GetPressedGamepadButton();
    if (pressed && !g_gamepadCaptured) {
        g_gamepadCaptured = pressed;
        InvalidateRect(g_panel, nullptr, FALSE);
    }
    if (!pressed && g_gamepadCaptured) {
        // 按键松开 → 写入并完成录制
        int modIdx = g_recModIdx;
        int keyIdx = g_recKeyIdx;
        if (modIdx >= 0 && modIdx < g_modCount &&
            keyIdx >= 0 && keyIdx < g_mods[modIdx].hotkeyCount) {
            wcscpy_s(g_mods[modIdx].hotkeys[keyIdx], g_gamepadCaptured);
            // 重新拼接 hotkey 字符串
            std::wstring joined;
            for (int h = 0; h < g_mods[modIdx].hotkeyCount; ++h) {
                if (h > 0) joined += L", ";
                joined += g_mods[modIdx].hotkeys[h];
            }
            static wchar_t s_gpHkBuf[128];
            wcscpy_s(s_gpHkBuf, joined.c_str());
            g_mods[modIdx].hotkey = s_gpHkBuf;
            WriteHotkeysFile(modIdx);
            Log("[ModManager] gamepad hotkey recorded: mod=%d key=%ls\n",
                modIdx, g_gamepadCaptured);
        }
        g_gamepadCaptured = nullptr;
        RecordKeyWritten();
    }
}

// ============================================================
// 手柄面板导航轮询（在 mod_tick 中调用）
// 逻辑：
//   面板可见 + 未录制 + 非确认弹窗：
//     D-pad 上/下 移动光标（光标为 displayIdx），D-pad 左/右 切换行内焦点（改键/开关），
//     R1 激活（切换开关/进入改键），Circle 关闭面板
//   确认弹窗打开：
//     D-pad 左/右 切换选项，R1 确认，Circle 取消
// ============================================================
static void PollGamepadNav() {
    if (!g_panelVisible) return;

    // ---- 录制流程分支（选择阶段：下一个/结束）----
    if (g_recModIdx >= 0) {
        if (g_recStage == 1) {
            bool left  = AnyGpPressed(GP_DPAD_LEFT);
            bool right = AnyGpPressed(GP_DPAD_RIGHT);
            bool confirm = AnyGpPressed(GP_R1);   // R1 确认
            bool circle = AnyGpPressed(GP_CIRCLE);
            if (g_recNextRelease) {
                if (!left && !right && !confirm && !circle) g_recNextRelease = false;
                return;
            }
            if (left || right) {
                g_recNextSel = 1 - g_recNextSel;
                g_recNextRelease = true;
                InvalidateRect(g_panel, nullptr, FALSE);
            } else if (confirm) {
                g_recNextRelease = true;
                if (g_recNextSel == 0) RecordNext();
                else CancelRecordKey();
            } else if (circle) {
                g_recNextRelease = true;
                CancelRecordKey();
            }
        }
        return;  // 捕获阶段不导航（由 PollGamepadForRecording 处理）
    }

    // ---- 确认弹窗分支 ----
    if (g_confirmDlg) {
        bool left  = AnyGpPressed(GP_DPAD_LEFT);
        bool right = AnyGpPressed(GP_DPAD_RIGHT);
        bool confirm = AnyGpPressed(GP_R1);   // R1 确认键
        bool circle = AnyGpPressed(GP_CIRCLE);
        if (g_confirmDlgRelease) {
            if (!left && !right && !confirm && !circle) g_confirmDlgRelease = false;
        } else {
            if (left || right) {
                g_confirmSel = 1 - g_confirmSel;
                g_confirmDlgRelease = true;
                InvalidateRect(g_panel, nullptr, FALSE);
            } else if (confirm) {
                g_confirmDlgRelease = true;
                if (g_confirmSel == 0) ConfirmApply();
                else ConfirmCancel();
            } else if (circle) {
                g_confirmDlgRelease = true;
                ConfirmCancel();
            }
        }
        return;
    }

    // ---- 主面板导航 ----
    bool up      = AnyGpPressed(GP_DPAD_UP);
    bool down    = AnyGpPressed(GP_DPAD_DOWN);
    bool left    = AnyGpPressed(GP_DPAD_LEFT);
    bool right   = AnyGpPressed(GP_DPAD_RIGHT);
    bool confirm = AnyGpPressed(GP_R1);   // R1 确认键
    bool circle  = AnyGpPressed(GP_CIRCLE);

    if (g_gpNeedsRelease) {
        if (!up && !down && !left && !right && !confirm && !circle) g_gpNeedsRelease = false;
        return;
    }

    if (circle) {
        g_gpNeedsRelease = true;
        HidePanel();
        return;
    }

    if (up || down) {
        g_gpNeedsRelease = true;
        if (down) {
            // 移到下一行
            if (g_gpCursor < g_modCount - 1) {
                g_gpCursor++;
            }
        } else {
            if (g_gpCursor > 0) {
                g_gpCursor--;
            }
        }
        // 切换行后：重置行内焦点到改键区（0）
        g_gpFocus = 0;
        // 若光标移出可视区，滚动跟随
        if (g_gpCursor >= 0) {
            int maxScroll = g_visibleCount - VISIBLE_ROWS;
            if (maxScroll < 0) maxScroll = 0;
            if (g_gpCursor < g_scrollTop) g_scrollTop = g_gpCursor;
            if (g_gpCursor > g_scrollTop + VISIBLE_ROWS - 1) g_scrollTop = g_gpCursor - (VISIBLE_ROWS - 1);
            if (g_scrollTop > maxScroll) g_scrollTop = maxScroll;
        }
        InvalidateRect(g_panel, nullptr, FALSE);
        return;
    }

    // 手柄 D-pad 左/右：切换行内焦点（改键区 ↔ 开关）
    if (left || right) {
        g_gpNeedsRelease = true;
        int i = (g_gpCursor >= 0) ? ArrayIdxOfDisplay(g_gpCursor) : -1;
        if (i >= 0 && g_mods[i].hotkeyCount > 0 && !g_mods[i].isSelf &&
            (!g_mods[i].thirdParty || g_mods[i].rekeyable)) {
            g_gpFocus = 1 - g_gpFocus;
            InvalidateRect(g_panel, nullptr, FALSE);
        }
        return;
    }

    if (confirm) {
        g_gpNeedsRelease = true;
        if (g_gpCursor < 0) { g_gpCursor = 0; g_gpFocus = 0; InvalidateRect(g_panel, nullptr, FALSE); return; }
        int i = ArrayIdxOfDisplay(g_gpCursor);
        if (i < 0) return;
        // 行内焦点：0=改键（有热键则进录制，否则切换开关） 1=开关
        if (g_gpFocus == 0 && !g_inSave && g_mods[i].hotkeyCount > 0 &&
            !g_mods[i].isSelf &&
            (!g_mods[i].thirdParty || g_mods[i].rekeyable) && g_recModIdx < 0) {
            BeginRecordKey(i, 0);
            InvalidateRect(g_panel, nullptr, FALSE);
        } else {
            ToggleMod(i);
        }
        return;
    }
}

// ============================================================
// 路径工具
// ============================================================
static std::wstring GetGameDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p.resize(pos);
    return p;
}

// ============================================================
// 版本比较：从文件夹名提取版本后缀进行比较
// 返回值 >0 表示 a 比 b 新，<0 表示 a 比 b 旧，0 表示相同
// 比较规则：逐段数字比较，无数字的按字符串比较（diag 等后缀视为更新）
// ============================================================
static int CompareVersion(const wchar_t* a, const wchar_t* b) {
    // 跳过前缀部分，从第一个 '_' 之后开始比较
    const wchar_t* va = wcschr(a, L'_');
    const wchar_t* vb = wcschr(b, L'_');
    va = va ? va + 1 : a;
    vb = vb ? vb + 1 : b;
    // 逐段比较：数字段按数值比较，非数字段按字符串比较
    while (*va && *vb) {
        // 跳过前导 'v'
        while (*va == L'v' || *va == L'V') va++;
        while (*vb == L'v' || *vb == L'V') vb++;
        if (*va >= L'0' && *va <= L'9' && *vb >= L'0' && *vb <= L'9') {
            // 数字段：解析为整数后比较
            int na = 0, nb = 0;
            while (*va >= L'0' && *va <= L'9') { na = na * 10 + (*va - L'0'); va++; }
            while (*vb >= L'0' && *vb <= L'9') { nb = nb * 10 + (*vb - L'0'); vb++; }
            if (na != nb) return na - nb;
        } else {
            // 非数字段：逐字符比较（不区分大小写）
            wchar_t ca = (wchar_t)towlower(*va);
            wchar_t cb = (wchar_t)towlower(*vb);
            if (ca != cb) return (int)ca - (int)cb;
            va++; vb++;
        }
        // 跳过分隔符（. - _ 等）
        if ((*va == L'.' || *va == L'-' || *va == L'_') &&
            (*vb == L'.' || *vb == L'-' || *vb == L'_')) {
            va++; vb++;
        } else if (*va == L'.') va++;
        else if (*vb == L'.') vb++;
    }
    // 剩余部分长的为新版
    if (*va && !*vb) return 1;
    if (!*va && *vb) return -1;
    return 0;
}

// ============================================================
// 递归删除目录（Win32 API 实现，替代 _wsystem("rmdir /s /q")）
// ============================================================
static BOOL RemoveDirectoryRecursive(const std::wstring& path) {
    // 先清空属性（可能有只读文件）
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);

    // 枚举并递归删除子项
    std::wstring search = path + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return FALSE;

    BOOL ok = TRUE;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        std::wstring full = path + L"\\" + fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // 递归删除子目录
            if (!RemoveDirectoryRecursive(full)) ok = FALSE;
        } else {
            SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(full.c_str())) ok = FALSE;
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    // 删除空目录本身
    if (!RemoveDirectoryW(path.c_str())) ok = FALSE;
    return ok;
}
// ============================================================
// 自动清理旧版本：扫描三个目录，同前缀按配额保留备用
// 规则：
//   1. 优先级：Mods > 待用MOD > 弃用MOD
//   2. Mods 中的活跃版本优先级最高；Mods 中并存的旧版本移到弃用MOD（防双加载）
//   3. v1.2.11（用户拍板 2026-10-05）：待用MOD 保留最新 2 份备用，其余移到弃用MOD
//      （旧规则：只留活跃版 1 份且清空待用MOD同前缀全部，回退无从可取）
//   4. _pre_ 临时备份不占备用名额，一律移到弃用MOD
// ============================================================
static void CleanupStaleFolders() {
    // 收集三个目录中所有文件夹，按前缀分组
    struct FolderEntry {
        std::wstring name;
        std::wstring fullDir;   // 所在目录
        int dirPriority;        // 0=Mods, 1=待用MOD, 2=弃用MOD
    };

    struct FindData {
        const wchar_t* dir;
        int priority;
    };
    FindData searchDirs[3] = {
        { g_modsDir.c_str(), 0 },
        { g_storageDir.c_str(), 1 },
        { nullptr, 2 }  // 弃用MOD 后面填
    };

    // 前缀 → 文件夹列表
    struct PrefixGroup {
        const wchar_t* prefix;
        std::vector<FolderEntry> entries;
    };
    std::vector<PrefixGroup> groups;

    // 初始化所有模板前缀
    for (int i = 0; i < g_modCount; ++i) {
        PrefixGroup g;
        g.prefix = g_mods[i].folderPrefix;
        if (!g.prefix || !*g.prefix) continue;
        groups.push_back(std::move(g));
    }

    // 扫描三个目录
    for (int d = 0; d < 3; ++d) {
        if (!searchDirs[d].dir) continue;
        std::wstring search = std::wstring(searchDirs[d].dir) + L"\\*";
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW(search.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == L'.') continue;

            // 匹配到哪个前缀
            for (auto& g : groups) {
                size_t preLen = wcslen(g.prefix);
                if (_wcsnicmp(fd.cFileName, g.prefix, preLen) != 0) continue;
                if (fd.cFileName[preLen] != 0 && fd.cFileName[preLen] != L'_') continue;

                FolderEntry e;
                e.name = fd.cFileName;
                e.fullDir = searchDirs[d].dir;
                e.dirPriority = searchDirs[d].priority;
                g.entries.push_back(std::move(e));
                break;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    int cleanedCount = 0;
    for (auto& g : groups) {
        if (g.entries.empty()) continue;

        // 按优先级排序：Mods(0) 优先于 待用MOD(1) 优先于 弃用MOD(2)
        // 同优先级内按版本号降序（最新在前）
        std::sort(g.entries.begin(), g.entries.end(), [](const FolderEntry& a, const FolderEntry& b) {
            if (a.dirPriority != b.dirPriority)
                return a.dirPriority < b.dirPriority;
            return CompareVersion(a.name.c_str(), b.name.c_str()) > 0;
        });

        // v1.2.11: 备用配额保留策略
        //   - Mods：保留最新 1 份（活跃版）；并存的旧版本移到弃用MOD（防双加载）
        //   - 待用MOD：保留最新 2 份备用（用户拍板：至少保留两个版本备用）
        //   - _pre_ 临时备份不占配额，一律移到弃用MOD
        bool modsKept = false;   // Mods 活跃版是否已保留
        int  storageKept = 0;    // 待用MOD 已保留备用份数

        for (size_t k = 0; k < g.entries.size(); ++k) {
            FolderEntry& e = g.entries[k];

            // 弃用MOD 中的重复直接跳过（已经在弃用目录了）
            if (e.dirPriority == 2) continue;

            // _pre_ 临时备份：不占备用配额，一律移到弃用MOD
            if (e.name.find(L"_pre_") == std::wstring::npos) {
                if (e.dirPriority == 0) {
                    if (!modsKept) { modsKept = true; continue; }  // 保留 Mods 活跃版
                } else {
                    if (storageKept < 2) { storageKept++; continue; }  // 保留待用MOD 最新 2 份备用
                }
            }

            // 移到 弃用MOD
            std::wstring src = e.fullDir + L"\\" + e.name;
            std::wstring dst = g_abandonDir + L"\\" + e.name;

            // 目标已存在则跳过
            if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
                Log("[ModManager] cleanup: %ls already in abandon, removing from %ls\n",
                    e.name.c_str(), e.dirPriority == 0 ? L"Mods" : L"\x5F85\x7528MOD");
                // 从原目录删除（弃用MOD 已有一份）
                // 用 Win32 API 递归删除（替代 _wsystem 避免命令注入风险）
                RemoveDirectoryRecursive(src);

            } else {
                if (MoveFileW(src.c_str(), dst.c_str())) {
                    Log("[ModManager] cleanup: %ls → abandon (was in %ls)\n",
                        e.name.c_str(), e.dirPriority == 0 ? L"Mods" : L"\x5F85\x7528MOD");
                    cleanedCount++;
                } else {
                    Log("[ModManager] cleanup: FAILED to move %ls (err=%lu)\n",
                        e.name.c_str(), GetLastError());
                }
            }
        }
    }

    if (cleanedCount > 0)
        Log("[ModManager] cleanup: %d stale folders moved to abandon\n", cleanedCount);
}

// ============================================================
// 运行时扫描目录：为每个 MOD 模板自动匹配实际文件夹 + 提取版本号
// 匹配规则：文件夹名以 folderPrefix 开头，且下一个字符是 '_' 或字符串结束
// 版本提取：第一个 '_' 之后的内容（如 v1.2.2、v0.3.2_diag、gloaming）
// ============================================================
static void ScanModFolders() {
    for (int i = 0; i < g_modCount; ++i) {
        g_mods[i].folder[0]  = 0;
        g_mods[i].version[0] = 0;

        const wchar_t* prefix = g_mods[i].folderPrefix;
        if (!prefix || !*prefix) continue;

        // 先扫 mods 目录，再扫 待用MOD 目录
        const wchar_t* dirs[2] = { g_modsDir.c_str(), g_storageDir.c_str() };
        for (int d = 0; d < 2; ++d) {
            std::wstring search = std::wstring(dirs[d]) + L"\\*";
            WIN32_FIND_DATAW fd = {};
            HANDLE h = FindFirstFileW(search.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == L'.') continue;

                size_t preLen = wcslen(prefix);
                if (_wcsnicmp(fd.cFileName, prefix, preLen) != 0) continue;
                // 下一个字符必须是 '_' 或字符串结束
                if (fd.cFileName[preLen] != 0 && fd.cFileName[preLen] != L'_') continue;

                // 匹配成功
                wcsncpy_s(g_mods[i].folder, MAX_PATH, fd.cFileName, _TRUNCATE);

                // 提取版本：第一个 '_' 之后内容
                const wchar_t* us = wcschr(fd.cFileName, L'_');
                if (us && us[1]) {
                    wcsncpy_s(g_mods[i].version, 64, us + 1, _TRUNCATE);
                }

                Log("[ModManager] auto: %ls → folder=[%ls] version=[%ls] (in %ls)\n",
                    prefix, g_mods[i].folder, g_mods[i].version,
                    d == 0 ? L"Mods" : L"\x5F85\x7528MOD");
                break;
            } while (FindNextFileW(h, &fd));
            FindClose(h);

            if (g_mods[i].folder[0]) break;
        }

        if (!g_mods[i].folder[0]) {
            Log("[ModManager] auto: %ls → NOT FOUND\n", prefix);
        }
    }
}

// ============================================================
// 目录扫描：确定每个 MOD 的开关状态 + 按启用排序
// ============================================================
static void ScanToggleState() {
    for (int i = 0; i < g_modCount; ++i) {
        if (g_mods[i].isSelf) { g_mods[i].toggle = ToggleState::Self; continue; }
        if (!g_mods[i].folder[0]) { g_mods[i].toggle = ToggleState::Missing; continue; }

        std::wstring modPath = g_modsDir + L"\\" + g_mods[i].folder;
        std::wstring altPath  = g_storageDir + L"\\" + g_mods[i].folder;

        DWORD m = GetFileAttributesW(modPath.c_str());
        DWORD a = GetFileAttributesW(altPath.c_str());
        bool inMods   = (m != INVALID_FILE_ATTRIBUTES && (m & FILE_ATTRIBUTE_DIRECTORY));
        bool inAlt    = (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY));

        if (inMods)      g_mods[i].toggle = ToggleState::Enabled;
        else if (inAlt)  g_mods[i].toggle = ToggleState::Disabled;
        else             g_mods[i].toggle = ToggleState::Missing;
    }

    // 重排 displayIdx：已启用（含自身）在前，停用在后，按原顺序稳定排列
    // v1.2.7: 目录不存在的 MOD（Missing，如已剔除/移出的 ProductionAuto 等）
    //         直接隐藏——displayIdx=-1 不占行，渲染/点击/手柄自然跳过
    int next = 0;
    for (int i = 0; i < g_modCount; ++i)
        if (g_mods[i].toggle == ToggleState::Enabled ||
            g_mods[i].toggle == ToggleState::Self)
            g_mods[i].displayIdx = next++;
    for (int i = 0; i < g_modCount; ++i) {
        if (g_mods[i].toggle == ToggleState::Enabled ||
            g_mods[i].toggle == ToggleState::Self) continue;
        if (g_mods[i].toggle == ToggleState::Missing)
            g_mods[i].displayIdx = -1;   // 隐藏：目录不存在
        else
            g_mods[i].displayIdx = next++;
    }
    g_visibleCount = next;

    Log("[ModManager] toggle scan + sort done, %d mods (%d visible)\n",
        g_modCount, g_visibleCount);
}

// ============================================================
// 读取文件全部内容到字符串
// ============================================================
static std::string ReadFileContent(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return "";
    std::string content;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) content += buf;
    fclose(f);
    return content;
}

// 前向声明
static void ParseHotkeys(ModEntry& mod);

// ============================================================
// 第三方 MOD 热键自动扫描
// 
// 优先级：qol_meta.txt > README 扫描 > 缓存
// qol_meta.txt 格式（UTF-8，放在 MOD 目录内）：
//   # 注释行
//   [hotkey]
//   1=F6|传送菜单
//   2=F7|HUD显示
//   rekeyable=true
//   maxkeys=6
//
// README 扫描：搜索 "F\d+" 和 "按键" "热键" "快捷键" 等关键词
// 缓存：modmanager_hotkey_cache.txt（游戏根目录），按文件夹名缓存
// ============================================================

// ---- 缓存文件路径 ----
static std::wstring g_hotkeyCachePath;

// ---- 已扫描过的文件夹集合（本次运行内不重复扫描） ----
static bool g_thirdPartyScanDone = false;

// ============================================================
// 读取缓存文件
// 格式：每行 folder|hotkey|rekeyable
// 如：MineHelper_v1.1.1|F9|0
// ============================================================
static std::string ReadHotkeyCache() {
    return ReadFileContent(g_hotkeyCachePath);
}

// ============================================================
// 写入缓存文件（全量重写）
// ============================================================
static void WriteHotkeyCache(const std::string& content) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_hotkeyCachePath.c_str(), L"wb") != 0 || !f) return;
    fwrite(content.c_str(), 1, content.size(), f);
    fclose(f);
}

// ============================================================
// 从缓存中查找指定文件夹的热键
// 返回：找到则填充 hotkey/rekeyable，返回 true
// ============================================================
static bool FindInCache(const std::string& cache, const std::string& folder,
                        std::string& outHotkey, bool& outRekeyable) {
    std::string line;
    size_t pos = 0;
    while (pos < cache.size()) {
        size_t eol = cache.find('\n', pos);
        if (eol == std::string::npos) eol = cache.size();
        line = cache.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = eol + 1;
        
        // 解析 folder|hotkey|rekeyable
        size_t p1 = line.find('|');
        if (p1 == std::string::npos) continue;
        std::string fol = line.substr(0, p1);
        if (fol != folder) continue;
        
        size_t p2 = line.find('|', p1 + 1);
        if (p2 == std::string::npos) {
            outHotkey = line.substr(p1 + 1);
            outRekeyable = false;
        } else {
            outHotkey = line.substr(p1 + 1, p2 - p1 - 1);
            outRekeyable = (line.substr(p2 + 1) == "1");
        }
        return true;
    }
    return false;
}

// ============================================================
// 生成缓存行
// ============================================================
static std::string MakeCacheLine(const std::string& folder,
                                  const std::string& hotkey, bool rekeyable) {
    return folder + "|" + (hotkey.empty() ? "none" : hotkey) + "|" + (rekeyable ? "1" : "0") + "\n";
}

// ============================================================
// 更新缓存中某文件夹的条目（有则替换，无则追加）
// ============================================================
static void UpdateCacheEntry(std::string& cache, const std::string& folder,
                              const std::string& hotkey, bool rekeyable) {
    std::string newLine = MakeCacheLine(folder, hotkey, rekeyable);
    // 查找并替换
    std::string prefix = folder + "|";
    size_t pos = 0;
    while (pos < cache.size()) {
        size_t eol = cache.find('\n', pos);
        if (eol == std::string::npos) eol = cache.size();
        std::string line = cache.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.substr(0, prefix.size()) == prefix) {
            // 找到匹配行，替换
            cache.replace(pos, eol - pos + 1, newLine);
            return;
        }
        pos = eol + 1;
    }
    // 未找到，追加
    if (!cache.empty() && cache.back() != '\n') cache += '\n';
    cache += newLine;
}

// ============================================================
// 读取 qol_meta.txt（未来 MOD 标准）
// 格式见上方注释
// 填充 mod.hotkey / mod.rekeyable / mod.hotkeyNames
// ============================================================
static bool ParseQolMeta(ModEntry& mod, const std::wstring& modDir) {
    std::wstring metaPath = modDir + L"\\qol_meta.txt";
    std::string content = ReadFileContent(metaPath);
    if (content.empty()) return false;
    
    bool found = false;
    bool rekeyable = false;
    int maxKeys = 6;
    std::string hotkeys[6];
    std::string hotkeyNames[6];
    int count = 0;
    
    // 逐行解析
    size_t pos = 0;
    while (pos < content.size() && count < maxKeys) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) eol = content.size();
        std::string line = content.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = eol + 1;
        
        // 跳过空行和注释
        if (line.empty() || line[0] == '#') continue;
        
        // rekeyable=true/false
        if (line == "rekeyable=true") { rekeyable = true; found = true; continue; }
        if (line == "rekeyable=false") { rekeyable = false; found = true; continue; }
        
        // maxkeys=N
        if (line.substr(0, 8) == "maxkeys=") {
            maxKeys = atoi(line.c_str() + 8);
            if (maxKeys < 1) maxKeys = 1;
            if (maxKeys > 6) maxKeys = 6;
            found = true;
            continue;
        }
        
        // [hotkey] section header
        if (line == "[hotkey]") { found = true; continue; }
        
        // 1=F6|功能名  格式
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string keyPart = line.substr(eq + 1);
        
        // 提取功能名（| 分隔）
        std::string keyStr, nameStr;
        size_t bar = keyPart.find('|');
        if (bar != std::string::npos) {
            keyStr = keyPart.substr(0, bar);
            nameStr = keyPart.substr(bar + 1);
        } else {
            keyStr = keyPart;
        }
        
        // 去除前后空格
        while (!keyStr.empty() && keyStr.back() == ' ') keyStr.pop_back();
        while (!keyStr.empty() && keyStr.front() == ' ') keyStr.erase(0, 1);
        
        if (!keyStr.empty() && keyStr != "none" && count < maxKeys) {
            hotkeys[count] = keyStr;
            hotkeyNames[count] = nameStr;
            count++;
            found = true;
        }
    }
    
    if (!found || count == 0) return false;
    
    // 拼接热键字符串
    std::string joined;
    for (int i = 0; i < count; ++i) {
        if (i > 0) joined += ", ";
        joined += hotkeys[i];
    }
    
    // v1.1.2: 改用动态分配避免 static 缓冲区轮转覆盖（旧 16 槽轮转在 >16 次
    // 调用后会覆盖早期值，导致 hotkey 指针指向被覆盖的字符串）
    wchar_t* buf = new wchar_t[128];
    MultiByteToWideChar(CP_UTF8, 0, joined.c_str(), -1, buf, 128);
    mod.hotkey = buf;
    
    // 写入功能名
    for (int i = 0; i < count && i < 6; ++i) {
        MultiByteToWideChar(CP_UTF8, 0, hotkeyNames[i].c_str(), -1,
                           mod.hotkeyNames[i], 32);
    }
    
    mod.rekeyable = rekeyable;
    Log("[ModManager] qol_meta.txt: %ls -> %hs (rekeyable=%d, count=%d)\n",
        mod.folder, joined.c_str(), rekeyable, count);
    return true;
}

// ============================================================
// 从 README/说明文件中扫描热键
// 策略：搜索 F1~F24、数字键、PageUp/Down 等 VK 名称
// 只提取，不做语义解析
// ============================================================
static std::string ScanReadmeForHotkeys(const std::string& content) {
    // 常见热键模式
    static const char* patterns[] = {
        // F1~F24
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9",
        "F10", "F11", "F12", "F13", "F14", "F15", "F16",
        // PageUp/PageDown
        "PageUp", "PageDown", "PgUp", "PgDn",
        // 其他
        "Home", "End", "Insert", "Delete",
        nullptr
    };
    
    std::string found;
    int count = 0;
    
    // 转小写搜索
    std::string lower;
    lower.reserve(content.size());
    for (char c : content) lower += (char)tolower(c);
    
    for (int i = 0; patterns[i] && count < 6; ++i) {
        std::string pat = patterns[i];
        std::string patLower;
        for (char c : pat) patLower += (char)tolower(c);
        
        // 在全文中搜索
        size_t pos = lower.find(patLower);
        if (pos == std::string::npos) continue;
        
        // 验证：F1 后面不能跟数字（否则可能是 F10~F24 的子串）
        if (pat[0] == 'F' && pat.size() == 2) {
            // 检查后面是否还有数字
            size_t nextPos = pos + patLower.size();
            if (nextPos < lower.size() && isdigit((unsigned char)lower[nextPos]))
                continue;  // 是 F10+ 的子串，跳过
        }
        // 验证：前面不能是字母（避免 "PageF6" 之类）
        if (pos > 0) {
            char prev = lower[pos - 1];
            if (isalnum((unsigned char)prev) || prev == '_')
                continue;
        }
        
        // 去重
        bool dup = false;
        size_t fp = 0;
        while (fp < found.size()) {
            size_t fe = found.find(',', fp);
            std::string existing = (fe == std::string::npos) ?
                found.substr(fp) : found.substr(fp, fe - fp);
            while (!existing.empty() && existing.front() == ' ') existing.erase(0, 1);
            if (existing == pat) { dup = true; break; }
            if (fe == std::string::npos) break;
            fp = fe + 1;
        }
        if (!dup) {
            if (count > 0) found += ", ";
            found += pat;
            count++;
        }
    }
    
    return found;
}

// ============================================================
// 扫描第三方 MOD 的热键
// 对每个 thirdParty=true 的 MOD：
//   1. 查缓存（按 folder 名匹配）
//   2. 缓存 miss → 读 qol_meta.txt
//   3. 无 qol_meta.txt → 扫描 README/说明文件
//   4. 更新缓存
// ============================================================
static void ScanThirdPartyHotkeys() {
    if (g_thirdPartyScanDone) return;
    g_thirdPartyScanDone = true;
    
    std::string cache = ReadHotkeyCache();
    bool cacheChanged = false;
    
    for (int i = 0; i < g_modCount; ++i) {
        if (!g_mods[i].thirdParty) continue;
        if (!g_mods[i].folder[0]) continue;  // 未找到目录
        
        // 确定实际目录路径
        std::wstring modDir;
        std::wstring modsPath = g_modsDir + L"\\" + g_mods[i].folder;
        std::wstring altPath  = g_storageDir + L"\\" + g_mods[i].folder;
        if (GetFileAttributesW(modsPath.c_str()) != INVALID_FILE_ATTRIBUTES)
            modDir = modsPath;
        else if (GetFileAttributesW(altPath.c_str()) != INVALID_FILE_ATTRIBUTES)
            modDir = altPath;
        if (modDir.empty()) continue;
        
        // 文件夹名（含版本号）作为缓存 key
        char folderNarrow[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, g_mods[i].folder, -1,
                            folderNarrow, sizeof(folderNarrow), nullptr, nullptr);
        std::string folderKey(folderNarrow);
        
        // 1. 查缓存
        std::string cachedHotkey;
        bool cachedRekeyable = false;
        if (FindInCache(cache, folderKey, cachedHotkey, cachedRekeyable)) {
            if (cachedHotkey == "none" || cachedHotkey.empty()) {
                g_mods[i].hotkey = nullptr;
                g_mods[i].rekeyable = cachedRekeyable;
                Log("[ModManager] third-party hotkey (cached none): %hs\n", folderKey.c_str());
            } else {
                // v1.1.2: 改用动态分配避免 static 缓冲区轮转覆盖
                wchar_t* buf = new wchar_t[128];
                MultiByteToWideChar(CP_UTF8, 0, cachedHotkey.c_str(), -1, buf, 128);
                g_mods[i].hotkey = buf;
                g_mods[i].rekeyable = cachedRekeyable;
                Log("[ModManager] third-party hotkey (cached): %hs -> %hs\n",
                    folderKey.c_str(), cachedHotkey.c_str());
            }
            continue;
        }
        
        // 2. 尝试 qol_meta.txt
        if (ParseQolMeta(g_mods[i], modDir)) {
            // 有 qol_meta.txt，已填充
            std::string hotkeyNarrow;
            if (g_mods[i].hotkey) {
                char hb[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, g_mods[i].hotkey, -1, hb, sizeof(hb), nullptr, nullptr);
                hotkeyNarrow = hb;
            }
            UpdateCacheEntry(cache, folderKey, hotkeyNarrow, g_mods[i].rekeyable);
            cacheChanged = true;
            continue;
        }
        
        // 3. 扫描 README / 说明文件
        std::string readmeContent;
        // 尝试常见文件名
        const wchar_t* readmeNames[] = {
            L"README.md", L"readme.md", L"README.txt", L"readme.txt",
            L"\\x8BF4\\x660E.txt", L"\\x8BF4\\x660E.md",
            nullptr
        };
        for (int r = 0; readmeNames[r]; ++r) {
            std::wstring rPath = modDir + L"\\" + readmeNames[r];
            std::string rContent = ReadFileContent(rPath);
            if (!rContent.empty()) {
                readmeContent = rContent;
                break;
            }
        }
        
        std::string scannedHotkey;
        if (!readmeContent.empty()) {
            scannedHotkey = ScanReadmeForHotkeys(readmeContent);
        }
        
        // 4. 填充并更新缓存
        if (scannedHotkey.empty()) {
            g_mods[i].hotkey = nullptr;
            g_mods[i].rekeyable = false;
            UpdateCacheEntry(cache, folderKey, "none", false);
            Log("[ModManager] third-party hotkey (none): %hs\n", folderKey.c_str());
        } else {
            // v1.1.2: 改用动态分配避免 static 缓冲区轮转覆盖
            wchar_t* buf = new wchar_t[128];
            MultiByteToWideChar(CP_UTF8, 0, scannedHotkey.c_str(), -1, buf, 128);
            g_mods[i].hotkey = buf;
            g_mods[i].rekeyable = false;  // README 扫描出的热键不可改
            Log("[ModManager] third-party hotkey (scanned): %hs -> %hs\n",
                folderKey.c_str(), scannedHotkey.c_str());
            UpdateCacheEntry(cache, folderKey, scannedHotkey, false);
        }
        cacheChanged = true;
    }
    
    // 写回缓存
    if (cacheChanged) {
        WriteHotkeyCache(cache);
        Log("[ModManager] hotkey cache updated\n");
    }
}

// ============================================================
// 自动读取热键：解析 qol_hotkeys.txt（feature=key 每行一个），
// 按 dllName 匹配覆盖注册表 hotkey
// 支持多按键：key 字段可用逗号分隔（如 "4, D-pad Up"）
// ============================================================
static void ApplyHotkeysFile() {
    std::wstring path = g_gameDir + L"\\qol_hotkeys.txt";
    std::string content = ReadFileContent(path);
    if (content.empty()) return;

    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) eol = content.size();
        std::string line = content.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = eol + 1;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string feature = line.substr(0, eq);
        std::string key     = line.substr(eq + 1);
        if (feature.empty()) continue;
        bool noKey = (key.empty() || key == "none");  // 显式无热键

        // 匹配注册表（按 dllName 前缀，如 "chestsort" ↔ "chestsort.dll"）
        for (int i = 0; i < g_modCount; ++i) {
            if (!g_mods[i].dllName) continue;
            char dllNarrow[MAX_PATH] = {};
            WideCharToMultiByte(CP_ACP, 0, g_mods[i].dllName, -1,
                                dllNarrow, sizeof(dllNarrow), nullptr, nullptr);
            std::string dll(dllNarrow);
            size_t dot = dll.find('.');
            std::string base = (dot == std::string::npos) ? dll : dll.substr(0, dot);
            if (base != feature) continue;

            // 更新 hotkey（用静态缓冲区存字符串）
            // 第三方 MOD 热键由 ScanThirdPartyHotkeys 在扫描阶段填充，不在此覆盖
            if (g_mods[i].thirdParty) {
                break;
            }
            // v1.1.2: 改用动态分配避免 static 缓冲区轮转覆盖
            wchar_t* buf = new wchar_t[128];
            if (!noKey)
                MultiByteToWideChar(CP_ACP, 0, key.c_str(), -1, buf, 128);
            g_mods[i].hotkey = noKey ? nullptr : buf;
            Log("[ModManager] hotkey auto: %s = %s\n", feature.c_str(),
                noKey ? "(none)" : key.c_str());
            break;
        }
    }

    // 为所有 MOD 解析 hotkey 字符串到 hotkeys 数组
    for (int i = 0; i < g_modCount; ++i) {
        ParseHotkeys(g_mods[i]);
    }
}

// ============================================================
// 扫描 MOD 加载状态
// ============================================================
static void ScanModStatus() {
    // v1.2.7: 每次扫描先重扫 mods/待用MOD 目录（folder/version 实时刷新）
    ScanModFolders();
    // 先确定开关位置 + 排序
    ScanToggleState();
    // 扫描第三方 MOD 热键（README/qol_meta.txt → 缓存）
    ScanThirdPartyHotkeys();
    // 自动读取热键注册文件
    ApplyHotkeysFile();

    // 1. 解析 steam_proxy.log 获取加载顺序
    std::wstring proxyLogPath = g_gameDir + L"\\steam_proxy.log";
    std::string proxyLog = ReadFileContent(proxyLogPath);
    std::string proxyLogLower;
    if (!proxyLog.empty()) {
        proxyLogLower.reserve(proxyLog.size());
        for (char c : proxyLog) proxyLogLower += (char)tolower(c);
    }

    for (int i = 0; i < g_modCount; ++i) {
        if (g_mods[i].isSelf) {
            g_mods[i].status = ModStatus::Loaded;
            g_mods[i].loadOrder = g_modCount;
            continue;
        }
        // 未启用（待用MOD）→ 直接 NotLoaded
        if (g_mods[i].toggle != ToggleState::Enabled) {
            g_mods[i].status = ModStatus::NotLoaded;
            g_mods[i].loadOrder = -1;
            continue;
        }
        if (!g_mods[i].dllName) { // 数据 MOD 无 DLL，算已加载
            g_mods[i].status = ModStatus::Loaded;
            g_mods[i].loadOrder = i;
            continue;
        }

        char dllNarrow[MAX_PATH] = {};
        WideCharToMultiByte(CP_ACP, 0, g_mods[i].dllName, -1,
                            dllNarrow, sizeof(dllNarrow), nullptr, nullptr);
        for (char* c = dllNarrow; *c; ++c) *c = (char)tolower(*c);

        bool found = !proxyLogLower.empty() && dllNarrow[0] &&
                     proxyLogLower.find(dllNarrow) != std::string::npos;
        if (found) { g_mods[i].status = ModStatus::Loaded; g_mods[i].loadOrder = i; }
        else       { g_mods[i].status = ModStatus::NotLoaded; g_mods[i].loadOrder = -1; }
    }

    // 2. 解析各 MOD 日志，更新精确状态
    for (int i = 0; i < g_modCount; ++i) {
        if (!g_mods[i].logFile) continue;
        if (g_mods[i].toggle != ToggleState::Enabled) continue;

        std::wstring logPath = g_gameDir + L"\\" + g_mods[i].logFile;
        std::string content = ReadFileContent(logPath);
        if (content.empty()) continue;

        if (content.find("unsupported exe") != std::string::npos ||
            content.find("feature disabled safely") != std::string::npos) {
            g_mods[i].status = ModStatus::Unsupported;
            continue;
        }

        bool hasError = false;
        const char* errorMarkers[] = { "FAILED", "ERROR", "crash", "assert", nullptr };
        for (int j = 0; errorMarkers[j]; ++j)
            if (content.find(errorMarkers[j]) != std::string::npos) { hasError = true; break; }

        if (hasError) {
            size_t searchFrom = 0;
            bool realError = false;
            while (true) {
                size_t pos = std::string::npos;
                for (int j = 0; errorMarkers[j]; ++j) {
                    size_t p = content.find(errorMarkers[j], searchFrom);
                    if (p != std::string::npos && (pos == std::string::npos || p < pos)) pos = p;
                }
                if (pos == std::string::npos) break;
                size_t lineStart = content.rfind('\n', pos);
                lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
                size_t lineEnd = content.find('\n', pos);
                if (lineEnd == std::string::npos) lineEnd = content.length();
                std::string line = content.substr(lineStart, lineEnd - lineStart);
                bool isBenign = (line.find("0 ") != std::string::npos &&
                                line.find("error") != std::string::npos) ||
                               line.find("no error") != std::string::npos ||
                               line.find("No error") != std::string::npos ||
                               line.find("0 errors") != std::string::npos;
                if (!isBenign) { realError = true; break; }
                searchFrom = lineEnd;
            }
            if (realError) { g_mods[i].status = ModStatus::Error; continue; }
        }
        g_mods[i].status = ModStatus::Loaded;
    }

    // 预计算热键冲突缓存
    for (int i = 0; i < g_modCount && i < 32; ++i) {
        g_conflictCache[i] = -1;  // 标记需要重新计算
    }
    g_conflictCacheValid = false;
    Log("[ModManager] scan complete, %d mods tracked\n", g_modCount);
}

// ============================================================
// 解析热键字符串到 hotkeys 数组
// 输入："4, D-pad Up" 或 "9" 或 nullptr
// ============================================================
static void ParseHotkeys(ModEntry& mod) {
    mod.hotkeyCount = 0;
    if (!mod.hotkey) return;
    // 复制到临时缓冲区解析
    wchar_t tmp[128] = {};
    wcsncpy_s(tmp, mod.hotkey, _TRUNCATE);
    wchar_t* ctx = nullptr;
    wchar_t* token = wcstok_s(tmp, L",", &ctx);
    while (token && mod.hotkeyCount < 6) {
        // 去除前后空格
        while (*token == L' ') token++;
        wchar_t* end = token + wcslen(token);
        while (end > token && *(end - 1) == L' ') *--end = 0;
        if (*token) {
            wcsncpy_s(mod.hotkeys[mod.hotkeyCount], token, _TRUNCATE);
            mod.hotkeyCount++;
        }
        token = wcstok_s(nullptr, L",", &ctx);
    }
}

// ============================================================
// 按键冲突检测：逐键比较，任一按键与其他 MOD 重复即冲突
// 返回冲突按键索引（-1 = 无冲突）
// ============================================================
static int FindConflictingHotkey(int index) {
    if (g_mods[index].hotkeyCount == 0) return -1;
    // 检查缓存
    if (g_conflictCacheValid && index < 32 && g_conflictCache[index] >= -1) {
        if (g_conflictCache[index] == -2) return -1;  // 无冲突
        if (g_conflictCache[index] >= 0) return g_conflictCache[index];  // 冲突键索引
    }
    for (int h = 0; h < g_mods[index].hotkeyCount; ++h) {
        for (int i = 0; i < g_modCount; ++i) {
            if (i == index) continue;
            for (int h2 = 0; h2 < g_mods[i].hotkeyCount; ++h2) {
                if (wcscmp(g_mods[index].hotkeys[h], g_mods[i].hotkeys[h2]) == 0)
                    return h;
            }
        }
    }
    // 存缓存
    if (index < 32) { g_conflictCache[index] = -2; }
    return -1;
}

// ============================================================
// 依赖检测：该 MOD 声明的依赖 MOD 是否已启用
// ============================================================
static const wchar_t* MissingDependency(int index) {
    if (!g_mods[index].dependsOn) return nullptr;
    for (int i = 0; i < g_modCount; ++i) {
        if (i == index) continue;
        if (g_mods[i].name &&
            wcscmp(g_mods[i].name, g_mods[index].dependsOn) == 0 &&
            g_mods[i].toggle == ToggleState::Enabled) {
            return nullptr; // 依赖已启用
        }
    }
    return g_mods[index].dependsOn; // 依赖缺失
}

// ============================================================
// 执行开关切换（移动文件夹）
// ============================================================
static bool SetModEnabled(int index, bool enable) {
    if (index < 0 || index >= g_modCount) return false;
    if (g_mods[index].isSelf) return false; // 不可停用自身
    if (g_mods[index].toggle == ToggleState::Missing) return false;

    ToggleState want = enable ? ToggleState::Enabled : ToggleState::Disabled;
    if (g_mods[index].toggle == want) return true; // 已在目标状态

    const std::wstring src  = (enable ? g_storageDir : g_modsDir) + L"\\" + g_mods[index].folder;
    const std::wstring dest = (enable ? g_modsDir : g_storageDir) + L"\\" + g_mods[index].folder;

    // v1.2.15: toggle 同步落盘日志——异步日志队列在进程非正常退出（卡死被结束）
    // 会丢日志，2026-10-06 用户关闭 MOD 后游戏卡死时零日志证据。关键操作直接
    // 同步追加到独立小文件，卡死时证据已在盘上。
    {
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        wchar_t diagLine[512] = {};
        _snwprintf_s(diagLine, _TRUNCATE,
                     L"[%04d-%02d-%02d %02d:%02d:%02d] toggle %s %ls -> %ls\r\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                     st.wSecond, enable ? L"ENABLE " : L"DISABLE",
                     src.c_str(), dest.c_str());
        FILE* diagLog = _wfopen(
            (g_gameDir + L"\\qol_modmanager_toggle.log").c_str(), L"a");
        if (diagLog) {
            fwprintf(diagLog, L"%ls", diagLine);
            fclose(diagLog);
        }
    }

    // v1.2.15: 目标目录存在时改为带时间戳改名保留（旧逻辑无条件
    // RemoveDirectoryW——仅空目录可删、失败不检查、且有无警告删除数据风险；
    // 非空 dest 时 MoveFileExW 对目录会失败）
    DWORD dAttr = GetFileAttributesW(dest.c_str());
    if (dAttr != INVALID_FILE_ATTRIBUTES) {
        SYSTEMTIME rst = {};
        GetLocalTime(&rst);
        wchar_t stamp[32] = {};
        swprintf_s(stamp, L"_conflict_%04d%02d%02d_%02d%02d%02d",
                   rst.wYear, rst.wMonth, rst.wDay,
                   rst.wHour, rst.wMinute, rst.wSecond);
        const std::wstring rescue = dest + stamp;
        if (MoveFileW(dest.c_str(), rescue.c_str())) {
            Log("[ModManager] dest existed, renamed to %ls\n", rescue.c_str());
        } else {
            Log("[ModManager] dest rename failed (err=%lu), move may fail\n",
                GetLastError());
        }
    }

    BOOL ok = MoveFileExW(src.c_str(), dest.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED |
                          MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        Log("[ModManager] MoveFileEx failed: %ls -> %ls (err=%lu)\n",
            src.c_str(), dest.c_str(), GetLastError());
        return false;
    }

    g_mods[index].toggle = want;
    Log("[ModManager] %ls %ls\n", g_mods[index].folder,
        enable ? "ENABLED" : "DISABLED");
    return true;
}

// 前向声明
static void RequestRestart();

// ============================================================
// 按键录制（游戏内改键原型）
// ============================================================

// VK → 展示名（与 QoL_Shared::ParseVkName 相反映射）
static void VkToName(WORD vk, wchar_t* out, int outLen) {
    if (!out || outLen <= 0) return;
    if (vk >= '0' && vk <= '9') { swprintf_s(out, (size_t)outLen, L"%c", (wchar_t)vk); return; }
    if (vk >= 'A' && vk <= 'Z') { swprintf_s(out, (size_t)outLen, L"%c", (wchar_t)vk); return; }
    if (vk >= VK_F1 && vk <= VK_F24) { swprintf_s(out, (size_t)outLen, L"F%d", vk - VK_F1 + 1); return; }
    switch (vk) {
    case VK_RETURN:  wcscpy_s(out, (size_t)outLen, L"Enter"); return;
    case VK_SPACE:   wcscpy_s(out, (size_t)outLen, L"Space"); return;
    case VK_TAB:     wcscpy_s(out, (size_t)outLen, L"Tab"); return;
    case VK_ESCAPE:  wcscpy_s(out, (size_t)outLen, L"Esc"); return;
    case VK_BACK:    wcscpy_s(out, (size_t)outLen, L"Backspace"); return;
    case VK_DELETE:  wcscpy_s(out, (size_t)outLen, L"Del"); return;
    case VK_INSERT:  wcscpy_s(out, (size_t)outLen, L"Ins"); return;
    case VK_HOME:    wcscpy_s(out, (size_t)outLen, L"Home"); return;
    case VK_END:     wcscpy_s(out, (size_t)outLen, L"End"); return;
    case VK_PRIOR:   wcscpy_s(out, (size_t)outLen, L"PgUp"); return;
    case VK_NEXT:    wcscpy_s(out, (size_t)outLen, L"PgDn"); return;
    case VK_UP:      wcscpy_s(out, (size_t)outLen, L"Up"); return;
    case VK_DOWN:    wcscpy_s(out, (size_t)outLen, L"Down"); return;
    case VK_LEFT:    wcscpy_s(out, (size_t)outLen, L"Left"); return;
    case VK_RIGHT:   wcscpy_s(out, (size_t)outLen, L"Right"); return;
    case VK_CAPITAL: wcscpy_s(out, (size_t)outLen, L"CapsLock"); return;
    case VK_LSHIFT:  wcscpy_s(out, (size_t)outLen, L"LShift"); return;
    case VK_RSHIFT:  wcscpy_s(out, (size_t)outLen, L"RShift"); return;
    case VK_LCONTROL: wcscpy_s(out, (size_t)outLen, L"LCtrl"); return;
    case VK_RCONTROL: wcscpy_s(out, (size_t)outLen, L"RCtrl"); return;
    case VK_LMENU:   wcscpy_s(out, (size_t)outLen, L"LAlt"); return;
    case VK_RMENU:   wcscpy_s(out, (size_t)outLen, L"RAlt"); return;
    default: break;
    }
    swprintf_s(out, (size_t)outLen, L"VK_%u", (unsigned)vk);
}

// 把某 MOD 的热键描述写入 qol_hotkeys.txt（feature=key 每行一个）
static void WriteHotkeysFile(int index) {
    if (index < 0 || index >= g_modCount) return;
    if (g_mods[index].thirdParty && !g_mods[index].rekeyable) return;  // 第三方不可改键 MOD 不写入
    char dllNarrow[MAX_PATH] = {};
    WideCharToMultiByte(CP_ACP, 0, g_mods[index].dllName, -1,
                        dllNarrow, sizeof(dllNarrow), nullptr, nullptr);
    std::string dll(dllNarrow);
    size_t dot = dll.find('.');
    std::string feature = (dot == std::string::npos) ? dll : dll.substr(0, dot);
    if (feature.empty()) return;

    // 汇总该 MOD 当前所有按键为逗号分隔字符串
    std::string keyCsv;
    for (int h = 0; h < g_mods[index].hotkeyCount; ++h) {
        if (h > 0) keyCsv += ", ";
        char keyNarrow[64] = {};
        WideCharToMultiByte(CP_ACP, 0, g_mods[index].hotkeys[h], -1,
                            keyNarrow, sizeof(keyNarrow), nullptr, nullptr);
        keyCsv += keyNarrow;
    }
    if (keyCsv.empty()) keyCsv = "none";

    // 复用 QolRegisterHotKey 写入（它内部会移除旧 feature 行再追加）
    QolRegisterHotKey(feature.c_str(), keyCsv.c_str());
    Log("[ModManager] hotkey saved: %s = %s\n", feature.c_str(), keyCsv.c_str());
}

// 删除当前录制的按键并完成
static void RecordDeleteKey() {
    if (g_recModIdx < 0) return;
    int modIdx = g_recModIdx;
    int keyIdx = g_recKeyIdx;
    if (modIdx >= 0 && modIdx < g_modCount && keyIdx >= 0 && keyIdx < g_mods[modIdx].hotkeyCount) {
        // 将该按键设为空字符串
        g_mods[modIdx].hotkeys[keyIdx][0] = 0;
        // 重新拼接 hotkey 字符串（跳过空键）
        std::wstring joined;
        for (int h = 0; h < g_mods[modIdx].hotkeyCount; ++h) {
            if (g_mods[modIdx].hotkeys[h][0] == 0) continue;
            if (!joined.empty()) joined += L", ";
            joined += g_mods[modIdx].hotkeys[h];
        }
        static wchar_t s_delBuf[128];
        wcscpy_s(s_delBuf, joined.c_str());
        g_mods[modIdx].hotkey = joined.empty() ? nullptr : s_delBuf;
        WriteHotkeysFile(modIdx);
        Log("[ModManager] hotkey deleted: mod=%d key=%d\n", modIdx, keyIdx);
    }
    CancelRecordKey();
}

// 开始录制某个按键
static void BeginRecordKey(int modIdx, int keyIdx) {
    g_recModIdx = modIdx;
    g_recKeyIdx = keyIdx;
    g_recStage = 0;            // 进入捕获阶段
    g_recGpWait = true;        // 等待手柄全部松开，避免录到进入时的键
    g_capturedVk = -1;
    g_gamepadCaptured = nullptr;
    Log("[ModManager] record start: mod=%ls key=%d\n",
        g_mods[modIdx].folder, keyIdx);
    SetCapture(g_panel);   // 捕获鼠标，便于 ESC 取消
    InvalidateRect(g_panel, nullptr, FALSE);
}

// 当前键已写入：若还有更多键 → 进入"下一个/结束"选择阶段；否则直接结束录制
static void RecordKeyWritten() {
    if (g_recModIdx < 0) return;
    if (g_recKeyIdx + 1 < g_mods[g_recModIdx].hotkeyCount) {
        g_recStage = 1;
        g_recNextSel = 0;
        g_recNextRelease = true;
    } else {
        CancelRecordKey();
    }
    InvalidateRect(g_panel, nullptr, FALSE);
}

// 选择"下一个"：开始录制下一槽位
static void RecordNext() {
    if (g_recModIdx < 0) return;
    if (g_recKeyIdx + 1 < g_mods[g_recModIdx].hotkeyCount) {
        g_recKeyIdx++;
        g_recStage = 0;        // 回到捕获阶段
        g_recGpWait = true;    // 防抖：等待手柄全部松开再接受输入
        g_capturedVk = -1;
        g_gamepadCaptured = nullptr;
        InvalidateRect(g_panel, nullptr, FALSE);
    } else {
        CancelRecordKey();
    }
}

// 取消录制
static void CancelRecordKey() {
    if (g_recModIdx >= 0 && GetCapture() == g_panel) ReleaseCapture();
    g_recModIdx = -1;
    g_recKeyIdx = -1;
    g_recStage = 0;
    g_recGpWait = false;
    g_capturedVk = -1;
    g_gamepadCaptured = nullptr;
    InvalidateRect(g_panel, nullptr, FALSE);
}

// ============================================================
// 切换指定 MOD 的开关（UI 调用；直接切换并记录，关闭面板时统一提示）
// ============================================================
static void ToggleMod(int index) {
    if (index < 0 || index >= g_modCount) return;
    if (g_mods[index].isSelf) return;
    if (g_inSave) {
        Log("[ModManager] blocked toggle (in save)\n");
        return;
    }
    bool enable = (g_mods[index].toggle != ToggleState::Enabled);
    if (SetModEnabled(index, enable)) {
        g_restartRequested = true;   // 记录本轮有修改
        g_changedCount++;
        if (g_changedNames.empty()) g_changedNames = g_mods[index].name;
        else g_changedNames += L"\u3001" + std::wstring(g_mods[index].name); // 、
        ScanToggleState();           // 重排序 + 更新开关
        InvalidateRect(g_panel, nullptr, FALSE);
        // 不在这里 RequestRestart，等待面板关闭时统一确认
    }
}

// ============================================================
// 存档检测：尝试独占打开 save.001（运行时被占用即视为已进存档）
// ============================================================
static bool DetectInSave() {
    // 存档目录：Roaming\Nippon Ichi Software, Inc\Honogurashinoniwa\<steamid>
    wchar_t roam[MAX_PATH] = {};
    if (!SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, roam))
        return false;
    std::wstring saveRoot = std::wstring(roam) + L"\\Nippon Ichi Software, Inc\\Honogurashinoniwa";
    WIN32_FIND_DATAW fd = {};
    HANDLE hf = FindFirstFileW((saveRoot + L"\\*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    std::wstring saveDir;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.cFileName[0] != L'.') { saveDir = saveRoot + L"\\" + fd.cFileName; found = true; }
        }
    } while (found == false && FindNextFileW(hf, &fd));
    FindClose(hf);
    if (!found || saveDir.empty()) return false;

    std::wstring saveFile = saveDir + L"\\save.001";
    HANDLE h = CreateFileW(saveFile.c_str(), GENERIC_READ, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return (e == ERROR_SHARING_VIOLATION); // 独占失败 → 已被游戏占用 → 已进存档
    }
    CloseHandle(h);
    return false;
}

// ============================================================
// 绘制工具
// ============================================================
static void RoundFill(HDC dc, const RECT& rect, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HRGN region = CreateRoundRectRgn(rect.left, rect.top, rect.right + 1,
                                     rect.bottom + 1, radius, radius);
    if (brush && region) FillRgn(dc, region, brush);
    if (region) DeleteObject(region);
    if (brush) DeleteObject(brush);
}

static void DrawTextEx(HDC dc, HFONT font, COLORREF color,
                       const wchar_t* text, RECT rect, UINT flags) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HFONT oldFont = nullptr;
    if (font) oldFont = (HFONT)SelectObject(dc, font);
    DrawTextW(dc, text, -1, &rect, flags | DT_NOPREFIX);
    if (font) SelectObject(dc, oldFont);
}

// 行矩形（按显示顺序 + 滚动偏移）
static RECT RowRect(int displayIdx) {
    int row = displayIdx - g_scrollTop;
    RECT r;
    r.left = LEFT_MARGIN;
    r.right = PANEL_WIDTH - RIGHT_MARGIN;
    r.top = ROW_START_Y + row * (ROW_HEIGHT + ROW_GAP);
    r.bottom = r.top + ROW_HEIGHT;
    return r;
}

// 录制选择弹窗矩形（弹窗本体 + 下一个/结束 两个按钮）
static void GetRecChoiceRects(RECT* box, RECT* nextBtn, RECT* delBtn, RECT* endBtn) {
    const int rw = S(520), rh = S(210);
    int left = (PANEL_WIDTH - rw) / 2, top = (PANEL_HEIGHT - rh) / 2;
    if (box) *box = { left, top, left + rw, top + rh };
    const int rbW = S(140), rbH = S(40), rbGap = S(20);
    int by = top + rh - rbH - S(48);
    int totalW = rbW * 3 + rbGap * 2;
    int sx = (PANEL_WIDTH - totalW) / 2;
    if (nextBtn) *nextBtn = RECT{ sx, by, sx + rbW, by + rbH };
    if (delBtn)  *delBtn  = RECT{ sx + rbW + rbGap, by, sx + rbW * 2 + rbGap, by + rbH };
    if (endBtn)  *endBtn  = RECT{ sx + rbW * 2 + rbGap * 2, by, sx + rbW * 3 + rbGap * 2, by + rbH };
}

// 显示顺序 → 数组下标
static int ArrayIdxOfDisplay(int displayIdx) {
    for (int i = 0; i < g_modCount; ++i)
        if (g_mods[i].displayIdx == displayIdx) return i;
    return -1;
}

// ============================================================
// 状态文本与颜色
// ============================================================
static const wchar_t* ToggleText(ToggleState t) {
    switch (t) {
    case ToggleState::Enabled:  return L"\u5DF2\u542F\u7528";       // 已启用
    case ToggleState::Disabled: return L"\u5DF2\u505C\u7528";       // 已停用
    case ToggleState::Self:     return L"\u81EA\u8EAB";             // 自身
    default:                    return L"\u5F02\u5E38";             // 异常
    }
}

static const wchar_t* StatusText(ModStatus s) {
    switch (s) {
    case ModStatus::Loaded:      return L"\u5DF2\u542F\u7528";
    case ModStatus::Unsupported: return L"\u5F02\u5E38 \u00b7 exe\u4E0D\u5339\u914D";
    case ModStatus::Error:       return L"\u8FD0\u884C\u5F02\u5E38";
    case ModStatus::NotLoaded:   return L"\u672A\u52A0\u8F7D";
    default:                     return L"\u672A\u77E5";
    }
}

static COLORREF StatusColor(ModStatus s) {
    switch (s) {
    case ModStatus::Loaded:      return Colors::Accent2;
    case ModStatus::Unsupported: return Colors::Danger;
    case ModStatus::Error:       return Colors::Danger;
    case ModStatus::NotLoaded:   return Colors::TextDim;
    default:                     return Colors::TextDim;
    }
}

// ============================================================
// 绘制面板
// ============================================================
static void PaintPanel(HWND window, HDC destination) {
    RECT client = {};
    GetClientRect(window, &client);

    HDC memory = CreateCompatibleDC(destination);
    HBITMAP bitmap = memory ?
        CreateCompatibleBitmap(destination, client.right, client.bottom) : nullptr;
    HBITMAP oldBitmap = nullptr;
    HDC dc = destination;
    if (memory && bitmap) {
        oldBitmap = (HBITMAP)SelectObject(memory, bitmap);
        dc = memory;
    }

    HBRUSH bg = CreateSolidBrush(Colors::Bg);
    if (bg) { FillRect(dc, &client, bg); DeleteObject(bg); }

// 标题
    RECT titleRect = { LEFT_MARGIN, S(12), PANEL_WIDTH - S(60), S(36) };
    DrawTextEx(dc, g_titleFont, Colors::Accent,
               L"Village MOD \u7BA1\u7406\u53F0", titleRect,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 副标题（操作提示，QoL 套件标识已在告示中体现，不再重复）
    wchar_t sub[320];
    swprintf_s(sub, L"F9/Esc \u5173\u95ED%s%s",
               g_inSave ? L"  \u00B7  \u5B58\u6863\u4E2D\u9501\u5B9A" : L"",
               g_restartRequested ? L"  \u00B7  \u5F85\u91CD\u542F" : L"");
    // v1.2.3: P0 性能探针摘要（各 MOD tick ema 耗时，共享内存读取）
    {
        uint32_t perfCount = 0;
        const qol::budget::BudgetModEntry* perfMods =
            qol::budget::ModEntries(&perfCount);
        if (perfMods && perfCount > 0) {
            wchar_t* p = sub + wcslen(sub);
            size_t remain = (sizeof(sub) / sizeof(wchar_t)) - wcslen(sub);
            if (remain > 48) {
                *p++ = L' '; *p++ = L'\u00B7'; *p++ = L' '; remain -= 3;
                for (uint32_t pi = 0; pi < perfCount && remain > 30; ++pi) {
                    if (perfMods[pi].frames == 0) continue;
                    int w = _snwprintf_s(p, remain, _TRUNCATE, L"%hs %lluu ",
                        perfMods[pi].name,
                        (unsigned long long)perfMods[pi].ema_us);
                    if (w <= 0) break;
                    p += w; remain -= (size_t)w;
                }
            }
        }
    }
    RECT subRect = { LEFT_MARGIN + S(2), S(36), PANEL_WIDTH - S(60), S(54) };
    DrawTextEx(dc, g_smallFont, Colors::TextDim, sub, subRect,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 关闭按钮（右上角 X）
    RECT closeRect = { PANEL_WIDTH - S(46), S(16), PANEL_WIDTH - S(18), S(44) };
    HPEN closePen = CreatePen(PS_SOLID, 2, Colors::TextDim);
    if (closePen) {
        HPEN oldPen = (HPEN)SelectObject(dc, closePen);
        MoveToEx(dc, closeRect.left + S(8), closeRect.top + S(8), nullptr);
        LineTo(dc, closeRect.right - S(8), closeRect.bottom - S(8));
        MoveToEx(dc, closeRect.right - S(8), closeRect.top + S(8), nullptr);
        LineTo(dc, closeRect.left + S(8), closeRect.bottom - S(8));
        SelectObject(dc, oldPen);
        DeleteObject(closePen);
    }

    // 顶部警示栏（放大：字体 + 高度）
    RECT warnRect = { LEFT_MARGIN, S(56), PANEL_WIDTH - RIGHT_MARGIN, S(92) };
    wchar_t warn[200];
    COLORREF warnColor;
    if (g_inSave) {
        swprintf_s(warn, L"\u5B58\u6863\u4E2D\u9501\u5B9A \u00B7 \u7981\u6B62\u4FEE\u6539\u5F00\u5173");
        warnColor = Colors::Danger;
        RoundFill(dc, warnRect, S(6), RGB(40, 30, 30));
    } else if (g_restartRequested) {
        swprintf_s(warn, L"\u5DF2\u4FEE\u6539 \u00B7 \u5173\u95ED\u540E\u81EA\u52A8\u91CD\u542F\u6E38\u620F\u751F\u6548");
        warnColor = Colors::Accent;
        RoundFill(dc, warnRect, S(6), RGB(46, 42, 32));
    } else {
        swprintf_s(warn, L"\u9F20\u6807\u70B9\u51FB\u5F00\u5173 \u00B7 \u624B\u67C4\u65B9\u5411\u952E\u9009\u62E9 / R1 \u786E\u8BA4 \u00B7 Esc/F9/L3+R3 \u5173\u95ED \u00B7 \u4FEE\u6539\u540E\u91CD\u542F\u751F\u6548");
        warnColor = Colors::TextDim;
        RoundFill(dc, warnRect, S(6), Colors::Panel);
    }

    DrawTextEx(dc, g_bodyFont, warnColor, warn, warnRect,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 可见行范围（考虑滚动）
    int firstVisible = g_scrollTop;
    int maxScroll = g_visibleCount - VISIBLE_ROWS;
    if (maxScroll < 0) maxScroll = 0;
    if (g_scrollTop > maxScroll) g_scrollTop = maxScroll;
    int lastVisible = firstVisible + VISIBLE_ROWS;
    if (lastVisible > g_visibleCount) lastVisible = g_visibleCount;

    // MOD 行
    for (int d = firstVisible; d < lastVisible; ++d) {
        int i = ArrayIdxOfDisplay(d);
        if (i < 0) continue;
        RECT row = RowRect(d);
        bool isError = (g_mods[i].status == ModStatus::Unsupported ||
                        g_mods[i].status == ModStatus::Error);
        bool isOff   = (g_mods[i].toggle != ToggleState::Enabled);
        bool isSelf  = g_mods[i].isSelf;
        bool locked  = g_inSave || isSelf;
        bool depMiss = (MissingDependency(i) != nullptr);

        COLORREF rowColor = isError ? RGB(36, 28, 30) : Colors::Panel;
        // 手柄光标所在行高亮边框
        if (d == g_gpCursor) {
            HPEN gpPen = CreatePen(PS_SOLID, 2, Colors::Accent);
            if (gpPen) {
                HPEN oldPen = (HPEN)SelectObject(dc, gpPen);
                HBRUSH oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, row.left, row.top, row.right, row.bottom);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(gpPen);
            }
        }
        RoundFill(dc, row, S(8), rowColor);

        // 左侧强调条（异常红 / 依赖缺黄 / 停用灰 / 正常绿）
        RECT bar = row;
        bar.right = bar.left + S(4);
        if (isError)      RoundFill(dc, bar, S(2), Colors::Danger);
        else if (depMiss) RoundFill(dc, bar, S(2), Colors::Warn);
        else if (!isOff)  RoundFill(dc, bar, S(2), Colors::Accent2);

        // 序号
        wchar_t numText[4];
        swprintf_s(numText, L"%d", i + 1);
        RECT numRect = { row.left + S(14), row.top + S(10), row.left + S(46), row.top + S(40) };
        RoundFill(dc, numRect, S(6), Colors::Panel3);
        DrawTextEx(dc, g_bodyFont,
                   isOff ? Colors::TextDim : Colors::Accent,
                   numText, numRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

// ---- 新布局（v0.5.2）：自左向右 = 序号 | 名称(中文) | 英文+版本 | 依赖警示 | 快捷键格子×N | 改键按钮 | 竖线 | 开关 ----
        // 右侧固定区：开关（最右，与改键按钮同尺寸对齐）| 竖线（开关左侧）
        const int fixedH = S(32);                    // 与改键按钮同高
        const int fixedTop = row.top + (ROW_HEIGHT - fixedH) / 2;   // 垂直居中
        // 开关 54×32，与改键按钮同尺寸、同垂直位置（右端距 row.right S(6)px）
        RECT togg = { row.right - S(60), fixedTop, row.right - S(6), fixedTop + fixedH };

        // 开关 toggle（滑动开关：亮轨道 + 圆形旋钮，状态清晰）
        if (isSelf) {
            RoundFill(dc, togg, S(12), Colors::Panel3);
            DrawTextEx(dc, g_smallFont, Colors::TextDim, L"\u25CB", togg,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            bool on = !isOff;
            // 轨道：整个 54×32 区域，上下边与改键按钮完全对齐
            RECT track = togg;
            // 开=亮绿，关=深灰（锁定未启用时强制深灰）
            COLORREF trackBg = on ? RGB(104, 150, 88) : RGB(52, 56, 62);
            if (locked && !on) trackBg = RGB(52, 56, 62);
            RoundFill(dc, track, S(14), trackBg);
            // 圆形旋钮：直径 S(24)，垂直居中，开=白，关=浅灰
            const int knobD = S(24);
            const int knobY = togg.top + (fixedH - knobD) / 2;
            int knobX = on ? (togg.right - knobD - S(3)) : (togg.left + S(3));
            RECT knobR { knobX, knobY, knobX + knobD, knobY + knobD };
            RoundFill(dc, knobR, knobD / 2, on ? RGB(245, 245, 240) : RGB(150, 154, 160));
            if (locked && !on) RoundFill(dc, knobR, knobD / 2, RGB(120, 124, 130));
        }
        // 手柄焦点高亮（行选中且焦点在开关区；无热键行焦点默认在开关）
        if (d == g_gpCursor && (g_gpFocus == 1 || g_mods[i].hotkeyCount == 0)) {
            HPEN fp = CreatePen(PS_SOLID, 2, Colors::Accent);
            if (fp) {
                HPEN oldP = (HPEN)SelectObject(dc, fp);
                HBRUSH oldB = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
                RoundRect(dc, togg.left - S(3), togg.top - S(3),
                          togg.right + S(3), togg.bottom + S(3), S(14), S(14));
                SelectObject(dc, oldB);
                SelectObject(dc, oldP);
                DeleteObject(fp);
            }
        }

        // ---- 竖线（开关左侧，留 S(10)px 空位）----
        const int sepX = togg.left - S(10);
        HPEN sepPen = CreatePen(PS_SOLID, 1, Colors::Border);
        if (sepPen) {
            HPEN oldPen = (HPEN)SelectObject(dc, sepPen);
            MoveToEx(dc, sepX, row.top + S(8), nullptr);
            LineTo(dc, sepX, row.bottom - S(8));
            SelectObject(dc, oldPen);
            DeleteObject(sepPen);
        }

        // ---- 改键按钮（竖线左侧，统一位置）----
        const int recW = S(54);
        if (g_mods[i].hotkeyCount > 0 && !isSelf &&
            (!g_mods[i].thirdParty || g_mods[i].rekeyable)) {
            wchar_t recText[32];
            bool anyRecording = (g_recModIdx == i);
            if (anyRecording) swprintf_s(recText, L"\u5F55\u5236\u4E2D");
            else             swprintf_s(recText, L"\u6539\u952E");
            RECT recRect = { sepX - S(8) - recW, fixedTop, sepX - S(8), fixedTop + fixedH };
            COLORREF recBg = anyRecording ? RGB(60, 52, 30) : RGB(52, 56, 60);
            RoundFill(dc, recRect, S(5), recBg);
            DrawTextEx(dc, g_bodyFont,
                       anyRecording ? Colors::Accent : Colors::TextDim,
                       recText, recRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            // 手柄焦点高亮（行选中且焦点在改键区）
            if (d == g_gpCursor && g_gpFocus == 0) {
                HPEN fp = CreatePen(PS_SOLID, 2, Colors::Accent);
                if (fp) {
                    HPEN oldP = (HPEN)SelectObject(dc, fp);
                    HBRUSH oldB = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
                    RoundRect(dc, recRect.left - S(2), recRect.top - S(2),
                              recRect.right + S(2), recRect.bottom + S(2), S(6), S(6));
                    SelectObject(dc, oldB);
                    SelectObject(dc, oldP);
                    DeleteObject(fp);
                }
            }
            // 记录命中区域（供点击/手柄激活检测）
            if (i < 32) g_keyRects[i][6] = recRect;
        }

        // ---- 快捷键格子（改键按钮左侧，一格一个键，横排不换行）----
        int tagRight = sepX - S(8) - recW - S(8);   // 格子区右端（紧贴改键按钮左缘）
        if (g_mods[i].hotkeyCount > 0) {
            int conflictKey = FindConflictingHotkey(i);
            for (int h = g_mods[i].hotkeyCount - 1; h >= 0; --h) {
                wchar_t keyText[16];
                bool recording = (g_recModIdx == i && g_recKeyIdx == h);
                if (recording && g_capturedVk < 0 && !g_gamepadCaptured) {
                    wcscpy_s(keyText, L"\u00B7\u00B7\u00B7");
                } else if (recording && g_gamepadCaptured) {
                    wcscpy_s(keyText, g_gamepadCaptured);
                } else {
                    wcscpy_s(keyText, g_mods[i].hotkeys[h]);
                }
                RECT cell = { tagRight - KEYCELL_W, row.top + S(14),
                              tagRight - KEYCELL_GAP, row.top + S(14) + KEYCELL_H };
                bool thisConflict = (h == conflictKey);
                COLORREF cellBg = thisConflict ? RGB(52, 40, 30) : RGB(45, 50, 38);
                if (recording) cellBg = RGB(60, 52, 30);
                RoundFill(dc, cell, S(5), cellBg);
                HPEN cellPen = CreatePen(PS_SOLID, 1,
                    thisConflict ? Colors::Danger :
                    (recording ? Colors::Accent : Colors::Border));
                if (cellPen) {
                    HPEN oldPen = (HPEN)SelectObject(dc, cellPen);
                    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
                    RoundRect(dc, cell.left, cell.top, cell.right, cell.bottom, S(5), S(5));
                    SelectObject(dc, oldBrush);
                    SelectObject(dc, oldPen);
                    DeleteObject(cellPen);
                }
                DrawTextEx(dc, g_smallFont,
                           recording ? Colors::Accent :
                           (thisConflict ? Colors::Danger : Colors::Accent),
                           keyText, cell, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                // 记录命中区域（供点击检测）
                if (i < 32 && h < 6) g_keyRects[i][h] = cell;
                tagRight -= (KEYCELL_W + KEYCELL_GAP);
            }
        }

        // ---- 依赖缺失警示（快捷键左侧）----
        int depW = 0;
        if (depMiss) {
            SIZE depSize = {};
            if (g_smallFont) SelectObject(dc, g_smallFont);
            GetTextExtentPoint32W(dc, L"\u4F9D\u8D56 \u672A\u542F\u7528", 6, &depSize);
            depW = depSize.cx + S(16);
            RECT depRect = { tagRight - depW, row.top + S(8), tagRight, row.top + S(30) };
            RoundFill(dc, depRect, S(5), Colors::WarnBg);
            DrawTextEx(dc, g_smallFont, Colors::Warn, L"\u4F9D\u8D56 \u672A\u542F\u7528", depRect,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            tagRight -= (depW + S(6));
        }

        // ---- 名称区：中文名（第一行）、英文名+版本号（第二行，版本号小字跟在英文右侧）----
        const wchar_t* fullName = g_mods[i].name;
        wchar_t enName[64] = {}, cnName[64] = {};
        const wchar_t* sp = fullName ? wcschr(fullName, L' ') : nullptr;
        if (sp && sp > fullName && sp[1]) {
            wcsncpy_s(enName, 64, fullName, (size_t)(sp - fullName));
            wcscpy_s(cnName, sp + 1);
        } else {
            wcscpy_s(cnName, fullName ? fullName : L"");
        }
        // 名称区右端（动态收缩到列警示左侧，或默认到 tagRight）
        int nameRight = tagRight - S(8);
        if (nameRight < row.left + S(58) + S(40)) nameRight = row.left + S(58) + S(40); // 保底宽度
        RECT nameRect = { row.left + S(58), row.top + S(6), nameRight, row.bottom - S(6) };

        // 版本号小字（跟随英文名行，紧跟英文名右侧）
        int vW = 0;
        if (g_mods[i].version[0]) {
            SIZE verSize = {};
            if (g_smallFont) SelectObject(dc, g_smallFont);
            GetTextExtentPoint32W(dc, g_mods[i].version,
                                  (int)wcslen(g_mods[i].version), &verSize);
            vW = verSize.cx + S(10);
        }
        // 中文名（第一行，占满名称区）
        RECT cnRect = { nameRect.left, nameRect.top,
                        nameRect.right, nameRect.top + S(26) };
        DrawTextEx(dc, g_bodyFont,
                   isOff ? Colors::TextDim : Colors::Text,
                   cnName, cnRect,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (enName[0]) {
            // 英文名（第二行）
            RECT enRect = { nameRect.left, nameRect.top + S(26),
                            nameRect.right, nameRect.bottom };
            // 先测量英文名宽度，版本号紧跟其后
            SIZE enSize = {};
            if (g_smallFont) SelectObject(dc, g_smallFont);
            GetTextExtentPoint32W(dc, enName, (int)wcslen(enName), &enSize);
            int enW = enSize.cx;
            // 版本号（紧跟英文名右侧，小字呈现）
            RECT verRect = {};
            bool showVer = false;
            if (vW > 0) {
                int vX = enRect.left + enW + S(8);
                if (vX + vW <= enRect.right - S(4)) {
                    verRect = { vX, enRect.top + S(3), vX + vW, enRect.bottom - S(3) };
                    showVer = true;
                }
            }
            // 英文名绘制区：版本号左侧留 S(8)px
            int enRight = showVer ? verRect.left - S(8) : enRect.right;
            RECT enTextRect = { enRect.left, enRect.top, enRight, enRect.bottom };
            DrawTextEx(dc, g_smallFont, Colors::TextDim,
                       enName, enTextRect,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (showVer) {
                RoundFill(dc, verRect, S(5), Colors::Panel3);
                DrawTextEx(dc, g_smallFont, Colors::TextDim,
                           g_mods[i].version, verRect,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
    }

    // 底部滚动提示（如有更多行，字体放大 + 底部居中）
    if (maxScroll > 0 && g_scrollTop < maxScroll) {
        wchar_t more[32];
        swprintf_s(more, L"\u25BC \u6EDA\u52A8\u67E5\u770B\u66F4\u591A");
        RECT moreRect = { LEFT_MARGIN, PANEL_HEIGHT - S(34), PANEL_WIDTH - RIGHT_MARGIN, PANEL_HEIGHT - S(6) };
        DrawTextEx(dc, g_bodyFont, Colors::TextDim, more, moreRect,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // ============================================================
    // 自绘确认弹窗（覆盖在面板之上，支持手柄操作）
    // ============================================================
    if (g_confirmDlg) {
        // 半透明遮罩
        HBRUSH mask = CreateSolidBrush(RGB(10, 12, 14));
        if (mask) {
            HDC maskDC = CreateCompatibleDC(destination);
            HBITMAP maskBmp = CreateCompatibleBitmap(destination,
                client.right, client.bottom);
            HBITMAP maskOld = nullptr;
            if (maskDC && maskBmp) {
                maskOld = (HBITMAP)SelectObject(maskDC, maskBmp);
                FillRect(maskDC, &client, mask);
            }
            if (maskDC && maskBmp) {
                // 半透明混合
                BLENDFUNCTION blend = { AC_SRC_OVER, 0, 180, 0 };
                AlphaBlend(dc, 0, 0, client.right, client.bottom, maskDC,
                           0, 0, client.right, client.bottom, blend);
            }
            if (maskOld) SelectObject(maskDC, maskOld);
            if (maskBmp) DeleteObject(maskBmp);
            if (maskDC) DeleteDC(maskDC);
            DeleteObject(mask);
        }

        // 确认框
        int dw = S(520), dh = S(220);
        RECT boxRect = { (PANEL_WIDTH - dw) / 2, (PANEL_HEIGHT - dh) / 2,
                         (PANEL_WIDTH + dw) / 2, (PANEL_HEIGHT + dh) / 2 };
        RoundFill(dc, boxRect, S(10), Colors::Panel2);
        // 边框
        HPEN bPen = CreatePen(PS_SOLID, 2, Colors::Border);
        if (bPen) {
            HPEN oldPen = (HPEN)SelectObject(dc, bPen);
            HBRUSH oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, boxRect.left, boxRect.top, boxRect.right, boxRect.bottom, S(10), S(10));
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(bPen);
        }

        // 标题
        RECT dlgTitle = { boxRect.left + S(24), boxRect.top + S(16),
                          boxRect.right - S(24), boxRect.top + S(46) };
        DrawTextEx(dc, g_bodyFont, Colors::Accent,
                   L"\u786E\u8BA4\u5E94\u7528\u4FEE\u6539", dlgTitle,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 正文
        wchar_t dlgMsg[400];
        // 截断过长的 MOD 名称列表，避免缓冲区溢出
        const wchar_t* names = g_changedNames.c_str();
        size_t maxNameLen = 200;
        if (g_changedNames.length() > maxNameLen) {
            static std::wstring s_truncated;
            s_truncated = g_changedNames.substr(0, maxNameLen);
            s_truncated += L"...";
            names = s_truncated.c_str();
        }
        swprintf_s(dlgMsg,
            L"\u5DF2\u4FEE\u6539 %d \u4E2A MOD\uFF1A%ls\n"
            L"\u4FEE\u6539\u9700\u8981\u5173\u95ED\u6E38\u620F\u5E76\u91CD\u542F\u624D\u80FD\u751F\u6548\u3002",
            g_changedCount, names);
        RECT dlgBody = { boxRect.left + S(24), boxRect.top + S(52),
                         boxRect.right - S(24), boxRect.top + S(128) };
        DrawTextEx(dc, g_smallFont, Colors::Text, dlgMsg, dlgBody,
                   DT_LEFT | DT_TOP | DT_WORDBREAK);

        // 两个按钮：是(重启) / 否(保持运行)
        int btnW = S(180), btnH = S(40);
        int btnY = boxRect.bottom - btnH - S(20);
        int gap = S(30);
        int totalW = btnW * 2 + gap;
        int startX = (PANEL_WIDTH - totalW) / 2;
        RECT btnYes = { startX, btnY, startX + btnW, btnY + btnH };
        RECT btnNo  = { startX + btnW + gap, btnY, startX + btnW * 2 + gap, btnY + btnH };

        // 否(保持运行) 选中高亮
        COLORREF yesBg = (g_confirmSel == 0) ? RGB(90, 70, 40) : Colors::Panel3;
        COLORREF noBg  = (g_confirmSel == 1) ? RGB(90, 70, 40) : Colors::Panel3;
        RoundFill(dc, btnYes, S(8), yesBg);
        RoundFill(dc, btnNo, S(8), noBg);
        DrawTextEx(dc, g_bodyFont, g_confirmSel == 0 ? Colors::Accent : Colors::Text,
                   L"\u662F \u00B7 \u91CD\u542F", btnYes,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextEx(dc, g_bodyFont, g_confirmSel == 1 ? Colors::Accent : Colors::Text,
                   L"\u5426 \u00B7 \u4FDD\u6301\u8FD0\u884C", btnNo,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // 手柄提示
        RECT dlgHint = { boxRect.left + S(24), boxRect.bottom - S(58),
                         boxRect.right - S(24), boxRect.bottom - S(38) };
        DrawTextEx(dc, g_smallFont, Colors::TextDim,
                   L"\u65B9\u5411\u952E \u5DE6\u53F3 \u9009\u62E9 \u00B7 R1 \u786E\u8BA4 \u00B7 \u25CB \u53D6\u6D88",
                   dlgHint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // ============================================================
    // 录制控制弹窗（录完一键后：选择\"下一个\"继续录 / \"结束\"完成）
    // ============================================================
    if (g_recModIdx >= 0 && g_recStage == 1) {
        RECT box, nextBtn, delBtn, endBtn;
        GetRecChoiceRects(&box, &nextBtn, &delBtn, &endBtn);

        // 半透明遮罩
        HBRUSH mask = CreateSolidBrush(RGB(10, 12, 14));
        if (mask) {
            HDC maskDC = CreateCompatibleDC(destination);
            HBITMAP maskBmp = CreateCompatibleBitmap(destination,
                client.right, client.bottom);
            HBITMAP maskOld = nullptr;
            if (maskDC && maskBmp) {
                maskOld = (HBITMAP)SelectObject(maskDC, maskBmp);
                FillRect(maskDC, &client, mask);
            }
            if (maskDC && maskBmp) {
                BLENDFUNCTION blend = { AC_SRC_OVER, 0, 160, 0 };
                AlphaBlend(dc, 0, 0, client.right, client.bottom, maskDC,
                           0, 0, client.right, client.bottom, blend);
            }
            if (maskOld) SelectObject(maskDC, maskOld);
            if (maskBmp) DeleteObject(maskBmp);
            if (maskDC) DeleteDC(maskDC);
            DeleteObject(mask);
        }

        // 弹窗本体
        RoundFill(dc, box, S(10), Colors::Panel2);
        HPEN rbPen = CreatePen(PS_SOLID, 2, Colors::Border);
        if (rbPen) {
            HPEN oldPen = (HPEN)SelectObject(dc, rbPen);
            HBRUSH oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, box.left, box.top, box.right, box.bottom, S(10), S(10));
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(rbPen);
        }

        // 标题
        RECT recTitle = { box.left + S(24), box.top + S(18),
                          box.right - S(24), box.top + S(48) };
        DrawTextEx(dc, g_bodyFont, Colors::Accent,
                   L"\u5F55\u5236\u5B8C\u6210 \u00B7 \u4E0B\u4E00\u6B65\uFF1F", recTitle,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 当前按键提示
        wchar_t recMsg[160];
        swprintf_s(recMsg,
            L"\u5DF2\u5F55\u5165\u952E %d/%d \uFF1A\u70B9\u51FB\u201C\u4E0B\u4E00\u4E2A\u201D\u7EE7\u7EED\u5F55\u5236\uFF0C\u201C\u7ED3\u675F\u201D\u5B8C\u6210\u3002",
            g_recKeyIdx + 1, g_mods[g_recModIdx].hotkeyCount);
        RECT msgRect = { box.left + S(24), box.top + S(52),
                         box.right - S(24), box.top + S(96) };
        DrawTextEx(dc, g_smallFont, Colors::Text, recMsg, msgRect,
                   DT_LEFT | DT_TOP | DT_WORDBREAK);

        // 下一个 / 结束 按钮
        COLORREF nextBg = (g_recNextSel == 0) ? RGB(90, 70, 40) : Colors::Panel3;
        COLORREF endBg  = (g_recNextSel == 1) ? RGB(90, 70, 40) : Colors::Panel3;
        RoundFill(dc, nextBtn, S(8), nextBg);
        RoundFill(dc, endBtn, S(8), endBg);
        DrawTextEx(dc, g_bodyFont, g_recNextSel == 0 ? Colors::Accent : Colors::Text,
                   L"\u4E0B\u4E00\u4E2A", nextBtn,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextEx(dc, g_bodyFont, g_recNextSel == 1 ? Colors::Accent : Colors::Text,
                   L"\u7ED3\u675F", endBtn,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // 手柄提示
        RECT recHint = { box.left + S(24), box.bottom - S(40),
                         box.right - S(24), box.bottom - S(20) };
        DrawTextEx(dc, g_smallFont, Colors::TextDim,
                   L"\u65B9\u5411\u952E \u5DE6\u53F3 \u9009\u62E9 \u00B7 R1 \u786E\u8BA4 \u00B7 \u25CB \u7ED3\u675F",
                   recHint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // 双缓冲提交
    if (memory && bitmap) {
        BitBlt(destination, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
    } else {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
    }
}

// ============================================================
// 查找游戏主窗口（作为面板 owner）
// ============================================================
static BOOL CALLBACK FindOwnerCallback(HWND window, LPARAM data) {
    if (window == g_panel || !IsWindowVisible(window)) return TRUE;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId()) return TRUE;
    LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0 || GetWindow(window, GW_OWNER)) return TRUE;
    *reinterpret_cast<HWND*>(data) = window;
    return FALSE;
}

static HWND FindGameWindow() {
    HWND foreground = GetForegroundWindow();
    DWORD processId = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &processId);
    if (foreground && processId == GetCurrentProcessId() && foreground != g_panel)
        return foreground;
    if (g_owner && IsWindow(g_owner)) return g_owner;
    HWND found = nullptr;
    EnumWindows(FindOwnerCallback, reinterpret_cast<LPARAM>(&found));
    return found;
}

// 撤销本轮所有改动：把开关状态恢复为面板打开时的初始状态
static void RevertChanges() {
    Log("[ModManager] reverting %d changes\n", g_changedCount);
    for (int i = 0; i < g_modCount; ++i) {
        if (g_mods[i].isSelf) continue;
        if (g_mods[i].toggle == ToggleState::Missing) continue;
        // 面板打开时是 Enabled/Disabled，而现在变了 → 恢复
        if (g_mods[i].startToggle == ToggleState::Enabled ||
            g_mods[i].startToggle == ToggleState::Disabled) {
            bool wantEnable = (g_mods[i].startToggle == ToggleState::Enabled);
            if ((wantEnable && g_mods[i].toggle != ToggleState::Enabled) ||
                (!wantEnable && g_mods[i].toggle == ToggleState::Enabled)) {
                SetModEnabled(i, wantEnable);
            }
        }
    }
    g_restartRequested = false;
    g_changedCount = 0;
    g_changedNames.clear();
    ScanToggleState();
    Log("[ModManager] revert done\n");
}

// ============================================================
// 隐藏/显示面板
// ============================================================
// 确认弹窗辅助：确认应用改动（重启）或放弃（保持运行）
static void ConfirmApply() {
    g_confirmDlg = false;
    Log("[ModManager] user confirmed %d changes, requesting restart\n",
        g_changedCount);
    RequestRestart();
}

static void ConfirmCancel() {
    g_confirmDlg = false;
    RevertChanges();
    // 撤销改动后关闭面板
    if (g_panel) {
        QolHudMarkHiddenByMod(g_panel);  // v1.2.6: MOD 主动隐藏，防回前台误还原
        ShowWindow(g_panel, SW_HIDE);
    }
    g_panelVisible = false;
    g_gpCursor = -1;
    g_gpFocus = 0;
    g_gpNeedsRelease = true;
    if (g_owner && IsWindow(g_owner)) { /* 全屏模式下不抢焦点，游戏从未失去前台 */ }
}

// 关闭面板（若期间有改动则先弹自绘确认窗）
static void HidePanel() {
    if (!g_panel || !IsWindowVisible(g_panel)) return;
    if (g_recModIdx >= 0) CancelRecordKey();  // 面板关闭时终止录制

    // 若期间改过开关 → 弹自绘确认窗（支持手柄）
    if (g_restartRequested && g_changedCount > 0) {
        g_confirmDlg = true;
        g_confirmSel = 1;   // 默认选"否"（保持运行）
        g_confirmDlgRelease = true;
        InvalidateRect(g_panel, nullptr, FALSE);
        Log("[ModManager] showing confirm dialog\n");
        return;  // 面板保持显示，绘制确认层
    }

    // 无改动：直接隐藏
    QolHudMarkHiddenByMod(g_panel);  // v1.2.6: MOD 主动隐藏，防回前台误还原
    ShowWindow(g_panel, SW_HIDE);
    g_panelVisible = false;
    g_gpCursor = -1;
    g_gpFocus = 0;
    g_gpNeedsRelease = true;
    if (g_owner && IsWindow(g_owner)) { /* 全屏模式下不抢焦点 */ }
    Log("[ModManager] panel hidden\n");
}

static bool ShowPanel() {
    if (!g_panel) return false;
    if (IsWindowVisible(g_panel)) return true;

    ScanModStatus();
    g_inSave = DetectInSave();

    // 记录本轮打开面板时的初始状态 + 重置修改跟踪
    for (int i = 0; i < g_modCount; ++i) g_mods[i].startToggle = g_mods[i].toggle;
    g_restartRequested = false;
    g_changedCount = 0;
    g_changedNames.clear();
    // 重置手柄导航状态
    g_gpCursor = -1;
    g_gpFocus = 0;
    g_gpNeedsRelease = true;
    g_confirmDlg = false;
    g_confirmDlgRelease = true;

    g_owner = FindGameWindow();
    // v1.2.10: 移除面板 owner 绑定（GWLP_HWNDPARENT）——owned TOPMOST 面板干扰
    // Alt+Tab 前台切换，游戏窗口切不回（切窗修复第四轮，详见 game_window.h v1.5）。
    // g_owner 仍用于下方 anchor 定位与显示器解析，故保留赋值。

    RECT anchor = {};
    if (!g_owner || !GetWindowRect(g_owner, &anchor)) {
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        HMONITOR mon = MonitorFromWindow(g_owner, MONITOR_DEFAULTTOPRIMARY);
        if (GetMonitorInfoW(mon, &info)) anchor = info.rcWork;
        else SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);
    }

    // ---- 自适应缩放：按游戏窗口/工作区计算 scale，重建字体与圆角 Region ----
    int winW = anchor.right - anchor.left;
    int winH = anchor.bottom - anchor.top;
    float s = 1.0f;
    if (winW > 0 && winH > 0) {
        float sw = (float)winW / 760.0f;
        float sh = (float)winH / 760.0f;
        s = (sw < sh) ? sw : sh;
        if (s > 1.0f) s = 1.0f;
        if (s < 0.5f) s = 0.5f;
    }
    if (s > g_scale + 0.001f || s < g_scale - 0.001f || !g_titleFont || !g_bodyFont || !g_smallFont) {
        g_scale = s;
        g_panelW = S(760);
        g_panelH = S(760);
        RebuildScaledFonts();
        HRGN rounded = CreateRoundRectRgn(0, 0, g_panelW + 1, g_panelH + 1, S(12), S(12));
        if (rounded && !SetWindowRgn(g_panel, rounded, FALSE)) DeleteObject(rounded);
        Log("[ModManager] adaptive panel applied\n");
    }
    int x = anchor.left + ((anchor.right - anchor.left) - PANEL_WIDTH) / 2;
    int y = anchor.top + ((anchor.bottom - anchor.top) - PANEL_HEIGHT) / 2;

    SetWindowPos(g_panel, HWND_TOPMOST, x, y, PANEL_WIDTH, PANEL_HEIGHT,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    InvalidateRect(g_panel, nullptr, FALSE);
    g_panelVisible = true;
    Log("[ModManager] panel shown inSave=%d\n", (int)g_inSave);
    return true;
}

static bool InitPanel();

static void TogglePanel() {
    if (g_panel && IsWindowVisible(g_panel)) {
        HidePanel();
    } else {
        if (!g_panel && !InitPanel()) {
            Log("[ModManager] InitPanel failed\n");
            return;
        }
        ShowPanel();
    }
}

// ============================================================
// 窗口过程
// ============================================================
static LRESULT CALLBACK PanelWndProc(HWND window, UINT message,
                                     WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(window, &paint);
        PaintPanel(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        short delta = (short)HIWORD(wParam);
        int maxScroll = g_visibleCount - VISIBLE_ROWS;
        if (maxScroll < 0) maxScroll = 0;
        if (delta > 0 && g_scrollTop > 0) g_scrollTop--;
        if (delta < 0 && g_scrollTop < maxScroll) g_scrollTop++;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        int newHover = -1;
        for (int d = g_scrollTop; d < g_scrollTop + VISIBLE_ROWS; ++d) {
            RECT r = RowRect(d);
            if (PtInRect(&r, pt)) { newHover = d; break; }
        }
        if (newHover != g_hoverRow) {
            g_hoverRow = newHover;
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    }
    case WM_KEYDOWN:
        // 确认弹窗打开时：Esc/空格 触发默认按钮
        if (g_confirmDlg) {
            if (wParam == VK_ESCAPE) { ConfirmCancel(); return 0; }
            if (wParam == VK_RETURN) {
                if (g_confirmSel == 0) ConfirmApply();
                else ConfirmCancel();
                return 0;
            }
            if (wParam == VK_LEFT || wParam == VK_RIGHT) {
                g_confirmSel = 1 - g_confirmSel;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            break;
        }
        // 录制模式下：捕获按键 / 选择阶段键盘操作
        if (g_recModIdx >= 0) {
            if (g_recStage == 1) {
                // 选择阶段：←/→ 切换（三选），Enter 确认，Esc 结束
                if (wParam == VK_ESCAPE) { CancelRecordKey(); return 0; }
                if (wParam == VK_LEFT) {
                    g_recNextSel = (g_recNextSel + 2) % 3;
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                if (wParam == VK_RIGHT) {
                    g_recNextSel = (g_recNextSel + 1) % 3;
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                if (wParam == VK_RETURN) {
                    if (g_recNextSel == 0) RecordNext();
                    else if (g_recNextSel == 1) RecordDeleteKey();
                    else CancelRecordKey();
                    return 0;
                }
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                CancelRecordKey();
                return 0;
            }
            // 忽略修饰键单独按下（等待组合键），但允许 F 键/数字键/字母键
            if (wParam == VK_SHIFT || wParam == VK_CONTROL || wParam == VK_MENU)
                return 0;
            g_capturedVk = (int)wParam;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_ESCAPE) { HidePanel(); return 0; }
        if (wParam == VK_UP)   { if (g_scrollTop > 0) g_scrollTop--; InvalidateRect(window, nullptr, FALSE); return 0; }
        if (wParam == VK_DOWN) { int m = g_visibleCount - VISIBLE_ROWS; if (m < 0) m = 0; if (g_scrollTop < m) g_scrollTop++; InvalidateRect(window, nullptr, FALSE); return 0; }
        break;
    case WM_KEYUP:
        // 选择阶段：按键已松开但不在捕获中，直接忽略
        if (g_recModIdx >= 0 && g_recStage == 1) break;
        // 录制完成：按键已松开 → 写入
        if (g_recModIdx >= 0 && g_capturedVk > 0) {
            int modIdx = g_recModIdx;
            int keyIdx = g_recKeyIdx;
            WORD vk = (WORD)g_capturedVk;
            // 映射到展示名
            wchar_t name[32];
            VkToName(vk, name, 32);
            // 更新该 MOD 的按键
            if (modIdx >= 0 && modIdx < g_modCount && keyIdx >= 0 && keyIdx < g_mods[modIdx].hotkeyCount) {
                wcscpy_s(g_mods[modIdx].hotkeys[keyIdx], name);
                // 更新 hotkey 字符串（由 hotkeys 数组重新拼回，供显示）
                std::wstring joined;
                for (int h = 0; h < g_mods[modIdx].hotkeyCount; ++h) {
                    if (h > 0) joined += L", ";
                    joined += g_mods[modIdx].hotkeys[h];
                }
                static wchar_t s_hkBuf[128];
                wcscpy_s(s_hkBuf, joined.c_str());
                g_mods[modIdx].hotkey = s_hkBuf;
                WriteHotkeysFile(modIdx);   // 写回 qol_hotkeys.txt
            }
            RecordKeyWritten();
            return 0;
        }
        break;
    case WM_LBUTTONUP: {
        POINT pt = { static_cast<short>(LOWORD(lParam)),
                     static_cast<short>(HIWORD(lParam)) };

        // 录制选择弹窗打开时：点击下一个/结束
        if (g_recModIdx >= 0 && g_recStage == 1) {
            RECT box, nextBtn, delBtn, endBtn;
            GetRecChoiceRects(&box, &nextBtn, &delBtn, &endBtn);
            if (PtInRect(&nextBtn, pt)) { RecordNext(); return 0; }
            if (PtInRect(&delBtn, pt))  { RecordDeleteKey(); return 0; }
            if (PtInRect(&endBtn, pt))  { CancelRecordKey(); return 0; }
            return 0;  // 点弹窗外不处理
        }

        // 确认弹窗打开时：点击按钮
        if (g_confirmDlg) {
            int dw = S(520), dh = S(220);
            int boxLeft = (PANEL_WIDTH - dw) / 2, boxTop = (PANEL_HEIGHT - dh) / 2;
            int btnW = S(180), btnH = S(40);
            int btnY = boxTop + dh - btnH - S(20);
            int gap = S(30);
            int totalW = btnW * 2 + gap;
            int startX = (PANEL_WIDTH - totalW) / 2;
            RECT btnYes = { startX, btnY, startX + btnW, btnY + btnH };
            RECT btnNo  = { startX + btnW + gap, btnY, startX + btnW * 2 + gap, btnY + btnH };
            if (PtInRect(&btnYes, pt)) { ConfirmApply(); return 0; }
            if (PtInRect(&btnNo, pt))  { ConfirmCancel(); return 0; }
            // 点框外视为取消
            RECT boxRect = { boxLeft, boxTop, boxLeft + dw, boxTop + dh };
            (void)boxRect;
            return 0;
        }

        // 关闭按钮
        RECT closeRect = { PANEL_WIDTH - S(46), S(16), PANEL_WIDTH - S(18), S(44) };
        if (PtInRect(&closeRect, pt)) { HidePanel(); return 0; }

        // 改键按钮点击 → 录制第一个热键槽（若当前无录制）
        for (int d = g_scrollTop; d < g_scrollTop + VISIBLE_ROWS; ++d) {
            int i = ArrayIdxOfDisplay(d);
            if (i < 0) continue;
            if (g_mods[i].hotkeyCount > 0 && !g_mods[i].isSelf &&
                (!g_mods[i].thirdParty || g_mods[i].rekeyable) &&
                PtInRect(&g_keyRects[i][6], pt)) {
                if (!g_inSave && g_recModIdx < 0) { BeginRecordKey(i, 0); }
                return 0;
            }
        }
        // 按键标签点击 → 开始录制（优先于整行点击）
        for (int d = g_scrollTop; d < g_scrollTop + VISIBLE_ROWS; ++d) {
            int i = ArrayIdxOfDisplay(d);
            if (i < 0) continue;
            if (g_mods[i].thirdParty && !g_mods[i].rekeyable) continue;  // 第三方不可改键 MOD 热键只读
            for (int h = 0; h < g_mods[i].hotkeyCount && h < 6; ++h) {
                if (PtInRect(&g_keyRects[i][h], pt)) {
                    if (!g_inSave) { BeginRecordKey(i, h); }
                    return 0;
                }
            }
        }
        // 行点击
        for (int d = g_scrollTop; d < g_scrollTop + VISIBLE_ROWS; ++d) {
            RECT r = RowRect(d);
            if (PtInRect(&r, pt)) {
                int i = ArrayIdxOfDisplay(d);
                if (i >= 0) ToggleMod(i);
                return 0;
            }
        }
        return 0;
    }
    case WM_CLOSE: HidePanel(); return 0;
    case WM_DESTROY:
        if (g_titleFont) { DeleteObject(g_titleFont); g_titleFont = nullptr; }
        if (g_bodyFont)  { DeleteObject(g_bodyFont);  g_bodyFont = nullptr; }
        if (g_smallFont) { DeleteObject(g_smallFont); g_smallFont = nullptr; }
        g_panel = nullptr;
        g_panelVisible = false;
        return 0;
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// ============================================================
// 初始化面板窗口
// ============================================================
static bool InitPanel() {
    if (g_panel) return true;

    WNDCLASSEXW cls = {};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = PanelWndProc;
    cls.hInstance = g_module;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    cls.lpszClassName = PANEL_CLASS;
    if (!RegisterClassExW(&cls)) {
        DWORD err = GetLastError();
        WNDCLASSEXW existing = {};
        existing.cbSize = sizeof(existing);
        if (err != ERROR_CLASS_ALREADY_EXISTS ||
            !GetClassInfoExW(g_module, PANEL_CLASS, &existing) ||
            existing.lpfnWndProc != PanelWndProc ||
            existing.hInstance != g_module) {
            Log("[ModManager] class register failed (err=%lu)\n", err);
            return false;
        }
    }

    RebuildScaledFonts();
    if (!g_titleFont || !g_bodyFont || !g_smallFont) {
        Log("[ModManager] font creation failed\n");
        if (g_titleFont) DeleteObject(g_titleFont);
        if (g_bodyFont) DeleteObject(g_bodyFont);
        if (g_smallFont) DeleteObject(g_smallFont);
        g_titleFont = g_bodyFont = g_smallFont = nullptr;
        return false;
    }

    g_owner = FindGameWindow();
    // v1.2.16: 面板 hWndParent 改 nullptr——owner 绑定 + TOPMOST + v1.6 失焦渐隐
    // （窗口保持可见但透明）= KB-075 同款切窗干扰（2026-10-06 用户实锤：
    // 开面板后 Alt+Tab 游戏切不回；其余 6 个 HUD 均已摘除 owner 唯面板漏网）。
    // g_owner 保留赋值仅供 anchor 定位/显示器解析。
    g_panel = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        PANEL_CLASS, L"Village Mod Manager", WS_POPUP,
        0, 0, PANEL_WIDTH, PANEL_HEIGHT,
        nullptr, nullptr, g_module, nullptr);
    if (!g_panel) {
        Log("[ModManager] CreateWindowExW failed (err=%lu)\n", GetLastError());
        DeleteObject(g_titleFont); DeleteObject(g_bodyFont); DeleteObject(g_smallFont);
        g_titleFont = g_bodyFont = g_smallFont = nullptr;
        return false;
    }

    SetLayeredWindowAttributes(g_panel, 0, 250, LWA_ALPHA);
    HRGN rounded = CreateRoundRectRgn(0, 0, PANEL_WIDTH + 1, PANEL_HEIGHT + 1, S(12), S(12));
    if (rounded && !SetWindowRgn(g_panel, rounded, FALSE)) DeleteObject(rounded);

    Log("[ModManager] panel init ok\n");
    return true;
}

// ============================================================
// QoL 快捷菜单（F1）：开关型 MOD 的轻量切换浮层
// ============================================================
static HWND g_quickMenu = nullptr;
static bool g_quickMenuVisible = false;
static bool g_quickMenuNeedsRelease = true;
static HFONT g_quickMenuFont = nullptr;

static constexpr int QM_MAX_ITEMS = 32;
static QolQuickMenuItem g_qmItems[QM_MAX_ITEMS];
static int g_qmCount = 0;
    static bool g_qmDigitPrev[6] = {};
static ULONGLONG g_qmLastPaint = 0;
static const wchar_t* g_qmClass = L"VillageQoLQuickMenu";
// v1.2.5: 标题与宽度测量共用同一文本（旧版测量少了“· R1 确认”导致
// 提示被截断显示不完全）；可见项上限与高度/点击/绘制统一（9 项）
static const wchar_t* g_qmTitle =
    L"QoL \x5FEB\x6377\x83DC\x5355  (F2/L1+R1/Esc \x5173\x95ED \x00B7 R1 \x786E\x8BA4)";
static constexpr int QM_VISIBLE_ITEMS = 9;

static int g_qmCursor = -1;  // v1.2.1: 手柄光标（-1=未选中）
static bool g_qmGpNeedsRelease = true;  // 手柄边沿触发
static ULONGLONG g_qmLastNavTick = 0;  // 手柄导航防抖

static LRESULT CALLBACK QmWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_LBUTTONDOWN) {
        // v1.2.1: 鼠标点击切换开关
        int mx = (short)LOWORD(lParam);
        int my = (short)HIWORD(lParam);
        if (my >= 46) {
            int idx = (my - 46) / 32;
            if (idx >= 0 && idx < g_qmCount && idx < 9) {
                if (g_qmItems[idx].toggle) {
                    g_qmItems[idx].toggle();
                    g_qmCursor = idx;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    g_qmLastPaint = GetTickCount64();
                }
            }
        }
        return 0;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(18, 18, 26));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        if (!g_quickMenuFont)
            g_quickMenuFont = CreateFontW(22, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei");
        HGDIOBJ oldFont = SelectObject(dc, g_quickMenuFont);
        SetBkMode(dc, TRANSPARENT);
        // 标题
        SetTextColor(dc, RGB(160, 160, 170));
        TextOutW(dc, 14, 8, g_qmTitle, (int)wcslen(g_qmTitle));
        // 分隔线
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(50, 50, 60));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, 14, 38, nullptr); LineTo(dc, rc.right - 14, 38);
        SelectObject(dc, oldPen); DeleteObject(pen);
        // 菜单项
        const int qmVis = g_qmCount < QM_VISIBLE_ITEMS ? g_qmCount : QM_VISIBLE_ITEMS;
        for (int i = 0; i < qmVis; ++i) {
            int y = 46 + i * 32;
            // v1.2.1: 手柄光标高亮
            if (i == g_qmCursor) {
                RECT hl = { 14, y - 2, rc.right - 14, y + 30 };
                HBRUSH hb = CreateSolidBrush(RGB(40, 50, 70));
                FillRect(dc, &hl, hb);
                DeleteObject(hb);
            }
            // v1.1.4: 去掉快捷键标签，只显示名称和开关
            wchar_t name[64];
            MultiByteToWideChar(CP_UTF8, 0, g_qmItems[i].name, -1, name, 64);
            SetTextColor(dc, RGB(230, 230, 230));
            TextOutW(dc, 18, y, name, (int)wcslen(name));  // v1.1.4: 去序号后左移
            bool on = g_qmItems[i].isOn ? g_qmItems[i].isOn() : false;
            // v1.2.3: 开关显示改为滑块（轨道+旋钮，与主面板一致），替代文字 [开]/[关]
            const int trackW = 44, trackH = 20;
            const int trackX = rc.right - 14 - trackW;
            const int trackY = y + (32 - trackH) / 2;
            RECT track = { trackX, trackY, trackX + trackW, trackY + trackH };
            HPEN oldPen0 = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
            HBRUSH trackBg = CreateSolidBrush(on ? RGB(104, 150, 88) : RGB(52, 56, 62));
            HBRUSH oldBr = (HBRUSH)SelectObject(dc, trackBg);
            RoundRect(dc, track.left, track.top, track.right, track.bottom, trackH, trackH);
            SelectObject(dc, oldBr); DeleteObject(trackBg);
            // 旋钮：开=靠右（白色），关=靠左（浅灰）
            const int knobD = 16;
            const int knobY = trackY + (trackH - knobD) / 2;
            int knobX = on ? (track.right - knobD - 2) : (track.left + 2);
            HBRUSH knobBg = CreateSolidBrush(on ? RGB(245, 245, 240) : RGB(150, 154, 160));
            SelectObject(dc, knobBg);
            Ellipse(dc, knobX, knobY, knobX + knobD, knobY + knobD);
            SelectObject(dc, oldBr); DeleteObject(knobBg);
            SelectObject(dc, oldPen0);
        }
        SelectObject(dc, oldFont);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) {
        g_quickMenu = nullptr;
        g_quickMenuVisible = false;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void QmCollect() {
    g_qmCount = 0;
    for (int i = 0; i < g_modCount && g_qmCount < QM_MAX_ITEMS; ++i) {
        if (!g_mods[i].dllName) continue;
        if (g_mods[i].toggle != ToggleState::Enabled &&
            g_mods[i].toggle != ToggleState::Self) continue;
        HMODULE h = GetModuleHandleW(g_mods[i].dllName);
        if (!h) continue;
        auto fn = (int (*)(QolQuickMenuItem*, int))
            GetProcAddress(h, "QolQuickMenuItems");
        if (!fn) continue;
        int n = fn(&g_qmItems[g_qmCount], QM_MAX_ITEMS - g_qmCount);
        if (n > 0) g_qmCount += n;
    }
    Log("[ModManager] quick menu: %d items collected\n", g_qmCount);
}

// 测量快捷菜单所需宽度（根据最长菜单项名称自适应）
static int QmCalcWidth() {
    if (!g_quickMenuFont)
        g_quickMenuFont = CreateFontW(22, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei");
    HDC screen = GetDC(nullptr);
    HGDIOBJ oldFont = SelectObject(screen, g_quickMenuFont);
    // 标题宽度（与绘制共用 g_qmTitle，保证窗口宽度 >= 实际文本宽度）
    SIZE ts; GetTextExtentPoint32W(screen, g_qmTitle, (int)wcslen(g_qmTitle), &ts);
    int maxW = ts.cx + 28;  // 标题 + 两侧 padding
    // 各菜单项宽度
    int qmVis = g_qmCount < QM_VISIBLE_ITEMS ? g_qmCount : QM_VISIBLE_ITEMS;
    for (int i = 0; i < qmVis; ++i) {
        wchar_t name[64];
        MultiByteToWideChar(CP_UTF8, 0, g_qmItems[i].name, -1, name, 64);
        SIZE ns; GetTextExtentPoint32W(screen, name, (int)wcslen(name), &ns);
        // 名称(从18px起) + 右侧状态(76px) + padding(28px)
        int itemW = 18 + ns.cx + 76 + 28;
        if (itemW > maxW) maxW = itemW;
    }
    SelectObject(screen, oldFont);
    ReleaseDC(nullptr, screen);
    if (maxW < 300) maxW = 300;  // 最小宽度
    return maxW;
}

static void QmShow() {
    QmCollect();
    const int qmVis = g_qmCount < QM_VISIBLE_ITEMS ? g_qmCount : QM_VISIBLE_ITEMS;
    int h = 46 + qmVis * 32 + 14;
    if (h < 90) h = 90;
    int w = QmCalcWidth();
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    if (!g_quickMenu) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = QmWndProc;
        wc.hInstance = g_module;
        wc.lpszClassName = g_qmClass;
        RegisterClassW(&wc);
        g_quickMenu = CreateWindowExW(
            // v1.2.6: 补 WS_EX_TOOLWINDOW——浮层菜单不是工具窗口会被
            // QolFindGameWindow 枚举误认成游戏窗口（F2 闪现即隐藏事故根因），
            // 顺带从 Alt+Tab 列表移除
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            g_qmClass, L"", WS_POPUP,
            sx, sy, w, h, nullptr, nullptr, g_module, nullptr);
        if (!g_quickMenu) { Log("[ModManager] quick menu create failed\n"); return; }
        SetLayeredWindowAttributes(g_quickMenu, RGB(0, 0, 0), 220, LWA_ALPHA);
    }
    SetWindowPos(g_quickMenu, HWND_TOPMOST, sx, sy, w, h,
                 SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    g_quickMenuVisible = true;
    memset(g_qmDigitPrev, 0, sizeof(g_qmDigitPrev));
    g_qmLastPaint = GetTickCount64();
    Log("[ModManager] quick menu shown (%d items, %dx%d)\n", g_qmCount, w, h);
}

static void QmHide() {
    if (g_quickMenu) {
        QolHudMarkHiddenByMod(g_quickMenu);  // v1.2.6: MOD 主动隐藏，防回前台误还原
        ShowWindow(g_quickMenu, SW_HIDE);
    }
    g_quickMenuVisible = false;
}

static void PollQuickMenu() {
    // F2 或手柄 L1+R1：快捷菜单（开关型 MOD 切换）
    bool f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    bool gpLR = PollGamepadL1R1();
    if (g_quickMenuNeedsRelease) {
        if (!f2 && !gpLR) g_quickMenuNeedsRelease = false;
    } else if (f2 || gpLR) {
        g_quickMenuNeedsRelease = true;
        if (g_quickMenuVisible) { QmHide(); return; }
        QmShow();
        return;
    }
    if (!g_quickMenuVisible || !g_quickMenu) return;

    // v1.2.1: 手柄导航（D-pad 上下选 + R1/Cross 确认）
    {
        ULONGLONG nowTick = GetTickCount64();
        bool canNav = (nowTick - g_qmLastNavTick) > 150;  // 150ms 防抖
        bool dpadUp = AnyGpPressed(GP_DPAD_UP);
        bool dpadDown = AnyGpPressed(GP_DPAD_DOWN);
        bool r1 = AnyGpPressed(GP_R1);
        bool cross = AnyGpPressed(GP_CROSS);
        bool circle = AnyGpPressed(GP_CIRCLE);

        // Circle 关闭
        if (circle) {
            if (g_qmGpNeedsRelease) {
                if (!circle) g_qmGpNeedsRelease = false;
            } else {
                QmHide();
                g_qmGpNeedsRelease = true;
                return;
            }
        }

        if (g_qmGpNeedsRelease) {
            if (!dpadUp && !dpadDown && !r1 && !cross && !circle)
                g_qmGpNeedsRelease = false;
        } else if (canNav) {
            if (dpadUp) {
                if (g_qmCursor < 0) g_qmCursor = 0;
                else if (g_qmCursor > 0) g_qmCursor--;
                g_qmLastNavTick = nowTick;
                g_qmGpNeedsRelease = true;
                InvalidateRect(g_quickMenu, nullptr, FALSE);
            } else if (dpadDown) {
                int maxIdx = (g_qmCount < 6) ? g_qmCount - 1 : 5;
                if (g_qmCursor < 0) g_qmCursor = 0;
                else if (g_qmCursor < maxIdx) g_qmCursor++;
                g_qmLastNavTick = nowTick;
                g_qmGpNeedsRelease = true;
                InvalidateRect(g_quickMenu, nullptr, FALSE);
            } else if ((r1 || cross) && g_qmCursor >= 0 && g_qmCursor < g_qmCount && g_qmCursor < 6) {
                if (g_qmItems[g_qmCursor].toggle) {
                    g_qmItems[g_qmCursor].toggle();
                    InvalidateRect(g_quickMenu, nullptr, FALSE);
                    g_qmLastPaint = GetTickCount64();
                }
                g_qmGpNeedsRelease = true;
            }
        }
    }

    // Esc 关闭（边沿）
    static bool escPrev = false;
    bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (esc && !escPrev) { QmHide(); escPrev = true; return; }
    if (!esc) escPrev = false;

    // 定时刷新状态显示（~300ms）
    ULONGLONG now = GetTickCount64();
    if (now - g_qmLastPaint > 300) {
        InvalidateRect(g_quickMenu, nullptr, FALSE);
        g_qmLastPaint = now;
    }
}

static void PumpQuickMenu() {
    if (!g_quickMenu) return;
    if (!QolGameInForeground()) { QolHudGuardVisibility(g_quickMenu); return; }  // v1.6: 失焦守卫兜底（alpha 渐隐）后 pump 静默
    MSG msg = {};
    while (PeekMessageW(&msg, g_quickMenu, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// 手柄 L3+R3 同时按下检测（用于打开 MOD 管理面板；v1.2.4 应用户要求
// L2+R2+↓ → L3+R3，因 L1+R1 已被快捷菜单占用）
static bool PollGamepadL3R3() {
    if (AnyGpPressed(GP_L3) && AnyGpPressed(GP_R3)) return true;
    // 路径 3: joyGetPosEx 同步回退（老设备；按钮位与 XInput 不同且驱动
    // 映射各异，保留旧 L2+R2+↓ 组合作为该路径的兼容开关键）
    if (!g_xinputReady && !(g_hidReady && g_gpHID.connected.load(std::memory_order_relaxed))) {
        JOYINFOEX ji = {};
        ji.dwSize = sizeof(ji);
        ji.dwFlags = JOY_RETURNBUTTONS | JOY_RETURNPOV;
        if (joyGetPosEx(JOYSTICKID1, &ji) == JOYERR_NOERROR) {
            DWORD pov = ji.dwPOV;
            bool dpadDown = (pov >= 13500 && pov < 22500);
            return (ji.dwButtons & 0x040) && (ji.dwButtons & 0x080) && dpadDown;
        }
    }
    return false;
}

// ============================================================
// 热键检测（键盘 F9 + 手柄 L3+R3）
// 注：v1.2.3 应用户要求 F11 → F9；v1.2.4 应用户要求手柄 L2+R2+↓ → L3+R3
// ============================================================
static void PollHotkey() {
    if (g_recModIdx >= 0) return;  // 录制中禁用热键，避免面板意外关闭

    // 键盘 F9
    bool f9down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    bool kbdPressed = false;
    if (g_needsRelease) {
        if (!f9down) g_needsRelease = false;
    } else if (f9down) {
        kbdPressed = true;
        g_needsRelease = true;
    }

    // 手柄 L3+R3
    bool gpDown = PollGamepadL3R3();
    bool gpPressed = false;
    if (g_gamepadNeedsRelease) {
        if (!gpDown) g_gamepadNeedsRelease = false;
    } else if (gpDown) {
        gpPressed = true;
        g_gamepadNeedsRelease = true;
    }

    if (kbdPressed || gpPressed) TogglePanel();
}

// ============================================================
// 消息泵
// ============================================================
static void PumpPanel() {
    if (!g_panel) return;
    if (!QolGameInForeground()) { QolHudGuardVisibility(g_panel); return; }  // v1.6: 失焦守卫兜底（alpha 渐隐）后 pump 静默
    MSG msg = {};
    while (PeekMessageW(&msg, g_panel, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ============================================================
// 申请重启：提示 + 自动关闭游戏
// ============================================================
static void RequestRestart() {
    Log("[ModManager] restart requested\n");
    // 显示重启提示（短暂重绘）
    if (g_panel && IsWindow(g_panel)) {
        InvalidateRect(g_panel, nullptr, FALSE);
        UpdateWindow(g_panel);
        Sleep(1200);
        QolHudMarkHiddenByMod(g_panel);  // v1.2.6: MOD 主动隐藏，防回前台误还原
        ShowWindow(g_panel, SW_HIDE);
        g_panelVisible = false;
    }

    // 找到游戏窗口，请求正常关闭（让其有机会保存）
    HWND wnd = FindGameWindow();
    if (wnd) {
        PostMessageW(wnd, WM_CLOSE, 0, 0);
        Sleep(150);
    }

    // v1.1.2: TerminateProcess 前检测存档是否正在写入
    // 通过尝试独占打开 save.001：能独占说明游戏没在写存档，可以安全终止
    // 若独占失败（SHARING_VIOLATION）说明游戏正在写入存档，等待其完成
    // 最多等待 5 秒，超时后强制终止（避免无限卡住）
    {
        wchar_t roam[MAX_PATH] = {};
        std::wstring saveFile;
        if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, roam)) {
            std::wstring saveRoot = std::wstring(roam) +
                L"\\Nippon Ichi Software, Inc\\Honogurashinoniwa";
            WIN32_FIND_DATAW fd = {};
            HANDLE hf = FindFirstFileW((saveRoot + L"\\*").c_str(), &fd);
            if (hf != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY &&
                        fd.cFileName[0] != L'.') {
                        saveFile = saveRoot + L"\\" + fd.cFileName + L"\\save.001";
                        break;
                    }
                } while (FindNextFileW(hf, &fd));
                FindClose(hf);
            }
        }

        if (!saveFile.empty()) {
            bool saveSafe = false;
            for (int attempt = 0; attempt < 50; ++attempt) {  // 50 × 100ms = 5 秒
                HANDLE h = CreateFileW(saveFile.c_str(),
                    GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    CloseHandle(h);
                    saveSafe = true;
                    Log("[ModManager] save file available, safe to terminate (attempt %d)\n", attempt);
                    break;
                }
                if (GetLastError() != ERROR_SHARING_VIOLATION) {
                    // 文件不存在或其他错误，不阻塞终止
                    saveSafe = true;
                    Log("[ModManager] save check skipped (error %lu), proceeding\n", GetLastError());
                    break;
                }
                Log("[ModManager] save file locked (writing), waiting... (attempt %d)\n", attempt);
                Sleep(100);
            }
            if (!saveSafe) {
                Log("[ModManager] WARNING: save still locked after 5s, forcing terminate\n");
            }
        } else {
            Log("[ModManager] no save file found, proceeding with terminate\n");
        }
    }

    // 兜底：直接结束进程（确保游戏退出）
    TerminateProcess(GetCurrentProcess(), 0);
}

// ============================================================
// 插件入口
// ============================================================
extern "C" __declspec(dllexport) void mod_init(void) {
    LogOpen("modmanager");
    Log("[ModManager] mod_init — v1.2.5 build 25311578 v1.20\n");

    // 初始化手柄输入（三路混合：XInput 线程 + HID 直读线程 + joyGetPosEx 回退）
    InitGamepad();

    g_gameDir    = GetGameDir();
    g_modsDir    = g_gameDir + L"\\Mods";
    g_storageDir = g_gameDir + L"\\\x5F85\x7528MOD";   // 待用MOD
    g_abandonDir = g_gameDir + L"\\\x5F03\x7528MOD";   // 弃用MOD
    g_hotkeyCachePath = g_gameDir + L"\\modmanager_hotkey_cache.txt";
    Log("[ModManager] gameDir=%ls\n", g_gameDir.c_str());

    if (GetFileAttributesW(g_storageDir.c_str()) == INVALID_FILE_ATTRIBUTES)
        CreateDirectoryW(g_storageDir.c_str(), nullptr);
    if (GetFileAttributesW(g_abandonDir.c_str()) == INVALID_FILE_ATTRIBUTES)
        CreateDirectoryW(g_abandonDir.c_str(), nullptr);

    ScanModFolders();
    CleanupStaleFolders();
    ScanModStatus();
    g_inSave = DetectInSave();

    Log("[ModManager] init done, %d mods tracked, inSave=%d\n",
        g_modCount, (int)g_inSave);
}

extern "C" __declspec(dllexport) void mod_tick(void) {
    if (!g_inSave && !g_panelVisible) {
        static int saveCheckCooldown = 0;
        if (saveCheckCooldown > 0) {
            saveCheckCooldown--;
        } else {
            g_inSave = DetectInSave();
            saveCheckCooldown = 120;  // 节流：每 ~2 秒检测一次（60fps）
        }
        if (g_inSave) Log("[ModManager] entered save, toggles locked\n");
    }
    PollHotkey();
    PollQuickMenu();
    // 手柄改键录制轮询（面板可见且正在录制时）
    if (g_panelVisible && g_recModIdx >= 0) {
        PollGamepadForRecording();
    }
    // 手柄面板导航轮询（面板可见时）
    if (g_panelVisible) {
        PollGamepadNav();
    }
    PumpPanel();
    PumpQuickMenu();
    // v1.2.6: 失焦隐藏面板/快捷菜单（不飘桌面）
    QolHudGuardVisibility(g_panel);
    QolHudGuardVisibility(g_quickMenu);
}

extern "C" __declspec(dllexport) void unload(void) {
    Log("[ModManager] unload\n");
    ShutdownGamepad();
    if (g_quickMenu && IsWindow(g_quickMenu)) {
        DestroyWindow(g_quickMenu);
        g_quickMenu = nullptr;
    }
    if (g_quickMenuFont) { DeleteObject(g_quickMenuFont); g_quickMenuFont = nullptr; }
    if (g_panel && IsWindow(g_panel)) {
        DestroyWindow(g_panel);
        g_panel = nullptr;
    }
    LogClose();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)lpReserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
