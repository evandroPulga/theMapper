// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Navigation, Audio, Game Framework, and Build/Validation tools.
 */
class FNwiroIKGameplayTools
{
public:
	// === Navigation ===
	static FString BuildNavigation(const FString& JsonCommand);
	static FString QueryNavigationPath(const FString& JsonCommand);
	static FString GetNavigationInfo(const FString& JsonCommand);

	// === Audio ===
	static FString SpawnSound(const FString& JsonCommand);
	static FString SetAudioProperties(const FString& JsonCommand);
	static FString GetSoundInfo(const FString& JsonCommand);

	// === Game Framework ===
	static FString CreateGameMode(const FString& JsonCommand);
	static FString CreatePlayerController(const FString& JsonCommand);
	static FString CreateGameState(const FString& JsonCommand);
	static FString CreatePlayerState(const FString& JsonCommand);
	static FString CreateHUD(const FString& JsonCommand);
	static FString GetGameFrameworkInfo(const FString& JsonCommand);

	// === Build / Validation ===
	static FString GetProjectInfo(const FString& JsonCommand);
	static FString ListProjectModules(const FString& JsonCommand);
	static FString ValidateAssets(const FString& JsonCommand);
	static FString GetMapCheckErrors(const FString& JsonCommand);
	static FString GetBuildConfiguration(const FString& JsonCommand);
};
