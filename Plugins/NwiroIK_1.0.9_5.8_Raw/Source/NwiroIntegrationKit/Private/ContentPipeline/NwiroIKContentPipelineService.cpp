// Copyright 2026 Nwiro. All Rights Reserved.
#include "NwiroIKContentPipelineService.h"

#include "NwiroIKContentPipelineService.h"
#include "ContentPipeline/NwiroIKContentImporter.h"
#include "ContentPipeline/NwiroIKContentPaths.h"
#include "ContentPipeline/NwiroIKZipExtractor.h"
#include "NwiroIKPanel.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
#include "ContentBrowserModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "IContentBrowserSingleton.h"
#include "SWebBrowser.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroContentPipeline, Log, All);

namespace
{
	constexpr int64 DefaultChunkSizeBytes = 64LL * 1024LL * 1024LL;
	constexpr int64 DefaultSafetyMarginBytes = 512LL * 1024LL * 1024LL;
	constexpr double ProgressEventMinIntervalSeconds = 1.0;
	constexpr int64 ProgressEventMinBytes = 8LL * 1024LL * 1024LL;

	FString StateToString(const ENwiroIKContentOperationState State)
	{
		switch (State)
		{
		case ENwiroIKContentOperationState::Idle:
			return TEXT("idle");
		case ENwiroIKContentOperationState::Downloading:
			return TEXT("downloading");
		case ENwiroIKContentOperationState::Verifying:
			return TEXT("verifying");
		case ENwiroIKContentOperationState::Unzipping:
			return TEXT("unzipping");
		case ENwiroIKContentOperationState::Importing:
			return TEXT("importing");
		case ENwiroIKContentOperationState::Completed:
			return TEXT("completed");
		case ENwiroIKContentOperationState::Failed:
			return TEXT("failed");
		case ENwiroIKContentOperationState::Cancelled:
			return TEXT("cancelled");
		default:
			return TEXT("unknown");
		}
	}

	bool IsHexChar(const TCHAR C)
	{
		return (C >= TEXT('0') && C <= TEXT('9'))
			|| (C >= TEXT('a') && C <= TEXT('f'))
			|| (C >= TEXT('A') && C <= TEXT('F'));
	}

	bool TryExtractHashFromOutput(const FString& Output, const int32 ExpectedLength, FString& OutHash)
	{
		TArray<FString> Lines;
		Output.ParseIntoArrayLines(Lines, true);

		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty())
			{
				continue;
			}

			FString HexOnly;
			HexOnly.Reserve(Line.Len());
			for (const TCHAR C : Line)
			{
				if (IsHexChar(C))
				{
					HexOnly.AppendChar(C);
				}
			}

			if (HexOnly.Len() == ExpectedLength)
			{
				HexOnly.ToLowerInline();
				OutHash = HexOnly;
				return true;
			}
		}

		return false;
	}

	FString ToJsonString(const TSharedRef<FJsonObject>& JsonObject)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(JsonObject, Writer);
		return Output;
	}
}

struct FNwiroIKContentPipelineService::FImpl
{
	struct FTelemetryEvent
	{
		FString Name;
		FDateTime TimestampUtc;
		double DurationSeconds = 0.0;
		FString ErrorCode;
		FString ErrorMessage;
		int64 DownloadedBytes = 0;
		int64 TotalBytes = -1;
		double Progress = 0.0;
		FString State;
	};

	struct FOperation
	{
		FNwiroIKContentRequest Request;
		ENwiroIKContentOperationState State = ENwiroIKContentOperationState::Idle;
		FString NormalizedHash;
		FString ZipPath;
		FString PartialZipPath;
		FString ExtractedPath;
		FString UnzipMarkerPath;
		FString ImportMarkerPath;
		FString ProjectPath;
		FString ProjectId;
		FString ErrorCode;
		FString ErrorMessage;
		int64 DownloadedBytes = 0;
		int64 TotalBytes = -1;
		int32 RetryCount = 0;
		int32 IntegrityRetryCount = 0;
		double LastProgressEventSeconds = 0.0;
		int64 LastProgressEventBytes = 0;
		bool bZipDownloaded = false;
		bool bUnzipComplete = false;
		bool bImportedInProject = false;
		bool bHashVerified = false;
		bool bCancelled = false;
		bool bRangeSupportValidated = false;
		bool bRangeSupported = false;
		bool bRangeCheckInFlight = false;
		bool bRangeGetProbeAttempted = false;
		TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveRequest;
		FDateTime CreatedAtUtc = FDateTime::UtcNow();
		FDateTime UpdatedAtUtc = FDateTime::UtcNow();
		FDateTime StageStartedAtUtc = FDateTime::UtcNow();
		TArray<FTelemetryEvent> Events;
	};

	TMap<FString, FOperation> Operations;
	bool bIsShuttingDown = false;

	bool StartDownloadAndImport(const FNwiroIKContentRequest& InRequest, FString& OutError);
	bool StartImportFromCache(const FString& ContentId, const FString& Hash, const FString& RelativeImportPath, FString& OutError);
	bool CancelOperation(const FString& ContentId, const FString& Hash, FString& OutError);
	bool DeleteCachedContent(const FString& Hash, bool bDeleteImportedInProject, FString& OutError);
	void Shutdown();
	FString GetOperationStatusJson(const FString& ContentId, const FString& Hash);
	FString GetContentStateJson(const FString& ContentId, const FString& Hash) const;
	FString GetCachedContentsJson() const;
	bool IsContentCached(const FString& Hash) const;
	bool IsZipDownloaded(const FString& Hash) const;
	bool IsUnzipComplete(const FString& Hash) const;
	bool IsImportedInCurrentProject(const FString& Hash) const;

private:
	void StartRangeSupportCheck(const FString& NormalizedHash, bool bUseRangeGetProbe);
	void OnRangeSupportCheckComplete(const FString& NormalizedHash, const FHttpRequestPtr& HttpRequest, const FHttpResponsePtr& Response, bool bWasSuccessful, bool bWasRangeGetProbe);
	void SendNextChunkRequest(const FString& NormalizedHash);
	void OnDownloadProgress(const FString& NormalizedHash, int64 RequestBytesReceived);
	void OnChunkResponse(const FString& NormalizedHash, const FHttpRequestPtr& HttpRequest, const FHttpResponsePtr& Response, bool bWasSuccessful);
	void FinalizeDownload(const FString& NormalizedHash);
	void StartIntegrityVerification(const FString& NormalizedHash);
	void StartUnzip(const FString& NormalizedHash);
	void StartImport(const FString& NormalizedHash);
	void RefreshAssetRegistry(const FNwiroIKImportResult& ImportResult);
	void HandleRetryOrFailure(const FString& NormalizedHash, const FString& ErrorCode, const FString& ErrorMessage, bool bRestartFromScratch);
	bool CheckDiskSpace(const FOperation& Op, FString& OutError) const;
	bool ComputeFileHash(const FString& FilePath, bool bSHA256, FString& OutHash, FString& OutError) const;
	double GetOverallProgress(const FOperation& Op) const;
	void EmitEvent(FOperation& Op, const FString& EventName, const FString& ErrorCode, const FString& ErrorMessage, int64 DownloadedBytes = -1, int64 TotalBytes = -1, double Progress = -1.0);
	void DispatchEventToBrowser(const FOperation& Op, const FTelemetryEvent& Event);
	FString BuildStatusJson(const FOperation& Op) const;
	FString BuildPassiveStatusJson(const FString& ContentId, const FString& NormalizedHash) const;
	void CollectTopLevelImportedFolders(const FString& Hash, TSet<FString>& OutFolders) const;
};

bool FNwiroIKContentPipelineService::FImpl::StartDownloadAndImport(const FNwiroIKContentRequest& InRequest, FString& OutError)
{
	if (bIsShuttingDown)
	{
		OutError = TEXT("Pipeline is shutting down.");
		return false;
	}

	FNwiroIKContentRequest Request = InRequest;
	Request.Hash = FNwiroIKContentPaths::NormalizeHash(Request.Hash);
	Request.RelativeImportPath.TrimStartAndEndInline();

	if (Request.Hash.IsEmpty())
	{
		OutError = TEXT("Hash is required.");
		return false;
	}
	if (Request.Url.IsEmpty())
	{
		OutError = TEXT("URL is required.");
		return false;
	}

	Request.ChunkSizeBytes = Request.ChunkSizeBytes > 0 ? Request.ChunkSizeBytes : DefaultChunkSizeBytes;
	Request.MaxRetryCount = FMath::Clamp(Request.MaxRetryCount, 0, 20);
	Request.BaseRetryDelaySeconds = FMath::Max(Request.BaseRetryDelaySeconds, 0.1);
	Request.DiskSafetyMarginBytes = Request.DiskSafetyMarginBytes > 0 ? Request.DiskSafetyMarginBytes : DefaultSafetyMarginBytes;

	FString DirectoryError;
	if (!FNwiroIKContentPaths::EnsureBaseDirectories(DirectoryError))
	{
		OutError = DirectoryError;
		return false;
	}

	FOperation& Op = Operations.FindOrAdd(Request.Hash);
	if (Op.State == ENwiroIKContentOperationState::Downloading
		|| Op.State == ENwiroIKContentOperationState::Verifying
		|| Op.State == ENwiroIKContentOperationState::Unzipping
		|| Op.State == ENwiroIKContentOperationState::Importing)
	{
		OutError = TEXT("Operation is already running for this hash.");
		return false;
	}

	Op.Request = Request;
	if (Op.Request.ContentId.IsEmpty())
	{
		Op.Request.ContentId = Request.Hash;
	}
	Op.NormalizedHash = Request.Hash;
	Op.ZipPath = FNwiroIKContentPaths::MakeZipPath(Request.Hash);
	Op.PartialZipPath = FNwiroIKContentPaths::MakePartialZipPath(Request.Hash);
	Op.ExtractedPath = FNwiroIKContentPaths::MakeExtractedPath(Request.Hash);
	Op.UnzipMarkerPath = FNwiroIKContentPaths::MakeUnzipMarkerPath(Request.Hash);
	Op.ImportMarkerPath = FNwiroIKContentPaths::MakeImportMarkerPathForProject(Request.Hash);
	Op.ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Op.ProjectId = FNwiroIKContentPaths::SanitizePathToken(Op.ProjectPath);
	Op.ErrorCode.Reset();
	Op.ErrorMessage.Reset();
	Op.RetryCount = 0;
	Op.IntegrityRetryCount = 0;
	Op.LastProgressEventSeconds = 0.0;
	Op.LastProgressEventBytes = 0;
	Op.bCancelled = false;
	Op.bRangeSupportValidated = false;
	Op.bRangeSupported = false;
	Op.bRangeCheckInFlight = false;
	Op.bRangeGetProbeAttempted = false;
	Op.UpdatedAtUtc = FDateTime::UtcNow();
	Op.CreatedAtUtc = FDateTime::UtcNow();
	Op.Events.Reset();

	if (!CheckDiskSpace(Op, OutError))
	{
		Op.State = ENwiroIKContentOperationState::Failed;
		Op.ErrorCode = TEXT("insufficient_disk_space");
		Op.ErrorMessage = OutError;
		EmitEvent(Op, TEXT("download_fail"), Op.ErrorCode, Op.ErrorMessage);
		return false;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	Op.bZipDownloaded = PlatformFile.FileExists(*Op.ZipPath);
	Op.bUnzipComplete = PlatformFile.FileExists(*Op.UnzipMarkerPath);
	Op.bImportedInProject = PlatformFile.FileExists(*Op.ImportMarkerPath);

	if (Op.bZipDownloaded)
	{
		Op.DownloadedBytes = PlatformFile.FileSize(*Op.ZipPath);
		Op.TotalBytes = Op.Request.ExpectedZipSizeBytes > 0 ? Op.Request.ExpectedZipSizeBytes : Op.DownloadedBytes;
		Op.State = ENwiroIKContentOperationState::Verifying;
		Op.StageStartedAtUtc = FDateTime::UtcNow();
		StartIntegrityVerification(Op.NormalizedHash);
		return true;
	}

	Op.State = ENwiroIKContentOperationState::Downloading;
	Op.StageStartedAtUtc = FDateTime::UtcNow();
	Op.DownloadedBytes = 0;
	Op.TotalBytes = Op.Request.ExpectedZipSizeBytes > 0 ? Op.Request.ExpectedZipSizeBytes : -1;

	if (PlatformFile.FileExists(*Op.PartialZipPath))
	{
		const int64 PartialSize = PlatformFile.FileSize(*Op.PartialZipPath);
		if (PartialSize > 0)
		{
			Op.DownloadedBytes = PartialSize;
		}
	}

	EmitEvent(Op, TEXT("download_start"), TEXT(""), TEXT(""));
	SendNextChunkRequest(Op.NormalizedHash);
	return true;
}

bool FNwiroIKContentPipelineService::FImpl::StartImportFromCache(
	const FString& ContentId,
	const FString& Hash,
	const FString& RelativeImportPath,
	FString& OutError)
{
	if (bIsShuttingDown)
	{
		OutError = TEXT("Pipeline is shutting down.");
		return false;
	}

	const FString NormalizedHash = FNwiroIKContentPaths::NormalizeHash(Hash);
	if (NormalizedHash.IsEmpty())
	{
		OutError = TEXT("Hash is required.");
		return false;
	}

	FString DirectoryError;
	if (!FNwiroIKContentPaths::EnsureBaseDirectories(DirectoryError))
	{
		OutError = DirectoryError;
		return false;
	}

	FOperation& Op = Operations.FindOrAdd(NormalizedHash);
	if (Op.State == ENwiroIKContentOperationState::Downloading
		|| Op.State == ENwiroIKContentOperationState::Verifying
		|| Op.State == ENwiroIKContentOperationState::Unzipping
		|| Op.State == ENwiroIKContentOperationState::Importing)
	{
		OutError = TEXT("Operation is already running for this hash.");
		return false;
	}

	Op.NormalizedHash = NormalizedHash;
	Op.ZipPath = FNwiroIKContentPaths::MakeZipPath(NormalizedHash);
	Op.PartialZipPath = FNwiroIKContentPaths::MakePartialZipPath(NormalizedHash);
	Op.ExtractedPath = FNwiroIKContentPaths::MakeExtractedPath(NormalizedHash);
	Op.UnzipMarkerPath = FNwiroIKContentPaths::MakeUnzipMarkerPath(NormalizedHash);
	Op.ImportMarkerPath = FNwiroIKContentPaths::MakeImportMarkerPathForProject(NormalizedHash);
	Op.ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Op.ProjectId = FNwiroIKContentPaths::SanitizePathToken(Op.ProjectPath);
	Op.ErrorCode.Reset();
	Op.ErrorMessage.Reset();
	Op.bCancelled = false;
	Op.RetryCount = 0;
	Op.IntegrityRetryCount = 0;
	Op.bRangeSupportValidated = false;
	Op.bRangeSupported = false;
	Op.bRangeCheckInFlight = false;
	Op.bRangeGetProbeAttempted = false;
	Op.CreatedAtUtc = FDateTime::UtcNow();
	Op.UpdatedAtUtc = FDateTime::UtcNow();
	Op.StageStartedAtUtc = FDateTime::UtcNow();
	Op.Events.Reset();

	if (!ContentId.IsEmpty())
	{
		Op.Request.ContentId = ContentId;
	}
	else if (Op.Request.ContentId.IsEmpty())
	{
		Op.Request.ContentId = NormalizedHash;
	}
	Op.Request.Hash = NormalizedHash;
	Op.Request.bAutoImport = true;
	Op.Request.RelativeImportPath = RelativeImportPath;
	Op.Request.RelativeImportPath.TrimStartAndEndInline();

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	const bool bZipExists = PlatformFile.FileExists(*Op.ZipPath);
	const bool bExtractedExists = PlatformFile.DirectoryExists(*Op.ExtractedPath);
	const bool bUnzipMarkerExists = PlatformFile.FileExists(*Op.UnzipMarkerPath);

	if (!bZipExists && !bExtractedExists && !bUnzipMarkerExists)
	{
		OutError = TEXT("No cached artifact found for this hash.");
		return false;
	}

	Op.bZipDownloaded = bZipExists;
	Op.bUnzipComplete = bUnzipMarkerExists || bExtractedExists;

	if (Op.bZipDownloaded)
	{
		Op.DownloadedBytes = PlatformFile.FileSize(*Op.ZipPath);
		Op.TotalBytes = Op.DownloadedBytes;
	}

	if (Op.bUnzipComplete)
	{
		// If extracted content exists but marker is missing, write marker for consistency.
		if (bExtractedExists && !bUnzipMarkerExists)
		{
			const FString MarkerDir = FPaths::GetPath(Op.UnzipMarkerPath);
			if (!MarkerDir.IsEmpty() && !PlatformFile.DirectoryExists(*MarkerDir))
			{
				PlatformFile.CreateDirectoryTree(*MarkerDir);
			}
			FFileHelper::SaveStringToFile(FDateTime::UtcNow().ToIso8601(), *Op.UnzipMarkerPath);
		}

		StartImport(NormalizedHash);
		return true;
	}

	if (Op.bZipDownloaded)
	{
		StartUnzip(NormalizedHash);
		return true;
	}

	OutError = TEXT("Cached zip not found and extracted content is incomplete.");
	return false;
}

bool FNwiroIKContentPipelineService::FImpl::DeleteCachedContent(
	const FString& Hash,
	const bool bDeleteImportedInProject,
	FString& OutError)
{
	const FString NormalizedHash = FNwiroIKContentPaths::NormalizeHash(Hash);
	if (NormalizedHash.IsEmpty())
	{
		OutError = TEXT("Hash is required.");
		return false;
	}

	if (FOperation* Op = Operations.Find(NormalizedHash))
	{
		if (Op->State == ENwiroIKContentOperationState::Downloading
			|| Op->State == ENwiroIKContentOperationState::Verifying
			|| Op->State == ENwiroIKContentOperationState::Unzipping
			|| Op->State == ENwiroIKContentOperationState::Importing)
		{
			OutError = TEXT("Cannot delete cache while operation is running.");
			return false;
		}
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	const FString ZipPath = FNwiroIKContentPaths::MakeZipPath(NormalizedHash);
	const FString PartialPath = FNwiroIKContentPaths::MakePartialZipPath(NormalizedHash);
	const FString ExtractedPath = FNwiroIKContentPaths::MakeExtractedPath(NormalizedHash);
	const FString UnzipMarkerPath = FNwiroIKContentPaths::MakeUnzipMarkerPath(NormalizedHash);
	const FString CurrentImportMarker = FNwiroIKContentPaths::MakeImportMarkerPathForProject(NormalizedHash);

	TSet<FString> ImportedFoldersToDelete;
	if (bDeleteImportedInProject)
	{
		auto TryAddRelativeFolder = [&ImportedFoldersToDelete](const FString& InFolder) -> bool
		{
			FString RelativeFolder = InFolder;
			RelativeFolder.ReplaceInline(TEXT("\\"), TEXT("/"));
			RelativeFolder.TrimStartAndEndInline();

			RelativeFolder.RemoveFromStart(TEXT("/Game/"));
			RelativeFolder.RemoveFromStart(TEXT("Game/"));
			RelativeFolder.RemoveFromStart(TEXT("Content/"));
			while (RelativeFolder.StartsWith(TEXT("/")))
			{
				RelativeFolder.RightChopInline(1);
			}
			while (RelativeFolder.EndsWith(TEXT("/")))
			{
				RelativeFolder.LeftChopInline(1);
			}

			if (RelativeFolder.IsEmpty()
				|| RelativeFolder.Contains(TEXT(".."))
				|| RelativeFolder.Contains(TEXT(":")))
			{
				return false;
			}

			ImportedFoldersToDelete.Add(RelativeFolder);
			return true;
		};

		if (PlatformFile.FileExists(*CurrentImportMarker))
		{
			FString MarkerJsonText;
			if (FFileHelper::LoadFileToString(MarkerJsonText, *CurrentImportMarker))
			{
				TSharedPtr<FJsonObject> MarkerJson;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MarkerJsonText);
				if (FJsonSerializer::Deserialize(Reader, MarkerJson) && MarkerJson.IsValid())
				{
					const TArray<TSharedPtr<FJsonValue>>* ImportedFoldersField = nullptr;
					if (MarkerJson->TryGetArrayField(TEXT("imported_game_folders"), ImportedFoldersField))
					{
						for (const TSharedPtr<FJsonValue>& FolderValue : *ImportedFoldersField)
						{
							if (FolderValue.IsValid())
							{
								TryAddRelativeFolder(FolderValue->AsString());
							}
						}
					}

					FString RelativeImportPathFromMarker;
					if (MarkerJson->TryGetStringField(TEXT("relative_import_path"), RelativeImportPathFromMarker))
					{
						TryAddRelativeFolder(RelativeImportPathFromMarker);
					}
				}
			}
		}
	}

	if (PlatformFile.FileExists(*ZipPath) && !PlatformFile.DeleteFile(*ZipPath))
	{
		OutError = FString::Printf(TEXT("Failed to delete cache zip: %s"), *ZipPath);
		return false;
	}

	if (PlatformFile.FileExists(*PartialPath) && !PlatformFile.DeleteFile(*PartialPath))
	{
		OutError = FString::Printf(TEXT("Failed to delete partial zip: %s"), *PartialPath);
		return false;
	}

	if (PlatformFile.DirectoryExists(*ExtractedPath) && !PlatformFile.DeleteDirectoryRecursively(*ExtractedPath))
	{
		OutError = FString::Printf(TEXT("Failed to delete extracted directory: %s"), *ExtractedPath);
		return false;
	}

	if (PlatformFile.FileExists(*UnzipMarkerPath) && !PlatformFile.DeleteFile(*UnzipMarkerPath))
	{
		OutError = FString::Printf(TEXT("Failed to delete unzip marker: %s"), *UnzipMarkerPath);
		return false;
	}

	if (PlatformFile.FileExists(*CurrentImportMarker))
	{
		PlatformFile.DeleteFile(*CurrentImportMarker);
	}

	// Remove all project import markers for this hash (across project tokens).
	TArray<FString> MarkerFiles;
	IFileManager::Get().FindFilesRecursive(MarkerFiles, *FNwiroIKContentPaths::GetImportsRoot(), TEXT("*.import.ok"), true, false, false);
	const FString MarkerSuffix = NormalizedHash + TEXT(".import.ok");
	for (const FString& Marker : MarkerFiles)
	{
		if (Marker.EndsWith(MarkerSuffix, ESearchCase::IgnoreCase))
		{
			PlatformFile.DeleteFile(*Marker);
		}
	}

	if (bDeleteImportedInProject)
	{
		FString DeleteWarnings;

		for (const FString& Folder : ImportedFoldersToDelete)
		{
			const FString FolderPath = FPaths::Combine(FPaths::ProjectContentDir(), Folder);
			if (PlatformFile.DirectoryExists(*FolderPath))
			{
				if (!PlatformFile.DeleteDirectoryRecursively(*FolderPath))
				{
					if (!DeleteWarnings.IsEmpty())
					{
						DeleteWarnings += TEXT(" ");
					}
					DeleteWarnings += FString::Printf(TEXT("Failed to delete imported folder: %s"), *FolderPath);
				}
			}
		}

		if (ImportedFoldersToDelete.IsEmpty())
		{
			DeleteWarnings = TEXT("Cache deleted. Imported project assets were not removed because no import marker metadata was available.");
		}

		if (!DeleteWarnings.IsEmpty())
		{
			OutError = DeleteWarnings;
		}
	}

	Operations.Remove(NormalizedHash);
	return true;
}

FString FNwiroIKContentPipelineService::FImpl::GetCachedContentsJson() const
{
	TSet<FString> Hashes;

	TArray<FString> ZipNames;
	IFileManager::Get().FindFiles(ZipNames, *FPaths::Combine(FNwiroIKContentPaths::GetCompressedRoot(), TEXT("*.zip")), true, false);
	for (const FString& ZipName : ZipNames)
	{
		Hashes.Add(FPaths::GetBaseFilename(ZipName));
	}

	TArray<FString> ExtractedDirs;
	IFileManager::Get().FindFiles(ExtractedDirs, *FPaths::Combine(FNwiroIKContentPaths::GetExtractedRoot(), TEXT("*")), false, true);
	for (const FString& DirName : ExtractedDirs)
	{
		Hashes.Add(DirName);
	}

	for (const TPair<FString, FOperation>& Pair : Operations)
	{
		Hashes.Add(Pair.Key);
	}

	TArray<FString> SortedHashes = Hashes.Array();
	SortedHashes.Sort();

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FString& Hash : SortedHashes)
	{
		const FString NormalizedHash = FNwiroIKContentPaths::NormalizeHash(Hash);
		const FString ZipPath = FNwiroIKContentPaths::MakeZipPath(NormalizedHash);
		const FString ExtractedPath = FNwiroIKContentPaths::MakeExtractedPath(NormalizedHash);

		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("hash"), NormalizedHash);
		Item->SetStringField(TEXT("zip_path"), ZipPath);
		Item->SetStringField(TEXT("extracted_path"), ExtractedPath);
		Item->SetBoolField(TEXT("is_zip_downloaded"), IsZipDownloaded(NormalizedHash));
		Item->SetBoolField(TEXT("is_unzip_complete"), IsUnzipComplete(NormalizedHash));
		Item->SetBoolField(TEXT("is_imported_in_project"), IsImportedInCurrentProject(NormalizedHash));
		Item->SetBoolField(TEXT("is_content_cached"), IsContentCached(NormalizedHash));
		Item->SetNumberField(
			TEXT("zip_size_bytes"),
			FPlatformFileManager::Get().GetPlatformFile().FileExists(*ZipPath)
				? FPlatformFileManager::Get().GetPlatformFile().FileSize(*ZipPath)
				: 0);

		const FOperation* Op = Operations.Find(NormalizedHash);
		if (Op)
		{
			Item->SetStringField(TEXT("content_id"), Op->Request.ContentId);
			Item->SetStringField(TEXT("state"), StateToString(Op->State));
			Item->SetStringField(TEXT("error_code"), Op->ErrorCode);
			Item->SetStringField(TEXT("error_message"), Op->ErrorMessage);
		}
		else
		{
			Item->SetStringField(TEXT("content_id"), NormalizedHash);
			Item->SetStringField(TEXT("state"), TEXT("idle"));
			Item->SetStringField(TEXT("error_code"), TEXT(""));
			Item->SetStringField(TEXT("error_message"), TEXT(""));
		}

		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	Root->SetArrayField(TEXT("items"), Items);
	Root->SetNumberField(TEXT("count"), Items.Num());
	return ToJsonString(Root);
}

bool FNwiroIKContentPipelineService::FImpl::CancelOperation(const FString& ContentId, const FString& Hash, FString& OutError)
{
	(void)ContentId;

	const FString NormalizedHash = FNwiroIKContentPaths::NormalizeHash(Hash);
	FOperation* Op = Operations.Find(NormalizedHash);
	if (!Op)
	{
		OutError = TEXT("No operation found for hash.");
		return false;
	}

	Op->bCancelled = true;
	Op->State = ENwiroIKContentOperationState::Cancelled;
	Op->UpdatedAtUtc = FDateTime::UtcNow();
	Op->ErrorCode = TEXT("cancelled_by_user");
	Op->ErrorMessage = TEXT("Operation cancelled by user.");

	if (Op->ActiveRequest.IsValid())
	{
		Op->ActiveRequest->CancelRequest();
		Op->ActiveRequest.Reset();
	}

	EmitEvent(*Op, TEXT("download_cancel"), Op->ErrorCode, Op->ErrorMessage);
	return true;
}

void FNwiroIKContentPipelineService::FImpl::Shutdown()
{
	bIsShuttingDown = true;

	for (TPair<FString, FOperation>& Pair : Operations)
	{
		FOperation& Op = Pair.Value;
		Op.bCancelled = true;
		if (Op.ActiveRequest.IsValid())
		{
			Op.ActiveRequest->CancelRequest();
			Op.ActiveRequest.Reset();
		}
	}
}

FString FNwiroIKContentPipelineService::FImpl::GetOperationStatusJson(const FString& ContentId, const FString& Hash)
{
	const FString NormalizedHash = FNwiroIKContentPaths::NormalizeHash(Hash);
	const FOperation* Op = Operations.Find(NormalizedHash);
	if (!Op)
	{
		return BuildPassiveStatusJson(ContentId, NormalizedHash);
	}

	return BuildStatusJson(*Op);
}

FString FNwiroIKContentPipelineService::FImpl::GetContentStateJson(const FString& ContentId, const FString& Hash) const
{
	const FString NormalizedHash = FNwiroIKContentPaths::NormalizeHash(Hash);
	const bool bZipDownloaded = IsZipDownloaded(NormalizedHash);
	const bool bUnzipComplete = IsUnzipComplete(NormalizedHash);
	const bool bImported = IsImportedInCurrentProject(NormalizedHash);
	const bool bCached = bZipDownloaded && bUnzipComplete;

	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("content_id"), ContentId);
	Json->SetStringField(TEXT("hash"), NormalizedHash);
	Json->SetBoolField(TEXT("is_content_cached"), bCached);
	Json->SetBoolField(TEXT("is_zip_downloaded"), bZipDownloaded);
	Json->SetBoolField(TEXT("is_unzip_complete"), bUnzipComplete);
	Json->SetBoolField(TEXT("is_imported_in_project"), bImported);

	const FOperation* Op = Operations.Find(NormalizedHash);
	if (Op)
	{
		Json->SetStringField(TEXT("operation_state"), StateToString(Op->State));
		Json->SetNumberField(TEXT("progress"), GetOverallProgress(*Op));
		Json->SetStringField(TEXT("error_code"), Op->ErrorCode);
		Json->SetStringField(TEXT("error_message"), Op->ErrorMessage);
	}
	else
	{
		Json->SetStringField(TEXT("operation_state"), TEXT("idle"));
		Json->SetNumberField(TEXT("progress"), 0.0);
		Json->SetStringField(TEXT("error_code"), TEXT(""));
		Json->SetStringField(TEXT("error_message"), TEXT(""));
	}

	return ToJsonString(Json);
}

bool FNwiroIKContentPipelineService::FImpl::IsContentCached(const FString& Hash) const
{
	const FString NormalizedHash = FNwiroIKContentPaths::NormalizeHash(Hash);
	return IsZipDownloaded(NormalizedHash) && IsUnzipComplete(NormalizedHash);
}

bool FNwiroIKContentPipelineService::FImpl::IsZipDownloaded(const FString& Hash) const
{
	const FString NormalizedHash = FNwiroIKContentPaths::NormalizeHash(Hash);
	return FPlatformFileManager::Get().GetPlatformFile().FileExists(*FNwiroIKContentPaths::MakeZipPath(NormalizedHash));
}

bool FNwiroIKContentPipelineService::FImpl::IsUnzipComplete(const FString& Hash) const
{
	const FString NormalizedHash = FNwiroIKContentPaths::NormalizeHash(Hash);
	return FPlatformFileManager::Get().GetPlatformFile().FileExists(*FNwiroIKContentPaths::MakeUnzipMarkerPath(NormalizedHash));
}

bool FNwiroIKContentPipelineService::FImpl::IsImportedInCurrentProject(const FString& Hash) const
{
	const FString NormalizedHash = FNwiroIKContentPaths::NormalizeHash(Hash);
	return FPlatformFileManager::Get().GetPlatformFile().FileExists(*FNwiroIKContentPaths::MakeImportMarkerPathForProject(NormalizedHash));
}

void FNwiroIKContentPipelineService::FImpl::StartRangeSupportCheck(const FString& NormalizedHash, const bool bUseRangeGetProbe)
{
	FOperation* Op = Operations.Find(NormalizedHash);
	if (!Op || Op->bCancelled || bIsShuttingDown)
	{
		return;
	}

	if (Op->bRangeSupportValidated || Op->bRangeCheckInFlight)
	{
		return;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Op->Request.Url);
	Request->SetVerb(bUseRangeGetProbe ? TEXT("GET") : TEXT("HEAD"));
	Request->SetTimeout(30.0f);
	if (bUseRangeGetProbe)
	{
		Request->SetHeader(TEXT("Range"), TEXT("bytes=0-0"));
	}
	Request->OnProcessRequestComplete().BindLambda(
		[this, NormalizedHash, bUseRangeGetProbe](FHttpRequestPtr HttpRequest, FHttpResponsePtr Response, bool bWasSuccessful)
		{
			OnRangeSupportCheckComplete(NormalizedHash, HttpRequest, Response, bWasSuccessful, bUseRangeGetProbe);
		});

	Op->bRangeCheckInFlight = true;
	Op->ActiveRequest = Request;
	Op->UpdatedAtUtc = FDateTime::UtcNow();

	if (!Request->ProcessRequest())
	{
		Op->bRangeCheckInFlight = false;
		Op->ActiveRequest.Reset();
		HandleRetryOrFailure(
			NormalizedHash,
			TEXT("http_process_failed"),
			TEXT("Unable to start HTTP preflight request."),
			false);
	}
}

void FNwiroIKContentPipelineService::FImpl::OnRangeSupportCheckComplete(
	const FString& NormalizedHash,
	const FHttpRequestPtr& HttpRequest,
	const FHttpResponsePtr& Response,
	const bool bWasSuccessful,
	const bool bWasRangeGetProbe)
{
	(void)HttpRequest;

	FOperation* Op = Operations.Find(NormalizedHash);
	if (!Op)
	{
		return;
	}

	Op->bRangeCheckInFlight = false;
	Op->ActiveRequest.Reset();

	if (Op->bCancelled || bIsShuttingDown)
	{
		return;
	}

	if (!bWasSuccessful || !Response.IsValid())
	{
		HandleRetryOrFailure(
			NormalizedHash,
			TEXT("network_error"),
			TEXT("HTTP preflight request failed or returned invalid response."),
			false);
		return;
	}

	const int32 ResponseCode = Response->GetResponseCode();
	if (bWasRangeGetProbe)
	{
		if (ResponseCode == 206)
		{
			const FString ContentRange = Response->GetHeader(TEXT("Content-Range")).TrimStartAndEnd();
			if (Op->TotalBytes <= 0 && !ContentRange.IsEmpty())
			{
				int32 SlashIndex = INDEX_NONE;
				if (ContentRange.FindChar(TEXT('/'), SlashIndex))
				{
					const FString TotalSize = ContentRange.Mid(SlashIndex + 1);
					const int64 ParsedTotal = FCString::Atoi64(*TotalSize);
					if (ParsedTotal > 0)
					{
						Op->TotalBytes = ParsedTotal;
					}
				}
			}

			Op->bRangeSupportValidated = true;
			Op->bRangeSupported = true;
			SendNextChunkRequest(NormalizedHash);
			return;
		}

		if (ResponseCode == 200)
		{
			Op->State = ENwiroIKContentOperationState::Failed;
			Op->ErrorCode = TEXT("range_not_supported");
			Op->ErrorMessage = TEXT("Server ignored range probe (bytes=0-0). Byte-range requests are required for large files.");
			Op->UpdatedAtUtc = FDateTime::UtcNow();
			EmitEvent(*Op, TEXT("download_fail"), Op->ErrorCode, Op->ErrorMessage);
			return;
		}

		HandleRetryOrFailure(
			NormalizedHash,
			FString::Printf(TEXT("http_%d"), ResponseCode),
			FString::Printf(TEXT("Unexpected HTTP range probe response code: %d"), ResponseCode),
			false);
		return;
	}

	// HEAD preflight path: record content length if available, then verify real GET range behavior.
	if (ResponseCode >= 200 && ResponseCode < 400)
	{
		const FString ContentLengthHeader = Response->GetHeader(TEXT("Content-Length")).TrimStartAndEnd();
		if (Op->TotalBytes <= 0 && !ContentLengthHeader.IsEmpty())
		{
			const int64 ParsedLength = FCString::Atoi64(*ContentLengthHeader);
			if (ParsedLength > 0)
			{
				Op->TotalBytes = ParsedLength;
			}
		}

		if (!Op->bRangeGetProbeAttempted)
		{
			Op->bRangeGetProbeAttempted = true;
			StartRangeSupportCheck(NormalizedHash, true);
			return;
		}

		Op->State = ENwiroIKContentOperationState::Failed;
		Op->ErrorCode = TEXT("range_not_supported");
		Op->ErrorMessage = TEXT("Server did not pass byte-range probe.");
		Op->UpdatedAtUtc = FDateTime::UtcNow();
		EmitEvent(*Op, TEXT("download_fail"), Op->ErrorCode, Op->ErrorMessage);
		return;
	}

	if ((ResponseCode == 405 || ResponseCode == 501) && !Op->bRangeGetProbeAttempted)
	{
		Op->bRangeGetProbeAttempted = true;
		StartRangeSupportCheck(NormalizedHash, true);
		return;
	}

	HandleRetryOrFailure(
		NormalizedHash,
		FString::Printf(TEXT("http_%d"), ResponseCode),
		FString::Printf(TEXT("Unexpected HTTP preflight response code: %d"), ResponseCode),
		false);
}

void FNwiroIKContentPipelineService::FImpl::SendNextChunkRequest(const FString& NormalizedHash)
{
	FOperation* Op = Operations.Find(NormalizedHash);
	if (!Op || Op->bCancelled || bIsShuttingDown)
	{
		return;
	}

	// Skip the HEAD/Range preflight entirely. Many CDNs (Cloudflare R2, S3
	// presigned URLs, etc.) only sign a specific method/header set, so a HEAD
	// or Range probe returns 403 even though the actual GET works. Assume
	// range support; if the server actually doesn't support it, we'll get the
	// full file back in one chunk and finish anyway.
	if (!Op->bRangeSupportValidated)
	{
		Op->bRangeSupportValidated = true;
		Op->bRangeSupported = true;
	}

	if (Op->TotalBytes > 0 && Op->DownloadedBytes >= Op->TotalBytes)
	{
		FinalizeDownload(NormalizedHash);
		return;
	}

	const int64 ChunkSize = Op->Request.ChunkSizeBytes;
	const int64 StartOffset = Op->DownloadedBytes;
	const int64 EndOffset = (Op->TotalBytes > 0)
		? FMath::Min(StartOffset + ChunkSize - 1, Op->TotalBytes - 1)
		: StartOffset + ChunkSize - 1;

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Op->Request.Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(120.0f);
	Request->SetHeader(TEXT("Range"), FString::Printf(TEXT("bytes=%lld-%lld"), StartOffset, EndOffset));

#if (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
	Request->OnRequestProgress64().BindLambda([this, NormalizedHash](FHttpRequestPtr HttpRequest, uint64 BytesSent, uint64 BytesReceived)
	{
		OnDownloadProgress(NormalizedHash, static_cast<int64>(BytesReceived));
	});
#else
	Request->OnRequestProgress().BindLambda([this, NormalizedHash](FHttpRequestPtr HttpRequest, int32 BytesSent, int32 BytesReceived)
	{
		OnDownloadProgress(NormalizedHash, static_cast<int64>(BytesReceived));
	});
#endif

	Request->OnProcessRequestComplete().BindLambda(
		[this, NormalizedHash](FHttpRequestPtr HttpRequest, FHttpResponsePtr Response, bool bWasSuccessful)
		{
			OnChunkResponse(NormalizedHash, HttpRequest, Response, bWasSuccessful);
		});

	Op->ActiveRequest = Request;
	Op->UpdatedAtUtc = FDateTime::UtcNow();

	if (!Request->ProcessRequest())
	{
		HandleRetryOrFailure(
			NormalizedHash,
			TEXT("http_process_failed"),
			TEXT("Unable to start HTTP request."),
			false);
	}
}

void FNwiroIKContentPipelineService::FImpl::OnDownloadProgress(const FString& NormalizedHash, const int64 RequestBytesReceived)
{
	FOperation* Op = Operations.Find(NormalizedHash);
	if (!Op || Op->bCancelled || Op->State != ENwiroIKContentOperationState::Downloading)
	{
		return;
	}

	const int64 CurrentDownload = Op->DownloadedBytes + RequestBytesReceived;
	// Only report a percentage when we ACTUALLY know the full file size.
	// Falling back to `Total = CurrentDownload` produced progress=1.0 on
	// every early progress callback (before Content-Range was parsed),
	// causing the bar to jump to 100% then drop back.
	const double CurrentProgress = Op->TotalBytes > 0
		? FMath::Clamp(static_cast<double>(CurrentDownload) / static_cast<double>(Op->TotalBytes), 0.0, 1.0)
		: 0.0;

	const double Now = FPlatformTime::Seconds();
	const bool bPassedTimeGate = (Now - Op->LastProgressEventSeconds) >= ProgressEventMinIntervalSeconds;
	const bool bPassedSizeGate = (CurrentDownload - Op->LastProgressEventBytes) >= ProgressEventMinBytes;
	if (bPassedTimeGate || bPassedSizeGate)
	{
		Op->LastProgressEventSeconds = Now;
		Op->LastProgressEventBytes = CurrentDownload;
		EmitEvent(*Op, TEXT("download_progress"), TEXT(""), TEXT(""), CurrentDownload, Op->TotalBytes, CurrentProgress);
	}
}

void FNwiroIKContentPipelineService::FImpl::OnChunkResponse(
	const FString& NormalizedHash,
	const FHttpRequestPtr& HttpRequest,
	const FHttpResponsePtr& Response,
	const bool bWasSuccessful)
{
	(void)HttpRequest;

	FOperation* Op = Operations.Find(NormalizedHash);
	if (!Op)
	{
		return;
	}

	Op->ActiveRequest.Reset();

	if (Op->bCancelled || bIsShuttingDown)
	{
		return;
	}

	if (!bWasSuccessful || !Response.IsValid())
	{
		HandleRetryOrFailure(
			NormalizedHash,
			TEXT("network_error"),
			TEXT("HTTP request failed or returned invalid response."),
			false);
		return;
	}

	const int32 ResponseCode = Response->GetResponseCode();
	const bool bPartialResponse = ResponseCode == 206;
	const bool bFullResponse = ResponseCode == 200 && Op->DownloadedBytes == 0;
	if (!bPartialResponse && !bFullResponse)
	{
		HandleRetryOrFailure(
			NormalizedHash,
			FString::Printf(TEXT("http_%d"), ResponseCode),
			FString::Printf(TEXT("Unexpected HTTP response code: %d"), ResponseCode),
			false);
		return;
	}

	if (Op->TotalBytes <= 0)
	{
		const FString ContentRange = Response->GetHeader(TEXT("Content-Range"));
		if (!ContentRange.IsEmpty())
		{
			int32 SlashIndex = INDEX_NONE;
			if (ContentRange.FindChar(TEXT('/'), SlashIndex))
			{
				const FString TotalSize = ContentRange.Mid(SlashIndex + 1);
				Op->TotalBytes = FCString::Atoi64(*TotalSize);
			}
		}

		if (Op->TotalBytes <= 0)
		{
			const int64 ContentLength = Response->GetContentLength();
			if (ContentLength > 0 && bFullResponse)
			{
				Op->TotalBytes = ContentLength;
			}
		}
	}

	const TArray<uint8>& Payload = Response->GetContent();
	if (Payload.IsEmpty())
	{
		HandleRetryOrFailure(
			NormalizedHash,
			TEXT("empty_payload"),
			TEXT("Received an empty download payload."),
			false);
		return;
	}

	if (!FFileHelper::SaveArrayToFile(Payload, *Op->PartialZipPath, &IFileManager::Get(), FILEWRITE_Append))
	{
		HandleRetryOrFailure(
			NormalizedHash,
			TEXT("file_write_failed"),
			TEXT("Failed to append download chunk to partial file."),
			false);
		return;
	}

	Op->DownloadedBytes += Payload.Num();
	Op->RetryCount = 0;
	Op->UpdatedAtUtc = FDateTime::UtcNow();

	if (Op->TotalBytes > 0 && Op->DownloadedBytes >= Op->TotalBytes)
	{
		FinalizeDownload(NormalizedHash);
		return;
	}

	if (bFullResponse)
	{
		FinalizeDownload(NormalizedHash);
		return;
	}

	SendNextChunkRequest(NormalizedHash);
}

void FNwiroIKContentPipelineService::FImpl::FinalizeDownload(const FString& NormalizedHash)
{
	FOperation* Op = Operations.Find(NormalizedHash);
	if (!Op || Op->bCancelled)
	{
		return;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*Op->ZipPath))
	{
		PlatformFile.DeleteFile(*Op->ZipPath);
	}

	const bool bMoveSuccess = PlatformFile.MoveFile(*Op->ZipPath, *Op->PartialZipPath);
	if (!bMoveSuccess)
	{
		HandleRetryOrFailure(
			NormalizedHash,
			TEXT("finalize_move_failed"),
			TEXT("Failed to finalize download file."),
			true);
		return;
	}

	Op->bZipDownloaded = true;
	Op->UpdatedAtUtc = FDateTime::UtcNow();
	EmitEvent(*Op, TEXT("download_success"), TEXT(""), TEXT(""), Op->DownloadedBytes, Op->TotalBytes, 1.0);
	StartIntegrityVerification(NormalizedHash);
}

void FNwiroIKContentPipelineService::FImpl::StartIntegrityVerification(const FString& NormalizedHash)
{
	FOperation* Op = Operations.Find(NormalizedHash);
	if (!Op || Op->bCancelled)
	{
		return;
	}

	Op->State = ENwiroIKContentOperationState::Verifying;
	Op->StageStartedAtUtc = FDateTime::UtcNow();
	Op->UpdatedAtUtc = FDateTime::UtcNow();

	const FString ZipPath = Op->ZipPath;
	const FString ExpectedSHA256 = Op->Request.ExpectedSHA256.TrimStartAndEnd();
	const FString ExpectedMD5 = Op->Request.ExpectedMD5.TrimStartAndEnd();
	const int32 MaxRetryCount = Op->Request.MaxRetryCount;

	Async(EAsyncExecution::ThreadPool, [this, NormalizedHash, ZipPath, ExpectedSHA256, ExpectedMD5, MaxRetryCount]()
	{
		bool bVerifySuccess = true;
		bool bDidHashVerification = false;
		FString VerifyErrorCode;
		FString VerifyErrorMessage;

		if (!ExpectedSHA256.IsEmpty() || !ExpectedMD5.IsEmpty())
		{
			bDidHashVerification = true;

			FString ComputedHash;
			FString ComputeError;
			if (!ExpectedSHA256.IsEmpty())
			{
				if (!ComputeFileHash(ZipPath, true, ComputedHash, ComputeError))
				{
					// SHA256 fallback to MD5 only if caller explicitly provided MD5.
					if (!ExpectedMD5.IsEmpty())
					{
						if (!ComputeFileHash(ZipPath, false, ComputedHash, ComputeError)
							|| !ComputedHash.Equals(ExpectedMD5, ESearchCase::IgnoreCase))
						{
							bVerifySuccess = false;
							VerifyErrorCode = TEXT("hash_mismatch");
							VerifyErrorMessage = TEXT("Hash verification failed (SHA256 fallback MD5 mismatch).");
						}
					}
					else
					{
						bVerifySuccess = false;
						VerifyErrorCode = TEXT("sha256_unavailable");
						VerifyErrorMessage = ComputeError.IsEmpty()
							? TEXT("SHA256 verification failed.")
							: ComputeError;
					}
				}
				else if (!ComputedHash.Equals(ExpectedSHA256, ESearchCase::IgnoreCase))
				{
					bVerifySuccess = false;
					VerifyErrorCode = TEXT("hash_mismatch");
					VerifyErrorMessage = TEXT("SHA256 mismatch.");
				}
			}
			else if (!ExpectedMD5.IsEmpty())
			{
				if (!ComputeFileHash(ZipPath, false, ComputedHash, ComputeError)
					|| !ComputedHash.Equals(ExpectedMD5, ESearchCase::IgnoreCase))
				{
					bVerifySuccess = false;
					VerifyErrorCode = TEXT("hash_mismatch");
					VerifyErrorMessage = TEXT("MD5 mismatch.");
				}
			}
		}

		AsyncTask(ENamedThreads::GameThread, [this, NormalizedHash, bVerifySuccess, bDidHashVerification, VerifyErrorCode, VerifyErrorMessage, MaxRetryCount]()
		{
			FOperation* LocalOp = Operations.Find(NormalizedHash);
			if (!LocalOp || LocalOp->bCancelled || bIsShuttingDown)
			{
				return;
			}

			if (!bVerifySuccess)
			{
				++LocalOp->IntegrityRetryCount;
				if (LocalOp->IntegrityRetryCount <= MaxRetryCount)
				{
					HandleRetryOrFailure(
						NormalizedHash,
						VerifyErrorCode.IsEmpty() ? TEXT("hash_mismatch") : VerifyErrorCode,
						VerifyErrorMessage.IsEmpty() ? TEXT("File integrity check failed.") : VerifyErrorMessage,
						true);
				}
				else
				{
					LocalOp->State = ENwiroIKContentOperationState::Failed;
					LocalOp->ErrorCode = VerifyErrorCode.IsEmpty() ? TEXT("hash_mismatch") : VerifyErrorCode;
					LocalOp->ErrorMessage = VerifyErrorMessage.IsEmpty() ? TEXT("File integrity check failed.") : VerifyErrorMessage;
					EmitEvent(*LocalOp, TEXT("download_fail"), LocalOp->ErrorCode, LocalOp->ErrorMessage);
				}
				return;
			}

			LocalOp->bHashVerified = bDidHashVerification;

			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
			const bool bUnzipMarkerExists = PlatformFile.FileExists(*LocalOp->UnzipMarkerPath);
			const bool bExtractedExists = PlatformFile.DirectoryExists(*LocalOp->ExtractedPath);
			if (bUnzipMarkerExists || bExtractedExists)
			{
				LocalOp->bUnzipComplete = true;

				// Keep marker/state consistent if extracted content exists but marker is missing.
				if (bExtractedExists && !bUnzipMarkerExists)
				{
					const FString MarkerDir = FPaths::GetPath(LocalOp->UnzipMarkerPath);
					if (!MarkerDir.IsEmpty() && !PlatformFile.DirectoryExists(*MarkerDir))
					{
						PlatformFile.CreateDirectoryTree(*MarkerDir);
					}
					FFileHelper::SaveStringToFile(FDateTime::UtcNow().ToIso8601(), *LocalOp->UnzipMarkerPath);
				}

				if (!LocalOp->Request.bAutoImport)
				{
					LocalOp->State = ENwiroIKContentOperationState::Completed;
					LocalOp->UpdatedAtUtc = FDateTime::UtcNow();
					return;
				}

				StartImport(NormalizedHash);
				return;
			}

			StartUnzip(NormalizedHash);
		});
	});
}

void FNwiroIKContentPipelineService::FImpl::StartUnzip(const FString& NormalizedHash)
{
	FOperation* Op = Operations.Find(NormalizedHash);
	if (!Op || Op->bCancelled)
	{
		return;
	}

	Op->State = ENwiroIKContentOperationState::Unzipping;
	Op->StageStartedAtUtc = FDateTime::UtcNow();
	Op->UpdatedAtUtc = FDateTime::UtcNow();
	EmitEvent(*Op, TEXT("unzip_start"), TEXT(""), TEXT(""));

	const FString ZipPath = Op->ZipPath;
	const FString ExtractedPath = Op->ExtractedPath;
	const FString UnzipMarkerPath = Op->UnzipMarkerPath;
	const bool bStripRootFolder = Op->Request.bStripRootFolderOnUnzip;

	Async(EAsyncExecution::ThreadPool, [this, NormalizedHash, ZipPath, ExtractedPath, UnzipMarkerPath, bStripRootFolder]()
	{
		FString Error;
		bool bSuccess = true;

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (PlatformFile.DirectoryExists(*ExtractedPath))
		{
			PlatformFile.DeleteDirectoryRecursively(*ExtractedPath);
		}
		if (!PlatformFile.CreateDirectoryTree(*ExtractedPath))
		{
			bSuccess = false;
			Error = FString::Printf(TEXT("Failed to create extracted path: %s"), *ExtractedPath);
		}

		if (bSuccess)
		{
			bSuccess = FNwiroIKZipExtractor::ExtractZip(ZipPath, ExtractedPath, bStripRootFolder, Error);
		}

		if (bSuccess)
		{
			const FString MarkerDir = FPaths::GetPath(UnzipMarkerPath);
			if (!MarkerDir.IsEmpty() && !PlatformFile.DirectoryExists(*MarkerDir))
			{
				PlatformFile.CreateDirectoryTree(*MarkerDir);
			}
			if (!FFileHelper::SaveStringToFile(FDateTime::UtcNow().ToIso8601(), *UnzipMarkerPath))
			{
				bSuccess = false;
				Error = TEXT("Failed to write unzip completion marker.");
			}
		}

		AsyncTask(ENamedThreads::GameThread, [this, NormalizedHash, bSuccess, Error]()
		{
			FOperation* LocalOp = Operations.Find(NormalizedHash);
			if (!LocalOp || LocalOp->bCancelled || bIsShuttingDown)
			{
				return;
			}

			if (!bSuccess)
			{
				LocalOp->State = ENwiroIKContentOperationState::Failed;
				LocalOp->ErrorCode = TEXT("unzip_failed");
				LocalOp->ErrorMessage = Error;
				EmitEvent(*LocalOp, TEXT("unzip_fail"), LocalOp->ErrorCode, LocalOp->ErrorMessage);
				return;
			}

			LocalOp->bUnzipComplete = true;
			EmitEvent(*LocalOp, TEXT("unzip_success"), TEXT(""), TEXT(""));

			if (!LocalOp->Request.bAutoImport)
			{
				LocalOp->State = ENwiroIKContentOperationState::Completed;
				LocalOp->UpdatedAtUtc = FDateTime::UtcNow();
				return;
			}

			StartImport(NormalizedHash);
		});
	});
}

void FNwiroIKContentPipelineService::FImpl::StartImport(const FString& NormalizedHash)
{
	FOperation* Op = Operations.Find(NormalizedHash);
	if (!Op || Op->bCancelled)
	{
		return;
	}

	Op->State = ENwiroIKContentOperationState::Importing;
	Op->StageStartedAtUtc = FDateTime::UtcNow();
	Op->UpdatedAtUtc = FDateTime::UtcNow();
	EmitEvent(*Op, TEXT("import_start"), TEXT(""), TEXT(""));

	const FString ExtractedPath = Op->ExtractedPath;
	const FString RelativeImportPath = Op->Request.RelativeImportPath;
	const FString ImportMarkerPath = Op->ImportMarkerPath;

	Async(EAsyncExecution::ThreadPool, [this, NormalizedHash, ExtractedPath, RelativeImportPath, ImportMarkerPath]()
	{
		const FNwiroIKImportResult ImportResult = FNwiroIKContentImporter::ImportToCurrentProject(ExtractedPath, RelativeImportPath);

		AsyncTask(ENamedThreads::GameThread, [this, NormalizedHash, ImportResult, ImportMarkerPath]()
		{
			FOperation* LocalOp = Operations.Find(NormalizedHash);
			if (!LocalOp || LocalOp->bCancelled || bIsShuttingDown)
			{
				return;
			}

			if (!ImportResult.bSuccess)
			{
				LocalOp->State = ENwiroIKContentOperationState::Failed;
				LocalOp->ErrorCode = TEXT("import_failed");
				LocalOp->ErrorMessage = ImportResult.Error;
				EmitEvent(*LocalOp, TEXT("import_fail"), LocalOp->ErrorCode, LocalOp->ErrorMessage);
				return;
			}

			RefreshAssetRegistry(ImportResult);

			const FString MarkerDir = FPaths::GetPath(ImportMarkerPath);
			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
			if (!MarkerDir.IsEmpty() && !PlatformFile.DirectoryExists(*MarkerDir))
			{
				PlatformFile.CreateDirectoryTree(*MarkerDir);
			}

			TSharedRef<FJsonObject> MarkerJson = MakeShared<FJsonObject>();
			MarkerJson->SetStringField(TEXT("project_path"), LocalOp->ProjectPath);
			MarkerJson->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
			MarkerJson->SetStringField(TEXT("relative_import_path"), LocalOp->Request.RelativeImportPath);

			TSet<FString> UniqueFolders;
			for (const FString& Folder : ImportResult.GameFoldersToSync)
			{
				if (!Folder.IsEmpty())
				{
					UniqueFolders.Add(Folder);
				}
			}
			TArray<FString> SortedFolders = UniqueFolders.Array();
			SortedFolders.Sort();

			TArray<TSharedPtr<FJsonValue>> ImportedFoldersJson;
			for (const FString& Folder : SortedFolders)
			{
				ImportedFoldersJson.Add(MakeShared<FJsonValueString>(Folder));
			}
			MarkerJson->SetArrayField(TEXT("imported_game_folders"), ImportedFoldersJson);

			FFileHelper::SaveStringToFile(ToJsonString(MarkerJson), *ImportMarkerPath);

			LocalOp->bImportedInProject = true;
			LocalOp->State = ENwiroIKContentOperationState::Completed;
			LocalOp->UpdatedAtUtc = FDateTime::UtcNow();
			EmitEvent(*LocalOp, TEXT("import_success"), TEXT(""), TEXT(""));
		});
	});
}

void FNwiroIKContentPipelineService::FImpl::RefreshAssetRegistry(const FNwiroIKImportResult& ImportResult)
{
	if (!ImportResult.AbsolutePathsToScan.IsEmpty())
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRegistryModule.Get().ScanPathsSynchronous(ImportResult.AbsolutePathsToScan, true);
	}

	if (!ImportResult.GameFoldersToSync.IsEmpty())
	{
		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		ContentBrowserModule.Get().SyncBrowserToFolders(ImportResult.GameFoldersToSync);
	}
}

void FNwiroIKContentPipelineService::FImpl::HandleRetryOrFailure(
	const FString& NormalizedHash,
	const FString& ErrorCode,
	const FString& ErrorMessage,
	const bool bRestartFromScratch)
{
	FOperation* Op = Operations.Find(NormalizedHash);
	if (!Op || Op->bCancelled || bIsShuttingDown)
	{
		return;
	}

	++Op->RetryCount;
	EmitEvent(*Op, TEXT("download_fail"), ErrorCode, ErrorMessage);

	if (Op->RetryCount > Op->Request.MaxRetryCount)
	{
		Op->State = ENwiroIKContentOperationState::Failed;
		Op->ErrorCode = ErrorCode;
		Op->ErrorMessage = ErrorMessage;
		Op->UpdatedAtUtc = FDateTime::UtcNow();
		return;
	}

	const double DelaySeconds = FMath::Pow(2.0, static_cast<double>(Op->RetryCount - 1)) * Op->Request.BaseRetryDelaySeconds;
	Op->UpdatedAtUtc = FDateTime::UtcNow();

	Async(EAsyncExecution::ThreadPool, [this, NormalizedHash, DelaySeconds, bRestartFromScratch]()
	{
		FPlatformProcess::Sleep(static_cast<float>(DelaySeconds));

		AsyncTask(ENamedThreads::GameThread, [this, NormalizedHash, bRestartFromScratch]()
		{
			FOperation* LocalOp = Operations.Find(NormalizedHash);
			if (!LocalOp || LocalOp->bCancelled || bIsShuttingDown)
			{
				return;
			}

			if (bRestartFromScratch)
			{
				IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
				if (PlatformFile.FileExists(*LocalOp->ZipPath))
				{
					PlatformFile.DeleteFile(*LocalOp->ZipPath);
				}
				if (PlatformFile.FileExists(*LocalOp->PartialZipPath))
				{
					PlatformFile.DeleteFile(*LocalOp->PartialZipPath);
				}
				LocalOp->bZipDownloaded = false;
				LocalOp->DownloadedBytes = 0;
				LocalOp->TotalBytes = LocalOp->Request.ExpectedZipSizeBytes > 0 ? LocalOp->Request.ExpectedZipSizeBytes : -1;
			}

			LocalOp->State = ENwiroIKContentOperationState::Downloading;
			LocalOp->StageStartedAtUtc = FDateTime::UtcNow();
			LocalOp->bRangeSupportValidated = false;
			LocalOp->bRangeSupported = false;
			LocalOp->bRangeCheckInFlight = false;
			LocalOp->bRangeGetProbeAttempted = false;
			SendNextChunkRequest(NormalizedHash);
		});
	});
}

bool FNwiroIKContentPipelineService::FImpl::CheckDiskSpace(const FOperation& Op, FString& OutError) const
{
	const int64 ZipBudget = Op.Request.ExpectedZipSizeBytes > 0
		? Op.Request.ExpectedZipSizeBytes
		: (2LL * 1024LL * 1024LL * 1024LL);
	const int64 UnzipBudget = Op.Request.EstimatedUnzippedSizeBytes > 0
		? Op.Request.EstimatedUnzippedSizeBytes
		: (ZipBudget * 2);

	const uint64 RequiredBytes = static_cast<uint64>(
		FMath::Max<int64>(0, ZipBudget + UnzipBudget + Op.Request.DiskSafetyMarginBytes));

	uint64 TotalBytes = 0;
	uint64 FreeBytes = 0;
	if (!FPlatformMisc::GetDiskTotalAndFreeSpace(FNwiroIKContentPaths::GetDepotRoot(), TotalBytes, FreeBytes))
	{
		OutError = TEXT("Could not query free disk space for cache location.");
		return false;
	}

	if (FreeBytes < RequiredBytes)
	{
		OutError = FString::Printf(
			TEXT("Insufficient disk space. Required: %.2f GB, Available: %.2f GB."),
			static_cast<double>(RequiredBytes) / (1024.0 * 1024.0 * 1024.0),
			static_cast<double>(FreeBytes) / (1024.0 * 1024.0 * 1024.0));
		return false;
	}

	return true;
}

bool FNwiroIKContentPipelineService::FImpl::ComputeFileHash(
	const FString& FilePath,
	const bool bSHA256,
	FString& OutHash,
	FString& OutError) const
{
	OutHash.Reset();
	OutError.Reset();

	// Use UE's built-in MD5 hashing for platform-independent fallback support.
	if (!bSHA256)
	{
		const FMD5Hash MD5Hash = FMD5Hash::HashFile(*FilePath);
		if (MD5Hash.IsValid())
		{
			OutHash = BytesToHex(MD5Hash.GetBytes(), MD5Hash.GetSize());
			OutHash.ToLowerInline();
			return true;
		}

		OutError = TEXT("Unable to compute MD5 hash.");
		return false;
	}

	int32 ReturnCode = -1;
	FString StdOut;
	FString StdErr;

#if PLATFORM_WINDOWS
	const FString Algorithm = TEXT("SHA256");
	const FString Args = FString::Printf(TEXT("-hashfile \"%s\" %s"), *FilePath, *Algorithm);
	FPlatformProcess::ExecProcess(TEXT("certutil"), *Args, &ReturnCode, &StdOut, &StdErr);
	const int32 ExpectedLen = 64;
	if (ReturnCode == 0 && TryExtractHashFromOutput(StdOut, ExpectedLen, OutHash))
	{
		return true;
	}
#elif PLATFORM_MAC
	const FString Args = FString::Printf(TEXT("-a 256 \"%s\""), *FilePath);
	FPlatformProcess::ExecProcess(TEXT("shasum"), *Args, &ReturnCode, &StdOut, &StdErr);
	if (ReturnCode == 0 && TryExtractHashFromOutput(StdOut, 64, OutHash))
	{
		return true;
	}
#elif PLATFORM_LINUX
	const FString Args = FString::Printf(TEXT("\"%s\""), *FilePath);
	FPlatformProcess::ExecProcess(TEXT("sha256sum"), *Args, &ReturnCode, &StdOut, &StdErr);
	if (ReturnCode == 0 && TryExtractHashFromOutput(StdOut, 64, OutHash))
	{
		return true;
	}
#endif

	OutError = StdErr.IsEmpty() ? StdOut : StdErr;
	if (OutError.IsEmpty())
	{
		OutError = TEXT("Unable to compute SHA256 hash.");
	}
	return false;
}

double FNwiroIKContentPipelineService::FImpl::GetOverallProgress(const FOperation& Op) const
{
	switch (Op.State)
	{
	case ENwiroIKContentOperationState::Downloading:
	{
		if (Op.TotalBytes > 0)
		{
			return FMath::Clamp(static_cast<double>(Op.DownloadedBytes) / static_cast<double>(Op.TotalBytes), 0.0, 1.0) * 0.70;
		}
		return 0.05;
	}
	case ENwiroIKContentOperationState::Verifying:
		return 0.75;
	case ENwiroIKContentOperationState::Unzipping:
		return 0.85;
	case ENwiroIKContentOperationState::Importing:
		return 0.93;
	case ENwiroIKContentOperationState::Completed:
		return 1.0;
	case ENwiroIKContentOperationState::Failed:
	case ENwiroIKContentOperationState::Cancelled:
	case ENwiroIKContentOperationState::Idle:
	default:
		return 0.0;
	}
}

void FNwiroIKContentPipelineService::FImpl::EmitEvent(
	FOperation& Op,
	const FString& EventName,
	const FString& ErrorCode,
	const FString& ErrorMessage,
	const int64 DownloadedBytes,
	const int64 TotalBytes,
	const double Progress)
{
	FTelemetryEvent Event;
	Event.Name = EventName;
	Event.TimestampUtc = FDateTime::UtcNow();
	Event.DurationSeconds = (Event.TimestampUtc - Op.CreatedAtUtc).GetTotalSeconds();
	Event.ErrorCode = ErrorCode;
	Event.ErrorMessage = ErrorMessage;
	Event.DownloadedBytes = DownloadedBytes >= 0 ? DownloadedBytes : Op.DownloadedBytes;
	Event.TotalBytes = TotalBytes >= 0 ? TotalBytes : Op.TotalBytes;
	Event.Progress = Progress >= 0.0 ? Progress : GetOverallProgress(Op);
	Event.State = StateToString(Op.State);

	if (Op.Events.Num() >= 256)
	{
		Op.Events.RemoveAt(0);
	}
	Op.Events.Add(Event);
	Op.UpdatedAtUtc = Event.TimestampUtc;

	if (EventName == TEXT("download_fail")
		|| EventName == TEXT("unzip_fail")
		|| EventName == TEXT("import_fail"))
	{
		UE_LOG(LogNwiroContentPipeline, Error,
			TEXT("[%s] content=%s hash=%s code=%s msg=%s"),
			*EventName,
			*Op.Request.ContentId,
			*Op.NormalizedHash,
			*ErrorCode,
			*ErrorMessage);
	}
	else if (EventName == TEXT("download_progress"))
	{
		UE_LOG(LogNwiroContentPipeline, Verbose,
			TEXT("[%s] content=%s hash=%s downloaded=%lld total=%lld"),
			*EventName,
			*Op.Request.ContentId,
			*Op.NormalizedHash,
			Event.DownloadedBytes,
			Event.TotalBytes);
	}
	else
	{
		UE_LOG(LogNwiroContentPipeline, Log,
			TEXT("[%s] content=%s hash=%s state=%s"),
			*EventName,
			*Op.Request.ContentId,
			*Op.NormalizedHash,
			*Event.State);
	}

	DispatchEventToBrowser(Op, Event);
}

void FNwiroIKContentPipelineService::FImpl::DispatchEventToBrowser(const FOperation& Op, const FTelemetryEvent& Event)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("event"), Event.Name);
	Json->SetStringField(TEXT("content_id"), Op.Request.ContentId);
	Json->SetStringField(TEXT("hash"), Op.NormalizedHash);
	Json->SetStringField(TEXT("project_id"), Op.ProjectId);
	Json->SetStringField(TEXT("project_path"), Op.ProjectPath);
	Json->SetStringField(TEXT("timestamp"), Event.TimestampUtc.ToIso8601());
	Json->SetNumberField(TEXT("duration"), Event.DurationSeconds);
	Json->SetStringField(TEXT("error_code"), Event.ErrorCode);
	Json->SetStringField(TEXT("error_message"), Event.ErrorMessage);
	Json->SetNumberField(TEXT("downloaded_bytes"), Event.DownloadedBytes);
	Json->SetNumberField(TEXT("total_bytes"), Event.TotalBytes);
	Json->SetNumberField(TEXT("progress"), Event.Progress);
	Json->SetStringField(TEXT("state"), Event.State);
	const FString Payload = ToJsonString(Json);

	if (!IsInGameThread())
	{
		AsyncTask(ENamedThreads::GameThread, [Payload]()
		{
			if (FNwiroIKPanel::WebBrowserWidget.IsValid())
			{
				const FString Script = FString::Printf(
					TEXT("window.dispatchEvent(new CustomEvent('nwiro:content_event', { detail: %s }));"),
					*Payload);
				FNwiroIKPanel::WebBrowserWidget->ExecuteJavascript(Script);
			}
		});
		return;
	}

	if (FNwiroIKPanel::WebBrowserWidget.IsValid())
	{
		const FString Script = FString::Printf(
			TEXT("window.dispatchEvent(new CustomEvent('nwiro:content_event', { detail: %s }));"),
			*Payload);
		FNwiroIKPanel::WebBrowserWidget->ExecuteJavascript(Script);
	}
}

FString FNwiroIKContentPipelineService::FImpl::BuildStatusJson(const FOperation& Op) const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("content_id"), Op.Request.ContentId);
	Json->SetStringField(TEXT("hash"), Op.NormalizedHash);
	Json->SetStringField(TEXT("state"), StateToString(Op.State));
	Json->SetNumberField(TEXT("progress"), GetOverallProgress(Op));
	Json->SetNumberField(TEXT("downloaded_bytes"), Op.DownloadedBytes);
	Json->SetNumberField(TEXT("total_bytes"), Op.TotalBytes);
	Json->SetBoolField(TEXT("is_zip_downloaded"), Op.bZipDownloaded || IsZipDownloaded(Op.NormalizedHash));
	Json->SetBoolField(TEXT("is_unzip_complete"), Op.bUnzipComplete || IsUnzipComplete(Op.NormalizedHash));
	Json->SetBoolField(TEXT("is_imported_in_project"), Op.bImportedInProject || IsImportedInCurrentProject(Op.NormalizedHash));
	Json->SetBoolField(TEXT("is_content_cached"), IsContentCached(Op.NormalizedHash));
	Json->SetStringField(TEXT("error_code"), Op.ErrorCode);
	Json->SetStringField(TEXT("error_message"), Op.ErrorMessage);
	Json->SetStringField(TEXT("project_id"), Op.ProjectId);
	Json->SetStringField(TEXT("project_path"), Op.ProjectPath);
	Json->SetStringField(TEXT("zip_path"), Op.ZipPath);
	Json->SetStringField(TEXT("extracted_path"), Op.ExtractedPath);

	TArray<TSharedPtr<FJsonValue>> EventArray;
	for (const FTelemetryEvent& Event : Op.Events)
	{
		TSharedRef<FJsonObject> EventJson = MakeShared<FJsonObject>();
		EventJson->SetStringField(TEXT("event"), Event.Name);
		EventJson->SetStringField(TEXT("timestamp"), Event.TimestampUtc.ToIso8601());
		EventJson->SetNumberField(TEXT("duration"), Event.DurationSeconds);
		EventJson->SetStringField(TEXT("error_code"), Event.ErrorCode);
		EventJson->SetStringField(TEXT("error_message"), Event.ErrorMessage);
		EventJson->SetNumberField(TEXT("downloaded_bytes"), Event.DownloadedBytes);
		EventJson->SetNumberField(TEXT("total_bytes"), Event.TotalBytes);
		EventJson->SetNumberField(TEXT("progress"), Event.Progress);
		EventJson->SetStringField(TEXT("state"), Event.State);
		EventArray.Add(MakeShared<FJsonValueObject>(EventJson));
	}
	Json->SetArrayField(TEXT("events"), EventArray);

	return ToJsonString(Json);
}

FString FNwiroIKContentPipelineService::FImpl::BuildPassiveStatusJson(const FString& ContentId, const FString& NormalizedHash) const
{
	const bool bZipDownloaded = IsZipDownloaded(NormalizedHash);
	const bool bUnzipComplete = IsUnzipComplete(NormalizedHash);
	const bool bImported = IsImportedInCurrentProject(NormalizedHash);

	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("content_id"), ContentId.IsEmpty() ? NormalizedHash : ContentId);
	Json->SetStringField(TEXT("hash"), NormalizedHash);
	Json->SetStringField(TEXT("state"), TEXT("idle"));
	Json->SetNumberField(TEXT("progress"), 0.0);
	Json->SetNumberField(TEXT("downloaded_bytes"), bZipDownloaded ? FPlatformFileManager::Get().GetPlatformFile().FileSize(*FNwiroIKContentPaths::MakeZipPath(NormalizedHash)) : 0);
	Json->SetNumberField(TEXT("total_bytes"), -1);
	Json->SetBoolField(TEXT("is_zip_downloaded"), bZipDownloaded);
	Json->SetBoolField(TEXT("is_unzip_complete"), bUnzipComplete);
	Json->SetBoolField(TEXT("is_imported_in_project"), bImported);
	Json->SetBoolField(TEXT("is_content_cached"), bZipDownloaded && bUnzipComplete);
	Json->SetStringField(TEXT("error_code"), TEXT(""));
	Json->SetStringField(TEXT("error_message"), TEXT(""));
	Json->SetStringField(TEXT("project_id"), FNwiroIKContentPaths::SanitizePathToken(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())));
	Json->SetStringField(TEXT("project_path"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	Json->SetStringField(TEXT("zip_path"), FNwiroIKContentPaths::MakeZipPath(NormalizedHash));
	Json->SetStringField(TEXT("extracted_path"), FNwiroIKContentPaths::MakeExtractedPath(NormalizedHash));
	Json->SetArrayField(TEXT("events"), TArray<TSharedPtr<FJsonValue>>());
	return ToJsonString(Json);
}

void FNwiroIKContentPipelineService::FImpl::CollectTopLevelImportedFolders(const FString& Hash, TSet<FString>& OutFolders) const
{
	const FString NormalizedHash = FNwiroIKContentPaths::NormalizeHash(Hash);
	const FString ExtractedPath = FNwiroIKContentPaths::MakeExtractedPath(NormalizedHash);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*ExtractedPath))
	{
		return;
	}

	TArray<FString> TopLevelFiles;
	TArray<FString> TopLevelDirs;
	IFileManager::Get().FindFiles(TopLevelFiles, *FPaths::Combine(ExtractedPath, TEXT("*")), true, false);
	IFileManager::Get().FindFiles(TopLevelDirs, *FPaths::Combine(ExtractedPath, TEXT("*")), false, true);

	auto AddNonReserved = [&OutFolders](const FString& Name)
	{
		if (Name.IsEmpty()
			|| Name.Equals(TEXT("Developers"), ESearchCase::IgnoreCase)
			|| Name.Equals(TEXT("Collections"), ESearchCase::IgnoreCase))
		{
			return;
		}
		OutFolders.Add(Name);
	};

	// Case 1: extracted/.../Content exists, use folders under Content.
	if (TopLevelDirs.ContainsByPredicate([](const FString& Dir) { return Dir.Equals(TEXT("Content"), ESearchCase::IgnoreCase); }))
	{
		const FString ContentPath = FPaths::Combine(ExtractedPath, TEXT("Content"));
		TArray<FString> ContentSubDirs;
		IFileManager::Get().FindFiles(ContentSubDirs, *FPaths::Combine(ContentPath, TEXT("*")), false, true);
		for (const FString& Dir : ContentSubDirs)
		{
			AddNonReserved(Dir);
		}
		return;
	}

	// Case 2: hash wrapper folder containing a single package dir.
	if (TopLevelFiles.IsEmpty() && TopLevelDirs.Num() == 1)
	{
		AddNonReserved(TopLevelDirs[0]);
		return;
	}

	// Case 3: package dirs directly at extracted root.
	for (const FString& Dir : TopLevelDirs)
	{
		AddNonReserved(Dir);
	}
}

namespace
{
	TUniquePtr<FNwiroIKContentPipelineService> GContentPipelineService;
}

FNwiroIKContentPipelineService& FNwiroIKContentPipelineService::Get()
{
	if (!GContentPipelineService.IsValid())
	{
		GContentPipelineService.Reset(new FNwiroIKContentPipelineService());
	}
	return *GContentPipelineService.Get();
}

void FNwiroIKContentPipelineService::ShutdownSingleton()
{
	if (GContentPipelineService.IsValid())
	{
		GContentPipelineService->Impl->Shutdown();
	}
}

FNwiroIKContentPipelineService::FNwiroIKContentPipelineService()
	: Impl(MakeUnique<FImpl>())
{
}

FNwiroIKContentPipelineService::~FNwiroIKContentPipelineService() = default;

bool FNwiroIKContentPipelineService::StartDownloadAndImport(const FNwiroIKContentRequest& Request, FString& OutError)
{
	return Impl->StartDownloadAndImport(Request, OutError);
}

bool FNwiroIKContentPipelineService::StartImportFromCache(
	const FString& ContentId,
	const FString& Hash,
	const FString& RelativeImportPath,
	FString& OutError)
{
	return Impl->StartImportFromCache(ContentId, Hash, RelativeImportPath, OutError);
}

bool FNwiroIKContentPipelineService::CancelOperation(const FString& ContentId, const FString& Hash, FString& OutError)
{
	return Impl->CancelOperation(ContentId, Hash, OutError);
}

bool FNwiroIKContentPipelineService::DeleteCachedContent(const FString& Hash, const bool bDeleteImportedInProject, FString& OutError)
{
	return Impl->DeleteCachedContent(Hash, bDeleteImportedInProject, OutError);
}

FString FNwiroIKContentPipelineService::GetOperationStatusJson(const FString& ContentId, const FString& Hash)
{
	return Impl->GetOperationStatusJson(ContentId, Hash);
}

FString FNwiroIKContentPipelineService::GetContentStateJson(const FString& ContentId, const FString& Hash)
{
	return Impl->GetContentStateJson(ContentId, Hash);
}

FString FNwiroIKContentPipelineService::GetCachedContentsJson() const
{
	return Impl->GetCachedContentsJson();
}

bool FNwiroIKContentPipelineService::IsContentCached(const FString& Hash) const
{
	return Impl->IsContentCached(Hash);
}

bool FNwiroIKContentPipelineService::IsZipDownloaded(const FString& Hash) const
{
	return Impl->IsZipDownloaded(Hash);
}

bool FNwiroIKContentPipelineService::IsUnzipComplete(const FString& Hash) const
{
	return Impl->IsUnzipComplete(Hash);
}

bool FNwiroIKContentPipelineService::IsImportedInCurrentProject(const FString& Hash) const
{
	return Impl->IsImportedInCurrentProject(Hash);
}
