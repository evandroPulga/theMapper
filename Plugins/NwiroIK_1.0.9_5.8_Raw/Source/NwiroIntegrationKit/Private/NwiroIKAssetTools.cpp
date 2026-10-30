// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKAssetTools.h"
#include "NwiroIKAssetGuard.h"
#include "Engine/Blueprint.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "Json.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroAsset, Log, All);

// ============================================================
// HELPERS
// ============================================================

static void SerializePropertyValue(UObject* Obj, FProperty* Prop, TSharedPtr<FJsonObject>& OutObj, int32 Depth)
{
	if (!Prop || !Obj) return;

	FString PropName = Prop->GetName();
	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);

	// Skip deprecated/editor-only metadata
	if (Prop->HasMetaData(TEXT("DeprecatedProperty"))) return;

	// Bool
	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
	{
		OutObj->SetBoolField(PropName, BoolProp->GetPropertyValue(ValuePtr));
		return;
	}

	// Numeric (int/float/double)
	if (FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
	{
		if (NumProp->IsFloatingPoint())
			OutObj->SetNumberField(PropName, NumProp->GetFloatingPointPropertyValue(ValuePtr));
		else if (NumProp->IsInteger())
			OutObj->SetNumberField(PropName, (double)NumProp->GetSignedIntPropertyValue(ValuePtr));
		else
		{
			// Enum underlying
			FString Val;
			NumProp->ExportTextItem_Direct(Val, ValuePtr, nullptr, Obj, PPF_None);
			OutObj->SetStringField(PropName, Val);
		}
		return;
	}

	// String types
	if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
	{
		OutObj->SetStringField(PropName, StrProp->GetPropertyValue(ValuePtr));
		return;
	}
	if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
	{
		OutObj->SetStringField(PropName, NameProp->GetPropertyValue(ValuePtr).ToString());
		return;
	}
	if (FTextProperty* TextProp = CastField<FTextProperty>(Prop))
	{
		OutObj->SetStringField(PropName, TextProp->GetPropertyValue(ValuePtr).ToString());
		return;
	}

	// Enum
	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		FString Val;
		EnumProp->ExportTextItem_Direct(Val, ValuePtr, nullptr, Obj, PPF_None);
		OutObj->SetStringField(PropName, Val);
		return;
	}
	if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
	{
		if (ByteProp->Enum)
		{
			FString Val = ByteProp->Enum->GetNameStringByValue(ByteProp->GetPropertyValue(ValuePtr));
			OutObj->SetStringField(PropName, Val);
		}
		else
		{
			OutObj->SetNumberField(PropName, (double)ByteProp->GetPropertyValue(ValuePtr));
		}
		return;
	}

	// Object reference
	if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
	{
		UObject* RefObj = ObjProp->GetObjectPropertyValue(ValuePtr);
		if (RefObj)
		{
			if (Depth > 0)
			{
				TSharedPtr<FJsonObject> SubObj = MakeShareable(new FJsonObject());
				SubObj->SetStringField(TEXT("class"), RefObj->GetClass()->GetName());
				SubObj->SetStringField(TEXT("path"), RefObj->GetPathName());
				OutObj->SetObjectField(PropName, SubObj);
			}
			else
			{
				OutObj->SetStringField(PropName, RefObj->GetPathName());
			}
		}
		else
		{
			OutObj->SetStringField(PropName, TEXT("None"));
		}
		return;
	}

	// Struct
	if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		if (Depth > 0)
		{
			TSharedPtr<FJsonObject> StructObj = MakeShareable(new FJsonObject());
			for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
			{
				FProperty* InnerProp = *It;
				void* InnerPtr = InnerProp->ContainerPtrToValuePtr<void>(ValuePtr);
				// Use export text for struct members
				FString Val;
				InnerProp->ExportTextItem_Direct(Val, InnerPtr, nullptr, nullptr, PPF_None);
				StructObj->SetStringField(InnerProp->GetName(), Val);
			}
			OutObj->SetObjectField(PropName, StructObj);
		}
		else
		{
			FString Val;
			StructProp->ExportTextItem_Direct(Val, ValuePtr, nullptr, Obj, PPF_None);
			OutObj->SetStringField(PropName, Val);
		}
		return;
	}

	// Array
	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		FScriptArrayHelper Helper(ArrayProp, ValuePtr);
		TArray<TSharedPtr<FJsonValue>> Arr;
		int32 Count = FMath::Min(Helper.Num(), 50); // Cap at 50 elements
		for (int32 i = 0; i < Count; i++)
		{
			FString Val;
			ArrayProp->Inner->ExportTextItem_Direct(Val, Helper.GetRawPtr(i), nullptr, Obj, PPF_None);
			Arr.Add(MakeShareable(new FJsonValueString(Val)));
		}
		OutObj->SetArrayField(PropName, Arr);
		if (Helper.Num() > 50)
		{
			OutObj->SetNumberField(PropName + TEXT("_totalCount"), Helper.Num());
		}
		return;
	}

	// Fallback: export as text
	FString Val;
	Prop->ExportTextItem_Direct(Val, ValuePtr, nullptr, Obj, PPF_None);
	if (!Val.IsEmpty())
	{
		OutObj->SetStringField(PropName, Val);
	}
}

// ============================================================
// READ ASSET
// ============================================================

FString FNwiroIKAssetTools::ReadAsset(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path;
	for (const TCHAR* K : { TEXT("path"), TEXT("assetPath"), TEXT("asset_path"), TEXT("name") }) {
		if (Cmd->TryGetStringField(K, Path) && !Path.IsEmpty()) break;
	}
	int32 Depth = Cmd->HasField(TEXT("depth")) ? (int32)Cmd->GetNumberField(TEXT("depth")) : 1;
	if (Path.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"path required (accepted: path / assetPath / name)\"}");

	// Try direct load
	UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);

	// If not found, search by name — only meaningful when Path isn't a
	// full /Game/... path (a fully-qualified path means the user is
	// specific; we shouldn't substring-match other random assets).
	if (!Asset && !Path.StartsWith(TEXT("/")))
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		FARFilter Filter;
		Filter.PackagePaths.Add(TEXT("/Game"));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> Assets;
		ARM.Get().GetAssets(Filter, Assets);

		for (const FAssetData& A : Assets)
		{
			const FString N = A.AssetName.ToString();
			if (N.Equals(Path, ESearchCase::IgnoreCase) ||
				(Path.Len() >= 3 && N.Contains(Path, ESearchCase::IgnoreCase)))
			{
				Asset = NwiroSafeRegistryLoad(A);
				break;
			}
		}
	}

	if (!Asset)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Asset not found: %s\"}"), *Path);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
	Result->SetStringField(TEXT("path"), Asset->GetPathName());
	Result->SetStringField(TEXT("package"), Asset->GetPackage()->GetName());

	// Serialize all properties
	TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject());
	for (TFieldIterator<FProperty> It(Asset->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;
		// Skip internal/transient properties
		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;
		SerializePropertyValue(Asset, Prop, Properties, Depth);
	}
	Result->SetObjectField(TEXT("properties"), Properties);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// FIND ASSETS
// ============================================================

FString FNwiroIKAssetTools::FindAssets(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SearchTerm = Cmd->GetStringField(TEXT("searchTerm"));
	FString ClassFilter = Cmd->GetStringField(TEXT("classFilter"));
	FString PathFilter = Cmd->GetStringField(TEXT("path"));
	int32 MaxResults = Cmd->HasField(TEXT("maxResults")) ? (int32)Cmd->GetNumberField(TEXT("maxResults")) : 50;

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FARFilter Filter;
	Filter.PackagePaths.Add(PathFilter.IsEmpty() ? TEXT("/Game") : FName(*PathFilter));
	Filter.bRecursivePaths = true;

	// Class filter
	if (!ClassFilter.IsEmpty())
	{
		UClass* FilterClass = FindObject<UClass>(nullptr, *ClassFilter);
		if (!FilterClass)
		{
			// Try common class names
			FString FullPath = FString::Printf(TEXT("/Script/Engine.%s"), *ClassFilter);
			FilterClass = FindObject<UClass>(nullptr, *FullPath);
		}
		if (FilterClass)
		{
			Filter.ClassPaths.Add(FilterClass->GetClassPathName());
			Filter.bRecursiveClasses = true;
		}
	}

	TArray<FAssetData> Assets;
	ARM.Get().GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& A : Assets)
	{
		FString Name = A.AssetName.ToString();
		if (!SearchTerm.IsEmpty() && !Name.Contains(SearchTerm, ESearchCase::IgnoreCase)) continue;

		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("path"), FString::Printf(TEXT("%s.%s"), *A.PackageName.ToString(), *Name));
		Obj->SetStringField(TEXT("class"), A.AssetClassPath.GetAssetName().ToString());
		Results.Add(MakeShareable(new FJsonValueObject(Obj)));

		if (Results.Num() >= MaxResults) break;
	}

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetBoolField(TEXT("success"), true);
	Root->SetArrayField(TEXT("assets"), Results);
	Root->SetNumberField(TEXT("count"), Results.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// GET ASSET THUMBNAIL
// ============================================================

FString FNwiroIKAssetTools::GetAssetThumbnail(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path;
	for (const TCHAR* K : { TEXT("path"), TEXT("assetPath"), TEXT("asset_path"), TEXT("name") }) {
		if (Cmd->TryGetStringField(K, Path) && !Path.IsEmpty()) break;
	}
	int32 Width = Cmd->HasField(TEXT("width")) ? (int32)Cmd->GetNumberField(TEXT("width")) : 256;
	int32 Height = Cmd->HasField(TEXT("height")) ? (int32)Cmd->GetNumberField(TEXT("height")) : 256;

	UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
	if (!Asset)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Asset not found: %s\"}"), *Path);

	// CRITICAL crash guard: GenerateThumbnailForObjectToSaveToDisk renders a
	// live preview, which for a Blueprint instantiates its generated-class CDO.
	// If that class is half-compiled (null ClassConstructor/ClassWithin), the
	// instantiation asserts InClass->ClassConstructor (UObjectGlobals.cpp:3396)
	// and HARD-CRASHES the editor. When the asset is a Blueprint whose class
	// isn't safely constructible, skip the live render and go straight to the
	// on-disk cached thumbnail (Tier 2) instead of crashing.
	auto IsClassThumbnailSafe = [](UClass* C) -> bool
	{
		return C && C->IsValidLowLevel()
			&& C->ClassConstructor != nullptr
			&& C->ClassWithin != nullptr
			&& !C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
	};
	bool bSkipLiveRender = false;
	if (UBlueprint* BP = Cast<UBlueprint>(Asset))
	{
		bSkipLiveRender = !IsClassThumbnailSafe(BP->GeneratedClass);
	}

	// Tier 1: fresh render via ThumbnailTools (skipped if BP class is unsafe).
	FObjectThumbnail* Thumbnail = bSkipLiveRender
		? nullptr
		: ThumbnailTools::GenerateThumbnailForObjectToSaveToDisk(Asset);
	const TCHAR* ThumbnailSource = TEXT("generated");

	// Tier 2: cached thumbnail from the asset's package — Blueprint thumbnails
	// often skip the live render and just read from on-disk cache. This is
	// what the editor's Content Browser does.
	if (!Thumbnail || Thumbnail->GetImageWidth() == 0)
	{
		FThumbnailMap LoadedThumbnails;
		const FName FullName = *FString::Printf(TEXT("%s %s"),
			*Asset->GetClass()->GetName(),
			*Asset->GetPathName());
		TArray<FName> Names; Names.Add(FullName);
		ThumbnailTools::ConditionallyLoadThumbnailsForObjects(Names, LoadedThumbnails);
		if (FObjectThumbnail* Cached = LoadedThumbnails.Find(FullName))
		{
			if (Cached->GetImageWidth() > 0)
			{
				Thumbnail = Cached;
				ThumbnailSource = TEXT("package-cache");
			}
		}
	}

	if (!Thumbnail || Thumbnail->GetImageWidth() == 0)
		return TEXT("{\"success\":false,\"error\":\"Failed to generate thumbnail (asset has no cached thumbnail and live render returned empty — common for newly-created BPs that were never opened in the editor)\"}");

	// Convert to PNG
	IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> PngWrapper = ImageModule.CreateImageWrapper(EImageFormat::PNG);

	const TArray<uint8>& ImgData = Thumbnail->GetUncompressedImageData();
	// Defensive: cached thumbnails sometimes report a non-zero width/height
	// but still have empty pixel data — calling SetRaw on a null buffer
	// hits a fatal ImageWrapperBase assertion. Bail out cleanly instead.
	if (ImgData.Num() == 0 || ImgData.GetData() == nullptr)
		return TEXT("{\"success\":false,\"error\":\"Thumbnail has zero-byte pixel data (cached header present but pixels missing — open the asset in the editor once to refresh)\"}");
	if (!PngWrapper->SetRaw(ImgData.GetData(), ImgData.Num(), Thumbnail->GetImageWidth(), Thumbnail->GetImageHeight(), ERGBFormat::BGRA, 8))
		return TEXT("{\"success\":false,\"error\":\"Failed to encode thumbnail\"}");

	const TArray64<uint8> PngData = PngWrapper->GetCompressed();

	// Save to temp file
	FString FileName = FPaths::GetBaseFilename(Path) + TEXT("_thumb.png");
	FString SavePath = FPaths::ProjectSavedDir() / TEXT("NwiroThumbnails") / FileName;
	FFileHelper::SaveArrayToFile(PngData, *SavePath);

	return FString::Printf(TEXT("{\"success\":true,\"path\":\"%s\",\"width\":%d,\"height\":%d,\"source\":\"%s\"}"),
		*SavePath.Replace(TEXT("\\"), TEXT("/")), Thumbnail->GetImageWidth(), Thumbnail->GetImageHeight(), ThumbnailSource);
}
