// logging.h — 统一日志：qol_<feature>.log（游戏根目录）
//
// v2.0 异步落盘（P1-3）：主线程入队，后台线程批量刷盘（默认 200ms 间隔）。
//   - async 开关：qol_log.cfg（独立小文件，非 HotConfig，避免鸡生蛋）
//     格式：async=1（默认异步）或 async=0（全同步直写）
//     后台线程每 ~1s 重读 qol_log.cfg 实现热切换
//   - LogE() 为 ERROR 级别：始终同步直写，保证崩溃前最后一条错误可见
//   - 队列溢出：丢弃最旧条目 + 计数告警，绝不阻塞游戏线程
//   - 多 DLL：每 DLL 静态编入一份 = 各自一个后台线程（14-16 个低频睡眠线程，
//     开销可忽略；共享线程需跨 DLL IPC，复杂度高风险大，不取）
//
// API 兼容两套约定：
//   - 旧骨架：LogOpen(name) / Log(fmt,...) / LogClose()
//   - 新骨架：qol::SetLogName(name) / qol::Log(fmt,...)
//   - v2.0 新增：LogE(fmt,...) 错误级同步直写 / LogFlush() 手动刷新队列
#pragma once
#include <cstdio>
#include <cstdarg>

#ifdef __cplusplus
extern "C" {
#endif

// 打开日志文件（同一进程多次调用会切换文件名并重开；请在加载线程调用一次）
void LogOpen(const char* feature);
// 可变参数版本（INFO 级别，异步入队或同步直写取决于 qol_log.cfg）
void Log(const char* fmt, ...);
// va_list 版本（供命名空间转发）
void LogV(const char* fmt, va_list ap);
// ERROR 级别：始终同步直写（不进入异步队列），崩溃前最后一条错误可见
void LogE(const char* fmt, ...);
// va_list 版本的 ERROR 级别
void LogEv(const char* fmt, va_list ap);
// 手动刷新异步队列（阻塞直到队列清空），供 shutdown 路径调用
void LogFlush(void);
// 关闭日志文件（flush 队列 + 停止后台线程）
void LogClose(void);
// 注册 MOD 热键：写入 qol_hotkeys.txt（供 ModManager 自动读取）
//   feature: MOD 英文名（小写，如 "autofish"）
//   key:     热键描述，支持多键（如 "9"、"4, D-pad Up"、"F6"）
//           多键用逗号分隔，ModManager 会全部显示并逐个检测冲突
// 注意：不以 RegisterHotKey 命名，避免与 Win32 API 重名冲突
void QolRegisterHotKey(const char* feature, const char* key);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace qol {

// 与全局 C 函数等价（提供命名空间风格，供新代码使用）
inline void SetLogName(const char* feature) { ::LogOpen(feature); }

inline void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ::LogV(fmt, ap);
    va_end(ap);
}

inline void LogE(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ::LogEv(fmt, ap);
    va_end(ap);
}

struct ScopeLog {
    const char* name;
    ScopeLog(const char* n) : name(n) { ::Log("[%s] enter", name); }
    ~ScopeLog() { ::Log("[%s] leave", name); }
};

} // namespace qol
#endif
