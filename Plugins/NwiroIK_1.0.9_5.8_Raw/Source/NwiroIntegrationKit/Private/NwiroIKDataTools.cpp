// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKDataTools.h"
#include "NwiroIKAssetGuard.h"
#include "NwiroIKTransactionHelper.h"
#include "Engine/DataTable.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Engine/UserDefinedEnum.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Factories/DataTableFactory.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Json.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroData, Log, All);

// ============================================================
// CREATE DATA TABLE
// ============================================================

FString FNwiroIKDataTools::CreateDataTable(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	FString RowStructName = Cmd->GetStringField(TEXT("rowStruct"));

	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/Data");

	// Find the row struct. The LLM may pass any of:
	//   - A bare struct name ("MyRowStruct" / "S_MyRow")
	//   - An engine struct shorthand
	//   - A full content path ("/Game/Foo/S_MyRow" or ".S_MyRow")
	// Try each lookup tier before giving up.
	UScriptStruct* RowStruct = nullptr;
	if (!RowStructName.IsEmpty())
	{
		// 1) Path-shaped input → load directly from disk.
		if (RowStructName.StartsWith(TEXT("/")))
		{
			FString PackagePath = RowStructName;
			int32 Dot; if (PackagePath.FindLastChar('.', Dot)) PackagePath = PackagePath.Left(Dot);
			if (UObject* Loaded = LoadObject<UObject>(nullptr, *PackagePath))
			{
				RowStruct = Cast<UScriptStruct>(Loaded);
				if (!RowStruct)
				{
					// UserDefinedStructs are loaded as UUserDefinedStruct
					// which is a subclass of UScriptStruct — Cast handles
					// both. Falling here means the asset just isn't a struct.
				}
			}
		}

		// 2) Already-loaded by global name (native C++ structs).
		if (!RowStruct) RowStruct = FindObject<UScriptStruct>(nullptr, *RowStructName);
		// 3) Engine module shorthand.
		if (!RowStruct) RowStruct = FindObject<UScriptStruct>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *RowStructName));
		// 4) Walk user defined structs in /Game.
		if (!RowStruct)
		{
			FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			FARFilter Filter;
			Filter.ClassPaths.Add(UUserDefinedStruct::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths = true;
			Filter.PackagePaths.Add(TEXT("/Game"));
			TArray<FAssetData> Assets;
			ARM.Get().GetAssets(Filter, Assets);
			for (const FAssetData& A : Assets)
			{
				if (A.AssetName.ToString().Equals(RowStructName, ESearchCase::IgnoreCase))
				{
					RowStruct = Cast<UScriptStruct>(A.GetAsset());
					break;
				}
			}
		}
	}

	// DataTables MUST have a row struct — UDataTableFactory rejects creation
	// when Struct is null. The LLM very often forgets to pass one; surface
	// the requirement explicitly rather than letting CreateAsset return
	// null with no context.
	if (!RowStruct)
	{
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Row struct '%s' not found (or 'rowStruct' arg missing). Pass either a bare struct name like 'MyRowStruct' or a full asset path like '/Game/Data/S_MyRow'.\"}"),
			*RowStructName.Replace(TEXT("\""), TEXT("\\\"")));
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UDataTableFactory* Factory = NewObject<UDataTableFactory>();
	Factory->Struct = RowStruct;

	{ const FString _C = NwiroCheckCreateConflict(Path / Name, Name, UDataTable::StaticClass()); if (!_C.IsEmpty()) return _C; }

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateDataTable", "AI: Create DataTable"));

	UObject* NewAsset = AssetTools.CreateAsset(Name, Path, UDataTable::StaticClass(), Factory);
	UDataTable* DT = Cast<UDataTable>(NewAsset);

	// Factory fallback: directly construct + bind the row struct.
	if (!DT)
	{
		const FString FullPath = Path / Name;
		UPackage* Package = CreatePackage(*FullPath);
		if (Package)
		{
			Package->FullyLoad();
			Package->AddToRoot();
			DT = NewObject<UDataTable>(Package, *Name, RF_Public | RF_Standalone);
			if (DT)
			{
				DT->RowStruct = RowStruct;
				FAssetRegistryModule::AssetCreated(DT);
			}
			Package->RemoveFromRoot();
		}
	}

	if (!DT)
	{
		Tx.Cancel();
		return TEXT("{\"success\":false,\"error\":\"Failed to create DataTable (both factory and NewObject paths failed)\"}");
	}

	Tx.AlsoModify(DT);
	DT->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"rowStruct\":\"%s\"}"),
		*Name, *DT->GetPathName(), RowStruct ? *RowStruct->GetName() : TEXT("None"));
}

// ============================================================
// ADD DATA TABLE ROW
// ============================================================

FString FNwiroIKDataTools::AddDataTableRow(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString TablePath;
	for (const TCHAR* K : { TEXT("table"), TEXT("path"), TEXT("assetPath"), TEXT("dataTable"), TEXT("tablePath") }) {
		if (Cmd->TryGetStringField(K, TablePath) && !TablePath.IsEmpty()) break;
	}
	FString RowName = Cmd->GetStringField(TEXT("rowName"));

	UObject* Asset = UEditorAssetLibrary::LoadAsset(TablePath);
	UDataTable* DT = Cast<UDataTable>(Asset);
	if (!DT)
	{
		// Search by name
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		FARFilter Filter;
		Filter.ClassPaths.Add(UDataTable::StaticClass()->GetClassPathName());
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(TEXT("/Game"));
		TArray<FAssetData> Assets;
		ARM.Get().GetAssets(Filter, Assets);
		for (const FAssetData& A : Assets)
		{
			if (A.AssetName.ToString().Contains(TablePath, ESearchCase::IgnoreCase))
			{
				DT = Cast<UDataTable>(A.GetAsset());
				break;
			}
		}
	}

	if (!DT) return FString::Printf(TEXT("{\"success\":false,\"error\":\"DataTable not found: %s\"}"), *TablePath);
	if (RowName.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'rowName'\"}");

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "AddDataTableRow", "AI: Add DataTable Row"), DT);

	// Add row via JSON. CreateTableFromJSONString REPLACES all rows, so we
	// gather existing rows first and rewrite the full set with the new one
	// appended (or overwriting on rowName collision when bUpdate is set).
	const TSharedPtr<FJsonObject>* ValuesObj = nullptr;
	if (Cmd->TryGetObjectField(TEXT("values"), ValuesObj) && ValuesObj && ValuesObj->IsValid())
	{
		bool bUpdate = false;
		Cmd->TryGetBoolField(TEXT("update"), bUpdate);

		// Build the new row JSON snippet (without surrounding braces) so we
		// can splice it cleanly into the rebuilt array.
		FString NewRowInner;
		TSharedRef<TJsonWriter<>> JW = TJsonWriterFactory<>::Create(&NewRowInner);
		FJsonSerializer::Serialize((*ValuesObj).ToSharedRef(), JW);
		const FString NewRowFields = NewRowInner.Mid(1, NewRowInner.Len() - 2);

		// Read existing rows as a JSON array.
		const FString ExistingJson = DT->GetTableAsJSON();
		TArray<TSharedPtr<FJsonValue>> ExistingArr;
		{
			TSharedRef<TJsonReader<>> RR = TJsonReaderFactory<>::Create(ExistingJson);
			FJsonSerializer::Deserialize(RR, ExistingArr);
		}

		// Rebuild output array — drop existing row with this name when
		// updating, otherwise keep all and append.
		TArray<FString> RowJsons;
		RowJsons.Reserve(ExistingArr.Num() + 1);
		for (const TSharedPtr<FJsonValue>& V : ExistingArr)
		{
			const TSharedPtr<FJsonObject>& Obj = V->AsObject();
			if (!Obj.IsValid()) continue;
			FString ExistingName;
			Obj->TryGetStringField(TEXT("Name"), ExistingName);
			if (bUpdate && ExistingName.Equals(RowName)) continue;
			FString Buf;
			TSharedRef<TJsonWriter<>> RW = TJsonWriterFactory<>::Create(&Buf);
			FJsonSerializer::Serialize(Obj.ToSharedRef(), RW);
			RowJsons.Add(Buf);
		}
		RowJsons.Add(FString::Printf(TEXT("{\"Name\":\"%s\",%s}"), *RowName, *NewRowFields));

		const FString JsonString = FString::Printf(TEXT("[%s]"), *FString::Join(RowJsons, TEXT(",")));
		DT->CreateTableFromJSONString(JsonString);
	}

	DT->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"table\":\"%s\",\"row\":\"%s\",\"rowCount\":%d}"),
		*DT->GetName(), *RowName, DT->GetRowMap().Num());
}

// ============================================================
// READ DATA TABLE
// ============================================================

FString FNwiroIKDataTools::ReadDataTable(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString TablePath;
	for (const TCHAR* K : { TEXT("table"), TEXT("path"), TEXT("assetPath"), TEXT("dataTable"), TEXT("tablePath") }) {
		if (Cmd->TryGetStringField(K, TablePath) && !TablePath.IsEmpty()) break;
	}

	UObject* Asset = UEditorAssetLibrary::LoadAsset(TablePath);
	UDataTable* DT = Cast<UDataTable>(Asset);
	if (!DT)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"DataTable not found: %s\"}"), *TablePath);

	FString JsonOut = DT->GetTableAsJSON();

	return FString::Printf(TEXT("{\"success\":true,\"table\":\"%s\",\"rowStruct\":\"%s\",\"rowCount\":%d,\"rows\":%s}"),
		*DT->GetName(),
		DT->RowStruct ? *DT->RowStruct->GetName() : TEXT("None"),
		DT->GetRowMap().Num(),
		*JsonOut);
}

// ============================================================
// IMPORT DATA TABLE JSON
// ============================================================

FString FNwiroIKDataTools::ImportDataTableJson(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString TablePath;
	for (const TCHAR* K : { TEXT("table"), TEXT("path"), TEXT("assetPath"), TEXT("dataTable"), TEXT("tablePath") }) {
		if (Cmd->TryGetStringField(K, TablePath) && !TablePath.IsEmpty()) break;
	}
	FString JsonData = Cmd->GetStringField(TEXT("json"));

	UObject* Asset = UEditorAssetLibrary::LoadAsset(TablePath);
	UDataTable* DT = Cast<UDataTable>(Asset);
	if (!DT)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"DataTable not found: %s\"}"), *TablePath);

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "ImportDataTableJson", "AI: Import DataTable JSON"), DT);

	TArray<FString> Problems = DT->CreateTableFromJSONString(JsonData);

	DT->MarkPackageDirty();

	if (Problems.Num() > 0)
	{
		return FString::Printf(TEXT("{\"success\":true,\"warnings\":\"%s\",\"rowCount\":%d}"),
			*FString::Join(Problems, TEXT("; ")), DT->GetRowMap().Num());
	}

	return FString::Printf(TEXT("{\"success\":true,\"rowCount\":%d}"), DT->GetRowMap().Num());
}

// ============================================================
// CREATE STRUCT
// ============================================================

FString FNwiroIKDataTools::CreateStruct(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/Data");

	FString FullPath = Path / Name;
	{ const FString _C = NwiroCheckCreateConflict(FullPath, Name, UUserDefinedStruct::StaticClass()); if (!_C.IsEmpty()) return _C; }

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateStruct", "AI: Create Struct"));

	UPackage* Package = CreatePackage(*FullPath);
	if (!Package)
	{
		Tx.Cancel();
		return TEXT("{\"success\":false,\"error\":\"Failed to create package\"}");
	}
	Tx.AlsoModify(Package);

	UUserDefinedStruct* NewStruct = FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(*Name), RF_Public | RF_Standalone);
	if (!NewStruct)
	{
		Tx.Cancel();
		return TEXT("{\"success\":false,\"error\":\"Failed to create struct\"}");
	}
	Tx.AlsoModify(NewStruct);

	// Add fields
	const TArray<TSharedPtr<FJsonValue>>* Fields;
	int32 FieldCount = 0;
	if (Cmd->TryGetArrayField(TEXT("fields"), Fields))
	{
		for (const TSharedPtr<FJsonValue>& FieldVal : *Fields)
		{
			const TSharedPtr<FJsonObject>& FieldObj = FieldVal->AsObject();
			if (!FieldObj.IsValid()) continue;

			FString FieldName = FieldObj->GetStringField(TEXT("name"));
			FString FieldType = FieldObj->GetStringField(TEXT("type"));

			if (FieldName.IsEmpty() || FieldType.IsEmpty()) continue;

			// Map type string to FEdGraphPinType
			FEdGraphPinType PinType;
			PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean; // default

			FString TypeLower = FieldType.ToLower();
			if (TypeLower == TEXT("bool") || TypeLower == TEXT("boolean"))
				PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			else if (TypeLower == TEXT("int") || TypeLower == TEXT("integer"))
				PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
			else if (TypeLower == TEXT("float"))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
				PinType.PinSubCategory = TEXT("float");
			}
			else if (TypeLower == TEXT("double") || TypeLower == TEXT("real"))
			{
				// "real" is an alias defaulting to highest precision (double).
				PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
				PinType.PinSubCategory = TEXT("double");
			}
			else if (TypeLower == TEXT("string"))
				PinType.PinCategory = UEdGraphSchema_K2::PC_String;
			else if (TypeLower == TEXT("name"))
				PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
			else if (TypeLower == TEXT("text"))
				PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
			else if (TypeLower == TEXT("vector"))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
			}
			else if (TypeLower == TEXT("rotator"))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
			}
			else if (TypeLower == TEXT("transform"))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
			}
			else if (TypeLower == TEXT("color") || TypeLower == TEXT("linearcolor"))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
			}

			FStructureEditorUtils::AddVariable(NewStruct, PinType);
			// Rename the variable
			TArray<FStructVariableDescription>& Vars = FStructureEditorUtils::GetVarDesc(NewStruct);
			if (Vars.Num() > 0)
			{
				FStructureEditorUtils::RenameVariable(NewStruct, Vars.Last().VarGuid, FieldName);
			}

			FieldCount++;
		}
	}

	FAssetRegistryModule::AssetCreated(NewStruct);
	NewStruct->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"fieldCount\":%d}"),
		*Name, *NewStruct->GetPathName(), FieldCount);
}

// ============================================================
// CREATE ENUM
// ============================================================

FString FNwiroIKDataTools::CreateEnum(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/Data");

	FString FullPath = Path / Name;
	{ const FString _C = NwiroCheckCreateConflict(FullPath, Name, UUserDefinedEnum::StaticClass()); if (!_C.IsEmpty()) return _C; }

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateEnum", "AI: Create Enum"));

	UPackage* Package = CreatePackage(*FullPath);
	if (!Package)
	{
		Tx.Cancel();
		return TEXT("{\"success\":false,\"error\":\"Failed to create package\"}");
	}
	Tx.AlsoModify(Package);

	UUserDefinedEnum* NewEnum = Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(Package, FName(*Name), RF_Public | RF_Standalone));
	if (!NewEnum)
	{
		Tx.Cancel();
		return TEXT("{\"success\":false,\"error\":\"Failed to create enum\"}");
	}
	Tx.AlsoModify(NewEnum);

	// Add values
	const TArray<TSharedPtr<FJsonValue>>* Values;
	int32 ValueCount = 0;
	if (Cmd->TryGetArrayField(TEXT("values"), Values))
	{
		for (const TSharedPtr<FJsonValue>& Val : *Values)
		{
			FString ValueName = Val->AsString();
			if (ValueName.IsEmpty()) continue;

			FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(NewEnum);
			// Rename to the desired name
			int64 EnumVal = NewEnum->GetMaxEnumValue() - 2; // -1 for MAX, -1 for 0-indexed
			FText DisplayName = FText::FromString(ValueName);
			FEnumEditorUtils::SetEnumeratorDisplayName(NewEnum, EnumVal, DisplayName);

			ValueCount++;
		}
	}

	FAssetRegistryModule::AssetCreated(NewEnum);
	NewEnum->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"valueCount\":%d}"),
		*Name, *NewEnum->GetPathName(), ValueCount);
}
