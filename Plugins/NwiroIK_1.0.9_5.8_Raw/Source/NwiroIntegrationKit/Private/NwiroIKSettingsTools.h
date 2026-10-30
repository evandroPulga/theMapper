// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * World settings, project settings, and game mode manipulation for Nwiro.
 * All methods are static, return JSON strings.
 */
class FNwiroIKSettingsTools
{
public:
	// Get current world settings (gravity, game mode, kill Z, etc.)
	static FString GetWorldSettings();

	// Set world settings from JSON
	static FString SetWorldSettings(const FString& JsonCommand);

	// Set game mode + default pawn + controller on world settings
	static FString SetGameMode(const FString& JsonCommand);

	// Get project settings (default maps, global game mode)
	static FString GetProjectSettings(const FString& JsonCommand);

	// Set project settings
	static FString SetProjectSettings(const FString& JsonCommand);

	// List all actors in current level
	static FString GetLevelActors(const FString& JsonCommand);

	// Spawn an actor in the level
	static FString SpawnActor(const FString& JsonCommand);

	// Delete an actor from the level
	static FString DeleteActor(const FString& JsonCommand);

	// Move/rotate/scale an actor
	static FString TransformActor(const FString& JsonCommand);

	// Get/set actor properties
	static FString GetActorProperty(const FString& JsonCommand);
	static FString SetActorProperty(const FString& JsonCommand);

	// Actor manipulation
	static FString DuplicateActor(const FString& JsonCommand);
	static FString RenameActor(const FString& JsonCommand);
	static FString AttachActor(const FString& JsonCommand);
	static FString DetachActor(const FString& JsonCommand);
	static FString SelectActor(const FString& JsonCommand);

	// Execute arbitrary Python code in the editor (via PythonScriptPlugin)
	static FString ExecutePython(const FString& JsonCommand);
};
