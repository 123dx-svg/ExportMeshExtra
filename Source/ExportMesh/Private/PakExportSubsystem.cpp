#include "PakExportSubsystem.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "DesktopPlatform/Public/DesktopPlatformModule.h"
#include "DesktopPlatform/Public/IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/RegexHelper.h"
#include "Modules/ModuleManager.h"
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
		if (!ResponseFilePath.IsEmpty())
		{
			IFileManager::Get().Delete(*ResponseFilePath);
			ResponseFilePath.Empty();
		}
		PendingLevelPackageNames.Reset();
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

	PendingLevelPackageNames.Reset();
	ResponseFilePath.Empty();
	for (const FAssetData& Asset : LevelAssets)
	{
		if (Asset.AssetClassPath.GetAssetName() != FName("World"))
		{
			UE_LOG(LogTemp, Warning, TEXT("PakExport: Asset %s is not a Level/World"), *Asset.AssetName.ToString());
			return false;
		}

		PendingLevelPackageNames.Add(Asset.PackageName.ToString());
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

	// 在文件名后追加优先级后缀：Name_P<priority>.pak（_P 让 UE/SimOne 识别为高优先级 patch 包）
	{
		const FString PakDir = FPaths::GetPath(OutputPakPath);
		const FString PakBaseName = FPaths::GetBaseFilename(OutputPakPath);
		FString PakExt = FPaths::GetExtension(OutputPakPath);
		if (PakExt.IsEmpty())
		{
			PakExt = TEXT("pak");
		}
		OutputPakPath = PakDir / FString::Printf(TEXT("%s_P%d.%s"), *PakBaseName, PakPriority, *PakExt);
	}

	State = EPakExportState::Cooking;
	CurrentProgress = 0.0f;
	LaunchCookProcess(PendingLevelPackageNames);
	ShowProgressNotification();

	return true;
}

void UPakExportSubsystem::LaunchCookProcess(const TArray<FString>& LevelPackageNames)
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

	FString MapParam = FString::Join(LevelPackageNames, TEXT("+"));

	FString Args = FString::Printf(
		TEXT("BuildCookRun -project=\"%s\" -cook -Map=%s -platform=%s ")
		TEXT("-clientconfig=Shipping -iterate -nocompileeditor ")
		TEXT("-SkipCookingEditorContent -utf8output -noP4 -skipstage"),
		*ProjectPath, *MapParam, *PlatformStr
	);

	UE_LOG(LogTemp, Log, TEXT("PakExport: Launching cook: %s %s"), *UATPath, *Args);

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

bool UPakExportSubsystem::BuildPakResponseFile()
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TSet<FName> Closure;
	TArray<FName> Stack;
	for (const FString& PackageName : PendingLevelPackageNames)
	{
		Stack.Add(FName(*PackageName));
	}

	while (!Stack.IsEmpty())
	{
		const FName PackageName = Stack.Pop();
		if (Closure.Contains(PackageName))
		{
			continue;
		}

		Closure.Add(PackageName);

		TArray<FName> Dependencies;
		AssetRegistry.GetDependencies(
			PackageName,
			Dependencies,
			UE::AssetRegistry::EDependencyCategory::Package
		);

		for (const FName Dependency : Dependencies)
		{
			if (!Closure.Contains(Dependency))
			{
				Stack.Add(Dependency);
			}
		}
	}

	const FString ProjectName = FPaths::GetBaseFilename(FPaths::GetProjectFilePath());
	TArray<FString> PlatformDirs;
#if PLATFORM_WINDOWS
	PlatformDirs.Add(TEXT("Windows"));
	PlatformDirs.Add(TEXT("WindowsNoEditor"));
#else
	PlatformDirs.Add(TEXT("Linux"));
	PlatformDirs.Add(TEXT("LinuxNoEditor"));
#endif

	FString CookedContentDir;
	for (const FString& PlatformDir : PlatformDirs)
	{
		const FString CandidateDir = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("Saved/Cooked") / PlatformDir / ProjectName / TEXT("Content")
		);
		if (IFileManager::Get().DirectoryExists(*CandidateDir))
		{
			CookedContentDir = CandidateDir;
			break;
		}
	}

	if (CookedContentDir.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("PakExport: Cooked content directory not found under Saved/Cooked"));
		return false;
	}

	TArray<FString> ResponseLines;
	TSet<FString> IncludedSourceFiles;
	for (const FName PackageName : Closure)
	{
		const FString PackageString = PackageName.ToString();
		if (!PackageString.StartsWith(TEXT("/Game/")))
		{
			continue;
		}

		const FString RelativePackagePath = PackageString.RightChop(6);
		const FString CookedFileBase = CookedContentDir / RelativePackagePath;
		const FString CookedFileDir = FPaths::GetPath(CookedFileBase);
		const FString CookedFileBaseName = FPaths::GetBaseFilename(CookedFileBase);

		TArray<FString> FoundFiles;
		IFileManager::Get().FindFiles(
			FoundFiles,
			*(CookedFileDir / (CookedFileBaseName + TEXT(".*"))),
			true,
			false
		);

		const FString RelativeDir = FPaths::GetPath(RelativePackagePath);
		for (const FString& FoundFile : FoundFiles)
		{
			FString SourceAbs = CookedFileDir / FoundFile;
			FPaths::NormalizeFilename(SourceAbs);
			if (IncludedSourceFiles.Contains(SourceAbs))
			{
				continue;
			}

			IncludedSourceFiles.Add(SourceAbs);

			const FString RelativeMountFile = RelativeDir.IsEmpty() ? FoundFile : RelativeDir / FoundFile;
			const FString MountPath = FString::Printf(
				TEXT("../../../%s/Content/%s"),
				*ProjectName,
				*RelativeMountFile
			);
			ResponseLines.Add(FString::Printf(TEXT("\"%s\" \"%s\"\n"), *SourceAbs, *MountPath));
		}
	}

	if (ResponseLines.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("PakExport: No cooked /Game/ files resolved for selected level dependency closure"));
		return false;
	}

	ResponseFilePath = FPaths::ProjectIntermediateDir() / TEXT("PakExportResponse.txt");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ResponseFilePath), true);
	const bool bSaved = FFileHelper::SaveStringToFile(
		FString::Join(ResponseLines, TEXT("")),
		*ResponseFilePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM
	);
	if (!bSaved)
	{
		UE_LOG(LogTemp, Error, TEXT("PakExport: Failed to write response file: %s"), *ResponseFilePath);
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("PakExport: Wrote %d cooked files from %d hard dependency packages to response file: %s"), ResponseLines.Num(), Closure.Num(), *ResponseFilePath);
	return true;
}

void UPakExportSubsystem::LaunchPakProcess()
{
	FString UnrealPakPath;
#if PLATFORM_WINDOWS
	UnrealPakPath = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/Win64/UnrealPak.exe"));
#else
	UnrealPakPath = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/Linux/UnrealPak"));
#endif

	const FString Args = FString::Printf(
		TEXT("\"%s\" -create=\"%s\" -compress"),
		*OutputPakPath,
		*ResponseFilePath
	);

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPakPath), true);
	if (IFileManager::Get().FileExists(*OutputPakPath))
	{
		IFileManager::Get().Delete(*OutputPakPath);
	}

	UE_LOG(LogTemp, Log, TEXT("PakExport: Launching UnrealPak: %s %s"), *UnrealPakPath, *Args);

	FPlatformProcess::CreatePipe(ReadPipe, WritePipe);
	ProcessHandle = FPlatformProcess::CreateProc(
		*UnrealPakPath, *Args,
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
		ProcessHandle = FProcHandle();
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
	if (State == EPakExportState::Cooking)
	{
		FString Stage;
		float Percent = 0.0f;
		if (PollProcessOutput(Stage, Percent))
		{
			const FString CookStage = Stage.IsEmpty() ? TEXT("Cook中") : FString::Printf(TEXT("Cook中 - %s"), *Stage);
			UpdateNotificationProgress(Percent, CookStage);
			OnProgress.Broadcast(Percent, CookStage);
		}

		if (!IsProcessRunningInternal())
		{
			int32 ExitCode = 0;
			GetProcessExitCode(ExitCode);
			CleanupPipes();

			if (ExitCode == 0)
			{
				if (BuildPakResponseFile())
				{
					State = EPakExportState::Packing;
					CurrentProgress = 0.0f;
					LaunchPakProcess();
				}
				else
				{
					State = EPakExportState::Failed;
				}
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("PakExport: Cook process failed with exit code %d"), ExitCode);
				State = EPakExportState::Failed;
			}
		}
	}
	else if (State == EPakExportState::Packing)
	{
		FString Stage;
		float Percent = 0.0f;
		if (PollProcessOutput(Stage, Percent))
		{
			const FString PackStage = Stage.IsEmpty() ? TEXT("打包中") : FString::Printf(TEXT("打包中 - %s"), *Stage);
			UpdateNotificationProgress(Percent, PackStage);
			OnProgress.Broadcast(Percent, PackStage);
		}
		else
		{
			const FString PackStage = TEXT("打包中");
			UpdateNotificationProgress(CurrentProgress, PackStage);
			OnProgress.Broadcast(CurrentProgress, PackStage);
		}

		if (!IsProcessRunningInternal())
		{
			int32 ExitCode = 0;
			GetProcessExitCode(ExitCode);
			State = (ExitCode == 0) ? EPakExportState::Completing : EPakExportState::Failed;
			if (ExitCode != 0)
			{
				UE_LOG(LogTemp, Error, TEXT("PakExport: UnrealPak process failed with exit code %d"), ExitCode);
			}
		}
	}
	else if (State == EPakExportState::Completing)
	{
		const int64 OutputPakSize = IFileManager::Get().FileSize(*OutputPakPath);
		if (IFileManager::Get().FileExists(*OutputPakPath) || OutputPakSize > 0)
		{
			CompleteNotification(true, TEXT(""));
			OnComplete.Broadcast(true, OutputPakPath);
			FPlatformProcess::ExploreFolder(*FPaths::GetPath(OutputPakPath));
		}
		else
		{
			CompleteNotification(false, TEXT("UnrealPak未生成pak文件"));
			OnComplete.Broadcast(false, TEXT("UnrealPak未生成pak文件"));
		}

		CleanupPipes();
		if (!ResponseFilePath.IsEmpty())
		{
			IFileManager::Get().Delete(*ResponseFilePath);
			ResponseFilePath.Empty();
		}
		PendingLevelPackageNames.Reset();
		State = EPakExportState::Idle;
	}
	else if (State == EPakExportState::Failed)
	{
		CompleteNotification(false, TEXT("PAK导出进程异常退出"));
		OnComplete.Broadcast(false, TEXT("PAK导出进程异常退出"));
		CleanupPipes();
		if (!ResponseFilePath.IsEmpty())
		{
			IFileManager::Get().Delete(*ResponseFilePath);
			ResponseFilePath.Empty();
		}
		PendingLevelPackageNames.Reset();
		State = EPakExportState::Idle;
	}
	else if (State == EPakExportState::Cancelling)
	{
		KillProcess();
		DismissNotification();
		OnComplete.Broadcast(false, TEXT("已取消"));
		CleanupPipes();
		if (!ResponseFilePath.IsEmpty())
		{
			IFileManager::Get().Delete(*ResponseFilePath);
			ResponseFilePath.Empty();
		}
		PendingLevelPackageNames.Reset();
		State = EPakExportState::Idle;
	}

	return true;
}

void UPakExportSubsystem::CancelPakExport()
{
	if (State == EPakExportState::Cooking || State == EPakExportState::Packing)
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

void UPakExportSubsystem::SetPakPriority(int32 Priority)
{
	PakPriority = Priority;
}

int32 UPakExportSubsystem::GetPakPriority() const
{
	return PakPriority;
}
