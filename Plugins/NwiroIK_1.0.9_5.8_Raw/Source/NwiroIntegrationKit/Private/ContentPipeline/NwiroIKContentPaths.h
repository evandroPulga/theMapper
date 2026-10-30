// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FNwiroIKContentPaths
{
public:
	static FString GetHomeDirectory();
	static FString GetDepotRoot();
	static FString GetCompressedRoot();
	static FString GetExtractedRoot();
	static FString GetImportsRoot();
	static FString GetStateRoot();

	static FString MakeZipPath(const FString& Hash);
	static FString MakePartialZipPath(const FString& Hash);
	static FString MakeExtractedPath(const FString& Hash);
	static FString MakeUnzipMarkerPath(const FString& Hash);
	static FString MakeImportMarkerPathForProject(const FString& Hash);

	static FString NormalizeHash(const FString& Hash);
	static FString SanitizePathToken(const FString& InValue);
	static bool EnsureBaseDirectories(FString& OutError);
};
