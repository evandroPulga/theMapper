// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Host-side hub that aggregates every EXTERNAL tool source into the MCP tool
 * layer. Mirrored in Nwiro Pro (FNwiroToolRegistry).
 *
 * Sources, each landing in its own assigned namespace:
 *   - plugin.<id>.<tool>   INwiroToolProvider modular features        (Phase 1)
 *   - plugin.<id>.<tool>   UFUNCTION(meta=(NwiroTool=...)) reflection  (Phase 2)
 *   - ue.<toolset>.<tool>  Epic 5.8 native UToolsetDefinition import   (Phase 3)
 *
 * Two integration points used by the MCP server:
 *   - AppendEnabledProviderDefs / AppendAllProviderDefsWithState: append the
 *     external, namespaced defs to the tools JSON array (reserved-namespace
 *     guards, dedup). The Enabled variant applies the user-disabled-namespace
 *     filter and is the ONLY one that may feed a model/agent; the WithState
 *     variant serializes everything (each def carrying `"disabled":true|false`)
 *     and exists solely for the settings UI, so disabling a source never hides
 *     its own re-enable toggle.
 *   - TryDispatch: route a namespaced call to the right source, on the game
 *     thread, returning a JSON result.
 *
 * The advertised set is FROZEN at session start (see EnsureFrozen) so the cached
 * tool-name prefix stays byte-stable across turns.
 */
class FNwiroIKToolRegistry
{
public:
	/** Append the external, namespaced tool defs the user has ENABLED to `J`
	 *  (the in-progress tools JSON array body). Adds a separating comma only
	 *  when needed and never leaves a trailing comma. No-op when nothing
	 *  external is registered. Every model/agent-facing consumer uses this. */
	static void AppendEnabledProviderDefs(FString& J);

	/** Append ALL external tool defs to `J`, including user-disabled ones, each
	 *  def carrying a `"disabled":true|false` field. For the settings UI only —
	 *  never feed this list to a model or agent. */
	static void AppendAllProviderDefsWithState(FString& J);

	/** If `ToolName` is namespaced (plugin./ue./mcp.), run it and write the JSON
	 *  result into OutResult, returning true. Returns false otherwise so the
	 *  caller continues to its own fallback. Rejects user-disabled namespaces. */
	static bool TryDispatch(const FString& ToolName, const FString& ArgsJson, FString& OutResult);

	// ─── End-user tool-source controls (settings) ───────────────────────────
	/** Namespaces (e.g. "plugin.acme", "ue") the user has switched off. Disabled
	 *  namespaces never appear in tools/list and are rejected in dispatch. */
	static void SetDisabledNamespaces(const TArray<FString>& Namespaces);
	static TArray<FString> GetDisabledNamespaces();

	/** Re-resolve all sources (call when providers (un)register or the user
	 *  toggles a source). The next session start re-freezes. */
	static void Invalidate();
};
