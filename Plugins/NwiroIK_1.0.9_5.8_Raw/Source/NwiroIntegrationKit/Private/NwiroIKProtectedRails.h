// Copyright 2026 Nwiro. All Rights Reserved.
//
// Managed-tier permission rails for the NwiroIntegrationKit plugin.
//
// These values MUST MATCH `shared/permissions/protected.ts` in the
// nwiro-agents repo. Drift is caught at code review — this file and the
// TS source are the two canonical definitions.
//
// Enforcement: rails run at the top of FNwiroIKMCPServer::HandleMCPPost()
// before any mode or session-allow check. They cannot be bypassed by
// bypassPermissions mode — that's the point.
//
// Compile-time flag: NWIRO_PROTECTED_RAILS_ENFORCED defaults to enabled in
// every non-debug build. Local dev opts out via NWIRO_PROTECTED_RAILS_DISABLED=1
// in .Target.cs. Marketplace/release safety never depends on a define being set.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// ─── Compile-time flag ────────────────────────────────────────────────
// Keep this block byte-identical between NwiroIKProtectedRails.h and
// NwiroProtectedRails.h. Drift caught at review.
#if !defined(NWIRO_PROTECTED_RAILS_DISABLED) || NWIRO_PROTECTED_RAILS_DISABLED == 0
	#define NWIRO_PROTECTED_RAILS_ENFORCED 1
#else
	#define NWIRO_PROTECTED_RAILS_ENFORCED 0
#endif

namespace NwiroIKProtectedRails
{
	/** Tools that are never allowed to execute regardless of mode / session allow. */
	inline const TArray<FString> DeniedTools = {
		// execute_python intentionally NOT denied — users opted to allow it for
		// Codex/Claude Code automation flows. Protection leans on the loopback-only
		// MCP server binding + per-client approval UX. File-path rails below still fire.
	};

	/** File-path prefix blocks. Applied when a tool call has a `file_path` argument. */
	inline const TArray<FString> DeniedFilePathPrefixes = {
		TEXT("/Engine/"),
		TEXT("/Plugins/"),
	};

	/** Substrings that suggest path-traversal attempts. */
	inline const TArray<FString> DeniedFilePathSubstrings = {
		TEXT("../"),
		TEXT("..\\"),
	};

	/**
	 * Evaluate rails against a tool call. Returns an empty FString if the call
	 * is allowed to proceed; returns a non-empty human-readable reason if the
	 * call must be blocked.
	 *
	 * Call sites must respect the return value even when the caller is in
	 * bypassPermissions mode — rails outrank mode.
	 */
	FString Check(const FString& ToolName, const TSharedPtr<FJsonObject>& Args);
}
