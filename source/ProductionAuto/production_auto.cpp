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
// 绫诲瀷鍒悕锛坉input8.cpp 涓殑瀹氫箟锛?
// ============================================================
using u64 = std::uint64_t;
using u32 = std::uint32_t;

// ============================================================
// 日志（QoL_Shared）
// ============================================================
#include "logging.h"
#include "memory_cache.h"  // v1.1.27: FastRegion 区域缓存
#include "hotkey.h"        // 统一热键机制（HUD 切换键）

// ProductionAuto 日志开关：发布版关闭日志
// v1.1.28b-diag: 临时开启排查链式失效（v1.1.29 已修复，关闭）
// #define PRODUCTIONAUTO_LOGGING
// #define PRODUCTIONAUTO_LOGGING
#ifdef PRODUCTIONAUTO_LOGGING
  // 使用 QoL_Shared 的日志系统（logging.h 真实实现）
#else
  #define LogOpen(x)  ((void)0)
  #define Log(...)    ((void)0)
  #define LogV(...)   ((void)0)
  #define LogClose()  ((void)0)
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
// build 鎸囩汗楠岃瘉
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
// 甯搁噺锛?inl 闇€瑕佺殑澶栭儴瀹氫箟锛?
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

// ---- GameClock 鍙戝竷鍣ㄥ師瀛愬彉閲?----
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
// production_automation.inl 第588行引用此变量（原定义在 auto_pet.inl）
static std::atomic<u64> g_autoPetMorningReadyGeneration{0};
// production_automation.inl 第259行定义此变量，但 cpp 需在
// ProductionPublishMainWorldGameClock 在 .inl include 之前引用，因同一编译层（static），在此提前定义，.inl 中的定义需删除
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
// 引入 .inl 前向声明
// nearby_chest_sort.inl 和 production_automation.inl 之前编译
// 但调用了后者中定义的函数。原始 dinput8.cpp 在 L2548-2566 统一前向声明。
static u64 ProductionBeginMainCallback();
static void ProductionSetMainCallbackPhase(unsigned phase);
static void ProductionOnMainThreadPreUpdate(u64 callbackSequence,
                                            bool callbackWasInterrupted);
static void ProductionOnMainThreadUpdate();
static void ProductionCompleteMainCallback(u64 callbackSequence);
static void ProductionObserveWorldContext(
    bool valid, u64 naturalMorningGeneration = 0);
static bool ProductionModalInputIsBlocked();
static bool ProductionForceReleaseModalInputBlock();

// ============================================================
// 前向声明（production_hud.inl）
// HUD 刷新在 production_automation.inl 扫描完成后调用，
// 但 production_hud.inl 在 production_automation.inl 之后 include，
// 因此需要前向声明。
// ============================================================
static void ProductionHudRefresh();

// ============================================================
// 引入 .inl 实现（所有外部依赖已在上方提供）
// 顺序遵循原 dinput8.cpp
//   nearby（基础）、production_automation（核心/数据类型）、panel（UI）
// panel 引入 production 定义的 ProductionState/ProductionReason/g_productionBindings 等。
// 因此 HUD 窗口实现和插件入口必须在 .inl include 之后。
// ============================================================
#include "nearby_chest_sort.inl"
#include "production_automation.inl"
#include "production_hud.inl"

// production_auto.cpp -- ProductionAuto: 链式自动化（加工机自动投料/出料）//
// 从 dinput8.cpp 原型 .inl 提取的独立 DLL 插件。
// 实现逻辑见：
//   nearby_chest_sort.inl          -- 基础设施层（RVA 已更新至 build 24969282）

// ============================================================

//   production_automation.inl       -- 核心逻辑（RVA 已更新至 build 24969282）
//   processing_recipe_table.inl     -- 配方表（932 条，从 Automate 权威表生成）
// 本文件提供：
//   1. .inl 所需的全部外部依赖桩函数
//   2. GameClock 发布器（hook CGameTime::advance 主世界调用点，发布游戏时间）
//   3. AutoPet 桩函数（空实现，仅维持编译）
// 版本：1.1.28b，适配 build 25094764 (v1.09)
//
// .inl 在文件尾部按依赖顺序 #include 引入。


// ============================================================
// 鎻掍欢鍏ュ彛
// ============================================================

extern "C" __declspec(dllexport) void mod_init(void) {
    LogOpen("productionauto");
    Log("[ProductionAuto] mod_init loaded, build %s %s\n",
        SUPPORTED_BUILD_NUMBER, SUPPORTED_GAME_VERSION);

    g_module = GetModuleHandleW(nullptr);

    g_supportedExe = VerifyExeBuild();
    if (!g_supportedExe) {
        Log("[ProductionAuto] unsupported exe; feature disabled safely\n");
        return;
    }

    PackageOpenWriteGate();

    // 鍏堝畨瑁呮父鎴忔椂閽?hook锛堝彂甯冩椂闂达紝渚?Production scheduler 浣跨敤锛
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

    // 初始化 HUD 切换热键（从 qol_hotkeys.txt 读取 productionauto 行，默认 F5）
    QolHotKeysInit(&g_productionHudHotkeys, "productionauto");
    QolRegisterHotKey("productionauto", "F5");
    Log("[ProductionAuto] HUD hotkey registered (productionauto)\n");
}

extern "C" __declspec(dllexport) void mod_tick(void) {
    static bool s_tickFirstCall = true;
    if (s_tickFirstCall) {
        s_tickFirstCall = false;
        Log("[ProductionAuto] mod_tick first call confirmed\n");
    }
    ProductionHudPoll();
}

extern "C" __declspec(dllexport) void unload(void) {
    Log("[ProductionAuto] unload");

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
    ProductionHudCleanup();

    LogClose();
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID /*lpReserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hMod;
    }
    return TRUE;
}
