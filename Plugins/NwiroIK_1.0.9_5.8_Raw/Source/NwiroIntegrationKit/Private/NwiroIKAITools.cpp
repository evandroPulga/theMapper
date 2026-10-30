// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKAITools.h"
#include "NwiroIKTransactionHelper.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
// BlackboardDataFactory not available in all UE versions, use AssetTools directly
#include "IPythonScriptPlugin.h"
#include "Json.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroAI, Log, All);

namespace
{
	// LLM-friendly arg picker: accept multiple aliases agents naturally use.
	FString PickAssetPath(const TSharedPtr<FJsonObject>& Cmd)
	{
		for (const TCHAR* Key : { TEXT("path"), TEXT("assetPath"), TEXT("blueprint"),
			TEXT("treePath"), TEXT("stateTree"), TEXT("blackboard"),
			TEXT("behaviorTree"), TEXT("bbPath"), TEXT("btPath"), TEXT("stPath") })
		{
			FString V;
			if (Cmd->TryGetStringField(Key, V) && !V.IsEmpty()) return V;
		}
		return FString();
	}

	bool PickArrayField(const TSharedPtr<FJsonObject>& Cmd,
		std::initializer_list<const TCHAR*> Keys,
		const TArray<TSharedPtr<FJsonValue>>*& Out)
	{
		for (const TCHAR* K : Keys)
		{
			if (Cmd->TryGetArrayField(K, Out)) return true;
		}
		return false;
	}
}

// ============================================================
// CREATE BEHAVIOR TREE
// ============================================================

FString FNwiroIKAITools::CreateBehaviorTree(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	FString BBPath = Cmd->GetStringField(TEXT("blackboard"));

	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/AI");

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateBehaviorTree", "AI: Create BehaviorTree"));

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* NewAsset = AssetTools.CreateAsset(Name, Path, UBehaviorTree::StaticClass(), nullptr);
	UBehaviorTree* BT = Cast<UBehaviorTree>(NewAsset);

	if (!BT)
	{
		Tx.Cancel();
		return TEXT("{\"success\":false,\"error\":\"Failed to create BehaviorTree\"}");
	}
	Tx.AlsoModify(BT);

	// Link blackboard if provided
	if (!BBPath.IsEmpty())
	{
		UObject* BBAsset = UEditorAssetLibrary::LoadAsset(BBPath);
		UBlackboardData* BB = Cast<UBlackboardData>(BBAsset);
		if (!BB)
		{
			// Search by name
			FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			FARFilter Filter;
			Filter.ClassPaths.Add(UBlackboardData::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths = true;
			Filter.PackagePaths.Add(TEXT("/Game"));
			TArray<FAssetData> Assets;
			ARM.Get().GetAssets(Filter, Assets);
			for (const FAssetData& A : Assets)
			{
				if (A.AssetName.ToString().Contains(BBPath, ESearchCase::IgnoreCase))
				{
					BB = Cast<UBlackboardData>(A.GetAsset());
					break;
				}
			}
		}
		if (BB)
		{
			BT->BlackboardAsset = BB;
		}
	}

	BT->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"blackboard\":\"%s\"}"),
		*Name, *BT->GetPathName(),
		BT->BlackboardAsset ? *BT->BlackboardAsset->GetName() : TEXT("None"));
}

// ============================================================
// READ BEHAVIOR TREE
// ============================================================

FString FNwiroIKAITools::ReadBehaviorTree(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = PickAssetPath(Cmd);
	UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
	UBehaviorTree* BT = Cast<UBehaviorTree>(Asset);
	if (!BT)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"BehaviorTree not found: %s\"}"), *Path);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), BT->GetName());
	Result->SetStringField(TEXT("path"), BT->GetPathName());
	Result->SetStringField(TEXT("blackboard"), BT->BlackboardAsset ? BT->BlackboardAsset->GetPathName() : TEXT("None"));

	// Read tree structure recursively
	TFunction<TSharedPtr<FJsonObject>(UBTCompositeNode*)> SerializeNode;
	SerializeNode = [&](UBTCompositeNode* Node) -> TSharedPtr<FJsonObject>
	{
		if (!Node) return nullptr;

		TSharedPtr<FJsonObject> NodeObj = MakeShareable(new FJsonObject());
		NodeObj->SetStringField(TEXT("name"), Node->GetNodeName());
		NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());

		// Children
		TArray<TSharedPtr<FJsonValue>> Children;
		for (int32 i = 0; i < Node->Children.Num(); i++)
		{
			const FBTCompositeChild& Child = Node->Children[i];
			TSharedPtr<FJsonObject> ChildObj = MakeShareable(new FJsonObject());

			if (Child.ChildTask)
			{
				ChildObj->SetStringField(TEXT("name"), Child.ChildTask->GetNodeName());
				ChildObj->SetStringField(TEXT("class"), Child.ChildTask->GetClass()->GetName());
				ChildObj->SetStringField(TEXT("type"), TEXT("Task"));
			}
			else if (Child.ChildComposite)
			{
				ChildObj = SerializeNode(Child.ChildComposite);
				if (ChildObj.IsValid())
					ChildObj->SetStringField(TEXT("type"), TEXT("Composite"));
			}

			// Decorators
			TArray<TSharedPtr<FJsonValue>> Decorators;
			for (UBTDecorator* Dec : Child.Decorators)
			{
				if (!Dec) continue;
				TSharedRef<FJsonObject> D = MakeShareable(new FJsonObject());
				D->SetStringField(TEXT("name"), Dec->GetNodeName());
				D->SetStringField(TEXT("class"), Dec->GetClass()->GetName());
				Decorators.Add(MakeShareable(new FJsonValueObject(D)));
			}
			if (Decorators.Num() > 0 && ChildObj.IsValid())
				ChildObj->SetArrayField(TEXT("decorators"), Decorators);

			if (ChildObj.IsValid())
				Children.Add(MakeShareable(new FJsonValueObject(ChildObj)));
		}
		NodeObj->SetArrayField(TEXT("children"), Children);

		// Services
		TArray<TSharedPtr<FJsonValue>> Services;
		for (UBTService* Svc : Node->Services)
		{
			if (!Svc) continue;
			TSharedRef<FJsonObject> S = MakeShareable(new FJsonObject());
			S->SetStringField(TEXT("name"), Svc->GetNodeName());
			S->SetStringField(TEXT("class"), Svc->GetClass()->GetName());
			Services.Add(MakeShareable(new FJsonValueObject(S)));
		}
		if (Services.Num() > 0)
			NodeObj->SetArrayField(TEXT("services"), Services);

		return NodeObj;
	};

	if (BT->RootNode)
	{
		TSharedPtr<FJsonObject> TreeObj = SerializeNode(BT->RootNode);
		if (TreeObj.IsValid())
			Result->SetObjectField(TEXT("root"), TreeObj);
	}

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// CREATE BLACKBOARD
// ============================================================

FString FNwiroIKAITools::CreateBlackboard(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/AI");

	FString FullBBPath = Path / Name;

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateBlackboard", "AI: Create Blackboard"));

	UPackage* BBPackage = CreatePackage(*FullBBPath);
	if (!BBPackage)
	{
		Tx.Cancel();
		return TEXT("{\"success\":false,\"error\":\"Failed to create package\"}");
	}
	Tx.AlsoModify(BBPackage);

	UObject* NewAsset = NewObject<UBlackboardData>(BBPackage, FName(*Name), RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(NewAsset);
	UBlackboardData* BB = Cast<UBlackboardData>(NewAsset);

	if (!BB)
	{
		Tx.Cancel();
		return TEXT("{\"success\":false,\"error\":\"Failed to create Blackboard\"}");
	}
	Tx.AlsoModify(BB);

	// Add keys
	const TArray<TSharedPtr<FJsonValue>>* Keys;
	int32 KeyCount = 0;
	if (Cmd->TryGetArrayField(TEXT("keys"), Keys))
	{
		for (const TSharedPtr<FJsonValue>& KeyVal : *Keys)
		{
			const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();
			if (!KeyObj.IsValid()) continue;

			FString KeyName = KeyObj->GetStringField(TEXT("name"));
			FString KeyType = KeyObj->GetStringField(TEXT("type")).ToLower();

			if (KeyName.IsEmpty()) continue;

			FBlackboardEntry NewKey;
			NewKey.EntryName = FName(*KeyName);

			if (KeyType == TEXT("bool"))
				NewKey.KeyType = NewObject<UBlackboardKeyType_Bool>(BB);
			else if (KeyType == TEXT("int") || KeyType == TEXT("integer"))
				NewKey.KeyType = NewObject<UBlackboardKeyType_Int>(BB);
			else if (KeyType == TEXT("float"))
				NewKey.KeyType = NewObject<UBlackboardKeyType_Float>(BB);
			else if (KeyType == TEXT("string"))
				NewKey.KeyType = NewObject<UBlackboardKeyType_String>(BB);
			else if (KeyType == TEXT("name"))
				NewKey.KeyType = NewObject<UBlackboardKeyType_Name>(BB);
			else if (KeyType == TEXT("vector"))
				NewKey.KeyType = NewObject<UBlackboardKeyType_Vector>(BB);
			else if (KeyType == TEXT("rotator"))
				NewKey.KeyType = NewObject<UBlackboardKeyType_Rotator>(BB);
			else if (KeyType == TEXT("object"))
				NewKey.KeyType = NewObject<UBlackboardKeyType_Object>(BB);
			else if (KeyType == TEXT("class"))
				NewKey.KeyType = NewObject<UBlackboardKeyType_Class>(BB);
			else if (KeyType == TEXT("enum"))
				NewKey.KeyType = NewObject<UBlackboardKeyType_Enum>(BB);
			else
				NewKey.KeyType = NewObject<UBlackboardKeyType_Object>(BB); // default

			BB->Keys.Add(NewKey);
			KeyCount++;
		}
	}

	BB->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"keyCount\":%d}"),
		*Name, *BB->GetPathName(), KeyCount);
}

// ============================================================
// EDIT BLACKBOARD
// ============================================================

FString FNwiroIKAITools::EditBlackboard(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = PickAssetPath(Cmd);
	UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
	UBlackboardData* BB = Cast<UBlackboardData>(Asset);
	if (!BB)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blackboard not found: %s\"}"), *Path);

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "EditBlackboard", "AI: Edit Blackboard"), BB);

	int32 Added = 0, Removed = 0;

	// Remove keys
	const TArray<TSharedPtr<FJsonValue>>* RemoveArr = nullptr;
	if (PickArrayField(Cmd, { TEXT("remove_keys"), TEXT("removeKeys"), TEXT("remove") }, RemoveArr))
	{
		for (const TSharedPtr<FJsonValue>& Val : *RemoveArr)
		{
			FString KeyName = Val->AsString();
			BB->Keys.RemoveAll([&](const FBlackboardEntry& E) {
				if (E.EntryName == FName(*KeyName)) { Removed++; return true; }
				return false;
			});
		}
	}

	// Add keys (same logic as CreateBlackboard)
	const TArray<TSharedPtr<FJsonValue>>* AddArr = nullptr;
	if (PickArrayField(Cmd, { TEXT("add_keys"), TEXT("addKeys"), TEXT("add"), TEXT("keys") }, AddArr))
	{
		for (const TSharedPtr<FJsonValue>& KeyVal : *AddArr)
		{
			const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();
			if (!KeyObj.IsValid()) continue;

			FString KeyName = KeyObj->GetStringField(TEXT("name"));
			FString KeyType = KeyObj->GetStringField(TEXT("type")).ToLower();
			if (KeyName.IsEmpty()) continue;

			FBlackboardEntry NewKey;
			NewKey.EntryName = FName(*KeyName);

			if (KeyType == TEXT("bool")) NewKey.KeyType = NewObject<UBlackboardKeyType_Bool>(BB);
			else if (KeyType == TEXT("int")) NewKey.KeyType = NewObject<UBlackboardKeyType_Int>(BB);
			else if (KeyType == TEXT("float")) NewKey.KeyType = NewObject<UBlackboardKeyType_Float>(BB);
			else if (KeyType == TEXT("string")) NewKey.KeyType = NewObject<UBlackboardKeyType_String>(BB);
			else if (KeyType == TEXT("vector")) NewKey.KeyType = NewObject<UBlackboardKeyType_Vector>(BB);
			else if (KeyType == TEXT("rotator")) NewKey.KeyType = NewObject<UBlackboardKeyType_Rotator>(BB);
			else if (KeyType == TEXT("object")) NewKey.KeyType = NewObject<UBlackboardKeyType_Object>(BB);
			else NewKey.KeyType = NewObject<UBlackboardKeyType_Object>(BB);

			BB->Keys.Add(NewKey);
			Added++;
		}
	}

	BB->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"added\":%d,\"removed\":%d,\"totalKeys\":%d}"),
		Added, Removed, BB->Keys.Num());
}

// ============================================================
// ADD BEHAVIOR TREE NODES (via Python bridge)
// ============================================================

FString FNwiroIKAITools::AddBehaviorTreeNodes(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString BTPath = PickAssetPath(Cmd);
	if (BTPath.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'path' (also tried assetPath/blueprint/treePath/behaviorTree)\"}");

	UObject* Asset = UEditorAssetLibrary::LoadAsset(BTPath);
	UBehaviorTree* BT = Cast<UBehaviorTree>(Asset);
	if (!BT)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"BehaviorTree not found: %s\"}"), *BTPath);

	// Optional blackboard binding (LLMs often pass this alongside nodes).
	bool bBoundBlackboard = false;
	FString BBPath;
	if (Cmd->TryGetStringField(TEXT("blackboard"), BBPath) && !BBPath.IsEmpty())
	{
		if (UObject* BBAsset = UEditorAssetLibrary::LoadAsset(BBPath))
		{
			if (UBlackboardData* BB = Cast<UBlackboardData>(BBAsset))
			{
				BT->BlackboardAsset = BB;
				BT->MarkPackageDirty();
				bBoundBlackboard = true;
			}
		}
	}

	// Two argument shapes supported:
	//   1) Batch: nodes:[{ type, parent, ref, name }, ...]  — preferred for LLMs
	//   2) Legacy single: nodeType / nodeClass / parent
	const TArray<TSharedPtr<FJsonValue>>* NodesArr = nullptr;
	int32 Recorded = 0;
	TArray<FString> UnknownTypes;
	TArray<FString> ValidTypes = { TEXT("Selector"), TEXT("Sequence"), TEXT("SimpleParallel"),
		TEXT("MoveTo"), TEXT("Wait"), TEXT("RunBehavior"), TEXT("FinishWithResult"),
		TEXT("PlaySound"), TEXT("RotateToFaceBBEntry"), TEXT("Task"), TEXT("Composite"),
		TEXT("Decorator"), TEXT("Service") };
	auto IsValid = [&](const FString& T) {
		for (const FString& V : ValidTypes) if (V.Equals(T, ESearchCase::IgnoreCase)) return true;
		return false;
	};

	if (PickArrayField(Cmd, { TEXT("nodes"), TEXT("entries"), TEXT("items") }, NodesArr) && NodesArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *NodesArr)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			if (!O.IsValid()) continue;
			FString T = O->GetStringField(TEXT("type"));
			if (T.IsEmpty()) O->TryGetStringField(TEXT("nodeClass"), T);
			if (T.IsEmpty()) continue;
			if (!IsValid(T)) { UnknownTypes.Add(T); continue; }
			Recorded++;
		}
		BT->MarkPackageDirty();

		// Build errors array text (used both for unknown-type signal and as a list).
		FString ErrList;
		for (int32 i = 0; i < UnknownTypes.Num(); i++)
		{
			if (i > 0) ErrList += TEXT(", ");
			ErrList += FString::Printf(TEXT("\"Unknown node type: %s\""), *UnknownTypes[i]);
		}
		// If every spec was unknown (no valid nodes), report graceful failure
		// so callers can detect bad LLM input. Mixed batches stay success.
		const bool bAllUnknown = (Recorded == 0) && (UnknownTypes.Num() > 0);
		return FString::Printf(TEXT("{\"success\":%s,\"recorded\":%d,\"path\":\"%s\",\"errors\":[%s],\"note\":\"Node specs validated and BT package marked dirty. Full graph wiring needs execute_python (BehaviorTreeEditor module is internal).\"}"),
			bAllUnknown ? TEXT("false") : TEXT("true"), Recorded, *BT->GetPathName(), *ErrList);
	}

	// Legacy single-node mode.
	FString NodeClass = Cmd->GetStringField(TEXT("nodeClass"));
	FString NodeType = Cmd->GetStringField(TEXT("nodeType"));
	if (NodeClass.IsEmpty()) NodeClass = NodeType;
	if (NodeClass.IsEmpty())
	{
		// No nodes provided. If we at least bound the blackboard, treat as success.
		if (bBoundBlackboard)
		{
			return FString::Printf(TEXT("{\"success\":true,\"blackboardBound\":\"%s\",\"note\":\"BT updated with blackboard binding (no nodes provided).\"}"),
				*BBPath);
		}
		return TEXT("{\"success\":false,\"error\":\"Missing 'nodeClass' (or pass nodes:[] array)\"}");
	}

	return FString::Printf(TEXT("{\"success\":true,\"recorded\":1,\"nodeClass\":\"%s\",\"note\":\"Use execute_python for full graph manipulation.\"}"),
		*NodeClass);
}
