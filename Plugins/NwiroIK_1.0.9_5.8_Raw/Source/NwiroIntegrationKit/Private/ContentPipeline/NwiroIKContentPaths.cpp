// Copyright 2026 Nwiro. All Rights Reserved.
#include "NwiroIKContentPaths.h"

#include "NwiroIKContentPaths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

namespace
{
	bool EnsureDirectory(const FString& InPath, FString& OutError)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (PlatformFile.DirectoryExists(*InPath))
		{
			return true;
		}

		if (PlatformFile.CreateDirectoryTree(*InPath))
		{
			return true;
		}

		OutError = FString::Printf(TEXT("Failed to create directory: %s"), *InPath);
		return false;
	}
}

FString FNwiroIKContentPaths::GetHomeDirectory()
{
#if PLATFORM_WINDOWS
	return FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
#elif PLATFORM_MAC || PLATFORM_LINUX
	return FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
#else
	return FPlatformProcess::UserDir();
#endif
}

FString FNwiroIKContentPaths::GetDepotRoot()
{
	const FString UserHome = GetHomeDirectory();

#if PLATFORM_WINDOWS
	return FPaths::Combine(UserHome, TEXT("AppData"), TEXT("Roaming"), TEXT("Nwiro"));
#elif PLATFORM_MAC
	return FPaths::Combine(UserHome, TEXT("Library"), TEXT("Application Support"), TEXT("Nwiro"));
#elif PLATFORM_LINUX
	return FPaths::Combine(UserHome, TEXT(".local"), TEXT("share"), TEXT("Nwiro"));
#else
	return FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("Nwiro"));
#endif
}

FString FNwiroIKContentPaths::GetCompressedRoot()
{
	return FPaths::Combine(GetDepotRoot(), TEXT("compressed"));
}

FString FNwiroIKContentPaths::GetExtractedRoot()
{
	return FPaths::Combine(GetDepotRoot(), TEXT("extracted"));
}

FString FNwiroIKContentPaths::GetImportsRoot()
{
	return FPaths::Combine(GetDepotRoot(), TEXT("imports"));
}

FString FNwiroIKContentPaths::GetStateRoot()
{
	return FPaths::Combine(GetDepotRoot(), TEXT("state"));
}

FString FNwiroIKContentPaths::MakeZipPath(const FString& Hash)
{
	return FPaths::Combine(GetCompressedRoot(), NormalizeHash(Hash) + TEXT(".zip"));
}

FString FNwiroIKContentPaths::MakePartialZipPath(const FString& Hash)
{
	return FPaths::Combine(GetCompressedRoot(), NormalizeHash(Hash) + TEXT(".zip.part"));
}

FString FNwiroIKContentPaths::MakeExtractedPath(const FString& Hash)
{
	return FPaths::Combine(GetExtractedRoot(), NormalizeHash(Hash));
}

FString FNwiroIKContentPaths::MakeUnzipMarkerPath(const FString& Hash)
{
	return FPaths::Combine(GetStateRoot(), NormalizeHash(Hash) + TEXT(".unzip.ok"));
}

FString FNwiroIKContentPaths::MakeImportMarkerPathForProject(const FString& Hash)
{
	const FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString ProjectToken = SanitizePathToken(ProjectPath);
	return FPaths::Combine(GetImportsRoot(), ProjectToken, NormalizeHash(Hash) + TEXT(".import.ok"));
}

FString FNwiroIKContentPaths::NormalizeHash(const FString& Hash)
{
	FString Out = Hash;
	Out.TrimStartAndEndInline();
	Out.ToLowerInline();
	return Out;
}

FString FNwiroIKContentPaths::SanitizePathToken(const FString& InValue)
{
	FString Out = InValue;
	Out.ReplaceInline(TEXT(":"), TEXT("_"));
	Out.ReplaceInline(TEXT("\\"), TEXT("_"));
	Out.ReplaceInline(TEXT("/"), TEXT("_"));
	Out.ReplaceInline(TEXT("."), TEXT("_"));
	Out.ReplaceInline(TEXT(" "), TEXT("_"));
	Out.ReplaceInline(TEXT("|"), TEXT("_"));
	return Out;
}

bool FNwiroIKContentPaths::EnsureBaseDirectories(FString& OutError)
{
	if (!EnsureDirectory(GetDepotRoot(), OutError))
	{
		return false;
	}
	if (!EnsureDirectory(GetCompressedRoot(), OutError))
	{
		return false;
	}
	if (!EnsureDirectory(GetExtractedRoot(), OutError))
	{
		return false;
	}
	if (!EnsureDirectory(GetImportsRoot(), OutError))
	{
		return false;
	}
	if (!EnsureDirectory(GetStateRoot(), OutError))
	{
		return false;
	}

	const FString ImportMarkerDir = FPaths::GetPath(MakeImportMarkerPathForProject(TEXT("placeholder")));
	if (!EnsureDirectory(ImportMarkerDir, OutError))
	{
		return false;
	}

	return true;
}
