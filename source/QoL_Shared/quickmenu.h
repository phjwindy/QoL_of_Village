// quickmenu.h —— QoL 快捷菜单跨 DLL 约定（v1.0）
//
// 设计：
//   开关型 MOD 在自己的 DLL 中导出 QolQuickMenuItems()，
//   宿主（ModManager F1 快捷菜单）打开时动态 GetProcAddress
//   拉取菜单项，数字键触发 toggle 回调。
//
//   无初始化时序问题：菜单每次打开都实时查询，不维护注册表。
//   无跨 DLL 全局状态：约定只靠导出符号。
//
// 用法（开关型 MOD 侧）：
//   #include "quickmenu.h"
//
//   static bool QuickAutoIsOn() { return g_xxx.load(); }
//   static void QuickAutoToggle() { /* 切换逻辑 */ }
//
//   extern "C" __declspec(dllexport) int QolQuickMenuItems(
//           QolQuickMenuItem* items, int maxItems) {
//       int n = 2;  // 本 MOD 提供的开关项数
//       if (items && maxItems >= 1) items[0] = { "自动钓鱼", QuickAutoIsOn, QuickAutoToggle };
//       if (items && maxItems >= 2) items[1] = { "垃圾排除", QuickNoTrashIsOn, QuickNoTrashToggle };
//       return n;  // items 为空或不足时仍返回真实项数
//   }
//
// 宿主（ModManager）枚举：g_mods[].dllName → GetModuleHandleW →
//   GetProcAddress("QolQuickMenuItems") → 调用收集。
//
// 线程安全：isOn/toggle 回调由宿主窗口线程调用，MOD 侧实现
//   应自行保证原子性（atomic / volatile LONG 已够用）。

#pragma once

struct QolQuickMenuItem {
    const char* name;    // UTF-8 显示名（宿主转 UTF-16 渲染）
    bool (*isOn)();      // 当前开关状态（nullptr = 不显示状态）
    void (*toggle)();    // 切换动作（内部自行判断可用性并提示）
};

// 每个开关型 MOD 导出一次；返回本 MOD 的开关项总数
extern "C" __declspec(dllexport) int QolQuickMenuItems(
    QolQuickMenuItem* items, int maxItems);
