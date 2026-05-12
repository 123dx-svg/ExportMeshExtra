# ExportMeshExtra

Unreal Engine 5.2 编辑器插件，为 SimOne 生态系统提供网格导出、性能分析和缩略图导出功能。

## 功能概览

### ExportMeshExtra 模块
- **GLB 批量导出** — 支持 StaticMesh、SkeletalMesh、Blueprint 资产导出为 GLB 格式
- **网格简化** — 基于 GeometryScript 的 QEM 算法，支持多 LOD 批量导出
- **枢轴点对齐** — 10 种对齐模式（底部居中、居中、左上等）
- **网格合并** — 将多个基本组件合并为单个 StaticMesh
- **SIM 工具菜单** — 在关卡编辑器主菜单注册自定义工具入口

### ThumbnailExporter 模块
- **缩略图导出** — 将资产缩略图渲染为 PNG 文件或 UTexture2D 资产
- **批量处理** — 支持多选资产批量导出，带进度跟踪
- **自定义预设** — 可配置尺寸、背景、后处理、泛光、视角等参数
- **自动裁剪** — 分轴自动裁剪空白区域（X/Y 轴独立控制）
- **反射支持** — 玻璃/金属材质的反射和屏幕空间反射

## 安装

1. 将 `ExportMeshExtra` 文件夹复制到项目的 `Plugins/` 目录
2. 确保已启用 `GeometryScripting` 插件（依赖项）
3. 重新编译项目或重启编辑器

**预编译版本**：在 [Releases](../../releases) 页面下载对应引擎版本的打包插件，解压到 `Plugins/` 目录即可使用。

## 快速上手

### GLB 导出（蓝图）

```
// 单个资产导出
SelectedAssets = GetSelectedAssetData()
ExportAssetToGLBWithConfig(SelectedAssets[0], "D:/Exports/", 0.8, "_Low", BottomCenter)

// 批量多 LOD 导出
Configs:
  - (0.0,  "",      None)         → 原始版本
  - (0.5,  "_LOD1", BottomCenter) → 保留 50%
  - (0.75, "_LOD2", BottomCenter) → 保留 25%
  - (0.9,  "_LOD3", BottomCenter) → 保留 10%

ExportAssetToGLBBatch(AssetData, "D:/Exports/Model.glb", Configs)
```

### 缩略图导出

在内容浏览器中右键选中的资产 → 选择缩略图导出预设即可。  
预设配置路径：项目设置 → Thumbnail Exporter

## GLB 材质兼容性

| 材质类型 | 未简化导出 | 简化导出 |
|---------|:---------:|:-------:|
| Default Lit（标准 PBR） | ✅ | ✅ |
| Masked（遮罩） | ✅ | ⚠️ |
| Translucent（半透明） | ⚠️ | ❌ |
| Thin Translucent | ⚠️ | ❌ |
| Clear Coat（清漆） | ⚠️ | ❌ |

> 标准 PBR 材质（BaseColor + Normal + Roughness/Metallic）导出效果最佳。  
> 透明/特殊材质建议创建简化替代材质用于导出。

## 环境要求

- Unreal Engine 5.2
- 依赖插件：GeometryScripting
- 平台：Win64 / Linux / Mac（仅编辑器）

## 参与贡献

欢迎提交 Pull Request。所有 PR 需要通过代码审查后方可合并。

## 相关文档

- [使用文档](https://doc.weixin.qq.com/doc/w3_AeYAlAa7AOQCNukkUnFt8Szarf0v4?scode=ACQA0Qe1ABEH0tU95AeYAlAa7AOQ)

## 许可证

本项目仅供内部使用。ThumbnailExporter 模块版权归 Big Cat Energising (2023) 所有。
