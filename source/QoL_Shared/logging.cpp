// logging.cpp — v2.0 异步落盘实现（P1-3）
//
// 架构：
//   主线程 Log() → 格式化 → 入队（CRITICAL_SECTION，~微秒级）
//   后台线程   → 200ms 唤醒 → 批量出队 → fwrite + fflush → 文件
//   LogE()     → 始终同步直写（跳过队列），崩溃前 ERROR 可见
//
// 开关：qol_log.cfg（游戏根目录，非 HotConfig，避免循环依赖）
//   async=1（默认异步）/ async=0（全同步，LogE 仍同步）
//   后台线程每 ~1s 重读实现热切换
//
// 线程生命周期：
//   LogOpen() → CreateThread（从 DllMain ATTACH 调用，CRT已初始化）
//   atexit()  → 注册 LogFlushAndShutdown（CRT DETACH 阶段调用，晚于 DllMain DETACH）
//   崩溃      → VEH → TerminateProcess，线程直接死亡，队列中未刷盘条目丢失
//               LogE 已同步直写，ERROR 不丢
//
// 多 DLL：每 DLL 各自一个后台线程。14-16 个 200ms 睡眠线程：
//   - 栈：默认 1MB 预留/4KB 提交 ×16 = 16MB 预留/64KB 提交（可忽略）
//   - 调度：Sleep 状态线程不占 CPU，仅在 200ms 唤醒窗口微耗
//   - 对比共享线程方案：需命名内存映射 + 跨 DLL 同步 + 协调者 DLL，
//     复杂度高且引入单点故障风险，收益极低（每秒 <50 行日志），不取
#include "logging.h"
#include <windows.h>
#include <string>
#include <cstring>
#include <cstdlib>   // atexit

// ============================================================
// 异步队列
// ============================================================
struct LogEntry {
    char buf[8192];   // 格式化后的完整行（含时间戳）
    int  len;          // 有效长度
};

static constexpr int QUEUE_SIZE = 256;   // 环形缓冲槽位数
static LogEntry  g_queue[QUEUE_SIZE];
static int       g_qHead = 0;            // 后台线程读取位置
static int       g_qTail = 0;            // 主线程写入位置
static CRITICAL_SECTION g_qLock;         // 队列锁（极短临界区）
static CONDITION_VARIABLE g_qCv;         // 唤醒后台线程
static unsigned long g_droppedCount = 0;  // 溢出丢弃计数

// ============================================================
// 全局状态
// ============================================================
static FILE* g_log = nullptr;
static char  g_name[64] = "default";
static HANDLE g_thread = nullptr;
static volatile LONG g_threadRunning = 0;
static volatile LONG g_stopFlag = 0;
static volatile LONG g_asyncEnabled = 1;  // 1=异步(默认)，0=全同步
static bool g_initialized = false;        // 队列锁/线程是否已初始化
static long long g_cfgCheckCounter = 0;   // qol_log.cfg 重读计数器

// 游戏目录缓存（与 hot_config.cpp 同源逻辑）
static char g_gameDir[MAX_PATH] = {0};
static char g_logCfgPath[MAX_PATH] = {0};

static void InitGameDir() {
    if (g_gameDir[0]) return;
    if (!GetModuleFileNameA(nullptr, g_gameDir, MAX_PATH)) return;
    char* slash = (char*)strrchr(g_gameDir, '\\');
    if (slash) *slash = '\0';
    snprintf(g_logCfgPath, MAX_PATH, "%s\\qol_log.cfg", g_gameDir);
}

// 独立读取 qol_log.cfg（不依赖 HotConfig）
static void ReadLogConfig() {
    InitGameDir();
    FILE* f = fopen(g_logCfgPath, "rb");
    if (!f) {
        g_asyncEnabled = 1;  // 无配置文件 = 默认异步
        return;
    }
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    // 解析 async=N
    const char* p = strstr(buf, "async");
    if (p) {
        p += 5;
        while (*p && (*p == ' ' || *p == '=' || *p == '\t')) p++;
        if (*p == '0') g_asyncEnabled = 0;
        else g_asyncEnabled = 1;
    } else {
        g_asyncEnabled = 1;
    }
}

// ============================================================
// 同步直写（sync direct write）
// 保留原 UTF-8→GBK 转换逻辑
// ============================================================
static void SyncWrite(const char* buf) {
    if (!g_log || !buf) return;
    // ASCII 快速路径
    bool pureAscii = true;
    for (const char* p = buf; *p; ++p) {
        if (static_cast<unsigned char>(*p) >= 0x80) { pureAscii = false; break; }
    }
    if (pureAscii) {
        fputs(buf, g_log);
        fputc('\n', g_log);
        fflush(g_log);
        return;
    }
    // UTF-8 → GBK
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, buf, -1, nullptr, 0);
    if (wideLen > 0) {
        std::wstring wide;
        wide.resize(wideLen - 1);
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, &wide[0], wideLen);
        int ansiLen = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1,
                                          nullptr, 0, nullptr, nullptr);
        if (ansiLen > 0) {
            std::string ansi;
            ansi.resize(ansiLen - 1);
            WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1,
                                &ansi[0], ansiLen, nullptr, nullptr);
            fwrite(ansi.data(), 1, ansi.size(), g_log);
            fputc('\n', g_log);
            fflush(g_log);
            return;
        }
    }
    fputs(buf, g_log);
    fputc('\n', g_log);
    fflush(g_log);
}

// ============================================================
// 入队（主线程调用，极短临界区）
// ============================================================
static void Enqueue(const char* buf, int len) {
    EnterCriticalSection(&g_qLock);
    int next = (g_qTail + 1) % QUEUE_SIZE;
    if (next == g_qHead) {
        // 队列满：丢弃最旧（移动 head），计数
        g_qHead = (g_qHead + 1) % QUEUE_SIZE;
        g_droppedCount++;
    }
    memcpy(g_queue[g_qTail].buf, buf, len);
    g_queue[g_qTail].buf[len] = '\0';
    g_queue[g_qTail].len = len;
    g_qTail = next;
    LeaveCriticalSection(&g_qLock);
    WakeConditionVariable(&g_qCv);  // 唤醒后台线程
}

// ============================================================
// 后台刷盘线程
// ============================================================
static DWORD WINAPI LogThreadProc(LPVOID) {
    // 设置低优先级，不与游戏线程争抢
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    char batch[QUEUE_SIZE * 128];  // 批量写入缓冲（粗估每行≤128字节）
    while (true) {
        // 等待：有数据或超时或停止
        EnterCriticalSection(&g_qLock);
        while (g_qHead == g_qTail && !g_stopFlag) {
            SleepConditionVariableCS(&g_qCv, &g_qLock, 200);
        }
        if (g_stopFlag && g_qHead == g_qTail) {
            LeaveCriticalSection(&g_qLock);
            break;  // 停止且队列空 → 退出
        }
        // 批量出队
        int batchLen = 0;
        int drained = 0;
        while (g_qHead != g_qTail && drained < QUEUE_SIZE) {
            LogEntry& e = g_queue[g_qHead];
            int need = e.len + 1;  // +1 for '\n'
            if (batchLen + need > (int)sizeof(batch) - 1) break;
            memcpy(batch + batchLen, e.buf, e.len);
            batchLen += e.len;
            batch[batchLen++] = '\n';
            g_qHead = (g_qHead + 1) % QUEUE_SIZE;
            drained++;
        }
        // 采样溢出计数（在锁内读取）
        unsigned long dropped = g_droppedCount;
        LeaveCriticalSection(&g_qLock);

        // 批量写入文件
        if (batchLen > 0 && g_log) {
            // ASCII 快速路径（批量写入不做 UTF-8→GBK，日志绝大多数为 ASCII）
            fwrite(batch, 1, batchLen, g_log);
            fflush(g_log);
        }

        // 溢出告警（每周期最多一次，不刷屏）
        if (dropped > 0 && g_log) {
            char warn[128];
            int wl = snprintf(warn, sizeof(warn),
                "[logging] WARNING: %lu log entries dropped (queue overflow)\n",
                dropped);
            if (wl > 0 && g_log) {
                fwrite(warn, 1, wl, g_log);
                fflush(g_log);
            }
            // 重置计数（已告警）
            InterlockedExchange(&g_droppedCount, 0);
        }

        // 每 ~1s 重读 qol_log.cfg（5 个 200ms 周期）
        if (++g_cfgCheckCounter >= 5) {
            g_cfgCheckCounter = 0;
            ReadLogConfig();
        }
    }
    return 0;
}

// ============================================================
// atexit 清理（CRT DETACH 阶段调用，晚于 DllMain DETACH）
// ============================================================
static void LogFlushAndShutdown() {
    if (!g_initialized) return;
    // 信号停止 + 唤醒
    InterlockedExchange(&g_stopFlag, 1);
    WakeConditionVariable(&g_qCv);
    // 等待线程退出（最多 2s）
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    // 最终 flush（线程退出后队列可能仍有残留）
    EnterCriticalSection(&g_qLock);
    char batch[QUEUE_SIZE * 128];
    int batchLen = 0;
    while (g_qHead != g_qTail) {
        LogEntry& e = g_queue[g_qHead];
        int need = e.len + 1;
        if (batchLen + need > (int)sizeof(batch) - 1) break;
        memcpy(batch + batchLen, e.buf, e.len);
        batchLen += e.len;
        batch[batchLen++] = '\n';
        g_qHead = (g_qHead + 1) % QUEUE_SIZE;
    }
    LeaveCriticalSection(&g_qLock);
    if (batchLen > 0 && g_log) {
        fwrite(batch, 1, batchLen, g_log);
        fflush(g_log);
    }
    DeleteCriticalSection(&g_qLock);
    g_initialized = false;
}

static void EnsureInit() {
    if (g_initialized) return;
    InitializeCriticalSection(&g_qLock);
    InitializeConditionVariable(&g_qCv);
    ReadLogConfig();  // 初始读取 qol_log.cfg
    InterlockedExchange(&g_stopFlag, 0);
    InterlockedExchange(&g_threadRunning, 1);
    g_thread = CreateThread(nullptr, 0, LogThreadProc, nullptr, 0, nullptr);
    if (g_thread) {
        g_initialized = true;
        atexit(LogFlushAndShutdown);  // 注册 CRT 清理钩子
    } else {
        InterlockedExchange(&g_threadRunning, 0);
        DeleteCriticalSection(&g_qLock);
        // 线程创建失败：退化为同步模式
        g_asyncEnabled = 0;
    }
}

static void EnsureOpen() {
    if (g_log) return;
    char path[MAX_PATH] = {0};
    snprintf(path, MAX_PATH, "qol_%s.log", g_name);
    g_log = fopen(path, "a");
    if (g_log) setvbuf(g_log, nullptr, _IONBF, 0);  // 无缓冲（后台线程已批量）
}

// ============================================================
// 公开 API
// ============================================================

extern "C" void LogOpen(const char* feature) {
    if (feature && *feature) {
        snprintf(g_name, sizeof(g_name), "%s", feature);
    }
    if (g_log) { fclose(g_log); g_log = nullptr; }
    char path[MAX_PATH] = {0};
    snprintf(path, MAX_PATH, "qol_%s.log", g_name);
    g_log = fopen(path, "w");  // 截断模式：每次启动清空旧日志
    if (g_log) {
        setvbuf(g_log, nullptr, _IONBF, 0);
    }
    EnsureInit();  // 初始化队列 + 启动后台线程
}

extern "C" void LogV(const char* fmt, va_list ap) {
    if (!fmt) return;
    EnsureOpen();

    // 格式化时间戳 + 内容
    char buf[8192] = {0};
    SYSTEMTIME st = {0};
    GetLocalTime(&st);
    int prefix = snprintf(buf, sizeof(buf), "[%02u:%02u:%02u.%03u] ",
                          st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    vsnprintf(buf + prefix, sizeof(buf) - prefix - 1, fmt, ap);

    int len = (int)strlen(buf);

    if (g_asyncEnabled && g_initialized) {
        Enqueue(buf, len);
    } else {
        SyncWrite(buf);
    }
}

extern "C" void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

extern "C" void LogEv(const char* fmt, va_list ap) {
    if (!fmt) return;
    EnsureOpen();

    char buf[8192] = {0};
    SYSTEMTIME st = {0};
    GetLocalTime(&st);
    int prefix = snprintf(buf, sizeof(buf), "[%02u:%02u:%02u.%03u] [ERROR] ",
                          st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    vsnprintf(buf + prefix, sizeof(buf) - prefix - 1, fmt, ap);

    // ERROR 级别：始终同步直写，不进入异步队列
    SyncWrite(buf);
}

extern "C" void LogE(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogEv(fmt, ap);
    va_end(ap);
}

extern "C" void LogFlush(void) {
    if (!g_initialized) return;
    // 唤醒后台线程做一次刷盘
    WakeConditionVariable(&g_qCv);
    // 等待队列清空（最多 500ms）
    for (int i = 0; i < 50; i++) {
        EnterCriticalSection(&g_qLock);
        bool empty = (g_qHead == g_qTail);
        LeaveCriticalSection(&g_qLock);
        if (empty) break;
        Sleep(10);
    }
}

extern "C" void LogClose(void) {
    LogFlushAndShutdown();
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

// 热键注册：写入 qol_hotkeys.txt（游戏根目录，UTF-8）
// 格式：feature=key（每行一个），供 ModManager 自动读取覆盖
// v1.2（2026-10-05 修复三个缺陷，KB-078）：
//   ① 旧版 while(pos<=size) 在行尾 '\n' 时多跑一轮产生空行，每次调用净增 1 空行；
//   ② 旧版固定 4096 缓冲截断——文件超 4095 字节后，尾部新注册行被下次调用丢弃
//     （qol_hotkeys.txt 因此损坏：3877 行仅剩 2 行内容，其余 MOD 注册行写而即失）；
//   ③ 本版全文件动态读 + 循环边界改 pos<size + 重建时跳过空行（存量空行自愈清理）。
extern "C" void QolRegisterHotKey(const char* feature, const char* key) {
    if (!feature || !key) return;
    const char* path = "qol_hotkeys.txt";
    std::string src;
    FILE* f = fopen(path, "rb");
    if (f) {
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) src.append(buf, n);
        fclose(f);
    }
    char featureLine[128] = {0};
    snprintf(featureLine, sizeof(featureLine), "%s=", feature);
    const size_t featureLineLen = strlen(featureLine);
    std::string out;
    size_t pos = 0;
    while (pos < src.size()) {
        size_t eol = src.find('\n', pos);
        if (eol == std::string::npos) eol = src.size();
        std::string line = src.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;                                       // 跳过空行（自愈）
        if (line.compare(0, featureLineLen, featureLine) == 0) continue;  // 丢弃同 feature 旧行
        out += line;
        out += "\n";
    }
    out += featureLine;
    out += key;
    out += "\n";
    f = fopen(path, "wb");
    if (f) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
    }
}
