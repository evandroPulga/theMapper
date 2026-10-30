// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Generic asset reading, searching, and thumbnail tools.
 * Works with any UObject-derived asset via reflection.
 */
class FNwiroIKAssetTools
{
public:
	// Read any asset's properties via reflection
	static FString ReadAsset(const FString& JsonCommand);

	// Search assets by name/class across the project
	static FString FindAssets(const FString& JsonCommand);

	// Render an asset's editor thumbnail to file
	static FString GetAssetThumbnail(const FString& JsonCommand);
};
