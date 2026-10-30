// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

enum class ENwiroIKContentOperationState : uint8
{
	Idle,
	Downloading,
	Verifying,
	Unzipping,
	Importing,
	Completed,
	Failed,
	Cancelled
};

struct FNwiroIKContentRequest
{
	FString ContentId;
	FString Hash;
	FString Url;
	FString ExpectedSHA256;
	FString ExpectedMD5;
	FString RelativeImportPath;
	int64 ExpectedZipSizeBytes = 0;
	int64 EstimatedUnzippedSizeBytes = 0;
	int64 ChunkSizeBytes = 64LL * 1024LL * 1024LL;
	int64 DiskSafetyMarginBytes = 512LL * 1024LL * 1024LL;
	int32 MaxRetryCount = 4;
	double BaseRetryDelaySeconds = 1.0;
	bool bAutoImport = true;
	bool bStripRootFolderOnUnzip = false;
};
