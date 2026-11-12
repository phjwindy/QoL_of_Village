// safe_call.h —— SEH 安全的原生函数调用包装
// ====================================================================
// MSVC 的 __try/__except (SEH) 不能和带析构的 C++ 对象（bool、std::vector
// 等）在同一个函数块中使用（C2712）。本头文件提供一组 NOINLINE 的纯 POD
// wrapper 函数，把原生函数调用隔离在无 C++ 对象的作用域内。
//
// 用法（以 SickleHarvest 为例）：
//   #include "safe_call.h"
//
//   // bool 返回，1 参数
//   bool result = qol::SafeCallBool1(g_gimmickIsHarvestItem, status);
//
//   // int 返回，1 参数（SEH 返回哨兵值 -1）
//   int id = qol::SafeCallInt1(g_gimmickGetHarvestItemID, status);
//
//   // void 返回，2 参数
//   qol::SafeCallVoid2(g_spatialSearch, spatialIndex, bounds, callback, -1, -1);
//
// 设计要点：
//   1. 每个 wrapper 都是 __declspec(noinline)，防止编译器内联合并作用域
//   2. wrapper 内部只出现 POD 类型（标量、指针），不含任何带析构的对象
//   3. SEH 异常时返回哨兵值（bool=false, int=-1, void=直接吞掉）
//   4. 同时提供带 out 参数的版本，用于区分"调用失败"和"返回 false"
//
// 命名约定：
//   SafeCall<RetType><ArgCount>  —— 如 SafeCallBool1, SafeCallVoid2
//   SafeCallOut<RetType><ArgCount> —— 带 out 参数版本，返回 uint32_t 状态码
//
// 状态码约定（SafeCallOut 系列）：
//   0  = 未调用（fn 或 arg 为空）
//   1  = 调用成功，结果写入 out
//   0xFFFFFFFF = SEH 异常
// ====================================================================
#pragma once
#include <cstdint>
#include <windows.h>

// ============================================================
// 内部实现：每个特化都是独立的 NOINLINE 函数
// ============================================================

// ---- bool 返回值 ----

__declspec(noinline)
static bool SafeCallBool1Impl(bool (__fastcall *fn)(void*), void* a1) noexcept {
    __try { return fn(a1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline)
static bool SafeCallBool2Impl(bool (__fastcall *fn)(void*, void*), void* a1, void* a2) noexcept {
    __try { return fn(a1, a2); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline)
static bool SafeCallBool3Impl(bool (__fastcall *fn)(void*, void*, void*), void* a1, void* a2, void* a3) noexcept {
    __try { return fn(a1, a2, a3); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- int 返回值 ----

__declspec(noinline)
static int SafeCallInt1Impl(int (__fastcall *fn)(void*), void* a1) noexcept {
    __try { return fn(a1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

__declspec(noinline)
static int SafeCallInt2Impl(int (__fastcall *fn)(void*, void*), void* a1, void* a2) noexcept {
    __try { return fn(a1, a2); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

__declspec(noinline)
static int SafeCallInt3Impl(int (__fastcall *fn)(void*, void*, void*), void* a1, void* a2, void* a3) noexcept {
    __try { return fn(a1, a2, a3); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// ---- unsigned short 返回值 ----

__declspec(noinline)
static unsigned short SafeCallUShort1Impl(unsigned short (__fastcall *fn)(void*), void* a1) noexcept {
    __try { return fn(a1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

__declspec(noinline)
static unsigned short SafeCallUShort2Impl(unsigned short (__fastcall *fn)(void*, void*), void* a1, void* a2) noexcept {
    __try { return fn(a1, a2); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

__declspec(noinline)
static unsigned short SafeCallUShort3Impl(unsigned short (__fastcall *fn)(void*, void*, void*), void* a1, void* a2, void* a3) noexcept {
    __try { return fn(a1, a2, a3); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ---- void 返回值 ----

__declspec(noinline)
static void SafeCallVoid1Impl(void (__fastcall *fn)(void*), void* a1) noexcept {
    __try { fn(a1); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline)
static void SafeCallVoid2Impl(void (__fastcall *fn)(void*, void*), void* a1, void* a2) noexcept {
    __try { fn(a1, a2); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline)
static void SafeCallVoid3Impl(void (__fastcall *fn)(void*, void*, void*), void* a1, void* a2, void* a3) noexcept {
    __try { fn(a1, a2, a3); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline)
static void SafeCallVoid4Impl(void (__fastcall *fn)(void*, void*, void*, void*), void* a1, void* a2, void* a3, void* a4) noexcept {
    __try { fn(a1, a2, a3, a4); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- void* 返回值 ----

__declspec(noinline)
static void* SafeCallPtr1Impl(void* (__fastcall *fn)(void*), void* a1) noexcept {
    __try { return fn(a1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

__declspec(noinline)
static void* SafeCallPtr2Impl(void* (__fastcall *fn)(void*, void*), void* a1, void* a2) noexcept {
    __try { return fn(a1, a2); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

__declspec(noinline)
static void* SafeCallPtr3Impl(void* (__fastcall *fn)(void*, void*, void*), void* a1, void* a2, void* a3) noexcept {
    __try { return fn(a1, a2, a3); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ---- 带 out 参数的状态码版本 ----
// 返回 0=未调用, 1=成功, 0xFFFFFFFF=异常

__declspec(noinline)
static uint32_t SafeCallOutBool1Impl(bool (__fastcall *fn)(void*), void* a1, bool* out) noexcept {
    if (!fn || !a1 || !out) return 0;
    __try { *out = fn(a1); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFF; }
}

__declspec(noinline)
static uint32_t SafeCallOutInt1Impl(int (__fastcall *fn)(void*), void* a1, int* out) noexcept {
    if (!fn || !a1 || !out) return 0;
    __try { *out = fn(a1); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFF; }
}

__declspec(noinline)
static uint32_t SafeCallOutUShort1Impl(unsigned short (__fastcall *fn)(void*), void* a1, unsigned short* out) noexcept {
    if (!fn || !a1 || !out) return 0;
    __try { *out = fn(a1); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFF; }
}

__declspec(noinline)
static uint32_t SafeCallOutPtr1Impl(void* (__fastcall *fn)(void*), void* a1, void** out) noexcept {
    if (!fn || !a1 || !out) return 0;
    __try { *out = fn(a1); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFF; }
}

#ifdef __cplusplus
namespace qol {

// ============================================================
// 直接调用版（SEH 异常时返回默认值）
// ============================================================

// bool 返回
inline bool SafeCallBool1(bool (__fastcall *fn)(void*), void* a1) noexcept {
    if (!fn || !a1) return false;
    return SafeCallBool1Impl(fn, a1);
}

inline bool SafeCallBool2(bool (__fastcall *fn)(void*, void*), void* a1, void* a2) noexcept {
    if (!fn) return false;
    return SafeCallBool2Impl(fn, a1, a2);
}

inline bool SafeCallBool3(bool (__fastcall *fn)(void*, void*, void*), void* a1, void* a2, void* a3) noexcept {
    if (!fn) return false;
    return SafeCallBool3Impl(fn, a1, a2, a3);
}

// int 返回
inline int SafeCallInt1(int (__fastcall *fn)(void*), void* a1) noexcept {
    if (!fn || !a1) return -1;
    return SafeCallInt1Impl(fn, a1);
}

inline int SafeCallInt2(int (__fastcall *fn)(void*, void*), void* a1, void* a2) noexcept {
    if (!fn) return -1;
    return SafeCallInt2Impl(fn, a1, a2);
}

inline int SafeCallInt3(int (__fastcall *fn)(void*, void*, void*), void* a1, void* a2, void* a3) noexcept {
    if (!fn) return -1;
    return SafeCallInt3Impl(fn, a1, a2, a3);
}

// unsigned short 返回
inline unsigned short SafeCallUShort1(unsigned short (__fastcall *fn)(void*), void* a1) noexcept {
    if (!fn || !a1) return 0;
    return SafeCallUShort1Impl(fn, a1);
}

inline unsigned short SafeCallUShort2(unsigned short (__fastcall *fn)(void*, void*), void* a1, void* a2) noexcept {
    if (!fn) return 0;
    return SafeCallUShort2Impl(fn, a1, a2);
}

inline unsigned short SafeCallUShort3(unsigned short (__fastcall *fn)(void*, void*, void*), void* a1, void* a2, void* a3) noexcept {
    if (!fn) return 0;
    return SafeCallUShort3Impl(fn, a1, a2, a3);
}

// void 返回
inline void SafeCallVoid1(void (__fastcall *fn)(void*), void* a1) noexcept {
    if (!fn) return;
    SafeCallVoid1Impl(fn, a1);
}

inline void SafeCallVoid2(void (__fastcall *fn)(void*, void*), void* a1, void* a2) noexcept {
    if (!fn) return;
    SafeCallVoid2Impl(fn, a1, a2);
}

inline void SafeCallVoid3(void (__fastcall *fn)(void*, void*, void*), void* a1, void* a2, void* a3) noexcept {
    if (!fn) return;
    SafeCallVoid3Impl(fn, a1, a2, a3);
}

inline void SafeCallVoid4(void (__fastcall *fn)(void*, void*, void*, void*), void* a1, void* a2, void* a3, void* a4) noexcept {
    if (!fn) return;
    SafeCallVoid4Impl(fn, a1, a2, a3, a4);
}

// void* 返回
inline void* SafeCallPtr1(void* (__fastcall *fn)(void*), void* a1) noexcept {
    if (!fn || !a1) return nullptr;
    return SafeCallPtr1Impl(fn, a1);
}

inline void* SafeCallPtr2(void* (__fastcall *fn)(void*, void*), void* a1, void* a2) noexcept {
    if (!fn) return nullptr;
    return SafeCallPtr2Impl(fn, a1, a2);
}

inline void* SafeCallPtr3(void* (__fastcall *fn)(void*, void*, void*), void* a1, void* a2, void* a3) noexcept {
    if (!fn) return nullptr;
    return SafeCallPtr3Impl(fn, a1, a2, a3);
}

// ============================================================
// 带 out 参数版（返回状态码，可区分失败 vs 返回 false）
// ============================================================

inline uint32_t SafeCallOutBool1(bool (__fastcall *fn)(void*), void* a1, bool* out) noexcept {
    return SafeCallOutBool1Impl(fn, a1, out);
}

inline uint32_t SafeCallOutInt1(int (__fastcall *fn)(void*), void* a1, int* out) noexcept {
    return SafeCallOutInt1Impl(fn, a1, out);
}

inline uint32_t SafeCallOutUShort1(unsigned short (__fastcall *fn)(void*), void* a1, unsigned short* out) noexcept {
    return SafeCallOutUShort1Impl(fn, a1, out);
}

inline uint32_t SafeCallOutPtr1(void* (__fastcall *fn)(void*), void* a1, void** out) noexcept {
    return SafeCallOutPtr1Impl(fn, a1, out);
}

} // namespace qol
#endif
