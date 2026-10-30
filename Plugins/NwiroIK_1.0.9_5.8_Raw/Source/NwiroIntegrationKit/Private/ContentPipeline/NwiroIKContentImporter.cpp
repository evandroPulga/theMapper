// Copyright 2026 Nwiro. All Rights Reserved.
#include "NwiroIKContentImporter.h"

#include "NwiroIKContentImporter.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroContentImporter, Log, All);

namespace
{
	FString NormalizeRelativeImportPath(const FString& RelativeImportPath)
	{
		FString Out = RelativeImportPath;
		Out.ReplaceInline(TEXT("\\"), TEXT("/"));
		Out.TrimStartAndEndInline();

		while (Out.StartsWith(TEXT("/")))
		{
			Out.RightChopInline(1);
		}
		while (Out.EndsWith(TEXT("/")))
		{
			Out.LeftChopInline(1);
		}

		if (Out.StartsWith(TEXT("Content/")))
		{
			Out.RightChopInline(8);
		}
		if (Out.StartsWith(TEXT("Game/")))
		{
			Out.RightChopInline(5);
		}
		if (Out.StartsWith(TEXT("/Game/")))
		{
			Out.RightChopInline(6);
		}

		return Out;
	}

	void AddSyncFolderIfValid(const FString& RelativePath, TSet<FString>& InOutFolders)
	{
		FString LocalRelativePath = RelativePath;
		LocalRelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));
		LocalRelativePath.TrimStartAndEndInline();
		if (LocalRelativePath.IsEmpty())
		{
			return;
		}

		int32 SlashIndex = INDEX_NONE;
		if (LocalRelativePath.FindChar(TEXT('/'), SlashIndex))
		{
			LocalRelativePath = LocalRelativePath.Left(SlashIndex);
		}

		if (!LocalRelativePath.IsEmpty())
		{
			InOutFolders.Add(FString::Printf(TEXT("/Game/%s"), *LocalRelativePath));
		}
	}

	bool ShouldSkipImportedRelativePath(const FString& RelativePath)
	{
		FString Normalized = RelativePath;
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Normalized.StartsWith(TEXT("/")))
		{
			Normalized.RightChopInline(1);
		}

		FString TopLevelFolder = Normalized;
		int32 SlashIndex = INDEX_NONE;
		if (TopLevelFolder.FindChar(TEXT('/'), SlashIndex))
		{
			TopLevelFolder = TopLevelFolder.Left(SlashIndex);
		}

		return TopLevelFolder.Equals(TEXT("Splash"), ESearchCase::IgnoreCase)
			|| TopLevelFolder.Equals(TEXT("Developer"), ESearchCase::IgnoreCase)
			|| TopLevelFolder.Equals(TEXT("Developers"), ESearchCase::IgnoreCase)
			|| TopLevelFolder.Equals(TEXT("Collections"), ESearchCase::IgnoreCase);
	}
}

FNwiroIKImportResult FNwiroIKContentImporter::ImportToCurrentProject(
	const FString& ExtractedDirectory,
	const FString& RelativeImportPath)
{
	FNwiroIKImportResult Result;

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*ExtractedDirectory))
	{
		Result.Error = FString::Printf(TEXT("Extracted directory does not exist: %s"), *ExtractedDirectory);
		return Result;
	}

	FString ContentRoot;
	FNwiroIKContentImporter::ESourceLayout SourceLayout = FNwiroIKContentImporter::ESourceLayout::ContentFolder;
	if (!FindSourceContentRoot(ExtractedDirectory, ContentRoot, SourceLayout, Result.Error))
	{
		return Result;
	}

	// Some archives are packaged as ".../Content/Content/...".
	// In ContentFolder mode, collapse redundant wrapper layers to avoid importing into ".../<ImportRoot>/Content/...".
	if (SourceLayout == FNwiroIKContentImporter::ESourceLayout::ContentFolder)
	{
		FString EffectiveRoot = ContentRoot;
		for (;;)
		{
			TArray<FString> TopLevelFiles;
			TArray<FString> TopLevelDirs;
			const FString WildcardPath = FPaths::Combine(EffectiveRoot, TEXT("*"));
			IFileManager::Get().FindFiles(TopLevelFiles, *WildcardPath, true, false);
			IFileManager::Get().FindFiles(TopLevelDirs, *WildcardPath, false, true);

			const bool bSingleNestedContent =
				TopLevelFiles.IsEmpty()
				&& TopLevelDirs.Num() == 1
				&& TopLevelDirs[0].Equals(TEXT("Content"), ESearchCase::IgnoreCase);

			if (!bSingleNestedContent)
			{
				break;
			}

			const FString NestedContentRoot = FPaths::Combine(EffectiveRoot, TopLevelDirs[0]);
			if (!PlatformFile.DirectoryExists(*NestedContentRoot))
			{
				break;
			}

			EffectiveRoot = NestedContentRoot;
		}

		ContentRoot = EffectiveRoot;
	}

	UE_LOG(
		LogNwiroContentImporter,
		Log,
		TEXT("Import source resolved. ExtractedDir=%s SourceRoot=%s Layout=%s"),
		*ExtractedDirectory,
		*ContentRoot,
		SourceLayout == FNwiroIKContentImporter::ESourceLayout::ContentFolder ? TEXT("ContentFolder") : TEXT("PackageRoot"));

	const FString ImportRoot = NormalizeRelativeImportPath(RelativeImportPath);
	FString DestinationRoot;
	if (SourceLayout == ESourceLayout::ContentFolder)
	{
		DestinationRoot = ImportRoot.IsEmpty()
			? FPaths::ProjectContentDir()
			: FPaths::Combine(FPaths::ProjectContentDir(), ImportRoot);
	}
	else
	{
		// PackageRoot mode: Keep source root folder(s) directly under Project/Content.
		DestinationRoot = FPaths::ProjectContentDir();
	}

	if (!PlatformFile.DirectoryExists(*DestinationRoot) && !PlatformFile.CreateDirectoryTree(*DestinationRoot))
	{
		Result.Error = FString::Printf(TEXT("Failed to create destination root: %s"), *DestinationRoot);
		return Result;
	}

	UE_LOG(
		LogNwiroContentImporter,
		Log,
		TEXT("Import destination resolved. ImportRoot=%s DestinationRoot=%s"),
		ImportRoot.IsEmpty() ? TEXT("<default>") : *ImportRoot,
		*DestinationRoot);

	TArray<FString> SourceFiles;
	PlatformFile.FindFilesRecursively(SourceFiles, *ContentRoot, nullptr);
	if (SourceFiles.IsEmpty())
	{
		Result.Error = FString::Printf(TEXT("No files found under content root: %s"), *ContentRoot);
		return Result;
	}

	TSet<FString> AbsolutePathsToScan;
	TSet<FString> GameFoldersToSync;
	for (const FString& SourceFile : SourceFiles)
	{
		FString RelativePath = SourceFile;
		FPaths::MakePathRelativeTo(RelativePath, *ContentRoot);
		RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));

		// Defensive fix: never re-nest "Content" under destination import root.
		// If archive layout still leaks a leading "Content/" segment, trim it.
		if (SourceLayout == ESourceLayout::ContentFolder)
		{
			while (RelativePath.StartsWith(TEXT("Content/"), ESearchCase::IgnoreCase))
			{
				RelativePath.RightChopInline(8);
			}
		}

		if (ShouldSkipImportedRelativePath(RelativePath))
		{
			++Result.SkippedFiles;
			continue;
		}

		const FString DestinationFilePath = FPaths::Combine(DestinationRoot, RelativePath);
		const FString DestinationDir = FPaths::GetPath(DestinationFilePath);
		if (!DestinationDir.IsEmpty() &&
			!PlatformFile.DirectoryExists(*DestinationDir) &&
			!PlatformFile.CreateDirectoryTree(*DestinationDir))
		{
			Result.Error = FString::Printf(TEXT("Failed to create destination directory: %s"), *DestinationDir);
			return Result;
		}

		if (PlatformFile.FileExists(*DestinationFilePath))
		{
			++Result.SkippedFiles;
		}
		else
		{
			if (!PlatformFile.CopyFile(*DestinationFilePath, *SourceFile))
			{
				Result.Error = FString::Printf(TEXT("Failed to copy file to destination: %s"), *DestinationFilePath);
				return Result;
			}
			++Result.CopiedFiles;
		}

		FString ScanRelativePath;
		if (SourceLayout == ESourceLayout::ContentFolder)
		{
			ScanRelativePath = ImportRoot;
			if (!ScanRelativePath.IsEmpty())
			{
				ScanRelativePath = FPaths::Combine(ScanRelativePath, RelativePath);
			}
			else
			{
				ScanRelativePath = RelativePath;
			}
		}
		else
		{
			ScanRelativePath = RelativePath;
		}
		ScanRelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));

		AddSyncFolderIfValid(ScanRelativePath, GameFoldersToSync);
	}

	for (const FString& Folder : GameFoldersToSync)
	{
		FString RelativeGamePath = Folder;
		RelativeGamePath.RemoveFromStart(TEXT("/Game/"));
		const FString AbsolutePath = FPaths::Combine(FPaths::ProjectContentDir(), RelativeGamePath);
		AbsolutePathsToScan.Add(AbsolutePath);
	}

	Result.AbsolutePathsToScan.Append(AbsolutePathsToScan.Array());
	Result.GameFoldersToSync.Append(GameFoldersToSync.Array());
	Result.bSuccess = true;
	return Result;
}

bool FNwiroIKContentImporter::FindSourceContentRoot(
	const FString& ExtractedDirectory,
	FString& OutContentRoot,
	FNwiroIKContentImporter::ESourceLayout& OutLayout,
	FString& OutError)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	const FString DirectContentPath = FPaths::Combine(ExtractedDirectory, TEXT("Content"));
	if (PlatformFile.DirectoryExists(*DirectContentPath))
	{
		OutContentRoot = DirectContentPath;
		OutLayout = FNwiroIKContentImporter::ESourceLayout::ContentFolder;
		return true;
	}

	// IPlatformFile::FindFilesRecursively only supports file search (3 params).
	// Detect nested ".../Content/..." roots by scanning asset files and deriving the parent Content directory.
	TArray<FString> AssetFiles;
	PlatformFile.FindFilesRecursively(AssetFiles, *ExtractedDirectory, TEXT("uasset"));
	if (AssetFiles.IsEmpty())
	{
		PlatformFile.FindFilesRecursively(AssetFiles, *ExtractedDirectory, TEXT("umap"));
	}

	for (const FString& AssetFile : AssetFiles)
	{
		FString Normalized = AssetFile;
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));

		const int32 ContentMarkerIndex = Normalized.Find(TEXT("/Content/"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
		if (ContentMarkerIndex != INDEX_NONE)
		{
			OutContentRoot = Normalized.Left(ContentMarkerIndex + 8); // include "/Content"
			OutLayout = FNwiroIKContentImporter::ESourceLayout::ContentFolder;
			return true;
		}
	}

	// Fallback: packs without a Content root (e.g. PackName/Maps, PackName/Meshes...)
	// are imported from extracted root while preserving their top-level folder.
	if (!AssetFiles.IsEmpty())
	{
		// If extracted root is just a hash wrapper containing a single package folder,
		// import from that folder instead of importing the hash folder itself.
		TArray<FString> TopLevelFiles;
		TArray<FString> TopLevelDirs;
		const FString WildcardPath = FPaths::Combine(ExtractedDirectory, TEXT("*"));
		IFileManager::Get().FindFiles(TopLevelFiles, *WildcardPath, true, false);
		IFileManager::Get().FindFiles(TopLevelDirs, *WildcardPath, false, true);

		if (TopLevelFiles.IsEmpty() && TopLevelDirs.Num() == 1)
		{
			OutContentRoot = FPaths::Combine(ExtractedDirectory, TopLevelDirs[0]);
		}
		else
		{
			OutContentRoot = ExtractedDirectory;
		}
		OutLayout = FNwiroIKContentImporter::ESourceLayout::PackageRoot;
		return true;
	}

	OutError = FString::Printf(TEXT("No Content directory found under extracted folder: %s"), *ExtractedDirectory);
	return false;
}
