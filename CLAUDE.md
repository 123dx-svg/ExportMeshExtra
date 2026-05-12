# CLAUDE.md

此文件为 Claude Code (claude.ai/code) 在此代码库中工作时提供指导。

## 概述

ExportMeshExtra 是一个 Unreal Engine 5.2 编辑器插件，提供网格导出工具、性能分析工具和缩略图导出功能。该插件专为 SIM (SimOne) 生态系统设计，提供自定义编辑器菜单扩展和蓝图可访问的工具函数。

## 构建系统

这是一个使用 Unreal Build Tool (UBT) 的虚幻引擎插件。当父级虚幻引擎项目编译时，插件会自动构建。

**构建插件：**
- 插件通过虚幻引擎编辑器或项目的 .sln 文件进行编译
- 构建配置由 Unreal Build Tool 使用每个模块中的 `.Build.cs` 文件管理
- 目标平台：Win64、Linux、Mac

**模块依赖：**
- ExportMeshExtra 模块依赖：Core, CoreUObject, Engine, Slate, SlateCore, MeshMergeUtilities, AssetRegistry, UnrealEd, DesktopPlatform, MeshDescription, StaticMeshDescription, EditorScriptingUtilities, Blutility, UMGEditor, ToolMenus, ContentBrowser, GLTFExporter, GeometryCore, GeometryFramework, GeometryScriptingCore, GeometryScriptingEditor
- ThumbnailExporter 模块依赖：Core, DeveloperSettings, CoreUObject, Engine, Slate, SlateCore, DesktopPlatform, ImageWrapper, UnrealEd, RHI, RenderCore

## 架构

### 双模块结构

插件由两个独立的编辑器模块组成：

**1. ExportMeshExtra 模块** (`Source/ExportMesh/`)
- 提供网格合并和导出功能
- 在关卡编辑器主菜单中注册自定义"SIM工具"菜单
- 从 Content/Tools 生成编辑器实用工具小组件，用于资产导出和性能优化
- 用于网格操作和文件对话框的蓝图函数库

**2. ThumbnailExporter 模块** (`Source/ThumbnailExporter/`)
- 第三方模块（Copyright 2023 Big Cat Energising）
- 扩展内容浏览器上下文菜单以导出资产缩略图
- 将缩略图渲染为纹理资产或本地 PNG 文件
- 通过项目设置支持可配置的导出预设
- 使用自定义渲染管线和场景捕捉来生成高质量缩略图

### 关键组件

**菜单注册** (ExportMeshExtra.cpp:37-107)
- `RegisterMenus()` 使用"SIMTools"部分扩展 `LevelEditor.MainMenu`
- 加载并生成两个编辑器实用工具小组件：
  - `/ExportMeshExtra/Tools/ExportAllAssets.ExportAllAssets` - 资产导出工具
  - `/ExportMeshExtra/Tools/PerformanceTools.PerformanceTools` - 性能优化工具

**蓝图函数库** (ExportMeshBPLibrary.h)
- `MergeActorToStaticMesh()` - 将基本组件合并为单个静态网格
- `GetComponentsFromBlueprintAsset()` - 从蓝图资产中提取组件
- `SetStaticMeshPivot()` - 使用 10 种对齐模式对齐静态网格枢轴点（不调整、底部居中、居中、左上等）
- `OpenFileDialogSelectDirectory()` / `OpenFileDialog()` - 原生文件对话框工具
- **GLB导出功能：**
  - `ExportAssetToGLB()` - 基础GLB导出（支持StaticMesh、SkeletalMesh、Blueprint）
  - `ExportAssetToGLBWithConfig()` - 便捷方法：使用单个配置导出GLB（减面、后缀、枢轴点）
  - `ExportAssetToGLBBatch()` - 批量导出：使用多个配置一次性导出多个GLB文件

**缩略图导出系统** (ThumbnailExporter.cpp)
- 通过 `OnExtendContentBrowserAssetSelectionMenu()` 扩展上下文菜单
- 基于预设的导出，具有可配置的设置（尺寸、背景、后处理、泛光、视角）
- 批量导出，具有进度跟踪和基于帧的渲染管线
- 两种导出模式：
  - 资产创建：在项目中创建 UTexture2D 资产
  - 本地导出：将 PNG 文件导出到用户选择的目录

**缩略图渲染** (ThumbnailExporterSettings.h)
- `FThumbnailCreationConfig` 结构体控制所有渲染参数
- 支持分轴自动调整大小：
  - `bAutoThumbnailSizeX`：裁剪 X 轴（左右）空白
  - `bAutoThumbnailSizeY`：裁剪 Y 轴（上下）空白
  - 只选 X：X 方向填满 ThumbnailSize 无留白，Y 方向等比缩放（不足居中、超出裁剪），强制方形输出
  - 只选 Y：Y 方向填满 ThumbnailSize 无留白，X 方向等比缩放（不足居中、超出裁剪），强制方形输出
  - 都选：完全裁剪后等比例缩放，最长边为 ThumbnailSize，输出可能非方形
- 可配置捕捉源（HDR 带反转不透明度以实现透明）
- 自定义视角覆盖（OrbitPitch, OrbitYaw）
- 半透明材质的 Alpha 去预乘
- 反射支持（玻璃/金属材质）：
  - `bEnableReflections`：启用反射、全局光照、环境反射、高光
  - `bEnableScreenSpaceReflections`：启用屏幕空间反射（需先启用 bEnableReflections）

### 内容结构

- `Content/BP/` - 蓝图资产
- `Content/Map/` - 地图资产
- `Content/Tools/` - 菜单系统引用的编辑器实用工具小组件

## 开发模式

**模块初始化：**
- 两个模块都在 `StartupModule()` 中注册编辑器扩展
- ExportMeshExtra 使用 `UToolMenus::RegisterStartupCallback()` 进行菜单注册
- ThumbnailExporter 使用 `FContentBrowserModule::GetAllAssetViewContextMenuExtenders()` 注册上下文菜单

**异步缩略图导出：**
- 批量导出使用 `FTSTicker` 进行逐帧处理
- 三阶段管线：LoadAsset → WarmupRender → Export
- 预热阶段确保纹理在最终渲染之前完全加载
- 使用 `FScopedSlowTask` 实现可取消的进度对话框

**静态网格枢轴点对齐：**
- `EPivotAlign` 枚举提供 10 种对齐模式（包括"不调整"），带有中文显示名称
- 使用网格描述 API 以新顶点位置重建静态网格

**网格简化导出（GeometryScript）：**
- 使用 GeometryScript API 进行网格简化（Standard QEM 算法）
- `SimplifyStaticMesh()` - 基于减面密度简化网格
  - SimplifyDensity (0-1)：表示要减少的三角形比例
  - 0.0 = 不减面，0.5 = 减少50%保留50%，0.8 = 减少80%保留20%
  - 保护 UV 接缝和尖锐边缘
- `AdjustStaticMeshPivot()` - 在不修改原始资产的情况下调整枢轴点
- 批量导出优化：Blueprint只需合并一次，然后应用多个配置
- 所有临时资产使用 RF_Transient 标志，导出后自动清理

**GLB 导出配置结构体 (FGLBExportConfig)：**
- `SimplifyDensity` (float 0-1)：减面密度，0.8表示减少80%的面数
- `GLBSuffix` (FString)：导出文件后缀，例如"_Low"、"_LOD1"
- `PivotAlign` (EPivotAlign)：枢轴点对齐方式
- `bEnabled` (bool)：是否启用此配置

**使用示例：**

```cpp
// 示例1：基础导出（无简化）- 使用传统UObject方式
ExportAssetToGLB(MyAsset, "D:/Exports/MyAsset.glb");

// 示例2：单个配置导出（简化80%，添加后缀，调整枢轴点）- 使用FAssetData
// 在蓝图中：使用 GetSelectedAssetData 节点获取选中的资产数据
TArray<FAssetData> SelectedAssets = UEditorUtilityLibrary::GetSelectedAssetData();
if (SelectedAssets.Num() > 0)
{
    ExportAssetToGLBWithConfig(SelectedAssets[0], "D:/Exports/", 0.8, "_Low", EPivotAlign::BottomCenter);
    // 输出：D:/Exports/MyAsset_Low.glb（保留20%的三角形，枢轴点在底部居中）
}

// 示例3：批量导出多个LOD版本
TArray<FGLBExportConfig> Configs;
Configs.Add(FGLBExportConfig(0.0, "", EPivotAlign::None));              // 原始版本
Configs.Add(FGLBExportConfig(0.5, "_LOD1", EPivotAlign::BottomCenter)); // LOD1: 减少50%
Configs.Add(FGLBExportConfig(0.75, "_LOD2", EPivotAlign::BottomCenter));// LOD2: 减少75%
Configs.Add(FGLBExportConfig(0.9, "_LOD3", EPivotAlign::BottomCenter)); // LOD3: 减少90%

TArray<FAssetData> SelectedAssets = UEditorUtilityLibrary::GetSelectedAssetData();
if (SelectedAssets.Num() > 0)
{
    ExportAssetToGLBBatch(SelectedAssets[0], "D:/Exports/MyAsset.glb", Configs);
    // 输出：
    // - D:/Exports/MyAsset.glb (100% 三角形)
    // - D:/Exports/MyAsset_LOD1.glb (50% 三角形)
    // - D:/Exports/MyAsset_LOD2.glb (25% 三角形)
    // - D:/Exports/MyAsset_LOD3.glb (10% 三角形)
}

// 示例4：批量处理多个资产
TArray<FAssetData> SelectedAssets = UEditorUtilityLibrary::GetSelectedAssetData();
for (const FAssetData& AssetData : SelectedAssets)
{
    TArray<FGLBExportConfig> Configs;
    Configs.Add(FGLBExportConfig(0.0, ""));
    Configs.Add(FGLBExportConfig(0.8, "_Low"));
    ExportAssetToGLBBatch(AssetData, "D:/Exports/", Configs);
}
```

**蓝图工作流优势：**

使用 `FAssetData` 参数的新接口与蓝图编辑器工作流更加契合：
1. 无需手动类型转换：直接使用 `GetSelectedAssetData` 的返回值
2. 自动类型识别：函数内部根据 AssetClassPath 自动判断是 StaticMesh、SkeletalMesh 还是 Blueprint
3. 延迟加载：只有在需要时才加载资产，提高性能
4. 更好的错误提示：在加载失败时提供明确的错误信息

**接口对比：**

旧方式（需要类型转换）：
```
GetSelectedAssetsOfClass(StaticMesh) → Cast → ExportAssetToGLBBatch
```

新方式（直接使用）：
```
GetSelectedAssetData → ExportAssetToGLBBatch (自动识别类型)
```

**文件命名规则：**
- 如果 OutputPath 是目录（无扩展名）：`Directory/AssetName[Suffix].glb`
- 如果 OutputPath 是完整文件路径（.glb）：`Directory/BaseFileName[Suffix].glb`
- 如果 OutputPath 有其他扩展名：替换为 `.glb`

**性能建议：**
- 使用 `ExportAssetToGLBBatch` 批量导出多个版本，性能优于多次调用单个导出
- Blueprint 只合并一次，然后复用于所有配置
- 临时资产自动清理，无需手动管理内存

**已知限制：**
- SkeletalMesh 不支持简化和枢轴点调整（降级为原始导出）
- 极端简化（> 95%）可能导致网格退化
- 简化会重新计算法线和切线，可能导致轻微的视觉差异

**GLB材质限制（重要）：**
GLB是为Web/跨平台设计的简化3D格式，不支持UE的高级材质特性。以下材质类型在导出后可能显示不正确：

- **Thin Translucent（薄半透明）**：玻璃、灯罩等透明材质
  - 症状：在外部查看器中显示为粉红色或不透明
  - 原因：GLB不支持该着色模型，降级为Default Lit后烘焙可能失败

- **Clear Coat（清漆涂层）**：车漆、湿润表面等
  - 症状：失去双层反射效果
  - 原因：GLB不支持Clear Coat，仅导出基础层

- **自定义着色器/复杂材质图**：
  - 症状：材质外观简化或错误
  - 原因：GLB仅支持基础PBR材质（BaseColor, Metallic, Roughness, Normal, Emissive）

**最佳实践建议：**

1. **简单材质导出最可靠**
   - BaseColor + Normal + Roughness/Metallic 的标准PBR材质导出效果最好
   - 避免使用Material Instance的复杂参数

2. **为导出创建简化材质**（推荐用于生产）
   ```
   方法：
   1. 在Blueprint中为透明/特殊效果组件创建第二套材质
   2. 使用简单的Opaque或Masked材质替代Thin Translucent
   3. 使用标准PBR材质替代Clear Coat
   4. 导出前临时切换到简化材质
   ```

3. **简化导出的特殊考虑**
   - 网格简化后UV可能轻微变形，增加材质烘焙失败的风险
   - 未简化版本材质正常是预期行为
   - 如果必须使用复杂材质，使用未简化版本（SimplifyDensity=0.0）

4. **替代方案**
   - 如果目标应用支持，考虑使用FBX格式（对UE材质支持更好）
   - 对于SimOne等仿真环境，确认其GLB材质支持范围

**材质导出兼容性矩阵：**

| 材质类型 | 未简化导出 | 简化导出 | 备注 |
|---------|-----------|---------|------|
| Default Lit（标准PBR） | ✅ 完全支持 | ✅ 完全支持 | 推荐 |
| Masked（遮罩） | ✅ 完全支持 | ⚠️ 部分支持 | UV变形可能影响遮罩 |
| Translucent（半透明） | ⚠️ 部分支持 | ❌ 可能失败 | 可能显示为粉色 |
| Thin Translucent | ⚠️ 降级为Default Lit | ❌ 烘焙可能失败 | 玻璃建议用简化材质 |
| Clear Coat | ⚠️ 失去清漆层 | ❌ 烘焙可能失败 | 车漆建议用标准PBR |
| Subsurface | ⚠️ 降级为Default Lit | ❌ 不支持 | 皮肤等材质 |
| Material Instance | ✅ 尝试烘焙 | ⚠️ 可能失败 | 建议用基础材质 |

**故障排除：**

如果导出的模型有粉红色材质：
1. 检查UE编辑器中该材质的着色模型（Shading Model）
2. 如果是Thin Translucent或Clear Coat，这是已知限制
3. 选项A：接受限制（粉红色部分通常是玻璃/透明件）
4. 选项B：为这些组件创建简化的Opaque材质用于导出
5. 选项C：仅导出未简化版本（SimplifyDensity=0.0）

**日志关键词：**
```
LogGLTFExporter: Warning: Unsupported shading model (XXX) in material YYY, will export as Default Lit
→ 该材质使用了不支持的着色模型，已降级

LogGLTFExporter: Warning: Material XXX won't be exported with clear coat bottom normal
→ Clear Coat材质的双层效果将丢失

LogMaterialBaking: Verbose: Performing material baking for 1 materials
→ 材质正在被烘焙到纹理
```

## 插件配置

**插件定义** (ExportMeshExtra.uplugin)
- 版本：1.7.0
- 引擎版本：5.2.0
- 需要 GeometryScripting 插件
- 两个模块都是仅编辑器模式，LoadingPhase 为 "Default"

**缩略图导出器设置：**
- 通过项目设置 → Thumbnail Exporter 访问
- 定义具有不同配置的多个导出预设
- 每个预设在内容浏览器上下文菜单中显示为单独的菜单项

## 重要说明

- 这是一个中文语言项目（菜单标签、工具提示、枚举显示名称均为中文）
- ExportMeshExtra 模块通过 `/ExportMeshExtra/Tools/` 中的硬编码路径引用资产
- ThumbnailExporter 使用为 `UThumbnailExporterThumbnailDummy` 类注册的自定义缩略图渲染器
- 本地缩略图导出完成后会自动打开导出文件夹
- 该插件扩展了虚幻引擎的缩略图系统，而不是替换内置功能
