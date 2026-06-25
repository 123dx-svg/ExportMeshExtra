#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "AssetRegistry/AssetData.h"
#include "Containers/Ticker.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformProcess.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "PakExportSubsystem.generated.h"

USTRUCT(BlueprintType)
struct FPakExportConfig
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "ExportMeshExtra|PakExport")
	TArray<FString> LevelPaths;

	UPROPERTY(BlueprintReadWrite, Category = "ExportMeshExtra|PakExport")
	FString OutputPakPath;
};

UENUM(BlueprintType)
enum class EPakExportState : uint8
{
	Idle UMETA(DisplayName = "空闲"),
	Cooking UMETA(DisplayName = "Cook中"),
	Packing UMETA(DisplayName = "打包中"),
	Completing UMETA(DisplayName = "完成中"),
	Cancelling UMETA(DisplayName = "取消中"),
	Failed UMETA(DisplayName = "失败")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPakExportComplete, bool, bSuccess, FString, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPakExportProgress, float, Percent, FString, Stage);

UCLASS()
class EXPORTMESHEXTRA_API UPakExportSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra|PakExport")
	bool StartPakExport(const TArray<FAssetData>& LevelAssets);

	UFUNCTION(BlueprintCallable, Category = "ExportMeshExtra|PakExport")
	void CancelPakExport();

	UFUNCTION(BlueprintPure, Category = "ExportMeshExtra|PakExport")
	bool IsPakExportRunning() const;

	UFUNCTION(BlueprintPure, Category = "ExportMeshExtra|PakExport")
	float GetPakExportProgress() const;

	UPROPERTY(BlueprintAssignable, Category = "ExportMeshExtra|PakExport")
	FOnPakExportComplete OnComplete;

	UPROPERTY(BlueprintAssignable, Category = "ExportMeshExtra|PakExport")
	FOnPakExportProgress OnProgress;

private:
	EPakExportState State = EPakExportState::Idle;
	FProcHandle ProcessHandle;
	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	FString OutputPakPath;
	TArray<FString> PendingLevelPackageNames;
	FString ResponseFilePath;
	TSharedPtr<SNotificationItem> NotificationPtr;
	float CurrentProgress = 0.0f;
	FTSTicker::FDelegateHandle TickerHandle;

	void LaunchCookProcess(const TArray<FString>& LevelPackageNames);
	void LaunchPakProcess();
	bool BuildPakResponseFile();
	bool PollProcessOutput(FString& OutStage, float& OutPercent);
	void KillProcess();
	bool IsProcessRunningInternal() const;
	bool GetProcessExitCode(int32& OutCode) const;
	void CleanupPipes();
	void ShowProgressNotification();
	void UpdateNotificationProgress(float Percent, const FString& Stage);
	void CompleteNotification(bool bSuccess, const FString& Message);
	void DismissNotification();
	bool TickPakExport(float DeltaTime);
};
