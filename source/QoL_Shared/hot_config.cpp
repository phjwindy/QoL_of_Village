// hot_config.cpp — v2.0 后台轮询实现（P1-4）
//
// 变更要点：
//   - HotConfig_Poll() 首次调用：在游戏线程执行首次 LoadFromJson() + 启动后台线程
//   - 后续 Poll() 调用：无操作（返回 0），不读文件不阻塞游戏线程
//   - 后台线程：1s 间隔 mtime 轮询，变化则 LoadFromJson → ApplyValue → volatile
//   - 线程生命周期：首次 Poll 启动，atexit 停止（晚于 DllMain DETACH）
//   - 崩溃：线程随进程死亡，无资源泄漏（仅 volatile 变量，无需清理）
//
// JSON 解析：极简手写解析器（不依赖第三方库，避免构建复杂度）
//   - 只需读键值对（string→number），无需数组/嵌套对象
//   - 格式：{ "modName": { "key": value, ... }, ... }
//   - value 为整数或浮点数，支持 0xHEX 前缀
//
// mtime 轮询：GetFileAttributesEx 获取文件修改时间
//   - 后台线程每 1 秒检查一次
//   - mtime 变化则重读文件
//
// 作者：PHJ&消失的清风
#include "hot_config.h"
#include "logging.h"
#include <windows.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>

// ---- 注册表条目 ----
struct HotEntry {
    char     modName[32];   // MOD 名
    void*    addr;          // volatile 变量地址
    char     key[64];       // JSON 键名
    HotType  type;          // 参数类型
    long long defVal;       // 默认值
};

// ---- 全局状态 ----
static std::vector<HotEntry> g_entries;

// ---- 后台线程状态 ----
static HANDLE g_bgThread = nullptr;
static volatile LONG g_bgStopFlag = 0;
static volatile LONG g_bgStarted = 0;
static volatile LONG g_reloadGen = 0;  // reload 代际计数（前后各+1）

// ---- 文件路径缓存 ----
static char g_gameDir[MAX_PATH] = {0};
static char g_unifiedPath[MAX_PATH] = {0};
static char g_overridesDir[MAX_PATH] = {0};

static void InitPaths() {
    if (g_gameDir[0]) return;
    if (!GetModuleFileNameA(nullptr, g_gameDir, MAX_PATH)) {
        HMODULE hSelf = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&InitPaths, &hSelf);
        GetModuleFileNameA(hSelf, g_gameDir, MAX_PATH);
    }
    char* lastSlash = (char*)strrchr(g_gameDir, '\\');
    if (lastSlash) *lastSlash = '\0';
    snprintf(g_unifiedPath, MAX_PATH, "%s\\QoL_hot.json", g_gameDir);
    snprintf(g_overridesDir, MAX_PATH, "%s\\QoL_hot.overrides", g_gameDir);
}

// ---- 极简 JSON 值提取 ----
static bool ExtractJsonValue(const char* json, const char* modName,
                              const char* key, double* outVal) {
    char modPattern[64];
    snprintf(modPattern, sizeof(modPattern), "\"%s\"", modName);
    const char* modPos = strstr(json, modPattern);
    if (!modPos) return false;

    const char* braceStart = strchr(modPos, '{');
    if (!braceStart) return false;
    int depth = 0;
    const char* braceEnd = braceStart;
    for (const char* p = braceStart; *p; ++p) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) { braceEnd = p; break; } }
    }
    if (depth != 0) return false;

    char keyPattern[80];
    snprintf(keyPattern, sizeof(keyPattern), "\"%s\"", key);
    const char* keyPos = nullptr;
    for (const char* p = braceStart + 1; p < braceEnd; ++p) {
        if (strncmp(p, keyPattern, strlen(keyPattern)) == 0) {
            keyPos = p; break;
        }
    }
    if (!keyPos) return false;

    const char* valStart = keyPos + strlen(keyPattern);
    while (*valStart && (*valStart == ' ' || *valStart == '\t' ||
                         *valStart == ':' || *valStart == '\n' ||
                         *valStart == '\r')) valStart++;
    if (*valStart == '"') valStart++;
    if (!*valStart) return false;

    if (strncmp(valStart, "true", 4) == 0) { *outVal = 1.0; return true; }
    if (strncmp(valStart, "false", 5) == 0) { *outVal = 0.0; return true; }
    if (strncmp(valStart, "0x", 2) == 0 || strncmp(valStart, "0X", 2) == 0) {
        *outVal = (double)(uintptr_t)_strtoui64(valStart, nullptr, 16);
        return true;
    }
    char* endPtr = nullptr;
    double val = strtod(valStart, &endPtr);
    if (endPtr == valStart) return false;
    *outVal = val;
    return true;
}

// ---- 读取文件内容 ----
static char* ReadFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return nullptr; }
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(f); return nullptr; }
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

// ---- 将值写入 volatile 变量 ----
static uintptr_t GetMainImageSize() {
    static uintptr_t cached = 0;
    if (cached) return cached;
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) return 0;
    const unsigned char* base = (const unsigned char*)exe;
    if (base[0] != 'M' || base[1] != 'Z') return 0;
    const uint32_t eLfanew = *(const uint32_t*)(base + 0x3C);
    const uint32_t sizeOfImage = *(const uint32_t*)(base + eLfanew + 0x50);
    cached = sizeOfImage;
    return cached;
}

static bool IsRvaSane(uintptr_t rva) {
    if (rva == 0 || rva == ~static_cast<uintptr_t>(0)) return false;
    const uintptr_t imageSize = GetMainImageSize();
    if (!imageSize) return true;
    return rva < imageSize;
}

static void ApplyValue(void* addr, HotType type, double val) {
    switch (type) {
        case HOT_INT:   *(volatile int*)addr       = (int)val;        break;
        case HOT_BOOL:  *(volatile int*)addr       = (val != 0) ? 1 : 0; break;
        case HOT_RVA: {
            const uintptr_t rva = (uintptr_t)(size_t)val;
            if (!IsRvaSane(rva)) {
                Log("[HotConfig] reject insane RVA 0x%llX (key addr %p), keep 0x%llX\n",
                    (unsigned long long)rva, addr,
                    (unsigned long long)*(volatile uintptr_t*)addr);
                break;
            }
            *(volatile uintptr_t*)addr = rva;
            break;
        }
        case HOT_FLOAT: *(volatile float*)addr     = (float)val;     break;
        case HOT_INT64: *(volatile int64_t*)addr   = (int64_t)val;   break;
    }
}

// ---- 从 JSON 加载所有已注册参数 ----
static void LoadFromJson() {
    InitPaths();

    char* unified = ReadFile(g_unifiedPath);

    for (auto& e : g_entries) {
        char overridePath[MAX_PATH];
        snprintf(overridePath, MAX_PATH, "%s\\QoL_hot.%s.json", g_gameDir, e.modName);
        char* override = ReadFile(overridePath);
        double val;
        bool found = false;

        if (override) {
            found = ExtractJsonValue(override, e.modName, e.key, &val);
            free(override);
        }

        if (!found && unified) {
            found = ExtractJsonValue(unified, e.modName, e.key, &val);
        }

        if (found) {
            ApplyValue(e.addr, e.type, val);
        }
    }

    if (unified) free(unified);
}

// ---- mtime 检查 ----
static FILETIME g_lastUnifiedTime = {0};
static bool g_firstLoad = true;

static bool CheckMtime() {
    InitPaths();
    bool changed = false;

    WIN32_FILE_ATTRIBUTE_DATA wad;
    if (GetFileAttributesExA(g_unifiedPath, GetFileExInfoStandard, &wad)) {
        if (g_firstLoad ||
            CompareFileTime(&wad.ftLastWriteTime, &g_lastUnifiedTime) != 0) {
            g_lastUnifiedTime = wad.ftLastWriteTime;
            changed = true;
        }
    }

    static FILETIME g_lastOverrideTimes[32] = {{0}};
    static char g_lastOverrideNames[32][32] = {{0}};
    static int g_overrideCount = 0;
    for (auto& e : g_entries) {
        char overridePath[MAX_PATH];
        snprintf(overridePath, MAX_PATH, "%s\\QoL_hot.%s.json", g_gameDir, e.modName);
        WIN32_FILE_ATTRIBUTE_DATA oad;
        if (GetFileAttributesExA(overridePath, GetFileExInfoStandard, &oad)) {
            int slot = -1;
            for (int i = 0; i < g_overrideCount; i++) {
                if (strcmp(g_lastOverrideNames[i], e.modName) == 0) { slot = i; break; }
            }
            if (slot < 0 && g_overrideCount < 32) {
                slot = g_overrideCount++;
                strncpy(g_lastOverrideNames[slot], e.modName, 31);
            }
            if (slot >= 0) {
                if (g_firstLoad || CompareFileTime(&oad.ftLastWriteTime, &g_lastOverrideTimes[slot]) != 0) {
                    g_lastOverrideTimes[slot] = oad.ftLastWriteTime;
                    changed = true;
                }
            }
        }
    }

    g_firstLoad = false;
    return changed;
}

// ============================================================
// 后台轮询线程（v2.0 新增）
// ============================================================
static DWORD WINAPI HotConfigBgThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    while (true) {
        if (g_bgStopFlag) break;

        // 1s 间隔轮询（可被 stopFlag 提前唤醒）
        for (int i = 0; i < 10 && !g_bgStopFlag; i++) {
            Sleep(100);
        }
        if (g_bgStopFlag) break;

        // CheckMtime + LoadFromJson（后台线程上下文）
        if (CheckMtime()) {
            InterlockedIncrement(&g_reloadGen);  // reload 开始
            LoadFromJson();
            InterlockedIncrement(&g_reloadGen);  // reload 结束
            Log("[HotConfig] background reloaded %d entries (gen=%ld)\n",
                (int)g_entries.size(), (long)g_reloadGen);
        }
    }
    return 0;
}

// ---- atexit 清理 ----
static void HotConfigShutdown() {
    if (!g_bgStarted) return;
    InterlockedExchange(&g_bgStopFlag, 1);
    if (g_bgThread) {
        WaitForSingleObject(g_bgThread, 2000);
        CloseHandle(g_bgThread);
        g_bgThread = nullptr;
    }
    g_bgStarted = 0;
}

// ---- 启动后台线程（首次 Poll 后调用）----
static void StartBgThread() {
    if (g_bgStarted) return;
    InterlockedExchange(&g_bgStopFlag, 0);
    g_bgThread = CreateThread(nullptr, 0, HotConfigBgThread, nullptr, 0, nullptr);
    if (g_bgThread) {
        InterlockedExchange(&g_bgStarted, 1);
        atexit(HotConfigShutdown);
    }
}

// ============================================================
// 公开 API 实现
// ============================================================

extern "C" void HotConfig_Register(const char* modName, void* addr,
                                     const char* key, HotType type, long long defVal) {
    HotEntry e;
    strncpy(e.modName, modName, sizeof(e.modName) - 1);
    e.modName[sizeof(e.modName) - 1] = '\0';
    e.addr = addr;
    strncpy(e.key, key, sizeof(e.key) - 1);
    e.key[sizeof(e.key) - 1] = '\0';
    e.type = type;
    e.defVal = defVal;
    g_entries.push_back(e);

    // 立即应用默认值
    ApplyValue(addr, type, (double)defVal);
}

extern "C" int HotConfig_Poll(void) {
    // v2.0：首次调用执行首次加载 + 启动后台线程
    // 后续调用为无操作（不读文件不阻塞游戏线程）
    if (g_bgStarted) return 0;

    if (g_entries.empty()) return 0;

    // 首次加载（游戏线程，保证 mod_init 完成时参数已就绪）
    if (CheckMtime()) {
        InterlockedIncrement(&g_reloadGen);
        LoadFromJson();
        InterlockedIncrement(&g_reloadGen);
        Log("[HotConfig] initial load %d entries\n", (int)g_entries.size());
    }

    // 启动后台线程（后续 reload 由后台线程负责）
    StartBgThread();
    return 1;
}

extern "C" void HotConfig_DumpCE(const char* modName) {
    int count = 0;
    for (auto& e : g_entries) {
        if (strcmp(e.modName, modName) != 0) continue;
        count++;
    }
    Log("[Hot] %s: %d params registered\n", modName, count);
    for (auto& e : g_entries) {
        if (strcmp(e.modName, modName) != 0) continue;
        const char* typeStr = "?";
        switch (e.type) {
            case HOT_INT:   typeStr = "int";   break;
            case HOT_BOOL:  typeStr = "bool";  break;
            case HOT_RVA:   typeStr = "rva";   break;
            case HOT_FLOAT: typeStr = "float"; break;
            case HOT_INT64: typeStr = "i64";   break;
        }
        uintptr_t addrVal = (uintptr_t)e.addr;
        if (e.type == HOT_RVA) {
            Log("[Hot] &%s=%p (%s=%s 0x%llX)\n", e.key, (void*)addrVal,
                e.key, typeStr, (unsigned long long)*(volatile uintptr_t*)e.addr);
        } else {
            Log("[Hot] &%s=%p (%s=%s %lld)\n", e.key, (void*)addrVal,
                e.key, typeStr, (long long)*(volatile int*)e.addr);
        }
    }
}

extern "C" int HotConfig_Count(const char* modName) {
    int count = 0;
    for (auto& e : g_entries) {
        if (strcmp(e.modName, modName) == 0) count++;
    }
    return count;
}
