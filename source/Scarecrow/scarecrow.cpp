// scarecrow.cpp —— 稻草人与洒水器重叠 (v1.0.8 正式版)
//
// v1.0.3: SEH 保护 g_currentWorldKey/g_resolveGeometryStatus；发布版关闭日志。
//
// v1.0.6: v1.20 (build 25311578) 适配：7 个 RVA 重定位 + expected 数组新 exe 实读。
//        CURRENT_WORLD_KEY 为 TLS 存根模板(调用者锚点法定位 42/42 一致)。
//
// 原理（源自 BigL233 dinput8.cpp 1862-2489 行）：
//   放置稻草人时，游戏通过 3 个碰撞检测回调判断格子是否被占用。
//   本插件拦截这条调用链，当碰撞对象是洒水器（gimmick_sprinkler）时，
//   将碰撞结果从"冲突"改为"通过"，允许稻草人与洒水器放在同一格。
//   其他碰撞（稻草人自身、未知类型等）保持原样拒绝。
//
// 4 个 hook：
//   1. placement-entry        (RVA 0x1DE1A0)  主入口，检测当前放置物是否稻草人
//   2. same-land-predicate     (RVA 0x1E6E00)  同地块碰撞检测
//   3. existing-object-predicate (RVA 0x1E01E0) 已有物体碰撞检测
//   4. geometry-intersection-callback (vtable slot 0xE74D58 → RVA 0x6D0290) 几何相交回调
//
// 自动生效，无需按键。
// 适配 build 25311578 / v1.20。

#include <windows.h>
#include "obs.h"              // v1.6.x: 统一观测层（AI 可读寄存器 + toast）
#include <cstdint>
#include <atomic>
#include <cstring>

#include "logging.h"
#include "exe_guard.h"   // v-local: 统一游戏版本闸门
#include "budget.h"        // P0 帧耗时探针（跨 DLL 共享）
#include "state.h"         // P1-2 状态感知暂停（载入/菜单静默）
#include "hot_config.h"
#include "memory_cache.h"
#include "patch_safety.h"  // v1.0.2: FastRegion 区域缓存
#include "hook.h"          // v1.0.15: qol::HookInstall（5字节+RIP重定位+relay），几何回调专用

// 日志开关：发布版禁用日志输出
// 调试时取消注释下行即可开启日志
// 日志运行时开关（2026-10-24）：编译期宏 SCARECROW_LOGGING 退役，改由 QoL_Shared 运行时门控。
// 开启方式：游戏根 qol_log.cfg 写 level=1（约 1s 生效，免重编免重启）；缺省/level=0 为静默。

using u64 = std::uint64_t;
using u32 = std::uint32_t;

// ============================================================
// 常量（build 25311578 / v1.20）
// ============================================================

// ---- hook 目标 RVA ----
static volatile uintptr_t RVA_IS_PLACEMENT_GIMMICK = 0x1e1270;
static volatile uintptr_t RVA_PLACEMENT_LAND_PREDICATE = 0x1e9ed0;
static volatile uintptr_t RVA_PLACEMENT_OBJECT_PREDICATE = 0x1e32b0;
static volatile uintptr_t RVA_GEOMETRY_INTERSECTION_WRAPPER = 0x6e7500;
static volatile uintptr_t RVA_GEOMETRY_CALLBACK_VTABLE_SLOT = 0xE74D58;
static volatile uintptr_t RVA_CURRENT_WORLD_KEY = 0xe9070;
static volatile uintptr_t RVA_RESOLVE_GEOMETRY_STATUS = 0x6dac70;

// ---- 结构体偏移 ----
static volatile uintptr_t GIMMICK_DATA_HOLDER_OFFSET = 0x240;
static volatile uintptr_t GIMMICK_MODULE_NAME_OFFSET = 0x0E0;  // v1.20: 0xD8→0xE0

// ---- 字节签名 ----
static const unsigned char kExpectedPlacement[15] = {
    0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x55,
    0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55
};
static const unsigned char kExpectedLandPredicate[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30
};
static const unsigned char kExpectedObjectPredicate[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20
};
static const unsigned char kExpectedGeometryWrapper[20] = {
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x81,
        0xEC, 0xC0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05,
        0x2C, 0x5B, 0x95, 0x00,
    };
static const unsigned char kExpectedCurrentWorldKey[28] = {
        0x48, 0x83, 0xEC, 0x28, 0x65, 0x48, 0x8B, 0x04,
        0x25, 0x58, 0x00, 0x00, 0x00, 0xB9, 0x10, 0x00,
        0x00, 0x00, 0x48, 0x8B, 0x00, 0x8B, 0x04, 0x01,
        0x39, 0x05, 0x3E, 0x37,
    };
static const unsigned char kExpectedResolver[16] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x4C, 0x8B, 0xD9,
    0x44, 0x0F, 0xB6, 0xD2, 0x48, 0xBB, 0xB3, 0x01
};

// ============================================================
// 全局状态
// ============================================================
namespace G {
    uintptr_t base = 0;
    bool ready = false;
}

// 当前 DLL 实例的模块句柄（DllMain 记录）。用于防重复加载检测：
// 若进程内已存在一个"更早加载"的 scarecrow.dll，则本副本视为重复，跳过安装。
static HMODULE g_module = nullptr;
static bool    g_duplicate = false;   // 本副本是否被判定为重复加载
static bool    g_hookReady = false;
static std::atomic<bool> g_enabled{false};

// ---- 函数指针类型 ----
using PlacementFunction = bool (__fastcall *)(void*, u64, void*, bool, const void*, u64*);
using PlacementPredicateFunction = bool (__fastcall *)(void*, void*);
using CurrentWorldKeyFunction = int (__fastcall *)();
using ResolveGeometryStatusFunction = void** (__fastcall *)(void*, u64, void**);

// ---- 原始函数指针 ----
static PlacementFunction g_originalPlacement = nullptr;
static PlacementPredicateFunction g_originalLandPredicate = nullptr;
static PlacementPredicateFunction g_originalObjectPredicate = nullptr;
static PlacementPredicateFunction g_originalGeometryWrapper = nullptr;
static CurrentWorldKeyFunction g_currentWorldKey = nullptr;
static ResolveGeometryStatusFunction g_resolveGeometryStatus = nullptr;

// ============================================================
// 内存工具
// ============================================================
static bool WriteMem(void* target, const void* data, size_t size) {
    return qol::WritePatchChecked(target, data, size);
}

static bool IsReadable(const void* pointer, size_t size) {
    return qol_mem::IsReadable(pointer, size);
}

static bool SealExecutableMemory(void* address, size_t size) {
    DWORD previousProtection = 0;
    if (!address || size == 0) return false;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READ, &previousProtection))
        return false;
    return FlushInstructionCache(GetCurrentProcess(), address, size) != 0;
}

// ============================================================
// Gimmick 类型识别
// 通过 status+0x240 → holder → data → data+0x0D8 → 模块名字符串
// 判断是稻草人(gimmick_scarecrow)还是洒水器(gimmick_sprinkler)
// ============================================================

enum class GimmickKind : unsigned char {
    Unresolved,
    Scarecrow,
    Sprinkler,
    Other,
};

// 线程局部：当前正在放置的物体类型（Unresolved = 无追踪）
static thread_local GimmickKind g_pendingPlacementKind = GimmickKind::Unresolved;

// 互补配对判定：当前正在放置的物体与碰撞对象构成"稻草人↔洒水器"对
static bool IsPairedOverlap(GimmickKind pendingKind, GimmickKind candidateKind) {
    return (pendingKind == GimmickKind::Scarecrow &&
            candidateKind == GimmickKind::Sprinkler) ||
           (pendingKind == GimmickKind::Sprinkler &&
            candidateKind == GimmickKind::Scarecrow);
}

// ---- holder→kind 类型缓存 ----
// holder 是同一 gimmick 类型所有实例（预览 + 已放置）共享的不可变模块描述符。
// 运行期证据表明 CGimmickStatus 对象变化时 holder 保持不变，因此按 holder 缓存
// 类型判定结果，可将每次放置帧的第二次 VirtualQuery 从热路径完全移除。
// （模式参考权威源码 dinput8.cpp 1960-2053 行 g_statusTypeCache）
static constexpr unsigned SCARECROW_STATUS_TYPE_CACHE_SIZE = 64;
struct StatusTypeCacheEntry {
    void* holder;
    GimmickKind kind;
};
static thread_local StatusTypeCacheEntry
    g_statusTypeCache[SCARECROW_STATUS_TYPE_CACHE_SIZE] = {};
static thread_local unsigned g_statusTypeCacheNext = 0;

static GimmickKind IdentifyGimmick(void* status) {
    if (!status || !IsReadable(status, GIMMICK_DATA_HOLDER_OFFSET + sizeof(void*)))
        return GimmickKind::Unresolved;

    void* holder = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(status) + GIMMICK_DATA_HOLDER_OFFSET);
    if (!holder || !IsReadable(holder, sizeof(void*)))
        return GimmickKind::Unresolved;

    // 缓存命中：同一 holder 的类型判定结果在运行期内不变
    for (const StatusTypeCacheEntry& cached : g_statusTypeCache) {
        if (cached.holder == holder) return cached.kind;
    }

    void* data = *reinterpret_cast<void**>(holder);
    if (!data || !IsReadable(data, GIMMICK_MODULE_NAME_OFFSET + sizeof(void*)))
        return GimmickKind::Unresolved;

    const char* moduleName = *reinterpret_cast<const char**>(
        reinterpret_cast<uintptr_t>(data) + GIMMICK_MODULE_NAME_OFFSET);
    if (!moduleName || !IsReadable(moduleName, 32))
        return GimmickKind::Unresolved;

    GimmickKind kind = GimmickKind::Other;
    if (strcmp(moduleName, "gimmick_scarecrow") == 0)
        kind = GimmickKind::Scarecrow;
    else if (strcmp(moduleName, "gimmick_sprinkler") == 0)
        kind = GimmickKind::Sprinkler;

    // 写入缓存（环形 64 槽，覆盖最旧的条目）
    StatusTypeCacheEntry& cached =
        g_statusTypeCache[g_statusTypeCacheNext++ % SCARECROW_STATUS_TYPE_CACHE_SIZE];
    cached.holder = holder;
    cached.kind = kind;
    return kind;
}

// ============================================================
// Detour 函数
// ============================================================

// 主入口：拦截放置函数，检测当前放置物类型（稻草人或洒水器）
static bool __fastcall PlacementDetour(void* mapInformation, u64 position,
                                        void* targetStatusRef, bool option,
                                        const void* statusVector, u64* errorCode) {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return g_originalPlacement(mapInformation, position, targetStatusRef,
                                    option, statusVector, errorCode);
    }

    // 读取待放置物体的类型
    void* status = nullptr;
    if (IsReadable(targetStatusRef, sizeof(void*)))
        status = *reinterpret_cast<void**>(targetStatusRef);

    GimmickKind pendingKind = IdentifyGimmick(status);

    // 只有放置稻草人或洒水器时才需要追踪（互补配对用）
    if (pendingKind == GimmickKind::Scarecrow ||
        pendingKind == GimmickKind::Sprinkler) {
        g_pendingPlacementKind = pendingKind;
        Log("[Scarecrow] placement begin kind=%d position=0x%llx\n",
            static_cast<int>(pendingKind),
            static_cast<unsigned long long>(position));
    }

    bool result = g_originalPlacement(mapInformation, position, targetStatusRef,
                                       option, statusVector, errorCode);

    if (g_pendingPlacementKind != GimmickKind::Unresolved) {
        Log("[Scarecrow] placement end kind=%d result=%d\n",
            static_cast<int>(g_pendingPlacementKind), result ? 1 : 0);
        g_pendingPlacementKind = GimmickKind::Unresolved;
    }

    return result;
}

// 同地块碰撞检测 detour
static bool __fastcall LandPredicateDetour(void* callback, void* candidateRef) {
    bool nativeCollision = g_originalLandPredicate(callback, candidateRef);
    if (g_pendingPlacementKind == GimmickKind::Unresolved || !nativeCollision)
        return nativeCollision;

    void* status = IsReadable(candidateRef, sizeof(void*))
                       ? *reinterpret_cast<void**>(candidateRef) : nullptr;
    GimmickKind kind = IdentifyGimmick(status);
    if (IsPairedOverlap(g_pendingPlacementKind, kind)) {
        Log("[Scarecrow] land predicate: paired collision -> allow\n");
        return false;  // 放行互补碰撞
    }
    return nativeCollision;
}

// 已有物体碰撞检测 detour
static bool __fastcall ObjectPredicateDetour(void* callback, void* status) {
    bool nativeCollision = g_originalObjectPredicate(callback, status);
    if (g_pendingPlacementKind == GimmickKind::Unresolved || !nativeCollision)
        return nativeCollision;

    GimmickKind kind = IdentifyGimmick(status);
    if (IsPairedOverlap(g_pendingPlacementKind, kind)) {
        Log("[Scarecrow] object predicate: paired collision -> allow\n");
        return false;
    }
    return nativeCollision;
}

// 几何相交回调 detour
static bool __fastcall GeometryWrapperDetour(void* callback, void* candidateRef) {
    bool nativeCollision = g_originalGeometryWrapper(callback, candidateRef);
    if (g_pendingPlacementKind == GimmickKind::Unresolved || !nativeCollision)
        return nativeCollision;

    // candidateRef 指向一个 component，需要通过 resolver 获取 status
    // 两个原生调用（g_currentWorldKey / g_resolveGeometryStatus）均以 __try 包裹：
    // 本函数无带析构的 C++ 对象（纯 POD 局部变量），可直接内联 SEH，避免 C2712。
    void* component = IsReadable(candidateRef, sizeof(void*))
                          ? *reinterpret_cast<void**>(candidateRef) : nullptr;
    void* status = nullptr;
    if (component && g_currentWorldKey && g_resolveGeometryStatus) {
        int worldKey = -1;  // SEH 异常时为 -1 哨兵，跳过后续 resolver
        __try {
            worldKey = g_currentWorldKey();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[Scarecrow] geometry wrapper: currentWorldKey SEH exception code=0x%08X\n",
                GetExceptionCode());
        }
        if (worldKey != -1) {
            void* fallback = nullptr;
            void** resolved = nullptr;
            __try {
                resolved = g_resolveGeometryStatus(
                    component, static_cast<u64>(static_cast<u32>(worldKey)), &fallback);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("[Scarecrow] geometry wrapper: resolveGeometryStatus SEH exception code=0x%08X\n",
                    GetExceptionCode());
            }
            if (resolved) status = *resolved;
        }
    }

    GimmickKind kind = IdentifyGimmick(status);
    if (IsPairedOverlap(g_pendingPlacementKind, kind)) {
        Log("[Scarecrow] geometry wrapper: paired collision -> allow\n");
        return false;
    }
    return nativeCollision;
}

// ============================================================
// Hook 安装
// ============================================================

// 函数 hook：trampoline 方式（复制原始序言 + 跳回）
static bool InstallFunctionHook(const char* name, uintptr_t rva,
                                const unsigned char* expected, size_t length,
                                void* detour, void** original) {
    uintptr_t base = G::base;
    unsigned char* target = reinterpret_cast<unsigned char*>(base + rva);

    if (memcmp(target, expected, length) != 0) {
        Log("[Scarecrow] hook=%s rva=0x%llx byte check FAILED\n", name,
            static_cast<unsigned long long>(rva));
        return false;
    }

    // 分配 trampoline
    unsigned char* trampoline = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 96, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!trampoline) return false;

    // 复制原始序言 + 跳回 target+length
    memcpy(trampoline, expected, length);
    unsigned char jumpBack[14] = {0xFF, 0x25, 0, 0, 0, 0};
    *reinterpret_cast<u64*>(jumpBack + 6) =
        reinterpret_cast<u64>(target + length);
    memcpy(trampoline + length, jumpBack, sizeof(jumpBack));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 96);

    if (!SealExecutableMemory(trampoline, 96)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("[Scarecrow] hook=%s trampoline sealing failed\n", name);
        return false;
    }

    // 写入跳转：14 字节绝对跳转 + NOP 填充
    unsigned char hook[32] = {0xFF, 0x25, 0, 0, 0, 0};
    *reinterpret_cast<u64*>(hook + 6) = reinterpret_cast<u64>(detour);
    for (size_t i = 14; i < length; ++i) hook[i] = 0x90;

    *original = trampoline;  // 先发布 trampoline
    if (!WriteMem(target, hook, length) ||
        memcmp(target, hook, length) != 0) {
        *original = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("[Scarecrow] hook=%s write/readback FAILED\n", name);
        return false;
    }

    Log("[Scarecrow] hook=%s rva=0x%llx installed\n", name,
        static_cast<unsigned long long>(rva));
    return true;
}

// vtable hook：直接替换 vtable 槽位指针
static bool InstallVtableHook(const char* name, uintptr_t slotRva,
                               uintptr_t originalRva,
                               const unsigned char* expected,
                               size_t expectedLength, void* detour,
                               void** original) {
    uintptr_t base = G::base;
    unsigned char* originalFunction =
        reinterpret_cast<unsigned char*>(base + originalRva);

    if (memcmp(originalFunction, expected, expectedLength) != 0) {
        Log("[Scarecrow] hook=%s original function rva=0x%llx byte check FAILED\n",
            name, static_cast<unsigned long long>(originalRva));
        return false;
    }

    void** slot = reinterpret_cast<void**>(base + slotRva);
    void* expectedPointer = originalFunction;
    if (!IsReadable(slot, sizeof(void*)) || *slot != expectedPointer) {
        Log("[Scarecrow] hook=%s vtable slot rva=0x%llx pointer check FAILED\n",
            name, static_cast<unsigned long long>(slotRva));
        return false;
    }

    *original = expectedPointer;  // 原始函数体不改动，直接指向
    if (!WriteMem(slot, reinterpret_cast<const unsigned char*>(&detour),
                  sizeof(detour)) || *slot != detour) {
        *original = nullptr;
        Log("[Scarecrow] hook=%s vtable write FAILED\n", name);
        return false;
    }

    Log("[Scarecrow] hook=%s vtable_slot_rva=0x%llx installed\n", name,
        static_cast<unsigned long long>(slotRva));
    return true;
}

// 几何相交回调专用 hook：用 5 字节 E9+relay（qol::HookInstall），而非 14 字节绝对跳转。
// 根因（2026-10-29 定位）：该函数序言第 4 条是 RIP 相对的
//   `48 8B 05 2C 5B 95 00 = mov rax,[rip+0x955B2C]`（位于 +13）。
//   旧 InstallFunctionHook 把整段 20 字节序言原样拷进 trampoline 且**不重定位 RIP**，
//   trampoline 里这条读指令的 rip 变了、disp32 没变 → 读到未映射地址 → AV
//   → 游戏自带 VEH 抢先 TerminateProcess → 静默消失（"读档即崩且无日志"的真凶）。
// 5 字节 hook 只覆盖首条整指令 `48 89 5C 24 18`（mov [rsp+18],rbx），
//   回跳到 +5 继续执行，那条 RIP 指令保留在原址、rip 与 disp32 都正确 → 不再崩。
static bool InstallGeometryCallbackHook() {
    uintptr_t base = G::base;
    unsigned char* target = reinterpret_cast<unsigned char*>(
        base + RVA_GEOMETRY_INTERSECTION_WRAPPER);
    // 先整体校验 20 字节仍是预期函数（前 4 条整指令），再打 5 字节 hook
    if (memcmp(target, kExpectedGeometryWrapper,
               sizeof(kExpectedGeometryWrapper)) != 0) {
        Log("[Scarecrow] hook=geometry-intersection-callback rva=0x%llx byte check FAILED\n",
            static_cast<unsigned long long>(RVA_GEOMETRY_INTERSECTION_WRAPPER));
        return false;
    }
    uintptr_t orig = 0;
    if (!qol::HookInstall(reinterpret_cast<uintptr_t>(target),
                          reinterpret_cast<uintptr_t>(&GeometryWrapperDetour),
                          &orig)) {
        Log("[Scarecrow] hook=geometry-intersection-callback install FAILED\n");
        return false;
    }
    g_originalGeometryWrapper =
        reinterpret_cast<PlacementPredicateFunction>(orig);
    Log("[Scarecrow] hook=geometry-intersection-callback rva=0x%llx installed (5B)\n",
        static_cast<unsigned long long>(RVA_GEOMETRY_INTERSECTION_WRAPPER));
    return true;
}

static bool InstallHooks() {
    if (g_hookReady) return true;
    uintptr_t base = G::base;

    // 1. 验证辅助函数签名（几何相交回调 resolver 用）
    if (memcmp(reinterpret_cast<void*>(base + RVA_CURRENT_WORLD_KEY),
               kExpectedCurrentWorldKey, sizeof(kExpectedCurrentWorldKey)) != 0 ||
        memcmp(reinterpret_cast<void*>(base + RVA_RESOLVE_GEOMETRY_STATUS),
               kExpectedResolver, sizeof(kExpectedResolver)) != 0) {
        Log("[Scarecrow] geometry resolver signature FAILED\n");
        return false;
    }
    g_currentWorldKey = reinterpret_cast<CurrentWorldKeyFunction>(
        base + RVA_CURRENT_WORLD_KEY);
    g_resolveGeometryStatus = reinterpret_cast<ResolveGeometryStatusFunction>(
        base + RVA_RESOLVE_GEOMETRY_STATUS);

    // 2. 逐个安装 4 个 hook（顺序：3 个 predicate → 1 个主入口）
    // H7: 安装失败时逐个回滚已成功的 hook
    bool ready =
        InstallFunctionHook("same-land-predicate",
            RVA_PLACEMENT_LAND_PREDICATE,
            kExpectedLandPredicate, sizeof(kExpectedLandPredicate),
            reinterpret_cast<void*>(&LandPredicateDetour),
            reinterpret_cast<void**>(&g_originalLandPredicate)) &&
        InstallFunctionHook("existing-object-predicate",
            RVA_PLACEMENT_OBJECT_PREDICATE,
            kExpectedObjectPredicate, sizeof(kExpectedObjectPredicate),
            reinterpret_cast<void*>(&ObjectPredicateDetour),
            reinterpret_cast<void**>(&g_originalObjectPredicate)) &&
        // 2026-10-28 build 25640656：几何回调不再是「vtable 槽里直接放函数指针」——
        //   旧槽 0xE74D58 在本版装的是 0x47C620（一个 jmp thunk），而
        //   base+0x6E7500（= 旧 0x6D0290 的继任者，前 16 字节除 rip 位移完全一致）
        //   只出现在 std::function 包装体 `_Func_impl_no_alloc<lambda...>` 的调用槽，
        //   patch 那个槽会波及所有同类回调 → **不能再用槽替换法**。
        //   改为和另外三个 hook 一样，直接对函数本体做 trampoline hook。
        // v1.0.15：改用 5 字节 hook（见 InstallGeometryCallbackHook 注释）——
        //   旧 14 字节 trampoline 未重定位 RIP 相对指令，导致读档静默崩溃。
        InstallGeometryCallbackHook() &&
        InstallFunctionHook("placement-entry",
            RVA_IS_PLACEMENT_GIMMICK,
            kExpectedPlacement, sizeof(kExpectedPlacement),
            reinterpret_cast<void*>(&PlacementDetour),
            reinterpret_cast<void**>(&g_originalPlacement));

    if (!ready) {
        Log("[Scarecrow] hook chain incomplete; rolling back installed hooks\n");
        // H7: 逆序回滚已安装的 hook
        if (g_originalPlacement) {
            WriteMem(reinterpret_cast<void*>(base + RVA_IS_PLACEMENT_GIMMICK),
                     kExpectedPlacement, sizeof(kExpectedPlacement));
            VirtualFree(g_originalPlacement, 0, MEM_RELEASE);
            g_originalPlacement = nullptr;
        }
        if (g_originalGeometryWrapper) {
            // v1.0.15：5 字节 hook 回滚——还原首 5 字节并释放 trampoline
            //   （qol::HookInstall 分配的 relay 常驻 64B，卸载/回滚时不回收，可忽略）
            qol::HookRemoveEx(
                static_cast<uintptr_t>(base + RVA_GEOMETRY_INTERSECTION_WRAPPER),
                kExpectedGeometryWrapper,
                reinterpret_cast<uintptr_t>(g_originalGeometryWrapper));
            g_originalGeometryWrapper = nullptr;
        }
        if (g_originalObjectPredicate) {
            WriteMem(reinterpret_cast<void*>(base + RVA_PLACEMENT_OBJECT_PREDICATE),
                     kExpectedObjectPredicate, sizeof(kExpectedObjectPredicate));
            VirtualFree(g_originalObjectPredicate, 0, MEM_RELEASE);
            g_originalObjectPredicate = nullptr;
        }
        if (g_originalLandPredicate) {
            WriteMem(reinterpret_cast<void*>(base + RVA_PLACEMENT_LAND_PREDICATE),
                     kExpectedLandPredicate, sizeof(kExpectedLandPredicate));
            VirtualFree(g_originalLandPredicate, 0, MEM_RELEASE);
            g_originalLandPredicate = nullptr;
        }
        return false;
    }

    g_hookReady = true;
    g_enabled.store(true, std::memory_order_relaxed);
    Log("[Scarecrow] all hooks installed; scarecrow x sprinkler overlap enabled\n");
    return true;
}

// ============================================================
// 插件入口
// ============================================================
// 防重复加载检测：
// 若进程内已有另一份 scarecrow.dll 被加载（双载，多见于同一 DLL 因
// 历史原因同时存在于多个 mods 子目录），本副本跳过全部 hook 安装，
// 避免重复 patch 导致严重卡顿。
// 判据：GetModuleHandleW 返回的首个 scarecrow.dll 模块基址 ≠ 本实例，
//       说明已有一份更早的实例存在 → 本副本为重复。
static bool IsDuplicateInstance() {
    HMODULE first = GetModuleHandleW(L"scarecrow.dll");
    return (first != nullptr && first != g_module);
}

extern "C" __declspec(dllexport) void mod_init(void) {
    LogOpen("scarecrow");
    qol::obs::Init(nullptr);  // v1.6.x: 统一观测层接入
    qol::obs::SetInt("scarecrow", "ready", 1);
    qol::obs::EventF(1, 2500, L"[Scarecrow] 已加载：观测层接入");
    // v-local：游戏版本闸门（village.exe 非 v1.20 build 25311578 则不启用）
    if (!qol::ExeBuildSupported()) {
        Log("[Scarecrow] 游戏版本不匹配（非 v1.20 build 25311578）→ 本 MOD 不启用\n");
        return;
    }
    Log("[Scarecrow] mod_init 开始");

    // ---- HotConfig 热调参注册 ----
    HotConfig_Register("scarecrow", (void*)&RVA_IS_PLACEMENT_GIMMICK, "RVA_IS_PLACEMENT_GIMMICK", HOT_RVA, 0x1e1270);
    HotConfig_Register("scarecrow", (void*)&RVA_PLACEMENT_LAND_PREDICATE, "RVA_PLACEMENT_LAND_PREDICATE", HOT_RVA, 0x1e9ed0);
    HotConfig_Register("scarecrow", (void*)&RVA_PLACEMENT_OBJECT_PREDICATE, "RVA_PLACEMENT_OBJECT_PREDICATE", HOT_RVA, 0x1e32b0);
    HotConfig_Register("scarecrow", (void*)&RVA_GEOMETRY_INTERSECTION_WRAPPER, "RVA_GEOMETRY_INTERSECTION_WRAPPER", HOT_RVA, 0x6e7500);
    HotConfig_Register("scarecrow", (void*)&RVA_GEOMETRY_CALLBACK_VTABLE_SLOT, "RVA_GEOMETRY_CALLBACK_VTABLE_SLOT", HOT_RVA, 0xE74D58);
    HotConfig_Register("scarecrow", (void*)&RVA_CURRENT_WORLD_KEY, "RVA_CURRENT_WORLD_KEY", HOT_RVA, 0xe9070);
    HotConfig_Register("scarecrow", (void*)&RVA_RESOLVE_GEOMETRY_STATUS, "RVA_RESOLVE_GEOMETRY_STATUS", HOT_RVA, 0x6dac70);
    HotConfig_Register("scarecrow", (void*)&GIMMICK_DATA_HOLDER_OFFSET, "GIMMICK_DATA_HOLDER_OFFSET", HOT_RVA, 0x240);
    HotConfig_Register("scarecrow", (void*)&GIMMICK_MODULE_NAME_OFFSET, "GIMMICK_MODULE_NAME_OFFSET", HOT_RVA, 0x0E0);
    HotConfig_Poll();
    HotConfig_DumpCE("scarecrow");

    if (IsDuplicateInstance()) {
        g_duplicate = true;
        g_hookReady = false;
        g_enabled.store(false, std::memory_order_relaxed);
        G::ready = false;
        Log("[Scarecrow] [跳过] 检测到已存在另一份 scarecrow.dll 实例，本副本不安装 hook");
        return;
    }

    G::base = (uintptr_t)GetModuleHandleW(nullptr);
    Log("[Scarecrow] 游戏基址: 0x%llX", (unsigned long long)G::base);

    if (!InstallHooks()) {
        Log("[Scarecrow] [警告] hook 安装失败，功能未生效");
    } else {
        Log("[Scarecrow] 稻草人与洒水器重叠已启用");
    }

    G::ready = true;
    Log("[Scarecrow] mod_init 完成 (ready=%d enabled=%d)",
        G::ready ? 1 : 0, g_enabled.load(std::memory_order_relaxed) ? 1 : 0);
}

extern "C" __declspec(dllexport) void mod_tick(void) {
    qol::budget::BeginFrame();  // P0: 帧锚定（幂等，多 DLL 安全）
    static int s_bdgSlot = -1;  // P0 探针（惰性注册，析构自动上报）
    if (s_bdgSlot < 0) s_bdgSlot = qol::budget::Slot("Scarecrow");
    struct BdgGuard {
        int slot; uint64_t t0;
        ~BdgGuard() { qol::budget::Report(slot, qol::budget::NowUs() - t0); }
    } bdgGuard = { s_bdgSlot, qol::budget::NowUs() };

    if (QolGameBusy()) return;  // P1-2: 载入/菜单期间静默
    HotConfig_Poll();
}

extern "C" __declspec(dllexport) void unload(void) {
    Log("[Scarecrow] unload 开始");
    const uintptr_t base = G::base;

    // H7: 按各 hook 指针非空逐个还原，不依赖 g_hookReady 总开关
    if (base) {
        // 1. 还原 3 个函数 hook（写回原始序言字节）
        if (g_originalLandPredicate) {
            WriteMem(reinterpret_cast<void*>(base + RVA_PLACEMENT_LAND_PREDICATE),
                     kExpectedLandPredicate, sizeof(kExpectedLandPredicate));
        }
        if (g_originalObjectPredicate) {
            WriteMem(reinterpret_cast<void*>(base + RVA_PLACEMENT_OBJECT_PREDICATE),
                     kExpectedObjectPredicate, sizeof(kExpectedObjectPredicate));
        }
        if (g_originalPlacement) {
            WriteMem(reinterpret_cast<void*>(base + RVA_IS_PLACEMENT_GIMMICK),
                     kExpectedPlacement, sizeof(kExpectedPlacement));
        }

        // 2. 还原几何回调函数本体 hook（v1.0.15：5 字节回滚 + 释放 trampoline）
        if (g_originalGeometryWrapper) {
            qol::HookRemoveEx(
                static_cast<uintptr_t>(base + RVA_GEOMETRY_INTERSECTION_WRAPPER),
                kExpectedGeometryWrapper,
                reinterpret_cast<uintptr_t>(g_originalGeometryWrapper));
        }

        // 3. 释放 trampoline 内存（仅函数 hook 有 trampoline）
        if (g_originalLandPredicate) {
            VirtualFree(g_originalLandPredicate, 0, MEM_RELEASE);
            g_originalLandPredicate = nullptr;
        }
        if (g_originalObjectPredicate) {
            VirtualFree(g_originalObjectPredicate, 0, MEM_RELEASE);
            g_originalObjectPredicate = nullptr;
        }
        if (g_originalPlacement) {
            VirtualFree(g_originalPlacement, 0, MEM_RELEASE);
            g_originalPlacement = nullptr;
        }

        g_originalGeometryWrapper = nullptr;
    }

    g_hookReady = false;
    g_enabled.store(false, std::memory_order_relaxed);
    Log("[Scarecrow] unload 完成，所有 hook 已还原");
    LogClose();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)lpReserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        qol::obs::Shutdown();  // v1.6.x: 解观测层映射（仅本 DLL 视图）
    }
    return TRUE;
}
