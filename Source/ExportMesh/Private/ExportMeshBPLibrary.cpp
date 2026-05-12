// Copyright Epic Games, Inc. All Rights Reserved.

#include "ExportMeshBPLibrary.h"

#include "ComponentReregisterContext.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "IDesktopPlatform.h"
#include "IMeshMergeUtilities.h"
#include "MeshMergeModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Widgets/SWindow.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshAttributes.h"
#include "Components/StaticMeshComponent.h"
#include "StaticMeshCompiler.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Components/SkeletalMeshComponent.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Exporters/Exporter.h"
#include "AssetExportTask.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Camera/CameraComponent.h"
#include "CineCameraComponent.h"

// GeometryScript headers for mesh simplification
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshSimplifyFunctions.h"

// For ZIP compression
#include "Misc/FileHelper.h"
#include "HAL/PlatformProcess.h"

// For Material Instance checking
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"

// For rendering commands
#include "RenderingThread.h"
#include "RenderCommandFence.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "UDynamicMesh.h"

// Json headers
#include "CineCameraComponent.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"

UExportMeshBPLibrary::UExportMeshBPLibrary(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{

}


void UExportMeshBPLibrary::MergeActorToStaticMesh(const FString& InBasePackageName,
	const TArray<UPrimitiveComponent*>& ComponentsToMerge, TArray<UObject*>& AssetsRef,bool GenerateLightMapUV)
{
	AssetsRef.Reset();
	if (InBasePackageName.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("MergeActorToStaticMesh: InBasePackageName is empty."));
		return;
	}

	//确认模块是否加载成功
	const IMeshMergeUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshMergeModule>("MeshMergeUtilities").GetUtilities();
	FVector MergedActorLocation;
	FMeshMergingSettings InSettings;

	// 基础设置
	InSettings.bGenerateLightMapUV = GenerateLightMapUV;
	InSettings.bBakeVertexDataToMesh = true;
	InSettings.bMergeMaterials = false;  // 不合并材质，保留原始材质
	InSettings.bMergePhysicsData = false;
	InSettings.bComputedLightMapResolution = false;

	// 禁用可能导致问题的功能
	InSettings.bUseLandscapeCulling = false;
	InSettings.bIncludeImposters = false;
	InSettings.bSupportRayTracing = false;
	InSettings.bAllowDistanceField = false;

	// LOD 设置
	InSettings.LODSelectionType = EMeshLODSelectionType::AllLODs;

	UE_LOG(LogTemp, Log, TEXT("MergeActorToStaticMesh: Starting merge with %d components to package: %s"),
		ComponentsToMerge.Num(), *InBasePackageName);
	

	if (ComponentsToMerge.Num())
	{
		TArray<UPrimitiveComponent*> ValidComponents;
		ValidComponents.Reserve(ComponentsToMerge.Num());
		UWorld* World = nullptr;
		for (UPrimitiveComponent* Component : ComponentsToMerge)
		{
			if (!IsValid(Component))
			{
				UE_LOG(LogTemp, Warning, TEXT("MergeActorToStaticMesh: Skipped invalid component."));
				continue;
			}

			// 检查是否为 StaticMeshComponent（SkeletalMesh 应已在调用前转换为 StaticMesh）
			UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(Component);
			if (!StaticMeshComp)
			{
				UE_LOG(LogTemp, Warning, TEXT("MergeActorToStaticMesh: Skipped non-StaticMeshComponent: %s (%s). SkeletalMesh should be converted before merge."),
					*Component->GetName(), *Component->GetClass()->GetName());
				continue;
			}

			// 验证 StaticMesh 是否有效
			UStaticMesh* StaticMesh = StaticMeshComp->GetStaticMesh();
			if (!StaticMesh || !IsValid(StaticMesh))
			{
				UE_LOG(LogTemp, Warning, TEXT("MergeActorToStaticMesh: Skipped component with invalid StaticMesh: %s"), *Component->GetName());
				continue;
			}

			// 验证 StaticMesh 是否有有效的渲染数据
			if (StaticMesh->GetNumLODs() == 0)
			{
				UE_LOG(LogTemp, Warning, TEXT("MergeActorToStaticMesh: Skipped component with empty LOD data: %s (Mesh: %s)"),
					*Component->GetName(), *StaticMesh->GetName());
				continue;
			}

			// 等待 StaticMesh 编译完成
			if (StaticMesh->IsCompiling())
			{
				UE_LOG(LogTemp, Log, TEXT("MergeActorToStaticMesh: Waiting for StaticMesh compilation: %s"), *StaticMesh->GetName());
				FStaticMeshCompilingManager::Get().FinishCompilation({StaticMesh});
			}

			UWorld* ComponentWorld = Component->GetWorld();
			if (!ComponentWorld)
			{
				UE_LOG(LogTemp, Warning, TEXT("MergeActorToStaticMesh: Skipped component with no world: %s"), *Component->GetName());
				continue;
			}
			if (!World)
			{
				World = ComponentWorld;
			}
			if (ComponentWorld != World)
			{
				UE_LOG(LogTemp, Warning, TEXT("MergeActorToStaticMesh: Skipped component from different world: %s"), *Component->GetName());
				continue;
			}

			ValidComponents.Add(Component);
			UE_LOG(LogTemp, Log, TEXT("MergeActorToStaticMesh: Added valid component: %s"), *Component->GetName());
		}

		if (!World || ValidComponents.Num() == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("MergeActorToStaticMesh: No valid components to merge."));
			return;
		}

		checkf(World != nullptr, TEXT("Invalid World retrieved from Mesh components"));
		const float ScreenAreaSize = TNumericLimits<float>::Max();
		TArray<UObject*> MergeAssets;

		UE_LOG(LogTemp, Log, TEXT("MergeActorToStaticMesh: Calling MergeComponentsToStaticMesh with %d valid components"), ValidComponents.Num());

		// If the merge destination package already exists, it is possible that the mesh is already used in a scene somewhere, or its materials or even just its textures.
		// Static primitives uniform buffers could become invalid after the operation completes and lead to memory corruption. To avoid it, we force a global reregister.
		bool bPackageExists = FindObject<UObject>(nullptr, *InBasePackageName) != nullptr;
		UE_LOG(LogTemp, Log, TEXT("MergeActorToStaticMesh: Package exists: %s"), bPackageExists ? TEXT("true") : TEXT("false"));

		// 使用 silent build 避免立即触发可能导致崩溃的 PostEditChange/Build
		// 我们将在稍后手动安全地构建
		const bool bSilent = true;

		if (bPackageExists)
		{
			FGlobalComponentReregisterContext GlobalReregister;
			MeshUtilities.MergeComponentsToStaticMesh(ValidComponents, World, InSettings, nullptr, nullptr, InBasePackageName, MergeAssets, MergedActorLocation, ScreenAreaSize, bSilent);
		}
		else
		{
			MeshUtilities.MergeComponentsToStaticMesh(ValidComponents, World, InSettings, nullptr, nullptr, InBasePackageName, MergeAssets, MergedActorLocation, ScreenAreaSize, bSilent);
		}

		UE_LOG(LogTemp, Log, TEXT("MergeActorToStaticMesh: MergeComponentsToStaticMesh completed, created %d assets"), MergeAssets.Num());

		for (UObject* Asset : MergeAssets)
		{
			if (IsValid(Asset))
			{
				// 检查是否为 StaticMesh 并确保其有效性
				if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
				{
					UE_LOG(LogTemp, Log, TEXT("MergeActorToStaticMesh: Processing merged StaticMesh: %s"), *StaticMesh->GetName());

					// 验证 MeshDescription 是否存在
					if (StaticMesh->GetNumSourceModels() > 0)
					{
						FMeshDescription* MeshDesc = StaticMesh->GetMeshDescription(0);
						if (MeshDesc && MeshDesc->Vertices().Num() > 0)
						{
							UE_LOG(LogTemp, Log, TEXT("MergeActorToStaticMesh: StaticMesh has valid mesh description with %d vertices"), MeshDesc->Vertices().Num());

							// 手动构建 StaticMesh（silent 模式下不会自动构建）
							try
							{
								StaticMesh->Build(true); // true = silent build
								UE_LOG(LogTemp, Log, TEXT("MergeActorToStaticMesh: StaticMesh built successfully"));

								// 等待编译完成
								if (StaticMesh->IsCompiling())
								{
									FStaticMeshCompilingManager::Get().FinishCompilation({StaticMesh});
								}

								AssetsRef.Add(Asset);
								UE_LOG(LogTemp, Log, TEXT("MergeActorToStaticMesh: Successfully created StaticMesh: %s"), *StaticMesh->GetName());
							}
							catch (...)
							{
								UE_LOG(LogTemp, Error, TEXT("MergeActorToStaticMesh: Exception during StaticMesh build: %s"), *StaticMesh->GetName());
							}
						}
						else
						{
							UE_LOG(LogTemp, Warning, TEXT("MergeActorToStaticMesh: StaticMesh has empty mesh description: %s"), *StaticMesh->GetName());
						}
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("MergeActorToStaticMesh: StaticMesh has no source models: %s"), *StaticMesh->GetName());
					}
				}
				else
				{
					AssetsRef.Add(Asset);
				}
			}
		}
	}

	if (AssetsRef.Num())
	{
		FAssetRegistryModule& AssetRegistry = FModuleManager::Get().LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		int32 AssetCount = AssetsRef.Num();
		for (int32 AssetIndex = 0; AssetIndex < AssetCount; AssetIndex++)
		{
			UObject* Asset = AssetsRef[AssetIndex];
			if (!IsValid(Asset))
			{
				continue;
			}
			AssetRegistry.AssetCreated(Asset);
			if (GEditor)
			{
				GEditor->BroadcastObjectReimported(Asset);
			}
		}
	
		//Also notify the content browser that the new assets exists
		//内容栏中选中该资产
		// FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
		// ContentBrowserModule.Get().SyncBrowserToAssets(AssetsRef, true);

		//移除临时Actor
		//TempActor->Destroy();
	}
}

void UExportMeshBPLibrary::GetComponentsFromBlueprintAsset(const UBlueprint* Blueprint,
	TArray<UPrimitiveComponent*>& OutComponents)
{
	OutComponents.Reset();


	if (!IsValid(Blueprint))
	{
		UE_LOG(LogTemp, Warning, TEXT("GetComponentsFromBlueprintAsset: Blueprint is invalid."));
		return;
	}

	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	if (!GeneratedClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("GetComponentsFromBlueprintAsset: Failed to get blueprint generated class."));
		return;
	}

	// 创建临时Actor实例
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* TempActor = GEditor->GetEditorWorldContext().World()->SpawnActor<AActor>(GeneratedClass, SpawnParams);
	if (!IsValid(TempActor))
	{
		UE_LOG(LogTemp, Warning, TEXT("GetComponentsFromBlueprintAsset: Failed to spawn temp actor."));
		return;
	}

	TempActor->Tags.AddUnique("PythonTemp");

	// 获取所有实例化后的组件
	TArray<UActorComponent*> InstanceComponents;
	TempActor->GetComponents(InstanceComponents);

	// 遍历所有组件，收集非空的静态网格体组件和骨骼网格体组件（跳过Camera/CineCamera及其子组件）
	for (UActorComponent* Component : InstanceComponents)
	{
		UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(Component);
		if (!PrimComp)
		{
			continue;
		}

		// 判断是否为有效的 Mesh 组件
		bool bIsValidMesh = false;
		FString MeshName;

		if (UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(PrimComp))
		{
			if (IsValid(StaticMeshComp->GetStaticMesh()))
			{
				bIsValidMesh = true;
				MeshName = StaticMeshComp->GetStaticMesh()->GetName();
			}
		}
		else if (USkeletalMeshComponent* SkelMeshComp = Cast<USkeletalMeshComponent>(PrimComp))
		{
			if (IsValid(SkelMeshComp->GetSkeletalMeshAsset()))
			{
				bIsValidMesh = true;
				MeshName = SkelMeshComp->GetSkeletalMeshAsset()->GetName();
			}
		}

		if (!bIsValidMesh)
		{
			continue;
		}

		// 检查该组件或其任何父组件是否为Camera/CineCamera
		bool bIsCameraRelated = false;
		USceneComponent* Parent = PrimComp->GetAttachParent();
		while (Parent)
		{
			if (Parent->IsA<UCameraComponent>() || Parent->IsA<UCineCameraComponent>())
			{
				bIsCameraRelated = true;
				break;
			}
			Parent = Parent->GetAttachParent();
		}

		if (bIsCameraRelated)
		{
			UE_LOG(LogTemp, Log, TEXT("GetComponentsFromBlueprintAsset: Skipping camera-related mesh component '%s' (mesh: '%s')"),
				*PrimComp->GetName(), *MeshName);
			continue;
		}

		OutComponents.Add(PrimComp);
		UE_LOG(LogTemp, Log, TEXT("GetComponentsFromBlueprintAsset: Found mesh component '%s' with mesh '%s'"),
			*PrimComp->GetName(), *MeshName);
	}

	UE_LOG(LogTemp, Log, TEXT("GetComponentsFromBlueprintAsset: Collected %d mesh components (StaticMesh + SkeletalMesh)."), OutComponents.Num());

	// 注意：临时Actor未销毁，根据需求决定是否调用 TempActor->Destroy();
}

bool UExportMeshBPLibrary::SetStaticMeshPivot(UStaticMesh* StaticMesh, EPivotAlign Align)
{
#if WITH_EDITOR
	if (!IsValid(StaticMesh))
	{
		return false;
	}

	const int32 NumLODs = StaticMesh->GetNumSourceModels();
	if (NumLODs <= 0)
	{
		return false;
	}

	FMeshDescription* BaseMeshDesc = StaticMesh->GetMeshDescription(0);
	if (!BaseMeshDesc)
	{
		return false;
	}

	FStaticMeshAttributes BaseAttributes(*BaseMeshDesc);
	const TVertexAttributesRef<FVector3f> BasePositions = BaseAttributes.GetVertexPositions();
	if (BaseMeshDesc->Vertices().Num() == 0)
	{
		return false;
	}

	FBox Bounds(ForceInit);
	for (const FVertexID VertexID : BaseMeshDesc->Vertices().GetElementIDs())
	{
		Bounds += FVector(BasePositions[VertexID]);
	}

	if (!Bounds.IsValid)
	{
		return false;
	}

	const FVector Min = Bounds.Min;
	const FVector Max = Bounds.Max;

	// X轴保持居中，左右对齐使用Y轴，顶部/底部使用Z轴
	float PivotX = (Min.X + Max.X) * 0.5f;
	float PivotY = (Min.Y + Max.Y) * 0.5f;
	float PivotZ = (Min.Z + Max.Z) * 0.5f;

	switch (Align)
	{
	case EPivotAlign::BottomCenter:
		PivotZ = Min.Z;
		break;
	case EPivotAlign::BottomRight:
		PivotZ = Min.Z;
		PivotY = Max.Y;
		break;
	case EPivotAlign::BottomLeft:
		PivotZ = Min.Z;
		PivotY = Min.Y;
		break;
	case EPivotAlign::Center:
		break;
	case EPivotAlign::CenterLeft:
		PivotY = Min.Y;
		break;
	case EPivotAlign::CenterRight:
		PivotY = Max.Y;
		break;
	case EPivotAlign::TopCenter:
		PivotZ = Max.Z;
		break;
	case EPivotAlign::TopRight:
		PivotZ = Max.Z;
		PivotY = Max.Y;
		break;
	case EPivotAlign::TopLeft:
		PivotZ = Max.Z;
		PivotY = Min.Y;
		break;
	default:
		break;
	}

	const FVector Offset = -FVector(PivotX, PivotY, PivotZ);
	const FVector3f Offset3f(Offset);

	StaticMesh->Modify();

	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
	{
		FMeshDescription* MeshDesc = StaticMesh->GetMeshDescription(LODIndex);
		if (!MeshDesc)
		{
			continue;
		}

		FStaticMeshAttributes Attributes(*MeshDesc);
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		for (const FVertexID VertexID : MeshDesc->Vertices().GetElementIDs())
		{
			Positions[VertexID] = Positions[VertexID] + Offset3f;
		}

		StaticMesh->CommitMeshDescription(LODIndex);
	}

	StaticMesh->Build(false);
	StaticMesh->PostEditChange();
	StaticMesh->MarkPackageDirty();

	return true;
#else
	return false;
#endif
}


FString UExportMeshBPLibrary::OpenFileDialogWithAssetName()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform)
	{
		FString OutFolderName;
		FString DefaultPath = FPaths::ProjectDir();

		if (DesktopPlatform->OpenDirectoryDialog(
			nullptr,
			NSLOCTEXT("ExportMesh", "SelectFolder", "Select Folder").ToString(),
			DefaultPath,
			OutFolderName))
		{
			// 从内容浏览器中获取当前选中的资产
			FString AssetName;
			if (GEditor)
			{
				FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
				TArray<FAssetData> SelectedAssets;
				ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

				if (SelectedAssets.Num() > 0)
				{
					// 获取第一个选中资产的名称（不带扩展名）
					AssetName = SelectedAssets[0].AssetName.ToString();
					UE_LOG(LogTemp, Log, TEXT("OpenFileDialogWithAssetName: Found selected asset: %s"), *AssetName);
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("OpenFileDialogWithAssetName: No asset selected in Content Browser"));
				}
			}

			// 如果有资产名，则拼接到路径后
			if (!AssetName.IsEmpty())
			{
				FString ResultPath = FPaths::Combine(OutFolderName, AssetName);
				UE_LOG(LogTemp, Log, TEXT("OpenFileDialogWithAssetName: Returning path: %s"), *ResultPath);
				return ResultPath;
			}

			return OutFolderName;
		}
	}
	return FString();
}

bool UExportMeshBPLibrary::ExportAssetToGLB(UObject* Asset, const FString& OutputPath)
{
	if (!Asset)
	{
		UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLB: Asset is null"));
		return false;
	}

	if (OutputPath.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLB: OutputPath is empty"));
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Starting export of %s to %s"), *Asset->GetName(), *OutputPath);

	// 规范化路径（将所有分隔符统一为平台标准格式）
	FString NormalizedPath = FPaths::ConvertRelativePathToFull(OutputPath);
	NormalizedPath.ReplaceInline(TEXT("/"), TEXT("\\"));

	UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Normalized input path: %s"), *NormalizedPath);

	// 检查输入路径是否已经有扩展名
	FString Extension = FPaths::GetExtension(NormalizedPath);
	UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Detected extension: '%s'"), *Extension);

	FString FinalOutputPath;
	FString OutputDirectory;

	if (Extension.IsEmpty())
	{
		// 没有扩展名，把输入路径作为目录，在该目录内生成文件
		OutputDirectory = NormalizedPath;
		FString AssetFileName = Asset->GetName() + TEXT(".glb");
		FinalOutputPath = FPaths::Combine(OutputDirectory, AssetFileName);

		UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Input treated as directory, output file will be: %s"), *AssetFileName);
	}
	else if (Extension.Equals(TEXT("glb"), ESearchCase::IgnoreCase))
	{
		// 已经是 .glb 文件路径，直接使用
		FinalOutputPath = NormalizedPath;
		OutputDirectory = FPaths::GetPath(FinalOutputPath);
		UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Input is a .glb file path"));
	}
	else
	{
		// 有其他扩展名，替换为 .glb
		FinalOutputPath = FPaths::SetExtension(NormalizedPath, TEXT("glb"));
		OutputDirectory = FPaths::GetPath(FinalOutputPath);
		UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Replaced extension with .glb"));
	}

	// 确保输出目录存在
	UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Output directory: %s"), *OutputDirectory);

	if (!FPaths::DirectoryExists(OutputDirectory))
	{
		UE_LOG(LogTemp, Warning, TEXT("ExportAssetToGLB: Output directory does not exist, creating: %s"), *OutputDirectory);
		IFileManager::Get().MakeDirectory(*OutputDirectory, true);
	}

	UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Final output path: %s"), *FinalOutputPath);

	// 创建导出任务
	UAssetExportTask* ExportTask = NewObject<UAssetExportTask>();
	ExportTask->Object = Asset;
	ExportTask->Filename = FinalOutputPath;
	ExportTask->bSelected = false;
	ExportTask->bReplaceIdentical = true;
	ExportTask->bPrompt = false;
	ExportTask->bUseFileArchive = false;
	ExportTask->bWriteEmptyFiles = false;
	ExportTask->bAutomated = true;

	// 识别资产类型并设置导出器
	UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
	USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset);
	UBlueprint* Blueprint = Cast<UBlueprint>(Asset);

	if (StaticMesh)
	{
		UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Detected StaticMesh: %s"), *StaticMesh->GetName());
		ExportTask->Exporter = nullptr; // 让系统自动选择 GLB 导出器
	}
	else if (SkeletalMesh)
	{
		UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Detected SkeletalMesh: %s"), *SkeletalMesh->GetName());
		ExportTask->Exporter = nullptr; // 让系统自动选择 GLB 导出器
	}
	else if (Blueprint)
	{
		UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Detected Blueprint: %s"), *Blueprint->GetName());

		// 从蓝图中提取 Mesh 组件（StaticMesh + SkeletalMesh）
		TArray<UPrimitiveComponent*> Components;
		GetComponentsFromBlueprintAsset(Blueprint, Components);

		if (Components.Num() == 0)
		{
			UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLB: Blueprint has no mesh components (StaticMesh or SkeletalMesh)"));
			return false;
		}

		// 将 SkeletalMeshComponent 转换为 StaticMeshComponent 以便合并
		TArray<UObject*> ConversionTempAssets;
		ConvertSkeletalComponentsForMerge(Components, ConversionTempAssets);

		if (Components.Num() == 0)
		{
			UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLB: No valid mesh components after conversion"));
			CleanupTempAssets(ConversionTempAssets);
			CleanupTempActors();
			return false;
		}

		// 创建临时合并的 StaticMesh
		FString TempPackageName = FString::Printf(TEXT("/Game/Temp/TempMerged_%s"), *Blueprint->GetName());
		TArray<UObject*> MergedAssets;

		MergeActorToStaticMesh(TempPackageName, Components, MergedAssets, false);

		// 现在可以安全地清理临时生成的Actor（组件数据已经合并到StaticMesh中）
		if (GEditor && GEditor->GetEditorWorldContext().World())
		{
			UWorld* World = GEditor->GetEditorWorldContext().World();
			TArray<AActor*> ActorsToDestroy;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (Actor && Actor->Tags.Contains(FName("PythonTemp")))
				{
					ActorsToDestroy.Add(Actor);
				}
			}

			for (AActor* Actor : ActorsToDestroy)
			{
				UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Destroying temporary actor: %s"), *Actor->GetName());
				Actor->Destroy();
			}
		}

		if (MergedAssets.Num() == 0)
		{
			UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLB: Failed to merge blueprint components"));
			CleanupTempAssets(ConversionTempAssets);
			return false;
		}

		// 清理转换临时资产（合并完成后不再需要）
		CleanupTempAssets(ConversionTempAssets);

		// 使用合并后的 StaticMesh 进行导出
		ExportTask->Object = MergedAssets[0];
		UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Using merged StaticMesh for export"));
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLB: Unsupported asset type: %s"), *Asset->GetClass()->GetName());
		return false;
	}

	// 让 UE 通过文件扩展名自动选择合适的 GLB/GLTF 导出器
	// 系统会根据 .glb 扩展名自动匹配 GLTFExporter
	ExportTask->Exporter = nullptr;

	UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Letting system auto-select exporter based on .glb extension"));

	// 执行导出
	bool bSuccess = UExporter::RunAssetExportTask(ExportTask);

	if (bSuccess)
	{
		UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Export task completed"));

		// 检查文件是否在预期位置
		if (IFileManager::Get().FileExists(*FinalOutputPath))
		{
			UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Successfully exported to %s"), *FinalOutputPath);
			return true;
		}

		// 文件不在预期位置，尝试在输出目录中查找
		UE_LOG(LogTemp, Warning, TEXT("ExportAssetToGLB: File not found at expected location, searching..."));

		FString OutputDir = FPaths::GetPath(FinalOutputPath);
		FString FileName = FPaths::GetCleanFilename(FinalOutputPath);
		FString AssetNameOnly = ExportTask->Object->GetName();

		// 可能的文件位置列表
		TArray<FString> PossiblePaths;
		PossiblePaths.Add(FinalOutputPath); // 预期路径
		PossiblePaths.Add(FPaths::Combine(OutputDir, AssetNameOnly + TEXT(".glb"))); // 使用资产名
		PossiblePaths.Add(FPaths::Combine(FPaths::GetPath(OutputDir), AssetNameOnly + TEXT(".glb"))); // 父目录
		PossiblePaths.Add(FPaths::Combine(FPaths::GetPath(OutputDir), FileName)); // 父目录 + 原始文件名

		FString FoundPath;
		for (const FString& TestPath : PossiblePaths)
		{
			UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Checking path: %s"), *TestPath);
			if (IFileManager::Get().FileExists(*TestPath))
			{
				FoundPath = TestPath;
				UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Found file at: %s"), *FoundPath);
				break;
			}
		}

		if (FoundPath.IsEmpty())
		{
			UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLB: Export succeeded but cannot find output file"));
			return false;
		}

		// 如果文件不在预期位置，移动它
		if (FoundPath != FinalOutputPath)
		{
			UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Moving file from %s to %s"), *FoundPath, *FinalOutputPath);

			if (IFileManager::Get().Move(*FinalOutputPath, *FoundPath, true, true))
			{
				UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLB: Successfully moved file to %s"), *FinalOutputPath);
				return true;
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLB: Failed to move file, but it exists at: %s"), *FoundPath);
				return false;
			}
		}

		return true;
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLB: Failed to export asset"));
	}

	return false;
}

// ==================== 新增功能：批量导出GLB（支持减面、后缀、枢轴点） ====================

FString UExportMeshBPLibrary::ComputeOutputPath(const FString& BasePath, const FString& AssetName, const FString& Suffix)
{
	// 规范化路径
	FString NormalizedPath = FPaths::ConvertRelativePathToFull(BasePath);
	NormalizedPath.ReplaceInline(TEXT("/"), TEXT("\\"));

	FString Extension = FPaths::GetExtension(NormalizedPath);

	if (Extension.IsEmpty())
	{
		// BasePath 是目录 → 在目录内生成文件
		FString FileName = AssetName + Suffix + TEXT(".glb");
		return FPaths::Combine(NormalizedPath, FileName);
	}
	else if (Extension.Equals(TEXT("glb"), ESearchCase::IgnoreCase))
	{
		// BasePath 是完整文件路径 → 插入后缀
		FString BaseFileName = FPaths::GetBaseFilename(NormalizedPath);
		FString Directory = FPaths::GetPath(NormalizedPath);
		FString NewFileName = BaseFileName + Suffix + TEXT(".glb");
		return FPaths::Combine(Directory, NewFileName);
	}
	else
	{
		// 其他扩展名 → 替换为 .glb
		FString BaseFileName = FPaths::GetBaseFilename(NormalizedPath);
		FString Directory = FPaths::GetPath(NormalizedPath);
		FString NewFileName = BaseFileName + Suffix + TEXT(".glb");
		return FPaths::Combine(Directory, NewFileName);
	}
}

UStaticMesh* UExportMeshBPLibrary::SimplifyStaticMesh(UStaticMesh* SourceMesh, float SimplifyDensity, TArray<UObject*>& OutTempAssets)
{
	if (!SourceMesh || !IsValid(SourceMesh))
	{
		UE_LOG(LogTemp, Error, TEXT("SimplifyStaticMesh: SourceMesh is invalid"));
		return nullptr;
	}

	// SimplifyDensity 为 0 表示不减面
	if (SimplifyDensity <= 0.0f || SimplifyDensity >= 1.0f)
	{
		UE_LOG(LogTemp, Warning, TEXT("SimplifyStaticMesh: SimplifyDensity %.2f is out of valid range (0-1), using original mesh"), SimplifyDensity);
		return SourceMesh;
	}

	UE_LOG(LogTemp, Log, TEXT("SimplifyStaticMesh: Starting simplification with density %.2f for mesh %s"), SimplifyDensity, *SourceMesh->GetName());

	// 1. 创建 DynamicMesh
	UDynamicMesh* DynamicMesh = NewObject<UDynamicMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!DynamicMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("SimplifyStaticMesh: Failed to create DynamicMesh"));
		return SourceMesh;
	}

	// 2. StaticMesh → DynamicMesh
	FGeometryScriptCopyMeshFromAssetOptions CopyOptions;
	CopyOptions.bApplyBuildSettings = true;
	CopyOptions.bRequestTangents = true;

	FGeometryScriptMeshReadLOD ReadLOD;
	ReadLOD.LODType = EGeometryScriptLODType::MaxAvailable;
	ReadLOD.LODIndex = 0;

	EGeometryScriptOutcomePins CopyOutcome;
	DynamicMesh = UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(
		SourceMesh,
		DynamicMesh,
		CopyOptions,
		ReadLOD,
		CopyOutcome
	);

	if (CopyOutcome != EGeometryScriptOutcomePins::Success)
	{
		UE_LOG(LogTemp, Error, TEXT("SimplifyStaticMesh: Failed to copy mesh from StaticMesh"));
		return SourceMesh;
	}

	// 3. 计算目标三角形数
	int32 OriginalTriCount = DynamicMesh->GetTriangleCount();
	if (OriginalTriCount == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("SimplifyStaticMesh: Source mesh has no triangles"));
		return SourceMesh;
	}

	// SimplifyDensity 表示要减少的比例，所以保留的比例是 (1 - SimplifyDensity)
	float RetainPercent = 1.0f - SimplifyDensity;
	int32 TargetTriCount = FMath::Max(4, FMath::RoundToInt(OriginalTriCount * RetainPercent));

	UE_LOG(LogTemp, Log, TEXT("SimplifyStaticMesh: Original triangles: %d, Target triangles: %d (retain %.1f%%)"),
		OriginalTriCount, TargetTriCount, RetainPercent * 100.0f);

	// 4. 应用简化
	FGeometryScriptSimplifyMeshOptions SimplifyOptions;
	SimplifyOptions.Method = EGeometryScriptRemoveMeshSimplificationType::StandardQEM;
	SimplifyOptions.bAllowSeamCollapse = false;  // 保护 UV 接缝
	SimplifyOptions.bAllowSeamSmoothing = false;

	DynamicMesh = UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTriangleCount(
		DynamicMesh,
		TargetTriCount,
		SimplifyOptions,
		nullptr  // Debug parameter
	);

	if (!DynamicMesh || DynamicMesh->GetTriangleCount() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("SimplifyStaticMesh: Simplification failed, using original mesh"));
		return SourceMesh;
	}

	int32 FinalTriCount = DynamicMesh->GetTriangleCount();
	UE_LOG(LogTemp, Log, TEXT("SimplifyStaticMesh: Simplified to %d triangles (%.1f%% reduction)"),
		FinalTriCount, (1.0f - (float)FinalTriCount / OriginalTriCount) * 100.0f);

	// 5. 创建临时 StaticMesh
	FString TempMeshName = FString::Printf(TEXT("TempSimplified_%s_%d"), *SourceMesh->GetName(), FMath::Rand());
	UStaticMesh* SimplifiedMesh = NewObject<UStaticMesh>(
		GetTransientPackage(),
		FName(*TempMeshName),
		RF_Transient
	);

	if (!SimplifiedMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("SimplifyStaticMesh: Failed to create temporary StaticMesh"));
		return SourceMesh;
	}

	// 6. 先复制材质槽（在CopyMeshToStaticMesh之前）
	TArray<FStaticMaterial> SourceMaterials = SourceMesh->GetStaticMaterials();
	SimplifiedMesh->GetStaticMaterials().Empty();

	UE_LOG(LogTemp, Log, TEXT("SimplifyStaticMesh: Copying %d material slots from source mesh"), SourceMaterials.Num());

	for (int32 i = 0; i < SourceMaterials.Num(); ++i)
	{
		const FStaticMaterial& Mat = SourceMaterials[i];
		SimplifiedMesh->GetStaticMaterials().Add(Mat);

		// 记录材质信息
		FString MaterialName = Mat.MaterialInterface ? Mat.MaterialInterface->GetName() : TEXT("NULL");
		UE_LOG(LogTemp, Log, TEXT("SimplifyStaticMesh: Material slot %d: %s (SlotName: %s)"),
			i, *MaterialName, *Mat.MaterialSlotName.ToString());
	}

	// 7. DynamicMesh → StaticMesh
	FGeometryScriptCopyMeshToAssetOptions ToAssetOptions;
	ToAssetOptions.bEnableRecomputeNormals = true;
	ToAssetOptions.bEnableRecomputeTangents = true;
	ToAssetOptions.bReplaceMaterials = false;  // 不替换材质，使用我们已经设置的
	ToAssetOptions.bEmitTransaction = false;

	FGeometryScriptMeshWriteLOD WriteLOD;
	WriteLOD.bWriteHiResSource = false;
	WriteLOD.LODIndex = 0;

	EGeometryScriptOutcomePins ToAssetOutcome;
	UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
		DynamicMesh,
		SimplifiedMesh,
		ToAssetOptions,
		WriteLOD,
		ToAssetOutcome
	);

	if (ToAssetOutcome != EGeometryScriptOutcomePins::Success)
	{
		UE_LOG(LogTemp, Error, TEXT("SimplifyStaticMesh: Failed to copy DynamicMesh to StaticMesh"));
		return SourceMesh;
	}

	// 8. 验证网格数据
	if (SimplifiedMesh->GetNumSourceModels() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("SimplifyStaticMesh: SimplifiedMesh has no source models after copy"));
		return SourceMesh;
	}

	FMeshDescription* MeshDesc = SimplifiedMesh->GetMeshDescription(0);
	if (!MeshDesc || MeshDesc->Vertices().Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("SimplifyStaticMesh: SimplifiedMesh has no vertices after copy"));
		return SourceMesh;
	}

	UE_LOG(LogTemp, Log, TEXT("SimplifyStaticMesh: Mesh description has %d vertices"), MeshDesc->Vertices().Num());

	// 9. 提交并构建网格
	SimplifiedMesh->CommitMeshDescription(0);
	SimplifiedMesh->Build(true);

	if (SimplifiedMesh->IsCompiling())
	{
		UE_LOG(LogTemp, Log, TEXT("SimplifyStaticMesh: Waiting for mesh compilation..."));
		FStaticMeshCompilingManager::Get().FinishCompilation({SimplifiedMesh});
	}

	// 10. 验证渲染数据
	if (SimplifiedMesh->GetNumLODs() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("SimplifyStaticMesh: SimplifiedMesh has no LODs after build"));
		return SourceMesh;
	}

	UE_LOG(LogTemp, Log, TEXT("SimplifyStaticMesh: SimplifiedMesh has %d LODs, %d materials"),
		SimplifiedMesh->GetNumLODs(), SimplifiedMesh->GetStaticMaterials().Num());

	// 11. 记录为临时资产
	OutTempAssets.Add(SimplifiedMesh);

	UE_LOG(LogTemp, Log, TEXT("SimplifyStaticMesh: Successfully created simplified mesh"));
	return SimplifiedMesh;
}

UStaticMesh* UExportMeshBPLibrary::AdjustStaticMeshPivot(UStaticMesh* SourceMesh, EPivotAlign Align, TArray<UObject*>& OutTempAssets)
{
	if (!SourceMesh || !IsValid(SourceMesh) || Align == EPivotAlign::None)
	{
		return SourceMesh;
	}

	UE_LOG(LogTemp, Log, TEXT("AdjustStaticMeshPivot: Adjusting pivot for mesh %s"), *SourceMesh->GetName());

	// 创建临时StaticMesh副本
	FString TempMeshName = FString::Printf(TEXT("TempPivot_%s_%d"), *SourceMesh->GetName(), FMath::Rand());
	UStaticMesh* TempMesh = DuplicateObject<UStaticMesh>(SourceMesh, GetTransientPackage(), FName(*TempMeshName));

	if (!TempMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("AdjustStaticMeshPivot: Failed to duplicate StaticMesh"));
		return SourceMesh;
	}

	TempMesh->SetFlags(RF_Transient);
	OutTempAssets.Add(TempMesh);

	// 应用枢轴点调整（修改临时网格）
	const int32 NumLODs = TempMesh->GetNumSourceModels();
	if (NumLODs <= 0)
	{
		return TempMesh;
	}

	FMeshDescription* BaseMeshDesc = TempMesh->GetMeshDescription(0);
	if (!BaseMeshDesc || BaseMeshDesc->Vertices().Num() == 0)
	{
		return TempMesh;
	}

	// 计算包围盒
	FStaticMeshAttributes BaseAttributes(*BaseMeshDesc);
	const TVertexAttributesRef<FVector3f> BasePositions = BaseAttributes.GetVertexPositions();

	FBox Bounds(ForceInit);
	for (const FVertexID VertexID : BaseMeshDesc->Vertices().GetElementIDs())
	{
		Bounds += FVector(BasePositions[VertexID]);
	}

	if (!Bounds.IsValid)
	{
		return TempMesh;
	}

	const FVector Min = Bounds.Min;
	const FVector Max = Bounds.Max;

	float PivotX = (Min.X + Max.X) * 0.5f;
	float PivotY = (Min.Y + Max.Y) * 0.5f;
	float PivotZ = (Min.Z + Max.Z) * 0.5f;

	switch (Align)
	{
	case EPivotAlign::BottomCenter:
		PivotZ = Min.Z;
		break;
	case EPivotAlign::BottomRight:
		PivotZ = Min.Z;
		PivotY = Max.Y;
		break;
	case EPivotAlign::BottomLeft:
		PivotZ = Min.Z;
		PivotY = Min.Y;
		break;
	case EPivotAlign::Center:
		break;
	case EPivotAlign::CenterLeft:
		PivotY = Min.Y;
		break;
	case EPivotAlign::CenterRight:
		PivotY = Max.Y;
		break;
	case EPivotAlign::TopCenter:
		PivotZ = Max.Z;
		break;
	case EPivotAlign::TopRight:
		PivotZ = Max.Z;
		PivotY = Max.Y;
		break;
	case EPivotAlign::TopLeft:
		PivotZ = Max.Z;
		PivotY = Min.Y;
		break;
	default:
		break;
	}

	const FVector Offset = -FVector(PivotX, PivotY, PivotZ);
	const FVector3f Offset3f(Offset);

	// 调整所有LOD的顶点位置
	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
	{
		FMeshDescription* MeshDesc = TempMesh->GetMeshDescription(LODIndex);
		if (!MeshDesc)
		{
			continue;
		}

		FStaticMeshAttributes Attributes(*MeshDesc);
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		for (const FVertexID VertexID : MeshDesc->Vertices().GetElementIDs())
		{
			Positions[VertexID] = Positions[VertexID] + Offset3f;
		}

		TempMesh->CommitMeshDescription(LODIndex);
	}

	TempMesh->Build(true);
	if (TempMesh->IsCompiling())
	{
		FStaticMeshCompilingManager::Get().FinishCompilation({TempMesh});
	}

	UE_LOG(LogTemp, Log, TEXT("AdjustStaticMeshPivot: Successfully adjusted pivot"));
	return TempMesh;
}

void UExportMeshBPLibrary::CleanupTempAssets(TArray<UObject*>& TempAssets)
{
	for (UObject* Asset : TempAssets)
	{
		if (IsValid(Asset))
		{
			Asset->ConditionalBeginDestroy();
		}
	}
	TempAssets.Reset();
}

void UExportMeshBPLibrary::CleanupTempActors()
{
	if (GEditor && GEditor->GetEditorWorldContext().World())
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		TArray<AActor*> ActorsToDestroy;

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor && Actor->Tags.Contains(FName("PythonTemp")))
			{
				ActorsToDestroy.Add(Actor);
			}
		}

		for (AActor* Actor : ActorsToDestroy)
		{
			UE_LOG(LogTemp, Log, TEXT("CleanupTempActors: Destroying temporary actor: %s"), *Actor->GetName());
			Actor->Destroy();
		}
	}
}

// ==================== SkeletalMesh → StaticMesh 转换 ====================

UStaticMesh* UExportMeshBPLibrary::ConvertSkeletalMeshToStaticMesh(USkeletalMesh* SkelMesh)
{
	if (!SkelMesh || !IsValid(SkelMesh))
	{
		UE_LOG(LogTemp, Error, TEXT("ConvertSkeletalMeshToStaticMesh: SkelMesh is invalid"));
		return nullptr;
	}

	FSkeletalMeshRenderData* RenderData = SkelMesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("ConvertSkeletalMeshToStaticMesh: No render data for %s"), *SkelMesh->GetName());
		return nullptr;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];
	const uint32 NumVertices = LODData.GetNumVertices();
	if (NumVertices == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("ConvertSkeletalMeshToStaticMesh: No vertices in %s"), *SkelMesh->GetName());
		return nullptr;
	}

	UE_LOG(LogTemp, Log, TEXT("ConvertSkeletalMeshToStaticMesh: Converting %s (%d vertices, %d sections)"),
		*SkelMesh->GetName(), NumVertices, LODData.RenderSections.Num());

	// 创建 MeshDescription
	FMeshDescription MeshDesc;
	FStaticMeshAttributes StaticMeshAttribs(MeshDesc);
	StaticMeshAttribs.Register();

	// 获取顶点缓冲区
	const FPositionVertexBuffer& PositionBuffer = LODData.StaticVertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& VertexBuffer = LODData.StaticVertexBuffers.StaticMeshVertexBuffer;
	const int32 NumUVChannels = VertexBuffer.GetNumTexCoords();

	// 创建 PolygonGroups（每个 Section 对应一个材质）
	TArray<FPolygonGroupID> PolygonGroupIDs;
	for (int32 SectionIdx = 0; SectionIdx < LODData.RenderSections.Num(); ++SectionIdx)
	{
		PolygonGroupIDs.Add(MeshDesc.CreatePolygonGroup());
	}

	// 获取索引缓冲区
	TArray<uint32> IndexData;
	LODData.MultiSizeIndexContainer.GetIndexBuffer(IndexData);
	const int32 NumIndices = IndexData.Num();

	if (NumIndices == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("ConvertSkeletalMeshToStaticMesh: No indices in %s"), *SkelMesh->GetName());
		return nullptr;
	}

	// 预分配空间
	const int32 NumTriangles = NumIndices / 3;
	MeshDesc.ReserveNewVertices(NumVertices);
	MeshDesc.ReserveNewVertexInstances(NumIndices); // 每个三角形顶点引用都需要一个 VertexInstance
	MeshDesc.ReserveNewTriangles(NumTriangles);
	MeshDesc.ReserveNewPolygons(NumTriangles);
	MeshDesc.ReserveNewPolygonGroups(LODData.RenderSections.Num());

	// 创建顶点
	TArray<FVertexID> VertexIDs;
	VertexIDs.Reserve(NumVertices);
	TVertexAttributesRef<FVector3f> VertexPositions = StaticMeshAttribs.GetVertexPositions();

	for (uint32 i = 0; i < NumVertices; ++i)
	{
		FVertexID VID = MeshDesc.CreateVertex();
		VertexIDs.Add(VID);
		VertexPositions[VID] = PositionBuffer.VertexPosition(i);
	}

	// 获取 VertexInstance 属性引用
	TVertexInstanceAttributesRef<FVector3f> InstanceNormals = StaticMeshAttribs.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> InstanceTangents = StaticMeshAttribs.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> InstanceBinormalSigns = StaticMeshAttribs.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector2f> InstanceUVs = StaticMeshAttribs.GetVertexInstanceUVs();
	InstanceUVs.SetNumChannels(NumUVChannels);

	// 按 Section 创建三角形
	for (int32 SectionIdx = 0; SectionIdx < LODData.RenderSections.Num(); ++SectionIdx)
	{
		const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIdx];
		const uint32 FirstIndex = Section.BaseIndex;
		const uint32 SectionNumTriangles = Section.NumTriangles;

		for (uint32 TriIdx = 0; TriIdx < SectionNumTriangles; ++TriIdx)
		{
			// 获取三角形的三个顶点索引
			uint32 TriIndices[3];
			TriIndices[0] = IndexData[FirstIndex + TriIdx * 3];
			TriIndices[1] = IndexData[FirstIndex + TriIdx * 3 + 1];
			TriIndices[2] = IndexData[FirstIndex + TriIdx * 3 + 2];

			// 为每个三角形顶点创建 VertexInstance
			TArray<FVertexInstanceID> TriVertInstances;
			TriVertInstances.Reserve(3);

			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				uint32 VertIdx = TriIndices[Corner];
				FVertexInstanceID VIID = MeshDesc.CreateVertexInstance(VertexIDs[VertIdx]);
				TriVertInstances.Add(VIID);

				// 设置法线
				FVector3f Normal = VertexBuffer.VertexTangentZ(VertIdx);
				InstanceNormals[VIID] = Normal;

				// 设置切线（VertexTangentX 返回 FVector4f，W 分量存储 BinormalSign）
				FVector4f TangentWithSign = VertexBuffer.VertexTangentX(VertIdx);
				InstanceTangents[VIID] = FVector3f(TangentWithSign.X, TangentWithSign.Y, TangentWithSign.Z);
				InstanceBinormalSigns[VIID] = TangentWithSign.W;

				// 设置 UV
				for (int32 UVIdx = 0; UVIdx < NumUVChannels; ++UVIdx)
				{
					InstanceUVs.Set(VIID, UVIdx, VertexBuffer.GetVertexUV(VertIdx, UVIdx));
				}
			}

			// 创建多边形（三角形）
			MeshDesc.CreatePolygon(PolygonGroupIDs[SectionIdx], TriVertInstances);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("ConvertSkeletalMeshToStaticMesh: Built MeshDescription with %d vertices, %d triangles"),
		MeshDesc.Vertices().Num(), MeshDesc.Triangles().Num());

	// 创建 StaticMesh
	FString TempMeshName = FString::Printf(TEXT("TempSkelConvert_%s_%d"), *SkelMesh->GetName(), FMath::Rand());
	UStaticMesh* NewStaticMesh = NewObject<UStaticMesh>(GetTransientPackage(), FName(*TempMeshName), RF_Transient);
	if (!NewStaticMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("ConvertSkeletalMeshToStaticMesh: Failed to create StaticMesh"));
		return nullptr;
	}

	// 设置 MeshDescription
	NewStaticMesh->SetNumSourceModels(1);
	FMeshDescription* DestMeshDesc = NewStaticMesh->GetMeshDescription(0);
	if (!DestMeshDesc)
	{
		NewStaticMesh->CreateMeshDescription(0);
		DestMeshDesc = NewStaticMesh->GetMeshDescription(0);
	}
	*DestMeshDesc = MoveTemp(MeshDesc);
	NewStaticMesh->CommitMeshDescription(0);

	// 复制材质
	const TArray<FSkeletalMaterial>& SkelMaterials = SkelMesh->GetMaterials();
	for (int32 SectionIdx = 0; SectionIdx < LODData.RenderSections.Num(); ++SectionIdx)
	{
		int32 MaterialIdx = LODData.RenderSections[SectionIdx].MaterialIndex;
		if (SkelMaterials.IsValidIndex(MaterialIdx) && SkelMaterials[MaterialIdx].MaterialInterface)
		{
			NewStaticMesh->GetStaticMaterials().Add(
				FStaticMaterial(SkelMaterials[MaterialIdx].MaterialInterface,
				               SkelMaterials[MaterialIdx].MaterialSlotName));
			UE_LOG(LogTemp, Log, TEXT("ConvertSkeletalMeshToStaticMesh: Material slot %d: %s"),
				SectionIdx, *SkelMaterials[MaterialIdx].MaterialInterface->GetName());
		}
		else
		{
			NewStaticMesh->GetStaticMaterials().Add(FStaticMaterial());
			UE_LOG(LogTemp, Warning, TEXT("ConvertSkeletalMeshToStaticMesh: Material slot %d: NULL (index %d)"),
				SectionIdx, MaterialIdx);
		}
	}

	// 构建 StaticMesh
	NewStaticMesh->Build(true);
	if (NewStaticMesh->IsCompiling())
	{
		FStaticMeshCompilingManager::Get().FinishCompilation({NewStaticMesh});
	}

	// 验证
	if (NewStaticMesh->GetNumLODs() == 0 || NewStaticMesh->GetNumSourceModels() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("ConvertSkeletalMeshToStaticMesh: Built mesh has no LODs"));
		return nullptr;
	}

	UE_LOG(LogTemp, Log, TEXT("ConvertSkeletalMeshToStaticMesh: Successfully converted %s to StaticMesh (%d LODs, %d materials)"),
		*SkelMesh->GetName(), NewStaticMesh->GetNumLODs(), NewStaticMesh->GetStaticMaterials().Num());

	return NewStaticMesh;
}

void UExportMeshBPLibrary::ConvertSkeletalComponentsForMerge(TArray<UPrimitiveComponent*>& Components, TArray<UObject*>& OutTempAssets)
{
	for (int32 i = Components.Num() - 1; i >= 0; --i)
	{
		USkeletalMeshComponent* SkelComp = Cast<USkeletalMeshComponent>(Components[i]);
		if (!SkelComp)
		{
			continue;
		}

		USkeletalMesh* SkelMesh = SkelComp->GetSkeletalMeshAsset();
		if (!SkelMesh || !IsValid(SkelMesh))
		{
			UE_LOG(LogTemp, Warning, TEXT("ConvertSkeletalComponentsForMerge: Removing invalid SkeletalMeshComponent: %s"),
				*SkelComp->GetName());
			Components.RemoveAt(i);
			continue;
		}

		// 转换 SkeletalMesh → StaticMesh
		UStaticMesh* ConvertedMesh = ConvertSkeletalMeshToStaticMesh(SkelMesh);
		if (!ConvertedMesh)
		{
			UE_LOG(LogTemp, Warning, TEXT("ConvertSkeletalComponentsForMerge: Conversion failed for %s, removing from list"),
				*SkelComp->GetName());
			Components.RemoveAt(i);
			continue;
		}
		OutTempAssets.Add(ConvertedMesh);

		// 创建临时 StaticMeshComponent 替代 SkeletalMeshComponent
		AActor* OwnerActor = SkelComp->GetOwner();
		if (!OwnerActor)
		{
			UE_LOG(LogTemp, Warning, TEXT("ConvertSkeletalComponentsForMerge: No owner actor for %s"), *SkelComp->GetName());
			Components.RemoveAt(i);
			continue;
		}

		UStaticMeshComponent* TempStaticComp = NewObject<UStaticMeshComponent>(OwnerActor, NAME_None, RF_Transient);
		TempStaticComp->SetStaticMesh(ConvertedMesh);
		TempStaticComp->SetWorldTransform(SkelComp->GetComponentTransform());
		TempStaticComp->RegisterComponent();
		OutTempAssets.Add(TempStaticComp);

		UE_LOG(LogTemp, Log, TEXT("ConvertSkeletalComponentsForMerge: Replaced SkeletalMeshComponent '%s' with StaticMeshComponent (mesh: %s)"),
			*SkelComp->GetName(), *ConvertedMesh->GetName());

		// 替换数组中的条目
		Components[i] = TempStaticComp;
	}
}

bool UExportMeshBPLibrary::ExportAssetToGLBWithConfig(
	const FAssetData& AssetData,
	const FString& OutputPath,
	float SimplifyDensity,
	const FString& GLBSuffix,
	EPivotAlign PivotAlign)
{
	TArray<FGLBExportConfig> Configs;
	Configs.Add(FGLBExportConfig(SimplifyDensity, GLBSuffix, PivotAlign));
	return ExportAssetToGLBBatch(AssetData, OutputPath, Configs);
}

bool UExportMeshBPLibrary::ExportAssetToGLBBatch(const FAssetData& AssetData, const FString& OutputPath, const TArray<FGLBExportConfig>& ExportConfigs)
{
	// 验证 AssetData
	if (!AssetData.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLBBatch: AssetData is not valid"));
		return false;
	}

	if (OutputPath.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLBBatch: OutputPath is empty"));
		return false;
	}

	if (ExportConfigs.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("ExportAssetToGLBBatch: No export configs provided, using default"));
		// 加载资产
		UObject* Asset = AssetData.GetAsset();
		if (!Asset)
		{
			UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLBBatch: Failed to load asset from AssetData"));
			return false;
		}
		return ExportAssetToGLB(Asset, OutputPath);
	}

	// 获取资产类名和名称
	FString AssetClassName = AssetData.AssetClassPath.GetAssetName().ToString();
	FString AssetName = AssetData.AssetName.ToString();

	UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: Starting batch export of %s (Class: %s) with %d configurations"),
		*AssetName, *AssetClassName, ExportConfigs.Num());

	// 存储所有临时资产
	TArray<UObject*> TempAssets;

	// 根据 AssetData 的类名识别资产类型
	UStaticMesh* SourceStaticMesh = nullptr;
	UObject* LoadedAsset = nullptr;
	bool bIsFromBlueprint = false;

	// 判断资产类型（不需要立即加载）
	if (AssetClassName.Equals(TEXT("StaticMesh")))
	{
		// StaticMesh 类型
		UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: Detected StaticMesh asset: %s"), *AssetName);

		LoadedAsset = AssetData.GetAsset();
		if (!LoadedAsset)
		{
			UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLBBatch: Failed to load StaticMesh asset"));
			return false;
		}

		SourceStaticMesh = Cast<UStaticMesh>(LoadedAsset);
		if (!SourceStaticMesh)
		{
			UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLBBatch: Asset is not a valid StaticMesh"));
			return false;
		}
	}
	else if (AssetClassName.Equals(TEXT("SkeletalMesh")))
	{
		// SkeletalMesh 类型（不支持简化）
		UE_LOG(LogTemp, Warning, TEXT("ExportAssetToGLBBatch: SkeletalMesh detected. Simplification and pivot adjustment not supported, exporting original"));

		LoadedAsset = AssetData.GetAsset();
		if (!LoadedAsset)
		{
			UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLBBatch: Failed to load SkeletalMesh asset"));
			return false;
		}

		// SkeletalMesh 不支持简化，逐个导出原始网格
		bool bAllSuccess = true;
		for (const FGLBExportConfig& Config : ExportConfigs)
		{
			if (!Config.bEnabled)
			{
				continue;
			}

			FString FinalPath = ComputeOutputPath(OutputPath, AssetName, Config.GLBSuffix);
			bool bSuccess = ExportAssetToGLB(LoadedAsset, FinalPath);
			bAllSuccess = bAllSuccess && bSuccess;
		}
		return bAllSuccess;
	}
	else if (AssetClassName.Equals(TEXT("Blueprint")) || AssetClassName.Contains(TEXT("Blueprint")))
	{
		// Blueprint 类型
		UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: Detected Blueprint asset: %s"), *AssetName);
		bIsFromBlueprint = true;

		LoadedAsset = AssetData.GetAsset();
		if (!LoadedAsset)
		{
			UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLBBatch: Failed to load Blueprint asset"));
			return false;
		}

		UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset);
		if (!Blueprint)
		{
			UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLBBatch: Asset is not a valid Blueprint"));
			return false;
		}

		// 从蓝图中提取 Mesh 组件（StaticMesh + SkeletalMesh）
		TArray<UPrimitiveComponent*> Components;
		GetComponentsFromBlueprintAsset(Blueprint, Components);

		if (Components.Num() == 0)
		{
			UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLBBatch: Blueprint has no mesh components (StaticMesh or SkeletalMesh)"));
			CleanupTempActors();
			return false;
		}

		// 将 SkeletalMeshComponent 转换为 StaticMeshComponent 以便合并
		TArray<UObject*> ConversionTempAssets;
		ConvertSkeletalComponentsForMerge(Components, ConversionTempAssets);

		if (Components.Num() == 0)
		{
			UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLBBatch: No valid mesh components after conversion"));
			CleanupTempAssets(ConversionTempAssets);
			CleanupTempActors();
			return false;
		}

		// 创建临时合并的 StaticMesh
		FString TempPackageName = FString::Printf(TEXT("/Game/Temp/TempMergedBatch_%s"), *AssetName);
		TArray<UObject*> MergedAssets;

		MergeActorToStaticMesh(TempPackageName, Components, MergedAssets, false);
		CleanupTempActors();
		CleanupTempAssets(ConversionTempAssets);

		if (MergedAssets.Num() == 0)
		{
			UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLBBatch: Failed to merge blueprint components"));
			return false;
		}

		SourceStaticMesh = Cast<UStaticMesh>(MergedAssets[0]);
		if (!SourceStaticMesh)
		{
			UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLBBatch: Merged asset is not a StaticMesh"));
			return false;
		}

		TempAssets.Add(SourceStaticMesh);

		// 记录合并后的材质信息
		int32 MaterialCount = SourceStaticMesh->GetStaticMaterials().Num();
		UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: Successfully merged blueprint to StaticMesh with %d materials"), MaterialCount);

		for (int32 i = 0; i < MaterialCount; ++i)
		{
			UMaterialInterface* Mat = SourceStaticMesh->GetMaterial(i);
			FString MatName = Mat ? Mat->GetName() : TEXT("NULL");
			UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: Merged mesh material slot %d: %s"), i, *MatName);
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLBBatch: Unsupported asset type: %s (Class: %s)"), *AssetName, *AssetClassName);
		return false;
	}

	// 批量处理所有配置
	bool bAllSuccess = true;
	int32 ExportedCount = 0;
	FString ActualOutputDirectory = "";  // 记录实际的输出目录

	for (const FGLBExportConfig& Config : ExportConfigs)
	{
		if (!Config.bEnabled)
		{
			UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: Skipping disabled config with suffix '%s'"), *Config.GLBSuffix);
			continue;
		}

		// 参数验证
		if (Config.SimplifyDensity < 0.0f || Config.SimplifyDensity > 1.0f)
		{
			UE_LOG(LogTemp, Warning, TEXT("ExportAssetToGLBBatch: Invalid SimplifyDensity %.2f (suffix: %s), skipping"),
				Config.SimplifyDensity, *Config.GLBSuffix);
			continue;
		}

		UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: Processing config [Density: %.2f, Suffix: '%s', Pivot: %d]"),
			Config.SimplifyDensity, *Config.GLBSuffix, (int32)Config.PivotAlign);

		// 应用简化（如果需要）
		UStaticMesh* ProcessedMesh = SourceStaticMesh;
		if (Config.SimplifyDensity > 0.0f && Config.SimplifyDensity < 1.0f)
		{
			UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: Applying simplification with density %.2f"), Config.SimplifyDensity);
			ProcessedMesh = SimplifyStaticMesh(SourceStaticMesh, Config.SimplifyDensity, TempAssets);
			if (!ProcessedMesh)
			{
				UE_LOG(LogTemp, Warning, TEXT("ExportAssetToGLBBatch: Simplification failed, using original mesh"));
				ProcessedMesh = SourceStaticMesh;
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: Simplified mesh has %d LODs, %d materials, %d vertices"),
					ProcessedMesh->GetNumLODs(),
					ProcessedMesh->GetStaticMaterials().Num(),
					ProcessedMesh->GetMeshDescription(0) ? ProcessedMesh->GetMeshDescription(0)->Vertices().Num() : 0);
			}
		}

		// 应用枢轴点调整（如果需要）
		if (Config.PivotAlign != EPivotAlign::None)
		{
			ProcessedMesh = AdjustStaticMeshPivot(ProcessedMesh, Config.PivotAlign, TempAssets);
			if (!ProcessedMesh)
			{
				UE_LOG(LogTemp, Warning, TEXT("ExportAssetToGLBBatch: Pivot adjustment failed, using previous mesh"));
				ProcessedMesh = SourceStaticMesh;
			}
		}

		// 构造输出路径
		FString FinalPath = ComputeOutputPath(OutputPath, AssetName, Config.GLBSuffix);
		FString OutputDirectory = FPaths::GetPath(FinalPath);

		// 记录第一个有效的输出目录（用于manifest.json）
		if (ActualOutputDirectory.IsEmpty())
		{
			ActualOutputDirectory = OutputDirectory;
			UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: Set output directory for manifest: %s"), *ActualOutputDirectory);
		}

		// 确保输出目录存在
		if (!FPaths::DirectoryExists(OutputDirectory))
		{
			IFileManager::Get().MakeDirectory(*OutputDirectory, true);
		}

		UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: Exporting to: %s"), *FinalPath);

		// 验证导出前的材质信息
		int32 ExportMaterialCount = ProcessedMesh->GetStaticMaterials().Num();
		UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: ProcessedMesh has %d material slots before export"), ExportMaterialCount);
		for (int32 i = 0; i < ExportMaterialCount; ++i)
		{
			UMaterialInterface* Mat = ProcessedMesh->GetMaterial(i);
			if (Mat)
			{
				UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: Export material slot %d: %s (Valid)"), i, *Mat->GetName());
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("ExportAssetToGLBBatch: Export material slot %d: NULL (Will appear as pink in viewer!)"), i);
			}
		}

		// 创建导出任务
		UAssetExportTask* ExportTask = NewObject<UAssetExportTask>();
		ExportTask->Object = ProcessedMesh;
		ExportTask->Filename = FinalPath;
		ExportTask->bSelected = false;
		ExportTask->bReplaceIdentical = true;
		ExportTask->bPrompt = false;
		ExportTask->bUseFileArchive = false;
		ExportTask->bWriteEmptyFiles = false;
		ExportTask->bAutomated = true;
		ExportTask->Exporter = nullptr; // Auto-select GLB exporter

		// 执行导出
		bool bSuccess = UExporter::RunAssetExportTask(ExportTask);

		if (bSuccess && IFileManager::Get().FileExists(*FinalPath))
		{
			UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: Successfully exported to %s"), *FinalPath);
			ExportedCount++;
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("ExportAssetToGLBBatch: Failed to export to %s"), *FinalPath);
			bAllSuccess = false;
		}
	}

	// 生成manifest.json
	if (bAllSuccess && ExportedCount > 0 && SourceStaticMesh && !ActualOutputDirectory.IsEmpty())
	{
		// 计算包围盒 - 统一使用导出的StaticMesh计算，确保与GLB文件一致
		UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: Calculating bounds from %s mesh"),
			bIsFromBlueprint ? TEXT("merged") : TEXT("source"));

		FBox MeshBounds = CalculateStaticMeshBounds(SourceStaticMesh);

		// 获取资产路径
		FString AssetPathString = AssetData.GetObjectPathString();

		// 使用实际的输出目录（与GLB文件相同）
		GenerateManifestJson(ActualOutputDirectory, AssetName, AssetPathString, MeshBounds, ExportConfigs);
	}

	// 清理临时资产
	CleanupTempAssets(TempAssets);

	UE_LOG(LogTemp, Log, TEXT("ExportAssetToGLBBatch: Batch export completed. %d/%d files exported successfully"),
		ExportedCount, ExportConfigs.Num());

	return bAllSuccess && (ExportedCount > 0);
}

// ==================== Manifest JSON生成 ====================

FBox UExportMeshBPLibrary::CalculateStaticMeshBounds(UStaticMesh* Mesh)
{
	if (!Mesh || !IsValid(Mesh))
	{
		return FBox(ForceInit);
	}

	FBox BoundingBox = Mesh->GetBoundingBox();
	UE_LOG(LogTemp, Log, TEXT("CalculateStaticMeshBounds: Mesh %s bounds: Min(%s), Max(%s)"),
		*Mesh->GetName(), *BoundingBox.Min.ToString(), *BoundingBox.Max.ToString());

	return BoundingBox;
}

FBox UExportMeshBPLibrary::CalculateBlueprintMeshBounds(const TArray<UPrimitiveComponent*>& MeshComponents)
{
	FBox CombinedBounds(ForceInit);
	int32 ValidMeshCount = 0;

	for (UPrimitiveComponent* Component : MeshComponents)
	{
		if (!IsValid(Component))
		{
			continue;
		}

		bool bValidComponent = false;

		// 计算StaticMeshComponent的包围盒
		if (UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(Component))
		{
			UStaticMesh* StaticMesh = StaticMeshComp->GetStaticMesh();
			if (StaticMesh && IsValid(StaticMesh))
			{
				bValidComponent = true;
			}
		}
		// 计算SkeletalMeshComponent的包围盒
		else if (USkeletalMeshComponent* SkelMeshComp = Cast<USkeletalMeshComponent>(Component))
		{
			USkeletalMesh* SkelMesh = SkelMeshComp->GetSkeletalMeshAsset();
			if (SkelMesh && IsValid(SkelMesh))
			{
				bValidComponent = true;
			}
		}

		if (bValidComponent)
		{
			FBox ComponentBounds = Component->Bounds.GetBox();
			CombinedBounds += ComponentBounds;
			ValidMeshCount++;

			UE_LOG(LogTemp, Log, TEXT("CalculateBlueprintMeshBounds: Component %s bounds: Min(%s), Max(%s)"),
				*Component->GetName(), *ComponentBounds.Min.ToString(), *ComponentBounds.Max.ToString());
		}
	}

	UE_LOG(LogTemp, Log, TEXT("CalculateBlueprintMeshBounds: Combined %d meshes, final bounds: Min(%s), Max(%s)"),
		ValidMeshCount, *CombinedBounds.Min.ToString(), *CombinedBounds.Max.ToString());

	return CombinedBounds;
}

bool UExportMeshBPLibrary::GenerateManifestJson(
	const FString& OutputDirectory,
	const FString& AssetName,
	const FString& AssetPath,
	const FBox& MeshBounds,
	const TArray<FGLBExportConfig>& ExportConfigs)
{
	if (OutputDirectory.IsEmpty() || AssetName.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("GenerateManifestJson: OutputDirectory or AssetName is empty"));
		return false;
	}

	// 计算dimensions（包围盒尺寸）- 从厘米转换为米
	FVector BoundsSize = MeshBounds.GetSize();
	float Height = BoundsSize.Z / 100.0f;  // UE中Z是高度，cm转m
	float Width = BoundsSize.Y / 100.0f;   // Y是宽度，cm转m
	float Length = BoundsSize.X / 100.0f;  // X是长度，cm转m

	// 计算originToCenter（几何中心相对于枢轴点的偏移量）- 从厘米转换为米
	FVector BoundsCenter = MeshBounds.GetCenter();
	FVector OriginToCenter;
	OriginToCenter.X = -BoundsCenter.X / 100.0f;  // 几何中心X反向，cm转m
	OriginToCenter.Y = 0.0f;                       // 默认为0
	OriginToCenter.Z = BoundsCenter.Z / 100.0f;   // 几何中心Z，cm转m

	// 如果偏移量绝对值小于0.01米，则设为0
	if (FMath::Abs(OriginToCenter.X) < 0.01f)
	{
		OriginToCenter.X = 0.0f;
	}
	if (FMath::Abs(OriginToCenter.Z) < 0.01f)
	{
		OriginToCenter.Z = 0.0f;
	}

	UE_LOG(LogTemp, Log, TEXT("GenerateManifestJson: Dimensions - H:%.2f W:%.2f L:%.2f"),
		Height, Width, Length);
	UE_LOG(LogTemp, Log, TEXT("GenerateManifestJson: OriginToCenter - X:%.2f Y:%.2f Z:%.2f"),
		OriginToCenter.X, OriginToCenter.Y, OriginToCenter.Z);

	// 查找refined和reduced文件名
	FString RefinedObj = AssetName + TEXT(".glb");
	FString ReducedObj = AssetName + TEXT("_LowPoly.glb");

	// 从ExportConfigs中提取实际的文件名
	for (const FGLBExportConfig& Config : ExportConfigs)
	{
		if (!Config.bEnabled)
		{
			continue;
		}

		if (Config.SimplifyDensity <= 0.0f)
		{
			// 未简化版本
			RefinedObj = AssetName + Config.GLBSuffix + TEXT(".glb");
		}
		else
		{
			// 简化版本
			ReducedObj = AssetName + Config.GLBSuffix + TEXT(".glb");
		}
	}

	// 确定资产路径字段
	FString TargetStaticMeshPath = "";
	FString TargetSkeletonPath = "";
	FString TargetSkeletalMeshPath = "";
	FString TargetBPPath = "";

	if (AssetPath.Contains(TEXT("StaticMesh")))
	{
		TargetStaticMeshPath = AssetPath;
	}
	else if (AssetPath.Contains(TEXT("SkeletalMesh")) || AssetPath.Contains(TEXT("Skeleton")))
	{
		if (AssetPath.EndsWith(TEXT("_Skeleton")))
		{
			TargetSkeletonPath = AssetPath;
		}
		else
		{
			TargetSkeletalMeshPath = AssetPath;
		}
	}
	else
	{
		// 假设是Blueprint，添加_C后缀
		TargetBPPath = AssetPath;
		if (!TargetBPPath.IsEmpty() && !TargetBPPath.EndsWith(TEXT("_C")))
		{
			TargetBPPath += TEXT("_C");
		}
	}

	// 创建JSON对象
	TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);

	JsonObject->SetStringField(TEXT("name"), AssetName);
	JsonObject->SetStringField(TEXT("model"), AssetName);
	JsonObject->SetStringField(TEXT("reduced_obj"), ReducedObj);
	JsonObject->SetStringField(TEXT("refined_obj"), RefinedObj);
	JsonObject->SetStringField(TEXT("texture"), TEXT("texture.png"));
	JsonObject->SetStringField(TEXT("thumbnail"), TEXT("thumbnail.png"));
	JsonObject->SetStringField(TEXT("thumbnail2"), TEXT("thumbnail2.png"));
	JsonObject->SetStringField(TEXT("TargetStaticMeshPath"), TargetStaticMeshPath);
	JsonObject->SetStringField(TEXT("TargetSkeletonPath"), TargetSkeletonPath);
	JsonObject->SetStringField(TEXT("TargetSkeletalMeshPath"), TargetSkeletalMeshPath);
	JsonObject->SetStringField(TEXT("TargetBPPath"), TargetBPPath);
	JsonObject->SetStringField(TEXT("unit"), TEXT("m"));

	// dimensions对象
	TSharedPtr<FJsonObject> DimensionsObject = MakeShareable(new FJsonObject);
	DimensionsObject->SetNumberField(TEXT("height"), Height);
	DimensionsObject->SetNumberField(TEXT("width"), Width);
	DimensionsObject->SetNumberField(TEXT("length"), Length);
	JsonObject->SetObjectField(TEXT("dimensions"), DimensionsObject);

	// originToCenter对象
	TSharedPtr<FJsonObject> OriginToCenterObject = MakeShareable(new FJsonObject);
	OriginToCenterObject->SetNumberField(TEXT("x"), OriginToCenter.X);
	OriginToCenterObject->SetNumberField(TEXT("y"), OriginToCenter.Y);
	OriginToCenterObject->SetNumberField(TEXT("z"), OriginToCenter.Z);
	JsonObject->SetObjectField(TEXT("originToCenter"), OriginToCenterObject);

	// 序列化为字符串
	FString JsonString;
	TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&JsonString);
	if (!FJsonSerializer::Serialize(JsonObject.ToSharedRef(), JsonWriter))
	{
		UE_LOG(LogTemp, Error, TEXT("GenerateManifestJson: Failed to serialize JSON"));
		return false;
	}

	// 写入文件
	FString ManifestPath = FPaths::Combine(OutputDirectory, TEXT("manifest.json"));
	if (!FFileHelper::SaveStringToFile(JsonString, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogTemp, Error, TEXT("GenerateManifestJson: Failed to write manifest.json to %s"), *ManifestPath);
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("GenerateManifestJson: Successfully created manifest.json at %s"), *ManifestPath);
	return true;
}

bool UExportMeshBPLibrary::CompressDirectoryToZip(const FString& DirectoryPath, FString& OutZipPath)
{
	// 验证输入路径
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*DirectoryPath))
	{
		UE_LOG(LogTemp, Error, TEXT("CompressDirectoryToZip: Directory does not exist: %s"), *DirectoryPath);
		return false;
	}

	// 获取文件夹名称（例如：BP_WCLW_P7000）
	FString FolderName = FPaths::GetCleanFilename(DirectoryPath);

	// 构建zip文件路径：F:/SimForArt/Output/BP_WCLW_P7000/BP_WCLW_P7000.zip
	OutZipPath = FPaths::Combine(DirectoryPath, FolderName + TEXT(".zip"));

	UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: Compressing %s to %s"), *DirectoryPath, *OutZipPath);

	// 收集文件夹内所有文件（不包括zip文件本身）
	TArray<FString> FilesToCompress;
	PlatformFile.FindFilesRecursively(FilesToCompress, *DirectoryPath, nullptr);

	// 过滤掉zip文件（避免压缩自身）
	FilesToCompress.RemoveAll([&OutZipPath](const FString& FilePath) {
		return FilePath.EndsWith(TEXT(".zip"));
	});

	if (FilesToCompress.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("CompressDirectoryToZip: No files to compress in %s"), *DirectoryPath);
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: Found %d files to compress"), FilesToCompress.Num());

	// 规范化路径（统一使用反斜杠）- 在外层作用域定义以便后续使用
	FString NormalizedDirPath = DirectoryPath.Replace(TEXT("/"), TEXT("\\"));
	FString NormalizedZipPath = OutZipPath.Replace(TEXT("/"), TEXT("\\"));

	// 跨平台压缩命令
	FString Command;
	FString CommandArgs;
	int32 ReturnCode = 0;
	FString StdOut;
	FString StdErr;
	bool bSuccess = false;

#if PLATFORM_WINDOWS
	// Windows: 使用PowerShell Compress-Archive
	// 方案：创建临时批处理脚本，避免所有引号嵌套问题

	// 创建临时批处理文件路径
	FString TempBatPath = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("CompressZip.bat"));
	TempBatPath = TempBatPath.Replace(TEXT("/"), TEXT("\\"));

	// 写入批处理内容（调用PowerShell）
	FString BatContent = FString::Printf(
		TEXT("@echo off\r\n")
		TEXT("powershell -NoProfile -ExecutionPolicy Bypass -Command \"Compress-Archive -Path '%s\\*' -DestinationPath '%s' -Force\"\r\n")
		TEXT("if %%ERRORLEVEL%% EQU 0 (\r\n")
		TEXT("    echo COMPRESS_SUCCESS\r\n")
		TEXT(") else (\r\n")
		TEXT("    echo COMPRESS_FAILED\r\n")
		TEXT("    exit /b 1\r\n")
		TEXT(")\r\n"),
		*NormalizedDirPath,
		*NormalizedZipPath
	);

	if (!FFileHelper::SaveStringToFile(BatContent, *TempBatPath, FFileHelper::EEncodingOptions::ForceAnsi))
	{
		UE_LOG(LogTemp, Error, TEXT("CompressDirectoryToZip: Failed to create temp script: %s"), *TempBatPath);
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: [Windows] Executing PowerShell compression..."));
	UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: Source: %s\\*"), *NormalizedDirPath);
	UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: Target: %s"), *NormalizedZipPath);

	// 使用cmd执行批处理文件
	Command = TEXT("cmd.exe");
	CommandArgs = FString::Printf(TEXT("/C \"%s\""), *TempBatPath);

	bSuccess = FPlatformProcess::ExecProcess(
		*Command,
		*CommandArgs,
		&ReturnCode,
		&StdOut,
		&StdErr
	);

	// 清理临时脚本
	PlatformFile.DeleteFile(*TempBatPath);

	UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: ExecProcess returned: %s, ReturnCode: %d"), bSuccess ? TEXT("true") : TEXT("false"), ReturnCode);
	if (!StdOut.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: StdOut: %s"), *StdOut.TrimStartAndEnd());
	}
	if (!StdErr.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("CompressDirectoryToZip: StdErr: %s"), *StdErr.TrimStartAndEnd());
	}

	// 检查输出是否包含成功标志
	if (StdOut.Contains(TEXT("COMPRESS_SUCCESS")))
	{
		UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: Compression command completed successfully"));
	}
	else if (StdOut.Contains(TEXT("COMPRESS_FAILED")))
	{
		UE_LOG(LogTemp, Error, TEXT("CompressDirectoryToZip: Compression command failed"));
	}

#elif PLATFORM_LINUX || PLATFORM_MAC
	// Linux/Mac: 使用zip命令
	// 需要切换到目录内执行，确保压缩包内不包含父路径
	FString ZipFileName = FolderName + TEXT(".zip");
	Command = TEXT("/bin/sh");
	CommandArgs = FString::Printf(
		TEXT("-c \"cd '%s' && zip -r '%s' . -x '*.zip'\""),
		*DirectoryPath,
		*ZipFileName
	);

	UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: [Unix] Executing: %s %s"), *Command, *CommandArgs);

	bSuccess = FPlatformProcess::ExecProcess(
		*Command,
		*CommandArgs,
		&ReturnCode,
		&StdOut,
		&StdErr
	);

#else
	UE_LOG(LogTemp, Error, TEXT("CompressDirectoryToZip: Unsupported platform"));
	return false;
#endif

	if (!bSuccess || ReturnCode != 0)
	{
		UE_LOG(LogTemp, Error, TEXT("CompressDirectoryToZip: Compression command failed. ReturnCode: %d"), ReturnCode);
		UE_LOG(LogTemp, Error, TEXT("CompressDirectoryToZip: StdOut: %s"), *StdOut);
		UE_LOG(LogTemp, Error, TEXT("CompressDirectoryToZip: StdErr: %s"), *StdErr);
		return false;
	}

	// 等待文件系统同步并验证文件创建（最多等待5秒）
	// 注意：UE的FileExists()在某些环境下可能有路径格式问题，我们使用FileSize()作为备用验证
	bool bFileCreated = false;
	FString DetectedPath;

	// 尝试多种路径格式（UE的文件系统API可能对路径格式敏感）
	TArray<FString> PathsToCheck;
	PathsToCheck.Add(OutZipPath);                                      // 原始路径 (F:/...)
	PathsToCheck.Add(NormalizedZipPath);                               // Windows路径 (F:\...)
	PathsToCheck.Add(FPaths::ConvertRelativePathToFull(OutZipPath));  // 完整路径

	for (int32 i = 0; i < 50 && !bFileCreated; ++i)
	{
		for (const FString& PathToCheck : PathsToCheck)
		{
			// 尝试两种验证方法：FileExists() 或 FileSize() > 0
			if (PlatformFile.FileExists(*PathToCheck))
			{
				bFileCreated = true;
				DetectedPath = PathToCheck;
				UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: Zip file detected (FileExists) after %d ms"), i * 100);
				break;
			}
			else if (PlatformFile.FileSize(*PathToCheck) > 0)
			{
				bFileCreated = true;
				DetectedPath = PathToCheck;
				UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: Zip file detected (FileSize) after %d ms"), i * 100);
				break;
			}
		}
		if (!bFileCreated)
		{
			FPlatformProcess::Sleep(0.1f); // 等待100ms
		}
	}

	if (!bFileCreated)
	{
		UE_LOG(LogTemp, Warning, TEXT("CompressDirectoryToZip: Standard verification failed, checking directory contents..."));

		// 最后的手段：列出目录中的所有文件
		TArray<FString> FoundFiles;
		PlatformFile.FindFiles(FoundFiles, *DirectoryPath, TEXT(".zip"));

		if (FoundFiles.Num() > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: Found %d .zip files in directory"), FoundFiles.Num());
			for (const FString& FoundFile : FoundFiles)
			{
				UE_LOG(LogTemp, Log, TEXT("  - %s"), *FoundFile);
				if (FoundFile.Contains(FolderName))
				{
					bFileCreated = true;
					DetectedPath = FoundFile;
					UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: Matched expected zip file"));
					break;
				}
			}
		}
	}

	if (!bFileCreated)
	{
		UE_LOG(LogTemp, Error, TEXT("CompressDirectoryToZip: Zip file verification failed after all attempts"));
		UE_LOG(LogTemp, Error, TEXT("CompressDirectoryToZip: Expected file: %s"), *FolderName);
		UE_LOG(LogTemp, Error, TEXT("CompressDirectoryToZip: This may be a file system cache issue - check the directory manually"));
		return false;
	}

	// 获取文件大小用于日志
	int64 ZipFileSize = PlatformFile.FileSize(*DetectedPath);
	if (ZipFileSize < 0) ZipFileSize = PlatformFile.FileSize(*OutZipPath);
	if (ZipFileSize < 0) ZipFileSize = PlatformFile.FileSize(*NormalizedZipPath);

	if (ZipFileSize > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: Successfully created zip file: %s (%.2f MB)"),
			*OutZipPath, ZipFileSize / 1024.0f / 1024.0f);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: Successfully created zip file: %s"), *OutZipPath);
	}

	// 自动打开文件夹（跨平台）
	FPlatformProcess::ExploreFolder(*DirectoryPath);
	UE_LOG(LogTemp, Log, TEXT("CompressDirectoryToZip: Opened folder: %s"), *DirectoryPath);

	return true;
}

void UExportMeshBPLibrary::GetMaterialInstancesFromBlueprintByParent(
	const FAssetData& AssetData,
	UMaterialInterface* ParentMaterial,
	TArray<UMaterialInstance*>& OutMaterialInstances)
{
	OutMaterialInstances.Empty();

	// 验证ParentMaterial参数
	if (!ParentMaterial || !IsValid(ParentMaterial))
	{
		UE_LOG(LogTemp, Error, TEXT("GetMaterialInstancesFromBlueprintByParent: Invalid ParentMaterial"));
		return;
	}

	// 检查是否是Blueprint资产
	if (!AssetData.IsValid() || AssetData.AssetClassPath.GetAssetName() != FName("Blueprint"))
	{
		UE_LOG(LogTemp, Error, TEXT("GetMaterialInstancesFromBlueprintByParent: AssetData is not a Blueprint asset (Class: %s)"),
			*AssetData.AssetClassPath.ToString());
		return;
	}

	// 加载Blueprint资产
	UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
	if (!Blueprint || !IsValid(Blueprint))
	{
		UE_LOG(LogTemp, Error, TEXT("GetMaterialInstancesFromBlueprintByParent: Failed to load Blueprint from AssetData: %s"),
			*AssetData.AssetName.ToString());
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("GetMaterialInstancesFromBlueprintByParent: Searching in Blueprint '%s' for materials with parent '%s'"),
		*Blueprint->GetName(), *ParentMaterial->GetName());

	// 获取Blueprint中的所有StaticMeshComponent
	TArray<UPrimitiveComponent*> Components;
	GetComponentsFromBlueprintAsset(Blueprint, Components);

	if (Components.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("GetMaterialInstancesFromBlueprintByParent: No components found in Blueprint"));
		// 清理临时Actor
		CleanupTempActors();
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("GetMaterialInstancesFromBlueprintByParent: Found %d components to check"), Components.Num());

	// 使用Set去重
	TSet<UMaterialInstance*> UniqueMaterials;
	int32 TotalMaterialsChecked = 0;

	// 遍历所有组件
	for (UPrimitiveComponent* Component : Components)
	{
		UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(Component);
		if (!StaticMeshComp)
		{
			continue;
		}

		UStaticMesh* StaticMesh = StaticMeshComp->GetStaticMesh();
		if (!StaticMesh || !IsValid(StaticMesh))
		{
			continue;
		}

		// 获取StaticMesh的静态材质数量
		const int32 NumMaterials = StaticMesh->GetStaticMaterials().Num();

		UE_LOG(LogTemp, Verbose, TEXT("  Component '%s' - StaticMesh '%s' has %d material slots"),
			*StaticMeshComp->GetName(), *StaticMesh->GetName(), NumMaterials);

		// 遍历所有材质插槽
		for (int32 MaterialIndex = 0; MaterialIndex < NumMaterials; ++MaterialIndex)
		{
			// 优先检查组件覆盖的材质，如果没有则使用StaticMesh的默认材质
			UMaterialInterface* Material = StaticMeshComp->GetMaterial(MaterialIndex);
			if (!Material || !IsValid(Material))
			{
				Material = StaticMesh->GetMaterial(MaterialIndex);
			}

			if (!Material || !IsValid(Material))
			{
				continue;
			}

			TotalMaterialsChecked++;

			// 检查是否是MaterialInstance
			UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(Material);
			if (!MaterialInstance)
			{
				// 不是MaterialInstance，跳过
				continue;
			}

			// 检查Parent是否匹配
			UMaterialInterface* Parent = MaterialInstance->Parent;
			if (Parent == ParentMaterial)
			{
				// 找到匹配的材质实例
				if (!UniqueMaterials.Contains(MaterialInstance))
				{
					UniqueMaterials.Add(MaterialInstance);
					UE_LOG(LogTemp, Log, TEXT("    ✓ Found matching material: %s (Parent: %s)"),
						*MaterialInstance->GetName(), *Parent->GetName());
				}
			}
			else
			{
				// 递归检查Parent的Parent（支持多层材质实例链）
				UMaterialInstance* ParentInstance = Cast<UMaterialInstance>(Parent);
				while (ParentInstance)
				{
					if (ParentInstance->Parent == ParentMaterial)
					{
						if (!UniqueMaterials.Contains(MaterialInstance))
						{
							UniqueMaterials.Add(MaterialInstance);
							UE_LOG(LogTemp, Log, TEXT("    ✓ Found matching material (indirect): %s (Parent chain: %s -> %s)"),
								*MaterialInstance->GetName(), *Parent->GetName(), *ParentMaterial->GetName());
						}
						break;
					}
					ParentInstance = Cast<UMaterialInstance>(ParentInstance->Parent);
				}
			}
		}
	}

	// 转换Set到数组
	OutMaterialInstances = UniqueMaterials.Array();

	UE_LOG(LogTemp, Log, TEXT("GetMaterialInstancesFromBlueprintByParent: Checked %d materials, found %d unique material instances"),
		TotalMaterialsChecked, OutMaterialInstances.Num());

	// 清理临时Actor（由GetComponentsFromBlueprintAsset创建）
	CleanupTempActors();
	UE_LOG(LogTemp, Log, TEXT("GetMaterialInstancesFromBlueprintByParent: Cleaned up temporary actors"));
}

int32 UExportMeshBPLibrary::SetMaterialInstancesParent(
	const TArray<UMaterialInstance*>& MaterialInstances,
	UMaterialInterface* NewParentMaterial)
{
	// 验证新父材质参数
	if (!NewParentMaterial || !IsValid(NewParentMaterial))
	{
		UE_LOG(LogTemp, Error, TEXT("SetMaterialInstancesParent: Invalid NewParentMaterial"));
		return 0;
	}

	if (MaterialInstances.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("SetMaterialInstancesParent: MaterialInstances array is empty"));
		return 0;
	}

	UE_LOG(LogTemp, Log, TEXT("SetMaterialInstancesParent: Replacing parent material for %d material instances with '%s'"),
		MaterialInstances.Num(), *NewParentMaterial->GetName());

	int32 SuccessCount = 0;
	int32 SkippedCount = 0;
	int32 FailedCount = 0;

	// 创建材质更新上下文，自动处理所有使用这些材质的组件重新注册
	// 这会确保场景中使用这些材质的对象在修改完成前不会被渲染
	TArray<UMaterialInterface*> MaterialsToUpdate;
	for (UMaterialInstance* MaterialInstance : MaterialInstances)
	{
		if (MaterialInstance && IsValid(MaterialInstance))
		{
			MaterialsToUpdate.Add(MaterialInstance);
		}
	}

	// 使用FMaterialUpdateContext自动管理材质更新
	FMaterialUpdateContext MaterialUpdateContext(FMaterialUpdateContext::EOptions::Default, GMaxRHIShaderPlatform);
	for (UMaterialInterface* Material : MaterialsToUpdate)
	{
		MaterialUpdateContext.AddMaterialInterface(Material);
	}

	for (UMaterialInstance* MaterialInstance : MaterialInstances)
	{
		// 验证材质实例
		if (!MaterialInstance || !IsValid(MaterialInstance))
		{
			SkippedCount++;
			UE_LOG(LogTemp, Warning, TEXT("  ✗ Skipped invalid material instance"));
			continue;
		}

		// 获取当前Parent
		UMaterialInterface* OldParent = MaterialInstance->Parent;
		FString OldParentName = OldParent ? OldParent->GetName() : TEXT("None");

		// 检查是否需要替换（避免重复设置）
		if (OldParent == NewParentMaterial)
		{
			SkippedCount++;
			UE_LOG(LogTemp, Verbose, TEXT("  ○ Skipped '%s' (already has this parent)"), *MaterialInstance->GetName());
			continue;
		}

		// 标记对象即将修改（用于Undo/Redo）
		MaterialInstance->Modify();

		// 获取Parent属性
		FProperty* ParentProperty = FindFProperty<FProperty>(UMaterialInstance::StaticClass(), TEXT("Parent"));
		if (!ParentProperty)
		{
			FailedCount++;
			UE_LOG(LogTemp, Error, TEXT("  ✗ Failed to find Parent property for '%s'"), *MaterialInstance->GetName());
			continue;
		}

		// 通知即将修改属性（PreEditChange）
		MaterialInstance->PreEditChange(ParentProperty);

		// 设置新的Parent材质（直接赋值）
		MaterialInstance->Parent = NewParentMaterial;

		// 构建属性变更事件
		FPropertyChangedEvent PropertyChangedEvent(ParentProperty, EPropertyChangeType::ValueSet);

		// 通知编辑器属性已修改（PostEditChangeProperty）
		MaterialInstance->PostEditChangeProperty(PropertyChangedEvent);

		// 标记包为修改状态（使其可保存）
		MaterialInstance->MarkPackageDirty();

		SuccessCount++;
		UE_LOG(LogTemp, Log, TEXT("  ✓ '%s': Parent changed from '%s' to '%s'"),
			*MaterialInstance->GetName(), *OldParentName, *NewParentMaterial->GetName());
	}

	// 批量修改完成后，刷新渲染命令队列，确保渲染线程资源状态同步
	if (SuccessCount > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("SetMaterialInstancesParent: Flushing rendering commands..."));
		FlushRenderingCommands();
	}

	// 总结日志
	UE_LOG(LogTemp, Log, TEXT("SetMaterialInstancesParent: Completed - Success: %d, Skipped: %d, Failed: %d"),
		SuccessCount, SkippedCount, FailedCount);

	if (SuccessCount > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("SetMaterialInstancesParent: %d material instance(s) modified. Please save the assets."), SuccessCount);
	}

	return SuccessCount;
}




