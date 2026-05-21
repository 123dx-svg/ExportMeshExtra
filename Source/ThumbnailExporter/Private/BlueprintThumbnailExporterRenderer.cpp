// Copyright 2023 Big Cat Energising. All Rights Reserved.


#include "BlueprintThumbnailExporterRenderer.h"

#include "ThumbnailExporterSettings.h"
#include "ThumbnailExporterThumbnailDummy.h"
#include "ThumbnailExporterScene.h"
#include "CanvasTypes.h"
#include "EngineUtils.h"

#include "RendererInterface.h"
#include "EngineModule.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "LegacyScreenPercentageDriver.h"
#include "RenderingThread.h"
#include "RenderGraphBuilder.h"

#include "CanvasItem.h"

UBlueprintThumbnailExporterRenderer::UBlueprintThumbnailExporterRenderer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{

}

UBlueprintThumbnailExporterRenderer::~UBlueprintThumbnailExporterRenderer()
{

}

static ESimpleElementBlendMode GetMaterialBlendMode(const UMaterialInterface* Material)
{
	switch (Material->GetBlendMode())
	{
	case EBlendMode::BLEND_Additive:
		return ESimpleElementBlendMode::SE_BLEND_Additive;

	case EBlendMode::BLEND_Translucent:
		return ESimpleElementBlendMode::SE_BLEND_Translucent;

	default:
	case EBlendMode::BLEND_Opaque:
		return ESimpleElementBlendMode::SE_BLEND_Opaque;
	}
}

void UBlueprintThumbnailExporterRenderer::DrawThumbnailWithConfig(FThumbnailCreationParams& CreationParams)
{
	bool bCanRender = false;
	UBlueprint* Blueprint = Cast<UBlueprint>(CreationParams.Object);

	FThumbnailExporterScene* ThumbnailScene = &GetThumbnailScene(CreationParams.CreationConfig);

	// Strict validation - it may hopefully fix UE-35705.
	const bool bIsBlueprintValid = IsValid(Blueprint)
		&& IsValid(Blueprint->GeneratedClass)
		&& Blueprint->bHasBeenRegenerated
		//&& Blueprint->IsUpToDate() - This condition makes the thumbnail blank whenever the BP is dirty. It seems too strict.
		&& !Blueprint->bBeingCompiled
		&& !Blueprint->HasAnyFlags(RF_Transient);
	if (bIsBlueprintValid)
	{
		ThumbnailScene->SetBlueprint(Blueprint);

		bCanRender = true;
	}

	//UMaterialInterface* Material = Cast<UMaterialInterface>(CreationParams.Object);
	//if (IsValid(Material))
	//{
	//	Material->EnsureIsComplete();
	//	
	//	FCanvasTileItem TileItem(FVector2D(0, 0), Material->GetRenderProxy(), CreationParams.GetThumbnailSize());
	//	TileItem.BlendMode = GetMaterialBlendMode(Material);
	//	TileItem.SetColor(CreationParams.CreationConfig.ThumbnailBackground);
	//	TileItem.Draw(CreationParams.Canvas);
	//	return;
	//}

	UStaticMesh* StaticMesh = Cast<UStaticMesh>(CreationParams.Object);
	if (IsValid(StaticMesh))
	{
		ThumbnailScene->SetStaticMesh(StaticMesh);
		ThumbnailScene->GetScene()->UpdateSpeedTreeWind(0.0);

		bCanRender = true;
	}

	USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(CreationParams.Object);
	if (IsValid(SkeletalMesh))
	{
		ThumbnailScene->SetSkeletalMesh(SkeletalMesh);
		bCanRender = true;
	}

	if (bCanRender)
	{
		if (CreationParams.CreationDelegate.IsBound())
		{
			CreationParams.CreationConfig = CreationParams.CreationDelegate.Execute(CreationParams.CreationConfig, ThumbnailScene->GetPreviewActor().Get());
			ThumbnailScene->SetThumbnailCreationConfig(CreationParams.CreationConfig);
		}
		else
		{
			ThumbnailScene->SetThumbnailCreationConfig(CreationParams.CreationConfig);
		}

		ThumbnailScene->UpdatePreviewActorComponents();

		FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(CreationParams.RenderTarget, ThumbnailScene->GetScene(), FEngineShowFlags(ESFIM_Game))
			.SetTime(UThumbnailRenderer::GetTime())
			.SetDeferClear(true)
			.SetResolveScene(false)
			.SetAdditionalViewFamily(CreationParams.bAdditionalViewFamily));

		ViewFamily.bThumbnailRendering = true;

		if (CreationParams.bIsAlpha)
		{
			ViewFamily.EngineShowFlags.DisableAdvancedFeatures();
			ViewFamily.EngineShowFlags.SetScreenPercentage(false);
			ViewFamily.EngineShowFlags.MotionBlur = 0;
			ViewFamily.EngineShowFlags.Fog = 0;
			ViewFamily.EngineShowFlags.DepthOfField = 0;
			ViewFamily.EngineShowFlags.LocalExposure = 0;
			ViewFamily.EngineShowFlags.Vignette = 0;
			ViewFamily.EngineShowFlags.Grain = 0;
			ViewFamily.EngineShowFlags.Atmosphere = 0;
			ViewFamily.EngineShowFlags.LOD = 0;
			ViewFamily.EngineShowFlags.AntiAliasing = 0;
			ViewFamily.EngineShowFlags.PostProcessMaterial = 0;
			ViewFamily.EngineShowFlags.Translucency = 1;
			ViewFamily.EngineShowFlags.SeparateTranslucency = 0;
			ViewFamily.EngineShowFlags.Refraction = 1;
			ViewFamily.EngineShowFlags.Tonemapper = 0;
			ViewFamily.EngineShowFlags.ColorGrading = 0;
			ViewFamily.EngineShowFlags.IndirectLightingCache = 0;
			ViewFamily.EngineShowFlags.PostProcessing = false;
			ViewFamily.EngineShowFlags.Bloom = false;
			ViewFamily.bIsHDR = true;

			ViewFamily.SceneCaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
			ViewFamily.SceneCaptureCompositeMode = ESceneCaptureCompositeMode::SCCM_Overwrite;
		}
		else
		{
			// Only disable advanced features if reflections are not enabled
			// 只有在不需要反射时才禁用高级特性
			if (!CreationParams.CreationConfig.bEnableReflections)
			{
				ViewFamily.EngineShowFlags.DisableAdvancedFeatures();
			}

			ViewFamily.EngineShowFlags.MotionBlur = 0;

			// Resolve scene is needed for LDR rendering, no idea why
			ViewFamily.bResolveScene = true;

			ViewFamily.EngineShowFlags.SetScreenPercentage(false);
			ViewFamily.EngineShowFlags.Fog = 0;
			ViewFamily.EngineShowFlags.DepthOfField = 0;
			ViewFamily.EngineShowFlags.LocalExposure = 0;
			ViewFamily.EngineShowFlags.Vignette = 0;
			ViewFamily.EngineShowFlags.Grain = 0;
			ViewFamily.EngineShowFlags.Atmosphere = 0;
			ViewFamily.EngineShowFlags.LOD = 0;
			ViewFamily.EngineShowFlags.AntiAliasing = CreationParams.CreationConfig.bEnableAntiAliasing ? 1 : 0;
			ViewFamily.EngineShowFlags.PostProcessMaterial = 0;
			ViewFamily.EngineShowFlags.Translucency = 1;
			ViewFamily.EngineShowFlags.SeparateTranslucency = 1;
			ViewFamily.EngineShowFlags.Refraction = 1;
			//ViewFamily.EngineShowFlags.Tonemapper = 1;
			//ViewFamily.EngineShowFlags.ColorGrading = 1;
			//ViewFamily.EngineShowFlags.IndirectLightingCache = 0;
			ViewFamily.EngineShowFlags.PostProcessing = CreationParams.CreationConfig.bEnablePostProcessing;
			ViewFamily.EngineShowFlags.Bloom = CreationParams.CreationConfig.bEnableBloom;

			// Enable reflections for glass/metal materials
			// 为玻璃/金属材质启用反射
			if (CreationParams.CreationConfig.bEnableReflections)
			{
				ViewFamily.EngineShowFlags.Lighting = 1;
				ViewFamily.EngineShowFlags.GlobalIllumination = 1;
				ViewFamily.EngineShowFlags.ReflectionEnvironment = 1;
				ViewFamily.EngineShowFlags.ScreenSpaceReflections = CreationParams.CreationConfig.bEnableScreenSpaceReflections ? 1 : 0;
				ViewFamily.EngineShowFlags.AmbientOcclusion = 1;
				ViewFamily.EngineShowFlags.DynamicShadows = 1;
			}

			ViewFamily.SceneCaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
			ViewFamily.SceneCaptureCompositeMode = ESceneCaptureCompositeMode::SCCM_Overwrite;
		}

		FSceneView* View = ThumbnailScene->CreateView(&ViewFamily, 0, 0, CreationParams.Width, CreationParams.Height);
		View->BackgroundColor = CreationParams.CreationConfig.GetAdjustedBackgroundColor();

		// 曝光锁定：使用手动 EV 偏移，禁用自动曝光，避免不同资产的明暗漂移。
		if (!CreationParams.bIsAlpha && CreationParams.CreationConfig.bLockExposure)
		{
			FFinalPostProcessSettings& PP = View->FinalPostProcessSettings;
			PP.bOverride_AutoExposureMethod = true;
			PP.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
			PP.bOverride_AutoExposureBias = true;
			PP.AutoExposureBias = CreationParams.CreationConfig.ManualExposureBias;
			PP.bOverride_AutoExposureMinBrightness = true;
			PP.AutoExposureMinBrightness = 1.0f;
			PP.bOverride_AutoExposureMaxBrightness = true;
			PP.AutoExposureMaxBrightness = 1.0f;
			ViewFamily.EngineShowFlags.EyeAdaptation = 0;
		}

		RenderViewFamily(CreationParams.Canvas, &ViewFamily, View);

		// If we used a creation delegate, then delete the scene.
		// The scene can be messed up by the creation delegate, so its better to just recreate it
		if (CreationParams.CreationDelegate.IsBound())
		{
			for (FThumbnailExporterScene* DestroyThumbnailScene : ThumbnailScenes)
			{
				if (DestroyThumbnailScene == ThumbnailScene)
				{
					delete DestroyThumbnailScene;
					ThumbnailScenes.Remove(ThumbnailScene);
					break;
				}
			}
		}
	}
}

bool UBlueprintThumbnailExporterRenderer::CanVisualizeAsset(UObject* Object)
{
	if (Cast<UThumbnailExporterThumbnailDummy>(Object))
	{
		return true;
	}

	if (Cast<UStaticMesh>(Object))
	{
		return true;
	}

	if (Cast<USkeletalMesh>(Object))
	{
		return true;
	}

	//if (Cast<UMaterialInterface>(Object))
	//{
	//	return true;
	//}

	return Super::CanVisualizeAsset(Object);
}

void UBlueprintThumbnailExporterRenderer::BeginDestroy()
{
	for (FThumbnailExporterScene* ThumbnailScene : ThumbnailScenes)
	{
		delete ThumbnailScene;
	}
	ThumbnailScenes.Empty();

	Super::BeginDestroy();
}

void UBlueprintThumbnailExporterRenderer::ResetThumbnailScenes()
{
	for (FThumbnailExporterScene* ThumbnailScene : ThumbnailScenes)
	{
		delete ThumbnailScene;
	}
	ThumbnailScenes.Empty();
}

void UBlueprintThumbnailExporterRenderer::RenderViewFamily(FCanvas* Canvas, FSceneViewFamily* ViewFamily, FSceneView* View)
{
	if ((ViewFamily == nullptr) || (View == nullptr))
	{
		return;
	}

	check(ViewFamily->Views.Num() == 1 && (ViewFamily->Views[0] == View));

	ViewFamily->EngineShowFlags.ScreenPercentage = false;
	ViewFamily->bThumbnailRendering = true;
	if (ViewFamily->GetScreenPercentageInterface() == nullptr)
	{
		ViewFamily->SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(
			*ViewFamily, /* GlobalResolutionFraction = */ 1.0f));
	}

	GetRendererModule().BeginRenderingViewFamily(Canvas, ViewFamily);
}

FThumbnailExporterScene& UBlueprintThumbnailExporterRenderer::GetThumbnailScene(const FThumbnailCreationConfig& CreationConfig)
{
	for (FThumbnailExporterScene* ThumbnailScene : ThumbnailScenes)
	{
		
		ThumbnailScene->SetThumbnailCreationConfig(CreationConfig);
		if (ThumbnailScene->GetBackgroundMeshesHidden() == CreationConfig.bHideThumbnailBackgroundMeshes)
		{
			return *ThumbnailScene;
		}
	}

	return *ThumbnailScenes.Add_GetRef(new FThumbnailExporterScene(CreationConfig));
}
