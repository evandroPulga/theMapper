// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FNwiroIKImportResult
{
	bool bSuccess = false;
	int32 CopiedFiles = 0;
	int32 SkippedFiles = 0;
	FString Error;
	TArray<FString> AbsolutePathsToScan;
	TArray<FString> GameFoldersToSync;
};

class FNwiroIKContentImporter
{
public:
	static FNwiroIKImportResult ImportToCurrentProject(
		const FString& ExtractedDirectory,
		const FString& RelativeImportPath);

private:
	enum class ESourceLayout : uint8
	{
		ContentFolder,
		PackageRoot
	};

	static bool FindSourceContentRoot(
		const FString& ExtractedDirectory,
		FString& OutContentRoot,
		ESourceLayout& OutLayout,
		FString& OutError);
};
