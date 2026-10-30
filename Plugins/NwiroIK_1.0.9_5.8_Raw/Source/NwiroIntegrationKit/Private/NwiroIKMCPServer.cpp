// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKMCPServer.h"
#include "NwiroIKToolRegistry.h"
#include "NwiroIKAssetGuard.h"
#include "NwiroIKTransactionHelper.h"
#include "NwiroIKBridge.h"
#include "Misc/Base64.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "NwiroIKBlueprintTools.h"
#include "NwiroIKMaterialTools.h"
#include "NwiroIKSettingsTools.h"
#include "NwiroIKPCGTools.h"
#include "NwiroIKInputTools.h"
#include "NwiroIKAssetTools.h"
#include "NwiroIKEditorTools.h"
#include "NwiroIKDataTools.h"
#include "NwiroIKAnimTools.h"
#include "NwiroIKSequencerTools.h"
#include "NwiroIKAITools.h"
#include "NwiroIKWidgetTools.h"
#include "NwiroIKNiagaraTools.h"
#include "NwiroIKStateTreeTools.h"
#include "NwiroIKTools.h"
#include "NwiroIKEnvironmentTools.h"
#include "NwiroIKGameplayTools.h"
#include "NwiroIKResourceProvider.h"
#include "NwiroIKLevelTools.h"
#include "NwiroIKGASTools.h"
#include "NwiroIKPIETools.h"
#include "NwiroIKDebugTools.h"
#include "NwiroIKProtectedRails.h"
#include "NwiroIKPathSandbox.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Json.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/CriticalSection.h"
// ElevenLabs audio import → USoundWave (factory auto-picked by extension)
#include "AssetToolsModule.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "Async/Future.h"
#include "Async/Async.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroMCP, Log, All);

TSharedPtr<IHttpRouter> FNwiroIKMCPServer::HttpRouter = nullptr;
int32 FNwiroIKMCPServer::BoundPort = 0;
int32 FNwiroIKMCPServer::RequestedPort = 0;
bool FNwiroIKMCPServer::bRunning = false;
FString FNwiroIKMCPServer::SessionId;
TArray<FNwiroIKMCPServer::FPendingToolCall> FNwiroIKMCPServer::PendingToolCalls;
FCriticalSection FNwiroIKMCPServer::PendingToolCallsLock;
int32 FNwiroIKMCPServer::NextPermissionId = 1000;
bool FNwiroIKMCPServer::bSessionAllowed = false;

// Best-effort crash-safe replacement. NOT transactionally atomic, but IFileManager::Move
// with bReplace=true maps to MoveFileEx(MOVEFILE_REPLACE_EXISTING) on Windows and
// rename() on POSIX — both close the post-delete/pre-move window that a naive
// delete-then-move leaves open.
//
// Requirement: tmp file MUST live in the same directory as the target. POSIX rename() is
// atomic only within the same filesystem; same-directory is the safe proxy.
static bool SaferReplaceWriteString(const FString& Content, const FString& TargetPath)
{
	const FString TmpPath = TargetPath + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(Content, *TmpPath))
	{
		return false;
	}
	if (!IFileManager::Get().Move(*TargetPath, *TmpPath, /*bReplace=*/true))
	{
		IFileManager::Get().Delete(*TmpPath);
		return false;
	}
	return true;
}

// ============================================================
// Tool definitions JSON — what Claude sees
// ============================================================

// Tool annotations are a client-facing risk hint. Keep this allowlist explicit:
// unknown built-ins and third-party tools remain write-capable until their
// implementations have been audited for side effects.
static bool IsReadOnlyMcpTool(const FString& ToolName)
{
	static const TSet<FString> ReadOnlyTools = {
		TEXT("bp_find_unconnected_pins"),
		TEXT("bp_get_watch_values"),
		TEXT("bp_list_breakpoints"),
		TEXT("find_assets"),
		TEXT("find_blueprint_nodes"),
		TEXT("find_blueprints"),
		TEXT("find_circular_dependencies"),
		TEXT("find_input_actions"),
		TEXT("find_material_functions"),
		TEXT("find_materials"),
		TEXT("find_orphan_assets"),
		TEXT("find_pcg_graphs"),
		TEXT("find_static_meshes"),
		TEXT("find_textures"),
		TEXT("get_actor_property"),
		TEXT("get_asset_referencers"),
		TEXT("get_asset_references"),
		TEXT("get_build_configuration"),
		TEXT("get_dependency_tree"),
		TEXT("get_foliage_stats"),
		TEXT("get_game_framework_info"),
		TEXT("get_gas_info"),
		TEXT("get_landscape_info"),
		TEXT("get_level_actors"),
		TEXT("get_level_info"),
		TEXT("get_map_check_errors"),
		TEXT("get_navigation_info"),
		TEXT("get_physics_info"),
		TEXT("get_project_info"),
		TEXT("get_project_settings"),
		TEXT("get_replication_info"),
		TEXT("get_sound_info"),
		TEXT("get_spline_info"),
		TEXT("get_world_partition_info"),
		TEXT("get_world_settings"),
		TEXT("inspect_material"),
		TEXT("inspect_material_function"),
		TEXT("inspect_material_graph"),
		TEXT("list_attribute_sets"),
		TEXT("list_gameplay_abilities"),
		TEXT("list_gameplay_effects"),
		TEXT("list_project_modules"),
		TEXT("list_resources"),
		TEXT("list_voices_elevenlabs"),
		TEXT("pie_get_blackboard_key"),
		TEXT("pie_get_game_state"),
		TEXT("pie_get_property"),
		TEXT("pie_list_actors"),
		TEXT("query_navigation_path"),
		TEXT("read_anim_blueprint"),
		TEXT("read_asset"),
		TEXT("read_behavior_tree"),
		TEXT("read_blueprint"),
		TEXT("read_data_table"),
		TEXT("read_file"),
		TEXT("read_ik_retargeter"),
		TEXT("read_ik_rig"),
		TEXT("read_log"),
		TEXT("read_montage"),
		TEXT("read_niagara_system"),
		TEXT("read_pose_search_database"),
		TEXT("read_resource"),
		TEXT("read_sequence"),
		TEXT("read_state_tree"),
		TEXT("read_widget_blueprint"),
		TEXT("search_assets"),
		TEXT("validate_assets"),
	};
	return ReadOnlyTools.Contains(ToolName);
}

static void AddMcpToolAnnotations(TArray<TSharedPtr<FJsonValue>>& Tools)
{
	for (const TSharedPtr<FJsonValue>& ToolValue : Tools)
	{
		if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object) continue;
		const TSharedPtr<FJsonObject> Tool = ToolValue->AsObject();
		if (!Tool.IsValid()) continue;

		FString ToolName;
		Tool->TryGetStringField(TEXT("name"), ToolName);
		const bool bReadOnly = IsReadOnlyMcpTool(ToolName);

		TSharedPtr<FJsonObject> Annotations = MakeShareable(new FJsonObject);
		Annotations->SetBoolField(TEXT("readOnlyHint"), bReadOnly);
		Annotations->SetBoolField(TEXT("destructiveHint"), !bReadOnly);
		Annotations->SetBoolField(TEXT("idempotentHint"), bReadOnly);
		Tool->SetObjectField(TEXT("annotations"), Annotations);
	}
}

/** Shared builder: hardcoded built-in defs + namespaced provider defs.
 *  bForSettings=false → enabled-only provider defs (the only list an agent may
 *  see). bForSettings=true → all provider defs with per-def `"disabled"` flags
 *  (settings UI only). Built-ins are identical in both modes and carry no
 *  disabled field — they are not user-disableable. */
static FString BuildToolDefinitionsJson(bool bForSettings)
{
	FString J;
	J += TEXT("[");
	J += TEXT("{\"name\":\"find_blueprints\",\"description\":\"Search Blueprint assets\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"searchTerm\":{\"type\":\"string\"}},\"required\":[\"searchTerm\"]}},");
	J += TEXT("{\"name\":\"read_blueprint\",\"description\":\"Read Blueprint structure. Pass optional 'graph' to get full node/pin data for a specific function graph instead of the summary.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"assetPath\":{\"type\":\"string\"},\"graph\":{\"type\":\"string\",\"description\":\"Optional: name of a function or event graph to return in full detail (nodes + pins + guids). Omit for summary view.\"}},\"required\":[\"assetPath\"]}},");
	J += TEXT("{\"name\":\"create_blueprint\",\"description\":\"Create Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"parentClass\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"edit_blueprint\",\"description\":\"Edit Blueprint. ALL nodes and connections MUST be in ONE call. Node types: CallFunction(function:'PrintString'),Event(event:'ReceiveBeginPlay'),CustomEvent,VariableGet,VariableSet,Branch,Sequence,ForLoop,ForEachLoop,WhileLoop,Gate,DoOnce,FlipFlop,Delay,PrintString,SetTimer,DestroyActor,SetActorLocation,GetActorLocation,SpawnActor(Class pin needs TSubclassOf<Actor>: use type:'class' subtype:'ActorClassName' variable or set_pin_defaults with class/name/Blueprint path). UMG nodes are generic: CreateWidget, AddToViewport, RemoveFromParent, SetPercent, SetText; set WidgetClass/Class pins with a WidgetBlueprint asset path via set_pin_defaults. To access a component/widget/property use GetProperty NOT CallFunction: type:'GetProperty',property:'StaticMeshComponent',ownerClass:'StaticMeshActor' — Target input pin, output pin named after the property (not ReturnValue), pure node (no exec pins). To get a component from self use GetComponent: type:'GetComponent',component:'CharacterMovement' — pure. Common pins: execute/then (exec flow), ReturnValue, self/Target. VariableGet: output pin is the variable name not ReturnValue. PrintString: execute,InString,then. Event: then. Branch: execute,Condition,True,False. Delay: execute,Duration,Completed.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"graph\":{\"type\":\"string\"},\"add_nodes\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"ref\":{\"type\":\"string\",\"description\":\"Unique ID for this node, used in connect_pins\"},\"type\":{\"type\":\"string\",\"description\":\"CallFunction, Event, CustomEvent, VariableGet, Branch, CreateWidget, AddToViewport, SetPercent, SetText, etc.\"},\"function\":{\"type\":\"string\",\"description\":\"For CallFunction: PrintString, Delay, SetText, etc.\"},\"event\":{\"type\":\"string\",\"description\":\"For Event: ReceiveBeginPlay, ReceiveTick\"},\"target\":{\"type\":\"string\",\"description\":\"For CallFunction: component/widget instance name or class name to search when resolving the function. Use for component/object/widget functions; avoids brute-force hitting the wrong class.\"}},\"required\":[\"ref\",\"type\"]}},{\"type\":\"string\",\"description\":\"JSON-encoded array (compatibility; prefer a real array)\"}]},\"connect_pins\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"from\":{\"type\":\"string\",\"description\":\"ref.PinName e.g. mynode.then\"},\"to\":{\"type\":\"string\",\"description\":\"ref.PinName e.g. other.execute\"}},\"required\":[\"from\",\"to\"]}},{\"type\":\"string\",\"description\":\"JSON-encoded array (compatibility; prefer a real array)\"}]},\"set_pin_defaults\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"ref\":{\"type\":\"string\"},\"pin\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"ref\",\"pin\",\"value\"]}},{\"type\":\"string\",\"description\":\"JSON-encoded array (compatibility; prefer a real array)\"}]},\"add_variables\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"type\":{\"type\":\"string\",\"description\":\"bool,int,float,string,vector,rotator,object,class,struct,enum (+optional subtype)\"}},\"required\":[\"name\",\"type\"]}},{\"type\":\"string\",\"description\":\"JSON-encoded array (compatibility; prefer a real array)\"}]},\"remove_variables\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"description\":\"Variable name (string) or object with a 'name' field\"}},{\"type\":\"string\",\"description\":\"JSON-encoded array (compatibility; prefer a real array)\"}]},\"add_components\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"description\":\"Each item needs 'name' and either 'class' or 'type'. Example: {'name':'CubeMesh','class':'StaticMeshComponent','properties':{'static_mesh':'/Engine/BasicShapes/Cube.Cube'}}\",\"properties\":{\"name\":{\"type\":\"string\"},\"class\":{\"type\":\"string\",\"description\":\"Component class, e.g. StaticMeshComponent ('type' also accepted)\"},\"parent\":{\"type\":\"string\"},\"makeRoot\":{\"type\":\"boolean\"},\"properties\":{\"type\":\"object\",\"description\":\"Initial property values, e.g. {'static_mesh':'/Engine/BasicShapes/Cube.Cube'}\"}},\"required\":[\"name\"]}},{\"type\":\"string\",\"description\":\"JSON-encoded array (compatibility; prefer a real array)\"}]},\"compile\":{\"type\":\"boolean\"}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"find_blueprint_nodes\",\"description\":\"Search node types\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}},");
	J += TEXT("{\"name\":\"render_blueprint_graph\",\"description\":\"Render any Blueprint's graph (EventGraph by default) to PNG. Draws nodes from graph data (headless-safe). Auto-organizes node layout first unless organize:false.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"graph\":{\"type\":\"string\",\"description\":\"Graph name (default: EventGraph or first ubergraph)\"},\"organize\":{\"type\":\"boolean\",\"description\":\"Auto-arrange nodes before rendering (default true)\"},\"width\":{\"type\":\"number\"},\"height\":{\"type\":\"number\"},\"saveTo\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"organize_blueprint_nodes\",\"description\":\"Auto-arrange a Blueprint graph's nodes so they don't overlap, laid out left-to-right along exec flow (headless layered layout). Organizes the named graph, or all graphs if none given. Saves the asset.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"graph\":{\"type\":\"string\",\"description\":\"Graph name (default: all graphs)\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"set_cdo_property\",\"description\":\"Set CDO property\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"property\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"blueprint\",\"property\",\"value\"]}},");
	J += TEXT("{\"name\":\"get_world_settings\",\"description\":\"Get world settings\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"set_world_settings\",\"description\":\"Set gravity/killZ\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"globalGravityZ\":{\"type\":\"number\"},\"killZ\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"set_game_mode\",\"description\":\"Set game mode, pawn, controller, HUD, game state, spectator classes\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"gameModeClass\":{\"type\":\"string\"},\"defaultPawnClass\":{\"type\":\"string\"},\"playerControllerClass\":{\"type\":\"string\"},\"hudClass\":{\"type\":\"string\"},\"gameStateClass\":{\"type\":\"string\"},\"spectatorClass\":{\"type\":\"string\"}},\"required\":[\"gameModeClass\"]}},");
	J += TEXT("{\"name\":\"get_project_settings\",\"description\":\"Read project settings\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"category\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"set_project_settings\",\"description\":\"Set project settings\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"settings\":{\"type\":\"object\"}},\"required\":[\"settings\"]}},");
	J += TEXT("{\"name\":\"find_materials\",\"description\":\"Search materials\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"searchTerm\":{\"type\":\"string\"}},\"required\":[\"searchTerm\"]}},");
	J += TEXT("{\"name\":\"inspect_material\",\"description\":\"Read material params\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"assetPath\":{\"type\":\"string\"}},\"required\":[\"assetPath\"]}},");
	J += TEXT("{\"name\":\"create_material\",\"description\":\"Create empty material asset. Use edit_material after this to add expressions and connections.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"edit_material\",\"description\":\"Edit material graph. Send ALL expressions AND connections in ONE call — refs only exist within that call. expressions: [{type,name}] where type is UE class (TextureSampleParameter2D,ScalarParameter,VectorParameter,Multiply,Divide,Add,Subtract,Lerp,Ceil,Floor,Clamp,Power,Desaturation,OneMinus,ComponentMask,VertexColor,WorldPosition,Time,Sine,Cosine,Abs,Dot,Normalize,Fresnel,Constant,Constant3Vector). name sets both ref AND UE5 ParameterName. connections: [{from,to}] where from/to is RefName (default output) or RefName.Pin. Material pins: Material.BaseColor,Material.Normal,Material.Roughness,Material.Metallic,Material.EmissiveColor,Material.Opacity,Material.AmbientOcclusion. Math pins: A,B. Lerp: A,B,Alpha. Desaturation: Input(0),Fraction. MaterialFunctionCall expressions bind via 'materialFunction' (path of the function to call); aliases accepted: function, functionPath, function_path, material_function, assetPath (alias here is scoped to the expression object — distinct from the top-level assetPath parameter).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"assetPath\":{\"type\":\"string\"},\"expressions\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\"}},{\"type\":\"string\"}]},\"connections\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\"}},{\"type\":\"string\"}]}},\"required\":[\"assetPath\"]}},");
	J += TEXT("{\"name\":\"inspect_material_graph\",\"description\":\"Inspect material graph structure and expressions\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"material\":{\"type\":\"string\"}},\"required\":[]}},");
	J += TEXT("{\"name\":\"create_material_instance\",\"description\":\"Create material instance\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"parent\":{\"type\":\"string\"}},\"required\":[\"name\",\"parent\"]}},");
	J += TEXT("{\"name\":\"apply_material\",\"description\":\"Apply material to actor\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"materialPath\":{\"type\":\"string\"},\"actorPath\":{\"type\":\"string\"}},\"required\":[\"materialPath\",\"actorPath\"]}},");
	J += TEXT("{\"name\":\"find_textures\",\"description\":\"Search textures\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"searchTerm\":{\"type\":\"string\"}},\"required\":[\"searchTerm\"]}},");
	J += TEXT("{\"name\":\"create_input_action\",\"description\":\"Create input action\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"valueType\":{\"type\":\"string\"}},\"required\":[\"name\",\"valueType\"]}},");
	J += TEXT("{\"name\":\"create_input_mapping_context\",\"description\":\"Create input mapping\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"mappings\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}},\"required\":[\"name\",\"mappings\"]}},");
	J += TEXT("{\"name\":\"find_input_actions\",\"description\":\"Search input actions\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"searchTerm\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"get_level_actors\",\"description\":\"List level actors\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"classFilter\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"find_static_meshes\",\"description\":\"Search static meshes\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"searchTerm\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"clear_graph\",\"description\":\"DESTRUCTIVE: removes ALL nodes from a blueprint graph. Only use this when the user explicitly asks to start the graph over. NEVER call this in response to an edit_blueprint or connect_pins error — those errors include the available node refs and pin names so you can self-correct without wiping anything.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"graph\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"delete_blueprint\",\"description\":\"Delete a blueprint asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"assetPath\":{\"type\":\"string\"}},\"required\":[\"assetPath\"]}},");
	J += TEXT("{\"name\":\"duplicate_blueprint\",\"description\":\"Duplicate a blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"},\"newName\":{\"type\":\"string\"},\"destinationPath\":{\"type\":\"string\"}},\"required\":[\"source\",\"newName\"]}},");
	J += TEXT("{\"name\":\"rename_blueprint\",\"description\":\"Rename a blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"assetPath\":{\"type\":\"string\"},\"newName\":{\"type\":\"string\"}},\"required\":[\"assetPath\",\"newName\"]}},");
	J += TEXT("{\"name\":\"spawn_actor\",\"description\":\"Spawn an actor in the level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"class\":{\"type\":\"string\"},\"blueprint\":{\"type\":\"string\"},\"label\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"delete_actor\",\"description\":\"Delete an actor from the level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"transform_actor\",\"description\":\"Move/rotate/scale an actor. Location: x,y,z. Rotation: pitch,yaw,roll. Scale: scale (uniform) or scaleX,scaleY,scaleZ\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"pitch\":{\"type\":\"number\"},\"yaw\":{\"type\":\"number\"},\"roll\":{\"type\":\"number\"},\"scale\":{\"type\":\"number\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"get_actor_property\",\"description\":\"Get a property value from an actor in the level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"property\":{\"type\":\"string\"}},\"required\":[\"name\",\"property\"]}},");
	J += TEXT("{\"name\":\"set_actor_property\",\"description\":\"Set a property value on an actor in the level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"property\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"name\",\"property\",\"value\"]}},");
	J += TEXT("{\"name\":\"execute_python\",\"description\":\"Execute Python code in UE5 editor. Use unreal module for engine API. Most powerful tool — can do anything the editor can do.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\",\"description\":\"Python code to execute\"}},\"required\":[\"code\"]}},");
	J += TEXT("{\"name\":\"search_assets\",\"description\":\"Search all project assets by name. Returns name, path, and class.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}},");
	J += TEXT("{\"name\":\"write_file\",\"description\":\"Write content to a file. Creates or overwrites. Requires 'File Editor' extension enabled for this chat.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\",\"description\":\"Path relative to project directory. Absolute paths must resolve within the project; anything outside is rejected.\"},\"content\":{\"type\":\"string\",\"description\":\"File content to write\"}},\"required\":[\"file_path\",\"content\"]}},");
	J += TEXT("{\"name\":\"read_file\",\"description\":\"Read content from a file inside the project directory. Always available - no extension required.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\",\"description\":\"Path relative to project directory. Absolute paths must resolve within the project; anything outside is rejected.\"}},\"required\":[\"file_path\"]}},");
	J += TEXT("{\"name\":\"delete_file\",\"description\":\"Delete a file from disk. Requires 'File Editor' extension enabled for this chat.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\",\"description\":\"Path relative to project directory. Absolute paths must resolve within the project; anything outside is rejected.\"}},\"required\":[\"file_path\"]}},");
	J += TEXT("{\"name\":\"rename_file\",\"description\":\"Rename or move a file. Requires 'File Editor' extension enabled for this chat.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"old_path\":{\"type\":\"string\",\"description\":\"Source path relative to project directory. Absolute paths must resolve within the project; anything outside is rejected.\"},\"new_path\":{\"type\":\"string\",\"description\":\"Destination path relative to project directory. Absolute paths must resolve within the project; anything outside is rejected.\"}},\"required\":[\"old_path\",\"new_path\"]}},");

	// Blueprint standalone tools
	J += TEXT("{\"name\":\"delete_node\",\"description\":\"Delete node(s) from a blueprint graph by ref\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"graph\":{\"type\":\"string\"},\"ref\":{\"type\":\"string\"},\"refs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"create_function_graph\",\"description\":\"Create a custom function graph in a blueprint. The auto-created entry node ref equals the function name — use FunctionName.then as the first exec output pin in connect_pins.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"functions\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}},\"pure\":{\"type\":\"boolean\"}},\"required\":[\"blueprint\",\"name\"]}},");
	J += TEXT("{\"name\":\"add_interface\",\"description\":\"Add a Blueprint Interface to a blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"interface\":{\"type\":\"string\"},\"interfaces\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"remove_interface\",\"description\":\"Remove a Blueprint Interface from a blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"interface\":{\"type\":\"string\"},\"interfaces\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"create_event_dispatcher\",\"description\":\"Create an Event Dispatcher on a blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"dispatchers\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}},\"required\":[\"blueprint\",\"name\"]}},");
	J += TEXT("{\"name\":\"reparent_blueprint\",\"description\":\"Change a blueprint parent class\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"parentClass\":{\"type\":\"string\"}},\"required\":[\"blueprint\",\"parentClass\"]}},");
	J += TEXT("{\"name\":\"remove_component\",\"description\":\"Remove component(s) from a blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"components\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"edit_component\",\"description\":\"Set properties on an EXISTING blueprint component (mesh, physics, etc). Cannot add new components — use edit_blueprint with add_components for that.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"properties\":{\"type\":\"object\"},\"components\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}},\"required\":[\"blueprint\"]}},");

	// Material standalone tools
	J += TEXT("{\"name\":\"delete_material\",\"description\":\"Delete a material asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"set_material_property\",\"description\":\"Set material properties: blend_mode (Opaque/Masked/Translucent/Additive/Modulate), shading_model (DefaultLit/Unlit/Subsurface/ClearCoat/TwoSidedFoliage), two_sided (bool), opacity_mask_clip_value (number)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"material\":{\"type\":\"string\"},\"blend_mode\":{\"type\":\"string\"},\"shading_model\":{\"type\":\"string\"},\"two_sided\":{\"type\":\"boolean\"},\"opacity_mask_clip_value\":{\"type\":\"number\"}},\"required\":[\"material\"]}},");
	J += TEXT("{\"name\":\"edit_material_instance\",\"description\":\"Edit material instance parameters\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"scalars\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}},\"vectors\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}},\"textures\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"delete_expression\",\"description\":\"Delete expression(s) from a material by ref\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"material\":{\"type\":\"string\"},\"ref\":{\"type\":\"string\"},\"refs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"material\"]}},");

	// Material Function tools (Policy 1.1 strict)
	J += TEXT("{\"name\":\"edit_material_function\",");
	J += TEXT("\"description\":\"Edit a UMaterialFunction asset (Policy 1.1 strict). Target: assetPath. Nested contract: addInputs [{name or ref,type,sortPriority,description,x,y,previewValue,useAsDefault,defaultValue}], type enum Scalar|Vector2|Vector3|Vector4|Texture2D|TextureCube|StaticBool|MaterialAttributes. previewValue is the canonical default-value slot; its shape depends on type — Scalar:number, Vector2/3/4:array[N] of numbers, StaticBool:boolean. Texture2D/TextureCube/MaterialAttributes do not accept previewValue (INVALID_VALUE). useAsDefault:boolean controls whether PreviewValue substitutes when the input is unconnected at the call site. defaultValue is an alias accepted for AI compatibility; it normalizes to previewValue plus useAsDefault:true (unless useAsDefault is supplied explicitly) and emits an ALIAS_NORMALIZED warning naming the canonical pair. addOutputs [{name or ref,sortPriority,description,x,y}]. addExpressions [{name or ref,type,class,x,y,materialFunction,properties,default,defaultValue,texture,value,r,g,b,a,exponent,exponentIn}]. For MaterialFunctionCall, use canonical materialFunction only; aliases such as function/functionPath/material_function/assetPath are rejected. connect [{from,to}] uses dot pin syntax, e.g. {\\\"from\\\":\\\"Alpha\\\",\\\"to\\\":\\\"LerpNode.Alpha\\\"}. Colon syntax (Ref:Pin) is accepted as an alias and normalized to dot syntax with an ALIAS_NORMALIZED warning naming the canonical form. disconnect [{to}] only, same dot syntax. removeInputs/removeOutputs/deleteExpressions are string arrays. setMetadata {description,category,exposeToLibrary}. clearGraph true removes graph expressions. Array fields (addInputs, addOutputs, addExpressions, connect, disconnect, removeInputs, removeOutputs, deleteExpressions) accept real JSON arrays (canonical) or, as a framework-compatibility input only, a JSON-encoded string representation; the string form is parsed in place and emits an ALIAS_NORMALIZED warning naming `array` as canonical. Unknown or invalid top-level/nested schema fails before mutation. Response envelope always includes success, policy_version, _callId, messages, created, modified, skipped, warnings, errors, and failure error string.\",");
	J += TEXT("\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"_callId\":{\"type\":\"string\"},\"assetPath\":{\"type\":\"string\"},\"clearGraph\":{\"type\":\"boolean\"},\"addInputs\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\"},\"ref\":{\"type\":\"string\"},\"type\":{\"type\":\"string\",\"enum\":[\"Scalar\",\"Vector2\",\"Vector3\",\"Vector4\",\"Texture2D\",\"TextureCube\",\"StaticBool\",\"MaterialAttributes\"]},\"sortPriority\":{\"type\":\"number\"},\"description\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"previewValue\":{},\"useAsDefault\":{\"type\":\"boolean\"},\"defaultValue\":{}},\"required\":[\"type\"],\"anyOf\":[{\"required\":[\"name\"]},{\"required\":[\"ref\"]}]},\"description\":\"Canonical JSON array. JSON-encoded string accepted as compatibility input.\"},{\"type\":\"string\"}]},\"addOutputs\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\"},\"ref\":{\"type\":\"string\"},\"sortPriority\":{\"type\":\"number\"},\"description\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"}},\"anyOf\":[{\"required\":[\"name\"]},{\"required\":[\"ref\"]}]}},{\"type\":\"string\"}]},\"addExpressions\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\"},\"ref\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"},\"class\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"materialFunction\":{\"type\":\"string\"},\"properties\":{\"type\":\"object\"},\"default\":{},\"defaultValue\":{},\"texture\":{\"type\":\"string\"},\"value\":{},\"r\":{\"type\":\"number\"},\"g\":{\"type\":\"number\"},\"b\":{\"type\":\"number\"},\"a\":{\"type\":\"number\"},\"exponent\":{\"type\":\"number\"},\"exponentIn\":{\"type\":\"number\"}},\"required\":[\"type\"],\"anyOf\":[{\"required\":[\"name\"]},{\"required\":[\"ref\"]}]}},{\"type\":\"string\"}]},\"connect\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}},\"required\":[\"from\",\"to\"]}},{\"type\":\"string\"}]},\"disconnect\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"to\":{\"type\":\"string\"}},\"required\":[\"to\"]}},{\"type\":\"string\"}]},\"removeInputs\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"string\"}},{\"type\":\"string\"}]},\"removeOutputs\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"string\"}},{\"type\":\"string\"}]},\"deleteExpressions\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"string\"}},{\"type\":\"string\"}]},\"setMetadata\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"description\":{\"type\":\"string\"},\"category\":{\"type\":\"string\"},\"exposeToLibrary\":{\"type\":\"boolean\"}}}},\"required\":[\"assetPath\"]}},");

	J += TEXT("{\"name\":\"inspect_material_function\",");
	J += TEXT("\"description\":\"Read a UMaterialFunction asset (Policy 1.1 strict, read-only). Target: assetPath (canonical; 'path' is rejected). Returns the function's name, description, category, exposedToLibrary, inputs[], outputs[], expressions[], connections[], referencedBy[] plus the policy envelope (success, policy_version, _callId, messages, created, modified, skipped, warnings, errors). Orphan expressions (no outgoing connection) are reported as structured warnings[] with code ORPHAN_EXPRESSION (tool-specific extension). Function inputs whose UE input type is not representable in edit_material_function (e.g. Texture2DArray, VolumeTexture) are still emitted in inputs[] with type:'Unsupported' plus a warnings[] entry with code UNSUPPORTED_INPUT_TYPE; allowed[] lists the eight contract types. Failure cases emit NOT_FOUND when the asset doesn't exist and MISSING_REQUIRED_FIELD when assetPath is omitted.\",");
	J += TEXT("\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"_callId\":{\"type\":\"string\"},\"assetPath\":{\"type\":\"string\"}},\"required\":[\"assetPath\"]}},");

	J += TEXT("{\"name\":\"find_material_functions\",");
	J += TEXT("\"description\":\"Search for UMaterialFunction assets (Policy 1.1 strict, read-only). Inputs: searchTerm (case-insensitive substring; omit to list all), searchRoot (package path to search under, default /Game), maxResults (default 50, capped at 200). Returns functions[] where each row uses canonical assetPath plus name, description, exposedToLibrary, category. Envelope: success, policy_version, _callId, count, truncated, and the six standard arrays. When truncated, also emits returnedCount, totalAllowed, and discoveryTool (set to this tool name so the AI knows to call again with a narrower searchTerm or higher maxResults).\",");
	J += TEXT("\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"_callId\":{\"type\":\"string\"},\"searchTerm\":{\"type\":\"string\"},\"searchRoot\":{\"type\":\"string\"},\"maxResults\":{\"type\":\"number\"}}}},");

	J += TEXT("{\"name\":\"create_material_function\",");
	J += TEXT("\"description\":\"Create a new empty UMaterialFunction asset (Policy 1.1 strict). Canonical happy path: send 'assetPath' as a full destination path like '/Game/Foo/MF_Bar'. The 'name + path' pair is accepted only as a migration alias and emits ALIAS_NORMALIZED warnings. If both 'assetPath' and 'name'/'path' are supplied and they resolve to different targets, the call fails with INVALID_VALUE before any mutation. 'MF_' prefix is auto-applied to missing names (warned). Optional metadata: description, category, exposeToLibrary (default true). Fails with ASSET_ALREADY_EXISTS when the target already exists, MISSING_REQUIRED_FIELD when neither assetPath nor name is supplied, INVALID_VALUE for malformed paths or destinations outside /Game/. Returns the policy envelope with one created[] entry: {op:'create_material_function', userName, assignedRef:<canonical assetPath>, type:'MaterialFunction'}.\",");
	J += TEXT("\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"_callId\":{\"type\":\"string\"},\"assetPath\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"},\"category\":{\"type\":\"string\"},\"exposeToLibrary\":{\"type\":\"boolean\"}}}},");

	J += TEXT("{\"name\":\"delete_material_function\",");
	J += TEXT("\"destructive\":true,");
	J += TEXT("\"description\":\"Delete a UMaterialFunction asset (Policy 1.1 strict M2-destructive). Two-flag safety: 'preview' (default true) controls preview vs execute; 'allowBrokenReferences' (default false) is additionally required to delete an asset that has referencers. Send 'preview:false' alone for unreferenced assets; for referenced assets you MUST send BOTH 'preview:false' AND 'allowBrokenReferences:true'. PREVIEW response: success:true, preview:true, destructive:true, plus wouldDelete[] (canonical M2) and wouldModify[] with one {op:'break_reference', assetPath, type} row per referencer; no mutation occurs. EXECUTE success: deletion is recorded in modified[] as {op:'delete', assetPath, type:'MaterialFunction'} alongside any break_reference rows for now-broken referencers. There is no deleted[] array — deletions live in modified[] per rule 16. REFUSAL on preview:false without allowBrokenReferences:true returns success:false with errors[].code='WOULD_BREAK_REFERENCES' (allowed:['true']). Tool-specific extension codes emitted: WOULD_BREAK_REFERENCES (errors[], when allowBrokenReferences is required but missing), BROKEN_REFERENCES (warnings[], after a successful destructive delete with referencers). Both boolean fields (preview, allowBrokenReferences) accept real JSON booleans (canonical) or, as a framework-compatibility input only, the exact JSON strings 'true' / 'false'; the string form is normalized in place and emits an ALIAS_NORMALIZED warning naming `boolean` as canonical. Fails with NOT_FOUND when the asset doesn't exist and INVALID_VALUE when the target asset is not a MaterialFunction. preview/refusal branches never mutate; allowBrokenReferences is checked before DeleteAsset is called.\",");
	J += TEXT("\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"_callId\":{\"type\":\"string\"},\"assetPath\":{\"type\":\"string\"},\"preview\":{\"oneOf\":[{\"type\":\"boolean\"},{\"type\":\"string\",\"enum\":[\"true\",\"false\"]}]},\"allowBrokenReferences\":{\"oneOf\":[{\"type\":\"boolean\"},{\"type\":\"string\",\"enum\":[\"true\",\"false\"]}]}},\"required\":[\"assetPath\"]}},");

	// Actor tools
	J += TEXT("{\"name\":\"duplicate_actor\",\"description\":\"Duplicate an actor in the level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"newName\":{\"type\":\"string\"},\"offsetX\":{\"type\":\"number\"},\"offsetY\":{\"type\":\"number\"},\"offsetZ\":{\"type\":\"number\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"rename_actor\",\"description\":\"Rename an actor label in the level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"newName\":{\"type\":\"string\"}},\"required\":[\"name\",\"newName\"]}},");
	J += TEXT("{\"name\":\"attach_actor\",\"description\":\"Attach a child actor to a parent actor\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"child\":{\"type\":\"string\"},\"parent\":{\"type\":\"string\"},\"socketName\":{\"type\":\"string\"},\"rule\":{\"type\":\"string\",\"description\":\"KeepWorld (default), KeepRelative, SnapToTarget\"}},\"required\":[\"child\",\"parent\"]}},");
	J += TEXT("{\"name\":\"detach_actor\",\"description\":\"Detach an actor from its parent\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"rule\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"select_actor\",\"description\":\"Select actor(s) in the editor\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"names\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}},");

	// Input tools
	J += TEXT("{\"name\":\"delete_input_action\",\"description\":\"Delete an InputAction or InputMappingContext asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"edit_mapping_context\",\"description\":\"Edit an InputMappingContext: add or remove key mappings\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"add_mappings\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}},\"remove_mappings\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}},");

	// === Generic Asset Tools ===
	J += TEXT("{\"name\":\"read_asset\",\"description\":\"Read any asset properties via reflection. Works with any UObject asset type.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"depth\":{\"type\":\"number\",\"description\":\"Recursion depth (default:1)\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"find_assets\",\"description\":\"Search assets by name and optional class filter\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"searchTerm\":{\"type\":\"string\"},\"classFilter\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"maxResults\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"get_asset_thumbnail\",\"description\":\"Render asset thumbnail to PNG file\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"width\":{\"type\":\"number\"},\"height\":{\"type\":\"number\"}},\"required\":[\"path\"]}},");

	// === Editor Tools ===
	J += TEXT("{\"name\":\"take_screenshot\",\"description\":\"Capture editor viewport screenshot\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"filename\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"read_log\",\"description\":\"Read recent UE Output Log entries. Call this after a compile error or unexpected tool failure to get full diagnostic detail. Useful categories: 'LogK2Compiler' for Blueprint compile details, 'LogNwiroBP' for tool resolution details, 'LogBlueprint' for graph errors. Use severity:'Error' to filter to errors only.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"lines\":{\"type\":\"number\",\"description\":\"Number of lines (default:50)\"},\"severity\":{\"type\":\"string\",\"description\":\"Filter: Error/Warning/Log\"},\"category\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"play_in_editor\",\"description\":\"Start Play in Editor (PIE)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"stop_pie\",\"description\":\"Stop Play in Editor\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");

	// === Data Structure Tools ===
	J += TEXT("{\"name\":\"create_data_table\",\"description\":\"Create a DataTable asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"rowStruct\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"add_data_table_row\",\"description\":\"Add a row to a DataTable\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"table\":{\"type\":\"string\"},\"rowName\":{\"type\":\"string\"},\"values\":{\"type\":\"object\"}},\"required\":[\"table\",\"rowName\"]}},");
	J += TEXT("{\"name\":\"read_data_table\",\"description\":\"Read all rows from a DataTable\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"table\":{\"type\":\"string\"}},\"required\":[\"table\"]}},");
	J += TEXT("{\"name\":\"import_data_table_json\",\"description\":\"Import rows into DataTable from JSON\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"table\":{\"type\":\"string\"},\"json\":{\"type\":\"string\"}},\"required\":[\"table\",\"json\"]}},");
	J += TEXT("{\"name\":\"create_struct\",\"description\":\"Create a UserDefinedStruct with fields\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"fields\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"}}}}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"create_enum\",\"description\":\"Create a UserDefinedEnum with values\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"values\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"name\"]}},");

	// === Animation Tools ===
	J += TEXT("{\"name\":\"create_montage\",\"description\":\"Create AnimMontage from skeleton or animation\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"animation\":{\"type\":\"string\"},\"skeleton\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_montage\",\"description\":\"Read AnimMontage structure\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"add_montage_section\",\"description\":\"Add section to montage\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"montage\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"startTime\":{\"type\":\"number\"}},\"required\":[\"montage\",\"name\",\"startTime\"]}},");
	J += TEXT("{\"name\":\"link_montage_sections\",\"description\":\"Link montage sections\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"montage\":{\"type\":\"string\"},\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}},\"required\":[\"montage\",\"from\",\"to\"]}},");
	J += TEXT("{\"name\":\"add_montage_notify\",\"description\":\"Add notify to montage\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"montage\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"time\":{\"type\":\"number\"},\"duration\":{\"type\":\"number\"}},\"required\":[\"montage\",\"name\",\"time\"]}},");
	J += TEXT("{\"name\":\"create_anim_blueprint\",\"description\":\"Create AnimBlueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"skeleton\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\",\"skeleton\"]}},");
	J += TEXT("{\"name\":\"read_anim_blueprint\",\"description\":\"Read AnimBlueprint structure\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");

	// === Sequencer Tools ===
	J += TEXT("{\"name\":\"create_sequence\",\"description\":\"Create LevelSequence\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"fps\":{\"type\":\"number\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_sequence\",\"description\":\"Read LevelSequence structure\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"add_sequence_binding\",\"description\":\"Bind actor to sequence\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sequence\":{\"type\":\"string\"},\"actor\":{\"type\":\"string\"}},\"required\":[\"sequence\",\"actor\"]}},");
	J += TEXT("{\"name\":\"add_sequence_track\",\"description\":\"Add track to sequence binding. Types: Transform,Float,Bool,Audio,Event,CameraCut\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sequence\":{\"type\":\"string\"},\"binding\":{\"type\":\"string\"},\"trackType\":{\"type\":\"string\"}},\"required\":[\"sequence\",\"trackType\"]}},");
	J += TEXT("{\"name\":\"add_sequence_keyframe\",\"description\":\"Add transform keyframe\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sequence\":{\"type\":\"string\"},\"binding\":{\"type\":\"string\"},\"frame\":{\"type\":\"number\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"pitch\":{\"type\":\"number\"},\"yaw\":{\"type\":\"number\"},\"roll\":{\"type\":\"number\"}},\"required\":[\"sequence\",\"binding\",\"frame\"]}},");
	J += TEXT("{\"name\":\"set_sequence_range\",\"description\":\"Set sequence playback range\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sequence\":{\"type\":\"string\"},\"startFrame\":{\"type\":\"number\"},\"endFrame\":{\"type\":\"number\"},\"fps\":{\"type\":\"number\"}},\"required\":[\"sequence\",\"startFrame\",\"endFrame\"]}},");

	// === AI Tools ===
	J += TEXT("{\"name\":\"create_behavior_tree\",\"description\":\"Create BehaviorTree asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"blackboard\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_behavior_tree\",\"description\":\"Read BehaviorTree structure\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"create_blackboard\",\"description\":\"Create Blackboard with keys\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"keys\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"}}}}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"edit_blackboard\",\"description\":\"Add/remove Blackboard keys\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"add_keys\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}},\"remove_keys\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"path\"]}},");

	// === Widget Blueprint Tools ===
	J += TEXT("{\"name\":\"create_widget_blueprint\",\"description\":\"Create UMG WidgetBlueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"rootWidget\":{\"type\":\"string\",\"description\":\"CanvasPanel(default),VerticalBox,HorizontalBox,Overlay\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_widget_blueprint\",\"description\":\"Read WidgetBlueprint tree\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"add_widget\",\"description\":\"Add widget to WidgetBlueprint. Classes: Button,TextBlock,Image,CanvasPanel,VerticalBox,HorizontalBox,Overlay,ScrollBox,Slider,CheckBox,ProgressBar,EditableTextBox,Border,Spacer,SizeBox\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"widgetClass\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"parent\":{\"type\":\"string\"}},\"required\":[\"blueprint\",\"widgetClass\"]}},");
	J += TEXT("{\"name\":\"set_widget_property\",\"description\":\"Set property on a widget via reflection\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"widget\":{\"type\":\"string\"},\"property\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"blueprint\",\"widget\",\"property\",\"value\"]}},");
	J += TEXT("{\"name\":\"render_widget_blueprint\",\"description\":\"Render a UMG WidgetBlueprint to PNG via FWidgetRenderer. Returns absolute file path. Optional width/height (default 1280x720) and saveTo (default ProjectSavedDir/NwiroWidgetRenders).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"width\":{\"type\":\"number\"},\"height\":{\"type\":\"number\"},\"saveTo\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");

	// === Niagara Tools ===
	J += TEXT("{\"name\":\"create_niagara_system\",\"description\":\"Create Niagara particle system\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_niagara_system\",\"description\":\"Read Niagara system structure\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"set_niagara_parameter\",\"description\":\"Set exposed parameter on Niagara system\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"system\":{\"type\":\"string\"},\"parameter\":{\"type\":\"string\"},\"value\":{\"type\":\"number\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"required\":[\"system\",\"parameter\"]}},");

	// === State Tree Tools ===
	J += TEXT("{\"name\":\"create_state_tree\",\"description\":\"Create StateTree asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_state_tree\",\"description\":\"Read StateTree structure (states, tasks, transitions)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"add_state_tree_state\",\"description\":\"Add a state to a StateTree (top-level or as child of parent)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"stateTree\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"parent\":{\"type\":\"string\"}},\"required\":[\"stateTree\",\"name\"]}},");

#if NWIRO_HAS_IK_TOOLS
	// === IK Rig Tools ===
	J += TEXT("{\"name\":\"create_ik_rig\",\"description\":\"Create IKRig asset with skeleton\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"skeleton\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_ik_rig\",\"description\":\"Read IKRig (goals, solvers, retarget chains)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"add_ik_goal\",\"description\":\"Add IK effector goal to IKRig\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"rig\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"bone\":{\"type\":\"string\"}},\"required\":[\"rig\",\"name\",\"bone\"]}},");
	J += TEXT("{\"name\":\"add_ik_solver\",\"description\":\"Add IK solver to IKRig. Types: FBIKSolver, LimbSolver, SetTransform, PoleSolver\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"rig\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"}},\"required\":[\"rig\",\"type\"]}},");
	J += TEXT("{\"name\":\"add_retarget_chain\",\"description\":\"Add retarget chain to IKRig\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"rig\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"startBone\":{\"type\":\"string\"},\"endBone\":{\"type\":\"string\"}},\"required\":[\"rig\",\"name\",\"startBone\",\"endBone\"]}},");

	// === IK Retargeter Tools ===
	J += TEXT("{\"name\":\"create_ik_retargeter\",\"description\":\"Create IKRetargeter with source/target IKRigs\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"sourceIKRig\":{\"type\":\"string\"},\"targetIKRig\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_ik_retargeter\",\"description\":\"Read IKRetargeter (chain mappings, source/target)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"set_chain_mapping\",\"description\":\"Map source chain to target chain in IKRetargeter\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"retargeter\":{\"type\":\"string\"},\"sourceChain\":{\"type\":\"string\"},\"targetChain\":{\"type\":\"string\"}},\"required\":[\"retargeter\",\"sourceChain\",\"targetChain\"]}},");

	// === Pose Search / Motion Matching Tools ===
	J += TEXT("{\"name\":\"create_pose_search_schema\",\"description\":\"Create PoseSearch schema for motion matching\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"skeleton\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"create_pose_search_database\",\"description\":\"Create PoseSearch database with schema\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"schema\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_pose_search_database\",\"description\":\"Read PoseSearch database (animations, schema)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"add_pose_search_animation\",\"description\":\"Add animation to PoseSearch database\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"database\":{\"type\":\"string\"},\"animation\":{\"type\":\"string\"}},\"required\":[\"database\",\"animation\"]}},");
#endif // NWIRO_HAS_IK_TOOLS

	// === BT / AnimBP / Niagara edit tools ===
	J += TEXT("{\"name\":\"add_behavior_tree_nodes\",\"description\":\"Add nodes to BehaviorTree (via Python bridge)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"nodeType\":{\"type\":\"string\"},\"nodeClass\":{\"type\":\"string\"},\"parent\":{\"type\":\"string\"}},\"required\":[\"path\",\"nodeClass\"]}},");
	J += TEXT("{\"name\":\"add_anim_bp_state_machine\",\"description\":\"Add state machine to AnimBlueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"add_niagara_emitter\",\"description\":\"Add emitter to Niagara system\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"system\":{\"type\":\"string\"},\"emitter\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"required\":[\"system\",\"emitter\"]}},");

	// === Environment Tools ===
	J += TEXT("{\"name\":\"set_post_process\",\"description\":\"Configure PostProcessVolume: bloom, exposure, vignette, saturation, contrast\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"bloom_intensity\":{\"type\":\"number\"},\"auto_exposure_min\":{\"type\":\"number\"},\"auto_exposure_max\":{\"type\":\"number\"},\"vignette_intensity\":{\"type\":\"number\"},\"saturation\":{\"type\":\"number\"},\"contrast\":{\"type\":\"number\"},\"infinite_extent\":{\"type\":\"boolean\"}}}},");
	J += TEXT("{\"name\":\"set_fog\",\"description\":\"Configure ExponentialHeightFog: density, falloff, distance, volumetric\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"density\":{\"type\":\"number\"},\"height_falloff\":{\"type\":\"number\"},\"start_distance\":{\"type\":\"number\"},\"volumetric\":{\"type\":\"boolean\"}}}},");
	J += TEXT("{\"name\":\"set_sky_atmosphere\",\"description\":\"Configure SkyAtmosphere properties\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"set_light_properties\",\"description\":\"Set light properties: intensity, color, temperature, shadows, radius, cone angles\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"intensity\":{\"type\":\"number\"},\"color\":{\"type\":\"object\"},\"temperature\":{\"type\":\"number\"},\"cast_shadows\":{\"type\":\"boolean\"},\"source_radius\":{\"type\":\"number\"},\"attenuation_radius\":{\"type\":\"number\"},\"inner_cone_angle\":{\"type\":\"number\"},\"outer_cone_angle\":{\"type\":\"number\"}},\"required\":[\"actor\"]}},");

	// === Physics Tools ===
	J += TEXT("{\"name\":\"set_physics_simulation\",\"description\":\"Enable/configure physics on actor: simulate, gravity, mass, damping\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"simulate\":{\"type\":\"boolean\"},\"gravity\":{\"type\":\"boolean\"},\"mass\":{\"type\":\"number\"},\"linear_damping\":{\"type\":\"number\"},\"angular_damping\":{\"type\":\"number\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"set_collision_profile\",\"description\":\"Set collision profile on actor\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"profile\":{\"type\":\"string\"}},\"required\":[\"actor\",\"profile\"]}},");
	J += TEXT("{\"name\":\"add_physics_constraint\",\"description\":\"Create physics constraint between two actors. Types: Fixed, Hinge, BallSocket\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor1\":{\"type\":\"string\"},\"actor2\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"}},\"required\":[\"actor1\",\"actor2\"]}},");
	J += TEXT("{\"name\":\"get_physics_info\",\"description\":\"Get physics state of an actor\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"}},\"required\":[\"actor\"]}},");

	// === Spline Tools ===
	J += TEXT("{\"name\":\"create_spline_actor\",\"description\":\"Create spline actor with initial points\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\"},\"points\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}}}},");
	J += TEXT("{\"name\":\"add_spline_point\",\"description\":\"Add point to spline\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"set_spline_point\",\"description\":\"Move a spline point\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"index\":{\"type\":\"number\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"required\":[\"actor\",\"index\"]}},");
	J += TEXT("{\"name\":\"remove_spline_point\",\"description\":\"Remove spline point by index\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"index\":{\"type\":\"number\"}},\"required\":[\"actor\",\"index\"]}},");
	J += TEXT("{\"name\":\"get_spline_info\",\"description\":\"Get spline details: points, length, closed state\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"set_spline_closed\",\"description\":\"Toggle spline closed loop\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"closed\":{\"type\":\"boolean\"}},\"required\":[\"actor\",\"closed\"]}},");
	J += TEXT("{\"name\":\"set_spline_point_type\",\"description\":\"Set spline point type: Linear, Curve, Constant, CurveClamped\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"index\":{\"type\":\"number\"},\"type\":{\"type\":\"string\"}},\"required\":[\"actor\",\"index\",\"type\"]}},");

	// === Navigation Tools ===
	J += TEXT("{\"name\":\"build_navigation\",\"description\":\"Build navmesh. Requires NavMeshBoundsVolume in level.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"query_navigation_path\",\"description\":\"Find navigation path between two points\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"startX\":{\"type\":\"number\"},\"startY\":{\"type\":\"number\"},\"startZ\":{\"type\":\"number\"},\"endX\":{\"type\":\"number\"},\"endY\":{\"type\":\"number\"},\"endZ\":{\"type\":\"number\"}},\"required\":[\"startX\",\"startY\",\"startZ\",\"endX\",\"endY\",\"endZ\"]}},");
	J += TEXT("{\"name\":\"get_navigation_info\",\"description\":\"Get navmesh build status and bounds volume count\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");

	// === Audio Tools ===
	J += TEXT("{\"name\":\"spawn_sound\",\"description\":\"Spawn AmbientSound actor with sound asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sound\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"label\":{\"type\":\"string\"},\"volume\":{\"type\":\"number\"},\"pitch\":{\"type\":\"number\"}},\"required\":[\"sound\"]}},");
	J += TEXT("{\"name\":\"set_audio_properties\",\"description\":\"Set audio component properties on actor\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"volume\":{\"type\":\"number\"},\"pitch\":{\"type\":\"number\"},\"autoActivate\":{\"type\":\"boolean\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"get_sound_info\",\"description\":\"Get sound asset details: duration, channels, sample rate\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");

	// === Game Framework Tools ===
	J += TEXT("{\"name\":\"create_game_mode\",\"description\":\"Create GameMode Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"create_player_controller\",\"description\":\"Create PlayerController Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"create_game_state\",\"description\":\"Create GameState Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"create_player_state\",\"description\":\"Create PlayerState Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"create_hud\",\"description\":\"Create HUD Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"get_game_framework_info\",\"description\":\"Get current game framework configuration\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");

	// === Build / Validation Tools ===
	J += TEXT("{\"name\":\"get_project_info\",\"description\":\"Get project name, engine version, paths\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"list_project_modules\",\"description\":\"List loaded project modules\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"validate_assets\",\"description\":\"Run basic asset validation\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"get_map_check_errors\",\"description\":\"Get map check errors\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"get_build_configuration\",\"description\":\"Get current build config and platform\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");

	// === Resources ===
	J += TEXT("{\"name\":\"list_resources\",\"description\":\"List available MCP resources (read-only context data)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"read_resource\",\"description\":\"Read a resource by URI. URIs: nwiro://project/info, nwiro://level/current, nwiro://editor/selection, nwiro://editor/performance, nwiro://editor/log, nwiro://editor/viewport\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"uri\":{\"type\":\"string\"}},\"required\":[\"uri\"]}},");

	// === Level Tools ===
	J += TEXT("{\"name\":\"new_level\",\"description\":\"Create a new empty level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"template\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"open_level\",\"description\":\"Open an existing level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"save_level\",\"description\":\"Save current level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"get_level_info\",\"description\":\"Get level metadata: name, actors, streaming levels, bounds\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");

	// === Macro Tools ===
	J += TEXT("{\"name\":\"create_basic_level\",\"description\":\"Create basic level with floor, sun, skylight, player start\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"create_light_rig\",\"description\":\"Create 3-point light rig: key, fill, rim + skylight\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"radius\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"create_grid_layout\",\"description\":\"Arrange meshes in a grid pattern\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"mesh\":{\"type\":\"string\"},\"rows\":{\"type\":\"number\"},\"cols\":{\"type\":\"number\"},\"spacing\":{\"type\":\"number\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"create_ring_layout\",\"description\":\"Arrange meshes in a circle/ring\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"mesh\":{\"type\":\"string\"},\"count\":{\"type\":\"number\"},\"radius\":{\"type\":\"number\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"faceCenter\":{\"type\":\"boolean\"}}}},");

	// === Landscape Tools ===
	J += TEXT("{\"name\":\"create_landscape\",\"description\":\"Create a landscape from an EXPLICIT component layout. Optionally import a 16-bit grayscale heightmap PNG as its geometry (heightmapPath); omit for flat. modify=true re-imports onto an existing landscape. ensureLayersOnly=true (with landscapeName) does NOT create/import — it only ensures the type-correct Patches edit layer exists on that landscape (M2 existing-flat base). Layout is explicit: sectionSize (7/15/31/63/127/255) + numSubsections (1 or 2) + componentCount; resolution = componentCount*numSubsections*sectionSize+1. scale defaults [100,100,100], location defaults to origin. (The caller/backend computes the layout, scale and centered location.)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"heightmapPath\":{\"type\":\"string\"},\"sectionSize\":{\"type\":\"number\"},\"numSubsections\":{\"type\":\"number\"},\"componentCount\":{\"type\":\"number\"},\"scale\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},\"location\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},\"material\":{\"type\":\"string\"},\"editLayers\":{\"type\":\"boolean\"},\"ensureLayersOnly\":{\"type\":\"boolean\"},\"modify\":{\"type\":\"boolean\"},\"landscapeName\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"set_landscape_material\",\"description\":\"Set material on landscape\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"material\":{\"type\":\"string\"}},\"required\":[\"material\"]}},");
	J += TEXT("{\"name\":\"get_landscape_info\",\"description\":\"Get landscape details. Optional landscapeName targets a specific landscape by label; omitted returns the first landscape in the level.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"landscapeName\":{\"type\":\"string\"}}}},");

	// === Foliage Tools ===
	J += TEXT("{\"name\":\"add_foliage_type\",\"description\":\"Register mesh as foliage type\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"mesh\":{\"type\":\"string\"}},\"required\":[\"mesh\"]}},");
	J += TEXT("{\"name\":\"paint_foliage\",\"description\":\"Paint foliage instances\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"erase_foliage\",\"description\":\"Erase foliage instances\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"get_foliage_stats\",\"description\":\"Get foliage instance counts per type\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");

	// === Networking Tools ===
	J += TEXT("{\"name\":\"get_replication_info\",\"description\":\"Get actor replication settings\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"set_replication_settings\",\"description\":\"Configure actor replication\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"replicates\":{\"type\":\"boolean\"},\"replicateMovement\":{\"type\":\"boolean\"},\"netUpdateFrequency\":{\"type\":\"number\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"set_net_dormancy\",\"description\":\"Set network dormancy mode: Awake, DormantAll, DormantPartial, Initial, Never\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"mode\":{\"type\":\"string\"}},\"required\":[\"actor\",\"mode\"]}},");

	// === World Partition Tools ===
	J += TEXT("{\"name\":\"get_world_partition_info\",\"description\":\"Get World Partition status\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"load_world_partition_region\",\"description\":\"Load WP editor cells in region\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"minX\":{\"type\":\"number\"},\"minY\":{\"type\":\"number\"},\"minZ\":{\"type\":\"number\"},\"maxX\":{\"type\":\"number\"},\"maxY\":{\"type\":\"number\"},\"maxZ\":{\"type\":\"number\"}}}},");

	// === Undo/Redo ===
	J += TEXT("{\"name\":\"undo\",\"description\":\"Undo editor operations (max 50)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"redo\",\"description\":\"Redo editor operations (max 50)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"number\"}}}},");

	// === GAS Tools ===
	J += TEXT("{\"name\":\"create_gameplay_ability\",\"description\":\"Create Gameplay Ability Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"create_gameplay_effect\",\"description\":\"Create Gameplay Effect Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"create_attribute_set\",\"description\":\"Create Attribute Set Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"list_gameplay_abilities\",\"description\":\"List Gameplay Ability assets\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"list_gameplay_effects\",\"description\":\"List Gameplay Effect assets\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"list_attribute_sets\",\"description\":\"List Attribute Set assets\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"get_gas_info\",\"description\":\"Get GAS configuration on actor or asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"}},\"required\":[\"actor\"]}},");

	// === PCG Tools (default — Extended PCG extension uses its own pipeline) ===
	J += TEXT("{\"name\":\"create_pcg_graph\",\"description\":\"Create an empty UPCGGraph asset. Step 1 of placing procedural content.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Asset name (no extension)\"},\"path\":{\"type\":\"string\",\"description\":\"Folder, default /Game/PCG\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"find_pcg_graphs\",\"description\":\"List existing PCG graph assets in the project.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"spawn_pcg_volume\",\"description\":\"Spawn a PCGVolume actor in the current level pointing at a UPCGGraph asset and trigger Generate(). Step 2 after create_pcg_graph.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"graph\":{\"type\":\"string\",\"description\":\"Full UPCGGraph asset path, e.g. /Game/PCG/Forest\"},\"label\":{\"type\":\"string\",\"description\":\"Actor label, default PCG_Volume\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"scaleX\":{\"type\":\"number\",\"description\":\"Default 20 (=2000cm)\"},\"scaleY\":{\"type\":\"number\"},\"scaleZ\":{\"type\":\"number\",\"description\":\"Default 5 (=500cm)\"}},\"required\":[\"graph\"]}},");
	J += TEXT("{\"name\":\"pcg_generate\",\"description\":\"Force a PCGVolume in the level to re-run its graph.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\",\"description\":\"Label of the PCGVolume actor\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"add_pcg_node\",\"description\":\"Add a node of the given UPCGSettings subclass to a UPCGGraph. Friendly names like StaticMeshSpawner / SurfaceSampler / SelfPruning / CopyPoints / DensityFilter are resolved via fuzzy class lookup. Use this to populate a graph before pcg_generate.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"graph\":{\"type\":\"string\",\"description\":\"Full UPCGGraph asset path, e.g. /Game/PCG/Forest\"},\"node\":{\"type\":\"string\",\"description\":\"Settings class friendly name, e.g. StaticMeshSpawner\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"}},\"required\":[\"graph\",\"node\"]}},");

	// === PIE Runtime Control ===
	J += TEXT("{\"name\":\"pie_teleport_actor\",\"description\":\"Teleport actor during PIE gameplay\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"required\":[\"actor\",\"x\",\"y\",\"z\"]}},");
	J += TEXT("{\"name\":\"pie_spawn_actor\",\"description\":\"Spawn actor in PIE world\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"class\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"pie_destroy_actor\",\"description\":\"Destroy actor in PIE world\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"pie_get_property\",\"description\":\"Read actor property during PIE\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"property\":{\"type\":\"string\"}},\"required\":[\"actor\",\"property\"]}},");
	J += TEXT("{\"name\":\"pie_set_property\",\"description\":\"Set actor property during PIE\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"property\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"actor\",\"property\",\"value\"]}},");
	J += TEXT("{\"name\":\"pie_set_blackboard_key\",\"description\":\"Set AI Blackboard key during PIE. Types: bool,int,float,string,vector,object\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"key\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"},\"value\":{},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"required\":[\"actor\",\"key\"]}},");
	J += TEXT("{\"name\":\"pie_get_blackboard_key\",\"description\":\"Read AI Blackboard key during PIE\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"key\":{\"type\":\"string\"}},\"required\":[\"actor\",\"key\"]}},");
	J += TEXT("{\"name\":\"pie_move_ai_to\",\"description\":\"Command AI to move to location during PIE\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"acceptanceRadius\":{\"type\":\"number\"}},\"required\":[\"actor\",\"x\",\"y\",\"z\"]}},");
	J += TEXT("{\"name\":\"pie_stop_ai\",\"description\":\"Stop AI movement and behavior tree during PIE\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"pie_get_game_state\",\"description\":\"Get PIE game state: time, player position, actor/pawn count\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"pie_list_actors\",\"description\":\"List actors in PIE world with class filter\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"classFilter\":{\"type\":\"string\"},\"limit\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"pie_console_command\",\"description\":\"Execute console command in PIE world\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}},");

	// === Blueprint Debugger ===
	J += TEXT("{\"name\":\"bp_get_compile_errors\",\"description\":\"Get Blueprint compile errors and warnings with node/graph info\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"bp_set_breakpoint\",\"description\":\"Set breakpoint on a Blueprint node\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"node\":{\"type\":\"string\"}},\"required\":[\"blueprint\",\"node\"]}},");
	J += TEXT("{\"name\":\"bp_remove_breakpoint\",\"description\":\"Remove breakpoint from a Blueprint node\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"node\":{\"type\":\"string\"}},\"required\":[\"blueprint\",\"node\"]}},");
	J += TEXT("{\"name\":\"bp_list_breakpoints\",\"description\":\"List all breakpoints in a Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"bp_get_watch_values\",\"description\":\"Get watched pin values in Blueprint (debug)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"bp_add_watch\",\"description\":\"Add watch on a Blueprint node pin\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"node\":{\"type\":\"string\"},\"pin\":{\"type\":\"string\"}},\"required\":[\"blueprint\",\"node\",\"pin\"]}},");

	// === Blueprint Error Fixer ===
	J += TEXT("{\"name\":\"bp_fix_broken_references\",\"description\":\"Find and remove broken pin connections in Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"bp_fix_deprecated_nodes\",\"description\":\"Refresh deprecated nodes to newer versions\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"bp_refresh_all_nodes\",\"description\":\"Refresh all nodes and recompile Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"bp_find_unconnected_pins\",\"description\":\"Find output exec pins with no connections (logic flow gaps)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");

	// === Asset Dependency ===
	J += TEXT("{\"name\":\"get_asset_references\",\"description\":\"Get all assets this asset depends on\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"get_asset_referencers\",\"description\":\"Get all assets that reference this asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"find_orphan_assets\",\"description\":\"Find assets not referenced by anything\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"maxResults\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"find_circular_dependencies\",\"description\":\"Detect circular dependency chains from an asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"get_dependency_tree\",\"description\":\"Get full dependency tree as recursive JSON\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"depth\":{\"type\":\"number\"}},\"required\":[\"path\"]}},");

	// === 3D Generation Tools ===
	J += TEXT("{\"name\":\"generate_3d_model_meshy\",\"description\":\"Generate a 3D model from a text description using Meshy AI. Requires 'Meshy 3D' extension enabled for this chat.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"Detailed description of the 3D model to generate\"}},\"required\":[\"prompt\"]}},");
	J += TEXT("{\"name\":\"generate_texture_meshy\",\"description\":\"Generate textures for an existing 3D model using Meshy AI. Requires 'Meshy 3D' extension enabled. Provide the model_url (GLB/FBX/OBJ URL) or model_id (Meshy task ID) and a text prompt describing the desired texture.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"model_url\":{\"type\":\"string\",\"description\":\"URL to a 3D model file (GLB/FBX/OBJ)\"},\"model_id\":{\"type\":\"string\",\"description\":\"Meshy task ID from a previous generation\"},\"prompt\":{\"type\":\"string\",\"description\":\"Description of the desired texture\"}},\"required\":[\"prompt\"]}},");
	J += TEXT("{\"name\":\"generate_3d_model_tripo\",\"description\":\"Generate a 3D model from a text description using Tripo3D. Requires 'Tripo 3D' extension enabled for this chat.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"Detailed description of the 3D model to generate\"}},\"required\":[\"prompt\"]}},");

	// === ElevenLabs Audio Tools ===
	J += TEXT("{\"name\":\"generate_voice_elevenlabs\",\"description\":\"Generate text-to-speech audio (dialogue, NPC lines, narration) using ElevenLabs. Requires 'ElevenLabs' extension enabled. The audio is saved to the project's Saved/Nwiro/audio folder and emitted as an elevenlabs_task event so the chat panel can preview/import it.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Text to convert to speech\"},\"voiceId\":{\"type\":\"string\",\"description\":\"ElevenLabs voice ID (use list_voices_elevenlabs to discover)\"},\"modelId\":{\"type\":\"string\",\"description\":\"Optional. Defaults to eleven_multilingual_v2\"},\"languageCode\":{\"type\":\"string\",\"description\":\"Optional ISO language code (e.g. en, tr)\"}},\"required\":[\"text\",\"voiceId\"]}},");
	J += TEXT("{\"name\":\"generate_sfx_elevenlabs\",\"description\":\"Generate a sound effect from a text description using ElevenLabs sound-generation. Requires 'ElevenLabs' extension enabled. The audio is saved to the project's Saved/Nwiro/audio folder.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"Description of the sound (e.g. 'footsteps on snow', 'sword clash on metal shield')\"},\"durationSeconds\":{\"type\":\"number\",\"description\":\"Optional. 0.5-30, omit for auto.\"},\"promptInfluence\":{\"type\":\"number\",\"description\":\"Optional. 0-1, default 0.3\"},\"loop\":{\"type\":\"boolean\",\"description\":\"Optional. Generate a seamlessly looping clip.\"}},\"required\":[\"prompt\"]}},");
	J += TEXT("{\"name\":\"generate_music_elevenlabs\",\"description\":\"Generate music or longer-form audio (3s-10min) from a text description using ElevenLabs Music. Requires 'ElevenLabs' extension enabled.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"Description of the music to generate\"},\"durationMs\":{\"type\":\"integer\",\"description\":\"Optional. 3000-600000 milliseconds.\"},\"instrumental\":{\"type\":\"boolean\",\"description\":\"Optional. Force instrumental (no vocals).\"}},\"required\":[\"prompt\"]}},");
	J += TEXT("{\"name\":\"list_voices_elevenlabs\",\"description\":\"List ElevenLabs voices available to the user (premade + cloned + community). Returns array of {voice_id, name, category, labels, preview_url}. Requires 'ElevenLabs' extension enabled.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"search\":{\"type\":\"string\",\"description\":\"Optional search query (matches name, description, labels)\"},\"category\":{\"type\":\"string\",\"description\":\"Optional. premade | cloned | generated | professional\"},\"pageSize\":{\"type\":\"integer\",\"description\":\"Optional. 1-100, default 10\"}},\"required\":[]}},");

	// === fal.ai Material Tools ===
	J += TEXT("{\"name\":\"generate_material_fal\",\"description\":\"Generate a seamless tileable PBR material (basecolor, normal, roughness, metalness, height) from a text prompt or reference image using fal-ai/patina/material. Requires 'fal.ai' extension enabled. The resulting maps appear in the fal.ai panel where the user can preview and import them as UTexture2D assets.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"Material description (e.g. 'weathered copper roof', 'mossy stone wall')\"},\"imageUrl\":{\"type\":\"string\",\"description\":\"Optional reference image URL for image-to-material\"},\"strength\":{\"type\":\"number\",\"description\":\"Optional 0-1, default 0.6. Image-to-image override strength.\"},\"numInferenceSteps\":{\"type\":\"integer\",\"description\":\"Optional 1-8, default 8\"},\"numImages\":{\"type\":\"integer\",\"description\":\"Optional 1-4, default 1\"},\"seed\":{\"type\":\"integer\",\"description\":\"Optional. Reproducibility.\"},\"maps\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional subset of [basecolor, normal, roughness, metalness, height]\"},\"upscaleFactor\":{\"type\":\"integer\",\"description\":\"Optional 0|2|4, default 0\"}},\"required\":[\"prompt\"]}},");

	// Third-party tool sources — append their namespaced defs before the array
	// closes. Agents get the enabled set; the settings UI gets everything with
	// per-def disabled flags so a disabled source keeps its re-enable toggle.
	if (bForSettings)
	{
		FNwiroIKToolRegistry::AppendAllProviderDefsWithState(J);
	}
	else
	{
		FNwiroIKToolRegistry::AppendEnabledProviderDefs(J);
	}

	// Strip the trailing comma each entry leaves behind so the array closes cleanly.
	if (J.EndsWith(TEXT(",")))
	{
		J.LeftChopInline(1);
	}
	J += TEXT("]");
	return J;
}

FString FNwiroIKMCPServer::GetEnabledToolDefinitionsJson()
{
	return BuildToolDefinitionsJson(/*bForSettings=*/false);
}

FString FNwiroIKMCPServer::GetToolDefinitionsForSettingsJson()
{
	return BuildToolDefinitionsJson(/*bForSettings=*/true);
}

// ============================================================
// Start / Stop
// ============================================================

void FNwiroIKMCPServer::Start(int32 Port)
{
	if (bRunning) return;

	// Remember the requested (base) port so an auto-rebind can target it even
	// though Stop() zeroes BoundPort.
	RequestedPort = Port;

	FHttpServerModule& HttpModule = FHttpServerModule::Get();

	// Try binding to port, fallback if occupied
	for (int32 i = 0; i < 10; ++i)
	{
		int32 TryPort = Port + i;
		HttpRouter = HttpModule.GetHttpRouter(TryPort, true);
		if (HttpRouter.IsValid())
		{
			BoundPort = TryPort;
			break;
		}
	}

	if (!HttpRouter.IsValid())
	{
		UE_LOG(LogNwiroMCP, Error, TEXT("Failed to bind HTTP server on ports %d-%d"), Port, Port + 9);
		return;
	}

	// Register routes
	HttpRouter->BindRoute(FHttpPath(TEXT("/mcp")), EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateStatic(&FNwiroIKMCPServer::HandleMCPPost));

	HttpRouter->BindRoute(FHttpPath(TEXT("/mcp")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateStatic(&FNwiroIKMCPServer::HandleMCPGet));

	HttpRouter->BindRoute(FHttpPath(TEXT("/mcp")), EHttpServerRequestVerbs::VERB_OPTIONS,
		FHttpRequestHandler::CreateStatic(&FNwiroIKMCPServer::HandleMCPOptions));

	HttpRouter->BindRoute(FHttpPath(TEXT("/mcp")), EHttpServerRequestVerbs::VERB_DELETE,
		FHttpRequestHandler::CreateStatic(&FNwiroIKMCPServer::HandleMCPDelete));

	HttpModule.StartAllListeners();

	bRunning = true;

	// Auto-write Claude config
	WriteClaudeConfig();
}

void FNwiroIKMCPServer::Stop()
{
	if (!bRunning) return;

	// Drain parked tool-call callbacks so clients don't hang to their OWN timeout
	// (e.g. codex tool_timeout_sec=120) when the server is torn down mid-permission.
	// Fire BEFORE HttpRouter.Reset() (HTTP module must still be alive). Snapshot
	// under the lock, fire AFTER unlocking — mirrors RespondToToolPermission's
	// discipline (dispatch may use FEvent and must not run under the lock). Zero
	// behavioral change when PendingToolCalls is empty (the normal case).
	TArray<FPendingToolCall> Drained;
	{
		FScopeLock Lock(&PendingToolCallsLock);
		Drained = MoveTemp(PendingToolCalls);
		PendingToolCalls.Reset();
	}
	for (FPendingToolCall& P : Drained)
	{
		const FString Body = MakeJsonRpcError(P.JsonRpcId, -32000, TEXT("MCP server restarting; please retry"));
		P.Callback(MakeJsonResponse(503, Body));
	}

	if (HttpRouter.IsValid())
	{
		HttpRouter.Reset();
	}

	bRunning = false;
	BoundPort = 0;
}

bool FNwiroIKMCPServer::IsRunning()
{
	return bRunning;
}

int32 FNwiroIKMCPServer::GetPort()
{
	return BoundPort;
}

bool FNwiroIKMCPServer::IsHealthy()
{
	// v1 liveness: running flag + router still valid. (A silently-dead OS listener
	// behind a still-valid router object is NOT caught here — a real TCP self-probe
	// is deferred to a later increment.)
	return bRunning && HttpRouter.IsValid();
}

bool FNwiroIKMCPServer::Restart()
{
	const int32 Port = (RequestedPort > 0) ? RequestedPort : 5353;
	Stop();      // drain-safe: fails parked tool-call callbacks, then resets router
	Start(Port); // re-binds POST/GET/OPTIONS/DELETE + rewrites .mcp.json/config.toml
	return IsHealthy();
}

// ============================================================
// Claude Config — auto-write .claude/settings.json
// ============================================================

void FNwiroIKMCPServer::WriteClaudeConfig()
{
	FString McpUrl = FString::Printf(TEXT("http://localhost:%d/mcp"), BoundPort);

	// Merge our entry into .mcp.json instead of overwriting — other plugins
	// (or the user) may already have servers registered there.
	FString McpJsonPath = FPaths::Combine(FPaths::ProjectDir(), TEXT(".mcp.json"));
	TSharedPtr<FJsonObject> McpRoot;
	{
		FString Existing;
		if (FFileHelper::LoadFileToString(Existing, *McpJsonPath))
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Existing);
			FJsonSerializer::Deserialize(Reader, McpRoot);
		}
		if (!McpRoot.IsValid()) McpRoot = MakeShareable(new FJsonObject);
	}
	const TSharedPtr<FJsonObject>* ServersPtr = nullptr;
	TSharedPtr<FJsonObject> Servers;
	if (McpRoot->TryGetObjectField(TEXT("mcpServers"), ServersPtr) && ServersPtr && (*ServersPtr).IsValid())
		Servers = *ServersPtr;
	else
		Servers = MakeShareable(new FJsonObject);
	TSharedPtr<FJsonObject> Entry = MakeShareable(new FJsonObject);
	// Claude Code valid transport types are "stdio" | "http" | "sse". "url" is rejected
	// → Claude Code silently reports "MCP tools not available in session."
	Entry->SetStringField(TEXT("type"), TEXT("http"));
	Entry->SetStringField(TEXT("url"), McpUrl);
	// No auth header — MCP server is loopback-only (127.0.0.1), the bind address
	// is the trust boundary. Matches VS Code LSP / Docker daemon pattern.
	Servers->SetObjectField(TEXT("nwiro"), Entry);
	McpRoot->SetObjectField(TEXT("mcpServers"), Servers);
	FString McpJson;
	TSharedRef<TJsonWriter<>> McpWriter = TJsonWriterFactory<>::Create(&McpJson);
	FJsonSerializer::Serialize(McpRoot.ToSharedRef(), McpWriter);
	if (!SaferReplaceWriteString(McpJson, McpJsonPath))
	{
		UE_LOG(LogNwiroMCP, Warning, TEXT("Failed to write .mcp.json."));
	}

	// Ensure project has a .git dir (Claude Code needs it to find .mcp.json)
	FString GitDir = FPaths::Combine(FPaths::ProjectDir(), TEXT(".git"));
	if (!FPaths::DirectoryExists(GitDir))
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.CreateDirectory(*GitDir);
		// Write minimal HEAD file so git recognizes it
		FString HeadPath = FPaths::Combine(GitDir, TEXT("HEAD"));
		FFileHelper::SaveStringToFile(TEXT("ref: refs/heads/main\n"), *HeadPath);
	}

	// CLAUDE.md is intentionally NOT written. Claude Code can discover every
	// available tool via the MCP `tools/list` call against the .mcp.json server
	// entry above — a static markdown copy would only go stale.

	// Write Codex MCP config (~/.codex/config.toml — global)
	FString UserHome;
#if PLATFORM_WINDOWS
	UserHome = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
#else
	UserHome = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
#endif
	FString CodexConfigDir = FPaths::Combine(UserHome, TEXT(".codex"));
	IPlatformFile& PlatformFile2 = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile2.CreateDirectoryTree(*CodexConfigDir);
	FString CodexConfigPath = FPaths::Combine(CodexConfigDir, TEXT("config.toml"));

	// Read existing config, strip any prior [mcp_servers.nwiro] block, then append a fresh
	// canonical block (port may have changed between Starts). Line-based stripping is robust
	// against adjacent sections like [mcp_servers.nwiro_old] or comments containing '[' —
	// a raw string find/slice is not.
	FString ExistingConfig;
	FFileHelper::LoadFileToString(ExistingConfig, *CodexConfigPath);

	TArray<FString> Lines;
	ExistingConfig.ParseIntoArrayLines(Lines, /*InCullEmpty=*/ false);

	FString Rebuilt;
	bool bInNwiroSection = false;
	for (const FString& Line : Lines)
	{
		const FString Trimmed = Line.TrimStartAndEnd();
		const bool bIsSectionHeader = Trimmed.StartsWith(TEXT("[")) && Trimmed.EndsWith(TEXT("]"));
		if (bIsSectionHeader)
		{
			bInNwiroSection = Trimmed.Equals(TEXT("[mcp_servers.nwiro]"));
			if (bInNwiroSection) continue;
		}
		if (!bInNwiroSection) Rebuilt += Line + LINE_TERMINATOR;
	}

	// No bearer header — MCP server is loopback-only.
	Rebuilt += FString::Printf(
		TEXT("\n[mcp_servers.nwiro]\n")
		TEXT("url = \"http://127.0.0.1:%d/mcp\"\n")
		TEXT("tool_timeout_sec = 120\n"),
		BoundPort);

	if (!SaferReplaceWriteString(Rebuilt, CodexConfigPath))
	{
		UE_LOG(LogNwiroMCP, Warning, TEXT("Failed to write Codex config; external MCP access may be stale."));
		// Do not fail Start() — Claude Code path may still work via .mcp.json.
	}
}

// ============================================================
// HTTP Handlers
// ============================================================

TUniquePtr<FHttpServerResponse> FNwiroIKMCPServer::MakeJsonResponse(int32 Code, const FString& Body)
{
	auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
	// No CORS headers: MCP clients are CLIs that never send Origin, so CORS is irrelevant
	// for them, and permitting `Access-Control-Allow-Origin: *` would let any website the
	// user visits drive the editor. Loopback-only binding is the trust boundary.
	Response->Code = static_cast<EHttpServerResponseCodes>(Code);
	return Response;
}

bool FNwiroIKMCPServer::HandleMCPOptions(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	OnComplete(MakeJsonResponse(200, TEXT("{}")));
	return true;
}

// codex-rmcp-compat: Codex's Streamable-HTTP client issues DELETE /mcp at session
// teardown to release the MCP session. The router had no DELETE handler, so UE
// returned a 404 that codex logged as "fail to delete session: ... HTTP 404".
// Reply 200 application/json so teardown is clean. Session state is ephemeral
// (a single static SessionId), so there is nothing to free server-side.
bool FNwiroIKMCPServer::HandleMCPDelete(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	OnComplete(MakeJsonResponse(200, TEXT("{}")));
	return true;
}

bool FNwiroIKMCPServer::HandleMCPGet(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// We only support POST (Streamable HTTP). Return 405 to stop SSE reconnect loops.
	auto Response = MakeJsonResponse(405, TEXT("{\"error\":\"Use POST\"}"));
	OnComplete(MoveTemp(Response));
	return true;
}

bool FNwiroIKMCPServer::HandleMCPPost(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// No bearer auth — loopback-only HTTP binding is the trust boundary.

	// Parse request body
	FString BodyStr;
	if (Request.Body.Num() > 0)
	{
		FUTF8ToTCHAR Converter((const ANSICHAR*)Request.Body.GetData(), Request.Body.Num());
		BodyStr = FString(Converter.Length(), Converter.Get());
	}

	if (BodyStr.IsEmpty())
	{
		OnComplete(MakeJsonResponse(400, MakeJsonRpcError(TEXT("null"), -32700, TEXT("Empty request body"))));
		return true;
	}


	TSharedPtr<FJsonObject> JsonRequest;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyStr);
	if (!FJsonSerializer::Deserialize(Reader, JsonRequest) || !JsonRequest.IsValid())
	{
		OnComplete(MakeJsonResponse(400, MakeJsonRpcError(TEXT("null"), -32700, TEXT("Invalid JSON"))));
		return true;
	}

	FString Method = JsonRequest->HasField(TEXT("method")) ? JsonRequest->GetStringField(TEXT("method")) : TEXT("");

	// Notifications (no "id" field) — return 202 Accepted per Streamable HTTP spec
	if (!JsonRequest->HasField(TEXT("id")))
	{
		// Explicit FString to disambiguate FString vs FUtf8String overloads (UE 5.7+).
		auto Response = FHttpServerResponse::Create(FString(), TEXT("text/plain"));
		Response->Code = static_cast<EHttpServerResponseCodes>(202);
		OnComplete(MoveTemp(Response));
		return true;
	}

	// ─── Managed-tier protected rails ────────────────────────────────
	// Rails outrank every mode and every allow layer. They CANNOT be
	// bypassed by bypassPermissions or any session-allow flag — that's
	// the entire point of the managed tier. Runs before bBypass is even
	// computed so the bypass logic below never sees rail-blocked calls.
#if NWIRO_PROTECTED_RAILS_ENFORCED
	if (Method == TEXT("tools/call"))
	{
		const TSharedPtr<FJsonObject>* RailsParamsPtr = nullptr;
		if (JsonRequest->TryGetObjectField(TEXT("params"), RailsParamsPtr))
		{
			TSharedPtr<FJsonObject> RailsParams = *RailsParamsPtr;
			if (RailsParams.IsValid())
			{
				FString RailsToolName;
				if (RailsParams->TryGetStringField(TEXT("name"), RailsToolName))
				{
					TSharedPtr<FJsonObject> RailsArgs;
					const TSharedPtr<FJsonObject>* RailsArgsPtr = nullptr;
					if (RailsParams->TryGetObjectField(TEXT("arguments"), RailsArgsPtr))
					{
						RailsArgs = *RailsArgsPtr;
					}
					const FString RailsDeny = NwiroIKProtectedRails::Check(RailsToolName, RailsArgs);
					if (!RailsDeny.IsEmpty())
					{
						// Extract JSON-RPC id for the error response (mirrors the
						// extraction pattern used in the permission-prompt branch
						// below so error shape stays consistent).
						FString RailsRpcId = TEXT("null");
						if (JsonRequest->HasField(TEXT("id")))
						{
							TSharedPtr<FJsonValue> IdVal = JsonRequest->TryGetField(TEXT("id"));
							if (IdVal.IsValid())
							{
								if (IdVal->Type == EJson::Number)
									RailsRpcId = FString::Printf(TEXT("%d"), (int32)IdVal->AsNumber());
								else if (IdVal->Type == EJson::String)
									RailsRpcId = TEXT("\"") + IdVal->AsString() + TEXT("\"");
							}
						}
						OnComplete(MakeJsonResponse(200, MakeJsonRpcError(RailsRpcId, -32000, RailsDeny)));
						return true;
					}
				}
			}
		}
	}
#endif

	// Check if this is a tools/call — gate through permission system
	// Skip permission gate if: session already allowed, bypass mode, or ACP adapters are active
	// (ACP adapters have their own permission system — double-gating causes tool calls to hang)
	bool bBypass = bSessionAllowed;
	if (UNwiroIKBridge::Instance)
	{
		FString Mode = UNwiroIKBridge::Instance->GetMode();
		if (Mode == TEXT("bypassPermissions") || Mode == TEXT("dontAsk"))
			bBypass = true;
		// ACP adapters handle permissions themselves — bypass MCP permission gate
		if (!UNwiroIKBridge::Instance->GetAdapter().IsEmpty())
			bBypass = true;
	}
	else
	{
		// Headless policy: no Bridge means no UI can prompt. Access is gated by the loopback
		// binding (only local processes can connect), and Protected Rails + Path Sandbox
		// remain hard limits. This is delegated access, not per-call approval.
		bBypass = true;
	}
	// Client-provided _meta.bypassPermissions is intentionally NOT honored. The bypass
	// decision is server-side only; trusting a client flag here would hand it the keys.
	if (Method == TEXT("tools/call") && !bBypass)
	{
		// Extract tool name and args for permission request
		TSharedPtr<FJsonObject> CallParams;
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		if (JsonRequest->TryGetObjectField(TEXT("params"), ParamsPtr)) CallParams = *ParamsPtr;

		FString ToolName = CallParams.IsValid() && CallParams->HasField(TEXT("name"))
			? CallParams->GetStringField(TEXT("name")) : TEXT("unknown");

		FString JsonRpcId = TEXT("null");
		if (JsonRequest->HasField(TEXT("id")))
		{
			TSharedPtr<FJsonValue> IdVal = JsonRequest->TryGetField(TEXT("id"));
			if (IdVal.IsValid())
			{
				if (IdVal->Type == EJson::Number) JsonRpcId = FString::Printf(TEXT("%d"), (int32)IdVal->AsNumber());
				else if (IdVal->Type == EJson::String) JsonRpcId = TEXT("\"") + IdVal->AsString() + TEXT("\"");
			}
		}

		FString ArgsJson;
		if (CallParams.IsValid() && CallParams->HasField(TEXT("arguments")))
		{
			TSharedPtr<FJsonObject> Args = CallParams->GetObjectField(TEXT("arguments"));
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> AW =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ArgsJson);
			FJsonSerializer::Serialize(Args.ToSharedRef(), AW);
		}

		// Extract toolUseId from _meta for chat routing
		FString ToolUseId;
		if (CallParams.IsValid() && CallParams->HasField(TEXT("_meta")))
		{
			TSharedPtr<FJsonObject> Meta = CallParams->GetObjectField(TEXT("_meta"));
			if (Meta.IsValid() && Meta->HasField(TEXT("claudecode/toolUseId")))
				ToolUseId = Meta->GetStringField(TEXT("claudecode/toolUseId"));
		}

		// Resolve chatId from toolUseId
		FString PermChatId;
		if (UNwiroIKBridge::Instance && !ToolUseId.IsEmpty())
			PermChatId = UNwiroIKBridge::Instance->GetChatIdForToolUse(ToolUseId);

		// Create pending permission request
		int32 PermId;
		{
			FScopeLock Lock(&PendingToolCallsLock);
			PermId = NextPermissionId++;
			FPendingToolCall Pending;
			Pending.PermissionId = PermId;
			Pending.JsonRpcId = JsonRpcId;
			Pending.ToolName = ToolName;
			Pending.ArgsJson = ArgsJson;
			Pending.Callback = OnComplete;
			PendingToolCalls.Add(MoveTemp(Pending));
		}

		// Send permission request to frontend via Bridge with correct chatId
		if (UNwiroIKBridge::Instance)
		{
			TSharedPtr<FJsonObject> PermJson = MakeShareable(new FJsonObject);
			PermJson->SetNumberField(TEXT("id"), PermId);
			PermJson->SetStringField(TEXT("toolName"), ToolName);

			TArray<TSharedPtr<FJsonValue>> Options;
			TSharedPtr<FJsonObject> AllowOpt = MakeShareable(new FJsonObject);
			AllowOpt->SetStringField(TEXT("optionId"), TEXT("allow"));
			AllowOpt->SetStringField(TEXT("name"), TEXT("Allow"));
			AllowOpt->SetStringField(TEXT("kind"), TEXT("allow_once"));
			Options.Add(MakeShareable(new FJsonValueObject(AllowOpt)));

			TSharedPtr<FJsonObject> DenyOpt = MakeShareable(new FJsonObject);
			DenyOpt->SetStringField(TEXT("optionId"), TEXT("deny"));
			DenyOpt->SetStringField(TEXT("name"), TEXT("Deny"));
			DenyOpt->SetStringField(TEXT("kind"), TEXT("reject_once"));
			Options.Add(MakeShareable(new FJsonValueObject(DenyOpt)));

			PermJson->SetArrayField(TEXT("options"), Options);

			FString PermJsonStr;
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> PW =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PermJsonStr);
			FJsonSerializer::Serialize(PermJson.ToSharedRef(), PW);

			UNwiroIKBridge::Instance->EnqueueResponse(TEXT("permission_request"), PermJsonStr, PermChatId);
		}

		return true; // Response will be sent later via RespondToToolPermission
	}

	// Non-tools/call: process immediately
	TSharedPtr<FJsonObject> Result = ProcessJsonRpc(JsonRequest);

	// Serialize response
	FString ResponseBody;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ResponseBody);
	FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);

	auto Response = MakeJsonResponse(200, ResponseBody);

	// Add MCP headers
	Response->Headers.Add(TEXT("MCP-Protocol-Version"), { TEXT("2025-03-26") });
	if (!SessionId.IsEmpty())
	{
		Response->Headers.Add(TEXT("MCP-Session-Id"), { SessionId });
	}


	OnComplete(MoveTemp(Response));
	return true;
}

// ============================================================
// JSON-RPC Processing
// ============================================================

void FNwiroIKMCPServer::RespondToToolPermission(int32 PermissionId, bool bAllowed)
{
	FScopeLock Lock(&PendingToolCallsLock);

	int32 Idx = PendingToolCalls.IndexOfByPredicate([PermissionId](const FPendingToolCall& P) {
		return P.PermissionId == PermissionId;
	});

	if (Idx == INDEX_NONE)
	{
		UE_LOG(LogNwiroMCP, Warning, TEXT("MCP: Permission response for unknown id=%d"), PermissionId);
		return;
	}

	FPendingToolCall Pending = MoveTemp(PendingToolCalls[Idx]);
	PendingToolCalls.RemoveAt(Idx);

	// Must release lock before calling game-thread dispatch (DispatchTool may use FEvent)
	Lock.Unlock();

	if (bAllowed)
	{
		// Execute the tool on game thread
		FString ToolResult = DispatchTool(Pending.ToolName, Pending.ArgsJson);

		TSharedPtr<FJsonObject> ResultObj = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> ContentArr;
		TSharedPtr<FJsonObject> TextContent = MakeShareable(new FJsonObject);
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		TextContent->SetStringField(TEXT("text"), ToolResult);
		ContentArr.Add(MakeShareable(new FJsonValueObject(TextContent)));
		ResultObj->SetArrayField(TEXT("content"), ContentArr);

		FString ResponseBody = MakeJsonRpcResponse(Pending.JsonRpcId, ResultObj);
		auto Response = MakeJsonResponse(200, ResponseBody);
		Response->Headers.Add(TEXT("MCP-Protocol-Version"), { TEXT("2025-03-26") });
		if (!SessionId.IsEmpty())
			Response->Headers.Add(TEXT("MCP-Session-Id"), { SessionId });

		Pending.Callback(MoveTemp(Response));
	}
	else
	{
		FString ResponseBody = MakeJsonRpcError(Pending.JsonRpcId, -32000, TEXT("User denied permission for this tool call"));
		auto Response = MakeJsonResponse(200, ResponseBody);
		Response->Headers.Add(TEXT("MCP-Protocol-Version"), { TEXT("2025-03-26") });

		Pending.Callback(MoveTemp(Response));
	}
}

TSharedPtr<FJsonObject> FNwiroIKMCPServer::ProcessJsonRpc(const TSharedPtr<FJsonObject>& Request)
{
	FString Method = Request->GetStringField(TEXT("method"));

	// Get ID — preserve original value for echo
	TSharedPtr<FJsonValue> RequestId;
	if (Request->HasField(TEXT("id")))
	{
		RequestId = Request->TryGetField(TEXT("id"));
	}
	if (!RequestId.IsValid())
	{
		RequestId = MakeShareable(new FJsonValueNull());
	}

	// Get params
	const TSharedPtr<FJsonObject>* Params = nullptr;
	Request->TryGetObjectField(TEXT("params"), Params);
	TSharedPtr<FJsonObject> ParamsObj = Params ? *Params : MakeShareable(new FJsonObject);

	// Dispatch
	TSharedPtr<FJsonObject> ResultObj;

	if (Method == TEXT("initialize"))
	{
		ResultObj = HandleInitialize(ParamsObj);
		SessionId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}
	else if (Method == TEXT("tools/list"))
	{
		ResultObj = HandleToolsList(ParamsObj);
	}
	else if (Method == TEXT("tools/call"))
	{
		ResultObj = HandleToolsCall(ParamsObj);
	}
	else if (Method == TEXT("resources/list"))
	{
		ResultObj = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> EmptyArr;
		ResultObj->SetArrayField(TEXT("resources"), EmptyArr);
	}
	else if (Method == TEXT("resources/templates/list"))
	{
		ResultObj = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> EmptyArr;
		ResultObj->SetArrayField(TEXT("resourceTemplates"), EmptyArr);
	}
	else if (Method == TEXT("prompts/list"))
	{
		ResultObj = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> EmptyArr;
		ResultObj->SetArrayField(TEXT("prompts"), EmptyArr);
	}
	else if (Method == TEXT("ping"))
	{
		ResultObj = MakeShareable(new FJsonObject);
	}
	else
	{
		// Unknown method
		TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
		Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));

		TSharedPtr<FJsonObject> Error = MakeShareable(new FJsonObject);
		Error->SetNumberField(TEXT("code"), -32601);
		Error->SetStringField(TEXT("message"), FString::Printf(TEXT("Unknown method: %s"), *Method));
		Response->SetObjectField(TEXT("error"), Error);
		Response->SetField(TEXT("id"), RequestId);
		return Response;
	}

	// Wrap in JSON-RPC response
	TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetField(TEXT("id"), RequestId);
	Response->SetObjectField(TEXT("result"), ResultObj);

	return Response;
}

// ============================================================
// MCP Method Handlers
// ============================================================

TSharedPtr<FJsonObject> FNwiroIKMCPServer::HandleInitialize(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetStringField(TEXT("protocolVersion"), TEXT("2025-03-26"));

	TSharedPtr<FJsonObject> Capabilities = MakeShareable(new FJsonObject);
	TSharedPtr<FJsonObject> ToolsCap = MakeShareable(new FJsonObject);
	ToolsCap->SetBoolField(TEXT("listChanged"), true);
	Capabilities->SetObjectField(TEXT("tools"), ToolsCap);
	Result->SetObjectField(TEXT("capabilities"), Capabilities);

	TSharedPtr<FJsonObject> ServerInfo = MakeShareable(new FJsonObject);
	ServerInfo->SetStringField(TEXT("name"), TEXT("nwiro"));
	ServerInfo->SetStringField(TEXT("version"), TEXT("1.0.0"));
	Result->SetObjectField(TEXT("serverInfo"), ServerInfo);

	return Result;
}

TSharedPtr<FJsonObject> FNwiroIKMCPServer::HandleToolsList(const TSharedPtr<FJsonObject>& Params)
{
	// Agent-facing: enabled tools only — disabled sources must never reach a model.
	FString ToolsJson = GetEnabledToolDefinitionsJson();

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);

	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	TSharedRef<TJsonReader<>> ToolsReader = TJsonReaderFactory<>::Create(ToolsJson);
	if (!FJsonSerializer::Deserialize(ToolsReader, ToolsArray))
	{
		UE_LOG(LogNwiroMCP, Error, TEXT("Failed to deserialize MCP tool definitions"));
		ToolsArray.Reset();
	}
	AddMcpToolAnnotations(ToolsArray);

	Result->SetArrayField(TEXT("tools"), ToolsArray);
	return Result;
}

static constexpr int32 MaxMcpToolUiPayloadChars = 16 * 1024;

static FString TruncateMcpToolUiPayload(const FString& Payload)
{
	if (Payload.Len() <= MaxMcpToolUiPayloadChars) return Payload;

	const FString Suffix = FString::Printf(
		TEXT("...[truncated, %d chars total]"), Payload.Len());
	const int32 PrefixLength = FMath::Max(0, MaxMcpToolUiPayloadChars - Suffix.Len());
	return Payload.Left(PrefixLength) + Suffix;
}

static TSharedPtr<FJsonObject> MakeMcpToolStartEventData(
	const FString& ToolName,
	const FString& ToolCallId,
	const FString& Input)
{
	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject);
	Data->SetStringField(TEXT("name"), ToolName);
	Data->SetStringField(TEXT("id"), ToolCallId);
	Data->SetStringField(TEXT("input"), TruncateMcpToolUiPayload(Input));
	return Data;
}

static TSharedPtr<FJsonObject> MakeMcpToolEndEventData(
	const FString& ToolName,
	const FString& ToolCallId,
	const FString& Output,
	bool bToolOk)
{
	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject);
	Data->SetStringField(TEXT("name"), ToolName);
	Data->SetStringField(TEXT("id"), ToolCallId);
	Data->SetStringField(TEXT("output"), TruncateMcpToolUiPayload(Output));
	Data->SetBoolField(TEXT("ok"), bToolOk);
	return Data;
}

TSharedPtr<FJsonObject> FNwiroIKMCPServer::HandleToolsCall(const TSharedPtr<FJsonObject>& Params)
{
	FString ToolName = Params->GetStringField(TEXT("name"));

	// Get arguments as JSON string
	FString ArgsJson;
	const TSharedPtr<FJsonObject>* ArgsObj;
	if (Params->TryGetObjectField(TEXT("arguments"), ArgsObj))
	{
		// Defense-in-depth: cap any string field whose name suggests it will
		// be wrapped in FName (UE NAME_SIZE limit ~1024 chars, fatal assert
		// on overflow). Catches the common case where a downstream tool
		// forgot its own guard. Fields that legitimately carry large
		// payloads (code, json, text, content, script, csv) stay uncapped.
		// Discovered via fuzz/blueprint-2 (100KB name → UnrealNames.cpp:3206).
		static const TSet<FString> NameLikeFields = {
			TEXT("name"), TEXT("path"), TEXT("assetPath"), TEXT("asset_path"),
			TEXT("blueprint"), TEXT("actor"), TEXT("class"), TEXT("parentClass"),
			TEXT("parent_class"), TEXT("component"), TEXT("variable"), TEXT("function"),
			TEXT("graph"), TEXT("event"), TEXT("property"), TEXT("label"),
			TEXT("ref"), TEXT("subtype"), TEXT("rowName"), TEXT("table"),
			TEXT("dataTable"), TEXT("material"), TEXT("materialPath"), TEXT("mesh"),
			TEXT("skeleton"), TEXT("sound"), TEXT("widget"), TEXT("widgetBlueprint"),
			TEXT("sequence"), TEXT("binding"), TEXT("mappingContext"), TEXT("animation"),
			TEXT("montage"),
		};
		FString BadFieldName;
		FString BadReason;
		for (const auto& Pair : (*ArgsObj)->Values)
		{
			const FString Key(*Pair.Key);
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String) continue;
			if (!NameLikeFields.Contains(Key)) continue;
			FString S; Pair.Value->TryGetString(S);
			if (S.Len() > 1024)
			{
				BadFieldName = Key;
				BadReason = FString::Printf(TEXT("%d chars long; max 1024 for name/path fields (UE FName limit)"), S.Len());
				break;
			}
			// Path-traversal escape sequences in name/path fields can either
			// corrupt the asset registry or hit native filesystem APIs that
			// then crash. Reject `..`, `\\`, leading `/` in non-path fields.
			// Discovered by fuzz/material-2: edit_material{assetPath:"../../etc/passwd"}
			// crashed via CreateMaterial auto-create path.
			if (S.Contains(TEXT("..")) || S.Contains(TEXT("\\")))
			{
				BadFieldName = Pair.Key;
				BadReason = TEXT("contains path-traversal sequence ('..' or '\\\\'). Reject /Game/-relative or absolute paths only.");
				break;
			}
		}
		if (!BadFieldName.IsEmpty())
		{
			TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
			TSharedRef<FJsonObject> TextContent = MakeShareable(new FJsonObject());
			TextContent->SetStringField(TEXT("type"), TEXT("text"));
			TextContent->SetStringField(TEXT("text"), FString::Printf(
				TEXT("{\"success\":false,\"error\":\"Argument '%s' %s. Rejected at MCP layer to prevent server crash.\"}"),
				*BadFieldName, *BadReason));
			TArray<TSharedPtr<FJsonValue>> Content;
			Content.Add(MakeShareable(new FJsonValueObject(TextContent)));
			Result->SetArrayField(TEXT("content"), Content);
			return Result;
		}

		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ArgsJson);
		FJsonSerializer::Serialize(ArgsObj->ToSharedRef(), Writer);
	}
	else
	{
		ArgsJson = TEXT("{}");
	}


	// tool-visibility: announce the tool to the frontend BEFORE dispatch. Codex
	// reaches this MCP server directly and emits NO ACP tool_call, so its tool
	// activity is otherwise invisible AND starves the 90s midstream watchdog of a
	// liveness signal during the (up to 60s) dispatch window -> false stall. The
	// frontend bumps the watchdog on mcp_tool_* unconditionally, and renders a
	// BADGE only for codex (claude/localllm already badge via ACP tool_call, so
	// gating avoids a double badge). A matching mcp_tool_end fires below on EVERY
	// path (success/error/timeout) — the flow from here to that emit is linear
	// (no early return), so one-start-one-end holds by construction.
	static FThreadSafeCounter GMcpToolCallSeq;
	const FString McpToolCallId = FString::Printf(TEXT("%d"), GMcpToolCallSeq.Increment());
	if (UNwiroIKBridge::Instance)
	{
		const TSharedPtr<FJsonObject> StartData =
			MakeMcpToolStartEventData(ToolName, McpToolCallId, ArgsJson);
		FString StartJson;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> SW =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&StartJson);
		FJsonSerializer::Serialize(StartData.ToSharedRef(), SW);
		UNwiroIKBridge::Instance->EnqueueResponse(TEXT("mcp_tool_start"), StartJson);
		// F4 gap-close: mark the live turn "streamed" so a stray mid-turn
		// classified stdout line (codex's recovered "unauthorized") can't tear
		// down a codex TOOL-FIRST turn (one whose only activity so far is this
		// direct-MCP tool call, invisible to the ACP bHasStreamed latch).
		// Marshaled to the game thread — ChatSessions is game-thread-owned — and
		// lands well before the later auth line.
		AsyncTask(ENamedThreads::GameThread, []()
		{
			if (UNwiroIKBridge::Instance) { UNwiroIKBridge::Instance->MarkActiveTurnStreamed(); }
		});
	}

	FString ToolResult;
	if (IsInGameThread())
	{
		ToolResult = DispatchTool(ToolName, ArgsJson);
	}
	else
	{
		// Bounded wait — the prior `DoneEvent->Wait()` (no timeout) wedged the
		// MCP worker thread whenever the game thread was blocked (PIE start /
		// transition, modal dialog, slow asset import). One stuck request held
		// the worker; the next one queued behind it; eventually every worker
		// was stuck and MCP looked "dead" even though UE was alive.
		//
		// TPromise/TFuture keeps the storage alive past the wait timeout, so a
		// late completion writes into the shared state safely instead of
		// touching dangling stack locals.
		const double kDispatchTimeoutSec = 60.0;
		TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise =
			MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
		TFuture<FString> Future = Promise->GetFuture();

		// Copy args by value into the lambda — caller's `ToolName`/`ArgsJson`
		// may unwind before the game thread picks this up.
		FString ToolNameCopy = ToolName;
		FString ArgsJsonCopy = ArgsJson;
		AsyncTask(ENamedThreads::GameThread,
			[Promise, ToolNameCopy = MoveTemp(ToolNameCopy), ArgsJsonCopy = MoveTemp(ArgsJsonCopy)]() mutable
		{
			Promise->SetValue(DispatchTool(ToolNameCopy, ArgsJsonCopy));
		});

		const bool bReady = Future.WaitFor(FTimespan::FromSeconds(kDispatchTimeoutSec));
		if (bReady)
		{
			ToolResult = Future.Get();
		}
		else
		{
			ToolResult = FString::Printf(
				TEXT("{\"success\":false,\"error\":\"Tool dispatch timed out after %.0fs — game thread blocked. Common cause: PIE running heavy logic, modal dialog open, or a prior tool hung. Stop PIE / dismiss dialogs and retry.\"}"),
				kDispatchTimeoutSec);
		}
		// Note: we do NOT block on Future after timeout. If the AsyncTask
		// eventually fires, it writes to the still-alive Promise — the result
		// is dropped, no UB.
	}

	// Notify frontend the tool finished (mcp_tool_end) — matching id completes the
	// codex badge opened on mcp_tool_start, plus a second watchdog liveness bump.
	// Reached on EVERY path incl. the 60s dispatch timeout above (which sets a
	// success:false ToolResult and falls through here) -> one end per start.
	if (UNwiroIKBridge::Instance)
	{
		const bool bToolOk = !ToolResult.Contains(TEXT("\"success\":false"));
		const TSharedPtr<FJsonObject> EndData =
			MakeMcpToolEndEventData(ToolName, McpToolCallId, ToolResult, bToolOk);
		FString EndJson;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> EW =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&EndJson);
		FJsonSerializer::Serialize(EndData.ToSharedRef(), EW);
		UNwiroIKBridge::Instance->EnqueueResponse(TEXT("mcp_tool_end"), EndJson);
	}

	// Build MCP response
	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);

	TArray<TSharedPtr<FJsonValue>> Content;
	TSharedPtr<FJsonObject> TextContent = MakeShareable(new FJsonObject);
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), ToolResult);
	Content.Add(MakeShareable(new FJsonValueObject(TextContent)));

	Result->SetArrayField(TEXT("content"), Content);

	// Nwiro's own handlers use the {"success":bool,...} convention. If the
	// result is a JSON object reporting success:false, surface it to the model
	// as an MCP error so it can recover. Non-JSON or success-absent results are
	// left as-is (isError defaults to false).
	TSharedPtr<FJsonObject> ResultObj;
	TSharedRef<TJsonReader<>> ResultReader = TJsonReaderFactory<>::Create(ToolResult);
	bool bSuccess = true;
	if (FJsonSerializer::Deserialize(ResultReader, ResultObj) && ResultObj.IsValid()
		&& ResultObj->TryGetBoolField(TEXT("success"), bSuccess) && !bSuccess)
	{
		Result->SetBoolField(TEXT("isError"), true);
	}

	return Result;
}

// ============================================================
// Tool Dispatch — calls existing C++ tool implementations
// ============================================================

// Forward declaration: the original 226-branch dispatch body below, renamed
// and made TU-local. FNwiroIKMCPServer::DispatchTool (still the name/signature
// the 3 call sites in this file use) is a thin passthrough to this.
static FString DispatchToolImpl(const FString& ToolName, const FString& ArgsJson);

// Stash a file into a per-operation, collision-proof trash dir under
// <Project>/Saved/Nwiro/Trash/<ts>-<guid>/. The GUID makes every op's dir unique,
// so two files with the same basename can NEVER overwrite each other in trash
// (bReplace=false is a second guard). bMove=true relocates (for delete);
// bMove=false copies (a pre-overwrite backup that leaves the original in place
// until the write succeeds). Returns the trash path, or empty on failure —
// callers MUST treat empty as failure and NOT proceed with the destructive op.
static FString StashFileToTrash(const FString& AbsPath, bool bMove)
{
	const FString TrashDir = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("Nwiro"), TEXT("Trash"),
		FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")) + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!IFileManager::Get().MakeDirectory(*TrashDir, /*Tree=*/true))
		return FString();
	const FString Dest = FPaths::Combine(TrashDir, FPaths::GetCleanFilename(AbsPath));
	const bool bOk = bMove
		? IFileManager::Get().Move(*Dest, *AbsPath, /*bReplace=*/false)
		: (IFileManager::Get().Copy(*Dest, *AbsPath, /*bReplace=*/false) == COPY_OK);
	return bOk ? Dest : FString();
}

FString FNwiroIKMCPServer::DispatchTool(const FString& ToolName, const FString& ArgsJson)
{
	return DispatchToolImpl(ToolName, ArgsJson);
}

static FString DispatchToolImpl(const FString& ToolName, const FString& ArgsJson)
{
	// Helper: extract a string field from JSON args
	auto ExtractField = [&ArgsJson](const FString& FieldName) -> FString
	{
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(R, Parsed) && Parsed.IsValid() && Parsed->HasField(FieldName))
		{
			return Parsed->GetStringField(FieldName);
		}
		return ArgsJson; // fallback to raw string
	};

	// Blueprint tools
	if (ToolName == TEXT("find_blueprints"))
		return FNwiroIKBlueprintTools::FindBlueprints(ExtractField(TEXT("searchTerm")));
	if (ToolName == TEXT("read_blueprint"))
		return FNwiroIKBlueprintTools::ReadBlueprint(ArgsJson);
	if (ToolName == TEXT("edit_blueprint"))
		return FNwiroIKBlueprintTools::EditBlueprint(ArgsJson);
	if (ToolName == TEXT("create_blueprint"))
		return FNwiroIKBlueprintTools::CreateBlueprint(ArgsJson);
	if (ToolName == TEXT("render_blueprint_graph"))
		return FNwiroIKBlueprintTools::RenderBlueprintGraph(ArgsJson);
	if (ToolName == TEXT("organize_blueprint_nodes"))
		return FNwiroIKBlueprintTools::OrganizeBlueprintNodes(ArgsJson);
	if (ToolName == TEXT("find_blueprint_nodes"))
	{
		// Parse the args JSON to extract the actual query string — passing the
		// raw JSON blob as the query (the previous behaviour) meant Contains()
		// never matched anything.
		FString FbnQuery;
		FString FbnBlueprint;
		TSharedPtr<FJsonObject> FbnArgs;
		TSharedRef<TJsonReader<>> FbnReader = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(FbnReader, FbnArgs) && FbnArgs.IsValid())
		{
			FbnArgs->TryGetStringField(TEXT("query"), FbnQuery);
			FbnArgs->TryGetStringField(TEXT("blueprint"), FbnBlueprint);
		}
		return FNwiroIKBlueprintTools::FindBlueprintNodes(FbnQuery, FbnBlueprint);
	}
	if (ToolName == TEXT("clear_graph"))
		return FNwiroIKBlueprintTools::ClearGraph(ArgsJson);
	if (ToolName == TEXT("delete_blueprint"))
		return FNwiroIKBlueprintTools::DeleteBlueprint(ArgsJson);
	if (ToolName == TEXT("duplicate_blueprint"))
		return FNwiroIKBlueprintTools::DuplicateBlueprint(ArgsJson);
	if (ToolName == TEXT("rename_blueprint"))
		return FNwiroIKBlueprintTools::RenameBlueprint(ArgsJson);
	if (ToolName == TEXT("delete_node"))
		return FNwiroIKBlueprintTools::DeleteNode(ArgsJson);
	if (ToolName == TEXT("create_function_graph"))
		return FNwiroIKBlueprintTools::CreateFunctionGraph(ArgsJson);
	if (ToolName == TEXT("add_interface"))
		return FNwiroIKBlueprintTools::AddInterface(ArgsJson);
	if (ToolName == TEXT("remove_interface"))
		return FNwiroIKBlueprintTools::RemoveInterface(ArgsJson);
	if (ToolName == TEXT("create_event_dispatcher"))
		return FNwiroIKBlueprintTools::CreateEventDispatcher(ArgsJson);
	if (ToolName == TEXT("reparent_blueprint"))
		return FNwiroIKBlueprintTools::ReparentBlueprint(ArgsJson);
	if (ToolName == TEXT("remove_component"))
		return FNwiroIKBlueprintTools::RemoveComponent(ArgsJson);
	if (ToolName == TEXT("edit_component"))
		return FNwiroIKBlueprintTools::EditComponent(ArgsJson);

	// Material tools
	if (ToolName == TEXT("find_materials"))
		return FNwiroIKMaterialTools::FindMaterials(ArgsJson);
	if (ToolName == TEXT("inspect_material"))
		return FNwiroIKMaterialTools::InspectMaterial(ArgsJson);
	if (ToolName == TEXT("create_material"))
		return FNwiroIKMaterialTools::CreateMaterial(ArgsJson);
	if (ToolName == TEXT("edit_material"))
		return FNwiroIKMaterialTools::EditMaterial(ArgsJson);
	if (ToolName == TEXT("inspect_material_graph"))
		return FNwiroIKMaterialTools::InspectMaterialGraph(ArgsJson);
	if (ToolName == TEXT("create_material_instance"))
		return FNwiroIKMaterialTools::CreateMaterialInstance(ArgsJson);
	if (ToolName == TEXT("find_textures"))
		return FNwiroIKMaterialTools::FindTextures(ArgsJson);
	if (ToolName == TEXT("apply_material"))
		return FNwiroIKMaterialTools::ApplyMaterial(ArgsJson);
	if (ToolName == TEXT("delete_material"))
		return FNwiroIKMaterialTools::DeleteMaterial(ArgsJson);
	if (ToolName == TEXT("set_material_property"))
		return FNwiroIKMaterialTools::SetMaterialProperty(ArgsJson);
	if (ToolName == TEXT("edit_material_instance"))
		return FNwiroIKMaterialTools::EditMaterialInstance(ArgsJson);
	if (ToolName == TEXT("delete_expression"))
		return FNwiroIKMaterialTools::DeleteExpression(ArgsJson);
	if (ToolName == TEXT("find_material_functions"))
		return FNwiroIKMaterialTools::FindMaterialFunctions(ArgsJson);
	if (ToolName == TEXT("create_material_function"))
		return FNwiroIKMaterialTools::CreateMaterialFunction(ArgsJson);
	if (ToolName == TEXT("inspect_material_function"))
		return FNwiroIKMaterialTools::InspectMaterialFunction(ArgsJson);
	if (ToolName == TEXT("edit_material_function"))
		return FNwiroIKMaterialTools::EditMaterialFunction(ArgsJson);
	if (ToolName == TEXT("delete_material_function"))
		return FNwiroIKMaterialTools::DeleteMaterialFunction(ArgsJson);

	// Settings tools
	if (ToolName == TEXT("get_world_settings"))
		return FNwiroIKSettingsTools::GetWorldSettings();
	if (ToolName == TEXT("set_world_settings"))
		return FNwiroIKSettingsTools::SetWorldSettings(ArgsJson);
	if (ToolName == TEXT("set_game_mode"))
		return FNwiroIKSettingsTools::SetGameMode(ArgsJson);
	if (ToolName == TEXT("get_project_settings"))
		return FNwiroIKSettingsTools::GetProjectSettings(ArgsJson);
	if (ToolName == TEXT("set_project_settings"))
		return FNwiroIKSettingsTools::SetProjectSettings(ArgsJson);
	if (ToolName == TEXT("get_level_actors"))
		return FNwiroIKSettingsTools::GetLevelActors(ArgsJson);
	if (ToolName == TEXT("spawn_actor"))
		return FNwiroIKSettingsTools::SpawnActor(ArgsJson);
	if (ToolName == TEXT("delete_actor"))
		return FNwiroIKSettingsTools::DeleteActor(ArgsJson);
	if (ToolName == TEXT("transform_actor"))
		return FNwiroIKSettingsTools::TransformActor(ArgsJson);
	if (ToolName == TEXT("get_actor_property"))
		return FNwiroIKSettingsTools::GetActorProperty(ArgsJson);
	if (ToolName == TEXT("set_actor_property"))
		return FNwiroIKSettingsTools::SetActorProperty(ArgsJson);
	if (ToolName == TEXT("execute_python"))
		return FNwiroIKSettingsTools::ExecutePython(ArgsJson);
	if (ToolName == TEXT("search_assets"))
	{
		// Use the Bridge instance when available so we share state with the
		// editor browser. If it's null (very early startup), do the search
		// inline so the LLM doesn't see a confusing "Unknown tool" error.
		if (UNwiroIKBridge::Instance) return UNwiroIKBridge::Instance->SearchAssets(ArgsJson);

		// Inline fallback — mirrors UNwiroIKBridge::SearchAssets minus the
		// Bridge dependency. Pure asset-registry read.
		FString SearchTerm;
		TSharedPtr<FJsonObject> JsonObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
		{
			JsonObj->TryGetStringField(TEXT("query"), SearchTerm);
		}
		const bool bShowAll = SearchTerm.IsEmpty() || SearchTerm == TEXT("*");
		const FString QueryLower = SearchTerm.ToLower();

		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		// `false` second arg = include in-memory (unsaved) assets too. Without
		// this, BPs the agent just created via create_blueprint but didn't yet
		// save are invisible — and most of an LLM-driven session's new assets
		// fall into that bucket because save_level only persists levels.
		TArray<FAssetData> AllAssets;
		ARM.Get().GetAllAssets(AllAssets, false);

		FString Out = TEXT("{\"success\":true,\"assets\":[");
		int32 Count = 0;
		bool bFirst = true;
		for (const FAssetData& A : AllAssets)
		{
			if (Count >= 15) break;
			const FString Path = A.GetObjectPathString();
			if (!Path.StartsWith(TEXT("/Game/"))) continue;
			const FString Name = A.AssetName.ToString();
			if (!bShowAll && !Name.ToLower().Contains(QueryLower)) continue;
			if (!bFirst) Out += TEXT(",");
			Out += FString::Printf(TEXT("{\"name\":\"%s\",\"path\":\"%s\",\"class\":\"%s\"}"),
				*Name, *Path.Replace(TEXT("\""), TEXT("\\\"")), *A.AssetClassPath.GetAssetName().ToString());
			bFirst = false;
			Count++;
		}
		Out += FString::Printf(TEXT("],\"count\":%d,\"source\":\"inline-fallback\"}"), Count);
		return Out;
	}

	// PCG (default tools — Extended PCG extension uses its own server pipeline)
	if (ToolName == TEXT("create_pcg_graph"))
		return FNwiroIKPCGTools::CreatePcgGraph(ArgsJson);
	if (ToolName == TEXT("find_pcg_graphs"))
		return FNwiroIKPCGTools::FindPcgGraphs(ArgsJson);
	if (ToolName == TEXT("spawn_pcg_volume"))
		return FNwiroIKPCGTools::SpawnPcgVolume(ArgsJson);
	if (ToolName == TEXT("pcg_generate"))
		return FNwiroIKPCGTools::PcgGenerate(ArgsJson);
	if (ToolName == TEXT("add_pcg_node"))
		return FNwiroIKPCGTools::AddPcgNode(ArgsJson);

	// File WRITES require the File Editor extension. read_file is deliberately
	// ungated (2026-07-10): reads are non-destructive and already path-sandboxed,
	// so a local model can answer code questions ("open DataAsset.h") without the
	// user first enabling anything; only write/delete/rename need the toggle.
	// Do not re-add read_file here.
	if (ToolName == TEXT("write_file") || ToolName == TEXT("delete_file") || ToolName == TEXT("rename_file"))
	{
		if (UNwiroIKBridge::Instance && !UNwiroIKBridge::Instance->IsChatExtensionEnabled(TEXT("fileEditor")))
			return TEXT("{\"success\":false,\"error\":\"Writing, deleting, or renaming files requires the File Editor extension for this chat. Enable it from the Extensions menu in the chat input area. Reading files needs no extension.\"}");
	}

	// Hallucination-tolerant arg picker — returns first non-empty string in Names.
	auto PickStr = [](const TSharedPtr<FJsonObject>& Args, std::initializer_list<const TCHAR*> Names) -> FString
	{
		for (const TCHAR* N : Names)
		{
			FString V;
			if (Args->TryGetStringField(N, V) && !V.IsEmpty())
				return V;
		}
		return FString();
	};

	if (ToolName == TEXT("write_file"))
	{
		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(R, Args) && Args.IsValid())
		{
			FString FilePath = PickStr(Args, { TEXT("file_path"), TEXT("path"), TEXT("filePath"), TEXT("filepath"), TEXT("filename"), TEXT("file") });
			if (FilePath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"file_path required (accepted: file_path/path/filePath/filename)\"}");
			// Content: take the FIRST PRESENT alias and require it be a STRING (an
			// explicit empty string is allowed = intentional clear). An absent
			// field, or a present-but-non-string value (null/object/array), must
			// NOT slip through — HasField alone is type-blind, so {"content":null}
			// would otherwise truncate an existing file to zero bytes and report
			// success. This is the sharpest silent-data-loss edge now that File
			// Editor auto-enables for Local LLM.
			FString Content;
			bool bContentPresent = false;
			bool bContentIsString = false;
			for (const TCHAR* ContentKey : { TEXT("content"), TEXT("text"), TEXT("body"), TEXT("data") })
			{
				if (Args->HasField(ContentKey))
				{
					bContentPresent = true;
					bContentIsString = Args->TryGetStringField(ContentKey, Content); // false, Content untouched, for null/object/array
					break; // first present alias decides (so content:"" wins over text:"x")
				}
			}
			if (!bContentPresent)
				return TEXT("{\"success\":false,\"error\":\"content required — refusing to write with no content, which would erase an existing file. Provide a content string (an empty string is allowed to intentionally clear a file).\"}");
			if (!bContentIsString)
				return TEXT("{\"success\":false,\"error\":\"content must be a string — refusing to write, since a null or non-string content would erase an existing file.\"}");
			// Sandbox path against ProjectDir — rejects /etc/passwd, ../escape, etc.
			FilePath = NwiroIKPathSandbox::TryResolveSandboxed(FilePath);
			if (FilePath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"Path outside project sandbox. Use a path relative to the project directory, or an absolute path that resolves inside it.\"}");
			// Overwrite is silent & permanent (SaveStringToFile truncates). Back up
			// any existing file to trash FIRST via a COPY (original stays put), then
			// write — so a bad overwrite by the local model is recoverable, and a
			// failed backup aborts the write (fail-closed, like delete/rename).
			FString BackedUpTo;
			if (IFileManager::Get().FileExists(*FilePath))
			{
				BackedUpTo = StashFileToTrash(FilePath, /*bMove=*/false);
				if (BackedUpTo.IsEmpty())
					return TEXT("{\"success\":false,\"error\":\"Could not back up the existing file before overwriting; write aborted.\"}");
			}
			bool bOk = FFileHelper::SaveStringToFile(Content, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			return FString::Printf(TEXT("{\"success\":%s,\"file_path\":\"%s\",\"content\":%s,\"backed_up_to\":%s}"),
				bOk ? TEXT("true") : TEXT("false"), *FilePath.Replace(TEXT("\\"), TEXT("/")),
				bOk ? TEXT("\"written\"") : TEXT("\"failed\""),
				BackedUpTo.IsEmpty() ? TEXT("null") : *FString::Printf(TEXT("\"%s\""), *BackedUpTo.Replace(TEXT("\\"), TEXT("/"))));
		}
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	}
	if (ToolName == TEXT("read_file"))
	{
		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(R, Args) && Args.IsValid())
		{
			FString FilePath = PickStr(Args, { TEXT("file_path"), TEXT("path"), TEXT("filePath"), TEXT("filepath"), TEXT("filename"), TEXT("file") });
			if (FilePath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"file_path required (accepted: file_path/path/filePath/filename)\"}");
			FilePath = NwiroIKPathSandbox::TryResolveSandboxed(FilePath);
			if (FilePath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"Path outside project sandbox. Use a path relative to the project directory, or an absolute path that resolves inside it.\"}");
			FString Content;
			if (FFileHelper::LoadFileToString(Content, *FilePath))
			{
				Content.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
				Content.ReplaceInline(TEXT("\""), TEXT("\\\""));
				Content.ReplaceInline(TEXT("\n"), TEXT("\\n"));
				Content.ReplaceInline(TEXT("\r"), TEXT(""));
				Content.ReplaceInline(TEXT("\t"), TEXT("\\t"));
				return FString::Printf(TEXT("{\"success\":true,\"file_path\":\"%s\",\"content\":\"%s\"}"),
					*FilePath.Replace(TEXT("\\"), TEXT("/")), *Content);
			}
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"File not found: %s\"}"), *FilePath);
		}
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	}
	if (ToolName == TEXT("delete_file"))
	{
		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(R, Args) && Args.IsValid())
		{
			FString FilePath = PickStr(Args, { TEXT("file_path"), TEXT("path"), TEXT("filePath"), TEXT("filepath"), TEXT("filename"), TEXT("file") });
			if (FilePath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"file_path required (accepted: file_path/path/filePath/filename)\"}");
			FilePath = NwiroIKPathSandbox::TryResolveSandboxed(FilePath);
			if (FilePath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"Path outside project sandbox. Use a path relative to the project directory, or an absolute path that resolves inside it.\"}");
			// SOFT-delete: move to a collision-proof trash dir instead of a
			// permanent Delete (IFileManager has no recycle bin). File Editor
			// defaults ON for Local LLM and the shim has no per-action permission
			// prompt yet, so a hallucinated delete by a weak local model must be
			// recoverable. If the move fails, nothing is deleted (fail-closed).
			const FString TrashDest = StashFileToTrash(FilePath, /*bMove=*/true);
			if (TrashDest.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"Could not move the file to the trash; nothing was deleted.\"}");
			return FString::Printf(TEXT("{\"success\":true,\"file_path\":\"%s\",\"trashed_to\":\"%s\"}"),
				*FilePath.Replace(TEXT("\\"), TEXT("/")), *TrashDest.Replace(TEXT("\\"), TEXT("/")));
		}
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	}
	if (ToolName == TEXT("rename_file"))
	{
		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(R, Args) && Args.IsValid())
		{
			FString OldPath = PickStr(Args, { TEXT("old_path"), TEXT("from"), TEXT("source"), TEXT("src"), TEXT("oldPath"), TEXT("old"), TEXT("old_file"), TEXT("oldFile") });
			FString NewPath = PickStr(Args, { TEXT("new_path"), TEXT("to"), TEXT("dest"), TEXT("destination"), TEXT("newPath"), TEXT("new"), TEXT("new_file"), TEXT("newFile") });
			if (OldPath.IsEmpty() || NewPath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"old_path and new_path required (accepted: old_path/from/source, new_path/to/dest)\"}");
			OldPath = NwiroIKPathSandbox::TryResolveSandboxed(OldPath);
			NewPath = NwiroIKPathSandbox::TryResolveSandboxed(NewPath);
			if (OldPath.IsEmpty() || NewPath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"Path outside project sandbox. Use a path relative to the project directory, or an absolute path that resolves inside it.\"}");
			// Refuse to overwrite an existing destination. Move() defaults to
			// bReplace=true, so a rename onto an existing file would silently
			// destroy it with no undo — unacceptable now that File Editor defaults
			// ON for Local LLM with no per-action prompt. bReplace=false is a
			// second guard against a race between the check and the move.
			if (IFileManager::Get().FileExists(*NewPath))
				return TEXT("{\"success\":false,\"error\":\"Destination already exists; refusing to overwrite it. Choose a different name, or delete the target first.\"}");
			bool bOk = IFileManager::Get().Move(*NewPath, *OldPath, /*bReplace=*/false);
			return FString::Printf(TEXT("{\"success\":%s,\"old_path\":\"%s\",\"new_path\":\"%s\"}"),
				bOk ? TEXT("true") : TEXT("false"),
				*OldPath.Replace(TEXT("\\"), TEXT("/")), *NewPath.Replace(TEXT("\\"), TEXT("/")));
		}
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	}
	if (ToolName == TEXT("duplicate_actor"))
		return FNwiroIKSettingsTools::DuplicateActor(ArgsJson);
	if (ToolName == TEXT("rename_actor"))
		return FNwiroIKSettingsTools::RenameActor(ArgsJson);
	if (ToolName == TEXT("attach_actor"))
		return FNwiroIKSettingsTools::AttachActor(ArgsJson);
	if (ToolName == TEXT("detach_actor"))
		return FNwiroIKSettingsTools::DetachActor(ArgsJson);
	if (ToolName == TEXT("select_actor"))
		return FNwiroIKSettingsTools::SelectActor(ArgsJson);

	// Input tools
	if (ToolName == TEXT("create_input_action"))
		return FNwiroIKInputTools::CreateInputAction(ArgsJson);
	if (ToolName == TEXT("create_input_mapping_context"))
		return FNwiroIKInputTools::CreateInputMappingContext(ArgsJson);
	if (ToolName == TEXT("find_input_actions"))
		return FNwiroIKInputTools::FindInputActions(ArgsJson);
	if (ToolName == TEXT("delete_input_action"))
		return FNwiroIKInputTools::DeleteInputAction(ArgsJson);
	if (ToolName == TEXT("edit_mapping_context"))
		return FNwiroIKInputTools::EditMappingContext(ArgsJson);

	// Asset tools
	if (ToolName == TEXT("read_asset"))
		return FNwiroIKAssetTools::ReadAsset(ArgsJson);
	if (ToolName == TEXT("find_assets"))
		return FNwiroIKAssetTools::FindAssets(ArgsJson);
	if (ToolName == TEXT("find_static_meshes"))
	{
		// Alias: inject classFilter="StaticMesh" into args and delegate to
		// FindAssets. Keeps the advertised tool cheap (no new impl) while
		// preserving the searchTerm + path + maxResults path FindAssets
		// already handles. Empty searchTerm = discovery mode (list all
		// meshes in path), matching find_input_actions precedent.
		TSharedPtr<FJsonObject> Cmd;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
			Cmd = MakeShareable(new FJsonObject());
		Cmd->SetStringField(TEXT("classFilter"), TEXT("StaticMesh"));
		FString Rewritten;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Rewritten);
		FJsonSerializer::Serialize(Cmd.ToSharedRef(), W);
		return FNwiroIKAssetTools::FindAssets(Rewritten);
	}
	if (ToolName == TEXT("get_asset_thumbnail"))
		return FNwiroIKAssetTools::GetAssetThumbnail(ArgsJson);

	// Editor tools
	if (ToolName == TEXT("take_screenshot"))
		return FNwiroIKEditorTools::TakeScreenshot(ArgsJson);
	if (ToolName == TEXT("read_log"))
		return FNwiroIKEditorTools::ReadLog(ArgsJson);
	if (ToolName == TEXT("play_in_editor"))
		return FNwiroIKEditorTools::PlayInEditor(ArgsJson);
	if (ToolName == TEXT("stop_pie"))
		return FNwiroIKEditorTools::StopPIE(ArgsJson);

	// Data tools
	if (ToolName == TEXT("create_data_table"))
		return FNwiroIKDataTools::CreateDataTable(ArgsJson);
	if (ToolName == TEXT("add_data_table_row"))
		return FNwiroIKDataTools::AddDataTableRow(ArgsJson);
	if (ToolName == TEXT("read_data_table"))
		return FNwiroIKDataTools::ReadDataTable(ArgsJson);
	if (ToolName == TEXT("import_data_table_json"))
		return FNwiroIKDataTools::ImportDataTableJson(ArgsJson);
	if (ToolName == TEXT("create_struct"))
		return FNwiroIKDataTools::CreateStruct(ArgsJson);
	if (ToolName == TEXT("create_enum"))
		return FNwiroIKDataTools::CreateEnum(ArgsJson);

	// Animation tools
	if (ToolName == TEXT("create_montage"))
		return FNwiroIKAnimTools::CreateMontage(ArgsJson);
	if (ToolName == TEXT("read_montage"))
		return FNwiroIKAnimTools::ReadMontage(ArgsJson);
	if (ToolName == TEXT("add_montage_section"))
		return FNwiroIKAnimTools::AddMontageSection(ArgsJson);
	if (ToolName == TEXT("link_montage_sections"))
		return FNwiroIKAnimTools::LinkMontageSections(ArgsJson);
	if (ToolName == TEXT("add_montage_notify"))
		return FNwiroIKAnimTools::AddMontageNotify(ArgsJson);
	if (ToolName == TEXT("create_anim_blueprint"))
		return FNwiroIKAnimTools::CreateAnimBlueprint(ArgsJson);
	if (ToolName == TEXT("read_anim_blueprint"))
		return FNwiroIKAnimTools::ReadAnimBlueprint(ArgsJson);

	// Sequencer tools
	if (ToolName == TEXT("create_sequence"))
		return FNwiroIKSequencerTools::CreateSequence(ArgsJson);
	if (ToolName == TEXT("read_sequence"))
		return FNwiroIKSequencerTools::ReadSequence(ArgsJson);
	if (ToolName == TEXT("add_sequence_binding"))
		return FNwiroIKSequencerTools::AddSequenceBinding(ArgsJson);
	if (ToolName == TEXT("add_sequence_track"))
		return FNwiroIKSequencerTools::AddSequenceTrack(ArgsJson);
	if (ToolName == TEXT("add_sequence_keyframe"))
		return FNwiroIKSequencerTools::AddSequenceKeyframe(ArgsJson);
	if (ToolName == TEXT("set_sequence_range"))
		return FNwiroIKSequencerTools::SetSequenceRange(ArgsJson);

	// AI tools
	if (ToolName == TEXT("create_behavior_tree"))
		return FNwiroIKAITools::CreateBehaviorTree(ArgsJson);
	if (ToolName == TEXT("read_behavior_tree"))
		return FNwiroIKAITools::ReadBehaviorTree(ArgsJson);
	if (ToolName == TEXT("create_blackboard"))
		return FNwiroIKAITools::CreateBlackboard(ArgsJson);
	if (ToolName == TEXT("edit_blackboard"))
		return FNwiroIKAITools::EditBlackboard(ArgsJson);

	// Widget tools
	if (ToolName == TEXT("create_widget_blueprint"))
		return FNwiroIKWidgetTools::CreateWidgetBlueprint(ArgsJson);
	if (ToolName == TEXT("read_widget_blueprint"))
		return FNwiroIKWidgetTools::ReadWidgetBlueprint(ArgsJson);
	if (ToolName == TEXT("add_widget"))
		return FNwiroIKWidgetTools::AddWidget(ArgsJson);
	if (ToolName == TEXT("set_widget_property"))
		return FNwiroIKWidgetTools::SetWidgetProperty(ArgsJson);
	if (ToolName == TEXT("render_widget_blueprint"))
		return FNwiroIKWidgetTools::RenderWidgetBlueprint(ArgsJson);

	// Niagara tools
	if (ToolName == TEXT("create_niagara_system"))
		return FNwiroIKNiagaraTools::CreateNiagaraSystem(ArgsJson);
	if (ToolName == TEXT("read_niagara_system"))
		return FNwiroIKNiagaraTools::ReadNiagaraSystem(ArgsJson);
	if (ToolName == TEXT("set_niagara_parameter"))
		return FNwiroIKNiagaraTools::SetNiagaraParameter(ArgsJson);

	// State Tree tools
	if (ToolName == TEXT("create_state_tree"))
		return FNwiroIKStateTreeTools::CreateStateTree(ArgsJson);
	if (ToolName == TEXT("read_state_tree"))
		return FNwiroIKStateTreeTools::ReadStateTree(ArgsJson);
	if (ToolName == TEXT("add_state_tree_state"))
		return FNwiroIKStateTreeTools::AddStateTreeState(ArgsJson);

#if NWIRO_HAS_IK_TOOLS
	// IK Rig tools
	if (ToolName == TEXT("create_ik_rig"))
		return FNwiroIKTools::CreateIKRig(ArgsJson);
	if (ToolName == TEXT("read_ik_rig"))
		return FNwiroIKTools::ReadIKRig(ArgsJson);
	if (ToolName == TEXT("add_ik_goal"))
		return FNwiroIKTools::AddIKGoal(ArgsJson);
	if (ToolName == TEXT("add_ik_solver"))
		return FNwiroIKTools::AddIKSolver(ArgsJson);
	if (ToolName == TEXT("add_retarget_chain"))
		return FNwiroIKTools::AddRetargetChain(ArgsJson);

	// IK Retargeter tools
	if (ToolName == TEXT("create_ik_retargeter"))
		return FNwiroIKTools::CreateIKRetargeter(ArgsJson);
	if (ToolName == TEXT("read_ik_retargeter"))
		return FNwiroIKTools::ReadIKRetargeter(ArgsJson);
	if (ToolName == TEXT("set_chain_mapping"))
		return FNwiroIKTools::SetChainMapping(ArgsJson);

	// Pose Search / Motion Matching tools
	if (ToolName == TEXT("create_pose_search_schema"))
		return FNwiroIKTools::CreatePoseSearchSchema(ArgsJson);
	if (ToolName == TEXT("create_pose_search_database"))
		return FNwiroIKTools::CreatePoseSearchDatabase(ArgsJson);
	if (ToolName == TEXT("read_pose_search_database"))
		return FNwiroIKTools::ReadPoseSearchDatabase(ArgsJson);
	if (ToolName == TEXT("add_pose_search_animation"))
		return FNwiroIKTools::AddPoseSearchAnimation(ArgsJson);
#endif // NWIRO_HAS_IK_TOOLS

	// BT / AnimBP / Niagara edit tools
	if (ToolName == TEXT("add_behavior_tree_nodes"))
		return FNwiroIKAITools::AddBehaviorTreeNodes(ArgsJson);
	if (ToolName == TEXT("add_anim_bp_state_machine"))
		return FNwiroIKAnimTools::AddAnimBPStateMachine(ArgsJson);
	if (ToolName == TEXT("add_niagara_emitter"))
		return FNwiroIKNiagaraTools::AddNiagaraEmitter(ArgsJson);

	// Environment tools
	if (ToolName == TEXT("set_post_process"))
		return FNwiroIKEnvironmentTools::SetPostProcess(ArgsJson);
	if (ToolName == TEXT("set_fog"))
		return FNwiroIKEnvironmentTools::SetFog(ArgsJson);
	if (ToolName == TEXT("set_sky_atmosphere"))
		return FNwiroIKEnvironmentTools::SetSkyAtmosphere(ArgsJson);
	if (ToolName == TEXT("set_light_properties"))
		return FNwiroIKEnvironmentTools::SetLightProperties(ArgsJson);

	// Physics tools
	if (ToolName == TEXT("set_physics_simulation"))
		return FNwiroIKEnvironmentTools::SetPhysicsSimulation(ArgsJson);
	if (ToolName == TEXT("set_collision_profile"))
		return FNwiroIKEnvironmentTools::SetCollisionProfile(ArgsJson);
	if (ToolName == TEXT("add_physics_constraint"))
		return FNwiroIKEnvironmentTools::AddPhysicsConstraint(ArgsJson);
	if (ToolName == TEXT("get_physics_info"))
		return FNwiroIKEnvironmentTools::GetPhysicsInfo(ArgsJson);

	// Spline tools
	if (ToolName == TEXT("create_spline_actor"))
		return FNwiroIKEnvironmentTools::CreateSplineActor(ArgsJson);
	if (ToolName == TEXT("add_spline_point"))
		return FNwiroIKEnvironmentTools::AddSplinePoint(ArgsJson);
	if (ToolName == TEXT("set_spline_point"))
		return FNwiroIKEnvironmentTools::SetSplinePoint(ArgsJson);
	if (ToolName == TEXT("remove_spline_point"))
		return FNwiroIKEnvironmentTools::RemoveSplinePoint(ArgsJson);
	if (ToolName == TEXT("get_spline_info"))
		return FNwiroIKEnvironmentTools::GetSplineInfo(ArgsJson);
	if (ToolName == TEXT("set_spline_closed"))
		return FNwiroIKEnvironmentTools::SetSplineClosed(ArgsJson);
	if (ToolName == TEXT("set_spline_point_type"))
		return FNwiroIKEnvironmentTools::SetSplinePointType(ArgsJson);

	// Navigation tools
	if (ToolName == TEXT("build_navigation"))
		return FNwiroIKGameplayTools::BuildNavigation(ArgsJson);
	if (ToolName == TEXT("query_navigation_path"))
		return FNwiroIKGameplayTools::QueryNavigationPath(ArgsJson);
	if (ToolName == TEXT("get_navigation_info"))
		return FNwiroIKGameplayTools::GetNavigationInfo(ArgsJson);

	// Audio tools
	if (ToolName == TEXT("spawn_sound"))
		return FNwiroIKGameplayTools::SpawnSound(ArgsJson);
	if (ToolName == TEXT("set_audio_properties"))
		return FNwiroIKGameplayTools::SetAudioProperties(ArgsJson);
	if (ToolName == TEXT("get_sound_info"))
		return FNwiroIKGameplayTools::GetSoundInfo(ArgsJson);

	// Game Framework tools
	if (ToolName == TEXT("create_game_mode"))
		return FNwiroIKGameplayTools::CreateGameMode(ArgsJson);
	if (ToolName == TEXT("create_player_controller"))
		return FNwiroIKGameplayTools::CreatePlayerController(ArgsJson);
	if (ToolName == TEXT("create_game_state"))
		return FNwiroIKGameplayTools::CreateGameState(ArgsJson);
	if (ToolName == TEXT("create_player_state"))
		return FNwiroIKGameplayTools::CreatePlayerState(ArgsJson);
	if (ToolName == TEXT("create_hud"))
		return FNwiroIKGameplayTools::CreateHUD(ArgsJson);
	if (ToolName == TEXT("get_game_framework_info"))
		return FNwiroIKGameplayTools::GetGameFrameworkInfo(ArgsJson);

	// Build / Validation tools
	if (ToolName == TEXT("get_project_info"))
		return FNwiroIKGameplayTools::GetProjectInfo(ArgsJson);
	if (ToolName == TEXT("list_project_modules"))
		return FNwiroIKGameplayTools::ListProjectModules(ArgsJson);
	if (ToolName == TEXT("validate_assets"))
		return FNwiroIKGameplayTools::ValidateAssets(ArgsJson);
	if (ToolName == TEXT("get_map_check_errors"))
		return FNwiroIKGameplayTools::GetMapCheckErrors(ArgsJson);
	if (ToolName == TEXT("get_build_configuration"))
		return FNwiroIKGameplayTools::GetBuildConfiguration(ArgsJson);

	// Resources
	if (ToolName == TEXT("list_resources"))
		return FNwiroIKResourceProvider::GetResourceList();
	if (ToolName == TEXT("read_resource"))
	{
		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> RR = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(RR, Args) && Args.IsValid())
			return FNwiroIKResourceProvider::ReadResource(Args->GetStringField(TEXT("uri")));
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	}

	// Level tools
	if (ToolName == TEXT("new_level")) return FNwiroIKLevelTools::NewLevel(ArgsJson);
	if (ToolName == TEXT("open_level")) return FNwiroIKLevelTools::OpenLevel(ArgsJson);
	if (ToolName == TEXT("save_level")) return FNwiroIKLevelTools::SaveLevel(ArgsJson);
	if (ToolName == TEXT("get_level_info")) return FNwiroIKLevelTools::GetLevelInfo(ArgsJson);

	// Macro tools
	if (ToolName == TEXT("create_basic_level")) return FNwiroIKLevelTools::CreateBasicLevel(ArgsJson);
	if (ToolName == TEXT("create_light_rig")) return FNwiroIKLevelTools::CreateLightRig(ArgsJson);
	if (ToolName == TEXT("create_grid_layout")) return FNwiroIKLevelTools::CreateGridLayout(ArgsJson);
	if (ToolName == TEXT("create_ring_layout")) return FNwiroIKLevelTools::CreateRingLayout(ArgsJson);

	// Landscape tools
	if (ToolName == TEXT("create_landscape")) return FNwiroIKLevelTools::CreateLandscape(ArgsJson);
	if (ToolName == TEXT("set_landscape_material")) return FNwiroIKLevelTools::SetLandscapeMaterial(ArgsJson);
	if (ToolName == TEXT("get_landscape_info")) return FNwiroIKLevelTools::GetLandscapeInfo(ArgsJson);

	// Foliage tools
	if (ToolName == TEXT("add_foliage_type")) return FNwiroIKLevelTools::AddFoliageType(ArgsJson);
	if (ToolName == TEXT("paint_foliage")) return FNwiroIKLevelTools::PaintFoliage(ArgsJson);
	if (ToolName == TEXT("erase_foliage")) return FNwiroIKLevelTools::EraseFoliage(ArgsJson);
	if (ToolName == TEXT("get_foliage_stats")) return FNwiroIKLevelTools::GetFoliageStats(ArgsJson);

	// Networking tools
	if (ToolName == TEXT("get_replication_info")) return FNwiroIKLevelTools::GetReplicationInfo(ArgsJson);
	if (ToolName == TEXT("set_replication_settings")) return FNwiroIKLevelTools::SetReplicationSettings(ArgsJson);
	if (ToolName == TEXT("set_net_dormancy")) return FNwiroIKLevelTools::SetNetDormancy(ArgsJson);

	// World Partition tools
	if (ToolName == TEXT("get_world_partition_info")) return FNwiroIKLevelTools::GetWorldPartitionInfo(ArgsJson);
	if (ToolName == TEXT("load_world_partition_region")) return FNwiroIKLevelTools::LoadWorldPartitionRegion(ArgsJson);

	// Undo/Redo
	if (ToolName == TEXT("undo")) return FNwiroIKLevelTools::Undo(ArgsJson);
	if (ToolName == TEXT("redo")) return FNwiroIKLevelTools::Redo(ArgsJson);

	// GAS tools
	if (ToolName == TEXT("create_gameplay_ability")) return FNwiroIKGASTools::CreateGameplayAbility(ArgsJson);
	if (ToolName == TEXT("create_gameplay_effect")) return FNwiroIKGASTools::CreateGameplayEffect(ArgsJson);
	if (ToolName == TEXT("create_attribute_set")) return FNwiroIKGASTools::CreateAttributeSet(ArgsJson);
	if (ToolName == TEXT("list_gameplay_abilities")) return FNwiroIKGASTools::ListGameplayAbilities(ArgsJson);
	if (ToolName == TEXT("list_gameplay_effects")) return FNwiroIKGASTools::ListGameplayEffects(ArgsJson);
	if (ToolName == TEXT("list_attribute_sets")) return FNwiroIKGASTools::ListAttributeSets(ArgsJson);
	if (ToolName == TEXT("get_gas_info")) return FNwiroIKGASTools::GetGASInfo(ArgsJson);

	// PCG tools — handled by FNwiroIKPCGTools higher up in the dispatch.

	// PIE Runtime Control
	if (ToolName == TEXT("pie_teleport_actor")) return FNwiroIKPIETools::PIETeleportActor(ArgsJson);
	if (ToolName == TEXT("pie_spawn_actor")) return FNwiroIKPIETools::PIESpawnActor(ArgsJson);
	if (ToolName == TEXT("pie_destroy_actor")) return FNwiroIKPIETools::PIEDestroyActor(ArgsJson);
	if (ToolName == TEXT("pie_get_property")) return FNwiroIKPIETools::PIEGetProperty(ArgsJson);
	if (ToolName == TEXT("pie_set_property")) return FNwiroIKPIETools::PIESetProperty(ArgsJson);
	if (ToolName == TEXT("pie_set_blackboard_key")) return FNwiroIKPIETools::PIESetBlackboardKey(ArgsJson);
	if (ToolName == TEXT("pie_get_blackboard_key")) return FNwiroIKPIETools::PIEGetBlackboardKey(ArgsJson);
	if (ToolName == TEXT("pie_move_ai_to")) return FNwiroIKPIETools::PIEMoveAITo(ArgsJson);
	if (ToolName == TEXT("pie_stop_ai")) return FNwiroIKPIETools::PIEStopAI(ArgsJson);
	if (ToolName == TEXT("pie_get_game_state")) return FNwiroIKPIETools::PIEGetGameState(ArgsJson);
	if (ToolName == TEXT("pie_list_actors")) return FNwiroIKPIETools::PIEListActors(ArgsJson);
	if (ToolName == TEXT("pie_console_command")) return FNwiroIKPIETools::PIEConsoleCommand(ArgsJson);

	// Blueprint Debugger
	if (ToolName == TEXT("bp_get_compile_errors")) return FNwiroIKDebugTools::BPGetCompileErrors(ArgsJson);
	if (ToolName == TEXT("bp_set_breakpoint")) return FNwiroIKDebugTools::BPSetBreakpoint(ArgsJson);
	if (ToolName == TEXT("bp_remove_breakpoint")) return FNwiroIKDebugTools::BPRemoveBreakpoint(ArgsJson);
	if (ToolName == TEXT("bp_list_breakpoints")) return FNwiroIKDebugTools::BPListBreakpoints(ArgsJson);
	if (ToolName == TEXT("bp_get_watch_values")) return FNwiroIKDebugTools::BPGetWatchValues(ArgsJson);
	if (ToolName == TEXT("bp_add_watch")) return FNwiroIKDebugTools::BPAddWatch(ArgsJson);

	// Blueprint Error Fixer
	if (ToolName == TEXT("bp_fix_broken_references")) return FNwiroIKDebugTools::BPFixBrokenReferences(ArgsJson);
	if (ToolName == TEXT("bp_fix_deprecated_nodes")) return FNwiroIKDebugTools::BPFixDeprecatedNodes(ArgsJson);
	if (ToolName == TEXT("bp_refresh_all_nodes")) return FNwiroIKDebugTools::BPRefreshAllNodes(ArgsJson);
	if (ToolName == TEXT("bp_find_unconnected_pins")) return FNwiroIKDebugTools::BPFindUnconnectedPins(ArgsJson);

	// Asset Dependency
	if (ToolName == TEXT("get_asset_references")) return FNwiroIKDebugTools::GetAssetReferences(ArgsJson);
	if (ToolName == TEXT("get_asset_referencers")) return FNwiroIKDebugTools::GetAssetReferencers(ArgsJson);
	if (ToolName == TEXT("find_orphan_assets")) return FNwiroIKDebugTools::FindOrphanAssets(ArgsJson);
	if (ToolName == TEXT("find_circular_dependencies")) return FNwiroIKDebugTools::FindCircularDependencies(ArgsJson);
	if (ToolName == TEXT("get_dependency_tree")) return FNwiroIKDebugTools::GetDependencyTree(ArgsJson);

	// SetCDOProperty
	if (ToolName == TEXT("set_cdo_property"))
	{
		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(R, Args) || !Args.IsValid())
			return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

		FString BPName = Args->GetStringField(TEXT("blueprint"));
		FString PropName = Args->GetStringField(TEXT("property"));
		FString Value = Args->GetStringField(TEXT("value"));

		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BPName);
		if (!BP)
		{
			FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			FARFilter Filter;
			Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;
			Filter.bRecursivePaths = true;
			TArray<FAssetData> Assets;
			ARM.Get().GetAssets(Filter, Assets);
			for (const FAssetData& A : Assets)
			{
				if (A.AssetName.ToString().Equals(BPName, ESearchCase::IgnoreCase))
				{
					BP = Cast<UBlueprint>(NwiroSafeRegistryLoad(A));
					break;
				}
			}
		}
		if (!BP || !BP->GeneratedClass)
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

		// GetDefaultObject() crashes outright (CoreUObject UObjectGlobals.cpp:3396
		// "InClass && InClass->ClassWithin && InClass->ClassConstructor") when the
		// generated class is half-constructed or never compiled. Validate state
		// before dereferencing so the caller sees an error instead of an editor crash.
		UClass* GenClass = BP->GeneratedClass;
		if (!GenClass->ClassWithin || !GenClass->ClassConstructor)
		{
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"GeneratedClass for %s is half-constructed (no ClassConstructor) — recompile the blueprint and retry. Asset path: %s\"}"),
				*BPName, *BP->GetPathName());
		}

		UObject* CDO = GenClass->GetDefaultObject();
		if (!CDO) return TEXT("{\"success\":false,\"error\":\"No CDO\"}");

		FProperty* Prop = BP->GeneratedClass->FindPropertyByName(FName(*PropName));
		if (!Prop)
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"Property not found: %s\"}"), *PropName);

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
		{
			FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "SetBlueprintProperty", "AI: Set Blueprint Property"), CDO);
			Tx.AlsoModify(BP);

			if (Prop->ImportText_Direct(*Value, ValuePtr, CDO, PPF_None))
			{
				CDO->MarkPackageDirty();
				// Compile first, then mirror. Compile re-derives NewVariables[].DefaultValue
				// from the CDO via ExportText; for FBoolProperty etc. that round-trip can
				// land empty. Doing the mirror AFTER compile makes the read_blueprint
				// view consistent with the user's input.
				FKismetEditorUtilities::CompileBlueprint(BP);
				const int32 VarIdx = FBlueprintEditorUtils::FindNewVariableIndex(BP, FName(*PropName));
				if (VarIdx != INDEX_NONE)
				{
					// Refresh prop ptr after compile (class regen may invalidate Prop)
					FProperty* P2 = BP->GeneratedClass ? BP->GeneratedClass->FindPropertyByName(FName(*PropName)) : Prop;
					UObject* CDO2 = BP->GeneratedClass ? BP->GeneratedClass->GetDefaultObject() : CDO;
					FString Exported;
					if (P2 && CDO2)
					{
						void* V2 = P2->ContainerPtrToValuePtr<void>(CDO2);
						P2->ExportTextItem_Direct(Exported, V2, V2, CDO2, PPF_None);
					}
					if (Exported.IsEmpty()) Exported = Value;
					BP->NewVariables[VarIdx].DefaultValue = Exported;
					BP->MarkPackageDirty();
				}
				return FString::Printf(TEXT("{\"success\":true,\"property\":\"%s\",\"value\":\"%s\"}"), *PropName, *Value);
			}
			Tx.Cancel();
		}
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to set %s\"}"), *PropName);
	}

	// Utility
	if (ToolName == TEXT("clear_refs"))
	{
		FNwiroIKBlueprintTools::ClearNodeRefs();
		FNwiroIKMaterialTools::ClearExpressionRefs();
		return TEXT("{\"success\":true,\"message\":\"All refs cleared\"}");
	}

	// === 3D Generation: Meshy ===
	if (ToolName == TEXT("generate_3d_model_meshy"))
	{
		if (UNwiroIKBridge::Instance && !UNwiroIKBridge::Instance->IsChatExtensionEnabled(TEXT("meshy")))
			return TEXT("{\"success\":false,\"error\":\"Meshy 3D extension is not enabled for this chat. Enable it from the Extensions menu in the chat input.\"}");

		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(R, Args) || !Args.IsValid())
			return TEXT("{\"success\":false,\"error\":\"Invalid arguments\"}");

		FString Prompt = Args->GetStringField(TEXT("prompt"));
		if (Prompt.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"prompt is required\"}");

		extern FString LoadNwiroSecret(const FString& Key);
		FString ApiKey = LoadNwiroSecret(TEXT("MESHY_API_KEY"));
		if (ApiKey.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"Meshy API key not configured. Set it in Settings > Extensions > Meshy 3D.\"}");

		TSharedRef<FJsonObject> Body = MakeShareable(new FJsonObject);
		Body->SetStringField(TEXT("mode"), TEXT("preview"));
		Body->SetStringField(TEXT("prompt"), Prompt);
		Body->SetStringField(TEXT("ai_model"), TEXT("meshy-6"));
		Body->SetStringField(TEXT("topology"), TEXT("quad"));
		Body->SetNumberField(TEXT("target_polycount"), 30000);
		Body->SetBoolField(TEXT("should_remesh"), true);
		TArray<TSharedPtr<FJsonValue>> Formats;
		Formats.Add(MakeShareable(new FJsonValueString(TEXT("glb"))));
		Formats.Add(MakeShareable(new FJsonValueString(TEXT("fbx"))));
		Formats.Add(MakeShareable(new FJsonValueString(TEXT("obj"))));
		Body->SetArrayField(TEXT("target_formats"), Formats);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body, W);

		auto HttpReq = FHttpModule::Get().CreateRequest();
		HttpReq->SetURL(TEXT("https://api.meshy.ai/openapi/v2/text-to-3d"));
		HttpReq->SetVerb(TEXT("POST"));
		HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		HttpReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
		HttpReq->SetContentAsString(BodyStr);

		FString CapturedPrompt = Prompt;
		HttpReq->OnProcessRequestComplete().BindLambda(
			[CapturedPrompt](FHttpRequestPtr, FHttpResponsePtr Response, bool bSuccess)
			{
				auto PushError = [](const FString& Msg) {
					if (UNwiroIKBridge::Instance)
					{
						FString EventData = FString::Printf(TEXT("{\"provider\":\"meshy\",\"message\":\"%s\"}"),
							*Msg.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\"")));
						UNwiroIKBridge::Instance->PushEvent(TEXT("asset3d_error"), EventData);
					}
				};
				if (!bSuccess || !Response.IsValid()) { UE_LOG(LogTemp, Error, TEXT("Nwiro: Meshy API request failed")); PushError(TEXT("Network error contacting Meshy")); return; }
				int32 Status = Response->GetResponseCode();
				FString ResponseBody = Response->GetContentAsString();
				if (Status < 200 || Status >= 300)
				{
					UE_LOG(LogTemp, Error, TEXT("Nwiro: Meshy API error %d: %s"), Status, *ResponseBody.Left(200));
					FString ErrMsg = FString::Printf(TEXT("Meshy %d"), Status);
					TSharedPtr<FJsonObject> ErrJson;
					TSharedRef<TJsonReader<>> ErrReader = TJsonReaderFactory<>::Create(ResponseBody);
					if (FJsonSerializer::Deserialize(ErrReader, ErrJson) && ErrJson.IsValid())
					{
						FString Msg;
						if (ErrJson->TryGetStringField(TEXT("message"), Msg) && !Msg.IsEmpty()) ErrMsg = Msg;
					}
					PushError(ErrMsg);
					return;
				}
				TSharedPtr<FJsonObject> RespJson;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
				if (FJsonSerializer::Deserialize(Reader, RespJson) && RespJson.IsValid() && RespJson->HasField(TEXT("result")))
				{
					FString TaskId = RespJson->GetStringField(TEXT("result"));
					if (UNwiroIKBridge::Instance)
					{
						FString EventData = FString::Printf(TEXT("{\"taskId\":\"%s\",\"prompt\":\"%s\"}"),
							*TaskId, *CapturedPrompt.Replace(TEXT("\""), TEXT("\\\"")));
						UNwiroIKBridge::Instance->PushEvent(TEXT("meshy_task"), EventData);
					}
				}
			});

		HttpReq->ProcessRequest();
		return TEXT("{\"success\":true,\"status\":\"submitted\",\"message\":\"Generation request submitted to Meshy. This is asynchronous — the model will appear in the Meshy panel when ready. Do NOT tell the user the model is ready yet.\"}");
	}

	// === 3D Texture: Meshy ===
	if (ToolName == TEXT("generate_texture_meshy"))
	{
		if (UNwiroIKBridge::Instance && !UNwiroIKBridge::Instance->IsChatExtensionEnabled(TEXT("meshy")))
			return TEXT("{\"success\":false,\"error\":\"Meshy 3D extension is not enabled for this chat.\"}");

		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(R, Args) || !Args.IsValid())
			return TEXT("{\"success\":false,\"error\":\"Invalid arguments\"}");

		FString Prompt = Args->GetStringField(TEXT("prompt"));
		if (Prompt.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"prompt is required\"}");

		FString ModelUrl = Args->HasField(TEXT("model_url")) ? Args->GetStringField(TEXT("model_url")) : TEXT("");
		FString ModelId = Args->HasField(TEXT("model_id")) ? Args->GetStringField(TEXT("model_id")) : TEXT("");
		if (ModelUrl.IsEmpty() && ModelId.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"Either model_url or model_id is required\"}");

		extern FString LoadNwiroSecret(const FString& Key);
		FString ApiKey = LoadNwiroSecret(TEXT("MESHY_API_KEY"));
		if (ApiKey.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"Meshy API key not configured.\"}");

		TSharedRef<FJsonObject> Body = MakeShareable(new FJsonObject);
		if (!ModelUrl.IsEmpty()) Body->SetStringField(TEXT("model_url"), ModelUrl);
		if (!ModelId.IsEmpty()) Body->SetStringField(TEXT("model_id"), ModelId);
		Body->SetStringField(TEXT("prompt"), Prompt);
		Body->SetStringField(TEXT("ai_model"), TEXT("meshy-6"));

		FString BodyStr;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body, W);

		auto HttpReq = FHttpModule::Get().CreateRequest();
		HttpReq->SetURL(TEXT("https://api.meshy.ai/openapi/v2/text-to-texture"));
		HttpReq->SetVerb(TEXT("POST"));
		HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		HttpReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
		HttpReq->SetContentAsString(BodyStr);

		FString CapturedPrompt = Prompt;
		HttpReq->OnProcessRequestComplete().BindLambda(
			[CapturedPrompt](FHttpRequestPtr, FHttpResponsePtr Response, bool bSuccess)
			{
				auto PushError = [](const FString& Msg) {
					if (UNwiroIKBridge::Instance)
					{
						FString EventData = FString::Printf(TEXT("{\"provider\":\"meshy\",\"message\":\"%s\"}"),
							*Msg.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\"")));
						UNwiroIKBridge::Instance->PushEvent(TEXT("asset3d_error"), EventData);
					}
				};
				if (!bSuccess || !Response.IsValid()) { PushError(TEXT("Network error contacting Meshy")); return; }
				int32 Status = Response->GetResponseCode();
				FString ResponseBody = Response->GetContentAsString();
				if (Status < 200 || Status >= 300)
				{
					FString ErrMsg = FString::Printf(TEXT("Meshy texture %d"), Status);
					TSharedPtr<FJsonObject> ErrJson;
					TSharedRef<TJsonReader<>> ErrReader = TJsonReaderFactory<>::Create(ResponseBody);
					if (FJsonSerializer::Deserialize(ErrReader, ErrJson) && ErrJson.IsValid())
					{
						FString Msg;
						if (ErrJson->TryGetStringField(TEXT("message"), Msg) && !Msg.IsEmpty()) ErrMsg = Msg;
					}
					PushError(ErrMsg);
					return;
				}
				TSharedPtr<FJsonObject> RespJson;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
				if (FJsonSerializer::Deserialize(Reader, RespJson) && RespJson.IsValid() && RespJson->HasField(TEXT("result")))
				{
					FString TaskId = RespJson->GetStringField(TEXT("result"));
					if (UNwiroIKBridge::Instance)
					{
						FString EventData = FString::Printf(TEXT("{\"taskId\":\"%s\",\"prompt\":\"%s\"}"),
							*TaskId, *CapturedPrompt.Replace(TEXT("\""), TEXT("\\\"")));
						UNwiroIKBridge::Instance->PushEvent(TEXT("meshy_task"), EventData);
					}
				}
			});

		HttpReq->ProcessRequest();
		return TEXT("{\"success\":true,\"status\":\"submitted\",\"message\":\"Texture generation submitted to Meshy. The result will appear in the Meshy panel when ready.\"}");
	}

	// === 3D Generation: Tripo ===
	if (ToolName == TEXT("generate_3d_model_tripo"))
	{
		if (UNwiroIKBridge::Instance && !UNwiroIKBridge::Instance->IsChatExtensionEnabled(TEXT("tripo")))
			return TEXT("{\"success\":false,\"error\":\"Tripo 3D extension is not enabled for this chat. Enable it from the Extensions menu in the chat input.\"}");

		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(R, Args) || !Args.IsValid())
			return TEXT("{\"success\":false,\"error\":\"Invalid arguments\"}");

		FString Prompt = Args->GetStringField(TEXT("prompt"));
		if (Prompt.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"prompt is required\"}");

		extern FString LoadNwiroSecret(const FString& Key);
		FString ApiKey = LoadNwiroSecret(TEXT("TRIPO_API_KEY"));
		if (ApiKey.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"Tripo API key not configured. Set it in Settings > Extensions > Tripo 3D.\"}");

		TSharedRef<FJsonObject> Body = MakeShareable(new FJsonObject);
		Body->SetStringField(TEXT("type"), TEXT("text_to_model"));
		Body->SetStringField(TEXT("prompt"), Prompt);
		Body->SetStringField(TEXT("model_version"), TEXT("v2.5-20250123"));

		FString BodyStr;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body, W);

		auto HttpReq = FHttpModule::Get().CreateRequest();
		HttpReq->SetURL(TEXT("https://api.tripo3d.ai/v2/openapi/task"));
		HttpReq->SetVerb(TEXT("POST"));
		HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		HttpReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
		HttpReq->SetContentAsString(BodyStr);

		FString CapturedPrompt = Prompt;
		HttpReq->OnProcessRequestComplete().BindLambda(
			[CapturedPrompt](FHttpRequestPtr, FHttpResponsePtr Response, bool bSuccess)
			{
				auto PushError = [](const FString& Msg) {
					if (UNwiroIKBridge::Instance)
					{
						FString EventData = FString::Printf(TEXT("{\"provider\":\"tripo\",\"message\":\"%s\"}"),
							*Msg.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\"")));
						UNwiroIKBridge::Instance->PushEvent(TEXT("asset3d_error"), EventData);
					}
				};
				if (!bSuccess || !Response.IsValid()) { UE_LOG(LogTemp, Error, TEXT("Nwiro: Tripo API request failed")); PushError(TEXT("Network error contacting Tripo")); return; }
				int32 Status = Response->GetResponseCode();
				FString ResponseBody = Response->GetContentAsString();
				auto ParseTripoMessage = [&ResponseBody](FString& OutMsg) {
					TSharedPtr<FJsonObject> J;
					TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ResponseBody);
					if (FJsonSerializer::Deserialize(R, J) && J.IsValid())
					{
						FString Msg;
						if (J->TryGetStringField(TEXT("message"), Msg) && !Msg.IsEmpty()) { OutMsg = Msg; return true; }
					}
					return false;
				};
				if (Status < 200 || Status >= 300)
				{
					UE_LOG(LogTemp, Error, TEXT("Nwiro: Tripo API error %d: %s"), Status, *ResponseBody.Left(200));
					FString ErrMsg = FString::Printf(TEXT("Tripo %d"), Status);
					ParseTripoMessage(ErrMsg);
					PushError(ErrMsg);
					return;
				}
				TSharedPtr<FJsonObject> RespJson;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
				if (FJsonSerializer::Deserialize(Reader, RespJson) && RespJson.IsValid())
				{
					int32 Code = 0;
					RespJson->TryGetNumberField(TEXT("code"), Code);
					const TSharedPtr<FJsonObject>* DataObj = nullptr;
					if (Code == 0 && RespJson->TryGetObjectField(TEXT("data"), DataObj) && DataObj && (*DataObj).IsValid())
					{
						FString TaskId;
						if ((*DataObj)->TryGetStringField(TEXT("task_id"), TaskId))
						{
							if (UNwiroIKBridge::Instance)
							{
								FString EventData = FString::Printf(TEXT("{\"taskId\":\"%s\",\"prompt\":\"%s\"}"),
									*TaskId, *CapturedPrompt.Replace(TEXT("\""), TEXT("\\\"")));
								UNwiroIKBridge::Instance->PushEvent(TEXT("tripo_task"), EventData);
							}
						}
					}
					else
					{
						FString ErrMsg = FString::Printf(TEXT("Tripo error code %d"), Code);
						ParseTripoMessage(ErrMsg);
						PushError(ErrMsg);
					}
				}
			});

		HttpReq->ProcessRequest();
		return TEXT("{\"success\":true,\"status\":\"submitted\",\"message\":\"Generation request submitted to Tripo. This is asynchronous — the model will appear in the Tripo panel when ready. Do NOT tell the user the model is ready yet.\"}");
	}

	// === ElevenLabs: Voices list (sync — small JSON response) ===
	if (ToolName == TEXT("list_voices_elevenlabs"))
	{
		if (UNwiroIKBridge::Instance && !UNwiroIKBridge::Instance->IsChatExtensionEnabled(TEXT("elevenlabs")))
			return TEXT("{\"success\":false,\"error\":\"ElevenLabs extension is not enabled for this chat. Enable it from the Extensions menu in the chat input.\"}");

		extern FString LoadNwiroSecret(const FString& Key);
		FString ApiKey = LoadNwiroSecret(TEXT("ELEVENLABS_API_KEY"));
		if (ApiKey.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"ElevenLabs API key not configured. Set it in Settings > Extensions > ElevenLabs.\"}");

		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		FString Search, Category;
		int32 PageSize = 0;
		if (FJsonSerializer::Deserialize(R, Args) && Args.IsValid())
		{
			Args->TryGetStringField(TEXT("search"), Search);
			Args->TryGetStringField(TEXT("category"), Category);
			int32 Tmp; if (Args->TryGetNumberField(TEXT("pageSize"), Tmp)) PageSize = Tmp;
		}
		FString Url = TEXT("https://api.elevenlabs.io/v2/voices");
		TArray<FString> Q;
		if (!Search.IsEmpty())   Q.Add(FString::Printf(TEXT("search=%s"), *FGenericPlatformHttp::UrlEncode(Search)));
		if (!Category.IsEmpty()) Q.Add(FString::Printf(TEXT("category=%s"), *Category));
		if (PageSize > 0)        Q.Add(FString::Printf(TEXT("page_size=%d"), PageSize));
		if (Q.Num() > 0) Url += TEXT("?") + FString::Join(Q, TEXT("&"));

		auto HttpReq = FHttpModule::Get().CreateRequest();
		HttpReq->SetURL(Url);
		HttpReq->SetVerb(TEXT("GET"));
		HttpReq->SetHeader(TEXT("xi-api-key"), ApiKey);

		// Synchronous-ish: spin a brief loop on the request. Voices list is
		// typically <50KB and returns in <500ms, so a 5s budget is plenty.
		bool bDone = false;
		FString ResponseBody;
		int32 ResponseStatus = 0;
		HttpReq->OnProcessRequestComplete().BindLambda(
			[&bDone, &ResponseBody, &ResponseStatus](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
			{
				if (bOk && Resp.IsValid()) { ResponseBody = Resp->GetContentAsString(); ResponseStatus = Resp->GetResponseCode(); }
				bDone = true;
			});
		HttpReq->ProcessRequest();
		const double Deadline = FPlatformTime::Seconds() + 5.0;
		while (!bDone && FPlatformTime::Seconds() < Deadline) { FPlatformProcess::Sleep(0.02f); FHttpModule::Get().GetHttpManager().Tick(0.02f); }
		if (!bDone) return TEXT("{\"success\":false,\"error\":\"ElevenLabs voices request timed out\"}");
		if (ResponseStatus < 200 || ResponseStatus >= 300)
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"ElevenLabs %d\",\"raw\":%s}"),
				ResponseStatus, *(ResponseBody.Left(400).Replace(TEXT("\""), TEXT("\\\""))));
		// Wrap upstream JSON inside success envelope so the LLM sees a uniform shape.
		return FString::Printf(TEXT("{\"success\":true,\"voices_response\":%s}"), *ResponseBody);
	}

	// === ElevenLabs: TTS / SFX / Music (async, binary audio → save to disk + event) ===
	auto RunElevenLabsAudio = [&ToolName, &ArgsJson](const FString& Kind) -> FString
	{
		if (UNwiroIKBridge::Instance && !UNwiroIKBridge::Instance->IsChatExtensionEnabled(TEXT("elevenlabs")))
			return TEXT("{\"success\":false,\"error\":\"ElevenLabs extension is not enabled for this chat. Enable it from the Extensions menu in the chat input.\"}");

		extern FString LoadNwiroSecret(const FString& Key);
		FString ApiKey = LoadNwiroSecret(TEXT("ELEVENLABS_API_KEY"));
		if (ApiKey.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"ElevenLabs API key not configured. Set it in Settings > Extensions > ElevenLabs.\"}");

		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(R, Args) || !Args.IsValid())
			return TEXT("{\"success\":false,\"error\":\"Invalid arguments\"}");

		// Build upstream URL + body per tool.
		FString Url;
		TSharedRef<FJsonObject> Body = MakeShareable(new FJsonObject);
		FString HumanPrompt;  // for event payload
		if (Kind == TEXT("voice"))
		{
			FString Text, VoiceId, ModelId, LangCode;
			Args->TryGetStringField(TEXT("text"), Text);
			Args->TryGetStringField(TEXT("voiceId"), VoiceId);
			Args->TryGetStringField(TEXT("modelId"), ModelId);
			Args->TryGetStringField(TEXT("languageCode"), LangCode);
			if (Text.IsEmpty() || VoiceId.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"text + voiceId required\"}");
			Url = FString::Printf(TEXT("https://api.elevenlabs.io/v1/text-to-speech/%s"), *FGenericPlatformHttp::UrlEncode(VoiceId));
			Body->SetStringField(TEXT("text"), Text);
			Body->SetStringField(TEXT("model_id"), ModelId.IsEmpty() ? TEXT("eleven_multilingual_v2") : ModelId);
			if (!LangCode.IsEmpty()) Body->SetStringField(TEXT("language_code"), LangCode);
			HumanPrompt = Text.Left(120);
		}
		else if (Kind == TEXT("sfx"))
		{
			FString Prompt; double DurationSeconds = 0.0; double PromptInfluence = -1.0; bool bLoop = false;
			Args->TryGetStringField(TEXT("prompt"), Prompt);
			Args->TryGetNumberField(TEXT("durationSeconds"), DurationSeconds);
			Args->TryGetNumberField(TEXT("promptInfluence"), PromptInfluence);
			Args->TryGetBoolField(TEXT("loop"), bLoop);
			if (Prompt.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"prompt required\"}");
			Url = TEXT("https://api.elevenlabs.io/v1/sound-generation");
			Body->SetStringField(TEXT("text"), Prompt);
			if (DurationSeconds > 0) Body->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
			if (PromptInfluence >= 0) Body->SetNumberField(TEXT("prompt_influence"), PromptInfluence);
			if (bLoop) Body->SetBoolField(TEXT("loop"), true);
			HumanPrompt = Prompt.Left(120);
		}
		else if (Kind == TEXT("music"))
		{
			FString Prompt; int32 DurationMs = 0; bool bInstrumental = false;
			Args->TryGetStringField(TEXT("prompt"), Prompt);
			Args->TryGetNumberField(TEXT("durationMs"), DurationMs);
			Args->TryGetBoolField(TEXT("instrumental"), bInstrumental);
			if (Prompt.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"prompt required\"}");
			Url = TEXT("https://api.elevenlabs.io/v1/music");
			Body->SetStringField(TEXT("prompt"), Prompt);
			if (DurationMs > 0) Body->SetNumberField(TEXT("music_length_ms"), DurationMs);
			if (bInstrumental) Body->SetBoolField(TEXT("force_instrumental"), true);
			HumanPrompt = Prompt.Left(120);
		}
		else
		{
			return TEXT("{\"success\":false,\"error\":\"Unknown elevenlabs kind\"}");
		}

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Wr = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body, Wr);

		auto HttpReq = FHttpModule::Get().CreateRequest();
		HttpReq->SetURL(Url);
		HttpReq->SetVerb(TEXT("POST"));
		HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		HttpReq->SetHeader(TEXT("xi-api-key"), ApiKey);
		HttpReq->SetContentAsString(BodyStr);

		FString CapturedKind = Kind;
		FString CapturedPrompt = HumanPrompt;
		HttpReq->OnProcessRequestComplete().BindLambda(
			[CapturedKind, CapturedPrompt](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
			{
				auto PushError = [&CapturedKind](const FString& Msg)
				{
					if (UNwiroIKBridge::Instance)
					{
						const FString Esc = Msg.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
						const FString Data = FString::Printf(TEXT("{\"provider\":\"elevenlabs\",\"kind\":\"%s\",\"message\":\"%s\"}"), *CapturedKind, *Esc);
						UNwiroIKBridge::Instance->PushEvent(TEXT("elevenlabs_error"), Data);
					}
				};
				if (!bOk || !Resp.IsValid()) { PushError(TEXT("Network error contacting ElevenLabs")); return; }
				const int32 Status = Resp->GetResponseCode();
				if (Status < 200 || Status >= 300)
				{
					PushError(FString::Printf(TEXT("ElevenLabs %d: %s"), Status, *Resp->GetContentAsString().Left(200)));
					return;
				}
				const TArray<uint8>& Bytes = Resp->GetContent();
				if (Bytes.Num() == 0) { PushError(TEXT("ElevenLabs returned empty audio")); return; }
				const FString B64 = FBase64::Encode(Bytes);
				if (UNwiroIKBridge::Instance)
				{
					const FString EscPrompt = CapturedPrompt.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
					const FString Data = FString::Printf(
						TEXT("{\"kind\":\"%s\",\"prompt\":\"%s\",\"ext\":\"mp3\",\"bytes\":%d,\"b64\":\"%s\"}"),
						*CapturedKind, *EscPrompt, Bytes.Num(), *B64);
					UNwiroIKBridge::Instance->PushEvent(TEXT("elevenlabs_task"), Data);
				}
			});

		HttpReq->ProcessRequest();
		return FString::Printf(
			TEXT("{\"success\":true,\"status\":\"submitted\",\"kind\":\"%s\",\"message\":\"ElevenLabs %s generation submitted. The audio will appear in the ElevenLabs side panel where the user can preview and import. Do NOT tell the user the audio is ready yet — they will see it in the side panel.\"}"),
			*Kind, *Kind);
	};

	if (ToolName == TEXT("generate_voice_elevenlabs")) return RunElevenLabsAudio(TEXT("voice"));
	if (ToolName == TEXT("generate_sfx_elevenlabs"))   return RunElevenLabsAudio(TEXT("sfx"));
	if (ToolName == TEXT("generate_music_elevenlabs")) return RunElevenLabsAudio(TEXT("music"));

	// === fal.ai material generation ===
	// Posts to https://fal.run/fal-ai/patina/material with the user's FAL_KEY,
	// then downloads each image in the response and pushes a fal_task event
	// with the maps (base64) so the panel can preview + import as UTexture2D.
	// === fal.ai material generation — meshy pattern ===
	// MCP server hits fal.run directly with the user's FAL_KEY, parses the
	// response (URLs only, not bytes), and pushes a fal_urls event so the
	// panel can render previews via <img src> (proxied through the backend
	// for same-origin). MCP runs standalone — does not depend on the panel
	// being open.
	if (ToolName == TEXT("generate_material_fal"))
	{
		if (UNwiroIKBridge::Instance && !UNwiroIKBridge::Instance->IsChatExtensionEnabled(TEXT("fal")))
			return TEXT("{\"success\":false,\"error\":\"fal.ai extension is not enabled for this chat. Enable it from the Extensions menu in the chat input.\"}");

		extern FString LoadNwiroSecret(const FString& Key);
		FString ApiKey = LoadNwiroSecret(TEXT("FAL_KEY"));
		if (ApiKey.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"fal.ai API key not configured. Set it in Settings > Extensions > fal.ai.\"}");

		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> ArgsReader = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(ArgsReader, Args) || !Args.IsValid())
			return TEXT("{\"success\":false,\"error\":\"Invalid arguments JSON\"}");

		FString Prompt;
		Args->TryGetStringField(TEXT("prompt"), Prompt);
		if (Prompt.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"prompt required\"}");

		const FString TaskId = FString::Printf(TEXT("fal_%lld_%d"), FDateTime::UtcNow().ToUnixTimestamp(), FMath::RandRange(0, 99999));

		// Pending event so the panel renders a card while we wait on fal.run.
		if (UNwiroIKBridge::Instance)
		{
			const FString EscPrompt = Prompt.Left(160).Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
			UNwiroIKBridge::Instance->PushEvent(TEXT("fal_pending"),
				FString::Printf(TEXT("{\"id\":\"%s\",\"prompt\":\"%s\"}"), *TaskId, *EscPrompt));
		}

		// Build upstream body (snake_case for fal).
		TSharedRef<FJsonObject> Body = MakeShareable(new FJsonObject);
		Body->SetStringField(TEXT("prompt"), Prompt);
		FString ImageUrl; if (Args->TryGetStringField(TEXT("imageUrl"), ImageUrl) && !ImageUrl.IsEmpty()) Body->SetStringField(TEXT("image_url"), ImageUrl);
		double NumD = 0;
		if (Args->TryGetNumberField(TEXT("strength"), NumD)) Body->SetNumberField(TEXT("strength"), NumD);
		if (Args->TryGetNumberField(TEXT("numInferenceSteps"), NumD)) Body->SetNumberField(TEXT("num_inference_steps"), NumD);
		if (Args->TryGetNumberField(TEXT("numImages"), NumD)) Body->SetNumberField(TEXT("num_images"), NumD);
		if (Args->TryGetNumberField(TEXT("seed"), NumD)) Body->SetNumberField(TEXT("seed"), NumD);
		if (Args->TryGetNumberField(TEXT("upscaleFactor"), NumD)) Body->SetNumberField(TEXT("upscale_factor"), NumD);
		const TArray<TSharedPtr<FJsonValue>>* MapsArr = nullptr;
		if (Args->TryGetArrayField(TEXT("maps"), MapsArr) && MapsArr) Body->SetArrayField(TEXT("maps"), *MapsArr);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Wr = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body, Wr);

		auto HttpReq = FHttpModule::Get().CreateRequest();
		HttpReq->SetURL(TEXT("https://fal.run/fal-ai/patina/material"));
		HttpReq->SetVerb(TEXT("POST"));
		HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		HttpReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Key %s"), *ApiKey));
		HttpReq->SetContentAsString(BodyStr);

		FString CapturedTaskId = TaskId;
		HttpReq->OnProcessRequestComplete().BindLambda(
			[CapturedTaskId](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
			{
				auto PushError = [&CapturedTaskId](const FString& Msg)
				{
					UE_LOG(LogTemp, Warning, TEXT("[Nwiro][fal] error: %s"), *Msg);
					if (UNwiroIKBridge::Instance)
					{
						const FString Esc = Msg.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
						UNwiroIKBridge::Instance->PushEvent(TEXT("fal_error"),
							FString::Printf(TEXT("{\"id\":\"%s\",\"message\":\"%s\"}"), *CapturedTaskId, *Esc));
					}
				};
				if (!bOk || !Resp.IsValid()) { PushError(TEXT("Network error contacting fal.ai")); return; }
				const int32 Status = Resp->GetResponseCode();
				if (Status < 200 || Status >= 300)
				{
					PushError(FString::Printf(TEXT("fal.ai %d: %s"), Status, *Resp->GetContentAsString().Left(200)));
					return;
				}

				TSharedPtr<FJsonObject> RespJson;
				TSharedRef<TJsonReader<>> RespReader = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
				if (!FJsonSerializer::Deserialize(RespReader, RespJson) || !RespJson.IsValid())
				{
					PushError(TEXT("fal.ai returned non-JSON")); return;
				}
				const TArray<TSharedPtr<FJsonValue>>* Images = nullptr;
				if (!RespJson->TryGetArrayField(TEXT("images"), Images) || !Images || Images->Num() == 0)
				{
					PushError(TEXT("fal.ai response had no images")); return;
				}

				// Push URLs only — small payload, no bytes through CEF bridge.
				FString UrlsJson = TEXT("[");
				bool bFirst = true;
				for (const TSharedPtr<FJsonValue>& V : *Images)
				{
					const TSharedPtr<FJsonObject> Obj = V->AsObject();
					if (!Obj.IsValid()) continue;
					FString Url; if (!Obj->TryGetStringField(TEXT("url"), Url) || Url.IsEmpty()) continue;
					FString MapType; Obj->TryGetStringField(TEXT("map_type"), MapType);
					FString CType;   Obj->TryGetStringField(TEXT("content_type"), CType);
					const FString Ext = CType.Contains(TEXT("jpeg")) || CType.Contains(TEXT("jpg")) ? TEXT("jpg")
									   : CType.Contains(TEXT("webp")) ? TEXT("webp") : TEXT("png");
					const FString EscUrl = Url.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
					const FString EscType = MapType.Replace(TEXT("\""), TEXT("\\\""));
					if (!bFirst) UrlsJson += TEXT(",");
					UrlsJson += FString::Printf(TEXT("{\"mapType\":\"%s\",\"ext\":\"%s\",\"url\":\"%s\"}"), *EscType, *Ext, *EscUrl);
					bFirst = false;
				}
				UrlsJson += TEXT("]");
				if (UNwiroIKBridge::Instance)
				{
					UNwiroIKBridge::Instance->PushEvent(TEXT("fal_urls"),
						FString::Printf(TEXT("{\"id\":\"%s\",\"urls\":%s}"), *CapturedTaskId, *UrlsJson));
				}
			});

		HttpReq->ProcessRequest();
		return FString::Printf(TEXT("{\"success\":true,\"status\":\"submitted\",\"id\":\"%s\",\"message\":\"fal.ai material generation submitted. The maps will appear in the fal.ai side panel where the user can preview and import. Do NOT tell the user the textures are ready yet — they will see them in the side panel.\"}"), *TaskId);
	}

	// Not a built-in tool — try registered third-party providers before failing.
	{
		FString ProviderResult;
		if (FNwiroIKToolRegistry::TryDispatch(ToolName, ArgsJson, ProviderResult))
		{
			return ProviderResult;
		}
	}

	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Unknown tool: %s\"}"), *ToolName);
}

// ============================================================
// Helper functions
// ============================================================

FString FNwiroIKMCPServer::MakeJsonRpcResponse(const FString& Id, const TSharedPtr<FJsonObject>& Result)
{
	FString ResultJson;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ResultJson);
	FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);

	return FString::Printf(TEXT("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}"), *Id, *ResultJson);
}

FString FNwiroIKMCPServer::MakeJsonRpcError(const FString& Id, int32 Code, const FString& Message)
{
	return FString::Printf(TEXT("{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}"),
		*Id, Code, *Message);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/NwiroIKMCPAnnotationsSpec.inl"
#endif
