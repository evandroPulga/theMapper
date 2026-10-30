// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKTools.h"

#if NWIRO_HAS_IK_TOOLS
#include "Rig/IKRigDefinition.h"
#include "Retargeter/IKRetargeter.h"
#include "RigEditor/IKRigDefinitionFactory.h"
#include "RetargetEditor/IKRetargetFactory.h"
#include "RigEditor/IKRigController.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchSchema.h"
#include "PoseSearch/PoseSearchFeatureChannel.h"
#include "PoseSearchDatabaseFactory.h"
#include "PoseSearchSchemaFactory.h"
#include "Animation/Skeleton.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Json.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroIK, Log, All);

// FIKRigController / FIKRetargeterController only exist in UE 5.7+.
// On older engines these tools return an unsupported error. The tool
// definitions are still listed (the LLM sees them) but execution is
// gated here at compile time.

// ============================================================
// HELPERS
// ============================================================

static USkeleton* FindSkeletonByPath(const FString& PathOrName)
{
	UObject* Asset = UEditorAssetLibrary::LoadAsset(PathOrName);
	if (USkeleton* Skel = Cast<USkeleton>(Asset)) return Skel;

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FARFilter Filter;
	Filter.ClassPaths.Add(USkeleton::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(TEXT("/Game"));
	TArray<FAssetData> Assets;
	ARM.Get().GetAssets(Filter, Assets);
	for (const FAssetData& A : Assets)
	{
		if (A.AssetName.ToString().Contains(PathOrName, ESearchCase::IgnoreCase))
		{
			return Cast<USkeleton>(A.GetAsset());
		}
	}
	return nullptr;
}

// ============================================================
// CREATE IK RIG
// ============================================================

FString FNwiroIKTools::CreateIKRig(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	FString SkelPath = Cmd->GetStringField(TEXT("skeleton"));

	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/Animation");

	// Add IK_ prefix if missing
	if (!Name.StartsWith(TEXT("IK_"))) Name = TEXT("IK_") + Name;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UIKRigDefinitionFactory* Factory = NewObject<UIKRigDefinitionFactory>();
	UObject* NewAsset = AssetTools.CreateAsset(Name, Path, UIKRigDefinition::StaticClass(), Factory);
	UIKRigDefinition* IKRig = Cast<UIKRigDefinition>(NewAsset);

	if (!IKRig)
		return TEXT("{\"success\":false,\"error\":\"Failed to create IKRig\"}");

	// Set skeleton via controller
	if (!SkelPath.IsEmpty())
	{
		USkeleton* Skeleton = FindSkeletonByPath(SkelPath);
		if (Skeleton)
		{
			// Use the skeletal mesh from the skeleton's preview mesh
			USkeletalMesh* PreviewMesh = Skeleton->GetPreviewMesh();
			if (PreviewMesh)
			{
				UIKRigController* Controller = UIKRigController::GetController(IKRig);
				if (Controller) Controller->SetSkeletalMesh(PreviewMesh);
			}
		}
	}

	IKRig->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\"}"),
		*Name, *IKRig->GetPathName());
}

// ============================================================
// READ IK RIG
// ============================================================

FString FNwiroIKTools::ReadIKRig(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
	UIKRigDefinition* IKRig = Cast<UIKRigDefinition>(Asset);

	if (!IKRig)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"IKRig not found: %s\"}"), *Path);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), IKRig->GetName());
	Result->SetStringField(TEXT("path"), IKRig->GetPathName());

	UIKRigController* ControllerPtr = UIKRigController::GetController(IKRig);
	UIKRigController& Controller = *ControllerPtr;

	// Skeletal mesh
	USkeletalMesh* Mesh = Controller.GetSkeletalMesh();
	Result->SetStringField(TEXT("skeletalMesh"), Mesh ? Mesh->GetPathName() : TEXT("None"));

	// Goals
	TArray<TSharedPtr<FJsonValue>> Goals;
	for (const UIKRigEffectorGoal* Goal : Controller.GetAllGoals())
	{
		if (!Goal) continue;
		TSharedRef<FJsonObject> G = MakeShareable(new FJsonObject());
		G->SetStringField(TEXT("name"), Goal->GoalName.ToString());
		G->SetStringField(TEXT("boneName"), Goal->BoneName.ToString());
		Goals.Add(MakeShareable(new FJsonValueObject(G)));
	}
	Result->SetArrayField(TEXT("goals"), Goals);

	// Solvers
	TArray<TSharedPtr<FJsonValue>> Solvers;
	for (const FIKRigSolverBase* Solver : Controller.GetSolverArray())
	{
		if (!Solver) continue;
		TSharedRef<FJsonObject> S = MakeShareable(new FJsonObject());
		S->SetStringField(TEXT("name"), Solver->GetNiceName().ToString());
		S->SetStringField(TEXT("class"), Solver->GetSolverSettingsType() ? Solver->GetSolverSettingsType()->GetName() : TEXT("Unknown"));
		S->SetBoolField(TEXT("enabled"), Solver->IsEnabled());
		Solvers.Add(MakeShareable(new FJsonValueObject(S)));
	}
	Result->SetArrayField(TEXT("solvers"), Solvers);

	// Retarget chains
	TArray<TSharedPtr<FJsonValue>> Chains;
	for (const FBoneChain& Chain : Controller.GetRetargetChains())
	{
		TSharedRef<FJsonObject> C = MakeShareable(new FJsonObject());
		C->SetStringField(TEXT("name"), Chain.ChainName.ToString());
		C->SetStringField(TEXT("startBone"), Chain.StartBone.BoneName.ToString());
		C->SetStringField(TEXT("endBone"), Chain.EndBone.BoneName.ToString());
		Chains.Add(MakeShareable(new FJsonValueObject(C)));
	}
	Result->SetArrayField(TEXT("retargetChains"), Chains);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// ADD IK GOAL
// ============================================================

FString FNwiroIKTools::AddIKGoal(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString RigPath = Cmd->GetStringField(TEXT("rig"));
	FString GoalName = Cmd->GetStringField(TEXT("name"));
	FString BoneName = Cmd->GetStringField(TEXT("bone"));

	UObject* Asset = UEditorAssetLibrary::LoadAsset(RigPath);
	UIKRigDefinition* IKRig = Cast<UIKRigDefinition>(Asset);
	if (!IKRig)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"IKRig not found: %s\"}"), *RigPath);

	UIKRigController* ControllerPtr = UIKRigController::GetController(IKRig);
	UIKRigController& Controller = *ControllerPtr;
	FName GoalResult = Controller.AddNewGoal(FName(*GoalName), FName(*BoneName));

	if (GoalResult == NAME_None)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to add goal '%s' on bone '%s'\"}"), *GoalName, *BoneName);

	IKRig->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"goal\":\"%s\",\"bone\":\"%s\"}"), *GoalName, *BoneName);
}

// ============================================================
// CREATE IK RETARGETER
// ============================================================

FString FNwiroIKTools::CreateIKRetargeter(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	FString SourceIKRigPath = Cmd->GetStringField(TEXT("sourceIKRig"));
	FString TargetIKRigPath = Cmd->GetStringField(TEXT("targetIKRig"));

	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/Animation");

	// Add RTG_ prefix if missing
	if (!Name.StartsWith(TEXT("RTG_"))) Name = TEXT("RTG_") + Name;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UIKRetargetFactory* Factory = NewObject<UIKRetargetFactory>();
	UObject* NewAsset = AssetTools.CreateAsset(Name, Path, UIKRetargeter::StaticClass(), Factory);
	UIKRetargeter* Retargeter = Cast<UIKRetargeter>(NewAsset);

	if (!Retargeter)
		return TEXT("{\"success\":false,\"error\":\"Failed to create IKRetargeter\"}");

	// Set source and target IK Rigs
	UIKRetargeterController* ControllerPtr = UIKRetargeterController::GetController(Retargeter);
	UIKRetargeterController& Controller = *ControllerPtr;

	if (!SourceIKRigPath.IsEmpty())
	{
		UIKRigDefinition* SourceRig = Cast<UIKRigDefinition>(UEditorAssetLibrary::LoadAsset(SourceIKRigPath));
		if (SourceRig)
		{
			Controller.SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
		}
	}

	if (!TargetIKRigPath.IsEmpty())
	{
		UIKRigDefinition* TargetRig = Cast<UIKRigDefinition>(UEditorAssetLibrary::LoadAsset(TargetIKRigPath));
		if (TargetRig)
		{
			Controller.SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);
		}
	}

	Retargeter->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\"}"),
		*Name, *Retargeter->GetPathName());
}

// ============================================================
// READ IK RETARGETER
// ============================================================

FString FNwiroIKTools::ReadIKRetargeter(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
	UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Asset);

	if (!Retargeter)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"IKRetargeter not found: %s\"}"), *Path);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Retargeter->GetName());
	Result->SetStringField(TEXT("path"), Retargeter->GetPathName());

	UIKRetargeterController* ControllerPtr = UIKRetargeterController::GetController(Retargeter);
	UIKRetargeterController& Controller = *ControllerPtr;

	// Source/Target IK Rigs
	const UIKRigDefinition* SourceRig = Controller.GetIKRig(ERetargetSourceOrTarget::Source);
	const UIKRigDefinition* TargetRig = Controller.GetIKRig(ERetargetSourceOrTarget::Target);
	Result->SetStringField(TEXT("sourceIKRig"), SourceRig ? SourceRig->GetPathName() : TEXT("None"));
	Result->SetStringField(TEXT("targetIKRig"), TargetRig ? TargetRig->GetPathName() : TEXT("None"));

	// Chain mappings
	TArray<TSharedPtr<FJsonValue>> Mappings;
	const FRetargetChainMapping* ChainMapping = Controller.GetChainMapping(NAME_None);
	if (ChainMapping)
	{
		for (const FRetargetChainPair& Pair : ChainMapping->GetChainPairs())
		{
			TSharedRef<FJsonObject> M = MakeShareable(new FJsonObject());
			M->SetStringField(TEXT("sourceChain"), Pair.SourceChainName.ToString());
			M->SetStringField(TEXT("targetChain"), Pair.TargetChainName.ToString());
			Mappings.Add(MakeShareable(new FJsonValueObject(M)));
		}
	}
	Result->SetArrayField(TEXT("chainMappings"), Mappings);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// CREATE POSE SEARCH SCHEMA
// ============================================================

FString FNwiroIKTools::CreatePoseSearchSchema(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	FString SkelPath = Cmd->GetStringField(TEXT("skeleton"));

	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/Animation/MotionMatching");

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UPoseSearchSchemaFactory* Factory = NewObject<UPoseSearchSchemaFactory>();
	UObject* NewAsset = AssetTools.CreateAsset(Name, Path, UPoseSearchSchema::StaticClass(), Factory);
	UPoseSearchSchema* Schema = Cast<UPoseSearchSchema>(NewAsset);

	if (!Schema)
		return TEXT("{\"success\":false,\"error\":\"Failed to create PoseSearchSchema\"}");

	// Set skeleton
	if (!SkelPath.IsEmpty())
	{
		USkeleton* Skeleton = FindSkeletonByPath(SkelPath);
		if (Skeleton)
		{
				Schema->AddSkeleton(Skeleton);
		}
	}

	Schema->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\"}"),
		*Name, *Schema->GetPathName());
}

// ============================================================
// CREATE POSE SEARCH DATABASE
// ============================================================

FString FNwiroIKTools::CreatePoseSearchDatabase(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	FString SchemaPath = Cmd->GetStringField(TEXT("schema"));

	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/Animation/MotionMatching");

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UPoseSearchDatabaseFactory* Factory = NewObject<UPoseSearchDatabaseFactory>();
	UObject* NewAsset = AssetTools.CreateAsset(Name, Path, UPoseSearchDatabase::StaticClass(), Factory);
	UPoseSearchDatabase* DB = Cast<UPoseSearchDatabase>(NewAsset);

	if (!DB)
		return TEXT("{\"success\":false,\"error\":\"Failed to create PoseSearchDatabase\"}");

	// Link schema
	if (!SchemaPath.IsEmpty())
	{
		UPoseSearchSchema* Schema = Cast<UPoseSearchSchema>(UEditorAssetLibrary::LoadAsset(SchemaPath));
		if (Schema)
		{
				DB->Schema = Schema;
		}
	}

	DB->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\"}"),
		*Name, *DB->GetPathName());
}

// ============================================================
// READ POSE SEARCH DATABASE
// ============================================================

FString FNwiroIKTools::ReadPoseSearchDatabase(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
	UPoseSearchDatabase* DB = Cast<UPoseSearchDatabase>(Asset);

	if (!DB)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"PoseSearchDatabase not found: %s\"}"), *Path);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), DB->GetName());
	Result->SetStringField(TEXT("path"), DB->GetPathName());
	Result->SetStringField(TEXT("schema"), DB->Schema ? DB->Schema->GetPathName() : TEXT("None"));

	// Animation assets in database
	TArray<TSharedPtr<FJsonValue>> Anims;
	const int32 NumAnims = DB->GetNumAnimationAssets();
	for (int32 i = 0; i < NumAnims; ++i)
	{
		const FPoseSearchDatabaseAnimationAsset* AnimAsset = DB->GetDatabaseAnimationAsset(i);
		if (!AnimAsset) continue;
		TSharedRef<FJsonObject> A = MakeShareable(new FJsonObject());
		UObject* AnimObj = AnimAsset->AnimAsset.Get();
		A->SetStringField(TEXT("asset"), AnimObj ? AnimObj->GetPathName() : TEXT("None"));
		Anims.Add(MakeShareable(new FJsonValueObject(A)));
	}
	Result->SetArrayField(TEXT("animations"), Anims);
	Result->SetNumberField(TEXT("animationCount"), Anims.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// ADD IK SOLVER
// ============================================================

FString FNwiroIKTools::AddIKSolver(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString RigPath = Cmd->GetStringField(TEXT("rig"));
	FString SolverType = Cmd->GetStringField(TEXT("type"));

	UObject* Asset = UEditorAssetLibrary::LoadAsset(RigPath);
	UIKRigDefinition* IKRig = Cast<UIKRigDefinition>(Asset);
	if (!IKRig)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"IKRig not found: %s\"}"), *RigPath);

	UIKRigController* ControllerPtr = UIKRigController::GetController(IKRig);
	UIKRigController& Controller = *ControllerPtr;

	// Find solver class by iterating registered types
	UClass* SolverClass = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->IsChildOf(UIKRigSolver::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
		{
			if (It->GetName().Contains(SolverType, ESearchCase::IgnoreCase))
			{
				SolverClass = *It;
				break;
			}
		}
	}

	if (!SolverClass)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Solver type not found: %s. Try: FBIKSolver, LimbSolver, SetTransform, PoleSolver\"}"), *SolverType);

	int32 SolverIndex = Controller.AddSolver(SolverClass->GetPathName());
	if (SolverIndex == INDEX_NONE)
		return TEXT("{\"success\":false,\"error\":\"Failed to add solver\"}");

	IKRig->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"solverType\":\"%s\",\"solverIndex\":%d}"),
		*SolverClass->GetName(), SolverIndex);
}

// ============================================================
// ADD RETARGET CHAIN
// ============================================================

FString FNwiroIKTools::AddRetargetChain(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString RigPath = Cmd->GetStringField(TEXT("rig"));
	FString ChainName = Cmd->GetStringField(TEXT("name"));
	FString StartBone = Cmd->GetStringField(TEXT("startBone"));
	FString EndBone = Cmd->GetStringField(TEXT("endBone"));

	UObject* Asset = UEditorAssetLibrary::LoadAsset(RigPath);
	UIKRigDefinition* IKRig = Cast<UIKRigDefinition>(Asset);
	if (!IKRig)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"IKRig not found: %s\"}"), *RigPath);

	UIKRigController* ControllerPtr = UIKRigController::GetController(IKRig);
	UIKRigController& Controller = *ControllerPtr;
	FName ChainResult = Controller.AddRetargetChain(FName(*ChainName), FName(*StartBone), FName(*EndBone), NAME_None);

	if (ChainResult == NAME_None)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to add retarget chain '%s' (%s -> %s)\"}"), *ChainName, *StartBone, *EndBone);

	IKRig->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"chain\":\"%s\",\"startBone\":\"%s\",\"endBone\":\"%s\"}"),
		*ChainName, *StartBone, *EndBone);
}

// ============================================================
// SET CHAIN MAPPING (IK Retargeter)
// ============================================================

FString FNwiroIKTools::SetChainMapping(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString RetargetPath = Cmd->GetStringField(TEXT("retargeter"));
	FString SourceChain = Cmd->GetStringField(TEXT("sourceChain"));
	FString TargetChain = Cmd->GetStringField(TEXT("targetChain"));

	UObject* Asset = UEditorAssetLibrary::LoadAsset(RetargetPath);
	UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Asset);
	if (!Retargeter)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"IKRetargeter not found: %s\"}"), *RetargetPath);

	UIKRetargeterController* ControllerPtr = UIKRetargeterController::GetController(Retargeter);
	UIKRetargeterController& Controller = *ControllerPtr;
	Controller.SetSourceChain(FName(*SourceChain), FName(*TargetChain), NAME_None);

	Retargeter->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"sourceChain\":\"%s\",\"targetChain\":\"%s\"}"),
		*SourceChain, *TargetChain);
}

// ============================================================
// ADD POSE SEARCH ANIMATION
// ============================================================

FString FNwiroIKTools::AddPoseSearchAnimation(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString DBPath = Cmd->GetStringField(TEXT("database"));
	FString AnimPath = Cmd->GetStringField(TEXT("animation"));

	UObject* DBAsset = UEditorAssetLibrary::LoadAsset(DBPath);
	UPoseSearchDatabase* DB = Cast<UPoseSearchDatabase>(DBAsset);
	if (!DB)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"PoseSearchDatabase not found: %s\"}"), *DBPath);

	UAnimSequence* AnimSeq = Cast<UAnimSequence>(UEditorAssetLibrary::LoadAsset(AnimPath));
	if (!AnimSeq)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Animation not found: %s\"}"), *AnimPath);

	// Add animation via the database's public API
	FPoseSearchDatabaseAnimationAsset NewAsset;
	NewAsset.AnimAsset = AnimSeq;
	DB->AddAnimationAsset(NewAsset);

	DB->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"database\":\"%s\",\"animation\":\"%s\"}"),
		*DB->GetName(), *AnimSeq->GetName());
}

#endif // NWIRO_HAS_IK_TOOLS
