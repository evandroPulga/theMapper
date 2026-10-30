// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Level management, Macro shortcuts, Landscape, Foliage,
 * Networking, World Partition, and Undo/Redo tools.
 */
class FNwiroIKLevelTools
{
public:
	// === Level ===
	static FString NewLevel(const FString& JsonCommand);
	static FString OpenLevel(const FString& JsonCommand);
	static FString SaveLevel(const FString& JsonCommand);
	static FString GetLevelInfo(const FString& JsonCommand);

	// === Macro / Shortcuts ===
	static FString CreateBasicLevel(const FString& JsonCommand);
	static FString CreateLightRig(const FString& JsonCommand);
	static FString CreateGridLayout(const FString& JsonCommand);
	static FString CreateRingLayout(const FString& JsonCommand);

	// === Landscape ===
	static FString CreateLandscape(const FString& JsonCommand);
	static FString SetLandscapeMaterial(const FString& JsonCommand);
	static FString GetLandscapeInfo(const FString& JsonCommand);

	// === Foliage ===
	static FString AddFoliageType(const FString& JsonCommand);
	static FString PaintFoliage(const FString& JsonCommand);
	static FString EraseFoliage(const FString& JsonCommand);
	static FString GetFoliageStats(const FString& JsonCommand);

	// === Networking ===
	static FString GetReplicationInfo(const FString& JsonCommand);
	static FString SetReplicationSettings(const FString& JsonCommand);
	static FString SetNetDormancy(const FString& JsonCommand);

	// === World Partition ===
	static FString GetWorldPartitionInfo(const FString& JsonCommand);
	static FString LoadWorldPartitionRegion(const FString& JsonCommand);

	// === Undo/Redo ===
	static FString Undo(const FString& JsonCommand);
	static FString Redo(const FString& JsonCommand);
};
