// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKNiagaraTools.h"
#include "NwiroIKTransactionHelper.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "Factories/Factory.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Json.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroNiagara, Log, All);

// ============================================================
// CREATE NIAGARA SYSTEM
// ============================================================

FString FNwiroIKNiagaraTools::CreateNiagaraSystem(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));

	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/FX");

	// Add NS_ prefix if missing
	if (!Name.StartsWith(TEXT("NS_"))) Name = TEXT("NS_") + Name;

	const FString FullPath = Path / Name;
	// Idempotent — return existing instead of crashing on duplicate create.
	if (UNiagaraSystem* Existing = LoadObject<UNiagaraSystem>(nullptr, *(FullPath + TEXT(".") + Name)))
	{
		return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"existed\":true}"),
			*Name, *Existing->GetPathName());
	}

	// Niagara requires NiagaraSystemFactoryNew — passing nullptr factory derefs null inside Niagara
	// module and crashes the editor. Load the factory class dynamically to avoid a hard
	// NiagaraEditor module dependency.
	UClass* FactoryClass = LoadObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraSystemFactoryNew"));
	if (!FactoryClass)
		return TEXT("{\"success\":false,\"error\":\"NiagaraSystemFactoryNew class not found. Ensure Niagara plugin is enabled and the editor is running.\"}");

	UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	if (!Factory)
		return TEXT("{\"success\":false,\"error\":\"Failed to instantiate Niagara factory\"}");

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateNiagaraSystem", "AI: Create Niagara System"));

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* NewAsset = AssetTools.CreateAsset(Name, Path, UNiagaraSystem::StaticClass(), Factory);
	UNiagaraSystem* System = Cast<UNiagaraSystem>(NewAsset);

	if (!System)
	{
		Tx.Cancel();
		return TEXT("{\"success\":false,\"error\":\"Failed to create NiagaraSystem. Ensure Niagara plugin is enabled.\"}");
	}

	Tx.AlsoModify(System);
	System->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\"}"),
		*Name, *System->GetPathName());
}

// ============================================================
// READ NIAGARA SYSTEM
// ============================================================

FString FNwiroIKNiagaraTools::ReadNiagaraSystem(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
	UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
	if (!System)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"NiagaraSystem not found: %s\"}"), *Path);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), System->GetName());
	Result->SetStringField(TEXT("path"), System->GetPathName());

	// Emitter handles
	TArray<TSharedPtr<FJsonValue>> Emitters;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		TSharedRef<FJsonObject> E = MakeShareable(new FJsonObject());
		E->SetStringField(TEXT("name"), Handle.GetName().ToString());
		E->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		E->SetStringField(TEXT("id"), Handle.GetId().ToString());
		Emitters.Add(MakeShareable(new FJsonValueObject(E)));
	}
	Result->SetArrayField(TEXT("emitters"), Emitters);
	Result->SetNumberField(TEXT("emitterCount"), Emitters.Num());

	// User parameters (exposed)
	TArray<TSharedPtr<FJsonValue>> Params;
	for (const FNiagaraVariable& Var : System->GetExposedParameters().ReadParameterVariables())
	{
		TSharedRef<FJsonObject> P = MakeShareable(new FJsonObject());
		P->SetStringField(TEXT("name"), Var.GetName().ToString());
		P->SetStringField(TEXT("type"), Var.GetType().GetName());
		Params.Add(MakeShareable(new FJsonValueObject(P)));
	}
	Result->SetArrayField(TEXT("userParameters"), Params);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// SET NIAGARA PARAMETER
// ============================================================

FString FNwiroIKNiagaraTools::SetNiagaraParameter(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SystemPath = Cmd->GetStringField(TEXT("system"));
	FString ParamName = Cmd->GetStringField(TEXT("parameter"));

	UObject* Asset = UEditorAssetLibrary::LoadAsset(SystemPath);
	UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
	if (!System)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"NiagaraSystem not found: %s\"}"), *SystemPath);

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "SetNiagaraParameter", "AI: Set Niagara Parameter"), System);

	// Find the parameter by iterating exposed parameters
	bool bFound = false;
	bool bSet = false;
	FString TypeName;

	for (const FNiagaraVariable& Var : System->GetExposedParameters().ReadParameterVariables())
	{
		if (Var.GetName().ToString() == ParamName)
		{
			bFound = true;
			TypeName = Var.GetType().GetName();

			// Use the overload that takes FNiagaraVariable directly
			FNiagaraParameterStore& Store = System->GetExposedParameters();

			if (TypeName.Contains(TEXT("Float")) && Cmd->HasField(TEXT("value")))
			{
				float Val = (float)Cmd->GetNumberField(TEXT("value"));
				Store.SetParameterValue<float>(Val, Var);
				bSet = true;
			}
			else if (TypeName.Contains(TEXT("Int")) && Cmd->HasField(TEXT("value")))
			{
				int32 Val = (int32)Cmd->GetNumberField(TEXT("value"));
				Store.SetParameterValue<int32>(Val, Var);
				bSet = true;
			}
			break;
		}
	}

	if (!bFound)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Parameter not found: %s\"}"), *ParamName);
	if (!bSet)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Cannot set parameter of type: %s\"}"), *TypeName);

	System->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"parameter\":\"%s\",\"type\":\"%s\"}"), *ParamName, *TypeName);
}

// ============================================================
// ADD NIAGARA EMITTER
// ============================================================

FString FNwiroIKNiagaraTools::AddNiagaraEmitter(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SystemPath = Cmd->GetStringField(TEXT("system"));
	FString EmitterPath = Cmd->GetStringField(TEXT("emitter"));
	FString EmitterName = Cmd->GetStringField(TEXT("name"));

	UObject* SysAsset = UEditorAssetLibrary::LoadAsset(SystemPath);
	UNiagaraSystem* System = Cast<UNiagaraSystem>(SysAsset);
	if (!System)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"NiagaraSystem not found: %s\"}"), *SystemPath);

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "AddNiagaraEmitter", "AI: Add Niagara Emitter"), System);

	// If emitter path given, add existing emitter to system
	if (!EmitterPath.IsEmpty())
	{
		UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(UEditorAssetLibrary::LoadAsset(EmitterPath));
		if (!Emitter)
		{
			// Search by name
			FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			FARFilter Filter;
			Filter.ClassPaths.Add(UNiagaraEmitter::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths = true;
			TArray<FAssetData> Assets;
			ARM.Get().GetAssets(Filter, Assets);
			for (const FAssetData& A : Assets)
			{
				if (A.AssetName.ToString().Contains(EmitterPath, ESearchCase::IgnoreCase))
				{
					Emitter = Cast<UNiagaraEmitter>(A.GetAsset());
					break;
				}
			}
		}

		if (Emitter)
		{
			FNiagaraEmitterHandle Handle = System->AddEmitterHandle(*Emitter, FName(EmitterName.IsEmpty() ? Emitter->GetName() : *EmitterName), Emitter->GetExposedVersion().VersionGuid);

			System->MarkPackageDirty();

			return FString::Printf(TEXT("{\"success\":true,\"emitter\":\"%s\",\"system\":\"%s\"}"),
				*Handle.GetName().ToString(), *System->GetName());
		}

		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Emitter not found: %s\"}"), *EmitterPath);
	}

	return TEXT("{\"success\":false,\"error\":\"Provide 'emitter' path to add an existing emitter to the system\"}");
}
