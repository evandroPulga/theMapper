// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Blueprint Debugger, Error Fixer, and Asset Dependency tools.
 */
class FNwiroIKDebugTools
{
public:
	// === Blueprint Debugger ===
	static FString BPGetCompileErrors(const FString& JsonCommand);
	static FString BPSetBreakpoint(const FString& JsonCommand);
	static FString BPRemoveBreakpoint(const FString& JsonCommand);
	static FString BPListBreakpoints(const FString& JsonCommand);
	static FString BPGetWatchValues(const FString& JsonCommand);
	static FString BPAddWatch(const FString& JsonCommand);

	// === Blueprint Error Fixer ===
	static FString BPFixBrokenReferences(const FString& JsonCommand);
	static FString BPFixDeprecatedNodes(const FString& JsonCommand);
	static FString BPRefreshAllNodes(const FString& JsonCommand);
	static FString BPFindUnconnectedPins(const FString& JsonCommand);

	// === Asset Dependency ===
	static FString GetAssetReferences(const FString& JsonCommand);
	static FString GetAssetReferencers(const FString& JsonCommand);
	static FString FindOrphanAssets(const FString& JsonCommand);
	static FString FindCircularDependencies(const FString& JsonCommand);
	static FString GetDependencyTree(const FString& JsonCommand);
};
