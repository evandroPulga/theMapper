// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include <miniz.h>

#include "CoreMinimal.h"

class FNwiroIKZipExtractor
{
public:
	static bool ExtractZip(
		const FString& ZipFilePath,
		const FString& OutputDirectory,
		bool bStripRootFolder,
		FString& OutError);
};
