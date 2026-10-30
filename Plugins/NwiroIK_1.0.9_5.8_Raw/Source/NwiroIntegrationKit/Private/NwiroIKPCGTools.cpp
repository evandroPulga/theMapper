// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKPCGTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "PCGVolume.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroPCG, Log, All);

namespace
{
	UWorld* GetEditorWorldSafe()
	{
		if (!GEditor) return nullptr;
		return GEditor->GetEditorWorldContext().World();
	}

	FString JsonToString(const TSharedRef<FJsonObject>& Json)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Json, W);
		return Out;
	}

	TSharedRef<FJsonObject> Fail(const FString& Error)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetBoolField(TEXT("success"), false);
		J->SetStringField(TEXT("error"), Error);
		return J;
	}

	TSharedPtr<FJsonObject> ParseArgs(const FString& JsonCommand)
	{
		TSharedPtr<FJsonObject> Cmd;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
		FJsonSerializer::Deserialize(Reader, Cmd);
		return Cmd;
	}
}

// ============================================================
// CREATE PCG GRAPH
// ============================================================

FString FNwiroIKPCGTools::CreatePcgGraph(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd = ParseArgs(JsonCommand);
	if (!Cmd.IsValid())
	{
		return JsonToString(Fail(TEXT("Invalid JSON")));
	}

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->HasField(TEXT("path")) ? Cmd->GetStringField(TEXT("path")) : TEXT("/Game/PCG");
	if (Name.IsEmpty()) return JsonToString(Fail(TEXT("'name' is required")));

	// Strip .uasset / leading slashes
	Name.RemoveFromEnd(TEXT(".uasset"));
	if (!Path.StartsWith(TEXT("/"))) Path = TEXT("/") + Path;
	while (Path.EndsWith(TEXT("/"))) Path.RemoveAt(Path.Len() - 1);

	const FString PackagePath = Path + TEXT("/") + Name;

	// Bail if it already exists
	if (UObject* Existing = LoadObject<UPCGGraph>(nullptr, *PackagePath))
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetBoolField(TEXT("success"), true);
		J->SetBoolField(TEXT("alreadyExists"), true);
		J->SetStringField(TEXT("name"), Name);
		J->SetStringField(TEXT("path"), Existing->GetPathName());
		return JsonToString(J);
	}

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package) return JsonToString(Fail(TEXT("Failed to create package")));

	UPCGGraph* Graph = NewObject<UPCGGraph>(Package, *Name, RF_Public | RF_Standalone);
	if (!Graph) return JsonToString(Fail(TEXT("Failed to create UPCGGraph")));

	FAssetRegistryModule::AssetCreated(Graph);
	Graph->MarkPackageDirty();

	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetBoolField(TEXT("success"), true);
	J->SetStringField(TEXT("name"), Name);
	J->SetStringField(TEXT("path"), Graph->GetPathName());
	return JsonToString(J);
}

// ============================================================
// FIND PCG GRAPHS
// ============================================================

FString FNwiroIKPCGTools::FindPcgGraphs(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd = ParseArgs(JsonCommand);
	const FString Query = (Cmd.IsValid() && Cmd->HasField(TEXT("query"))) ? Cmd->GetStringField(TEXT("query")) : FString();

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	FARFilter Filter;
	Filter.ClassPaths.Add(UPCGGraph::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(TEXT("/Game"));

	TArray<FAssetData> Found;
	ARM.Get().GetAssets(Filter, Found);

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FAssetData& A : Found)
	{
		const FString Name = A.AssetName.ToString();
		if (!Query.IsEmpty() && !Name.Contains(Query, ESearchCase::IgnoreCase)) continue;

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("path"), A.GetObjectPathString());
		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("success"), true);
	Out->SetNumberField(TEXT("count"), Arr.Num());
	Out->SetArrayField(TEXT("graphs"), Arr);
	return JsonToString(Out);
}

// ============================================================
// SPAWN PCG VOLUME
// ============================================================

FString FNwiroIKPCGTools::SpawnPcgVolume(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd = ParseArgs(JsonCommand);
	if (!Cmd.IsValid()) return JsonToString(Fail(TEXT("Invalid JSON")));

	FString GraphPath = Cmd->GetStringField(TEXT("graph"));
	// Accept name/label/actor as the desired actor label — agents naturally
	// reach for "name" rather than the more obscure "label".
	FString Label;
	for (const TCHAR* K : { TEXT("label"), TEXT("name"), TEXT("actor") })
	{
		FString V; if (Cmd->TryGetStringField(K, V) && !V.IsEmpty()) { Label = V; break; }
	}
	if (Label.IsEmpty()) Label = TEXT("PCG_Volume");
	if (GraphPath.IsEmpty()) return JsonToString(Fail(TEXT("'graph' (PCG graph asset path) is required")));

	UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
	if (!Graph) return JsonToString(Fail(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath)));

	UWorld* World = GetEditorWorldSafe();
	if (!World) return JsonToString(Fail(TEXT("No editor world")));

	// Default location/scale; allow override via scalar fields OR
	// location:[x,y,z] array, OR size:N scalar (uniform box edge length cm,
	// converted to scale assuming default 100cm box).
	FVector Location(0, 0, 0);
	FVector Scale(20, 20, 5); // 2000x2000x500 cm box by default
	const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
	if (Cmd->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() >= 3)
		Location = FVector((*LocArr)[0]->AsNumber(), (*LocArr)[1]->AsNumber(), (*LocArr)[2]->AsNumber());
	if (Cmd->HasField(TEXT("x"))) Location.X = Cmd->GetNumberField(TEXT("x"));
	if (Cmd->HasField(TEXT("y"))) Location.Y = Cmd->GetNumberField(TEXT("y"));
	if (Cmd->HasField(TEXT("z"))) Location.Z = Cmd->GetNumberField(TEXT("z"));
	if (Cmd->HasField(TEXT("size")))
	{
		const double S = Cmd->GetNumberField(TEXT("size")) / 100.0;
		Scale = FVector(S, S, S);
	}
	if (Cmd->HasField(TEXT("scaleX"))) Scale.X = Cmd->GetNumberField(TEXT("scaleX"));
	if (Cmd->HasField(TEXT("scaleY"))) Scale.Y = Cmd->GetNumberField(TEXT("scaleY"));
	if (Cmd->HasField(TEXT("scaleZ"))) Scale.Z = Cmd->GetNumberField(TEXT("scaleZ"));

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	APCGVolume* Volume = World->SpawnActor<APCGVolume>(APCGVolume::StaticClass(), Location, FRotator::ZeroRotator, Params);
	if (!Volume) return JsonToString(Fail(TEXT("Failed to spawn APCGVolume")));

	Volume->SetActorScale3D(Scale);
	Volume->SetActorLabel(Label);

	if (UPCGComponent* Comp = Volume->FindComponentByClass<UPCGComponent>())
	{
		Comp->SetGraph(Graph);
		// Auto-generating on spawn crashed UE_PCG with ACCESS_VIOLATION when
		// the graph is empty (no SpawnPoints / sources wired). Defer the
		// generation — caller can use pcg_generate explicitly once the graph
		// is wired. Discovered by scenario/09-procedural-level: PCG crashed
		// during scenario "all" right after spawn returned success.
		// Comp->Generate();
	}

	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetBoolField(TEXT("success"), true);
	J->SetStringField(TEXT("actor"), Volume->GetActorLabel());
	J->SetStringField(TEXT("graph"), GraphPath);
	J->SetStringField(TEXT("location"), FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), Location.X, Location.Y, Location.Z));
	return JsonToString(J);
}

// ============================================================
// PCG GENERATE (force re-run on an existing volume)
// ============================================================

FString FNwiroIKPCGTools::PcgGenerate(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd = ParseArgs(JsonCommand);
	if (!Cmd.IsValid()) return JsonToString(Fail(TEXT("Invalid JSON")));

	// Hallucination-tolerant arg picker — accept actor/name/label/volume.
	FString TargetLabel;
	for (const TCHAR* K : { TEXT("actor"), TEXT("name"), TEXT("label"), TEXT("volume") })
	{
		FString V;
		if (Cmd->TryGetStringField(K, V) && !V.IsEmpty()) { TargetLabel = V; break; }
	}
	if (TargetLabel.IsEmpty()) return JsonToString(Fail(TEXT("'actor' (label of the PCG volume) is required (also tried name/label/volume)")));

	UWorld* World = GetEditorWorldSafe();
	if (!World) return JsonToString(Fail(TEXT("No editor world")));

	// Three-tier fuzzy match — exact label, then case-insensitive substring on
	// label, then on UE5 internal name (which gets auto-suffixed). The user
	// passes the label they intended at spawn, UE may add `_N`. Match anything
	// sensible.
	APCGVolume* Vol = nullptr;
	for (TActorIterator<APCGVolume> It(World); It; ++It)
	{
		APCGVolume* V = *It;
		if (V && V->GetActorLabel().Equals(TargetLabel, ESearchCase::IgnoreCase)) { Vol = V; break; }
	}
	if (!Vol)
	{
		for (TActorIterator<APCGVolume> It(World); It; ++It)
		{
			APCGVolume* V = *It;
			if (V && (V->GetActorLabel().Contains(TargetLabel, ESearchCase::IgnoreCase)
				|| V->GetName().Contains(TargetLabel, ESearchCase::IgnoreCase)))
			{ Vol = V; break; }
		}
	}

	int32 Hits = 0;
	FString SkipReason;
	if (Vol)
	{
		if (UPCGComponent* Comp = Vol->FindComponentByClass<UPCGComponent>())
		{
			// Guard: calling Generate() on an empty / unwired graph crashes
			// UE_PCG with ACCESS_VIOLATION. Verify the graph has at least one
			// node before triggering. Discovered by scenario/09 + sanity sweep.
			UPCGGraphInterface* GI = Comp->GetGraphInstance();
			UPCGGraph* G = GI ? GI->GetGraph() : nullptr;
			const bool bHasNodes = G && G->GetNodes().Num() > 0;
			if (!bHasNodes)
			{
				SkipReason = TEXT("PCGVolume's graph is empty (no nodes) — Generate would crash. Wire SpawnPoints / StaticMeshSpawner first via the PCG editor or execute_python.");
			}
			else
			{
				Comp->Generate();
				Hits++;
			}
		}
	}

	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetBoolField(TEXT("success"), Hits > 0);
	J->SetNumberField(TEXT("regenerated"), Hits);
	if (Hits == 0)
	{
		if (!SkipReason.IsEmpty()) J->SetStringField(TEXT("error"), SkipReason);
		else J->SetStringField(TEXT("error"), FString::Printf(TEXT("No PCGVolume with label '%s' (tried exact + substring on label + UE internal name)"), *TargetLabel));
	}
	else J->SetStringField(TEXT("matched"), Vol->GetActorLabel());
	return JsonToString(J);
}

// ============================================================
// ADD PCG NODE
// ============================================================
// Generic primitive: add a node of the given settings class to a PCG graph.
// The LLM passes a friendly name like "StaticMeshSpawner" / "SurfaceSampler"
// / "SelfPruning" and we resolve it to the right UPCGSettings subclass via
// class iteration. This is the missing piece scenario 09-procedural-level
// needs to actually populate a graph (otherwise pcg_generate has nothing to
// run and graceful-refuses).

static UClass* ResolvePcgSettingsClass(const FString& Requested)
{
	if (Requested.IsEmpty()) return nullptr;
	const FString WantLower = Requested.ToLower().Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT(""));

	// Try by exact class name first (case-insensitive), then by display name
	// containment, then by short-name containment. Settings classes typically
	// follow the pattern UPCGFooSettings — try with and without the prefix.
	UClass* BestMatch = nullptr;
	int32 BestScore = 0;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* C = *It;
		if (!C || !C->IsChildOf(UObject::StaticClass())) continue;
		const FString CN = C->GetName();
		if (!CN.StartsWith(TEXT("PCG"), ESearchCase::IgnoreCase)) continue;
		if (!CN.EndsWith(TEXT("Settings"), ESearchCase::IgnoreCase)) continue;
		if (C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;

		const FString CnLower = CN.ToLower();
		// Strip leading "pcg" and trailing "settings" for matching
		FString CnCore = CnLower;
		CnCore.RemoveFromStart(TEXT("pcg"));
		CnCore.RemoveFromEnd(TEXT("settings"));

		int32 Score = 0;
		if (CnCore == WantLower) Score = 100;                       // exact
		else if (CnCore.EndsWith(WantLower)) Score = 80;            // suffix
		else if (CnCore.StartsWith(WantLower)) Score = 70;          // prefix
		else if (CnCore.Contains(WantLower)) Score = 50;            // substring
		else continue;

		if (Score > BestScore)
		{
			BestScore = Score;
			BestMatch = C;
		}
	}
	return BestMatch;
}

FString FNwiroIKPCGTools::AddPcgNode(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd = ParseArgs(JsonCommand);
	if (!Cmd.IsValid()) return JsonToString(Fail(TEXT("Invalid JSON")));

	// Hallucination-tolerant arg picker.
	auto Pick = [&](std::initializer_list<const TCHAR*> Names) -> FString {
		for (const TCHAR* N : Names) { FString V; if (Cmd->TryGetStringField(N, V) && !V.IsEmpty()) return V; }
		return FString();
	};
	const FString GraphPath = Pick({ TEXT("graph"), TEXT("assetPath"), TEXT("path"), TEXT("pcgGraph") });
	const FString NodeKind  = Pick({ TEXT("node"), TEXT("nodeType"), TEXT("type"), TEXT("kind"), TEXT("settings"), TEXT("class") });
	if (GraphPath.IsEmpty()) return JsonToString(Fail(TEXT("'graph' (path to UPCGGraph asset) is required")));
	if (NodeKind.IsEmpty())  return JsonToString(Fail(TEXT("'node' (settings class name like StaticMeshSpawner / SurfaceSampler / SelfPruning) is required")));

	UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
	if (!Graph) return JsonToString(Fail(FString::Printf(TEXT("UPCGGraph not found at path: %s"), *GraphPath)));

	UClass* SettingsClass = ResolvePcgSettingsClass(NodeKind);
	if (!SettingsClass)
		return JsonToString(Fail(FString::Printf(TEXT("Could not resolve PCG settings class for '%s'. Try names like StaticMeshSpawner, SurfaceSampler, SelfPruning, CopyPoints, DensityFilter."), *NodeKind)));

	// Create the settings + node, register with graph.
	UPCGSettings* Settings = NewObject<UPCGSettings>(Graph, SettingsClass, NAME_None, RF_Transactional);
	if (!Settings)
		return JsonToString(Fail(FString::Printf(TEXT("Failed to NewObject<UPCGSettings>(%s)"), *SettingsClass->GetName())));

	UPCGNode* NewNode = Graph->AddNodeOfType(SettingsClass, Settings);
	if (!NewNode)
	{
		// Older 5.x or alternate API surface — try AddNode with explicit setup.
		NewNode = NewObject<UPCGNode>(Graph, NAME_None, RF_Transactional);
		if (NewNode)
		{
			NewNode->SetSettingsInterface(Settings);
			Graph->AddNode(NewNode);
		}
	}
	if (!NewNode)
		return JsonToString(Fail(TEXT("Graph->AddNodeOfType and manual AddNode both failed")));

	// Optional position
	double X = 0, Y = 0;
	if (Cmd->TryGetNumberField(TEXT("x"), X)) NewNode->PositionX = X;
	if (Cmd->TryGetNumberField(TEXT("y"), Y)) NewNode->PositionY = Y;

	Graph->MarkPackageDirty();

	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetBoolField(TEXT("success"), true);
	J->SetStringField(TEXT("graph"), Graph->GetPathName());
	J->SetStringField(TEXT("settingsClass"), SettingsClass->GetName());
	J->SetStringField(TEXT("nodeName"), NewNode->GetName());
	J->SetNumberField(TEXT("totalNodes"), Graph->GetNodes().Num());
	return JsonToString(J);
}
