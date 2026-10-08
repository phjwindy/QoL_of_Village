---
AIGC:
  ContentProducer: '001191110102MAD55U9H0F10002'
  ContentPropagator: '001191110102MAD55U9H0F10002'
  Label: '1'
  ProduceID: '26e913c9-a877-4611-8e4f-733f814971d7'
  PropagateID: '26e913c9-a877-4611-8e4f-733f814971d7'
  ReservedCode1: '66ffa19b-45af-4ba9-9477-2fe4d02e4d67'
  ReservedCode2: '66ffa19b-45af-4ba9-9477-2fe4d02e4d67'
---

# 箱子快速归类 Mod（ChestSort v1.2.3）

## 这是什么

按**手柄方向键上（D-pad Up）**或**数字键 4**，自动将背包中的物品转移到附近箱子中已有的同类堆叠上。
不需要打开箱子界面，走路路过按一下即可归类。

- 只往**已有同类物品**的箱子堆叠上叠加，不会创建新槽位
- 只操作 720 单位范围内的箱子（与游戏 16 格世界网格一致）
- 转移前校验容量、转移后验证数量一致性，不会丢物
- 走路零开销（扫描在后台分片完成）
- 支持 Xbox 手柄、DualSense 手柄、其他 DirectInput 手柄，热插拔自动重连

## 安装

本 Mod 通过游戏根目录的 `steam_api64.dll` 桥接基座加载，与其它 Mod 共存。

1. 关闭游戏；
2. 把整个 `ChestSort_v1.2.3` 文件夹放入游戏根目录的 `Mods` 文件夹；
3. 确认游戏根目录已有桥接基座（`steam_api64.dll` + `steam_api64_org.dll`）；
4. 直接从 Steam 启动游戏。

## 使用

| 操作 | 效果 |
|------|------|
| 手柄 **方向键上** | 将背包物品转移到附近箱子的同类堆叠上 |
| 键盘 **数字键 4** | 同上（键盘备选） |

- 附近没有箱子或没有同类物品时不会有任何反应；
- 第一次按键会有极短延迟（后台扫描注册表），之后秒响应；
- 走远后到了新的箱子区域，第一次按键会重新扫描，之后又是秒响应；
- 按住不松开不会重复触发，松开后再按才会再次生效（边沿触发）。

### 手柄兼容性

| 手柄类型 | 检测方式 | 热插拔 |
|----------|----------|--------|
| Xbox 手柄 | XInput（后台线程 ~120Hz 采样） | 自动重连 |
| DualSense（PS5） | HID 直读（后台阻塞读取输入报告） | 断开后每 2 秒重新枚举 |
| 其他 DirectInput 手柄 | joyGetPosEx 同步回退 | 每 2 秒刷新设备列表 |

三路检测自动选择优先级：XInput > HID > joyGetPosEx，哪条路有手柄用哪条。

## 卸载

直接删除游戏根目录 `Mods\ChestSort_v1.2.3` 文件夹即可。

## 文件清单

| 文件 | 说明 |
|------|------|
| `chestsort.dll` | 插件本体 |
| `README.md` | 本说明文档 |
| `chestsort.cpp` | 源代码（供后续开发者参考维护） |

> 正常运行时**不会生成任何日志文件**。如需调试，参见下文。

## 调试日志开关

发布版默认禁用所有日志输出，不生成 `qol_chestsort.log`。

如需开启日志（排查问题、适配新版本等），需重新编译：

1. 打开源码 `chestsort.cpp`；
2. 找到文件顶部的日志开关（`#include "logging.h"` 之后）：
   ```
   //#define CHESTSORT_LOGGING
   ```
3. 取消注释，改为：
   ```
   #define CHESTSORT_LOGGING
   ```
4. 重新编译（Release x64），部署 `chestsort.dll` 到游戏 `Mods` 目录；
5. 启动游戏后操作，日志会写入游戏根目录的 `qol_chestsort.log`（每次启动清空旧日志）。

调试完毕后将注释改回，重新编译即可恢复无日志模式。

## 适配新游戏版本

游戏更新后本 Mod 可能失效。以下情况需要重新定位地址：

| 地址类型 | 数量 | 适配方式 | 工具 |
|----------|------|----------|------|
| AOB 签名函数 | 9 个 | 自动适配（字节签名扫描） | 无需操作 |
| 硬编码 RVA | 5 个 | 需重新定位 | vtable_scan_tool + RTTI 验证 |

适配流程：

1. 开启调试日志（见上节），启动游戏；
2. 查看 `qol_chestsort.log`，检查 AOB 扫描结果是否全部 OK；
3. 如果 AOB 全部 OK 但按键无反应或崩溃，说明硬编码 RVA 需更新；
4. 用 vtable_scan_tool 重新定位以下 5 个 RVA：
   - `RVA_GAME_ROOT`（0x10CD990）
   - `WORLD_OBJECT_REGISTRY_RVA`（0x10D5A60）
   - `SEARCH_CALLBACK_VTABLE_RVA`（0xE17168）
   - `RVA_BATCH_RECALCULATE`（0x13CD90）
   - `RVA_BATCH_UI_REFRESH`（0x0F1300）
5. 更新源码中的常量，重新编译部署。

## 技术架构（供 Mod 作者参考）

### 性能优化

| 优化点 | 说明 |
|--------|------|
| VirtualQuery 区域缓存 | 同内存区域的多次 `IsReadable` 检查只调一次系统调用，批量扫描提速 ~100 倍 |
| 全局箱子注册表 | 搜索过的箱子坐标永久保留，走远才增量补搜（720 半径），避免重复 `spatialSearch` |
| 分片扫描 | `IsChestStatus` 过滤拆成每帧 12 条，在 `mod_tick` 后台无感完成 |
| 工作缓存复用 | 玩家移动 < 50 单位时复用上次的箱子物品缓存，不重读 |
| 手柄独立线程 | XInput/HID 检测在后台线程运行，不阻塞游戏主线程 |

### 关键函数调用链

```
mod_init  → AOB 扫描定位 9 个引擎函数 + 5 个硬编码 RVA
           → InitGamepad（启动 XInput 线程 + HID 线程 + joyGetPosEx 探测）
mod_tick  → ProcessRegistryChunk（每帧 12 条分片过滤）
           → PollInput
              → CheckDpadUpPressed（XInput > HID > joyGetPosEx 三路优先级）
              → 检测数字键 4（备选）
              → DoSort
                 → StartRegistryScan（仅在走到新区域时调一次 spatialSearch）
                 → BuildWorkCache（坐标过滤 + 实时读物品）
                 → itemAdjust 转移（容量校验 → 加目标 → 减源 → 清槽位 → 释放引用）
                 → batchRecalc + batchUIRefresh + batchDirty（三件套刷新）
```

### 安全措施

- 所有指针访问前 `IsReadable` 检查（含区域缓存加速）
- 转移前：`commandCapacity` 实时校验目标堆剩余容量
- 转移中：先增目标、验证数量正确后才减源，失败均回滚
- 转移后：`InterlockedCompareExchangePointer` 清空槽位 + `intrusiveRelease` 释放引用
- 全程 SEH 保护，异常立即停止不继续操作
- 零位置保护：玩家位置接近零（<1.0）视为世界未加载，跳过归类

## 版本信息

- 适配游戏版本：v1.08.1（build 24969282）
- 插件版本：v1.2.3
- 开发环境：VS2026 Community / C++ / x64

## 警告

- 转移操作会修改游戏内存中的物品数据，请提前备份存档；
- 过剧情时建议不要使用；
- 游戏更新后可能失效，需重新适配地址后才能使用。

---

作者：PHJ。转载或分享时请注明出处。

