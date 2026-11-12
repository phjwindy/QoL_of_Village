// hook.cpp —— x64 5字节跳转钩子（自研，不依赖 MinHook）
// 采用"跳板页 + 保存原始5字节 + jmp 回 target+5"的安全模型：
//   *out_orig 指向跳板：先执行原函数前5字节，再跳回 target+5，可多次调用原函数。
// 注意：需要 target 前 5 字节长度内不存在跨指令边界问题，且原函数可安全重入。
// v2: trampoline RIP-relative 重定位 + HookRemoveEx 带字节还原。
// v3: relay 近距离跳板解决 >2GB 距离（见 AllocNear 注释）。
// v4: 两处 RWX 分配都向 QoL_crash\trampolines.txt 登记归属——
//     崩溃时若 RIP 落在"不属于任何模块"的地址，crash_dump.h v24 能据此
//     直接指认「这是哪个 MOD 的钩子跳板」，而不是让你猜。
#include "hook.h"
#include "crash_dump.h"
#include <windows.h>
#include <cstring>
#include <cstdint>

namespace qol {

// 计算 E9 rel32 位移并做 ±2GB 溢出检查（返回 false 表示溢出，禁止静默截断）
static bool MakeRel32(uintptr_t from, uintptr_t to, int32_t* out) {
    intptr_t delta = (intptr_t)(to - from);
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    *out = (int32_t)delta;
    return true;
}

#pragma pack(push, 1)
struct JmpAbs {
    uint8_t   mov_rax[2] = {0x48, 0xB8}; // mov rax, imm64
    uintptr_t imm64      = 0;
    uint8_t   jmp_rax[2] = {0xFF, 0xE0}; // jmp rax
};
struct JmpRel32 {
    uint8_t opcode = 0xE9;
    int32_t rel32  = 0;
};
#pragma pack(pop)

// ---- RIP-relative 指令检测与重定位 ----
// 检查 5 字节窗口内是否存在 RIP-relative 指令（ModRM 含 [rip+disp32]）。
// 如果存在，修正 disp32 使得重定位后仍指向原绝对地址。
// 返回 true 表示已处理（或无需处理），false 表示检测到不可重定位指令。
static bool RelocateRipRelative(uint8_t* code, uintptr_t oldRip, uintptr_t newRip) {
    // x64 RIP-relative 指令的特征：ModRM 字节的 mod=00, rm=101（即 [rip+disp32]）
    // 常见模式：8B/89/0F(8x)/FF(15) 等，后跟 4 字节 disp32
    // 简化策略：扫描 5 字节窗口内的每个可能位置，查找以 disp32 结尾的 RIP-relative 编码
    for (int i = 0; i < 5; ++i) {
        uint8_t b = code[i];
        // 单字节指令不可能是 RIP-relative 前缀，跳过常见前缀
        if (b == 0x40 || b == 0x41 || b == 0x42 || b == 0x43 ||
            b == 0x44 || b == 0x45 || b == 0x46 || b == 0x47 ||
            b == 0x48 || b == 0x49 || b == 0x4C || b == 0x4D ||
            b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3) {
            continue;
        }
        // 检查是否是带 ModRM 的操作码（需要至少 op+modrm+disp32 = 6 字节，
        // 但我们的窗口只有 5 字节，所以只在 i<=1 时才可能有完整 disp32）
        if (i > 1) break;  // disp32 需要 4 字节空间，op+modrm=2 字节，最多从 i=0 或 i=1 开始

        // 尝试解码：op (可选 REX prefix) + ModRM
        int modrmIdx = i;
        if (b >= 0x40 && b <= 0x4F) {
            // REX prefix，ModRM 在下一位
            if (i + 1 >= 5) continue;
            modrmIdx = i + 1;
        }

        // 检查 ModRM 字节是否指示 [rip+disp32]（mod=00, rm=101）
        uint8_t modrm = code[modrmIdx];
        uint8_t mod = (modrm >> 6) & 3;
        uint8_t rm = modrm & 7;
        if (mod == 0 && rm == 5) {
            // [rip+disp32]：disp32 从 modrmIdx+1 开始
            int disp32Idx = modrmIdx + 1;
            if (disp32Idx + 4 > 5) {
                // disp32 超出 5 字节窗口——指令跨越边界，无法安全重定位
                return false;
            }
            int32_t oldDisp;
            memcpy(&oldDisp, code + disp32Idx, 4);
            // 原指令的 RIP = oldRip + (disp32Idx + 4)（指令执行完后的 PC）
            // 原目标地址 = oldRip + (disp32Idx + 4) + oldDisp
            // 新 RIP = newRip + (disp32Idx + 4)
            // newDisp = 原目标地址 - 新 RIP = oldRip + (disp32Idx + 4) + oldDisp - (newRip + (disp32Idx + 4))
            //         = oldRip + oldDisp - newRip
            int64_t newDisp = (int64_t)oldDisp + (int64_t)oldRip - (int64_t)newRip;
            // 检查溢出
            if (newDisp < INT32_MIN || newDisp > INT32_MAX) return false;
            int32_t newDisp32 = (int32_t)newDisp;
            memcpy(code + disp32Idx, &newDisp32, 4);
            // 只处理第一处 RIP-relative（5 字节窗口内通常最多一处）
            return true;
        }
    }
    return true;  // 无 RIP-relative 指令，无需修正
}

// ---- v3：远端 DLL 安全的安装方式 ----
// 旧版直接 E9 rel32 跳到 detour，但插件 DLL 与 exe 常相距 >2GB（实测 11.6GB）
// → MakeRel32 失败 → 全部 hook 安装失败（探针 push=0 pop=0 collect=0 的根因）。
// 现在：在 target ±2GB 内分配一段 relay，relay 里用 mov rax,imm64; jmp rax
// 绝对跳转到 detour；target 只 E9 到 relay（距离必然达标）。
static uintptr_t AllocNear(uintptr_t target, size_t size) {
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    uintptr_t base = target & ~(gran - 1);
    for (int64_t step = gran; step < 0x70000000LL; step += gran) {
        uintptr_t up = base + (uintptr_t)step;
        void* p = VirtualAlloc((void*)up, size, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
        if (p) {
            const intptr_t d = (intptr_t)p - (intptr_t)target;
            if (d > INT32_MIN && d < INT32_MAX) return (uintptr_t)p;
            VirtualFree(p, 0, MEM_RELEASE);
        }
        if (base > (uintptr_t)step) {
            uintptr_t down = base - (uintptr_t)step;
            p = VirtualAlloc((void*)down, size, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
            if (p) {
                const intptr_t d = (intptr_t)p - (intptr_t)target;
                if (d > INT32_MIN && d < INT32_MAX) return (uintptr_t)p;
                VirtualFree(p, 0, MEM_RELEASE);
            }
        }
    }
    return 0;
}

bool HookInstall(uintptr_t target, uintptr_t detour, uintptr_t* out_orig) {
    if (!target || !detour || !out_orig) return false;

    // 1) 跳板：5 字节原指令 + mov rax,target+5; jmp rax（绝对回跳，不依赖距离）
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 64,
                                            MEM_COMMIT | MEM_RESERVE,
                                            PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(tramp, (void*)target, 5);
    if (!RelocateRipRelative(tramp, target, (uintptr_t)tramp)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    tramp[5] = 0x48; tramp[6] = 0xB8;                 // mov rax, imm64
    *(uintptr_t*)(tramp + 7) = target + 5;
    tramp[15] = 0xFF; tramp[16] = 0xE0;               // jmp rax
    FlushInstructionCache(GetCurrentProcess(), tramp, 17);
    // ★ 归属登记：崩溃取证用（owner 由 probe 地址反查本 DLL 得到）
    qol::detail::QolRegisterTrampoline((void*)&HookInstall, tramp, 64,
                                       "HookInstall-tramp", target + 5);

    // 2) relay：target 附近，mov rax,detour; jmp rax
    uintptr_t relay = AllocNear(target, 64);
    if (!relay) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    uint8_t* r = (uint8_t*)relay;
    r[0] = 0x48; r[1] = 0xB8;
    *(uintptr_t*)(r + 2) = detour;
    r[10] = 0xFF; r[11] = 0xE0;
    FlushInstructionCache(GetCurrentProcess(), r, 12);
    // ★ 归属登记：relay 的跳转目标就是 detour（位于本 MOD 的 DLL 内），
    //   这条记录把"野执行地址"与"具体 MOD 的 detour"绑定起来。
    qol::detail::QolRegisterTrampoline((void*)&HookInstall, (void*)relay, 64,
                                       "HookInstall-relay", detour);

    // 3) target: E9 rel32 -> relay
    int32_t rel = 0;
    if (!MakeRel32(target + 5, relay, &rel)) {
        VirtualFree((void*)relay, 0, MEM_RELEASE);
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect((void*)target, 5, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree((void*)relay, 0, MEM_RELEASE);
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    uint8_t patch[5] = {0xE9, 0, 0, 0, 0};
    memcpy(patch + 1, &rel, 4);
    memcpy((void*)target, patch, 5);
    VirtualProtect((void*)target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 5);

    *out_orig = (uintptr_t)tramp;
    return true;
}
// v2: 带原始字节和跳板指针的完整卸载
bool HookRemoveEx(uintptr_t target, const uint8_t* savedBytes, uintptr_t tramp) {
    if (!target || !savedBytes) return false;
    DWORD old = 0;
    if (!VirtualProtect((void*)target, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)target, savedBytes, 5);
    VirtualProtect((void*)target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 5);
    if (tramp) VirtualFree((void*)tramp, 0, MEM_RELEASE);
    return true;
}

bool HookRemove(uintptr_t target) {
    // 骨架版不实现运行时还原（插件卸载多随进程退出）。
    // 如需还原，请使用 HookRemoveEx(target, savedBytes, tramp)。
    (void)target;
    return false;
}

} // namespace qol