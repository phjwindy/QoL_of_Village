// hot_config.h — QoL 热调参框架：JSON 配置热重载 + CE 实时改参数
//
// v2.0 后台轮询（P1-4）：mtime 检查 + 文件读取移至后台线程，
//   游戏线程不再读文件，只读 volatile 内存快照。
//   - 后台线程 1s 间隔轮询 QoL_hot.json / 覆盖文件 mtime
//   - mtime 变化 → 后台线程读文件 + 解析 + ApplyValue → volatile 变量
//   - HotConfig_Poll() 首次调用（mod_init 中）执行首次加载 + 启动后台线程，
//     后续调用（mod_tick 中）为无操作（返回 0）
//   - 14 MOD 调用代码无需修改
//
// 线程安全：
//   - volatile 保证可见性；x64 对齐 32/64 位读写原子
//   - 后台线程写 volatile 值，游戏线程（hook 回调/mod_tick）读 volatile 值
//   - 最坏情况：读到半新半旧的值 → semanticsOk 检查失败 → 安全禁用，不崩溃
//   - g_reloadGen 计数器：后台线程 reload 前后各 +1，可选用于检测 reload 窗口
//
// 设计目标：改参数不重编译、不重启游戏
//   1. QoL_hot.json 统一配置（按 MOD 名分组）
//   2. 各 MOD 目录下 QoL_hot.<modname>.json 独立覆盖（优先级更高）
//   3. 优先级：独立文件 > 统一文件分组 > 代码内置默认值
//   4. CE 实时改：volatile 全局变量 + mod_init 打印地址
//
// 用法（各 MOD cpp）：
//   #include "hot_config.h"
//   static volatile uintptr_t g_rva_redraw = 0x2F52F0;  // 默认值
//   static volatile int      g_enabled = 1;
//   extern "C" void mod_init() {
//       HotConfig_Register("monstermark", &g_rva_redraw, "RVA_NIGHT_MAP_REDRAW", HOT_RVA, 0x2F52F0);
//       HotConfig_Register("monstermark", &g_enabled,   "enabled",              HOT_INT, 1);
//       // ... Init 逻辑
//   }
//   extern "C" void mod_tick() {
//       HotConfig_Poll();  // v2.0：首次=加载+启动后台，后续=无操作
//   }
//
// 作者：PHJ&消失的清风
#pragma once
#include <cstdint>
#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

// ---- 参数类型 ----
typedef enum {
    HOT_INT   = 1,   // int (32-bit)
    HOT_BOOL  = 2,   // int (0 or 1)
    HOT_RVA   = 3,   // uintptr_t (64-bit, 用于 RVA/偏移量)
    HOT_FLOAT = 4,   // float (32-bit)
    HOT_INT64 = 5,   // int64_t (64-bit)
} HotType;

// ---- 注册一个可热调参数 ----
// modName:  MOD 名（如 "monstermark"），对应 JSON 分组键
// addr:     volatile 变量地址（实际类型按 type 解释）
// key:      JSON 键名（如 "RVA_NIGHT_MAP_REDRAW"）
// type:     参数类型
// defVal:   默认值（long long，按类型截断/转换）
void HotConfig_Register(const char* modName, void* addr,
                         const char* key, HotType type, long long defVal);

// ---- 在 mod_tick 中调用（v2.0：首次=加载+启动后台线程，后续=无操作）----
// 返回值：首次调用返回 1（已加载），后续返回 0
int HotConfig_Poll(void);

// ---- 打印 CE 地址表（在 mod_init 末尾调用，所有 Register 之后）----
void HotConfig_DumpCE(const char* modName);

// ---- 获取某 MOD 注册的参数数量 ----
int HotConfig_Count(const char* modName);

#ifdef __cplusplus
}
#endif
