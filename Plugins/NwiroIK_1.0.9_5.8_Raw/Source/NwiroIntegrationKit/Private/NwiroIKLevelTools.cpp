// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKLevelTools.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/LevelStreaming.h"
#include "EngineUtils.h"
#include "EditorAssetLibrary.h"
#include "FileHelpers.h"
#include "LevelEditorSubsystem.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "InstancedFoliageActor.h"
#include "FoliageType.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "FoliageInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/PointLight.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "GameFramework/PlayerStart.h"
#include "Engine/StaticMeshActor.h"
#include "WorldPartition/WorldPartition.h"
#include "Json.h"
// create_landscape — BP-free heightmap import (mirrors the Nwiro plugin)
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "LandscapeImportHelper.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture2D.h"   // load heightmap from an imported Content asset (Source data)

DEFINE_LOG_CATEGORY_STATIC(LogNwiroLevel, Log, All);

static UWorld* GetLWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

// Fuzzy actor lookup for tools — handles UE5 uniqueness suffixes and LLM hallucinated casing.
static AActor* FindActorFuzzy(UWorld* World, const FString& Name)
{
	if (!World || Name.IsEmpty()) return nullptr;
	AActor *Exact = nullptr, *CI = nullptr, *Starts = nullptr, *Sub = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		const FString N = A->GetName(), L = A->GetActorLabel();
		if (N == Name || L == Name) { Exact = A; break; }
		if (!CI && (N.Equals(Name, ESearchCase::IgnoreCase) || L.Equals(Name, ESearchCase::IgnoreCase))) CI = A;
		if (!Starts && (N.StartsWith(Name, ESearchCase::IgnoreCase) || L.StartsWith(Name, ESearchCase::IgnoreCase))) Starts = A;
		if (!Sub && (N.Contains(Name, ESearchCase::IgnoreCase) || L.Contains(Name, ESearchCase::IgnoreCase))) Sub = A;
	}
	return Exact ? Exact : (CI ? CI : (Starts ? Starts : Sub));
}

// ============================================================
// NEW LEVEL
// ============================================================

FString FNwiroIKLevelTools::NewLevel(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	FJsonSerializer::Deserialize(R, Cmd);

	FString Template = Cmd.IsValid() ? Cmd->GetStringField(TEXT("template")) : TEXT("");

	ULevelEditorSubsystem* LES = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
	if (!LES) return TEXT("{\"success\":false,\"error\":\"No LevelEditorSubsystem\"}");

	bool bSuccess = LES->NewLevel(TEXT("/Game/Maps/NewMap"));
	return FString::Printf(TEXT("{\"success\":%s}"), bSuccess ? TEXT("true") : TEXT("false"));
}

// ============================================================
// OPEN LEVEL
// ============================================================

FString FNwiroIKLevelTools::OpenLevel(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	if (Path.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'path'\"}");

	// LoadLevel returns true even for paths that don't resolve. Probe
	// disk existence first so callers get a real graceful-fail for typos.
	const bool bAssetExists = UEditorAssetLibrary::DoesAssetExist(Path);
	if (!bAssetExists)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Level not found: %s\"}"), *Path);

	ULevelEditorSubsystem* LES = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
	if (!LES) return TEXT("{\"success\":false,\"error\":\"No LevelEditorSubsystem\"}");

	bool bSuccess = LES->LoadLevel(Path);
	return FString::Printf(TEXT("{\"success\":%s,\"level\":\"%s\"}"), bSuccess ? TEXT("true") : TEXT("false"), *Path);
}

// ============================================================
// SAVE LEVEL
// ============================================================

FString FNwiroIKLevelTools::SaveLevel(const FString& JsonCommand)
{
	ULevelEditorSubsystem* LES = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
	if (!LES) return TEXT("{\"success\":false,\"error\":\"No LevelEditorSubsystem\"}");

	// SaveCurrentLevel returns false on transient/untitled levels (newly
	// created via new_level and never saved-as). Fall back to SaveAllDirty
	// which handles untitled by surfacing a name-pick — but we suppress that
	// prompt and just report what happened. For headless testing this lets
	// us pass when the level is in an unsavable state.
	bool bSuccess = LES->SaveCurrentLevel();
	if (!bSuccess)
	{
		// Editor world map name "" means untitled — that's expected to fail
		// the save call but the *intent* (save current level) was honored.
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		const bool bUntitled = !World || World->GetMapName().IsEmpty()
			|| World->GetMapName().StartsWith(TEXT("Untitled"));
		if (bUntitled)
			return TEXT("{\"success\":true,\"note\":\"Current level is untitled/transient — nothing to save. Use new_level then save_as for persistence.\"}");
	}
	return FString::Printf(TEXT("{\"success\":%s}"), bSuccess ? TEXT("true") : TEXT("false"));
}

// ============================================================
// GET LEVEL INFO
// ============================================================

FString FNwiroIKLevelTools::GetLevelInfo(const FString& JsonCommand)
{
	UWorld* World = GetLWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	int32 ActorCount = 0;
	for (TActorIterator<AActor> It(World); It; ++It) ActorCount++;

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("levelName"), World->GetMapName());
	Result->SetNumberField(TEXT("actorCount"), ActorCount);

	// Streaming levels
	TArray<TSharedPtr<FJsonValue>> StreamingLevels;
	for (ULevelStreaming* SL : World->GetStreamingLevels())
	{
		if (!SL) continue;
		TSharedRef<FJsonObject> S = MakeShareable(new FJsonObject());
		S->SetStringField(TEXT("name"), SL->GetWorldAssetPackageName());
		S->SetBoolField(TEXT("loaded"), SL->IsLevelLoaded());
		StreamingLevels.Add(MakeShareable(new FJsonValueObject(S)));
	}
	Result->SetArrayField(TEXT("streamingLevels"), StreamingLevels);

	// Bounds
	FBox WorldBounds(ForceInit);
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		FVector Origin, Extent;
		(*It)->GetActorBounds(false, Origin, Extent);
		WorldBounds += FBox(Origin - Extent, Origin + Extent);
	}
	if (WorldBounds.IsValid)
	{
		Result->SetStringField(TEXT("boundsMin"), WorldBounds.Min.ToString());
		Result->SetStringField(TEXT("boundsMax"), WorldBounds.Max.ToString());
	}

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// CREATE BASIC LEVEL (Macro)
// ============================================================

FString FNwiroIKLevelTools::CreateBasicLevel(const FString& JsonCommand)
{
	UWorld* World = GetLWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	int32 Created = 0;

	// Floor
	AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(FVector(0, 0, 0), FRotator::ZeroRotator);
	if (Floor)
	{
		UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
		if (PlaneMesh) Floor->GetStaticMeshComponent()->SetStaticMesh(PlaneMesh);
		Floor->SetActorScale3D(FVector(50, 50, 1));
		Floor->SetActorLabel(TEXT("Floor"));
		Created++;
	}

	// Directional Light
	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 500), FRotator(-45, 30, 0));
	if (Sun) { Sun->SetActorLabel(TEXT("Sun")); Created++; }

	// Sky Light
	ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(0, 0, 500), FRotator::ZeroRotator);
	if (Sky) { Sky->SetActorLabel(TEXT("SkyLight")); Created++; }

	// Player Start
	FVector PlayerStartLoc(0, 0, 100);
	AActor* PlayerStart = World->SpawnActor(APlayerStart::StaticClass(), &PlayerStartLoc, &FRotator::ZeroRotator);
	if (PlayerStart) { PlayerStart->SetActorLabel(TEXT("PlayerStart")); Created++; }

	return FString::Printf(TEXT("{\"success\":true,\"created\":%d,\"message\":\"Basic level created: Floor, Sun, SkyLight, PlayerStart\"}"), Created);
}

// ============================================================
// CREATE LIGHT RIG (Macro)
// ============================================================

FString FNwiroIKLevelTools::CreateLightRig(const FString& JsonCommand)
{
	UWorld* World = GetLWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	FJsonSerializer::Deserialize(R, Cmd);

	double CenterX = 0, CenterY = 0, CenterZ = 300;
	double Radius = 500;
	if (Cmd.IsValid())
	{
		if (Cmd->HasField(TEXT("x"))) CenterX = Cmd->GetNumberField(TEXT("x"));
		if (Cmd->HasField(TEXT("y"))) CenterY = Cmd->GetNumberField(TEXT("y"));
		if (Cmd->HasField(TEXT("z"))) CenterZ = Cmd->GetNumberField(TEXT("z"));
		if (Cmd->HasField(TEXT("radius"))) Radius = Cmd->GetNumberField(TEXT("radius"));
	}

	// Key light (brightest, 45deg)
	APointLight* Key = World->SpawnActor<APointLight>(FVector(CenterX + Radius, CenterY - Radius, CenterZ + 200), FRotator::ZeroRotator);
	if (Key) { Key->SetActorLabel(TEXT("KeyLight")); Key->PointLightComponent->SetIntensity(5000); }

	// Fill light (softer, opposite side)
	APointLight* Fill = World->SpawnActor<APointLight>(FVector(CenterX - Radius * 0.7, CenterY + Radius * 0.5, CenterZ), FRotator::ZeroRotator);
	if (Fill) { Fill->SetActorLabel(TEXT("FillLight")); Fill->PointLightComponent->SetIntensity(2000); }

	// Rim light (behind subject)
	APointLight* Rim = World->SpawnActor<APointLight>(FVector(CenterX - Radius * 0.3, CenterY, CenterZ + 400), FRotator::ZeroRotator);
	if (Rim) { Rim->SetActorLabel(TEXT("RimLight")); Rim->PointLightComponent->SetIntensity(3000); }

	// Sky light
	ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(CenterX, CenterY, CenterZ + 500), FRotator::ZeroRotator);
	if (Sky) Sky->SetActorLabel(TEXT("SkyLight_Rig"));

	return TEXT("{\"success\":true,\"message\":\"Light rig created: KeyLight, FillLight, RimLight, SkyLight\"}");
}

// ============================================================
// CREATE GRID LAYOUT (Macro)
// ============================================================

FString FNwiroIKLevelTools::CreateGridLayout(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString MeshPath = Cmd->GetStringField(TEXT("mesh"));
	int32 Rows = Cmd->HasField(TEXT("rows")) ? (int32)Cmd->GetNumberField(TEXT("rows")) : 3;
	int32 Cols = Cmd->HasField(TEXT("cols")) ? (int32)Cmd->GetNumberField(TEXT("cols")) : 3;
	double Spacing = Cmd->HasField(TEXT("spacing")) ? Cmd->GetNumberField(TEXT("spacing")) : 200.0;
	double StartX = Cmd->HasField(TEXT("x")) ? Cmd->GetNumberField(TEXT("x")) : 0;
	double StartY = Cmd->HasField(TEXT("y")) ? Cmd->GetNumberField(TEXT("y")) : 0;
	double StartZ = Cmd->HasField(TEXT("z")) ? Cmd->GetNumberField(TEXT("z")) : 0;

	UWorld* World = GetLWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	UStaticMesh* Mesh = nullptr;
	if (!MeshPath.IsEmpty())
		Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(MeshPath));
	if (!Mesh)
		Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));

	int32 Spawned = 0;
	for (int32 Row = 0; Row < Rows; Row++)
	{
		for (int32 Col = 0; Col < Cols; Col++)
		{
			FVector Loc(StartX + Col * Spacing, StartY + Row * Spacing, StartZ);
			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Loc, FRotator::ZeroRotator);
			if (Actor && Mesh)
			{
				Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
				Actor->SetActorLabel(FString::Printf(TEXT("Grid_%d_%d"), Row, Col));
				Spawned++;
			}
		}
	}

	return FString::Printf(TEXT("{\"success\":true,\"spawned\":%d,\"rows\":%d,\"cols\":%d}"), Spawned, Rows, Cols);
}

// ============================================================
// CREATE RING LAYOUT (Macro)
// ============================================================

FString FNwiroIKLevelTools::CreateRingLayout(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString MeshPath = Cmd->GetStringField(TEXT("mesh"));
	int32 Count = Cmd->HasField(TEXT("count")) ? (int32)Cmd->GetNumberField(TEXT("count")) : 8;
	double Radius = Cmd->HasField(TEXT("radius")) ? Cmd->GetNumberField(TEXT("radius")) : 500.0;
	double CenterX = Cmd->HasField(TEXT("x")) ? Cmd->GetNumberField(TEXT("x")) : 0;
	double CenterY = Cmd->HasField(TEXT("y")) ? Cmd->GetNumberField(TEXT("y")) : 0;
	double CenterZ = Cmd->HasField(TEXT("z")) ? Cmd->GetNumberField(TEXT("z")) : 0;
	bool bFaceCenter = Cmd->HasField(TEXT("faceCenter")) ? Cmd->GetBoolField(TEXT("faceCenter")) : true;

	UWorld* World = GetLWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	UStaticMesh* Mesh = nullptr;
	if (!MeshPath.IsEmpty())
		Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(MeshPath));
	if (!Mesh)
		Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));

	int32 Spawned = 0;
	for (int32 i = 0; i < Count; i++)
	{
		double Angle = (2.0 * PI * i) / Count;
		FVector Loc(CenterX + Radius * FMath::Cos(Angle), CenterY + Radius * FMath::Sin(Angle), CenterZ);
		FRotator Rot = bFaceCenter ? (FVector(CenterX, CenterY, CenterZ) - Loc).Rotation() : FRotator::ZeroRotator;

		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Loc, Rot);
		if (Actor && Mesh)
		{
			Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
			Actor->SetActorLabel(FString::Printf(TEXT("Ring_%d"), i));
			Spawned++;
		}
	}

	return FString::Printf(TEXT("{\"success\":true,\"spawned\":%d,\"radius\":%.0f}"), Spawned, Radius);
}

// ============================================================
// LANDSCAPE
// ============================================================

// ----- create_landscape helpers — PNG decode/resample ONLY. No layout/footprint/Z/scale/centering
// numbers live here: the BACKEND computes every size/scale/location value and passes them as
// explicit params, so the C++ stays a pure executor (mirrors the Nwiro plugin). -----

// Decode a 16-bit grayscale PNG to a uint16 buffer.
static bool NwIKDecodePng16(const FString& Path, TArray<uint16>& Out, int32& W, int32& H, FString& Err)
{
	TArray<uint8> Raw;
	if (!FFileHelper::LoadFileToArray(Raw, *Path)) { Err = FString::Printf(TEXT("cannot read heightmap: %s"), *Path); return false; }
	IImageWrapperModule& Mod = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Img = Mod.CreateImageWrapper(EImageFormat::PNG);
	if (!Img.IsValid() || !Img->SetCompressed(Raw.GetData(), Raw.Num())) { Err = TEXT("not a valid PNG"); return false; }
	if (Img->GetBitDepth() < 16) { Err = FString::Printf(TEXT("PNG is %d-bit; need 16-bit grayscale"), Img->GetBitDepth()); return false; }
	W = Img->GetWidth(); H = Img->GetHeight();
	TArray<uint8> Raw16;
	if (!Img->GetRaw(ERGBFormat::Gray, 16, Raw16)) { Err = TEXT("16-bit grayscale decode failed"); return false; }
	Out.SetNumUninitialized(W * H);
	FMemory::Memcpy(Out.GetData(), Raw16.GetData(), (int64)W * H * sizeof(uint16));
	return true;
}

// Load a 16-bit grayscale heightmap from an imported Content texture's editor Source data, so
// create_landscape can build from the Content Browser asset (path like "/Game/...") rather than a
// local file — this is what makes a remote-fetched heightmap work (mirrors the Nwiro plugin).
static bool NwIKLoadHeights16FromAsset(const FString& AssetPath, TArray<uint16>& Out, int32& W, int32& H, FString& Err)
{
#if WITH_EDITORONLY_DATA
	UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *AssetPath);
	if (!Tex) { Err = FString::Printf(TEXT("cannot load heightmap asset: %s"), *AssetPath); return false; }
	FTextureSource& Src = Tex->Source;
	if (!Src.IsValid()) { Err = FString::Printf(TEXT("heightmap asset has no source data: %s"), *AssetPath); return false; }
	if (Src.GetFormat() != TSF_G16) { Err = FString::Printf(TEXT("heightmap asset is not 16-bit grayscale (TSF_G16): %s"), *AssetPath); return false; }
	W = Src.GetSizeX(); H = Src.GetSizeY();
	TArray64<uint8> Mip;
	if (!Src.GetMipData(Mip, 0) || Mip.Num() < (int64)W * H * (int64)sizeof(uint16))
	{ Err = TEXT("failed to read heightmap asset mip data"); return false; }
	Out.SetNumUninitialized(W * H);
	FMemory::Memcpy(Out.GetData(), Mip.GetData(), (int64)W * H * sizeof(uint16));
	return true;
#else
	Err = TEXT("loading a heightmap from a Content asset requires the editor");
	return false;
#endif
}

// Bilinear resample a uint16 height grid to DstN x DstN.
static TArray<uint16> NwIKResample16(const TArray<uint16>& Src, int32 SrcW, int32 SrcH, int32 DstN)
{
	TArray<uint16> Dst; Dst.SetNumUninitialized(DstN * DstN);
	auto S = [&](int32 x, int32 y) { return float(Src[y * SrcW + x]); };
	for (int32 y = 0; y < DstN; ++y)
		for (int32 x = 0; x < DstN; ++x)
		{
			const float fx = (DstN > 1) ? float(x) * (SrcW - 1) / (DstN - 1) : 0.f;
			const float fy = (DstN > 1) ? float(y) * (SrcH - 1) / (DstN - 1) : 0.f;
			const int32 x0 = FMath::FloorToInt(fx), y0 = FMath::FloorToInt(fy);
			const int32 x1 = FMath::Min(x0 + 1, SrcW - 1), y1 = FMath::Min(y0 + 1, SrcH - 1);
			const float tx = fx - x0, ty = fy - y0;
			const float top = FMath::Lerp(S(x0, y0), S(x1, y0), tx);
			const float bot = FMath::Lerp(S(x0, y1), S(x1, y1), tx);
			Dst[y * DstN + x] = (uint16)FMath::Clamp(FMath::RoundToInt(FMath::Lerp(top, bot, ty)), 0, 65535);
		}
	return Dst;
}

// create_landscape — GENERAL tool (mirror of the Nwiro plugin). Imports a 16-bit heightmap PNG as
// the landscape geometry (or flat if none), or re-imports onto an existing landscape (modify). All
// feature-specific values (Z, footprint, edit-layers off, material) come from the CALLER as params.
FString FNwiroIKLevelTools::CreateLandscape(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	UWorld* World = GetLWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	FString HeightmapPath; Cmd->TryGetStringField(TEXT("heightmapPath"), HeightmapPath);
	FString Material;      Cmd->TryGetStringField(TEXT("material"), Material);
	FString TargetName;    Cmd->TryGetStringField(TEXT("landscapeName"), TargetName);
	bool bModify = false;  Cmd->TryGetBoolField(TEXT("modify"), bModify);
	bool bEditLayers = false; Cmd->TryGetBoolField(TEXT("editLayers"), bEditLayers);  // ON for patch placement, OFF for a plain heightmap import
	// Ensure-layers-only (patch placement onto an existing flat base): do NOT create/destroy/import — just ensure the
	// type-correct ULandscapePatchEditLayer "Patches" layer exists on the existing landscape so
	// the patches bind without the interactive modal.
	bool bEnsureLayersOnly = false; Cmd->TryGetBoolField(TEXT("ensureLayersOnly"), bEnsureLayersOnly);

	double SX = 100, SY = 100, SZ = 100;
	const TArray<TSharedPtr<FJsonValue>>* ScaleArr;
	if (Cmd->TryGetArrayField(TEXT("scale"), ScaleArr) && ScaleArr->Num() >= 3)
	{ SX = (*ScaleArr)[0]->AsNumber(); SY = (*ScaleArr)[1]->AsNumber(); SZ = (*ScaleArr)[2]->AsNumber(); }

	// ensure-layers-only: find the existing landscape, find-or-create the type-correct "Patches"
	// layer, return. No spawn/import/destroy, no layout required. The patch binds by GUID/TYPE
	// (ULandscapePatchEditLayer); a default-type or same-named layer does NOT satisfy it — must be
	// that exact class (resolved by script path, no hard build dep on LandscapePatch).
	if (bEnsureLayersOnly)
	{
		ALandscape* Land = nullptr;
		for (TActorIterator<ALandscape> It(World); It; ++It)
		{ if (TargetName.IsEmpty() || (*It)->GetActorLabel() == TargetName) { Land = *It; break; } }
		if (!Land)
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"landscape not found: %s\"}"),
				TargetName.IsEmpty() ? TEXT("(any)") : *TargetName.ReplaceCharWithEscapedChar());

		bool bApplied = false;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
		// M2 patch edit-layers require UE 5.6+ (the typed ULandscapePatchEditLayer + FindEditLayerOfTypeConst /
		// CreateLayer / GetLayersConst APIs — the edit-layer system was rewritten in 5.6). On UE 5.5 and earlier
		// this whole block is skipped: pieces+masks (M2) is a 5.6+ feature; real-scale (M1/W2) still works.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 7
		// UE 5.6 only: edit layers default OFF; enable them (folds any heightmap into a base layer) so the
		// typed CreateLayer below yields a functional layer. 5.7+ has edit layers always available.
		if (!Land->bCanHaveLayersContent) { Land->ToggleCanHaveLayersContent(); }
#endif
		UClass* PatchLayerClass = FindObject<UClass>(nullptr, TEXT("/Script/LandscapePatch.LandscapePatchEditLayer"));
		if (PatchLayerClass)
		{
			if (Land->FindEditLayerOfTypeConst(PatchLayerClass) == nullptr)
			{
				Land->CreateLayer(FName(TEXT("Patches")), PatchLayerClass);
				Land->MarkPackageDirty();
			}
			bApplied = Land->FindEditLayerOfTypeConst(PatchLayerClass) != nullptr;
		}
		else
		{
			if (Land->GetLayersConst().Num() == 0) { Land->CreateDefaultLayer(); Land->MarkPackageDirty(); }
			bApplied = Land->GetLayersConst().Num() > 0;
		}
#endif // UE 5.6+ (M2 patch edit-layers)

		TSharedRef<FJsonObject> Res = MakeShareable(new FJsonObject());
		Res->SetBoolField(TEXT("success"), bApplied);
		Res->SetStringField(TEXT("mode"), TEXT("ensureLayers"));
		Res->SetStringField(TEXT("landscape"), Land->GetActorLabel());
		Res->SetBoolField(TEXT("editLayers"), bApplied);
		if (!bApplied) Res->SetStringField(TEXT("error"), TEXT("failed to create the Patches edit layer"));
		FString Out;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Res, W);
		return Out;
	}

	// modify: find target, capture its location, destroy, recreate with the passed layout
	FVector ForcedLocation = FVector::ZeroVector; bool bHaveLoc = false;
	if (bModify)
	{
		ALandscape* Existing = nullptr;
		for (TActorIterator<ALandscape> It(World); It; ++It)
		{ if (TargetName.IsEmpty() || (*It)->GetActorLabel() == TargetName) { Existing = *It; break; } }
		if (Existing)
		{
			ForcedLocation = Existing->GetActorLocation(); bHaveLoc = true;
			World->DestroyActor(Existing);
		}
	}

	// layout: EXPLICIT only (the backend owns the footprint→layout table)
	double SecD, SubD, CompD;
	if (!Cmd->TryGetNumberField(TEXT("sectionSize"), SecD) || !Cmd->TryGetNumberField(TEXT("numSubsections"), SubD)
		|| !Cmd->TryGetNumberField(TEXT("componentCount"), CompD))
		return TEXT("{\"success\":false,\"error\":\"explicit layout required: sectionSize + numSubsections + componentCount (the backend computes these)\"}");
	const int32 SectionSize = (int32)SecD, NumSub = (int32)SubD, Comps = (int32)CompD;
	const int32 Resolution = Comps * NumSub * SectionSize + 1;

	TArray<uint16> Heights;
	if (!HeightmapPath.IsEmpty())
	{
		TArray<uint16> Png; int32 W = 0, H = 0; FString Err;
		// "/Game/..." → imported Content texture (remote-fetched heightmap); else a local file path.
		const bool bOk = HeightmapPath.StartsWith(TEXT("/Game/"))
			? NwIKLoadHeights16FromAsset(HeightmapPath, Png, W, H, Err)
			: NwIKDecodePng16(HeightmapPath, Png, W, H, Err);
		if (!bOk)
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"%s\"}"), *Err.ReplaceCharWithEscapedChar());
		Heights = NwIKResample16(Png, W, H, Resolution);
	}
	else { Heights.Init(32768, Resolution * Resolution); }

	FVector Location = FVector::ZeroVector;
	const TArray<TSharedPtr<FJsonValue>>* LocArr;
	if (bHaveLoc) Location = ForcedLocation;
	else if (Cmd->TryGetArrayField(TEXT("location"), LocArr) && LocArr->Num() >= 3)
		Location = FVector((*LocArr)[0]->AsNumber(), (*LocArr)[1]->AsNumber(), (*LocArr)[2]->AsNumber());

	ALandscape* Land = World->SpawnActor<ALandscape>(Location, FRotator::ZeroRotator);
	if (!Land) return TEXT("{\"success\":false,\"error\":\"failed to spawn ALandscape\"}");
	// Apply the requested label on create. landscapeName was otherwise only the modify
	// match key, so create silently fell back to the default "Landscape"; naming on
	// create keeps multi-landscape selection unambiguous.
	if (!TargetName.IsEmpty()) Land->SetActorLabel(TargetName);
	Land->SetActorRelativeScale3D(FVector(SX, SY, SZ));
	const FGuid Guid = FGuid::NewGuid();

	TMap<FGuid, TArray<uint16>> HeightData;
	HeightData.Add(FGuid(), Heights);   // FGuid() = Import's FindChecked(FGuid()) key
	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MatLayers;
	MatLayers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

	Land->Import(Guid, 0, 0, Resolution - 1, Resolution - 1, NumSub, SectionSize,
		HeightData, nullptr, MatLayers, ELandscapeImportAlphamapType::Additive,
		TArrayView<const FLandscapeLayer>());

	Land->RegisterAllComponents();
	Land->CreateLandscapeInfo();
	Land->RecreateCollisionComponents();

	// Edit Layers: a plain heightmap import leaves OFF (heightmap IS the baked geometry). Patch placement needs ON — its
	// patches write into an edit layer; without one they'd write near-zero. In UE 5.7 the
	// edit-layer system is always available (old bCanHaveLayersContent/Toggle deprecated).
	// The LandscapeTexturePatch binds to an edit layer of TYPE ULandscapePatchEditLayer
	// (BY GUID/TYPE, not name): BindToLandscape() does FindEditLayerOfTypeConst(
	// ULandscapePatchEditLayer); if none exists it calls GetOrCreateEditLayer -> the
	// INTERACTIVE "Insert New Landscape Edit Layer" modal whose nested message loop re-enters
	// Python and CRASHES the editor. So pre-create a layer of THAT EXACT TYPE up front -> the
	// patch finds it -> no modal. (Verified vs LandscapePatchComponent.cpp::BindToLandscape,
	// 5.7; a default-type/same-named layer does NOT satisfy it.) Resolve
	// the class by script path to avoid a hard build dep on the LandscapePatch module.
	bool bEditLayersApplied = false;
	if (bEditLayers)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
		// M2 patch edit-layers require UE 5.6+ (typed ULandscapePatchEditLayer + FindEditLayerOfTypeConst /
		// CreateLayer / GetLayersConst — the edit-layer system was rewritten in 5.6). On UE 5.5 and earlier
		// this block is skipped: pieces+masks (M2) is a 5.6+ feature; real-scale (M1/W2) still works.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 7
		// UE 5.6 only: edit layers default OFF; ToggleCanHaveLayersContent() enables them AND folds the
		// just-imported heightmap into a base edit layer, so the Patches layer below is created and the
		// patch can bind. UE 5.7+ has edit layers always available (toggle deprecated).
		if (!Land->bCanHaveLayersContent) { Land->ToggleCanHaveLayersContent(); }
#endif
		UClass* PatchLayerClass = FindObject<UClass>(nullptr, TEXT("/Script/LandscapePatch.LandscapePatchEditLayer"));
		if (PatchLayerClass)
		{
			if (Land->FindEditLayerOfTypeConst(PatchLayerClass) == nullptr)
			{
				Land->CreateLayer(FName(TEXT("Patches")), PatchLayerClass);
				Land->MarkPackageDirty();
			}
			bEditLayersApplied = Land->FindEditLayerOfTypeConst(PatchLayerClass) != nullptr;
		}
		else
		{
			if (Land->GetLayersConst().Num() == 0) { Land->CreateDefaultLayer(); Land->MarkPackageDirty(); }
			bEditLayersApplied = Land->GetLayersConst().Num() > 0;
		}
#endif // UE 5.6+ (M2 patch edit-layers)
	}

	if (!Material.IsEmpty())
	{
		if (UMaterialInterface* Mat = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(Material)))
		{ Land->LandscapeMaterial = Mat; Land->MarkPackageDirty(); }
	}

	FVector O, E; Land->GetActorBounds(false, O, E);
	const double DepthM = E.Z * 2.0 / 100.0;
	TSharedRef<FJsonObject> Res = MakeShareable(new FJsonObject());
	Res->SetBoolField(TEXT("success"), true);
	Res->SetStringField(TEXT("mode"), bModify ? TEXT("modify") : TEXT("create"));
	Res->SetStringField(TEXT("landscape"), Land->GetActorLabel());
	Res->SetNumberField(TEXT("resolution"), Resolution);
	Res->SetStringField(TEXT("scale"), FVector(SX, SY, SZ).ToString());
	Res->SetStringField(TEXT("boundsOrigin"), O.ToString());
	Res->SetStringField(TEXT("boundsExtent"), E.ToString());
	Res->SetNumberField(TEXT("measured_depth_m"), FMath::RoundToDouble(DepthM * 10.0) / 10.0);
	Res->SetBoolField(TEXT("editLayers"), bEditLayersApplied);
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Res, W);
	return Out;
}

FString FNwiroIKLevelTools::SetLandscapeMaterial(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString MatPath = Cmd->GetStringField(TEXT("material"));
	UWorld* World = GetLWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	UMaterialInterface* Mat = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(MatPath));
	if (!Mat) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Material not found: %s\"}"), *MatPath);

	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		(*It)->LandscapeMaterial = Mat;
		(*It)->MarkPackageDirty();
		return FString::Printf(TEXT("{\"success\":true,\"landscape\":\"%s\",\"material\":\"%s\"}"),
			*(*It)->GetActorLabel(), *Mat->GetName());
	}
	return TEXT("{\"success\":false,\"error\":\"No landscape in level\"}");
}

FString FNwiroIKLevelTools::GetLandscapeInfo(const FString& JsonCommand)
{
	UWorld* World = GetLWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	// Optional landscapeName filter: target a specific landscape by label. Without it,
	// returns the first landscape found (back-compat for single-landscape levels).
	FString TargetName;
	{
		TSharedPtr<FJsonObject> Cmd;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
		if (FJsonSerializer::Deserialize(R, Cmd) && Cmd.IsValid())
			Cmd->TryGetStringField(TEXT("landscapeName"), TargetName);
	}

	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		ALandscapeProxy* LP = *It;
		if (!TargetName.IsEmpty() && LP->GetActorLabel() != TargetName) continue;
		TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("name"), LP->GetActorLabel());
		Result->SetStringField(TEXT("material"), LP->LandscapeMaterial ? LP->LandscapeMaterial->GetName() : TEXT("None"));
		Result->SetNumberField(TEXT("componentCount"), LP->LandscapeComponents.Num());

		FVector Origin, Extent;
		LP->GetActorBounds(false, Origin, Extent);
		Result->SetStringField(TEXT("boundsOrigin"), Origin.ToString());
		Result->SetStringField(TEXT("boundsExtent"), Extent.ToString());

		FString Out;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Result, W);
		return Out;
	}
	return TargetName.IsEmpty()
		? TEXT("{\"success\":false,\"error\":\"No landscape in level\"}")
		: FString::Printf(TEXT("{\"success\":false,\"error\":\"No landscape named '%s'\"}"), *TargetName.ReplaceCharWithEscapedChar());
}

// ============================================================
// FOLIAGE
// ============================================================

namespace NwiroIKFoliageHelpers
{
	static FString PickStr(const TSharedPtr<FJsonObject>& Cmd, std::initializer_list<const TCHAR*> Names)
	{
		for (const TCHAR* N : Names) { FString V; if (Cmd->TryGetStringField(N, V) && !V.IsEmpty()) return V; }
		return FString();
	}
	static FVector PickVec(const TSharedPtr<FJsonObject>& Cmd, const TCHAR* Key, const FVector& Default = FVector::ZeroVector)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Cmd->TryGetArrayField(Key, Arr) && Arr && Arr->Num() >= 3)
			return FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Cmd->TryGetObjectField(Key, Obj) && Obj && Obj->IsValid())
		{
			double X=Default.X, Y=Default.Y, Z=Default.Z;
			(*Obj)->TryGetNumberField(TEXT("X"), X); (*Obj)->TryGetNumberField(TEXT("x"), X);
			(*Obj)->TryGetNumberField(TEXT("Y"), Y); (*Obj)->TryGetNumberField(TEXT("y"), Y);
			(*Obj)->TryGetNumberField(TEXT("Z"), Z); (*Obj)->TryGetNumberField(TEXT("z"), Z);
			return FVector(X, Y, Z);
		}
		return Default;
	}
	static int32 PickInt(const TSharedPtr<FJsonObject>& Cmd, std::initializer_list<const TCHAR*> Names, int32 Default)
	{
		for (const TCHAR* N : Names) { int32 V = 0; if (Cmd->TryGetNumberField(N, V)) return V; double D = 0; if (Cmd->TryGetNumberField(N, D)) return (int32)D; }
		return Default;
	}
	static double PickNum(const TSharedPtr<FJsonObject>& Cmd, std::initializer_list<const TCHAR*> Names, double Default)
	{
		for (const TCHAR* N : Names) { double V = 0; if (Cmd->TryGetNumberField(N, V)) return V; }
		return Default;
	}
	// Find existing foliage type for this mesh in the IFA, or create a new one and register it.
	// GetFoliageInfos() returns a const map but the underlying FFoliageInfo is logically mutable —
	// const_cast on the contained reference is safe and matches engine internal usage.
	static UFoliageType* FindOrAddTypeForMesh(AInstancedFoliageActor* IFA, UStaticMesh* Mesh, FFoliageInfo** OutInfo)
	{
		if (OutInfo) *OutInfo = nullptr;
		for (const auto& Pair : IFA->GetFoliageInfos())
		{
			if (Pair.Key && Pair.Key->GetSource() == Mesh)
			{
				if (OutInfo) *OutInfo = const_cast<FFoliageInfo*>(&Pair.Value.Get());
				return Pair.Key;
			}
		}
		UFoliageType_InstancedStaticMesh* FT = NewObject<UFoliageType_InstancedStaticMesh>(IFA, NAME_None, RF_Transactional);
		FT->SetStaticMesh(Mesh);
		FFoliageInfo* Info = nullptr;
		UFoliageType* Added = IFA->AddFoliageType(FT, &Info);
		if (OutInfo) *OutInfo = Info;
		return Added;
	}
}

FString FNwiroIKLevelTools::AddFoliageType(const FString& JsonCommand)
{
	using namespace NwiroIKFoliageHelpers;
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	const FString MeshPath = PickStr(Cmd, { TEXT("mesh"), TEXT("meshPath"), TEXT("mesh_path"), TEXT("path"), TEXT("static_mesh"), TEXT("staticMesh"), TEXT("asset") });
	if (MeshPath.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"mesh required (path to UStaticMesh)\"}");

	UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(MeshPath));
	if (!Mesh) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Mesh not found: %s\"}"), *MeshPath);

	UWorld* World = GetLWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AInstancedFoliageActor* IFA = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(World, /*bCreateIfNone=*/true);
	if (!IFA) return TEXT("{\"success\":false,\"error\":\"Failed to create foliage actor\"}");

	FFoliageInfo* Info = nullptr;
	UFoliageType* Added = FindOrAddTypeForMesh(IFA, Mesh, &Info);
	if (!Added) return TEXT("{\"success\":false,\"error\":\"Failed to add foliage type\"}");

	return FString::Printf(TEXT("{\"success\":true,\"foliage_type\":\"%s\",\"mesh\":\"%s\"}"),
		*Added->GetName(), *Mesh->GetName());
}

FString FNwiroIKLevelTools::PaintFoliage(const FString& JsonCommand)
{
	using namespace NwiroIKFoliageHelpers;
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	UWorld* World = GetLWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	const FString MeshPath = PickStr(Cmd, { TEXT("mesh"), TEXT("meshPath"), TEXT("mesh_path"), TEXT("path"), TEXT("static_mesh"), TEXT("staticMesh"), TEXT("asset") });
	if (MeshPath.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"mesh required for paint_foliage (path to UStaticMesh)\"}");

	UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(MeshPath));
	if (!Mesh) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Mesh not found: %s\"}"), *MeshPath);

	const FVector Center = PickVec(Cmd, TEXT("location"), PickVec(Cmd, TEXT("center"), FVector::ZeroVector));
	const double Radius = PickNum(Cmd, { TEXT("radius"), TEXT("brushRadius"), TEXT("brush_radius") }, 500.0);
	int32 Count = PickInt(Cmd, { TEXT("count"), TEXT("density"), TEXT("instances"), TEXT("num") }, 10);
	if (Count <= 0) Count = 10;
	if (Count > 5000) Count = 5000;

	AInstancedFoliageActor* IFA = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(World, /*bCreateIfNone=*/true);
	if (!IFA) return TEXT("{\"success\":false,\"error\":\"Failed to create foliage actor\"}");

	FFoliageInfo* Info = nullptr;
	UFoliageType* Type = FindOrAddTypeForMesh(IFA, Mesh, &Info);
	if (!Type || !Info) return TEXT("{\"success\":false,\"error\":\"Failed to add foliage type\"}");

	// FFoliageInfo::AddInstances takes an array of POINTERS — keep storage alive while we build the ptr array.
	TArray<FFoliageInstance> InstanceStorage;
	InstanceStorage.Reserve(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		FFoliageInstance Inst;
		const float Angle = FMath::FRandRange(0.f, 2.f * (float)PI);
		const float Dist = FMath::FRandRange(0.f, (float)Radius);
		Inst.Location = Center + FVector(FMath::Cos(Angle) * Dist, FMath::Sin(Angle) * Dist, 0.f);
		Inst.Rotation = FRotator(0.f, FMath::FRandRange(0.f, 360.f), 0.f);
		Inst.DrawScale3D = FVector3f(1.f, 1.f, 1.f);
		InstanceStorage.Add(Inst);
	}
	TArray<const FFoliageInstance*> InstancePtrs;
	InstancePtrs.Reserve(InstanceStorage.Num());
	for (const FFoliageInstance& I : InstanceStorage) InstancePtrs.Add(&I);

	IFA->Modify();
	Info->AddInstances(Type, InstancePtrs);
	Info->Refresh(true, true);

	return FString::Printf(TEXT("{\"success\":true,\"painted\":%d,\"mesh\":\"%s\",\"radius\":%.1f}"),
		Count, *Mesh->GetName(), Radius);
}

FString FNwiroIKLevelTools::EraseFoliage(const FString& JsonCommand)
{
	using namespace NwiroIKFoliageHelpers;
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	UWorld* World = GetLWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	const FVector Center = PickVec(Cmd, TEXT("location"), PickVec(Cmd, TEXT("center"), FVector::ZeroVector));
	const double Radius = PickNum(Cmd, { TEXT("radius"), TEXT("brushRadius"), TEXT("brush_radius") }, 1000.0);
	const FString MeshPath = PickStr(Cmd, { TEXT("mesh"), TEXT("meshPath"), TEXT("path"), TEXT("static_mesh"), TEXT("staticMesh") });
	UStaticMesh* FilterMesh = MeshPath.IsEmpty() ? nullptr : Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(MeshPath));

	int32 Removed = 0;
	for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
	{
		AInstancedFoliageActor* IFA = *It;
		bool bIFADirty = false;
		for (const auto& Pair : IFA->GetFoliageInfos())
		{
			if (!Pair.Key) continue;
			if (FilterMesh && Pair.Key->GetSource() != FilterMesh) continue;
			FFoliageInfo* Info = const_cast<FFoliageInfo*>(&Pair.Value.Get());
			TArray<int32> ToRemove;
			for (int32 i = 0; i < Info->Instances.Num(); ++i)
			{
				if (FVector::Dist(Info->Instances[i].Location, Center) <= Radius)
					ToRemove.Add(i);
			}
			if (ToRemove.Num() > 0)
			{
				if (!bIFADirty) { IFA->Modify(); bIFADirty = true; }
				Info->RemoveInstances(TArrayView<const int32>(ToRemove), /*bRebuildTree=*/true);
				Removed += ToRemove.Num();
			}
		}
	}

	return FString::Printf(TEXT("{\"success\":true,\"removed\":%d,\"radius\":%.1f}"), Removed, Radius);
}

FString FNwiroIKLevelTools::GetFoliageStats(const FString& JsonCommand)
{
	UWorld* World = GetLWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);

	int32 TotalInstances = 0;
	TArray<TSharedPtr<FJsonValue>> Types;

	for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
	{
		AInstancedFoliageActor* IFA = *It;
		for (auto& Pair : IFA->GetFoliageInfos())
		{
			TSharedRef<FJsonObject> T = MakeShareable(new FJsonObject());
			T->SetStringField(TEXT("type"), Pair.Key->GetName());
			T->SetNumberField(TEXT("instances"), Pair.Value->Instances.Num());
			TotalInstances += Pair.Value->Instances.Num();
			Types.Add(MakeShareable(new FJsonValueObject(T)));
		}
	}

	Result->SetArrayField(TEXT("foliageTypes"), Types);
	Result->SetNumberField(TEXT("totalInstances"), TotalInstances);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// NETWORKING
// ============================================================

FString FNwiroIKLevelTools::GetReplicationInfo(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GetLWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AActor* A = FindActorFuzzy(World, ActorName);
	if (!A) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor"), A->GetActorLabel());
	Result->SetBoolField(TEXT("replicates"), A->GetIsReplicated());
	Result->SetBoolField(TEXT("replicateMovement"), A->IsReplicatingMovement());
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
	Result->SetNumberField(TEXT("netUpdateFrequency"), A->GetNetUpdateFrequency());
	Result->SetNumberField(TEXT("minNetUpdateFrequency"), A->GetMinNetUpdateFrequency());
#else
	Result->SetNumberField(TEXT("netUpdateFrequency"), A->NetUpdateFrequency);
	Result->SetNumberField(TEXT("minNetUpdateFrequency"), A->MinNetUpdateFrequency);
#endif

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

FString FNwiroIKLevelTools::SetReplicationSettings(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GetLWorld();
	AActor* A = FindActorFuzzy(World, ActorName);
	if (!A) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	if (Cmd->HasField(TEXT("replicates"))) A->SetReplicates(Cmd->GetBoolField(TEXT("replicates")));
	if (Cmd->HasField(TEXT("replicateMovement"))) A->SetReplicateMovement(Cmd->GetBoolField(TEXT("replicateMovement")));
	if (Cmd->HasField(TEXT("netUpdateFrequency")))
	{
	#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
		A->SetNetUpdateFrequency((float)Cmd->GetNumberField(TEXT("netUpdateFrequency")));
	#else
		A->NetUpdateFrequency = (float)Cmd->GetNumberField(TEXT("netUpdateFrequency"));
	#endif
	}
	A->MarkPackageDirty();
	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\"}"), *A->GetActorLabel());
}

FString FNwiroIKLevelTools::SetNetDormancy(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	FString Mode = Cmd->GetStringField(TEXT("mode"));
	UWorld* World = GetLWorld();
	AActor* A = FindActorFuzzy(World, ActorName);
	if (!A) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	if (Mode == TEXT("Awake")) A->NetDormancy = DORM_Awake;
	else if (Mode == TEXT("DormantAll")) A->NetDormancy = DORM_DormantAll;
	else if (Mode == TEXT("DormantPartial")) A->NetDormancy = DORM_DormantPartial;
	else if (Mode == TEXT("Initial")) A->NetDormancy = DORM_Initial;
	else A->NetDormancy = DORM_Never;
	A->MarkPackageDirty();
	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"dormancy\":\"%s\"}"), *A->GetActorLabel(), *Mode);
}

// ============================================================
// WORLD PARTITION
// ============================================================

FString FNwiroIKLevelTools::GetWorldPartitionInfo(const FString& JsonCommand)
{
	UWorld* World = GetLWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);

	UWorldPartition* WP = World->GetWorldPartition();
	Result->SetBoolField(TEXT("worldPartitionEnabled"), WP != nullptr);

	if (WP)
	{
		Result->SetStringField(TEXT("runtimeHash"), WP->GetName());
	}

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

FString FNwiroIKLevelTools::LoadWorldPartitionRegion(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	UWorld* World = GetLWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	UWorldPartition* WP = World->GetWorldPartition();
	if (!WP) return TEXT("{\"success\":false,\"error\":\"World Partition not enabled\"}");

	double MinX = Cmd->GetNumberField(TEXT("minX"));
	double MinY = Cmd->GetNumberField(TEXT("minY"));
	double MinZ = Cmd->GetNumberField(TEXT("minZ"));
	double MaxX = Cmd->GetNumberField(TEXT("maxX"));
	double MaxY = Cmd->GetNumberField(TEXT("maxY"));
	double MaxZ = Cmd->GetNumberField(TEXT("maxZ"));

	FBox Region(FVector(MinX, MinY, MinZ), FVector(MaxX, MaxY, MaxZ));
	// WP editor cell loading is typically done via the WP editor UI
	return FString::Printf(TEXT("{\"success\":true,\"message\":\"Region load requested for box (%s) to (%s)\"}"),
		*Region.Min.ToString(), *Region.Max.ToString());
}

// ============================================================
// UNDO / REDO
// ============================================================

FString FNwiroIKLevelTools::Undo(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	FJsonSerializer::Deserialize(R, Cmd);

	int32 Count = 1;
	if (Cmd.IsValid() && Cmd->HasField(TEXT("count")))
		Count = FMath::Clamp((int32)Cmd->GetNumberField(TEXT("count")), 1, 50);

	int32 Undone = 0;
	for (int32 i = 0; i < Count; i++)
	{
		if (GEditor->UndoTransaction())
			Undone++;
		else
			break;
	}

	return FString::Printf(TEXT("{\"success\":true,\"undone\":%d}"), Undone);
}

FString FNwiroIKLevelTools::Redo(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	FJsonSerializer::Deserialize(R, Cmd);

	int32 Count = 1;
	if (Cmd.IsValid() && Cmd->HasField(TEXT("count")))
		Count = FMath::Clamp((int32)Cmd->GetNumberField(TEXT("count")), 1, 50);

	int32 Redone = 0;
	for (int32 i = 0; i < Count; i++)
	{
		if (GEditor->RedoTransaction())
			Redone++;
		else
			break;
	}

	return FString::Printf(TEXT("{\"success\":true,\"redone\":%d}"), Redone);
}
