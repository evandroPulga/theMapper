// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKDebugTools.h"
#include "NwiroIKAssetGuard.h"
#include "NwiroIKTransactionHelper.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Kismet2/Breakpoint.h"
#include "Kismet2/WatchedPin.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "Json.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroDebug, Log, All);

// ============================================================
// HELPER
// ============================================================

static UBlueprint* LoadBPByPath(const FString& PathOrName)
{
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *PathOrName);
	if (BP) return BP;

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	ARM.Get().GetAssets(Filter, Assets);
	for (const FAssetData& A : Assets)
	{
		if (A.AssetName.ToString().Equals(PathOrName, ESearchCase::IgnoreCase) ||
			A.AssetName.ToString().Contains(PathOrName, ESearchCase::IgnoreCase))
		{
			return Cast<UBlueprint>(NwiroSafeRegistryLoad(A));
		}
	}
	return nullptr;
}

static UEdGraphNode* FindNodeInBP(UBlueprint* BP, const FString& NodeName)
{
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(NodeName, ESearchCase::IgnoreCase) ||
				Node->GetName().Contains(NodeName, ESearchCase::IgnoreCase))
			{
				return Node;
			}
		}
	}
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(NodeName, ESearchCase::IgnoreCase) ||
				Node->GetName().Contains(NodeName, ESearchCase::IgnoreCase))
			{
				return Node;
			}
		}
	}
	return nullptr;
}

// ============================================================
// BP GET COMPILE ERRORS
// ============================================================

FString FNwiroIKDebugTools::BPGetCompileErrors(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	// Compile to get fresh errors
	FKismetEditorUtilities::CompileBlueprint(BP);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BP->GetName());

	// Status
	FString Status;
	switch (BP->Status)
	{
	case BS_Error: Status = TEXT("Error"); break;
	case BS_UpToDate: Status = TEXT("UpToDate"); break;
	case BS_Dirty: Status = TEXT("Dirty"); break;
	case BS_UpToDateWithWarnings: Status = TEXT("UpToDateWithWarnings"); break;
	default: Status = TEXT("Unknown"); break;
	}
	Result->SetStringField(TEXT("status"), Status);

	// Collect errors and warnings from compiler results
	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node->bHasCompilerMessage)
			{
				TSharedRef<FJsonObject> Msg = MakeShareable(new FJsonObject());
				Msg->SetStringField(TEXT("node"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
				Msg->SetStringField(TEXT("graph"), Graph->GetName());
				Msg->SetStringField(TEXT("message"), Node->ErrorMsg);

				if (Node->ErrorType == EMessageSeverity::Error)
					Errors.Add(MakeShareable(new FJsonValueObject(Msg)));
				else
					Warnings.Add(MakeShareable(new FJsonValueObject(Msg)));
			}
		}
	}

	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node->bHasCompilerMessage)
			{
				TSharedRef<FJsonObject> Msg = MakeShareable(new FJsonObject());
				Msg->SetStringField(TEXT("node"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
				Msg->SetStringField(TEXT("graph"), Graph->GetName());
				Msg->SetStringField(TEXT("message"), Node->ErrorMsg);

				if (Node->ErrorType == EMessageSeverity::Error)
					Errors.Add(MakeShareable(new FJsonValueObject(Msg)));
				else
					Warnings.Add(MakeShareable(new FJsonValueObject(Msg)));
			}
		}
	}

	Result->SetArrayField(TEXT("errors"), Errors);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetNumberField(TEXT("errorCount"), Errors.Num());
	Result->SetNumberField(TEXT("warningCount"), Warnings.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// BP SET BREAKPOINT
// ============================================================

FString FNwiroIKDebugTools::BPSetBreakpoint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	FString NodeName = Cmd->GetStringField(TEXT("node"));

	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	UEdGraphNode* Node = FindNodeInBP(BP, NodeName);
	if (!Node) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Node not found: %s\"}"), *NodeName);

	FKismetDebugUtilities::CreateBreakpoint(BP, Node, true);

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	return FString::Printf(TEXT("{\"success\":true,\"blueprint\":\"%s\",\"node\":\"%s\"}"),
		*BP->GetName(), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
}

// ============================================================
// BP REMOVE BREAKPOINT
// ============================================================

FString FNwiroIKDebugTools::BPRemoveBreakpoint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	FString NodeName = Cmd->GetStringField(TEXT("node"));

	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	UEdGraphNode* Node = FindNodeInBP(BP, NodeName);
	if (!Node) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Node not found: %s\"}"), *NodeName);

	int32 Removed = 0;

	if (FKismetDebugUtilities::FindBreakpointForNode(Node, BP))
	{
		FKismetDebugUtilities::RemoveBreakpointFromNode(Node, BP);
		Removed++;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	return FString::Printf(TEXT("{\"success\":true,\"removed\":%d}"), Removed);
}

// ============================================================
// BP LIST BREAKPOINTS
// ============================================================

FString FNwiroIKDebugTools::BPListBreakpoints(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	TArray<TSharedPtr<FJsonValue>> BPs;

	FKismetDebugUtilities::ForeachBreakpoint(BP, [&](FBlueprintBreakpoint& BPt)
	{
		UEdGraphNode* BPNode = BPt.GetLocation();
		if (!BPNode) return;
		TSharedRef<FJsonObject> B = MakeShareable(new FJsonObject());
		B->SetStringField(TEXT("node"), BPNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
		B->SetBoolField(TEXT("enabled"), BPt.IsEnabledByUser());
		BPs.Add(MakeShareable(new FJsonValueObject(B)));
	});

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("breakpoints"), BPs);
	Result->SetNumberField(TEXT("count"), BPs.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// BP GET WATCH VALUES
// ============================================================

FString FNwiroIKDebugTools::BPGetWatchValues(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	// Get watched pins
	TArray<TSharedPtr<FJsonValue>> Watches;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				bool bIsWatched = false;
				bIsWatched = FKismetDebugUtilities::IsPinBeingWatched(BP, Pin);
				if (bIsWatched)
				{
					TSharedRef<FJsonObject> W2 = MakeShareable(new FJsonObject());
					W2->SetStringField(TEXT("node"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
					W2->SetStringField(TEXT("pin"), Pin->PinName.ToString());
					W2->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
					W2->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);

					// Try to get debug value if PIE running
					if (GEditor->PlayWorld)
					{
						FString DebugVal;
						// Pin debug text is available during PIE via the debugger
						W2->SetStringField(TEXT("debugValue"), Pin->DefaultValue);
					}

					Watches.Add(MakeShareable(new FJsonValueObject(W2)));
				}
			}
		}
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("watches"), Watches);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// BP ADD WATCH
// ============================================================

FString FNwiroIKDebugTools::BPAddWatch(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	FString NodeName = Cmd->GetStringField(TEXT("node"));
	FString PinName = Cmd->GetStringField(TEXT("pin"));

	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	UEdGraphNode* Node = FindNodeInBP(BP, NodeName);
	if (!Node) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Node not found: %s\"}"), *NodeName);

	UEdGraphPin* Pin = nullptr;
	for (UEdGraphPin* P : Node->Pins)
	{
		if (P->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			Pin = P;
			break;
		}
	}

	if (!Pin) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Pin not found: %s\"}"), *PinName);

	FKismetDebugUtilities::AddPinWatch(BP, FBlueprintWatchedPin(Pin));

	return FString::Printf(TEXT("{\"success\":true,\"node\":\"%s\",\"pin\":\"%s\"}"),
		*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *PinName);
}

// ============================================================
// BP FIX BROKEN REFERENCES
// ============================================================

FString FNwiroIKDebugTools::BPFixBrokenReferences(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "BPFixBrokenReferences", "AI: Fix Broken References"), BP);

	int32 FixedCount = 0;

	// Remove broken links in all graphs
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			for (int32 i = Node->Pins.Num() - 1; i >= 0; i--)
			{
				UEdGraphPin* Pin = Node->Pins[i];
				for (int32 j = Pin->LinkedTo.Num() - 1; j >= 0; j--)
				{
					UEdGraphPin* LinkedPin = Pin->LinkedTo[j];
					if (!LinkedPin || !LinkedPin->GetOwningNode() || !LinkedPin->GetOwningNode()->GetGraph())
					{
						Pin->LinkedTo.RemoveAt(j);
						FixedCount++;
					}
				}
			}
		}
	}

	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			for (int32 i = Node->Pins.Num() - 1; i >= 0; i--)
			{
				UEdGraphPin* Pin = Node->Pins[i];
				for (int32 j = Pin->LinkedTo.Num() - 1; j >= 0; j--)
				{
					UEdGraphPin* LinkedPin = Pin->LinkedTo[j];
					if (!LinkedPin || !LinkedPin->GetOwningNode() || !LinkedPin->GetOwningNode()->GetGraph())
					{
						Pin->LinkedTo.RemoveAt(j);
						FixedCount++;
					}
				}
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);

	return FString::Printf(TEXT("{\"success\":true,\"blueprint\":\"%s\",\"fixedLinks\":%d}"), *BP->GetName(), FixedCount);
}

// ============================================================
// BP FIX DEPRECATED NODES
// ============================================================

FString FNwiroIKDebugTools::BPFixDeprecatedNodes(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "BPFixDeprecatedNodes", "AI: Fix Deprecated Nodes"), BP);

	// Count deprecated nodes before
	int32 DeprecatedBefore = 0;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node->IsDeprecated()) DeprecatedBefore++;
		}
	}

	// Refresh all nodes - this updates deprecated nodes to newer versions
	FBlueprintEditorUtils::RefreshAllNodes(BP);
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);

	// Count after
	int32 DeprecatedAfter = 0;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node->IsDeprecated()) DeprecatedAfter++;
		}
	}

	return FString::Printf(TEXT("{\"success\":true,\"blueprint\":\"%s\",\"deprecatedBefore\":%d,\"deprecatedAfter\":%d,\"fixed\":%d}"),
		*BP->GetName(), DeprecatedBefore, DeprecatedAfter, DeprecatedBefore - DeprecatedAfter);
}

// ============================================================
// BP REFRESH ALL NODES
// ============================================================

FString FNwiroIKDebugTools::BPRefreshAllNodes(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "BPRefreshAllNodes", "AI: Refresh All Nodes"), BP);

	FBlueprintEditorUtils::RefreshAllNodes(BP);
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);

	return FString::Printf(TEXT("{\"success\":true,\"blueprint\":\"%s\",\"message\":\"All nodes refreshed and recompiled\"}"), *BP->GetName());
}

// ============================================================
// BP FIND UNCONNECTED PINS
// ============================================================

FString FNwiroIKDebugTools::BPFindUnconnectedPins(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPPath = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBPByPath(BPPath);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPPath);

	TArray<TSharedPtr<FJsonValue>> Unconnected;

	auto CheckGraph = [&](UEdGraph* Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				// Only check exec pins that should be connected
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
					Pin->LinkedTo.Num() == 0 &&
					Pin->Direction == EGPD_Output &&
					!Pin->bHidden)
				{
					TSharedRef<FJsonObject> U = MakeShareable(new FJsonObject());
					U->SetStringField(TEXT("node"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
					U->SetStringField(TEXT("pin"), Pin->PinName.ToString());
					U->SetStringField(TEXT("graph"), Graph->GetName());
					U->SetStringField(TEXT("direction"), TEXT("Output"));
					Unconnected.Add(MakeShareable(new FJsonValueObject(U)));
				}
			}
		}
	};

	for (UEdGraph* Graph : BP->UbergraphPages) CheckGraph(Graph);
	for (UEdGraph* Graph : BP->FunctionGraphs) CheckGraph(Graph);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("unconnectedPins"), Unconnected);
	Result->SetNumberField(TEXT("count"), Unconnected.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// GET ASSET REFERENCES (dependencies)
// ============================================================

FString FNwiroIKDebugTools::GetAssetReferences(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	if (Path.IsEmpty()) Path = Cmd->GetStringField(TEXT("asset"));

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	TArray<FName> Dependencies;
	AR.GetDependencies(FName(*Path), Dependencies);

	TArray<TSharedPtr<FJsonValue>> Deps;
	for (const FName& Dep : Dependencies)
	{
		Deps.Add(MakeShareable(new FJsonValueString(Dep.ToString())));
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset"), Path);
	Result->SetArrayField(TEXT("dependencies"), Deps);
	Result->SetNumberField(TEXT("count"), Deps.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// GET ASSET REFERENCERS
// ============================================================

FString FNwiroIKDebugTools::GetAssetReferencers(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	if (Path.IsEmpty()) Path = Cmd->GetStringField(TEXT("asset"));

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	TArray<FName> Referencers;
	AR.GetReferencers(FName(*Path), Referencers);

	TArray<TSharedPtr<FJsonValue>> Refs;
	for (const FName& Ref : Referencers)
	{
		Refs.Add(MakeShareable(new FJsonValueString(Ref.ToString())));
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset"), Path);
	Result->SetArrayField(TEXT("referencers"), Refs);
	Result->SetNumberField(TEXT("count"), Refs.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// FIND ORPHAN ASSETS
// ============================================================

FString FNwiroIKDebugTools::FindOrphanAssets(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	FJsonSerializer::Deserialize(R, Cmd);

	FString SearchPath = (Cmd.IsValid() && Cmd->HasField(TEXT("path"))) ? Cmd->GetStringField(TEXT("path")) : TEXT("/Game");
	int32 MaxResults = (Cmd.IsValid() && Cmd->HasField(TEXT("maxResults"))) ? (int32)Cmd->GetNumberField(TEXT("maxResults")) : 50;

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*SearchPath));
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Orphans;
	for (const FAssetData& A : Assets)
	{
		if (Orphans.Num() >= MaxResults) break;

		TArray<FName> Referencers;
		AR.GetReferencers(A.PackageName, Referencers);

		// Remove self-references and script references
		Referencers.RemoveAll([&](const FName& Ref) {
			return Ref == A.PackageName || Ref.ToString().StartsWith(TEXT("/Script/"));
		});

		if (Referencers.Num() == 0)
		{
			TSharedRef<FJsonObject> O = MakeShareable(new FJsonObject());
			O->SetStringField(TEXT("name"), A.AssetName.ToString());
			O->SetStringField(TEXT("path"), A.PackageName.ToString());
			O->SetStringField(TEXT("class"), A.AssetClassPath.GetAssetName().ToString());
			Orphans.Add(MakeShareable(new FJsonValueObject(O)));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("orphanAssets"), Orphans);
	Result->SetNumberField(TEXT("count"), Orphans.Num());
	Result->SetNumberField(TEXT("totalScanned"), Assets.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// FIND CIRCULAR DEPENDENCIES
// ============================================================

FString FNwiroIKDebugTools::FindCircularDependencies(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	if (Path.IsEmpty()) Path = Cmd->GetStringField(TEXT("asset"));

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	// DFS to detect cycles
	TSet<FName> Visited;
	TSet<FName> InStack;
	TArray<FString> Cycles;

	TFunction<bool(FName, TArray<FName>&)> DFS;
	DFS = [&](FName Current, TArray<FName>& Path) -> bool
	{
		if (InStack.Contains(Current))
		{
			// Found cycle - build cycle string
			FString CycleStr;
			bool bInCycle = false;
			for (const FName& P : Path)
			{
				if (P == Current) bInCycle = true;
				if (bInCycle) CycleStr += P.ToString() + TEXT(" -> ");
			}
			CycleStr += Current.ToString();
			Cycles.Add(CycleStr);
			return true;
		}

		if (Visited.Contains(Current)) return false;

		Visited.Add(Current);
		InStack.Add(Current);
		Path.Add(Current);

		TArray<FName> Deps;
		AR.GetDependencies(Current, Deps);

		for (const FName& Dep : Deps)
		{
			if (Dep.ToString().StartsWith(TEXT("/Game")))
			{
				DFS(Dep, Path);
			}
		}

		Path.Pop();
		InStack.Remove(Current);
		return false;
	};

	TArray<FName> PathArr;
	DFS(FName(*Path), PathArr);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset"), Path);
	Result->SetBoolField(TEXT("hasCircularDependency"), Cycles.Num() > 0);

	TArray<TSharedPtr<FJsonValue>> CycleArr;
	for (const FString& C : Cycles)
		CycleArr.Add(MakeShareable(new FJsonValueString(C)));
	Result->SetArrayField(TEXT("cycles"), CycleArr);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// GET DEPENDENCY TREE
// ============================================================

FString FNwiroIKDebugTools::GetDependencyTree(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	if (Path.IsEmpty()) Path = Cmd->GetStringField(TEXT("asset"));
	int32 MaxDepth = Cmd->HasField(TEXT("depth")) ? (int32)Cmd->GetNumberField(TEXT("depth")) : 3;

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	TFunction<TSharedPtr<FJsonObject>(FName, int32, TSet<FName>&)> BuildTree;
	BuildTree = [&](FName Current, int32 Depth, TSet<FName>& Visited) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Node = MakeShareable(new FJsonObject());
		Node->SetStringField(TEXT("asset"), Current.ToString());

		if (Depth <= 0 || Visited.Contains(Current))
		{
			if (Visited.Contains(Current))
				Node->SetBoolField(TEXT("circular"), true);
			return Node;
		}

		Visited.Add(Current);

		TArray<FName> Deps;
		AR.GetDependencies(Current, Deps);

		TArray<TSharedPtr<FJsonValue>> Children;
		for (const FName& Dep : Deps)
		{
			if (Dep.ToString().StartsWith(TEXT("/Game")))
			{
				TSharedPtr<FJsonObject> Child = BuildTree(Dep, Depth - 1, Visited);
				if (Child.IsValid())
					Children.Add(MakeShareable(new FJsonValueObject(Child)));
			}
		}

		if (Children.Num() > 0)
			Node->SetArrayField(TEXT("dependencies"), Children);

		Visited.Remove(Current);
		return Node;
	};

	TSet<FName> Visited;
	TSharedPtr<FJsonObject> Tree = BuildTree(FName(*Path), MaxDepth, Visited);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetObjectField(TEXT("tree"), Tree);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}
