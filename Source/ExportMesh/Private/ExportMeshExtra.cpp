// Copyright Epic Games, Inc. All Rights Reserved.

#include "ExportMeshExtra.h"

#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "EditorUtilitySubsystem.h"
#include "EditorAssetLibrary.h"
#include "EditorUtilityWidgetBlueprint.h"
#include "LevelEditor.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "FExportMeshExtraModule"

void FExportMeshExtraModule::StartupModule()
{
	if (UToolMenus::IsToolMenuUIEnabled())
	{
		RegisterMenus();
	}
	else
	{
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FExportMeshExtraModule::RegisterMenus));
	}
}

void FExportMeshExtraModule::ShutdownModule()
{
	if (UToolMenus::IsToolMenuUIEnabled())
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}
}

void FExportMeshExtraModule::RegisterMenus()
{
	UToolMenus* Menus = UToolMenus::Get();
	if (!Menus)
	{
		return;
	}

	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* MainMenu = Menus->ExtendMenu("LevelEditor.MainMenu");
	if (!MainMenu)
	{
		return;
	}
	const FName SectionName("SIMTools");

	UToolMenu* SimToolsMenu = MainMenu->AddSubMenu(
		OwnerScoped.GetOwner(),
		SectionName,
		"SIMTools",
		LOCTEXT("SimToolsMenu", "SIM工具"),
		LOCTEXT("SimToolsMenuTooltip", "SIM工具")
	);

	if (SimToolsMenu)
	{
		FToolMenuSection& ToolsSection = SimToolsMenu->AddSection("Scripts", LOCTEXT("ToolsSection", "Scripts"));
		ToolsSection.AddMenuEntry(
			"ExportAllAssets",
			LOCTEXT("ExportAllAssetsLabel", "导出外挂资产"),
			LOCTEXT("ExportAllAssetsTooltip", "打开 SIM导出外挂资产 工具"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "SystemWideCommands.SummonOpenAssetDialog"),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				if (UEditorUtilitySubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>() : nullptr)
				{
					UEditorUtilityWidgetBlueprint* Asset = Cast<UEditorUtilityWidgetBlueprint>(
						UEditorAssetLibrary::LoadAsset(TEXT("/ExportMeshExtra/Tools/ExportAllAssets.ExportAllAssets"))
					);
					if (Asset)
					{
						Subsystem->SpawnAndRegisterTab(Asset);
					}
				}
			}))
		);

		ToolsSection.AddMenuEntry(
			"PerformanceTools",
			LOCTEXT("PerformanceToolsLabel", "性能优化"),
			LOCTEXT("PerformanceToolsTooltip", "打开 SIM性能优化 工具"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Advanced"),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				if (UEditorUtilitySubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>() : nullptr)
				{
					UEditorUtilityWidgetBlueprint* Asset = Cast<UEditorUtilityWidgetBlueprint>(
						UEditorAssetLibrary::LoadAsset(TEXT("/ExportMeshExtra/Tools/PerformanceTools.PerformanceTools"))
					);
					if (Asset)
					{
						Subsystem->SpawnAndRegisterTab(Asset);
					}
				}
			}))
		);
	}

	Menus->RefreshAllWidgets();
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FExportMeshExtraModule, ExportMeshExtra)
