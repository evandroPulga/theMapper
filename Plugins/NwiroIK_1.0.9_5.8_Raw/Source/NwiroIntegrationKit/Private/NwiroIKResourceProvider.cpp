// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKResourceProvider.h"
#include "NwiroIKEditorTools.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "LevelEditor.h"
#include "SLevelViewport.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Selection.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "HAL/PlatformMemory.h"
#include "Json.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroResource, Log, All);

// ============================================================
// GET RESOURCE LIST
// ============================================================

FString FNwiroIKResourceProvider::GetResourceList()
{
	TArray<TSharedPtr<FJsonValue>> Resources;

	auto AddRes = [&](const FString& Uri, const FString& Name, const FString& Desc)
	{
		TSharedRef<FJsonObject> R = MakeShareable(new FJsonObject());
		R->SetStringField(TEXT("uri"), Uri);
		R->SetStringField(TEXT("name"), Name);
		R->SetStringField(TEXT("description"), Desc);
		R->SetStringField(TEXT("mimeType"), TEXT("application/json"));
		Resources.Add(MakeShareable(new FJsonValueObject(R)));
	};

	AddRes(TEXT("nwiro://project/info"), TEXT("Project Info"), TEXT("Engine version, project name, paths"));
	AddRes(TEXT("nwiro://level/current"), TEXT("Current Level"), TEXT("Level name, actor count, class distribution"));
	AddRes(TEXT("nwiro://editor/selection"), TEXT("Editor Selection"), TEXT("Currently selected actors"));
	AddRes(TEXT("nwiro://editor/performance"), TEXT("Performance Stats"), TEXT("FPS, memory, actor count"));
	AddRes(TEXT("nwiro://editor/log"), TEXT("Editor Log"), TEXT("Recent output log entries"));
	AddRes(TEXT("nwiro://editor/viewport"), TEXT("Viewport Info"), TEXT("Camera position, rotation, FOV"));

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("resources"), Resources);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// READ RESOURCE
// ============================================================

FString FNwiroIKResourceProvider::ReadResource(const FString& Uri)
{
	if (Uri == TEXT("nwiro://project/info")) return GetProjectInfo();
	if (Uri == TEXT("nwiro://level/current")) return GetCurrentLevel();
	if (Uri == TEXT("nwiro://editor/selection")) return GetEditorSelection();
	if (Uri == TEXT("nwiro://editor/performance")) return GetPerformanceStats();
	if (Uri == TEXT("nwiro://editor/log")) return GetEditorLog();
	if (Uri == TEXT("nwiro://editor/viewport")) return GetViewportInfo();

	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Unknown resource: %s\"}"), *Uri);
}

// ============================================================
// PROJECT INFO
// ============================================================

FString FNwiroIKResourceProvider::GetProjectInfo()
{
	TSharedRef<FJsonObject> R = MakeShareable(new FJsonObject());
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("projectName"), FApp::GetProjectName());
	R->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	R->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	R->SetStringField(TEXT("contentDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));
	R->SetStringField(TEXT("platform"), FPlatformProperties::PlatformName());
	R->SetNumberField(TEXT("cpuCores"), (double)FPlatformMisc::NumberOfCores());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(R, W);
	return Out;
}

// ============================================================
// CURRENT LEVEL
// ============================================================

FString FNwiroIKResourceProvider::GetCurrentLevel()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	TSharedRef<FJsonObject> R = MakeShareable(new FJsonObject());
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("levelName"), World->GetMapName());

	// Count actors by class
	TMap<FString, int32> ClassCounts;
	int32 Total = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		FString ClassName = (*It)->GetClass()->GetName();
		ClassCounts.FindOrAdd(ClassName)++;
		Total++;
	}

	R->SetNumberField(TEXT("actorCount"), Total);

	TArray<TSharedPtr<FJsonValue>> Classes;
	ClassCounts.ValueSort([](int32 A, int32 B) { return A > B; });
	int32 Count = 0;
	for (const auto& Pair : ClassCounts)
	{
		if (Count >= 20) break; // Top 20
		TSharedRef<FJsonObject> C = MakeShareable(new FJsonObject());
		C->SetStringField(TEXT("class"), Pair.Key);
		C->SetNumberField(TEXT("count"), Pair.Value);
		Classes.Add(MakeShareable(new FJsonValueObject(C)));
		Count++;
	}
	R->SetArrayField(TEXT("classDistribution"), Classes);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(R, W);
	return Out;
}

// ============================================================
// EDITOR SELECTION
// ============================================================

FString FNwiroIKResourceProvider::GetEditorSelection()
{
	TSharedRef<FJsonObject> R = MakeShareable(new FJsonObject());
	R->SetBoolField(TEXT("success"), true);

	TArray<TSharedPtr<FJsonValue>> Actors;
	USelection* Selection = GEditor->GetSelectedActors();
	for (int32 i = 0; i < Selection->Num(); i++)
	{
		AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i));
		if (!Actor) continue;

		TSharedRef<FJsonObject> A = MakeShareable(new FJsonObject());
		A->SetStringField(TEXT("name"), Actor->GetActorLabel());
		A->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		FVector Loc = Actor->GetActorLocation();
		A->SetNumberField(TEXT("x"), Loc.X);
		A->SetNumberField(TEXT("y"), Loc.Y);
		A->SetNumberField(TEXT("z"), Loc.Z);
		A->SetBoolField(TEXT("hidden"), Actor->IsHidden());
		Actors.Add(MakeShareable(new FJsonValueObject(A)));
	}
	R->SetArrayField(TEXT("selectedActors"), Actors);
	R->SetNumberField(TEXT("count"), Actors.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(R, W);
	return Out;
}

// ============================================================
// PERFORMANCE STATS
// ============================================================

FString FNwiroIKResourceProvider::GetPerformanceStats()
{
	TSharedRef<FJsonObject> R = MakeShareable(new FJsonObject());
	R->SetBoolField(TEXT("success"), true);

	// FPS
	extern ENGINE_API float GAverageFPS;
	extern ENGINE_API float GAverageMS;
	R->SetNumberField(TEXT("fps"), GAverageFPS);
	R->SetNumberField(TEXT("frameTimeMs"), GAverageMS);

	// Memory
	FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
	R->SetNumberField(TEXT("usedPhysicalMB"), (double)MemStats.UsedPhysical / (1024 * 1024));
	R->SetNumberField(TEXT("availablePhysicalMB"), (double)MemStats.AvailablePhysical / (1024 * 1024));
	R->SetNumberField(TEXT("usedVirtualMB"), (double)MemStats.UsedVirtual / (1024 * 1024));

	// Actor count
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (World)
	{
		int32 ActorCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It) ActorCount++;
		R->SetNumberField(TEXT("actorCount"), ActorCount);
	}

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(R, W);
	return Out;
}

// ============================================================
// EDITOR LOG
// ============================================================

FString FNwiroIKResourceProvider::GetEditorLog()
{
	TArray<FString> Lines = FNwiroIKLogCapture::Get().GetRecentLines(200);

	TSharedRef<FJsonObject> R = MakeShareable(new FJsonObject());
	R->SetBoolField(TEXT("success"), true);

	TArray<TSharedPtr<FJsonValue>> LinesArr;
	for (const FString& L : Lines)
		LinesArr.Add(MakeShareable(new FJsonValueString(L)));
	R->SetArrayField(TEXT("lines"), LinesArr);
	R->SetNumberField(TEXT("count"), LinesArr.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(R, W);
	return Out;
}

// ============================================================
// VIEWPORT INFO
// ============================================================

FString FNwiroIKResourceProvider::GetViewportInfo()
{
	TSharedRef<FJsonObject> R = MakeShareable(new FJsonObject());
	R->SetBoolField(TEXT("success"), true);

	FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
	TSharedPtr<IAssetViewport> Viewport = LevelEditorModule.GetFirstActiveViewport();

	if (Viewport.IsValid())
	{
		FEditorViewportClient& Client = Viewport->GetAssetViewportClient();
		FVector Loc = Client.GetViewLocation();
		FRotator Rot = Client.GetViewRotation();
		R->SetNumberField(TEXT("x"), Loc.X);
		R->SetNumberField(TEXT("y"), Loc.Y);
		R->SetNumberField(TEXT("z"), Loc.Z);
		R->SetNumberField(TEXT("pitch"), Rot.Pitch);
		R->SetNumberField(TEXT("yaw"), Rot.Yaw);
		R->SetNumberField(TEXT("roll"), Rot.Roll);
		R->SetNumberField(TEXT("fov"), Client.ViewFOV);
	}

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(R, W);
	return Out;
}
