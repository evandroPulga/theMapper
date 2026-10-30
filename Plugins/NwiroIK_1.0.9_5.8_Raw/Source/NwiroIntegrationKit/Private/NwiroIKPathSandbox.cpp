// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKPathSandbox.h"
#include "Misc/Paths.h"

FString NwiroIKPathSandbox::TryResolveSandboxed(const FString& InputPath)
{
	if (InputPath.IsEmpty())
	{
		return FString();
	}

	// Always combine with ProjectDir — do NOT branch on IsRelative.
	// Absolute inputs that already live under ProjectDir survive the
	// IsUnderDirectory check below; absolute inputs outside ProjectDir
	// (e.g. /etc/passwd, C:\Windows\...) fold onto a path that resolves
	// outside ProjectDir and get rejected.
	const FString Combined = FPaths::Combine(FPaths::ProjectDir(), InputPath);
	const FString Resolved = FPaths::ConvertRelativePathToFull(Combined);
	const FString ProjectDirFull = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	if (!FPaths::IsUnderDirectory(Resolved, ProjectDirFull))
	{
		return FString();
	}

	return Resolved;
}
