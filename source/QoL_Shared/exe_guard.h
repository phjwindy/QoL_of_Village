// exe_guard.h —— 统一的游戏版本闸门（header-only，各 MOD 共用）
//
// 目的：MOD 启用前先确认"当前跑的 village.exe 就是适配过的那一版"。
// 以前只有 MonsterMark / ProductionAuto / SaveGuard 做整包指纹校验，
// 其余 MOD 依赖各补丁点的 AOB 校验（点位对不上才失败），一旦版本更新
// 但点位恰好仍匹配，就会在未验证的 exe 上动补丁 / 写存档状态。
//
// 判定方式（廉价指纹，避免每个 DLL 都全量哈希 18MB）：
//   文件大小 + 头 64KB SHA-256（取前 8 字节作为 tag）
// 只在本进程内算一次（static 缓存），供各 MOD 与 ModManager 共用。
//
// 用法（mod_init / DllMain 内，LogOpen 之后）：
//   #include "exe_guard.h"
//   if (!qol::ExeBuildSupported()) {
//       Log("[xxx] 游戏版本不匹配（非 v1.20 build 25311578）→ 不启用\n");
//       return;            // DllMain 里用 return TRUE;
//   }
//
// 更新游戏后：重新计算下方常量（.temp/exe_fingerprint.py），
// 逐个 MOD 复测通过后再更新此头文件。
#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <cstdint>

#pragma comment(lib, "bcrypt.lib")

namespace qol {

// 当前适配版本：Village in the Shade build 25640656（2026-10-27 免费更新2）
// size = 18438664
// head64k SHA-256 = 30903E0D0ACB734084EAC9250DD6924B4C3A9A5A9353D43E6CD88D4641517F63
//                   ^^^^^^^^^^^^^^^^ 前 8 字节即 tag = 0x30903E0D0ACB7340
inline constexpr unsigned long long kSupportedExeSize = 18438664ULL;
inline constexpr unsigned long long kSupportedExeTag  = 0x30903E0D0ACB7340ULL;

struct ExeFingerprint {
    unsigned long long size = 0;
    unsigned long long tag = 0;
    bool valid = false;
};

namespace exe_detail {

inline bool ReadFileBlock(HANDLE file, unsigned long long offset,
                          DWORD length, unsigned char* out) {
    LARGE_INTEGER pos = {};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, pos, nullptr, FILE_BEGIN)) return false;
    DWORD read = 0;
    return ReadFile(file, out, length, &read, nullptr) && read == length;
}

inline bool Sha256OfBlock(unsigned char* data, DWORD length,
                         unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return false;
    bool ok = false;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        ok = (BCryptHashData(hash, data, length, 0) == 0) &&
             (BCryptFinishHash(hash, out, 32, 0) == 0);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

}  // namespace exe_detail

// 当前进程所在 exe 的指纹（进程内只算一次）
inline const ExeFingerprint& ExeFingerprintOfSelf() {
    static ExeFingerprint s_fp = []() {
        ExeFingerprint fp;
        wchar_t path[MAX_PATH] = {};
        if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return fp;

        HANDLE file = CreateFileW(path, GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return fp;

        LARGE_INTEGER size = {};
        static unsigned char buffer[65536];
        bool ok = false;
        if (GetFileSizeEx(file, &size)) {
            fp.size = static_cast<unsigned long long>(size.QuadPart);
            ok = exe_detail::ReadFileBlock(file, 0, 65536, buffer);
        }
        CloseHandle(file);

        unsigned char digest[32] = {};
        if (!ok || !exe_detail::Sha256OfBlock(buffer, 65536, digest)) return fp;

        unsigned long long tag = 0;
        for (int i = 0; i < 8; ++i)
            tag = (tag << 8) | digest[i];
        fp.tag = tag;
        fp.valid = true;
        return fp;
    }();
    return s_fp;
}

// 是否就是适配过的那一版
inline bool ExeBuildSupported() {
    const ExeFingerprint& fp = ExeFingerprintOfSelf();
    return fp.valid && fp.size == kSupportedExeSize &&
           fp.tag == kSupportedExeTag;
}

}  // namespace qol
