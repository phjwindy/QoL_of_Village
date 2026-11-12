// aobscan.h —— AOB 特征码扫描（支持 '??' 通配符）
// 用法：ScanAOB("village.exe", "48 89 5C 24 18 55 56 57 ?? ?? 48 83 EC 28");
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace qol {

// 解析 "AA BB CC ?? DD" 为字节 + 掩码（mask=true 表示需匹配）
bool ParseAOB(const char* text, std::vector<uint8_t>& out_bytes, std::vector<bool>& out_mask);

// 在指定模块内扫描，返回第一个匹配 VA（找不到返回 0）
uintptr_t ScanModuleAOB(const char* moduleName, const char* aobText);

// 扫描整个进程（慎用，慢）—— 一般 ScanModuleAOB 足够
uintptr_t ScanProcessAOB(const char* aobText);

// 简易特征码表：按 build 号查表（DEV_GUIDE §6）
struct AOBEntry {
    const char* name;
    const char* sig_24646798; // BigL233 原始（参考/占位）
    const char* sig_24969282; // ★1.08.1 真实值，由用户提供
};

// 便捷函数：从模块内扫描 AOB 字符串并返回地址；失败返回 0
uintptr_t FindPattern(const char* moduleName, const char* aobText);

// ===== preflight 掩码验证（v1.20+ 推荐替代裸 memcmp，FLiNG 式跨版本免疫）=====
// 背景：旧 preflight 用 memcmp 全字节比对 SIG_ 数组，E8 rel32 / mov disp32 /
//       栈帧大小等版本敏感字节一变就 FAIL，需逐 RVA 人工重扫。
//       带掩码后这些位置通配，小版本更新自动免疫。
//
// 用法一（数组式，配 SIG_/MASK_ 数组对）：
//   static const uint8_t SIG[]  = {0x48,0x89,0x5C,0x24,0x00,0xE8,0,0,0,0};
//   static const uint8_t MASK[] = {0xFF,0xFF,0xFF,0xFF,0x00,0xFF,0x00,0x00,0x00,0x00};
//   //                            prologue↑        栈帧↑通配  E8↑  rel32↑全部通配
//   if (qol::VerifyBytes(p, SIG, MASK, sizeof(SIG))) { /* preflight PASS */ }
//   MASK[i] != 0 → 该位置必须匹配；MASK[i] == 0 → 通配跳过
//
// 用法二（文本式，与 aobscan2.py 签名字符串同源，迁移成本最低）：
//   if (qol::VerifyAOBText(p, "48 89 5C 24 ?? E8 ?? ?? ?? ??")) { /* PASS */ }
//   '??' 位置跳过比对 —— 离线扫描工具与运行时校验共用同一份签名字符串，
//   杜绝「工具扫得到、运行时 memcmp 却 FAIL」的双重维护漂移。
//
// 两者均带 SEH 保护：RVA 处内存不可读时返回 false（不崩溃），与现有
// preflight 的 fail-safe 语义一致。
bool VerifyBytes(const void* addr, const uint8_t* sig, const uint8_t* mask, size_t len);
bool VerifyAOBText(const void* addr, const char* aobText);

} // namespace qol

// ===== 全局兼容别名（旧骨架 chestsort.cpp 以无命名空间方式调用）=====
// 扫描主模块（village.exe / 本进程 exe）
static inline uintptr_t ScanAOB(const char* /*module*/, const char* aobText) {
    return qol::ScanModuleAOB(nullptr, aobText);
}
