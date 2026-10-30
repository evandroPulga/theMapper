// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * State Tree tools: create, read, compile.
 */
class FNwiroIKStateTreeTools
{
public:
	static FString CreateStateTree(const FString& JsonCommand);
	static FString ReadStateTree(const FString& JsonCommand);
	static FString AddStateTreeState(const FString& JsonCommand);
};
