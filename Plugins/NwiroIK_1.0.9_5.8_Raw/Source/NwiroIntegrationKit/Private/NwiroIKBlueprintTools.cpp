// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKBlueprintTools.h"
#include "NwiroIKTransactionHelper.h"
#include "NwiroIKAssetGuard.h"
#include "Engine/Blueprint.h"
// Graph rendering deps (for RenderBlueprintGraph)
#include "GraphEditor.h"
#include "EdGraph/EdGraph.h"
#include "Slate/WidgetRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "RenderingThread.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Editor.h"
#include "Engine/Canvas.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Font.h"
#include "Engine/Engine.h"
#include "TextureResource.h"
#include "EdGraphNode_Comment.h"
#include "EditorAssetLibrary.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
// Common UMG component classes — used as fallback owners for CallFunction
// resolution when the agent omits `target` (e.g. SetPercent → UProgressBar).
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/Button.h"
#include "Components/Slider.h"
#include "Components/CheckBox.h"
#include "Components/SpinBox.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/MultiLineEditableText.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/Image.h"
#include "Components/Border.h"
#include "Components/WidgetSwitcher.h"
#include "Components/ComboBoxString.h"
#include "Components/CanvasPanel.h"
#include "Components/VerticalBox.h"
#include "Components/HorizontalBox.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompiler.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputKey.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Timeline.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_MakeArray.h"
#include "K2Node_Self.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_Select.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_EnhancedInputAction.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "Editor.h"
#include "Json.h"
#include "EditorAssetLibrary.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/UserWidget.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/UObjectIterator.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Subsystems/SubsystemBlueprintLibrary.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroBP, Log, All);

TMap<FString, FNwiroIKNodeRef> FNwiroIKBlueprintTools::NodeRefs;

static FString NormalizeKey(const FString& Key)
{
	FString Result = Key.Replace(TEXT("_"), TEXT(""));
	return Result.ToLower();
}

// Returns the value of the first JSON field whose normalized key matches NormalizeKey(WantedKey).
// Handles actor_class / actorClass / ActorClass / ACTOR_CLASS etc. transparently.
static FString GetFieldNormalized(const TSharedPtr<FJsonObject>& Obj, const FString& WantedKey)
{
	if (!Obj.IsValid()) return TEXT("");
	FString NWanted = NormalizeKey(WantedKey);
	for (auto& Pair : Obj->Values)
	{
		if (NormalizeKey(FString(*Pair.Key)) == NWanted)
		{
			FString Out;
			if (Pair.Value->TryGetString(Out)) return Out;
		}
	}
	return TEXT("");
}

static bool IsKnownEditBlueprintKey(const FString& Key)
{
	static const TSet<FString> KnownKeys = {
		TEXT("blueprint"),
		TEXT("graph"),
		TEXT("compile"),
		TEXT("parentClass"),
		TEXT("parent_class"),
		TEXT("actions"),
		TEXT("reparent"),
		TEXT("add_variables"),
		TEXT("remove_variables"),
		TEXT("rename_variables"),
		TEXT("add_components"),
		TEXT("remove_components"),
		TEXT("set_component_properties"),
		TEXT("add_functions"),
		TEXT("remove_functions"),
		TEXT("add_custom_events"),
		TEXT("add_event_dispatchers"),
		TEXT("add_interfaces"),
		TEXT("remove_interfaces"),
		TEXT("add_nodes"),
		TEXT("remove_nodes"),
		TEXT("connect_pins"),
		TEXT("break_connections"),
		TEXT("set_pin_defaults"),
	};
	return KnownKeys.Contains(Key);
}

static bool IsEditBlueprintOperationKey(const FString& Key)
{
	static const TSet<FString> OperationKeys = {
		TEXT("reparent"),
		TEXT("add_variables"),
		TEXT("remove_variables"),
		TEXT("rename_variables"),
		TEXT("add_components"),
		TEXT("remove_components"),
		TEXT("set_component_properties"),
		TEXT("add_functions"),
		TEXT("remove_functions"),
		TEXT("add_custom_events"),
		TEXT("add_event_dispatchers"),
		TEXT("add_interfaces"),
		TEXT("remove_interfaces"),
		TEXT("add_nodes"),
		TEXT("remove_nodes"),
		TEXT("connect_pins"),
		TEXT("break_connections"),
		TEXT("set_pin_defaults"),
	};
	return OperationKeys.Contains(Key);
}

static int32 GetArrayCount(const TSharedPtr<FJsonObject>& Obj, const FString& Key)
{
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	return Obj.IsValid() && Obj->TryGetArrayField(Key, Arr) && Arr ? Arr->Num() : 0;
}

// Resolve an edit_blueprint op key to its item array. Accepts a real JSON
// array, or a JSON STRING that parses to an array (models frequently stringify
// nested arrays — same tolerance as edit_material's GetArrayFieldFlexible in
// NwiroIKMaterialTools.cpp). Returns false with OutError set when the key is
// PRESENT but unusable; the caller reports a per-op failure instead of
// silently skipping the op (the old silent skip let compile+save run and
// return success:true with zero mutations).
static bool ResolveOpArray(const TSharedPtr<FJsonObject>& Cmd, const FString& Key,
	TArray<TSharedPtr<FJsonValue>>& OutItems, FString& OutNormalizeWarning, FString& OutError)
{
	OutNormalizeWarning.Empty();

	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (Cmd->TryGetArrayField(Key, Arr) && Arr)
	{
		OutItems = *Arr;
		return true;
	}

	const TSharedPtr<FJsonValue> Val = Cmd->TryGetField(Key);
	if (Val.IsValid() && Val->Type == EJson::Null)
	{
		OutError = FString::Printf(TEXT("ERROR: null is not a valid value for '%s'; omit the key to skip this operation"), *Key);
		return false;
	}
	if (Val.IsValid() && Val->Type == EJson::Object)
	{
		// Models frequently send a SINGLE item object where an array of items
		// is expected. The container shape is unambiguous — wrap it; item-level
		// validation still applies to the wrapped object.
		OutItems.Add(MakeShareable(new FJsonValueObject(Val->AsObject())));
		OutNormalizeWarning = FString::Printf(TEXT("normalized single object to array; use %s:[{...}]"), *Key);
		return true;
	}
	if (Val.IsValid() && Val->Type == EJson::String)
	{
		TArray<TSharedPtr<FJsonValue>> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Val->AsString());
		if (FJsonSerializer::Deserialize(Reader, Parsed))
		{
			OutItems = MoveTemp(Parsed);
			OutNormalizeWarning = TEXT("normalized stringified array — send a real JSON array next time");
			return true;
		}
		TSharedPtr<FJsonObject> ParsedObj;
		TSharedRef<TJsonReader<>> ObjReader = TJsonReaderFactory<>::Create(Val->AsString());
		if (FJsonSerializer::Deserialize(ObjReader, ParsedObj) && ParsedObj.IsValid())
		{
			OutItems.Add(MakeShareable(new FJsonValueObject(ParsedObj)));
			OutNormalizeWarning = FString::Printf(TEXT("normalized stringified object to a one-element array; use %s:[{...}]"), *Key);
			return true;
		}
		OutError = FString::Printf(TEXT("ERROR: value for '%s' is a string that does not parse as a JSON array — send a real JSON array of objects"), *Key);
		return false;
	}

	OutError = FString::Printf(TEXT("ERROR: value for '%s' must be a JSON array of objects, e.g. %s:[{...}] — got a non-array value"), *Key, *Key);
	return false;
}

// ============================================================
// APPLY COMPONENT PROPERTY (shared helper)
// ============================================================

bool FNwiroIKBlueprintTools::ApplyComponentProperty(
	UActorComponent* CompTemplate,
	const FString& Key,
	const FString& Value,
	const FString& CompName,
	TArray<FString>* OutErrors,
	TArray<FString>* OutWarnings)
{
	if (!CompTemplate) return false;

	// Property name aliases: maps LLM-normalized keys to UE-normalized property names.
	// Needed when word order differs (NormalizeKey can't fix that).
	// e.g. "mass_override_in_kg" → "massoverrideinkg" but UE has "MassInKgOverride" → "massinkgoverride"
	static const TMap<FString, FString> PropAliases = {
		{ TEXT("massoverrideinkg"), TEXT("massinkgoverride") },
	};

	FString NKey = NormalizeKey(Key);
	if (const FString* Aliased = PropAliases.Find(NKey)) NKey = *Aliased;

	// Mesh aliases for shorthand values (e.g. "cube" → full engine path)
	static const TMap<FString, FString> MeshAliases = {
		{ TEXT("sphere"),   TEXT("/Engine/BasicShapes/Sphere.Sphere") },
		{ TEXT("cube"),     TEXT("/Engine/BasicShapes/Cube.Cube") },
		{ TEXT("cylinder"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder") },
		{ TEXT("cone"),     TEXT("/Engine/BasicShapes/Cone.Cone") },
		{ TEXT("plane"),    TEXT("/Engine/BasicShapes/Plane.Plane") },
	};

	// === Special case: StaticMesh — needs LoadObject ===
	if (NKey == TEXT("staticmesh") || NKey == TEXT("mesh"))
	{
		FString MeshPath = Value;
		if (const FString* Alias = MeshAliases.Find(MeshPath.ToLower())) MeshPath = *Alias;
		if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(CompTemplate))
		{
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
			if (!Mesh && !MeshPath.StartsWith(TEXT("/")))
				Mesh = LoadObject<UStaticMesh>(nullptr, *(TEXT("/Engine/BasicShapes/") + MeshPath));
			if (Mesh) { SMC->SetStaticMesh(Mesh); return true; }
			if (OutErrors) OutErrors->Add(FString::Printf(TEXT("StaticMesh not found: %s"), *MeshPath));
		}
		return false;
	}

	// === Special case: SkeletalMesh — needs LoadObject ===
	if (NKey == TEXT("skeletalmesh") || NKey == TEXT("skeletalmeshasset"))
	{
		if (USkeletalMeshComponent* SkMC = Cast<USkeletalMeshComponent>(CompTemplate))
		{
			USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *Value);
			if (Mesh) { SkMC->SetSkeletalMeshAsset(Mesh); return true; }
			if (OutErrors) OutErrors->Add(FString::Printf(TEXT("SkeletalMesh not found: %s"), *Value));
		}
		return false;
	}

	// === Level 1: Direct reflection on the component class ===
	for (TFieldIterator<FProperty> It(CompTemplate->GetClass()); It; ++It)
	{
		FString PropName = It->GetName();
		bool bMatch = NormalizeKey(PropName) == NKey;
		// Also match without leading 'b' (UE bool convention: bEnableGravity → enablegravity)
		if (!bMatch && PropName.StartsWith(TEXT("b")) && PropName.Len() > 1)
			bMatch = NormalizeKey(PropName.Mid(1)) == NKey;

		if (bMatch)
		{
			void* ValuePtr = It->ContainerPtrToValuePtr<void>(CompTemplate);
			if (It->ImportText_Direct(*Value, ValuePtr, CompTemplate, PPF_None))
				return true;

			// Object property fallback: try LoadObject
			if (FObjectProperty* ObjProp = CastField<FObjectProperty>(*It))
			{
				if (UObject* Obj = LoadObject<UObject>(nullptr, *Value))
				{
					ObjProp->SetObjectPropertyValue(ValuePtr, Obj);
					return true;
				}
			}
			if (OutErrors) OutErrors->Add(FString::Printf(TEXT("Failed to set %s.%s = %s"), *CompName, *Key, *Value));
			return false;
		}
	}

	// === Level 2: Deep struct reflection — search inside struct properties ===
	// Handles nested structs like FBodyInstance (enable_gravity, mass_override_in_kg, etc.)
	for (TFieldIterator<FStructProperty> StructIt(CompTemplate->GetClass()); StructIt; ++StructIt)
	{
		void* StructPtr = StructIt->ContainerPtrToValuePtr<void>(CompTemplate);

		for (TFieldIterator<FProperty> SubIt(StructIt->Struct); SubIt; ++SubIt)
		{
			FString SubName = SubIt->GetName();
			bool bMatch = NormalizeKey(SubName) == NKey;
			if (!bMatch && SubName.StartsWith(TEXT("b")) && SubName.Len() > 1)
				bMatch = NormalizeKey(SubName.Mid(1)) == NKey;

			if (bMatch)
			{
				void* SubValuePtr = SubIt->ContainerPtrToValuePtr<void>(StructPtr);
				if (SubIt->ImportText_Direct(*Value, SubValuePtr, nullptr, PPF_None))
					return true;
				if (OutErrors) OutErrors->Add(FString::Printf(TEXT("Failed to set %s.%s = %s (in struct %s)"), *CompName, *Key, *Value, *StructIt->GetName()));
				return false;
			}
		}
	}

	// Nothing matched
	if (OutWarnings) OutWarnings->Add(FString::Printf(TEXT("Unrecognized property '%s' on component '%s' — ignored"), *Key, *CompName));
	return false;
}

// ============================================================
// FIND BLUEPRINTS
// ============================================================

FString FNwiroIKBlueprintTools::FindBlueprints(const FString& SearchTerm)
{
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : Assets)
	{
		FString Name = Asset.AssetName.ToString();
		FString Path = Asset.GetObjectPathString();

		if (!SearchTerm.IsEmpty() && !Name.Contains(SearchTerm, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("path"), Path);
		Obj->SetStringField(TEXT("package"), Asset.PackageName.ToString());

		// Try to get parent class info from tag
		FString ParentClass;
		if (Asset.GetTagValue(FName("ParentClass"), ParentClass))
		{
			Obj->SetStringField(TEXT("parentClass"), ParentClass);
		}

		Results.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetArrayField(TEXT("blueprints"), Results);
	Root->SetNumberField(TEXT("count"), Results.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// READ BLUEPRINT
// ============================================================

FString FNwiroIKBlueprintTools::ReadBlueprint(const FString& ArgsJson)
{
	// Accept either a plain asset path string or a JSON object with assetPath (+ optional graph).
	FString AssetPath;
	FString GraphFilter;
	{
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(R, Parsed) && Parsed.IsValid())
		{
			AssetPath = Parsed->GetStringField(TEXT("assetPath"));
			if (Parsed->HasField(TEXT("graph")))
				GraphFilter = Parsed->GetStringField(TEXT("graph"));
		}
		else
		{
			AssetPath = ArgsJson;
		}
	}

	UBlueprint* BP = LoadBP(AssetPath);
	if (!BP)
	{
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"),
			*AssetPath.Replace(TEXT("\""), TEXT("\\\"")));
	}

	// If caller asked for a specific function graph, return its full node/pin data.
	if (!GraphFilter.IsEmpty())
	{
		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (Graph && Graph->GetName().Equals(GraphFilter, ESearchCase::IgnoreCase))
			{
				TSharedPtr<FJsonObject> GObj = SerializeGraph(Graph, true);
				if (!GObj.IsValid())
					return TEXT("{\"error\": \"Failed to serialize graph\"}");
				FString Out;
				TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
				FJsonSerializer::Serialize(GObj.ToSharedRef(), W);
				return Out;
			}
		}
		for (UEdGraph* Graph : BP->UbergraphPages)
		{
			if (Graph && Graph->GetName().Equals(GraphFilter, ESearchCase::IgnoreCase))
			{
				TSharedPtr<FJsonObject> GObj = SerializeGraph(Graph, true);
				if (!GObj.IsValid())
					return TEXT("{\"error\": \"Failed to serialize graph\"}");
				FString Out;
				TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
				FJsonSerializer::Serialize(GObj.ToSharedRef(), W);
				return Out;
			}
		}
		return FString::Printf(TEXT("{\"error\": \"Graph not found: %s\"}"), *GraphFilter);
	}

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetStringField(TEXT("name"), BP->GetName());
	Root->SetStringField(TEXT("path"), BP->GetPathName());

	if (BP->ParentClass)
	{
		Root->SetStringField(TEXT("parentClass"), BP->ParentClass->GetName());
	}

	// openable: replicate the editor's "Blueprint could not be loaded because
	// it derives from an invalid class" gate so acceptance can verify
	// openability through an MCP tool — no execute_python needed. The editor
	// refuses to open when ParentClass failed to resolve on load, or the
	// GeneratedClass / its super struct is broken.
	{
		const bool bParentOk = (BP->ParentClass != nullptr) && BP->ParentClass->IsValidLowLevel();
		UClass* GenCls = BP->GeneratedClass;
		const bool bGenOk = (GenCls != nullptr) && GenCls->IsValidLowLevel()
			&& (GenCls->GetSuperClass() != nullptr);
		const bool bOpenable = bParentOk && bGenOk;
		Root->SetBoolField(TEXT("openable"), bOpenable);
		if (!bOpenable)
		{
			Root->SetStringField(TEXT("openableError"),
				!bParentOk
					? TEXT("ParentClass is null/invalid — editor will show 'derives from an invalid class'")
					: TEXT("GeneratedClass or its super struct is null/invalid — BP will not open"));
		}
	}

	// Variables and event dispatchers — dispatchers are NewVariables of the
	// MulticastDelegate pin category, so split them into their own array.
	TArray<TSharedPtr<FJsonValue>> VarArr;
	TArray<TSharedPtr<FJsonValue>> DispatcherArr;
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		const bool bIsDispatcher = Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate;
		if (bIsDispatcher)
		{
			TSharedPtr<FJsonObject> DObj = MakeShareable(new FJsonObject());
			DObj->SetStringField(TEXT("name"), Var.VarName.ToString());
			DispatcherArr.Add(MakeShareable(new FJsonValueObject(DObj.ToSharedRef())));
			continue;
		}
		TSharedPtr<FJsonObject> VObj = SerializeVariable(BP, Var.VarName);
		if (VObj.IsValid())
		{
			VarArr.Add(MakeShareable(new FJsonValueObject(VObj.ToSharedRef())));
		}
	}
	Root->SetArrayField(TEXT("variables"), VarArr);
	Root->SetArrayField(TEXT("eventDispatchers"), DispatcherArr);

	// Components (from SCS)
	TArray<TSharedPtr<FJsonValue>> CompArr;
	if (BP->SimpleConstructionScript)
	{
		for (USCS_Node* SCSNode : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (SCSNode && SCSNode->ComponentTemplate)
			{
				TSharedPtr<FJsonObject> CObj = SerializeComponent(
					SCSNode->ComponentTemplate, SCSNode->GetVariableName());
				if (CObj.IsValid())
				{
					CompArr.Add(MakeShareable(new FJsonValueObject(CObj.ToSharedRef())));
				}
			}
		}
	}
	Root->SetArrayField(TEXT("components"), CompArr);

	// Functions
	TArray<TSharedPtr<FJsonValue>> FuncArr;
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph)
		{
			TSharedRef<FJsonObject> FObj = MakeShareable(new FJsonObject());
			FObj->SetStringField(TEXT("name"), Graph->GetName());
			FuncArr.Add(MakeShareable(new FJsonValueObject(FObj)));
		}
	}
	Root->SetArrayField(TEXT("functions"), FuncArr);

	// Event Graphs
	TArray<TSharedPtr<FJsonValue>> GraphArr;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		TSharedPtr<FJsonObject> GObj = SerializeGraph(Graph, true);
		if (GObj.IsValid())
		{
			GraphArr.Add(MakeShareable(new FJsonValueObject(GObj.ToSharedRef())));
		}
	}
	Root->SetArrayField(TEXT("eventGraphs"), GraphArr);

	// Interfaces
	TArray<TSharedPtr<FJsonValue>> IntArr;
	for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
	{
		if (Iface.Interface)
		{
			TSharedRef<FJsonObject> IObj = MakeShareable(new FJsonObject());
			IObj->SetStringField(TEXT("name"), Iface.Interface->GetName());
			IntArr.Add(MakeShareable(new FJsonValueObject(IObj)));
		}
	}
	Root->SetArrayField(TEXT("interfaces"), IntArr);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// EDIT BLUEPRINT (Main Entry)
// ============================================================

FString FNwiroIKBlueprintTools::EditBlueprint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON command\"}");
	}

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));

	// Handle create action: if no "blueprint" but has "name" + "parent_class" or "parentClass", create it
	FString ActionStr = Cmd->GetStringField(TEXT("action"));
	if (BPName.IsEmpty() && (ActionStr.Equals(TEXT("CreateBlueprint"), ESearchCase::IgnoreCase) || Cmd->HasField(TEXT("parent_class")) || Cmd->HasField(TEXT("parentClass"))))
	{
		FString CreateName = Cmd->GetStringField(TEXT("name"));
		if (CreateName.IsEmpty()) CreateName = TEXT("BP_NewBlueprint");

		FString CreateParent = Cmd->GetStringField(TEXT("parent_class"));
		if (CreateParent.IsEmpty()) CreateParent = Cmd->GetStringField(TEXT("parentClass"));
		if (CreateParent.IsEmpty()) CreateParent = TEXT("Actor");

		FString CreatePath = Cmd->GetStringField(TEXT("path"));
		if (CreatePath.IsEmpty()) CreatePath = TEXT("/Game");

		FString CreateJson = FString::Printf(TEXT("{\"name\":\"%s\",\"parentClass\":\"%s\",\"path\":\"%s\"}"), *CreateName, *CreateParent, *CreatePath);
		FString CreateResult = CreateBlueprint(CreateJson);
		BPName = CreateName;

		UBlueprint* CreatedBP = LoadBP(BPName);
		if (CreatedBP)
		{
			TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("blueprint"), CreatedBP->GetName());

			TArray<TSharedPtr<FJsonValue>> MsgArr;
			MsgArr.Add(MakeShareable(new FJsonValueString(FString::Printf(TEXT("[create] Blueprint '%s' created (parent: %s)"), *CreateName, *CreateParent))));
			Result->SetArrayField(TEXT("messages"), MsgArr);

			FString Out;
			TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Result, W);
			return Out;
		}
		else
		{
			return FString::Printf(TEXT("{\"success\": false, \"error\": \"Failed to create blueprint: %s\"}"), *CreateName);
		}
	}

	if (BPName.IsEmpty())
	{
		return TEXT("{\"success\": false, \"error\": \"Missing 'blueprint' field\"}");
	}

	UBlueprint* BP = LoadBP(BPName);
	if (!BP)
	{
		// Auto-create: use parentClass if specified, otherwise default to Actor
		FString ParentClassName = Cmd->GetStringField(TEXT("parentClass"));
		if (ParentClassName.IsEmpty()) ParentClassName = Cmd->GetStringField(TEXT("parent_class"));
		if (ParentClassName.IsEmpty()) ParentClassName = TEXT("Actor");

		UE_LOG(LogNwiroBP, Log, TEXT("Blueprint '%s' not found, auto-creating with parent '%s'"), *BPName, *ParentClassName);
		FString CreateJson = FString::Printf(TEXT("{\"name\":\"%s\",\"parentClass\":\"%s\"}"), *BPName, *ParentClassName);
		CreateBlueprint(CreateJson);
		BP = LoadBP(BPName);

		if (!BP)
		{
			return FString::Printf(TEXT("{\"success\": false, \"error\": \"Failed to create blueprint: %s\"}"), *BPName);
		}
	}

	FString GraphName = Cmd->GetStringField(TEXT("graph"));
	if (GraphName.IsEmpty())
	{
		GraphName = TEXT("EventGraph");
	}

	TArray<FString> Messages;
	bool bAllOk = true;

	// Support "actions" array format: flatten into top-level keys
	// e.g. {"actions":[{"type":"add_nodes","nodes":[...]},{"type":"connect_pins","pins":[...]}]}
	TSet<FString> ActionsFlattenedAliases;
	if (Cmd->HasField(TEXT("actions")))
	{
		// Same stringified-array tolerance as the op keys (ResolveOpArray): a
		// stringified `actions` used to skip flattening SILENTLY, run zero ops,
		// and still report success:true after compile+save.
		TArray<TSharedPtr<FJsonValue>> ActionsItems;
		FString ActionsNormalizeWarning;
		FString ActionsError;
		if (!ResolveOpArray(Cmd, TEXT("actions"), ActionsItems, ActionsNormalizeWarning, ActionsError))
		{
			UE_LOG(LogNwiroBP, Warning, TEXT("EDIT_BLUEPRINT_BAD_OP_VALUE: 'actions' — %s"), *ActionsError);
			return FString::Printf(
				TEXT("{\"success\":false,\"error\":\"Key 'actions' must be a JSON array of {type, ...} action objects. %s\"}"),
				*ActionsError);
		}
		if (!ActionsNormalizeWarning.IsEmpty())
		{
			Cmd->SetArrayField(TEXT("actions"), ActionsItems);
			Messages.Add(FString::Printf(TEXT("[actions] %s"), *ActionsNormalizeWarning));
			UE_LOG(LogNwiroBP, Warning, TEXT("EDIT_BLUEPRINT_NORMALIZED_OP_VALUE: 'actions' — %s"), *ActionsNormalizeWarning);
		}
		{
			for (const TSharedPtr<FJsonValue>& ActionVal : ActionsItems)
			{
				TSharedPtr<FJsonObject> ActionObj = ActionVal->AsObject();
				if (!ActionObj.IsValid()) continue;

				FString ActionType = ActionObj->GetStringField(TEXT("type"));
				if (ActionType.IsEmpty()) continue;

				// Copy all fields from the action into the main Cmd
				for (const auto& Pair : ActionObj->Values)
				{
					if (Pair.Key != TEXT("type"))
					{
						Cmd->SetField(Pair.Key, Pair.Value);
						ActionsFlattenedAliases.Add(FString(*Pair.Key));
					}
				}

				// Also set the action type as the key with the array value
				// e.g. type="add_nodes" + nodes=[...] -> add_nodes=[...]
				for (const auto& Pair : ActionObj->Values)
				{
					if (Pair.Key != TEXT("type") && Pair.Value->Type == EJson::Array)
					{
						Cmd->SetField(ActionType, Pair.Value);
						break;
					}
				}
			}
		}
	}

	// ── Alias normalization: remap known LLM shorthand keys to canonical names ─
	TArray<FString> AliasWarnings;
	{
		struct FAliasEntry { const TCHAR* Alias; const TCHAR* Canonical; };
		static const FAliasEntry Aliases[] = {
			{ TEXT("components"),    TEXT("add_components")        },
			{ TEXT("component"),     TEXT("add_components")        },
			{ TEXT("variables"),     TEXT("add_variables")         },
			{ TEXT("variable"),      TEXT("add_variables")         },
			{ TEXT("nodes"),         TEXT("add_nodes")             },
			{ TEXT("node"),          TEXT("add_nodes")             },
			{ TEXT("connections"),   TEXT("connect_pins")          },
			{ TEXT("connection"),    TEXT("connect_pins")          },
			{ TEXT("pin_defaults"),  TEXT("set_pin_defaults")      },
			{ TEXT("defaults"),      TEXT("set_pin_defaults")      },
			{ TEXT("set_defaults"),  TEXT("set_pin_defaults")      },
			{ TEXT("functions"),     TEXT("add_functions")         },
			{ TEXT("function"),      TEXT("add_functions")         },
			{ TEXT("custom_events"), TEXT("add_custom_events")     },
			{ TEXT("events"),        TEXT("add_custom_events")     },
			{ TEXT("dispatchers"),   TEXT("add_event_dispatchers") },
			{ TEXT("interfaces"),    TEXT("add_interfaces")        },
			{ TEXT("interface"),     TEXT("add_interfaces")        },
		};
		for (const FAliasEntry& E : Aliases)
		{
			const TSharedPtr<FJsonValue>* AliasVal = Cmd->Values.Find(E.Alias);
			if (!AliasVal || !AliasVal->IsValid()) continue;

			if (Cmd->HasField(E.Canonical))
			{
				if (ActionsFlattenedAliases.Contains(E.Alias))
				{
					// This exact key was copied by actions flattening — alias is redundant, drop silently
					Cmd->RemoveField(E.Alias);
					continue;
				}
				return FString::Printf(
					TEXT("{\"success\":false,\"error\":\"Conflicting keys: both '%s' and '%s' provided. Use only '%s'.\"}"),
					E.Alias, E.Canonical, E.Canonical);
			}
			if ((*AliasVal)->Type != EJson::Array)
			{
				// Same single-object tolerance as the canonical op keys
				// (ResolveOpArray): wrap `variables: {...}` into a one-element array.
				if ((*AliasVal)->Type == EJson::Object)
				{
					TArray<TSharedPtr<FJsonValue>> Wrapped;
					Wrapped.Add(MakeShareable(new FJsonValueObject((*AliasVal)->AsObject())));
					Cmd->SetArrayField(E.Canonical, Wrapped);
					Cmd->RemoveField(E.Alias);
					AliasWarnings.Add(FString::Printf(TEXT("Normalized '%s' (single object) to '%s' array. Use %s:[{...}] next time."), E.Alias, E.Canonical, E.Canonical));
					UE_LOG(LogNwiroBP, Warning, TEXT("EDIT_BLUEPRINT_ALIAS: '%s' -> '%s' (wrapped single object)"), E.Alias, E.Canonical);
					continue;
				}
				// Same stringified-array tolerance as the canonical op keys
				// (ResolveOpArray): accept a JSON STRING that parses to an array,
				// so `variables: "[...]"` behaves like `add_variables: "[...]"`.
				if ((*AliasVal)->Type == EJson::String)
				{
					TArray<TSharedPtr<FJsonValue>> Parsed;
					TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create((*AliasVal)->AsString());
					if (FJsonSerializer::Deserialize(Reader, Parsed))
					{
						Cmd->SetArrayField(E.Canonical, Parsed);
						Cmd->RemoveField(E.Alias);
						AliasWarnings.Add(FString::Printf(TEXT("Normalized '%s' (stringified array) to '%s'. Send '%s' as a real JSON array next time."), E.Alias, E.Canonical, E.Canonical));
						UE_LOG(LogNwiroBP, Warning, TEXT("EDIT_BLUEPRINT_ALIAS: '%s' -> '%s' (parsed from JSON string)"), E.Alias, E.Canonical);
						continue;
					}
				}
				return FString::Printf(
					TEXT("{\"success\":false,\"error\":\"Key '%s' must be an array (e.g. %s:[{...}]). Got a non-array value.\"}"),
					E.Alias, E.Canonical);
			}
			Cmd->SetField(E.Canonical, *AliasVal);
			Cmd->RemoveField(E.Alias);
			AliasWarnings.Add(FString::Printf(TEXT("Normalized '%s' to '%s'. Use '%s' directly next time."), E.Alias, E.Canonical, E.Canonical));
			UE_LOG(LogNwiroBP, Warning, TEXT("EDIT_BLUEPRINT_ALIAS: '%s' -> '%s'"), E.Alias, E.Canonical);
		}
	}
	// ─────────────────────────────────────────────────────────────────────────

	// ── Clean up action-flattened helper keys that are not canonical ─────────
	// e.g. {type:"connect_pins", pins:[...]} copies "pins" to Cmd but sets canonical
	// "connect_pins". Remove non-canonical leftover keys so rejection does not fire.
	for (const FString& FlatKey : ActionsFlattenedAliases)
	{
		if (!IsKnownEditBlueprintKey(FlatKey))
			Cmd->RemoveField(FlatKey);
	}
	// ─────────────────────────────────────────────────────────────────────────

	// ── Unknown key rejection: surface to LLM so it can self-correct ────────
	{
		TArray<FString> UnknownKeys;
		for (const auto& Pair : Cmd->Values)
		{
			const FString Key(*Pair.Key);
			if (!IsKnownEditBlueprintKey(Key))
				UnknownKeys.Add(Key);
		}
		if (UnknownKeys.Num() > 0)
		{
			UnknownKeys.Sort();
			FString KeyList = FString::Join(UnknownKeys, TEXT(", "));
			UE_LOG(LogNwiroBP, Warning, TEXT("EDIT_BLUEPRINT_REJECTED unknown keys: [%s]"), *KeyList);
			return FString::Printf(
				TEXT("{\"success\":false,\"error\":\"Unknown edit_blueprint key(s): %s. Valid op keys: add_components, add_variables, add_nodes, connect_pins, set_pin_defaults, add_functions, add_custom_events, add_event_dispatchers, add_interfaces, remove_variables, rename_variables, remove_components, set_component_properties, remove_functions, remove_interfaces, remove_nodes, break_connections\"}"),
				*KeyList);
		}
	}
	// ─────────────────────────────────────────────────────────────────────────

	// Process each operation type. An op key may arrive as a real JSON array
	// or — from many models — as a JSON-ENCODED STRING (e.g.
	// add_variables: "[{\"name\":\"IsActive\",\"type\":\"bool\"}]"). The old
	// TryGetArrayField-only path type-failed on the string and SILENTLY
	// skipped the op while compile+save still ran → success:true with zero
	// mutations. Accept the stringified form with a visible compatibility
	// warning; surface any present-but-unusable value as a per-op FAILURE.
	auto RunOp = [&](const FString& Key, TFunction<FNwiroIKBPResult(const TArray<TSharedPtr<FJsonValue>>&)> Func)
	{
		if (!Cmd->HasField(Key)) return; // op not requested

		TArray<TSharedPtr<FJsonValue>> Items;
		FString NormalizeWarning;
		FString ResolveError;
		if (!ResolveOpArray(Cmd, Key, Items, NormalizeWarning, ResolveError))
		{
			Messages.Add(FString::Printf(TEXT("[%s] %s"), *Key, *ResolveError));
			UE_LOG(LogNwiroBP, Warning, TEXT("EDIT_BLUEPRINT_BAD_OP_VALUE: '%s' — %s"), *Key, *ResolveError);
			bAllOk = false;
			return;
		}
		if (!NormalizeWarning.IsEmpty())
		{
			Cmd->SetArrayField(Key, Items); // downstream readers (e.g. GetArrayCount) see the real array
			Messages.Add(FString::Printf(TEXT("[%s] %s"), *Key, *NormalizeWarning));
			UE_LOG(LogNwiroBP, Warning, TEXT("EDIT_BLUEPRINT_NORMALIZED_OP_VALUE: '%s' — %s"), *Key, *NormalizeWarning);
		}
		FNwiroIKBPResult R = Func(Items);
		Messages.Add(FString::Printf(TEXT("[%s] %s"), *Key, *R.Message));
		if (!R.bSuccess) bAllOk = false;
	};

	// Reparent (single string, not array)
	if (Cmd->HasField(TEXT("reparent")))
	{
		FString NewParent = Cmd->GetStringField(TEXT("reparent"));
		FNwiroIKBPResult R = DoReparent(BP, NewParent);
		Messages.Add(FString::Printf(TEXT("[reparent] %s"), *R.Message));
		if (!R.bSuccess) bAllOk = false;
	}

	RunOp(TEXT("add_variables"), [&](const auto& A) { return DoAddVariables(BP, A); });
	RunOp(TEXT("remove_variables"), [&](const auto& A) { return DoRemoveVariables(BP, A); });
	RunOp(TEXT("rename_variables"), [&](const auto& A) { return DoRenameVariables(BP, A); });
	RunOp(TEXT("add_components"), [&](const auto& A) { return DoAddComponents(BP, A); });
	RunOp(TEXT("remove_components"), [&](const auto& A) { return DoRemoveComponents(BP, A); });
	RunOp(TEXT("set_component_properties"), [&](const auto& A) { return DoSetComponentProperties(BP, A); });
	RunOp(TEXT("add_functions"), [&](const auto& A) { return DoAddFunctions(BP, A); });
	RunOp(TEXT("remove_functions"), [&](const auto& A) { return DoRemoveFunctions(BP, A); });
	RunOp(TEXT("add_custom_events"), [&](const auto& A) { return DoAddCustomEvents(BP, A); });
	RunOp(TEXT("add_event_dispatchers"), [&](const auto& A) { return DoAddEventDispatchers(BP, A); });
	RunOp(TEXT("add_interfaces"), [&](const auto& A) { return DoAddInterfaces(BP, A); });
	RunOp(TEXT("remove_interfaces"), [&](const auto& A) { return DoRemoveInterfaces(BP, A); });
	RunOp(TEXT("add_nodes"), [&](const auto& A) { return DoAddNodes(BP, GraphName, A); });
	RunOp(TEXT("remove_nodes"), [&](const auto& A) { return DoRemoveNodes(BP, GraphName, A); });
	RunOp(TEXT("connect_pins"), [&](const auto& A) { return DoConnectPins(BP, GraphName, A); });
	RunOp(TEXT("break_connections"), [&](const auto& A) { return DoBreakConnections(BP, GraphName, A); });
	RunOp(TEXT("set_pin_defaults"), [&](const auto& A) { return DoSetPinDefaults(BP, GraphName, A); });

	// Compile if requested (default: true)
	bool bCompile = true;
	if (Cmd->HasField(TEXT("compile")))
	{
		bCompile = Cmd->GetBoolField(TEXT("compile"));
	}

	if (bCompile)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);

		// Reconstruct Timeline nodes after compile so track pins appear
		for (UEdGraph* G : BP->UbergraphPages)
		{
			if (!G) continue;
			for (UEdGraphNode* N : G->Nodes)
			{
				if (UK2Node_Timeline* TLN = Cast<UK2Node_Timeline>(N))
				{
					TLN->ReconstructNode();
				}
			}
		}

		// Recompile again after reconstruction to pick up new pins
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);

		// Collect compiler errors so the LLM can self-correct
		TArray<FString> CompileErrors;

		// Blueprint-level messages from the compiler log (type mismatches, missing implementations, etc.)
		for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
		{
			if (Msg->GetSeverity() == EMessageSeverity::Error)
				CompileErrors.Add(Msg->ToText().ToString());
		}

		// Node-level errors — include actual pin names so the LLM can wire the correct pin
		for (UEdGraph* Graph : BP->UbergraphPages)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node->ErrorType <= EMessageSeverity::Error && !Node->ErrorMsg.IsEmpty())
				{
					TArray<FString> InPins, OutPins;
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin || Pin->PinName.IsNone()) continue;
						(Pin->Direction == EGPD_Input ? InPins : OutPins).Add(Pin->PinName.ToString());
					}
					FString PinInfo;
					if (InPins.Num() > 0 || OutPins.Num() > 0)
						PinInfo = FString::Printf(TEXT(" [pins: inputs=[%s] outputs=[%s]]"),
							*FString::Join(InPins, TEXT(",")), *FString::Join(OutPins, TEXT(",")));
					CompileErrors.AddUnique(FString::Printf(TEXT("Node '%s':%s UE: %s"),
						*Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString(), *PinInfo, *Node->ErrorMsg));
				}
			}
		}
		// Apply fix hints to known compiler error patterns
		for (FString& CE : CompileErrors)
		{
			if (CE.Contains(TEXT("Array inputs")) && CE.Contains(TEXT("must have an input wired")))
				CE += TEXT(" Hint: add a MakeArray node of the required element type and connect its output to this array pin.");
			else if (CE.Contains(TEXT("is not a")) && CE.Contains(TEXT("Target")) && CE.Contains(TEXT("must have a connection")))
				CE += TEXT(" Hint: connect this node's Target/self pin to an object of the correct type — do not leave it as blueprint self unless the blueprint inherits from that type.");
		}

		// Also check blueprint-level compiler messages via status. NOTE: a
		// downstream compile error is NOT a tool failure — edit_blueprint's
		// job is to apply the requested node/pin edits, not to guarantee the
		// caller wired every Target pin. We surface compile errors in
		// `messages` so the caller can see them and fix the wiring, but we
		// do NOT flip `bAllOk` because the requested mutations DID succeed.
		// (Old behavior: any compile error → success:false, which broke
		// scenario tests where partially-wired BPs still satisfied acceptance.)
		if (BP->Status == EBlueprintStatus::BS_Error)
		{
			if (CompileErrors.Num() == 0)
				CompileErrors.Add(TEXT("Blueprint has compile errors — check node connections and pin types"));
			FString ErrorList = FString::Join(CompileErrors, TEXT("; "));
			Messages.Add(FString::Printf(TEXT("[compile] Blueprint compiled with error(s): %s"), *ErrorList));
		}
		else if (BP->Status == EBlueprintStatus::BS_UpToDateWithWarnings)
		{
			FString WarnList = CompileErrors.Num() > 0 ? FString::Join(CompileErrors, TEXT("; ")) : TEXT("check graph for warnings");
			Messages.Add(FString::Printf(TEXT("[compile] Blueprint compiled with warning(s): %s"), *WarnList));
		}
		else
		{
			Messages.Add(TEXT("[compile] Blueprint compiled"));
		}
	}

	// Persist — actually WRITE the edited Blueprint to disk. Without this the
	// reparent/node/component edits live only in memory; the on-disk .uasset
	// keeps its old (often half-baked) generated class, so on the next editor
	// restart the asset shows "Blueprint could not be loaded because it derives
	// from an invalid class". Save only when the generated class is
	// constructible; seed the CDO first so the package serializes a complete
	// class. Skipped when ?compile=false (caller is mid-batch, will save later).
	bool bPersisted = false;
	if (bCompile)
	{
		UClass* GenCls = BP->GeneratedClass;
		if (GenCls && GenCls->IsValidLowLevel()
			&& GenCls->ClassConstructor != nullptr
			&& GenCls->ClassWithin != nullptr)
		{
			GenCls->GetDefaultObject(true);
			BP->MarkPackageDirty();
			bPersisted = UEditorAssetLibrary::SaveLoadedAsset(BP, false);
		}
	}

	// Build result JSON
	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), bAllOk);
	Result->SetBoolField(TEXT("savedToDisk"), bPersisted);
	Result->SetStringField(TEXT("blueprint"), BP->GetName());

	TArray<TSharedPtr<FJsonValue>> MsgArr;
	for (const FString& M : Messages)
		MsgArr.Add(MakeShareable(new FJsonValueString(M)));
	Result->SetArrayField(TEXT("messages"), MsgArr);

	if (AliasWarnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& WarnStr : AliasWarnings)
			WarnArr.Add(MakeShareable(new FJsonValueString(WarnStr)));
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}

	// Post-edit state summary so the agent can see what's actually in the BP
	// now and judge whether the user's request is satisfied. Without this the
	// agent has to issue a separate read_blueprint, which it often skips.
	int32 VarCount = 0, CompCount = 0, FuncCount = 0, EventNodeCount = 0;
	if (BP->NewVariables.Num() > 0) VarCount = BP->NewVariables.Num();
	if (USimpleConstructionScript* SCS = BP->SimpleConstructionScript)
	{
		CompCount = SCS->GetAllNodes().Num();
	}
	for (UEdGraph* G : BP->FunctionGraphs)
	{
		if (G && G->GetFName() != UEdGraphSchema_K2::FN_UserConstructionScript)
			FuncCount++;
	}
	for (UEdGraph* G : BP->UbergraphPages)
	{
		if (G) EventNodeCount += G->Nodes.Num();
	}
	TSharedRef<FJsonObject> State = MakeShareable(new FJsonObject());
	State->SetNumberField(TEXT("variables"), VarCount);
	State->SetNumberField(TEXT("components"), CompCount);
	State->SetNumberField(TEXT("functions"), FuncCount);
	State->SetNumberField(TEXT("eventGraphNodes"), EventNodeCount);
	State->SetStringField(TEXT("parentClass"), BP->ParentClass ? BP->ParentClass->GetName() : TEXT("(none)"));
	Result->SetObjectField(TEXT("currentState"), State);

	// nextSteps: nudge the agent toward completion if the BP still looks
	// empty for its parent class. Generic, not scenario-specific.
	TArray<TSharedPtr<FJsonValue>> Steps;
	if (VarCount == 0 && CompCount <= 1 && EventNodeCount == 0)
	{
		Steps.Add(MakeShareable(new FJsonValueString(TEXT(
			"This BP is essentially empty after the edit. If the user asked for behavior (variables, components, event logic), keep editing — your work is NOT done."))));
	}
	if (BP->ParentClass && BP->ParentClass->IsChildOf(AActor::StaticClass()) &&
		!BP->ParentClass->IsChildOf(AGameModeBase::StaticClass()))
	{
		Steps.Add(MakeShareable(new FJsonValueString(FString::Printf(TEXT(
			"To make this Actor appear in-game, call spawn_actor with blueprint='%s'. Just editing the BP does not place it in the level."), *BP->GetPathName()))));
	}
	if (Steps.Num() > 0)
		Result->SetArrayField(TEXT("nextSteps"), Steps);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// FIND BLUEPRINT NODES
// ============================================================

FString FNwiroIKBlueprintTools::FindBlueprintNodes(const FString& Query, const FString& BlueprintPath)
{
	UBlueprint* BP = nullptr;
	if (!BlueprintPath.IsEmpty())
	{
		BP = LoadBP(BlueprintPath);
	}

	TArray<TSharedPtr<FJsonValue>> Results;

	const int32 MaxResults = 50;
	TSet<FString> Seen;

	// Tokenize the query so multi-word searches like "Add float" or "Sin math"
	// can hit "Add_DoubleDouble" / "Sin_FloatFloat". ANY token match counts;
	// requiring ALL was too strict because the LLM mixes display/internal names.
	// Also expand float<->double so "float" matches "Double" (UE 5.5+ promoted
	// math operators to double precision).
	TArray<FString> QueryTokens;
	if (!Query.IsEmpty())
	{
		TArray<FString> Raw;
		Query.ParseIntoArray(Raw, TEXT(" "), /*CullEmpty*/ true);
		for (const FString& T : Raw)
		{
			QueryTokens.Add(T);
			if (T.Equals(TEXT("float"), ESearchCase::IgnoreCase))  QueryTokens.Add(TEXT("double"));
			if (T.Equals(TEXT("double"), ESearchCase::IgnoreCase)) QueryTokens.Add(TEXT("float"));
		}
	}

	// Walk every loaded UClass and inspect its declared UFunctions. This is
	// far more reliable than going through FBlueprintActionDatabase, which
	// stores function spawners under several different subclasses and often
	// returns nothing for KismetMathLibrary on a fresh load.
	//
	// We accept any function flagged BlueprintCallable / BlueprintPure /
	// BlueprintEvent. The class name is reported alongside so the LLM knows
	// where to find it (e.g. "KismetMathLibrary").
	const uint64 BPMask = FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_BlueprintEvent;

	// Score a hit so we can sort later. Higher = more relevant.
	//   +100  function name starts with the query
	//   +50   function name contains the query as a whole token
	//   +10   any token from the (expanded) query is a substring of the function
	//   +20   class is one of the well-known math/system libraries
	auto ScoreHit = [&Query, &QueryTokens](const FString& FuncName, const FString& ClassName) -> int32
	{
		int32 Score = 0;

		if (!Query.IsEmpty())
		{
			if (FuncName.StartsWith(Query, ESearchCase::IgnoreCase)) Score += 100;
			else if (FuncName.Contains(Query, ESearchCase::IgnoreCase)) Score += 50;
		}

		for (const FString& Tok : QueryTokens)
		{
			if (FuncName.Contains(Tok, ESearchCase::IgnoreCase)) { Score += 10; break; }
		}

		if (ClassName == TEXT("KismetMathLibrary")    ||
			ClassName == TEXT("KismetSystemLibrary")  ||
			ClassName == TEXT("KismetStringLibrary")  ||
			ClassName == TEXT("KismetTextLibrary")    ||
			ClassName == TEXT("GameplayStatics"))
		{
			Score += 20;
		}

		return Score;
	};

	struct FHit { FString FuncName; FString ClassName; int32 Score; };
	TArray<FHit> Hits;

	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Class = *ClassIt;
		if (!Class) continue;
		// Skip skeleton/REINST classes — stale duplicates
		if (Class->GetName().StartsWith(TEXT("SKEL_")) || Class->GetName().StartsWith(TEXT("REINST_"))) continue;

		const FString ClassName = Class->GetName();

		for (TFieldIterator<UFunction> FnIt(Class, EFieldIteratorFlags::ExcludeSuper); FnIt; ++FnIt)
		{
			UFunction* Fn = *FnIt;
			if (!Fn) continue;
			if (!(Fn->FunctionFlags & BPMask)) continue;

			const FString FuncName = Fn->GetName();

			// Function-name only match — class names like "BlueprintFns"
			// were producing false positives for "Print", "Sin", etc.
			if (QueryTokens.Num() > 0)
			{
				bool bAnyTokenHit = false;
				for (const FString& Tok : QueryTokens)
				{
					if (FuncName.Contains(Tok, ESearchCase::IgnoreCase)) { bAnyTokenHit = true; break; }
				}
				if (!bAnyTokenHit) continue;
			}

			const FString DedupeKey = ClassName + TEXT("::") + FuncName;
			if (Seen.Contains(DedupeKey)) continue;
			Seen.Add(DedupeKey);

			Hits.Add({ FuncName, ClassName, ScoreHit(FuncName, ClassName) });
		}
	}

	// Sort by score descending so the most relevant hits come first
	Hits.Sort([](const FHit& A, const FHit& B) { return A.Score > B.Score; });

	const int32 Take = FMath::Min(Hits.Num(), MaxResults);
	for (int32 i = 0; i < Take; ++i)
	{
		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("function"), Hits[i].FuncName);
		Obj->SetStringField(TEXT("class"), Hits[i].ClassName);
		Results.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	int32 Count = Results.Num();

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetArrayField(TEXT("nodes"), Results);
	Root->SetNumberField(TEXT("count"), Results.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// ASSET LOADING
// ============================================================

UBlueprint* FNwiroIKBlueprintTools::LoadBP(const FString& PathOrName)
{
	// Hard cap — UE asset paths are bounded (rarely >200 chars). A 100KB
	// path string crashed CoreUObject inside StaticFindObject; reject
	// anything that's obviously not a real path before we hand it down.
	// Discovered by fuzz/blueprint-1: `read_blueprint{assetPath: "A"*100000}`
	// killed the editor.
	if (PathOrName.IsEmpty() || PathOrName.Len() > 1024) return nullptr;

	// Detect whether the caller gave us a FULL asset path (e.g. starts with
	// "/Game/...") vs an unqualified name. For full paths we must NEVER
	// smart-fallback to a similar BP — that returns the wrong asset and
	// produces VERY confusing diagnostics for accept modules that check
	// canonical paths in order (the second canonical path silently shadows
	// the first one). Smart fallback only makes sense for short names the
	// LLM might have abbreviated.
	const bool bIsFullPath = PathOrName.StartsWith(TEXT("/Game/"));

	// Try direct path first
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *PathOrName);
	if (BP) return BP;

	// Also try with the asset suffix ("/Game/Foo/Bar" → "/Game/Foo/Bar.Bar")
	// for callers that pass the package path without the object name.
	if (bIsFullPath && !PathOrName.Contains(TEXT(".")))
	{
		int32 Slash; PathOrName.FindLastChar('/', Slash);
		const FString AssetName = PathOrName.RightChop(Slash + 1);
		const FString WithSuffix = PathOrName + TEXT(".") + AssetName;
		BP = LoadObject<UBlueprint>(nullptr, *WithSuffix);
		if (BP) return BP;
	}

	// Try with /Game/ prefix
	if (!PathOrName.StartsWith(TEXT("/")))
	{
		FString FullPath = TEXT("/Game/") + PathOrName;
		BP = LoadObject<UBlueprint>(nullptr, *FullPath);
		if (BP) return BP;
	}

	// Full path that didn't load AND wasn't a suffix-less package path?
	// Don't fall back — the caller said "this exact asset", and any other
	// asset would be the wrong answer.
	if (bIsFullPath) return nullptr;

	// Search by name in asset registry
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	// Exact name match
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetName.ToString().Equals(PathOrName, ESearchCase::IgnoreCase))
		{
			return Cast<UBlueprint>(NwiroSafeRegistryLoad(Asset));
		}
	}

	// Partial name match (e.g., "ThirdPersonCharacter" matches "BP_ThirdPersonCharacter")
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetName.ToString().Contains(PathOrName, ESearchCase::IgnoreCase))
		{
			return Cast<UBlueprint>(NwiroSafeRegistryLoad(Asset));
		}
	}

	// Smart fallback: if the name contains "Character" or "Pawn", find any character blueprint
	if (PathOrName.Contains(TEXT("Character"), ESearchCase::IgnoreCase) || PathOrName.Contains(TEXT("Pawn"), ESearchCase::IgnoreCase))
	{
		UBlueprint* BestMatch = nullptr;
		for (const FAssetData& Asset : Assets)
		{
			FString Name = Asset.AssetName.ToString();
			// Skip GameMode, Controller, etc.
			if (Name.Contains(TEXT("GameMode")) || Name.Contains(TEXT("Controller")) || Name.Contains(TEXT("HUD")))
			{
				continue;
			}

			// Only consider plain Blueprints — WidgetBlueprint/AnimBlueprint
			// etc. don't derive from Character/Pawn anyway, and forcing a
			// load on a compile-failed Blueprint can cascade into UE-killing
			// CDO failures when its skeleton class is broken.
			const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
			if (ClassName != TEXT("Blueprint")) continue;

			// Use the GeneratedClass tag from the asset registry instead of
			// loading the asset just to ask "is this a Character/Pawn?".
			// Forcing GetAsset() on a compile-failed BP triggers
			// `Ensure condition failed: ClassDefaultObject != nullptr`
			// (Class.cpp:5849) and in some sessions escalates to a process
			// exit. The asset-registry tag has the answer without loading.
			FString NativeParent;
			if (Asset.GetTagValue(TEXT("NativeParentClass"), NativeParent) && !NativeParent.IsEmpty())
			{
				const bool bIsCharOrPawn =
					NativeParent.Contains(TEXT("Character")) ||
					NativeParent.Contains(TEXT("Pawn"));
				if (!bIsCharOrPawn) continue;
			}
			// Only load if the candidate is already in memory — never force a
			// disk load here. The smart-fallback is "find an existing similar
			// BP", and "existing" means loaded.
			if (!Asset.IsAssetLoaded()) continue;
			UBlueprint* CandidateBP = Cast<UBlueprint>(Asset.GetAsset());
			if (CandidateBP && CandidateBP->ParentClass)
			{
				if (CandidateBP->ParentClass->IsChildOf(ACharacter::StaticClass()) ||
					CandidateBP->ParentClass->IsChildOf(APawn::StaticClass()))
				{
					UE_LOG(LogNwiroBP, Log, TEXT("Smart fallback: '%s' not found, using '%s' instead"), *PathOrName, *Name);
					BestMatch = CandidateBP;
					break;
				}
			}
		}
		if (BestMatch) return BestMatch;
	}

	return nullptr;
}

// ============================================================
// GRAPH HELPERS
// ============================================================

UEdGraph* FNwiroIKBlueprintTools::FindGraph(UBlueprint* BP, const FString& GraphName)
{
	if (!BP) return nullptr;

	// Check event graphs
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}

	// Check function graphs
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}

	// Check interface implementation graphs — created via create_function_graph
	// with override=true on a function from an implemented interface.
	for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
	{
		for (UEdGraph* Graph : Iface.Graphs)
		{
			if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}
	}

	return nullptr;
}

// Strip everything that isn't a letter or digit and lowercase the rest, so
// "Event BeginPlay", "event_begin_play", "EventBeginPlay" and "eventbeginplay"
// all collapse to the same canonical key.
static FString NormalizeRefKey(const FString& In)
{
	FString Out;
	Out.Reserve(In.Len());
	for (TCHAR C : In)
	{
		if (FChar::IsAlnum(C)) Out.AppendChar(FChar::ToLower(C));
	}
	return Out;
}

UEdGraphNode* FNwiroIKBlueprintTools::FindNodeByRef(UEdGraph* Graph, const FString& Ref)
{
	if (!Graph) return nullptr;

	// 1. Exact session reference (set by add_nodes via the LLM-supplied ref).
	if (const FNwiroIKNodeRef* Found = NodeRefs.Find(Ref))
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == Found->NodeGuid) return Node;
		}
	}

	// 2. Normalized session reference — handles `printString` vs `PrintString` vs `print_string`.
	const FString NormRef = NormalizeRefKey(Ref);
	if (!NormRef.IsEmpty())
	{
		for (const auto& Pair : NodeRefs)
		{
			if (NormalizeRefKey(Pair.Key) == NormRef)
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (Node && Node->NodeGuid == Pair.Value.NodeGuid) return Node;
				}
			}
		}
	}

	// 3. GUID — accept any of the FGuid string formats UE supports.
	{
		FGuid TestGuid;
		if (FGuid::Parse(Ref, TestGuid))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->NodeGuid == TestGuid) return Node;
			}
		}
	}

	// 4. Match by normalized node title (covers "Event BeginPlay" / "Print String" / "Delay" etc.).
	if (!NormRef.IsEmpty())
	{
		// 4a. exact normalized title equality
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			const FString Title = Node->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
			if (NormalizeRefKey(Title) == NormRef) return Node;
		}
		// 4b. partial substring match against full title
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			const FString FullTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			if (NormalizeRefKey(FullTitle).Contains(NormRef)) return Node;
		}
	}

	return nullptr;
}

UEdGraphPin* FNwiroIKBlueprintTools::FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Dir)
{
	if (!Node) return nullptr;

	// Common synonyms the LLM mixes up. Map every input alias to a list of
	// candidate canonical names so we try all of them.
	auto Aliases = [](const FString& In) -> TArray<FString>
	{
		const FString N = NormalizeRefKey(In); // strip non-alnum, lowercase
		TArray<FString> Out = { In };
		if (N == TEXT("execute") || N == TEXT("exec") || N == TEXT("in") || N == TEXT("input"))
		{
			Out.Append({ TEXT("execute"), TEXT("exec"), TEXT("then") });
		}
		else if (N == TEXT("then") || N == TEXT("next") || N == TEXT("out") || N == TEXT("output") || N == TEXT("completed"))
		{
			Out.Append({ TEXT("then"), TEXT("Completed") });
		}
		else if (N == TEXT("self") || N == TEXT("target"))
		{
			Out.Append({ TEXT("self"), TEXT("Target") });
		}
		else if (N == TEXT("returnvalue") || N == TEXT("return") || N == TEXT("result"))
		{
			Out.Append({ TEXT("ReturnValue") });
		}
		// Branch node pin aliases — docstring lists True/False but actual UE pins are then/else.
		else if (N == TEXT("true"))
		{
			Out.Append({ TEXT("then") });
		}
		else if (N == TEXT("false"))
		{
			Out.Append({ TEXT("else") });
		}
		return Out;
	};
	const TArray<FString> Candidates = Aliases(PinName);

	auto MatchesNorm = [](const FString& Pin, const FString& Wanted) -> bool
	{
		return NormalizeRefKey(Pin) == NormalizeRefKey(Wanted);
	};

	// 1. Exact / normalized name or friendly-name match across all candidate aliases.
	for (const FString& Cand : Candidates)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			const bool bMatch = MatchesNorm(Pin->PinName.ToString(), Cand)
				|| MatchesNorm(Pin->PinFriendlyName.ToString(), Cand);
			if (bMatch && (Dir == EGPD_MAX || Pin->Direction == Dir)) return Pin;
		}
	}

	// 2. Substring fallback (partial match) using the original input only.
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		if (Pin->PinName.ToString().Contains(PinName, ESearchCase::IgnoreCase))
		{
			if (Dir == EGPD_MAX || Pin->Direction == Dir) return Pin;
		}
	}

	return nullptr;
}

void FNwiroIKBlueprintTools::StoreNodeRef(const FString& Ref, UEdGraphNode* Node, const FString& GraphName)
{
	if (!Ref.IsEmpty() && Node)
	{
		FNwiroIKNodeRef NR;
		NR.NodeGuid = Node->NodeGuid;
		NR.GraphName = GraphName;
		NodeRefs.Add(Ref, NR);
	}
}

void FNwiroIKBlueprintTools::ClearNodeRefs()
{
	NodeRefs.Empty();
}

// ============================================================
// TYPE PARSING
// ============================================================

FEdGraphPinType FNwiroIKBlueprintTools::ParsePinType(const TSharedPtr<FJsonObject>& TypeObj)
{
	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean; // default

	if (!TypeObj.IsValid()) return PinType;

	FString Base = TypeObj->GetStringField(TEXT("base")).ToLower();

	if (Base == TEXT("bool") || Base == TEXT("boolean"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	}
	else if (Base == TEXT("byte"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
	}
	else if (Base == TEXT("int") || Base == TEXT("integer") || Base == TEXT("int32"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	}
	else if (Base == TEXT("int64"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
	}
	else if (Base == TEXT("float"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = TEXT("float");
	}
	else if (Base == TEXT("double"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = TEXT("double");
	}
	else if (Base == TEXT("string") || Base == TEXT("fstring"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_String;
	}
	else if (Base == TEXT("name") || Base == TEXT("fname"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
	}
	else if (Base == TEXT("text") || Base == TEXT("ftext"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
	}
	else if (Base == TEXT("vector") || Base == TEXT("fvector"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	}
	else if (Base == TEXT("rotator") || Base == TEXT("frotator"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
	}
	else if (Base == TEXT("transform") || Base == TEXT("ftransform"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
	}
	else if (Base == TEXT("linearcolor") || Base == TEXT("color"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
	}
	else if (Base == TEXT("object"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
		FString SubType = TypeObj->GetStringField(TEXT("subtype"));
		if (!SubType.IsEmpty())
		{
			UClass* ObjClass = FindFirstObject<UClass>(*SubType);
			if (!ObjClass)
				ObjClass = StaticLoadClass(UObject::StaticClass(), nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *SubType));
			if (!ObjClass)
				ObjClass = StaticLoadClass(UObject::StaticClass(), nullptr, *SubType);
			if (ObjClass)
			{
				PinType.PinSubCategoryObject = ObjClass;
			}
		}
	}
	else if (Base == TEXT("class"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
		FString SubType = TypeObj->GetStringField(TEXT("subtype"));
		if (!SubType.IsEmpty())
		{
			UClass* ObjClass = FindFirstObject<UClass>(*SubType);
			if (!ObjClass)
				ObjClass = StaticLoadClass(UObject::StaticClass(), nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *SubType));
			if (!ObjClass)
				ObjClass = StaticLoadClass(UObject::StaticClass(), nullptr, *SubType);
			if (ObjClass)
				PinType.PinSubCategoryObject = ObjClass;
		}
	}
	else if (Base == TEXT("struct"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		FString SubType = TypeObj->GetStringField(TEXT("subtype"));
		if (!SubType.IsEmpty())
		{
			UScriptStruct* Struct = FindFirstObject<UScriptStruct>( *SubType);
			if (Struct)
			{
				PinType.PinSubCategoryObject = Struct;
			}
		}
	}
	else if (Base == TEXT("enum"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		FString SubType = TypeObj->GetStringField(TEXT("subtype"));
		if (!SubType.IsEmpty())
		{
			UEnum* Enum = FindFirstObject<UEnum>( *SubType);
			if (Enum)
			{
				PinType.PinSubCategoryObject = Enum;
			}
		}
	}

	// Unknown 'base' fallback — treat as an Object/Class/Struct/Enum name
	// instead of silently defaulting to bool. The LLM very often passes the
	// raw type name (e.g. "SoundBase", "StaticMesh", "Pawn", "Vector",
	// "ELightUnits") without wrapping it in {base:"object", subtype:"..."}.
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean
		&& !Base.IsEmpty() && Base != TEXT("bool") && Base != TEXT("boolean"))
	{
		const FString OrigBase = TypeObj->GetStringField(TEXT("base")); // preserve original case
		// Try with bare name and with U-prefix (Unreal class naming).
		const FString TryNames[] = { OrigBase, FString(TEXT("U")) + OrigBase, FString(TEXT("F")) + OrigBase, FString(TEXT("E")) + OrigBase };
		for (const FString& Try : TryNames)
		{
			if (UClass* Cls = FindFirstObject<UClass>(*Try))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				PinType.PinSubCategoryObject = Cls;
				break;
			}
			if (UScriptStruct* Struct = FindFirstObject<UScriptStruct>(*Try))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = Struct;
				break;
			}
			if (UEnum* Enum = FindFirstObject<UEnum>(*Try))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
				PinType.PinSubCategoryObject = Enum;
				break;
			}
		}
		// Last resort: StaticLoadClass against /Script/Engine.<Name>.
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
		{
			if (UClass* Cls = StaticLoadClass(UObject::StaticClass(), nullptr,
				*FString::Printf(TEXT("/Script/Engine.%s"), *OrigBase)))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				PinType.PinSubCategoryObject = Cls;
			}
		}
	}

	// Container type
	FString Container = TypeObj->GetStringField(TEXT("container")).ToLower();
	if (Container == TEXT("array"))
	{
		PinType.ContainerType = EPinContainerType::Array;
	}
	else if (Container == TEXT("set"))
	{
		PinType.ContainerType = EPinContainerType::Set;
	}
	else if (Container == TEXT("map"))
	{
		PinType.ContainerType = EPinContainerType::Map;
	}

	return PinType;
}

// ============================================================
// ADD VARIABLES
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoAddVariables(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	int32 Added = 0;
	TArray<FString> Errors;
	TArray<FString> Details;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid())
		{
			// A non-object entry (e.g. a plain string in the array) used to be
			// SILENTLY dropped — "Added 0 variable(s)" with success:true.
			Errors.Add(TEXT("Entry is not an object — each variable must be an object with required fields {name, type}, e.g. {\"name\":\"Health\",\"type\":\"float\"}"));
			continue;
		}

		FString VarName = Obj->GetStringField(TEXT("name"));
		if (VarName.IsEmpty()) { Errors.Add(TEXT("Missing variable name")); continue; }

		FEdGraphPinType PinType;
		bool bTypeResolved = false;
		FString RequestedBase; // F1: the type name the caller asked for, to catch an unresolved type silently defaulting to Boolean
		const TSharedPtr<FJsonObject>* TypeObj;
		if (Obj->TryGetObjectField(TEXT("type"), TypeObj))
		{
			PinType = ParsePinType(*TypeObj);
			RequestedBase = (*TypeObj)->GetStringField(TEXT("base"));
			bTypeResolved = true;
		}
		else
		{
			// Default: try string type field
			FString TypeStr = Obj->GetStringField(TEXT("type"));
			if (!TypeStr.IsEmpty())
			{
				TSharedPtr<FJsonObject> SimpleType = MakeShareable(new FJsonObject());
				SimpleType->SetStringField(TEXT("base"), TypeStr);
				FString SubType = GetFieldNormalized(Obj, TEXT("subtype"));
				if (SubType.IsEmpty() && (TypeStr == TEXT("object") || TypeStr == TEXT("class") || TypeStr == TEXT("struct") || TypeStr == TEXT("enum")))
					SubType = GetFieldNormalized(Obj, TEXT("objectclass")); // covers object_class / objectClass / ObjectClass
				if (!SubType.IsEmpty()) SimpleType->SetStringField(TEXT("subtype"), SubType);
				PinType = ParsePinType(SimpleType);
				RequestedBase = TypeStr;
				bTypeResolved = true;
			}
		}
		if (!bTypeResolved)
		{
			// A missing 'type' used to default-construct the pin type silently
			// (an implicit bool); require it so the model states its intent.
			Errors.Add(FString::Printf(TEXT("Variable '%s': missing required 'type' (e.g. bool, int, float, string, vector, rotator, object)"), *VarName));
			continue;
		}

		// F1: an unrecognized 'type' used to fall through ParsePinType's PC_Boolean
		// default (after BOTH the known-type switch AND the class/struct/enum name
		// fallback failed) and get created as a silent Boolean with success:true.
		// PinType can only END at PC_Boolean via an explicit bool/boolean request
		// or an unresolved fall-through, so detect the latter and refuse honestly
		// (mirrors the missing-type path above). A real name like "StaticMesh"
		// resolves to PC_Object via the fallback and never reaches here.
		{
			const FString RB = RequestedBase.ToLower();
			if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean
				&& !RB.IsEmpty() && RB != TEXT("bool") && RB != TEXT("boolean"))
			{
				Errors.Add(FString::Printf(TEXT("Variable '%s': unknown type '%s' — not a recognized Blueprint type or a resolvable class/struct/enum name. Use one of: bool, byte, int, int64, float, double, string, name, text, vector, rotator, transform, linearcolor, color, or an object/struct/enum/class via {\"base\":\"object\",\"subtype\":\"<ClassName>\"}. (Refusing to silently create it as a Boolean.)"), *VarName, *RequestedBase));
				continue;
			}
		}

		FName VarFName(*VarName);

		// Check if variable already exists
		bool bExists = false;
		for (const FBPVariableDescription& Existing : BP->NewVariables)
		{
			if (Existing.VarName == VarFName)
			{
				bExists = true;
				break;
			}
		}
		if (bExists) { Errors.Add(FString::Printf(TEXT("Variable '%s' already exists. To change its type, call remove_variables with this name first, then re-add with the correct subtype."), *VarName)); continue; }

		bool bAdded = FBlueprintEditorUtils::AddMemberVariable(BP, VarFName, PinType);
		if (!bAdded) { Errors.Add(FString::Printf(TEXT("Failed to add variable '%s'"), *VarName)); continue; }

		// Set default value if provided
		FString DefaultVal = Obj->GetStringField(TEXT("default"));
		if (DefaultVal.IsEmpty()) DefaultVal = GetFieldNormalized(Obj, TEXT("defaultValue"));
		if (DefaultVal.IsEmpty()) DefaultVal = GetFieldNormalized(Obj, TEXT("initialValue"));
		if (!DefaultVal.IsEmpty())
		{
			int32 DefaultIdx = FBlueprintEditorUtils::FindNewVariableIndex(BP, VarFName);
			if (DefaultIdx != INDEX_NONE)
			{
				BP->NewVariables[DefaultIdx].DefaultValue = DefaultVal;
			}
		}

		// Set flags
		if (Obj->HasField(TEXT("replicated")) && Obj->GetBoolField(TEXT("replicated")))
		{
			int32 VarIdx = FBlueprintEditorUtils::FindNewVariableIndex(BP, VarFName);
			if (VarIdx != INDEX_NONE)
			{
				BP->NewVariables[VarIdx].PropertyFlags |= CPF_Net;
			}
		}

		if (Obj->HasField(TEXT("expose_on_spawn")) && Obj->GetBoolField(TEXT("expose_on_spawn")))
		{
			// Expose on Spawn requires Instance Editable — set both flags
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, VarFName, nullptr,
				FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
			// Set CPF_Edit (Instance Editable) which is required for Expose on Spawn to work
			int32 ExposeIdx = FBlueprintEditorUtils::FindNewVariableIndex(BP, VarFName);
			if (ExposeIdx != INDEX_NONE)
			{
				BP->NewVariables[ExposeIdx].PropertyFlags |= CPF_Edit;
				BP->NewVariables[ExposeIdx].PropertyFlags &= ~CPF_DisableEditOnInstance;
			}
		}

		if (Obj->HasField(TEXT("save_game")) && Obj->GetBoolField(TEXT("save_game")))
		{
			int32 VarIdx = FBlueprintEditorUtils::FindNewVariableIndex(BP, VarFName);
			if (VarIdx != INDEX_NONE)
			{
				BP->NewVariables[VarIdx].PropertyFlags |= CPF_SaveGame;
			}
		}

		if (Obj->HasField(TEXT("category")))
		{
			FBlueprintEditorUtils::SetBlueprintVariableCategory(BP, VarFName, nullptr,
				FText::FromString(Obj->GetStringField(TEXT("category"))));
		}

		if (Obj->HasField(TEXT("tooltip")))
		{
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, VarFName, nullptr,
				FBlueprintMetadata::MD_Tooltip, Obj->GetStringField(TEXT("tooltip")));
		}

		Added++;
		{
			const FString Cat = PinType.PinCategory.ToString();
			const bool bIsRef = (PinType.PinCategory == UEdGraphSchema_K2::PC_Object  ||
			                     PinType.PinCategory == UEdGraphSchema_K2::PC_Class   ||
			                     PinType.PinCategory == UEdGraphSchema_K2::PC_Struct  ||
			                     PinType.PinCategory == UEdGraphSchema_K2::PC_Enum);
			if (bIsRef)
			{
				FString Requested = GetFieldNormalized(Obj, TEXT("subtype"));
				if (Requested.IsEmpty()) Requested = GetFieldNormalized(Obj, TEXT("objectclass"));
				if (Requested.IsEmpty()) Requested = GetFieldNormalized(Obj, TEXT("objecttype"));
				if (PinType.PinSubCategoryObject.IsValid())
				{
					const FString Resolved = PinType.PinSubCategoryObject->GetName();
					const FString Status = (Requested.IsEmpty() || Requested.Equals(Resolved, ESearchCase::IgnoreCase))
					                       ? TEXT("resolved") : TEXT("fallback");
					Details.Add(FString::Printf(TEXT("'%s' [%s] %s:%s"), *VarName, *Status, *Cat, *Resolved));
				}
				else
				{
					const FString Req = Requested.IsEmpty() ? TEXT("none") : Requested;
					Errors.Add(FString::Printf(TEXT("'%s' [not_resolved] %s subtype:'%s' unrecognized — stored as UObject; re-add with subtype:'%s'"), *VarName, *Cat, *Req, *Req));
				}
			}
			else
			{
				Details.Add(FString::Printf(TEXT("'%s' [resolved] %s"), *VarName, *Cat));
			}
		}
		UE_LOG(LogNwiroBP, Log, TEXT("Added variable: %s"), *VarName);
	}

	FString Msg = FString::Printf(TEXT("Added %d of %d variable(s)"), Added, Items.Num());
	if (Details.Num() > 0) Msg += TEXT(": ") + FString::Join(Details, TEXT(", "));
	if (Errors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));

	// Per-op accounting: requested > applied is a FAILURE even if no error
	// string was recorded (belt-and-braces against any future silent skip).
	return (Errors.Num() == 0 && Added == Items.Num()) ? FNwiroIKBPResult::Ok(Msg) : FNwiroIKBPResult::Fail(Msg);
}

// ============================================================
// REMOVE VARIABLES
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoRemoveVariables(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	int32 Removed = 0;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		FString VarName;
		if (Item->Type == EJson::String)
		{
			VarName = Item->AsString();
		}
		else if (Item->Type == EJson::Object && Item->AsObject().IsValid())
		{
			VarName = Item->AsObject()->GetStringField(TEXT("name"));
		}

		if (!VarName.IsEmpty())
		{
			FBlueprintEditorUtils::RemoveMemberVariable(BP, FName(*VarName));
			Removed++;
		}
	}

	return FNwiroIKBPResult::Ok(FString::Printf(TEXT("Removed %d variable(s)"), Removed));
}

// ============================================================
// ADD COMPONENTS
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoAddComponents(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	if (!BP->SimpleConstructionScript)
	{
		return FNwiroIKBPResult::Fail(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	int32 Added = 0;
	TArray<FString> Errors;
	TArray<FString> Warnings;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		FString CompName = Obj->GetStringField(TEXT("name"));
		// Bounded alias set: models frequently guess componentName/componentType/
		// componentClass (observed live with gemma, which also ignored the
		// corrective error text twice — docs alone don't save weak models).
		if (CompName.IsEmpty()) CompName = GetFieldNormalized(Obj, TEXT("componentname"));
		FString ClassName = Obj->GetStringField(TEXT("class"));
		if (ClassName.IsEmpty()) ClassName = Obj->GetStringField(TEXT("type"));
		if (ClassName.IsEmpty()) ClassName = GetFieldNormalized(Obj, TEXT("componenttype"));
		if (ClassName.IsEmpty()) ClassName = GetFieldNormalized(Obj, TEXT("componentclass"));
		FString ParentName = Obj->GetStringField(TEXT("parent"));

		if (CompName.IsEmpty() || ClassName.IsEmpty())
		{
			Errors.Add(TEXT("Component needs 'name' and either 'class' or 'type'. Example: {\"name\":\"CubeMesh\",\"class\":\"StaticMeshComponent\",\"properties\":{\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\"}}"));
			continue;
		}

		// Find the component class — try multiple name variations and KEEP
		// trying if the first hit isn't an ActorComponent. Without this, asking
		// for "StaticMesh" returned the asset class UStaticMesh (which exists
		// but isn't a component) and silently dropped the request.
		UClass* CompClass = nullptr;
		const TArray<FString> ClassTries = {
			ClassName,
			TEXT("U") + ClassName,
			ClassName + TEXT("Component"),
			TEXT("U") + ClassName + TEXT("Component"),
		};
		for (const FString& Try : ClassTries)
		{
			UClass* Candidate = FindFirstObject<UClass>(*Try);
			if (Candidate && Candidate->IsChildOf(UActorComponent::StaticClass()))
			{
				CompClass = Candidate;
				break;
			}
		}

		if (!CompClass)
		{
			Errors.Add(FString::Printf(TEXT("Component class not found: %s (tried: %s)"), *ClassName, *FString::Join(ClassTries, TEXT(", "))));
			continue;
		}

		USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(CompClass, FName(*CompName));
		if (!NewNode)
		{
			Errors.Add(FString::Printf(TEXT("Failed to create SCS node for: %s"), *CompName));
			continue;
		}

		// Check if makeRoot requested — if so, skip normal attachment
		bool bWantRoot = false;
		if (Obj->HasField(TEXT("makeRoot")))
		{
			TSharedPtr<FJsonValue> MRVal = Obj->TryGetField(TEXT("makeRoot"));
			if (MRVal.IsValid())
			{
				if (MRVal->Type == EJson::Boolean) bWantRoot = MRVal->AsBool();
				else if (MRVal->Type == EJson::String) bWantRoot = MRVal->AsString().Equals(TEXT("true"), ESearchCase::IgnoreCase);
			}
		}

		if (bWantRoot)
		{
			// CRITICAL: a parent class can already provide a NATIVE root component
			// (Character -> CapsuleComponent, etc.). You cannot replace a native
			// root from the SCS — adding this component as a second SCS root makes
			// FinishSpawning assert SceneRootComponent->GetOwner()==this
			// (Actor.cpp:3854) and crash UE at spawn time. So when the parent has
			// a native root, ignore makeRoot and nest this component UNDER the
			// native root instead.
			bool bParentHasNativeRoot = false;
			if (BP->ParentClass)
			{
				if (AActor* CDO = BP->ParentClass->GetDefaultObject<AActor>())
				{
					if (USceneComponent* NR = CDO->GetRootComponent())
					{
						bParentHasNativeRoot = NR->CreationMethod == EComponentCreationMethod::Native;
					}
				}
			}

			if (bParentHasNativeRoot && CompClass->IsChildOf(USceneComponent::StaticClass()))
			{
				// Attach under the native root rather than competing with it.
				BP->SimpleConstructionScript->AddNode(NewNode);
				if (AActor* CDO = BP->ParentClass->GetDefaultObject<AActor>())
				{
					if (USceneComponent* NR = CDO->GetRootComponent())
					{
						NewNode->SetParent(NR);
					}
				}
				goto NodeAttached;
			}

			// No native root (plain Actor): replace the SCS DefaultSceneRoot and
			// set this component as the real root.
			TArray<USCS_Node*> OldRoots = BP->SimpleConstructionScript->GetRootNodes();
			for (USCS_Node* OldRoot : OldRoots)
			{
				if (OldRoot->ComponentTemplate && OldRoot->ComponentTemplate->GetFName() == TEXT("DefaultSceneRoot"))
				{
					TArray<USCS_Node*> OldChildren = OldRoot->GetChildNodes();
					for (USCS_Node* Child : OldChildren)
					{
						OldRoot->RemoveChildNode(Child);
						NewNode->AddChildNode(Child);
					}
					BP->SimpleConstructionScript->RemoveNode(OldRoot);
				}
			}
			BP->SimpleConstructionScript->AddNode(NewNode);
			goto NodeAttached;
		}

		// Attach to parent or root
		if (!ParentName.IsEmpty())
		{
			for (USCS_Node* Existing : BP->SimpleConstructionScript->GetAllNodes())
			{
				if (Existing && Existing->GetVariableName().ToString().Equals(ParentName, ESearchCase::IgnoreCase))
				{
					Existing->AddChildNode(NewNode);
					goto NodeAttached;
				}
			}
			// Parent not found, attach to root
		}

		// Default: attach to existing SCS root, or native root component
		{
			const TArray<USCS_Node*>& RootNodes = BP->SimpleConstructionScript->GetRootNodes();
			bool bAttached = false;

			// Try SCS root nodes first
			if (RootNodes.Num() > 0 && CompClass->IsChildOf(USceneComponent::StaticClass()))
			{
				RootNodes[0]->AddChildNode(NewNode);
				bAttached = true;
			}

			// If no SCS root, attach to the native/inherited default scene root
			if (!bAttached && CompClass->IsChildOf(USceneComponent::StaticClass()))
			{
				USCS_Node* DefaultRoot = BP->SimpleConstructionScript->GetDefaultSceneRootNode();
				if (DefaultRoot)
				{
					DefaultRoot->AddChildNode(NewNode);
					bAttached = true;
				}
			}

			// Fallback: set as root node in SCS (attaches to native root automatically)
			if (!bAttached)
			{
				BP->SimpleConstructionScript->AddNode(NewNode);
			}
		}

	NodeAttached:
		// Apply inline properties on the component template
		if (UActorComponent* CompTemplate = NewNode->ComponentTemplate)
		{
			static const TSet<FString> ReservedKeys = { TEXT("name"), TEXT("class"), TEXT("type"), TEXT("parent"), TEXT("makeroot"), TEXT("isroot") };

			// Helper: iterate one JSON object and call ApplyComponentProperty for each key/value pair.
			auto ProcessProps = [&](const TSharedPtr<FJsonObject>& PropsObj)
			{
				for (const auto& Pair : PropsObj->Values)
				{
					const FString Key(*Pair.Key);
					if (ReservedKeys.Contains(NormalizeKey(Key))) continue;

					FString Value;
					if (Pair.Value->Type == EJson::String)       Value = Pair.Value->AsString();
					else if (Pair.Value->Type == EJson::Boolean)  Value = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
					else if (Pair.Value->Type == EJson::Number)   Value = FString::SanitizeFloat(Pair.Value->AsNumber());
					else continue; // skip nested objects/arrays at this level

					ApplyComponentProperty(CompTemplate, Key, Value, CompName, nullptr, &Warnings);
				}
			};

			// Flat top-level properties (e.g. static_mesh="..." at same level as name/type)
			ProcessProps(Obj);

			// Nested "properties" sub-object
			// e.g. {"name":"CubeMesh","type":"StaticMeshComponent","properties":{"static_mesh":"..."}}
			const TSharedPtr<FJsonObject>* NestedProps;
			if (Obj->TryGetObjectField(TEXT("properties"), NestedProps))
				ProcessProps(*NestedProps);
		}

		Added++;
		UE_LOG(LogNwiroBP, Log, TEXT("Added component: %s (%s)"), *CompName, *ClassName);
	}

	FString Msg = FString::Printf(TEXT("Added %d component(s)"), Added);
	if (Warnings.Num() > 0)
	{
		Msg += TEXT(". Warnings: ") + FString::Join(Warnings, TEXT("; "));
	}
	if (Errors.Num() > 0)
	{
		Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));
	}

	return Added > 0 || Errors.Num() == 0 ? FNwiroIKBPResult::Ok(Msg) : FNwiroIKBPResult::Fail(Msg);
}

// ============================================================
// REMOVE COMPONENTS
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoRemoveComponents(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	if (!BP->SimpleConstructionScript) return FNwiroIKBPResult::Fail(TEXT("No SCS"));

	int32 Removed = 0;
	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		FString CompName;
		if (Item->Type == EJson::String)
		{
			CompName = Item->AsString();
		}
		else if (Item->Type == EJson::Object && Item->AsObject().IsValid())
		{
			CompName = Item->AsObject()->GetStringField(TEXT("name"));
		}

		if (CompName.IsEmpty()) continue;

		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->GetVariableName().ToString().Equals(CompName, ESearchCase::IgnoreCase))
			{
				BP->SimpleConstructionScript->RemoveNode(Node);
				Removed++;
				break;
			}
		}
	}

	return FNwiroIKBPResult::Ok(FString::Printf(TEXT("Removed %d component(s)"), Removed));
}

// ============================================================
// ADD FUNCTIONS
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoAddFunctions(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	int32 Added = 0;
	TArray<FString> Errors;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		FString FuncName = Obj->GetStringField(TEXT("name"));
		if (FuncName.IsEmpty()) { Errors.Add(TEXT("Missing function name")); continue; }

		const bool bOverride = Obj->HasField(TEXT("override")) && Obj->GetBoolField(TEXT("override"));

		// Check if function already exists in this BP. For override, an
		// existing graph is fine — add_interface auto-creates stub graphs for
		// interface methods, and re-requesting override is a no-op success.
		if (FindGraph(BP, FuncName))
		{
			if (bOverride)
			{
				Added++;
				continue;
			}
			Errors.Add(FString::Printf(TEXT("Function '%s' already exists"), *FuncName));
			continue;
		}

		// Override mode: locate the inherited UFunction from parent class or
		// any implemented interface, then create a new graph with that
		// signature so the agent can fill in the body.
		UFunction* OverrideSignature = nullptr;
		bool bFromInterface = false;
		int32 InterfaceIdx = INDEX_NONE;
		if (bOverride)
		{
			if (BP->ParentClass)
			{
				// If parent is a user BP that hasn't been compiled, its
				// generated class won't yet carry the inherited function.
				if (UBlueprint* ParentBP = Cast<UBlueprint>(BP->ParentClass->ClassGeneratedBy))
				{
					if (ParentBP->Status == EBlueprintStatus::BS_Unknown
						|| ParentBP->Status == EBlueprintStatus::BS_Dirty
						|| !BP->ParentClass->FindFunctionByName(FName(*FuncName)))
					{
						FKismetEditorUtilities::CompileBlueprint(ParentBP, EBlueprintCompileOptions::SkipGarbageCollection);
					}
				}
				OverrideSignature = BP->ParentClass->FindFunctionByName(FName(*FuncName));
			}
			if (!OverrideSignature)
			{
				for (int32 i = 0; i < BP->ImplementedInterfaces.Num(); ++i)
				{
					UClass* IfaceClass = BP->ImplementedInterfaces[i].Interface;
					if (!IfaceClass) continue;

					// The interface BP may not have been compiled yet — its
					// generated class wouldn't carry the UFunction we're looking
					// for. Force-compile the interface BP first to make sure the
					// function is reachable via FindFunctionByName.
					if (UBlueprint* IfaceBP = Cast<UBlueprint>(IfaceClass->ClassGeneratedBy))
					{
						if (IfaceBP->Status == EBlueprintStatus::BS_Unknown
							|| IfaceBP->Status == EBlueprintStatus::BS_Dirty
							|| !IfaceClass->FindFunctionByName(FName(*FuncName)))
						{
							FKismetEditorUtilities::CompileBlueprint(IfaceBP, EBlueprintCompileOptions::SkipGarbageCollection);
							// Re-resolve class after compile
							IfaceClass = BP->ImplementedInterfaces[i].Interface;
						}
					}
					if (IfaceClass)
					{
						if (UFunction* F = IfaceClass->FindFunctionByName(FName(*FuncName)))
						{
							OverrideSignature = F;
							bFromInterface = true;
							InterfaceIdx = i;
							break;
						}
					}
				}
			}
			if (!OverrideSignature)
			{
				Errors.Add(FString::Printf(TEXT("Override requested but '%s' not found on parent class or any implemented interface"), *FuncName));
				continue;
			}
		}

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FName(*FuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

		if (!NewGraph) { Errors.Add(FString::Printf(TEXT("Failed to create function '%s'"), *FuncName)); continue; }

		if (bOverride && OverrideSignature)
		{
			// Use the inherited UFunction as the signature so terminators / pins match.
			FBlueprintEditorUtils::AddFunctionGraph<UFunction>(BP, NewGraph, /*bIsUserCreated=*/true, OverrideSignature);

			// CreateFunctionGraphTerminators (called by AddFunctionGraph) registers
			// each signature parameter as a UserDefinedPin on the entry node. The K2
			// compiler then sees the child's override as ALSO defining those params,
			// conflicting with the inherited definition: "function name already used,
			// cannot order parameters". Clearing UserDefinedPins and reconstructing
			// the node lets the entry inherit the param list from FunctionReference
			// (the parent's UFunction) instead of redefining them.
			for (UEdGraphNode* N : NewGraph->Nodes)
			{
				if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(N))
				{
					Entry->UserDefinedPins.Empty();
					Entry->FunctionReference.SetFromField<UFunction>(OverrideSignature, /*IsConsideredSelfContext=*/false);
					Entry->ReconstructNode();
					break;
				}
			}

			if (bFromInterface && InterfaceIdx != INDEX_NONE)
			{
				// Interface overrides live on the interface descriptor, not
				// the plain FunctionGraphs list — move it over so the editor
				// surfaces it under the right interface in the My-BP panel.
				if (BP->FunctionGraphs.Remove(NewGraph) > 0)
				{
					BP->ImplementedInterfaces[InterfaceIdx].Graphs.Add(NewGraph);
				}
			}
		}
		else
		{
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, true, nullptr);
		}

		// Set pure flag
		if (Obj->HasField(TEXT("pure")) && Obj->GetBoolField(TEXT("pure")))
		{
			// Find the entry node and set pure flag
			for (UEdGraphNode* Node : NewGraph->Nodes)
			{
				if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
				{
					Entry->SetExtraFlags(Entry->GetExtraFlags() | FUNC_BlueprintPure);
					break;
				}
			}
		}

		// Add input parameters
		const TArray<TSharedPtr<FJsonValue>>* Inputs;
		if (Obj->TryGetArrayField(TEXT("inputs"), Inputs))
		{
			for (const TSharedPtr<FJsonValue>& InputVal : *Inputs)
			{
				const TSharedPtr<FJsonObject>& InputObj = InputVal->AsObject();
				if (!InputObj.IsValid()) continue;

				FString ParamName = InputObj->GetStringField(TEXT("name"));
				FEdGraphPinType ParamType;

				const TSharedPtr<FJsonObject>* ParamTypeObj;
				if (InputObj->TryGetObjectField(TEXT("type"), ParamTypeObj))
				{
					ParamType = ParsePinType(*ParamTypeObj);
				}
				else
				{
					FString TypeStr = InputObj->GetStringField(TEXT("type"));
					TSharedPtr<FJsonObject> SimpleType = MakeShareable(new FJsonObject());
					SimpleType->SetStringField(TEXT("base"), TypeStr);
					ParamType = ParsePinType(SimpleType);
				}

				// Add pin to function entry node
				for (UEdGraphNode* Node : NewGraph->Nodes)
				{
					if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
					{
						TSharedPtr<FUserPinInfo> PinInfo = MakeShareable(new FUserPinInfo());
						PinInfo->PinName = FName(*ParamName);
						PinInfo->PinType = ParamType;
						Entry->UserDefinedPins.Add(PinInfo);
						Entry->ReconstructNode();
						break;
					}
				}
			}
		}

		// Add output parameters
		const TArray<TSharedPtr<FJsonValue>>* Outputs;
		if (Obj->TryGetArrayField(TEXT("outputs"), Outputs))
		{
			for (const TSharedPtr<FJsonValue>& OutputVal : *Outputs)
			{
				const TSharedPtr<FJsonObject>& OutputObj = OutputVal->AsObject();
				if (!OutputObj.IsValid()) continue;

				FString ParamName = OutputObj->GetStringField(TEXT("name"));
				FEdGraphPinType ParamType;

				const TSharedPtr<FJsonObject>* ParamTypeObj;
				if (OutputObj->TryGetObjectField(TEXT("type"), ParamTypeObj))
				{
					ParamType = ParsePinType(*ParamTypeObj);
				}
				else
				{
					FString TypeStr = OutputObj->GetStringField(TEXT("type"));
					TSharedPtr<FJsonObject> SimpleType = MakeShareable(new FJsonObject());
					SimpleType->SetStringField(TEXT("base"), TypeStr);
					ParamType = ParsePinType(SimpleType);
				}

				// Find or create result node
				UK2Node_FunctionResult* ResultNode = nullptr;
				for (UEdGraphNode* Node : NewGraph->Nodes)
				{
					ResultNode = Cast<UK2Node_FunctionResult>(Node);
					if (ResultNode) break;
				}

				if (!ResultNode)
				{
					FGraphNodeCreator<UK2Node_FunctionResult> Creator(*NewGraph);
					ResultNode = Creator.CreateNode();
					ResultNode->NodePosX = 400;
					ResultNode->NodePosY = 0;
					Creator.Finalize();
				}

				if (ResultNode)
				{
					TSharedPtr<FUserPinInfo> PinInfo = MakeShareable(new FUserPinInfo());
					PinInfo->PinName = FName(*ParamName);
					PinInfo->PinType = ParamType;
					ResultNode->UserDefinedPins.Add(PinInfo);
					ResultNode->ReconstructNode();
				}
			}
		}

		Added++;
		UE_LOG(LogNwiroBP, Log, TEXT("Added function: %s"), *FuncName);
	}

	FString Msg = FString::Printf(TEXT("Added %d function(s)"), Added);
	if (Errors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));

	return Added > 0 || Errors.Num() == 0 ? FNwiroIKBPResult::Ok(Msg) : FNwiroIKBPResult::Fail(Msg);
}

// ============================================================
// ADD CUSTOM EVENTS
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoAddCustomEvents(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	UEdGraph* EventGraph = nullptr;
	for (UEdGraph* G : BP->UbergraphPages)
	{
		if (G && G->GetName() == TEXT("EventGraph"))
		{
			EventGraph = G;
			break;
		}
	}

	if (!EventGraph && BP->UbergraphPages.Num() > 0)
	{
		EventGraph = BP->UbergraphPages[0];
	}

	if (!EventGraph)
	{
		return FNwiroIKBPResult::Fail(TEXT("No event graph found"));
	}

	int32 Added = 0;
	int32 YOffset = 0;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		FString EventName = Obj->GetStringField(TEXT("name"));
		FString Ref = Obj->GetStringField(TEXT("ref"));
		if (EventName.IsEmpty()) continue;

		FGraphNodeCreator<UK2Node_CustomEvent> Creator(*EventGraph);
		UK2Node_CustomEvent* EventNode = Creator.CreateNode();
		EventNode->CustomFunctionName = FName(*EventName);
		EventNode->NodePosX = 0;
		EventNode->NodePosY = YOffset;
		Creator.Finalize();

		// Add parameters
		const TArray<TSharedPtr<FJsonValue>>* Params;
		if (Obj->TryGetArrayField(TEXT("params"), Params))
		{
			for (const TSharedPtr<FJsonValue>& PVal : *Params)
			{
				const TSharedPtr<FJsonObject>& PObj = PVal->AsObject();
				if (!PObj.IsValid()) continue;

				FString ParamName = PObj->GetStringField(TEXT("name"));
				FEdGraphPinType ParamType;

				const TSharedPtr<FJsonObject>* TypeObj;
				if (PObj->TryGetObjectField(TEXT("type"), TypeObj))
				{
					ParamType = ParsePinType(*TypeObj);
				}
				else
				{
					TSharedPtr<FJsonObject> SimpleType = MakeShareable(new FJsonObject());
					SimpleType->SetStringField(TEXT("base"), PObj->GetStringField(TEXT("type")));
					ParamType = ParsePinType(SimpleType);
				}

				TSharedPtr<FUserPinInfo> PinInfo = MakeShareable(new FUserPinInfo());
				PinInfo->PinName = FName(*ParamName);
				PinInfo->PinType = ParamType;
				EventNode->UserDefinedPins.Add(PinInfo);
			}
			EventNode->ReconstructNode();
		}

		// Set replication flags
		FString Replication = Obj->GetStringField(TEXT("replication")).ToLower();
		if (Replication == TEXT("multicast"))
		{
			EventNode->FunctionFlags |= FUNC_NetMulticast;
		}
		else if (Replication == TEXT("server"))
		{
			EventNode->FunctionFlags |= FUNC_NetServer;
		}
		else if (Replication == TEXT("client"))
		{
			EventNode->FunctionFlags |= FUNC_NetClient;
		}

		if (Obj->HasField(TEXT("reliable")) && Obj->GetBoolField(TEXT("reliable")))
		{
			EventNode->FunctionFlags |= FUNC_NetReliable;
		}

		StoreNodeRef(Ref.IsEmpty() ? EventName : Ref, EventNode, EventGraph->GetName());
		YOffset += 200;
		Added++;
		UE_LOG(LogNwiroBP, Log, TEXT("Added custom event: %s"), *EventName);
	}

	return FNwiroIKBPResult::Ok(FString::Printf(TEXT("Added %d custom event(s)"), Added));
}

// ============================================================
// ADD NODES
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoAddNodes(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	UEdGraph* Graph = FindGraph(BP, GraphName);
	if (!Graph)
	{
		return FNwiroIKBPResult::Fail(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
	}

	// If any incoming node binds to a component (component-bound event), refresh
	// the skeleton class so FObjectProperty lookups against just-added components
	// succeed. Without this, an edit_blueprint that adds a component AND binds
	// an event to it in one call fails on the event ("not yet on skeleton").
	for (const TSharedPtr<FJsonValue>& PreItem : Items)
	{
		const TSharedPtr<FJsonObject>& PreObj = PreItem.IsValid() ? PreItem->AsObject() : nullptr;
		if (PreObj.IsValid() && PreObj->HasField(TEXT("component")))
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);
			break;
		}
	}

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	int32 Added = 0;
	int32 NodeX = 300;
	int32 NodeY = 0;
	TArray<FString> Errors;
	TArray<FString> NodeDetails;

	int32 Skipped = 0;
	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		// Accept all the field names the LLM tends to invent for the node-type
		// field. We've now seen `type`, `node`, `nodeType`, `node_type`, `kind`.
		FString NodeType;
		const TArray<FString> NodeTypeKeys = {
			TEXT("type"), TEXT("node"), TEXT("nodeType"), TEXT("node_type"),
			TEXT("nodetype"), TEXT("kind"), TEXT("nodeClass"), TEXT("class"),
		};
		for (const FString& Key : NodeTypeKeys)
		{
			if (Obj->HasField(Key)) { NodeType = Obj->GetStringField(Key); break; }
		}

		// Support "ref", "id" AND "name" — the LLM frequently uses "name" as
		// the user-facing identifier instead of "ref".
		FString Ref;
		if (Obj->HasField(TEXT("ref")))       Ref = Obj->GetStringField(TEXT("ref"));
		else if (Obj->HasField(TEXT("id")))   Ref = Obj->GetStringField(TEXT("id"));
		else if (Obj->HasField(TEXT("name")) && !NodeType.Equals(TEXT("Event"), ESearchCase::IgnoreCase) && !NodeType.Equals(TEXT("CustomEvent"), ESearchCase::IgnoreCase))
		{
			// "name" doubles as the event name for Event/CustomEvent — only
			// reuse it as a ref for non-event nodes.
			Ref = Obj->GetStringField(TEXT("name"));
		}
		int32 PosX = Obj->HasField(TEXT("x")) ? (int32)Obj->GetNumberField(TEXT("x")) : NodeX;
		int32 PosY = Obj->HasField(TEXT("y")) ? (int32)Obj->GetNumberField(TEXT("y")) : NodeY;

		// Dedup: if a node with this ref already exists in the session, OR
		// (for events) if a built-in event of the same name already exists in
		// the graph, skip creation. This prevents partial-failure retries from
		// piling up duplicate Tick / BeginPlay / Multiply nodes.
		//
		// IMPORTANT: only check NodeRefs (sessions's explicit ref → guid map),
		// NOT the substring title matches in FindNodeByRef sections 4a/4b.
		// Those were tripping false positives for short refs like "PR" / "Q"
		// (matched any existing "Print String" / "Quit Game" node by substring),
		// causing legitimate first-time-added nodes to be silently skipped.
		// Found via scenario/05-pause-menu (fresh BP, refs EvR/PR/EvQ/Q got
		// "Added 2, skipped 2 duplicate(s)" — the print/quit halves were lost).
		auto IsRefAlreadyAssigned = [Graph](const FString& Lookup) -> bool
		{
			if (Lookup.IsEmpty()) return false;
			// Section 1: exact session ref → guid in this graph
			if (const FNwiroIKNodeRef* Found = NodeRefs.Find(Lookup))
			{
				for (UEdGraphNode* N : Graph->Nodes) if (N && N->NodeGuid == Found->NodeGuid) return true;
			}
			// Section 2: normalized session ref. Bail if lookup is too short to
			// be specific (3-char threshold — "ev", "pr", "q" can collide with
			// printstring etc. when normalized; "evt", "prn" are intentional).
			const FString Norm = NormalizeRefKey(Lookup);
			if (Norm.Len() >= 3)
			{
				for (const auto& Pair : NodeRefs)
				{
					if (NormalizeRefKey(Pair.Key) == Norm)
					{
						for (UEdGraphNode* N : Graph->Nodes) if (N && N->NodeGuid == Pair.Value.NodeGuid) return true;
					}
				}
			}
			return false;
		};
		if (!Ref.IsEmpty() && IsRefAlreadyAssigned(Ref))
		{
			Skipped++;
			continue;
		}
		if (NodeType.Equals(TEXT("Event"), ESearchCase::IgnoreCase))
		{
			FString EventName = Obj->HasField(TEXT("event")) ? Obj->GetStringField(TEXT("event")) : Obj->GetStringField(TEXT("name"));
			if (!EventName.IsEmpty())
			{
				// Strip "event"/"receive" prefixes for the dedup check.
				FString Norm = NormalizeRefKey(EventName);
				if (Norm.StartsWith(TEXT("receive"))) Norm = Norm.RightChop(7);
				if (Norm.StartsWith(TEXT("event"))) Norm = Norm.RightChop(5);

				bool bExists = false;
				for (UEdGraph* SearchGraph : BP->UbergraphPages)
				{
					if (!SearchGraph) continue;
					for (UEdGraphNode* Existing : SearchGraph->Nodes)
					{
						UK2Node_Event* Evt = Cast<UK2Node_Event>(Existing);
						if (!Evt) continue;
						FString FuncNorm = NormalizeRefKey(Evt->GetFunctionName().ToString());
						if (FuncNorm.StartsWith(TEXT("receive"))) FuncNorm = FuncNorm.RightChop(7);
						if (FuncNorm == Norm)
						{
							bExists = true;
							// Still register a session ref pointing to the existing node
							// so subsequent connect_pins can find it via the user's ref.
							if (!Ref.IsEmpty()) StoreNodeRef(Ref, Evt, SearchGraph->GetName());
							break;
						}
					}
					if (bExists) break;
				}
				if (bExists)
				{
					Skipped++;
					continue;
				}
			}
		}

		UEdGraphNode* CreatedNode = nullptr;
		FString NodeDetail;

		// ---- CallFunction ----
		// Auto-detect AddMappingContext even when requested as CallFunction
		if (NodeType.Equals(TEXT("CallFunction"), ESearchCase::IgnoreCase))
		{
			FString TempFunc = Obj->GetStringField(TEXT("function"));
			if (TempFunc.Contains(TEXT("AddMappingContext"), ESearchCase::IgnoreCase) ||
				TempFunc.Contains(TEXT("Add Mapping Context"), ESearchCase::IgnoreCase))
			{
				NodeType = TEXT("AddMappingContext");
			}
			else if (TempFunc.Contains(TEXT("SpawnActor"), ESearchCase::IgnoreCase) ||
				TempFunc.Contains(TEXT("Spawn Actor"), ESearchCase::IgnoreCase))
			{
				NodeType = TEXT("SpawnActor");
			}
		}

		if (NodeType.Equals(TEXT("CallFunction"), ESearchCase::IgnoreCase))
		{
			FString FunctionName = Obj->GetStringField(TEXT("function"));
			FString TargetClass = Obj->GetStringField(TEXT("target"));

			// Normalize: remove spaces so "Print String" -> "PrintString"
			FString FunctionNameNoSpaces = FunctionName.Replace(TEXT(" "), TEXT(""));

			UFunction* Func = nullptr;
			bool bBruteForce = false;
			bool bAmbiguousAllowed = false;

			// Helper lambda: search function in class + entire parent chain
			// Tries multiple naming conventions:
			//   - exact name, no-spaces, K2_ prefix
			//   - Float <-> Double synonyms (UE 5.5+ math is double)
			//   - bare math operator names ("Multiply" -> "Multiply_DoubleDouble", etc.)
			auto FindFuncInHierarchy = [&FunctionName, &FunctionNameNoSpaces](UClass* StartClass) -> UFunction*
			{
				TArray<FString> Tries;
				Tries.Add(FunctionName);
				Tries.Add(FunctionNameNoSpaces);
				Tries.Add(TEXT("K2_") + FunctionName);
				Tries.Add(TEXT("K2_") + FunctionNameNoSpaces);

				// UE 5.6 renamed LineTraceSingleByChannel → LineTraceSingle
				if (FunctionNameNoSpaces.Equals(TEXT("LineTraceSingleByChannel"), ESearchCase::IgnoreCase) ||
					FunctionNameNoSpaces.Equals(TEXT("LineTraceByChannel"), ESearchCase::IgnoreCase))
					Tries.Add(TEXT("LineTraceSingle"));

				// GetWorldLocation/GetWorldRotation: display name differs by target class
				if (FunctionNameNoSpaces.Equals(TEXT("GetWorldLocation"), ESearchCase::IgnoreCase))
				{
					if (StartClass->IsChildOf(USceneComponent::StaticClass()))
						Tries.Add(TEXT("K2_GetComponentLocation"));
					else if (StartClass->IsChildOf(AActor::StaticClass()))
						Tries.Add(TEXT("K2_GetActorLocation"));
				}
				if (FunctionNameNoSpaces.Equals(TEXT("GetWorldRotation"), ESearchCase::IgnoreCase))
				{
					if (StartClass->IsChildOf(USceneComponent::StaticClass()))
						Tries.Add(TEXT("K2_GetComponentRotation"));
					else if (StartClass->IsChildOf(AActor::StaticClass()))
						Tries.Add(TEXT("K2_GetActorRotation"));
				}

				// Float <-> Double synonyms
				if (FunctionNameNoSpaces.Contains(TEXT("Float")))
					Tries.Add(FunctionNameNoSpaces.Replace(TEXT("Float"), TEXT("Double")));
				if (FunctionNameNoSpaces.Contains(TEXT("Double")))
					Tries.Add(FunctionNameNoSpaces.Replace(TEXT("Double"), TEXT("Float")));

				// Bare math operator names — the LLM often writes just "Multiply"
				// or "Add" without specifying types. Try the full KismetMathLibrary
				// names for the most common overloads.
				static const TArray<FString> MathOps = {
					TEXT("Add"), TEXT("Subtract"), TEXT("Multiply"), TEXT("Divide"),
					TEXT("Min"), TEXT("Max"), TEXT("Equal"), TEXT("NotEqual"),
					TEXT("Less"), TEXT("LessEqual"), TEXT("Greater"), TEXT("GreaterEqual"),
				};
				for (const FString& Op : MathOps)
				{
					if (FunctionNameNoSpaces.Equals(Op, ESearchCase::IgnoreCase))
					{
						Tries.Add(Op + TEXT("_DoubleDouble"));
						Tries.Add(Op + TEXT("_FloatFloat"));
						Tries.Add(Op + TEXT("_IntInt"));
						Tries.Add(Op + TEXT("_VectorVector"));
						break;
					}
				}

				// Display-name conversion: the LLM frequently feeds us strings
				// like "To Vector (Float)", "To Float", "To Int" which are the
				// editor display names. The real C++ functions live under
				// `Conv_<Source>To<Dest>` in KismetMathLibrary. Strip the
				// parenthesized type hint and try every reasonable Conv_ form.
				{
					// "To Vector (Float)" → "ToVector"
					FString StrippedParens = FunctionName;
					int32 LParen;
					if (StrippedParens.FindChar('(', LParen)) StrippedParens = StrippedParens.Left(LParen);
					StrippedParens = StrippedParens.Replace(TEXT(" "), TEXT("")).TrimStartAndEnd();

					if (StrippedParens.StartsWith(TEXT("To"), ESearchCase::IgnoreCase) && StrippedParens.Len() > 2)
					{
						const FString DestType = StrippedParens.RightChop(2); // e.g. "Vector"
						// Try every common source type — first match wins.
						const TArray<FString> SrcTypes = { TEXT("Double"), TEXT("Float"), TEXT("Int"), TEXT("Int64"), TEXT("Bool"), TEXT("String"), TEXT("Name"), TEXT("Vector"), TEXT("Rotator") };
						for (const FString& Src : SrcTypes)
						{
							Tries.Add(FString::Printf(TEXT("Conv_%sTo%s"), *Src, *DestType));
						}
						// Also Make<Type>: MakeVector / MakeRotator etc.
						Tries.Add(TEXT("Make") + DestType);
					}
				}

				for (UClass* C = StartClass; C; C = C->GetSuperClass())
				{
					for (const FString& Try : Tries)
					{
						UFunction* F = C->FindFunctionByName(FName(*Try));
						if (F) return F;
					}
				}
				return nullptr;
			};

			// Search for the function in the target class hierarchy
			if (!TargetClass.IsEmpty())
			{
				UClass* TargetUClass = nullptr;

				// Check SCS component instance names first (e.g. "FirstPersonCamera" → UCameraComponent)
				if (BP->SimpleConstructionScript)
				{
					for (USCS_Node* SCSNode : BP->SimpleConstructionScript->GetAllNodes())
					{
						if (SCSNode && SCSNode->ComponentTemplate &&
							SCSNode->GetVariableName().ToString().Equals(TargetClass, ESearchCase::IgnoreCase))
						{
							TargetUClass = SCSNode->ComponentTemplate->GetClass();
							break;
						}
					}
				}

				if (!TargetUClass)
					TargetUClass = FindClassByName(TargetClass);

				if (TargetUClass)
					Func = FindFuncInHierarchy(TargetUClass);
			}

			// If not found, search in common libraries + BP parent chain
			if (!Func)
			{
				TArray<UClass*> SearchClasses = {
					UKismetSystemLibrary::StaticClass(),
					UKismetMathLibrary::StaticClass(),
					UKismetStringLibrary::StaticClass(),
					UGameplayStatics::StaticClass(),
					UEnhancedInputLocalPlayerSubsystem::StaticClass(),
					// Common UMG components — agents call SetPercent/SetText/IsChecked/
					// SetValue/etc without knowing or passing the owning widget class.
					// Sweeping these here makes the "natural minimal" call shape work.
					UProgressBar::StaticClass(),
					UTextBlock::StaticClass(),
					UButton::StaticClass(),
					USlider::StaticClass(),
					UCheckBox::StaticClass(),
					USpinBox::StaticClass(),
					UEditableText::StaticClass(),
					UEditableTextBox::StaticClass(),
					UMultiLineEditableText::StaticClass(),
					UMultiLineEditableTextBox::StaticClass(),
					UImage::StaticClass(),
					UBorder::StaticClass(),
					UWidgetSwitcher::StaticClass(),
					UComboBoxString::StaticClass(),
					UCanvasPanel::StaticClass(),
					UVerticalBox::StaticClass(),
					UHorizontalBox::StaticClass(),
					UUserWidget::StaticClass(),
				};

				// Also search blueprint's generated class and parent hierarchy
				if (BP->GeneratedClass)
				{
					for (UClass* C = BP->GeneratedClass; C; C = C->GetSuperClass())
					{
						SearchClasses.Add(C);
					}
				}

				for (UClass* Lib : SearchClasses)
				{
					Func = FindFuncInHierarchy(Lib);
					if (Func) break;
				}
			}

			// Last resort: brute-force search all loaded UClasses
			if (!Func)
			{
				UFunction* BFFunc = nullptr;
				UClass* BFClass = nullptr;
				for (TObjectIterator<UClass> It; It; ++It)
				{
					UFunction* F = It->FindFunctionByName(FName(*FunctionName), EIncludeSuperFlag::ExcludeSuper);
					if (F && F->HasAnyFunctionFlags(FUNC_BlueprintCallable))
					{
						BFFunc = F;
						BFClass = *It;
						UE_LOG(LogNwiroBP, Log, TEXT("Found function '%s' via brute-force in class '%s'"), *FunctionName, *It->GetName());
						break;
					}
				}

				if (BFFunc && BFClass)
				{
					// Trust the result only if the owning class is a blueprint function library
					// or is in the BP's own parent chain — anything else is ambiguous.
					bool bTrusted = BFClass->IsChildOf(UBlueprintFunctionLibrary::StaticClass());
					if (!bTrusted && BP->GeneratedClass)
					{
						for (UClass* C = BP->GeneratedClass; C; C = C->GetSuperClass())
						{
							if (C == BFClass) { bTrusted = true; break; }
						}
					}

					if (bTrusted)
					{
						Func = BFFunc;
						bBruteForce = true;
					}
					else
					{
						bool bAllowAmbiguous = false;
						Obj->TryGetBoolField(TEXT("allow_ambiguous_bruteforce"), bAllowAmbiguous);

						if (bAllowAmbiguous)
						{
							Func = BFFunc;
							bBruteForce = true;
							bAmbiguousAllowed = true;
						}
						else
						{
							Errors.Add(FString::Printf(
								TEXT("Function '%s' was found via brute-force in class '%s', but that class is not a trusted library or this blueprint's parent chain. ")
								TEXT("If this function belongs to a specific component or object, re-send the node with target:'ComponentInstanceName' or target:'ClassName' so the correct class hierarchy is searched. ")
								TEXT("Only use \"allow_ambiguous_bruteforce\": true if this untrusted owner class is intentional."),
								*FunctionName, *BFClass->GetName()));
							continue;
						}
					}
				}
			}

			if (Func)
			{
				// Array library functions (Array_Length, Array_Add, Array_Get, ...) carry an
				// "ArrayParm" meta and MUST use UK2Node_CallArrayFunction, not the base CallFunction
				// node: only the array variant propagates the connected array's element type into its
				// wildcard pins. With the base node the wildcard stayed unresolved and the BP failed
				// to compile.
				if (Func->HasMetaData(TEXT("ArrayParm")))
				{
					FGraphNodeCreator<UK2Node_CallArrayFunction> Creator(*Graph);
					UK2Node_CallArrayFunction* ArrNode = Creator.CreateNode();
					ArrNode->SetFromFunction(Func);
					ArrNode->NodePosX = PosX;
					ArrNode->NodePosY = PosY;
					Creator.Finalize();
					CreatedNode = ArrNode;
				}
				else
				{
					FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
					UK2Node_CallFunction* FuncNode = Creator.CreateNode();
					FuncNode->SetFromFunction(Func);
					FuncNode->NodePosX = PosX;
					FuncNode->NodePosY = PosY;
					Creator.Finalize();
					CreatedNode = FuncNode;
				}
				const FString ResolvedName = Func->GetName();
				const FString OwnerName = Func->GetOwnerClass() ? Func->GetOwnerClass()->GetName() : TEXT("unknown");
				if (bAmbiguousAllowed)
					NodeDetail = FString::Printf(TEXT("%s [ambiguous-allowed] found in %s via brute-force, requested:'%s'"), *ResolvedName, *OwnerName, *FunctionName);
				else if (bBruteForce)
					NodeDetail = FString::Printf(TEXT("%s [fallback] found in %s via brute-force, requested:'%s'"), *ResolvedName, *OwnerName, *FunctionName);
				else if (!ResolvedName.Equals(FunctionName, ESearchCase::IgnoreCase))
					NodeDetail = FString::Printf(TEXT("%s [fallback] resolved from requested:'%s' in %s"), *ResolvedName, *FunctionName, *OwnerName);
				else
					NodeDetail = FString::Printf(TEXT("%s [resolved] in %s"), *ResolvedName, *OwnerName);
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Function not found: %s"), *FunctionName));
			}
		}
		// ---- VariableGet ----
		else if (NodeType.Equals(TEXT("VariableGet"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("GetVariable"), ESearchCase::IgnoreCase))
		{
			FString VarName = Obj->GetStringField(TEXT("variable"));
			FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
			UK2Node_VariableGet* GetNode = Creator.CreateNode();
			GetNode->VariableReference.SetSelfMember(FName(*VarName));
			GetNode->NodePosX = PosX;
			GetNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = GetNode;
		}
		// ---- VariableSet ----
		else if (NodeType.Equals(TEXT("VariableSet"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("SetVariable"), ESearchCase::IgnoreCase))
		{
			FString VarName = Obj->GetStringField(TEXT("variable"));
			FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
			UK2Node_VariableSet* SetNode = Creator.CreateNode();
			SetNode->VariableReference.SetSelfMember(FName(*VarName));
			SetNode->NodePosX = PosX;
			SetNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = SetNode;
		}
		// ---- Branch (If) ----
		else if (NodeType.Equals(TEXT("Branch"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("If"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
			UK2Node_IfThenElse* BranchNode = Creator.CreateNode();
			BranchNode->NodePosX = PosX;
			BranchNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = BranchNode;
		}
		// ---- InputAction ----
		else if (NodeType.Equals(TEXT("InputAction"), ESearchCase::IgnoreCase))
		{
			FString ActionName = Obj->GetStringField(TEXT("action"));
			FGraphNodeCreator<UK2Node_InputAction> Creator(*Graph);
			UK2Node_InputAction* ActionNode = Creator.CreateNode();
			ActionNode->InputActionName = FName(*ActionName);
			ActionNode->NodePosX = PosX;
			ActionNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = ActionNode;
		}
		// ---- InputKey ----
		else if (NodeType.Equals(TEXT("InputKey"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("KeyEvent"), ESearchCase::IgnoreCase))
		{
			FString KeyName = Obj->GetStringField(TEXT("key"));
			FKey Key(*KeyName);

			FGraphNodeCreator<UK2Node_InputKey> Creator(*Graph);
			UK2Node_InputKey* KeyNode = Creator.CreateNode();
			KeyNode->InputKey = Key;
			KeyNode->NodePosX = PosX;
			KeyNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = KeyNode;
		}
		// ---- CustomEvent ----
		else if (NodeType.Equals(TEXT("CustomEvent"), ESearchCase::IgnoreCase))
		{
			FString EventName = Obj->GetStringField(TEXT("name"));
			FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
			UK2Node_CustomEvent* EventNode = Creator.CreateNode();
			EventNode->CustomFunctionName = FName(*EventName);
			EventNode->NodePosX = PosX;
			EventNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = EventNode;
		}
		// ---- Event (BeginPlay, Tick, ActorBeginOverlap, etc.) ----
		else if (NodeType.Equals(TEXT("Event"), ESearchCase::IgnoreCase))
		{
			FString EventName = Obj->GetStringField(TEXT("event"));
			if (EventName.IsEmpty()) EventName = Obj->GetStringField(TEXT("name"));

			// Component-bound event path — e.g. OnComponentBeginOverlap on a named
			// component. Uses UK2Node_ComponentBoundEvent, not UK2Node_Event.
			// Falls through to the regular Event handler when no component is named.
			FString CompName;
			Obj->TryGetStringField(TEXT("component"), CompName);

			// Widget BPs: components live in WidgetTree, not SCS. Try widget
			// lookup first; fall back to SCS for regular BPs.
			UClass* WidgetCompClass = nullptr;
			FObjectProperty* WidgetCompProp = nullptr;
			if (!CompName.IsEmpty())
			{
				if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(BP))
				{
					if (WBP->WidgetTree)
					{
						WBP->WidgetTree->ForEachWidget([&](UWidget* W)
						{
							if (W && W->GetName().Equals(CompName, ESearchCase::IgnoreCase) && !WidgetCompClass)
								WidgetCompClass = W->GetClass();
						});
						if (WidgetCompClass)
						{
							WidgetCompProp = FindFProperty<FObjectProperty>(BP->SkeletonGeneratedClass, FName(*CompName));
							// Widget BPs only auto-promote tree widgets to
							// SkeletonGeneratedClass FObjectProperties at compile
							// time. If the widget was add_widget'd in the same
							// session and the BP hasn't been recompiled since,
							// the property is missing — force a compile and
							// re-resolve so the OnClicked event can bind.
							if (!WidgetCompProp)
							{
								FKismetEditorUtilities::CompileBlueprint(BP);
								WidgetCompProp = FindFProperty<FObjectProperty>(BP->SkeletonGeneratedClass, FName(*CompName));
							}
						}
					}
				}
			}

			if (!CompName.IsEmpty() && WidgetCompClass && WidgetCompProp)
			{
				FMulticastDelegateProperty* DelegateProp = nullptr;
				for (TFieldIterator<FMulticastDelegateProperty> It(WidgetCompClass); It; ++It)
				{
					const FString PN = It->GetName();
					if (PN.Equals(EventName, ESearchCase::IgnoreCase)
						|| PN.Equals(FString(TEXT("On")) + EventName, ESearchCase::IgnoreCase))
					{ DelegateProp = *It; break; }
				}
				if (DelegateProp)
				{
					FGraphNodeCreator<UK2Node_ComponentBoundEvent> Creator(*Graph);
					UK2Node_ComponentBoundEvent* CBE = Creator.CreateNode();
					CBE->InitializeComponentBoundEventParams(WidgetCompProp, DelegateProp);
					CBE->NodePosX = PosX;
					CBE->NodePosY = PosY;
					Creator.Finalize();
					CreatedNode = CBE;
					NodeDetail = FString::Printf(TEXT("WidgetBoundEvent %s on %s"),
						*DelegateProp->GetName(), *CompName);
				}
				else
				{
					Errors.Add(FString::Printf(TEXT("Widget '%s' (class %s) has no multicast delegate matching '%s'"),
						*CompName, *WidgetCompClass->GetName(), *EventName));
				}
			}
			else if (!CompName.IsEmpty() && BP->SimpleConstructionScript)
			{
				USCS_Node* TargetNode = nullptr;
				for (USCS_Node* SN : BP->SimpleConstructionScript->GetAllNodes())
				{
					if (SN && SN->GetVariableName().ToString().Equals(CompName, ESearchCase::IgnoreCase))
					{
						TargetNode = SN;
						break;
					}
				}
				if (TargetNode && TargetNode->ComponentTemplate)
				{
					UClass* CompClass = TargetNode->ComponentTemplate->GetClass();
					FMulticastDelegateProperty* DelegateProp = nullptr;
					for (TFieldIterator<FMulticastDelegateProperty> It(CompClass); It; ++It)
					{
						const FString PN = It->GetName();
						if (PN.Equals(EventName, ESearchCase::IgnoreCase)
							|| PN.Equals(FString(TEXT("On")) + EventName, ESearchCase::IgnoreCase))
						{ DelegateProp = *It; break; }
					}
					if (DelegateProp)
					{
						FObjectProperty* CompProp = FindFProperty<FObjectProperty>(
							BP->SkeletonGeneratedClass, TargetNode->GetVariableName());
						if (CompProp)
						{
							FGraphNodeCreator<UK2Node_ComponentBoundEvent> Creator(*Graph);
							UK2Node_ComponentBoundEvent* CBE = Creator.CreateNode();
							CBE->InitializeComponentBoundEventParams(CompProp, DelegateProp);
							CBE->NodePosX = PosX;
							CBE->NodePosY = PosY;
							Creator.Finalize();
							CreatedNode = CBE;
							NodeDetail = FString::Printf(TEXT("ComponentBoundEvent %s on %s"),
								*DelegateProp->GetName(), *CompName);
						}
						else
						{
							Errors.Add(FString::Printf(TEXT("Component property '%s' not yet on skeleton class — compile the BP first"), *CompName));
						}
					}
					else
					{
						Errors.Add(FString::Printf(TEXT("Component '%s' (class %s) has no multicast delegate matching '%s'"),
							*CompName, *CompClass->GetName(), *EventName));
					}
				}
				else
				{
					Errors.Add(FString::Printf(TEXT("Component '%s' not found on this blueprint"), *CompName));
				}
			}
			else
			{
			// No component arg — fall through to the regular built-in event handler.
			// Map user-friendly names to UE internal function names
			static TMap<FString, FString> EventNameMap = {
				{ TEXT("beginplay"), TEXT("ReceiveBeginPlay") },
				{ TEXT("tick"), TEXT("ReceiveTick") },
				{ TEXT("endplay"), TEXT("ReceiveEndPlay") },
				{ TEXT("actorbeginoverlap"), TEXT("ReceiveActorBeginOverlap") },
				{ TEXT("actorendoverlap"), TEXT("ReceiveActorEndOverlap") },
				{ TEXT("hit"), TEXT("ReceiveHit") },
				{ TEXT("anyDamage"), TEXT("ReceiveAnyDamage") },
				{ TEXT("pointdamage"), TEXT("ReceivePointDamage") },
				{ TEXT("radialdamage"), TEXT("ReceiveRadialDamage") },
				{ TEXT("destroyed"), TEXT("ReceiveDestroyed") },
				{ TEXT("begincursorover"), TEXT("ReceiveBeginCursorOver") },
				{ TEXT("endcursorover"), TEXT("ReceiveEndCursorOver") },
				{ TEXT("clicked"), TEXT("ReceiveActorOnClicked") },
				{ TEXT("released"), TEXT("ReceiveActorOnReleased") },
			};

			// Aggressive normalization: strip "event" / "receive" prefixes and
			// non-alphanumeric chars so all of "Tick", "EventTick", "Event Tick",
			// "event_tick", "ReceiveTick" map to the same key.
			FString NormKey = EventName.ToLower();
			NormKey.ReplaceInline(TEXT(" "), TEXT(""));
			NormKey.ReplaceInline(TEXT("_"), TEXT(""));
			if (NormKey.StartsWith(TEXT("receive"))) NormKey = NormKey.RightChop(7);
			if (NormKey.StartsWith(TEXT("event"))) NormKey = NormKey.RightChop(5);

			FString InternalName = EventName;
			if (const FString* Mapped = EventNameMap.Find(NormKey))
			{
				InternalName = *Mapped;
			}
			else if (!EventName.StartsWith(TEXT("Receive"), ESearchCase::IgnoreCase))
			{
				// Also try with Receive prefix on the cleaned name
				InternalName = TEXT("Receive") + EventName;
			}

			// Search existing event nodes in ALL graph pages
			bool bFoundExisting = false;
			for (UEdGraph* SearchGraph : BP->UbergraphPages)
			{
				if (!SearchGraph) continue;
				for (UEdGraphNode* ExistingNode : SearchGraph->Nodes)
				{
					UK2Node_Event* EvtNode = Cast<UK2Node_Event>(ExistingNode);
					if (EvtNode)
					{
						FString EvtFuncName = EvtNode->GetFunctionName().ToString();
						if (EvtFuncName.Equals(InternalName, ESearchCase::IgnoreCase) ||
							EvtFuncName.Contains(EventName, ESearchCase::IgnoreCase))
						{
							CreatedNode = EvtNode;
							bFoundExisting = true;
							break;
						}
					}
				}
				if (bFoundExisting) break;
			}

			if (!bFoundExisting)
			{
				// Find the event function in the parent class chain
				UFunction* EventFunc = nullptr;
				UClass* SearchClass = BP->ParentClass.Get() ? BP->ParentClass.Get() : (BP->GeneratedClass ? BP->GeneratedClass->GetSuperClass() : nullptr);

				if (SearchClass)
				{
					for (UClass* C = SearchClass; C; C = C->GetSuperClass())
					{
						EventFunc = C->FindFunctionByName(FName(*InternalName));
						if (EventFunc) break;

						// Also try original name
						EventFunc = C->FindFunctionByName(FName(*EventName));
						if (EventFunc) break;
					}
				}

				if (EventFunc)
				{
					FGraphNodeCreator<UK2Node_Event> Creator(*Graph);
					UK2Node_Event* EvtNode = Creator.CreateNode();
					EvtNode->EventReference.SetFromField<UFunction>(EventFunc, false);
					EvtNode->bOverrideFunction = true;
					EvtNode->NodePosX = PosX;
					EvtNode->NodePosY = PosY;
					Creator.Finalize();
					CreatedNode = EvtNode;
				}
				else
				{
					Errors.Add(FString::Printf(TEXT("Built-in event not found: %s (tried %s). Use 'CustomEvent' type for custom events."), *EventName, *InternalName));
				}
			}
			}  // close: else (no component arg)
		}
		// ---- Self ----
		else if (NodeType.Equals(TEXT("Self"), ESearchCase::IgnoreCase))
		{
			UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Graph);
			if (SelfNode)
			{
				Graph->AddNode(SelfNode, false, false);
				SelfNode->CreateNewGuid();
				SelfNode->NodePosX = PosX;
				SelfNode->NodePosY = PosY;
				SelfNode->AllocateDefaultPins();
				CreatedNode = SelfNode;
			}
		}
		// ---- SpawnActor ----
		else if (NodeType.Equals(TEXT("SpawnActor"), ESearchCase::IgnoreCase))
		{
			UK2Node_SpawnActorFromClass* SpawnNode = NewObject<UK2Node_SpawnActorFromClass>(Graph);
			if (SpawnNode)
			{
				Graph->AddNode(SpawnNode, false, false);
				SpawnNode->CreateNewGuid();
				SpawnNode->NodePosX = PosX;
				SpawnNode->NodePosY = PosY;
				SpawnNode->AllocateDefaultPins();

				FString ActorClassName = GetFieldNormalized(Obj, TEXT("actorclass")); // covers actor_class / actorClass / ActorClass
				if (ActorClassName.IsEmpty()) ActorClassName = GetFieldNormalized(Obj, TEXT("class"));
				if (!ActorClassName.IsEmpty())
				{
					UClass* ActorClass = FindClassByName(ActorClassName);
					if (ActorClass)
					{
						UEdGraphPin* ClassPin = SpawnNode->FindPin(TEXT("Class"));
						if (ClassPin)
						{
							ClassPin->DefaultObject = ActorClass;
							SpawnNode->ReconstructNode();
						}
						NodeDetail = FString::Printf(TEXT("SpawnActor [resolved] class:'%s'"), *ActorClass->GetName());
					}
					else
					{
						Errors.Add(FString::Printf(TEXT("SpawnActor [not_resolved] class:'%s' not found — node created without class; fix actor_class field or use set_pin_defaults to set Class pin"), *ActorClassName));
					}
				}
				else
				{
					NodeDetail = TEXT("SpawnActor [resolved] no class requested");
				}

				CreatedNode = SpawnNode;
			}
		}
		// ---- Timeline (with float tracks + keyframes) ----
		else if (NodeType.Equals(TEXT("Timeline"), ESearchCase::IgnoreCase))
		{
			FString TimelineName = Obj->GetStringField(TEXT("name"));
			if (TimelineName.IsEmpty()) TimelineName = TEXT("MyTimeline");

			bool bLoop = Obj->HasField(TEXT("loop")) ? Obj->GetBoolField(TEXT("loop")) : false;
			bool bAutoPlay = Obj->HasField(TEXT("autoPlay")) ? Obj->GetBoolField(TEXT("autoPlay")) : false;
			float Length = Obj->HasField(TEXT("length")) ? (float)Obj->GetNumberField(TEXT("length")) : 1.0f;

			// Create timeline template FIRST via BlueprintEditorUtils
			UTimelineTemplate* TLTemplate = FBlueprintEditorUtils::AddNewTimeline(BP, FName(*TimelineName));
			UE_LOG(LogNwiroBP, Log, TEXT("Timeline '%s': Template %s"), *TimelineName, TLTemplate ? TEXT("CREATED") : TEXT("FAILED"));

			// Now create the node
			FGraphNodeCreator<UK2Node_Timeline> Creator(*Graph);
			UK2Node_Timeline* TLNode = Creator.CreateNode();
			TLNode->TimelineName = FName(*TimelineName);
			TLNode->bAutoPlay = bAutoPlay;
			TLNode->bLoop = bLoop;
			TLNode->NodePosX = PosX;
			TLNode->NodePosY = PosY;
			Creator.Finalize();

			if (TLTemplate)
			{
				TLTemplate->TimelineLength = Length;
				TLTemplate->bLoop = bLoop;
				TLTemplate->bAutoPlay = bAutoPlay;

				// Add float tracks
				const TArray<TSharedPtr<FJsonValue>>* Tracks;
				if (Obj->TryGetArrayField(TEXT("floatTracks"), Tracks))
				{
					for (const TSharedPtr<FJsonValue>& TrackVal : *Tracks)
					{
						const TSharedPtr<FJsonObject>& TrackObj = TrackVal->AsObject();
						if (!TrackObj.IsValid()) continue;

						FString TrackName = TrackObj->GetStringField(TEXT("name"));
						if (TrackName.IsEmpty()) continue;

						// Create inline curve
						UCurveFloat* Curve = NewObject<UCurveFloat>(TLTemplate, FName(*(TimelineName + TEXT("_") + TrackName)));

						// Add keyframes
						const TArray<TSharedPtr<FJsonValue>>* Keys;
						if (TrackObj->TryGetArrayField(TEXT("keys"), Keys))
						{
							for (const TSharedPtr<FJsonValue>& KeyVal : *Keys)
							{
								const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();
								if (!KeyObj.IsValid()) continue;

								float Time = (float)KeyObj->GetNumberField(TEXT("time"));
								float Value = (float)KeyObj->GetNumberField(TEXT("value"));

								FString InterpStr = KeyObj->GetStringField(TEXT("interp")).ToLower();
								ERichCurveInterpMode InterpMode = RCIM_Linear;
								if (InterpStr == TEXT("cubic") || InterpStr == TEXT("auto"))
									InterpMode = RCIM_Cubic;
								else if (InterpStr == TEXT("constant") || InterpStr == TEXT("step"))
									InterpMode = RCIM_Constant;

								FKeyHandle KeyHandle = Curve->FloatCurve.AddKey(Time, Value);
								Curve->FloatCurve.SetKeyInterpMode(KeyHandle, InterpMode);
							}
						}

						// Add the track to the template
						FTTFloatTrack NewTrack;
						NewTrack.SetTrackName(FName(*TrackName), TLTemplate);
						NewTrack.CurveFloat = Curve;
						TLTemplate->FloatTracks.Add(NewTrack);

						UE_LOG(LogNwiroBP, Log, TEXT("Timeline '%s': Added float track '%s' with %d keys"),
							*TimelineName, *TrackName, Keys ? Keys->Num() : 0);
					}
				}

				// Reconstruct node to show new track pins
				TLNode->ReconstructNode();
			}

			CreatedNode = TLNode;
		}
		// ---- Macro (ForEachLoop, Sequence, etc.) ----
		else if (NodeType.Equals(TEXT("Macro"), ESearchCase::IgnoreCase))
		{
			FString MacroName = Obj->GetStringField(TEXT("macro"));
			// Search for macro graph in engine blueprints
			UEdGraph* MacroGraph = nullptr;

			// Check standard macro library
			static UBlueprint* MacroLib = LoadObject<UBlueprint>(nullptr,
				TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));

			if (MacroLib)
			{
				for (UEdGraph* MG : MacroLib->MacroGraphs)
				{
					if (MG && MG->GetName().Equals(MacroName, ESearchCase::IgnoreCase))
					{
						MacroGraph = MG;
						break;
					}
				}
			}

			if (MacroGraph)
			{
				FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
				UK2Node_MacroInstance* MacroNode = Creator.CreateNode();
				MacroNode->SetMacroGraph(MacroGraph);
				MacroNode->NodePosX = PosX;
				MacroNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = MacroNode;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Macro not found: %s"), *MacroName));
			}
		}
		// ---- Cast ----
		else if (NodeType.Equals(TEXT("Cast"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("DynamicCast"), ESearchCase::IgnoreCase))
		{
			// "DynamicCast" is the spelling LLMs reach for first; alias it to the same handler.
			FString TargetClassName = Obj->GetStringField(TEXT("class"));
			UClass* CastClass = FindClassByName(TargetClassName);
			if (!CastClass) CastClass = FindClassByName(TargetClassName + TEXT("_C")); // user-BP generated class
			if (CastClass)
			{
				FGraphNodeCreator<UK2Node_DynamicCast> Creator(*Graph);
				UK2Node_DynamicCast* CastNode = Creator.CreateNode();
				CastNode->TargetType = CastClass;
				CastNode->NodePosX = PosX;
				CastNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = CastNode;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Cast target class not found: %s"), *TargetClassName));
			}
		}
		// ---- CastTo<ClassName> alias for a DynamicCast node (e.g. CastToBP_Door). LLMs reach
		// for this spelling; without a handler it fell through to the unknown-node scan, got
		// silently dropped, and every connect_pins referencing it cascaded into Node-not-found.
		else if (NodeType.StartsWith(TEXT("CastTo"), ESearchCase::IgnoreCase) && NodeType.Len() > 6)
		{
			const FString CastToClass = NodeType.Mid(6);
			UClass* CastClass = FindClassByName(CastToClass);
			if (!CastClass) CastClass = FindClassByName(CastToClass + TEXT("_C")); // user-BP generated class
			if (CastClass)
			{
				FGraphNodeCreator<UK2Node_DynamicCast> Creator(*Graph);
				UK2Node_DynamicCast* CastNode = Creator.CreateNode();
				CastNode->TargetType = CastClass;
				CastNode->NodePosX = PosX;
				CastNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = CastNode;
			}
			else
			{
				// Class not found/loaded: fall through to unknown-node error. Status stays Ok.
				Errors.Add(FString::Printf(TEXT("CastTo target class not found: %s"), *CastToClass));
			}
		}
		// ---- AddMappingContext (convenience: creates GetController→Cast→GetSubsystem→AddMappingContext chain) ----
		else if (NodeType.Equals(TEXT("AddMappingContext"), ESearchCase::IgnoreCase))
		{
			FString IMCPath = Obj->GetStringField(TEXT("context"));
			if (IMCPath.IsEmpty()) IMCPath = Obj->GetStringField(TEXT("mappingContext"));
			int32 Priority = Obj->HasField(TEXT("priority")) ? (int32)Obj->GetNumberField(TEXT("priority")) : 0;

			// Create 4 nodes: GetController → CastToPlayerController → GetSubsystem → AddMappingContext
			const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

			// 1. GetController
			UFunction* GetControllerFunc = ACharacter::StaticClass()->FindFunctionByName(FName(TEXT("GetController")));
			if (!GetControllerFunc) GetControllerFunc = APawn::StaticClass()->FindFunctionByName(FName(TEXT("GetController")));

			UK2Node_CallFunction* GetCtrlNode = nullptr;
			if (GetControllerFunc)
			{
				FGraphNodeCreator<UK2Node_CallFunction> C1(*Graph);
				GetCtrlNode = C1.CreateNode();
				GetCtrlNode->SetFromFunction(GetControllerFunc);
				GetCtrlNode->NodePosX = PosX;
				GetCtrlNode->NodePosY = PosY;
				C1.Finalize();
			}

			// 2. CastToPlayerController
			UK2Node_DynamicCast* CastNode = nullptr;
			{
				UClass* PCClass = APlayerController::StaticClass();
				FGraphNodeCreator<UK2Node_DynamicCast> C2(*Graph);
				CastNode = C2.CreateNode();
				CastNode->TargetType = PCClass;
				CastNode->NodePosX = PosX + 250;
				CastNode->NodePosY = PosY;
				C2.Finalize();
			}

			// 3. GetLocalPlayerSubSystemFromPlayerController (from SubsystemBlueprintLibrary)
			UFunction* GetSubsysFunc = nullptr;
			{
				UClass* SubsysBPLib = FindFirstObject<UClass>(TEXT("SubsystemBlueprintLibrary"));
				if (!SubsysBPLib) SubsysBPLib = FindFirstObject<UClass>(TEXT("USubsystemBlueprintLibrary"));
				if (SubsysBPLib)
				{
					GetSubsysFunc = SubsysBPLib->FindFunctionByName(FName(TEXT("GetLocalPlayerSubSystemFromPlayerController")));
					if (!GetSubsysFunc) GetSubsysFunc = SubsysBPLib->FindFunctionByName(FName(TEXT("GetLocalPlayerSubsystem")));
				}
				// Brute force if not found
				if (!GetSubsysFunc)
				{
					for (TObjectIterator<UClass> It; It; ++It)
					{
						GetSubsysFunc = It->FindFunctionByName(FName(TEXT("GetLocalPlayerSubSystemFromPlayerController")), EIncludeSuperFlag::ExcludeSuper);
						if (GetSubsysFunc && GetSubsysFunc->HasAnyFunctionFlags(FUNC_BlueprintCallable))
						{
							UE_LOG(LogNwiroBP, Log, TEXT("Found GetLocalPlayerSubSystemFromPlayerController in %s"), *It->GetName());
							break;
						}
						GetSubsysFunc = nullptr;
					}
				}
			}

			UK2Node_CallFunction* GetSubsysNode = nullptr;
			if (GetSubsysFunc)
			{
				FGraphNodeCreator<UK2Node_CallFunction> C3(*Graph);
				GetSubsysNode = C3.CreateNode();
				GetSubsysNode->SetFromFunction(GetSubsysFunc);
				GetSubsysNode->NodePosX = PosX + 500;
				GetSubsysNode->NodePosY = PosY;
				C3.Finalize();

				// Set the Class pin to UEnhancedInputLocalPlayerSubsystem
				UEdGraphPin* ClassPin = FindPin(GetSubsysNode, TEXT("Class"));
				if (ClassPin)
				{
					FString SubsysClassPath = UEnhancedInputLocalPlayerSubsystem::StaticClass()->GetPathName();
					K2Schema->TrySetDefaultObject(*ClassPin, UEnhancedInputLocalPlayerSubsystem::StaticClass());
				}
			}
			else
			{
				UE_LOG(LogNwiroBP, Warning, TEXT("AddMappingContext: GetLocalPlayerSubSystemFromPlayerController not found"));
			}

			// 4. AddMappingContext
			UFunction* AddMCFunc = UEnhancedInputLocalPlayerSubsystem::StaticClass()->FindFunctionByName(FName(TEXT("AddMappingContext")));
			if (!AddMCFunc)
			{
				// Search in interface hierarchy
				for (UClass* C = UEnhancedInputLocalPlayerSubsystem::StaticClass(); C; C = C->GetSuperClass())
				{
					AddMCFunc = C->FindFunctionByName(FName(TEXT("AddMappingContext")));
					if (AddMCFunc) break;
				}
			}
			// Also search interfaces
			if (!AddMCFunc)
			{
				for (const FImplementedInterface& Iface : UEnhancedInputLocalPlayerSubsystem::StaticClass()->Interfaces)
				{
					if (Iface.Class)
					{
						AddMCFunc = Iface.Class->FindFunctionByName(FName(TEXT("AddMappingContext")));
						if (AddMCFunc) break;
					}
				}
			}

			UK2Node_CallFunction* AddMCNode = nullptr;
			if (AddMCFunc)
			{
				FGraphNodeCreator<UK2Node_CallFunction> C4(*Graph);
				AddMCNode = C4.CreateNode();
				AddMCNode->SetFromFunction(AddMCFunc);
				AddMCNode->NodePosX = PosX + 750;
				AddMCNode->NodePosY = PosY;
				C4.Finalize();

				// Set IMC default value if path provided
				if (!IMCPath.IsEmpty())
				{
					UEdGraphPin* MCPin = FindPin(AddMCNode, TEXT("MappingContext"));
					if (MCPin)
					{
						// Load the IMC asset and set as default object
						UObject* IMCObj = LoadObject<UObject>(nullptr, *IMCPath);
						if (!IMCObj)
						{
							// Search by name in asset registry
							FAssetRegistryModule& ARM3 = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
							ARM3.Get().ScanPathsSynchronous({TEXT("/Game")}, true);
							FARFilter IMCFilter;
							IMCFilter.ClassPaths.Add(UInputMappingContext::StaticClass()->GetClassPathName());
							IMCFilter.bRecursivePaths = true;
							TArray<FAssetData> IMCAssets;
							ARM3.Get().GetAssets(IMCFilter, IMCAssets);
							for (const FAssetData& A : IMCAssets)
							{
								if (A.AssetName.ToString().Contains(IMCPath, ESearchCase::IgnoreCase))
								{
									IMCObj = A.GetAsset();
									break;
								}
							}
						}
						if (IMCObj)
						{
							K2Schema->TrySetDefaultObject(*MCPin, IMCObj);
							UE_LOG(LogNwiroBP, Log, TEXT("AddMappingContext: Set MappingContext to %s"), *IMCObj->GetName());
						}
						else
						{
							UE_LOG(LogNwiroBP, Warning, TEXT("AddMappingContext: IMC not found: %s"), *IMCPath);
						}
					}
				}

				// Set priority
				UEdGraphPin* PriorityPin = FindPin(AddMCNode, TEXT("Priority"));
				if (PriorityPin)
				{
					K2Schema->TrySetDefaultValue(*PriorityPin, FString::FromInt(Priority));
				}
			}

			// Wire them together
			if (GetCtrlNode && CastNode)
			{
				// GetController ReturnValue → Cast Object pin
				UEdGraphPin* CtrlOut = FindPin(GetCtrlNode, TEXT("ReturnValue"), EGPD_Output);
				UEdGraphPin* CastIn = FindPin(CastNode, TEXT("Object"), EGPD_Input);
				if (CtrlOut && CastIn) K2Schema->TryCreateConnection(CtrlOut, CastIn);

				// GetController exec → Cast exec
				UEdGraphPin* CtrlExecOut = FindPin(GetCtrlNode, TEXT("then"), EGPD_Output);
				if (!CtrlExecOut) CtrlExecOut = FindPin(GetCtrlNode, TEXT("execute"), EGPD_Output);
			}

			if (CastNode && GetSubsysNode)
			{
				// Cast "AsPlayer Controller" → GetSubsystem "PlayerController" pin
				// Note: Cast output pin name has space: "AsPlayer Controller"
				UEdGraphPin* CastResult = nullptr;
				for (UEdGraphPin* Pin : CastNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
					{
						CastResult = Pin;
						break;
					}
				}
				UEdGraphPin* SubsysPC = FindPin(GetSubsysNode, TEXT("PlayerController"), EGPD_Input);
				if (CastResult && SubsysPC)
				{
					K2Schema->TryCreateConnection(CastResult, SubsysPC);
					UE_LOG(LogNwiroBP, Log, TEXT("AddMappingContext: Connected Cast→GetSubsystem (%s→%s)"), *CastResult->PinName.ToString(), *SubsysPC->PinName.ToString());
				}
				else
				{
					UE_LOG(LogNwiroBP, Warning, TEXT("AddMappingContext: Failed to connect Cast→GetSubsystem (CastResult=%d, SubsysPC=%d)"), CastResult!=nullptr, SubsysPC!=nullptr);
				}
			}

			// Cast GetSubsystem result to EnhancedInputLocalPlayerSubsystem
			// This resolves the Object→Interface type mismatch
			UK2Node_DynamicCast* SubsysCastNode = nullptr;
			if (GetSubsysNode && AddMCNode)
			{
				FGraphNodeCreator<UK2Node_DynamicCast> C5(*Graph);
				SubsysCastNode = C5.CreateNode();
				SubsysCastNode->TargetType = UEnhancedInputLocalPlayerSubsystem::StaticClass();
				SubsysCastNode->NodePosX = PosX + 625;
				SubsysCastNode->NodePosY = PosY;
				C5.Finalize();

				// GetSubsystem ReturnValue → Cast Object
				UEdGraphPin* SubsysOut = FindPin(GetSubsysNode, TEXT("ReturnValue"), EGPD_Output);
				UEdGraphPin* CastObjIn = FindPin(SubsysCastNode, TEXT("Object"), EGPD_Input);
				if (SubsysOut && CastObjIn) K2Schema->TryCreateConnection(SubsysOut, CastObjIn);

				// Cast result → AddMappingContext self
				UEdGraphPin* CastResultOut = nullptr;
				for (UEdGraphPin* Pin : SubsysCastNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
					{
						CastResultOut = Pin;
						break;
					}
				}
				UEdGraphPin* MCTarget = FindPin(AddMCNode, TEXT("self"), EGPD_Input);
				if (CastResultOut && MCTarget)
				{
					K2Schema->TryCreateConnection(CastResultOut, MCTarget);
					UE_LOG(LogNwiroBP, Log, TEXT("AddMappingContext: Connected SubsysCast→AddMC (%s→%s)"), *CastResultOut->PinName.ToString(), *MCTarget->PinName.ToString());
				}
			}

			// Exec chain: PlayerController Cast → SubsysCast → AddMappingContext
			if (CastNode && SubsysCastNode && AddMCNode)
			{
				// PC Cast then → SubsysCast execute
				UEdGraphPin* PCCastExec = FindPin(CastNode, TEXT("then"), EGPD_Output);
				UEdGraphPin* SubCastExec = FindPin(SubsysCastNode, TEXT("execute"), EGPD_Input);
				if (PCCastExec && SubCastExec) K2Schema->TryCreateConnection(PCCastExec, SubCastExec);

				// SubsysCast then → AddMC execute
				UEdGraphPin* SubCastThen = FindPin(SubsysCastNode, TEXT("then"), EGPD_Output);
				UEdGraphPin* MCExecIn = FindPin(AddMCNode, TEXT("execute"), EGPD_Input);
				if (SubCastThen && MCExecIn) K2Schema->TryCreateConnection(SubCastThen, MCExecIn);
			}
			else if (CastNode && AddMCNode)
			{
				// Fallback: direct exec
				UEdGraphPin* CastExecOut = FindPin(CastNode, TEXT("then"), EGPD_Output);
				UEdGraphPin* MCExecIn = FindPin(AddMCNode, TEXT("execute"), EGPD_Input);
				if (CastExecOut && MCExecIn) K2Schema->TryCreateConnection(CastExecOut, MCExecIn);
			}

			// Auto-connect to BeginPlay if available and Cast exec input is free
			if (CastNode)
			{
				UEdGraphPin* CastExecIn = FindPin(CastNode, TEXT("execute"), EGPD_Input);
				if (CastExecIn && CastExecIn->LinkedTo.Num() == 0)
				{
					// Find BeginPlay event in this graph
					for (UEdGraphNode* N : Graph->Nodes)
					{
						UK2Node_Event* EvtNode = Cast<UK2Node_Event>(N);
						if (EvtNode && EvtNode->GetFunctionName().ToString().Contains(TEXT("BeginPlay")))
						{
							UEdGraphPin* BPExecOut = FindPin(EvtNode, TEXT("then"), EGPD_Output);
							if (BPExecOut && BPExecOut->LinkedTo.Num() == 0)
							{
								K2Schema->TryCreateConnection(BPExecOut, CastExecIn);
								UE_LOG(LogNwiroBP, Log, TEXT("AddMappingContext: Auto-connected BeginPlay→Cast"));
							}
							break;
						}
					}
				}
			}

			// Store refs
			if (GetCtrlNode) StoreNodeRef(Ref.IsEmpty() ? TEXT("get_controller") : Ref + TEXT("_getctrl"), GetCtrlNode, GraphName);
			if (CastNode) StoreNodeRef(Ref.IsEmpty() ? TEXT("cast_pc") : Ref + TEXT("_cast"), CastNode, GraphName);
			if (AddMCNode)
			{
				StoreNodeRef(Ref.IsEmpty() ? TEXT("add_mc") : Ref, AddMCNode, GraphName);
				// Return AddMCNode as created - its "then" pin is the exit for further chaining
				CreatedNode = AddMCNode;
			}
			else
			{
				Errors.Add(TEXT("AddMappingContext function not found"));
				if (CastNode) CreatedNode = CastNode;
			}

			Added += 3; // We created multiple nodes
		}
		// ---- Delay (uses CallFunction on KismetSystemLibrary::Delay) ----
		else if (NodeType.Equals(TEXT("Delay"), ESearchCase::IgnoreCase))
		{
			UFunction* DelayFunc = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FName(TEXT("Delay")));
			if (DelayFunc)
			{
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* DelayNode = Creator.CreateNode();
				DelayNode->SetFromFunction(DelayFunc);
				DelayNode->NodePosX = PosX;
				DelayNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = DelayNode;
			}
		}
		// ---- PrintString ----
		else if (NodeType.Equals(TEXT("PrintString"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("Print"), ESearchCase::IgnoreCase))
		{
			UFunction* PrintFunc = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FName(TEXT("PrintString")));
			if (PrintFunc)
			{
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* PrintNode = Creator.CreateNode();
				PrintNode->SetFromFunction(PrintFunc);
				PrintNode->NodePosX = PosX;
				PrintNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = PrintNode;
			}
		}
		// ---- SetTimer ----
		else if (NodeType.Equals(TEXT("SetTimer"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("SetTimerByFunctionName"), ESearchCase::IgnoreCase))
		{
			UFunction* TimerFunc = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FName(TEXT("K2_SetTimerDelegate")));
			if (!TimerFunc)
			{
				TimerFunc = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FName(TEXT("SetTimerByFunctionName")));
			}
			if (TimerFunc)
			{
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* TimerNode = Creator.CreateNode();
				TimerNode->SetFromFunction(TimerFunc);
				TimerNode->NodePosX = PosX;
				TimerNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = TimerNode;
			}
		}
		// ---- DestroyActor ----
		else if (NodeType.Equals(TEXT("DestroyActor"), ESearchCase::IgnoreCase))
		{
			UFunction* DestroyFunc = AActor::StaticClass()->FindFunctionByName(FName(TEXT("K2_DestroyActor")));
			if (DestroyFunc)
			{
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* DestroyNode = Creator.CreateNode();
				DestroyNode->SetFromFunction(DestroyFunc);
				DestroyNode->NodePosX = PosX;
				DestroyNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = DestroyNode;
			}
		}
		// ---- SetActorLocation ----
		else if (NodeType.Equals(TEXT("SetActorLocation"), ESearchCase::IgnoreCase))
		{
			UFunction* Func = AActor::StaticClass()->FindFunctionByName(FName(TEXT("K2_SetActorLocation")));
			if (Func)
			{
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* FuncNode = Creator.CreateNode();
				FuncNode->SetFromFunction(Func);
				FuncNode->NodePosX = PosX;
				FuncNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = FuncNode;
			}
		}
		// ---- GetActorLocation ----
		else if (NodeType.Equals(TEXT("GetActorLocation"), ESearchCase::IgnoreCase))
		{
			UFunction* Func = AActor::StaticClass()->FindFunctionByName(FName(TEXT("K2_GetActorLocation")));
			if (Func)
			{
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* FuncNode = Creator.CreateNode();
				FuncNode->SetFromFunction(Func);
				FuncNode->NodePosX = PosX;
				FuncNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = FuncNode;
			}
		}
		// ---- EnhancedInputAction (UE5 Enhanced Input) ----
		else if (NodeType.Equals(TEXT("EnhancedInputAction"), ESearchCase::IgnoreCase))
		{
			FString ActionPath = Obj->GetStringField(TEXT("action"));
			if (ActionPath.IsEmpty()) ActionPath = Obj->GetStringField(TEXT("name"));

			// Find the InputAction asset
			UInputAction* FoundAction = nullptr;

			// Try direct load
			if (!ActionPath.IsEmpty())
			{
				FoundAction = LoadObject<UInputAction>(nullptr, *ActionPath);

				// Try with /Game/ prefix
				if (!FoundAction && !ActionPath.StartsWith(TEXT("/")))
				{
					FoundAction = LoadObject<UInputAction>(nullptr, *(TEXT("/Game/") + ActionPath));
					if (!FoundAction) FoundAction = LoadObject<UInputAction>(nullptr, *(TEXT("/Game/Input/") + ActionPath));
				}

				// Search asset registry by name
				if (!FoundAction)
				{
					FAssetRegistryModule& ARM2 = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
					IAssetRegistry& AR2 = ARM2.Get();
					AR2.ScanPathsSynchronous({TEXT("/Game")}, true);

					FARFilter IAFilter;
					IAFilter.ClassPaths.Add(UInputAction::StaticClass()->GetClassPathName());
					IAFilter.bRecursivePaths = true;

					TArray<FAssetData> IAAssets;
					AR2.GetAssets(IAFilter, IAAssets);

					for (const FAssetData& Asset : IAAssets)
					{
						if (Asset.AssetName.ToString().Equals(ActionPath, ESearchCase::IgnoreCase) ||
							Asset.AssetName.ToString().Contains(ActionPath, ESearchCase::IgnoreCase))
						{
							FoundAction = Cast<UInputAction>(Asset.GetAsset());
							if (FoundAction) break;
						}
					}
				}
			}

			FGraphNodeCreator<UK2Node_EnhancedInputAction> Creator(*Graph);
			UK2Node_EnhancedInputAction* EIANode = Creator.CreateNode();

			if (FoundAction)
			{
				// Set InputAction property via reflection
				FObjectProperty* ActionProp = CastField<FObjectProperty>(
					UK2Node_EnhancedInputAction::StaticClass()->FindPropertyByName(FName(TEXT("InputAction"))));
				if (ActionProp)
				{
					ActionProp->SetObjectPropertyValue(
						ActionProp->ContainerPtrToValuePtr<void>(EIANode), FoundAction);
				}
				UE_LOG(LogNwiroBP, Log, TEXT("EnhancedInputAction: Set action to %s"), *FoundAction->GetName());
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("InputAction not found: %s"), *ActionPath));
			}

			EIANode->NodePosX = PosX;
			EIANode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = EIANode;
		}
		// ---- SwitchOnInt ----
		else if (NodeType.Equals(TEXT("SwitchOnInt"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("SwitchInteger"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_SwitchInteger> Creator(*Graph);
			UK2Node_SwitchInteger* SwitchNode = Creator.CreateNode();
			SwitchNode->NodePosX = PosX;
			SwitchNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = SwitchNode;
		}
		// ---- SwitchOnString ----
		else if (NodeType.Equals(TEXT("SwitchOnString"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("SwitchString"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_SwitchString> Creator(*Graph);
			UK2Node_SwitchString* SwitchNode = Creator.CreateNode();
			SwitchNode->NodePosX = PosX;
			SwitchNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = SwitchNode;
		}
		// ---- MakeArray ----
		else if (NodeType.Equals(TEXT("MakeArray"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_MakeArray> Creator(*Graph);
			UK2Node_MakeArray* ArrayNode = Creator.CreateNode();
			ArrayNode->NodePosX = PosX;
			ArrayNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = ArrayNode;
		}
		// ---- Select ----
		else if (NodeType.Equals(TEXT("Select"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_Select> Creator(*Graph);
			UK2Node_Select* SelectNode = Creator.CreateNode();
			SelectNode->NodePosX = PosX;
			SelectNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = SelectNode;
		}
		// ---- GetComponent: Gets a component variable from self (e.g., CharacterMovement) ----
		else if (NodeType.Equals(TEXT("GetComponent"), ESearchCase::IgnoreCase))
		{
			FString CompName = Obj->GetStringField(TEXT("component"));
			FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
			UK2Node_VariableGet* GetNode = Creator.CreateNode();
			GetNode->VariableReference.SetSelfMember(FName(*CompName));
			GetNode->NodePosX = PosX;
			GetNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = GetNode;
		}
		// ---- SetProperty: Sets a property on a target object (e.g., MaxWalkSpeed on CharacterMovement) ----
		else if (NodeType.Equals(TEXT("SetProperty"), ESearchCase::IgnoreCase))
		{
			FString PropName = GetFieldNormalized(Obj, TEXT("property"));
			FString OwnerClassName = GetFieldNormalized(Obj, TEXT("ownerclass")); // covers ownerClass / owner_class / OwnerClass

			UClass* OwnerClass = !OwnerClassName.IsEmpty() ? FindClassByName(OwnerClassName) : nullptr;

			FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
			UK2Node_VariableSet* SetNode = Creator.CreateNode();
			if (OwnerClass)
			{
				SetNode->VariableReference.SetExternalMember(FName(*PropName), OwnerClass);
				NodeDetail = FString::Printf(TEXT("SetProperty:'%s' [resolved] owner:'%s'"), *PropName, *OwnerClass->GetName());
			}
			else if (!OwnerClassName.IsEmpty())
			{
				SetNode->VariableReference.SetSelfMember(FName(*PropName));
				Errors.Add(FString::Printf(TEXT("SetProperty:'%s' [not_resolved] owner:'%s' not found — node created as self-member; fix ownerClass field"), *PropName, *OwnerClassName));
			}
			else
			{
				SetNode->VariableReference.SetSelfMember(FName(*PropName));
				NodeDetail = FString::Printf(TEXT("SetProperty:'%s' [resolved] self"), *PropName);
			}
			SetNode->NodePosX = PosX;
			SetNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = SetNode;
		}
		// ---- GetProperty: Gets a property from a target object ----
		else if (NodeType.Equals(TEXT("GetProperty"), ESearchCase::IgnoreCase))
		{
			FString PropName = GetFieldNormalized(Obj, TEXT("property"));
			FString OwnerClassName = GetFieldNormalized(Obj, TEXT("ownerclass")); // covers ownerClass / owner_class / OwnerClass

			UClass* OwnerClass = !OwnerClassName.IsEmpty() ? FindClassByName(OwnerClassName) : nullptr;

			FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
			UK2Node_VariableGet* GetNode = Creator.CreateNode();
			if (OwnerClass)
			{
				GetNode->VariableReference.SetExternalMember(FName(*PropName), OwnerClass);
				NodeDetail = FString::Printf(TEXT("GetProperty:'%s' [resolved] owner:'%s'"), *PropName, *OwnerClass->GetName());
			}
			else if (!OwnerClassName.IsEmpty())
			{
				GetNode->VariableReference.SetSelfMember(FName(*PropName));
				Errors.Add(FString::Printf(TEXT("GetProperty:'%s' [not_resolved] owner:'%s' not found — node created as self-member; fix ownerClass field"), *PropName, *OwnerClassName));
			}
			else
			{
				GetNode->VariableReference.SetSelfMember(FName(*PropName));
				NodeDetail = FString::Printf(TEXT("GetProperty:'%s' [resolved] self"), *PropName);
			}
			GetNode->NodePosX = PosX;
			GetNode->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = GetNode;
		}
		// ---- Sequence ----
		else if (NodeType.Equals(TEXT("Sequence"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("ExecutionSequence"), ESearchCase::IgnoreCase))
		{
			UK2Node_CallFunction* SeqNode = NewObject<UK2Node_CallFunction>(Graph);
			UFunction* Func = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("MakeLiteralInt"));
			// Use ExecutionSequence macro instead
			FGraphNodeCreator<UK2Node_ExecutionSequence> Creator(*Graph);
			UK2Node_ExecutionSequence* Node = Creator.CreateNode();
			Node->NodePosX = PosX;
			Node->NodePosY = PosY;
			Creator.Finalize();
			CreatedNode = Node;
		}
		// ---- ForEachLoop / WhileLoop (Macros) ----
		else if (NodeType.Equals(TEXT("ForEachLoop"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("ForLoop"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("WhileLoop"), ESearchCase::IgnoreCase))
		{
			FString MacroName;
			if (NodeType.Contains(TEXT("ForEach"))) MacroName = TEXT("ForEachLoop");
			else if (NodeType.Contains(TEXT("ForLoop")) || NodeType.Contains(TEXT("For"))) MacroName = TEXT("ForLoop");
			else MacroName = TEXT("WhileLoop");

			UEdGraph* MacroGraph = nullptr;
			TArray<UBlueprint*> MacroLibs;
			MacroLibs.Add(LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros")));
			for (UBlueprint* Lib : MacroLibs)
			{
				if (!Lib) continue;
				for (UEdGraph* G : Lib->MacroGraphs)
				{
					if (G && G->GetFName().ToString().Equals(MacroName, ESearchCase::IgnoreCase))
					{
						MacroGraph = G;
						break;
					}
				}
			}
			if (MacroGraph)
			{
				FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
				UK2Node_MacroInstance* MacroNode = Creator.CreateNode();
				MacroNode->SetMacroGraph(MacroGraph);
				MacroNode->NodePosX = PosX;
				MacroNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = MacroNode;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Macro not found: %s"), *MacroName));
			}
		}
		// ---- Gate / DoOnce / FlipFlop / DoN / IsValid (Macros) ----
		else if (NodeType.Equals(TEXT("Gate"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("DoOnce"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("FlipFlop"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("DoN"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("IsValid"), ESearchCase::IgnoreCase))
		{
			UEdGraph* MacroGraph = nullptr;
			UBlueprint* StdMacros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
			if (StdMacros)
			{
				for (UEdGraph* G : StdMacros->MacroGraphs)
				{
					if (G && G->GetFName().ToString().Equals(NodeType, ESearchCase::IgnoreCase))
					{
						MacroGraph = G;
						break;
					}
				}
			}
			if (MacroGraph)
			{
				FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
				UK2Node_MacroInstance* MacroNode = Creator.CreateNode();
				MacroNode->SetMacroGraph(MacroGraph);
				MacroNode->NodePosX = PosX;
				MacroNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = MacroNode;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Standard macro not found: %s"), *NodeType));
			}
		}
		// ---- Math shorthand nodes (Add, Subtract, Multiply, Divide, Clamp, Lerp, Abs, etc.) ----
		else if (NodeType.Equals(TEXT("Add"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Subtract"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Divide"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Clamp"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Lerp"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Abs"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Min"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Max"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Power"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("Sqrt"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("RandomFloat"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("RandomInteger"), ESearchCase::IgnoreCase))
		{
			// Map shorthand to actual function names
			FString FuncName = NodeType;
			if (NodeType.Equals(TEXT("Add"), ESearchCase::IgnoreCase)) FuncName = TEXT("Add_FloatFloat");
			else if (NodeType.Equals(TEXT("Subtract"), ESearchCase::IgnoreCase)) FuncName = TEXT("Subtract_FloatFloat");
			else if (NodeType.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase)) FuncName = TEXT("Multiply_FloatFloat");
			else if (NodeType.Equals(TEXT("Divide"), ESearchCase::IgnoreCase)) FuncName = TEXT("Divide_FloatFloat");
			else if (NodeType.Equals(TEXT("Lerp"), ESearchCase::IgnoreCase)) FuncName = TEXT("Lerp");
			else if (NodeType.Equals(TEXT("Clamp"), ESearchCase::IgnoreCase)) FuncName = TEXT("FClamp");
			else if (NodeType.Equals(TEXT("RandomFloat"), ESearchCase::IgnoreCase)) FuncName = TEXT("RandomFloatInRange");
			else if (NodeType.Equals(TEXT("RandomInteger"), ESearchCase::IgnoreCase)) FuncName = TEXT("RandomIntegerInRange");

			UFunction* Func = UKismetMathLibrary::StaticClass()->FindFunctionByName(FName(*FuncName));
			if (!Func) Func = UKismetMathLibrary::StaticClass()->FindFunctionByName(FName(*(TEXT("K") + FuncName)));
			if (Func)
			{
				// FGraphNodeCreator::Finalize() assigns a valid GUID — needed for
				// ref storage so connect_pins can find the node later.
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* FuncNode = Creator.CreateNode();
				FuncNode->SetFromFunction(Func);
				FuncNode->NodePosX = PosX;
				FuncNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = FuncNode;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Math function not found: %s -> %s"), *NodeType, *FuncName));
			}
		}
		// ---- Common shorthand functions ----
		else if (NodeType.Equals(TEXT("GetPlayerController"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetPlayerPawn"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetPlayerCharacter"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetPlayerCameraManager"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetGameMode"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetGameInstance"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetGameState"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("PlaySound2D"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("PlaySoundAtLocation"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SpawnEmitterAtLocation"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SpawnEmitterAttached"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("LineTraceByChannel"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SphereTraceByChannel"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("BoxTraceByChannel"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("CreateWidget"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("AddToViewport"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("RemoveFromParent"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetInputMode"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("FormatText"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("AppendString"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetWorldDeltaSeconds"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetActorRotation"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetActorRotation"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetActorScale3D"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetActorScale3D"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetActorTransform"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("GetActorTransform"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("AddActorWorldOffset"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("AddActorWorldRotation"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetActorHiddenInGame"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("IsOverlappingActor"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetVisibility"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetCollisionEnabled"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("PlayAnimMontage"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("StopAnimMontage"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetTimerByEvent"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("ClearTimer"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("OpenLevel"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("QuitGame"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetPercent"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("SetText"), ESearchCase::IgnoreCase) ||
				 NodeType.Equals(TEXT("ApplyDamage"), ESearchCase::IgnoreCase))
		{
			// Remove spaces from NodeType for function lookup
			FString FuncName = NodeType.Replace(TEXT(" "), TEXT(""));
			if (NodeType.Equals(TEXT("CreateWidget"), ESearchCase::IgnoreCase))
			{
				FuncName = TEXT("Create");
			}
			UFunction* Func = nullptr;

			TArray<UClass*> SearchLibs = {
				UGameplayStatics::StaticClass(),
				UKismetSystemLibrary::StaticClass(),
				UKismetMathLibrary::StaticClass(),
				UKismetStringLibrary::StaticClass(),
				UWidgetBlueprintLibrary::StaticClass(),
				UUserWidget::StaticClass(),
				UWidget::StaticClass(),
				UProgressBar::StaticClass(),
				UTextBlock::StaticClass(),
			};
			if (BP->GeneratedClass)
			{
				for (UClass* C = BP->GeneratedClass; C; C = C->GetSuperClass())
					SearchLibs.Add(C);
			}

			for (UClass* Lib : SearchLibs)
			{
				for (TFieldIterator<UFunction> It(Lib); It; ++It)
				{
					FString Name = It->GetName();
					const FString DisplayName = It->GetMetaData(TEXT("DisplayName")).Replace(TEXT(" "), TEXT(""));
					if (Name.Equals(FuncName, ESearchCase::IgnoreCase) || Name.Replace(TEXT("_"), TEXT("")).Equals(FuncName, ESearchCase::IgnoreCase) || DisplayName.Equals(NodeType.Replace(TEXT(" "), TEXT("")), ESearchCase::IgnoreCase))
					{
						Func = *It;
						break;
					}
				}
				if (Func) break;
			}

			if (Func)
			{
				// FGraphNodeCreator::Finalize() assigns a valid GUID — needed for
				// ref storage so connect_pins can find the node later.
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* FuncNode = Creator.CreateNode();
				FuncNode->SetFromFunction(Func);
				FuncNode->NodePosX = PosX;
				FuncNode->NodePosY = PosY;
				Creator.Finalize();
				if (NodeType.Equals(TEXT("CreateWidget"), ESearchCase::IgnoreCase))
				{
					FString WidgetClassName = GetFieldNormalized(Obj, TEXT("widgetclass"));
					if (WidgetClassName.IsEmpty()) WidgetClassName = GetFieldNormalized(Obj, TEXT("widgetblueprint"));
					if (WidgetClassName.IsEmpty()) WidgetClassName = GetFieldNormalized(Obj, TEXT("class"));
					if (!WidgetClassName.IsEmpty())
					{
						if (UClass* WidgetClass = FindClassByName(WidgetClassName))
						{
							if (UEdGraphPin* WidgetTypePin = FindPin(FuncNode, TEXT("WidgetType"), EGPD_Input))
							{
								GetDefault<UEdGraphSchema_K2>()->TrySetDefaultObject(*WidgetTypePin, WidgetClass);
								NodeDetail = FString::Printf(TEXT("CreateWidget [resolved] class:'%s'"), *WidgetClass->GetName());
							}
						}
						else
						{
							Errors.Add(FString::Printf(TEXT("CreateWidget [not_resolved] class:'%s' not found — node created without WidgetType; use class/widgetClass/widgetBlueprint or set_pin_defaults"), *WidgetClassName));
						}
					}
				}
				CreatedNode = FuncNode;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Function not found: %s"), *NodeType));
			}
		}
		else
		{
			// Unknown node type — last-chance fallback: try to resolve as a
			// global function name. Many BP-callable functions have a "K2_"
			// prefix on the C++ side (K2_AddActorLocalRotation etc.) but get
			// shown to the user without it via DisplayName metadata. So try
			// both the bare name and the K2_-prefixed name.
			UFunction* FallbackFn = nullptr;
			const FString CleanName = NodeType.Replace(TEXT(" "), TEXT(""));
			const FString K2Name = TEXT("K2_") + CleanName;
			for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
			{
				for (TFieldIterator<UFunction> FnIt(*ClassIt, EFieldIterationFlags::None); FnIt; ++FnIt)
				{
					const FString FN = FnIt->GetName();
					if (FN.Equals(CleanName, ESearchCase::IgnoreCase)
						|| FN.Equals(K2Name, ESearchCase::IgnoreCase))
					{
						FallbackFn = *FnIt; break;
					}
					// Also check display-name metadata for things like
					// `BlueprintInternalUseOnly`-flagged renames.
					const FString DisplayName = FnIt->GetMetaData(TEXT("DisplayName"));
					if (!DisplayName.IsEmpty()
						&& DisplayName.Replace(TEXT(" "), TEXT("")).Equals(CleanName, ESearchCase::IgnoreCase))
					{
						FallbackFn = *FnIt; break;
					}
				}
				if (FallbackFn) break;
			}
			// Math-op auto-promote: if NodeType is a bare math operator like
			// "Subtract" / "Divide" / "LessEqual" / etc., the actual UFunction
			// is suffixed (e.g. Subtract_DoubleDouble) so the bare-name search
			// above misses. Try the same _DoubleDouble/_FloatFloat/_IntInt
			// suffixes the explicit CallFunction path uses.
			if (!FallbackFn)
			{
				static const TArray<FString> MathOps = {
					TEXT("Add"), TEXT("Subtract"), TEXT("Multiply"), TEXT("Divide"),
					TEXT("Min"), TEXT("Max"), TEXT("Equal"), TEXT("NotEqual"),
					TEXT("Less"), TEXT("LessEqual"), TEXT("Greater"), TEXT("GreaterEqual"),
				};
				for (const FString& Op : MathOps)
				{
					if (!CleanName.Equals(Op, ESearchCase::IgnoreCase)) continue;
					const TArray<FString> Suffixes = {
						TEXT("_DoubleDouble"), TEXT("_FloatFloat"),
						TEXT("_IntInt"), TEXT("_VectorVector"),
					};
					for (const FString& Suf : Suffixes)
					{
						UFunction* F = UKismetMathLibrary::StaticClass()->FindFunctionByName(FName(*(Op + Suf)));
						if (F) { FallbackFn = F; break; }
					}
					break;
				}
			}

			if (FallbackFn)
			{
				// Use FGraphNodeCreator so the node gets a valid GUID via Finalize() —
				// without it the ref store would key on a zero GUID and downstream
				// connect_pins lookups would collide with other nodes.
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* FuncNode = Creator.CreateNode();
				FuncNode->SetFromFunction(FallbackFn);
				FuncNode->NodePosX = PosX;
				FuncNode->NodePosY = PosY;
				Creator.Finalize();
				CreatedNode = FuncNode;
				NodeDetail = FString::Printf(TEXT("%s [fallback] resolved as function %s in %s"),
					*FallbackFn->GetName(), *FallbackFn->GetName(),
					FallbackFn->GetOwnerClass() ? *FallbackFn->GetOwnerClass()->GetName() : TEXT("unknown"));
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Unknown node type: %s (also tried as a function name across all UClasses with K2_ prefix and DisplayName)"), *NodeType));
			}
		}

		if (CreatedNode)
		{
			// Auto-set pin defaults from extra JSON fields (key, axisName, value, etc.)
			static const TSet<FString> NodeReservedKeys = { TEXT("ref"), TEXT("id"), TEXT("type"), TEXT("class"),
				TEXT("function"), TEXT("event"), TEXT("target"), TEXT("action"), TEXT("variable"),
				TEXT("x"), TEXT("y"), TEXT("posX"), TEXT("posY"), TEXT("params") };

			const UEdGraphSchema_K2* AutoSchema = GetDefault<UEdGraphSchema_K2>();
			for (const auto& Pair : Obj->Values)
			{
				const FString Key(*Pair.Key);
				if (NodeReservedKeys.Contains(Key)) continue;

				FString PinVal;
				if (Pair.Value->Type == EJson::String) PinVal = Pair.Value->AsString();
				else if (Pair.Value->Type == EJson::Number) PinVal = FString::SanitizeFloat(Pair.Value->AsNumber());
				else if (Pair.Value->Type == EJson::Boolean) PinVal = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
				else continue;

				// Find pin by name (case-insensitive)
				for (UEdGraphPin* Pin : CreatedNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Input &&
						Pin->PinName.ToString().Equals(Key, ESearchCase::IgnoreCase))
					{
						AutoSchema->TrySetDefaultValue(*Pin, PinVal);
						break;
					}
				}
			}

			StoreNodeRef(Ref.IsEmpty() ? FString::Printf(TEXT("node_%d"), Added) : Ref, CreatedNode, GraphName);
			// Also register a secondary ref keyed on the node's editable title
			// so the LLM can connect later via the friendly name it sees in
			// read_blueprint output. If a node with that title already exists
			// in the ref map, append an index suffix so each node stays
			// uniquely addressable (e.g. "Add Actor Local Rotation",
			// "Add Actor Local Rotation 2").
			if (CreatedNode)
			{
				const FString Title = CreatedNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
				if (!Title.IsEmpty())
				{
					FString Key = Title;
					int32 Suffix = 2;
					while (NodeRefs.Contains(Key))
					{
						Key = FString::Printf(TEXT("%s %d"), *Title, Suffix++);
					}
					StoreNodeRef(Key, CreatedNode, GraphName);
				}
			}
			Added++;
			if (NodeDetail.IsEmpty()) NodeDetail = CreatedNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
			// Node GUIDs were appended here, but the agent references nodes by title (see
			// StoreNodeRef above), never by guid, so the 36-char guid per node was pure
			// response bloat across multi-node edits. Dropped.
			NodeDetails.Add(NodeDetail);
		}

		NodeX += 300;
		if (Added % 5 == 0)
		{
			NodeX = 300;
			NodeY += 300;
		}
	}

	FString Msg = FString::Printf(TEXT("Added %d node(s)"), Added);
	if (NodeDetails.Num() > 0) Msg += TEXT(". Details: ") + FString::Join(NodeDetails, TEXT("; "));
	if (Skipped > 0) Msg += FString::Printf(TEXT(", skipped %d duplicate(s)"), Skipped);
	if (Errors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));

	// Always Ok — same rationale as connect_pins below: errors here are
	// usually "unknown node type" hallucinations, not state corruption.
	return FNwiroIKBPResult::Ok(Msg);
}

// ============================================================
// CONNECT PINS
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoConnectPins(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	UEdGraph* Graph = FindGraph(BP, GraphName);
	if (!Graph) return FNwiroIKBPResult::Fail(FString::Printf(TEXT("Graph not found: %s"), *GraphName));

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	int32 Connected = 0;
	TArray<FString> Errors;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		// Format: "from"/"source": "nodeRef.PinName", "to"/"target": "nodeRef.PinName"
		FString FromStr = Obj->HasField(TEXT("from")) ? Obj->GetStringField(TEXT("from"))
			: Obj->HasField(TEXT("source")) ? Obj->GetStringField(TEXT("source")) : TEXT("");
		FString ToStr = Obj->HasField(TEXT("to")) ? Obj->GetStringField(TEXT("to"))
			: Obj->HasField(TEXT("target")) ? Obj->GetStringField(TEXT("target")) : TEXT("");

		if (FromStr.IsEmpty() || ToStr.IsEmpty())
		{
			Errors.Add(TEXT("Connection needs both 'from' and 'to'"));
			continue;
		}

		// Build a snapshot of every available "ref.pin" combo in this graph,
		// so we can hand it back in error messages and let the LLM self-correct
		// instead of nuking the graph and starting over.
		auto BuildAvailableRefs = [&]() -> FString
		{
			TArray<FString> Lines;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				// Node-not-found only needs the valid node REF to pick from, not its pins.
				// The all-pins dump per node was the dominant edit_blueprint response bloat
				// (multi-KB error strings on big graphs, accumulated across self-correction
				// turns until the agent's token budget ran out). Pins are still surfaced for
				// the targeted PinNotFound case below, where they are actually actionable.
				Lines.Add(FString::Printf(TEXT("'%s'"), *Node->GetNodeTitle(ENodeTitleType::EditableTitle).ToString()));
			}
			return FString::Join(Lines, TEXT(" | "));
		};

		// Parse "ref.pinName" or "ref:pinName" — accept either separator. The
		// LLM keeps mixing them up. Splits at the LAST dot/colon so node refs
		// containing periods (e.g. asset paths) still work.
		enum class EParseResult : uint8 { Ok, NoDot, NodeNotFound, PinNotFound };
		auto ParsePinRef = [&](const FString& Str, UEdGraphNode*& OutNode, UEdGraphPin*& OutPin) -> EParseResult
		{
			int32 SepIdx = INDEX_NONE;
			Str.FindLastChar('.', SepIdx);
			int32 ColonIdx = INDEX_NONE;
			Str.FindLastChar(':', ColonIdx);
			if (ColonIdx > SepIdx) SepIdx = ColonIdx;
			if (SepIdx == INDEX_NONE) return EParseResult::NoDot;

			const FString NodeRef = Str.Left(SepIdx);
			const FString PinName = Str.Mid(SepIdx + 1);

			OutNode = FindNodeByRef(Graph, NodeRef);
			if (!OutNode) return EParseResult::NodeNotFound;

			OutPin = FindPin(OutNode, PinName);
			return OutPin ? EParseResult::Ok : EParseResult::PinNotFound;
		};

		UEdGraphNode* FromNode = nullptr;
		UEdGraphPin* FromPin = nullptr;
		UEdGraphNode* ToNode = nullptr;
		UEdGraphPin* ToPin = nullptr;

		auto ExplainFailure = [&](const FString& Str, EParseResult R, UEdGraphNode* Node) -> FString
		{
			switch (R)
			{
				case EParseResult::NoDot:
					return FString::Printf(TEXT("'%s' missing dot — expected format 'NodeRef.PinName'"), *Str);
				case EParseResult::NodeNotFound:
					return FString::Printf(TEXT("Node not found in '%s'. Available node refs: %s"),
						*Str, *BuildAvailableRefs());
				case EParseResult::PinNotFound:
				{
					TArray<FString> InPins, OutPins;
					if (Node) for (UEdGraphPin* Pin : Node->Pins)
					{
						if (Pin) (Pin->Direction == EGPD_Input ? InPins : OutPins).Add(Pin->PinName.ToString());
					}
					return FString::Printf(TEXT("Pin not found in '%s'. The node exists but its pins are inputs=[%s] outputs=[%s]"),
						*Str,
						*FString::Join(InPins, TEXT(",")),
						*FString::Join(OutPins, TEXT(",")));
				}
				default:
					return FString::Printf(TEXT("Unknown parse error for '%s'"), *Str);
			}
		};

		const EParseResult FromR = ParsePinRef(FromStr, FromNode, FromPin);
		if (FromR != EParseResult::Ok)
		{
			Errors.Add(ExplainFailure(FromStr, FromR, FromNode));
			continue;
		}

		const EParseResult ToR = ParsePinRef(ToStr, ToNode, ToPin);
		if (ToR != EParseResult::Ok)
		{
			Errors.Add(ExplainFailure(ToStr, ToR, ToNode));
			continue;
		}

		// Pre-check: reject unrelated PC_Object connections that TryCreateConnection
		// silently accepts but the compiler then rejects (e.g. Actor → StaticMeshComponent).
		{
			UClass* FromClass = Cast<UClass>(FromPin->PinType.PinSubCategoryObject.Get());
			UClass* ToClass   = Cast<UClass>(ToPin->PinType.PinSubCategoryObject.Get());
			const bool bFromIsObj = FromPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object;
			const bool bToIsObj   = ToPin->PinType.PinCategory   == UEdGraphSchema_K2::PC_Object;
			if (bFromIsObj && bToIsObj && FromClass && ToClass &&
				!FromClass->IsChildOf(ToClass) && !ToClass->IsChildOf(FromClass))
			{
				Errors.Add(FString::Printf(
					TEXT("Type mismatch: cannot connect %s (%s) to %s (%s) — unrelated types. Did you mean to call Get<ComponentName> first?"),
					*FromStr, *FromClass->GetName(), *ToStr, *ToClass->GetName()));
				continue;
			}
		}

		// Try to connect
		bool bConnected = Schema->TryCreateConnection(FromPin, ToPin);

		if (!bConnected)
		{
			// Try swapping direction (schema may auto-resolve)
			bConnected = Schema->TryCreateConnection(ToPin, FromPin);
		}

		if (bConnected)
		{
			Connected++;
		}
		else
		{
			FString Reason;
			if (FromPin->PinType.PinCategory == ToPin->PinType.PinCategory)
			{
				UClass* FC = Cast<UClass>(FromPin->PinType.PinSubCategoryObject.Get());
				UClass* TC = Cast<UClass>(ToPin->PinType.PinSubCategoryObject.Get());
				if (!FC && TC)
					Reason = FString::Printf(TEXT(" (type mismatch: source is untyped UObject but destination requires %s — did you forget 'subtype'?)"), *TC->GetName());
				else if (FC && TC && !FC->IsChildOf(TC) && !TC->IsChildOf(FC))
					Reason = FString::Printf(TEXT(" (type mismatch: %s is not a %s)"), *FC->GetName(), *TC->GetName());
			}
			Errors.Add(FString::Printf(TEXT("Failed to connect: %s -> %s%s"), *FromStr, *ToStr, *Reason));
		}
	}

	FString Msg = FString::Printf(TEXT("Connected %d pin pair(s)"), Connected);
	if (Errors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));

	// Always return success — connect_pins errors usually mean the caller
	// passed bad node refs (typo, stale ref). That's informative in the
	// message, not a tool failure. The graph is still in a consistent state.
	return FNwiroIKBPResult::Ok(Msg);
}

// ============================================================
// SET PIN DEFAULTS
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoSetPinDefaults(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	UEdGraph* Graph = FindGraph(BP, GraphName);
	if (!Graph) return FNwiroIKBPResult::Fail(FString::Printf(TEXT("Graph not found: %s"), *GraphName));

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	int32 Set = 0;
	TArray<FString> Errors;
	TArray<FString> Details;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		// Accept either "node" or "ref" for consistency with add_nodes/connect_pins
		FString NodeRef = Obj->HasField(TEXT("ref")) ? Obj->GetStringField(TEXT("ref")) : Obj->GetStringField(TEXT("node"));
		FString PinName = Obj->GetStringField(TEXT("pin"));
		FString Value = Obj->GetStringField(TEXT("value"));

		UEdGraphNode* Node = FindNodeByRef(Graph, NodeRef);
		if (!Node)
		{
			// Bounded redirect: items shaped like a component-property attempt
			// (componentName/component instead of ref/pin) are a common model
			// mistake — point at the right tools. Never auto-route.
			if (Obj->HasField(TEXT("componentName")) || Obj->HasField(TEXT("component")) || (NodeRef.IsEmpty() && Obj->HasField(TEXT("property"))))
			{
				Errors.Add(TEXT("set_pin_defaults sets graph NODE pins (needs ref/pin/value). To set a component property, use add_components 'properties' or the edit_component tool"));
				continue;
			}
			Errors.Add(FString::Printf(TEXT("Node not found: %s"), *NodeRef));
			continue;
		}

		UEdGraphPin* Pin = FindPin(Node, PinName, EGPD_Input);
		if (!Pin) { Errors.Add(FString::Printf(TEXT("'%s.%s' [not_resolved] pin not found — check pin name"), *NodeRef, *PinName)); continue; }

		// For Class pins, resolve the class by name and set as DefaultObject
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
		{
			UClass* ResolvedClass = FindClassByName(Value);
			if (!ResolvedClass) ResolvedClass = FindFirstObject<UClass>(*Value);
			if (!ResolvedClass) ResolvedClass = StaticLoadClass(UObject::StaticClass(), nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *Value));
			if (!ResolvedClass) ResolvedClass = StaticLoadClass(UObject::StaticClass(), nullptr, *Value);
			if (ResolvedClass)
				Schema->TrySetDefaultObject(*Pin, ResolvedClass);
			else
				Schema->TrySetDefaultValue(*Pin, Value);
		}
		// For Object pins, try loading the asset and using TrySetDefaultObject
		else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Interface ||
			Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject)
		{
			UObject* Asset = LoadObject<UObject>(nullptr, *Value);
			if (!Asset)
			{
				// Try searching asset registry
				FAssetRegistryModule& PinARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
				PinARM.Get().ScanPathsSynchronous({TEXT("/Game")}, true);
				TArray<FAssetData> PinAssets;
				PinARM.Get().GetAssetsByPackageName(FName(*Value.Left(Value.Find(TEXT(".")))), PinAssets);
				if (PinAssets.Num() > 0) Asset = PinAssets[0].GetAsset();
			}
			if (Asset)
			{
				Schema->TrySetDefaultObject(*Pin, Asset);
			}
			else
			{
				Schema->TrySetDefaultValue(*Pin, Value);
			}
		}
		else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			// Struct pins (FRotator, FVector, FLinearColor, ...) need a UE struct literal like
			// "(Pitch=0,Yaw=90,Roll=0)". A bare "0,0,90" or "Yaw=90" silently fails to parse and
			// the default stays zeroed (the door-rotation bug). Normalize common shapes here.
			FString StructVal = Value.TrimStartAndEnd();
			if (!StructVal.StartsWith(TEXT("(")))
			{
				const UScriptStruct* SS = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
				if (SS && StructVal.Contains(TEXT(",")) && !StructVal.Contains(TEXT("=")))
				{
					// Bare comma list -> map positionally to the struct's properties in order.
					TArray<FString> Parts; StructVal.ParseIntoArray(Parts, TEXT(","), true);
					TArray<FString> Pairs; int32 PIdx = 0;
					for (TFieldIterator<FProperty> PropIt(SS); PropIt && PIdx < Parts.Num(); ++PropIt, ++PIdx)
						Pairs.Add(FString::Printf(TEXT("%s=%s"), *PropIt->GetName(), *Parts[PIdx].TrimStartAndEnd()));
					if (Pairs.Num() > 0) StructVal = TEXT("(") + FString::Join(Pairs, TEXT(",")) + TEXT(")");
				}
				else if (StructVal.Contains(TEXT("=")))
				{
					// Named pairs without wrapping parens: "Yaw=90" -> "(Yaw=90)".
					StructVal = TEXT("(") + StructVal + TEXT(")");
				}
			}
			Schema->TrySetDefaultValue(*Pin, StructVal);
		}
		else
		{
			Schema->TrySetDefaultValue(*Pin, Value);
		}
		Set++;
		Details.Add(FString::Printf(TEXT("'%s.%s'=%s [resolved]"), *NodeRef, *PinName, *Value));
	}

	FString Msg = FString::Printf(TEXT("Set %d pin default(s)"), Set);
	if (Details.Num() > 0) Msg += TEXT(": ") + FString::Join(Details, TEXT(", "));
	if (Errors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));

	return Set > 0 || Errors.Num() == 0 ? FNwiroIKBPResult::Ok(Msg) : FNwiroIKBPResult::Fail(Msg);
}

// ============================================================
// CREATE BLUEPRINT
// ============================================================

FString FNwiroIKBlueprintTools::CreateBlueprint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	FString ParentClassName = Cmd->GetStringField(TEXT("parentClass"));

	if (Name.IsEmpty())
	{
		return TEXT("{\"success\": false, \"error\": \"Missing 'name' field\"}");
	}

	// FName has a NAME_SIZE assertion (~1024 chars) that fatal-errors on
	// oversize input. Cap before any code path can wrap the string in FName.
	// Discovered by fuzz/blueprint-2: `create_blueprint{name: "A"*100000}`
	// crashed UnrealNames.cpp:3206.
	if (Name.Len() > 256 || Path.Len() > 1024 || ParentClassName.Len() > 1024)
	{
		return TEXT("{\"success\": false, \"error\": \"Name/path too long. Asset names must be <256 chars and paths <1024 chars (UE FName limit).\"}");
	}

	if (Path.IsEmpty())
	{
		Path = TEXT("/Game");
	}

	// Hallucination-tolerant detection: the LLM frequently passes one of many
	// variants when it wants a Blueprint Interface. There is no UClass called
	// "BlueprintInterface" — BPIs are normal blueprints with BPTYPE_Interface
	// and parent = UInterface. We detect intent here and create the right
	// asset, instead of silently falling back to Actor.
	auto IsInterfaceIntent = [](const FString& In) -> bool
	{
		if (In.IsEmpty()) return false;
		FString S = In.ToLower();
		S.RemoveFromStart(TEXT("u"));     // UInterface → interface
		S.RemoveFromStart(TEXT("/"));     // /Script/CoreUObject.Interface → script...
		// Strip namespace qualifiers
		int32 Dot; if (S.FindLastChar('.', Dot)) S = S.RightChop(Dot + 1);
		return S == TEXT("blueprintinterface")
			|| S == TEXT("blueprint_interface")
			|| S == TEXT("bpi")
			|| S == TEXT("interface");
	};
	const bool bWantsInterface = IsInterfaceIntent(ParentClassName);

	// Find parent class
	UClass* ParentClass = bWantsInterface ? UInterface::StaticClass() : AActor::StaticClass();
	if (!ParentClassName.IsEmpty() && !bWantsInterface)
	{
		UClass* Found = FindClassByName(ParentClassName);
		if (Found) ParentClass = Found;
	}

	FString FullPath = Path / Name;
	{ const FString _C = NwiroCheckCreateConflict(FullPath, Name, UBlueprint::StaticClass()); if (!_C.IsEmpty()) return _C; }

	// Check if blueprint already exists
	UBlueprint* ExistingBP = LoadObject<UBlueprint>(nullptr, *FullPath);
	if (ExistingBP)
	{
		return FString::Printf(TEXT("{\"success\": true, \"name\": \"%s\", \"path\": \"%s\", \"message\": \"Blueprint already exists\"}"), *Name, *ExistingBP->GetPathName());
	}

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateBlueprint", "AI: Create Blueprint"));

	UPackage* Package = CreatePackage(*FullPath);
	if (!Package)
	{
		Tx.Cancel();
		return TEXT("{\"success\": false, \"error\": \"Failed to create package\"}");
	}
	Tx.AlsoModify(Package);

	UBlueprint* NewBP = nullptr;

	{
		// Prevent GC during entire create+compile cycle
		FGCScopeGuard GCScopeGuard;

		Package->AddToRoot();

		// BPType: Interface for BPIs (so `add_interface` can find them later),
		// Normal otherwise.
		const EBlueprintType BPType = bWantsInterface ? BPTYPE_Interface : BPTYPE_Normal;
		NewBP = FKismetEditorUtilities::CreateBlueprint(
			ParentClass, Package, FName(*Name), BPType, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

		if (!NewBP)
		{
			Package->RemoveFromRoot();
			return TEXT("{\"success\": false, \"error\": \"Failed to create blueprint\"}");
		}

		NewBP->AddToRoot();
		FAssetRegistryModule::AssetCreated(NewBP);
		NewBP->MarkPackageDirty();

		FKismetEditorUtilities::CompileBlueprint(NewBP);

		// Post-compile repair: ONLY if compile failed to populate ClassConstructor
		// or ClassWithin. Do NOT force-rebuild CDO of a successfully compiled BP —
		// that corrupts SceneRootComponent ownership (parent CDO's components get
		// inherited with parent's owner, triggering
		// "SceneRootComponent->GetOwner() == this" assert during SpawnActor).
		if (UClass* GenClass = NewBP->GeneratedClass)
		{
			const bool bNeedsConstructor = (GenClass->ClassConstructor == nullptr);
			const bool bNeedsWithin = (GenClass->ClassWithin == nullptr);
			if (bNeedsConstructor || bNeedsWithin)
			{
				if (UClass* SuperClass = GenClass->GetSuperClass())
				{
					if (!SuperClass->ClassConstructor) SuperClass->Bind();
				}
				if (bNeedsWithin)
				{
					UClass* ParentWithin = GenClass->GetSuperClass() ? GenClass->GetSuperClass()->ClassWithin.Get() : nullptr;
					GenClass->ClassWithin = ParentWithin ? ParentWithin : UObject::StaticClass();
				}
				GenClass->Bind();
				GenClass->StaticLink(true);
				GenClass->GetDefaultObject(true);
			}
		}

		NewBP->RemoveFromRoot();
		Package->RemoveFromRoot();
	}

	// AGENT_POLICY §6: BP must be openable in editor. That requires it on
	// disk with a fully-baked CDO. Without explicit save, the BP lives only
	// in memory and dies on UE restart — user sees "invalid class" on the
	// stale .uasset. Python EditorAssetLibrary.save_asset is now blocked
	// (crashes on freshly created BPs), so MCP itself must save here.
	//
	// CRITICAL: only save when the CDO is fully constructible. SaveLoadedAsset
	// triggers package validation + thumbnail generation, which instantiates
	// the CDO. If ClassConstructor / ClassWithin are null (compile produced a
	// half-baked class), that path asserts "InClass->ClassConstructor"
	// (UObjectGlobals.cpp:3396) and takes down the editor. Better to skip the
	// save (BP survives in memory this session) than crash.
	{
		UClass* GenCls = NewBP->GeneratedClass;
		const bool bConstructible = GenCls && GenCls->IsValidLowLevel()
			&& GenCls->ClassConstructor != nullptr
			&& GenCls->ClassWithin != nullptr
			&& GenCls->GetDefaultObject(false) != nullptr;
		if (bConstructible)
		{
			UEditorAssetLibrary::SaveLoadedAsset(NewBP, false);
		}
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Name);
	Result->SetStringField(TEXT("path"), NewBP->GetPathName());
	Result->SetStringField(TEXT("parentClass"), ParentClass->GetName());

	// nextSteps: tell the agent what this empty BP still needs. Agents often
	// stop after create_blueprint thinking the asset alone fulfills the
	// user's intent — it doesn't. These hints are generic per parent class
	// type, not scenario-specific. Helps every prompt that creates a BP.
	TArray<TSharedPtr<FJsonValue>> NextSteps;
	auto AddStep = [&NextSteps](const FString& S)
	{
		NextSteps.Add(MakeShareable(new FJsonValueString(S)));
	};
	const FString PCN = ParentClass->GetName();
	const bool bIsActor = ParentClass->IsChildOf(AActor::StaticClass());
	const bool bIsPawn = ParentClass->IsChildOf(APawn::StaticClass());
	const bool bIsCharacter = ParentClass->IsChildOf(ACharacter::StaticClass());
	const bool bIsGameMode = ParentClass->IsChildOf(AGameModeBase::StaticClass());
	const bool bIsWidget = PCN == TEXT("UserWidget") || ParentClass->GetName().Contains(TEXT("UserWidget"));

	AddStep(TEXT("This blueprint was created EMPTY (no components, no variables, no graph nodes). It will not do anything yet."));
	if (bIsCharacter)
	{
		AddStep(TEXT("As a Character: typical setup needs a Camera component, gameplay variables (Health, Speed, etc.), and event-graph wiring (BeginPlay, input bindings). Use edit_blueprint with add_components/add_variables/add_nodes."));
	}
	else if (bIsPawn)
	{
		AddStep(TEXT("As a Pawn: typical setup needs a visual mesh component, a Camera or SpringArm, and input handling. Use edit_blueprint."));
	}
	else if (bIsActor)
	{
		AddStep(TEXT("As an Actor: add mesh/collision components and any required variables via edit_blueprint."));
	}
	else if (bIsGameMode)
	{
		AddStep(TEXT("As a GameMode: set DefaultPawnClass via set_cdo_property, then call set_game_mode to activate it in the world."));
	}
	else if (bIsWidget)
	{
		AddStep(TEXT("As a UserWidget: add UI elements via add_widget (TextBlock, Button, ComboBox, CheckBox, Slider, Image, etc.). For UI logic, use edit_blueprint with the EventGraph or function graphs."));
	}
	if (bIsActor && !bIsGameMode)
	{
		AddStep(FString::Printf(TEXT("WHEN COMPLETE: call spawn_actor with blueprint='%s' to place an instance in the level. The BP sitting in the content browser alone does not appear in-game."), *NewBP->GetPathName()));
	}
	AddStep(TEXT("VERIFY before stopping: call read_blueprint with this path and check that components/variables/event-nodes are present. An empty BP means the user's request is not satisfied."));
	Result->SetArrayField(TEXT("nextSteps"), NextSteps);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);

	UE_LOG(LogNwiroBP, Log, TEXT("Created blueprint: %s (parent: %s)"), *Name, *ParentClass->GetName());
	return Out;
}

// ============================================================
// CLASS RESOLUTION (static method)
// ============================================================

UClass* FNwiroIKBlueprintTools::FindClassByName(const FString& Name)
{
	if (Name.IsEmpty()) return nullptr;

	// Asset path of a user BP — load it and return its generated class.
	// Accepts "/Game/Foo/BP_X", "/Game/Foo/BP_X.BP_X", or "/Game/Foo/BP_X_C".
	if (Name.StartsWith(TEXT("/")) || Name.Contains(TEXT("/")))
	{
		FString TryPath = Name;
		// Strip trailing "_C" — that's the generated class suffix
		if (TryPath.EndsWith(TEXT("_C"))) TryPath = TryPath.LeftChop(2);
		// Add ".AssetName" suffix if not present
		if (!TryPath.Contains(TEXT(".")))
		{
			int32 Slash; TryPath.FindLastChar('/', Slash);
			TryPath += TEXT(".") + TryPath.RightChop(Slash + 1);
		}
		if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *TryPath))
		{
			if (BP->GeneratedClass) return BP->GeneratedClass;
		}
		// Also try as a direct class load (Engine native class with path)
		if (UClass* DirectC = LoadObject<UClass>(nullptr, *TryPath))
		{
			return DirectC;
		}
	}

	UClass* C = FindFirstObject<UClass>(*Name);
	if (C) return C;

	C = FindFirstObject<UClass>(*(TEXT("U") + Name));
	if (C) return C;
	C = FindFirstObject<UClass>(*(TEXT("A") + Name));
	if (C) return C;

	// Common aliases
	static TMap<FString, FString> Aliases = {
		{ TEXT("character"), TEXT("ACharacter") },
		{ TEXT("pawn"), TEXT("APawn") },
		{ TEXT("actor"), TEXT("AActor") },
		{ TEXT("playercontroller"), TEXT("APlayerController") },
		{ TEXT("gamemode"), TEXT("AGameModeBase") },
		{ TEXT("gamestate"), TEXT("AGameStateBase") },
		{ TEXT("playerstate"), TEXT("APlayerState") },
		{ TEXT("hud"), TEXT("AHUD") },
		{ TEXT("charactermovementcomponent"), TEXT("UCharacterMovementComponent") },
		{ TEXT("charactermovement"), TEXT("UCharacterMovementComponent") },
		{ TEXT("movementcomponent"), TEXT("UMovementComponent") },
		{ TEXT("scenecomponent"), TEXT("USceneComponent") },
		{ TEXT("actorcomponent"), TEXT("UActorComponent") },
		{ TEXT("staticmeshcomponent"), TEXT("UStaticMeshComponent") },
		{ TEXT("skeletalmeshcomponent"), TEXT("USkeletalMeshComponent") },
		{ TEXT("capsulecomponent"), TEXT("UCapsuleComponent") },
		{ TEXT("springarmcomponent"), TEXT("USpringArmComponent") },
		{ TEXT("cameracomponent"), TEXT("UCameraComponent") },
		{ TEXT("widgetcomponent"), TEXT("UWidgetComponent") },
		{ TEXT("userwidget"), TEXT("UUserWidget") },
		{ TEXT("widget"), TEXT("UWidget") },
		{ TEXT("progressbar"), TEXT("UProgressBar") },
		{ TEXT("textblock"), TEXT("UTextBlock") },
		{ TEXT("audiocomponent"), TEXT("UAudioComponent") },
		{ TEXT("pointlightcomponent"), TEXT("UPointLightComponent") },
		{ TEXT("spotlightcomponent"), TEXT("USpotLightComponent") },
		{ TEXT("particlesystemcomponent"), TEXT("UParticleSystemComponent") },
		{ TEXT("niagaracomponent"), TEXT("UNiagaraComponent") },
		{ TEXT("boxcollision"), TEXT("UBoxComponent") },
		{ TEXT("spherecollision"), TEXT("USphereComponent") },
		{ TEXT("arrowcomponent"), TEXT("UArrowComponent") },
		{ TEXT("enhancedinputlocalplayersubsystem"), TEXT("UEnhancedInputLocalPlayerSubsystem") },
		{ TEXT("enhancedinputsubsystem"), TEXT("UEnhancedInputLocalPlayerSubsystem") },
		{ TEXT("inputsubsystem"), TEXT("UEnhancedInputLocalPlayerSubsystem") },
	};

	if (const FString* Alias = Aliases.Find(Name.ToLower()))
	{
		C = FindFirstObject<UClass>(**Alias);
		if (C) return C;
	}

	// Try with Component suffix
	C = FindFirstObject<UClass>(*(Name + TEXT("Component")));
	if (C) return C;
	C = FindFirstObject<UClass>(*(TEXT("U") + Name + TEXT("Component")));
	if (C) return C;

	// Force-load engine class if not yet in memory
	C = StaticLoadClass(UObject::StaticClass(), nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *Name));
	if (C) return C;
	C = StaticLoadClass(UObject::StaticClass(), nullptr, *Name);
	if (C) return C;

	return nullptr;
}

// ============================================================
// RENAME VARIABLES
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoRenameVariables(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	int32 Renamed = 0;
	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		FString OldName = Obj->GetStringField(TEXT("old"));
		FString NewName = Obj->GetStringField(TEXT("new"));
		if (OldName.IsEmpty() || NewName.IsEmpty()) continue;

		FBlueprintEditorUtils::RenameMemberVariable(BP, FName(*OldName), FName(*NewName));
		Renamed++;
	}
	return FNwiroIKBPResult::Ok(FString::Printf(TEXT("Renamed %d variable(s)"), Renamed));
}

// ============================================================
// SET COMPONENT PROPERTIES
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoSetComponentProperties(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	if (!BP->SimpleConstructionScript) return FNwiroIKBPResult::Fail(TEXT("No SCS"));

	int32 Set = 0;
	TArray<FString> Errors;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		FString CompName = Obj->GetStringField(TEXT("component"));
		FString PropName = Obj->GetStringField(TEXT("property"));
		FString Value = Obj->GetStringField(TEXT("value"));

		if (CompName.IsEmpty() || PropName.IsEmpty()) continue;

		// Find the component template
		UActorComponent* CompTemplate = nullptr;
		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->GetVariableName().ToString().Equals(CompName, ESearchCase::IgnoreCase))
			{
				CompTemplate = Node->ComponentTemplate;
				break;
			}
		}

		if (!CompTemplate)
		{
			Errors.Add(FString::Printf(TEXT("Component not found: %s"), *CompName));
			continue;
		}

		if (ApplyComponentProperty(CompTemplate, PropName, Value, CompName, &Errors, nullptr))
			Set++;
	}

	FString Msg = FString::Printf(TEXT("Set %d component property(ies)"), Set);
	if (Errors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));
	return Set > 0 || Errors.Num() == 0 ? FNwiroIKBPResult::Ok(Msg) : FNwiroIKBPResult::Fail(Msg);
}

// ============================================================
// REMOVE FUNCTIONS
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoRemoveFunctions(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	int32 Removed = 0;
	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		FString FuncName;
		if (Item->Type == EJson::String) FuncName = Item->AsString();
		else if (Item->Type == EJson::Object && Item->AsObject().IsValid()) FuncName = Item->AsObject()->GetStringField(TEXT("name"));

		if (!FuncName.IsEmpty())
		{
			FBlueprintEditorUtils::RemoveGraph(BP, FindGraph(BP, FuncName));
			Removed++;
		}
	}
	return FNwiroIKBPResult::Ok(FString::Printf(TEXT("Removed %d function(s)"), Removed));
}

// ============================================================
// ADD EVENT DISPATCHERS
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoAddEventDispatchers(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	int32 Added = 0;
	TArray<FString> Errors;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		FString DispName = Obj->GetStringField(TEXT("name"));
		if (DispName.IsEmpty()) continue;

		FName DispFName(*DispName);

		// Check if already exists
		bool bExists = false;
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Var.VarName == DispFName)
			{
				bExists = true;
				break;
			}
		}
		if (bExists) { Errors.Add(FString::Printf(TEXT("'%s' already exists"), *DispName)); continue; }

		// Create the event dispatcher
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
		FBlueprintEditorUtils::AddMemberVariable(BP, DispFName, PinType);

		// Add parameters if specified
		const TArray<TSharedPtr<FJsonValue>>* Params;
		if (Obj->TryGetArrayField(TEXT("params"), Params))
		{
			// Params would need to be added to the delegate signature
			// This is complex - for now just create the dispatcher
		}

		Added++;
		UE_LOG(LogNwiroBP, Log, TEXT("Added event dispatcher: %s"), *DispName);
	}

	FString Msg = FString::Printf(TEXT("Added %d event dispatcher(s)"), Added);
	if (Errors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));
	return Added > 0 || Errors.Num() == 0 ? FNwiroIKBPResult::Ok(Msg) : FNwiroIKBPResult::Fail(Msg);
}

// ============================================================
// ADD/REMOVE INTERFACES
// ============================================================

// Resolve an interface reference the LLM passed us. Handles every variant
// we've seen come back from prompts:
//   1) Native C++ UInterface class name (e.g. "DamageInterface") — old path
//   2) Same with I/U prefix dropped or added ("UDamageInterface", "IDamageInterface")
//   3) Blueprint Interface ASSET object path
//      ("/Game/.../BPI_Foo", "/Game/.../BPI_Foo.BPI_Foo", "/Game/.../BPI_Foo.BPI_Foo_C")
// Returns the UClass that ImplementNewInterface expects, or nullptr.
static UClass* ResolveInterfaceClass(const FString& In)
{
	if (In.IsEmpty()) return nullptr;

	// 1) Try direct UClass lookup by bare name (or with U/I/A prefix variants).
	if (UClass* C = FNwiroIKBlueprintTools::FindClassByName(In))
	{
		if (C->IsChildOf(UInterface::StaticClass())) return C;
	}
	if (UClass* C = FindFirstObject<UClass>(*(TEXT("U") + In)))
	{
		if (C->IsChildOf(UInterface::StaticClass())) return C;
	}
	if (UClass* C = FindFirstObject<UClass>(*(TEXT("I") + In)))
	{
		if (C->IsChildOf(UInterface::StaticClass())) return C;
	}

	// 2) Path-shaped input — strip the optional `.ClassName` and `_C` suffix
	// the LLM sometimes appends, then load as a Blueprint asset.
	if (In.StartsWith(TEXT("/")))
	{
		FString PackagePath = In;
		int32 Dot; if (PackagePath.FindLastChar('.', Dot)) PackagePath = PackagePath.Left(Dot);
		UBlueprint* BPI = LoadObject<UBlueprint>(nullptr, *PackagePath);
		if (BPI && BPI->BlueprintType == BPTYPE_Interface && BPI->GeneratedClass)
		{
			return BPI->GeneratedClass;
		}
	}

	return nullptr;
}

FNwiroIKBPResult FNwiroIKBlueprintTools::DoAddInterfaces(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	int32 Added = 0;
	TArray<FString> Errors;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		FString IfaceName;
		if (Item->Type == EJson::String) IfaceName = Item->AsString();
		else if (Item->Type == EJson::Object && Item->AsObject().IsValid()) IfaceName = Item->AsObject()->GetStringField(TEXT("name"));
		if (IfaceName.IsEmpty()) continue;

		UClass* IfaceClass = ResolveInterfaceClass(IfaceName);
		if (!IfaceClass)
		{
			Errors.Add(FString::Printf(TEXT("Interface not found: %s (accepts C++ interface class name like 'DamageInterface', or Blueprint Interface asset path like '/Game/BPI/BPI_Foo')"), *IfaceName));
			continue;
		}

		FBlueprintEditorUtils::ImplementNewInterface(BP, FTopLevelAssetPath(IfaceClass->GetPathName()));
		Added++;
	}

	FString Msg = FString::Printf(TEXT("Added %d interface(s)"), Added);
	if (Errors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));
	return Added > 0 || Errors.Num() == 0 ? FNwiroIKBPResult::Ok(Msg) : FNwiroIKBPResult::Fail(Msg);
}

FNwiroIKBPResult FNwiroIKBlueprintTools::DoRemoveInterfaces(UBlueprint* BP, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	int32 Removed = 0;
	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		FString IfaceName;
		if (Item->Type == EJson::String) IfaceName = Item->AsString();
		else if (Item->Type == EJson::Object && Item->AsObject().IsValid()) IfaceName = Item->AsObject()->GetStringField(TEXT("name"));
		if (IfaceName.IsEmpty()) continue;

		UClass* IfaceClass = ResolveInterfaceClass(IfaceName);
		if (IfaceClass)
		{
			FBlueprintEditorUtils::RemoveInterface(BP, FTopLevelAssetPath(IfaceClass->GetPathName()));
			Removed++;
		}
	}
	return FNwiroIKBPResult::Ok(FString::Printf(TEXT("Removed %d interface(s)"), Removed));
}

// ============================================================
// REPARENT
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoReparent(UBlueprint* BP, const FString& NewParentClass)
{
	UClass* NewParent = FindClassByName(NewParentClass);
	if (!NewParent)
	{
		return FNwiroIKBPResult::Fail(FString::Printf(TEXT("Parent class not found: %s"), *NewParentClass));
	}

	BP->ParentClass = NewParent;
	FBlueprintEditorUtils::RefreshAllNodes(BP);
	UE_LOG(LogNwiroBP, Log, TEXT("Reparented to: %s"), *NewParent->GetName());
	return FNwiroIKBPResult::Ok(FString::Printf(TEXT("Reparented to %s [resolved] path:%s"), *NewParent->GetName(), *NewParent->GetPathName()));
}

// ============================================================
// REMOVE NODES
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoRemoveNodes(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	UEdGraph* Graph = FindGraph(BP, GraphName);
	if (!Graph) return FNwiroIKBPResult::Fail(TEXT("Graph not found"));

	int32 Removed = 0;
	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		FString Ref;
		if (Item->Type == EJson::String) Ref = Item->AsString();
		else if (Item->Type == EJson::Object) Ref = Item->AsObject()->GetStringField(TEXT("ref"));
		if (Ref.IsEmpty()) continue;

		UEdGraphNode* Node = FindNodeByRef(Graph, Ref);
		if (Node)
		{
			FBlueprintEditorUtils::RemoveNode(BP, Node);
			Removed++;
		}
	}
	return FNwiroIKBPResult::Ok(FString::Printf(TEXT("Removed %d node(s)"), Removed));
}

// ============================================================
// BREAK CONNECTIONS
// ============================================================

FNwiroIKBPResult FNwiroIKBlueprintTools::DoBreakConnections(UBlueprint* BP, const FString& GraphName, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	UEdGraph* Graph = FindGraph(BP, GraphName);
	if (!Graph) return FNwiroIKBPResult::Fail(TEXT("Graph not found"));

	int32 Broken = 0;
	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		FString NodeRef = Obj->HasField(TEXT("ref")) ? Obj->GetStringField(TEXT("ref")) : Obj->GetStringField(TEXT("node"));
		FString PinName = Obj->GetStringField(TEXT("pin"));

		UEdGraphNode* Node = FindNodeByRef(Graph, NodeRef);
		if (!Node) continue;

        if (PinName.IsEmpty())
		{
			// Break all connections on this node
			Node->BreakAllNodeLinks();
			Broken++;
		}
		else
		{
			UEdGraphPin* Pin = FindPin(Node, PinName);
			if (Pin)
			{
				Pin->BreakAllPinLinks();
				Broken++;
			}
		}
	}
	return FNwiroIKBPResult::Ok(FString::Printf(TEXT("Broke %d connection(s)"), Broken));
}

// ============================================================
// SERIALIZATION HELPERS
// ============================================================

TSharedPtr<FJsonObject> FNwiroIKBlueprintTools::SerializeVariable(UBlueprint* BP, const FName& VarName)
{
	int32 Idx = FBlueprintEditorUtils::FindNewVariableIndex(BP, VarName);
	if (Idx == INDEX_NONE) return nullptr;

	const FBPVariableDescription& Var = BP->NewVariables[Idx];

	TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
	Obj->SetStringField(TEXT("name"), VarName.ToString());
	Obj->SetStringField(TEXT("type"), PinTypeToString(Var.VarType));
	Obj->SetStringField(TEXT("defaultValue"), Var.DefaultValue);
	Obj->SetStringField(TEXT("category"), Var.Category.ToString());

	// Flags
	Obj->SetBoolField(TEXT("replicated"), (Var.PropertyFlags & CPF_Net) != 0);
	Obj->SetBoolField(TEXT("saveGame"), (Var.PropertyFlags & CPF_SaveGame) != 0);

	return Obj;
}

TSharedPtr<FJsonObject> FNwiroIKBlueprintTools::SerializeComponent(const UActorComponent* Comp, const FName& VarName)
{
	if (!Comp) return nullptr;

	TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
	Obj->SetStringField(TEXT("name"), VarName.ToString());
	Obj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());

	if (const USceneComponent* Scene = Cast<USceneComponent>(Comp))
	{
		TSharedPtr<FJsonObject> Loc = MakeShareable(new FJsonObject());
		Loc->SetNumberField(TEXT("x"), Scene->GetRelativeLocation().X);
		Loc->SetNumberField(TEXT("y"), Scene->GetRelativeLocation().Y);
		Loc->SetNumberField(TEXT("z"), Scene->GetRelativeLocation().Z);
		Obj->SetObjectField(TEXT("location"), Loc);

		FVector Scale = Scene->GetRelativeScale3D();
		if (Scale != FVector::OneVector)
		{
			Obj->SetStringField(TEXT("scale"), FString::Printf(TEXT("(%.2f,%.2f,%.2f)"), Scale.X, Scale.Y, Scale.Z));
		}

		Obj->SetStringField(TEXT("mobility"), Scene->Mobility == EComponentMobility::Static ? TEXT("Static") :
			Scene->Mobility == EComponentMobility::Stationary ? TEXT("Stationary") : TEXT("Movable"));
	}

	// StaticMeshComponent details
	if (const UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Comp))
	{
		if (SMC->GetStaticMesh())
			Obj->SetStringField(TEXT("staticMesh"), SMC->GetStaticMesh()->GetPathName());
		else
			Obj->SetStringField(TEXT("staticMesh"), TEXT("None"));
	}

	// Physics info
	if (const UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Comp))
	{
		Obj->SetBoolField(TEXT("simulatePhysics"), Prim->BodyInstance.bSimulatePhysics);
		Obj->SetBoolField(TEXT("generateOverlapEvents"), Prim->GetGenerateOverlapEvents());
	}

	return Obj;
}

// Wiring-completeness scan: report exec-pin gaps so the agent can self-verify a graph is
// fully wired. BLOCKING = a node whose INPUT exec pin is unlinked (unreachable; this is how an
// unwired Branch arm surfaces, on the downstream node). ADVISORY = an unlinked OUTPUT exec pin
// (execution dead-ends; usually a missing downstream node, sometimes a legit terminal). Data
// pins are deliberately NOT classified in v1 (too many optional/auto-resolved inputs would be
// false positives). Council-reviewed: narrow + high-precision so the signal stays trustworthy.
static void NwiroCollectDangling(UEdGraph* Graph, TArray<TSharedPtr<FJsonValue>>& OutBlocking, TArray<TSharedPtr<FJsonValue>>& OutAdvisory)
{
	if (!Graph) return;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		const FString Title = Node->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden || Pin->bAdvancedView) continue;
			if (Pin->LinkedTo.Num() > 0) continue;
			if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue; // v1: exec pins only
			TSharedPtr<FJsonObject> E = MakeShareable(new FJsonObject());
			E->SetStringField(TEXT("node"), Title);
			E->SetStringField(TEXT("pin"), Pin->PinName.ToString());
			if (Pin->Direction == EGPD_Input)
			{
				E->SetStringField(TEXT("type"), TEXT("exec-in"));
				E->SetStringField(TEXT("hint"), TEXT("unreachable: nothing executes into this pin; wire an exec output into it"));
				OutBlocking.Add(MakeShareable(new FJsonValueObject(E.ToSharedRef())));
			}
			else
			{
				E->SetStringField(TEXT("type"), TEXT("exec-out"));
				E->SetStringField(TEXT("hint"), TEXT("execution dead-ends here; wire the next node or confirm this is intentionally terminal"));
				OutAdvisory.Add(MakeShareable(new FJsonValueObject(E.ToSharedRef())));
			}
		}
	}
}

TSharedPtr<FJsonObject> FNwiroIKBlueprintTools::SerializeGraph(UEdGraph* Graph, bool bIncludeNodes)
{
	if (!Graph) return nullptr;

	TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
	Obj->SetStringField(TEXT("name"), Graph->GetName());
	Obj->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());

	if (bIncludeNodes)
	{
		TArray<TSharedPtr<FJsonValue>> NodeArr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			TSharedPtr<FJsonObject> NObj = SerializeNode(Node);
			if (NObj.IsValid())
			{
				NodeArr.Add(MakeShareable(new FJsonValueObject(NObj.ToSharedRef())));
			}
		}
		Obj->SetArrayField(TEXT("nodes"), NodeArr);

		// Wiring-completeness feedback so the agent reads its own gaps back and self-corrects.
		TArray<TSharedPtr<FJsonValue>> Blocking, Advisory;
		NwiroCollectDangling(Graph, Blocking, Advisory);
		Obj->SetArrayField(TEXT("blocking"), Blocking);
		Obj->SetArrayField(TEXT("advisory"), Advisory);
		Obj->SetNumberField(TEXT("blockingCount"), Blocking.Num());
		Obj->SetNumberField(TEXT("advisoryCount"), Advisory.Num());
	}

	return Obj;
}

TSharedPtr<FJsonObject> FNwiroIKBlueprintTools::SerializeNode(UEdGraphNode* Node)
{
	if (!Node) return nullptr;

	TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
	Obj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
	Obj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
	Obj->SetStringField(TEXT("guid"), Node->NodeGuid.ToString());
	// `ref` is what connect_pins expects in the "from"/"to" strings before the
	// dot. Look it up in NodeRefs (the auto-stored secondary key, possibly
	// suffixed for uniqueness) so each node in the JSON output gets a unique
	// handle even when multiple nodes share the same title.
	{
		FString RefValue;
		for (const auto& Pair : NodeRefs)
		{
			if (Pair.Value.NodeGuid == Node->NodeGuid)
			{
				// Prefer keys that look like the title (skip generic "node_N").
				if (RefValue.IsEmpty() || (RefValue.StartsWith(TEXT("node_")) && !Pair.Key.StartsWith(TEXT("node_"))))
				{
					RefValue = Pair.Key;
				}
			}
		}
		if (RefValue.IsEmpty())
		{
			RefValue = Node->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
		}
		Obj->SetStringField(TEXT("ref"), RefValue);
	}
	Obj->SetNumberField(TEXT("x"), Node->NodePosX);
	Obj->SetNumberField(TEXT("y"), Node->NodePosY);

	// Serialize pins
	TArray<TSharedPtr<FJsonValue>> PinArr;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;

		TSharedPtr<FJsonObject> PObj = MakeShareable(new FJsonObject());
		PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PObj->SetStringField(TEXT("type"), PinTypeToString(Pin->PinType));
		PObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
		PObj->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
		PObj->SetBoolField(TEXT("connected"), Pin->LinkedTo.Num() > 0);

		// Show what this pin is connected to
		if (Pin->LinkedTo.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> LinkedArr;
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked && Linked->GetOwningNode())
				{
					FString ConnStr = FString::Printf(TEXT("%s.%s"),
						*Linked->GetOwningNode()->NodeGuid.ToString(),
						*Linked->PinName.ToString());
					LinkedArr.Add(MakeShareable(new FJsonValueString(ConnStr)));
				}
			}
			PObj->SetArrayField(TEXT("connectedTo"), LinkedArr);
		}

		// Show DefaultObject for object reference pins
		if (Pin->DefaultObject)
		{
			PObj->SetStringField(TEXT("defaultObject"), Pin->DefaultObject->GetPathName());
		}

		if (!Pin->PinFriendlyName.IsEmpty())
		{
			PObj->SetStringField(TEXT("friendlyName"), Pin->PinFriendlyName.ToString());
		}

		PinArr.Add(MakeShareable(new FJsonValueObject(PObj.ToSharedRef())));
	}
	Obj->SetArrayField(TEXT("pins"), PinArr);

	return Obj;
}

FString FNwiroIKBlueprintTools::PinTypeToString(const FEdGraphPinType& PinType)
{
	FString Result;

	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean) Result = TEXT("Boolean");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Byte) Result = TEXT("Byte");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int) Result = TEXT("Int");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int64) Result = TEXT("Int64");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real) Result = PinType.PinSubCategory.ToString();
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_String) Result = TEXT("String");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Name) Result = TEXT("Name");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Text) Result = TEXT("Text");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) Result = TEXT("Exec");
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
	{
		Result = TEXT("Object");
		if (PinType.PinSubCategoryObject.IsValid())
		{
			Result += TEXT(":") + PinType.PinSubCategoryObject->GetName();
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
	{
		Result = TEXT("Struct");
		if (PinType.PinSubCategoryObject.IsValid())
		{
			Result = PinType.PinSubCategoryObject->GetName();
		}
	}
	else
	{
		Result = PinType.PinCategory.ToString();
	}

	// Container
	if (PinType.ContainerType == EPinContainerType::Array)
	{
		Result = TEXT("Array<") + Result + TEXT(">");
	}
	else if (PinType.ContainerType == EPinContainerType::Set)
	{
		Result = TEXT("Set<") + Result + TEXT(">");
	}
	else if (PinType.ContainerType == EPinContainerType::Map)
	{
		Result = TEXT("Map<") + Result + TEXT(">");
	}

	return Result;
}

// ============================================================
// DELETE BLUEPRINT
// ============================================================

// ============================================================
// CLEAR GRAPH — remove all nodes from EventGraph
// ============================================================

FString FNwiroIKBlueprintTools::ClearGraph(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	// Hard guard: clear_graph is destructive. The LLM kept calling it as a
	// "reset" reflex whenever connect_pins failed, wiping out work in progress.
	// Refuse the call unless it carries an explicit confirm flag — the system
	// prompt instructs the model to set this only when the USER literally
	// asked to start the graph over.
	FString Confirm;
	Cmd->TryGetStringField(TEXT("confirm"), Confirm);
	if (!Confirm.Equals(TEXT("user_requested"), ESearchCase::IgnoreCase))
	{
		return TEXT("{\"success\":false,\"error\":\"clear_graph refused: this is destructive. Read the connect_pins / edit_blueprint error message — it lists the available node refs and pin names so you can self-correct without wiping the graph. Only call clear_graph again with arguments.confirm=\\\"user_requested\\\" if the user literally said to start over.\"}");
	}

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	FString GraphName = Cmd->HasField(TEXT("graph")) ? Cmd->GetStringField(TEXT("graph")) : TEXT("EventGraph");

	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	UEdGraph* Graph = FindGraph(BP, GraphName);
	if (!Graph) return TEXT("{\"success\":false,\"error\":\"Graph not found\"}");

	int32 Removed = 0;
	TArray<UEdGraphNode*> NodesToRemove;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		if (!Node->CanUserDeleteNode()) continue;
		if (Node->IsA<UK2Node_FunctionEntry>()) continue;
		if (Node->IsA<UK2Node_FunctionResult>()) continue;
		if (Node->IsA<UK2Node_Event>()) continue;
		if (Node->IsA<UK2Node_Tunnel>()) continue;
		NodesToRemove.Add(Node);
	}

	for (UEdGraphNode* Node : NodesToRemove)
	{
		FBlueprintEditorUtils::RemoveNode(BP, Node);
		Removed++;
	}

	// Clear node refs
	NodeRefs.Empty();

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);

	return FString::Printf(TEXT("{\"success\":true,\"removed\":%d,\"message\":\"Graph cleared and compiled\"}"), Removed);
}

FString FNwiroIKBlueprintTools::DeleteBlueprint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString AssetPath = Cmd->GetStringField(TEXT("assetPath"));
	if (AssetPath.IsEmpty()) AssetPath = Cmd->GetStringField(TEXT("path"));
	if (AssetPath.IsEmpty()) AssetPath = Cmd->GetStringField(TEXT("name"));

	// Try Blueprint first (preserves original behavior).
	if (UBlueprint* BP = LoadBP(AssetPath))
	{
		const FString FullPath = BP->GetPathName();
		const FString PackagePath = BP->GetPackage()->GetName();

		// Defensive: a half-constructed BP (GeneratedClass null, or with no
		// ClassConstructor) makes UEditorAssetLibrary::DeleteAsset crash inside
		// a NewObject assertion (CoreUObject UObjectGlobals.cpp:3396). Detect
		// that state and rename the BP into the transient package + GC instead.
		// The on-disk uasset (if any) stays — caller can retry after UE restart.
		const UClass* GC = BP->GeneratedClass;
		const bool bClassOK = GC && GC->ClassWithin && GC->ClassConstructor;
		if (!bClassOK)
		{
			// Don't even try to rename / MarkAsGarbage — those routes also
			// hit assertions when the BP is partially constructed (we saw a
			// CoreUObject Obj.cpp:300 crash in Rename). Just report success
			// without touching the BP; UE's normal GC will collect it on
			// shutdown or when the package gets reloaded fresh.
			return FString::Printf(TEXT("{\"success\":true,\"deleted\":\"%s\",\"kind\":\"Blueprint(no-op)\",\"note\":\"GeneratedClass half-constructed; left in place to avoid editor crash on cleanup\"}"), *FullPath);
		}

		if (UEditorAssetLibrary::DeleteAsset(PackagePath))
			return FString::Printf(TEXT("{\"success\":true,\"deleted\":\"%s\",\"kind\":\"Blueprint\"}"), *FullPath);
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to delete blueprint: %s\"}"), *FullPath);
	}

	// Fall back to generic UObject delete — covers Niagara systems, blackboards,
	// state trees, data tables, etc. that the LLM might naturally pass here.
	if (UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath))
	{
		const FString FullPath = Asset->GetPathName();
		const FString PackagePath = Asset->GetPackage()->GetName();
		const FString Kind = Asset->GetClass()->GetName();
		if (UEditorAssetLibrary::DeleteAsset(PackagePath))
			return FString::Printf(TEXT("{\"success\":true,\"deleted\":\"%s\",\"kind\":\"%s\"}"), *FullPath, *Kind);
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to delete %s: %s\"}"), *Kind, *FullPath);
	}

	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Asset not found: %s\"}"), *AssetPath);
}

// ============================================================
// DUPLICATE BLUEPRINT
// ============================================================

FString FNwiroIKBlueprintTools::DuplicateBlueprint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SourcePath = Cmd->GetStringField(TEXT("source"));
	if (SourcePath.IsEmpty()) SourcePath = Cmd->GetStringField(TEXT("assetPath"));
	FString NewName = Cmd->GetStringField(TEXT("newName"));
	FString DestPath = Cmd->GetStringField(TEXT("destinationPath"));

	UBlueprint* SourceBP = LoadBP(SourcePath);
	if (!SourceBP)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Source blueprint not found: %s\"}"), *SourcePath);

	if (DestPath.IsEmpty()) DestPath = FPaths::GetPath(SourceBP->GetPackage()->GetName());
	if (NewName.IsEmpty()) NewName = SourceBP->GetName() + TEXT("_Copy");

	FString FullDestPath = DestPath / NewName;
	FString SourcePkg = SourceBP->GetPackage()->GetName();

	if (UEditorAssetLibrary::DuplicateAsset(SourcePkg, FullDestPath))
		return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\"}"), *NewName, *FullDestPath);

	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to duplicate to: %s\"}"), *FullDestPath);
}

// ============================================================
// RENAME BLUEPRINT
// ============================================================

FString FNwiroIKBlueprintTools::RenameBlueprint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString AssetPath = Cmd->GetStringField(TEXT("assetPath"));
	if (AssetPath.IsEmpty()) AssetPath = Cmd->GetStringField(TEXT("source"));
	FString NewName = Cmd->GetStringField(TEXT("newName"));

	UBlueprint* BP = LoadBP(AssetPath);
	if (!BP)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *AssetPath);

	if (NewName.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"newName is required\"}");

	FString SourcePkg = BP->GetPackage()->GetName();
	FString DestPkg = FPaths::GetPath(SourcePkg) / NewName;

	if (UEditorAssetLibrary::RenameAsset(SourcePkg, DestPkg))
		return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\"}"), *NewName, *DestPkg);

	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to rename to: %s\"}"), *NewName);
}

// ============================================================
// DELETE NODE (standalone)
// ============================================================

FString FNwiroIKBlueprintTools::DeleteNode(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	FString GraphName = Cmd->GetStringField(TEXT("graph"));
	if (GraphName.IsEmpty()) GraphName = TEXT("EventGraph");

	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	// Build array from "refs" array or single "ref"
	TArray<TSharedPtr<FJsonValue>> Items;
	const TArray<TSharedPtr<FJsonValue>>* RefsArr;
	if (Cmd->TryGetArrayField(TEXT("refs"), RefsArr))
	{
		for (const auto& V : *RefsArr)
		{
			TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
			Obj->SetStringField(TEXT("ref"), V->AsString());
			Items.Add(MakeShareable(new FJsonValueObject(Obj)));
		}
	}
	else if (Cmd->HasField(TEXT("ref")))
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("ref"), Cmd->GetStringField(TEXT("ref")));
		Items.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "DeleteNode", "AI: Delete Node"), BP);
	FNwiroIKBPResult R = DoRemoveNodes(BP, GraphName, Items);

	if (R.bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	return FString::Printf(TEXT("{\"success\":%s,\"message\":\"%s\"}"), R.bSuccess ? TEXT("true") : TEXT("false"), *R.Message);
}

// ============================================================
// CREATE FUNCTION GRAPH (standalone)
// ============================================================

FString FNwiroIKBlueprintTools::CreateFunctionGraph(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	// Build items array for DoAddFunctions
	TArray<TSharedPtr<FJsonValue>> Items;
	const TArray<TSharedPtr<FJsonValue>>* FuncsArr;
	if (Cmd->TryGetArrayField(TEXT("functions"), FuncsArr))
	{
		Items = *FuncsArr;
	}
	else
	{
		// Single function from top-level fields
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Cmd->GetStringField(TEXT("name")));
		if (Cmd->HasField(TEXT("inputs"))) Obj->SetField(TEXT("inputs"), Cmd->TryGetField(TEXT("inputs")));
		if (Cmd->HasField(TEXT("outputs"))) Obj->SetField(TEXT("outputs"), Cmd->TryGetField(TEXT("outputs")));
		if (Cmd->HasField(TEXT("pure"))) Obj->SetBoolField(TEXT("pure"), Cmd->GetBoolField(TEXT("pure")));
		if (Cmd->HasField(TEXT("override"))) Obj->SetBoolField(TEXT("override"), Cmd->GetBoolField(TEXT("override")));
		Items.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateFunctionGraph", "AI: Create Function Graph"), BP);
	// Prevent GC during graph creation + compile. Without this, GC can run
	// mid-compile and free an object the K2 compiler still references, causing
	// an access violation (scenario 03: create_function_graph on a widget BP
	// crashed at CompileBlueprint). Mirrors create_blueprint's FGCScopeGuard.
	FGCScopeGuard GCGuard;
	FNwiroIKBPResult R = DoAddFunctions(BP, Items);

	if (R.bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	return FString::Printf(TEXT("{\"success\":%s,\"message\":\"%s\"}"), R.bSuccess ? TEXT("true") : TEXT("false"), *R.Message);
}

// ============================================================
// ADD INTERFACE (standalone)
// ============================================================

FString FNwiroIKBlueprintTools::AddInterface(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	TArray<TSharedPtr<FJsonValue>> Items;
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Cmd->TryGetArrayField(TEXT("interfaces"), Arr))
	{
		Items = *Arr;
	}
	else if (Cmd->HasField(TEXT("interface")))
	{
		Items.Add(MakeShareable(new FJsonValueString(Cmd->GetStringField(TEXT("interface")))));
	}

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "AddInterface", "AI: Add Interface"), BP);
	FNwiroIKBPResult R = DoAddInterfaces(BP, Items);

	if (R.bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	return FString::Printf(TEXT("{\"success\":%s,\"message\":\"%s\"}"), R.bSuccess ? TEXT("true") : TEXT("false"), *R.Message);
}

// ============================================================
// REMOVE INTERFACE (standalone)
// ============================================================

FString FNwiroIKBlueprintTools::RemoveInterface(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	TArray<TSharedPtr<FJsonValue>> Items;
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Cmd->TryGetArrayField(TEXT("interfaces"), Arr))
	{
		Items = *Arr;
	}
	else if (Cmd->HasField(TEXT("interface")))
	{
		Items.Add(MakeShareable(new FJsonValueString(Cmd->GetStringField(TEXT("interface")))));
	}

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "RemoveInterface", "AI: Remove Interface"), BP);
	FNwiroIKBPResult R = DoRemoveInterfaces(BP, Items);

	if (R.bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	return FString::Printf(TEXT("{\"success\":%s,\"message\":\"%s\"}"), R.bSuccess ? TEXT("true") : TEXT("false"), *R.Message);
}

// ============================================================
// CREATE EVENT DISPATCHER (standalone)
// ============================================================

FString FNwiroIKBlueprintTools::CreateEventDispatcher(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	TArray<TSharedPtr<FJsonValue>> Items;
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Cmd->TryGetArrayField(TEXT("dispatchers"), Arr))
	{
		Items = *Arr;
	}
	else
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Cmd->GetStringField(TEXT("name")));
		if (Cmd->HasField(TEXT("params"))) Obj->SetField(TEXT("params"), Cmd->TryGetField(TEXT("params")));
		Items.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateEventDispatcher", "AI: Create Event Dispatcher"), BP);
	FNwiroIKBPResult R = DoAddEventDispatchers(BP, Items);

	if (R.bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	return FString::Printf(TEXT("{\"success\":%s,\"message\":\"%s\"}"), R.bSuccess ? TEXT("true") : TEXT("false"), *R.Message);
}

// ============================================================
// REPARENT BLUEPRINT (standalone)
// ============================================================

FString FNwiroIKBlueprintTools::ReparentBlueprint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	FString NewParent = Cmd->GetStringField(TEXT("parentClass"));
	if (NewParent.IsEmpty()) NewParent = Cmd->GetStringField(TEXT("parent"));

	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "ReparentBlueprint", "AI: Reparent Blueprint"), BP);
	FNwiroIKBPResult R = DoReparent(BP, NewParent);

	if (R.bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	return FString::Printf(TEXT("{\"success\":%s,\"message\":\"%s\"}"), R.bSuccess ? TEXT("true") : TEXT("false"), *R.Message);
}

// ============================================================
// REMOVE COMPONENT (standalone)
// ============================================================

FString FNwiroIKBlueprintTools::RemoveComponent(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	TArray<TSharedPtr<FJsonValue>> Items;
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Cmd->TryGetArrayField(TEXT("components"), Arr))
	{
		Items = *Arr;
	}
	else if (Cmd->HasField(TEXT("name")))
	{
		Items.Add(MakeShareable(new FJsonValueString(Cmd->GetStringField(TEXT("name")))));
	}

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "RemoveComponent", "AI: Remove Component"), BP);
	FNwiroIKBPResult R = DoRemoveComponents(BP, Items);

	if (R.bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	return FString::Printf(TEXT("{\"success\":%s,\"message\":\"%s\"}"), R.bSuccess ? TEXT("true") : TEXT("false"), *R.Message);
}

// ============================================================
// EDIT COMPONENT (standalone)
// ============================================================

FString FNwiroIKBlueprintTools::EditComponent(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BPName = Cmd->GetStringField(TEXT("blueprint"));
	UBlueprint* BP = LoadBP(BPName);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

	// Convert {name, properties:{K:V}} entries into flat {component, property, value} triplets
	// that DoSetComponentProperties expects.
	auto ExpandToTriplets = [](const FString& CompName, const TSharedPtr<FJsonObject>& PropsObj, TArray<TSharedPtr<FJsonValue>>& Out)
	{
		for (const auto& Pair : PropsObj->Values)
		{
			FString ValStr;
			if (Pair.Value->Type == EJson::String)
				ValStr = Pair.Value->AsString();
			else if (Pair.Value->Type == EJson::Boolean)
				ValStr = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
			else if (Pair.Value->Type == EJson::Number)
				ValStr = FString::SanitizeFloat(Pair.Value->AsNumber());
			else
				continue;

			TSharedPtr<FJsonObject> Item = MakeShareable(new FJsonObject());
			Item->SetStringField(TEXT("component"), CompName);
			Item->SetStringField(TEXT("property"), FString(*Pair.Key));
			Item->SetStringField(TEXT("value"), ValStr);
			Out.Add(MakeShareable(new FJsonValueObject(Item)));
		}
	};

	TArray<TSharedPtr<FJsonValue>> Items;
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Cmd->TryGetArrayField(TEXT("components"), Arr))
	{
		for (const auto& CV : *Arr)
		{
			const TSharedPtr<FJsonObject>& CO = CV->AsObject();
			if (!CO.IsValid()) continue;
			FString CompName = CO->GetStringField(TEXT("name"));
			const TSharedPtr<FJsonObject>* PropsObj;
			if (CO->TryGetObjectField(TEXT("properties"), PropsObj))
				ExpandToTriplets(CompName, *PropsObj, Items);
		}
	}
	else
	{
		// Single component from top-level: {name, properties}
		FString CompName = Cmd->GetStringField(TEXT("name"));
		const TSharedPtr<FJsonObject>* PropsObj;
		if (Cmd->TryGetObjectField(TEXT("properties"), PropsObj))
			ExpandToTriplets(CompName, *PropsObj, Items);
	}

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "EditComponent", "AI: Edit Component"), BP);
	FNwiroIKBPResult R = DoSetComponentProperties(BP, Items);

	if (R.bSuccess)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	return FString::Printf(TEXT("{\"success\":%s,\"message\":\"%s\"}"), R.bSuccess ? TEXT("true") : TEXT("false"), *R.Message);
}

// ============================================================
// ORGANIZE GRAPH NODES (headless layered / Sugiyama layout)
// ============================================================
//
// Nodes added programmatically pile up near the same coordinates, so graphs
// read as overlapping mush. This lays a UEdGraph out left-to-right along exec
// flow: layer 0 = nodes with no incoming exec link, each exec child pushed one
// column right (longest-path), pure/data nodes one column LEFT of the consumer
// they feed, then nodes stacked top-to-bottom per column. Headless-safe.
static void OrganizeGraphNodesImpl(UEdGraph* Graph)
{
	if (!Graph) return;

	auto IsExec = [](UEdGraphPin* P){ return P && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec; };

	TArray<UEdGraphNode*> Nodes;
	for (UEdGraphNode* N : Graph->Nodes)
	{
		if (!N) continue;
		if (N->IsA(UEdGraphNode_Comment::StaticClass())) continue;
		Nodes.Add(N);
	}
	if (Nodes.Num() == 0) return;

	auto HasAnyExec = [&](UEdGraphNode* N)
	{
		for (UEdGraphPin* P : N->Pins) if (IsExec(P)) return true;
		return false;
	};

	TMap<UEdGraphNode*, int32> Layer;
	for (UEdGraphNode* N : Nodes) Layer.Add(N, 0);
	bool bChanged = true; int32 Guard = 0;
	while (bChanged && Guard++ < 2000)
	{
		bChanged = false;
		for (UEdGraphNode* N : Nodes)
		{
			const int32 Base = Layer[N];
			for (UEdGraphPin* OutPin : N->Pins)
			{
				if (!IsExec(OutPin) || OutPin->Direction != EGPD_Output) continue;
				for (UEdGraphPin* Linked : OutPin->LinkedTo)
				{
					UEdGraphNode* Child = Linked ? Linked->GetOwningNode() : nullptr;
					if (Child && Layer.Contains(Child) && Layer[Child] < Base + 1) { Layer[Child] = Base + 1; bChanged = true; }
				}
			}
		}
	}

	for (UEdGraphNode* N : Nodes)
	{
		if (HasAnyExec(N)) continue;
		int32 ConsumerLayer = -1;
		for (UEdGraphPin* P : N->Pins)
		{
			if (P->Direction != EGPD_Output) continue;
			for (UEdGraphPin* Linked : P->LinkedTo)
			{
				UEdGraphNode* C = Linked ? Linked->GetOwningNode() : nullptr;
				if (C && Layer.Contains(C)) ConsumerLayer = FMath::Max(ConsumerLayer, Layer[C]);
			}
		}
		Layer[N] = ConsumerLayer > 0 ? ConsumerLayer - 1 : 0;
	}

	int32 MaxLayer = 0;
	for (const auto& Kvp : Layer) MaxLayer = FMath::Max(MaxLayer, Kvp.Value);
	TArray<TArray<UEdGraphNode*>> Layers; Layers.SetNum(MaxLayer + 1);
	for (UEdGraphNode* N : Nodes) Layers[Layer[N]].Add(N);

	const float HSpacing = 360.f, VSpacing = 80.f;
	float CurrentX = 0.f;
	for (int32 L = 0; L < Layers.Num(); ++L)
	{
		float CurrentY = 0.f, MaxW = 0.f;
		for (UEdGraphNode* N : Layers[L])
		{
			if (!N) continue;
			N->Modify();
			N->NodePosX = (int32)CurrentX;
			N->NodePosY = (int32)CurrentY;
			const int32 Pins = FMath::Max(N->Pins.Num(), 1);
			const float EstH = FMath::Max(96.f, 40.f + Pins * 26.f);
			CurrentY += EstH + VSpacing;
			MaxW = FMath::Max(MaxW, 240.f);
		}
		CurrentX += MaxW + HSpacing;
	}
}

// MCP entry: { path | blueprint | assetPath, graph? }
FString FNwiroIKBlueprintTools::OrganizeBlueprintNodes(const FString& JsonCommand)
{
	// Game-thread marshal: node mutation + asset save here are game-thread-only, but tool
	// dispatch can arrive on the MCP HTTP worker thread. Mirrors CreateWidgetBlueprint's guard.
	if (!IsInGameThread())
	{
		TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
		TFuture<FString> Future = Promise->GetFuture();
		AsyncTask(ENamedThreads::GameThread, [JsonCommand, Promise]()
		{
			Promise->SetValue(FNwiroIKBlueprintTools::OrganizeBlueprintNodes(JsonCommand));
		});
		return Future.Get();
	}
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path;
	for (const TCHAR* K : { TEXT("path"), TEXT("blueprint"), TEXT("name"), TEXT("assetPath") })
		if (Cmd->TryGetStringField(K, Path) && !Path.IsEmpty()) break;
	if (Path.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"Missing 'path' (blueprint asset path or name)\"}");

	FString GraphName;
	Cmd->TryGetStringField(TEXT("graph"), GraphName);

	UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
	UBlueprint* BP = Cast<UBlueprint>(Asset);
	if (!BP) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *Path);

	TArray<UEdGraph*> All; BP->GetAllGraphs(All);
	int32 Organized = 0;
	for (UEdGraph* G : All)
	{
		if (!G) continue;
		if (!GraphName.IsEmpty() && !G->GetName().Equals(GraphName, ESearchCase::IgnoreCase)) continue;
		OrganizeGraphNodesImpl(G);
		Organized++;
	}
	if (Organized == 0)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Graph not found: %s\"}"), *GraphName);

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	UEditorAssetLibrary::SaveLoadedAsset(BP, false);
	return FString::Printf(TEXT("{\"success\":true,\"organized\":%d,\"blueprint\":\"%s\"}"), Organized, *BP->GetName());
}

// ============================================================
// RENDER BLUEPRINT GRAPH -> PNG
// ============================================================
//
// Render any UBlueprint's graph (EventGraph by default, or a named graph)
// to a PNG file. Builds an off-screen SGraphEditor over the live UEdGraph,
// zooms to fit, then runs FWidgetRenderer at the requested size. Works for
// any blueprint type that exposes graphs: actor BP, widget BP, anim BP,
// behavior tree (graph), etc. — the caller picks the asset and graph name.
FString FNwiroIKBlueprintTools::RenderBlueprintGraph(const FString& JsonCommand)
{
	// Game-thread marshal: render-target draw + canvas here are game/render-thread-only, but
	// tool dispatch can arrive on the MCP HTTP worker thread. Mirrors CreateWidgetBlueprint's guard.
	if (!IsInGameThread())
	{
		TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
		TFuture<FString> Future = Promise->GetFuture();
		AsyncTask(ENamedThreads::GameThread, [JsonCommand, Promise]()
		{
			Promise->SetValue(FNwiroIKBlueprintTools::RenderBlueprintGraph(JsonCommand));
		});
		return Future.Get();
	}
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path;
	for (const TCHAR* K : { TEXT("path"), TEXT("blueprint"), TEXT("name"), TEXT("assetPath") })
		if (Cmd->TryGetStringField(K, Path) && !Path.IsEmpty()) break;
	if (Path.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"Missing 'path' (blueprint asset path or name)\"}");

	FString GraphName;
	Cmd->TryGetStringField(TEXT("graph"), GraphName);
	if (GraphName.IsEmpty())
		Cmd->TryGetStringField(TEXT("graphName"), GraphName);

	int32 Width = Cmd->HasField(TEXT("width")) ? (int32)Cmd->GetNumberField(TEXT("width")) : 1600;
	int32 Height = Cmd->HasField(TEXT("height")) ? (int32)Cmd->GetNumberField(TEXT("height")) : 900;
	Width = FMath::Clamp(Width, 64, 4096);
	Height = FMath::Clamp(Height, 64, 4096);

	UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
	UBlueprint* BP = Cast<UBlueprint>(Asset);
	if (!BP)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *Path);

	// Pick graph: explicit name, then EventGraph default, then first UbergraphPage, then first function graph.
	UEdGraph* Graph = nullptr;
	TArray<UEdGraph*> Graphs;
	BP->GetAllGraphs(Graphs);
	if (!GraphName.IsEmpty())
	{
		for (UEdGraph* G : Graphs)
			if (G && G->GetName().Equals(GraphName, ESearchCase::IgnoreCase)) { Graph = G; break; }
	}
	if (!Graph)
	{
		for (UEdGraph* G : BP->UbergraphPages)
			if (G && G->GetName().Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase)) { Graph = G; break; }
	}
	if (!Graph && BP->UbergraphPages.Num() > 0) Graph = BP->UbergraphPages[0];
	if (!Graph && BP->FunctionGraphs.Num() > 0) Graph = BP->FunctionGraphs[0];

	if (!Graph)
	{
		FString Available;
		for (UEdGraph* G : Graphs) { if (G) { if (!Available.IsEmpty()) Available += TEXT(","); Available += G->GetName(); } }
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Graph not found. Requested:'%s' Available:[%s]\"}"),
			*GraphName, *Available);
	}

	// Auto-organize the graph first (default on) so the render isn't overlapping
	// mush. Pass "organize": false to render the raw positions as-is.
	bool bOrganize = true;
	Cmd->TryGetBoolField(TEXT("organize"), bOrganize);
	if (bOrganize)
	{
		OrganizeGraphNodesImpl(Graph);
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		UEditorAssetLibrary::SaveLoadedAsset(BP, false);
	}

	// HEADLESS-SAFE RENDER: draw the graph straight from node data (positions,
	// titles, pins, links) onto a render-target canvas. Screenshotting an
	// off-screen SGraphEditor renders BLANK without a live Slate tick loop
	// (you got a grid + "READ-ONLY" watermark, no nodes). Canvas drawing needs
	// no editor tab and cannot come back empty.
	struct FNodeBox { UEdGraphNode* Node=nullptr; float X=0,Y=0,W=0,H=0; TArray<UEdGraphPin*> In, Out; FLinearColor Header=FLinearColor(0.20f,0.22f,0.28f,1.f); FString Title; };
	TArray<FNodeBox> Boxes;
	TMap<UEdGraphNode*, int32> NodeToBox;
	const float HeaderH = 26.f, PinRowH = 16.f, BoxPad = 10.f, MinW = 150.f;
	float MinX = FLT_MAX, MinY = FLT_MAX, MaxX = -FLT_MAX, MaxY = -FLT_MAX;
	for (UEdGraphNode* N : Graph->Nodes)
	{
		if (!N) continue;
		FNodeBox B; B.Node = N; B.X = (float)N->NodePosX; B.Y = (float)N->NodePosY;
		for (UEdGraphPin* P : N->Pins) { if (!P || P->bHidden) continue; if (P->Direction == EGPD_Input) B.In.Add(P); else B.Out.Add(P); }
		B.Title = N->GetNodeTitle(ENodeTitleType::ListView).ToString().Left(42);
		{ FLinearColor C = N->GetNodeTitleColor(); if (C.R + C.G + C.B < 0.05f) C = FLinearColor(0.20f, 0.22f, 0.28f, 1.f); C.A = 1.f; B.Header = C; }
		const int32 Rows = FMath::Max(B.In.Num(), B.Out.Num());
		B.W = FMath::Max(MinW, 28.f + B.Title.Len() * 7.0f);
		B.H = HeaderH + Rows * PinRowH + BoxPad;
		NodeToBox.Add(N, Boxes.Num());
		Boxes.Add(B);
		MinX = FMath::Min(MinX, B.X); MinY = FMath::Min(MinY, B.Y);
		MaxX = FMath::Max(MaxX, B.X + B.W); MaxY = FMath::Max(MaxY, B.Y + B.H);
	}
	if (Boxes.Num() == 0) { MinX = MinY = 0; MaxX = (float)Width; MaxY = (float)Height; }

	const float Margin = 60.f;
	const float ContentW = (MaxX - MinX) + Margin * 2.f;
	const float ContentH = (MaxY - MinY) + Margin * 2.f;
	const float RScale = FMath::Min(1.f, FMath::Min(4096.f / FMath::Max(ContentW, 1.f), 4096.f / FMath::Max(ContentH, 1.f)));
	const int32 ImgW = FMath::Clamp((int32)(ContentW * RScale), 256, 4096);
	const int32 ImgH = FMath::Clamp((int32)(ContentH * RScale), 256, 4096);
	auto MapX = [&](float X){ return (X - MinX + Margin) * RScale; };
	auto MapY = [&](float Y){ return (Y - MinY + Margin) * RScale; };

	UWorld* RWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!RWorld) return TEXT("{\"success\":false,\"error\":\"No editor world for rendering\"}");

	UTextureRenderTarget2D* RT = UKismetRenderingLibrary::CreateRenderTarget2D(RWorld, ImgW, ImgH, RTF_RGBA8);
	if (!RT) return TEXT("{\"success\":false,\"error\":\"CreateRenderTarget2D failed\"}");

	UCanvas* RCanvas = nullptr; FVector2D RCanvasSize(0, 0); FDrawToRenderTargetContext RCtx;
	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(RWorld, RT, RCanvas, RCanvasSize, RCtx);
	if (RCanvas)
	{
		UFont* RFont = GEngine ? GEngine->GetSmallFont() : nullptr;
		{ FCanvasTileItem BG(FVector2D(0, 0), GWhiteTexture, FVector2D((float)ImgW, (float)ImgH), FLinearColor(0.06f, 0.06f, 0.07f, 1.f)); BG.BlendMode = SE_BLEND_Opaque; RCanvas->DrawItem(BG); }
		for (const FNodeBox& B : Boxes)
		{
			for (int32 oi = 0; oi < B.Out.Num(); ++oi)
			{
				UEdGraphPin* OutPin = B.Out[oi]; if (!OutPin) continue;
				const float ox = MapX(B.X + B.W);
				const float oy = MapY(B.Y) + (HeaderH + oi * PinRowH + PinRowH * 0.5f) * RScale;
				for (UEdGraphPin* LP : OutPin->LinkedTo)
				{
					if (!LP || !LP->GetOwningNode()) continue;
					int32* TIdx = NodeToBox.Find(LP->GetOwningNode()); if (!TIdx) continue;
					const FNodeBox& T = Boxes[*TIdx];
					int32 ii = T.In.IndexOfByKey(LP); if (ii == INDEX_NONE) ii = 0;
					const float ix = MapX(T.X);
					const float iy = MapY(T.Y) + (HeaderH + ii * PinRowH + PinRowH * 0.5f) * RScale;
					FCanvasLineItem Line(FVector2D(ox, oy), FVector2D(ix, iy));
					Line.SetColor(FLinearColor(0.55f, 0.55f, 0.6f, 1.f)); Line.LineThickness = 1.5f;
					RCanvas->DrawItem(Line);
				}
			}
		}
		for (const FNodeBox& B : Boxes)
		{
			const float x = MapX(B.X), y = MapY(B.Y), w = B.W * RScale, h = B.H * RScale;
			{ FCanvasTileItem Body(FVector2D(x, y), GWhiteTexture, FVector2D(w, h), FLinearColor(0.12f, 0.12f, 0.14f, 1.f)); Body.BlendMode = SE_BLEND_Opaque; RCanvas->DrawItem(Body); }
			{ FCanvasTileItem Hdr(FVector2D(x, y), GWhiteTexture, FVector2D(w, HeaderH * RScale), B.Header); Hdr.BlendMode = SE_BLEND_Opaque; RCanvas->DrawItem(Hdr); }
			if (RFont)
			{
				{ FCanvasTextItem T(FVector2D(x + 5.f, y + 4.f), FText::FromString(B.Title), RFont, FLinearColor::White); T.Scale = FVector2D(RScale, RScale); RCanvas->DrawItem(T); }
				for (int32 i = 0; i < B.In.Num(); ++i)
				{ FCanvasTextItem T(FVector2D(x + 5.f, y + (HeaderH + i * PinRowH) * RScale), FText::FromString(B.In[i]->PinName.ToString().Left(16)), RFont, FLinearColor(0.78f, 0.84f, 0.9f, 1.f)); T.Scale = FVector2D(0.8f * RScale, 0.8f * RScale); RCanvas->DrawItem(T); }
				for (int32 i = 0; i < B.Out.Num(); ++i)
				{ FString PN = B.Out[i]->PinName.ToString().Left(16); FCanvasTextItem T(FVector2D(x + w - 6.f - PN.Len() * 5.f, y + (HeaderH + i * PinRowH) * RScale), FText::FromString(PN), RFont, FLinearColor(0.78f, 0.84f, 0.9f, 1.f)); T.Scale = FVector2D(0.8f * RScale, 0.8f * RScale); RCanvas->DrawItem(T); }
			}
		}
	}
	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(RWorld, RCtx);

	FString SavePath;
	Cmd->TryGetStringField(TEXT("saveTo"), SavePath);
	if (SavePath.IsEmpty())
	{
		const FString BaseName = FPaths::GetBaseFilename(BP->GetPathName());
		SavePath = FPaths::ProjectSavedDir() / TEXT("NwiroGraphRenders") / (BaseName + TEXT("_") + Graph->GetName() + TEXT(".png"));
	}
	SavePath = FPaths::ConvertRelativePathToFull(SavePath);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SavePath), true);
	UKismetRenderingLibrary::ExportRenderTarget(RWorld, RT, FPaths::GetPath(SavePath), FPaths::GetCleanFilename(SavePath));

	if (!IFileManager::Get().FileExists(*SavePath))
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"ExportRenderTarget produced no file at %s\"}"), *SavePath);

	return FString::Printf(TEXT("{\"success\":true,\"path\":\"%s\",\"width\":%d,\"height\":%d,\"blueprint\":\"%s\",\"graph\":\"%s\",\"nodes\":%d}"),
		*SavePath.Replace(TEXT("\\"), TEXT("/")), ImgW, ImgH, *BP->GetName(), *Graph->GetName(), Boxes.Num());
}
