// patch_safety.h —— 内存补丁安全写入（吸收自 BigL233 code_patch_safety.inl）
//
// 三项防御：
//   ① 参数空值校验
//   ② 写后读回验证（memcmp 确认实际写入）
//   ③ VirtualProtect 恢复原保护并校验返回值
//
// 用法：#include "patch_safety.h" 然后调用 qol::WritePatchChecked(dst, src, len)
#pragma once
#include <cstdint>
#include <cstring>
#include <windows.h>

namespace qol {

inline bool WritePatchChecked(void* dst, const void* src, size_t len) {
    if (!dst || !src || len == 0) return false;
    DWORD oldProt = 0;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &oldProt))
        return false;
    memcpy(dst, src, len);
    // ① 写后读回验证
    bool readbackOk = (memcmp(dst, src, len) == 0);
    // ② 恢复原保护并校验返回值
    DWORD tmpProt = 0;
    bool restored = (VirtualProtect(dst, len, oldProt, &tmpProt) != 0);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    return readbackOk && restored;
}

} // namespace qol
