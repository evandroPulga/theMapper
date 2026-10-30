// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKInputTools.h"
#include "NwiroIKTransactionHelper.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Json.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroInput, Log, All);

// ============================================================
// CREATE INPUT ACTION
// ============================================================

FString FNwiroIKInputTools::CreateInputAction(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	FString ValueTypeStr = Cmd->GetStringField(TEXT("valueType"));

	if (Name.IsEmpty()) return TEXT("{\"success\": false, \"error\": \"Missing 'name'\"}");

	// Hallucination-tolerant: when the agent passes a full asset path in
	// `name` (e.g. "/Game/_NwiroScenario/05/IA_Pause"), split it into a path
	// + leaf name instead of mangling it via Path/Name concat. The agent's
	// `path` arg only wins when `name` is a bare leaf.
	if (Name.Contains(TEXT("/")))
	{
		int32 LastSlash;
		Name.FindLastChar('/', LastSlash);
		const FString Embedded = Name.Left(LastSlash);
		Name = Name.Mid(LastSlash + 1);
		if (!Embedded.IsEmpty()) Path = Embedded; // override path arg with embedded
	}

	if (Path.IsEmpty()) Path = TEXT("/Game/Input");
	if (!Path.StartsWith(TEXT("/"))) Path = TEXT("/") + Path;

	// Add IA_ prefix if missing
	if (!Name.StartsWith(TEXT("IA_"))) Name = TEXT("IA_") + Name;

	// Determine value type
	EInputActionValueType ValueType = EInputActionValueType::Boolean; // Digital
	if (ValueTypeStr.Equals(TEXT("Axis1D"), ESearchCase::IgnoreCase) || ValueTypeStr.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
	{
		ValueType = EInputActionValueType::Axis1D;
	}
	else if (ValueTypeStr.Equals(TEXT("Axis2D"), ESearchCase::IgnoreCase) || ValueTypeStr.Equals(TEXT("Vector2D"), ESearchCase::IgnoreCase))
	{
		ValueType = EInputActionValueType::Axis2D;
	}
	else if (ValueTypeStr.Equals(TEXT("Axis3D"), ESearchCase::IgnoreCase) || ValueTypeStr.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
	{
		ValueType = EInputActionValueType::Axis3D;
	}

	// Create the asset
	FString FullPath = Path / Name;

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateInputAction", "AI: Create Input Action"));

	UPackage* Package = CreatePackage(*FullPath);
	if (!Package)
	{
		Tx.Cancel();
		return TEXT("{\"success\": false, \"error\": \"Failed to create package\"}");
	}
	Tx.AlsoModify(Package);

	UInputAction* IA = NewObject<UInputAction>(Package, UInputAction::StaticClass(), FName(*Name), RF_Public | RF_Standalone);
	if (!IA)
	{
		Tx.Cancel();
		return TEXT("{\"success\": false, \"error\": \"Failed to create InputAction\"}");
	}
	Tx.AlsoModify(IA);

	IA->ValueType = ValueType;

	FAssetRegistryModule::AssetCreated(IA);
	IA->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Name);
	Result->SetStringField(TEXT("path"), IA->GetPathName());
	Result->SetStringField(TEXT("valueType"), ValueTypeStr.IsEmpty() ? TEXT("Digital") : ValueTypeStr);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);

	UE_LOG(LogNwiroInput, Log, TEXT("Created InputAction: %s (%s)"), *Name, *ValueTypeStr);
	return Out;
}

// ============================================================
// CREATE INPUT MAPPING CONTEXT
// ============================================================

FString FNwiroIKInputTools::CreateInputMappingContext(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));

	if (Name.IsEmpty()) return TEXT("{\"success\": false, \"error\": \"Missing 'name'\"}");

	// Same path-in-name fix as CreateInputAction above — split when agent
	// passes a full asset path in `name`.
	if (Name.Contains(TEXT("/")))
	{
		int32 LastSlash;
		Name.FindLastChar('/', LastSlash);
		const FString Embedded = Name.Left(LastSlash);
		Name = Name.Mid(LastSlash + 1);
		if (!Embedded.IsEmpty()) Path = Embedded;
	}

	if (Path.IsEmpty()) Path = TEXT("/Game/Input");
	if (!Path.StartsWith(TEXT("/"))) Path = TEXT("/") + Path;

	// Add IMC_ prefix if missing
	if (!Name.StartsWith(TEXT("IMC_"))) Name = TEXT("IMC_") + Name;

	// Create the asset
	FString FullPath = Path / Name;

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateInputMappingContext", "AI: Create Input Mapping Context"));

	UPackage* Package = CreatePackage(*FullPath);
	if (!Package)
	{
		Tx.Cancel();
		return TEXT("{\"success\": false, \"error\": \"Failed to create package\"}");
	}
	Tx.AlsoModify(Package);

	UInputMappingContext* IMC = NewObject<UInputMappingContext>(Package, UInputMappingContext::StaticClass(), FName(*Name), RF_Public | RF_Standalone);
	if (!IMC)
	{
		Tx.Cancel();
		return TEXT("{\"success\": false, \"error\": \"Failed to create InputMappingContext\"}");
	}
	Tx.AlsoModify(IMC);

	// Add mappings
	const TArray<TSharedPtr<FJsonValue>>* Mappings;
	int32 MappedCount = 0;
	TArray<FString> Errors;

	if (Cmd->TryGetArrayField(TEXT("mappings"), Mappings))
	{
		for (const TSharedPtr<FJsonValue>& MapVal : *Mappings)
		{
			const TSharedPtr<FJsonObject>& MapObj = MapVal->AsObject();
			if (!MapObj.IsValid()) continue;

			FString ActionPath = MapObj->GetStringField(TEXT("action"));
			FString KeyName = MapObj->GetStringField(TEXT("key"));

			if (ActionPath.IsEmpty() || KeyName.IsEmpty()) continue;

			// Load the InputAction
			UInputAction* Action = LoadObject<UInputAction>(nullptr, *ActionPath);
			if (!Action)
			{
				// Try searching by name
				FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
				IAssetRegistry& AR = ARM.Get();
				AR.ScanPathsSynchronous({TEXT("/Game")}, true);

				FARFilter Filter;
				Filter.ClassPaths.Add(UInputAction::StaticClass()->GetClassPathName());
				Filter.bRecursivePaths = true;

				TArray<FAssetData> Assets;
				AR.GetAssets(Filter, Assets);

				for (const FAssetData& Asset : Assets)
				{
					if (Asset.AssetName.ToString().Equals(ActionPath, ESearchCase::IgnoreCase) ||
						Asset.AssetName.ToString().Contains(ActionPath, ESearchCase::IgnoreCase))
					{
						Action = Cast<UInputAction>(Asset.GetAsset());
						break;
					}
				}
			}

			if (!Action)
			{
				Errors.Add(FString::Printf(TEXT("InputAction not found: %s"), *ActionPath));
				continue;
			}

			// Map the key
			FKey Key(*KeyName);
			FEnhancedActionKeyMapping& Mapping = IMC->MapKey(Action, Key);

			// Add modifiers
			const TArray<TSharedPtr<FJsonValue>>* Modifiers;
			if (MapObj->TryGetArrayField(TEXT("modifiers"), Modifiers))
			{
				for (const TSharedPtr<FJsonValue>& ModVal : *Modifiers)
				{
					FString ModName = ModVal->AsString();
					if (ModName.Equals(TEXT("Negate"), ESearchCase::IgnoreCase))
					{
						UInputModifierNegate* Mod = NewObject<UInputModifierNegate>(IMC);
						Mapping.Modifiers.Add(Mod);
					}
					else if (ModName.Equals(TEXT("SwizzleYXZ"), ESearchCase::IgnoreCase))
					{
						UInputModifierSwizzleAxis* Mod = NewObject<UInputModifierSwizzleAxis>(IMC);
						Mod->Order = EInputAxisSwizzle::YXZ;
						Mapping.Modifiers.Add(Mod);
					}
					else if (ModName.Equals(TEXT("SwizzleZYX"), ESearchCase::IgnoreCase))
					{
						UInputModifierSwizzleAxis* Mod = NewObject<UInputModifierSwizzleAxis>(IMC);
						Mod->Order = EInputAxisSwizzle::ZYX;
						Mapping.Modifiers.Add(Mod);
					}
				}
			}

			MappedCount++;
		}
	}

	FAssetRegistryModule::AssetCreated(IMC);
	IMC->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Name);
	Result->SetStringField(TEXT("path"), IMC->GetPathName());
	Result->SetNumberField(TEXT("mappingCount"), MappedCount);

	if (Errors.Num() > 0)
	{
		Result->SetStringField(TEXT("errors"), FString::Join(Errors, TEXT("; ")));
	}

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);

	UE_LOG(LogNwiroInput, Log, TEXT("Created InputMappingContext: %s with %d mappings"), *Name, MappedCount);
	return Out;
}

// ============================================================
// FIND INPUT ACTIONS
// ============================================================

FString FNwiroIKInputTools::FindInputActions(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	FJsonSerializer::Deserialize(Reader, Cmd);

	FString SearchTerm = Cmd.IsValid() ? Cmd->GetStringField(TEXT("searchTerm")) : TEXT("");

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UInputAction::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : Assets)
	{
		FString Name = Asset.AssetName.ToString();
		if (!SearchTerm.IsEmpty() && !Name.Contains(SearchTerm, ESearchCase::IgnoreCase)) continue;

		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("path"), FString::Printf(TEXT("%s.%s"), *Asset.PackageName.ToString(), *Name));
		Results.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	// Also search for InputMappingContext
	FARFilter IMCFilter;
	IMCFilter.ClassPaths.Add(UInputMappingContext::StaticClass()->GetClassPathName());
	IMCFilter.PackagePaths.Add(TEXT("/Game"));
	IMCFilter.bRecursivePaths = true;

	TArray<FAssetData> IMCAssets;
	AR.GetAssets(IMCFilter, IMCAssets);

	TArray<TSharedPtr<FJsonValue>> IMCResults;
	for (const FAssetData& Asset : IMCAssets)
	{
		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Obj->SetStringField(TEXT("path"), FString::Printf(TEXT("%s.%s"), *Asset.PackageName.ToString(), *Asset.AssetName.ToString()));
		IMCResults.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetBoolField(TEXT("success"), true);
	Root->SetArrayField(TEXT("inputActions"), Results);
	Root->SetArrayField(TEXT("mappingContexts"), IMCResults);
	Root->SetNumberField(TEXT("actionCount"), Results.Num());
	Root->SetNumberField(TEXT("contextCount"), IMCResults.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// DELETE INPUT ACTION
// ============================================================

FString FNwiroIKInputTools::DeleteInputAction(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));

	if (Name.IsEmpty() && Path.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"Missing 'name' or 'path'\"}");

	// If path given, try direct delete
	if (!Path.IsEmpty())
	{
		if (UEditorAssetLibrary::DeleteAsset(Path))
			return FString::Printf(TEXT("{\"success\":true,\"deleted\":\"%s\"}"), *Path);
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to delete: %s\"}"), *Path);
	}

	// Search by name
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	// Try InputAction first, then InputMappingContext
	for (FTopLevelAssetPath ClassPath : {UInputAction::StaticClass()->GetClassPathName(), UInputMappingContext::StaticClass()->GetClassPathName()})
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(ClassPath);
		Filter.PackagePaths.Add(TEXT("/Game"));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName.ToString().Equals(Name, ESearchCase::IgnoreCase) ||
				Asset.AssetName.ToString().Contains(Name, ESearchCase::IgnoreCase))
			{
				FString PkgPath = Asset.PackageName.ToString();
				if (UEditorAssetLibrary::DeleteAsset(PkgPath))
					return FString::Printf(TEXT("{\"success\":true,\"deleted\":\"%s\"}"), *PkgPath);
				return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to delete: %s\"}"), *PkgPath);
			}
		}
	}

	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Asset not found: %s\"}"), *Name);
}

// ============================================================
// EDIT MAPPING CONTEXT
// ============================================================

FString FNwiroIKInputTools::EditMappingContext(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	auto PickStr = [&](std::initializer_list<const TCHAR*> Keys) -> FString {
		for (const TCHAR* K : Keys) { FString V; if (Cmd->TryGetStringField(K, V) && !V.IsEmpty()) return V; }
		return FString();
	};
	FString Name = PickStr({ TEXT("name"), TEXT("imcName"), TEXT("contextName") });
	FString Path = PickStr({ TEXT("path"), TEXT("mappingContext"), TEXT("contextPath"),
		TEXT("imcPath"), TEXT("assetPath") });

	// Load the IMC
	UInputMappingContext* IMC = nullptr;
	if (!Path.IsEmpty())
	{
		IMC = LoadObject<UInputMappingContext>(nullptr, *Path);
	}

	if (!IMC && !Name.IsEmpty())
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();

		FARFilter Filter;
		Filter.ClassPaths.Add(UInputMappingContext::StaticClass()->GetClassPathName());
		Filter.PackagePaths.Add(TEXT("/Game"));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName.ToString().Equals(Name, ESearchCase::IgnoreCase) ||
				Asset.AssetName.ToString().Contains(Name, ESearchCase::IgnoreCase))
			{
				IMC = Cast<UInputMappingContext>(Asset.GetAsset());
				break;
			}
		}
	}

	if (!IMC) return FString::Printf(TEXT("{\"success\":false,\"error\":\"MappingContext not found: %s\"}"), *(Path.IsEmpty() ? Name : Path));

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "EditMappingContext", "AI: Edit Mapping Context"), IMC);

	int32 Added = 0;
	int32 Removed = 0;
	TArray<FString> Errors;

	// Remove mappings
	const TArray<TSharedPtr<FJsonValue>>* RemoveArr;
	if (Cmd->TryGetArrayField(TEXT("remove_mappings"), RemoveArr))
	{
		for (const TSharedPtr<FJsonValue>& Val : *RemoveArr)
		{
			FString ActionName = Val->AsString();
			// Find and remove the action
			UInputAction* Action = nullptr;
			FAssetRegistryModule& ARM2 = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			FARFilter F;
			F.ClassPaths.Add(UInputAction::StaticClass()->GetClassPathName());
			F.PackagePaths.Add(TEXT("/Game"));
			F.bRecursivePaths = true;
			TArray<FAssetData> Found;
			ARM2.Get().GetAssets(F, Found);
			for (const FAssetData& A : Found)
			{
				if (A.AssetName.ToString().Equals(ActionName, ESearchCase::IgnoreCase) ||
					A.AssetName.ToString().Contains(ActionName, ESearchCase::IgnoreCase))
				{
					Action = Cast<UInputAction>(A.GetAsset());
					break;
				}
			}
			if (Action)
			{
				// Remove all mappings for this action
			TArray<FEnhancedActionKeyMapping> Mappings = IMC->GetMappings();
			for (const FEnhancedActionKeyMapping& Mapping : Mappings)
			{
				if (Mapping.Action == Action)
				{
					IMC->UnmapKey(Action, Mapping.Key);
				}
			}
				Removed++;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("Action not found for removal: %s"), *ActionName));
			}
		}
	}

	// Add mappings (same logic as CreateInputMappingContext)
	const TArray<TSharedPtr<FJsonValue>>* AddArr;
	if (Cmd->TryGetArrayField(TEXT("add_mappings"), AddArr))
	{
		for (const TSharedPtr<FJsonValue>& MapVal : *AddArr)
		{
			const TSharedPtr<FJsonObject>& MapObj = MapVal->AsObject();
			if (!MapObj.IsValid()) continue;

			FString ActionPath = MapObj->GetStringField(TEXT("action"));
			FString KeyName = MapObj->GetStringField(TEXT("key"));
			if (ActionPath.IsEmpty() || KeyName.IsEmpty()) continue;

			UInputAction* Action = LoadObject<UInputAction>(nullptr, *ActionPath);
			if (!Action)
			{
				FAssetRegistryModule& ARM3 = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
				FARFilter F;
				F.ClassPaths.Add(UInputAction::StaticClass()->GetClassPathName());
				F.PackagePaths.Add(TEXT("/Game"));
				F.bRecursivePaths = true;
				TArray<FAssetData> Found;
				ARM3.Get().GetAssets(F, Found);
				for (const FAssetData& A : Found)
				{
					if (A.AssetName.ToString().Equals(ActionPath, ESearchCase::IgnoreCase) ||
						A.AssetName.ToString().Contains(ActionPath, ESearchCase::IgnoreCase))
					{
						Action = Cast<UInputAction>(A.GetAsset());
						break;
					}
				}
			}

			if (!Action)
			{
				Errors.Add(FString::Printf(TEXT("Action not found: %s"), *ActionPath));
				continue;
			}

			FKey Key(*KeyName);
			FEnhancedActionKeyMapping& Mapping = IMC->MapKey(Action, Key);

			// Modifiers
			const TArray<TSharedPtr<FJsonValue>>* Modifiers;
			if (MapObj->TryGetArrayField(TEXT("modifiers"), Modifiers))
			{
				for (const TSharedPtr<FJsonValue>& ModVal : *Modifiers)
				{
					FString ModName = ModVal->AsString();
					if (ModName.Equals(TEXT("Negate"), ESearchCase::IgnoreCase))
					{
						Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(IMC));
					}
					else if (ModName.Equals(TEXT("SwizzleYXZ"), ESearchCase::IgnoreCase))
					{
						UInputModifierSwizzleAxis* Mod = NewObject<UInputModifierSwizzleAxis>(IMC);
						Mod->Order = EInputAxisSwizzle::YXZ;
						Mapping.Modifiers.Add(Mod);
					}
					else if (ModName.Equals(TEXT("SwizzleZYX"), ESearchCase::IgnoreCase))
					{
						UInputModifierSwizzleAxis* Mod = NewObject<UInputModifierSwizzleAxis>(IMC);
						Mod->Order = EInputAxisSwizzle::ZYX;
						Mapping.Modifiers.Add(Mod);
					}
				}
			}

			Added++;
		}
	}

	IMC->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), IMC->GetName());
	Result->SetNumberField(TEXT("added"), Added);
	Result->SetNumberField(TEXT("removed"), Removed);
	if (Errors.Num() > 0)
		Result->SetStringField(TEXT("errors"), FString::Join(Errors, TEXT("; ")));

	FString Out;
	TSharedRef<TJsonWriter<>> W2 = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W2);
	return Out;
}
