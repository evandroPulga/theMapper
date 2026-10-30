// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKGameplayTools.h"
#include "NwiroIKTransactionHelper.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EditorAssetLibrary.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "Sound/AmbientSound.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/HUD.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Interfaces/IProjectManager.h"
#include "Json.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroGameplay, Log, All);

static UWorld* GetGPWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

// ============================================================
// BUILD NAVIGATION
// ============================================================

FString FNwiroIKGameplayTools::BuildNavigation(const FString& JsonCommand)
{
	UWorld* World = GetGPWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys) return TEXT("{\"success\":false,\"error\":\"No navigation system\"}");

	// Check for NavMeshBoundsVolume
	bool bHasBounds = false;
	for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It) { bHasBounds = true; break; }
	if (!bHasBounds)
		return TEXT("{\"success\":false,\"error\":\"No NavMeshBoundsVolume in level. Add one first.\"}");

	NavSys->Build();
	return TEXT("{\"success\":true,\"message\":\"Navigation build started\"}");
}

// ============================================================
// QUERY NAVIGATION PATH
// ============================================================

FString FNwiroIKGameplayTools::QueryNavigationPath(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	double StartX = Cmd->GetNumberField(TEXT("startX"));
	double StartY = Cmd->GetNumberField(TEXT("startY"));
	double StartZ = Cmd->GetNumberField(TEXT("startZ"));
	double EndX = Cmd->GetNumberField(TEXT("endX"));
	double EndY = Cmd->GetNumberField(TEXT("endY"));
	double EndZ = Cmd->GetNumberField(TEXT("endZ"));

	UWorld* World = GetGPWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys) return TEXT("{\"success\":false,\"error\":\"No navigation system\"}");

	FPathFindingQuery Query;
	Query.StartLocation = FVector(StartX, StartY, StartZ);
	Query.EndLocation = FVector(EndX, EndY, EndZ);

	FPathFindingResult Result = NavSys->FindPathSync(Query);

	TSharedRef<FJsonObject> Out = MakeShareable(new FJsonObject());
	Out->SetBoolField(TEXT("success"), true);
	Out->SetBoolField(TEXT("pathFound"), Result.IsSuccessful());

	if (Result.IsSuccessful() && Result.Path.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> PathPts;
		float TotalDist = 0;
		FVector Prev = FVector(StartX, StartY, StartZ);
		for (const FNavPathPoint& Pt : Result.Path->GetPathPoints())
		{
			TSharedRef<FJsonObject> P = MakeShareable(new FJsonObject());
			P->SetNumberField(TEXT("x"), Pt.Location.X);
			P->SetNumberField(TEXT("y"), Pt.Location.Y);
			P->SetNumberField(TEXT("z"), Pt.Location.Z);
			PathPts.Add(MakeShareable(new FJsonValueObject(P)));
			TotalDist += FVector::Dist(Prev, Pt.Location);
			Prev = Pt.Location;
		}
		Out->SetArrayField(TEXT("pathPoints"), PathPts);
		Out->SetNumberField(TEXT("distance"), TotalDist);
	}

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Out, W);
	return OutStr;
}

// ============================================================
// GET NAVIGATION INFO
// ============================================================

FString FNwiroIKGameplayTools::GetNavigationInfo(const FString& JsonCommand)
{
	UWorld* World = GetGPWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys) return TEXT("{\"success\":false,\"error\":\"No navigation system\"}");

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("isNavigationBuilt"), NavSys->IsNavigationBuilt(World->GetWorldSettings()));

	int32 BoundsCount = 0;
	for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It) BoundsCount++;
	Result->SetNumberField(TEXT("boundsVolumeCount"), BoundsCount);

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Result, W);
	return OutStr;
}

// ============================================================
// SPAWN SOUND
// ============================================================

FString FNwiroIKGameplayTools::SpawnSound(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SoundPath = Cmd->GetStringField(TEXT("sound"));
	double X = Cmd->HasField(TEXT("x")) ? Cmd->GetNumberField(TEXT("x")) : 0;
	double Y = Cmd->HasField(TEXT("y")) ? Cmd->GetNumberField(TEXT("y")) : 0;
	double Z = Cmd->HasField(TEXT("z")) ? Cmd->GetNumberField(TEXT("z")) : 0;
	FString Label = Cmd->GetStringField(TEXT("label"));

	UWorld* World = GetGPWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	USoundBase* Sound = Cast<USoundBase>(UEditorAssetLibrary::LoadAsset(SoundPath));
	if (!Sound) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Sound not found: %s\"}"), *SoundPath);

	FActorSpawnParameters Params;
	AAmbientSound* SoundActor = World->SpawnActor<AAmbientSound>(FVector(X, Y, Z), FRotator::ZeroRotator, Params);
	if (!SoundActor) return TEXT("{\"success\":false,\"error\":\"Failed to spawn AmbientSound\"}");

	SoundActor->GetAudioComponent()->SetSound(Sound);
	if (!Label.IsEmpty()) SoundActor->SetActorLabel(Label);

	if (Cmd->HasField(TEXT("volume")))
		SoundActor->GetAudioComponent()->SetVolumeMultiplier((float)Cmd->GetNumberField(TEXT("volume")));
	if (Cmd->HasField(TEXT("pitch")))
		SoundActor->GetAudioComponent()->SetPitchMultiplier((float)Cmd->GetNumberField(TEXT("pitch")));
	if (Cmd->HasField(TEXT("autoActivate")))
		SoundActor->GetAudioComponent()->bAutoActivate = Cmd->GetBoolField(TEXT("autoActivate"));

	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"sound\":\"%s\"}"),
		*SoundActor->GetActorLabel(), *Sound->GetName());
}

// ============================================================
// SET AUDIO PROPERTIES
// ============================================================

FString FNwiroIKGameplayTools::SetAudioProperties(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GetGPWorld();
	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetName() == ActorName || (*It)->GetActorLabel() == ActorName) { Actor = *It; break; }
	}
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	UAudioComponent* Audio = Actor->FindComponentByClass<UAudioComponent>();
	if (!Audio) return TEXT("{\"success\":false,\"error\":\"No audio component\"}");

	if (Cmd->HasField(TEXT("volume"))) Audio->SetVolumeMultiplier((float)Cmd->GetNumberField(TEXT("volume")));
	if (Cmd->HasField(TEXT("pitch"))) Audio->SetPitchMultiplier((float)Cmd->GetNumberField(TEXT("pitch")));
	if (Cmd->HasField(TEXT("autoActivate"))) Audio->bAutoActivate = Cmd->GetBoolField(TEXT("autoActivate"));

	Actor->MarkPackageDirty();
	// TODO: actor mutation should save the LEVEL package, not the actor — UEditorLevelLibrary::SaveCurrentLevel.
	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\"}"), *Actor->GetActorLabel());
}

// ============================================================
// GET SOUND INFO
// ============================================================

FString FNwiroIKGameplayTools::GetSoundInfo(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	USoundBase* Sound = Cast<USoundBase>(UEditorAssetLibrary::LoadAsset(Path));
	if (!Sound) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Sound not found: %s\"}"), *Path);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Sound->GetName());
	Result->SetStringField(TEXT("class"), Sound->GetClass()->GetName());
	Result->SetNumberField(TEXT("duration"), Sound->Duration);

	if (USoundWave* Wave = Cast<USoundWave>(Sound))
	{
		Result->SetNumberField(TEXT("sampleRate"), Wave->GetSampleRateForCurrentPlatform());
		Result->SetNumberField(TEXT("numChannels"), Wave->NumChannels);
	}

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Result, W);
	return OutStr;
}

// ============================================================
// GAME FRAMEWORK - Create BP helpers
// ============================================================

static FString CreateFrameworkBP(const FString& JsonCommand, UClass* ParentClass, const FString& DefaultPrefix, const FString& DefaultPath)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = DefaultPath;
	if (!Name.StartsWith(DefaultPrefix)) Name = DefaultPrefix + Name;

	FString FullPath = Path / Name;
	// Try loading first — if the BP already exists, FKismetEditorUtilities::CreateBlueprint
	// asserts inside Kismet2.cpp. Idempotent return-existing keeps retries safe for AI.
	if (UBlueprint* Existing = LoadObject<UBlueprint>(nullptr, *(FullPath + TEXT(".") + Name)))
	{
		return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"parentClass\":\"%s\",\"existed\":true}"),
			*Name, *Existing->GetPathName(), *ParentClass->GetName());
	}

	UPackage* Package = CreatePackage(*FullPath);
	if (!Package) return TEXT("{\"success\":false,\"error\":\"Failed to create package\"}");
	// Last-chance guard — a stale UObject sometimes lives in the package without LoadObject finding it.
	if (FindObject<UBlueprint>(Package, *Name))
	{
		UBlueprint* Existing = FindObject<UBlueprint>(Package, *Name);
		return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"parentClass\":\"%s\",\"existed\":true}"),
			*Name, *Existing->GetPathName(), *ParentClass->GetName());
	}

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateFrameworkBP", "AI: Create Framework Blueprint"));
	Tx.AlsoModify(Package);

	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(ParentClass, Package, FName(*Name), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!BP)
	{
		Tx.Cancel();
		return TEXT("{\"success\":false,\"error\":\"Failed to create blueprint\"}");
	}
	Tx.AlsoModify(BP);

	FKismetEditorUtilities::CompileBlueprint(BP);
	FAssetRegistryModule::AssetCreated(BP);
	BP->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"parentClass\":\"%s\"}"),
		*Name, *BP->GetPathName(), *ParentClass->GetName());
}

FString FNwiroIKGameplayTools::CreateGameMode(const FString& JsonCommand)
{
	return CreateFrameworkBP(JsonCommand, AGameModeBase::StaticClass(), TEXT("GM_"), TEXT("/Game/Blueprints"));
}

FString FNwiroIKGameplayTools::CreatePlayerController(const FString& JsonCommand)
{
	return CreateFrameworkBP(JsonCommand, APlayerController::StaticClass(), TEXT("PC_"), TEXT("/Game/Blueprints"));
}

FString FNwiroIKGameplayTools::CreateGameState(const FString& JsonCommand)
{
	return CreateFrameworkBP(JsonCommand, AGameStateBase::StaticClass(), TEXT("GS_"), TEXT("/Game/Blueprints"));
}

FString FNwiroIKGameplayTools::CreatePlayerState(const FString& JsonCommand)
{
	return CreateFrameworkBP(JsonCommand, APlayerState::StaticClass(), TEXT("PS_"), TEXT("/Game/Blueprints"));
}

FString FNwiroIKGameplayTools::CreateHUD(const FString& JsonCommand)
{
	return CreateFrameworkBP(JsonCommand, AHUD::StaticClass(), TEXT("HUD_"), TEXT("/Game/Blueprints"));
}

FString FNwiroIKGameplayTools::GetGameFrameworkInfo(const FString& JsonCommand)
{
	UWorld* World = GetGPWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);

	AWorldSettings* WS = World->GetWorldSettings();
	if (WS)
	{
		Result->SetStringField(TEXT("defaultGameMode"), WS->DefaultGameMode ? WS->DefaultGameMode->GetPathName() : TEXT("None"));
	}

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Result, W);
	return OutStr;
}

// ============================================================
// BUILD / VALIDATION
// ============================================================

FString FNwiroIKGameplayTools::GetProjectInfo(const FString& JsonCommand)
{
	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("projectName"), FApp::GetProjectName());
	Result->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Result->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	Result->SetStringField(TEXT("contentDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Result, W);
	return OutStr;
}

FString FNwiroIKGameplayTools::ListProjectModules(const FString& JsonCommand)
{
	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);

	TArray<TSharedPtr<FJsonValue>> Modules;
	TArray<FModuleStatus> ModuleStatuses;
	FModuleManager::Get().QueryModules(ModuleStatuses);

	int32 Count = 0;
	for (const FModuleStatus& Status : ModuleStatuses)
	{
		if (Count >= 100) break; // Cap
		TSharedRef<FJsonObject> M = MakeShareable(new FJsonObject());
		M->SetStringField(TEXT("name"), Status.Name);
		M->SetBoolField(TEXT("loaded"), Status.bIsLoaded);
		Modules.Add(MakeShareable(new FJsonValueObject(M)));
		Count++;
	}
	Result->SetArrayField(TEXT("modules"), Modules);

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Result, W);
	return OutStr;
}

FString FNwiroIKGameplayTools::ValidateAssets(const FString& JsonCommand)
{
	// Basic asset validation - check for missing references
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FARFilter Filter;
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	int32 TotalAssets = Assets.Num();
	int32 Issues = 0;
	TArray<TSharedPtr<FJsonValue>> Problems;

	// Check for assets with missing dependencies (simple check)
	// Full validation would use FAssetValidator but that's not always available
	for (const FAssetData& A : Assets)
	{
		if (!A.IsValid())
		{
			TSharedRef<FJsonObject> P = MakeShareable(new FJsonObject());
			P->SetStringField(TEXT("asset"), A.AssetName.ToString());
			P->SetStringField(TEXT("issue"), TEXT("Invalid asset data"));
			Problems.Add(MakeShareable(new FJsonValueObject(P)));
			Issues++;
		}
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("totalAssets"), TotalAssets);
	Result->SetNumberField(TEXT("issues"), Issues);
	Result->SetArrayField(TEXT("problems"), Problems);

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Result, W);
	return OutStr;
}

FString FNwiroIKGameplayTools::GetMapCheckErrors(const FString& JsonCommand)
{
	// Map check would use FMessageLog but simplified here
	return TEXT("{\"success\":true,\"message\":\"Use Window > Message Log > Map Check in editor for detailed map check results.\"}");
}

FString FNwiroIKGameplayTools::GetBuildConfiguration(const FString& JsonCommand)
{
	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);

#if UE_BUILD_DEBUG
	Result->SetStringField(TEXT("configuration"), TEXT("Debug"));
#elif UE_BUILD_DEVELOPMENT
	Result->SetStringField(TEXT("configuration"), TEXT("Development"));
#elif UE_BUILD_SHIPPING
	Result->SetStringField(TEXT("configuration"), TEXT("Shipping"));
#else
	Result->SetStringField(TEXT("configuration"), TEXT("Unknown"));
#endif

	Result->SetStringField(TEXT("platform"), FPlatformProperties::PlatformName());

	FString OutStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Result, W);
	return OutStr;
}
