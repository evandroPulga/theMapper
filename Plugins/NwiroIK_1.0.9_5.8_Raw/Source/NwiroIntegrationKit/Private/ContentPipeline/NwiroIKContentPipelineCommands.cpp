// Copyright 2026 Nwiro. All Rights Reserved.
#include "NwiroIKContentPipelineCommands.h"

#include "NwiroIKContentPipelineCommands.h"
#include "ContentPipeline/NwiroIKContentPaths.h"
#include "ContentPipeline/NwiroIKContentPipelineService.h"
#include "ContentPipeline/NwiroIKContentPipelineTypes.h"
#include "HAL/IConsoleManager.h"
#include "Logging/LogMacros.h"
#include "Misc/DefaultValueHelper.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroContentCommands, Log, All);

IConsoleCommand* FNwiroIKContentPipelineCommands::StartCommand = nullptr;
IConsoleCommand* FNwiroIKContentPipelineCommands::CancelCommand = nullptr;
IConsoleCommand* FNwiroIKContentPipelineCommands::StatusCommand = nullptr;
IConsoleCommand* FNwiroIKContentPipelineCommands::StateCommand = nullptr;
IConsoleCommand* FNwiroIKContentPipelineCommands::PathsCommand = nullptr;
IConsoleCommand* FNwiroIKContentPipelineCommands::ImportFromCacheCommand = nullptr;
IConsoleCommand* FNwiroIKContentPipelineCommands::DeleteCacheCommand = nullptr;
IConsoleCommand* FNwiroIKContentPipelineCommands::ListCacheCommand = nullptr;

namespace
{
	bool TryGetOptionValue(const TArray<FString>& Args, const FString& Key, FString& OutValue)
	{
		const FString Prefix = Key + TEXT("=");
		for (const FString& Arg : Args)
		{
			if (Arg.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				OutValue = Arg.Mid(Prefix.Len());
				return true;
			}
		}
		return false;
	}

	bool TryParseBoolOption(const TArray<FString>& Args, const FString& Key, bool& OutBool)
	{
		FString Value;
		if (!TryGetOptionValue(Args, Key, Value))
		{
			return false;
		}

		if (Value.Equals(TEXT("1")) || Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("yes"), ESearchCase::IgnoreCase))
		{
			OutBool = true;
			return true;
		}
		if (Value.Equals(TEXT("0")) || Value.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("no"), ESearchCase::IgnoreCase))
		{
			OutBool = false;
			return true;
		}
		return false;
	}
}

void FNwiroIKContentPipelineCommands::Register()
{
	IConsoleManager& ConsoleManager = IConsoleManager::Get();

	StartCommand = ConsoleManager.RegisterConsoleCommand(
		TEXT("Nwiro.Content.Start"),
		TEXT("Start download+unzip+import.\nUsage: Nwiro.Content.Start <ContentId> <Hash> <Url> [ImportPath] [sha256=<hex>] [md5=<hex>] [zip=<bytes>] [unzip=<bytes>] [retry=<n>] [chunkmb=<n>] [autoimport=0|1] [striproot=0|1]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FNwiroIKContentPipelineCommands::HandleStart),
		ECVF_Default);

	CancelCommand = ConsoleManager.RegisterConsoleCommand(
		TEXT("Nwiro.Content.Cancel"),
		TEXT("Cancel an active operation.\nUsage: Nwiro.Content.Cancel <ContentId> <Hash>"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FNwiroIKContentPipelineCommands::HandleCancel),
		ECVF_Default);

	StatusCommand = ConsoleManager.RegisterConsoleCommand(
		TEXT("Nwiro.Content.Status"),
		TEXT("Print detailed JSON status.\nUsage: Nwiro.Content.Status <ContentId> <Hash>"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FNwiroIKContentPipelineCommands::HandleStatus),
		ECVF_Default);

	StateCommand = ConsoleManager.RegisterConsoleCommand(
		TEXT("Nwiro.Content.State"),
		TEXT("Print compact state JSON.\nUsage: Nwiro.Content.State <ContentId> <Hash>"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FNwiroIKContentPipelineCommands::HandleState),
		ECVF_Default);

	PathsCommand = ConsoleManager.RegisterConsoleCommand(
		TEXT("Nwiro.Content.Paths"),
		TEXT("Print cache/depot directories."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FNwiroIKContentPipelineCommands::HandlePaths),
		ECVF_Default);

	ImportFromCacheCommand = ConsoleManager.RegisterConsoleCommand(
		TEXT("Nwiro.Content.ImportFromCache"),
		TEXT("Import from already cached artifact.\nUsage: Nwiro.Content.ImportFromCache <Hash> [ImportPath|import=Path] [content=<ContentId>]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FNwiroIKContentPipelineCommands::HandleImportFromCache),
		ECVF_Default);

	DeleteCacheCommand = ConsoleManager.RegisterConsoleCommand(
		TEXT("Nwiro.Content.DeleteCache"),
		TEXT("Delete cached zip/extracted data for hash.\nUsage: Nwiro.Content.DeleteCache <Hash> [deleteimported=0|1]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FNwiroIKContentPipelineCommands::HandleDeleteCache),
		ECVF_Default);

	ListCacheCommand = ConsoleManager.RegisterConsoleCommand(
		TEXT("Nwiro.Content.ListCache"),
		TEXT("Print cached content list JSON."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FNwiroIKContentPipelineCommands::HandleListCache),
		ECVF_Default);
}

void FNwiroIKContentPipelineCommands::Unregister()
{
	IConsoleManager& ConsoleManager = IConsoleManager::Get();

	if (StartCommand)
	{
		ConsoleManager.UnregisterConsoleObject(StartCommand);
		StartCommand = nullptr;
	}
	if (CancelCommand)
	{
		ConsoleManager.UnregisterConsoleObject(CancelCommand);
		CancelCommand = nullptr;
	}
	if (StatusCommand)
	{
		ConsoleManager.UnregisterConsoleObject(StatusCommand);
		StatusCommand = nullptr;
	}
	if (StateCommand)
	{
		ConsoleManager.UnregisterConsoleObject(StateCommand);
		StateCommand = nullptr;
	}
	if (PathsCommand)
	{
		ConsoleManager.UnregisterConsoleObject(PathsCommand);
		PathsCommand = nullptr;
	}
	if (ImportFromCacheCommand)
	{
		ConsoleManager.UnregisterConsoleObject(ImportFromCacheCommand);
		ImportFromCacheCommand = nullptr;
	}
	if (DeleteCacheCommand)
	{
		ConsoleManager.UnregisterConsoleObject(DeleteCacheCommand);
		DeleteCacheCommand = nullptr;
	}
	if (ListCacheCommand)
	{
		ConsoleManager.UnregisterConsoleObject(ListCacheCommand);
		ListCacheCommand = nullptr;
	}
}

void FNwiroIKContentPipelineCommands::HandleStart(const TArray<FString>& Args)
{
	if (Args.Num() < 2)
	{
		UE_LOG(LogNwiroContentCommands, Warning, TEXT("Usage: Nwiro.Content.Start <ContentId> <Hash> <Url|url=host:port/file.zip> [ImportPath|import=Path] [sha256=<hex>] [md5=<hex>] [zip=<bytes>] [unzip=<bytes>] [retry=<n>] [chunkmb=<n>] [autoimport=0|1] [striproot=0|1]"));
		return;
	}

	FNwiroIKContentRequest Request;
	Request.ContentId = Args[0];
	Request.Hash = Args[1];

	// URL may be passed positionally or as key=value to avoid console parsing issues with "http://".
	if (Args.Num() >= 3 && !Args[2].Contains(TEXT("=")))
	{
		Request.Url = Args[2];
	}
	else
	{
		FString UrlOption;
		if (TryGetOptionValue(Args, TEXT("url"), UrlOption))
		{
			Request.Url = UrlOption;
		}
	}

	// Optional positional import path (if present and not key=value)
	if (Args.Num() >= 4 && !Args[3].Contains(TEXT("=")))
	{
		Request.RelativeImportPath = Args[3];
	}
	else
	{
		FString ImportOption;
		if (TryGetOptionValue(Args, TEXT("import"), ImportOption))
		{
			Request.RelativeImportPath = ImportOption;
		}
	}

	FString Value;
	if (TryGetOptionValue(Args, TEXT("sha256"), Value))
	{
		Request.ExpectedSHA256 = Value;
	}
	if (TryGetOptionValue(Args, TEXT("md5"), Value))
	{
		Request.ExpectedMD5 = Value;
	}
	if (TryGetOptionValue(Args, TEXT("zip"), Value))
	{
		int64 Parsed = 0;
		if (FDefaultValueHelper::ParseInt64(Value, Parsed))
		{
			Request.ExpectedZipSizeBytes = Parsed;
		}
	}
	if (TryGetOptionValue(Args, TEXT("unzip"), Value))
	{
		int64 Parsed = 0;
		if (FDefaultValueHelper::ParseInt64(Value, Parsed))
		{
			Request.EstimatedUnzippedSizeBytes = Parsed;
		}
	}
	if (TryGetOptionValue(Args, TEXT("retry"), Value))
	{
		int32 Parsed = 0;
		if (FDefaultValueHelper::ParseInt(Value, Parsed))
		{
			Request.MaxRetryCount = Parsed;
		}
	}
	if (TryGetOptionValue(Args, TEXT("chunkmb"), Value))
	{
		int64 Parsed = 0;
		if (FDefaultValueHelper::ParseInt64(Value, Parsed) && Parsed > 0)
		{
			Request.ChunkSizeBytes = Parsed * 1024LL * 1024LL;
		}
	}

	// Accept url=127.0.0.1:8000/file.zip and prepend scheme automatically.
	if (!Request.Url.IsEmpty() && !Request.Url.StartsWith(TEXT("http://")) && !Request.Url.StartsWith(TEXT("https://")))
	{
		Request.Url = TEXT("http://") + Request.Url;
	}

	if (Request.Url.IsEmpty())
	{
		UE_LOG(LogNwiroContentCommands, Error, TEXT("Start failed: URL is missing. Use positional URL or url=host:port/file.zip"));
		return;
	}

	bool ParsedBool = false;
	if (TryParseBoolOption(Args, TEXT("autoimport"), ParsedBool))
	{
		Request.bAutoImport = ParsedBool;
	}
	if (TryParseBoolOption(Args, TEXT("striproot"), ParsedBool))
	{
		Request.bStripRootFolderOnUnzip = ParsedBool;
	}

	FString Error;
	if (!FNwiroIKContentPipelineService::Get().StartDownloadAndImport(Request, Error))
	{
		UE_LOG(LogNwiroContentCommands, Error, TEXT("Start failed: %s"), *Error);
		return;
	}

	UE_LOG(
		LogNwiroContentCommands,
		Log,
		TEXT("Content pipeline started. content_id=%s hash=%s import=%s"),
		*Request.ContentId,
		*Request.Hash,
		Request.RelativeImportPath.IsEmpty() ? TEXT("<default>") : *Request.RelativeImportPath);
}

void FNwiroIKContentPipelineCommands::HandleCancel(const TArray<FString>& Args)
{
	if (Args.Num() < 2)
	{
		UE_LOG(LogNwiroContentCommands, Warning, TEXT("Usage: Nwiro.Content.Cancel <ContentId> <Hash>"));
		return;
	}

	FString Error;
	if (!FNwiroIKContentPipelineService::Get().CancelOperation(Args[0], Args[1], Error))
	{
		UE_LOG(LogNwiroContentCommands, Error, TEXT("Cancel failed: %s"), *Error);
		return;
	}

	UE_LOG(LogNwiroContentCommands, Log, TEXT("Cancel requested. content_id=%s hash=%s"), *Args[0], *Args[1]);
}

void FNwiroIKContentPipelineCommands::HandleStatus(const TArray<FString>& Args)
{
	if (Args.Num() < 2)
	{
		UE_LOG(LogNwiroContentCommands, Warning, TEXT("Usage: Nwiro.Content.Status <ContentId> <Hash>"));
		return;
	}

	const FString Json = FNwiroIKContentPipelineService::Get().GetOperationStatusJson(Args[0], Args[1]);
	UE_LOG(LogNwiroContentCommands, Log, TEXT("%s"), *Json);
}

void FNwiroIKContentPipelineCommands::HandleState(const TArray<FString>& Args)
{
	if (Args.Num() < 2)
	{
		UE_LOG(LogNwiroContentCommands, Warning, TEXT("Usage: Nwiro.Content.State <ContentId> <Hash>"));
		return;
	}

	const FString Json = FNwiroIKContentPipelineService::Get().GetContentStateJson(Args[0], Args[1]);
	UE_LOG(LogNwiroContentCommands, Log, TEXT("%s"), *Json);
}

void FNwiroIKContentPipelineCommands::HandlePaths(const TArray<FString>& Args)
{
	(void)Args;

	UE_LOG(LogNwiroContentCommands, Log, TEXT("DepotRoot: %s"), *FNwiroIKContentPaths::GetDepotRoot());
	UE_LOG(LogNwiroContentCommands, Log, TEXT("CompressedRoot: %s"), *FNwiroIKContentPaths::GetCompressedRoot());
	UE_LOG(LogNwiroContentCommands, Log, TEXT("ExtractedRoot: %s"), *FNwiroIKContentPaths::GetExtractedRoot());
	UE_LOG(LogNwiroContentCommands, Log, TEXT("StateRoot: %s"), *FNwiroIKContentPaths::GetStateRoot());
	UE_LOG(LogNwiroContentCommands, Log, TEXT("ImportsRoot: %s"), *FNwiroIKContentPaths::GetImportsRoot());
}

void FNwiroIKContentPipelineCommands::HandleImportFromCache(const TArray<FString>& Args)
{
	FString Hash;
	FString ContentId;
	FString RelativeImportPath;

	TryGetOptionValue(Args, TEXT("hash"), Hash);
	TryGetOptionValue(Args, TEXT("content"), ContentId);
	TryGetOptionValue(Args, TEXT("import"), RelativeImportPath);

	// Backward-compatible positional parse:
	// 1) <Hash> [ImportPath]
	// 2) <ContentId> <Hash> [ImportPath]
	if (Hash.IsEmpty())
	{
		if (Args.Num() >= 2 && !Args[0].Contains(TEXT("=")) && !Args[1].Contains(TEXT("=")))
		{
			ContentId = Args[0];
			Hash = Args[1];
			if (RelativeImportPath.IsEmpty() && Args.Num() >= 3 && !Args[2].Contains(TEXT("=")))
			{
				RelativeImportPath = Args[2];
			}
		}
		else if (Args.Num() >= 1 && !Args[0].Contains(TEXT("=")))
		{
			Hash = Args[0];
			if (RelativeImportPath.IsEmpty() && Args.Num() >= 2 && !Args[1].Contains(TEXT("=")))
			{
				RelativeImportPath = Args[1];
			}
		}
	}

	if (Hash.IsEmpty())
	{
		UE_LOG(LogNwiroContentCommands, Warning, TEXT("Usage: Nwiro.Content.ImportFromCache <Hash> [ImportPath|import=Path] [content=<ContentId>]"));
		return;
	}

	FString Error;
	if (!FNwiroIKContentPipelineService::Get().StartImportFromCache(ContentId, Hash, RelativeImportPath, Error))
	{
		UE_LOG(LogNwiroContentCommands, Error, TEXT("ImportFromCache failed: %s"), *Error);
		return;
	}

	UE_LOG(LogNwiroContentCommands, Log, TEXT("ImportFromCache started. content_id=%s hash=%s import=%s"),
		ContentId.IsEmpty() ? TEXT("<auto>") : *ContentId,
		*Hash,
		RelativeImportPath.IsEmpty() ? TEXT("<default>") : *RelativeImportPath);
}

void FNwiroIKContentPipelineCommands::HandleDeleteCache(const TArray<FString>& Args)
{
	FString Hash;
	TryGetOptionValue(Args, TEXT("hash"), Hash);

	if (Hash.IsEmpty() && Args.Num() >= 1 && !Args[0].Contains(TEXT("=")))
	{
		Hash = Args[0];
	}

	if (Hash.IsEmpty())
	{
		UE_LOG(LogNwiroContentCommands, Warning, TEXT("Usage: Nwiro.Content.DeleteCache <Hash> [deleteimported=0|1]"));
		return;
	}

	bool bDeleteImportedInProject = false;
	if (!TryParseBoolOption(Args, TEXT("deleteimported"), bDeleteImportedInProject)
		&& Args.Num() >= 2
		&& !Args[1].Contains(TEXT("=")))
	{
		bool ParsedValue = false;
		if (Args[1].Equals(TEXT("1"))
			|| Args[1].Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| Args[1].Equals(TEXT("yes"), ESearchCase::IgnoreCase))
		{
			ParsedValue = true;
			bDeleteImportedInProject = ParsedValue;
		}
		else if (Args[1].Equals(TEXT("0"))
			|| Args[1].Equals(TEXT("false"), ESearchCase::IgnoreCase)
			|| Args[1].Equals(TEXT("no"), ESearchCase::IgnoreCase))
		{
			ParsedValue = true;
			bDeleteImportedInProject = false;
		}

		if (!ParsedValue)
		{
			UE_LOG(LogNwiroContentCommands, Warning, TEXT("Invalid deleteimported value. Use 0/1 or true/false."));
			return;
		}
	}

	FString Error;
	if (!FNwiroIKContentPipelineService::Get().DeleteCachedContent(Hash, bDeleteImportedInProject, Error))
	{
		UE_LOG(LogNwiroContentCommands, Error, TEXT("DeleteCache failed: %s"), *Error);
		return;
	}

	if (!Error.IsEmpty())
	{
		UE_LOG(LogNwiroContentCommands, Warning, TEXT("%s"), *Error);
	}

	UE_LOG(LogNwiroContentCommands, Log, TEXT("DeleteCache completed. hash=%s deleteimported=%s"),
		*Hash,
		bDeleteImportedInProject ? TEXT("true") : TEXT("false"));
}

void FNwiroIKContentPipelineCommands::HandleListCache(const TArray<FString>& Args)
{
	(void)Args;
	const FString Json = FNwiroIKContentPipelineService::Get().GetCachedContentsJson();
	UE_LOG(LogNwiroContentCommands, Log, TEXT("%s"), *Json);
}
