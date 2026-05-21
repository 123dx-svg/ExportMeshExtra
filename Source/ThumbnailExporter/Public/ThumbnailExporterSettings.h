// Copyright 2023 Big Cat Energising. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Containers/Map.h"
#include "ThumbnailExporterSettings.generated.h"

USTRUCT(BlueprintType)
struct FThumbnailCreationConfig
{
	GENERATED_USTRUCT_BODY()

	// If true, then the thumbnail will not be placed in the same folder as the asset, and instead will be placed in ThumbnailOverridePath
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Filename")
		bool bOverrideThumbnailPath = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Filename", meta = (EditCondition="bOverrideThumbnailPath", ContentDir))
		FDirectoryPath ThumbnailOverridePath;

	// If true, then the thumbnail will not use the Prefix + AssetName + Suffix for the thumbnail filename, but will instead use ThumbnailOverrideFilename
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Filename")
		bool bOverrideThumbnailFilename = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Filename", meta = (EditCondition = "bOverrideThumbnailFilename"))
		FString ThumbnailOverrideFilename;

	// Prefix added to the beginning of the generated thumbnail
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Filename", meta = (EditCondition = "!bOverrideThumbnailFilename"))
		FString ThumbnailPrefix = "T_";

	// Suffixs added to the end of the generated thumbnail
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Filename", meta = (EditCondition = "!bOverrideThumbnailFilename"))
		FString ThumbnailSuffix = "_Icon";

	// Exported thumbnail size, in pixels
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Texture", meta = (ClampMin = 256, UIMin = 256))
		int32 ThumbnailSize = 256;

	// 超采样倍数：以 ThumbnailSize * N 的分辨率渲染，再下采样到目标尺寸，可显著缓解锯齿。
	// 1 = 不超采样；2~4 推荐；>4 性能开销大。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Texture", meta = (ClampMin = 1, ClampMax = 8, UIMin = 1, UIMax = 8))
		int32 SuperSampleScale = 2;

	// 是否启用引擎的 AntiAliasing show flag（与超采样可叠加）。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Texture")
		bool bEnableAntiAliasing = true;

	// 下采样后是否做一次轻微 Unsharp 锐化，弥补 box 下采样略软的问题。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Texture")
		bool bEnableMildSharpen = false;

	// 锐化强度（0 = 无效，1 = 中等，2 = 较强）。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Texture", meta = (EditCondition = "bEnableMildSharpen", ClampMin = 0.0, ClampMax = 2.0, UIMin = 0.0, UIMax = 2.0))
		float SharpenAmount = 0.25f;

	// If true, the output image will be cropped to the visible bounds on the X axis (left and right)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Texture")
		bool bAutoThumbnailSizeX = false;

	// If true, the output image will be cropped to the visible bounds on the Y axis (top and bottom)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Texture")
		bool bAutoThumbnailSizeY = false;

	// If true, un-premultiply RGB by alpha to preserve translucent details (e.g. glass)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Texture")
		bool bUnpremultiplyAlpha = true;

	// Use "SceneColor (HDR) in RGB, Inv Opacity in A" for transparency
	//UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = Thumbnail)
		TEnumAsByte<ESceneCaptureSource> ThumbnailCaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;

	//UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = Thumbnail)
		TEnumAsByte<ESceneCaptureCompositeMode> ThumbnailCompositeMode = ESceneCaptureCompositeMode::SCCM_Composite;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Texture")
		FLinearColor ThumbnailBackground = FLinearColor::Transparent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Texture")
		TEnumAsByte<TextureGroup> ThumbnailTextureGroup = TextureGroup::TEXTUREGROUP_UI;

	// Hide the background meshes present in the asset thumbnail. Hides the checkerboard background
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Scene")
		bool bHideThumbnailBackgroundMeshes = true;

	// Enable post-processing (like bloom) in the thumbnail
	// 
	// If transparent backgrounds are not working, try toggling off post-processing
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Scene")
		bool bEnablePostProcessing = true;

	// Enable bloom in the thumbnail
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Scene", meta = (EditCondition = "bEnablePostProcessing"))
		bool bEnableBloom = false;

	// Enable reflections in the thumbnail (useful for glass/metal materials)
	// 启用反射效果（玻璃、金属材质需要开启此选项）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Scene")
		bool bEnableReflections = false;

	// Enable screen space reflections for more accurate glass/metal reflections
	// 启用屏幕空间反射以获得更准确的玻璃/金属反射效果
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Scene", meta = (EditCondition = "bEnableReflections"))
		bool bEnableScreenSpaceReflections = false;

	// Intensity multiplier for sky light (affects reflections on glass/metal)
	// 天空光强度倍数（影响玻璃/金属的反射效果）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Scene", meta = (EditCondition = "bEnableReflections", ClampMin = 0.1, ClampMax = 10.0, UIMin = 0.1, UIMax = 10.0))
		float SkyLightIntensity = 1.0f;

	// 锁定曝光：使用手动曝光偏移，避免不同资产由于自动曝光导致明暗不一致。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Scene")
		bool bLockExposure = false;

	// 手动曝光偏移值（仅在 bLockExposure 启用时生效，单位 EV）。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Scene", meta = (EditCondition = "bLockExposure", ClampMin = -5.0, ClampMax = 5.0, UIMin = -5.0, UIMax = 5.0))
		float ManualExposureBias = 0.0f;

	// 启用 3 点光近似：在原默认光的基础上补充主光/补光/轮廓光，让模型更有立体感。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Scene")
		bool bUseThreePointLighting = false;

	// 主光强度（Key Light）。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Scene", meta = (EditCondition = "bUseThreePointLighting", ClampMin = 0.0, ClampMax = 20.0, UIMin = 0.0, UIMax = 20.0))
		float KeyLightIntensity = 6.0f;

	// 补光强度（Fill Light）。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Scene", meta = (EditCondition = "bUseThreePointLighting", ClampMin = 0.0, ClampMax = 20.0, UIMin = 0.0, UIMax = 20.0))
		float FillLightIntensity = 1.6f;

	// 轮廓光强度（Rim/Back Light）。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Scene", meta = (EditCondition = "bUseThreePointLighting", ClampMin = 0.0, ClampMax = 20.0, UIMin = 0.0, UIMax = 20.0))
		float RimBoost = 0.8f;

	// If true, export a PNG to a local folder instead of creating a texture asset
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Export")
		bool bExportToLocal = false;

	// If true, then when the thumbnail texture is created, a notification will pop up with a link to the texture
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = Thumbnail)
		bool bCreateThumbnailNotification = true;

	//如果启用则使用自定义缩略图视角
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Scene")
	bool bOverrideViewAngle = false;
	//自定义缩略图视角
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Scene", meta = (EditCondition = "bOverrideViewAngle"))
	float OrbitPitch = 45.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail|Scene", meta = (EditCondition = "bOverrideViewAngle"))
	float OrbitYaw = 0.0f;
	

	FLinearColor GetAdjustedBackgroundColor() const
	{
		// Invert the background alpha so we can match the inverted alpha of the scene capture
		if (InvertBackgroundAlpha())
		{
			return ThumbnailBackground.CopyWithNewOpacity(1.0 - ThumbnailBackground.A);
		}
		else
		{
			return ThumbnailBackground;
		}
	}

	bool InvertBackgroundAlpha() const
	{
		return ThumbnailCaptureSource == ESceneCaptureSource::SCS_SceneColorHDR;
	}
};

USTRUCT(BlueprintType)
struct FThumbnailCreationPreset
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail Creation Preset")
		FText MenuItemName = FText::FromString("Export to Texture");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail Creation Preset")
		FText MenuItemTooltip = FText::FromString("Export an asset's thumbnail to a texture. The export preset is defined in the Thumbnail Exporter config in the project settings.");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail Creation Preset", meta = (FullyExpand=true))
		FThumbnailCreationConfig PresetConfig;
};

/**
 * 
 */
UCLASS(config = Editor, defaultconfig, DisplayName = "Thumbnail Exporter")
class THUMBNAILEXPORTER_API UThumbnailExporterSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Thumbnail Exporter Settings", meta = (TitleProperty = "{MenuItemName}", ShowOnlyInnerProperties))
		TArray<FThumbnailCreationPreset> ThumbnailCreationPresets = { FThumbnailCreationPreset() };

	static UThumbnailExporterSettings* Get() { return GetMutableDefault<UThumbnailExporterSettings>(); }
};
