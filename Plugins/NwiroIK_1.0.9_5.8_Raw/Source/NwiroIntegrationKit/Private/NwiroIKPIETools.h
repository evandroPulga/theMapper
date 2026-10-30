// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * PIE Runtime Control tools - manipulate the game while it's running.
 * All tools operate on GEditor->PlayWorld (PIE world).
 */
class FNwiroIKPIETools
{
public:
	// Teleport actor to position during PIE
	static FString PIETeleportActor(const FString& JsonCommand);

	// Spawn actor in PIE world
	static FString PIESpawnActor(const FString& JsonCommand);

	// Destroy actor in PIE world
	static FString PIEDestroyActor(const FString& JsonCommand);

	// Get/Set property on PIE actor via reflection
	static FString PIEGetProperty(const FString& JsonCommand);
	static FString PIESetProperty(const FString& JsonCommand);

	// Set Blackboard key value on AI controller
	static FString PIESetBlackboardKey(const FString& JsonCommand);
	static FString PIEGetBlackboardKey(const FString& JsonCommand);

	// AI Controller commands
	static FString PIEMoveAITo(const FString& JsonCommand);
	static FString PIEStopAI(const FString& JsonCommand);

	// Get PIE game state (list actors, positions, etc.)
	static FString PIEGetGameState(const FString& JsonCommand);

	// List all actors in PIE world
	static FString PIEListActors(const FString& JsonCommand);

	// Execute console command in PIE
	static FString PIEConsoleCommand(const FString& JsonCommand);
};
