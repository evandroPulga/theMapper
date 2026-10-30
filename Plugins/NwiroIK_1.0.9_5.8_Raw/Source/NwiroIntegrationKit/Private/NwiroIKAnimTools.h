// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Animation tools: Montage creation/editing, AnimBP creation.
 */
class FNwiroIKAnimTools
{
public:
	// === Animation Montage ===
	static FString CreateMontage(const FString& JsonCommand);
	static FString ReadMontage(const FString& JsonCommand);
	static FString AddMontageSection(const FString& JsonCommand);
	static FString LinkMontageSections(const FString& JsonCommand);
	static FString AddMontageNotify(const FString& JsonCommand);

	// === Animation Blueprint ===
	static FString CreateAnimBlueprint(const FString& JsonCommand);
	static FString ReadAnimBlueprint(const FString& JsonCommand);
	static FString AddAnimBPStateMachine(const FString& JsonCommand);
};
