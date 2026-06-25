#include "PakExportSubsystem.h"

#include "DesktopPlatform/Public/DesktopPlatformModule.h"
#include "DesktopPlatform/Public/IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/RegexHelper.h"
#include "Logging/LogMacros.h"
#include "Widgets/Notifications/SNotificationList.h"

void UPakExportSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	State = EPakExportState::Idle;
	CurrentProgress = 0.0f;
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UPakExportSubsystem::TickPakExport)
	);
}

void UPakExportSubsystem::Deinitialize()
{
	if (State != EPakExportState::Idle)
	{
		KillProcess();
		CleanupPipes();
		DismissNotification();
	}

	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	Super::Deinitialize();
}

bool UPakExportSubsystem::StartPakExport(const TArray<FAssetData>& LevelAssets)
{
	if (State != EPakExportState::Idle)
	{
		UE_LOG(LogTemp, Warning, TEXT("PakExport: Already running, ignoring request"));
		return false;
	}

	if (LevelAssets.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("PakExport: No level assets provided"));
		return false;
	}

	TArray<FString> LevelPaths;
	for (const FAssetData& Asset : LevelAssets)
	{
		if (Asset.AssetClassPath.GetAssetName() != FName("World"))
		{
			UE_LOG(LogTemp, Warning, TEXT("PakExport: Asset %s is not a Level/World"), *Asset.AssetName.ToString());
			return false;
		}

		LevelPaths.Add(Asset.PackageName.ToString());
	}

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return false;
	}

	TArray<FString> SelectedFiles;
	const void* ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	const bool bOpened = DesktopPlatform->SaveFileDialog(
		ParentWindowHandle,
		TEXT("选择PAK输出位置"),
		FPaths::ProjectDir(),
		TEXT("Output.pak"),
		TEXT("PAK文件 (*.pak)|*.pak"),
		EFileDialogFlags::None,
		SelectedFiles
	);

	if (!bOpened || SelectedFiles.IsEmpty())
	{
		return false;
	}

	OutputPakPath = SelectedFiles[0];
	const FString ArchiveDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Saved/StagedPak"));

	State = EPakExportState::Running;
	CurrentProgress = 0.0f;
	LaunchUATProcess(LevelPaths, ArchiveDir);
	ShowProgressNotification();

	return true;
}

void UPakExportSubsystem::LaunchUATProcess(const TArray<FString>& LevelPaths, const FString& ArchiveDir)
{
	FString UATPath;
#if PLATFORM_WINDOWS
	UATPath = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Build/BatchFiles/RunUAT.bat"));
#else
	UATPath = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Build/BatchFiles/RunUAT.sh"));
#endif

	FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	FString PlatformStr;
#if PLATFORM_WINDOWS
	PlatformStr = TEXT("Win64");
#else
	PlatformStr = TEXT("Linux");
#endif

	FString MapParam = FString::Join(LevelPaths, TEXT("+"));

	FString Args = FString::Printf(
		TEXT("BuildCookRun -project=\"%s\" -Map=%s -archive -archivedirectory=\"%s\" ")
		TEXT("-clientconfig=Shipping -serverconfig=Shipping -platform=%s ")
		TEXT("-noP4 -cook -compressed -stage -package -pak -iterate ")
		TEXT("-NumCookersToSpawn=10 -utf8output -nocompileeditor ")
		TEXT("-SkipCookingEditorContent -prereqs"),
		*ProjectPath, *MapParam, *ArchiveDir, *PlatformStr
	);

	FPlatformProcess::CreatePipe(ReadPipe, WritePipe);
	ProcessHandle = FPlatformProcess::CreateProc(
		*UATPath, *Args,
		/*bLaunchDetached=*/false,
		/*bLaunchHidden=*/true,
		/*bLaunchReallyHidden=*/true,
		/*OutProcessID=*/nullptr,
		/*PriorityModifier=*/0,
		/*OptionalWorkingDirectory=*/nullptr,
		WritePipe,
		ReadPipe
	);
}

bool UPakExportSubsystem::PollProcessOutput(FString& OutStage, float& OutPercent)
{
	const FString Output = FPlatformProcess::ReadPipe(ReadPipe);
	if (Output.IsEmpty())
	{
		return false;
	}

	static const FRegexPattern ProgressPattern(TEXT("\\((\\d+)/(\\d+)\\)"));

	TArray<FString> Lines;
	Output.ParseIntoArrayLines(Lines);

	bool bFoundProgress = false;
	for (const FString& Line : Lines)
	{
		UE_LOG(LogTemp, Log, TEXT("UAT> %s"), *Line);
		FRegexMatcher Matcher(ProgressPattern, Line);
		if (!Matcher.FindNext())
		{
			continue;
		}

		const int32 Numerator = FCString::Atoi(*Matcher.GetCaptureGroup(1));
		const int32 Denominator = FCString::Atoi(*Matcher.GetCaptureGroup(2));
		if (Denominator <= 0)
		{
			continue;
		}

		OutPercent = (static_cast<float>(Numerator) / static_cast<float>(Denominator)) * 100.0f;
		OutStage = Line.Left(Line.Find(TEXT("("))).TrimEnd();
		CurrentProgress = OutPercent;
		bFoundProgress = true;
	}

	return bFoundProgress;
}

void UPakExportSubsystem::KillProcess()
{
	FPlatformProcess::TerminateProc(ProcessHandle);
}

bool UPakExportSubsystem::IsProcessRunningInternal() const
{
	FProcHandle HandleCopy = ProcessHandle;
	return FPlatformProcess::IsProcRunning(HandleCopy);
}

bool UPakExportSubsystem::GetProcessExitCode(int32& OutCode) const
{
	FProcHandle HandleCopy = ProcessHandle;
	return FPlatformProcess::GetProcReturnCode(HandleCopy, &OutCode);
}

void UPakExportSubsystem::CleanupPipes()
{
	if (ProcessHandle.IsValid())
	{
		FPlatformProcess::CloseProc(ProcessHandle);
	}
	FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
	ReadPipe = nullptr;
	WritePipe = nullptr;
}

void UPakExportSubsystem::ShowProgressNotification()
{
	FNotificationInfo Info(FText::FromString(TEXT("PAK打包中... 0%")));
	Info.bFireAndForget = false;
	Info.ExpireDuration = 0.0f;
	Info.bUseThrobber = true;

	FNotificationButtonInfo CancelButton(
		FText::FromString(TEXT("取消")),
		FText::FromString(TEXT("取消PAK打包")),
		FSimpleDelegate::CreateUObject(this, &UPakExportSubsystem::CancelPakExport)
	);
	Info.ButtonDetails.Add(CancelButton);

	NotificationPtr = FSlateNotificationManager::Get().AddNotification(Info);
	if (NotificationPtr.IsValid())
	{
		NotificationPtr->SetCompletionState(SNotificationItem::CS_Pending);
	}
}

void UPakExportSubsystem::UpdateNotificationProgress(float Percent, const FString& Stage)
{
	if (NotificationPtr.IsValid())
	{
		FString Text = FString::Printf(TEXT("PAK打包中... %.0f%%"), Percent);
		if (!Stage.IsEmpty())
		{
			Text += TEXT(" - ") + Stage;
		}
		NotificationPtr->SetText(FText::FromString(Text));
	}
}

void UPakExportSubsystem::CompleteNotification(bool bSuccess, const FString& Message)
{
	if (NotificationPtr.IsValid())
	{
		if (bSuccess)
		{
			NotificationPtr->SetText(FText::FromString(TEXT("PAK打包完成")));
			NotificationPtr->SetCompletionState(SNotificationItem::CS_Success);
		}
		else
		{
			NotificationPtr->SetText(FText::FromString(TEXT("PAK打包失败: ") + Message));
			NotificationPtr->SetCompletionState(SNotificationItem::CS_Fail);
		}

		NotificationPtr->ExpireAndFadeout();
		NotificationPtr = nullptr;
	}
}

void UPakExportSubsystem::DismissNotification()
{
	if (NotificationPtr.IsValid())
	{
		NotificationPtr->SetText(FText::FromString(TEXT("PAK打包已取消")));
		NotificationPtr->SetCompletionState(SNotificationItem::CS_Fail);
		NotificationPtr->ExpireAndFadeout();
		NotificationPtr = nullptr;
	}
}

bool UPakExportSubsystem::TickPakExport(float DeltaTime)
{
	if (State == EPakExportState::Running)
	{
		FString Stage;
		float Percent = 0.0f;
		if (PollProcessOutput(Stage, Percent))
		{
			UpdateNotificationProgress(Percent, Stage);
			OnProgress.Broadcast(Percent, Stage);
		}

		if (!IsProcessRunningInternal())
		{
			int32 ExitCode = 0;
			GetProcessExitCode(ExitCode);
			State = (ExitCode == 0) ? EPakExportState::Completing : EPakExportState::Failed;
		}
	}
	else if (State == EPakExportState::Completing)
	{
		const FString ProjectName = FPaths::GetBaseFilename(FPaths::GetProjectFilePath());
#if PLATFORM_WINDOWS
		const FString PrimaryPlatformDir = TEXT("Windows");
		const FString AlternatePlatformDir = TEXT("WindowsNoEditor");
#else
		const FString PrimaryPlatformDir = TEXT("Linux");
		const FString AlternatePlatformDir = TEXT("LinuxNoEditor");
#endif

		FString PakSearchPath = FPaths::ProjectDir() / TEXT("Saved/StagedPak") / PrimaryPlatformDir / ProjectName / TEXT("Content/Paks/*.pak");
		TArray<FString> FoundPaks;
		IFileManager::Get().FindFiles(FoundPaks, *PakSearchPath, true, false);

		if (FoundPaks.IsEmpty())
		{
			PakSearchPath = FPaths::ProjectDir() / TEXT("Saved/StagedPak") / AlternatePlatformDir / ProjectName / TEXT("Content/Paks/*.pak");
			IFileManager::Get().FindFiles(FoundPaks, *PakSearchPath, true, false);
		}

		if (!FoundPaks.IsEmpty())
		{
			const FString SourcePak = FPaths::GetPath(PakSearchPath) / FoundPaks[0];
			const int32 CopyResult = IFileManager::Get().Copy(*OutputPakPath, *SourcePak);
			if (CopyResult == COPY_OK)
			{
				CompleteNotification(true, TEXT(""));
				OnComplete.Broadcast(true, OutputPakPath);
				FPlatformProcess::ExploreFolder(*FPaths::GetPath(OutputPakPath));
			}
			else
			{
				State = EPakExportState::Failed;
				return true;
			}
		}
		else
		{
			CompleteNotification(false, TEXT("找不到生成的PAK文件"));
			OnComplete.Broadcast(false, TEXT("找不到生成的PAK文件"));
		}

		CleanupPipes();
		State = EPakExportState::Idle;
	}
	else if (State == EPakExportState::Failed)
	{
		CompleteNotification(false, TEXT("UAT进程异常退出"));
		OnComplete.Broadcast(false, TEXT("UAT进程异常退出"));
		CleanupPipes();
		State = EPakExportState::Idle;
	}
	else if (State == EPakExportState::Cancelling)
	{
		KillProcess();
		DismissNotification();
		OnComplete.Broadcast(false, TEXT("已取消"));
		CleanupPipes();
		State = EPakExportState::Idle;
	}

	return true;
}

void UPakExportSubsystem::CancelPakExport()
{
	if (State == EPakExportState::Running)
	{
		State = EPakExportState::Cancelling;
	}
}

bool UPakExportSubsystem::IsPakExportRunning() const
{
	return State != EPakExportState::Idle;
}

float UPakExportSubsystem::GetPakExportProgress() const
{
	return CurrentProgress;
}
