// Copyright 2023 Big Cat Energising. All Rights Reserved.


#include "ThumbnailExporter.h"

#include "ContentBrowserModule.h"
#include "ObjectTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopedSlowTask.h"
#include "Containers/Ticker.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IContentBrowserSingleton.h"
#include "ThumbnailExporterSettings.h"
#include "ThumbnailExporterRenderer.h"
#include "BlueprintThumbnailExporterRenderer.h"
#include "RenderingThread.h"
#include "ThumbnailExporterThumbnailDummy.h"

#define LOCTEXT_NAMESPACE "FThumbnailExporterModule"

namespace
{
	FString GLocalExportDirectoryOverride;
	FString GLastLocalExportDirectory;

	struct FScopedLocalExportDirectoryOverride
	{
		FString Previous;

		explicit FScopedLocalExportDirectoryOverride(const FString& InOverride)
			: Previous(GLocalExportDirectoryOverride)
		{
			GLocalExportDirectoryOverride = InOverride;
		}

		~FScopedLocalExportDirectoryOverride()
		{
			GLocalExportDirectoryOverride = Previous;
		}
	};

	static bool ChooseLocalExportDirectory(FString& OutDirectory)
	{
		IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
		if (!DesktopPlatform)
		{
			return false;
		}

		void* ParentWindowHandle = nullptr;
		if (FSlateApplication::IsInitialized())
		{
			ParentWindowHandle = const_cast<void*>(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr));
		}

		const FString Title = TEXT("Select Thumbnail Export Folder");
		const FString DefaultPath = GLastLocalExportDirectory.IsEmpty() ? FPaths::ProjectDir() : GLastLocalExportDirectory;
		if (!DesktopPlatform->OpenDirectoryDialog(ParentWindowHandle, Title, DefaultPath, OutDirectory))
		{
			return false;
		}

		if (OutDirectory.IsEmpty())
		{
			return false;
		}

		OutDirectory = FPaths::ConvertRelativePathToFull(OutDirectory);
		GLastLocalExportDirectory = OutDirectory;
		return true;
	}

	static bool ResolveLocalExportDirectory(FString& OutDirectory, const FThumbnailCreationConfig* CreationConfig = nullptr)
	{
		if (!GLocalExportDirectoryOverride.IsEmpty())
		{
			OutDirectory = GLocalExportDirectoryOverride;
			return true;
		}

		// 如果配置了 ThumbnailOverridePath 且本地导出模式，使用配置的路径
		if (CreationConfig && CreationConfig->bOverrideThumbnailPath && !CreationConfig->ThumbnailOverridePath.Path.IsEmpty())
		{
			OutDirectory = FPaths::ConvertRelativePathToFull(CreationConfig->ThumbnailOverridePath.Path);
			GLastLocalExportDirectory = OutDirectory;
			UE_LOG(LogTemp, Log, TEXT("ThumbnailExporter - Using configured override path: %s"), *OutDirectory);
			return true;
		}

		return ChooseLocalExportDirectory(OutDirectory);
	}

	static FString GetThumbnailBaseFilename(const FThumbnailCreationConfig& CreationConfig, const FAssetData& Asset)
	{
		if (CreationConfig.bOverrideThumbnailFilename)
		{
			const FString Base = FPaths::GetBaseFilename(CreationConfig.ThumbnailOverrideFilename);
			if (!Base.IsEmpty())
			{
				return Base;
			}
		}

		UObject* AssetObject = Asset.GetAsset();
		const FString AssetPath = AssetObject ? AssetObject->GetPathName() : FString();
		return CreationConfig.ThumbnailPrefix + FPaths::GetBaseFilename(AssetPath) + CreationConfig.ThumbnailSuffix;
	}

	static bool CreatePngData(const FObjectThumbnail& Thumbnail, TArray<uint8>& OutPngData)
	{
		const TArray<uint8>& RawData = Thumbnail.GetUncompressedImageData();
		if (RawData.Num() == 0)
		{
			return false;
		}

		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!ImageWrapper.IsValid())
		{
			return false;
		}

		if (!ImageWrapper->SetRaw(RawData.GetData(), RawData.Num(), Thumbnail.GetImageWidth(), Thumbnail.GetImageHeight(), ERGBFormat::BGRA, 8))
		{
			return false;
		}

		OutPngData = ImageWrapper->GetCompressed(100);
		return OutPngData.Num() > 0;
	}

	struct FBatchExportState
	{
		enum class EStage
		{
			LoadAsset,
			WarmupRender,
			Export
		};

		TArray<FAssetData> Assets;
		FThumbnailCreationConfig Config;
		FString LocalExportDirectory;
		FString PreviousLocalExportOverride;
		bool bExportToLocal = false;
		bool bAnyExported = false;
		int32 CurrentIndex = 0;
		EStage Stage = EStage::LoadAsset;
		int32 FramesUntilExport = 0;
		FAssetData PendingAsset;
		TUniquePtr<FScopedSlowTask> SlowTask;
	};

	TUniquePtr<FBatchExportState> GBatchExportState;
	FTSTicker::FDelegateHandle GBatchExportTickerHandle;

	static void CleanupBatchExport(bool bCompleted)
	{
		if (GBatchExportTickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GBatchExportTickerHandle);
			GBatchExportTickerHandle.Reset();
		}

		if (GBatchExportState)
		{
			if (GBatchExportState->bExportToLocal && bCompleted && GBatchExportState->bAnyExported && !GBatchExportState->LocalExportDirectory.IsEmpty())
			{
				FPlatformProcess::ExploreFolder(*GBatchExportState->LocalExportDirectory);
			}

			GLocalExportDirectoryOverride = GBatchExportState->PreviousLocalExportOverride;
			GBatchExportState.Reset();
		}
	}

	static bool ProcessBatchExport(float)
	{
		if (!GBatchExportState)
		{
			return false;
		}

		FBatchExportState& State = *GBatchExportState;
		if (State.SlowTask && State.SlowTask->ShouldCancel())
		{
			CleanupBatchExport(false);
			return false;
		}

		if (State.Stage == FBatchExportState::EStage::LoadAsset)
		{
			if (!State.Assets.IsValidIndex(State.CurrentIndex))
			{
				CleanupBatchExport(true);
				return false;
			}

			State.PendingAsset = State.Assets[State.CurrentIndex];
			State.FramesUntilExport = 1;
			State.Stage = FBatchExportState::EStage::WarmupRender;
			State.PendingAsset.GetAsset();
			return true;
		}

		if (State.FramesUntilExport > 0)
		{
			--State.FramesUntilExport;
			return true;
		}

		const FAssetData Asset = State.PendingAsset;

		if (State.Stage == FBatchExportState::EStage::WarmupRender)
		{
			UObject* AssetObject = Asset.GetAsset();
			if (AssetObject && FThumbnailExporterModule::CanCreateThumbnail({ Asset }))
			{
				FObjectThumbnail WarmupThumbnail;
				FThumbnailExporterRenderer::RenderThumbnail(
					State.Config,
					AssetObject,
					State.Config.ThumbnailSize,
					State.Config.ThumbnailSize,
					ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush,
					&WarmupThumbnail);
			}

			State.FramesUntilExport = 1;
			State.Stage = FBatchExportState::EStage::Export;
			return true;
		}

		if (State.Stage == FBatchExportState::EStage::Export)
		{
			if (State.SlowTask)
			{
				State.SlowTask->EnterProgressFrame(1.0f, FText::Format(LOCTEXT("ThumbnailExporterProgressAsset", "Exporting {0}"), FText::FromName(Asset.AssetName)));
			}

			FString OutFilePath;
			if (FThumbnailExporterModule::CanCreateThumbnail({ Asset }) && FThumbnailExporterModule::ExportThumbnail(State.Config, Asset, OutFilePath))
			{
				State.bAnyExported = true;
			}

			++State.CurrentIndex;
			State.Stage = FBatchExportState::EStage::LoadAsset;
			return true;
		}

		return true;
	}

	static void StartBatchExport(const TArray<FAssetData>& SelectedAssets, const FThumbnailCreationConfig& PresetConfig, const FString& LocalExportDirectory)
	{
		if (GBatchExportState)
		{
			CleanupBatchExport(false);
		}

		GBatchExportState = MakeUnique<FBatchExportState>();
		GBatchExportState->Assets = SelectedAssets;
		GBatchExportState->Config = PresetConfig;
		GBatchExportState->LocalExportDirectory = LocalExportDirectory;
		GBatchExportState->bExportToLocal = PresetConfig.bExportToLocal;
		GBatchExportState->PreviousLocalExportOverride = GLocalExportDirectoryOverride;

		if (PresetConfig.bExportToLocal && !LocalExportDirectory.IsEmpty())
		{
			GLocalExportDirectoryOverride = LocalExportDirectory;
		}

		GBatchExportState->SlowTask = MakeUnique<FScopedSlowTask>(SelectedAssets.Num(), LOCTEXT("ThumbnailExporterProgress", "Exporting thumbnails..."));
		GBatchExportState->SlowTask->MakeDialog(true);

		GBatchExportTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&ProcessBatchExport));
	}
}

void FThumbnailExporterModule::StartupModule()
{
	UThumbnailManager::Get().RegisterCustomRenderer(UThumbnailExporterThumbnailDummy::StaticClass(), UBlueprintThumbnailExporterRenderer::StaticClass());

	AddContentBrowserContextMenuExtender();
}

void FThumbnailExporterModule::ShutdownModule()
{
	RemoveContentBrowserContextMenuExtender();
}

void FThumbnailExporterModule::AddContentBrowserContextMenuExtender()
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	TArray<FContentBrowserMenuExtender_SelectedAssets>& CBMenuAssetExtenderDelegates = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();

	CBMenuAssetExtenderDelegates.Add(FContentBrowserMenuExtender_SelectedAssets::CreateStatic(&OnExtendContentBrowserAssetSelectionMenu));
	ContentBrowserExtenderDelegateHandle = CBMenuAssetExtenderDelegates.Last().GetHandle();
}

void FThumbnailExporterModule::RemoveContentBrowserContextMenuExtender() const
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	TArray<FContentBrowserMenuExtender_SelectedAssets>& CBMenuExtenderDelegates = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
	CBMenuExtenderDelegates.RemoveAll([this](const FContentBrowserMenuExtender_SelectedAssets& Delegate) { 
		return Delegate.GetHandle() == ContentBrowserExtenderDelegateHandle;
	});
}

TSharedRef<FExtender> FThumbnailExporterModule::OnExtendContentBrowserAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();
	Extender->AddMenuExtension(
		"CommonAssetActions",
		EExtensionHook::After,
		nullptr,
		FMenuExtensionDelegate::CreateStatic(&ExecuteSaveThumbnailAsTexture, SelectedAssets)
	);
	return Extender;
}

bool FThumbnailExporterModule::CanCreateThumbnail(const TArray<FAssetData>& SelectedAssets)
{
	UBlueprintThumbnailExporterRenderer* ThumbnailRenderer = UBlueprintThumbnailExporterRenderer::StaticClass()->GetDefaultObject<UBlueprintThumbnailExporterRenderer>();
	if (ThumbnailRenderer == nullptr)
	{
		return false;
	}

	for (const FAssetData& AssetData : SelectedAssets)
	{
		if (AssetData.IsValid())
		{
			if (ThumbnailRenderer->CanVisualizeAsset(AssetData.GetAsset()))
			{
				return true;
			}
		}
	}

	return false;
}

void FThumbnailExporterModule::ExecuteSaveThumbnailAsTexture(FMenuBuilder& MenuBuilder, const TArray<FAssetData> SelectedAssets)
{
	// Only create the menu if a blueprint is selected and it's renderable
	if (!CanCreateThumbnail(SelectedAssets))
	{
		return;
	}

	const TArray<FThumbnailCreationPreset>& ThumbnailCreationPresets = UThumbnailExporterSettings::Get()->ThumbnailCreationPresets;

	if (ThumbnailCreationPresets.Num() == 1)
	{
		MenuBuilder.BeginSection("Thumbnail Exporter", LOCTEXT("ThumbnailExporterAssetContext", "Thumbnail Exporter"));
		{
			MenuBuilder.AddMenuEntry(
				ThumbnailCreationPresets[0].MenuItemName,
				ThumbnailCreationPresets[0].MenuItemTooltip,
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([SelectedAssets]()
				{
					const FThumbnailCreationConfig& PresetConfig = UThumbnailExporterSettings::Get()->ThumbnailCreationPresets[0].PresetConfig;
					FString LocalExportDirectory;
					if (PresetConfig.bExportToLocal)
					{
						if (!ResolveLocalExportDirectory(LocalExportDirectory, &PresetConfig))
						{
							return;
						}
					}

					if (SelectedAssets.Num() > 1)
					{
						StartBatchExport(SelectedAssets, PresetConfig, LocalExportDirectory);
						return;
					}

					FScopedSlowTask SlowTask(SelectedAssets.Num(), LOCTEXT("ThumbnailExporterProgress", "Exporting thumbnails..."));
					SlowTask.MakeDialog(true);

					bool bAnyExported = false;
					TUniquePtr<FScopedLocalExportDirectoryOverride> ScopedLocalExportDirectory;
					if (PresetConfig.bExportToLocal)
					{
						ScopedLocalExportDirectory = MakeUnique<FScopedLocalExportDirectoryOverride>(LocalExportDirectory);
					}

					for (const FAssetData& Asset : SelectedAssets)
					{
						if (SlowTask.ShouldCancel())
						{
							break;
						}

						SlowTask.EnterProgressFrame(1.0f, FText::Format(LOCTEXT("ThumbnailExporterProgressAsset", "Exporting {0}"), FText::FromName(Asset.AssetName)));
						if (CanCreateThumbnail({ Asset }))
						{
							FString OutFilePath;
							if (ExportThumbnail(PresetConfig, Asset, OutFilePath))
							{
								bAnyExported = true;
							}
						}
					}

					if (PresetConfig.bExportToLocal && bAnyExported && !LocalExportDirectory.IsEmpty())
					{
						FPlatformProcess::ExploreFolder(*LocalExportDirectory);
					}
				})),
				NAME_None,
				EUserInterfaceActionType::Button
			);
		}
		MenuBuilder.EndSection();
	}
	else if (ThumbnailCreationPresets.Num() > 1)
	{
		MenuBuilder.BeginSection("Thumbnail Exporter", LOCTEXT("ThumbnailExporterAssetContext", "Thumbnail Exporter"));
		{
			MenuBuilder.AddSubMenu(
				LOCTEXT("ThumbnailExporterPresets", "Thumbnail Export Presets"),
				LOCTEXT("ThumbnailExporterPresetsTooltip", "Menu for the export preset list. The export presets are defined in the Thumbnail Exporter config in the project settings."),
				FNewMenuDelegate::CreateLambda([SelectedAssets](FMenuBuilder& InSubMenuBuilder)
				{
					const TArray<FThumbnailCreationPreset>& ThumbnailCreationPresets = UThumbnailExporterSettings::Get()->ThumbnailCreationPresets;
					for (int32 i = 0; i < ThumbnailCreationPresets.Num(); ++i)
					{
						InSubMenuBuilder.AddMenuEntry(
							ThumbnailCreationPresets[i].MenuItemName,
							ThumbnailCreationPresets[i].MenuItemTooltip,
							FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([SelectedAssets, i]()
							{
								const FThumbnailCreationConfig& PresetConfig = UThumbnailExporterSettings::Get()->ThumbnailCreationPresets[i].PresetConfig;
								FString LocalExportDirectory;
								if (PresetConfig.bExportToLocal)
								{
									if (!ResolveLocalExportDirectory(LocalExportDirectory, &PresetConfig))
									{
										return;
									}
								}

								if (SelectedAssets.Num() > 1)
								{
									StartBatchExport(SelectedAssets, PresetConfig, LocalExportDirectory);
									return;
								}

								FScopedSlowTask SlowTask(SelectedAssets.Num(), LOCTEXT("ThumbnailExporterProgress", "Exporting thumbnails..."));
								SlowTask.MakeDialog(true);

								bool bAnyExported = false;
								TUniquePtr<FScopedLocalExportDirectoryOverride> ScopedLocalExportDirectory;
								if (PresetConfig.bExportToLocal)
								{
									ScopedLocalExportDirectory = MakeUnique<FScopedLocalExportDirectoryOverride>(LocalExportDirectory);
								}

								for (const FAssetData& Asset : SelectedAssets)
								{
									if (SlowTask.ShouldCancel())
									{
										break;
									}

									SlowTask.EnterProgressFrame(1.0f, FText::Format(LOCTEXT("ThumbnailExporterProgressAsset", "Exporting {0}"), FText::FromName(Asset.AssetName)));
									if (CanCreateThumbnail({ Asset }))
									{
										FString OutFilePath;
										if (ExportThumbnail(PresetConfig, Asset, OutFilePath))
										{
											bAnyExported = true;
										}
									}
								}

								if (PresetConfig.bExportToLocal && bAnyExported && !LocalExportDirectory.IsEmpty())
								{
									FPlatformProcess::ExploreFolder(*LocalExportDirectory);
								}
							})),
							NAME_None,
							EUserInterfaceActionType::Button
						);
					}
				}),
				false,
				FSlateIcon()
			);
		}
		MenuBuilder.EndSection();
	}
}

bool FThumbnailExporterModule::GetThumbnailAssetPathAndFilename(const FThumbnailCreationConfig& CreationConfig, const FAssetData& Asset, FString& Path, FString& Filename)
{
	const FString AssetPath = Asset.GetAsset()->GetPathName();

	FString FullPath;
	if (CreationConfig.bOverrideThumbnailFilename)
	{
		FullPath = CreationConfig.ThumbnailOverrideFilename;
	}
	else
	{
		const FString& Prefix = CreationConfig.ThumbnailPrefix;
		const FString& Suffix = CreationConfig.ThumbnailSuffix;

		FullPath = Prefix + FPaths::GetBaseFilename(AssetPath) + Suffix;
	}

	if (CreationConfig.bOverrideThumbnailPath)
	{
		FullPath = CreationConfig.ThumbnailOverridePath.Path / FullPath;
	}
	else
	{
		FullPath = FPaths::GetPath(AssetPath) / FullPath;
	}

	Path = FPaths::GetPath(FullPath);
	Filename = FPaths::GetBaseFilename(FullPath);

	return true;
}

static UPackage* GetAssetPackage(const FThumbnailCreationConfig& CreationConfig, const FString& FullPath)
{
	UPackage* Package = CreatePackage(*FullPath);
	if (Package != nullptr)
	{
		Package->FullyLoad();
	}

	return Package;
}

bool FThumbnailExporterModule::ExportThumbnail(const FThumbnailCreationConfig& CreationConfig, const FAssetData& Asset, FString& ThumbnailPath, const FPreCreateThumbnail& CreationDelegate)
{
	FThumbnailCreationConfig ModifiedCreationConfig = CreationConfig;

	// Debug: 打印配置值以确认是否正确传递
	UE_LOG(LogTemp, Warning, TEXT("ExportThumbnail - bAutoThumbnailSizeX: %s, bAutoThumbnailSizeY: %s, bExportToLocal: %s, ThumbnailSize: %d"),
		ModifiedCreationConfig.bAutoThumbnailSizeX ? TEXT("true") : TEXT("false"),
		ModifiedCreationConfig.bAutoThumbnailSizeY ? TEXT("true") : TEXT("false"),
		ModifiedCreationConfig.bExportToLocal ? TEXT("true") : TEXT("false"),
		ModifiedCreationConfig.ThumbnailSize);

	if (ModifiedCreationConfig.bExportToLocal)
	{
		FString LocalDirectory;
		if (!ResolveLocalExportDirectory(LocalDirectory, &ModifiedCreationConfig))
		{
			return false;
		}

		UObject* AssetObject = Asset.GetAsset();

		// Warmup render: triggers texture streaming so materials are fully loaded
		{
			FObjectThumbnail WarmupThumbnail;
			FThumbnailExporterRenderer::RenderThumbnail(
				ModifiedCreationConfig,
				AssetObject,
				ModifiedCreationConfig.ThumbnailSize,
				ModifiedCreationConfig.ThumbnailSize,
				ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush,
				&WarmupThumbnail);
		}
		FlushRenderingCommands();

		// Actual render with fully streamed textures
		FObjectThumbnail LocalThumbnail;
		FThumbnailExporterRenderer::RenderThumbnail(
			ModifiedCreationConfig,
			AssetObject,
			ModifiedCreationConfig.ThumbnailSize,
			ModifiedCreationConfig.ThumbnailSize,
			ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush,
			&LocalThumbnail,
			CreationDelegate);

		if (LocalThumbnail.GetUncompressedImageData().Num() == 0)
		{
			return false;
		}

		FString BaseFilename = GetThumbnailBaseFilename(ModifiedCreationConfig, Asset);
		BaseFilename = FPaths::MakeValidFileName(BaseFilename);
		if (BaseFilename.IsEmpty())
		{
			BaseFilename = TEXT("Thumbnail");
		}

		FString FilePath = LocalDirectory / BaseFilename;
		if (!FilePath.EndsWith(TEXT(".png")))
		{
			FilePath += TEXT(".png");
		}

		if (!IFileManager::Get().MakeDirectory(*LocalDirectory, true) && !IFileManager::Get().DirectoryExists(*LocalDirectory))
		{
			return false;
		}

		TArray<uint8> PngData;
		if (!CreatePngData(LocalThumbnail, PngData))
		{
			return false;
		}

		if (!FFileHelper::SaveArrayToFile(PngData, *FilePath))
		{
			return false;
		}

		ThumbnailPath = FilePath;
		return true;
	}

	FString AssetPath, AssetFilename;
	if (!GetThumbnailAssetPathAndFilename(ModifiedCreationConfig, Asset, AssetPath, AssetFilename))
	{
		return false;
	}
	ThumbnailPath = AssetPath / AssetFilename;

	UPackage* Package = GetAssetPackage(ModifiedCreationConfig, ThumbnailPath);
	if (Package == nullptr)
	{
		return false;
	}

	UObject* AssetObjectForTexture = Asset.GetAsset();

	// Warmup render: triggers texture streaming so materials are fully loaded
	{
		FObjectThumbnail WarmupThumbnail;
		FThumbnailExporterRenderer::RenderThumbnail(
			ModifiedCreationConfig,
			AssetObjectForTexture,
			ModifiedCreationConfig.ThumbnailSize,
			ModifiedCreationConfig.ThumbnailSize,
			ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush,
			&WarmupThumbnail);
	}
	FlushRenderingCommands();

	// Actual render with fully streamed textures
	FObjectThumbnail LocalThumbnail;
	FThumbnailExporterRenderer::RenderThumbnail(
		ModifiedCreationConfig,
		AssetObjectForTexture,
		ModifiedCreationConfig.ThumbnailSize,
		ModifiedCreationConfig.ThumbnailSize,
		ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush,
		&LocalThumbnail,
		CreationDelegate);

	if (LocalThumbnail.GetUncompressedImageData().Num() == 0)
	{
		return false;
	}

	UTexture2D* NewTexture = NewObject<UTexture2D>(Package, *AssetFilename, RF_Public | RF_Standalone);
	NewTexture->AddToRoot();
	FTexturePlatformData* PlatformData = new FTexturePlatformData();
	PlatformData->SizeX = LocalThumbnail.GetImageWidth();
	PlatformData->SizeY = LocalThumbnail.GetImageHeight();
	PlatformData->PixelFormat = EPixelFormat::PF_B8G8R8A8;
	NewTexture->SetPlatformData(PlatformData);

	FTexture2DMipMap* Mip = new FTexture2DMipMap();
	PlatformData->Mips.Add(Mip);
	Mip->SizeX = LocalThumbnail.GetImageWidth();
	Mip->SizeY = LocalThumbnail.GetImageHeight();

	Mip->BulkData.Lock(LOCK_READ_WRITE);
	uint8* TextureData = (uint8*)Mip->BulkData.Realloc(LocalThumbnail.GetUncompressedImageData().Num());
	FMemory::Memcpy(TextureData, LocalThumbnail.GetUncompressedImageData().GetData(), LocalThumbnail.GetUncompressedImageData().Num());
	Mip->BulkData.Unlock();

	NewTexture->Source.Init(LocalThumbnail.GetImageWidth(), LocalThumbnail.GetImageHeight(), 1, 1, ETextureSourceFormat::TSF_BGRA8, LocalThumbnail.GetUncompressedImageData().GetData());
	NewTexture->LODGroup = ModifiedCreationConfig.ThumbnailTextureGroup;
	NewTexture->UpdateResource();
	Package->MarkPackageDirty();
	Package->FullyLoad();
	FAssetRegistryModule::AssetCreated(NewTexture);

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = EObjectFlags::RF_Public | EObjectFlags::RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	SaveArgs.bForceByteSwapping = true;
	FString PackageFileName = FPackageName::LongPackageNameToFilename(ThumbnailPath, FPackageName::GetAssetPackageExtension());
	UPackage::SavePackage(Package, NewTexture, *PackageFileName, SaveArgs);

	if (ModifiedCreationConfig.bCreateThumbnailNotification)
	{
		CreateThumbnailNotification(NewTexture);
	}

	return true;
}

void FThumbnailExporterModule::CreateThumbnailNotification(UTexture2D* NewTexture)
{
	FNotificationInfo NotificationInfo(LOCTEXT("GeneratedAssetIconNotification", "Generated asset icon"));
	NotificationInfo.ExpireDuration = 5.0f;

	const FSoftObjectPath SoftObjectPath(NewTexture);
	NotificationInfo.Hyperlink = FSimpleDelegate::CreateLambda([SoftObjectPath] {
		// Select the texture in Content Browser when the hyperlink is clicked
#if ENGINE_MINOR_VERSION == 0
		FAssetData AssetData = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get().GetAssetByObjectPath(SoftObjectPath.GetAssetPathName());
#else
		FAssetData AssetData = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get().GetAssetByObjectPath(SoftObjectPath.GetWithoutSubPath());
#endif
		FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().SyncBrowserToAssets({ AssetData });
	});

	NotificationInfo.HyperlinkText = FText::FromString(SoftObjectPath.ToString());
	FSlateNotificationManager::Get().AddNotification(NotificationInfo);
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FThumbnailExporterModule, ThumbnailExporter)
