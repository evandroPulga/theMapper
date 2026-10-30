// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * MCP Resource provider - read-only context data for AI agents.
 * Returns JSON for resources/read requests.
 */
class FNwiroIKResourceProvider
{
public:
	// Get list of all available resources as JSON
	static FString GetResourceList();

	// Read a specific resource by URI
	static FString ReadResource(const FString& Uri);

private:
	static FString GetProjectInfo();
	static FString GetCurrentLevel();
	static FString GetEditorSelection();
	static FString GetPerformanceStats();
	static FString GetEditorLog();
	static FString GetViewportInfo();
};
