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
