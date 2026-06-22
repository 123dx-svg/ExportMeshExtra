// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "ExportMeshBPLibrary.generated.h"

class UStaticMesh;
class USkeletalMesh;

UENUM(BlueprintType)
enum class EPivotAlign : uint8
{
	None UMETA(DisplayName = "None (不调整)"),
	BottomCenter UMETA(DisplayName = "Bottom Center (底部居中)"),
	BottomRight UMETA(DisplayName = "Bottom Right (底部右对齐)"),
	BottomLeft UMETA(DisplayName = "Bottom Left (底部左对齐)"),
	Center UMETA(DisplayName = "Center (居中)"),
	CenterLeft UMETA(DisplayName = "Center Left (左对齐)"),
	CenterRight UMETA(DisplayName = "Center Right (右对齐)"),
	TopCenter UMETA(DisplayName = "Top Center (顶部居中)"),
	TopRight UMETA(DisplayName = "Top Right (顶部右对齐)"),
	TopLeft UMETA(DisplayName = "Top Left (顶部左对齐)")
};

// GLB导出配置结构体
USTRUCT(BlueprintType)
struct FGLBExportConfig
{
	GENERATED_BODY()

	// 减面密度 (0.0-1.0)，表示要减少的面数比例
	// 例如：0.8 = 减少80%的面（保留20%），0.0 = 不减面，1.0 = 减少100%（最小化）
	UPROPERTY(BlueprintReadWrite, Category = "Export")
	float SimplifyDensity = 0.0f;

	// 导出文件的后缀名（例如："_Low"、"_LOD1"）
	UPROPERTY(BlueprintReadWrite, Category = "Export")
	FString GLBSuffix = TEXT("");

	// 枢轴点对齐方式
	UPROPERTY(BlueprintReadWrite, Category = "Export")
	EPivotAlign PivotAlign = EPivotAlign::None;

	// 是否启用此配置
	UPROPERTY(BlueprintReadWrite, Category = "Export")
	bool bEnabled = true;

	FGLBExportConfig() {}

	FGLBExportConfig(float InSimplifyDensity, const FString& InSuffix, EPivotAlign InPivotAlign = EPivotAlign::None)
		: SimplifyDensity(InSimplifyDensity)
		, GLBSuffix(InSuffix)
		, PivotAlign(InPivotAlign)
		, bEnabled(true)
	{}
};

/* 
*	Function library class.
*	Each function in it is expected to be static and represents blueprint node that can be called in any blueprint.
*
*	When declaring function you can define metadata for the node. Key function specifiers will be BlueprintPure and BlueprintCallable.
*	BlueprintPure - means the function does not affect the owning object in any way and thus creates a node without Exec pins.
*	BlueprintCallable - makes a function which can be executed in Blueprints - Thus it has Exec pins.
*	DisplayName - full name of the node, shown when you mouse over the node and in the blueprint drop down menu.
*				Its lets you name the node using characters not allowed in C++ function names.
*	CompactNodeTitle - the word(s) that appear on the node.
*	Keywords -	the list of keywords that helps you to find node when you search for it using Blueprint drop-down menu. 
*				Good example is "Print String" node which you can find also by using keyword "log".
*	Category -	the category your node will be under in the Blueprint drop-down menu.
*
*	For more info on custom blueprint nodes visit documentation:
*	https://wiki.unrealengine.com/Custom_Blueprint_Node_Creation
*/
UCLASS()
class UExportMeshBPLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_UCLASS_BODY()
	
public:
	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra")
	static void MergeActorToStaticMesh(const FString& InBasePackageName,const TArray<UPrimitiveComponent*>& ComponentsToMerge,TArray<UObject*>& AssetsRef,bool GenerateLightMapUV = false);

	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra")
	static void GetComponentsFromBlueprintAsset(const UBlueprint* Blueprint, TArray<UPrimitiveComponent*>& OutComponents);

	// 将StaticMesh枢轴移动到指定位置（按包围盒对齐）
	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra|StaticMesh")
	static bool SetStaticMeshPivot(UStaticMesh* StaticMesh, EPivotAlign Align);
	

	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra|FileDialog")
	static FString OpenFileDialogWithAssetName();

	// 仅返回用户选择的文件夹路径，不拼接资产名（用于批量导出的根目录选择）
	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra|FileDialog")
	static FString OpenExportFolderDialog();

	// 导出资产到本地 GLB 格式
	// 支持：StaticMesh、SkeletalMesh、包含StaticMesh的Blueprint
	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra|Export")
	static bool ExportAssetToGLB(UObject* Asset, const FString& OutputPath);

	// 批量导出资产到GLB格式，支持多种配置（减面、后缀、枢轴点）
	// AssetData: 资产数据（可直接使用GetSelectedAssetData返回值）
	// ExportConfigs: 导出配置数组，每个配置将生成一个对应的GLB文件
	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra|Export", meta = (AutoCreateRefTerm = "AssetData"))
	static bool ExportAssetToGLBBatch(const FAssetData& AssetData, const FString& OutputPath, const TArray<FGLBExportConfig>& ExportConfigs);

	// 便捷方法：导出单个配置的GLB
	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra|Export", meta = (AutoCreateRefTerm = "AssetData"))
	static bool ExportAssetToGLBWithConfig(
		const FAssetData& AssetData,
		const FString& OutputPath,
		float SimplifyDensity = 0.0f,
		const FString& GLBSuffix = TEXT(""),
		EPivotAlign PivotAlign = EPivotAlign::None
	);

	// 将文件夹内所有文件压缩为zip包（zip包生成在文件夹内，原文件保留）
	// 示例：CompressDirectoryToZip("F:/SimForArt/Output/BP_WCLW_P7000", OutZipPath)
	// 将压缩为：F:/SimForArt/Output/BP_WCLW_P7000/BP_WCLW_P7000.zip
	// 原文件保留不删除，方便预览
	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra|FileSystem")
	static bool CompressDirectoryToZip(const FString& DirectoryPath, FString& OutZipPath, bool bOpenFolderAfterZip = true);

	// 获取资产包围盒尺寸（单位：厘米），统一支持 StaticMesh / SkeletalMesh / Blueprint
	// Length=X, Width=Y, Height=Z（完整尺寸，非半尺寸）
	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra|Bounds", meta = (AutoCreateRefTerm = "AssetData"))
	static bool GetAssetBoundingBoxSize(
		const FAssetData& AssetData,
		float& Length,
		float& Width,
		float& Height
	);

	// 为选中的资产生成 external_agents.json（写入到 OutputDirectory）
	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra|Export", meta = (AutoCreateRefTerm = "SelectedAssets"))
	static bool GenerateExternalAgentsJson(
		const TArray<FAssetData>& SelectedAssets,
		const FString& OutputDirectory,
		const FString& OscSubCategory = TEXT("car"),
		const FString& SemanticsType = TEXT("Unknown")
	);

	// 获取导出时间戳字符串，格式 %Y_%m%d_%H%M%S（例如 2026_0622_151100）
	UFUNCTION(BlueprintPure, Category = "ExportMeshExtra|Export")
	static FString GetExportTimestamp();

	// 为单个资产构建并创建导出目录：{BaseDir}/{资产名}/{Timestamp}
	// 若 Timestamp 为空则内部自动生成。OutDir 返回最终绝对/完整目录路径。
	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra|Export", meta = (AutoCreateRefTerm = "AssetData"))
	static bool MakeAssetExportDir(
		const FString& BaseDir,
		const FAssetData& AssetData,
		const FString& Timestamp,
		FString& OutDir
	);

	// 在系统文件管理器中打开指定文件夹
	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra|FileSystem")
	static void OpenFolderInExplorer(const FString& FolderPath);

	// 从Blueprint资产中获取所有使用指定父材质的材质实例
	// AssetData: 要检查的资产数据（仅对Blueprint资产生效）
	// ParentMaterial: 父材质引用（用于筛选）
	// OutMaterialInstances: 输出符合条件的材质实例数组（自动去重）
	// 注意：此函数会在场景中生成临时Actor并在完成后自动清理
	// 示例：获取所有使用M_Car_Master作为父材质的材质实例
	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra|Material", meta = (AutoCreateRefTerm = "AssetData"))
	static void GetMaterialInstancesFromBlueprintByParent(
		const FAssetData& AssetData,
		UMaterialInterface* ParentMaterial,
		TArray<UMaterialInstance*>& OutMaterialInstances
	);

	// 批量替换材质实例的Parent材质
	// MaterialInstances: 要修改的材质实例数组
	// NewParentMaterial: 新的父材质引用
	// 返回：成功替换的材质实例数量
	// 示例：将所有材质实例的Parent从M_Old_Master替换为M_New_Master
	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra|Material")
	static int32 SetMaterialInstancesParent(
		const TArray<UMaterialInstance*>& MaterialInstances,
		UMaterialInterface* NewParentMaterial
	);

private:
	// 计算输出路径（添加后缀）
	static FString ComputeOutputPath(const FString& BasePath, const FString& AssetName, const FString& Suffix);

	// 简化StaticMesh
	static UStaticMesh* SimplifyStaticMesh(UStaticMesh* SourceMesh, float SimplifyDensity, TArray<UObject*>& OutTempAssets);

	// 调整StaticMesh枢轴点（内部使用版本，不修改原始资产）
	static UStaticMesh* AdjustStaticMeshPivot(UStaticMesh* SourceMesh, EPivotAlign Align, TArray<UObject*>& OutTempAssets);

	// 清理临时资产
	static void CleanupTempAssets(TArray<UObject*>& TempAssets);

	// 清理临时Actor
	static void CleanupTempActors();

	// 计算StaticMesh的包围盒
	static FBox CalculateStaticMeshBounds(UStaticMesh* Mesh);

	// 计算Blueprint中所有Mesh组件的合并包围盒（支持StaticMesh和SkeletalMesh）
	static FBox CalculateBlueprintMeshBounds(const TArray<UPrimitiveComponent*>& MeshComponents);

	// 生成manifest.json文件
	static bool GenerateManifestJson(
		const FString& OutputDirectory,
		const FString& AssetName,
		const FString& AssetPath,
		const FBox& MeshBounds,
		const TArray<FGLBExportConfig>& ExportConfigs
	);

	// 将SkeletalMesh转换为StaticMesh（提取LOD0几何数据）
	static UStaticMesh* ConvertSkeletalMeshToStaticMesh(USkeletalMesh* SkelMesh);

	// 将组件数组中的SkeletalMeshComponent转换为等效的StaticMeshComponent（就地替换）
	// 返回值：转换过程中生成的临时资产列表，调用者负责清理
	static void ConvertSkeletalComponentsForMerge(TArray<UPrimitiveComponent*>& Components, TArray<UObject*>& OutTempAssets);
};


