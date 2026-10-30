// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * AI tools: Behaviour Trees, Blackboard, EQS.
 */
class FNwiroIKAITools
{
public:
	static FString CreateBehaviorTree(const FString& JsonCommand);
	static FString ReadBehaviorTree(const FString& JsonCommand);
	static FString CreateBlackboard(const FString& JsonCommand);
	static FString EditBlackboard(const FString& JsonCommand);
	static FString AddBehaviorTreeNodes(const FString& JsonCommand);
};
