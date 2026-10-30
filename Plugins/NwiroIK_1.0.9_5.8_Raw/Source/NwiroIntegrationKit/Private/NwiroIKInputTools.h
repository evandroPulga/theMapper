// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Enhanced Input asset creation tools for Nwiro.
 * Creates InputAction and InputMappingContext assets.
 */
class FNwiroIKInputTools
{
public:
	// Create an InputAction asset (IA_Move, IA_Jump, etc.)
	static FString CreateInputAction(const FString& JsonCommand);

	// Create an InputMappingContext with key mappings
	static FString CreateInputMappingContext(const FString& JsonCommand);

	// List existing InputAction assets
	static FString FindInputActions(const FString& JsonCommand);

	// Delete an InputAction or InputMappingContext asset
	static FString DeleteInputAction(const FString& JsonCommand);

	// Edit an existing InputMappingContext (add/remove mappings)
	static FString EditMappingContext(const FString& JsonCommand);
};
