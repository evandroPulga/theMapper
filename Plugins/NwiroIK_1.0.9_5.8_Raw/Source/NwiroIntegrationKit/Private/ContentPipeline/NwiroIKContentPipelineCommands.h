// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct IConsoleCommand;

class FNwiroIKContentPipelineCommands
{
public:
	static void Register();
	static void Unregister();

private:
	static IConsoleCommand* StartCommand;
	static IConsoleCommand* CancelCommand;
	static IConsoleCommand* StatusCommand;
	static IConsoleCommand* StateCommand;
	static IConsoleCommand* PathsCommand;
	static IConsoleCommand* ImportFromCacheCommand;
	static IConsoleCommand* DeleteCacheCommand;
	static IConsoleCommand* ListCacheCommand;

	static void HandleStart(const TArray<FString>& Args);
	static void HandleCancel(const TArray<FString>& Args);
	static void HandleStatus(const TArray<FString>& Args);
	static void HandleState(const TArray<FString>& Args);
	static void HandlePaths(const TArray<FString>& Args);
	static void HandleImportFromCache(const TArray<FString>& Args);
	static void HandleDeleteCache(const TArray<FString>& Args);
	static void HandleListCache(const TArray<FString>& Args);
};
