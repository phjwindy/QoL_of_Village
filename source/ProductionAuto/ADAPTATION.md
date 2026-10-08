# ProductionAuto 版本适配指南

本文档面向 AI 协作者和人类开发者，说明游戏更新后如何适配 ProductionAuto MOD。

## 架构概述

所有版本相关数据集中在 `version_manifest.h` 一个文件中，分 6 个部分：

| 部分 | 内容 | 数量 |
|------|------|------|
| PART 1 | exe 指纹（文件大小 + SHA-256） | 1 组 |
| PART 2 | 通用 RVA（game_root, spatial_test） | 2 个 |
| PART 3 | GameClock hook RVA + 签名 | 17 RVA + 10 签名 |
| PART 4 | NearbyChestSort RVA + 签名 | 21 RVA + 17 签名 |
| PART 5 | ProductionAutomation RVA + 签名 | 25 RVA + 21 签名 |
| PART 6 | RTTI 地址（vtable/COL/type_descriptor） | 5 vtable + 4 COL + 3 type_descriptor |

RTTI 地址由 `rtti_scan.py` 自动定位（见第 4 步），无需 IDA/Ghidra。

**总计**：~50 个 RVA、~46 个 AOB 签名、~15 个 RTTI 地址。

## 签名的作用机制（重要）

本 MOD 中的签名校验采用 **定点 memcmp**，不是搜索：

```cpp
// nearby_chest_sort.inl 中的实现
bool NearbyCheckSignature(const char* name, uint8_t* address, 
                          const uint8_t* expected, size_t length) {
    if (memcmp(address, expected, length) != 0) {
        // 签名不匹配，校验失败
        return false;
    }
    return true;
}
```

调用方式：`NearbyCheckSignature("func", base + RVA, SIG_XXX, SIG_XXX_LEN)`

这意味着：
- **签名不需要全局唯一**——只需与预期 RVA 处的实际字节一致
- 多匹配签名在运行时完全无影响
- 唯一性要求只针对 `aob_scanner.ps1`（版本迁移搜索工具）

## 适配步骤

### 第 1 步：获取新 exe 信息

```powershell
$file = Get-Item "village.exe"
Write-Output "Size: $($file.Length)"
$sha = [System.Security.Cryptography.SHA256]::Create()
$hash = $sha.ComputeHash([System.IO.File]::ReadAllBytes($file.FullName))
Write-Output "SHA-256: $(($hash | ForEach-Object { '{0:X2}' -f $_ }) -join '')"
```

### 第 2 步：运行 AOB 扫描

```powershell
powershell -ExecutionPolicy Bypass -File aob_scanner.ps1 -ExePath "village.exe"
```

脚本会输出：
- 每个签名的匹配数和 RVA 地址
- 可直接粘贴的 `#define RVA_XXX` 片段

**结果判读**：
- **1 匹配**（绿色）：直接使用该 RVA
- **多匹配**（黄色）：需人工确认正确 RVA（对比函数特征）
- **0 匹配**（红色）：函数开头字节已变，需用 IDA/Ghidra 重新提取签名

### 第 3 步：更新 version_manifest.h

1. 修改 `SUPPORTED_EXE_SIZE`、`SUPPORTED_EXE_SHA256`、`SUPPORTED_GAME_VERSION`、`SUPPORTED_BUILD_NUMBER`
2. 用扫描结果更新所有 RVA 值
3. 对 0 匹配的函数，用 IDA/Ghidra 提取新签名并更新 `SIG_XXX` 数组和 `SIG_XXX_LEN`

### 第 4 步：更新 RTTI 地址（自动）

RTTI 地址由 `rtti_scan.py` 自动定位，无需 IDA/Ghidra：

```powershell
python .temp\rtti_scan\rtti_scan.py "village.exe"
```

脚本原理（x64 MSVC RTTI 固定结构）：
1. **type_descriptor**：搜索装饰名（如 `.?AVCGimmickStatus@@`），名字字符串往前 0x10 字节
2. **COL**：前 4 字节 signature==1 且 pTypeDescriptor(DWORD RVA) 指向已知 type_descriptor
3. **vtable**：某处 u64 == image_base + COL_RVA，且 vtable[+8]（vtable[0]）指向 .text

**已知局限**：部分类（如生产 Component）使用纯函数 vtable（vtable[0] 直接是函数、无 COL 指针），无 RTTI 元数据，脚本无法反推类型名。
这类 vtable 通过反汇编析构函数确认（`lea rax,[rip+..]` 写回 vtable 的指令，见 `.temp/rtti_scan/` 的 `rtti_reverse.py`/`disasm.py`）。

vtable/COL/type_descriptor 的更新规则：
1. **vtable**：直接使用扫描输出的 vtable RVA
2. **vtable[0]**：扫描输出的 vtable[0] 函数地址（用于校验）
3. **COL**：扫描输出的 COL RVA
4. **type_descriptor**：扫描输出的 type_descriptor RVA

### 第 5 步：编译

```powershell
& "d:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
    "ProductionAuto.vcxproj" /p:Configuration=Release /p:Platform=x64 /v:minimal /t:Rebuild
```

### 第 6 步：部署 + 测试

1. 复制 `productionauto.dll` 到 `mods\ProductionAuto_v1.0.0\`
2. 启动游戏
3. 检查 `qol_productionauto.log`，搜索 `OK` / `FAIL`
4. 按 F3 打开面板验证功能

## 签名编写规则

1. **避开位置相关字节**：
   - `E8 xx xx xx xx`（CALL rel32）—偏移随版本变化
   - `E9 xx xx xx xx`（JMP rel32）
   - `48 8D 05 xx xx xx xx`（LEA rax, [rip+disp32]）—RIP 相对
   - `48 8B 0D xx xx xx xx`（MOV rcx, [rip+disp32]）
   - 任何 `xx xx xx xx` disp32/imm32 中包含绝对地址的字节

2. **使用位置无关字节**：
   - 函数序言（push/sub rsp/mov store）
   - 寄存器操作（mov reg, reg / lea reg, [reg+disp8]）
   - 立即数（如 `B8 0x20 0x11 0x00 0x00` = mov eax, 0x1120）
   - 条件跳转的短偏移（Jcc rel8，如 `75 0B`）在函数内部相对稳定

3. **签名长度建议**：16-40 字节。太短易多匹配，太长可能包含位置相关字节

## 模板孪生函数

以下 3 组函数是 C++ 模板实例化，前 100+ 字节完全相同，无法通过签名区分：

| 函数对 | 说明 |
|--------|------|
| STATUS_INT_LOOKUP / STATUS_U64_LOOKUP | 仅模板参数类型不同 |
| STATUS_EVENT_LOOKUP | 与其他 lookup 函数共享序言 |

这些函数的签名保持短签名即可——运行时定点 memcmp 能正确校验。
版本迁移时需通过 RVA 上下文（相邻地址）或 IDA 函数名标注确认。

## PE 段映射

```
village.exe (build 24969282, v1.08.1)
.text:   VA=0x1000    RawOff=0x400     Size=0x9EAA00
.rdata:  VA=0x9EC000  RawOff=0x9EAE00  Size=0x606200
.data:   VA=0xFF3000  RawOff=0xFF1000  Size=0xCDE00
```

文件偏移 = RVA - (VA - RawOff) = RVA - 0xC00

## 配方表提取（方案 C：半自动）

`processing_recipe_table.inl` 的 455 条配方记录从 `data.dat` 自动提取生成，游戏更新后需重新执行此流程。

### data.dat 配方表格式

- **表起点**：0x738C0C（通过特征字节 `10 43 4E 0E 00 00 00 00 74 43 4E 0E 00 00 00 00` 自动定位）
- **记录大小**：108 字节/条
- **总记录数**：455 条（含 1 条无效记录，input[0]==0）

### 字段布局

| 偏移 | 类型 | 含义 |
|------|------|------|
| 0 | u32 | 104（记录头固定值） |
| 4 | u32 | outputItemId |
| 12 | u32 | inputItemId[0] |
| 16 | u32 | inputCount[0] |
| 20 | u32 | inputItemId[1] |
| 24 | u32 | inputCount[1] |
| 28 | u32 | outputCount |
| 32 | u64 | machineIds[0] |
| 40 | u64 | machineIds[1] |
| 48 | u64 | data recipeId |
| 84 | u32 | duration |
| 88..107 | — | 保留/0 |

### 生成规则

- **跳过** input[0]==0 的无效记录（仅 Record 0）
- **inl recipeId** = data 表前一条记录（含跳过的无效记录）的 u64@48
- **手工追加条目**：out=1455（data 中不存在），recipeId = data 最后一条记录的 u64@48
- 其余字段（machineIds、duration 等）取自 data **当前**记录

### 提取步骤

1. 编译 `recipe_gen.cpp`（位于 `.temp/recipe_scan/`）
2. 运行：`recipe_gen.exe <data.dat路径> 738C0C`
3. 生成器输出 `processing_recipe_table_gen2.inl`，包含完整 struct 定义和 count 常量
4. 验证：用 `recipe_cmp.exe` 或逐行对比确认条目数和字段
5. 替换 `processing_recipe_table.inl`，重新编译 ProductionAuto

### 辅助工具（位于 .temp/recipe_scan/）

| 工具 | 用途 |
|------|------|
| `recipe_scan.exe` | 搜索特征字节定位配方表起点 |
| `recipe_verify.exe` | 逐字段 dump 前几条记录验证格式 |
| `recipe_count.exe` | 统计总记录数 |
| `recipe_skip.exe` | 列出所有 input[0]==0 的无效记录 |
| `recipe_gen.exe` | **正式生成器**，输出 inl 格式配方表 |
| `recipe_cmp.exe` | 对比现有 inl 与 data 表，检查 recipeId/outputItemId 一致性 |
| `recipe_rid.exe` | 对比 data u64@48 序列与 inl recipeId 序列 |

## 文件清单

| 文件 | 用途 |
|------|------|
| `version_manifest.h` | 版本数据唯一来源（RVA + 签名 + RTTI） |
| `aob_scanner.ps1` | AOB 签名自动搜索工具 |
| `.temp/rtti_scan/rtti_scan.py` | RTTI 自动定位工具（type_descriptor/COL/vtable） |
| `.temp/rtti_scan/rtti_reverse.py` | 从已知 vtable 反推类型名（调试用） |
| `production_auto.cpp` | 主入口（mod_init / mod_tick / unload） |
| `nearby_chest_sort.inl` | 基础设施层（时钟 hook + 箱子扫描） |
| `production_automation.inl` | 核心逻辑（链式自动化） |
| `production_automation_panel.inl` | F3 面板 UI |
| `processing_recipe_table.inl` | 配方表数据（由 recipe_gen.exe 自动生成） |
| `ADAPTATION.md` | 本文件 |

---

作者：PHJ&消失的清风，转载或分享时请注明出处。

