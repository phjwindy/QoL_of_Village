#include "version_manifest.h"
#include <windows.h>
#include <wincrypt.h>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <atomic>
#include <new>

// ============================================================
// Type aliases (defined in dinput8.cpp)
// ============================================================
using u64 = std::uint64_t;
using u32 = std::uint32_t;

// ============================================================
// 日志（QoL_Shared）
// ============================================================
#include "logging.h"
#include "hook.h"   // v1.5.35: 原生接口探针
#include "memory_cache.h"  // v1.1.27: FastRegion 区域缓存
#include "hotkey.h"        // 统一热键机制（HUD 切换键）
#include "budget.h"
#include "obs.h"              // v1.6.x: 统一观测层（AI 可读寄存器 + toast）        // v1.1.53: P0 帧预算/耗时探针（跨 DLL 共享）

static int s_productionBudgetSlot = -1;  // v1.1.53: P0 探针槽位

// 日志运行时开关（2026-10-24）：编译期宏退役，改由 QoL_Shared 运行时门控。
// 开启方式：游戏根 qol_log.cfg 写 level=1（约 1s 生效，免重编免重启）；缺省/level=0 为静默。

// 性能止血 v1.1.60-diag-patch1：scan_yield 中间日志门控（默认关）
// 每 36ms 一条、744 条/分钟占日志 921KB；调试时 #define DIAG_SCAN_YIELD 1 开启
#ifndef DIAG_SCAN_YIELD
#define DIAG_SCAN_YIELD 0
#endif

// ============================================================
// 加密日志（inl 的 ProductionLog 宏依赖）
// dinput8.cpp 的 SecureLog 使用加密格式，这里同样实现
// ============================================================
template<size_t N>
struct EncodedLogFormat {
    unsigned char bytes[N] = {};

    consteval EncodedLogFormat(const char (&plain)[N]) {
        for (size_t index = 0; index < N; ++index) {
            const unsigned char mask = static_cast<unsigned char>(
                0x5d + 0x29 * index + 0x13 * index * index + 0x0b * N);
            bytes[index] = static_cast<unsigned char>(plain[index]) ^ mask;
        }
    }
};

template<EncodedLogFormat encoded, typename... Args>
static void SecureLog(Args... args) {
    char format[sizeof(encoded.bytes)] = {};
    const volatile unsigned char* source = encoded.bytes;
    for (size_t index = 0; index < sizeof(encoded.bytes); ++index) {
        const unsigned char mask = static_cast<unsigned char>(
            0x5d + 0x29 * index + 0x13 * index * index +
            0x0b * sizeof(encoded.bytes));
        format[index] = static_cast<char>(source[index] ^ mask);
    }
    Log(format, args...);
}

// ============================================================
// g_supportedExe -- build 指纹验证
// ============================================================
static bool g_supportedExe = false;

// ============================================================
// g_module -- .inl 通过 g_module 引入
// ============================================================
static HMODULE g_module = nullptr;

// ============================================================
// IsReadable -- 内存可读检查（与 dinput8.cpp 一致）
// ============================================================
static bool IsReadable(const void* pointer, size_t size) {
    return qol_mem::IsReadable(pointer, size);
}

// ============================================================
// PackageWritesAuthorized -- 独立 DLL 无包验证，直接允许
// ============================================================
static bool PackageWritesAuthorized() {
    return true;
}

static void PackageOpenWriteGate() {
    // standalone DLL: no package validation needed
}

// ============================================================
// WriteCodePatchChecked -- 安全代码补丁（与 code_patch_safety.inl 一致）
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
// SealExecutableMemory -- 将内存标记为 RX
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
// CodeDirectCallTargets -- 检查 E8 call 目标
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
// CodeRangeHasExpectedProtection -- 验证地址范围页保护
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
// CodeMsvcTypeMatches -- RTTI 类型校验（与 code_patch_safety.inl 一致）
// ============================================================
static bool CodeMsvcTypeMatches(uintptr_t imageBase, uintptr_t vtableRva,
                                 uintptr_t colRva, uintptr_t typeRva,
                                 const char* decoratedTypeName) {
    if (!imageBase || !decoratedTypeName) return false;
    const uintptr_t colSlot = imageBase + vtableRva - sizeof(void*);
    const char* mappedName = reinterpret_cast<const char*>(
        imageBase + typeRva + 16);
    const size_t nameSize = strlen(decoratedTypeName) + 1;
    return IsReadable(reinterpret_cast<void*>(colSlot), sizeof(void*)) &&
           IsReadable(mappedName, nameSize) &&
           *reinterpret_cast<const uintptr_t*>(colSlot) == imageBase + colRva &&
           memcmp(mappedName, decoratedTypeName, nameSize) == 0;
}

// ============================================================
// AllocateExecutableNear -- 在 target 2GB 范围分配可执行内存
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
// g_runtimeWriteIntegrityFault -- .inl 可能引用
// ============================================================
static std::atomic<bool> g_runtimeWriteIntegrityFault{false};

// ============================================================
// build fingerprint verification
// exe 大小与 SHA-256 来自 version_manifest.h
// ============================================================
#include <bcrypt.h>
static bool VerifyExeBuild() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    HANDLE hFile = CreateFileW(exePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        Log("[ProductionAuto] VerifyExeBuild: CreateFileW failed (err=%lu path=%ls)\n",
            GetLastError(), exePath);
        return false;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) ||
        fileSize.QuadPart != SUPPORTED_EXE_SIZE) {
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

    static const unsigned char expected[] = SUPPORTED_EXE_SHA256;

    bool match = memcmp(hash, expected, sizeof(hash)) == 0;
    if (match) {
        Log("[ProductionAuto] village.exe verified: build %s (%s)\n",
            SUPPORTED_BUILD_NUMBER, SUPPORTED_GAME_VERSION);
    } else {
        Log("[ProductionAuto] village.exe SHA-256 mismatch; feature disabled\n");
    }
    return match;
}

// ============================================================
// Constants: external definitions needed by the .inl
// RVA 来自 version_manifest.h
// ============================================================
// RVA_GAME_ROOT 与 RVA_PLACEMENT_SPATIAL_TEST 已在 version_manifest.h 中定义
// Note: GIMMICK_DATA_HOLDER_OFFSET / GIMMICK_MODULE_NAME_OFFSET / GIMMICK_POSITION_OFFSET
// 已在 version_manifest.h 中定义
// ============================================================
// StatusTypeInfo / GimmickKind -- .inl 引用的类型
// ============================================================
enum class GimmickKind : unsigned char {
    Unresolved,
    Scarecrow,
    Sprinkler,
    Other,
};

struct StatusTypeInfo {
    GimmickKind kind;
    void* holder;
    void* data;
    char moduleName[48];
};

// ============================================================
// SpatialTestFunction -- .inl 通过 g_originalSpatialTest 引入
// ============================================================
using SpatialTestFunction = bool (__fastcall *)(void*, void*, void*, int, int);
static SpatialTestFunction g_originalSpatialTest = nullptr;

// ============================================================
// FindSettingsOwner -- 移至 settings_panel.inl
// ============================================================
static HWND g_settingsOwner = nullptr;
static HWND g_toastWindow = nullptr;

static BOOL CALLBACK FindSettingsOwnerCallback(HWND window, LPARAM data) {
    if (window == g_toastWindow || !IsWindowVisible(window)) {
        return TRUE;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId()) return TRUE;
    LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extendedStyle & WS_EX_TOOLWINDOW) != 0 || GetWindow(window, GW_OWNER)) return TRUE;
    *reinterpret_cast<HWND*>(data) = window;
    return FALSE;
}

static HWND FindSettingsOwner() {
    HWND foreground = GetForegroundWindow();
    DWORD processId = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &processId);
    if (foreground && processId == GetCurrentProcessId() &&
        foreground != g_toastWindow) {
        return foreground;
    }
    if (g_settingsOwner && IsWindow(g_settingsOwner)) return g_settingsOwner;
    HWND found = nullptr;
    EnumWindows(FindSettingsOwnerCallback, reinterpret_cast<LPARAM>(&found));
    return found;
}

// ============================================================
// ShowToast -- 简化版 toast（无 GUI，仅写日志）
// 完整版 toast 依赖 settings_panel 基础设施，独立 DLL 中省略
// ============================================================

static void ShowToast(const wchar_t* message, COLORREF /*accent*/) {
    Log("[ProductionAuto][toast] %ls\n", message ? message : L"");
}

// ============================================================
// Production 前后台函数
// 独立 DLL 用 DirectInput 代理，简化实现
// ============================================================
static bool ProductionModalApiIsolationIsReady() {
    // 用 DirectInput 代理，直接允许（面板无模态输入隔离）
    return true;
}

static bool ProductionForegroundIsGameWindow() {
    HWND foreground = GetForegroundWindow();
    if (!foreground || !IsWindowVisible(foreground)) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    if (processId != GetCurrentProcessId()) return false;
    const LONG_PTR extendedStyle = GetWindowLongPtrW(foreground, GWL_EXSTYLE);
    return (extendedStyle & WS_EX_TOOLWINDOW) == 0 &&
           GetWindow(foreground, GW_OWNER) == nullptr;
}

static bool ProductionForegroundSupportsProductionModal() {
    return ProductionForegroundIsGameWindow();
}

static bool ProductionFeatureIsEnabled() {
    // 独立 DLL：默认启用
    return true;
}

// ============================================================
// GameClock 发布器 -- hook CGameTime::advance 主世界调用点
// 移植自 SaveBackup 的简化实现（不减速，只发布时间）
// ============================================================

// GameClock RVA 常量已在 version_manifest.h 中定义为宏，此处不再重复定义

static constexpr size_t GAME_TIME_RELAY_SIZE = 64;

static constexpr u64 RAW_SECONDS_PER_DAY = 86400;
static constexpr u64 RAW_SECONDS_PER_HOUR = 3600;
static constexpr u32 RAW_DAY_START_HOUR = 7;

struct GameTimeState {
    u64 tickRemainder;
    std::int64_t currentRawSecond;
    std::int64_t previousRawSecond;
};
static_assert(sizeof(GameTimeState) == 24, "verified CGameTime prefix size changed");

using GameTimeAdvanceFunction = u64 (__fastcall *)(GameTimeState*, u64);

// 签名校验数组（来自 version_manifest.h）
static const unsigned char expectedAdvance[] = SIG_GAME_TIME_ADVANCE;
static const unsigned char expectedHourDecode[] = SIG_GAME_TIME_HOUR_DECODE;
static const unsigned char expectedSerializedLayout[] = SIG_GAME_TIME_SERIALIZED_LAYOUT;
static const unsigned char expectedLocalGate[] = SIG_GAME_TIME_LOCAL_GATE;
static const unsigned char expectedLocalCall[] = SIG_GAME_TIME_LOCAL_CALL;
static const unsigned char expectedMainUpdateParameter[] = SIG_GAME_TIME_MAIN_UPDATE_PARAMETER;
static const unsigned char expectedMainCallContext[] = SIG_GAME_TIME_MAIN_CALL_CONTEXT;
static const unsigned char expectedFrameRegistration[] = SIG_GAME_TIME_FRAME_REGISTRATION;
static const unsigned char expectedFrameWrapper[] = SIG_GAME_TIME_FRAME_WRAPPER;
static const unsigned char expectedMainCall[] = SIG_GAME_TIME_MAIN_CALL;

// ---- GameClock publisher atomic variables ----
// 注意：inl 需要的 g_productionMainWorldGameClockSourceReady 和
// g_productionMainWorldGameClockWakeSequence，其他原子变量为本地实现
static std::atomic<u64> g_productionMainWorldGameClockSequence{0};
static std::atomic<std::int64_t> g_productionMainWorldGameSecond{0};
static std::atomic<u64> g_productionMainWorldGameClockWorldEpoch{0};
static std::atomic<u64> g_productionMainWorldGameClockLoadGeneration{
    UINT64_MAX};
static std::atomic<bool> g_productionMainWorldGameClockSourceReady{false};
static std::atomic<u64> g_productionMainWorldGameClockWakeSequence{0};

// ---- Hook 状态 ----
static std::atomic<GameTimeAdvanceFunction> g_originalGameTimeAdvance{nullptr};
static bool g_midnightSlowHookReady = false;
static void* g_midnightSlowRelay = nullptr;

// ---- 简化的时间发布（只需 worldEpoch 和 loadGeneration 作为一致性标记）----
static std::atomic<u64> g_autoPetLoadInFlightGeneration{0};
static std::atomic<u64> g_autoPetLoadCompletionGeneration{0};
static std::atomic<u64> g_autoPetLoadReadyGeneration{0};
static std::atomic<u64> g_autoPetHandledLoadGeneration{0};
// AutoPet 早间就绪钩子用的代际计数（原定义在 auto_pet.inl）
static std::atomic<u64> g_autoPetMorningReadyGeneration{0};
// 世界时钟发布器需在 .inl include 之前引用；与 .inl 同一编译层（static），
// 故在此提前定义，production_automation_v2.inl 内不得再定义同名变量。
static std::atomic<u64> g_productionWorldScheduleEpoch{0};

// ============================================================
// ProductionReadPublishedMainWorldGameClock
// 从原子变量读取发布的游戏时间（与 dinput8.cpp 一致）
// ============================================================
static bool ProductionReadPublishedMainWorldGameClock(
        u64 expectedWorldEpoch, u64 expectedLoadGeneration,
        std::int64_t* gameSecondOut) {
    if (!gameSecondOut ||
        !g_productionMainWorldGameClockSourceReady.load(
            std::memory_order_acquire)) return false;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const u64 before = g_productionMainWorldGameClockSequence.load(
            std::memory_order_acquire);
        if (before == 0 || (before & 1ULL) != 0) continue;
        const std::int64_t gameSecond =
            g_productionMainWorldGameSecond.load(std::memory_order_relaxed);
        const u64 worldEpoch =
            g_productionMainWorldGameClockWorldEpoch.load(
                std::memory_order_relaxed);
        const u64 loadGeneration =
            g_productionMainWorldGameClockLoadGeneration.load(
                std::memory_order_relaxed);
        const u64 after = g_productionMainWorldGameClockSequence.load(
            std::memory_order_acquire);
        if (before != after || (after & 1ULL) != 0) continue;
        if (worldEpoch != expectedWorldEpoch ||
            loadGeneration != expectedLoadGeneration) return false;
        *gameSecondOut = gameSecond;
        return true;
    }
    return false;
}

// ============================================================
// ProductionPublishMainWorldGameClock
// 在 CGameTime::advance 返回后发布游戏时间到原子变量
// ============================================================
static void ProductionPublishMainWorldGameClock(GameTimeState* state) {
    if (!state ||
        !g_productionMainWorldGameClockSourceReady.load(
            std::memory_order_acquire)) return;

    u64 version = g_productionMainWorldGameClockSequence.load(
        std::memory_order_acquire);
    if ((version & 1ULL) != 0 || version > UINT64_MAX - 2 ||
        !g_productionMainWorldGameClockSequence.compare_exchange_strong(
            version, version + 1, std::memory_order_acq_rel,
            std::memory_order_acquire)) return;

    g_productionMainWorldGameSecond.store(
        state->currentRawSecond, std::memory_order_relaxed);
    g_productionMainWorldGameClockWorldEpoch.store(
        g_productionWorldScheduleEpoch.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    g_productionMainWorldGameClockLoadGeneration.store(
        g_autoPetLoadCompletionGeneration.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    g_productionMainWorldGameClockSequence.store(
        version + 2, std::memory_order_release);
}

static u64 ProductionCallOriginalAndPublishMainWorldGameClock(
    GameTimeAdvanceFunction original, GameTimeState* state,
    u64 elapsedTicks) {
    const u64 result = original(state, elapsedTicks);
    ProductionPublishMainWorldGameClock(state);
    return result;
}

// ============================================================
// GameTimeAdvanceDetour -- 简化版 detour
// 不减速，只发布时间
// ============================================================
static u64 __fastcall GameTimeAdvanceDetour(
    GameTimeState* state, u64 elapsedTicks) {
    GameTimeAdvanceFunction original =
        g_originalGameTimeAdvance.load(std::memory_order_acquire);
    if (!original) return 0;
    return ProductionCallOriginalAndPublishMainWorldGameClock(
        original, state, elapsedTicks);
}

// ============================================================
// InstallGameClockHook -- 安装时钟 hook
// 签名校验 + relay 跳板 + callsite 补丁
// ============================================================
static bool InstallGameClockHook() {
    g_productionMainWorldGameClockSourceReady.store(
        false, std::memory_order_release);
    g_productionMainWorldGameClockSequence.store(0,
        std::memory_order_release);
    g_productionMainWorldGameSecond.store(0, std::memory_order_relaxed);
    g_productionMainWorldGameClockWorldEpoch.store(
        0, std::memory_order_relaxed);
    g_productionMainWorldGameClockLoadGeneration.store(
        UINT64_MAX, std::memory_order_relaxed);

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) return false;
    unsigned char* mainCall = reinterpret_cast<unsigned char*>(
        base + RVA_GAME_TIME_MAIN_CALL);

    const bool advanceOk = memcmp(reinterpret_cast<void*>(
        base + RVA_GAME_TIME_ADVANCE), expectedAdvance,
        sizeof(expectedAdvance)) == 0;
    const bool hourDecodeOk = memcmp(reinterpret_cast<void*>(
        base + RVA_GAME_TIME_HOUR_DECODE), expectedHourDecode,
        sizeof(expectedHourDecode)) == 0;
    const bool localExcludedOk = memcmp(reinterpret_cast<void*>(
        base + RVA_GAME_TIME_LOCAL_GATE), expectedLocalGate,
        sizeof(expectedLocalGate)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_GAME_TIME_LOCAL_CALL_CONTEXT),
               expectedLocalCall, sizeof(expectedLocalCall)) == 0 &&
        CodeDirectCallTargets(base + RVA_GAME_TIME_LOCAL_CALL,
                              base + RVA_GAME_TIME_ADVANCE);
    const bool mainCallOk = memcmp(reinterpret_cast<void*>(
        base + RVA_GAME_TIME_MAIN_UPDATE_PARAMETER), expectedMainUpdateParameter,
        sizeof(expectedMainUpdateParameter)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_GAME_TIME_MAIN_CALL_CONTEXT),
               expectedMainCallContext, sizeof(expectedMainCallContext)) == 0 &&
        CodeDirectCallTargets(base + RVA_GAME_TIME_MAIN_CALL,
                              base + RVA_GAME_TIME_ADVANCE);
    const bool frameChainOk = memcmp(reinterpret_cast<void*>(
        base + RVA_GAME_TIME_FRAME_REGISTRATION), expectedFrameRegistration,
        sizeof(expectedFrameRegistration)) == 0 &&
        memcmp(reinterpret_cast<void*>(base + RVA_GAME_TIME_FRAME_WRAPPER_CONTEXT),
               expectedFrameWrapper, sizeof(expectedFrameWrapper)) == 0 &&
        CodeDirectCallTargets(base + RVA_GAME_TIME_FRAME_WRAPPER_CALL,
                              base + RVA_GAME_TIME_MAIN_UPDATE_ENTRY);
    const bool mainStateTypeOk = CodeMsvcTypeMatches(
        base, RVA_GAME_TIME_MAIN_STATE_VTABLE, RVA_GAME_TIME_MAIN_STATE_COL,
        RVA_GAME_TIME_MAIN_STATE_TYPE,
        ".?AVCState_Main@CTask_Livelihood@@") &&
        IsReadable(reinterpret_cast<void*>(base + RVA_GAME_TIME_MAIN_STATE_VTABLE),
                   sizeof(void*)) &&
        *reinterpret_cast<const uintptr_t*>(
            base + RVA_GAME_TIME_MAIN_STATE_VTABLE) ==
            base + RVA_GAME_TIME_MAIN_STATE_DESTROY;
    const bool layoutOk = memcmp(reinterpret_cast<void*>(
        base + RVA_GAME_TIME_SERIALIZED_LAYOUT), expectedSerializedLayout,
        sizeof(expectedSerializedLayout)) == 0;
    const bool patchPageOk = CodeRangeHasExpectedProtection(mainCall, 5);
    Log("[ProductionAuto] stage=signature advance=%s hour_decode=%s "
        "main_call=%s frame_chain=%s main_state_rtti=%s "
        "serialized_layout=%s local_clock_excluded=%s patch_page=%s\n",
        advanceOk ? "OK" : "FAIL", hourDecodeOk ? "OK" : "FAIL",
        mainCallOk ? "OK" : "FAIL", frameChainOk ? "OK" : "FAIL",
        mainStateTypeOk ? "OK" : "FAIL", layoutOk ? "OK" : "FAIL",
        localExcludedOk ? "OK" : "FAIL", patchPageOk ? "OK" : "FAIL");
    if (!advanceOk || !hourDecodeOk || !mainCallOk || !frameChainOk ||
        !mainStateTypeOk || !layoutOk || !localExcludedOk || !patchPageOk) {
        Log("[ProductionAuto] semantic/callsite check failed; feature disabled safely\n");
        return false;
    }

    unsigned char* relay = static_cast<unsigned char*>(AllocateExecutableNear(
        reinterpret_cast<uintptr_t>(mainCall), GAME_TIME_RELAY_SIZE));
    if (!relay) {
        Log("[ProductionAuto] callsite relay allocation failed (error %lu)\n",
            GetLastError());
        return false;
    }
    unsigned char relayCode[14] = {0xff, 0x25, 0, 0, 0, 0};
    *reinterpret_cast<u64*>(relayCode + 6) =
        reinterpret_cast<u64>(&GameTimeAdvanceDetour);
    memcpy(relay, relayCode, sizeof(relayCode));
    FlushInstructionCache(GetCurrentProcess(), relay, sizeof(relayCode));
    if (!SealExecutableMemory(relay, GAME_TIME_RELAY_SIZE)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        Log("[ProductionAuto] callsite relay sealing failed\n");
        return false;
    }

    const std::int64_t displacement =
        reinterpret_cast<std::int64_t>(relay) -
        static_cast<std::int64_t>(reinterpret_cast<uintptr_t>(mainCall) + 5);
    if (displacement < INT32_MIN || displacement > INT32_MAX) {
        VirtualFree(relay, 0, MEM_RELEASE);
        Log("[ProductionAuto] callsite relay outside rel32 range\n");
        return false;
    }
    unsigned char redirect[5] = {0xe8, 0, 0, 0, 0};
    *reinterpret_cast<std::int32_t*>(redirect + 1) =
        static_cast<std::int32_t>(displacement);

    g_originalGameTimeAdvance.store(
        reinterpret_cast<GameTimeAdvanceFunction>(base + RVA_GAME_TIME_ADVANCE),
        std::memory_order_release);
    g_midnightSlowRelay = relay;
    const bool writeReported = WriteCodePatchChecked(
        mainCall, redirect, sizeof(redirect));
    const bool readbackOk = memcmp(mainCall, redirect, sizeof(redirect)) == 0;
    if (!writeReported || !readbackOk) {
        const bool rollbackReported = WriteCodePatchChecked(
            mainCall, expectedMainCall, sizeof(expectedMainCall));
        const bool rollbackOk = rollbackReported &&
            memcmp(mainCall, expectedMainCall, sizeof(expectedMainCall)) == 0;
        Log("[ProductionAuto] callsite write/readback failed write=%s readback=%s "
            "rollback=%s; RX relay retained feature_disabled=1\n",
            writeReported ? "OK" : "FAIL", readbackOk ? "OK" : "FAIL",
            rollbackOk ? "OK" : "UNRESOLVED");
        return false;
    }
    g_midnightSlowHookReady = true;
    g_productionMainWorldGameClockSourceReady.store(
        true, std::memory_order_release);
    Log("[ProductionAuto] stage=hook_installed callsite_rva=0x%llx "
        "target_rva=0x%llx scope=main_world_clock\n",
        static_cast<unsigned long long>(RVA_GAME_TIME_MAIN_CALL),
        static_cast<unsigned long long>(RVA_GAME_TIME_ADVANCE));
    return true;
}

// ============================================================
// AutoPet 桩函数（空实现，维持编译）
// ============================================================
static u64 AutoPetCaptureLoadReadyGeneration() { return 0; }
static u64 AutoPetCaptureLoadInFlightGeneration() { return 0; }
static u64 AutoPetBeginMainCallback() { return 0; }
static void AutoPetCompleteMainCallback(u64 /*callbackSequence*/) {}
static void AutoPetOnMainThreadUpdate(
    void* /*state*/, u64 /*readyGenerationAtCallbackEntry*/,
    u64 /*pendingDailyGenerationAtCallbackEntry*/,
    u64 /*reloadInvocationGenerationAtCallbackEntry*/,
    u64 /*loadReadyGenerationAtCallbackEntry*/,
    u64 /*loadInFlightGenerationAtCallbackEntry*/,
    u64 /*callbackSequence*/) {}
static u64 AutoPetCaptureMorningReadyGeneration() { return 0; }
static u64 AutoPetCapturePendingDailyGeneration() { return 0; }
static u64 AutoPetCaptureReloadInvocationGeneration() { return 0; }

// ============================================================
// 前向声明（.inl 定义）
// ============================================================
static bool InstallNearbyChestSort();
static bool InstallProductionAutomation();

// ============================================================
// 旧 .inl 前向声明（v2 空桩，nearby_chest_sort.inl 可能引用）
// ============================================================
// 旧 .cpp 前向声明桩（v2 实现，转发到 PaOnMainUpdate）
// 嵌套保护：Begin 返回非 0 = 顶层回调（可执行主逻辑），
// 返回 0 = 嵌套回调（跳过，防止重入）
// ============================================================
static std::atomic<int> g_paCallbackDepth{0};
static std::atomic<u64> g_paCallbackSequence{0};

static void PaOnMainUpdate();  // 在 production_automation_v2.inl 中定义
static u64 ProductionBeginMainCallback() {
    const int depth = g_paCallbackDepth.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (depth != 1) return 0;
    return g_paCallbackSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
}
static void ProductionSetMainCallbackPhase(unsigned) {}
static void ProductionOnMainThreadPreUpdate(u64, bool) {}
static void ProductionOnMainThreadUpdate() { PaOnMainUpdate(); }
static void ProductionCompleteMainCallback(u64) {
    const int depth = g_paCallbackDepth.load(std::memory_order_acquire);
    if (depth > 0) g_paCallbackDepth.fetch_sub(1, std::memory_order_acq_rel);
}
static void ProductionObserveWorldContext(bool, u64) {}
static bool ProductionModalInputIsBlocked() { return false; }
static bool ProductionForceReleaseModalInputBlock() { return false; }

// HUD 桩函数（v2 先不接 HUD；当前 HUD 由 production_hud_v2.inl 提供）
static void ProductionHudPoll() {}
static void ProductionHudRefresh() {}
static void ProductionHudCleanup() {}
static void ProductionHudInit() {}
// static void ProductionHudRefresh();  // 旧前向声明

// ============================================================
// 引入 .inl 实现（所有外部依赖已在上方提供）
// 顺序遵循原 dinput8.cpp
//   nearby（基础）、production_automation（核心/数据类型）、panel（UI）
// panel 引入 production 定义的 ProductionState/ProductionReason/g_productionBindings 等。
// 因此 HUD 窗口实现和插件入口必须在 .inl include 之后。
// ============================================================
// ============================================================
// HotConfig: volatile RVA 变量（CE 可热改）
// 宏 -> volatile 变量重定向，.inl 中使用宏名实际访问变量
// ============================================================
#include "hot_config.h"

static volatile uintptr_t g_rtti_component_vtable_target = 0xAF7E0;
#undef RTTI_COMPONENT_VTABLE_TARGET
#define RTTI_COMPONENT_VTABLE_TARGET g_rtti_component_vtable_target
static volatile uintptr_t g_rtti_gimmick_col = 0xECFA08;
#undef RTTI_GIMMICK_COL
#define RTTI_GIMMICK_COL g_rtti_gimmick_col
static volatile uintptr_t g_rtti_gimmick_secondary_col = 0xECF6D8;
#undef RTTI_GIMMICK_SECONDARY_COL
#define RTTI_GIMMICK_SECONDARY_COL g_rtti_gimmick_secondary_col
static volatile uintptr_t g_rtti_gimmick_secondary_hierarchy = 0xECF520;
#undef RTTI_GIMMICK_SECONDARY_HIERARCHY
#define RTTI_GIMMICK_SECONDARY_HIERARCHY g_rtti_gimmick_secondary_hierarchy
static volatile uintptr_t g_rtti_gimmick_secondary_vtable_target = 0x10B46C;
#undef RTTI_GIMMICK_SECONDARY_VTABLE_TARGET
#define RTTI_GIMMICK_SECONDARY_VTABLE_TARGET g_rtti_gimmick_secondary_vtable_target
static volatile uintptr_t g_rtti_gimmick_type_descriptor = 0x108E2A8;
#undef RTTI_GIMMICK_TYPE_DESCRIPTOR
#define RTTI_GIMMICK_TYPE_DESCRIPTOR g_rtti_gimmick_type_descriptor
static volatile uintptr_t g_rtti_gimmick_vtable_target = 0x107790;
#undef RTTI_GIMMICK_VTABLE_TARGET
#define RTTI_GIMMICK_VTABLE_TARGET g_rtti_gimmick_vtable_target
static volatile uintptr_t g_rtti_map_status_col = 0xED1AB0;
#undef RTTI_MAP_STATUS_COL
#define RTTI_MAP_STATUS_COL g_rtti_map_status_col
static volatile uintptr_t g_rtti_map_status_type_descriptor = 0x108C1C8;
#undef RTTI_MAP_STATUS_TYPE_DESCRIPTOR
#define RTTI_MAP_STATUS_TYPE_DESCRIPTOR g_rtti_map_status_type_descriptor
static volatile uintptr_t g_rtti_map_status_vtable_target = 0x11C230;
#undef RTTI_MAP_STATUS_VTABLE_TARGET
#define RTTI_MAP_STATUS_VTABLE_TARGET g_rtti_map_status_vtable_target
static volatile uintptr_t g_rtti_save_data_col = 0xED83B8;
#undef RTTI_SAVE_DATA_COL
#define RTTI_SAVE_DATA_COL g_rtti_save_data_col
static volatile uintptr_t g_rtti_save_data_type_descriptor = 0x1096260;
#undef RTTI_SAVE_DATA_TYPE_DESCRIPTOR
#define RTTI_SAVE_DATA_TYPE_DESCRIPTOR g_rtti_save_data_type_descriptor
static volatile uintptr_t g_rtti_save_data_vtable_target = 0x144810;
#undef RTTI_SAVE_DATA_VTABLE_TARGET
#define RTTI_SAVE_DATA_VTABLE_TARGET g_rtti_save_data_vtable_target
static volatile uintptr_t g_rva_game_root = 0x111cc90;
#undef RVA_GAME_ROOT
#define RVA_GAME_ROOT g_rva_game_root
static volatile uintptr_t g_rva_game_time_advance = 0x106770;
#undef RVA_GAME_TIME_ADVANCE
#define RVA_GAME_TIME_ADVANCE g_rva_game_time_advance
static volatile uintptr_t g_rva_game_time_frame_registration = 0x2DBC63;
#undef RVA_GAME_TIME_FRAME_REGISTRATION
#define RVA_GAME_TIME_FRAME_REGISTRATION g_rva_game_time_frame_registration
static volatile uintptr_t g_rva_game_time_frame_wrapper_call = 0x2dbd1a;
#undef RVA_GAME_TIME_FRAME_WRAPPER_CALL
#define RVA_GAME_TIME_FRAME_WRAPPER_CALL g_rva_game_time_frame_wrapper_call
static volatile uintptr_t g_rva_game_time_frame_wrapper_context = 0x2dbd14;
#undef RVA_GAME_TIME_FRAME_WRAPPER_CONTEXT
#define RVA_GAME_TIME_FRAME_WRAPPER_CONTEXT g_rva_game_time_frame_wrapper_context
static volatile uintptr_t g_rva_game_time_hour_decode = 0x1069A6;
#undef RVA_GAME_TIME_HOUR_DECODE
#define RVA_GAME_TIME_HOUR_DECODE g_rva_game_time_hour_decode
static volatile uintptr_t g_rva_game_time_local_call = 0x19B00E;
#undef RVA_GAME_TIME_LOCAL_CALL
#define RVA_GAME_TIME_LOCAL_CALL g_rva_game_time_local_call
static volatile uintptr_t g_rva_game_time_local_call_context = 0x19afe6;
#undef RVA_GAME_TIME_LOCAL_CALL_CONTEXT
#define RVA_GAME_TIME_LOCAL_CALL_CONTEXT g_rva_game_time_local_call_context
static volatile uintptr_t g_rva_game_time_local_gate = 0x19AFBC;
#undef RVA_GAME_TIME_LOCAL_GATE
#define RVA_GAME_TIME_LOCAL_GATE g_rva_game_time_local_gate
static volatile uintptr_t g_rva_game_time_main_call = 0x2d9dde;
#undef RVA_GAME_TIME_MAIN_CALL
#define RVA_GAME_TIME_MAIN_CALL g_rva_game_time_main_call
static volatile uintptr_t g_rva_game_time_main_call_context = 0x2D9DD3;
#undef RVA_GAME_TIME_MAIN_CALL_CONTEXT
#define RVA_GAME_TIME_MAIN_CALL_CONTEXT g_rva_game_time_main_call_context
static volatile uintptr_t g_rva_game_time_main_state_col = 0xef6eb0;
#undef RVA_GAME_TIME_MAIN_STATE_COL
#define RVA_GAME_TIME_MAIN_STATE_COL g_rva_game_time_main_state_col
static volatile uintptr_t g_rva_game_time_main_state_destroy = 0x2dbe30;
#undef RVA_GAME_TIME_MAIN_STATE_DESTROY
#define RVA_GAME_TIME_MAIN_STATE_DESTROY g_rva_game_time_main_state_destroy
static volatile uintptr_t g_rva_game_time_main_state_type = 0x10b2880;
#undef RVA_GAME_TIME_MAIN_STATE_TYPE
#define RVA_GAME_TIME_MAIN_STATE_TYPE g_rva_game_time_main_state_type
static volatile uintptr_t g_rva_game_time_main_state_vtable = 0xe5be50;
#undef RVA_GAME_TIME_MAIN_STATE_VTABLE
#define RVA_GAME_TIME_MAIN_STATE_VTABLE g_rva_game_time_main_state_vtable
static volatile uintptr_t g_rva_game_time_main_update_entry = 0x2d9cc0;
#undef RVA_GAME_TIME_MAIN_UPDATE_ENTRY
#define RVA_GAME_TIME_MAIN_UPDATE_ENTRY g_rva_game_time_main_update_entry
static volatile uintptr_t g_rva_game_time_main_update_parameter = 0x2D9CDE;
#undef RVA_GAME_TIME_MAIN_UPDATE_PARAMETER
#define RVA_GAME_TIME_MAIN_UPDATE_PARAMETER g_rva_game_time_main_update_parameter
static volatile uintptr_t g_rva_game_time_serialized_layout = 0x145238;
#undef RVA_GAME_TIME_SERIALIZED_LAYOUT
#define RVA_GAME_TIME_SERIALIZED_LAYOUT g_rva_game_time_serialized_layout
static volatile uintptr_t g_rva_nearby_batch_dirty = 0x147980;
#undef RVA_NEARBY_BATCH_DIRTY
#define RVA_NEARBY_BATCH_DIRTY g_rva_nearby_batch_dirty
static volatile uintptr_t g_rva_nearby_batch_recalculate = 0x149BF0;
#undef RVA_NEARBY_BATCH_RECALCULATE
#define RVA_NEARBY_BATCH_RECALCULATE g_rva_nearby_batch_recalculate
static volatile uintptr_t g_rva_nearby_batch_ui_refresh = 0x0FD4F0;
#undef RVA_NEARBY_BATCH_UI_REFRESH
#define RVA_NEARBY_BATCH_UI_REFRESH g_rva_nearby_batch_ui_refresh
static volatile uintptr_t g_rva_nearby_command_capacity = 0x4FF1B0;
#undef RVA_NEARBY_COMMAND_CAPACITY
#define RVA_NEARBY_COMMAND_CAPACITY g_rva_nearby_command_capacity
static volatile uintptr_t g_rva_nearby_forbidden_item = 0x1727E0;
#undef RVA_NEARBY_FORBIDDEN_ITEM
#define RVA_NEARBY_FORBIDDEN_ITEM g_rva_nearby_forbidden_item
static volatile uintptr_t g_rva_nearby_input_active = 0x81D8D0;
#undef RVA_NEARBY_INPUT_ACTIVE
#define RVA_NEARBY_INPUT_ACTIVE g_rva_nearby_input_active
static volatile uintptr_t g_rva_nearby_input_consume = 0x81d9a0;
#undef RVA_NEARBY_INPUT_CONSUME
#define RVA_NEARBY_INPUT_CONSUME g_rva_nearby_input_consume
static volatile uintptr_t g_rva_nearby_input_descriptor_data = 0xA24962;
#undef RVA_NEARBY_INPUT_DESCRIPTOR_DATA
#define RVA_NEARBY_INPUT_DESCRIPTOR_DATA g_rva_nearby_input_descriptor_data
static volatile uintptr_t g_rva_nearby_input_descriptor_vtable = 0xE6F840;
#undef RVA_NEARBY_INPUT_DESCRIPTOR_VTABLE
#define RVA_NEARBY_INPUT_DESCRIPTOR_VTABLE g_rva_nearby_input_descriptor_vtable
static volatile uintptr_t g_rva_nearby_input_event = 0x81DB00;
#undef RVA_NEARBY_INPUT_EVENT
#define RVA_NEARBY_INPUT_EVENT g_rva_nearby_input_event
static volatile uintptr_t g_rva_nearby_intrusive_release = 0xacc20;
#undef RVA_NEARBY_INTRUSIVE_RELEASE
#define RVA_NEARBY_INTRUSIVE_RELEASE g_rva_nearby_intrusive_release
static volatile uintptr_t g_rva_nearby_item_adjust = 0x10B920;
#undef RVA_NEARBY_ITEM_ADJUST
#define RVA_NEARBY_ITEM_ADJUST g_rva_nearby_item_adjust
static volatile uintptr_t g_rva_nearby_main_update = 0x2DBCE0;
#undef RVA_NEARBY_MAIN_UPDATE
#define RVA_NEARBY_MAIN_UPDATE g_rva_nearby_main_update
static volatile uintptr_t g_rva_nearby_player_capacity = 0x155700;
#undef RVA_NEARBY_PLAYER_CAPACITY
#define RVA_NEARBY_PLAYER_CAPACITY g_rva_nearby_player_capacity
static volatile uintptr_t g_rva_nearby_raw_vector_free = 0x0BAFF0;
#undef RVA_NEARBY_RAW_VECTOR_FREE
#define RVA_NEARBY_RAW_VECTOR_FREE g_rva_nearby_raw_vector_free
static volatile uintptr_t g_rva_nearby_refresh_world_object = 0x761050;
#undef RVA_NEARBY_REFRESH_WORLD_OBJECT
#define RVA_NEARBY_REFRESH_WORLD_OBJECT g_rva_nearby_refresh_world_object
static volatile uintptr_t g_rva_nearby_resolve_world_object = 0x120300;
#undef RVA_NEARBY_RESOLVE_WORLD_OBJECT
#define RVA_NEARBY_RESOLVE_WORLD_OBJECT g_rva_nearby_resolve_world_object
static volatile uintptr_t g_rva_nearby_search_callback_copy = 0x296df0;
#undef RVA_NEARBY_SEARCH_CALLBACK_COPY
#define RVA_NEARBY_SEARCH_CALLBACK_COPY g_rva_nearby_search_callback_copy
static volatile uintptr_t g_rva_nearby_search_callback_destroy = 0x1999b0;
#undef RVA_NEARBY_SEARCH_CALLBACK_DESTROY
#define RVA_NEARBY_SEARCH_CALLBACK_DESTROY g_rva_nearby_search_callback_destroy
static volatile uintptr_t g_rva_nearby_search_callback_invoke = 0x296d80;
#undef RVA_NEARBY_SEARCH_CALLBACK_INVOKE
#define RVA_NEARBY_SEARCH_CALLBACK_INVOKE g_rva_nearby_search_callback_invoke
static volatile uintptr_t g_rva_nearby_search_callback_vtable = 0xe59110;
#undef RVA_NEARBY_SEARCH_CALLBACK_VTABLE
#define RVA_NEARBY_SEARCH_CALLBACK_VTABLE g_rva_nearby_search_callback_vtable
static volatile uintptr_t g_rva_nearby_world_object_registry = 0x1124d60;
#undef RVA_NEARBY_WORLD_OBJECT_REGISTRY
#define RVA_NEARBY_WORLD_OBJECT_REGISTRY g_rva_nearby_world_object_registry
static volatile uintptr_t g_rva_placement_spatial_test = 0x196460;
#undef RVA_PLACEMENT_SPATIAL_TEST
#define RVA_PLACEMENT_SPATIAL_TEST g_rva_placement_spatial_test
static volatile uintptr_t g_rva_production_chest_commit = 0x373E80;
#undef RVA_PRODUCTION_CHEST_COMMIT
#define RVA_PRODUCTION_CHEST_COMMIT g_rva_production_chest_commit
static volatile uintptr_t g_rva_production_chest_push = 0x373180;
#undef RVA_PRODUCTION_CHEST_PUSH
#define RVA_PRODUCTION_CHEST_PUSH g_rva_production_chest_push
static volatile uintptr_t g_rva_production_component_vtable = 0xe43a88;
#undef RVA_PRODUCTION_COMPONENT_VTABLE
#define RVA_PRODUCTION_COMPONENT_VTABLE g_rva_production_component_vtable
static volatile uintptr_t g_rva_production_gimmick_secondary_vtable = 0xe43700;
#undef RVA_PRODUCTION_GIMMICK_SECONDARY_VTABLE
#define RVA_PRODUCTION_GIMMICK_SECONDARY_VTABLE g_rva_production_gimmick_secondary_vtable
static volatile uintptr_t g_rva_production_gimmick_vtable = 0xe43870;
#undef RVA_PRODUCTION_GIMMICK_VTABLE
#define RVA_PRODUCTION_GIMMICK_VTABLE g_rva_production_gimmick_vtable
static volatile uintptr_t g_rva_production_item_allocate = 0x8EA970;
#undef RVA_PRODUCTION_ITEM_ALLOCATE
#define RVA_PRODUCTION_ITEM_ALLOCATE g_rva_production_item_allocate
static volatile uintptr_t g_rva_production_item_ctor = 0x10BB70;
#undef RVA_PRODUCTION_ITEM_CTOR
#define RVA_PRODUCTION_ITEM_CTOR g_rva_production_item_ctor
static volatile uintptr_t g_rva_production_item_is_stackable = 0x10C130;
#undef RVA_PRODUCTION_ITEM_IS_STACKABLE
#define RVA_PRODUCTION_ITEM_IS_STACKABLE g_rva_production_item_is_stackable
static volatile uintptr_t g_rva_production_item_set_count = 0x10B8D0;
#undef RVA_PRODUCTION_ITEM_SET_COUNT
#define RVA_PRODUCTION_ITEM_SET_COUNT g_rva_production_item_set_count
static volatile uintptr_t g_rva_production_logical_child_attach = 0x75F7B0;
#undef RVA_PRODUCTION_LOGICAL_CHILD_ATTACH
#define RVA_PRODUCTION_LOGICAL_CHILD_ATTACH g_rva_production_logical_child_attach
static volatile uintptr_t g_rva_production_logical_child_vector = 0x75F80A;
#undef RVA_PRODUCTION_LOGICAL_CHILD_VECTOR
#define RVA_PRODUCTION_LOGICAL_CHILD_VECTOR g_rva_production_logical_child_vector
static volatile uintptr_t g_rva_production_logical_parent_read = 0x710B09;
#undef RVA_PRODUCTION_LOGICAL_PARENT_READ
#define RVA_PRODUCTION_LOGICAL_PARENT_READ g_rva_production_logical_parent_read
static volatile uintptr_t g_rva_production_map_status_vtable = 0xe44b70;
#undef RVA_PRODUCTION_MAP_STATUS_VTABLE
#define RVA_PRODUCTION_MAP_STATUS_VTABLE g_rva_production_map_status_vtable
static volatile uintptr_t g_rva_production_native_collect = 0x286cc0;
#undef RVA_PRODUCTION_NATIVE_COLLECT
#define RVA_PRODUCTION_NATIVE_COLLECT g_rva_production_native_collect
static volatile uintptr_t g_rva_production_native_load = 0x285840;
#undef RVA_PRODUCTION_NATIVE_LOAD
#define RVA_PRODUCTION_NATIVE_LOAD g_rva_production_native_load
static volatile uintptr_t g_rva_production_object_get_parent = 0x7116e0;
#undef RVA_PRODUCTION_OBJECT_GET_PARENT
#define RVA_PRODUCTION_OBJECT_GET_PARENT g_rva_production_object_get_parent
static volatile uintptr_t g_rva_production_output_helper = 0x173290;
#undef RVA_PRODUCTION_OUTPUT_HELPER
#define RVA_PRODUCTION_OUTPUT_HELPER g_rva_production_output_helper
static volatile uintptr_t g_rva_production_r12_guard = 0x1733d7;
#undef RVA_PRODUCTION_R12_GUARD
#define RVA_PRODUCTION_R12_GUARD g_rva_production_r12_guard
static volatile uintptr_t g_rva_production_pop = 0x285160;
#undef RVA_PRODUCTION_POP
#define RVA_PRODUCTION_POP g_rva_production_pop
static volatile uintptr_t g_rva_production_push = 0x284670;
#undef RVA_PRODUCTION_PUSH
#define RVA_PRODUCTION_PUSH g_rva_production_push
static volatile uintptr_t g_rva_production_recipe_mgr = 0x1124cf0;
#undef RVA_PRODUCTION_RECIPE_MGR
#define RVA_PRODUCTION_RECIPE_MGR g_rva_production_recipe_mgr
// v1.5.85【加工 UI 回归修复】玩家投料时游戏脚本会执行 4 条指令，PA 此前只
//   做了第 1 条（push），导致"面板空白 / 远处小进度条消失 / 无加工动画"。
//   补齐后三条（均为 ScriptCommand::Gimmick* 的底层实现）：
//     GimmickCreateProcessingUI      -> 近处大 UI（进度面板 + 写 UIPros）
//     GimmickCreateProcessingUI_Far  -> 远处小进度条
//     GimmickCreateProcessingManpuUI -> 加工动画（Manpu）
//   三条签名同为 (baseID, gimmickStatus)。baseID 由脚本传入，工程内称 objID；
//   引擎侧校验函数见下方 RVA_PRODUCTION_BASEID_RESOLVE（新版 0x1899C0），
//   返回 0 即 "invalid baseID" 并抛异常 —— 故调用前必须先用它预检。
// 2026-10-29【重新绑定】build 25640656 地址（KB-098）
//   旧版 near/far/manpu = 0x27D480 / 0x27DCE0 / 0x27E460（size 1193/1228/1324）。
//   2026-10-28 曾因"新版同址函数只有 635/439/598"误判为已重构而解绑为 0，
//   导致远处小进度条与 manpu 键写入全失效。本次用两条独立证据重新定址：
//     ①【内联立即数】far 把键名 "UIPrsFar" 当 8 字节立即数写进代码
//        （小端字节 55 49 50 72 73 46 61 72 —— 这就是它不在 .rdata 的原因）。
//        在新 exe 的 .text 直搜该字节序列，仅 2 处命中，其中一处所属函数
//        size=1228 与旧 far 完全相同，且命中点前后 48 字节与旧版逐字节相同。
//     ②【整簇对齐】新旧两版这一片函数的**尺寸与间隙逐项相同**：
//        1181, 1193, 370, 558, 1228, 599, 74, 1324, 861
//        （连中间的 3/7/0xE/2/4/9/6 字节 padding 间隔都一致）
//        → near / manpu 在同一簇里按序号对位即可锁定，无需依赖 AOB。
//   交叉验证：三址均为 .pdata 权威函数入口；且与旧址前 1024 字节相比
//        仅差 7~12 字节，全部是 3 字节一组的 rip-rel 重定位位移。
//   注意：near 仍按 v1.5.112 定案"交还游戏"，PA 不主动创建它；
//        但必须绑定 —— PaInstallApiProbe 要 hook near/far 做 baseID 捕获与诊断。
static volatile uintptr_t g_rva_production_ui_near = 0x285CE0;
#undef RVA_PRODUCTION_UI_NEAR
#define RVA_PRODUCTION_UI_NEAR g_rva_production_ui_near
static volatile uintptr_t g_rva_production_ui_far = 0x286540;
#undef RVA_PRODUCTION_UI_FAR
#define RVA_PRODUCTION_UI_FAR g_rva_production_ui_far
static volatile uintptr_t g_rva_production_ui_manpu = 0x286CC0;
#undef RVA_PRODUCTION_UI_MANPU
#define RVA_PRODUCTION_UI_MANPU g_rva_production_ui_manpu
// baseID 校验：FUN_140186930(int* table, u64 baseId) -> 对象指针，0=非法
static volatile uintptr_t g_rva_production_baseid_resolve = 0x1899C0;
// 2026-10-28 build 25640656 修正：旧值 0x186930 在新版是 **CRT sprintf 内部指令中段**，
//   调用即 AV（fault addr=0x8，开图/进存档秒退）。
//   定址依据：旧版 20+ 个调用点（mov rcx,[GIMMICK_MGR] 后 call）在新版
//   全部调用 0x1899C0；该类共 5 个字节完全相同的克隆（16A0F0/1899C0/29E720/
//   740750/741360），靠调用图唯一消歧。
#undef RVA_PRODUCTION_BASEID_RESOLVE
#define RVA_PRODUCTION_BASEID_RESOLVE g_rva_production_baseid_resolve
// DAT_141104c80 = 全局 baseID 注册表指针的所在地址（代码用 MOV rax,[0x141104C80] 取）
// 2026-10-28 修正：0x1104C80 是 GIMMICK_MGR 的**旧版**地址；
//   build 25640656 已迁到 0x1124D60（全 .text 85 处 rip-rel 读者，与 MonsterMark/AutoHarvest 同源）。
static volatile uintptr_t g_rva_production_baseid_table = 0x1124D60;
#undef RVA_PRODUCTION_BASEID_TABLE
#define RVA_PRODUCTION_BASEID_TABLE g_rva_production_baseid_table
// 加工动画判活计数 FUN_1401569c0(manager, ownerId, filterList)
//   语义（build 25640656 反编译实锤）：遍历 manager+0x33b8(0xb=11 槽) 与
//   manager+0x32c0(0x1e=30 槽) 两张动画实例表，累加「owner(**(inst+0x240))
//   == ownerId」的实例的 *(int*)(inst+0x260)，返回总和；>0 才写 transform
//   （动画真正出现），==0 不渲染。纯查询函数，零写入。
//   manager = *(void**)(*(void**)(base + RVA_GAME_ROOT) + 0x208)  ← 两层解引用，
//   不是 base+RVA_GAME_ROOT+0x208（v1.5.92 曾少一层，读到哈希样垃圾）。
//   ownerId 用 PaGetEventOwner(status) 取（= *(*(inst+0x240))），与 near 调用的
//   第 2 参数不是同一个对象（2026-10-26 打点实测）。
// KB-108【2026-11-01 订正】旧值 0x153EE0 是旧 build 残留，在本 build 落在
//   FUN_140153e90（unordered_map 查找，size 573）的**函数中段**（入口 +0x50）。
//   调用中段必抛异常，而 PaAnimAliveCount 的 __try/__except 会把它吞成 -2 ——
//   表现为"运行时异常"，极易被误判成游戏状态问题。真·判活 = 0x1569C0，
//   反编译比对：+0x33b8 取 0xb 次、+0x32c0 取 0x1e 次、累加 +0x260、比较
//   owner 两段解引用，与语义逐项吻合。地址经 HotConfig 注册可热改。
static volatile uintptr_t g_rva_manpu_alive = 0x1569C0;
#undef RVA_MANPU_ALIVE
#define RVA_MANPU_ALIVE g_rva_manpu_alive
static volatile uintptr_t g_rva_production_recipe_table = 0x2c1e0;
#undef RVA_PRODUCTION_RECIPE_TABLE
#define RVA_PRODUCTION_RECIPE_TABLE g_rva_production_recipe_table
static volatile uintptr_t g_rva_production_resolve_component = 0x14b3f0;
#undef RVA_PRODUCTION_RESOLVE_COMPONENT
#define RVA_PRODUCTION_RESOLVE_COMPONENT g_rva_production_resolve_component
static volatile uintptr_t g_rva_production_save_data_vtable = 0xE48230;
#undef RVA_PRODUCTION_SAVE_DATA_VTABLE
#define RVA_PRODUCTION_SAVE_DATA_VTABLE g_rva_production_save_data_vtable
static volatile uintptr_t g_rva_production_status_event_lookup = 0x15d740;
#undef RVA_PRODUCTION_STATUS_EVENT_LOOKUP
#define RVA_PRODUCTION_STATUS_EVENT_LOOKUP g_rva_production_status_event_lookup
static volatile uintptr_t g_rva_production_status_event_notify = 0x15D560;
#undef RVA_PRODUCTION_STATUS_EVENT_NOTIFY
#define RVA_PRODUCTION_STATUS_EVENT_NOTIFY g_rva_production_status_event_notify
static volatile uintptr_t g_rva_production_status_int_lookup = 0x15db60;
#undef RVA_PRODUCTION_STATUS_INT_LOOKUP
#define RVA_PRODUCTION_STATUS_INT_LOOKUP g_rva_production_status_int_lookup
static volatile uintptr_t g_rva_production_status_u64_lookup = 0x15dd20;
#undef RVA_PRODUCTION_STATUS_U64_LOOKUP
#define RVA_PRODUCTION_STATUS_U64_LOOKUP g_rva_production_status_u64_lookup
static volatile uintptr_t g_rva_production_target_accept = 0x20E100;
#undef RVA_PRODUCTION_TARGET_ACCEPT
#define RVA_PRODUCTION_TARGET_ACCEPT g_rva_production_target_accept
static volatile uintptr_t g_rva_production_target_select = 0x20DA30;
#undef RVA_PRODUCTION_TARGET_SELECT
#define RVA_PRODUCTION_TARGET_SELECT g_rva_production_target_select

// v1.5.98 链路探针浮窗：前向声明（定义在 production_chainhud.inl，hud 之后 include）
void PaChainAdd(int color, const wchar_t* line);
// v1.5.100：F6 链浮窗开关状态（v2.inl 探针引用，判断是否入屏）
extern std::atomic<bool> g_paChainActive;
#include "nearby_chest_sort.inl"
#include "production_automation_v2.inl"
#include "production_hud_v2.inl"
#include "production_chainhud.inl"

// production_auto.cpp -- ProductionAuto: 链式自动化（加工机自动投料/出料）//
// 从 dinput8.cpp 原型 .inl 提取的独立 DLL 插件。
// 实现逻辑见：
//   nearby_chest_sort.inl          -- 基础设施层（RVA 已更新至 build 24969282）
//   production_automation_v2.inl   -- 核心逻辑（链式自动化 / 自动投料出料）
//   production_hud_v2.inl          -- 生产状态 HUD
//   production_chainhud.inl        -- 链路浮窗（2Hz 采样）
//   processing_recipe_table.inl    -- 配方表（932 条，从 Automate 权威表生成）
// 注：早期 v1 版 / 原型面板 / 旧 HUD 三个孤儿 .inl 从未参与编译，
//     已于 2026-11-07 从源码树移除（副本见 Tools/.temp/backup/removed_dead_pa_20261107_2057）。
// 本文件提供：
//   1. .inl 所需的全部外部依赖桩函数
//   2. GameClock 发布器（hook CGameTime::advance 主世界调用点，发布游戏时间）
//   3. AutoPet 桩函数（空实现，仅维持编译）
// 版本：1.1.28b，适配 build 25094764 (v1.09)
//
// .inl 在文件尾部按依赖顺序 #include 引入。

// ============================================================
// Plugin entry points
// ============================================================

extern "C" __declspec(dllexport) void mod_init(void) {
    LogOpen("productionauto");
    qol::obs::Init(nullptr);  // v1.6.x: 统一观测层接入
    // 2026-10-29：此处版本号曾长期写死 v1.5.98（实际早已 v1.6.x），排查时被误导
    //   过一次。版本号本身由部署目录名决定，这里只能手写同步 —— 改版时务必一起改。
    Log("[ProductionAuto] mod_init loaded, build %s %s, MOD v1.6.60\n",
        SUPPORTED_BUILD_NUMBER, SUPPORTED_GAME_VERSION);

    g_module = GetModuleHandleW(nullptr);

    g_supportedExe = VerifyExeBuild();
    if (!g_supportedExe) {
        Log("[ProductionAuto] unsupported exe; feature disabled safely\n");
        return;
    }

    PackageOpenWriteGate();

    // Install game clock hook first (publish time, for Production scheduler)
    const bool clockReady = InstallGameClockHook();
    Log("[ProductionAuto] GameClockHook = %s\n", clockReady ? "OK" : "FAILED");

    // 安装 Nearby 基础层
    const bool nearbyReady = InstallNearbyChestSort();
    Log("[ProductionAuto] InstallNearbyChestSort = %s\n",
        nearbyReady ? "OK" : "FAILED");

    // 安装 Production 核心逻辑
    const bool productionReady = InstallProductionAutomation();
    Log("[ProductionAuto] InstallProductionAutomation = %s\n",
        productionReady ? "OK" : "FAILED");

    // ---- HotConfig: 注册所有 RVA 参数 ----
    HotConfig_Register("productionauto", (void*)&g_rva_game_root, "RVA_GAME_ROOT", HOT_RVA, 0x111CC90);
    HotConfig_Register("productionauto", (void*)&g_rtti_component_vtable_target, "RTTI_COMPONENT_VTABLE_TARGET", HOT_RVA, 0xAF7E0);
    HotConfig_Register("productionauto", (void*)&g_rtti_gimmick_col, "RTTI_GIMMICK_COL", HOT_RVA, 0xECFA08);
    HotConfig_Register("productionauto", (void*)&g_rtti_gimmick_secondary_col, "RTTI_GIMMICK_SECONDARY_COL", HOT_RVA, 0xECF6D8);
    HotConfig_Register("productionauto", (void*)&g_rtti_gimmick_secondary_hierarchy, "RTTI_GIMMICK_SECONDARY_HIERARCHY", HOT_RVA, 0xECF520);
    HotConfig_Register("productionauto", (void*)&g_rtti_gimmick_secondary_vtable_target, "RTTI_GIMMICK_SECONDARY_VTABLE_TARGET", HOT_RVA, 0x10B46C);
    HotConfig_Register("productionauto", (void*)&g_rtti_gimmick_type_descriptor, "RTTI_GIMMICK_TYPE_DESCRIPTOR", HOT_RVA, 0x108E2A8);
    HotConfig_Register("productionauto", (void*)&g_rtti_gimmick_vtable_target, "RTTI_GIMMICK_VTABLE_TARGET", HOT_RVA, 0x107790);
    HotConfig_Register("productionauto", (void*)&g_rtti_map_status_col, "RTTI_MAP_STATUS_COL", HOT_RVA, 0xED1AB0);
    HotConfig_Register("productionauto", (void*)&g_rtti_map_status_type_descriptor, "RTTI_MAP_STATUS_TYPE_DESCRIPTOR", HOT_RVA, 0x108C1C8);
    HotConfig_Register("productionauto", (void*)&g_rtti_map_status_vtable_target, "RTTI_MAP_STATUS_VTABLE_TARGET", HOT_RVA, 0x11C230);
    HotConfig_Register("productionauto", (void*)&g_rtti_save_data_col, "RTTI_SAVE_DATA_COL", HOT_RVA, 0xED83B8);
    HotConfig_Register("productionauto", (void*)&g_rtti_save_data_type_descriptor, "RTTI_SAVE_DATA_TYPE_DESCRIPTOR", HOT_RVA, 0x1096260);
    HotConfig_Register("productionauto", (void*)&g_rtti_save_data_vtable_target, "RTTI_SAVE_DATA_VTABLE_TARGET", HOT_RVA, 0x144810);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_advance, "RVA_GAME_TIME_ADVANCE", HOT_RVA, 0x106770);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_frame_registration, "RVA_GAME_TIME_FRAME_REGISTRATION", HOT_RVA, 0x2DBC63);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_frame_wrapper_call, "RVA_GAME_TIME_FRAME_WRAPPER_CALL", HOT_RVA, 0x2DBD1A);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_frame_wrapper_context, "RVA_GAME_TIME_FRAME_WRAPPER_CONTEXT", HOT_RVA, 0x2DBD14);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_hour_decode, "RVA_GAME_TIME_HOUR_DECODE", HOT_RVA, 0x1069A6);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_local_call, "RVA_GAME_TIME_LOCAL_CALL", HOT_RVA, 0x19B00E);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_local_call_context, "RVA_GAME_TIME_LOCAL_CALL_CONTEXT", HOT_RVA, 0x19AFE6);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_local_gate, "RVA_GAME_TIME_LOCAL_GATE", HOT_RVA, 0x19AFBC);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_main_call, "RVA_GAME_TIME_MAIN_CALL", HOT_RVA, 0x2D9DDE);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_main_call_context, "RVA_GAME_TIME_MAIN_CALL_CONTEXT", HOT_RVA, 0x2D9DD3);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_main_state_col, "RVA_GAME_TIME_MAIN_STATE_COL", HOT_RVA, 0xEF6EB0);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_main_state_destroy, "RVA_GAME_TIME_MAIN_STATE_DESTROY", HOT_RVA, 0x2DBE30);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_main_state_type, "RVA_GAME_TIME_MAIN_STATE_TYPE", HOT_RVA, 0x10B2880);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_main_state_vtable, "RVA_GAME_TIME_MAIN_STATE_VTABLE", HOT_RVA, 0xE5BE50);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_main_update_entry, "RVA_GAME_TIME_MAIN_UPDATE_ENTRY", HOT_RVA, 0x2D9CC0);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_main_update_parameter, "RVA_GAME_TIME_MAIN_UPDATE_PARAMETER", HOT_RVA, 0x2D9CDE);
    HotConfig_Register("productionauto", (void*)&g_rva_game_time_serialized_layout, "RVA_GAME_TIME_SERIALIZED_LAYOUT", HOT_RVA, 0x145238);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_batch_dirty, "RVA_NEARBY_BATCH_DIRTY", HOT_RVA, 0x147980);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_batch_recalculate, "RVA_NEARBY_BATCH_RECALCULATE", HOT_RVA, 0x149BF0);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_batch_ui_refresh, "RVA_NEARBY_BATCH_UI_REFRESH", HOT_RVA, 0x0FD4F0);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_command_capacity, "RVA_NEARBY_COMMAND_CAPACITY", HOT_RVA, 0x4FF1B0);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_forbidden_item, "RVA_NEARBY_FORBIDDEN_ITEM", HOT_RVA, 0x1727E0);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_input_active, "RVA_NEARBY_INPUT_ACTIVE", HOT_RVA, 0x81D8D0);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_input_consume, "RVA_NEARBY_INPUT_CONSUME", HOT_RVA, 0x81D9A0);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_input_descriptor_data, "RVA_NEARBY_INPUT_DESCRIPTOR_DATA", HOT_RVA, 0xA24962);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_input_descriptor_vtable, "RVA_NEARBY_INPUT_DESCRIPTOR_VTABLE", HOT_RVA, 0xE6F840);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_input_event, "RVA_NEARBY_INPUT_EVENT", HOT_RVA, 0x81DB00);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_intrusive_release, "RVA_NEARBY_INTRUSIVE_RELEASE", HOT_RVA, 0x0ACC20);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_item_adjust, "RVA_NEARBY_ITEM_ADJUST", HOT_RVA, 0x10B920);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_main_update, "RVA_NEARBY_MAIN_UPDATE", HOT_RVA, 0x2DBCE0);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_player_capacity, "RVA_NEARBY_PLAYER_CAPACITY", HOT_RVA, 0x155700);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_raw_vector_free, "RVA_NEARBY_RAW_VECTOR_FREE", HOT_RVA, 0x0BAFF0);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_refresh_world_object, "RVA_NEARBY_REFRESH_WORLD_OBJECT", HOT_RVA, 0x761050);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_resolve_world_object, "RVA_NEARBY_RESOLVE_WORLD_OBJECT", HOT_RVA, 0x120300);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_search_callback_copy, "RVA_NEARBY_SEARCH_CALLBACK_COPY", HOT_RVA, 0x296DF0);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_search_callback_destroy, "RVA_NEARBY_SEARCH_CALLBACK_DESTROY", HOT_RVA, 0x1999B0);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_search_callback_invoke, "RVA_NEARBY_SEARCH_CALLBACK_INVOKE", HOT_RVA, 0x296D80);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_search_callback_vtable, "RVA_NEARBY_SEARCH_CALLBACK_VTABLE", HOT_RVA, 0xE59110);
    HotConfig_Register("productionauto", (void*)&g_rva_nearby_world_object_registry, "RVA_NEARBY_WORLD_OBJECT_REGISTRY", HOT_RVA, 0x1124D60);
    HotConfig_Register("productionauto", (void*)&g_rva_placement_spatial_test, "RVA_PLACEMENT_SPATIAL_TEST", HOT_RVA, 0x196460);
    HotConfig_Register("productionauto", (void*)&g_rva_production_chest_commit, "RVA_PRODUCTION_CHEST_COMMIT", HOT_RVA, 0x373E80);
    HotConfig_Register("productionauto", (void*)&g_rva_production_chest_push, "RVA_PRODUCTION_CHEST_PUSH", HOT_RVA, 0x373180);
    HotConfig_Register("productionauto", (void*)&g_rva_production_component_vtable, "RVA_PRODUCTION_COMPONENT_VTABLE", HOT_RVA, 0xE43A88);
    HotConfig_Register("productionauto", (void*)&g_rva_production_gimmick_secondary_vtable, "RVA_PRODUCTION_GIMMICK_SECONDARY_VTABLE", HOT_RVA, 0xE43700);
    HotConfig_Register("productionauto", (void*)&g_rva_production_gimmick_vtable, "RVA_PRODUCTION_GIMMICK_VTABLE", HOT_RVA, 0xE43870);
    HotConfig_Register("productionauto", (void*)&g_rva_production_item_allocate, "RVA_PRODUCTION_ITEM_ALLOCATE", HOT_RVA, 0x8EA970);
    HotConfig_Register("productionauto", (void*)&g_rva_production_item_ctor, "RVA_PRODUCTION_ITEM_CTOR", HOT_RVA, 0x10BB70);
    HotConfig_Register("productionauto", (void*)&g_rva_production_item_is_stackable, "RVA_PRODUCTION_ITEM_IS_STACKABLE", HOT_RVA, 0x10C130);
    HotConfig_Register("productionauto", (void*)&g_rva_production_item_set_count, "RVA_PRODUCTION_ITEM_SET_COUNT", HOT_RVA, 0x10B8D0);
    HotConfig_Register("productionauto", (void*)&g_rva_production_logical_child_attach, "RVA_PRODUCTION_LOGICAL_CHILD_ATTACH", HOT_RVA, 0x75F7B0);
    HotConfig_Register("productionauto", (void*)&g_rva_production_logical_child_vector, "RVA_PRODUCTION_LOGICAL_CHILD_VECTOR", HOT_RVA, 0x75F80A);
    HotConfig_Register("productionauto", (void*)&g_rva_production_logical_parent_read, "RVA_PRODUCTION_LOGICAL_PARENT_READ", HOT_RVA, 0x710B09);
    HotConfig_Register("productionauto", (void*)&g_rva_production_map_status_vtable, "RVA_PRODUCTION_MAP_STATUS_VTABLE", HOT_RVA, 0xE44B70);
    HotConfig_Register("productionauto", (void*)&g_rva_production_native_collect, "RVA_PRODUCTION_NATIVE_COLLECT", HOT_RVA, 0x286CC0);
    HotConfig_Register("productionauto", (void*)&g_rva_production_native_load, "RVA_PRODUCTION_NATIVE_LOAD", HOT_RVA, 0x285840);
    HotConfig_Register("productionauto", (void*)&g_rva_production_object_get_parent, "RVA_PRODUCTION_OBJECT_GET_PARENT", HOT_RVA, 0x7116E0);
    HotConfig_Register("productionauto", (void*)&g_rva_production_output_helper, "RVA_PRODUCTION_OUTPUT_HELPER", HOT_RVA, 0x173290);
    HotConfig_Register("productionauto", (void*)&g_rva_production_pop, "RVA_PRODUCTION_POP", HOT_RVA, 0x285160);
    HotConfig_Register("productionauto", (void*)&g_rva_production_push, "RVA_PRODUCTION_PUSH", HOT_RVA, 0x284670);
    HotConfig_Register("productionauto", (void*)&g_rva_production_recipe_mgr, "RVA_PRODUCTION_RECIPE_MGR", HOT_RVA, 0x1124CF0);
    HotConfig_Register("productionauto", (void*)&g_rva_production_ui_near, "RVA_PRODUCTION_UI_NEAR", HOT_RVA, 0x285CE0);
    HotConfig_Register("productionauto", (void*)&g_rva_production_ui_far, "RVA_PRODUCTION_UI_FAR", HOT_RVA, 0x286540);
    HotConfig_Register("productionauto", (void*)&g_rva_production_ui_manpu, "RVA_PRODUCTION_UI_MANPU", HOT_RVA, 0x286CC0);
    // KB-108：判活计数函数（v1.6.19 起真正投入使用，且注册进热调参可热改）
    HotConfig_Register("productionauto", (void*)&g_rva_manpu_alive, "RVA_MANPU_ALIVE", HOT_RVA, 0x1569C0);
    HotConfig_Register("productionauto", (void*)&g_rva_production_baseid_resolve, "RVA_PRODUCTION_BASEID_RESOLVE", HOT_RVA, 0x1899C0);
    HotConfig_Register("productionauto", (void*)&g_rva_production_baseid_table, "RVA_PRODUCTION_BASEID_TABLE", HOT_RVA, 0x1124D60);
    HotConfig_Register("productionauto", (void*)&g_rva_production_recipe_table, "RVA_PRODUCTION_RECIPE_TABLE", HOT_RVA, 0x2C1E0);
    HotConfig_Register("productionauto", (void*)&g_rva_production_resolve_component, "RVA_PRODUCTION_RESOLVE_COMPONENT", HOT_RVA, 0x14B3F0);
    HotConfig_Register("productionauto", (void*)&g_rva_production_save_data_vtable, "RVA_PRODUCTION_SAVE_DATA_VTABLE", HOT_RVA, 0xE48230);
    HotConfig_Register("productionauto", (void*)&g_rva_production_status_event_lookup, "RVA_PRODUCTION_STATUS_EVENT_LOOKUP", HOT_RVA, 0x15D740);
    HotConfig_Register("productionauto", (void*)&g_rva_production_status_event_notify, "RVA_PRODUCTION_STATUS_EVENT_NOTIFY", HOT_RVA, 0x15D560);
    HotConfig_Register("productionauto", (void*)&g_rva_production_status_int_lookup, "RVA_PRODUCTION_STATUS_INT_LOOKUP", HOT_RVA, 0x15DB60);
    HotConfig_Register("productionauto", (void*)&g_rva_production_status_u64_lookup, "RVA_PRODUCTION_STATUS_U64_LOOKUP", HOT_RVA, 0x15DD20);
    HotConfig_Register("productionauto", (void*)&g_rva_production_target_accept, "RVA_PRODUCTION_TARGET_ACCEPT", HOT_RVA, 0x20E100);
    HotConfig_Register("productionauto", (void*)&g_rva_production_target_select, "RVA_PRODUCTION_TARGET_SELECT", HOT_RVA, 0x20DA30);
    HotConfig_Register("productionauto", (void*)&PA_POSITION_OFFSET, "PRODUCTION_POSITION_OFFSET", HOT_RVA, 0x0F0);  // v2: position 偏移热调参化
    // PRODUCTION_OUTPUT_TARGET_GLOBAL_RVA 已在旧 .inl 中定义，v2 暂不使用
    // HotConfig_Register("productionauto", (void*)&PRODUCTION_OUTPUT_TARGET_GLOBAL_RVA, "PRODUCTION_OUTPUT_TARGET_GLOBAL_RVA", HOT_RVA, 0x3150768);
    HotConfig_Register("productionauto", (void*)&g_rva_production_r12_guard, "RVA_PRODUCTION_R12_GUARD", HOT_RVA, 0x1733D7);
    HotConfig_Register("productionauto", (void*)&g_diagItemAdjustEnabled, "DIAG_ITEM_ADJUST", HOT_BOOL, 0);  // v1.1.45-diag: item_adjust 诊断热开关
    HotConfig_Register("productionauto", (void*)&g_paNativePushEnabled, "NATIVE_PUSH", HOT_BOOL, 0);  // v1.5.37
    HotConfig_Register("productionauto", (void*)&PA_CHEST_LINK_TILES, "CHEST_LINK_TILES", HOT_INT, 1);  // v1.6.48: 机器↔箱子联动格数=1 格内任意方位（v1.5.31 起只认对角=正方向断连，此值一直是死的，现在真正生效）
    HotConfig_Register("productionauto", (void*)&g_paTlsScopeFix, "TLS_SCOPE_FIX", HOT_BOOL, 1);  // v1.6.40: far/manpu 建 UI 前切换线程上下文到本机（0=只观察）
    HotConfig_Register("productionauto", (void*)&g_paUiCaseBFix, "UI_CASEB_FIX", HOT_BOOL, 0);  // v1.6.59: Case B 脏键恢复（走远后 PA 自建 UI 被销毁但键没清→清键强制重建）。默认关，实测开启验证
    HotConfig_Register("productionauto", (void*)&g_paUiNativeOnly, "UI_NATIVE_ONLY", HOT_BOOL, 0);  // v1.6.63 A/B: 1=PA 不自建 UI，交还游戏
    HotConfig_Register("productionauto", (void*)&g_paUiPiggyback, "UI_PIGGYBACK", HOT_BOOL, 0);   // v1.6.63 A/B: 1=游戏 near hook 内补建 far/manpu
    HotConfig_DumpCE("productionauto");
    s_productionBudgetSlot = qol::budget::Slot("ProductionAuto");  // v1.1.53: P0 探针注册
        // v1.1.45-diag: item_adjust 诊断 hook 常装（E9 rel32 安全方案，KB-061）。
    // 日志由 DIAG_ITEM_ADJUST 热开关控制（JSON 写 1 后 mod_tick Poll 热生效）。
    if (!NearbyInstallItemAdjustDiag()) {
        Log("[ProductionAuto] ItemAdjustDiag 安装失败（不影响主功能）\n");
    }

    Log("[ProductionAuto] HotConfig: 85 parameters registered\n");

    PaHudInit();  // F5 HUD 热键初始化
    PaChainInit(); // v1.5.100 链路浮窗（F6 热键切换，观测原版调用链）

    // v1.6.x: 观测层发布 PA 三层就绪态 + 启动 toast（AI 可零截图断言）
    qol::obs::SetInt("pa", "clock", clockReady ? 1 : 0);
    qol::obs::SetInt("pa", "nearby", nearbyReady ? 1 : 0);
    qol::obs::SetInt("pa", "production", productionReady ? 1 : 0);
    // v1.6.60：Case B 观测面板用独立命名空间（pa 8 槽已满，详见 .inl:2205 注释）；
    // 此处立即建槽，使观测器在维护链尚未跑起来时也能确认新 DLL 已加载。
    PaCaseBObsPublish();
    qol::obs::EventF(1, 2500, L"[PA] 已加载：观测层已接入（时钟/邻近/加工就绪）");

    Log("[ProductionAuto] v2 init done\n");
}

extern "C" __declspec(dllexport) void mod_tick(void) {
    static bool s_tickFirstCall = true;
    if (s_tickFirstCall) {
        s_tickFirstCall = false;
        Log("[ProductionAuto] mod_tick first call confirmed\n");
    }
    const uint64_t budgetT0 = qol::budget::NowUs();
    HotConfig_Poll();

    // v1.6.x: 观测层心跳（~10s 发布一次时钟源就绪态，供 AI 远程断言）
    static uint64_t s_obsHb = 0;
    if ((++s_obsHb % 600) == 0) {
        qol::obs::SetInt("pa", "clock",
            g_productionMainWorldGameClockSourceReady.load(std::memory_order_acquire) ? 1 : 0);
    }
    PaHudPoll();  // F5 HUD 切换轮询 + 周期刷新
    PaChainPoll();  // v1.5.100 链路浮窗 F6 切换 + 消息泵 + 周期刷新
    VerifyR12GuardPatch();  // R12 guard 周期完整性检查（节流 2s）
    qol::budget::Report(s_productionBudgetSlot,
                        qol::budget::NowUs() - budgetT0);  // v1.1.53: P0 自报
}

extern "C" __declspec(dllexport) void unload(void) {
    Log("[ProductionAuto] unload");
    PaUninstallR12Guard();  // 还原 R12 guard inline hook
    NearbyRemoveItemAdjustDiag();  // v1.1.45-diag: 还原 item_adjust 诊断 hook
    NearbyRemoveMainHook();        // v1.1.49: 还原 NearbyMainUpdate 主 hook (H2)

    // 还原 GameClock hook（还原 mainCall 原始5字节序言）
    if (g_midnightSlowHookReady) {
        const uintptr_t base =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (base) {
            unsigned char* mainCall = reinterpret_cast<unsigned char*>(
                base + RVA_GAME_TIME_MAIN_CALL);
            WriteCodePatchChecked(mainCall, expectedMainCall,
                                  sizeof(expectedMainCall));
            Log("[ProductionAuto] GameClock hook restored\n");
        }
        g_midnightSlowHookReady = false;
    }

    // 释放跳板 relay 内存
    if (g_midnightSlowRelay) {
        VirtualFree(g_midnightSlowRelay, 0, MEM_RELEASE);
        g_midnightSlowRelay = nullptr;
    }

    // Cleanup HUD window
    // ProductionHudCleanup();  // 旧 HUD 空桩（新 HUD 在下方 PaHudCleanup）
    PaHudCleanup();  // v2 HUD 窗口清理

    LogClose();
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID /*lpReserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hMod;
    } else if (reason == DLL_PROCESS_DETACH) {
        qol::obs::Shutdown();  // v1.6.x: 解观测层映射（仅本 DLL 视图）
    }
    return TRUE;
}
