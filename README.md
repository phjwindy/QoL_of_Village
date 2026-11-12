# QoL of Village — 静谧田园生活质量 MOD 合集

作者：PHJ&消失的清风  
适配：Steam build 25640656（v1.30）  
GitHub：https://github.com/phjwindy/QoL_of_Village  
转载或分享时请注明出处。

> ⚠️ 本合集仅适配游戏 v1.30（build 25640656），其他版本请勿使用，可能导致功能异常或存档损坏。

---

## 下载

- **二进制包**：`Village in the Shade QOLv1.30_releasev2.zip`（2.35 MB / 43 项），见仓库 [`releases/`](releases/) 目录下的同名文件
- **源码**：本仓库 `source/` 目录（tag `v1.30_releasev2`），20 个源码目录，含 `ProductionAuto` v1.6.63、`DebugHUD` 与 `QoL_Shared`。
- 二进制包内**不含** `ProductionAuto` 的 DLL：v1.6.63 为未验收实验版，含诊断日志与 `UI_NATIVE_ONLY` / `UI_PIGGYBACK` 两个 A/B 热开关；其二进制不在发布包 `MODS\` 内。

## 安装（三步）

1. **装加载器**（只需一次）：关闭游戏 → 进入游戏根目录（Steam 右键 → 管理 → 浏览本地文件）→ 把原版 `steam_api64.dll` 改名为 `steam_api64_org.dll` → 把包内 `steam_api64.dll` 复制进去。
2. **装 MOD**：在游戏根目录建 `mods\` 文件夹，把包内 `MODS\` 下的 17 个子文件夹原样复制进去（例如 `mods\AutoFish_v1.6.10\autofish.dll`）。
3. **开玩**：从 Steam 启动，MOD 自动加载；按 **F9** 打开管理面板查看状态，按 **F2** 打开快捷菜单。

**懒人法**：直接双击包内 `install.bat`（自动从 Steam 注册表定位游戏目录），或把 `village.exe` / 游戏目录拖到 `install.bat` 上。安装器会自动完成基座备份、旧版让位（同前缀旧版本移到 `待用MOD\`）、复制 MOD、版本校验四件事。校验不一致只警告、不阻断。

**回滚**：删掉游戏根目录的 `steam_api64.dll`，把 `steam_api64_org.dll` 改回原名即完全恢复；卸载某个 MOD 直接删 `mods\` 里对应文件夹。

> 详细的版本清单、坏档自检与逐项用法见包内 `MOD使用说明.txt`、`MOD情况简述.txt`、`玩家存档问题自检指引.md`、`发布说明_v1.30_releasev2.txt`。

## 热键速查

| 键 | 作用 |
|----|------|
| F2 / 手柄 L1+R1 | 快捷菜单（自动钓鱼、正常眼、时间冻结、书/齿轮标记等开关） |
| F4 | 自动采集树液：对箱子按一下设箱并开始，再按取消 |
| F6 | 关闭一击必杀（**默认开启**，启动/换档后约 8 秒生效） |
| F7 | 视野切换：原版 ↔ 2 倍 |
| F9 / 手柄 L3+R3 | 管理面板：查看/启停全部 MOD、改热键、查版本 |
| 4 / 十字键上 | 一键收纳（背包物品放进附近箱子同类堆） |
| 5 / 十字键左 | 播种范围：1×1 → 3×3 → 5×5 |

热键可在 F9 面板里改，保存后重进游戏生效。

## MOD 列表（17 个，v1.30_releasev2）

| MOD | 版本 | 功能 | 默认 / 触发 |
|-----|------|------|-------------|
| ModManager | v1.2.23 | MOD 管理面板与快捷菜单 | F9 / F2 |
| AutoHarvest | v1.9.18 | 自动采集树液 | 需按 F4 设收集箱 |
| ChestSort | v1.4.8 | 一键收纳 | 按 4 |
| Sower | v1.3.44 | 范围播种 | 按 5 切换 |
| CameraZoom | v1.2.0 | 视野 2 倍切换 | F7 |
| AutoFish | v1.6.10 | 自动钓鱼、循环抛竿、可屏蔽垃圾鱼 | 常规开启，F2 可关 |
| HuntOneShot | v1.1.21 | 狩猎一击必杀 | **默认开启**，F6 关 |
| SaveGuard | v1.3.18 | 存档巡检与净化，自动修复悬垂引用等坏档病灶 | 被动运行 |
| EyeFix | v1.2.15 | 通关后保持正常眼 | F2 菜单开关 |
| TimeFreeze | v1.3.5 | 时间冻结；睡觉/强制睡眠自动解冻，醒来保持解冻 | F2 菜单开关 |
| AutoPet | v0.6.14 | 每天自动抚摸家畜，`config.txt` 调好感倍率 | 被动自动 |
| BirthdayReminder | v1.2.1 | 当天 NPC 生日 HUD 提醒 | 被动自动 |
| MineHelper | v1.0.22 | 最多敲 5 块石头必出下一层出口 | 被动自动 |
| MonsterMark | v1.1.4 | 夜间地图标记老鼠/宝箱；书/齿轮可切换 | 被动自动（本版修正了 v1.20→v1.30 的两处坏地址） |
| Scarecrow | v1.0.14 | 稻草人与洒水器可同格 | 被动自动 |
| SelfServiceStore | v1.2.4 | 商店全天营业 | 被动自动 |
| SickleHarvest | v2.3.12 | 镰刀范围收割 + 摇果树 | 被动自动 |

`ProductionAuto`（生产链自动化）**只在源码里**，版本 v1.6.63，未随包发布二进制。

---

## 已知问题

- **ProductionAuto（v1.6.63，未验收实验版）**：小 UI / 加工动画不显示。场上 22 台加工中机器、角色走到机器群中间仍 `hits=0/0`，`uiPros` 全 0、UI 重建 no-op（函数早退、`baseFar=0`，拿不到 harvested baseId）；投料 / 收料主链正常，与角色位置、距离无关。
- 已知线索：PA 对象创建集中在进档瞬间（`harvestN=0`、bootstrap base=13、guard 锁 1 小时），游戏原生 near 类调用仅 2 次、far / manpu 零次，形成循环依赖。
- 该版本二进制不在发布包 `MODS\` 内；源码见 `source/ProductionAuto/`（`production_automation_v2.inl`、`production_auto.cpp`），含 `[PA][uifix]` 诊断日志与 `UI_NATIVE_ONLY` / `UI_PIGGYBACK` 两个 A/B 热开关（`QoL_hot.json` 1 秒轮询 mtime）。

---

## 项目简介与源码结构

基于 C++ DLL 插件架构，通过 AOB 签名定位 + 函数 Hook 实现体验优化。共享框架 `QoL_Shared` 被所有 MOD 复用。

```
QoL_of_Village/
├── README.md
├── embed_hash.py            DLL 篡改自校验：编译后哈希嵌入工具
├── steam_api64.dll          桥接基座（MOD 加载器，放游戏根目录）
├── steam_api64_README.md    基座说明
└── source/
    ├── QoL_Shared/          共享框架（9 模块）
    ├── PluginTemplate/      新 MOD 脚手架
    ├── AutoFish/  AutoHarvest/  AutoPet/  BirthdayReminder/  CameraZoom/
    ├── ChestSort/  DebugHUD/  EyeFix/  HuntOneShot/  MineHelper/
    ├── ModManager/  MonsterMark/  ProductionAuto/  SaveGuard/
    ├── Scarecrow/  SelfServiceStore/  SickleHarvest/  Sower/  TimeFreeze/
    └── 源码说明.txt
```

## 共享框架（QoL_Shared）

| 模块 | 功能 |
|------|------|
| `logging.h/.cpp` | 文件日志（`qol_<feature>.log`），FileShare 兼容读锁 |
| `aobscan.h/.cpp` | AOB 签名扫描，定位游戏函数 RVA |
| `hook.h/.cpp` | Trampoline Hook 安装/卸载，支持热移除 |
| `hotkey.h/.cpp` | 统一热键管理，键盘+手柄，运行时改键 |
| `feature.h` | MOD 基础接口（`mod_init` / `mod_tick` / `mod_unload`） |
| `safe_call.h` | SEH 安全的原生函数调用包装（解决 C2712） |
| `selfverify.h/.cpp` | DLL 篡改自校验框架（编译后嵌入 SHA-256，运行时验证） |

### safe_call.h 使用要点

`__try/__except`（SEH）不能和带析构的 C++ 对象在同一函数块使用（MSVC C2712）。`safe_call.h` 用 NOINLINE 纯 POD wrapper 隔离 SEH：

```cpp
#include "safe_call.h"

bool result = qol::SafeCallBool1(g_someFunc, status);
int  id     = qol::SafeCallInt1(g_someFunc, status);   // SEH 异常返回哨兵值 -1
int  code   = -1;
bool ok     = qol::SafeCallOutBool1(g_someFunc, status, &code);
```

## 防篡改自校验

1. 编译后由 `embed_hash.py` 计算 DLL 的 SHA-256（排除签名段），写入 `QOL_HASH_SIGNATURE` 预留段；
2. 运行时 `mod_init` 调用 `SelfVerifyInit()` 重算并比对；
3. 校验失败则日志留痕并禁用功能，避免被篡改的 DLL 损坏存档；
4. 开发阶段未嵌入哈希时自动跳过校验。

```bash
python embed_hash.py path/to/Mod.dll            # 单个
python embed_hash.py path/to/mods/              # 批量
python embed_hash.py path/to/Mod.dll --verify   # 只验证
```

## 开发环境

- **编译器**：MSBuild via vswhere，x64 Release，vcxproj PlatformToolset v145 + `/utf-8`
- **源码编码**：UTF-8 无 BOM，行尾 CRLF
- **加载方式**：`steam_api64` 桥接加载器扫描 `mods\` 下的 DLL
- **MOD 目录格式**：`mods\<MOD名>_v<版本号>\<MOD名>.dll`
- **DLL 接口**：导出 `mod_init` / `mod_tick` / `mod_unload`

## 开发原则

1. **原生函数优先**：涉及游戏系统交互优先调用 AOB 定位的原生函数，自写逆向逻辑天然脆弱；
2. **死档红线**：禁止手写清槽/重置状态，必须走原生 event notify 链；
3. **诊断优先于改码**：异常先读日志定位根因，不臆测；
4. **发布版干净**：关闭诊断日志，不残留调试代码；
5. **备份先行**：改源码前先备份。

## 插件开发快速上手

1. 复制 `PluginTemplate/`，重命名为新 MOD 名；
2. 改 `plugin.cpp` 三处标记（功能名 / AOB 签名 / Tick 逻辑）；
3. 改 `.vcxproj` 项目名与输出 DLL 名；
4. 在 `version_manifest.h` 填入当前 exe SHA-256 与 build 号；
5. 编译后放入 `mods\<MOD名>_v<版本号>\`。

```cpp
// 安装 hook（mod_init）
g_origFunc = (FuncType)InstallHook(targetRVA, detourFunc, hookSize);
// detour 中调用原函数
auto result = g_origFunc(args...);
// 卸载（mod_unload）
RemoveHook(g_hookHandle);
```

```cpp
LogOpen("myfeature");        // mod_init 中，创建 qol_myfeature.log
Log("info: value=%d", val);  // 发布版通过注释 #define 关闭
```

## 致谢

- **BigL233**：MOD 源码作者，本项目 QoL MOD 合集的原始代码基础
- **gloaming（小黑盒「哀喜」，[gloam-ing/VillageInTheShade_Mods](https://github.com/gloam-ing/VillageInTheShade_Mods)）**：基座 `steam_api64.dll` 桥接加载器源码作者及开发提点；该仓库（MIT 授权，已归档）提供 10 个 MOD 的 C 源码与 `shared/` 共享层源码支持

---

> 本项目为个人兴趣开发的免费 MOD，不涉及任何商业用途。  
> 游戏版权归原开发者所有，MOD 仅改善玩家体验，不修改游戏核心数据。  
> 作者：PHJ&消失的清风，转载或分享时请注明出处。
