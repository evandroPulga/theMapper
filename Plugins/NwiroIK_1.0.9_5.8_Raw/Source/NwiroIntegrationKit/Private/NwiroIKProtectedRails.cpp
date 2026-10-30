// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKProtectedRails.h"

FString NwiroIKProtectedRails::Check(const FString& ToolName, const TSharedPtr<FJsonObject>& Args)
{
	if (DeniedTools.Contains(ToolName))
	{
		return FString::Printf(TEXT("Tool '%s' blocked by managed policy."), *ToolName);
	}

	if (!Args.IsValid())
	{
		return FString();
	}

	FString FilePath;
	if (!Args->TryGetStringField(TEXT("file_path"), FilePath))
	{
		return FString();
	}

	for (const FString& Prefix : DeniedFilePathPrefixes)
	{
		if (FilePath.StartsWith(Prefix))
		{
			return FString::Printf(
				TEXT("Path '%s' is in a protected directory (%s)."),
				*FilePath, *Prefix);
		}
	}

	for (const FString& Sub : DeniedFilePathSubstrings)
	{
		if (FilePath.Contains(Sub))
		{
			return FString::Printf(
				TEXT("Path '%s' contains a path-traversal sequence."),
				*FilePath);
		}
	}

	return FString();
}
