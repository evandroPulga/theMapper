// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKEditorTools.h"
#include "Editor.h"
#include "LevelEditor.h"
#include "SLevelViewport.h"
#include "LevelEditorViewport.h"
#include "UnrealClient.h"
#include "Slate/SceneViewport.h"
#include "Engine/GameViewportClient.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ImageUtils.h"
#include "Json.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroEditor, Log, All);

// ============================================================
// LOG CAPTURE SINGLETON
// ============================================================

FNwiroIKLogCapture& FNwiroIKLogCapture::Get()
{
	static FNwiroIKLogCapture Instance;
	return Instance;
}

void FNwiroIKLogCapture::Register()
{
	GLog->AddOutputDevice(this);
}

void FNwiroIKLogCapture::Unregister()
{
	GLog->RemoveOutputDevice(this);
}

void FNwiroIKLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	FScopeLock ScopeLock(&Lock);

	FString Severity;
	switch (Verbosity)
	{
	case ELogVerbosity::Fatal:   Severity = TEXT("Fatal"); break;
	case ELogVerbosity::Error:   Severity = TEXT("Error"); break;
	case ELogVerbosity::Warning: Severity = TEXT("Warning"); break;
	case ELogVerbosity::Display: Severity = TEXT("Display"); break;
	default:                     Severity = TEXT("Log"); break;
	}

	FString Line = FString::Printf(TEXT("[%s][%s] %s"), *Severity, *Category.ToString(), V);
	Lines.Add(Line);

	if (Lines.Num() > MaxLines)
	{
		Lines.RemoveAt(0, Lines.Num() - MaxLines);
	}
}

TArray<FString> FNwiroIKLogCapture::GetRecentLines(int32 Count, const FString& SeverityFilter, const FString& CategoryFilter) const
{
	FScopeLock ScopeLock(&Lock);

	TArray<FString> Result;

	for (int32 i = Lines.Num() - 1; i >= 0 && Result.Num() < Count; --i)
	{
		const FString& Line = Lines[i];

		if (!SeverityFilter.IsEmpty() && !Line.Contains(FString::Printf(TEXT("[%s]"), *SeverityFilter), ESearchCase::IgnoreCase))
			continue;
		if (!CategoryFilter.IsEmpty() && !Line.Contains(CategoryFilter, ESearchCase::IgnoreCase))
			continue;

		Result.Insert(Line, 0);
	}

	return Result;
}

// ============================================================
// TAKE SCREENSHOT
// ============================================================

FString FNwiroIKEditorTools::TakeScreenshot(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	FJsonSerializer::Deserialize(Reader, Cmd);

	FString Filename = Cmd.IsValid() ? Cmd->GetStringField(TEXT("filename")) : TEXT("");
	if (Filename.IsEmpty())
	{
		Filename = FString::Printf(TEXT("Screenshot_%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}

	FString SaveDir = FPaths::ProjectSavedDir() / TEXT("NwiroScreenshots");
	FString FullPath = SaveDir / Filename;

	// Get the active level editor viewport
	FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
	TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule.GetFirstActiveViewport();

	if (!ActiveViewport.IsValid())
		return TEXT("{\"success\":false,\"error\":\"No active viewport\"}");

	FViewport* Viewport = ActiveViewport->GetActiveViewport();
	if (!Viewport)
		return TEXT("{\"success\":false,\"error\":\"No viewport found\"}");

	// Read pixels from viewport
	TArray<FColor> Bitmap;
	int32 Width = Viewport->GetSizeXY().X;
	int32 Height = Viewport->GetSizeXY().Y;

	if (Width == 0 || Height == 0)
		return TEXT("{\"success\":false,\"error\":\"Viewport has zero size\"}");

	bool bSuccess = Viewport->ReadPixels(Bitmap);
	if (!bSuccess || Bitmap.Num() == 0)
		return TEXT("{\"success\":false,\"error\":\"Failed to read viewport pixels\"}");

	// Encode to PNG
	IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> PngWrapper = ImageModule.CreateImageWrapper(EImageFormat::PNG);

	PngWrapper->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8);
	const TArray64<uint8> PngData = PngWrapper->GetCompressed();

	FFileHelper::SaveArrayToFile(PngData, *FullPath);

	UE_LOG(LogNwiroEditor, Log, TEXT("Screenshot saved: %s (%dx%d)"), *FullPath, Width, Height);

	return FString::Printf(TEXT("{\"success\":true,\"path\":\"%s\",\"width\":%d,\"height\":%d}"),
		*FullPath.Replace(TEXT("\\"), TEXT("/")), Width, Height);
}

// ============================================================
// READ LOG
// ============================================================

FString FNwiroIKEditorTools::ReadLog(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	FJsonSerializer::Deserialize(Reader, Cmd);

	int32 LineCount = 50;
	FString Severity, Category;

	if (Cmd.IsValid())
	{
		if (Cmd->HasField(TEXT("lines"))) LineCount = (int32)Cmd->GetNumberField(TEXT("lines"));
		Severity = Cmd->GetStringField(TEXT("severity"));
		Category = Cmd->GetStringField(TEXT("category"));
	}

	TArray<FString> LogLines = FNwiroIKLogCapture::Get().GetRecentLines(LineCount, Severity, Category);

	TArray<TSharedPtr<FJsonValue>> LinesArr;
	for (const FString& L : LogLines)
	{
		LinesArr.Add(MakeShareable(new FJsonValueString(L)));
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("lines"), LinesArr);
	Result->SetNumberField(TEXT("count"), LinesArr.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// PLAY IN EDITOR
// ============================================================

FString FNwiroIKEditorTools::PlayInEditor(const FString& JsonCommand)
{
	if (!GEditor) return TEXT("{\"success\":false,\"error\":\"No editor\"}");

	if (GEditor->PlayWorld)
		return TEXT("{\"success\":false,\"error\":\"PIE already running\"}");

	FRequestPlaySessionParams Params;
	// Default to PIE
	GEditor->RequestPlaySession(Params);

	return TEXT("{\"success\":true,\"message\":\"Play in Editor started\"}");
}

// ============================================================
// STOP PIE
// ============================================================

FString FNwiroIKEditorTools::StopPIE(const FString& JsonCommand)
{
	if (!GEditor) return TEXT("{\"success\":false,\"error\":\"No editor\"}");

	if (!GEditor->PlayWorld)
		return TEXT("{\"success\":false,\"error\":\"PIE not running\"}");

	GEditor->RequestEndPlayMap();
	return TEXT("{\"success\":true,\"message\":\"Play in Editor stopped\"}");
}
