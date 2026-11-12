// saveguard.cpp —— SaveGuard v1.2.1: 存档完整性巡检守护 + 运行时净化
//
// v1.2.1（2026-10-15 晨，稳定复现环境驱动）：B 类 v1.2 ctx-map 巡检回归
//   修复版——注册表地址 0x1104C10（当场 capstone 复核实锤 v1.2.0 地址
//   本就正确，昨晚“误诊地址错”实为裸 IsReadable 慢（v1.1.4 已根治））；
//   挂 FirstPass 进档后链路（v1.2.0 曾在主菜单阶段跑查的不是目标 map）；
//   QPC 护栏 3s 自禁用。检测目标=玩家/本机双实证崩溃签名（RVA 0x5D9022
//   读 ctx+0x58 map 悬空节点 value）。
//
// v1.1.4（2026-10-15 凌晨，三轮二分+活体 dump 实锤修复）：
//   ①快照后台线程化：25.7MB 复制+双轮哈希移出主线程，消除 2.6s 冻结
//     （实测：启动基线快照 + .systemsave 漂移快照，两分钟内冻两次）
//   ②触发收紧：只有真存档组（save.NNN/save.lst）变化才触发快照流程；
//     .systemsave 高频系统档不再计入（幂等短路同步改 slot 视图）
//   ③ignored_nonlive_file 环形表静音（旧版单名字对比致每 poll 刷 3 行）
//   ④logging.cpp 宽字符修复（QoL_Shared，随本版重编）：ANSI API 把
//     "故障日志" UTF-8 字节按 GBK 解释，10-10 17:31 后编译的全部 MOD
//     日志写进平行乱码目录，造成"日志静止"假象
//
// 第一版方案（2026-10-12 用户拍板，见 SaveGuard_交接说明.md）：
// 不 hook 游戏，mod_tick 轮询存档目录，对每个存档文件做 YKCMP 容器
// 完整性校验（魔数 / 固定长度 / ver / 数据长度 / 解压上限）；校验全部
// 通过时发布健康快照，检测到损坏时保留最近健康档 + 抢救损坏现场，
// 防止坏存档（越界覆盖 / 字段异常）覆盖好档后无从恢复。
//
// 本文件提供：
//   1. 插件入口 mod_init / mod_tick / DllMain
//   2. 巡检逻辑所需的全部外部依赖桩函数（哈希 / 内存检查 / 包门）
//   3. VerifyExeBuild —— village.exe v1.30 (build 25640656) 指纹验证
//
// 巡检状态机与校验逻辑见 save_guard.inl（文件末尾 #include 引入）。
//
// v1.1.0 新增（2026-10-13 玩家群体反馈存档污染，用户拍板并入）：
//   save_repair.inl 运行时净化模块（mod_tick 挂载，游戏主线程）：
//   A 类·机器槽净化（确定性修复：卡死 timer 无产出状态，PA 实锤病灶）
//   B 类·gadget 链健康巡检（只报告：悬空 status/vtable、链断裂——
//         盖“靠近树崩”等未实锤场景，AutoHarvest/MM 归因取证）
//   C 类·环境取证（mods 目录清单入日志，玩家回传日志即可归因版本适配问题）

// 存档格式引用 Tools\symbol_db\function_notes.md FN-009：
//   YKCMP_V1 容器头 0x14B = magic(8B) | ver(4B) | A(4B) | B(4B)
//   ver=4 → save.lst；ver=8 → save.NNN / .systemsave
//   槽位固定 3MB、.systemsave 固定 512KB；B=解压缓冲上限（实测 20MB）

#include <windows.h>
#include <wincrypt.h>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <new>

#include "obs.h"              // v1.6.x: 统一观测层（AI 可读寄存器 + toast）

// v1.2.1-crashfield（2026-10-15 诊断）：崩溃现场 detour hook 依赖
#include "hook.h"

// ============================================================
// 类型别名（与 SaveBackup 骨架一致）
// ============================================================
using u64 = std::uint64_t;
using u32 = std::uint32_t;

// ============================================================
// 日志（QoL_Shared，写入游戏根目录 qol_saveguard.log）
// ============================================================
#include "logging.h"

// SaveGuard 日志开关：巡检/告警日志默认启用（用户实测验证拦截依赖日志可见）
// 日志运行时开关（2026-10-24）：编译期宏 SAVEGUARD_LOGGING 退役，改由 QoL_Shared 运行时门控。
// 开启方式：游戏根 qol_log.cfg 写 level=1（约 1s 生效，免重编免重启）；缺省/level=0 为静默。

// ============================================================
// g_supportedExe —— build 指纹验证结果
// ============================================================
static bool g_supportedExe = false;

// ============================================================
// g_module —— .inl 通过 g_module 引用
// ============================================================
static HMODULE g_module = nullptr;

// ============================================================
// IsReadable —— 内存可读检查（v1.1.4：缓存版）
//
// v1.1.3 及以前是裸 VirtualQuery（零缓存）——2026-10-15 dump 实锤：
// 主线程死在 IsReadable→NtQueryVirtualMemory 内核调用（SG+0x20d0）
// 每次调用被安全软件内核 hook 拖到 ~5ms（火绒防注入过滤），巡检每节点
// 4~6 次检查 × 844 节点 ≈ 4000+ 次系统调用 = 单轮 21~50 秒主线程冻结
// （对照：其他 MOD 用共享库缓存版，同场景 8 个月生产正常）。
//
// 修复：对齐 QoL_Shared/memory_cache.h 的 FastRegion 缓存语义并扩容
// （1024 槽——全地图 gadget status 分布在数百个堆 region，32 槽不够）。
// 缓存仅加速"是"判定；未命中走真 VirtualQuery 后入表。thread_local
// 避免后台快照线程干扰。注意：缓存过期风险与共享库版本相同（可接受，
// PA/AutoHarvest 8 个月生产实证）。
// ============================================================
static bool IsReadable(const void* pointer, size_t size) {
    if (!pointer || size == 0) return false;
    uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    if (start < 0x10000 || start > 0x00007FFFFFFFFFFFULL) return false;
    if (start + size < start) return false;

    struct FastRegion {
        uintptr_t start;
        uintptr_t end;
    };
    constexpr int kSlots = 1024;
    static thread_local FastRegion regions[kSlots];
    static thread_local int regionCount = 0;
    static thread_local int replaceCursor = 0;

    for (int i = 0; i < regionCount; ++i) {
        if (start >= regions[i].start && start + size <= regions[i].end)
            return true;  // 命中缓存（重复查询同区域零系统调用）
    }
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(pointer, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t regionEnd = regionStart + mbi.RegionSize;
    if (start + size > regionEnd) return false;
    if (regionCount < kSlots) {
        regions[regionCount] = { regionStart, regionEnd };
        ++regionCount;
    } else {
        regions[replaceCursor] = { regionStart, regionEnd };
        replaceCursor = (replaceCursor + 1) % kSlots;
    }
    return true;
}

// ============================================================
// PackageWritesAuthorized —— 独立 DLL 无包验证，直接允许
// ============================================================
static bool PackageWritesAuthorized() {
    return true;
}

static void PackageOpenWriteGate() {
    // 独立 DLL：无需包验证
}

// ============================================================
// PackageHashFile —— SHA-256 文件哈希（从 author_integrity 提取的同款实现）
// ============================================================
static bool PackageHashFile(const wchar_t* path, unsigned char output[32],
                            unsigned* fileSize, DWORD* error) {
    if (error) *error = ERROR_SUCCESS;
    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = GetLastError();
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size)) {
        if (error) *error = GetLastError();
        CloseHandle(file);
        return false;
    }
    if (size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > 0xffffffffULL) {
        if (error) *error = ERROR_FILE_TOO_LARGE;
        CloseHandle(file);
        return false;
    }

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES,
                                   CRYPT_VERIFYCONTEXT) != FALSE;
    if (ok) ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
    if (!ok && error) *error = GetLastError();
    unsigned char buffer[64 * 1024] = {};
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) {
            if (error) *error = GetLastError();
            ok = false;
            break;
        }
        if (!read) break;
        if (!CryptHashData(hash, buffer, read, 0)) {
            if (error) *error = GetLastError();
            ok = false;
        }
    }
    DWORD hashSize = 32;
    if (ok) {
        ok = CryptGetHashParam(hash, HP_HASHVAL, output, &hashSize, 0) != FALSE &&
             hashSize == 32;
        if (!ok && error) *error = GetLastError();
    }
    SecureZeroMemory(buffer, sizeof(buffer));
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(file);
    if (ok && fileSize) *fileSize = static_cast<unsigned>(size.QuadPart);
    return ok;
}

// ============================================================
// VerifyExeBuild —— village.exe build 25640656（2026-10-27 免费更新2）指纹
// exe size=18438664, SHA-256=979AEBE31B42D181EE92E9DB0086F22F
//                   65F2213029471A50723F61EAA93119A5（Python hashlib 实算）
// 指纹不匹配时整体禁用巡检（fail-safe，游戏更新后等待版本适配）
// ============================================================
#include <bcrypt.h>

static bool VerifyExeBuild() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    HANDLE hFile = CreateFileW(exePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        Log("[SaveGuard] VerifyExeBuild: CreateFileW failed (err=%lu path=%ls)\n",
            GetLastError(), exePath);
        return false;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart != 18438664) {
        CloseHandle(hFile);
        return false;
    }

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

    // build 25640656 (2026-10-27 免费更新2) SHA-256
    static const unsigned char expected[32] = {
        0x97, 0x9A, 0xEB, 0xE3, 0x1B, 0x42, 0xD1, 0x81,
        0xEE, 0x92, 0xE9, 0xDB, 0x00, 0x86, 0xF2, 0x2F,
        0x65, 0xF2, 0x21, 0x30, 0x29, 0x47, 0x1A, 0x50,
        0x72, 0x3F, 0x61, 0xEA, 0xA9, 0x31, 0x19, 0xA5
    };

    bool match = memcmp(hash, expected, sizeof(hash)) == 0;
    if (match) {
        Log("[SaveGuard] village.exe verified: build 25640656\n");
    } else {
        Log("[SaveGuard] village.exe SHA-256 mismatch; patrol disabled safely\n");
    }
    return match;
}

// ============================================================
// 前向声明（实现在 save_guard.inl 中）
// ============================================================
static bool InitializeSaveGuard();
static void PumpSaveGuard();

// 实现在 save_repair.inl 中（v1.1.0 净化模块）
static void InitSaveRepair();
static void PumpSaveRepair();

// ============================================================
// 插件入口
// ============================================================

extern "C" __declspec(dllexport) void mod_init(void) {
    LogOpen("saveguard");
    qol::obs::Init(nullptr);  // v1.6.x: 统一观测层接入
    qol::obs::SetInt("saveguard", "ready", 0);
    Log("[SaveGuard] mod_init — build 25640656 v1.30 v1.3.18 (硬化表[0x5F019C]·崩源=已完成且(缺itemID键/id=0|id不在全局注册表)·v1.3.18 崩源查表 accessor 0x2A430→0x2C720 修正(误清树液提取器根因)+ctx空表保险丝)\n");

    g_module = GetModuleHandleW(nullptr);

    g_supportedExe = VerifyExeBuild();
    qol::obs::SetInt("saveguard", "exeOk", g_supportedExe ? 1 : 0);
    if (!g_supportedExe) {
        Log("[SaveGuard] unsupported exe; patrol disabled safely\n");
        qol::obs::EventF(3, 2500, L"[SaveGuard] 游戏版本不匹配→巡检不启用");
        return;
    }

    PackageOpenWriteGate();

    // 初始化存档巡检（存档目录解析失败时由 mod_tick 周期重试）
    const bool patrolReady = InitializeSaveGuard();
    Log("[SaveGuard] startup_ready=%d\n", patrolReady ? 1 : 0);
    qol::obs::SetInt("saveguard", "patrolReady", patrolReady ? 1 : 0);

    // v1.1.0 运行时净化初始化（函数绑定 + 环境取证；exe 门禁已验）
    InitSaveRepair();

    qol::obs::SetInt("saveguard", "ready", 1);
    qol::obs::EventF(1, 2500, L"[SaveGuard] 已加载：观测层接入（巡检+净化就绪）");
}

extern "C" __declspec(dllexport) void mod_tick(void) {
    // 每帧调用 PumpSaveGuard，内部按轮询节流执行巡检
    PumpSaveGuard();
    // v1.1.0 净化泵（节流状态机，主线程直调原生函数序列）
    if (g_supportedExe) PumpSaveRepair();
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID /*lpReserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hMod;
        DisableThreadLibraryCalls(hMod);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        qol::obs::Shutdown();  // v1.6.x: 解观测层映射（仅本 DLL 视图）
    }
    return TRUE;
}

// ============================================================
// 引入 .inl 巡检实现（所有外部依赖已在上方提供）
// ============================================================
#include "save_guard.inl"

// v1.1.0 净化模块（依赖上方 IsReadable/Log/LogE 与 g_supportedExe）
#include "save_repair.inl"
