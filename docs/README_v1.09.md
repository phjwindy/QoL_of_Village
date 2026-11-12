# QoL of Village — 静谧田园生活质量 MOD 合集

作者：PHJ&消失的清风  
适配：Steam build 25094764（v1.09）  
GitHub：https://github.com/phjwindy/QoL_of_Village  
小黑盒发布页：https://api.xiaoheihe.cn/v3/bbs/app/api/web/share?h_camp=link&h_src=YXBwX3NoYXJl&link_id=42907269165c&new_post_share_style=true  
转载或分享时请注明出处。

> ⚠️ 本 MOD 合集仅适配游戏 v1.09 版本（build 25094764），其他版本请勿使用，可能导致功能异常或存档损坏。

## MOD 列表

| MOD | 功能 |
|-----|------|
| AutoFish | 自动钓鱼——钓鱼小游戏自动完成，循环抛竿，可一键排除垃圾鱼 |
| AutoHarvest | 树液自动收集——按 F4 一键收取范围内的树液提取器 |
| ChestSort | 箱子快速归类——打开箱子按数字键 4，物品自动整理归类 |
| EyeFix | 视角切换——按数字键 0 在结局眼与正常眼之间切换 |
| ModManager | MOD 管理器——按 F2 打开面板管理各 MOD 开关，手柄 L1+R1 也可呼出 |
| MonsterMark | 夜间地图标记——自动标记鬼鼠、宝箱、书籍、齿轮等锚点位置 |
| ProductionAuto | 生产链自动化——自动投料、收料、设备绑定，按 F5 切换 HUD |
| Scarecrow | 稻草人与洒水器重叠——允许两者放置在同一格，不再冲突 |
| SickleHarvest | 镰刀范围收割——挥一次镰刀收割范围内所有成熟作物，果树自动摇树，木耳也能收 |
| Sower | 范围播种——按数字键 5 切换 1x1/3x3/5x5/连通播种模式 |
| AutoPet | 自动抚摸动物——靠近家畜自动抚摸，每日每只限一次 |

---

## 项目简介

《Village in the Shade》（静谧田园）QoL（Quality of Life）MOD 合集，基于 C++ DLL 插件架构，通过 AOB 签名定位 + 函数 Hook 实现游戏体验优化。当前适配游戏版本 **v1.09 (build 25094764)**。

## 目录结构

```
QoL_DevKit_Git/
├── steam_api64.dll        桥接基座（MOD 加载器，放游戏根目录）
├── steam_api64_README.md   基座说明文档
├── embed_hash.py           DLL 篡改自校验：编译后哈希嵌入工具
└── source/
    ├── QoL_Shared/        共享框架（9 个模块，所有 MOD 复用）
    ├── PluginTemplate/    新 MOD 脚手架模板
    ├── AutoFish/          自动钓鱼
    ├── AutoHarvest/       树液自动收集
    ├── AutoPet/           自动抚摸动物
    ├── ChestSort/         箱子快速归类
    ├── EyeFix/            结局后保持正常眼
    ├── ModManager/        游戏内 MOD 开关管理器
    ├── MonsterMark/       夜间怪物/宝箱地图标记
    ├── ProductionAuto/    生产链自动化（投料/收料/绑定）
    ├── Scarecrow/         稻草人与洒水器重叠
    ├── SickleHarvest/     镰刀范围收割
    └── Sower/             范围播种
```

## 共享框架（QoL_Shared）

所有 MOD 共用的基础设施，编译时通过 `#include` 引入：

| 模块 | 功能 |
|------|------|
| `logging.h/.cpp` | 文件日志（`qol_<feature>.log`），FileShare 兼容读锁 |
| `aobscan.h/.cpp` | AOB 签名扫描，定位游戏函数 RVA |
| `hook.h/.cpp` | Trampoline Hook 安装/卸载，支持热移除 |
| `hotkey.h/.cpp` | 统一热键管理，支持键盘+手柄，运行时改键 |
| `feature.h` | MOD 基础接口定义（mod_init/mod_tick/mod_unload） |
| `safe_call.h` | SEH 安全的原生函数调用包装（解决 C2712） |
| `selfverify.h/.cpp` | DLL 篡改自校验框架（编译后嵌入 SHA-256，运行时验证） |

### safe_call.h 使用要点

游戏原生函数调用时，`__try/__except`（SEH）不能和带析构的 C++ 对象在同一函数块使用（MSVC C2712）。`safe_call.h` 提供 NOINLINE 纯 POD wrapper 函数隔离 SEH：

```cpp
#include "safe_call.h"

// bool 返回，1 参数
bool result = qol::SafeCallBool1(g_someFunc, status);

// int 返回，1 参数（SEH 异常时返回哨兵值 -1）
int id = qol::SafeCallInt1(g_someFunc, status);

// bool 返回，2 参数，带状态码输出
int code = -1;
bool ok = qol::SafeCallOutBool1(g_someFunc, status, &code);
```

## 各 MOD 功能与状态

| MOD | 功能 | 当前版本 | 状态 |
|-----|------|----------|------|
| AutoFish | 钓鱼小游戏自动完成+循环抛竿+垃圾排除 | v1.4.0 | 已验证 |
| AutoHarvest | F4 收集范围内树液提取器树液 | v1.8.7 | 已验证 |
| AutoPet | 靠近家畜自动抚摸，每日每只限一次 | v0.5.2 | 已验证 |
| ChestSort | 数字键 4 触发箱子内物品自动归类 | v1.3.4 | 已验证 |
| EyeFix | 数字键 0 切换：结局眼↔正常眼 | v1.1.2 | 已验证 |
| ModManager | F2 面板管理 MOD 开关，手柄 L1+R1 | v1.1.4 | 已验证 |
| MonsterMark | 夜间自动标记鬼鼠/宝箱/书籍/齿轮 | v1.0.26 | 已验证 |
| ProductionAuto | F5 切 HUD，自动投料/收料/设备绑定 | v1.1.28 | 已验证 |
| Scarecrow | 稻草人与洒水器可放置在同一格 | v1.0.3 | 已验证 |
| SickleHarvest | 挥镰刀范围收割成熟作物+果树摇树+木耳 | v2.3.0 | 已验证 |
| Sower | 数字键 5 切换 1x1/3x3/5x5/连通播种 | v1.3.1 | 已验证 |

## 安装说明

### 1. 安装桥接基座

MOD 通过 `steam_api64.dll` 桥接基座加载，**只需安装一次**，后续增减 MOD 无需重装：

1. 关闭游戏
2. 将游戏根目录原有的 `steam_api64.dll` 改名为 `steam_api64_org.dll`
3. 把本仓库的 `steam_api64.dll` 复制到游戏根目录
4. 直接从 Steam 启动游戏即可

> 如需卸载基座：删除 `steam_api64.dll`，把 `steam_api64_org.dll` 改回 `steam_api64.dll` 即可完全恢复原版。
>
> 详细说明见 [steam_api64_README.md](steam_api64_README.md)

### 2. 安装 MOD

1. 编译产出 DLL（或获取已编译的 DLL）
2. 在游戏根目录的 `Mods\` 文件夹下创建子目录：`Mods\<MOD名>_v<版本号>\`
3. 将 DLL 放入对应子目录
4. 启动游戏，MOD 自动加载

```
游戏根目录/
├── steam_api64.dll          ← 桥接基座
├── steam_api64_org.dll       ← 原 Steamworks DLL（改名而来）
└── Mods/
    ├── AutoFish_v1.4.0/AutoFish.dll
    ├── AutoHarvest_v1.8.4/AutoHarvest.dll
    ├── ChestSort_v1.3.4/ChestSort.dll
    └── ...
```

## 防篡改自校验机制

所有 MOD DLL 集成了自校验框架（`selfverify.h/.cpp`），防止 DLL 被第三方篡改后损坏存档：1. 编译后由 `embed_hash.py` 计算完整 DLL 的 SHA-256（排除签名段），写入 DLL 内预留的签名段（`QOL_HASH_SIGNATURE`）
2. 运行时 `mod_init` 阶段调用 `SelfVerifyInit()`，重新计算自身 DLL 的 SHA-256 与嵌入值比对
3. 校验失败则日志留痕并禁用功能，避免被篡改的 DLL 损坏存档
4. 开发阶段（未嵌入哈希）自动跳过校验，不影响开发调试

### 编译后嵌入哈希

```bash
python embed_hash.py path/to/Mod.dll           # 单个 DLL
python embed_hash.py path/to/mods/              # 批量处理目录下所有 .dll
python embed_hash.py path/to/Mod.dll --verify    # 仅验证不写入
```

## 开发环境

- **编译器**：MSBuild via vswhere，x64 Release，vcxproj PlatformToolset v145 + `/utf-8`
- **源码编码**：UTF-8 无 BOM，行尾 CRLF
- **加载方式**：`steam_api64` 桥接加载器，扫描 `mods\` 目录下 DLL
- **MOD 目录格式**：`mods\<MOD名>_v<版本号>\<MOD名>.dll`
- **DLL 接口**：每个 MOD 导出 `mod_init` / `mod_tick` / `mod_unload` 三个函数

## 游戏更新适配流程

游戏发布新版本后，按以下步骤适配：

1. **计算新 exe 指纹**：SHA-256 + 文件大小
2. **AOB 重新扫描**：用 `aobscan2.py` 对新 exe 执行签名扫描，定位所有函数 RVA
3. **RTTI 重定位**：用 `rtti_scan2.py` 重新定位 vtable 类型地址
4. **逐个 RVA 验证**：用 `verify_rvas.py` 实读字节验证每个 RVA 处的函数序言
5. **配方表更新**：用 `recipe_scan2.py` 重新提取配方表
6. **VerifyExeBuild 硬编码 SHA 更新**：各 MOD 的 `version_manifest.h` 中 exe SHA 需更新
7. **编译部署+实测**：先适配 2~3 个核心 MOD 实测通过，再批量适配剩余

### 关键注意

- **代码非整体平移**：各函数偏移不等，.rdata/.data 段不能用 delta 推算
- **AOB 三层验证法**：①字节签名全 .text 搜索 → ②唯一匹配验 prologue → ③RVA 实读字节验证
- **四大坑**：vtable 重定位、VerifyExeBuild 硬编码 SHA 过期、E8 call rel32 位移跨版本变化、vtable 槽位函数整体重定位

## 已验证可用的原生函数

| 函数 | RVA | 用途 |
|------|-----|------|
| `output_helper` | 0x165DF0 | 物品物化+转移核心 |
| `native_load` | 0x26FF20 | 生产机器投料 |
| `StatusSearchByBaseID` | 0x250690 | 按 baseID 查找 GimmickStatus |
| `spatialSearch` | 0x189990 | 空间索引搜索 |
| `map_find` | 0x150B10 | 哈希表查找 |
| `HarvestSettle` | 0x211DD0 | 作物收割结算（SickleHarvest 木耳关键） |

## 开发原则

1. **原生函数优先**：涉及游戏系统交互时，优先调用原生函数（AOB 定位→直接调用），而非自写逆向逻辑。原生函数天然兼容所有槽位数和容器类型，自写逻辑天然脆弱
2. **死档红线**：禁止手写清槽/重置状态，必须走原生 event notify 链
3. **诊断优先于改码**：功能异常先读日志定位根因，不臆测
4. **发布版干净**：发布版关闭所有诊断日志，不残留调试代码
5. **备份先行**：修改源码前必须备份原文件

## 插件开发快速上手

1. 复制 `PluginTemplate/` 目录，重命名为新 MOD 名
2. 修改 `plugin.cpp` 中 3 处标记（功能名/AOB 签名/Tick 逻辑）
3. 修改 `.vcxproj` 中的项目名和输出 DLL 名
4. 在 `version_manifest.h` 中填入游戏的 exe SHA-256 和 build 号
5. 编译产出 DLL，放入 `mods\<MOD名>_v<版本号>\` 目录

### Hook 模式

```cpp
// 安装 hook（mod_init 中）
g_origFunc = (FuncType)InstallHook(targetRVA, detourFunc, hookSize);

// detour 函数中调用原函数
auto result = g_origFunc(args...);

// 卸载 hook（mod_unload 中）
RemoveHook(g_hookHandle);
```

### 日志模式

```cpp
LogOpen("myfeature");  // mod_init 中，创建 qol_myfeature.log
Log("info: value=%d", val);
// 发布版通过注释 #define 关闭日志
```

## 致谢

- **BigL233**：MOD 源码作者，本项目 QoL MOD 合集的原始代码基础
- **小黑盒用户「哀喜」**：基座 `steam_api64.dll` 桥接加载器源码作者及开发提点

---

> 本项目为个人兴趣开发的免费 MOD，不涉及任何商业用途。  
> 游戏版权归原开发者所有，MOD 仅改善玩家体验，不修改游戏核心数据。  
> 作者：PHJ&消失的清风，转载或分享时请注明出处。
