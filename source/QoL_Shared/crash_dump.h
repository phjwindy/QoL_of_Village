// crash_dump.h — 游戏崩溃自动收集（header-only，自动接入）
//
// v2 架构（2026-10-09）：header-only inline 实现，由 logging.cpp 的 LogOpen()
// 自动调用 InstallCrashHandler()。任何 MOD 只要像以前一样 LogOpen(feature)，
// 就自动获得崩溃收集能力——零 per-MOD 改动。
//
// ============================================================
// v24（2026-10-29）——「一次取证定位到具体函数」
//
// 起因：23:43 进存档崩溃两次，`crash_info.txt` 里 `stack: 0 frames`，
//       完全看不出是谁调用了野地址（rip=0x13A7333000D，不属于任何模块）。
//
// 根因（PE 导出表实证，不是猜测）：
//   kernel32.dll / ntdll.dll 的导出表里 **没有 "CaptureStackBackTrace"**，
//   真实导出名是 **"RtlCaptureStackBackTrace"**。旧代码用错名字 →
//   GetProcAddress 返回 NULL → frames 恒为 0。
//   （实证：kernel32 导出 1698 个名字，只有 PssCaptureSnapshot /
//    RtlCaptureContext / RtlCaptureStackBackTrace，无 CaptureStackBackTrace）
//
// v24 四件新武器：
//   ① 真调用栈：绑定 RtlCaptureStackBackTrace（安装期解析，崩溃时不
//      调 GetProcAddress 免 loader lock 死锁）；失败则回退「栈上返回
//      地址扫描」，仍能给出调用链。
//   ② 每帧翻译：module.dll+0xRVA（模块表走 PEB 加载链表遍历，
//      不调 Toolhelp、不拿锁，且对崩溃后才加载的模块也有效）。
//      配合 Tools/fault_symbolize.py + DevKit/bin/x64/Release/*.pdb
//      可离线符号化到函数名。
//   ③ 跳板归属：识别落在 MEM_PRIVATE/RWX（非模块）的执行帧，解出它
//      的 jmp 目标（mov rax,imm64;jmp rax / E9 rel32）→ 直接指认
//      「这是 Scarecrow.dll 的 hook 跳板，回跳 village.exe+0x...」。
//      这是定位「跳到野地址」类崩溃的关键。
//   ④ 崩溃点内存属性：VirtualQuery 记 state/type/protect/allocationBase
//      → 一眼看出「在数据页上执行」「跳板页已被 VirtualFree」。
//      另有 trampolines.txt 登记表（hook.cpp 登记 owner），双重归属校验。
//
// 产物：
//     QoL_crash\last_fault.txt        每条一行（保持旧 keys 兼容 faultlog.py）
//                                     + 紧随一行 [QoL-veh-ctx] 摘要（归属/内存/首帧链）
//     QoL_crash\veh\<n>_<ts>.txt      完整现场（只对可疑异常写，见下方 SUSPECT 判据）
//     QoL_crash\<ts>\crash_info.txt   旧 UEF 路径产物
//     QoL_crash\trampolines.txt       跳板归属登记表（安装期写，非崩溃路径）
//
// ============================================================
// v29（2026-10-31）——「真栈 + 抗二次崩 + dump 失败可诊断」
//
// 起因：2026-10-31 22:00/22:03 两份 TimeFreeze 现场，帧表前 6 帧全是
//       处理器自身（QolWriteFaultLine/QolWriteDetail/…）+ ntdll 异常
//       分发链，真实故障点被埋到 #06/#08，极易被误读成"崩在 crash_dump
//       里"。根因不是处理器崩了，而是 **RtlCaptureStackBackTrace 在
//       故障线程栈上运行，第一个帧必然是它自己的调用者**。
//
// v29 三处加固：
//   ① 真栈：QolWalkStackFromContext() 用 RtlLookupFunctionEntry +
//      RtlVirtualUnwind 从 EXCEPTION_POINTERS->ContextRecord 展开，
//      out[0] 就是故障指令本身 —— 零处理器帧、零分发链噪声。
//      （RtlCaptureStackBackTrace 降级为回退路径）
//   ② 抗二次崩：模块表从栈上 14KB 数组改为静态缓冲；last_fault.txt
//      句柄在**安装期预打开**，崩溃期只 WriteFile 不再 CreateFileW；
//      并写 [QoL-stage] 阶段标记 —— 万一处理器自己再崩，日志能指出
//      死在哪一步。
//   ③ MiniDumpWriteDump 常年产出 0 字节（10-09 起 20 份全是 0B）：
//      改用 LOAD_LIBRARY_SEARCH_SYSTEM32 固定加载系统 dbghelp，dump
//      类型从 MiniDumpNormal 提到 DataSegs+ThreadInfo+Unloaded，并把
//      返回值 / GetLastError 写进 crash_info.txt —— 下次崩就知道为什么。
//      注意：进程内写 dump 天生受限于目标进程堆状态，堆一坏就写不出；
//      这条只有「进程外 WER 兜底」(Tools/wer_localdump.py) 能救。
//
// 作者：PHJ&消失的清风
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <signal.h>

namespace qol {
namespace detail {

typedef BOOL (WINAPI *PFN_MiniDumpWriteDump)(
    HANDLE, DWORD, HANDLE, DWORD,
    const void*, const void*, void*);

// ---------------------------------------------------------------
// 0. 无格式符输出工具（wsprintf 不支持 %llX/%llu，2026-10-28 实测
//    把这些字段打成了 "lX"/"lu"。自己转，顺带避开 CRT 堆）
// ---------------------------------------------------------------
inline char* QolAppendStr(char* p, const char* s) {
    while (*s) *p++ = *s++;
    return p;
}
inline char* QolAppendChar(char* p, char c) { *p++ = c; return p; }
inline char* QolAppendHex64(char* p, unsigned long long v, int minDigits) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) {
        const unsigned d = static_cast<unsigned>(v & 0xFULL);
        tmp[n++] = static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10);
        v >>= 4;
    }
    while (n < minDigits && n < 20) tmp[n++] = '0';
    while (n > 0) *p++ = tmp[--n];
    return p;
}
inline char* QolAppendDec64(char* p, unsigned long long v) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) {
        tmp[n++] = static_cast<char>('0' + static_cast<int>(v % 10));
        v /= 10;
    }
    while (n > 0) *p++ = tmp[--n];
    return p;
}
// ---------------------------------------------------------------
// 1. 内存可读性 / 属性（替代 IsBadReadPtr：后者已被弃用且会改页表）
// ---------------------------------------------------------------
inline bool QolReadable(const void* p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION m;
    ZeroMemory(&m, sizeof(m));
    if (!VirtualQuery(p, &m, sizeof(m))) return false;
    if (m.State != MEM_COMMIT) return false;
    if (m.Protect == PAGE_NOACCESS || (m.Protect & PAGE_GUARD)) return false;
    const unsigned long long off =
        reinterpret_cast<unsigned long long>(p) -
        reinterpret_cast<unsigned long long>(m.BaseAddress);
    return off + n <= (unsigned long long)m.RegionSize;
}

inline const char* QolTypeName(DWORD type) {
    switch (type) {
        case MEM_IMAGE:   return "MEM_IMAGE";
        case MEM_MAPPED:  return "MEM_MAPPED";
        case MEM_PRIVATE: return "MEM_PRIVATE";
        default:          return "?";
    }
}
inline const char* QolProtectName(DWORD pr, bool execOnly) {
    switch (pr & ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) {
        case PAGE_READONLY:          return execOnly ? "-" : "R";
        case PAGE_READWRITE:         return execOnly ? "-" : "RW";
        case PAGE_WRITECOPY:         return execOnly ? "-" : "WC";
        case PAGE_EXECUTE:           return execOnly ? "X" : "X";
        case PAGE_EXECUTE_READ:      return execOnly ? "X" : "XR";
        case PAGE_EXECUTE_READWRITE: return execOnly ? "X" : "XRW";
        case PAGE_EXECUTE_WRITECOPY: return execOnly ? "X" : "XWC";
        case PAGE_NOACCESS:          return execOnly ? "-" : "NA";
        default:                     return "?";
    }
}

// ---------------------------------------------------------------
// 2. 模块表 —— 遍历 PEB 加载链表
//
//    为什么不用 CreateToolhelp32Snapshot：它在崩溃上下文中会触碰
//    loader/heap 锁，若异常恰好发生在持锁路径会直接死锁（表现为
//    「游戏卡住不退出」，比崩溃更难查）。PEB InLoadOrderModuleList
//    是纯链表只读遍历，无需任何 Win32 调用。
// ---------------------------------------------------------------
struct QolModEnt {
    unsigned long long base;
    unsigned long long size;
    char name[40];
};
constexpr int kQolMaxMods = 256;

inline int QolWalkModules(QolModEnt* out, int maxOut) {
    int n = 0;
    // TEB: GS:[0x30]；TEB+0x60 = PEB*
    void* teb = reinterpret_cast<void*>(__readgsqword(0x30));
    if (!teb || !QolReadable(teb, 0x80)) return 0;
    unsigned long long pPeb = *reinterpret_cast<unsigned long long*>(
        static_cast<char*>(teb) + 0x60);
    if (!QolReadable(reinterpret_cast<void*>(pPeb), 0x80)) return 0;
    unsigned long long pLdr = *reinterpret_cast<unsigned long long*>(
        reinterpret_cast<char*>(pPeb) + 0x18);
    if (!QolReadable(reinterpret_cast<void*>(pLdr), 0x40)) return 0;

    // InLoadOrderModuleList 位于 PEB_LDR_DATA+0x10；Flink 直指
    // LDR_DATA_TABLE_ENTRY 首址（+0x00），故 entry == link 指针
    LIST_ENTRY* head = reinterpret_cast<LIST_ENTRY*>(
        reinterpret_cast<char*>(pLdr) + 0x10);
    if (!QolReadable(head, sizeof(LIST_ENTRY))) return 0;
    LIST_ENTRY* e = head->Flink;
    for (int i = 0; i < 512 && n < maxOut; ++i) {
        if (!e || !QolReadable(e, 0x70)) break;
        unsigned long long entry = reinterpret_cast<unsigned long long>(e);
        unsigned long long base = *reinterpret_cast<unsigned long long*>(entry + 0x30);
        unsigned long long size = (unsigned long long)
            *reinterpret_cast<unsigned int*>(entry + 0x40);
        // BaseDllName (UNICODE_STRING) @ +0x58 : Length +0x58, Buffer +0x60
        unsigned short len = *reinterpret_cast<unsigned short*>(entry + 0x58);
        void* buf = *reinterpret_cast<void**>(entry + 0x60);
        QolModEnt& m = out[n];
        m.base = base;
        m.size = size;
        char* np = m.name;
        if (buf && len > 0 && len < 240 && QolReadable(buf, len)) {
            const wchar_t* w = static_cast<const wchar_t*>(buf);
            int copy = len / 2;
            if (copy > 36) copy = 36;
            for (int c = 0; c < copy; ++c) {
                unsigned ch = (unsigned)w[c];
                if (ch == 0) break;
                *np++ = static_cast<char>(ch < 0x80 ? ch : '?');
            }
        }
        *np = '\0';
        if (m.name[0] == '\0') { m.name[0] = '?'; m.name[1] = '\0'; }
        ++n;
        LIST_ENTRY* nx = e->Flink;
        if (nx == head || nx == e) break;
        e = nx;
    }
    return n;
}

inline const QolModEnt* QolFindMod(const QolModEnt* mods, int n,
                                   unsigned long long addr) {
    for (int i = 0; i < n; ++i) {
        if (addr >= mods[i].base && addr < mods[i].base + mods[i].size)
            return &mods[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------
// 3. 调用栈采集
//    ★ 关键修正：导出名必须是 "RtlCaptureStackBackTrace" ★
//    函数指针在 InstallCrashHandler 时解析好，崩溃路径不再调 GetProcAddress。
// ---------------------------------------------------------------
typedef unsigned short (WINAPI *PFN_RtlCaptureStackBackTrace)(
    unsigned long, unsigned long, void**, unsigned long*);

inline PFN_RtlCaptureStackBackTrace& QolCaptureFn() {
    static PFN_RtlCaptureStackBackTrace s_p = nullptr;
    return s_p;
}
inline void QolResolveCapture() {
    PFN_RtlCaptureStackBackTrace& p = QolCaptureFn();
    HMODULE h = GetModuleHandleW(L"kernel32.dll");
    if (h) p = reinterpret_cast<PFN_RtlCaptureStackBackTrace>(
        GetProcAddress(h, "RtlCaptureStackBackTrace"));
    if (!p) {
        HMODULE n = GetModuleHandleW(L"ntdll.dll");
        if (n) p = reinterpret_cast<PFN_RtlCaptureStackBackTrace>(
            GetProcAddress(n, "RtlCaptureStackBackTrace"));
    }
}

// 回退：扫描栈上疑似返回地址。
// 判据：值落在某模块的镜像范围内，或其所在页可执行；再看紧邻前若干字节
//       是否像 call/jmp 指令（提高信噪比）。无模块归属且无调用前导的记为 '?'。
inline int QolScanReturnAddresses(const QolModEnt* mods, int modCount,
                                  unsigned long long rsp, void** out,
                                  int maxOut) {
    if (!modCount || !rsp) return 0;
    // 用 TEB 里的栈范围上限，避免越界到别的 region
    void* teb = reinterpret_cast<void*>(__readgsqword(0x30));
    unsigned long long stackBase = 0;
    if (teb && QolReadable(teb, 0x20))
        stackBase = *reinterpret_cast<unsigned long long*>(
            static_cast<char*>(teb) + 0x08);
    unsigned long long p = (rsp + 7ULL) & ~7ULL;
    unsigned long long end = stackBase ? stackBase : (p + 0x20000ULL);
    if (end <= p) end = p + 0x20000ULL;
    if (end - p > 0x40000ULL) end = p + 0x40000ULL;

    int n = 0;
    for (; p < end && n < maxOut; p += 8) {
        if (!QolReadable(reinterpret_cast<void*>(p), 8)) break;
        unsigned long long v = *reinterpret_cast<unsigned long long*>(p);
        if (v < 0x10000ULL) continue;
        const QolModEnt* m = QolFindMod(mods, modCount, v - 1);
        MEMORY_BASIC_INFORMATION mb;
        ZeroMemory(&mb, sizeof(mb));
        bool known = false;
        if (VirtualQuery(reinterpret_cast<void*>(v), &mb, sizeof(mb)) &&
            mb.State == MEM_COMMIT) {
            const DWORD ep = mb.Protect & ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
            if (ep == PAGE_EXECUTE || ep == PAGE_EXECUTE_READ ||
                ep == PAGE_EXECUTE_READWRITE || ep == PAGE_EXECUTE_WRITECOPY)
                known = true;
        }
        if (!m && !known) continue;
        bool callsite = false;
        if (v >= 8 && QolReadable(reinterpret_cast<void*>(v - 8), 8)) {
            const unsigned char* code =
                reinterpret_cast<const unsigned char*>(v - 8);
            if (code[3] == 0xE8) callsite = true;                    // call rel32
            else if (code[6] == 0xFF && ((code[7] & 0x38) == 0x10 ||
                                         (code[7] >= 0xD0 && code[7] <= 0xDF)))
                callsite = true;                                     // call/jmp [r] or call r
        }
        if (!callsite && !m) continue;   // 无归属且无调用前导：噪声，丢弃
        out[n++] = reinterpret_cast<void*>(v);
    }
    return n;
}

// ---------------------------------------------------------------
// 3b. 真栈回溯（v29）—— RtlVirtualUnwind，从异常 CONTEXT 出发
//
// 为什么不用 RtlCaptureStackBackTrace 当主路：VEH 跑在故障线程自己的
// 栈上，capture 的第一帧必然是处理器自身，真实故障点被埋到第 6~8 帧，
// 中间还夹着 ntdll 异常分发链（KiUserExceptionDispatcher / 回调链）。
// 从 ContextRecord 展开则第一帧就是故障指令，调用者链干净。
//
// 崩溃期安全性：只读内存 + 只写本栈上的 CONTEXT，不分配、不加锁、
// 不碰 CRT；每帧都做可读/可执行校验，脏数据不会被当成帧。
// ---------------------------------------------------------------
typedef unsigned long long (*PFN_RtlLookupFunctionEntry)(
    unsigned long long, unsigned long long*, UNWIND_HISTORY_TABLE*);
typedef void* (*PFN_RtlVirtualUnwind)(
    unsigned long, unsigned long long, unsigned long long, void*,
    CONTEXT*, void**, unsigned long long*, KNONVOLATILE_CONTEXT_POINTERS*);

inline PFN_RtlLookupFunctionEntry& QolLookupFn() {
    static PFN_RtlLookupFunctionEntry s_p = nullptr;
    return s_p;
}
inline PFN_RtlVirtualUnwind& QolUnwindFn() {
    static PFN_RtlVirtualUnwind s_p = nullptr;
    return s_p;
}
inline void QolResolveUnwind() {
    HMODULE n = GetModuleHandleW(L"ntdll.dll");
    if (!n) return;
    QolLookupFn() = reinterpret_cast<PFN_RtlLookupFunctionEntry>(
        reinterpret_cast<void*>(GetProcAddress(n, "RtlLookupFunctionEntry")));
    QolUnwindFn() = reinterpret_cast<PFN_RtlVirtualUnwind>(
        reinterpret_cast<void*>(GetProcAddress(n, "RtlVirtualUnwind")));
}

inline bool QolLooksExecutable(unsigned long long addr) {
    MEMORY_BASIC_INFORMATION mb;
    ZeroMemory(&mb, sizeof(mb));
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mb, sizeof(mb))) return false;
    if (mb.State != MEM_COMMIT) return false;
    const DWORD pr = mb.Protect & ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
    return pr == PAGE_EXECUTE || pr == PAGE_EXECUTE_READ ||
           pr == PAGE_EXECUTE_READWRITE || pr == PAGE_EXECUTE_WRITECOPY;
}

// out[0] = 故障指令（ctx->Rip），其后为调用者返回地址链。
inline int QolWalkStackFromContext(const CONTEXT* ctxIn, const QolModEnt* mods,
                                   int modCount, unsigned long long* out,
                                   int maxOut) {
    if (!ctxIn || !out || maxOut <= 1) return 0;
    PFN_RtlLookupFunctionEntry lookup = QolLookupFn();
    PFN_RtlVirtualUnwind unwind = QolUnwindFn();
    if (!lookup || !unwind) return 0;

    CONTEXT c;
    memcpy(&c, ctxIn, sizeof(CONTEXT));
    UNWIND_HISTORY_TABLE hist;
    ZeroMemory(&hist, sizeof(hist));

    unsigned long long pc = static_cast<unsigned long long>(c.Rip);
    int n = 0;
    out[n++] = pc;
    for (int guard = 0; guard < maxOut + 16 && n < maxOut; ++guard) {
        unsigned long long imageBase = 0;
        void* fnEntry = reinterpret_cast<void*>(lookup(pc, &imageBase, &hist));
        unsigned long long nextPc = 0;
        if (fnEntry) {
            void* handlerData = nullptr;
            unsigned long long estFrame = 0;
            KNONVOLATILE_CONTEXT_POINTERS cp;
            ZeroMemory(&cp, sizeof(cp));
            c.Rip = pc;
            unwind(0 /*UNW_FLAG_NHANDLER*/, imageBase, pc, fnEntry, &c,
                   &handlerData, &estFrame, &cp);
            nextPc = static_cast<unsigned long long>(c.Rip);
        } else {
            // 叶子函数（无 unwind info）：返回地址就在 [rsp]
            const unsigned long long sp = static_cast<unsigned long long>(c.Rsp);
            if (!QolReadable(reinterpret_cast<void*>(sp), 8)) break;
            nextPc = *reinterpret_cast<unsigned long long*>(sp);
            c.Rsp = sp + 8;
            c.Rip = nextPc;
        }
        if (nextPc == 0 || nextPc == pc) break;
        bool ok = (mods && QolFindMod(mods, modCount, nextPc) != nullptr);
        if (!ok) ok = QolLooksExecutable(nextPc);
        if (!ok) break;                       // 脏数据，收工（别当帧）
        pc = nextPc;
        out[n++] = pc;
        if (c.Rsp == 0) break;
    }
    return n;
}

// ---------------------------------------------------------------
// 4. 跳板（trampoline / relay）识别
//    hook.cpp 的跳板都是 VirtualAlloc 出来的 MEM_PRIVATE/RWX，
//    不在任何模块里。识别它们的形态并解出跳转目标：
//      relay      : [+0] 48 B8 <imm64>       FF E0   → 跳到 detour（在 MOD DLL 内）
//      trampoline : [+0] 原 5 字节
//                   [+5] 48 B8 <target+5>    FF E0   → 跳回游戏函数
//      E9 rel32   : [+0] E9 <rel32>
//    ⇒ 目标落在哪个模块，就能直接指认「谁的钩子、钩的谁」。
// ---------------------------------------------------------------
struct QolJumpInfo {
    int kind;                    // 0=无 1=48B8;FFE0 2=E9 rel32 3=FF25 indirection
    unsigned long long target;   // 绝对目标
    int patternOff;              // 形态起始偏移
};
inline QolJumpInfo QolDecodeJump(unsigned long long pc, int maxScan) {
    QolJumpInfo ji;
    ji.kind = 0; ji.target = 0; ji.patternOff = 0;
    for (int off = 0; off <= maxScan && off <= 8; ++off) {
        if (!QolReadable(reinterpret_cast<void*>(pc + off), 12)) break;
        const unsigned char* c =
            reinterpret_cast<const unsigned char*>(pc + off);
        if (c[0] == 0x48 && c[1] == 0xB8 && c[10] == 0xFF && c[11] == 0xE0) {
            unsigned long long imm = 0;
            for (int i = 0; i < 8; ++i)
                imm |= (unsigned long long)c[2 + i] << (8 * i);
            ji.kind = 1; ji.target = imm; ji.patternOff = off;
            return ji;
        }
        if (c[0] == 0xE9) {
            long rel = 0;
            rel |= (long)c[1] << 0;  rel |= (long)c[2] << 8;
            rel |= (long)c[3] << 16; rel |= (long)c[4] << 24;
            ji.kind = 2;
            ji.target = pc + off + 5ULL + (unsigned long long)(long long)rel;
            ji.patternOff = off;
            return ji;
        }
        if (c[0] == 0xFF && c[1] == 0x25) {
            long rel = 0;
            rel |= (long)c[2] << 0;  rel |= (long)c[3] << 8;
            rel |= (long)c[4] << 16; rel |= (long)c[5] << 24;
            unsigned long long slot =
                pc + off + 6ULL + (unsigned long long)(long long)rel;
            if (QolReadable(reinterpret_cast<void*>(slot), 8)) {
                ji.kind = 3;
                ji.target = *reinterpret_cast<unsigned long long*>(slot);
                ji.patternOff = off;
                return ji;
            }
        }
    }
    return ji;
}

// ---------------------------------------------------------------
// 5. 跳板归属登记表（安装期写入，崩溃期读）
//    每行：owner=<dll> modBase=0x.. tramp=0x.. size=0x.. kind=.. target=0x..
//    进程切换时重建（各 DLL 共享一个 named file mapping 判会话）
// ---------------------------------------------------------------
inline unsigned long long& QolTrampSessionStamp() {
    static unsigned long long s_stamp = 0;   // 本进程已初始化标记
    return s_stamp;
}
inline const wchar_t* QolTrampMapName() {
    return L"Local\\QoL_TrampRegistry_v1";
}
inline void QolGetRootW(wchar_t* buf, int cch) {
    GetModuleFileNameW(nullptr, buf, (DWORD)cch);
    wchar_t* slash = wcsrchr(buf, L'\\');
    if (slash) *slash = L'\0';
}

// ---------------------------------------------------------------
// 5b. 崩溃日志句柄：安装期预打开（v29）
//     崩溃期只 WriteFile —— CreateFileW 要拿当前目录 / 堆 / 安全描述符，
//     堆一坏就是二次崩溃，把第一现场一起带走。
//     句柄故意不关闭：进程内常驻，LOG 被外部读取用共享读写打开。
// ---------------------------------------------------------------
inline HANDLE& QolFaultLogHandle() {
    static HANDLE s_h = INVALID_HANDLE_VALUE;
    return s_h;
}
inline HANDLE QolEnsureFaultLog() {
    HANDLE& slot = QolFaultLogHandle();
    if (slot != INVALID_HANDLE_VALUE) return slot;
    wchar_t root[MAX_PATH] = {};
    QolGetRootW(root, MAX_PATH);
    wchar_t dir[MAX_PATH] = {};
    wsprintfW(dir, L"%s\\QoL_crash", root);
    CreateDirectoryW(dir, nullptr);
    wchar_t path[MAX_PATH] = {};
    wsprintfW(path, L"%s\\last_fault.txt", dir);
    slot = CreateFileW(path, FILE_APPEND_DATA,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return slot;
}
inline void QolRawWrite(HANDLE h, const char* s, int n) {
    if (h == INVALID_HANDLE_VALUE || !s || n <= 0) return;
    DWORD wr = 0;
    WriteFile(h, s, (DWORD)n, &wr, nullptr);
}
// 阶段标记：处理器万一自己再崩，凭这一行能看出死在哪一步。
inline void QolStage(const char* key, unsigned long long val) {
    char buf[96];
    char* p = buf;
    p = QolAppendStr(p, "[QoL-stage] ");
    p = QolAppendStr(p, key);
    p = QolAppendStr(p, "=");
    p = QolAppendDec64(p, val);
    p = QolAppendStr(p, "\r\n");
    *p = '\0';
    QolRawWrite(QolFaultLogHandle(), buf, static_cast<int>(p - buf));
}

// owner 模块名：用返回地址反查自己所在的 DLL
inline void QolOwnerModuleName(void* probe, char* out, int cch) {
    out[0] = '\0';
    HMODULE hm = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)probe, &hm) && hm) {
        GetModuleFileNameA(hm, out, cch);
        char* slash = strrchr(out, '\\');
        if (slash) memmove(out, slash + 1, strlen(slash + 1) + 1);
    }
    if (out[0] == '\0') memcpy(out, "?", 2);
}

inline void QolRegisterTrampoline(void* probe, void* addr, unsigned long long size,
                                  const char* kind, unsigned long long target) {
    // 会话判定：新进程 → 重建文件（旧会话残留会误导归属）
    bool newSession = false;
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                    PAGE_READWRITE, 0, 32, QolTrampMapName());
    if (map) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) newSession = true;
        void* view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 32);
        if (view) {
            DWORD* pd = (DWORD*)view;
            const DWORD pid = GetCurrentProcessId();
            if (pd[0] != 0 && pd[0] != pid) newSession = true;
            pd[0] = pid;
            // view/map 故意不释放：进程内保持对象存活
        }
    }
    if (newSession) QolTrampSessionStamp() = GetTickCount64();

    wchar_t root[MAX_PATH] = {};
    QolGetRootW(root, MAX_PATH);
    wchar_t dir[MAX_PATH] = {};
    wsprintfW(dir, L"%s\\QoL_crash", root);
    CreateDirectoryW(dir, nullptr);
    wchar_t path[MAX_PATH] = {};
    wsprintfW(path, L"%s\\trampolines.txt", dir);

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    if (newSession) SetEndOfFile(h);   // 新会话：截断旧记录

    char owner[MAX_PATH] = {};
    QolOwnerModuleName(probe, owner, MAX_PATH);
    char line[600];
    char* p = line;
    p = QolAppendStr(p, "owner=");
    p = QolAppendStr(p, owner);
    p = QolAppendStr(p, " kind=");
    p = QolAppendStr(p, kind ? kind : "?");
    p = QolAppendStr(p, " tramp=0x");
    p = QolAppendHex64(p, (unsigned long long)(uintptr_t)addr, 0);
    p = QolAppendStr(p, " size=0x");
    p = QolAppendHex64(p, size, 0);
    p = QolAppendStr(p, " target=0x");
    p = QolAppendHex64(p, target, 0);
    p = QolAppendStr(p, "\r\n");
    *p = '\0';
    DWORD written = 0;
    WriteFile(h, line, (DWORD)lstrlenA(line), &written, nullptr);
    CloseHandle(h);
}

// 崩溃期：按地址查登记表（文本文件，容失败）
inline bool QolLookupTrampOwner(unsigned long long addr, char* ownerOut,
                                int cch, char* kindOut, int kcch) {
    ownerOut[0] = '\0'; kindOut[0] = '\0';
    static char buf[1 << 16];
    wchar_t root[MAX_PATH] = {};
    QolGetRootW(root, MAX_PATH);
    wchar_t path[MAX_PATH] = {};
    wsprintfW(path, L"%s\\QoL_crash\\trampolines.txt", root);
    HANDLE h = CreateFileW(path, FILE_GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD need = 0;
    char needle[32];
    {
        char* p = QolAppendStr(needle, "0x");
        p = QolAppendHex64(p, addr, 0);
        *p = '\0';
    }
    // 简单逐行匹配 owner= 与 target= 字段足矣，不做完整解析
    unsigned long total = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf + total, sizeof(buf) - total - 1, &got, nullptr) ||
            got == 0) break;
        total += got;
        buf[total] = '\0';
        if (total + 1 >= sizeof(buf)) break;
    }
    CloseHandle(h);
    if (!total) return false;
    char* line = buf;
    while (line && *line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strstr(line, needle)) {
            char* o = strstr(line, "owner=");
            char* k = strstr(line, " kind=");
            if (o) {
                o += 6;
                char* sp = strchr(o, ' ');
                int n = sp ? (int)(sp - o) : (int)strlen(o);
                if (n >= cch) n = cch - 1;
                memcpy(ownerOut, o, n); ownerOut[n] = '\0';
            }
            if (k) {
                k += 6;
                int n = 0;
                while (k[n] && k[n] != ' ' && n < kcch - 1) { kindOut[n] = k[n]; ++n; }
                kindOut[n] = '\0';
            }
            return true;
        }
        if (!nl) break;
        line = nl + 1;
    }
    return false;
}

// ---------------------------------------------------------------
// 6. 详细现场写出（供 Tools/fault_symbolize.py 消费）
//    可疑才写：非模块执行点 / 含跳板帧 / 前 3 次；限速保护以免刷爆磁盘
// ---------------------------------------------------------------
// ===============================================================
inline void QolWriteDetail(EXCEPTION_POINTERS* ep, const char* why) {
    static int s_seq = 0;
    static DWORD s_lastMs = 0;
    if (s_seq >= 24) return;                       // 单进程上限
    const DWORD now = GetTickCount();
    if (s_seq > 0 && now - s_lastMs < 1500) return; // 1.5s 内只记一次
    s_lastMs = now;
    const int seq = ++s_seq;

    static QolModEnt s_modsB[kQolMaxMods];   // v29：静态，别吃 14KB 栈
    QolModEnt* mods = s_modsB;
    ZeroMemory(mods, sizeof(s_modsB));
    int modCount = QolWalkModules(mods, kQolMaxMods);

    const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    const CONTEXT* ctx = ep->ContextRecord;
    if (!rec || !ctx) return;
    const unsigned long long rip = ctx->Rip;

    unsigned long long stack[64] = {};
    int frames = 0;
    const char* method = "RtlVirtualUnwind";
    frames = QolWalkStackFromContext(ctx, mods, modCount, stack, 64);
    if (frames < 2) {
        // 回退 A：运行时 capture（含处理器帧，但至少有链）
        void* f2[64] = {};
        int g = 0;
        PFN_RtlCaptureStackBackTrace cap = QolCaptureFn();
        if (cap) {
            unsigned short got = cap(0, 62, f2, nullptr);
            g = (int)got;
            if (g > 62) g = 62;
        }
        if (g >= 2) {
            for (int i = 0; i < g; ++i) stack[i] = (unsigned long long)(uintptr_t)f2[i];
            frames = g;
            method = "RtlCaptureStackBackTrace";
        } else {
            // 回退 B：栈扫描（最脏，但能兜底）
            int got = QolScanReturnAddresses(mods, modCount, ctx->Rsp, f2, 62);
            for (int i = 0; i < got && i < 64; ++i)
                stack[i] = (unsigned long long)(uintptr_t)f2[i];
            frames = got > 64 ? 64 : got;
            method = "stack-scan";
        }
    }
    QolStage("detail-stack", (unsigned long long)frames);

    wchar_t root[MAX_PATH] = {};
    QolGetRootW(root, MAX_PATH);
    wchar_t dir[MAX_PATH] = {}, sub[MAX_PATH] = {};
    wsprintfW(dir, L"%s\\QoL_crash", root);
    CreateDirectoryW(dir, nullptr);
    wsprintfW(sub, L"%s\\veh", dir);
    CreateDirectoryW(sub, nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t fileName[MAX_PATH] = {};
    wsprintfW(fileName, L"%s\\%02d_%02u%02u%02u.txt", sub, seq,
              st.wHour, st.wMinute, st.wSecond);

    HANDLE h = CreateFileW(fileName, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    // W()：格式化后直写文件。这里用 _vsnprintf 而非 wsprintfA，是因为
    // wsprintfA 不认 %llX/%llu（2026-10-28 实测打成 "lX"/"lu"）。
    // 只用 %s/%d/%u/%llX 这些不需要浮点与 locale 的转换，不触堆。
    DWORD wr = 0;
    char line[4096];
    auto W = [&](const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf(line, sizeof(line) - 1, fmt, ap);
        va_end(ap);
        if (n > 0) { line[n] = '\0'; WriteFile(h, line, (DWORD)n, &wr, nullptr); }
    };

    W("===== QoL crash detail #%d =====\r\n", seq);
    W("time      : %04u-%02u-%02u %02u:%02u:%02u.%03u\r\n",
      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
      st.wMilliseconds);
    W("reason    : %s\r\n", why ? why : "?");
    W("pid/tid   : %u / %u\r\n", GetCurrentProcessId(), GetCurrentThreadId());
    W("exception : 0x%08X\r\n", rec->ExceptionCode);
    unsigned long long info1 = 0;
    if (rec->NumberParameters >= 2) info1 = rec->ExceptionInformation[1];
    W("faultadr  : 0x%016llX\r\n", (unsigned long long)info1);
    if (rec->ExceptionCode == 0xC0000005 && rec->NumberParameters >= 1) {
        const unsigned long long t = rec->ExceptionInformation[0];
        W("faulttype : %s\r\n", t == 0 ? "read" : t == 1 ? "write" : t == 8 ? "DEP-exec" : "other");
    }

    // ---- 崩溃点：归属 + 内存属性 ----
    {
        MEMORY_BASIC_INFORMATION mb;
        ZeroMemory(&mb, sizeof(mb));
        const QolModEnt* m = QolFindMod(mods, modCount, rip);
        W("rip       : 0x%016llX  -> %s\r\n", rip,
          m ? m->name : "<NOT IN ANY MODULE>");
        if (m) W("           module rva = 0x%llX (base 0x%016llX)\r\n",
                 rip - m->base, m->base);
        if (VirtualQuery((void*)rip, &mb, sizeof(mb))) {
            const char* wown = "?";
            char owner[64] = {}, kind[32] = {};
            if (!m && QolLookupTrampOwner((unsigned long long)mb.AllocationBase,
                                          owner, sizeof(owner), kind, sizeof(kind)))
                wown = "trampolines.txt";
            W("memext    : allocBase=0x%016llX regionSize=0x%llX\r\n"
              "            state=%s type=%s protect=%s(%s)\r\n",
              (unsigned long long)mb.AllocationBase,
              (unsigned long long)mb.RegionSize,
              mb.State == MEM_COMMIT ? "COMMIT" : mb.State == MEM_RESERVE ? "RESERVE" : "FREE",
              QolTypeName(mb.Type), QolProtectName(mb.Protect, false),
              QolProtectName(mb.Protect, true)[0] == 'X' ? "executable" : "NOT-EXECUTABLE");
            if (!m && owner[0])
                W("            owner(from %s) = %s kind=%s\r\n", wown, owner, kind);
        }
    }

    W("regs      : rax=0x%016llX rcx=0x%016llX rdx=0x%016llX r8=0x%016llX\r\n"
      "            r9=0x%016llX rdi=0x%016llX rsi=0x%016llX rbx=0x%016llX\r\n"
      "            rbp=0x%016llX rsp=0x%016llX rip=0x%016llX\r\n",
      ctx->Rax, ctx->Rcx, ctx->Rdx, ctx->R8,
      ctx->R9, ctx->Rdi, ctx->Rsi, ctx->Rbx,
      ctx->Rbp, ctx->Rsp, ctx->Rip);

    // ---- 每帧 ----
    W("stack     : %d frames via %s\r\n", frames, method);
    for (int i = 0; i < frames; ++i) {
        const unsigned long long pc = stack[i];
        const QolModEnt* m = QolFindMod(mods, modCount, pc);
        if (m) {
            W("  #%02d 0x%016llX  %s+0x%llX\r\n", i, pc, m->name, pc - m->base);
        } else {
            const QolJumpInfo ji = QolDecodeJump(pc, 8);
            char dek[64] = {};
            if (ji.kind) {
                const QolModEnt* tm = QolFindMod(mods, modCount, ji.target);
                _snprintf(dek, sizeof(dek), " kind=%d target=0x%llX %s%s",
                          ji.kind, ji.target,
                          tm ? "-> " : "", tm ? tm->name : "<unresolved>");
            } else {
                memcpy(dek, " kind=unknown", 14);
            }
            char owner[64] = {}, knd[32] = {};
            bool knownOwner = false;
            MEMORY_BASIC_INFORMATION mb;
            ZeroMemory(&mb, sizeof(mb));
            if (VirtualQuery((void*)pc, &mb, sizeof(mb)))
                knownOwner = QolLookupTrampOwner(
                    (unsigned long long)mb.AllocationBase, owner, sizeof(owner),
                    knd, sizeof(knd));
            unsigned char codeBuf[12] = {};
            if (QolReadable((void*)pc, 12)) memcpy(codeBuf, (void*)pc, 12);
            W("  #%02d 0x%016llX  <NON-MODULE>%s%s%s\r\n"
              "            code=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
              i, pc, dek,
              knownOwner ? "  [registered: owner=" : "",
              knownOwner ? owner : "",
              knownOwner ? "]" : "",
              codeBuf[0], codeBuf[1], codeBuf[2], codeBuf[3],
              codeBuf[4], codeBuf[5], codeBuf[6], codeBuf[7],
              codeBuf[8], codeBuf[9], codeBuf[10], codeBuf[11]);
        }
    }
    QolStage("detail", (unsigned long long)frames);
    CloseHandle(h);
}

// ---------------------------------------------------------------
// ---------------------------------------------------------------
// 6b. C 运行时死亡钩子（v25 新增）
//     为什么需要：std::terminate / abort / 安全 CRT 无效参数 / 未捕获 C++
//     异常都**不走 SEH**（不是 0xC0000005 之类），旧 VEH 完全看不到，
//     表现为「一按进存档就静默消失、无 last_fault、无 WER dump、无事件日志」。
//     这些处理器在【正常栈】上运行（terminate 时栈还在 throw 现场），
//     所以能直接 RtlCaptureStackBackTrace + 写文件，给出完整调用链。
// ---------------------------------------------------------------
inline void QolWriteDeathFrames(const char* why) {
    static volatile LONG s_wrote = 0;
    if (InterlockedExchange(&s_wrote, 1) != 0) return;   // 只写一次

    void* fr[48] = {};
    int n = 0;
    PFN_RtlCaptureStackBackTrace cap = QolCaptureFn();
    if (cap) {
        unsigned short g = cap(1, 47, fr, nullptr);   // skip 自身 1 帧
        n = (int)g; if (n > 47) n = 47;
    }
    QolModEnt mods[kQolMaxMods];
    ZeroMemory(mods, sizeof(mods));
    int modCount = QolWalkModules(mods, kQolMaxMods);

    wchar_t gameRoot[MAX_PATH] = {};
    QolGetRootW(gameRoot, MAX_PATH);
    wchar_t path[MAX_PATH] = {};
    wsprintfW(path, L"%s\\QoL_crash\\last_fault.txt", gameRoot);
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    {
        char head[256];
        char* p = QolAppendStr(head, "\r\n===== QoL-death ");
        p = QolAppendStr(p, why ? why : "?");
        p = QolAppendStr(p, " pid=");
        p = QolAppendDec64(p, GetCurrentProcessId());
        p = QolAppendStr(p, " tid=");
        p = QolAppendDec64(p, GetCurrentThreadId());
        p = QolAppendStr(p, " =====\r\n");
        *p = '\0';
        WriteFile(h, head, (DWORD)(p - head), &wr, nullptr);
    }
    char line[1024];
    for (int i = 0; i < n; ++i) {
        const unsigned long long pc = (unsigned long long)(uintptr_t)fr[i];
        const QolModEnt* m = QolFindMod(mods, modCount, pc);
        char* p = line;
        p = QolAppendStr(p, "  #");
        if (i < 10) *p++ = (char)('0' + i);
        else { *p++ = (char)('0' + i / 10); *p++ = (char)('0' + i % 10); }
        p = QolAppendStr(p, " 0x");
        p = QolAppendHex64(p, pc, 0);
        p = QolAppendStr(p, "  ");
        p = QolAppendStr(p, m ? m->name : "?");
        p = QolAppendStr(p, "+0x");
        p = QolAppendHex64(p, m ? pc - m->base : 0ULL, 0);
        p = QolAppendStr(p, "\r\n");
        *p = '\0';
        WriteFile(h, line, (DWORD)(p - line), &wr, nullptr);
    }
    CloseHandle(h);
}

inline void QolOnTerminate() {
    QolWriteDeathFrames("std::terminate");
    // 不返回：运行时将走到默认 abort()
}
inline void QolOnAbort(int) {
    QolWriteDeathFrames("abort");
    signal(SIGABRT, SIG_DFL);   // 防止 abort 再次进入本处理器导致死循环
}
#ifdef _MSC_VER
inline void QolOnInvalidParam(const wchar_t*, const wchar_t*, const wchar_t*,
                              unsigned int, uintptr_t) {
    QolWriteDeathFrames("invalid_parameter");
}
#endif

// ---- 栈溢出极简写入器（v25 新增）：栈溢出时 VEH 自身没栈可跑，
//      必须用一个极省的 path（不分配大数组、不 walk 全模块表），只记
//      顶层 rip 的归属，确认"是栈溢出"即可（完整栈需靠复现时的二分）。
inline void QolWriteStackOverflowMarker(EXCEPTION_POINTERS* ep) {
    static volatile LONG s_so = 0;
    if (InterlockedExchange(&s_so, 1) != 0) return;
    const unsigned long long rip = ep && ep->ContextRecord ? ep->ContextRecord->Rip : 0;
    const unsigned long long rsp = ep && ep->ContextRecord ? ep->ContextRecord->Rsp : 0;
    // 边走 PEB 边比，不存全表
    char modName[48] = {}; unsigned long long modBase = 0;
    {
        void* teb = (void*)__readgsqword(0x30);
        if (teb && QolReadable(teb, 0x80)) {
            unsigned long long pPeb = *(unsigned long long*)((char*)teb + 0x60);
            if (QolReadable((void*)pPeb, 0x80)) {
                unsigned long long pLdr = *(unsigned long long*)((char*)pPeb + 0x18);
                if (QolReadable((void*)pLdr, 0x40)) {
                    LIST_ENTRY* head = (LIST_ENTRY*)((char*)pLdr + 0x10);
                    if (QolReadable(head, sizeof(LIST_ENTRY))) {
                        LIST_ENTRY* e = head->Flink;
                        for (int i = 0; i < 512; ++i) {
                            if (!e || !QolReadable(e, 0x70)) break;
                            unsigned long long entry = (unsigned long long)(uintptr_t)e;
                            unsigned long long base = *(unsigned long long*)(entry + 0x30);
                            unsigned int size = *(unsigned int*)(entry + 0x40);
                            if (rip >= base && rip < base + size) {
                                modBase = base;
                                unsigned short len = *(unsigned short*)(entry + 0x58);
                                void* buf = *(void**)(entry + 0x60);
                                if (buf && len > 0 && len < 240 && QolReadable(buf, len)) {
                                    const wchar_t* w = (const wchar_t*)buf;
                                    int copy = len / 2; if (copy > 40) copy = 40;
                                    for (int c = 0; c < copy; ++c) {
                                        unsigned ch = (unsigned)w[c];
                                        if (ch == 0) break;
                                        modName[c] = (char)(ch < 0x80 ? ch : '?');
                                    }
                                }
                                break;
                            }
                            LIST_ENTRY* nx = e->Flink;
                            if (nx == head || nx == e) break;
                            e = nx;
                        }
                    }
                }
            }
        }
    }
    wchar_t gameRoot[MAX_PATH] = {};
    QolGetRootW(gameRoot, MAX_PATH);
    wchar_t path[MAX_PATH] = {};
    wsprintfW(path, L"%s\\QoL_crash\\last_fault.txt", gameRoot);
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        char buf2[256];
        char* p = buf2;
        p = QolAppendStr(p, "\r\n===== QoL-STACKOVERFLOW rip=0x");
        p = QolAppendHex64(p, rip, 0);
        p = QolAppendStr(p, " mod=");
        p = QolAppendStr(p, modName[0] ? modName : "?");
        p = QolAppendStr(p, " rva=0x");
        p = QolAppendHex64(p, modBase ? rip - modBase : 0ULL, 0);
        p = QolAppendStr(p, " rsp=0x");
        p = QolAppendHex64(p, rsp, 0);
        p = QolAppendStr(p, " =====\r\n");
        *p = '\0';
        WriteFile(h, buf2, (DWORD)(p - buf2), &wr, nullptr);
        CloseHandle(h);
    }
}

// ---------------------------------------------------------------
// 6c. 进程退出 API hook（v26 新增）—— 抓「静默消失」
//     为什么 VEH / set_terminate / abort 全都看不到：TerminateProcess /
//     ExitProcess 是**直接结束进程**，不产生任何 SEH 异常、不进 terminate
//     处理器。表现为「一按存档就静默消失、无 last_fault、无 WER、无事件」。
//     本段 inline hook 这两个 API：被调用时先写调用栈（module+0xRVA）到
//     last_fault.txt 的 [QoL-exit] 块，再走 trampoline 执行原函数真正退出，
//     从而定位「是哪个 MOD 的哪行代码调用了终止」。
//
//     实现：覆盖目标函数首 12 字节（MS x64 标准 prologue：
//     mov[rsp+8],rbx / push rbp / mov rbp,rsp / sub rsp,XX 共 12 字节，
//     恰为完整指令边界）为 jmp detour；trampoline 执行原 12 字节后 jmp 回
//     原函数+12，保证原函数语义完整、无递归。
// ---------------------------------------------------------------
struct QolExitHookT {
    void* target = nullptr;
    void* tramp = nullptr;
    unsigned char orig[16] = {};
    bool ok = false;
};
inline QolExitHookT g_hkTerm;
inline QolExitHookT g_hkExit;
inline QolExitHookT g_hkNtTerm;   // v27：最底层终止 syscall（游戏崩溃处理器常直走 ntdll）

// why   : 被调用的终止 API 名（"TerminateProcess"/"ExitProcess"/"NtTerminateProcess"）
// aux   : 可选附加信息（如 NtTerminateProcess 的 NTSTATUS 退出码），0 表示无
inline void QolWriteExitFrames(const char* why, unsigned long long aux = 0) {
    static volatile LONG s_w = 0;
    if (InterlockedExchange(&s_w, 1) != 0) return;   // 只写一次
    void* fr[48] = {};
    int n = 0;
    PFN_RtlCaptureStackBackTrace cap = QolCaptureFn();
    if (cap) {
        unsigned short g = cap(1, 47, fr, nullptr);
        n = (int)g; if (n > 47) n = 47;
    }
    QolModEnt mods[kQolMaxMods];
    ZeroMemory(mods, sizeof(mods));
    int mc = QolWalkModules(mods, kQolMaxMods);
    wchar_t root[MAX_PATH] = {};
    QolGetRootW(root, MAX_PATH);
    wchar_t path[MAX_PATH] = {};
    wsprintfW(path, L"%s\\QoL_crash\\last_fault.txt", root);
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        char head[256];
        char* p = head;
        p = QolAppendStr(p, "\r\n===== QoL-exit ");
        p = QolAppendStr(p, why ? why : "?");
        p = QolAppendStr(p, " pid=");
        p = QolAppendDec64(p, GetCurrentProcessId());
        p = QolAppendStr(p, " tid=");
        p = QolAppendDec64(p, GetCurrentThreadId());
        if (aux) {
            p = QolAppendStr(p, " status=0x");
            p = QolAppendHex64(p, aux, 8);
        }
        p = QolAppendStr(p, " =====\r\n");
        *p = '\0';
        WriteFile(h, head, (DWORD)(p - head), &wr, nullptr);
        char line[1024];
        for (int i = 0; i < n; ++i) {
            const unsigned long long pc = (unsigned long long)(uintptr_t)fr[i];
            const QolModEnt* m = QolFindMod(mods, mc, pc);
            p = line;
            p = QolAppendStr(p, "  #");
            if (i < 10) *p++ = (char)('0' + i);
            else { *p++ = (char)('0' + i / 10); *p++ = (char)('0' + i % 10); }
            p = QolAppendStr(p, " 0x");
            p = QolAppendHex64(p, pc, 0);
            p = QolAppendStr(p, "  ");
            p = QolAppendStr(p, m ? m->name : "?");
            p = QolAppendStr(p, "+0x");
            p = QolAppendHex64(p, m ? pc - m->base : 0ULL, 0);
            p = QolAppendStr(p, "\r\n");
            *p = '\0';
            WriteFile(h, line, (DWORD)(p - line), &wr, nullptr);
        }
        CloseHandle(h);
    }
}

typedef BOOL (WINAPI* PFN_TerminateProcess)(HANDLE, UINT);
inline BOOL WINAPI QolTermDetour(HANDLE hProcess, UINT uExitCode) {
    QolWriteExitFrames("TerminateProcess", (unsigned long long)uExitCode);
    return ((PFN_TerminateProcess)g_hkTerm.tramp)(hProcess, uExitCode);
}
typedef void (WINAPI* PFN_ExitProcess)(UINT);
inline void WINAPI QolExitDetour(UINT uExitCode) {
    QolWriteExitFrames("ExitProcess", (unsigned long long)uExitCode);
    ((PFN_ExitProcess)g_hkExit.tramp)(uExitCode);
}
// v27：NtTerminateProcess（ntdll 最底层 syscall 包装）。
// 游戏自带崩溃处理器常不经 kernel32 而直走它，故必须在此层接管才能抓到「静默消失」。
typedef long (NTAPI* PFN_NtTerminateProcess)(HANDLE, unsigned long);  // NTSTATUS
inline long NTAPI QolNtTermDetour(HANDLE hProcess, unsigned long ExitStatus) {
    QolWriteExitFrames("NtTerminateProcess", (unsigned long long)ExitStatus);
    return ((PFN_NtTerminateProcess)g_hkNtTerm.tramp)(hProcess, ExitStatus);
}

// v30: 整段扫描版 RIP-relative 重定位器。
// 为何需要它：原 QolInstallExitHook 把系统函数（TerminateProcess/ExitProcess/
// NtTerminateProcess）前 12 字节 memcpy 进 XRW relay 页后，没有对其中落在第 5 字节
// 之后的 RIP 相对指令（call/jmp [rip+disp32]）做重定位。relay 页地址与原始函数
// 地址不同，disp32 仍按旧 RIP 算 → 目标落进 relay 页外 0x4B100 处 → 读 AV
// （0xC0000005），表现为「强关游戏才崩、任意 1 个 MOD 都崩、MM/Sower 位移一致」。
// hook.cpp::RelocateRipRelative 只扫 5 字节窗口，覆盖不到本场景，故此处单独实现
// 整段扫描版。数学与之一致：newDisp = oldDisp + oldBase - newBase（pos 项相消）。
static bool QolHasModRM(unsigned char op, bool two) {
    if (two) {
        // 双字节 0F xx：覆盖常见的、可能带内存操作数的组
        if (op >= 0x10 && op <= 0x17) return true;   // MOVUPS/MOVUPD/MOVSS/MOVHPS...
        if (op >= 0x28 && op <= 0x2F) return true;   // MOVAPS/CVT/UCOMI...
        if (op >= 0x40 && op <= 0x4F) return true;   // CMOVcc
        if (op >= 0x50 && op <= 0x5F) return true;   // MOVMSK/SQRT/ADDPS...
        if (op >= 0x60 && op <= 0x6F) return true;   // PUNPCK/PACK/PADD...
        if (op >= 0x70 && op <= 0x73) return true;   // PSHUF/PSxx group
        if (op >= 0x78 && op <= 0x7F) return true;   // 多数 SIMD（含 7E/7F）
        if (op >= 0x90 && op <= 0x9F) return true;   // SETcc/BT...
        if (op >= 0xA3 && op <= 0xA5) return true;   // BT/SHLD
        if (op >= 0xAC && op <= 0xAF) return true;   // SHRD/CMPXCHG
        if (op >= 0xB0 && op <= 0xB7) return true;   // CMPXCHG/MOVZX/MOVSX
        if (op >= 0xBA && op <= 0xBF) return true;   // BTC/BTR/BTS/BSF/BSR/MOVSX
        if (op >= 0xC0 && op <= 0xC7) return true;   // XADD/CMPPS/MOVNTI/PINSRW/SHUF/C7
        if (op >= 0xC8 && op <= 0xCF) return true;   // BSWAP/组
        if (op >= 0xD0 && op <= 0xDF) return true;
        if (op >= 0xE0 && op <= 0xEF) return true;
        if (op >= 0xF0 && op <= 0xFF) return true;
        return false; // syscall/UD2/CPUID/RDTSC/jcc rel32 等无 ModRM
    }
    // 单字节：算术/逻辑 MR-RM 形式（排除 0x04/0x05 等立即数形式）
    if ((op & 0xC0) == 0x00 && op <= 0x3B &&
        (op & 0x06) != 0x04 && (op & 0x06) != 0x06) return true;
    if (op == 0x63) return true;                 // MOVSXD
    if (op == 0x69 || op == 0x6B) return true;   // IMUL
    if (op >= 0x80 && op <= 0x8F) return true;   // 组1/TEST/MOV/LEA/POP
    if (op >= 0xC0 && op <= 0xC1) return true;   // shift imm
    if (op == 0xC6 || op == 0xC7) return true;   // 组11
    if (op >= 0xD0 && op <= 0xD3) return true;   // shift/rot
    if (op >= 0xD8 && op <= 0xDF) return true;   // FPU
    if (op == 0xF6 || op == 0xF7) return true;   // 组3
    if (op == 0xFE || op == 0xFF) return true;   // 组4/组5
    return false;
}

// 无 ModRM 指令的立即数字节数；-1 表示不认识（调用方保守停止扫描）
static int QolImmLen(unsigned char op, bool two, bool rexW) {
    if (two) return 0; // 双字节无 ModRM（syscall/CPUID/RDTSC/jcc...）立即数为 0
    if (op == 0x68) return 4;                 // push imm32
    if (op == 0x6A) return 1;                 // push imm8
    if (op >= 0xB8 && op <= 0xBF) return rexW ? 8 : 4; // mov r,imm
    if (op == 0xE8 || op == 0xE9) return 4;   // call/jmp rel32
    if (op == 0xEB) return 1;                 // jmp rel8
    if (op >= 0x70 && op <= 0x7F) return 1;   // jcc rel8
    if (op == 0xC2 || op == 0xCA) return 2;   // ret imm16 / retf imm16
    return 0; // push/pop r, ret, nop, leave, hlt, in/out, flags...
}

// 带 ModRM 指令在 ModRM 之后、可能存在的立即数字节数（用于继续解码，避免错位）
static int QolModRMImm(unsigned char op, bool two) {
    if (two) return 0; // 本场景涉及到的双字节指令均无需补立即数
    switch (op) {
        case 0x80: case 0x82: case 0x83: case 0x84: case 0x86: case 0x87: return 1;
        case 0x81: case 0x85: return 4;
        case 0xC0: case 0xC1: return 1;
        case 0xC6: return 1;
        case 0xC7: return 4;
        case 0xF6: case 0xF7: return 1; // 组3 仅部分 /r 带 imm8，保守给 1（不越过关键边界）
        case 0x69: return 4;
        case 0x6B: return 1;
        default: return 0;
    }
}

// 整段扫描：把 [0,len) 中每处 [rip+disp32]（含 SIB base==5）的 disp32 改写为
// 仍指向原系统 IAT 槽的值。解码遇无法安全分类的指令即停止（保守，不破坏后续字节）。
static bool QolRelocateRipRange(unsigned char* code, uintptr_t oldBase, uintptr_t newBase, int len) {
    int pos = 0;
    while (pos < len) {
        int start = pos;
        bool rexW = false;
        for (;;) {
            unsigned char b = code[pos];
            if (b >= 0x40 && b <= 0x4F) { rexW = (b & 0x08) != 0; pos++; continue; }
            if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
                b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
                b == 0x64 || b == 0x65) { pos++; continue; }
            break;
        }
        if (pos >= len) break;
        unsigned char op = code[pos++];
        bool two = false;
        if (op == 0x0F) { two = true; if (pos >= len) break; op = code[pos++]; }
        if (QolHasModRM(op, two)) {
            if (pos >= len) break;
            unsigned char modrm = code[pos++];
            unsigned char mod = (unsigned char)((modrm >> 6) & 3);
            unsigned char rm  = (unsigned char)(modrm & 7);
            int dispBytes = 0;
            bool ripRel = false;
            if (mod == 3) { dispBytes = 0; }
            else if (mod == 1) { dispBytes = 1; }
            else if (mod == 2) { dispBytes = 4; }
            else { // mod == 0
                if (rm == 5) { ripRel = true; dispBytes = 4; }
                else if (rm == 4) {
                    if (pos >= len) break;
                    unsigned char sib = code[pos++];
                    if ((sib & 7) == 5) { ripRel = true; dispBytes = 4; }
                }
            }
            if (ripRel) {
                if (pos + 4 > len) return false; // disp 落在复制区外，无法安全重定位
                long oldDisp; memcpy(&oldDisp, code + pos, 4);
                long long nd = (long long)oldDisp + (long long)oldBase - (long long)newBase;
                if (nd < -2147483647LL - 1 || nd > 2147483647LL) return false;
                long n32 = (long)nd;
                memcpy(code + pos, &n32, 4);
            }
            pos += dispBytes;
            pos += QolModRMImm(op, two);
        } else {
            int il = QolImmLen(op, two, rexW);
            if (il < 0) break;
            pos += il;
        }
        if (pos <= start) break; // 防死循环
    }
    return true;
}

// 单条指令长度（含前缀/操作码/ModRM/disp/立即数）。avail=可读字节上限。
// 无法安全解码时返回 -1（调用方保守回退到固定 relocLen，不截断）。
static int QolInstrLen(const unsigned char* code, int avail) {
    int pos = 0;
    bool rexW = false;
    for (;;) {
        if (pos >= avail) return -1;
        unsigned char b = code[pos];
        if (b >= 0x40 && b <= 0x4F) { rexW = (b & 0x08) != 0; pos++; continue; }
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
            b == 0x64 || b == 0x65) { pos++; continue; }
        break;
    }
    if (pos >= avail) return -1;
    unsigned char op = code[pos];
    bool two = (op == 0x0F);
    int opBytes = 1 + (two ? 1 : 0);
    if (two) { if (pos + 1 >= avail) return -1; op = code[pos + 1]; }
    int extra;
    if (QolHasModRM(op, two)) {
        int modrmIdx = pos + opBytes;
        if (modrmIdx >= avail) return -1;
        unsigned char modrm = code[modrmIdx];
        unsigned char mod = (unsigned char)((modrm >> 6) & 3);
        unsigned char rm  = (unsigned char)(modrm & 7);
        int disp = 0;
        if (mod == 1) disp = 1;
        else if (mod == 2) disp = 4;
        else if (mod == 0) {
            if (rm == 5) disp = 4;
            else if (rm == 4) {
                int sibIdx = modrmIdx + 1;
                if (sibIdx >= avail) return -1;
                unsigned char sib = code[sibIdx];
                if ((sib & 7) == 5) disp = 4;
            }
        }
        extra = 1 /*ModRM*/ + disp + QolModRMImm(op, two);
    } else {
        int il = QolImmLen(op, two, rexW);
        if (il < 0) return -1;
        extra = il;
    }
    return pos + opBytes + extra;
}

inline bool QolInstallExitHook(QolExitHookT& hk, const wchar_t* modName,
                               const char* api, void* detour, int relocLen = 12) {
    if (relocLen < 5 || relocLen > 16) relocLen = 12;
    HMODULE mod = GetModuleHandleW(modName);
    if (!mod) return false;
    void* p = GetProcAddress(mod, api);
    if (!p) return false;
    memcpy(hk.orig, p, relocLen);
    // v30: 复制整段「完整指令」，而非固定 relocLen 字节——
    // 否则会截断落在中间的指令（如本机 NtTerminateProcess 的 7 字节
    // test byte [rip+disp32]），tramp 执行到半截指令 → AV。
    int copyLen = relocLen;
    {
        int cl = 0;
        while (cl < relocLen) {
            int L = QolInstrLen((const unsigned char*)p + cl, 64 - cl);
            if (L <= 0) { cl = relocLen; break; }
            cl += L;
        }
        copyLen = cl;
    }
    // trampoline: 原 copyLen 字节 + 绝对尾跳（14 字节，无 ±2GB 距离限制）
    void* tr = VirtualAlloc(0, copyLen + 14, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    memcpy(tr, p, copyLen);
    // v30: 重定位整段复制字节中的 RIP 相对指令（崩溃修复核心）。
    // 复制进 relay 的字节若含 call/jmp/test [rip+disp32]，必须按新地址改写 disp32，
    // 否则强关游戏时读 AV（0xC0000005）。失败则放弃安装，避免带病上线。
    if (!QolRelocateRipRange((unsigned char*)tr, (uintptr_t)p, (uintptr_t)tr, copyLen)) {
        VirtualFree(tr, 0, MEM_RELEASE);
        return false;
    }
    // 绝对尾跳：mov rax, imm64; jmp rax → 跳回 p+copyLen（指令边界，无 2GB 距离限制）
    unsigned char* tail = (unsigned char*)tr + copyLen;
    tail[0] = 0x48; tail[1] = 0xB8;                                  // mov rax, imm64
    *reinterpret_cast<uintptr_t*>(tail + 2) = (uintptr_t)p + copyLen;
    tail[10] = 0xFF; tail[11] = 0xE0;                               // jmp rax
    hk.tramp = tr;
    hk.target = p;
    // 改 p 首 5 字节为 jmp detour
    long d2 = (long)((char*)detour - ((char*)p + 5));
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tr, 0, MEM_RELEASE);
        return false;
    }
    unsigned char patch[5] = {
        0xE9,
        (unsigned char)(d2 & 0xFF),
        (unsigned char)((d2 >> 8) & 0xFF),
        (unsigned char)((d2 >> 16) & 0xFF),
        (unsigned char)((d2 >> 24) & 0xFF) };
    memcpy(p, patch, 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    hk.ok = true;
    return true;
}

inline void QolHookProcessExit() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    QolInstallExitHook(g_hkTerm, L"kernel32.dll", "TerminateProcess", (void*)QolTermDetour);
    QolInstallExitHook(g_hkExit, L"kernel32.dll", "ExitProcess", (void*)QolExitDetour);
    // v27：最底层 syscall 包装 —— 覆盖「游戏崩溃处理器直走 ntdll」这条所有上层钩子都漏掉的路径。
    // v30：不再固定 12 字节，由 QolInstallExitHook 自动按指令边界复制（本机 NtTerminateProcess
    // 前 15 字节含一条 7 字节 test byte [rip+disp32]，必须整条复制并重定位，否则 tramp 截断崩溃）。
    QolInstallExitHook(g_hkNtTerm, L"ntdll.dll", "NtTerminateProcess", (void*)QolNtTermDetour, 12);
}

// 7. 第一优先级 VEH（2026-10-28 引入，v24 加固）
//    为什么必须 VEH：游戏自带 VEH 遇到访问违例会抢先 TerminateProcess，
//    SetUnhandledExceptionFilter 根本轮不到执行（表现为无声消失）。
//    AddVectoredExceptionHandler(first=1) 是 LIFO，我们的 DLL 后加载，
//    所以排在游戏 VEH 之前，能看到第一手异常。
// ---------------------------------------------------------------
inline void QolWriteFaultLine(EXCEPTION_POINTERS* ep) {
    static volatile LONG s_inHandler = 0;
    if (InterlockedExchange(&s_inHandler, 1) != 0) return;   // 防重入

    // v29 phase-0：在任何复杂逻辑（PEB 遍历 / 栈展开 / 文件路径拼装）
    // 之前，先把最原始的三要素落盘。后面全崩也留得下这一行。
    {
        char b[192];
        char* q = b;
        q = QolAppendStr(q, "\r\n[QoL-veh0] code=0x");
        q = QolAppendHex64(q, ep->ExceptionRecord->ExceptionCode, 8);
        q = QolAppendStr(q, " rip=0x");
        q = QolAppendHex64(q, (unsigned long long)(uintptr_t)ep->ContextRecord->Rip, 8);
        q = QolAppendStr(q, " rsp=0x");
        q = QolAppendHex64(q, (unsigned long long)(uintptr_t)ep->ContextRecord->Rsp, 0);
        if (ep->ExceptionRecord->NumberParameters >= 2) {
            q = QolAppendStr(q, " info0=");
            q = QolAppendDec64(q, (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0]);
            q = QolAppendStr(q, " info1=0x");
            q = QolAppendHex64(q, (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1], 0);
        }
        q = QolAppendStr(q, " tid=");
        q = QolAppendDec64(q, (unsigned long long)GetCurrentThreadId());
        q = QolAppendStr(q, "\r\n");
        *q = '\0';
        QolRawWrite(QolFaultLogHandle(), b, static_cast<int>(q - b));
    }

    DWORD code = ep->ExceptionRecord->ExceptionCode;
    const void* rip = reinterpret_cast<const void*>(ep->ContextRecord->Rip);
    ULONG_PTR info0 = 0, info1 = 0;
    if (ep->ExceptionRecord->NumberParameters >= 1)
        info0 = ep->ExceptionRecord->ExceptionInformation[0];
    if (ep->ExceptionRecord->NumberParameters >= 2)
        info1 = ep->ExceptionRecord->ExceptionInformation[1];

    // ---- 归属：先看有没有叛逆到模块外（MEM_PRIVATE 执行 = 跳板/野地址）----
    // v29：模块表放静态区。14KB 栈数组在「栈快见底」的场景里等于自杀。
    static QolModEnt s_modsA[kQolMaxMods];
    QolModEnt* mods = s_modsA;
    ZeroMemory(mods, sizeof(s_modsA));
    int modCount = QolWalkModules(mods, kQolMaxMods);
    QolStage("mods", (unsigned long long)modCount);
    const unsigned long long ripAbs = reinterpret_cast<unsigned long long>(rip);

    wchar_t modPath[MAX_PATH] = L"?";
    char modA[MAX_PATH * 2] = {};
    unsigned long long modBase = 0;
    MEMORY_BASIC_INFORMATION mbi;
    ZeroMemory(&mbi, sizeof(mbi));
    const QolModEnt* modHit = QolFindMod(mods, modCount, ripAbs);
    if (modHit) {
        modBase = modHit->base;
        for (int i = 0; modHit->name[i] && i < MAX_PATH - 1; ++i)
            modPath[i] = (wchar_t)(unsigned char)modHit->name[i];
        modPath[39] = L'\0';
        memcpy(modA, modHit->name, strlen(modHit->name) + 1);
    } else if (VirtualQuery(rip, &mbi, sizeof(mbi))) {
        modBase = reinterpret_cast<unsigned long long>(mbi.AllocationBase);
        if (modBase) {
            wchar_t full[MAX_PATH] = {};
            if (GetModuleFileNameW(reinterpret_cast<HMODULE>(modBase), full, MAX_PATH)) {
                const wchar_t* base = wcsrchr(full, L'\\');
                wcsncpy(modPath, base ? base + 1 : full, MAX_PATH - 1);
            }
            WideCharToMultiByte(CP_ACP, 0, modPath, -1, modA,
                                static_cast<int>(sizeof(modA)), nullptr, nullptr);
        }
    }

    const unsigned long long rvaInMod = modBase ? (ripAbs - modBase) : 0ULL;
    const unsigned long long exeBase =
        reinterpret_cast<unsigned long long>(GetModuleHandleW(nullptr));
    const unsigned long long exeRva = exeBase ? (ripAbs - exeBase) : 0ULL;

    // 是否可疑：执行点不在任何模块 → 跳板/野指针，必须留完整现场
    const bool outsideModule = (modHit == nullptr) || (strlen(modA) == 0);
    bool execWritablePage = false;
    {
        MEMORY_BASIC_INFORMATION me;
        ZeroMemory(&me, sizeof(me));
        if (VirtualQuery(rip, &me, sizeof(me)) && me.State == MEM_COMMIT) {
            const DWORD ep2 = me.Protect & ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
            if (ep2 == PAGE_EXECUTE_READWRITE || ep2 == PAGE_EXECUTE_WRITECOPY ||
                ep2 == PAGE_READWRITE || ep2 == PAGE_WRITECOPY)
                execWritablePage = true;
        }
    }
    if (code == 0xC0000005 && info0 == 8) execWritablePage = true;   // DEP：在数据页执行

    char line[1400] = {};
    {
        char* p = line;
        p = QolAppendStr(p, "[QoL-veh] code=0x");
        p = QolAppendHex64(p, code, 8);
        p = QolAppendStr(p, " rip=0x");
        p = QolAppendHex64(p, ripAbs, 8);
        p = QolAppendStr(p, " mod=");
        p = QolAppendStr(p, modA[0] ? modA : "?");
        p = QolAppendStr(p, " rva=0x");
        p = QolAppendHex64(p, rvaInMod, 0);
        p = QolAppendStr(p, " exeRva=0x");
        p = QolAppendHex64(p, exeRva, 0);
        p = QolAppendStr(p, " info0=");
        p = QolAppendDec64(p, info0);
        p = QolAppendStr(p, " info1=0x");
        p = QolAppendHex64(p, info1, 0);
        p = QolAppendStr(p, " tid=");
        p = QolAppendDec64(p, GetCurrentThreadId());
        p = QolAppendStr(p, "\r\n");
        *p = '\0';
    }

    // 栈样本（原始 qword，兜底用）
    char stackLine[768] = {};
    {
        char* p = QolAppendStr(stackLine, "[QoL-veh] stack:");
        const unsigned long long* sp =
            reinterpret_cast<const unsigned long long*>(ep->ContextRecord->Rsp);
        if (sp && QolReadable(sp, sizeof(unsigned long long) * 8)) {
            for (int i = 0; i < 8; ++i) {
                if (static_cast<int>(p - stackLine) >
                    static_cast<int>(sizeof(stackLine)) - 24) break;
                p = QolAppendStr(p, " ");
                p = QolAppendHex64(p, sp[i], 1);
            }
        }
        p = QolAppendStr(p, "\r\n");
        *p = '\0';
    }

    // 摘要上下文行：归属 + 内存属性 + 顶层几帧（修 MUB时代 part）——
    char ctx[1024] = {};
    {
        // v29：优先真栈（无处理器帧），失败才回退 capture / 栈扫描
        unsigned long long fr[16] = {};
        int nf = QolWalkStackFromContext(ep->ContextRecord, mods, modCount, fr, 12);
        if (nf < 2) {
            void* f2[24] = {};
            int g = 0;
            PFN_RtlCaptureStackBackTrace cap = QolCaptureFn();
            if (cap) { unsigned short k = cap(0, 12, f2, nullptr); g = (int)k; if (g > 12) g = 12; }
            if (g < 2) g = QolScanReturnAddresses(mods, modCount,
                                                  ep->ContextRecord->Rsp, f2, 12);
            for (int i = 0; i < g; ++i) fr[i] = (unsigned long long)(uintptr_t)f2[i];
            nf = g;
        }
        QolStage("unwind", (unsigned long long)nf);
        char* p = QolAppendStr(ctx, "[QoL-veh-ctx] frames=");
        p = QolAppendDec64(p, (unsigned long long)nf);
        if (mbi.State == MEM_COMMIT) {
            p = QolAppendStr(p, " mem=");
            p = QolAppendStr(p, QolTypeName(mbi.Type));
            p = QolAppendStr(p, "/");
            p = QolAppendStr(p, QolProtectName(mbi.Protect, false));
        }
        if (outsideModule) p = QolAppendStr(p, " OUTSIDE_MODULE");
        if (execWritablePage) p = QolAppendStr(p, " SUSPECT_EXEC_PAGE");
        p = QolAppendStr(p, " |");
        for (int i = 0; i < nf && i < 6; ++i) {
            const unsigned long long pc = fr[i];
            const QolModEnt* m2 = QolFindMod(mods, modCount, pc);
            p = QolAppendStr(p, " ");
            if (m2) {
                p = QolAppendStr(p, m2->name);
                p = QolAppendStr(p, "+0x");
                p = QolAppendHex64(p, pc - m2->base, 0);
            } else {
                QolJumpInfo ji = QolDecodeJump(pc, 8);
                p = QolAppendStr(p, "priv+0x0");
                if (ji.kind) {
                    const QolModEnt* tm = QolFindMod(mods, modCount, ji.target);
                    p = QolAppendStr(p, "->");
                    p = QolAppendStr(p, tm ? tm->name : "?");
                    if (tm) {
                        p = QolAppendStr(p, "+0x");
                        p = QolAppendHex64(p, ji.target - tm->base, 0);
                    }
                }
            }
        }
        p = QolAppendStr(p, "\r\n");
        *p = '\0';
    }

    // v29：走安装期预打开的句柄（崩溃期不再 CreateFileW / 不再 CloseHandle）
    HANDLE h = QolEnsureFaultLog();
    if (h != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char head[256];
        int hn = wsprintfA(head,
            "\r\n===== %04u-%02u-%02u %02u:%02u:%02u.%03u =====\r\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
        QolRawWrite(h, head, hn);
        QolRawWrite(h, line, (int)lstrlenA(line));
        QolRawWrite(h, stackLine, (int)lstrlenA(stackLine));
        QolRawWrite(h, ctx, (int)lstrlenA(ctx));
        QolStage("log", 1);
    }

    static int s_detailSeq = 0;
    const bool want = outsideModule || execWritablePage || (++s_detailSeq <= 3);
    InterlockedExchange(&s_inHandler, 0);
    if (want) {
        QolWriteDetail(ep, outsideModule ? "rip outside any module"
                                         : execWritablePage ? "executing rw page"
                                                            : "early occurrence");
    }
}

// v28：崩溃类异常全集判定（v24~v27 白名单漏掉的两类静默死法都在这里）
//  —— 快速失败家族 0xC00004xx（RaiseFailFastException 任意 status，含非 0xC0000409 的 /GS 变体）
//  —— 堆损坏 0xC0000374 / 致命回调 0xC0000606 / 无效参数 0xC0000420
// C++ EH(0xE06D7363)、单步(0x80000004)、断点(0x80000003 调试用)不在此列。
inline bool QolIsFatalCode(DWORD code) {
    if (code == 0xC0000005 || code == 0xC00000FD || code == 0xC000001D ||
        code == 0xC0000094 || code == 0xC0000374 || code == 0xC0000606 ||
        code == 0xC0000420 || code == 0xC0000409)
        return true;
    if (code >= 0xC0000400 && code <= 0xC00004FF) return true;  // 快速失败家族
    return false;
}

inline LONG CALLBACK QolFirstChanceVeh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!QolIsFatalCode(code))
        return EXCEPTION_CONTINUE_SEARCH;   // C++ EH / 断点等非崩溃类，放行
    // 一次性护栏：进程内第一次崩溃类异常即留痕；避免启动期良性异常反复刷屏，
    // 同时保证「真正的崩溃（通常是进程死前第一个 fatal 类异常）」一定被抓到。
    static volatile LONG s_fatalLogged = 0;
    if (InterlockedExchange(&s_fatalLogged, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    if (code == 0xC00000FD) {
        // 栈溢出：VEH 自身没栈可跑完整现场，走极简写入器确认即可
        QolWriteStackOverflowMarker(ep);
    } else {
        QolWriteFaultLine(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;   // 只留痕，不吞异常
}

inline void FormatTimestampW(SYSTEMTIME* st, wchar_t* buf, size_t cch) {
    swprintf(buf, cch, L"%04u%02u%02u_%02u%02u%02u",
             st->wYear, st->wMonth, st->wDay,
             st->wHour, st->wMinute, st->wSecond);
}

inline void CopyFileAsTail(const wchar_t* srcDir, const wchar_t* logName,
                           const wchar_t* dstDir) {
    wchar_t src[MAX_PATH], dst[MAX_PATH], tailName[MAX_PATH];
    swprintf(src, MAX_PATH, L"%s\\%s", srcDir, logName);
    swprintf(tailName, MAX_PATH, L"%s.tail", logName);
    swprintf(dst, MAX_PATH, L"%s\\%s", dstDir, tailName);
    CopyFileW(src, dst, FALSE);
}

inline LONG WINAPI QolCrashFilter(EXCEPTION_POINTERS* ep) {
    // UEF 路径：游戏没有抢先 TerminateProcess 时才走到这里。
    // v24：先产出与 VEH 同等级的详细现场（旧版这里只有 0 帧）
    QolWriteDetail(ep, "unhandled-filter");

    wchar_t gameRoot[MAX_PATH], ts[32], crashDir[MAX_PATH], parentDir[MAX_PATH];
    QolGetRootW(gameRoot, MAX_PATH);
    SYSTEMTIME st;
    GetLocalTime(&st);
    FormatTimestampW(&st, ts, 32);
    // 两级目录：<游戏根>\QoL_crash\<时间戳>\（CreateDirectoryW 单级，逐级创建）
    swprintf(parentDir, MAX_PATH, L"%s\\QoL_crash", gameRoot);
    CreateDirectoryW(parentDir, nullptr);
    swprintf(crashDir, MAX_PATH, L"%s\\QoL_crash\\%s", gameRoot, ts);
    CreateDirectoryW(crashDir, nullptr);

    // ---- crash_info.txt ----
    const DWORD excCode = ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    const void* excAddr = ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    const uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    wchar_t infoPath[MAX_PATH];
    swprintf(infoPath, MAX_PATH, L"%s\\crash_info.txt", crashDir);
    HANDLE hInfo = CreateFileW(infoPath, GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    // ---- crash.dmp（v29：固定系统 dbghelp + 提高 dump 类型 + 记录失败原因）----
    // 历史：2026-10-09 ~ 10-24 共 20 份 crash.dmp **全部 0 字节**，说明
    //       MiniDumpWriteDump 一直返回 FALSE 而没人发现（旧代码不看返回值）。
    //       进程内写 dump 受目标进程堆/锁状态支配，堆一坏就写不出 ——
    //       这一条 B 方案治不了，只能靠进程外 WER 兜底（C 方案）。
    BOOL dumpOk = FALSE;
    DWORD dumpErr = 0;
    unsigned long long dumpSize = 0;
    {
        HMODULE hDbg = LoadLibraryExW(L"dbghelp.dll", nullptr,
                                      LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!hDbg) hDbg = LoadLibraryW(L"dbghelp.dll");
        dumpErr = hDbg ? 0 : GetLastError();
        if (hDbg) {
            PFN_MiniDumpWriteDump pDump = reinterpret_cast<PFN_MiniDumpWriteDump>(
                reinterpret_cast<void*>(GetProcAddress(hDbg, "MiniDumpWriteDump")));
            if (pDump) {
                wchar_t dmpPath[MAX_PATH];
                swprintf(dmpPath, MAX_PATH, L"%s\\crash.dmp", crashDir);
                HANDLE hFile = CreateFileW(dmpPath, GENERIC_WRITE, 0, nullptr,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    // DataSegs | UnloadedModules | IndirectlyReferencedMemory | ThreadInfo
                    const DWORD kDumpFlags = 0x00000001u | 0x00000020u |
                                             0x00000040u | 0x00001000u;
                    dumpOk = pDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                                   kDumpFlags, ep, nullptr, nullptr);
                    if (!dumpOk) dumpErr = GetLastError();
                    LARGE_INTEGER sz;
                    sz.QuadPart = 0;
                    if (GetFileSizeEx(hFile, &sz))
                        dumpSize = static_cast<unsigned long long>(sz.QuadPart);
                    CloseHandle(hFile);
                } else {
                    dumpErr = GetLastError();
                }
            } else {
                dumpErr = GetLastError();
            }
        }
    }

    if (hInfo != INVALID_HANDLE_VALUE) {
        char line[1024];
        int n = _snprintf(line, sizeof(line),
            "[QoL Crash Collector]\r\n"
            "time      : %04u-%02u-%02u %02u:%02u:%02u\r\n"
            "exception : 0x%08X\r\n"
            "address   : 0x%p\r\n"
            "rva       : 0x%zX\r\n"
            "module    : village.exe (base 0x%zX)\r\n"
            "thread_id : %u\r\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            excCode, excAddr,
            exeBase ? (reinterpret_cast<uintptr_t>(excAddr) - exeBase) : 0,
            exeBase, GetCurrentThreadId());
        if (n > 0) {
            DWORD written = 0;
            WriteFile(hInfo, line, (DWORD)n, &written, nullptr);
        }
        char dl[256];
        int dn = _snprintf(dl, sizeof(dl),
            "dump      : ok=%d err=%u size=%llu flags=0x1061\r\n"
            "collector : v29 (true-stack / preopen-handle)\r\n",
            dumpOk ? 1 : 0, dumpErr, dumpSize);
        if (dn > 0) {
            DWORD written = 0;
            WriteFile(hInfo, dl, (DWORD)dn, &written, nullptr);
        }
        CloseHandle(hInfo);
    }

    // ---- 全部 qol_*.log 快照 ----
    wchar_t findPattern[MAX_PATH];
    swprintf(findPattern, MAX_PATH, L"%s\\qol_*.log", gameRoot);
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(findPattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            CopyFileAsTail(gameRoot, fd.cFileName, crashDir);
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    return EXCEPTION_CONTINUE_SEARCH;  // 不吞异常，交回系统默认流程
}

} // namespace detail

// ---------------------------------------------------------------
// 安装：进程级单例，且**高版本接管低版本**（混合部署时新版保证生效）
// 共享握手放在 named file mapping 里：{magic, version, pid}
// ---------------------------------------------------------------
constexpr unsigned long kQolCrashVersion = 29;  // v29: 真栈（RtlVirtualUnwind 从异常 CONTEXT 展开）+ 静态模块表 + 预打开日志句柄 + 阶段标记 + crash.dmp 失败可诊断（历史 20 份 0 字节）

inline bool InstallCrashHandler(const char* featureName) {
    static bool s_installed = false;
    if (s_installed) return true;
    s_installed = true;

    // 崩溃路径要用的函数指针 + 目录：**健康时机**先备好
    detail::QolResolveCapture();
    detail::QolResolveUnwind();      // v29：RtlLookupFunctionEntry / RtlVirtualUnwind
    detail::QolEnsureFaultLog();     // v29：预打开 last_fault.txt（崩溃期只 WriteFile）

    const wchar_t* kMapName = L"Local\\QoL_CrashDump_Ctrl_v24";
    struct Ctrl { unsigned long magic; unsigned long version; unsigned long pid; };
    bool install = false;
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    0, sizeof(Ctrl), kMapName);
    if (map) {
        const bool fresh = (GetLastError() != ERROR_ALREADY_EXISTS);
        Ctrl* c = static_cast<Ctrl*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0,
                                                   sizeof(Ctrl)));
        if (c) {
            const unsigned long magic = 0x51434424u;   // 'QCD$'
            if (fresh || c->magic != magic) {
                c->magic = magic;
                c->version = kQolCrashVersion;
                c->pid = GetCurrentProcessId();
                install = true;
            } else if (c->version < kQolCrashVersion) {
                // 旧版 handler 已在（例如同进程里还有未重编译的 MOD DLL）：
                // 新版接管 —— SetUnhandledExceptionFilter 会覆盖它的过滤器
                c->version = kQolCrashVersion;
                c->pid = GetCurrentProcessId();
                install = true;
            }
            // 故意不 UnmapView/CloseHandle：保持对象存活到进程结束
        }
    } else {
        install = true;   // 拿不到共享对象就按唯一实例处理
    }
    (void)featureName;
    if (!install) return true;

    SetUnhandledExceptionFilter(detail::QolCrashFilter);
    // v22：同时装第一优先级 VEH —— 游戏自带 VEH 会抢先 TerminateProcess，
    // 只有它能抢在游戏之前把「模块+RVA+调用栈」留下来（2026-10-28 进存档静默退出教训）。
    AddVectoredExceptionHandler(1, detail::QolFirstChanceVeh);
    // v25：同时接管 C 运行时死亡路径（terminate/abort/无效参数 不走 SEH，
    // 表现为"静默消失"）—— 这些处理器在【正常栈】上运行，能抓完整调用链。
    {
        static bool s_death = false;
        if (!s_death) {
            s_death = true;
            std::set_terminate(detail::QolOnTerminate);
            signal(SIGABRT, detail::QolOnAbort);
#ifdef _MSC_VER
            _set_invalid_parameter_handler(detail::QolOnInvalidParam);
            _set_abort_behavior(_WRITE_ABORT_MSG | _CALL_REPORTFAULT,
                                _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        }
    }
    // v26：接管进程退出 API（TerminateProcess/ExitProcess），抓「静默消失」
    detail::QolHookProcessExit();
    // 写一条安装成功标记，便于确认取证是否生效（每次进程启动一次）
    {
        // v29：复用预打开句柄，不再关 —— 崩溃期直接复用
        HANDLE h = detail::QolEnsureFaultLog();
        if (h != INVALID_HANDLE_VALUE) {
            char buf[256];
            char* p = buf;
            p = detail::QolAppendStr(p, "\r\n===== QoL-crash-init v=");
            p = detail::QolAppendDec64(p, kQolCrashVersion);
            p = detail::QolAppendStr(p, " VEH=1 death=1 exit=");
            p = detail::QolAppendDec64(p, (detail::g_hkTerm.ok || detail::g_hkExit.ok) ? 1 : 0);
            p = detail::QolAppendStr(p, " nt=");
            p = detail::QolAppendDec64(p, detail::g_hkNtTerm.ok ? 1 : 0);
            p = detail::QolAppendStr(p, " unwind=");
            p = detail::QolAppendDec64(p,
                (detail::QolLookupFn() && detail::QolUnwindFn()) ? 1 : 0);
            p = detail::QolAppendStr(p, " =====\r\n");
            *p = '\0';
            DWORD wr = 0;
            WriteFile(h, buf, (DWORD)(p - buf), &wr, nullptr);
        }
    }
    return true;
}

} // namespace qol
