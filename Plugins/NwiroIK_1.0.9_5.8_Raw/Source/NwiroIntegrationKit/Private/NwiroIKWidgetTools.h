// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Widget Blueprint (UMG) tools: create widget BPs, add widgets, set properties.
 */
class FNwiroIKWidgetTools
{
public:
	static FString CreateWidgetBlueprint(const FString& JsonCommand);
	static FString ReadWidgetBlueprint(const FString& JsonCommand);
	static FString AddWidget(const FString& JsonCommand);
	static FString SetWidgetProperty(const FString& JsonCommand);
	static FString RenderWidgetBlueprint(const FString& JsonCommand);
};
