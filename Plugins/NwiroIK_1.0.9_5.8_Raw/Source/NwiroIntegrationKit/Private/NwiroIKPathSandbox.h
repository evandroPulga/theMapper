// Copyright 2026 Nwiro. All Rights Reserved.
//
// File-tool path sandbox for the NwiroIntegrationKit plugin.
//
// Closes the absolute-path bypass present in the original file-tool
// resolution code (write_file, read_file, delete_file, rename_file).
//
// The bypass: the previous code branched on `FPaths::IsRelative(InputPath)`.
// For inputs like "/etc/passwd" or "C:\\Windows\\..." this returns false, so
// ProjectDir was NOT prepended and the absolute path was used as-is.
// Result: tool calls could touch any file on disk an LLM could name.
//
// Fix: unconditionally combine with ProjectDir, then verify the resolved
// path lives under ProjectDir via FPaths::IsUnderDirectory. Absolute paths
// inside the project still work; absolute paths outside fail.
//
// Audit confirmation (run before IK-PR-3 merge): all file-tool handlers in
// both plugins consume the resolved path via FFileHelper / IFileManager
// (pure filesystem). No tool routes file_path through the content registry
// or asset loader. Safe to apply the sandbox uniformly.

#pragma once

#include "CoreMinimal.h"

namespace NwiroIKPathSandbox
{
	/**
	 * Resolve InputPath to an absolute filesystem path confined to ProjectDir.
	 * Returns the safe absolute path on success, or an empty FString if the
	 * resolved path escapes ProjectDir.
	 *
	 * Callers must treat empty return as "deny this operation".
	 */
	FString TryResolveSandboxed(const FString& InputPath);
}
