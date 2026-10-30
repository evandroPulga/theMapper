// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Gameplay Ability System (GAS) and PCG tools.
 */
class FNwiroIKGASTools
{
public:
	// === GAS ===
	static FString CreateGameplayAbility(const FString& JsonCommand);
	static FString CreateGameplayEffect(const FString& JsonCommand);
	static FString CreateAttributeSet(const FString& JsonCommand);
	static FString ListGameplayAbilities(const FString& JsonCommand);
	static FString ListGameplayEffects(const FString& JsonCommand);
	static FString ListAttributeSets(const FString& JsonCommand);
	static FString GetGASInfo(const FString& JsonCommand);

	// === PCG ===
	static FString CreatePCGGraph(const FString& JsonCommand);
	static FString GetPCGInfo(const FString& JsonCommand);
	static FString ExecutePCG(const FString& JsonCommand);
};
