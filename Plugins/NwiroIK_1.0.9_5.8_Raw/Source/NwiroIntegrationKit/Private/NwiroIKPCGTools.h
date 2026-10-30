// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Basic PCG (Procedural Content Generation) MCP tools.
 *
 * These are the "default" PCG capabilities exposed to the LLM through the
 * normal MCP tool path — create graph, find graphs, drop a PCG Volume in
 * level, force-generate.
 *
 * The Extended PCG extension is a separate, fully-managed pipeline (see
 * api/src/pcg.ts + extentions/pcg/) that bypasses the LLM tool loop and
 * runs a single-call scene architect over the project's mesh library.
 */
class FNwiroIKPCGTools
{
public:
	static FString CreatePcgGraph(const FString& JsonCommand);
	static FString FindPcgGraphs(const FString& JsonCommand);
	static FString SpawnPcgVolume(const FString& JsonCommand);
	static FString PcgGenerate(const FString& JsonCommand);
	static FString AddPcgNode(const FString& JsonCommand);
};
