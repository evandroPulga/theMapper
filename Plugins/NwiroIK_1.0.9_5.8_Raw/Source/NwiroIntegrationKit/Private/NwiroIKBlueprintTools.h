// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UK2Node;

// Result from any blueprint edit operation
struct FNwiroIKBPResult
{
	bool bSuccess = false;
	FString Message;
	TSharedPtr<FJsonObject> Data;

	static FNwiroIKBPResult Ok(const FString& Msg)
	{
		FNwiroIKBPResult R;
		R.bSuccess = true;
		R.Message = Msg;
		return R;
	}

	static FNwiroIKBPResult Fail(const FString& Msg)
	{
		FNwiroIKBPResult R;
		R.bSuccess = false;
		R.Message = Msg;
		return R;
	}
};

// Tracks node references within an editing session (ref name -> node GUID)
struct FNwiroIKNodeRef
{
	FGuid NodeGuid;
	FString GraphName;
};

/**
 * Core blueprint manipulation logic for Nwiro.
 * All methods are static, called from UNwiroIKBridge.
 * Returns JSON strings for communication with the web UI.
 */
class FNwiroIKBlueprintTools
{
public:
	// ===== Bridge-facing API (returns JSON) =====

	// Search blueprints by name/path. Returns JSON array.
	static FString FindBlueprints(const FString& SearchTerm);

	// Render a Blueprint's graph (event graph or named graph) to PNG via SGraphEditor + FWidgetRenderer.
	static FString RenderBlueprintGraph(const FString& JsonCommand);
	// Auto-arrange a graph's nodes (headless layered layout) so they don't overlap.
	static FString OrganizeBlueprintNodes(const FString& JsonCommand);


	// Read blueprint structure (variables, components, functions, graphs). Returns JSON.
	// When ArgsJson contains a "graph" field, returns full node/pin data for that function graph.
	static FString ReadBlueprint(const FString& ArgsJson);

	// Main edit entry point. Accepts JSON with operations. Returns JSON result.
	static FString EditBlueprint(const FString& JsonCommand);

	// Create a new blueprint asset. Returns JSON with the created blueprint path.
	static FString CreateBlueprint(const FString& JsonCommand);

	// Search available node types for blueprint graphs. Returns JSON.
	static FString FindBlueprintNodes(const FString& Query, const FString& BlueprintPath);

	// Delete a blueprint asset.
	static FString DeleteBlueprint(const FString& JsonCommand);

	// Duplicate a blueprint asset.
	static FString DuplicateBlueprint(const FString& JsonCommand);

	// Rename a blueprint asset.
	static FString RenameBlueprint(const FString& JsonCommand);


	// Clear all user-created nodes from a blueprint graph (keeps events)
	static FString ClearGraph(const FString& JsonCommand);

	// Standalone wrappers for edit_blueprint sub-operations
	static FString DeleteNode(const FString& JsonCommand);
	static FString CreateFunctionGraph(const FString& JsonCommand);
	static FString AddInterface(const FString& JsonCommand);
	static FString RemoveInterface(const FString& JsonCommand);
	static FString CreateEventDispatcher(const FString& JsonCommand);
	static FString ReparentBlueprint(const FString& JsonCommand);
	static FString RemoveComponent(const FString& JsonCommand);
	static FString EditComponent(const FString& JsonCommand);

	// Clear session node references
	static void ClearNodeRefs();

	// ===== Class Resolution =====
	// Public so file-scope helpers (e.g. ResolveInterfaceClass) can reuse it.
	static UClass* FindClassByName(const FString& Name);

private:
	// ===== Asset Loading =====
	static UBlueprint* LoadBP(const FString& PathOrName);

	// ===== Graph Helpers =====
	static UEdGraph* FindGraph(UBlueprint* BP, const FString& GraphName);
	static UEdGraphNode* FindNodeByRef(UEdGraph* Graph, const FString& Ref);
	static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Dir = EGPD_MAX);
	static void StoreNodeRef(const FString& Ref, UEdGraphNode* Node, const FString& GraphName);

	// ===== Type Parsing =====
	static FEdGraphPinType ParsePinType(const TSharedPtr<FJsonObject>& TypeObj);

	// ===== Edit Operations =====
	static FNwiroIKBPResult DoAddVariables(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoRemoveVariables(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoRenameVariables(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoAddComponents(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoRemoveComponents(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoSetComponentProperties(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoAddFunctions(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoRemoveFunctions(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoAddCustomEvents(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoAddEventDispatchers(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoAddInterfaces(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoRemoveInterfaces(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoReparent(UBlueprint* BP, const FString& NewParentClass);
	static FNwiroIKBPResult DoAddNodes(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoRemoveNodes(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoConnectPins(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoBreakConnections(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items);
	static FNwiroIKBPResult DoSetPinDefaults(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items);

	// ===== Component Property Helper =====
	// Applies one key/value pair to a component template.
	// Handles special asset-loading cases, direct reflection, and deep struct reflection.
	// Returns true if the property was successfully set.
	static bool ApplyComponentProperty(UActorComponent* CompTemplate, const FString& Key, const FString& Value, const FString& CompName, TArray<FString>* OutErrors = nullptr, TArray<FString>* OutWarnings = nullptr);

	// ===== Serialization Helpers =====
	static TSharedPtr<FJsonObject> SerializeVariable(UBlueprint* BP, const FName& VarName);
	static TSharedPtr<FJsonObject> SerializeComponent(const UActorComponent* Comp, const FName& VarName);
	static TSharedPtr<FJsonObject> SerializeGraph(UEdGraph* Graph, bool bIncludeNodes);
	static TSharedPtr<FJsonObject> SerializeNode(UEdGraphNode* Node);
	static FString PinTypeToString(const FEdGraphPinType& PinType);

	// ===== Node Ref Storage =====
	static TMap<FString, FNwiroIKNodeRef> NodeRefs;
};
