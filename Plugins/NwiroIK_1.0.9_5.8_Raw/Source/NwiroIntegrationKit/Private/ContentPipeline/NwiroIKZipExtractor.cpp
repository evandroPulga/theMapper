// Copyright 2026 Nwiro. All Rights Reserved.
#include "NwiroIKZipExtractor.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

namespace
{
	FString BuildTargetPath(
		const FString& OutputDirectory,
		const FString& EntryPath,
		const bool bStripRootFolder,
		FString& InOutRootPrefix)
	{
		FString NormalizedEntryPath = EntryPath;
		NormalizedEntryPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		NormalizedEntryPath.TrimStartAndEndInline();

		if (!bStripRootFolder)
		{
			return FPaths::Combine(OutputDirectory, NormalizedEntryPath);
		}

		if (InOutRootPrefix.IsEmpty())
		{
			int32 SlashIndex = INDEX_NONE;
			if (NormalizedEntryPath.FindChar(TEXT('/'), SlashIndex))
			{
				InOutRootPrefix = NormalizedEntryPath.Left(SlashIndex + 1);
			}
		}

		if (!InOutRootPrefix.IsEmpty() && NormalizedEntryPath.StartsWith(InOutRootPrefix))
		{
			NormalizedEntryPath.RightChopInline(InOutRootPrefix.Len());
		}

		return FPaths::Combine(OutputDirectory, NormalizedEntryPath);
	}
}

bool FNwiroIKZipExtractor::ExtractZip(
	const FString& ZipFilePath,
	const FString& OutputDirectory,
	const bool bStripRootFolder,
	FString& OutError)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.FileExists(*ZipFilePath))
	{
		OutError = FString::Printf(TEXT("Zip file does not exist: %s"), *ZipFilePath);
		return false;
	}

	if (!PlatformFile.DirectoryExists(*OutputDirectory) && !PlatformFile.CreateDirectoryTree(*OutputDirectory))
	{
		OutError = FString::Printf(TEXT("Failed to create output directory: %s"), *OutputDirectory);
		return false;
	}

	mz_zip_archive ZipArchive = {};
	if (!mz_zip_reader_init_file(&ZipArchive, TCHAR_TO_ANSI(*ZipFilePath), 0))
	{
		OutError = FString::Printf(TEXT("Failed to open zip archive: %s"), *ZipFilePath);
		return false;
	}

	const int32 FileCount = static_cast<int32>(mz_zip_reader_get_num_files(&ZipArchive));
	FString RootPrefix;

	for (int32 FileIndex = 0; FileIndex < FileCount; ++FileIndex)
	{
		mz_zip_archive_file_stat Stat = {};
		if (!mz_zip_reader_file_stat(&ZipArchive, FileIndex, &Stat))
		{
			OutError = FString::Printf(TEXT("Failed to read zip entry stat for index %d"), FileIndex);
			mz_zip_reader_end(&ZipArchive);
			return false;
		}

		const FString EntryPath = ANSI_TO_TCHAR(Stat.m_filename);
		const FString TargetPath = BuildTargetPath(OutputDirectory, EntryPath, bStripRootFolder, RootPrefix);
		if (TargetPath.IsEmpty())
		{
			continue;
		}

		if (mz_zip_reader_is_file_a_directory(&ZipArchive, FileIndex))
		{
			if (!PlatformFile.DirectoryExists(*TargetPath) && !PlatformFile.CreateDirectoryTree(*TargetPath))
			{
				OutError = FString::Printf(TEXT("Failed to create directory: %s"), *TargetPath);
				mz_zip_reader_end(&ZipArchive);
				return false;
			}
			continue;
		}

		const FString TargetDirectory = FPaths::GetPath(TargetPath);
		if (!TargetDirectory.IsEmpty() &&
			!PlatformFile.DirectoryExists(*TargetDirectory) &&
			!PlatformFile.CreateDirectoryTree(*TargetDirectory))
		{
			OutError = FString::Printf(TEXT("Failed to create directory: %s"), *TargetDirectory);
			mz_zip_reader_end(&ZipArchive);
			return false;
		}

		if (!mz_zip_reader_extract_to_file(&ZipArchive, FileIndex, TCHAR_TO_ANSI(*TargetPath), 0))
		{
			OutError = FString::Printf(TEXT("Failed to extract file: %s"), *TargetPath);
			mz_zip_reader_end(&ZipArchive);
			return false;
		}
	}

	mz_zip_reader_end(&ZipArchive);
	return true;
}
