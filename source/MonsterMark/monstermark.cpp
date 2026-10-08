// monstermark.cpp —— MonsterMark: Ghost Rat / Treasure Box 地图标记 (v1.0.50 正式版：时间链修复+夜测通过)
//
// v1.0.36: 停用书/齿轮锚点标记（NightScanAnchors 不再调用）——0x1C1A00
// 无法区分"锚点有定义"与"今晚实际生成"，用户实测仍有空标（标识 3/4 无
// 东西）。暂只显示幽灵鼠/宝箱标记。待未来逆向"实际生成物列表"后再恢复。
//
// 从 dinput8.cpp 原始 .inl 提取的独立 DLL 插件。
// 实现逻辑见 night_map_markers.inl（已更新至 build 25311578 v1.20 RVA + byte array）。
// v1.0.40-diag: 40 个 expected 字节数组从新 exe 实读重新生成；OFFSET_X/Y 重定位；
//           内联 0x2DA410→0x2E9E63；回调三件套 vtable 铁证修正；SHA/大小指纹更新。
//
// 本文件提供 .inl 所需的全部外部依赖桩函数。
// .inl 在文件末尾通过 #include 引入。

#define MONSTERMARK_VERSION L"v1.0.50"

#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <cmath>
#include <new>

// ============================================================
// 类型别名（dinput8.cpp 中的定义）
// ============================================================
using u64 = std::uint64_t;
using u32 = std::uint32_t;

// ============================================================
// 日志（QoL_Shared）
// ============================================================
#include "logging.h"
#include "budget.h"        // P0 帧耗时探针（跨 DLL 共享）
#include "state.h"         // P1-2 状态感知暂停（载入/菜单静默）
#include "hot_config.h"

// MonsterMark 日志开关：诊断版开启（已验证正常后关闭）
// #define MONSTERMARK_LOGGING  // v1.0.50 正式版关闭（诊断能力保留在源码，夜间标记复发时重开宏重编即可）
#ifdef MONSTERMARK_LOGGING
  // 使用 QoL_Shared 的日志系统
#else
  #define LogOpen(x)  ((void)0)
  #define Log(...)    ((void)0)
  #define LogV(...)   ((void)0)
  #define LogClose()  ((void)0)
#endif

// ============================================================
// g_supportedExe — 新 build 指纹验证
// ============================================================
static bool g_supportedExe = false;

// ============================================================
// RVA_GAME_ROOT — .inl 通过 RVA_GAME_ROOT 引用（与 ChestSort 一致）
// ============================================================
static constexpr uintptr_t RVA_GAME_ROOT = 0x10FCBB0;  // build 25311578 (v1.20)

// ============================================================
// IsReadable — 内存可读检查（与 dinput8.cpp 一致）
// ============================================================
static bool IsReadable(const void* pointer, size_t size) {
    if (!pointer || size == 0) return false;
    uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    if (start < 0x10000 || start > 0x00007FFFFFFFFFFFULL) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(pointer, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t regionEnd = regionStart + mbi.RegionSize;
    if (start + size < start || start + size > regionEnd) return false;
    return true;
}

// ============================================================
// PackageWritesAuthorized — 独立 DLL 无包验证，直接允许
// ============================================================
static bool PackageWritesAuthorized() {
    return true;
}

static void PackageOpenWriteGate() {
    // 独立 DLL：无需包验证
}

// ============================================================
// WriteCodePatchChecked — 安全代码补丁（与 code_patch_safety.inl 一致）
// ============================================================
static bool WriteCodePatchChecked(void* address, const void* bytes,
                                  size_t size) {
    if (!PackageWritesAuthorized() ||
        !address || !bytes || size == 0) return false;
    DWORD previousProtection = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE,
                        &previousProtection)) return false;
    memcpy(address, bytes, size);
    const bool flushed = FlushInstructionCache(
        GetCurrentProcess(), address, size) != 0;
    DWORD temporaryProtection = 0;
    const bool restored = VirtualProtect(
        address, size, PAGE_EXECUTE_READ, &temporaryProtection) != 0;
    return flushed && restored;
}

// ============================================================
// SealExecutableMemory — 将内存标记为 RX
// ============================================================
static bool SealExecutableMemory(void* address, size_t size) {
    DWORD previousProtection = 0;
    if (!PackageWritesAuthorized() ||
        !address || size == 0 ||
        !VirtualProtect(address, size, PAGE_EXECUTE_READ,
                        &previousProtection)) return false;
    return FlushInstructionCache(GetCurrentProcess(), address, size) != 0;
}

// ============================================================
// CodeDirectCallTargets — 验证 E8 call 目标
// ============================================================
static bool CodeDirectCallTargets(uintptr_t callSite,
                                  uintptr_t expectedTarget) {
    if (!IsReadable(reinterpret_cast<void*>(callSite), 5) ||
        *reinterpret_cast<const unsigned char*>(callSite) != 0xe8) return false;
    std::int32_t displacement = 0;
    memcpy(&displacement, reinterpret_cast<void*>(callSite + 1),
           sizeof(displacement));
    return callSite + 5 + static_cast<intptr_t>(displacement) == expectedTarget;
}

// ============================================================
// CodeRangeHasExpectedProtection — 验证地址范围页保护
// ============================================================
static bool CodeRangeHasExpectedProtection(void* address, size_t size) {
    if (!address || size == 0) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 ||
        (mbi.Protect & 0xff) != PAGE_EXECUTE_READ) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t regionStart =
        reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (mbi.RegionSize > UINTPTR_MAX - regionStart ||
        size > UINTPTR_MAX - start) return false;
    return start >= regionStart && start + size <= regionStart + mbi.RegionSize;
}

// ============================================================
// AllocateExecutableNear — 在 target ±2GB 范围分配可执行内存
// ============================================================
static void* AllocateExecutableNear(uintptr_t target, size_t size) {
    SYSTEM_INFO info = {};
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t range = 0x7fff0000ull;
    const uintptr_t minimum = target > range ? target - range :
        reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t maximumByRange = target + range;
    const uintptr_t maximumSystem =
        reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t maximum = maximumByRange < maximumSystem ?
        maximumByRange : maximumSystem;

    uintptr_t address = minimum & ~(granularity - 1);
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION region = {};
        if (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) == 0) {
            break;
        }
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(region.BaseAddress);
        const uintptr_t regionEnd = regionBase + region.RegionSize;
        if (region.State == MEM_FREE) {
            const uintptr_t candidate =
                (regionBase + granularity - 1) & ~(granularity - 1);
            if (candidate >= minimum && candidate + size <= regionEnd &&
                candidate + size <= maximum) {
                void* allocated = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                                               MEM_COMMIT | MEM_RESERVE,
                                               PAGE_READWRITE);
                if (allocated) {
                    const std::int64_t displacement =
                        static_cast<std::int64_t>(reinterpret_cast<uintptr_t>(allocated)) -
                        static_cast<std::int64_t>(target + 5);
                    if (displacement >= INT32_MIN && displacement <= INT32_MAX) {
                        return allocated;
                    }
                    VirtualFree(allocated, 0, MEM_RELEASE);
                }
            }
        }
        if (regionEnd <= address) break;
        address = regionEnd;
    }
    return nullptr;
}

// ============================================================
// SpatialTestFunction — .inl 通过 g_originalSpatialTest 引用
// ============================================================
using SpatialTestFunction = bool (__fastcall *)(void*, void*, void*, int, int);
static SpatialTestFunction g_originalSpatialTest = nullptr;

// ============================================================
// build 指纹验证
// ============================================================
#include <bcrypt.h>

static bool VerifyExeBuild() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    HANDLE hFile = CreateFileW(exePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        Log("[MonsterMark] VerifyExeBuild: CreateFileW failed (err=%lu path=%ls)\n",
            GetLastError(), exePath);
        return false;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart != 18305544) {
        CloseHandle(hFile);
        return false;
    }

    // SHA-256 via BCrypt
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        CloseHandle(hFile);
        return false;
    }
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        CloseHandle(hFile);
        return false;
    }

    unsigned char buf[65536];
    DWORD bytesRead;
    while (ReadFile(hFile, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
        BCryptHashData(hHash, buf, bytesRead, 0);
    }
    CloseHandle(hFile);

    unsigned char hash[32];
    BCryptFinishHash(hHash, hash, sizeof(hash), 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    // build 25311578 (v1.20) SHA-256
    static const unsigned char expected[32] = {
        0xAC, 0x7D, 0x95, 0x5B, 0xAA, 0x91, 0x59, 0x29,
        0xB1, 0x85, 0xDF, 0x85, 0xCF, 0xA1, 0x3F, 0x7E,
        0x81, 0xAD, 0x57, 0xA9, 0xC6, 0x8F, 0x30, 0x8D,
        0xA7, 0x69, 0x73, 0x97, 0x75, 0x86, 0x9A, 0x05
    };

    bool match = memcmp(hash, expected, sizeof(hash)) == 0;
    if (match) {
        Log("[MonsterMark] village.exe verified: build 25311578 (1.20)\n");
    } else {
        Log("[MonsterMark] village.exe SHA-256 mismatch; feature disabled\n");
    }
    return match;
}

// ============================================================
// 前向声明（实现在 night_map_markers.inl 中）
// ============================================================
static bool InstallNightMapMarkers();
static bool InstallNightAnchorPosHook();
static void NightDiagScanCallbacks();
static void NightDiagEnumerateHashTable();
void NightHotConfigRegisterAll();  // HotConfig 注册（实现在 .inl 末尾）

// ============================================================
// 插件入口
// ============================================================

extern "C" __declspec(dllexport) void mod_init(void) {
    LogOpen("monstermark");
    Log("[MonsterMark] mod_init — build 25311578 v1.20 %S\n", MONSTERMARK_VERSION);
    QolRegisterHotKey("monstermark", "none");  // 无热键（自动生效）

    NightHotConfigRegisterAll();  // 在 .inl 中实现（可访问 static 变量）
    // v1.0.44 修复：hook 安装前先加载 JSON——HotConfig_Register 的默认值
    // 部分仍是 v1.09 旧值（覆盖了源码初值），若不先 Poll，InstallNightMapMarkers
    // 会用旧 RVA 装错位置（夜测无效的根因：装上后改变量也不会重装 hook）。
    HotConfig_Poll();  // 首次调用立即加载 QoL_hot.json / 覆盖文件
    HotConfig_DumpCE("monstermark");

    g_supportedExe = VerifyExeBuild();
    if (!g_supportedExe) {
        Log("[MonsterMark] unsupported exe; feature disabled safely\n");
        return;
    }

    PackageOpenWriteGate();

    bool ready = InstallNightMapMarkers();
    Log("[MonsterMark] InstallNightMapMarkers = %s\n", ready ? "OK" : "FAILED");
    if (ready) {
        bool anchorReady = InstallNightAnchorPosHook();
        Log("[MonsterMark] InstallNightAnchorPosHook = %s\n",
            anchorReady ? "OK" : "FAILED");
    }
}

extern "C" __declspec(dllexport) void mod_tick(void) {
    qol::budget::BeginFrame();  // P0: 帧锚定（幂等，多 DLL 安全）
    static int s_bdgSlot = -1;  // P0 探针（惰性注册，析构自动上报）
    if (s_bdgSlot < 0) s_bdgSlot = qol::budget::Slot("MonsterMark");
    struct BdgGuard {
        int slot; uint64_t t0;
        ~BdgGuard() { qol::budget::Report(slot, qol::budget::NowUs() - t0); }
    } bdgGuard = { s_bdgSlot, qol::budget::NowUs() };

    if (QolGameBusy()) return;  // P1-2: 载入/菜单期间静默
    HotConfig_Poll();  // 每 ~1s 检查 QoL_hot.json mtime，变化则重载
}

BOOL APIENTRY DllMain(HMODULE /*h*/, DWORD reason, LPVOID /*lpReserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        // 初始化推迟到 mod_init
    }
    return TRUE;
}

// ============================================================
// 引入 .inl 实现（所有外部依赖已在上方提供）
// ============================================================
#include "night_map_markers.inl"
