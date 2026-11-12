// selftest.h — 结构化自检结果上报（header-only，无 STL、无堆分配，DllMain 安全）
//
// 为什么需要它
// ------------
// 现在 MOD 自检结果只写在 qol_<mod>.log 的散文句里（"xxx check FAILED; feature disabled"），
// 排查要人肉 grep 十几个日志，还看不出是哪一项挂了。本头文件让每个 MOD 把自检项
// 以机器可读形式落到 <游戏根>\QoL_selftest_<mod>.json：
//
//   {"mod":"autofish","build":"25640656","time":"2026-10-28 21:00:11",
//    "result":"FAIL","checks":[{"k":"common_exit","ok":false,"d":"addr=0x21D036"}]}
//
// 每个 MOD 写自己的文件（互不竞争），Tools/selftest.py 负责汇总。
//
// 用法（mod_init 内，LogOpen 之后）
// --------------------------------
//   qol::SelfTest::Begin("autofish");
//   qol::SelfTest::Add("common_exit", commonExitOk, "addr=0x21D036");
//   ...
//   qol::SelfTest::Flush("25640656");   // 写盘（原子替换）
//
// 约束
// ----
//   * 固定缓冲 64 项 × 96 字节详情，无 new/malloc，无 STL
//   * 原子替换（先写 .tmp 再 MoveFileEx），避免读到半个文件
//   * 只在 mod_init / 关键状态变化时调用，禁止放每 tick 路径
//   * 函数内静态（C++11 安全），多 DLL 各自独立
#pragma once

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace qol {

class SelfTest {
public:
    static const int kMaxChecks = 64;
    static const int kDetailMax = 96;

    struct Entry {
        char key[32];
        char detail[kDetailMax];
        bool ok;
    };

    static void Begin(const char* modName) {
        Count() = 0;
        char* m = Mod();
        if (modName) {
            strncpy_s(m, 48, modName, _TRUNCATE);
        } else {
            m[0] = '\0';
        }
    }

    static void Add(const char* key, bool ok, const char* detail = nullptr) {
        if (Count() >= kMaxChecks || !key) return;
        Entry& e = Entries()[Count()++];
        strncpy_s(e.key, sizeof(e.key), key, _TRUNCATE);
        if (detail) {
            strncpy_s(e.detail, sizeof(e.detail), detail, _TRUNCATE);
        } else {
            e.detail[0] = '\0';
        }
        e.ok = ok;
    }

    static bool AllOk() {
        if (Count() <= 0) return false;
        for (int i = 0; i < Count(); ++i) {
            if (!Entries()[i].ok) return false;
        }
        return true;
    }

    static int FailCount() {
        int n = 0;
        for (int i = 0; i < Count(); ++i) {
            if (!Entries()[i].ok) ++n;
        }
        return n;
    }

    static bool Flush(const char* gameTag = nullptr) {
        char path[MAX_PATH];
        if (!ResultPath(path, sizeof(path))) return false;

        char tmp[MAX_PATH];
        _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", path);

        FILE* f = nullptr;
        if (fopen_s(&f, tmp, "wb") != 0 || !f) return false;

        SYSTEMTIME st;
        GetLocalTime(&st);
        const char* result = (FailCount() == 0 && Count() > 0) ? "PASS" : "FAIL";
        char esc[512];

        Jesc(Mod(), esc, sizeof(esc));
        fprintf(f, "{\"mod\":\"%s\",\"build\":\"%s\",\"time\":\"%04d-%02d-%02d %02d:%02d:%02d\",",
                esc, gameTag ? gameTag : "",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(f, "\"result\":\"%s\",\"checks\":[", result);
        for (int i = 0; i < Count(); ++i) {
            char ek[96], ed[256];
            Jesc(Entries()[i].key, ek, sizeof(ek));
            Jesc(Entries()[i].detail, ed, sizeof(ed));
            fprintf(f, "%s{\"k\":\"%s\",\"ok\":%s,\"d\":\"%s\"}", i ? "," : "", ek,
                    Entries()[i].ok ? "true" : "false", ed);
        }
        fprintf(f, "]}\n");
        fclose(f);

        if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileA(tmp);
            return false;
        }
        return true;
    }

private:
    static char* Mod() { static char s[48] = {0}; return s; }
    static int& Count() { static int c = 0; return c; }
    static Entry* Entries() { static Entry e[kMaxChecks] = {}; return e; }

    static void Jesc(const char* in, char* out, size_t outSize) {
        if (!out || outSize == 0) return;
        size_t o = 0;
        for (const char* p = in ? in : ""; *p && o + 7 < outSize; ++p) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"' || c == '\\') {
                out[o++] = '\\';
                out[o++] = static_cast<char>(c);
            } else if (c == '\n') {
                out[o++] = '\\'; out[o++] = 'n';
            } else if (c == '\r') {
                out[o++] = '\\'; out[o++] = 'r';
            } else if (c == '\t') {
                out[o++] = '\\'; out[o++] = 't';
            } else if (c < 0x20) {
                o += (size_t)_snprintf_s(out + o, outSize - o, _TRUNCATE, "\\u%04X", c);
            } else {
                out[o++] = static_cast<char>(c);
            }
        }
        out[o] = '\0';
    }

    static bool ResultPath(char* out, size_t outSize) {
        wchar_t exePath[MAX_PATH];
        if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return false;
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (!slash) return false;
        *(slash + 1) = L'\0';
        wchar_t wpath[MAX_PATH];
        _snwprintf_s(wpath, MAX_PATH, _TRUNCATE, L"%sQoL_selftest_%S.json",
                     exePath, Mod()[0] ? Mod() : "unknown");
        return WideCharToMultiByte(CP_UTF8, 0, wpath, -1, out, (int)outSize,
                                   nullptr, nullptr) > 0;
    }
};

}  // namespace qol
