// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKMaterialTools.h"
#include "NwiroIKPolicyResponse.h"
#include "NwiroIKTransactionHelper.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionAbs.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "MaterialEditingLibrary.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Json.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroMat, Log, All);

// Brings policy 1.1 response helpers (MakePolicyDiagnostic, MakePolicyFailureResponse,
// MakeCallId, ValidateJsonArrayObjects, etc.) into scope so existing call sites
// don't need to qualify. See NwiroIKPolicyResponse.h.
using namespace NwiroIKPolicy;

static FString NormalizeKey(const FString& Key)
{
	// Strip all non-alphanumeric characters (underscores, hyphens, spaces, dots, etc.)
	// and lowercase. So "material_function", "material-function", "Material.Function"
	// all collapse to "materialfunction". Callers compare against alphanumeric-only
	// canonicals — all current uses are safe with this broader stripping.
	FString Result;
	Result.Reserve(Key.Len());
	for (TCHAR C : Key)
	{
		if (FChar::IsAlnum(C))
		{
			Result.AppendChar(FChar::ToLower(C));
		}
	}
	return Result;
}

static TSharedPtr<FJsonValue> MakeDiagnostic(
	const FString& Code,
	const FString& Message,
	const FString& Operation = TEXT(""),
	const FString& Ref = TEXT(""),
	const FString& Field = TEXT(""),
	const FString& Value = TEXT(""),
	const TArray<FString>& Allowed = TArray<FString>()
)
{
	TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
	Obj->SetStringField(TEXT("code"), Code);
	Obj->SetStringField(TEXT("message"), Message);
	if (!Operation.IsEmpty()) Obj->SetStringField(TEXT("operation"), Operation);
	if (!Ref.IsEmpty()) Obj->SetStringField(TEXT("ref"), Ref);
	if (!Field.IsEmpty()) Obj->SetStringField(TEXT("field"), Field);
	if (!Value.IsEmpty()) Obj->SetStringField(TEXT("value"), Value);
	if (Allowed.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> AllowedArr;
		for (const FString& Item : Allowed)
			AllowedArr.Add(MakeShareable(new FJsonValueString(Item)));
		Obj->SetArrayField(TEXT("allowed"), AllowedArr);
	}
	return MakeShareable(new FJsonValueObject(Obj));
}

static void AppendDiagnostics(TArray<TSharedPtr<FJsonValue>>& Target, const TArray<TSharedPtr<FJsonValue>>& Source)
{
	for (const TSharedPtr<FJsonValue>& Item : Source)
		Target.Add(Item);
}

static void AddDiagnosticsToJson(
	const TSharedRef<FJsonObject>& Result,
	const TArray<TSharedPtr<FJsonValue>>& Errors,
	const TArray<TSharedPtr<FJsonValue>>& Warnings
)
{
	if (Errors.Num() > 0) Result->SetArrayField(TEXT("errors"), Errors);
	if (Warnings.Num() > 0) Result->SetArrayField(TEXT("warnings"), Warnings);
}

// Policy 1.1 response/envelope helpers (MakeEmptyJsonArray, SetStringArrayField,
// MakePolicyDiagnostic, SerializeJsonObject, MakeCallId, MakePolicyFailureResponse,
// JsonFieldIsType, ValidateJsonArrayObjects, ConvertDiagnosticForPolicy) live in
// NwiroIKPolicyResponse.h/.cpp (namespace NwiroIKPolicy). Brought into scope by the
// `using namespace NwiroIKPolicy` at the top of this file.
//
// NOTE: `NormalizeMaterialFunctionPolicyResponse` used to live here and re-shape
// the legacy `EditMaterialFunction` JSON output into a policy 1.1 envelope. G15
// retired that path — `EditMaterialFunction` now calls the `Do*` helpers
// directly and builds the envelope from `FNwiroIKMatResult` records.

static TArray<FString> GetExpressionInputNames(UMaterialExpression* Expr)
{
	TArray<FString> Names;
	if (!Expr) return Names;
	for (int32 i = 0; ; i++)
	{
		const FExpressionInput* Input = Expr->GetInput(i);
		if (!Input) break;
		FString PinName = Expr->GetInputName(i).ToString();
		Names.Add(PinName.IsEmpty() ? FString::FromInt(i) : PinName);
	}
	return Names;
}

static TArray<FString> GetExpressionOutputNames(UMaterialExpression* Expr)
{
	TArray<FString> Names;
	if (!Expr) return Names;
	TArray<FExpressionOutput> Outputs = Expr->GetOutputs();
	for (int32 i = 0; i < Outputs.Num(); i++)
	{
		FString PinName = Outputs[i].OutputName.ToString();
		// FName(NAME_None).ToString() == "None"; treat as unnamed and surface the index
		const bool bUnnamed = PinName.IsEmpty() || PinName.Equals(TEXT("None"), ESearchCase::IgnoreCase);
		Names.Add(bUnnamed ? FString::FromInt(i) : PinName);
	}
	return Names;
}

// Direct FExpressionInput assignment fallback when UMaterialEditingLibrary cannot
// match an unnamed source output by name. Only safe when the source has a single
// output AND the caller didn't request a specific named output.
static bool TryConnectByOutputIndex(
	UMaterialExpression* From,
	int32 FromOutputIdx,
	UMaterialExpression* To,
	const FString& ToInputName)
{
	if (!From || !To) return false;
	TArray<FExpressionOutput> Outputs = From->GetOutputs();
	if (!Outputs.IsValidIndex(FromOutputIdx)) return false;

	// Resolve target input by name (case-insensitive) or fall back to index 0 when empty
	int32 ToIdx = INDEX_NONE;
	FExpressionInput* ToInput = nullptr;
	if (ToInputName.IsEmpty())
	{
		ToInput = To->GetInput(0);
		ToIdx = 0;
	}
	else
	{
		for (int32 i = 0; ; i++)
		{
			FExpressionInput* In = To->GetInput(i);
			if (!In) break;
			const FString PinName = To->GetInputName(i).ToString();
			if (PinName.Equals(ToInputName, ESearchCase::IgnoreCase))
			{
				ToInput = In;
				ToIdx = i;
				break;
			}
		}
	}
	if (!ToInput) return false;

	const FExpressionOutput& Out = Outputs[FromOutputIdx];
	ToInput->Expression = From;
	ToInput->OutputIndex = FromOutputIdx;
	ToInput->Mask = Out.Mask;
	ToInput->MaskR = Out.MaskR;
	ToInput->MaskG = Out.MaskG;
	ToInput->MaskB = Out.MaskB;
	ToInput->MaskA = Out.MaskA;
	return true;
}

// True ONLY when (a) the source has exactly one output, AND (b) the caller named that
// output using one of the recognized aliases for an unnamed pin: empty, "None", "0",
// or "Output" (the inspect alias). A specific wrong pin name like "SomeBogus" must NOT
// trigger the fallback — those should keep failing loudly with the allowed-pins list.
// True when the requested source output name refers to an unnamed single output
// (empty string, "None", "0", or matches the only pin's name). Used to gate the
// index-based fallback so multi-output expressions still fail loudly on bad names.
static bool IsUnnamedSingleOutputRequest(UMaterialExpression* From, const FString& FromOutputName)
{
	if (!From) return false;
	TArray<FExpressionOutput> Outputs = From->GetOutputs();
	if (Outputs.Num() != 1) return false;

	if (FromOutputName.IsEmpty()) return true;
	if (FromOutputName.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return true;
	if (FromOutputName == TEXT("0")) return true;
	if (FromOutputName.Equals(TEXT("Output"), ESearchCase::IgnoreCase)) return true;
	return false;
}

static bool ResolveFunctionInputType(
	const FString& RawType,
	EFunctionInputType& OutType,
	FString& OutCanonical,
	FString& OutAliasMessage
)
{
	static const TMap<FString, TPair<EFunctionInputType, FString>> TypeMap = {
		{ TEXT("scalar"),             TPair<EFunctionInputType, FString>(FunctionInput_Scalar, TEXT("Scalar")) },
		{ TEXT("float"),              TPair<EFunctionInputType, FString>(FunctionInput_Scalar, TEXT("Scalar")) },
		{ TEXT("number"),             TPair<EFunctionInputType, FString>(FunctionInput_Scalar, TEXT("Scalar")) },
		{ TEXT("vector2"),            TPair<EFunctionInputType, FString>(FunctionInput_Vector2, TEXT("Vector2")) },
		{ TEXT("vec2"),               TPair<EFunctionInputType, FString>(FunctionInput_Vector2, TEXT("Vector2")) },
		{ TEXT("float2"),             TPair<EFunctionInputType, FString>(FunctionInput_Vector2, TEXT("Vector2")) },
		{ TEXT("vector"),             TPair<EFunctionInputType, FString>(FunctionInput_Vector3, TEXT("Vector3")) },
		{ TEXT("vector3"),            TPair<EFunctionInputType, FString>(FunctionInput_Vector3, TEXT("Vector3")) },
		{ TEXT("vec3"),               TPair<EFunctionInputType, FString>(FunctionInput_Vector3, TEXT("Vector3")) },
		{ TEXT("float3"),             TPair<EFunctionInputType, FString>(FunctionInput_Vector3, TEXT("Vector3")) },
		{ TEXT("color"),              TPair<EFunctionInputType, FString>(FunctionInput_Vector3, TEXT("Vector3")) },
		{ TEXT("rgb"),                TPair<EFunctionInputType, FString>(FunctionInput_Vector3, TEXT("Vector3")) },
		{ TEXT("vector4"),            TPair<EFunctionInputType, FString>(FunctionInput_Vector4, TEXT("Vector4")) },
		{ TEXT("vec4"),               TPair<EFunctionInputType, FString>(FunctionInput_Vector4, TEXT("Vector4")) },
		{ TEXT("float4"),             TPair<EFunctionInputType, FString>(FunctionInput_Vector4, TEXT("Vector4")) },
		{ TEXT("rgba"),               TPair<EFunctionInputType, FString>(FunctionInput_Vector4, TEXT("Vector4")) },
		{ TEXT("linearcolor"),        TPair<EFunctionInputType, FString>(FunctionInput_Vector4, TEXT("Vector4")) },
		{ TEXT("texture2d"),          TPair<EFunctionInputType, FString>(FunctionInput_Texture2D, TEXT("Texture2D")) },
		{ TEXT("texture"),            TPair<EFunctionInputType, FString>(FunctionInput_Texture2D, TEXT("Texture2D")) },
		{ TEXT("texturecube"),        TPair<EFunctionInputType, FString>(FunctionInput_TextureCube, TEXT("TextureCube")) },
		{ TEXT("cube"),               TPair<EFunctionInputType, FString>(FunctionInput_TextureCube, TEXT("TextureCube")) },
		{ TEXT("staticbool"),         TPair<EFunctionInputType, FString>(FunctionInput_StaticBool, TEXT("StaticBool")) },
		{ TEXT("bool"),               TPair<EFunctionInputType, FString>(FunctionInput_StaticBool, TEXT("StaticBool")) },
		{ TEXT("boolean"),            TPair<EFunctionInputType, FString>(FunctionInput_StaticBool, TEXT("StaticBool")) },
		{ TEXT("materialattributes"), TPair<EFunctionInputType, FString>(FunctionInput_MaterialAttributes, TEXT("MaterialAttributes")) },
	};

	const FString Normalized = NormalizeKey(RawType);
	const TPair<EFunctionInputType, FString>* Found = TypeMap.Find(Normalized);
	if (!Found) return false;

	OutType = Found->Key;
	OutCanonical = Found->Value;
	OutAliasMessage.Empty();
	if (!RawType.Equals(OutCanonical, ESearchCase::CaseSensitive))
	{
		OutAliasMessage = FString::Printf(TEXT("Normalized inputType alias '%s' to '%s'"), *RawType, *OutCanonical);
	}
	return true;
}

static TArray<FString> GetAllowedFunctionInputTypes()
{
	return {
		TEXT("Scalar"), TEXT("Vector2"), TEXT("Vector3"), TEXT("Vector4"),
		TEXT("Texture2D"), TEXT("TextureCube"), TEXT("StaticBool"), TEXT("MaterialAttributes"),
		TEXT("aliases: float, number, vector, color, rgb, rgba, vec2, vec3, vec4, float2, float3, float4, texture, texture_2d, texture_cube, bool, boolean, material_attributes")
	};
}

// Resolve a MaterialFunctionCall path from an alias-tolerant key set.
// Returns true when a recognized key has a non-empty value (caller should bind).
// Returns false otherwise; bOutKeyPresentButEmpty distinguishes "no key found" from
// "key found but value is empty" — the second case is a real failure the caller
// must diagnose, not a silent skip.
//
// OutAliasUsed is the actual key the caller wrote ONLY when it differs from the
// canonical "materialFunction"; empty otherwise. This keeps the convention
// "AliasUsed.IsEmpty() <=> canonical key was used".
//
// ExplicitAliases for MaterialFunctionCall.materialFunction. Note: "assetPath"
// here is the function-to-bind path scoped to this expression object — NOT the
// top-level edit_material/edit_material_function assetPath, which the wrapper
// already consumed before this helper sees Obj.
static bool ResolveMaterialFunctionCallPath(
	const TSharedPtr<FJsonObject>& Obj,
	FString& OutPath,
	FString& OutAliasUsed,
	bool& bOutKeyPresentButEmpty)
{
	OutPath.Empty();
	OutAliasUsed.Empty();
	bOutKeyPresentButEmpty = false;

	static const TArray<FString> ExplicitAliases = {
		TEXT("materialFunction"),   // canonical
		TEXT("function"),
		TEXT("functionPath"),
		TEXT("function_path"),
		TEXT("material_function"),
		TEXT("assetPath"),
	};

	auto MarkResult = [&](const FString& KeyUsed, const FString& Val) -> bool
	{
		if (Val.IsEmpty())
		{
			OutAliasUsed = KeyUsed.Equals(TEXT("materialFunction")) ? FString() : KeyUsed;
			bOutKeyPresentButEmpty = true;
			return false;
		}
		OutPath = Val;
		OutAliasUsed = KeyUsed.Equals(TEXT("materialFunction")) ? FString() : KeyUsed;
		return true;
	};

	// First pass: exact key match
	for (const FString& Key : ExplicitAliases)
	{
		if (!Obj->HasField(Key)) continue;
		FString Val;
		Obj->TryGetStringField(Key, Val);
		return MarkResult(Key, Val);
	}

	// Second pass: NormalizeKey match for unknown casing/punctuation on ANY recognized
	// alias — not just the canonical. Otherwise `Function`, `FunctionPath`, `Asset_Path`
	// would silently drop because they don't normalize to "materialfunction".
	// Note: `material_function` and `MaterialFunction` collapse to "materialfunction";
	// `function_path` and `functionPath` both collapse to "functionpath".
	static const TSet<FString> RecognizedAliasNorms = {
		NormalizeKey(TEXT("materialFunction")), // "materialfunction"
		NormalizeKey(TEXT("function")),         // "function"
		NormalizeKey(TEXT("functionPath")),     // "functionpath"
		NormalizeKey(TEXT("assetPath")),        // "assetpath"
	};
	for (const auto& Pair : Obj->Values)
	{
		const FString Key(*Pair.Key);
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String) continue;
		if (!RecognizedAliasNorms.Contains(NormalizeKey(Key))) continue;
		return MarkResult(Key, Pair.Value->AsString());
	}

	return false;
}

// ============================================================
// FIND MATERIALS
// ============================================================

FString FNwiroIKMaterialTools::FindMaterials(const FString& JsonCommand)
{
	// Parse the incoming JSON command
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	FString SearchTerm = Cmd->GetStringField(TEXT("searchTerm")).ToLower();

	// Use the Asset Registry to find all Material and MI assets under /Game
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FARFilter Filter;
	// Include both base Materials and Material Instances
	Filter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
	Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = false; // Exact class match only

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	// Cap results at 50 to avoid flooding the LLM context
	const int32 MaxResults = 50;

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : Assets)
	{
		FString Name = Asset.AssetName.ToString();

		// Apply search filter (case-insensitive substring match)
		if (!SearchTerm.IsEmpty() && !Name.ToLower().Contains(SearchTerm))
		{
			continue;
		}

		Results.Add(MakeShareable(new FJsonValueObject(SerializeMaterialEntry(Asset))));

		if (Results.Num() >= MaxResults) break;
	}

	// Build response - include truncation flag so the agent knows to narrow its search
	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetArrayField(TEXT("materials"), Results);
	Root->SetNumberField(TEXT("count"), Results.Num());
	Root->SetBoolField(TEXT("truncated"), Results.Num() >= MaxResults);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// INSPECT MATERIAL
// ============================================================

FString FNwiroIKMaterialTools::InspectMaterial(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	FString AssetPath;
	if (!Cmd->TryGetStringField(TEXT("path"), AssetPath))
		Cmd->TryGetStringField(TEXT("assetPath"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return TEXT("{\"success\": false, \"error\": \"Missing 'path' field\"}");
	}

	UMaterialInterface* Mat = LoadMaterialByPath(AssetPath);
	if (!Mat)
	{
		return FString::Printf(TEXT("{\"success\": false, \"error\": \"Material not found: %s\"}"), *AssetPath);
	}

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetBoolField(TEXT("success"), true);
	Root->SetStringField(TEXT("name"), Mat->GetName());
	Root->SetStringField(TEXT("path"), Mat->GetPathName());
	Root->SetBoolField(TEXT("isInstance"), Mat->IsA<UMaterialInstanceConstant>());

	// -- Scalar parameters --
	TArray<FMaterialParameterInfo> ScalarInfos;
	TArray<FGuid> ScalarGuids;
	Mat->GetAllScalarParameterInfo(ScalarInfos, ScalarGuids);

	TArray<TSharedPtr<FJsonValue>> ScalarArr;
	for (const FMaterialParameterInfo& Info : ScalarInfos)
	{
		float Value = 0.f;
		Mat->GetScalarParameterValue(Info, Value);

		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Info.Name.ToString());
		Obj->SetNumberField(TEXT("value"), Value);
		ScalarArr.Add(MakeShareable(new FJsonValueObject(Obj)));
	}
	Root->SetArrayField(TEXT("scalars"), ScalarArr);

	// -- Vector parameters --
	TArray<FMaterialParameterInfo> VectorInfos;
	TArray<FGuid> VectorGuids;
	Mat->GetAllVectorParameterInfo(VectorInfos, VectorGuids);

	TArray<TSharedPtr<FJsonValue>> VectorArr;
	for (const FMaterialParameterInfo& Info : VectorInfos)
	{
		FLinearColor Value;
		Mat->GetVectorParameterValue(Info, Value);

		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Info.Name.ToString());

		TSharedRef<FJsonObject> Val = MakeShareable(new FJsonObject());
		Val->SetNumberField(TEXT("r"), Value.R);
		Val->SetNumberField(TEXT("g"), Value.G);
		Val->SetNumberField(TEXT("b"), Value.B);
		Val->SetNumberField(TEXT("a"), Value.A);
		Obj->SetObjectField(TEXT("value"), Val);

		VectorArr.Add(MakeShareable(new FJsonValueObject(Obj)));
	}
	Root->SetArrayField(TEXT("vectors"), VectorArr);

	// -- Texture parameters --
	TArray<FMaterialParameterInfo> TextureInfos;
	TArray<FGuid> TextureGuids;
	Mat->GetAllTextureParameterInfo(TextureInfos, TextureGuids);

	TArray<TSharedPtr<FJsonValue>> TextureArr;
	for (const FMaterialParameterInfo& Info : TextureInfos)
	{
		UTexture* Texture = nullptr;
		Mat->GetTextureParameterValue(Info, Texture);

		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Info.Name.ToString());
		Obj->SetStringField(TEXT("path"), Texture ? Texture->GetPathName() : TEXT(""));
		TextureArr.Add(MakeShareable(new FJsonValueObject(Obj)));
	}
	Root->SetArrayField(TEXT("textures"), TextureArr);

	// -- Surface-level material properties (master or instance-resolved) --
	if (UMaterial* AsMat = Mat->GetMaterial())
	{
		// ShadingModel — resolve via the enum reflection so we hand back a
		// human-readable name (Unlit/DefaultLit/Subsurface/…) instead of an int.
		const EMaterialShadingModel SM = AsMat->GetShadingModels().GetFirstShadingModel();
		if (const UEnum* SMEnum = StaticEnum<EMaterialShadingModel>())
		{
			Root->SetStringField(TEXT("shadingModel"),
				SMEnum->GetNameStringByValue((int64)SM));
		}
		// Blend mode — emit both the EFFECTIVE mode (what the engine renders with,
		// after Masked→Opaque downgrade when bCanMaskedBeAssumedOpaque etc.) AND
		// the RAW field value (what the user set). They disagree often enough that
		// surfacing only one of them is misleading.
		const EBlendMode BMEffective = AsMat->GetBlendMode();
		const EBlendMode BMRaw = (EBlendMode)AsMat->BlendMode;
		if (const UEnum* BMEnum = StaticEnum<EBlendMode>())
		{
			Root->SetStringField(TEXT("blendMode"),
				BMEnum->GetNameStringByValue((int64)BMEffective));
			Root->SetStringField(TEXT("blendModeRaw"),
				BMEnum->GetNameStringByValue((int64)BMRaw));
		}
		Root->SetBoolField(TEXT("twoSided"), AsMat->IsTwoSided());
	}

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// CREATE / UPSERT MATERIAL INSTANCE
// ============================================================

FString FNwiroIKMaterialTools::CreateMaterialInstance(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	// -- Extract fields - accept multiple LLM naming variants --
	FString MIName = Cmd->GetStringField(TEXT("name"));

	// dest_folder / path / folder
	FString DestFolder;
	if (!Cmd->TryGetStringField(TEXT("dest_folder"), DestFolder))
		if (!Cmd->TryGetStringField(TEXT("path"), DestFolder))
			Cmd->TryGetStringField(TEXT("folder"), DestFolder);
	if (DestFolder.IsEmpty()) DestFolder = TEXT("/Game/Materials");

	// parent_material / parent / parentMaterial / master
	FString ParentPath;
	if (!Cmd->TryGetStringField(TEXT("parent_material"), ParentPath))
		if (!Cmd->TryGetStringField(TEXT("parent"), ParentPath))
			if (!Cmd->TryGetStringField(TEXT("parentMaterial"), ParentPath))
				Cmd->TryGetStringField(TEXT("master"), ParentPath);

	if (MIName.IsEmpty() || ParentPath.IsEmpty())
	{
		return TEXT("{\"success\": false, \"error\": \"Missing required fields: name and parent_material (or parent)\"}");
	}

	// Auto-prepend MI_ prefix to match UE5 asset naming convention
	if (!MIName.StartsWith(TEXT("MI_")))
	{
		MIName = TEXT("MI_") + MIName;
	}

	FString FullPath = FPaths::Combine(DestFolder, MIName);

	// -- Scalar / Vector / Texture arrays --
	const TArray<TSharedPtr<FJsonValue>>* Scalars  = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Vectors  = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Textures = nullptr;

	Cmd->TryGetArrayField(TEXT("scalars"),  Scalars);
	Cmd->TryGetArrayField(TEXT("vectors"),  Vectors);
	Cmd->TryGetArrayField(TEXT("textures"), Textures);

	static const TArray<TSharedPtr<FJsonValue>> Empty;
	const TArray<TSharedPtr<FJsonValue>>& ScalarRef  = Scalars  ? *Scalars  : Empty;
	const TArray<TSharedPtr<FJsonValue>>& VectorRef  = Vectors  ? *Vectors  : Empty;
	const TArray<TSharedPtr<FJsonValue>>& TextureRef = Textures ? *Textures : Empty;

	bool bIsUpdate = false;
	UMaterialInstanceConstant* MI = nullptr;

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateMaterialInstance", "AI: Create Material Instance"));

	// -- Upsert: load if exists, create if not --
	if (UEditorAssetLibrary::DoesAssetExist(FullPath))
	{
		// Asset already exists - load and update it
		MI = LoadMIByPath(FullPath);
		if (!MI)
		{
			return FString::Printf(TEXT("{\"success\": false, \"error\": \"Asset exists but failed to load: %s\"}"), *FullPath);
		}
		bIsUpdate = true;
		UE_LOG(LogNwiroMat, Log, TEXT("CreateMaterialInstance: Updating existing MI at '%s'"), *FullPath);
	}
	else
	{
		// Ensure destination folder exists before creating
		if (!UEditorAssetLibrary::DoesDirectoryExist(DestFolder))
		{
			UEditorAssetLibrary::MakeDirectory(DestFolder);
		}

		// Load the parent material
		UMaterialInterface* Parent = LoadMaterialByPath(ParentPath);
		if (!Parent)
		{
			return FString::Printf(TEXT("{\"success\": false, \"error\": \"Parent material not found: %s\"}"), *ParentPath);
		}

		// Create the MI asset through the asset tools pipeline
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
		Factory->InitialParent = Parent;

		UObject* NewAsset = AssetTools.CreateAsset(MIName, DestFolder, UMaterialInstanceConstant::StaticClass(), Factory);
		MI = Cast<UMaterialInstanceConstant>(NewAsset);

		if (!MI)
		{
			return FString::Printf(TEXT("{\"success\": false, \"error\": \"Failed to create MI asset: %s\"}"), *MIName);
		}

		UE_LOG(LogNwiroMat, Log, TEXT("CreateMaterialInstance: Created new MI '%s' from parent '%s'"), *MIName, *ParentPath);
	}

	// -- Apply all parameters --
	Tx.AlsoModify(MI);
	ApplyParametersToMI(MI, ScalarRef, VectorRef, TextureRef);

	MI->MarkPackageDirty();

	// -- Build response --
	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetBoolField(TEXT("success"), true);
	Root->SetStringField(TEXT("name"), MIName);
	Root->SetStringField(TEXT("path"), MI->GetPathName());
	Root->SetBoolField(TEXT("isUpdate"), bIsUpdate);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// UPDATE MATERIAL INSTANCE
// ============================================================

FString FNwiroIKMaterialTools::UpdateMaterialInstance(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	FString MIPath;
	for (const TCHAR* Key : { TEXT("path"), TEXT("assetPath"), TEXT("instancePath"), TEXT("materialInstancePath") })
	{
		if (Cmd->TryGetStringField(Key, MIPath) && !MIPath.IsEmpty()) break;
	}
	if (MIPath.IsEmpty())
	{
		return TEXT("{\"success\": false, \"error\": \"Missing path. Accepted: path / assetPath / instancePath\"}");
	}

	UMaterialInstanceConstant* MI = LoadMIByPath(MIPath);
	if (!MI)
	{
		return FString::Printf(TEXT("{\"success\": false, \"error\": \"Material instance not found: %s\"}"), *MIPath);
	}

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "UpdateMaterialInstance", "AI: Update Material Instance"), MI);

	// -- Extract parameter arrays --
	const TArray<TSharedPtr<FJsonValue>>* Scalars  = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Vectors  = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Textures = nullptr;

	Cmd->TryGetArrayField(TEXT("scalars"),  Scalars);
	Cmd->TryGetArrayField(TEXT("vectors"),  Vectors);
	Cmd->TryGetArrayField(TEXT("textures"), Textures);

	static const TArray<TSharedPtr<FJsonValue>> Empty;
	ApplyParametersToMI(
		MI,
		Scalars  ? *Scalars  : Empty,
		Vectors  ? *Vectors  : Empty,
		Textures ? *Textures : Empty
	);

	MI->MarkPackageDirty();

	UE_LOG(LogNwiroMat, Log, TEXT("UpdateMaterialInstance: Updated '%s'"), *MIPath);

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetBoolField(TEXT("success"), true);
	Root->SetStringField(TEXT("path"), MIPath);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// DELETE MATERIAL INSTANCE
// ============================================================

FString FNwiroIKMaterialTools::DeleteMaterialInstance(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	FString AssetPath = Cmd->GetStringField(TEXT("path"));
	if (AssetPath.IsEmpty())
	{
		return TEXT("{\"success\": false, \"error\": \"Missing 'path' field\"}");
	}

	if (!UEditorAssetLibrary::DoesAssetExist(AssetPath))
	{
		return FString::Printf(TEXT("{\"success\": false, \"error\": \"Asset does not exist: %s\"}"), *AssetPath);
	}

	bool bDeleted = UEditorAssetLibrary::DeleteAsset(AssetPath);
	if (!bDeleted)
	{
		// Usually means the asset is referenced elsewhere
		return FString::Printf(TEXT("{\"success\": false, \"error\": \"Could not delete asset (it may be in use): %s\"}"), *AssetPath);
	}

	UE_LOG(LogNwiroMat, Log, TEXT("DeleteMaterialInstance: Deleted '%s'"), *AssetPath);

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetBoolField(TEXT("success"), true);
	Root->SetStringField(TEXT("path"), AssetPath);
	Root->SetBoolField(TEXT("deleted"), true);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// APPLY MATERIAL TO ACTOR
// ============================================================

FString FNwiroIKMaterialTools::ApplyMaterial(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	// Accept the full range of arg names an LLM might invent — the schema canon
	// is materialPath/actorPath but agents naturally reach for material/actor
	// (matching other actor tools) and material_path/actor_path.
	FString MatPath, ActorTarget;
	for (const TCHAR* Key : { TEXT("materialPath"), TEXT("material_path"), TEXT("material"), TEXT("materialAsset"), TEXT("matPath") })
	{
		if (Cmd->TryGetStringField(Key, MatPath) && !MatPath.IsEmpty()) break;
	}
	for (const TCHAR* Key : { TEXT("actorPath"), TEXT("actor_path"), TEXT("actor"), TEXT("actorName"), TEXT("name"), TEXT("label"), TEXT("actor_target") })
	{
		if (Cmd->TryGetStringField(Key, ActorTarget) && !ActorTarget.IsEmpty()) break;
	}

	int32   SlotIndex   = 0;
	double TempSlotIndex = 0.0;
	if (Cmd->TryGetNumberField(TEXT("slot_index"), TempSlotIndex) ||
		Cmd->TryGetNumberField(TEXT("slotIndex"), TempSlotIndex))
	{
		SlotIndex = static_cast<int32>(TempSlotIndex);
	}

	if (MatPath.IsEmpty() || ActorTarget.IsEmpty())
	{
		return TEXT("{\"success\": false, \"error\": \"Missing required fields. Accepted: materialPath OR material; actorPath OR actor/name/label\"}");
	}

	// Load the material
	UMaterialInterface* Mat = LoadMaterialByPath(MatPath);
	if (!Mat)
	{
		return FString::Printf(TEXT("{\"success\": false, \"error\": \"Material not found: %s\"}"), *MatPath);
	}

	// -- Find actor in current level --
	// Priority: full path match -> actor label match -> actor name match
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return TEXT("{\"success\": false, \"error\": \"No editor world available\"}");
	}

	// Resolution strategy (most specific → most permissive). We need the
	// permissive tail because spawn_actor returns the requested name even
	// when UE5 appends a uniqueness suffix ("Foo" → "Foo_C_2"), and the
	// LLM also frequently passes just the actor label without the level
	// prefix. Each tier wins over the next.
	AActor* TargetActor = nullptr;
	AActor* CaseInsensitiveActor = nullptr;     // tier 2: case-insensitive label/name
	AActor* PrefixActor = nullptr;              // tier 3: actor name starts with target
	AActor* SubstringActor = nullptr;           // tier 4: target appears anywhere in name/label

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;

		// Tier 1 — exact full path match wins immediately.
		if (Actor->GetPathName() == ActorTarget)
		{
			TargetActor = Actor;
			break;
		}

		// Tier 1 — exact label / name match.
		if (Actor->GetActorLabel() == ActorTarget || Actor->GetName() == ActorTarget)
		{
			if (!TargetActor) TargetActor = Actor;
			continue;
		}

		// Tier 2 — case-insensitive label / name.
		if (Actor->GetActorLabel().Equals(ActorTarget, ESearchCase::IgnoreCase)
			|| Actor->GetName().Equals(ActorTarget, ESearchCase::IgnoreCase))
		{
			if (!CaseInsensitiveActor) CaseInsensitiveActor = Actor;
			continue;
		}

		// Tier 3 — actor name STARTS WITH target (handles UE5 uniqueness
		// suffixes like "_C_0", "_2", "_C_42").
		if (Actor->GetName().StartsWith(ActorTarget, ESearchCase::IgnoreCase)
			|| Actor->GetActorLabel().StartsWith(ActorTarget, ESearchCase::IgnoreCase))
		{
			if (!PrefixActor) PrefixActor = Actor;
			continue;
		}

		// Tier 4 — target string appears anywhere in name/label (LLM
		// sometimes drops or adds a prefix it pulled from BP class name).
		if (Actor->GetName().Contains(ActorTarget, ESearchCase::IgnoreCase)
			|| Actor->GetActorLabel().Contains(ActorTarget, ESearchCase::IgnoreCase))
		{
			if (!SubstringActor) SubstringActor = Actor;
		}
	}

	if (!TargetActor) TargetActor = CaseInsensitiveActor;
	if (!TargetActor) TargetActor = PrefixActor;
	if (!TargetActor) TargetActor = SubstringActor;

	if (!TargetActor)
	{
		return FString::Printf(TEXT("{\"success\": false, \"error\": \"Actor not found in level: %s\"}"), *ActorTarget);
	}

	// -- Apply to all mesh components --
	int32 AppliedCount = 0;

	TArray<UStaticMeshComponent*> StaticMeshes;
	TargetActor->GetComponents<UStaticMeshComponent>(StaticMeshes);

	TArray<USkeletalMeshComponent*> SkeletalMeshes;
	TargetActor->GetComponents<USkeletalMeshComponent>(SkeletalMeshes);

	for (UStaticMeshComponent* Comp : StaticMeshes)
	{
		Comp->SetMaterial(SlotIndex, Mat);
		AppliedCount++;
	}
	for (USkeletalMeshComponent* Comp : SkeletalMeshes)
	{
		Comp->SetMaterial(SlotIndex, Mat);
		AppliedCount++;
	}

	UE_LOG(LogNwiroMat, Log, TEXT("ApplyMaterial: Applied '%s' to %d meshes on actor '%s'"), *MatPath, AppliedCount, *ActorTarget);

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetBoolField(TEXT("success"), true);
	Root->SetNumberField(TEXT("appliedCount"), AppliedCount);
	Root->SetStringField(TEXT("materialPath"), MatPath);
	Root->SetStringField(TEXT("actorTarget"), ActorTarget);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// LIST MATERIAL SLOTS
// ============================================================

FString FNwiroIKMaterialTools::ListMaterialSlots(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	FString Target = Cmd->GetStringField(TEXT("target"));
	if (Target.IsEmpty())
	{
		return TEXT("{\"success\": false, \"error\": \"Missing 'target' field\"}");
	}

	TArray<TSharedPtr<FJsonValue>> SlotArr;

	// -- Try as actor in the current level first --
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) continue;

			bool bMatch = Actor->GetPathName() == Target
				|| Actor->GetActorLabel() == Target
				|| Actor->GetName() == Target;

			if (!bMatch) continue;

			// Collect slots from all static mesh components
			TArray<UStaticMeshComponent*> Comps;
			Actor->GetComponents<UStaticMeshComponent>(Comps);

			for (UStaticMeshComponent* Comp : Comps)
			{
				int32 NumSlots = Comp->GetNumMaterials();
				for (int32 i = 0; i < NumSlots; i++)
				{
					UMaterialInterface* Mat = Comp->GetMaterial(i);

					TSharedRef<FJsonObject> Slot = MakeShareable(new FJsonObject());
					Slot->SetNumberField(TEXT("index"), i);
					Slot->SetStringField(TEXT("component"), Comp->GetName());
					Slot->SetStringField(TEXT("material"), Mat ? Mat->GetPathName() : TEXT(""));
					SlotArr.Add(MakeShareable(new FJsonValueObject(Slot)));
				}
			}
			break; // Found actor, stop iterating
		}
	}

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetBoolField(TEXT("success"), true);
	Root->SetArrayField(TEXT("slots"), SlotArr);
	Root->SetNumberField(TEXT("count"), SlotArr.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// PRIVATE HELPERS
// ============================================================

UMaterialInterface* FNwiroIKMaterialTools::LoadMaterialByPath(const FString& AssetPath)
{
	// UEditorAssetLibrary::LoadAsset returns UObject* - cast to UMaterialInterface
	UObject* Loaded = UEditorAssetLibrary::LoadAsset(AssetPath);
	return Cast<UMaterialInterface>(Loaded);
}

UMaterialInstanceConstant* FNwiroIKMaterialTools::LoadMIByPath(const FString& AssetPath)
{
	UObject* Loaded = UEditorAssetLibrary::LoadAsset(AssetPath);
	return Cast<UMaterialInstanceConstant>(Loaded);
}

void FNwiroIKMaterialTools::ApplyParametersToMI(
	UMaterialInstanceConstant* MI,
	const TArray<TSharedPtr<FJsonValue>>& Scalars,
	const TArray<TSharedPtr<FJsonValue>>& Vectors,
	const TArray<TSharedPtr<FJsonValue>>& Textures)
{
	// -- Scalar parameters --
	for (const TSharedPtr<FJsonValue>& Val : Scalars)
	{
		const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
		if (!Obj.IsValid()) continue;

		FName ParamName(*Obj->GetStringField(TEXT("name")));
		float Value = static_cast<float>(Obj->GetNumberField(TEXT("value")));

		MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(ParamName), Value);
	}

	// -- Vector parameters --
	for (const TSharedPtr<FJsonValue>& Val : Vectors)
	{
		const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
		if (!Obj.IsValid()) continue;

		FName ParamName(*Obj->GetStringField(TEXT("name")));
		FLinearColor Color(
			static_cast<float>(Obj->GetNumberField(TEXT("r"))),
			static_cast<float>(Obj->GetNumberField(TEXT("g"))),
			static_cast<float>(Obj->GetNumberField(TEXT("b"))),
			Obj->HasField(TEXT("a")) ? static_cast<float>(Obj->GetNumberField(TEXT("a"))) : 1.0f
		);

		MI->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(ParamName), Color);
	}

	// -- Texture parameters --
	for (const TSharedPtr<FJsonValue>& Val : Textures)
	{
		const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
		if (!Obj.IsValid()) continue;

		FName    ParamName(*Obj->GetStringField(TEXT("name")));
		FString  TexPath = Obj->GetStringField(TEXT("path"));

		UTexture* Texture = Cast<UTexture>(UEditorAssetLibrary::LoadAsset(TexPath));
		if (Texture)
		{
			MI->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(ParamName), Texture);
		}
		else
		{
			UE_LOG(LogNwiroMat, Warning, TEXT("ApplyParametersToMI: Texture not found: %s"), *TexPath);
		}
	}
}

TSharedRef<FJsonObject> FNwiroIKMaterialTools::SerializeMaterialEntry(const FAssetData& Asset)
{
	TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
	FString Name = Asset.AssetName.ToString();

	// Build the full object path (Package.AssetName) used for loading
	FString Path = FString::Printf(TEXT("%s.%s"), *Asset.PackageName.ToString(), *Name);

	Obj->SetStringField(TEXT("name"), Name);
	Obj->SetStringField(TEXT("path"), Path);
	Obj->SetBoolField(TEXT("isInstance"), Asset.AssetClassPath == UMaterialInstanceConstant::StaticClass()->GetClassPathName());

	return Obj;
}

// ============================================================
// EXPRESSION REF STORAGE
// ============================================================

TMap<FString, TWeakObjectPtr<UMaterialExpression>> FNwiroIKMaterialTools::ExpressionRefs;
// Cache: short name -> full path for recently created materials
static TMap<FString, FString> RecentMaterialPaths;

void FNwiroIKMaterialTools::ClearExpressionRefs()
{
	ExpressionRefs.Empty();
}

// ============================================================
// LOAD MASTER MATERIAL
// ============================================================

UMaterial* FNwiroIKMaterialTools::LoadMasterMaterial(const FString& PathOrName)
{
	// Empty path → LoadObject crashed CoreUObject (fuzz/material-3 found this
	// via inspect_material_graph{}). Return null cleanly.
	if (PathOrName.IsEmpty() || PathOrName.Len() > 1023) return nullptr;
	// Check recent cache first (for materials created this session)
	if (const FString* CachedPath = RecentMaterialPaths.Find(PathOrName))
	{
		UMaterial* CachedMat = LoadObject<UMaterial>(nullptr, **CachedPath);
		if (CachedMat)
		{
			UE_LOG(LogNwiroMat, Log, TEXT("LoadMasterMaterial: Cache hit '%s' -> '%s'"), *PathOrName, **CachedPath);
			return CachedMat;
		}
	}

	// Try direct path (full asset path like /Game/.../M_Rock.M_Rock)
	UMaterial* Mat = LoadObject<UMaterial>(nullptr, *PathOrName);
	if (Mat) return Mat;

	// Try with /Game/ prefix
	if (!PathOrName.StartsWith(TEXT("/")))
	{
		Mat = LoadObject<UMaterial>(nullptr, *(TEXT("/Game/") + PathOrName));
		if (Mat) return Mat;
	}

	// Force rescan so recently created assets are found
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();
	AR.ScanPathsSynchronous({TEXT("/Game")}, true);

	FARFilter Filter;
	Filter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	// Strip M_ prefix for comparison
	FString CleanName = PathOrName;
	if (CleanName.StartsWith(TEXT("M_"))) CleanName = CleanName.Mid(2);

	// Exact name match
	for (const FAssetData& Asset : Assets)
	{
		FString AssetName = Asset.AssetName.ToString();
		if (AssetName.Equals(PathOrName, ESearchCase::IgnoreCase))
		{
			UE_LOG(LogNwiroMat, Log, TEXT("LoadMasterMaterial: Found '%s' at '%s'"), *PathOrName, *Asset.GetObjectPathString());
			return Cast<UMaterial>(Asset.GetAsset());
		}
	}

	// Match without M_ prefix
	for (const FAssetData& Asset : Assets)
	{
		FString AssetName = Asset.AssetName.ToString();
		FString AssetClean = AssetName.StartsWith(TEXT("M_")) ? AssetName.Mid(2) : AssetName;
		if (AssetClean.Equals(CleanName, ESearchCase::IgnoreCase) || AssetName.Equals(CleanName, ESearchCase::IgnoreCase))
		{
			UE_LOG(LogNwiroMat, Log, TEXT("LoadMasterMaterial: Found '%s' as '%s'"), *PathOrName, *Asset.GetObjectPathString());
			return Cast<UMaterial>(Asset.GetAsset());
		}
	}

	// Partial/contains match
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetName.ToString().Contains(PathOrName, ESearchCase::IgnoreCase))
		{
			UE_LOG(LogNwiroMat, Log, TEXT("LoadMasterMaterial: Partial match '%s' -> '%s'"), *PathOrName, *Asset.GetObjectPathString());
			return Cast<UMaterial>(Asset.GetAsset());
		}
	}

	UE_LOG(LogNwiroMat, Warning, TEXT("LoadMasterMaterial: NOT FOUND '%s' (searched %d materials)"), *PathOrName, Assets.Num());
	return nullptr;
}

// ============================================================
// RESOLVE EXPRESSION CLASS
// ============================================================

UClass* FNwiroIKMaterialTools::ResolveExpressionClass(const FString& ClassName)
{
	static TMap<FString, UClass*> Cache;
	if (UClass** Cached = Cache.Find(ClassName.ToLower()))
	{
		return *Cached;
	}

	// Alias map: short names -> full class names
	static TMap<FString, FString> Aliases = {
		{ TEXT("texturesample"), TEXT("MaterialExpressionTextureSample") },
		{ TEXT("texture"), TEXT("MaterialExpressionTextureSample") },
		{ TEXT("texturesample2d"), TEXT("MaterialExpressionTextureSample") },
		{ TEXT("textureparameter"), TEXT("MaterialExpressionTextureSampleParameter2D") },
		{ TEXT("textureparameter2d"), TEXT("MaterialExpressionTextureSampleParameter2D") },
		{ TEXT("scalarparameter"), TEXT("MaterialExpressionScalarParameter") },
		{ TEXT("scalar"), TEXT("MaterialExpressionScalarParameter") },
		{ TEXT("vectorparameter"), TEXT("MaterialExpressionVectorParameter") },
		{ TEXT("staticswitchparameter"), TEXT("MaterialExpressionStaticSwitchParameter") },
		{ TEXT("multiply"), TEXT("MaterialExpressionMultiply") },
		{ TEXT("mul"), TEXT("MaterialExpressionMultiply") },
		{ TEXT("add"), TEXT("MaterialExpressionAdd") },
		{ TEXT("subtract"), TEXT("MaterialExpressionSubtract") },
		{ TEXT("sub"), TEXT("MaterialExpressionSubtract") },
		{ TEXT("divide"), TEXT("MaterialExpressionDivide") },
		{ TEXT("div"), TEXT("MaterialExpressionDivide") },
		{ TEXT("lerp"), TEXT("MaterialExpressionLinearInterpolate") },
		{ TEXT("linearinterpolate"), TEXT("MaterialExpressionLinearInterpolate") },
		{ TEXT("constant"), TEXT("MaterialExpressionConstant") },
		{ TEXT("constant2"), TEXT("MaterialExpressionConstant2Vector") },
		{ TEXT("constant2vector"), TEXT("MaterialExpressionConstant2Vector") },
		{ TEXT("constant3"), TEXT("MaterialExpressionConstant3Vector") },
		{ TEXT("constant3vector"), TEXT("MaterialExpressionConstant3Vector") },
		{ TEXT("constant4"), TEXT("MaterialExpressionConstant4Vector") },
		{ TEXT("constant4vector"), TEXT("MaterialExpressionConstant4Vector") },
		{ TEXT("texturecoordinate"), TEXT("MaterialExpressionTextureCoordinate") },
		{ TEXT("texcoord"), TEXT("MaterialExpressionTextureCoordinate") },
		{ TEXT("uv"), TEXT("MaterialExpressionTextureCoordinate") },
		{ TEXT("fresnel"), TEXT("MaterialExpressionFresnel") },
		{ TEXT("panner"), TEXT("MaterialExpressionPanner") },
		{ TEXT("time"), TEXT("MaterialExpressionTime") },
		{ TEXT("power"), TEXT("MaterialExpressionPower") },
		{ TEXT("pow"), TEXT("MaterialExpressionPower") },
		{ TEXT("clamp"), TEXT("MaterialExpressionClamp") },
		{ TEXT("oneminus"), TEXT("MaterialExpressionOneMinus") },
		{ TEXT("1-x"), TEXT("MaterialExpressionOneMinus") },
		{ TEXT("worldposition"), TEXT("MaterialExpressionWorldPosition") },
		{ TEXT("normalize"), TEXT("MaterialExpressionNormalize") },
		{ TEXT("append"), TEXT("MaterialExpressionAppendVector") },
		{ TEXT("appendvector"), TEXT("MaterialExpressionAppendVector") },
		{ TEXT("componentmask"), TEXT("MaterialExpressionComponentMask") },
		{ TEXT("mask"), TEXT("MaterialExpressionComponentMask") },
		{ TEXT("dot"), TEXT("MaterialExpressionDotProduct") },
		{ TEXT("dotproduct"), TEXT("MaterialExpressionDotProduct") },
		{ TEXT("abs"), TEXT("MaterialExpressionAbs") },
		{ TEXT("vertexcolor"), TEXT("MaterialExpressionVertexColor") },
		{ TEXT("functioninput"), TEXT("MaterialExpressionFunctionInput") },
		{ TEXT("functionoutput"), TEXT("MaterialExpressionFunctionOutput") },
		{ TEXT("materialfunctioncall"), TEXT("MaterialExpressionMaterialFunctionCall") },
		{ TEXT("functioncall"), TEXT("MaterialExpressionMaterialFunctionCall") },
	};

	FString Resolved = ClassName;
	if (const FString* Alias = Aliases.Find(ClassName.ToLower()))
	{
		Resolved = *Alias;
	}
	else if (!Resolved.StartsWith(TEXT("MaterialExpression")))
	{
		Resolved = TEXT("MaterialExpression") + Resolved;
	}

	FString SearchPath = FString::Printf(TEXT("/Script/Engine.%s"), *Resolved);
	UClass* ExprClass = FindObject<UClass>(nullptr, *SearchPath);

	if (!ExprClass)
	{
		// Try with U prefix
		SearchPath = FString::Printf(TEXT("/Script/Engine.U%s"), *Resolved);
		ExprClass = FindObject<UClass>(nullptr, *SearchPath);
	}

	if (ExprClass)
	{
		Cache.Add(ClassName.ToLower(), ExprClass);
	}

	return ExprClass;
}

// ============================================================
// RESOLVE MATERIAL PROPERTY
// ============================================================

bool FNwiroIKMaterialTools::ResolveMaterialProperty(const FString& PropName, int32& OutProp)
{
	static TMap<FString, int32> Map = {
		{ TEXT("basecolor"), (int32)MP_BaseColor },
		{ TEXT("metallic"), (int32)MP_Metallic },
		{ TEXT("specular"), (int32)MP_Specular },
		{ TEXT("roughness"), (int32)MP_Roughness },
		{ TEXT("anisotropy"), (int32)MP_Anisotropy },
		{ TEXT("emissivecolor"), (int32)MP_EmissiveColor },
		{ TEXT("emissive"), (int32)MP_EmissiveColor },
		{ TEXT("opacity"), (int32)MP_Opacity },
		{ TEXT("opacitymask"), (int32)MP_OpacityMask },
		{ TEXT("normal"), (int32)MP_Normal },
		{ TEXT("tangent"), (int32)MP_Tangent },
		{ TEXT("worldpositionoffset"), (int32)MP_WorldPositionOffset },
		{ TEXT("subsurfacecolor"), (int32)MP_SubsurfaceColor },
		{ TEXT("ambientocclusion"), (int32)MP_AmbientOcclusion },
	};

	if (const int32* Found = Map.Find(PropName.ToLower()))
	{
		OutProp = *Found;
		return true;
	}
	return false;
}

// ============================================================
// SET EXPRESSION PROPERTIES
// ============================================================

void FNwiroIKMaterialTools::SetExpressionProperties(UMaterialExpression* Expr, const TSharedPtr<FJsonObject>& Props, TArray<FString>* OutWarnings, TArray<TSharedPtr<FJsonValue>>* OutErrors, TArray<FString>* OutUnknownPropertyKeys, TMap<FString, FString>* OutAssetMissPaths)
{
	if (!Expr || !Props.IsValid()) return;

	// Structural keys sent by the LLM to describe the expression - not actual UE properties.
	// Keys are stored in NormalizeKey form (lowercase, non-alphanumerics stripped).
	// MaterialFunctionCall aliases are listed here in addition to canonical "materialfunction"
	// so the binding-side resolver (ResolveMaterialFunctionCallPath) can claim them without
	// SetExpressionProperties also trying to set them as UE properties.
	// `materialfunction` covers: materialFunction, MaterialFunction, material_function, material-function.
	// `functionpath` covers:    functionPath, function_path, FunctionPath, Function_Path.
	// `assetpath` covers:       assetPath, asset_path, AssetPath.
	static const TSet<FString> ReservedKeys = {
		TEXT("type"), TEXT("class"), TEXT("name"), TEXT("x"), TEXT("y"), TEXT("ref"), TEXT("id"),
		TEXT("properties"), TEXT("inputtype"), TEXT("sortpriority"),
		TEXT("materialfunction"), TEXT("function"), TEXT("functionpath"), TEXT("assetpath")
	};

	// FunctionInput-scoped reserved keys. These fields are handled directly by
	// the FunctionInput branch (precheck-validated previewValue + alias, and
	// explicit useAsDefault). The property-bag iteration must skip them on
	// FunctionInput so it doesn't emit a spurious PROPERTY_NOT_FOUND for keys
	// that were already applied. They are NOT globally reserved because
	// `defaultValue` has legitimate special-case routing to
	// ScalarParameter::DefaultValue and VectorParameter::DefaultValue on
	// other expression classes.
	const bool bExprIsFunctionInput = Expr && Expr->IsA<UMaterialExpressionFunctionInput>();

	for (auto& Pair : Props->Values)
	{
		const FString Key(*Pair.Key);
		const FString NKeyEarly = NormalizeKey(Key);
		if (bExprIsFunctionInput &&
			(NKeyEarly == TEXT("previewvalue") ||
			 NKeyEarly == TEXT("useasdefault") ||
			 NKeyEarly == TEXT("defaultvalue")))
		{
			continue;
		}
		if (ReservedKeys.Contains(NKeyEarly)) continue;

		FName PropName(*Key);
		const FString NKey = NormalizeKey(Key);

		// Special handling for parameter default values.
		// LLM may send in multiple formats - handle all three:
		//   array:  [r, g, b, a]
		//   object: {"r":0,"g":0.5,"b":1}
		//   string: "(0, 0.5, 1)" or "0.5"
		if (NKey == TEXT("default") || NKey == TEXT("defaultvalue"))
		{
			if (UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
			{
				if (Pair.Value->Type == EJson::Array)
				{
					const TArray<TSharedPtr<FJsonValue>>& Arr = Pair.Value->AsArray();
					VP->DefaultValue.R = Arr.Num() > 0 ? (float)Arr[0]->AsNumber() : 0.f;
					VP->DefaultValue.G = Arr.Num() > 1 ? (float)Arr[1]->AsNumber() : 0.f;
					VP->DefaultValue.B = Arr.Num() > 2 ? (float)Arr[2]->AsNumber() : 0.f;
					VP->DefaultValue.A = Arr.Num() > 3 ? (float)Arr[3]->AsNumber() : 1.f;
				}
				else if (Pair.Value->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject>& ColorObj = Pair.Value->AsObject();
					VP->DefaultValue.R = (float)ColorObj->GetNumberField(TEXT("r"));
					VP->DefaultValue.G = (float)ColorObj->GetNumberField(TEXT("g"));
					VP->DefaultValue.B = (float)ColorObj->GetNumberField(TEXT("b"));
					VP->DefaultValue.A = ColorObj->HasField(TEXT("a")) ? (float)ColorObj->GetNumberField(TEXT("a")) : 1.f;
				}
				else if (Pair.Value->Type == EJson::String)
				{
					// e.g. "(0, 0.5, 1)" - parse via ImportText_Direct on the DefaultValue property
					FProperty* Prop = VP->GetClass()->FindPropertyByName(FName("DefaultValue"));
					if (Prop) Prop->ImportText_Direct(*Pair.Value->AsString(), Prop->ContainerPtrToValuePtr<void>(VP), nullptr, PPF_None);
				}
				continue;
			}
			if (UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
			{
				if (Pair.Value->Type == EJson::Number)
					SP->DefaultValue = (float)Pair.Value->AsNumber();
				else if (Pair.Value->Type == EJson::String)
					SP->DefaultValue = FCString::Atof(*Pair.Value->AsString());
				continue;
			}
		}

		// Special handling for Texture property
		if (NKey == TEXT("texture"))
		{
			FString TexPath = Pair.Value->AsString();
			UTexture* Tex = LoadObject<UTexture>(nullptr, *TexPath);
			if (!Tex && !TexPath.StartsWith(TEXT("/")))
			{
				Tex = LoadObject<UTexture>(nullptr, *(TEXT("/Game/") + TexPath));
			}
			if (Tex)
			{
				if (UMaterialExpressionTextureSample* TexSample = Cast<UMaterialExpressionTextureSample>(Expr))
				{
					TexSample->Texture = Tex;
				}
			}
			else
			{
				const FString Msg = FString::Printf(TEXT("Texture asset not found: %s for expression '%s'"), *TexPath, *Expr->GetName());
				UE_LOG(LogNwiroMat, Warning, TEXT("%s"), *Msg);
				if (OutWarnings) OutWarnings->Add(Msg);
				// New-schema caller supplies OutAssetMissPaths → caller emits policy
				// NOT_FOUND + Skipped with correct field-path prefix. Legacy callers
				// (master-material DoAddExpressions) receive the legacy diagnostic.
				if (OutAssetMissPaths)
				{
					OutAssetMissPaths->Add(Key, TexPath);
				}
				else if (OutErrors)
				{
					OutErrors->Add(MakeDiagnostic(
						TEXT("ASSET_NOT_FOUND"),
						Msg,
						TEXT("setExpressionProperties"),
						Expr->GetName(),
						TEXT("texture"),
						TexPath
					));
				}
			}
			continue;
		}

		// Special handling for DefaultValue on ScalarParameter
		if (NKey == TEXT("defaultvalue"))
		{
			if (UMaterialExpressionScalarParameter* ScalarParam = Cast<UMaterialExpressionScalarParameter>(Expr))
			{
				ScalarParam->DefaultValue = (float)Pair.Value->AsNumber();
				continue;
			}
			if (UMaterialExpressionVectorParameter* VecParam = Cast<UMaterialExpressionVectorParameter>(Expr))
			{
				const TSharedPtr<FJsonObject>* ColorObj;
				if (Pair.Value->TryGetObject(ColorObj))
				{
					VecParam->DefaultValue = FLinearColor(
						(float)(*ColorObj)->GetNumberField(TEXT("r")),
						(float)(*ColorObj)->GetNumberField(TEXT("g")),
						(float)(*ColorObj)->GetNumberField(TEXT("b")),
						(*ColorObj)->HasField(TEXT("a")) ? (float)(*ColorObj)->GetNumberField(TEXT("a")) : 1.0f
					);
				}
				continue;
			}
		}

		// Special handling for Constant value
		if (NKey == TEXT("r") || NKey == TEXT("value"))
		{
			if (UMaterialExpressionConstant* Const = Cast<UMaterialExpressionConstant>(Expr))
			{
				Const->R = (float)Pair.Value->AsNumber();
				continue;
			}
		}

		// Special handling for Fresnel exponent
		if (NKey == TEXT("exponentin") || NKey == TEXT("exponent"))
		{
			if (UMaterialExpressionFresnel* Fresnel = Cast<UMaterialExpressionFresnel>(Expr))
			{
				Fresnel->Exponent = (float)Pair.Value->AsNumber();
				continue;
			}
		}

		// Generic FLinearColor channel setter - works for any expression with a FLinearColor UPROPERTY
		// Only intercepts if a FLinearColor property is actually found; otherwise falls through to
		// generic reflection so that e.g. UMaterialExpressionConstant "r:15" still sets float R via reflection.
		if (NKey == TEXT("r") || NKey == TEXT("g") || NKey == TEXT("b") || NKey == TEXT("a"))
		{
			bool bHandled = false;
			for (TFieldIterator<FStructProperty> It(Expr->GetClass()); It; ++It)
			{
				if (It->Struct == TBaseStructure<FLinearColor>::Get())
				{
					FLinearColor* ColorPtr = It->ContainerPtrToValuePtr<FLinearColor>(Expr);
					if (NKey == TEXT("r"))      ColorPtr->R = (float)Pair.Value->AsNumber();
					else if (NKey == TEXT("g")) ColorPtr->G = (float)Pair.Value->AsNumber();
					else if (NKey == TEXT("b")) ColorPtr->B = (float)Pair.Value->AsNumber();
					else if (NKey == TEXT("a")) ColorPtr->A = (float)Pair.Value->AsNumber();
					bHandled = true;
					break;
				}
			}
			if (bHandled) continue;
			// No FLinearColor found - fall through to generic reflection below
		}

		// Generic property reflection
		FProperty* Prop = Expr->GetClass()->FindPropertyByName(PropName);
		if (!Prop)
		{
			// G1/G9 fallback: surface the unrecognized key to the caller so the
			// new-schema aggregator can emit PROPERTY_NOT_FOUND + a matching
			// skipped[] row. We keep the log/warning for legacy callers that
			// don't supply OutUnknownPropertyKeys.
			UE_LOG(LogNwiroMat, Warning, TEXT("Unrecognized material expression property '%s' - ignored"), *Pair.Key);
			if (OutWarnings) OutWarnings->Add(FString::Printf(TEXT("Unrecognized property '%s' on expression '%s' - ignored"), *Pair.Key, *Expr->GetName()));
			if (OutUnknownPropertyKeys) OutUnknownPropertyKeys->Add(Key);
			continue;
		}

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Expr);

		if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			FloatProp->SetPropertyValue(ValuePtr, (float)Pair.Value->AsNumber());
		}
		else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			DoubleProp->SetPropertyValue(ValuePtr, Pair.Value->AsNumber());
		}
		else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
		{
			IntProp->SetPropertyValue(ValuePtr, (int32)Pair.Value->AsNumber());
		}
		else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			BoolProp->SetPropertyValue(ValuePtr, Pair.Value->AsBool());
		}
		else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			NameProp->SetPropertyValue(ValuePtr, FName(*Pair.Value->AsString()));
		}
		else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			StrProp->SetPropertyValue(ValuePtr, Pair.Value->AsString());
		}
		else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			UObject* Loaded = UEditorAssetLibrary::LoadAsset(Pair.Value->AsString());
			if (Loaded)
			{
				ObjProp->SetObjectPropertyValue(ValuePtr, Loaded);
			}
			else
			{
				const FString AssetPath = Pair.Value->AsString();
				const FString Msg = FString::Printf(TEXT("Asset not found for property '%s' on expression '%s': %s"), *Pair.Key, *Expr->GetName(), *AssetPath);
				UE_LOG(LogNwiroMat, Warning, TEXT("%s"), *Msg);
				if (OutWarnings) OutWarnings->Add(Msg);
				// New-schema caller supplies OutAssetMissPaths → caller emits policy
				// NOT_FOUND + Skipped with correct field-path prefix. Legacy callers
				// receive the legacy diagnostic.
				if (OutAssetMissPaths)
				{
					OutAssetMissPaths->Add(Key, AssetPath);
				}
				else if (OutErrors)
				{
					OutErrors->Add(MakeDiagnostic(
						TEXT("ASSET_NOT_FOUND"),
						Msg,
						TEXT("setExpressionProperties"),
						Expr->GetName(),
						Key,
						AssetPath
					));
				}
			}
		}
	}
}

// ============================================================
// SET MATERIAL PROPERTIES
// ============================================================

void FNwiroIKMaterialTools::SetMaterialProperties(UMaterial* Mat, const TSharedPtr<FJsonObject>& Props)
{
	if (!Mat || !Props.IsValid()) return;

	// Accept the LLM-natural {property:"ShadingModel", value:"Unlit"} shape too —
	// rewrite it onto the snake_case Props the rest of this function knows about.
	if (Props->HasField(TEXT("property")) && Props->HasField(TEXT("value")))
	{
		FString PropName, PropVal;
		Props->TryGetStringField(TEXT("property"), PropName);
		Props->TryGetStringField(TEXT("value"), PropVal);
		const FString PNorm = PropName.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")).ToLower();
		if (PNorm == TEXT("shadingmodel"))   Props->SetStringField(TEXT("shading_model"), PropVal);
		else if (PNorm == TEXT("blendmode")) Props->SetStringField(TEXT("blend_mode"), PropVal);
		else if (PNorm == TEXT("twosided"))  Props->SetBoolField(TEXT("two_sided"), PropVal.ToLower() == TEXT("true"));
		else if (PNorm == TEXT("opacitymaskclipvalue")) Props->SetNumberField(TEXT("opacity_mask_clip_value"), FCString::Atof(*PropVal));
	}

	if (Props->HasField(TEXT("blend_mode")))
	{
		FString Mode = Props->GetStringField(TEXT("blend_mode")).ToLower();
		if (Mode == TEXT("opaque")) Mat->BlendMode = BLEND_Opaque;
		else if (Mode == TEXT("masked")) Mat->BlendMode = BLEND_Masked;
		else if (Mode == TEXT("translucent")) Mat->BlendMode = BLEND_Translucent;
		else if (Mode == TEXT("additive")) Mat->BlendMode = BLEND_Additive;
		else if (Mode == TEXT("modulate")) Mat->BlendMode = BLEND_Modulate;
	}

	if (Props->HasField(TEXT("shading_model")))
	{
		FString Model = Props->GetStringField(TEXT("shading_model")).ToLower();
		if (Model == TEXT("defaultlit") || Model == TEXT("default")) Mat->SetShadingModel(MSM_DefaultLit);
		else if (Model == TEXT("unlit")) Mat->SetShadingModel(MSM_Unlit);
		else if (Model == TEXT("subsurface")) Mat->SetShadingModel(MSM_Subsurface);
		else if (Model == TEXT("clearcoat")) Mat->SetShadingModel(MSM_ClearCoat);
		else if (Model == TEXT("twosidedfoliage")) Mat->SetShadingModel(MSM_TwoSidedFoliage);
	}

	if (Props->HasField(TEXT("two_sided")))
	{
		Mat->TwoSided = Props->GetBoolField(TEXT("two_sided"));
	}

	if (Props->HasField(TEXT("opacity_mask_clip_value")))
	{
		Mat->OpacityMaskClipValue = (float)Props->GetNumberField(TEXT("opacity_mask_clip_value"));
	}
}

// ============================================================
// CREATE MATERIAL
// ============================================================

FString FNwiroIKMaterialTools::CreateMaterial(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString DestFolder;
	if (!Cmd->TryGetStringField(TEXT("dest_folder"), DestFolder))
		if (!Cmd->TryGetStringField(TEXT("path"), DestFolder))
			Cmd->TryGetStringField(TEXT("folder"), DestFolder);
	if (Name.IsEmpty()) return TEXT("{\"success\": false, \"error\": \"Missing 'name'\"}");
	if (DestFolder.IsEmpty()) DestFolder = TEXT("/Game/Materials");

	// Add M_ prefix if missing
	if (!Name.StartsWith(TEXT("M_"))) Name = TEXT("M_") + Name;

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateMaterial", "AI: Create Material"));

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
	UObject* NewAsset = AssetTools.CreateAsset(Name, DestFolder, UMaterial::StaticClass(), Factory);
	UMaterial* Mat = Cast<UMaterial>(NewAsset);

	if (!Mat)
	{
		Tx.Cancel();
		return TEXT("{\"success\": false, \"error\": \"Failed to create material asset\"}");
	}
	Tx.AlsoModify(Mat);

	// Set initial properties
	const TSharedPtr<FJsonObject>* PropsObj;
	if (Cmd->TryGetObjectField(TEXT("properties"), PropsObj))
	{
		SetMaterialProperties(Mat, *PropsObj);
	}
	else
	{
		// Try top-level property fields
		SetMaterialProperties(Mat, Cmd);
	}

	// Cache the path so EditMaterial can find it by short name
	FString FullPath = Mat->GetPathName();
	RecentMaterialPaths.Add(Name, FullPath);
	// Also cache without M_ prefix
	if (Name.StartsWith(TEXT("M_")))
	{
		RecentMaterialPaths.Add(Name.Mid(2), FullPath);
	}

	// If expressions or connections were provided, process them via EditMaterial logic
	const TArray<TSharedPtr<FJsonValue>>* Exprs;
	const TArray<TSharedPtr<FJsonValue>>* Conns;
	bool bHasExprs = Cmd->TryGetArrayField(TEXT("expressions"), Exprs) || Cmd->TryGetArrayField(TEXT("add_expressions"), Exprs);
	bool bHasConns = Cmd->TryGetArrayField(TEXT("connections"), Conns) || Cmd->TryGetArrayField(TEXT("connect_expressions"), Conns);

	if (bHasExprs && Exprs)
		DoAddExpressions(Mat, *Exprs);
	if (bHasConns && Conns)
		DoConnectExpressions(Mat, *Conns);

	UMaterialEditingLibrary::RecompileMaterial(Mat);
	Mat->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Name);
	Result->SetStringField(TEXT("path"), FullPath);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);

	UE_LOG(LogNwiroMat, Log, TEXT("Created master material: %s at %s"), *Name, *FullPath);
	return Out;
}

// ============================================================
// EDIT MATERIAL (Compound)
// ============================================================

FString FNwiroIKMaterialTools::EditMaterial(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	FString MatPath;
	if (!Cmd->TryGetStringField(TEXT("material"), MatPath))
		if (!Cmd->TryGetStringField(TEXT("assetPath"), MatPath))
			Cmd->TryGetStringField(TEXT("path"), MatPath);
	if (MatPath.IsEmpty()) return TEXT("{\"success\": false, \"error\": \"Missing 'material' or 'assetPath' or 'path'\"}");

	// Clear expression refs from prior calls to prevent stale references
	ClearExpressionRefs();

	UMaterial* Mat = LoadMasterMaterial(MatPath);
	if (!Mat)
	{
		// Only auto-create when the path looks like a legitimate asset name.
		// Refuses fuzz/malformed inputs that would otherwise silently create
		// junk assets and slow the editor. Asset names are `[A-Za-z0-9_/.]+`.
		bool bLooksAsset = !MatPath.IsEmpty() && MatPath.Len() < 256;
		for (TCHAR C : MatPath)
		{
			if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('/') && C != TEXT('.') && C != TEXT('-'))
			{ bLooksAsset = false; break; }
		}
		if (!bLooksAsset)
			return FString::Printf(TEXT("{\"success\": false, \"error\": \"Material not found and path '%s' contains invalid characters for auto-create. Pass a valid /Game/-relative path or call create_material first.\"}"), *MatPath);

		FString CreateJson = FString::Printf(TEXT("{\"name\":\"%s\"}"), *MatPath);
		CreateMaterial(CreateJson);
		Mat = LoadMasterMaterial(MatPath);
		if (!Mat)
		{
			return FString::Printf(TEXT("{\"success\": false, \"error\": \"Material not found: %s\"}"), *MatPath);
		}
	}

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "EditMaterial", "AI: Edit Material"), Mat);

	TArray<FString> Messages;
	TArray<TSharedPtr<FJsonValue>> ErrorDiagnostics;
	TArray<TSharedPtr<FJsonValue>> WarningDiagnostics;
	TArray<TSharedPtr<FJsonValue>> CreatedAll;
	bool bAllOk = true;

	// Set material-level properties
	const TSharedPtr<FJsonObject>* PropsObj;
	if (Cmd->TryGetObjectField(TEXT("set_properties"), PropsObj))
	{
		SetMaterialProperties(Mat, *PropsObj);
		Messages.Add(TEXT("[set_properties] Material properties updated"));
	}

	// Helper lambda: get array field, or parse from JSON string if AI sent it as string
	auto GetArrayFieldFlexible = [&](const FString& Key1, const FString& Key2) -> TArray<TSharedPtr<FJsonValue>>
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Cmd->TryGetArrayField(Key1, Arr)) return *Arr;
		if (Cmd->TryGetArrayField(Key2, Arr)) return *Arr;

		// Fallback: AI may send array as JSON string - parse it
		for (const FString& Key : {Key1, Key2})
		{
			if (Cmd->HasField(Key) && Cmd->TryGetField(Key)->Type == EJson::String)
			{
				FString JsonStr = Cmd->GetStringField(Key);
				TArray<TSharedPtr<FJsonValue>> Parsed;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
				if (FJsonSerializer::Deserialize(Reader, Parsed))
				{
					return Parsed;
				}
			}
		}
		return {};
	};

	// Add expressions (accept "add_expressions", "expressions", or JSON string)
	TArray<TSharedPtr<FJsonValue>> AddExprsArr = GetArrayFieldFlexible(TEXT("add_expressions"), TEXT("expressions"));
	if (AddExprsArr.Num() > 0)
	{
		FNwiroIKMatResult R = DoAddExpressions(Mat, AddExprsArr);
		Messages.Add(FString::Printf(TEXT("[add_expressions] %s"), *R.Message));
		AppendDiagnostics(ErrorDiagnostics, R.Errors);
		AppendDiagnostics(WarningDiagnostics, R.Warnings);
		CreatedAll.Append(R.Created);
		if (!R.bSuccess) bAllOk = false;
	}

	// Connect expressions (accept "connect_expressions", "connections", or JSON string)
	TArray<TSharedPtr<FJsonValue>> ConnExprsArr = GetArrayFieldFlexible(TEXT("connect_expressions"), TEXT("connections"));
	if (ConnExprsArr.Num() > 0)
	{
		FNwiroIKMatResult R = DoConnectExpressions(Mat, ConnExprsArr);
		Messages.Add(FString::Printf(TEXT("[connect_expressions] %s"), *R.Message));
		AppendDiagnostics(ErrorDiagnostics, R.Errors);
		AppendDiagnostics(WarningDiagnostics, R.Warnings);
		if (!R.bSuccess) bAllOk = false;
	}

	// Remove expressions
	const TArray<TSharedPtr<FJsonValue>>* RemExprs;
	if (Cmd->TryGetArrayField(TEXT("remove_expressions"), RemExprs))
	{
		FNwiroIKMatResult R = DoRemoveExpressions(Mat, *RemExprs);
		Messages.Add(FString::Printf(TEXT("[remove_expressions] %s"), *R.Message));
		AppendDiagnostics(ErrorDiagnostics, R.Errors);
		AppendDiagnostics(WarningDiagnostics, R.Warnings);
		if (!R.bSuccess) bAllOk = false;
	}

	// Recompile
	bool bRecompile = true;
	if (Cmd->HasField(TEXT("recompile"))) bRecompile = Cmd->GetBoolField(TEXT("recompile"));
	if (bRecompile)
	{
		UMaterialEditingLibrary::RecompileMaterial(Mat);
		Mat->MarkPackageDirty();
		Messages.Add(TEXT("[recompile] Material recompiled"));
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), bAllOk);
	Result->SetStringField(TEXT("material"), Mat->GetName());

	TArray<TSharedPtr<FJsonValue>> MsgArr;
	for (const FString& M : Messages) MsgArr.Add(MakeShareable(new FJsonValueString(M)));
	Result->SetArrayField(TEXT("messages"), MsgArr);
	if (CreatedAll.Num() > 0)
	{
		Result->SetArrayField(TEXT("created"), CreatedAll);
		TSharedRef<FJsonObject> RefMap = MakeShareable(new FJsonObject());
		for (const TSharedPtr<FJsonValue>& V : CreatedAll)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid()) continue;
			// G2: function helpers emit `userName`; master-material helpers still emit `userRef`.
			// Prefer `userName` and fall back to `userRef` during the transition.
			FString UserName;
			if (!O->TryGetStringField(TEXT("userName"), UserName))
				O->TryGetStringField(TEXT("userRef"), UserName);
			const FString AssignedRef = O->GetStringField(TEXT("assignedRef"));
			if (!UserName.IsEmpty() && !AssignedRef.IsEmpty())
				RefMap->SetStringField(UserName, AssignedRef);
		}
		Result->SetObjectField(TEXT("refs"), RefMap);
	}
	AddDiagnosticsToJson(Result, ErrorDiagnostics, WarningDiagnostics);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// ADD EXPRESSIONS
// ============================================================

FNwiroIKMatResult FNwiroIKMaterialTools::DoAddExpressions(UMaterial* Mat, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	int32 Added = 0;
	TArray<FString> Errors;
	TArray<FString> Warnings;
	TArray<TSharedPtr<FJsonValue>> ErrorDiagnostics;
	TArray<TSharedPtr<FJsonValue>> WarningDiagnostics;
	TArray<TSharedPtr<FJsonValue>> CreatedRecords;

	// Auto-layout: spread nodes in a grid when no position specified
	// Start from left side, material result node is typically at x=0
	const int32 AutoSpacingX = 250;
	const int32 AutoSpacingY = 200;
	const int32 AutoStartX = -((Items.Num() + 1) / 2) * AutoSpacingX; // center horizontally
	int32 AutoIndex = 0;

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		const TSharedPtr<FJsonObject>& Obj = Item->AsObject();
		if (!Obj.IsValid()) continue;

		// Accept both "ref"/"class" and common aliases "name"/"type"
		FString Ref = Obj->HasField(TEXT("ref")) ? Obj->GetStringField(TEXT("ref"))
			: Obj->HasField(TEXT("name")) ? Obj->GetStringField(TEXT("name")) : TEXT("");
		FString ClassName = Obj->HasField(TEXT("class")) ? Obj->GetStringField(TEXT("class"))
			: Obj->HasField(TEXT("type")) ? Obj->GetStringField(TEXT("type")) : TEXT("");
		bool bHasPos = Obj->HasField(TEXT("x")) || Obj->HasField(TEXT("y"));
		int32 PosX = bHasPos ? (int32)Obj->GetNumberField(TEXT("x")) :
			AutoStartX + (AutoIndex % 4) * AutoSpacingX;
		int32 PosY = bHasPos ? (int32)Obj->GetNumberField(TEXT("y")) :
			(AutoIndex / 4) * AutoSpacingY;
		AutoIndex++;

		UClass* ExprClass = ResolveExpressionClass(ClassName);
		if (!ExprClass)
		{
			const FString Msg = FString::Printf(TEXT("Expression class not found: %s"), *ClassName);
			Errors.Add(Msg);
			ErrorDiagnostics.Add(MakeDiagnostic(TEXT("EXPRESSION_CLASS_NOT_FOUND"), Msg, TEXT("addExpressions"), Ref, TEXT("type"), ClassName));
			continue;
		}

		UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpression(Mat, ExprClass, PosX, PosY);
		if (!Expr)
		{
			const FString Msg = FString::Printf(TEXT("Failed to create expression: %s"), *ClassName);
			Errors.Add(Msg);
			ErrorDiagnostics.Add(MakeDiagnostic(TEXT("EXPRESSION_CREATE_FAILED"), Msg, TEXT("addExpressions"), Ref, TEXT("type"), ClassName));
			continue;
		}

		// Auto-set ParameterName from ref/name if it's a parameter expression
		if (!Ref.IsEmpty())
		{
			if (UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
				SP->ParameterName = FName(*Ref);
			else if (UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
				VP->ParameterName = FName(*Ref);
			else if (UMaterialExpressionTextureSampleParameter2D* TP = Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
				TP->ParameterName = FName(*Ref);
			// Also stash the user-given ref in Desc so DoConnectExpressions on a
			// LATER edit_material call can look the expression up by the same
			// name even though ExpressionRefs gets cleared between calls.
			if (Expr->Desc.IsEmpty()) Expr->Desc = Ref;
		}

		// MaterialFunctionCall - load and bind the referenced function asset
		if (UMaterialExpressionMaterialFunctionCall* FuncCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
		{
			FString FuncPath;
			FString AliasUsed;
			bool bKeyPresentButEmpty = false;
			if (ResolveMaterialFunctionCallPath(Obj, FuncPath, AliasUsed, bKeyPresentButEmpty))
			{
				UMaterialFunction* CalledFunc = LoadObject<UMaterialFunction>(nullptr, *FuncPath);
				if (!CalledFunc && !FuncPath.StartsWith(TEXT("/")))
					CalledFunc = LoadObject<UMaterialFunction>(nullptr, *(TEXT("/Game/") + FuncPath));
				if (CalledFunc)
				{
					FuncCall->SetMaterialFunction(CalledFunc);
					FuncCall->UpdateFromFunctionResource();

					if (!AliasUsed.IsEmpty())
					{
						const FString WMsg = FString::Printf(
							TEXT("Normalized MaterialFunctionCall key '%s' to 'materialFunction' for ref '%s'"),
							*AliasUsed, *Ref);
						Warnings.Add(WMsg);
						WarningDiagnostics.Add(MakeDiagnostic(TEXT("ALIAS_NORMALIZED"), WMsg, TEXT("addExpressions"), Ref, TEXT("materialFunction"), AliasUsed));
					}
				}
				else
				{
					const FString Msg = FString::Printf(TEXT("MaterialFunction asset not found: %s - call node created but unbound"), *FuncPath);
					Warnings.Add(Msg);
					ErrorDiagnostics.Add(MakeDiagnostic(TEXT("ASSET_NOT_FOUND"), Msg, TEXT("addExpressions"), Ref, TEXT("materialFunction"), FuncPath));
				}
			}
			else if (bKeyPresentButEmpty)
			{
				const FString FieldName = AliasUsed.IsEmpty() ? FString(TEXT("materialFunction")) : AliasUsed;
				const FString Msg = FString::Printf(
					TEXT("MaterialFunctionCall key '%s' provided but value is empty for ref '%s' - call node created but unbound"),
					*FieldName, *Ref);
				Errors.Add(Msg);
				ErrorDiagnostics.Add(MakeDiagnostic(TEXT("EMPTY_MATERIAL_FUNCTION_PATH"), Msg, TEXT("addExpressions"), Ref, FieldName, TEXT("")));
			}
			// else: no recognized key found, node created unbound (existing behavior).
			// The AI didn't try to bind, so no diagnostic is owed.
		}

		// Apply top-level fields as properties (e.g. exponent, r, g, b, value sent directly on the expression object)
		SetExpressionProperties(Expr, Obj, &Warnings, &ErrorDiagnostics);

		// Also apply nested "properties" sub-object if present (overrides top-level)
		const TSharedPtr<FJsonObject>* PropsObj;
		if (Obj->TryGetObjectField(TEXT("properties"), PropsObj))
		{
			SetExpressionProperties(Expr, *PropsObj, &Warnings, &ErrorDiagnostics);
		}

		// Store ref by user-provided name
		if (!Ref.IsEmpty())
		{
			ExpressionRefs.Add(Ref, Expr);
		}

		// Also store by UE5 auto-generated name so inspect + reconnect works
		FString UEName = Expr->GetName();
		if (!UEName.IsEmpty() && UEName != Ref)
		{
			ExpressionRefs.Add(UEName, Expr);
		}

		// Record created node so the caller can connect without an inspect round-trip
		TSharedRef<FJsonObject> Record = MakeShareable(new FJsonObject());
		Record->SetStringField(TEXT("userRef"), Ref);
		Record->SetStringField(TEXT("assignedRef"), UEName);
		Record->SetStringField(TEXT("type"), ClassName);
		CreatedRecords.Add(MakeShareable(new FJsonValueObject(Record)));

		Added++;
	}

	FString Msg = FString::Printf(TEXT("Added %d expression(s)"), Added);
	if (Warnings.Num() > 0) Msg += TEXT(". Warnings: ") + FString::Join(Warnings, TEXT("; "));
	if (Errors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));
	FNwiroIKMatResult Result = Added > 0 && Errors.Num() == 0 && ErrorDiagnostics.Num() == 0 ? FNwiroIKMatResult::Ok(Msg) : FNwiroIKMatResult::Fail(Msg);
	Result.Errors = ErrorDiagnostics;
	Result.Warnings = WarningDiagnostics;
	Result.Created = CreatedRecords;
	return Result;
}

// ============================================================
// CONNECT EXPRESSIONS
// ============================================================

FNwiroIKMatResult FNwiroIKMaterialTools::DoConnectExpressions(UMaterial* Mat, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	int32 Connected = 0;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> ErrorDiagnostics;
	TArray<TSharedPtr<FJsonValue>> WarningDiagnostics;

	for (int32 i = 0; i < Items.Num(); i++)
	{
		const TSharedPtr<FJsonObject>& Obj = Items[i]->AsObject();
		if (!Obj.IsValid()) continue;

		FString FromStr = Obj->GetStringField(TEXT("from"));
		FString ToStr = Obj->GetStringField(TEXT("to"));


		// Parse "ref.OutputName" - if no dot, use default output (index 0)
		int32 FromDot, ToDot;
		FString FromRef, FromOutput, ToRef, ToInput;
		if (FromStr.FindChar('.', FromDot))
		{
			FromRef = FromStr.Left(FromDot);
			FromOutput = FromStr.Mid(FromDot + 1);
		}
		else
		{
			FromRef = FromStr;
			FromOutput = TEXT(""); // default first output
		}
		if (ToStr.FindChar('.', ToDot))
		{
			ToRef = ToStr.Left(ToDot);
			ToInput = ToStr.Mid(ToDot + 1);
		}
		else
		{
			ToRef = ToStr;
			ToInput = TEXT(""); // default first input
		}

		// Find source expression - try ExpressionRefs first, then fall back to
		// searching the material's expressions by UE5 GetName() so that names
		// returned by inspect_material_graph (e.g. "MaterialExpressionScalarParameter_0") work.
		// Then also try matching by ParameterName (for *Parameter expressions)
		// and Desc field (where add_expressions stashes the user-given ref so
		// later calls can find them after ExpressionRefs got cleared).
		TWeakObjectPtr<UMaterialExpression>* FromExprPtr = ExpressionRefs.Find(FromRef);
		if (!FromExprPtr || !FromExprPtr->IsValid())
		{
			for (const TObjectPtr<UMaterialExpression>& E : Mat->GetExpressionCollection().Expressions)
			{
				if (!E) continue;
				const bool ByName = E->GetName().Equals(FromRef, ESearchCase::IgnoreCase);
				const bool ByDesc = !E->Desc.IsEmpty() && E->Desc.Equals(FromRef, ESearchCase::IgnoreCase);
				bool ByParam = false;
				if (auto* SP = Cast<UMaterialExpressionScalarParameter>(E.Get()))     ByParam = SP->ParameterName == FName(*FromRef);
				else if (auto* VP = Cast<UMaterialExpressionVectorParameter>(E.Get())) ByParam = VP->ParameterName == FName(*FromRef);
				else if (auto* TP = Cast<UMaterialExpressionTextureSampleParameter2D>(E.Get())) ByParam = TP->ParameterName == FName(*FromRef);
				if (ByName || ByDesc || ByParam)
				{
					ExpressionRefs.Add(FromRef, E);
					FromExprPtr = ExpressionRefs.Find(FromRef);
					break;
				}
			}
		}
		if (!FromExprPtr || !FromExprPtr->IsValid())
		{
			const FString Msg = FString::Printf(TEXT("Source expression not found: %s"), *FromRef);
			Errors.Add(Msg);
			ErrorDiagnostics.Add(MakeDiagnostic(TEXT("SOURCE_EXPRESSION_NOT_FOUND"), Msg, TEXT("connectExpressions"), FromRef, TEXT("from"), FromStr));
			continue;
		}
		UMaterialExpression* FromExpr = FromExprPtr->Get();

		// Check if target is a Material property (e.g., "Material.BaseColor")
		if (ToRef.Equals(TEXT("Material"), ESearchCase::IgnoreCase))
		{
			int32 MatProp;
			if (ResolveMaterialProperty(ToInput, MatProp))
			{
				bool bOk = UMaterialEditingLibrary::ConnectMaterialProperty(FromExpr, FromOutput, (EMaterialProperty)MatProp);
				if (bOk) { Connected++; }
				else
				{
					const TArray<FString> SourceOutputs = GetExpressionOutputNames(FromExpr);
					const FString Msg = FString::Printf(TEXT("Failed to connect %s -> Material.%s. Available source outputs on %s: [%s]"),
						*FromStr, *ToInput, *FromRef, *FString::Join(SourceOutputs, TEXT(", ")));
					Errors.Add(Msg);
					ErrorDiagnostics.Add(MakeDiagnostic(TEXT("CONNECT_FAILED"), Msg, TEXT("connectExpressions"), ToRef, TEXT("to"), ToStr, SourceOutputs));
				}
			}
			else
			{
				TArray<FString> AllowedProps = {
					TEXT("BaseColor"), TEXT("Metallic"), TEXT("Specular"), TEXT("Roughness"), TEXT("Anisotropy"),
					TEXT("EmissiveColor"), TEXT("Emissive"), TEXT("Opacity"), TEXT("OpacityMask"), TEXT("Normal"),
					TEXT("Tangent"), TEXT("WorldPositionOffset"), TEXT("SubsurfaceColor"), TEXT("AmbientOcclusion")
				};
				const FString Msg = FString::Printf(TEXT("Unknown material property: %s. Allowed material inputs: [%s]"), *ToInput, *FString::Join(AllowedProps, TEXT(", ")));
				Errors.Add(Msg);
				ErrorDiagnostics.Add(MakeDiagnostic(TEXT("UNKNOWN_MATERIAL_PROPERTY"), Msg, TEXT("connectExpressions"), ToRef, TEXT("to"), ToStr, AllowedProps));
			}
			continue;
		}

		// Target is another expression - same fallback logic as source
		TWeakObjectPtr<UMaterialExpression>* ToExprPtr = ExpressionRefs.Find(ToRef);
		if (!ToExprPtr || !ToExprPtr->IsValid())
		{
			for (const TObjectPtr<UMaterialExpression>& E : Mat->GetExpressionCollection().Expressions)
			{
				if (!E) continue;
				const bool ByName = E->GetName().Equals(ToRef, ESearchCase::IgnoreCase);
				const bool ByDesc = !E->Desc.IsEmpty() && E->Desc.Equals(ToRef, ESearchCase::IgnoreCase);
				bool ByParam = false;
				if (auto* SP = Cast<UMaterialExpressionScalarParameter>(E.Get()))     ByParam = SP->ParameterName == FName(*ToRef);
				else if (auto* VP = Cast<UMaterialExpressionVectorParameter>(E.Get())) ByParam = VP->ParameterName == FName(*ToRef);
				else if (auto* TP = Cast<UMaterialExpressionTextureSampleParameter2D>(E.Get())) ByParam = TP->ParameterName == FName(*ToRef);
				if (ByName || ByDesc || ByParam)
				{
					ExpressionRefs.Add(ToRef, E);
					ToExprPtr = ExpressionRefs.Find(ToRef);
					break;
				}
			}
		}
		if (!ToExprPtr || !ToExprPtr->IsValid())
		{
			const FString Msg = FString::Printf(TEXT("Target expression not found: %s"), *ToRef);
			Errors.Add(Msg);
			ErrorDiagnostics.Add(MakeDiagnostic(TEXT("TARGET_EXPRESSION_NOT_FOUND"), Msg, TEXT("connectExpressions"), ToRef, TEXT("to"), ToStr));
			continue;
		}

		bool bOk = UMaterialEditingLibrary::ConnectMaterialExpressions(FromExpr, FromOutput, ToExprPtr->Get(), ToInput);
		if (!bOk && IsUnnamedSingleOutputRequest(FromExpr, FromOutput))
		{
			// Bug #9: UE5 ConnectMaterialExpressions can fail to match unnamed single outputs
			// (TextureCoordinate, ComponentMask, OneMinus, Power, SquareRoot, DotProduct, ...).
			// Fall back to direct FExpressionInput assignment by output index 0 ONLY when the
			// source has exactly one output and the caller didn't ask for a specific named pin.
			if (TryConnectByOutputIndex(FromExpr, 0, ToExprPtr->Get(), ToInput))
			{
				bOk = true;
				const FString WMsg = FString::Printf(TEXT("Connected %s -> %s via index 0 (source has a single unnamed output; pass an empty output name to use the canonical form)"), *FromStr, *ToStr);
				WarningDiagnostics.Add(MakeDiagnostic(TEXT("UNNAMED_OUTPUT_NORMALIZED"), WMsg, TEXT("connectExpressions"), FromRef, TEXT("from"), FromStr));
			}
		}
		if (bOk) { Connected++; }
		else
		{
			const TArray<FString> SourceOutputs = GetExpressionOutputNames(FromExpr);
			const TArray<FString> TargetInputs = GetExpressionInputNames(ToExprPtr->Get());
			const FString Msg = FString::Printf(TEXT("Failed to connect %s -> %s. Available source outputs on %s: [%s]. Available target inputs on %s: [%s]"),
				*FromStr, *ToStr, *FromRef, *FString::Join(SourceOutputs, TEXT(", ")), *ToRef, *FString::Join(TargetInputs, TEXT(", ")));
			Errors.Add(Msg);
			TArray<FString> AllowedPins = TargetInputs;
			AllowedPins.Append(SourceOutputs);
			ErrorDiagnostics.Add(MakeDiagnostic(TEXT("CONNECT_FAILED"), Msg, TEXT("connectExpressions"), ToRef, TEXT("to"), ToStr, AllowedPins));
		}
	}

	FString Msg = FString::Printf(TEXT("Connected %d/%d expression(s)"), Connected, Items.Num());
	if (Errors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(Errors, TEXT("; "));
	FNwiroIKMatResult Result = Connected > 0 && Errors.Num() == 0 ? FNwiroIKMatResult::Ok(Msg) : FNwiroIKMatResult::Fail(Msg);
	Result.Errors = ErrorDiagnostics;
	Result.Warnings = WarningDiagnostics;
	return Result;
}

// ============================================================
// REMOVE EXPRESSIONS
// ============================================================

FNwiroIKMatResult FNwiroIKMaterialTools::DoRemoveExpressions(UMaterial* Mat, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	int32 Removed = 0;
	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		FString Ref = Item->AsString();
		if (Ref.IsEmpty()) continue;

		TWeakObjectPtr<UMaterialExpression>* ExprPtr = ExpressionRefs.Find(Ref);
		if (ExprPtr && ExprPtr->IsValid())
		{
			UMaterialEditingLibrary::DeleteMaterialExpression(Mat, ExprPtr->Get());
			ExpressionRefs.Remove(Ref);
			Removed++;
		}
	}
	return FNwiroIKMatResult::Ok(FString::Printf(TEXT("Removed %d expression(s)"), Removed));
}

// ============================================================
// INSPECT MATERIAL GRAPH
// ============================================================

FString FNwiroIKMaterialTools::InspectMaterialGraph(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	FString MatPath;
	if (!Cmd->TryGetStringField(TEXT("path"), MatPath))
		Cmd->TryGetStringField(TEXT("material"), MatPath);

	UMaterial* Mat = LoadMasterMaterial(MatPath);
	if (!Mat)
	{
		return FString::Printf(TEXT("{\"success\": false, \"error\": \"Material not found: %s\"}"), *MatPath);
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Mat->GetName());
	Result->SetStringField(TEXT("path"), Mat->GetPathName());

	// Properties
	TSharedRef<FJsonObject> PropsObj = MakeShareable(new FJsonObject());
	PropsObj->SetStringField(TEXT("blend_mode"),
		Mat->BlendMode == BLEND_Opaque ? TEXT("Opaque") :
		Mat->BlendMode == BLEND_Masked ? TEXT("Masked") :
		Mat->BlendMode == BLEND_Translucent ? TEXT("Translucent") :
		Mat->BlendMode == BLEND_Additive ? TEXT("Additive") : TEXT("Other"));
	PropsObj->SetBoolField(TEXT("two_sided"), Mat->TwoSided);
	Result->SetObjectField(TEXT("properties"), PropsObj);

	// Expressions - also register each one in ExpressionRefs so subsequent
	// edit_material / connect_expressions calls can reference them by name.
	TArray<TSharedPtr<FJsonValue>> ExprArr;
	for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressionCollection().Expressions)
	{
		if (!Expr) continue;
		TSharedPtr<FJsonObject> ExprObj = SerializeExpression(Expr);
		if (ExprObj.IsValid())
		{
			ExprArr.Add(MakeShareable(new FJsonValueObject(ExprObj.ToSharedRef())));
		}

		// Register by UE5 auto-generated name (e.g. "MaterialExpressionScalarParameter_0")
		FString UEName = Expr->GetName();
		if (!UEName.IsEmpty())
		{
			ExpressionRefs.Add(UEName, Expr);
		}

		// Also register a clean ref: class short name + index derived from UE name
		// e.g. "ScalarParameter_0" from "MaterialExpressionScalarParameter_0"
		FString CleanRef = UEName;
		if (CleanRef.StartsWith(TEXT("MaterialExpression")))
		{
			CleanRef = CleanRef.Mid(18); // len("MaterialExpression") == 18
		}
		if (!CleanRef.IsEmpty() && CleanRef != UEName)
		{
			ExpressionRefs.Add(CleanRef, Expr);
		}
	}
	Result->SetArrayField(TEXT("expressions"), ExprArr);
	Result->SetNumberField(TEXT("expressionCount"), ExprArr.Num());

	// Connections — walk each material-attribute input pin and record what's wired in.
	// Without this, agents see expressions but no graph topology and can't tell what
	// the material actually does.
	TArray<TSharedPtr<FJsonValue>> ConnArr;
	auto EmitInput = [&](const TCHAR* Slot, const FExpressionInput& Input)
	{
		if (!Input.Expression) return;
		TSharedRef<FJsonObject> C = MakeShareable(new FJsonObject());
		C->SetStringField(TEXT("from"), Input.Expression->GetName());
		C->SetStringField(TEXT("fromOutput"), Input.OutputIndex >= 0 ? FString::FromInt(Input.OutputIndex) : TEXT("0"));
		C->SetStringField(TEXT("to"), FString::Printf(TEXT("Material.%s"), Slot));
		ConnArr.Add(MakeShareable(new FJsonValueObject(C)));
	};
	UMaterialEditorOnlyData* EditorOnly = Mat->GetEditorOnlyData();
	if (EditorOnly)
	{
		EmitInput(TEXT("BaseColor"), EditorOnly->BaseColor);
		EmitInput(TEXT("Metallic"), EditorOnly->Metallic);
		EmitInput(TEXT("Specular"), EditorOnly->Specular);
		EmitInput(TEXT("Roughness"), EditorOnly->Roughness);
		EmitInput(TEXT("EmissiveColor"), EditorOnly->EmissiveColor);
		EmitInput(TEXT("Opacity"), EditorOnly->Opacity);
		EmitInput(TEXT("OpacityMask"), EditorOnly->OpacityMask);
		EmitInput(TEXT("Normal"), EditorOnly->Normal);
		EmitInput(TEXT("WorldPositionOffset"), EditorOnly->WorldPositionOffset);
		EmitInput(TEXT("Refraction"), EditorOnly->Refraction);
		EmitInput(TEXT("AmbientOcclusion"), EditorOnly->AmbientOcclusion);
		EmitInput(TEXT("PixelDepthOffset"), EditorOnly->PixelDepthOffset);
	}
	// Also walk inter-expression connections (e.g. Multiply.A pin connected to GlowCol).
	for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressionCollection().Expressions)
	{
		if (!Expr) continue;
		for (FExpressionInputIterator It{ Expr }; It; ++It)
		{
			if (It->Expression && It->Expression != Expr)
			{
				TSharedRef<FJsonObject> C = MakeShareable(new FJsonObject());
				C->SetStringField(TEXT("from"), It->Expression->GetName());
				const FName PinName = Expr->GetInputName(It.Index);
				C->SetStringField(TEXT("to"), FString::Printf(TEXT("%s.%s"),
					*Expr->GetName(), *PinName.ToString()));
				ConnArr.Add(MakeShareable(new FJsonValueObject(C)));
			}
		}
	}
	Result->SetArrayField(TEXT("connections"), ConnArr);
	Result->SetNumberField(TEXT("connectionCount"), ConnArr.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// SERIALIZE EXPRESSION
// ============================================================

TSharedPtr<FJsonObject> FNwiroIKMaterialTools::SerializeExpression(UMaterialExpression* Expr)
{
	if (!Expr) return nullptr;

	TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
	Obj->SetStringField(TEXT("class"), Expr->GetClass()->GetName());
	Obj->SetStringField(TEXT("name"), Expr->GetName());
	Obj->SetNumberField(TEXT("x"), Expr->MaterialExpressionEditorX);
	Obj->SetNumberField(TEXT("y"), Expr->MaterialExpressionEditorY);
	Obj->SetStringField(TEXT("description"), Expr->Desc);

	// Outputs
	TArray<TSharedPtr<FJsonValue>> OutputArr;
	TArray<FExpressionOutput>& Outputs = Expr->GetOutputs();
	for (int32 i = 0; i < Outputs.Num(); i++)
	{
		TSharedRef<FJsonObject> OutObj = MakeShareable(new FJsonObject());
		// Canonical unnamed-pin form is the numeric index string (matches GetExpressionOutputNames
		// and the connect path). "Output" remains accepted as an alias on the connect side.
		OutObj->SetStringField(TEXT("name"), Outputs[i].OutputName.IsNone() ? FString::FromInt(i) : Outputs[i].OutputName.ToString());
		OutObj->SetNumberField(TEXT("index"), i);
		OutputArr.Add(MakeShareable(new FJsonValueObject(OutObj)));
	}
	Obj->SetArrayField(TEXT("outputs"), OutputArr);

	// Inputs (iterate via GetInput until nullptr)
	TArray<TSharedPtr<FJsonValue>> InputArr;
	for (int32 i = 0; ; i++)
	{
		const FExpressionInput* Input = Expr->GetInput(i);
		if (!Input) break;

		TSharedRef<FJsonObject> InObj = MakeShareable(new FJsonObject());
		InObj->SetStringField(TEXT("name"), Expr->GetInputName(i).ToString());
		InObj->SetBoolField(TEXT("connected"), Input->Expression != nullptr);
		InputArr.Add(MakeShareable(new FJsonValueObject(InObj)));
	}
	Obj->SetArrayField(TEXT("inputs"), InputArr);

	// Type-specific properties
	if (UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
	{
		Obj->SetStringField(TEXT("parameterName"), SP->ParameterName.ToString());
		Obj->SetNumberField(TEXT("defaultValue"), SP->DefaultValue);
	}
	else if (UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
	{
		Obj->SetStringField(TEXT("parameterName"), VP->ParameterName.ToString());
	}
	else if (UMaterialExpressionTextureSample* TS = Cast<UMaterialExpressionTextureSample>(Expr))
	{
		if (TS->Texture)
		{
			Obj->SetStringField(TEXT("texture"), TS->Texture->GetPathName());
		}
	}
	else if (UMaterialExpressionMaterialFunctionCall* FuncCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
	{
		if (FuncCall->MaterialFunction)
		{
			Obj->SetStringField(TEXT("materialFunction"), FuncCall->MaterialFunction->GetPathName());
			Obj->SetStringField(TEXT("boundFunction"), FuncCall->MaterialFunction->GetPathName());
		}
		else
		{
			Obj->SetBoolField(TEXT("boundFunctionMissing"), true);
		}
	}
	else if (UMaterialExpressionConstant* C = Cast<UMaterialExpressionConstant>(Expr))
	{
		Obj->SetNumberField(TEXT("value"), C->R);
	}

	return Obj;
}

// ============================================================
// DELETE MATERIAL
// ============================================================

FString FNwiroIKMaterialTools::DeleteMaterial(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	FString Path = Cmd->GetStringField(TEXT("path"));
	if (Path.IsEmpty()) return TEXT("{\"success\": false, \"error\": \"Missing 'path'\"}");

	bool bDeleted = UEditorAssetLibrary::DeleteAsset(Path);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), bDeleted);
	Result->SetStringField(TEXT("path"), Path);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// FIND TEXTURES
// ============================================================

FString FNwiroIKMaterialTools::FindTextures(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");
	}

	FString SearchTerm = Cmd->GetStringField(TEXT("searchTerm")).ToLower();
	int32 MaxResults = Cmd->HasField(TEXT("max_results")) ? (int32)Cmd->GetNumberField(TEXT("max_results")) : 50;

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UTexture2D::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : Assets)
	{
		if (Results.Num() >= MaxResults) break;

		FString Name = Asset.AssetName.ToString();
		if (!SearchTerm.IsEmpty() && !Name.ToLower().Contains(SearchTerm)) continue;

		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("path"), FString::Printf(TEXT("%s.%s"), *Asset.PackageName.ToString(), *Name));
		Results.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetBoolField(TEXT("success"), true);
	Root->SetArrayField(TEXT("textures"), Results);
	Root->SetNumberField(TEXT("count"), Results.Num());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

// ============================================================
// SET MATERIAL PROPERTY (standalone)
// ============================================================

FString FNwiroIKMaterialTools::SetMaterialProperty(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString MatPath;
	if (!Cmd->TryGetStringField(TEXT("material"), MatPath))
		if (!Cmd->TryGetStringField(TEXT("assetPath"), MatPath))
			Cmd->TryGetStringField(TEXT("path"), MatPath);

	UMaterial* Mat = LoadMasterMaterial(MatPath);
	if (!Mat) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Material not found: %s\"}"), *MatPath);

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "SetMaterialProperty", "AI: Set Material Property"), Mat);

	SetMaterialProperties(Mat, Cmd);

	// PreEditChange/PostEditChange retained for property change notification (orthogonal to undo registration via Modify()).
	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	Mat->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"material\":\"%s\"}"), *Mat->GetPathName());
}

// ============================================================
// EDIT MATERIAL INSTANCE (standalone)
// ============================================================

FString FNwiroIKMaterialTools::EditMaterialInstance(const FString& JsonCommand)
{
	return UpdateMaterialInstance(JsonCommand);
}

// ============================================================
// DELETE EXPRESSION (standalone)
// ============================================================

FString FNwiroIKMaterialTools::DeleteExpression(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString MatPath;
	if (!Cmd->TryGetStringField(TEXT("material"), MatPath))
		Cmd->TryGetStringField(TEXT("assetPath"), MatPath);

	UMaterial* Mat = LoadMasterMaterial(MatPath);
	if (!Mat) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Material not found: %s\"}"), *MatPath);

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "DeleteExpression", "AI: Delete Expression"), Mat);

	TArray<TSharedPtr<FJsonValue>> Items;
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Cmd->TryGetArrayField(TEXT("refs"), Arr))
	{
		Items = *Arr;
	}
	else if (Cmd->HasField(TEXT("ref")))
	{
		Items.Add(MakeShareable(new FJsonValueString(Cmd->GetStringField(TEXT("ref")))));
	}

	FNwiroIKMatResult R = DoRemoveExpressions(Mat, Items);

	if (R.bSuccess)
	{
		// PreEditChange/PostEditChange retained for property change notification (orthogonal to undo registration via Modify()).
		Mat->PreEditChange(nullptr);
		Mat->PostEditChange();
		Mat->MarkPackageDirty();
	}
	else
	{
		Tx.Cancel();
	}

	return FString::Printf(TEXT("{\"success\":%s,\"message\":\"%s\"}"), R.bSuccess ? TEXT("true") : TEXT("false"), *R.Message);
}

// ============================================================
// LOAD MATERIAL FUNCTION
// ============================================================

UMaterialFunction* FNwiroIKMaterialTools::LoadMaterialFunction(const FString& PathOrName)
{
	if (PathOrName.Len() > 1023) return nullptr;
	UMaterialFunction* Func = LoadObject<UMaterialFunction>(nullptr, *PathOrName);
	if (Func) return Func;

	if (!PathOrName.StartsWith(TEXT("/")))
	{
		Func = LoadObject<UMaterialFunction>(nullptr, *(TEXT("/Game/") + PathOrName));
		if (Func) return Func;
	}

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();
	AR.ScanPathsSynchronous({TEXT("/Game")}, true);

	FARFilter Filter;
	Filter.ClassPaths.Add(UMaterialFunction::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	FString CleanName = PathOrName;
	if (CleanName.StartsWith(TEXT("MF_"))) CleanName = CleanName.Mid(3);

	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetName.ToString().Equals(PathOrName, ESearchCase::IgnoreCase))
			return Cast<UMaterialFunction>(Asset.GetAsset());
	}
	for (const FAssetData& Asset : Assets)
	{
		FString AssetClean = Asset.AssetName.ToString();
		if (AssetClean.StartsWith(TEXT("MF_"))) AssetClean = AssetClean.Mid(3);
		if (AssetClean.Equals(CleanName, ESearchCase::IgnoreCase))
			return Cast<UMaterialFunction>(Asset.GetAsset());
	}

	return nullptr;
}

// ============================================================
// FIND MATERIAL FUNCTIONS (Policy 1.1)
// ============================================================
// Read-only search. Strict input schema (`_callId`, `searchTerm`, `searchRoot`,
// `maxResults`). Per-row `assetPath` is canonical full-object form.
// When truncated, emits `truncated:true`, `returnedCount`, `totalAllowed`, and
// `discoveryTool` per rule 10.
FString FNwiroIKMaterialTools::FindMaterialFunctions(const FString& JsonCommand)
{
	// Phase 1: parse + validate
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("$"), TEXT("invalid-json"), { TEXT("object") }));
		return MakePolicyFailureResponse(TEXT(""), TEXT("Invalid JSON."), Errors);
	}

	const FString CallId = MakeCallId(Cmd);

	static const TSet<FString> AllowedTopLevel = {
		TEXT("_callId"), TEXT("searchTerm"), TEXT("searchRoot"), TEXT("maxResults")
	};
	TArray<FString> AllowedList = AllowedTopLevel.Array();
	AllowedList.Sort();

	TArray<TSharedPtr<FJsonValue>> ValidationErrors;
	for (const auto& Pair : Cmd->Values)
	{
		const FString Key(*Pair.Key);
		if (!AllowedTopLevel.Contains(Key))
		{
			ValidationErrors.Add(MakePolicyDiagnostic(TEXT("UNKNOWN_KEY"), TEXT("schema"), Key, Key, AllowedList));
		}
	}

	if (Cmd->HasField(TEXT("_callId")) && !JsonFieldIsType(Cmd, TEXT("_callId"), EJson::String))
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("_callId"), TEXT("non-string"), { TEXT("string") }));
	if (Cmd->HasField(TEXT("searchTerm")) && !JsonFieldIsType(Cmd, TEXT("searchTerm"), EJson::String))
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("searchTerm"), TEXT("non-string"), { TEXT("string") }));
	if (Cmd->HasField(TEXT("searchRoot")) && !JsonFieldIsType(Cmd, TEXT("searchRoot"), EJson::String))
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("searchRoot"), TEXT("non-string"), { TEXT("string") }));
	if (Cmd->HasField(TEXT("maxResults")) && !JsonFieldIsType(Cmd, TEXT("maxResults"), EJson::Number))
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("maxResults"), TEXT("non-number"), { TEXT("number") }));

	if (ValidationErrors.Num() > 0)
	{
		return MakePolicyFailureResponse(CallId, TEXT("Schema validation failed."), ValidationErrors);
	}

	// Phase 2: resolve parameters with defaults
	FString SearchTerm;
	Cmd->TryGetStringField(TEXT("searchTerm"), SearchTerm);
	SearchTerm = SearchTerm.ToLower();

	FString SearchRoot;
	Cmd->TryGetStringField(TEXT("searchRoot"), SearchRoot);
	if (SearchRoot.IsEmpty()) SearchRoot = TEXT("/Game");

	int32 MaxResults = 50;
	if (Cmd->HasField(TEXT("maxResults")))
	{
		double Raw = 0;
		Cmd->TryGetNumberField(TEXT("maxResults"), Raw);
		MaxResults = FMath::Clamp((int32)Raw, 1, 200);
	}

	// Phase 3: query asset registry
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UMaterialFunction::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(*SearchRoot));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	// Filter by searchTerm to compute true totalAllowed before truncation.
	TArray<FAssetData> Matches;
	Matches.Reserve(Assets.Num());
	for (const FAssetData& Asset : Assets)
	{
		FString Name = Asset.AssetName.ToString();
		if (!SearchTerm.IsEmpty() && !Name.ToLower().Contains(SearchTerm)) continue;
		Matches.Add(Asset);
	}

	const int32 TotalAllowed = Matches.Num();
	const int32 ReturnedCount = FMath::Min(TotalAllowed, MaxResults);
	const bool bTruncated = (ReturnedCount < TotalAllowed);

	// Phase 4: build per-row objects
	TArray<TSharedPtr<FJsonValue>> Functions;
	Functions.Reserve(ReturnedCount);
	for (int32 i = 0; i < ReturnedCount; ++i)
	{
		const FAssetData& Asset = Matches[i];
		const FString Name = Asset.AssetName.ToString();
		const FString AssetPath = FString::Printf(TEXT("%s.%s"), *Asset.PackageName.ToString(), *Name);

		TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("assetPath"), AssetPath);  // canonical (rule 16B)
		Obj->SetStringField(TEXT("name"), Name);

		if (UMaterialFunction* Func = Cast<UMaterialFunction>(Asset.GetAsset()))
		{
			if (!Func->Description.IsEmpty())
				Obj->SetStringField(TEXT("description"), Func->Description);
			Obj->SetBoolField(TEXT("exposedToLibrary"), Func->bExposeToLibrary);
			if (Func->LibraryCategoriesText.Num() > 0)
			{
				TArray<FString> CatParts;
				for (const FText& Cat : Func->LibraryCategoriesText)
					CatParts.Add(Cat.ToString());
				Obj->SetStringField(TEXT("category"), FString::Join(CatParts, TEXT("/")));
			}
		}

		Functions.Add(MakeShareable(new FJsonValueObject(Obj)));
	}

	// Phase 5: build policy envelope
	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("policy_version"), TEXT("1.1"));
	Result->SetStringField(TEXT("_callId"), CallId);
	Result->SetArrayField(TEXT("functions"), Functions);
	Result->SetNumberField(TEXT("count"), ReturnedCount);
	if (bTruncated)
	{
		// Rule 10: discoveryTool is REQUIRED whenever truncated:true.
		Result->SetBoolField(TEXT("truncated"), true);
		Result->SetNumberField(TEXT("returnedCount"), ReturnedCount);
		Result->SetNumberField(TEXT("totalAllowed"), TotalAllowed);
		Result->SetStringField(TEXT("discoveryTool"), TEXT("find_material_functions"));
	}
	else
	{
		Result->SetBoolField(TEXT("truncated"), false);
	}
	Result->SetArrayField(TEXT("messages"), MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("created"),  MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("modified"), MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("skipped"),  MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("warnings"), MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("errors"),   MakeEmptyJsonArray());

	return SerializeJsonObject(Result);
}

// ============================================================
// CREATE MATERIAL FUNCTION (Policy 1.1)
// ============================================================
// Canonical happy path: send `assetPath` ("/Game/Foo/MF_Bar"). The `name + path`
// pair is a migration alias and emits an ALIAS_NORMALIZED warning when used. If
// both forms are present and they disagree on the resolved full path, the call
// fails with INVALID_VALUE before any mutation (rule 14).
//
// Path normalization: accepts both "/Game/Foo/MF_Bar" and "/Game/Foo/MF_Bar.MF_Bar";
// the response always echoes the canonical full-object form. MF_ prefix is
// auto-applied if missing with an ALIAS_NORMALIZED warning so the AI learns
// the canonical name.
FString FNwiroIKMaterialTools::CreateMaterialFunction(const FString& JsonCommand)
{
	// Phase 1: parse + top-level validation
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("$"), TEXT("invalid-json"), { TEXT("object") }));
		return MakePolicyFailureResponse(TEXT(""), TEXT("Invalid JSON."), Errors);
	}

	const FString CallId = MakeCallId(Cmd);

	static const TSet<FString> AllowedTopLevel = {
		TEXT("_callId"), TEXT("assetPath"), TEXT("name"), TEXT("path"),
		TEXT("description"), TEXT("category"), TEXT("exposeToLibrary")
	};
	TArray<FString> AllowedList = AllowedTopLevel.Array();
	AllowedList.Sort();

	TArray<TSharedPtr<FJsonValue>> ValidationErrors;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	for (const auto& Pair : Cmd->Values)
	{
		const FString Key(*Pair.Key);
		if (!AllowedTopLevel.Contains(Key))
		{
			ValidationErrors.Add(MakePolicyDiagnostic(TEXT("UNKNOWN_KEY"), TEXT("schema"), Key, Key, AllowedList));
		}
	}

	// Type checks
	auto CheckStringType = [&](const TCHAR* Key)
	{
		if (Cmd->HasField(Key) && !JsonFieldIsType(Cmd, Key, EJson::String))
			ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), Key, TEXT("non-string"), { TEXT("string") }));
	};
	CheckStringType(TEXT("_callId"));
	CheckStringType(TEXT("assetPath"));
	CheckStringType(TEXT("name"));
	CheckStringType(TEXT("path"));
	CheckStringType(TEXT("description"));
	CheckStringType(TEXT("category"));
	if (Cmd->HasField(TEXT("exposeToLibrary")) && !JsonFieldIsType(Cmd, TEXT("exposeToLibrary"), EJson::Boolean))
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("exposeToLibrary"), TEXT("non-boolean"), { TEXT("boolean") }));

	if (ValidationErrors.Num() > 0)
	{
		return MakePolicyFailureResponse(CallId, TEXT("Schema validation failed."), ValidationErrors);
	}

	// Phase 2: resolve canonical assetPath from inputs.
	// "/Game/Foo/MF_Bar" → dir "/Game/Foo", name "MF_Bar"
	// "/Game/Foo/MF_Bar.MF_Bar" → dir "/Game/Foo", name "MF_Bar"
	// "/Game/Foo/MF_A.MF_B" → false (mismatched object suffix is invalid)
	auto SplitAssetPath = [](const FString& Raw, FString& OutDir, FString& OutName) -> bool
	{
		FString Package = Raw;
		FString SuffixAfterDot;
		int32 DotIdx;
		if (Package.FindChar('.', DotIdx))
		{
			SuffixAfterDot = Package.Mid(DotIdx + 1);
			Package = Package.Left(DotIdx);
		}
		int32 SlashIdx;
		if (!Package.FindLastChar('/', SlashIdx)) return false;
		OutDir  = Package.Left(SlashIdx);
		OutName = Package.Mid(SlashIdx + 1);
		if (OutDir.IsEmpty() || OutName.IsEmpty()) return false;
		// UE convention: the object-name suffix after the dot must equal the basename
		// before the dot. "/Game/Foo/MF_A.MF_B" is invalid — the AI likely typoed it.
		if (!SuffixAfterDot.IsEmpty() && !SuffixAfterDot.Equals(OutName, ESearchCase::CaseSensitive))
			return false;
		return true;
	};
	auto BuildCanonical = [](const FString& Dir, const FString& Name) -> FString
	{
		return FString::Printf(TEXT("%s/%s.%s"), *Dir, *Name, *Name);
	};

	const bool bHasAssetPath = Cmd->HasField(TEXT("assetPath"));
	const bool bHasName      = Cmd->HasField(TEXT("name"));
	const bool bHasPath      = Cmd->HasField(TEXT("path"));

	FString ResolvedDir;
	FString ResolvedName;
	FString CanonicalAssetPath;

	if (!bHasAssetPath && !bHasName)
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakePolicyDiagnostic(
			TEXT("MISSING_REQUIRED_FIELD"),
			TEXT("schema"),
			TEXT("assetPath"),
			TEXT("missing"),
			{ TEXT("assetPath") }
		));
		return MakePolicyFailureResponse(CallId, TEXT("Either 'assetPath' or 'name' is required."), Errors);
	}

	FString DirFromAssetPath, NameFromAssetPath;
	if (bHasAssetPath)
	{
		FString RawAssetPath;
		Cmd->TryGetStringField(TEXT("assetPath"), RawAssetPath);
		if (!SplitAssetPath(RawAssetPath, DirFromAssetPath, NameFromAssetPath))
		{
			TArray<TSharedPtr<FJsonValue>> Errors;
			Errors.Add(MakePolicyDiagnostic(
				TEXT("INVALID_VALUE"),
				TEXT("schema"),
				TEXT("assetPath"),
				RawAssetPath,
				{}
			));
			return MakePolicyFailureResponse(CallId, FString::Printf(TEXT("Malformed assetPath: %s"), *RawAssetPath), Errors);
		}
	}

	FString DirFromNamePath, NameFromNamePath;
	if (bHasName)
	{
		Cmd->TryGetStringField(TEXT("name"), NameFromNamePath);
		if (NameFromNamePath.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Errors;
			Errors.Add(MakePolicyDiagnostic(TEXT("INVALID_VALUE"), TEXT("schema"), TEXT("name"), TEXT(""), {}));
			return MakePolicyFailureResponse(CallId, TEXT("'name' must be a non-empty string."), Errors);
		}
		if (bHasPath)
		{
			Cmd->TryGetStringField(TEXT("path"), DirFromNamePath);
		}
		if (DirFromNamePath.IsEmpty())
		{
			DirFromNamePath = TEXT("/Game/Materials/Functions");
		}
		if (!NameFromNamePath.StartsWith(TEXT("MF_")))
		{
			Warnings.Add(MakePolicyDiagnostic(
				TEXT("ALIAS_NORMALIZED"),
				TEXT("schema"),
				TEXT("name"),
				NameFromNamePath,
				{ FString(TEXT("MF_")) + NameFromNamePath },
				TEXT("'MF_' prefix auto-applied to canonical asset name")
			));
			NameFromNamePath = TEXT("MF_") + NameFromNamePath;
		}
	}

	if (bHasAssetPath && bHasName)
	{
		FString CompareNameFromAssetPath = NameFromAssetPath;
		const bool bMfPrefixedAssetPath = !CompareNameFromAssetPath.StartsWith(TEXT("MF_"));
		if (bMfPrefixedAssetPath)
			CompareNameFromAssetPath = TEXT("MF_") + CompareNameFromAssetPath;

		const FString CanonFromA = BuildCanonical(DirFromAssetPath, CompareNameFromAssetPath);
		const FString CanonFromB = BuildCanonical(DirFromNamePath, NameFromNamePath);

		if (!CanonFromA.Equals(CanonFromB, ESearchCase::IgnoreCase))
		{
			TArray<TSharedPtr<FJsonValue>> Errors;
			Errors.Add(MakePolicyDiagnostic(
				TEXT("INVALID_VALUE"),
				TEXT("schema"),
				TEXT("assetPath"),
				CanonFromA,
				{ CanonFromB },
				TEXT("'assetPath' and 'name+path' resolve to different targets")
			));
			return MakePolicyFailureResponse(CallId, TEXT("'assetPath' and 'name+path' disagree."), Errors, Warnings);
		}

		ResolvedDir = DirFromAssetPath;
		ResolvedName = CompareNameFromAssetPath;
		CanonicalAssetPath = CanonFromA;

		// If MF_ was auto-applied to the assetPath-derived name during comparison,
		// the AI's canonical input was effectively rewritten — warn so the AI learns.
		if (bMfPrefixedAssetPath)
		{
			Warnings.Add(MakePolicyDiagnostic(
				TEXT("ALIAS_NORMALIZED"),
				TEXT("schema"),
				TEXT("assetPath"),
				NameFromAssetPath,
				{ CompareNameFromAssetPath },
				TEXT("'MF_' prefix auto-applied to assetPath's basename to match the canonical form")
			));
		}

		Warnings.Add(MakePolicyDiagnostic(
			TEXT("ALIAS_NORMALIZED"),
			TEXT("schema"),
			TEXT("name"),
			TEXT("name+path"),
			{ TEXT("assetPath") },
			TEXT("'assetPath' is the canonical input; 'name'/'path' are accepted as a migration alias pair")
		));
	}
	else if (bHasAssetPath)
	{
		ResolvedDir = DirFromAssetPath;
		ResolvedName = NameFromAssetPath;
		if (!ResolvedName.StartsWith(TEXT("MF_")))
		{
			Warnings.Add(MakePolicyDiagnostic(
				TEXT("ALIAS_NORMALIZED"),
				TEXT("schema"),
				TEXT("assetPath"),
				ResolvedName,
				{ FString(TEXT("MF_")) + ResolvedName },
				TEXT("'MF_' prefix auto-applied to canonical asset name")
			));
			ResolvedName = TEXT("MF_") + ResolvedName;
		}
		CanonicalAssetPath = BuildCanonical(ResolvedDir, ResolvedName);
	}
	else
	{
		ResolvedDir = DirFromNamePath;
		ResolvedName = NameFromNamePath;
		CanonicalAssetPath = BuildCanonical(ResolvedDir, ResolvedName);
		Warnings.Add(MakePolicyDiagnostic(
			TEXT("ALIAS_NORMALIZED"),
			TEXT("schema"),
			TEXT("name"),
			TEXT("name+path"),
			{ TEXT("assetPath") },
			FString::Printf(TEXT("Resolved to assetPath: %s — prefer sending 'assetPath' directly"), *CanonicalAssetPath)
		));
	}

	// Phase 3: target validation
	if (!ResolvedDir.StartsWith(TEXT("/Game/")) && !ResolvedDir.Equals(TEXT("/Game")))
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakePolicyDiagnostic(
			TEXT("INVALID_VALUE"),
			TEXT("schema"),
			TEXT("assetPath"),
			ResolvedDir,
			{ TEXT("/Game/...") }
		));
		return MakePolicyFailureResponse(CallId, TEXT("Destination must be under /Game/."), Errors, Warnings);
	}

	const FString PackagePath = FString::Printf(TEXT("%s/%s"), *ResolvedDir, *ResolvedName);

	// Validate UE package-name characters via FPackageName. Catches things like
	// spaces, dots, slashes, control chars in the asset name BEFORE we touch the
	// filesystem (rule 14 — validate before mutate). CreateAsset would otherwise
	// fail with a generic INTERNAL_ERROR after MakeDirectory had already run.
	{
		FText InvalidReason;
		if (!FPackageName::IsValidLongPackageName(PackagePath, true, &InvalidReason))
		{
			TArray<TSharedPtr<FJsonValue>> Errors;
			Errors.Add(MakePolicyDiagnostic(
				TEXT("INVALID_VALUE"),
				TEXT("schema"),
				TEXT("assetPath"),
				PackagePath,
				{},
				InvalidReason.ToString()
			));
			return MakePolicyFailureResponse(CallId, FString::Printf(TEXT("Invalid asset path: %s"), *InvalidReason.ToString()), Errors, Warnings);
		}
	}

	if (UEditorAssetLibrary::DoesAssetExist(PackagePath))
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakePolicyDiagnostic(
			TEXT("ASSET_ALREADY_EXISTS"),
			TEXT("schema"),
			TEXT("assetPath"),
			CanonicalAssetPath,
			{}
		));
		return MakePolicyFailureResponse(CallId, FString::Printf(TEXT("Material function already exists: %s"), *CanonicalAssetPath), Errors, Warnings);
	}

	// Phase 4: create
	if (!UEditorAssetLibrary::DoesDirectoryExist(ResolvedDir))
		UEditorAssetLibrary::MakeDirectory(ResolvedDir);

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateMaterialFunction", "AI: Create Material Function"));

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();
	UObject* NewAsset = AssetTools.CreateAsset(ResolvedName, ResolvedDir, UMaterialFunction::StaticClass(), Factory);
	UMaterialFunction* Func = Cast<UMaterialFunction>(NewAsset);
	if (!Func)
	{
		Tx.Cancel();
		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakePolicyDiagnostic(TEXT("INTERNAL_ERROR"), TEXT("create"), TEXT("assetPath"), CanonicalAssetPath, {}));
		return MakePolicyFailureResponse(CallId, TEXT("Failed to create material function asset."), Errors, Warnings);
	}

	bool bExposeToLibrary = true;
	Cmd->TryGetBoolField(TEXT("exposeToLibrary"), bExposeToLibrary);
	Func->bExposeToLibrary = bExposeToLibrary;

	FString Description;
	if (Cmd->TryGetStringField(TEXT("description"), Description) && !Description.IsEmpty())
		Func->Description = Description;

	FString Category;
	if (Cmd->TryGetStringField(TEXT("category"), Category) && !Category.IsEmpty())
	{
		// Clear first so the AI gets exactly the category it asked for, not
		// appended to UE's default "Misc" pre-populated by the factory.
		// Matches DoSetMetadataOnFunction's edit-side behavior.
		Func->LibraryCategoriesText.Empty();
		TArray<FString> Parts;
		Category.ParseIntoArray(Parts, TEXT("/"));
		for (const FString& Part : Parts)
			Func->LibraryCategoriesText.Add(FText::FromString(Part));
	}

	Tx.AlsoModify(Func);
	Func->MarkPackageDirty();

	// Phase 5: build response envelope
	const FString FinalAssetPath = Func->GetPathName();

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("policy_version"), TEXT("1.1"));
	Result->SetStringField(TEXT("_callId"), CallId);
	Result->SetStringField(TEXT("assetPath"), FinalAssetPath);
	Result->SetStringField(TEXT("name"), ResolvedName);

	TArray<TSharedPtr<FJsonValue>> Created;
	{
		TSharedRef<FJsonObject> Record = MakeShareable(new FJsonObject());
		Record->SetStringField(TEXT("op"), TEXT("create_material_function"));
		Record->SetStringField(TEXT("userName"), ResolvedName);
		Record->SetStringField(TEXT("assignedRef"), FinalAssetPath);
		Record->SetStringField(TEXT("type"), TEXT("MaterialFunction"));
		Created.Add(MakeShareable(new FJsonValueObject(Record)));
	}
	Result->SetArrayField(TEXT("created"), Created);

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeShareable(new FJsonValueString(FString::Printf(TEXT("Created %s"), *ResolvedName))));
	Result->SetArrayField(TEXT("messages"), Messages);
	Result->SetArrayField(TEXT("modified"), MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("skipped"),  MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetArrayField(TEXT("errors"),   MakeEmptyJsonArray());

	return SerializeJsonObject(Result);
}

// ============================================================
// INSPECT MATERIAL FUNCTION — INTERNAL RAW HELPER
// ============================================================
// Reads the function graph and emits a legacy-shaped JSON envelope (with
// `messages[]` sentinel strings for orphan/unsupported-type signals). The
// public InspectMaterialFunction is the policy 1.1 wrapper that reshapes
// this response into structured warnings[].
FString FNwiroIKMaterialTools::InspectMaterialFunctionRaw(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\": false, \"error\": \"Invalid JSON\"}");

	FString FuncPath;
	if (!Cmd->TryGetStringField(TEXT("assetPath"), FuncPath))
		Cmd->TryGetStringField(TEXT("path"), FuncPath);
	if (FuncPath.IsEmpty())
		return TEXT("{\"success\": false, \"error\": \"Missing 'assetPath'\"}");

	UMaterialFunction* Func = LoadMaterialFunction(FuncPath);
	if (!Func)
		return FString::Printf(TEXT("{\"success\": false, \"error\": \"Material function not found: %s\"}"), *FuncPath);

	ClearExpressionRefs();

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Func->GetName());
	Result->SetStringField(TEXT("path"), Func->GetPathName());
	if (!Func->Description.IsEmpty())
		Result->SetStringField(TEXT("description"), Func->Description);
	Result->SetBoolField(TEXT("exposedToLibrary"), Func->bExposeToLibrary);

	if (Func->LibraryCategoriesText.Num() > 0)
	{
		TArray<FString> CatStrings;
		for (const FText& Cat : Func->LibraryCategoriesText)
			CatStrings.Add(Cat.ToString());
		Result->SetStringField(TEXT("category"), FString::Join(CatStrings, TEXT("/")));
	}

	TArray<TSharedPtr<FJsonValue>> InputsArr, OutputsArr, ExprArr;
	TArray<FString> Messages;

	for (const TObjectPtr<UMaterialExpression>& ExprObjPtr : Func->GetExpressionCollection().Expressions)
	{
		if (!ExprObjPtr) continue;
		UMaterialExpression* Expr = ExprObjPtr.Get();

		// Register for follow-up edits
		FString UEName = Expr->GetName();
		if (!UEName.IsEmpty()) ExpressionRefs.Add(UEName, Expr);
		FString CleanRef = UEName;
		if (CleanRef.StartsWith(TEXT("MaterialExpression"))) CleanRef = CleanRef.Mid(18);
		if (!CleanRef.IsEmpty() && CleanRef != UEName) ExpressionRefs.Add(CleanRef, Expr);

		if (UMaterialExpressionFunctionInput* FI = Cast<UMaterialExpressionFunctionInput>(Expr))
		{
			TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
			FString InputName = FI->InputName.ToString();
			Obj->SetStringField(TEXT("name"), InputName);
			Obj->SetNumberField(TEXT("sortPriority"), FI->SortPriority);
			if (!FI->Description.IsEmpty()) Obj->SetStringField(TEXT("description"), FI->Description);

			// Map FI->InputType to the canonical public-contract type string.
			// Switch keyed on the enum (not int) so we don't depend on UE's
			// integer layout — UE 5.6 interleaves Texture2DArray / VolumeTexture
			// / TextureExternal between the contract values, so an array-indexed
			// approach silently drops `type` for high-numbered values like
			// StaticBool. For UE input types that exist but aren't representable
			// in this tool's schema, emit `type:"Unsupported"` and push a
			// sentinel into messages[] so the wrapper can re-shape it into a
			// structured UNSUPPORTED_INPUT_TYPE warning.
			const int32 InputsRowIndex = InputsArr.Num();  // index this row will land at
			FString TypeStr;
			bool bTypeSupported = true;
			switch (FI->InputType)
			{
				case FunctionInput_Scalar:             TypeStr = TEXT("Scalar"); break;
				case FunctionInput_Vector2:            TypeStr = TEXT("Vector2"); break;
				case FunctionInput_Vector3:            TypeStr = TEXT("Vector3"); break;
				case FunctionInput_Vector4:            TypeStr = TEXT("Vector4"); break;
				case FunctionInput_Texture2D:          TypeStr = TEXT("Texture2D"); break;
				case FunctionInput_TextureCube:        TypeStr = TEXT("TextureCube"); break;
				case FunctionInput_StaticBool:         TypeStr = TEXT("StaticBool"); break;
				case FunctionInput_MaterialAttributes: TypeStr = TEXT("MaterialAttributes"); break;
				default:
					TypeStr = TEXT("Unsupported");
					bTypeSupported = false;
					break;
			}
			Obj->SetStringField(TEXT("type"), TypeStr);
			if (!bTypeSupported)
			{
				// Sentinel for the InspectMaterialFunction wrapper to reshape — same
				// channel as the orphan-expression notice (legacy messages[]).
				Messages.Add(FString::Printf(
					TEXT("[input-unsupported-type] index=%d name='%s' ueValue=%d"),
					InputsRowIndex, *InputName, (int32)FI->InputType));
			}

			// previewValue mirrors the canonical write contract:
			// - Scalar          -> number
			// - Vector2/3/4     -> array of N numbers
			// - StaticBool      -> JSON boolean (storage detail FVector4f.X is hidden)
			// - Texture/MatAttrs-> omitted (no scalar/vector literal slot)
			// useAsDefault is always emitted.
			switch (FI->InputType)
			{
				case FunctionInput_Scalar:
					Obj->SetNumberField(TEXT("previewValue"), FI->PreviewValue.X);
					break;
				case FunctionInput_Vector2:
				{
					TArray<TSharedPtr<FJsonValue>> Arr;
					Arr.Add(MakeShareable(new FJsonValueNumber(FI->PreviewValue.X)));
					Arr.Add(MakeShareable(new FJsonValueNumber(FI->PreviewValue.Y)));
					Obj->SetArrayField(TEXT("previewValue"), Arr);
					break;
				}
				case FunctionInput_Vector3:
				{
					TArray<TSharedPtr<FJsonValue>> Arr;
					Arr.Add(MakeShareable(new FJsonValueNumber(FI->PreviewValue.X)));
					Arr.Add(MakeShareable(new FJsonValueNumber(FI->PreviewValue.Y)));
					Arr.Add(MakeShareable(new FJsonValueNumber(FI->PreviewValue.Z)));
					Obj->SetArrayField(TEXT("previewValue"), Arr);
					break;
				}
				case FunctionInput_Vector4:
				{
					TArray<TSharedPtr<FJsonValue>> Arr;
					Arr.Add(MakeShareable(new FJsonValueNumber(FI->PreviewValue.X)));
					Arr.Add(MakeShareable(new FJsonValueNumber(FI->PreviewValue.Y)));
					Arr.Add(MakeShareable(new FJsonValueNumber(FI->PreviewValue.Z)));
					Arr.Add(MakeShareable(new FJsonValueNumber(FI->PreviewValue.W)));
					Obj->SetArrayField(TEXT("previewValue"), Arr);
					break;
				}
				case FunctionInput_StaticBool:
					Obj->SetBoolField(TEXT("previewValue"), FI->PreviewValue.X != 0.0f);
					break;
				default:
					// Texture2D, TextureCube, MaterialAttributes: previewValue omitted.
					break;
			}
			Obj->SetBoolField(TEXT("useAsDefault"), FI->bUsePreviewValueAsDefault != 0);

			InputsArr.Add(MakeShareable(new FJsonValueObject(Obj)));
			if (!InputName.IsEmpty()) ExpressionRefs.Add(InputName, Expr);
		}
		else if (UMaterialExpressionFunctionOutput* FO = Cast<UMaterialExpressionFunctionOutput>(Expr))
		{
			TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
			FString OutputName = FO->OutputName.ToString();
			Obj->SetStringField(TEXT("name"), OutputName);
			Obj->SetNumberField(TEXT("sortPriority"), FO->SortPriority);
			if (!FO->Description.IsEmpty()) Obj->SetStringField(TEXT("description"), FO->Description);
			OutputsArr.Add(MakeShareable(new FJsonValueObject(Obj)));
			if (!OutputName.IsEmpty()) ExpressionRefs.Add(OutputName, Expr);
		}
		else
		{
			TSharedPtr<FJsonObject> ExprObj = SerializeExpression(Expr);
			if (ExprObj.IsValid())
				ExprArr.Add(MakeShareable(new FJsonValueObject(ExprObj.ToSharedRef())));

			// Orphan check: does any other expression reference this one?
			bool bHasOutgoing = false;
			for (const TObjectPtr<UMaterialExpression>& OtherPtr : Func->GetExpressionCollection().Expressions)
			{
				if (!OtherPtr || OtherPtr == Expr) continue;
				for (int32 i = 0; ; i++)
				{
					const FExpressionInput* Input = OtherPtr->GetInput(i);
					if (!Input) break;
					if (Input->Expression == Expr) { bHasOutgoing = true; break; }
				}
				if (bHasOutgoing) break;
			}
			if (!bHasOutgoing)
				Messages.Add(FString::Printf(TEXT("[graph] Expression '%s' is orphaned (no outgoing connection)"), *UEName));
		}
	}

	Result->SetArrayField(TEXT("inputs"), InputsArr);
	Result->SetArrayField(TEXT("outputs"), OutputsArr);
	Result->SetArrayField(TEXT("expressions"), ExprArr);

	// connections - build from expression inputs
	{
		// Build reverse map: expression ptr -> ref name
		TMap<UMaterialExpression*, FString> ExprToRef;
		for (const TObjectPtr<UMaterialExpression>& ExprObjPtr : Func->GetExpressionCollection().Expressions)
		{
			if (!ExprObjPtr) continue;
			UMaterialExpression* E = ExprObjPtr.Get();
			if (UMaterialExpressionFunctionInput* FI = Cast<UMaterialExpressionFunctionInput>(E))
				ExprToRef.Add(E, FI->InputName.ToString());
			else if (UMaterialExpressionFunctionOutput* FO = Cast<UMaterialExpressionFunctionOutput>(E))
				ExprToRef.Add(E, FO->OutputName.ToString());
			else
				ExprToRef.Add(E, E->GetName());
		}

		TArray<TSharedPtr<FJsonValue>> ConnArr;
		for (const TObjectPtr<UMaterialExpression>& ExprObjPtr : Func->GetExpressionCollection().Expressions)
		{
			if (!ExprObjPtr) continue;
			UMaterialExpression* ToExpr = ExprObjPtr.Get();
			const FString* ToRefPtr = ExprToRef.Find(ToExpr);
			if (!ToRefPtr) continue;
			const FString& ToRef = *ToRefPtr;

			for (int32 i = 0; ; i++)
			{
				const FExpressionInput* Input = ToExpr->GetInput(i);
				if (!Input) break;
				if (!Input->Expression) continue;
				const FString* FromRefPtr = ExprToRef.Find(Input->Expression);
				if (!FromRefPtr) continue;

				// Build "from" - append output pin name if non-default
				FString FromStr = *FromRefPtr;
				if (Input->OutputIndex > 0)
				{
					TArray<FExpressionOutput> Outs = Input->Expression->GetOutputs();
					if (Input->OutputIndex < Outs.Num())
					{
						FString OutPin = Outs[Input->OutputIndex].OutputName.ToString();
						FromStr += TEXT(".") + (OutPin.IsEmpty() ? FString::FromInt(Input->OutputIndex) : OutPin);
					}
				}

				// Build "to" - FunctionOutput has a single unnamed input, omit pin suffix
				FString ToStr;
				if (Cast<UMaterialExpressionFunctionOutput>(ToExpr))
				{
					ToStr = ToRef;
				}
				else
				{
					FString PinName = ToExpr->GetInputName(i).ToString();
					ToStr = PinName.IsEmpty() ? ToRef : (ToRef + TEXT(".") + PinName);
				}

				TSharedRef<FJsonObject> Conn = MakeShareable(new FJsonObject());
				Conn->SetStringField(TEXT("from"), FromStr);
				Conn->SetStringField(TEXT("to"), ToStr);
				ConnArr.Add(MakeShareable(new FJsonValueObject(Conn)));
			}
		}
		Result->SetArrayField(TEXT("connections"), ConnArr);
	}

	// referencedBy via AssetRegistry
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();
	TArray<FAssetIdentifier> Referencers;
	AR.GetReferencers(FAssetIdentifier(FName(*Func->GetOutermost()->GetName())), Referencers);

	TArray<TSharedPtr<FJsonValue>> RefsArr;
	for (const FAssetIdentifier& Ref : Referencers)
	{
		TSharedRef<FJsonObject> RefObj = MakeShareable(new FJsonObject());
		RefObj->SetStringField(TEXT("asset"), Ref.PackageName.ToString());
		RefsArr.Add(MakeShareable(new FJsonValueObject(RefObj)));
	}
	Result->SetArrayField(TEXT("referencedBy"), RefsArr);

	TArray<TSharedPtr<FJsonValue>> MsgArr;
	for (const FString& M : Messages) MsgArr.Add(MakeShareable(new FJsonValueString(M)));
	Result->SetArrayField(TEXT("messages"), MsgArr);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// INSPECT MATERIAL FUNCTION (Policy 1.1)
// ============================================================
// Read-only wrapper around InspectMaterialFunctionRaw. Validates strict input
// schema, delegates the heavy graph-walking work to the raw helper, re-shapes
// the response into a policy 1.1 envelope. Orphan messages from the raw
// `messages[]` move to structured `warnings[]` with `ORPHAN_EXPRESSION`, and
// unsupported-input-type sentinels become `UNSUPPORTED_INPUT_TYPE` warnings.
FString FNwiroIKMaterialTools::InspectMaterialFunction(const FString& JsonCommand)
{
	// Phase 1: parse + validate
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("$"), TEXT("invalid-json"), { TEXT("object") }));
		return MakePolicyFailureResponse(TEXT(""), TEXT("Invalid JSON."), Errors);
	}

	const FString CallId = MakeCallId(Cmd);

	static const TSet<FString> AllowedTopLevel = { TEXT("_callId"), TEXT("assetPath") };
	TArray<FString> AllowedList = AllowedTopLevel.Array();
	AllowedList.Sort();

	TArray<TSharedPtr<FJsonValue>> ValidationErrors;
	for (const auto& Pair : Cmd->Values)
	{
		const FString Key(*Pair.Key);
		if (!AllowedTopLevel.Contains(Key))
		{
			ValidationErrors.Add(MakePolicyDiagnostic(TEXT("UNKNOWN_KEY"), TEXT("schema"), Key, Key, AllowedList));
		}
	}

	FString AssetPath;
	Cmd->TryGetStringField(TEXT("assetPath"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("MISSING_REQUIRED_FIELD"), TEXT("schema"), TEXT("assetPath"), TEXT("missing"), { TEXT("assetPath") }));
	}
	if (Cmd->HasField(TEXT("assetPath")) && !JsonFieldIsType(Cmd, TEXT("assetPath"), EJson::String))
	{
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("assetPath"), TEXT("non-string"), { TEXT("string") }));
	}
	if (Cmd->HasField(TEXT("_callId")) && !JsonFieldIsType(Cmd, TEXT("_callId"), EJson::String))
	{
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("_callId"), TEXT("non-string"), { TEXT("string") }));
	}

	if (ValidationErrors.Num() > 0)
	{
		return MakePolicyFailureResponse(CallId, TEXT("Schema validation failed."), ValidationErrors);
	}

	// Phase 2: existence check
	UMaterialFunction* Func = LoadMaterialFunction(AssetPath);
	if (!Func)
	{
		TArray<TSharedPtr<FJsonValue>> NotFoundErrors;
		NotFoundErrors.Add(MakePolicyDiagnostic(
			TEXT("NOT_FOUND"),
			TEXT("schema"),
			TEXT("assetPath"),
			AssetPath,
			{}
		));
		return MakePolicyFailureResponse(CallId, FString::Printf(TEXT("Material function not found: %s"), *AssetPath), NotFoundErrors);
	}

	// Phase 3: delegate to legacy, then re-shape
	const FString RawResponse = InspectMaterialFunctionRaw(JsonCommand);

	TSharedPtr<FJsonObject> Raw;
	TSharedRef<TJsonReader<>> RawReader = TJsonReaderFactory<>::Create(RawResponse);
	if (!FJsonSerializer::Deserialize(RawReader, Raw) || !Raw.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakePolicyDiagnostic(TEXT("INTERNAL_ERROR"), TEXT("inspect_material_function"), TEXT("response"), TEXT("invalid-json"), {}));
		return MakePolicyFailureResponse(CallId, TEXT("Tool returned invalid JSON."), Errors);
	}

	// Phase 4: build policy envelope from legacy response
	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("policy_version"), TEXT("1.1"));
	Result->SetStringField(TEXT("_callId"), CallId);
	Result->SetStringField(TEXT("assetPath"), AssetPath);  // canonical (replaces legacy `path`)

	// Copy domain fields verbatim from legacy response
	FString FieldStr;
	if (Raw->TryGetStringField(TEXT("name"), FieldStr))
		Result->SetStringField(TEXT("name"), FieldStr);
	if (Raw->TryGetStringField(TEXT("description"), FieldStr))
		Result->SetStringField(TEXT("description"), FieldStr);
	if (Raw->TryGetStringField(TEXT("category"), FieldStr))
		Result->SetStringField(TEXT("category"), FieldStr);
	bool bExposed = false;
	if (Raw->TryGetBoolField(TEXT("exposedToLibrary"), bExposed))
		Result->SetBoolField(TEXT("exposedToLibrary"), bExposed);

	const TArray<TSharedPtr<FJsonValue>>* ArrPtr;
	if (Raw->TryGetArrayField(TEXT("inputs"), ArrPtr))       Result->SetArrayField(TEXT("inputs"), *ArrPtr);
	if (Raw->TryGetArrayField(TEXT("outputs"), ArrPtr))      Result->SetArrayField(TEXT("outputs"), *ArrPtr);
	if (Raw->TryGetArrayField(TEXT("expressions"), ArrPtr))  Result->SetArrayField(TEXT("expressions"), *ArrPtr);
	if (Raw->TryGetArrayField(TEXT("connections"), ArrPtr))  Result->SetArrayField(TEXT("connections"), *ArrPtr);
	if (Raw->TryGetArrayField(TEXT("referencedBy"), ArrPtr)) Result->SetArrayField(TEXT("referencedBy"), *ArrPtr);

	// Reshape messages into structured warnings[]:
	//   - "[graph] Expression 'X' is orphaned (...)"          → ORPHAN_EXPRESSION
	//   - "[input-unsupported-type] index=N name='X' ueValue=K" → UNSUPPORTED_INPUT_TYPE
	// Everything else stays in messages[].
	TArray<TSharedPtr<FJsonValue>> RemainingMessages;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	const TArray<TSharedPtr<FJsonValue>>* RawMsgs;
	if (Raw->TryGetArrayField(TEXT("messages"), RawMsgs))
	{
		for (const TSharedPtr<FJsonValue>& MsgVal : *RawMsgs)
		{
			if (!MsgVal.IsValid() || MsgVal->Type != EJson::String)
			{
				if (MsgVal.IsValid()) RemainingMessages.Add(MsgVal);
				continue;
			}
			const FString MsgStr = MsgVal->AsString();
			if (MsgStr.StartsWith(TEXT("[input-unsupported-type]")))
			{
				// Format: "[input-unsupported-type] index=N name='X' ueValue=K"
				int32 IndexVal = -1;
				int32 UeVal = -1;
				FString InputNameVal;

				int32 IdxPos = MsgStr.Find(TEXT("index="));
				if (IdxPos != INDEX_NONE)
				{
					const FString Tail = MsgStr.Mid(IdxPos + 6);
					int32 SpaceRel = INDEX_NONE;
					Tail.FindChar(' ', SpaceRel);
					LexFromString(IndexVal, *(SpaceRel == INDEX_NONE ? Tail : Tail.Left(SpaceRel)));
				}
				int32 NameStart = MsgStr.Find(TEXT("name='"));
				if (NameStart != INDEX_NONE)
				{
					const FString After = MsgStr.Mid(NameStart + 6);
					int32 EndQuote = INDEX_NONE;
					if (After.FindChar('\'', EndQuote)) InputNameVal = After.Left(EndQuote);
				}
				int32 UePos = MsgStr.Find(TEXT("ueValue="));
				if (UePos != INDEX_NONE)
				{
					const FString Tail = MsgStr.Mid(UePos + 8);
					LexFromString(UeVal, *Tail);
				}

				const FString FieldPath = (IndexVal >= 0)
					? FString::Printf(TEXT("inputs[%d].type"), IndexVal)
					: FString(TEXT("inputs[].type"));
				const FString ReceivedName = FString::Printf(TEXT("FunctionInput_%d"), UeVal);
				const FString HumanMsg = InputNameVal.IsEmpty()
					? FString(TEXT("This function input uses a UE input type that is not representable by edit_material_function."))
					: FString::Printf(TEXT("Input '%s' uses a UE input type that is not representable by edit_material_function."), *InputNameVal);

				Warnings.Add(MakePolicyDiagnostic(
					TEXT("UNSUPPORTED_INPUT_TYPE"),
					TEXT("inspect"),
					FieldPath,
					ReceivedName,
					{ TEXT("Scalar"), TEXT("Vector2"), TEXT("Vector3"), TEXT("Vector4"),
					  TEXT("Texture2D"), TEXT("TextureCube"), TEXT("StaticBool"), TEXT("MaterialAttributes") },
					HumanMsg
				));
				continue;
			}
			if (MsgStr.Contains(TEXT("[graph]")) && MsgStr.Contains(TEXT("orphaned")))
			{
				int32 FirstQuote = INDEX_NONE;
				if (MsgStr.FindChar('\'', FirstQuote))
				{
					const FString After = MsgStr.Mid(FirstQuote + 1);
					int32 SecondQuoteRel = INDEX_NONE;
					if (After.FindChar('\'', SecondQuoteRel))
					{
						const FString ExprName = After.Left(SecondQuoteRel);
						Warnings.Add(MakePolicyDiagnostic(
							TEXT("ORPHAN_EXPRESSION"),
							TEXT("graph"),
							TEXT("expressions"),  // field: structural location
							ExprName,             // received: the orphaned expression's name
							{},
							TEXT("Expression has no outgoing connection")
						));
						continue;
					}
				}
			}
			RemainingMessages.Add(MsgVal);
		}
	}
	Result->SetArrayField(TEXT("messages"), RemainingMessages);
	Result->SetArrayField(TEXT("warnings"), Warnings);

	// Read-only tool: the other envelope arrays are empty.
	Result->SetArrayField(TEXT("created"),  MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("modified"), MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("skipped"),  MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("errors"),   MakeEmptyJsonArray());

	return SerializeJsonObject(Result);
}

// ============================================================
// DO ADD EXPRESSIONS TO FUNCTION
// ============================================================

FNwiroIKMatResult FNwiroIKMaterialTools::DoAddExpressionsToFunction(UMaterialFunction* Func, const TArray<TSharedPtr<FJsonValue>>& Items, const TCHAR* ItemNoun)
{
	FNwiroIKMatResult R;
	R.bSuccess = true;  // helper-level execution succeeds; per-item failures live in Skipped+Errors

	const FString FuncPath = Func->GetPathName();

	// Op name is derived from ItemNoun so each record we emit is stamped with the
	// correct sub-operation key (addInputs / addOutputs / addExpressions). Per §4.2.
	FString OpName = TEXT("addExpressions");
	{
		const FString Noun(ItemNoun);
		if (Noun.Equals(TEXT("input"))) OpName = TEXT("addInputs");
		else if (Noun.Equals(TEXT("output"))) OpName = TEXT("addOutputs");
	}

	TArray<FString> LegacyWarnings;
	TArray<FString> LegacyErrors;
	int32 Added = 0;

	const int32 AutoSpacingX = 250;
	const int32 AutoSpacingY = 200;
	const int32 AutoStartX = -((Items.Num() + 1) / 2) * AutoSpacingX;
	int32 AutoIndex = 0;

	for (int32 Idx = 0; Idx < Items.Num(); ++Idx)
	{
		const TSharedPtr<FJsonObject>& Obj = Items[Idx]->AsObject();
		if (!Obj.IsValid()) continue;

		const FString FieldBase = FString::Printf(TEXT("%s[%d]"), *OpName, Idx);

		FString Ref = Obj->HasField(TEXT("ref")) ? Obj->GetStringField(TEXT("ref"))
			: Obj->HasField(TEXT("name")) ? Obj->GetStringField(TEXT("name")) : TEXT("");
		FString ClassName = Obj->HasField(TEXT("class")) ? Obj->GetStringField(TEXT("class"))
			: Obj->HasField(TEXT("type")) ? Obj->GetStringField(TEXT("type")) : TEXT("");
		bool bHasPos = Obj->HasField(TEXT("x")) || Obj->HasField(TEXT("y"));
		int32 PosX = bHasPos ? (int32)Obj->GetNumberField(TEXT("x")) : AutoStartX + (AutoIndex % 4) * AutoSpacingX;
		int32 PosY = bHasPos ? (int32)Obj->GetNumberField(TEXT("y")) : (AutoIndex / 4) * AutoSpacingY;
		AutoIndex++;

		EFunctionInputType ValidatedInputType = FunctionInput_Scalar;
		FString CanonicalInputType;
		FString InputAliasMessage;
		FString InputTypeStr;
		const bool bIsFunctionInput = NormalizeKey(ClassName) == TEXT("functioninput");
		const bool bHasInputType = Obj->TryGetStringField(TEXT("inputType"), InputTypeStr) || Obj->TryGetStringField(TEXT("inputtype"), InputTypeStr);
		if (bIsFunctionInput && bHasInputType)
		{
			if (!ResolveFunctionInputType(InputTypeStr, ValidatedInputType, CanonicalInputType, InputAliasMessage))
			{
				// G14: legacy INVALID_INPUT_TYPE → policy 8B INVALID_VALUE.
				const FString HumanMsg = FString::Printf(TEXT("Invalid inputType '%s' for input '%s'"), *InputTypeStr, *Ref);
				LegacyErrors.Add(HumanMsg);
				const FString FieldPath = FString::Printf(TEXT("%s.type"), *FieldBase);

				TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
				Skip->SetStringField(TEXT("op"), OpName);
				Skip->SetStringField(TEXT("field"), FieldPath);
				Skip->SetStringField(TEXT("reason"), TEXT("invalid input type"));
				R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

				R.Errors.Add(MakePolicyDiagnostic(
					TEXT("INVALID_VALUE"),
					OpName,
					FieldPath,
					InputTypeStr,
					GetAllowedFunctionInputTypes()
				));
				continue;
			}
		}

		UClass* ExprClass = ResolveExpressionClass(ClassName);
		if (!ExprClass)
		{
			// G14: legacy EXPRESSION_CLASS_NOT_FOUND → policy 8B INVALID_VALUE.
			// `allowed[]` stays empty (we don't enumerate the full UMaterialExpression
			// subclass set in the diagnostic); the common-types hint goes in `message`
			// so the AI has a self-correction starting point without us over-claiming
			// that allowed[] is the complete supported set.
			const FString HumanMsg = FString::Printf(TEXT("Expression class not found: %s"), *ClassName);
			LegacyErrors.Add(HumanMsg);
			const FString FieldPath = FString::Printf(TEXT("%s.type"), *FieldBase);

			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), OpName);
			Skip->SetStringField(TEXT("field"), FieldPath);
			Skip->SetStringField(TEXT("reason"), TEXT("expression class not found"));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

			R.Errors.Add(MakePolicyDiagnostic(
				TEXT("INVALID_VALUE"),
				OpName,
				FieldPath,
				ClassName,
				{},
				FString::Printf(TEXT("Expression class '%s' not recognized. Common types include Add, Multiply, Lerp, Clamp, TextureSample, Constant, Constant3Vector, ScalarParameter, VectorParameter, MaterialFunctionCall, Power, Subtract, Divide, OneMinus, Saturate, Abs, Normalize, Dot, Append, ComponentMask, Fresnel, WorldPosition, TextureCoordinate. Other UMaterialExpression subclasses may also be accepted."), *ClassName)
			));
			continue;
		}

		// G13 + validate-before-mutate (rule 14): for MaterialFunctionCall, fully
		// resolve and pre-load the referenced asset BEFORE creating the node. Any
		// failure here — missing key, empty key, asset not found — emits Skipped +
		// Errors and `continue` without mutating. This replaces the legacy
		// post-creation NOT_FOUND / MISSING_REQUIRED_FIELD reporting which left
		// unbound nodes behind in the graph.
		const bool bIsMaterialFunctionCall = ExprClass->IsChildOf(UMaterialExpressionMaterialFunctionCall::StaticClass());
		UMaterialFunction* PreloadedCalledFunc = nullptr;
		FString PreloadedAliasUsed;
		if (bIsMaterialFunctionCall)
		{
			FString PreCheckPath;
			bool bPreEmpty = false;
			const bool bResolved = ResolveMaterialFunctionCallPath(Obj, PreCheckPath, PreloadedAliasUsed, bPreEmpty);

			if (!bResolved)
			{
				// Two sub-cases: key absent entirely, or key present with empty value.
				// Both are MISSING_REQUIRED_FIELD in policy terms.
				const FString FieldName = (bPreEmpty && !PreloadedAliasUsed.IsEmpty())
					? PreloadedAliasUsed
					: FString(TEXT("materialFunction"));
				const FString FieldPath = FString::Printf(TEXT("%s.%s"), *FieldBase, *FieldName);
				const FString HumanMsg = bPreEmpty
					? FString::Printf(TEXT("MaterialFunctionCall key '%s' provided but value is empty for ref '%s'"), *FieldName, *Ref)
					: FString::Printf(TEXT("MaterialFunctionCall requires 'materialFunction' (ref '%s')"), *Ref);
				const FString Reason = bPreEmpty
					? TEXT("MaterialFunctionCall 'materialFunction' is empty")
					: TEXT("MaterialFunctionCall requires 'materialFunction'");
				LegacyErrors.Add(HumanMsg);

				TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
				Skip->SetStringField(TEXT("op"), OpName);
				Skip->SetStringField(TEXT("field"), FieldPath);
				Skip->SetStringField(TEXT("reason"), Reason);
				R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

				R.Errors.Add(MakePolicyDiagnostic(
					TEXT("MISSING_REQUIRED_FIELD"),
					OpName,
					FieldPath,
					bPreEmpty ? TEXT("empty") : TEXT("missing"),
					{ TEXT("materialFunction") }
				));
				continue;
			}

			// Key resolved to a non-empty path. Pre-load BEFORE creating the node.
			PreloadedCalledFunc = LoadMaterialFunction(PreCheckPath);
			if (!PreloadedCalledFunc)
			{
				const FString HumanMsg = FString::Printf(TEXT("MaterialFunction not found: %s"), *PreCheckPath);
				LegacyErrors.Add(HumanMsg);
				const FString FieldPath = FString::Printf(TEXT("%s.materialFunction"), *FieldBase);

				TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
				Skip->SetStringField(TEXT("op"), OpName);
				Skip->SetStringField(TEXT("field"), FieldPath);
				Skip->SetStringField(TEXT("reason"), TEXT("MaterialFunction asset not found"));
				R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

				R.Errors.Add(MakePolicyDiagnostic(
					TEXT("NOT_FOUND"),
					OpName,
					FieldPath,
					PreCheckPath,
					{}
				));
				continue;
			}
		}

		// G13 + validate-before-mutate (rule 14): for addInputs items that
		// declare an explicit input type, validate previewValue/defaultValue
		// shape BEFORE the node is created. Emits ALIAS_NORMALIZED warnings
		// here so successful validation implies the alias (if any) was honored.
		// On any failure: skip + error + `continue` — no node is created.
		bool bPreviewApply = false;
		FVector4f PreviewValueParsed(0.f, 0.f, 0.f, 0.f);
		bool bPreviewImpliedUseAsDefault = false;
		if (bIsFunctionInput && bHasInputType)
		{
			const bool bHasPreviewExplicit      = Obj->HasField(TEXT("previewValue"));
			const bool bHasDefaultAlias         = Obj->HasField(TEXT("defaultValue"));
			const bool bHasUseAsDefaultExplicit = Obj->HasField(TEXT("useAsDefault"));

			if (bHasPreviewExplicit || bHasDefaultAlias)
			{
				// The actual key the caller sent — used in skipped[]/errors[]
				// field paths so the AI sees its own input echoed back.
				// Alias warnings are emitted only after shape validation
				// succeeds, so we never advertise a "normalized" alias on a
				// rejected value.
				const FString SourceFieldName = bHasPreviewExplicit ? TEXT("previewValue") : TEXT("defaultValue");
				const FString SourceFieldPath = FString::Printf(TEXT("%s.%s"), *FieldBase, *SourceFieldName);

				const TSharedPtr<FJsonValue> Candidate = bHasPreviewExplicit
					? Obj->TryGetField(TEXT("previewValue"))
					: Obj->TryGetField(TEXT("defaultValue"));
				const bool bThisIsAliasOnly = !bHasPreviewExplicit && bHasDefaultAlias;

				// Type support check
				const bool bSupportsPreview =
					ValidatedInputType == FunctionInput_Scalar     ||
					ValidatedInputType == FunctionInput_Vector2    ||
					ValidatedInputType == FunctionInput_Vector3    ||
					ValidatedInputType == FunctionInput_Vector4    ||
					ValidatedInputType == FunctionInput_StaticBool;

				if (!bSupportsPreview)
				{
					TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
					Skip->SetStringField(TEXT("op"), OpName);
					Skip->SetStringField(TEXT("field"), SourceFieldPath);
					Skip->SetStringField(TEXT("reason"), TEXT("previewValue is not supported on this input type"));
					R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

					R.Errors.Add(MakePolicyDiagnostic(
						TEXT("INVALID_VALUE"), OpName, SourceFieldPath, CanonicalInputType,
						{ TEXT("Scalar"), TEXT("Vector2"), TEXT("Vector3"), TEXT("Vector4"), TEXT("StaticBool") },
						TEXT("previewValue is only meaningful for Scalar/Vector2/Vector3/Vector4/StaticBool inputs.")
					));
					continue;  // no node created; no alias warning emitted on failure
				}

				// Shape check against the declared input type.
				auto JsonTypeName = [](EJson T) -> FString {
					switch (T) {
						case EJson::String:  return TEXT("string");
						case EJson::Number:  return TEXT("number");
						case EJson::Boolean: return TEXT("boolean");
						case EJson::Array:   return TEXT("array");
						case EJson::Object:  return TEXT("object");
						case EJson::Null:    return TEXT("null");
						default:             return TEXT("unknown");
					}
				};
				auto EmitShapeFail = [&](const FString& Received, const TArray<FString>& Allowed)
				{
					TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
					Skip->SetStringField(TEXT("op"), OpName);
					Skip->SetStringField(TEXT("field"), SourceFieldPath);
					Skip->SetStringField(TEXT("reason"), TEXT("previewValue shape does not match input type"));
					R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));
					R.Errors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), OpName, SourceFieldPath, Received, Allowed));
				};

				bool bShapeOK = false;
				if (ValidatedInputType == FunctionInput_Scalar)
				{
					if (Candidate.IsValid() && Candidate->Type == EJson::Number)
					{
						PreviewValueParsed.X = (float)Candidate->AsNumber();
						bShapeOK = true;
					}
					else
					{
						EmitShapeFail(Candidate.IsValid() ? JsonTypeName(Candidate->Type) : TEXT("null"), { TEXT("number") });
					}
				}
				else if (ValidatedInputType == FunctionInput_StaticBool)
				{
					if (Candidate.IsValid() && Candidate->Type == EJson::Boolean)
					{
						PreviewValueParsed.X = Candidate->AsBool() ? 1.0f : 0.0f;
						bShapeOK = true;
					}
					else
					{
						EmitShapeFail(Candidate.IsValid() ? JsonTypeName(Candidate->Type) : TEXT("null"), { TEXT("boolean") });
					}
				}
				else
				{
					const int32 N =
						ValidatedInputType == FunctionInput_Vector2 ? 2 :
						ValidatedInputType == FunctionInput_Vector3 ? 3 : 4;
					const FString ExpectedShape = FString::Printf(TEXT("array[%d] of number"), N);

					if (!Candidate.IsValid() || Candidate->Type != EJson::Array)
					{
						EmitShapeFail(Candidate.IsValid() ? JsonTypeName(Candidate->Type) : TEXT("null"), { ExpectedShape });
					}
					else
					{
						const TArray<TSharedPtr<FJsonValue>>& Arr = Candidate->AsArray();
						if (Arr.Num() != N)
						{
							EmitShapeFail(FString::Printf(TEXT("array[%d]"), Arr.Num()), { ExpectedShape });
						}
						else
						{
							bool bAllNumbers = true;
							float Components[4] = { 0.f, 0.f, 0.f, 0.f };
							for (int32 K = 0; K < N; ++K)
							{
								if (!Arr[K].IsValid() || Arr[K]->Type != EJson::Number) { bAllNumbers = false; break; }
								Components[K] = (float)Arr[K]->AsNumber();
							}
							if (!bAllNumbers)
							{
								EmitShapeFail(TEXT("array-with-non-number"), { ExpectedShape });
							}
							else
							{
								PreviewValueParsed.X = Components[0];
								PreviewValueParsed.Y = Components[1];
								if (N >= 3) PreviewValueParsed.Z = Components[2];
								if (N >= 4) PreviewValueParsed.W = Components[3];
								bShapeOK = true;
							}
						}
					}
				}

				if (!bShapeOK) continue;  // no node created; no alias warning emitted on failure

				// Validation passed — only now is the alias guaranteed to be
				// honored, so emit ALIAS_NORMALIZED here (never on failure).
				if (bHasPreviewExplicit && bHasDefaultAlias)
				{
					R.Warnings.Add(MakePolicyDiagnostic(
						TEXT("ALIAS_NORMALIZED"), OpName,
						FString::Printf(TEXT("%s.defaultValue"), *FieldBase),
						TEXT("defaultValue"),
						{ TEXT("previewValue"), TEXT("useAsDefault") },
						TEXT("Both previewValue and defaultValue provided; canonical previewValue applied and defaultValue ignored.")
					));
				}
				else if (bThisIsAliasOnly)
				{
					R.Warnings.Add(MakePolicyDiagnostic(
						TEXT("ALIAS_NORMALIZED"), OpName,
						FString::Printf(TEXT("%s.defaultValue"), *FieldBase),
						TEXT("defaultValue"),
						{ TEXT("previewValue"), TEXT("useAsDefault") },
						bHasUseAsDefaultExplicit
							? TEXT("defaultValue normalized to previewValue. Explicit useAsDefault preserved.")
							: TEXT("defaultValue normalized to previewValue + useAsDefault:true. Use the canonical pair to suppress this warning.")
					));
				}

				bPreviewApply = true;
				bPreviewImpliedUseAsDefault = bThisIsAliasOnly && !bHasUseAsDefaultExplicit;
			}
		}

		UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Func, ExprClass, PosX, PosY);
		if (!Expr)
		{
			// G14: legacy EXPRESSION_CREATE_FAILED → policy 8B INTERNAL_ERROR.
			const FString HumanMsg = FString::Printf(TEXT("Failed to create expression: %s"), *ClassName);
			LegacyErrors.Add(HumanMsg);

			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), OpName);
			Skip->SetStringField(TEXT("field"), FieldBase);
			Skip->SetStringField(TEXT("reason"), TEXT("expression creation failed"));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

			R.Errors.Add(MakePolicyDiagnostic(TEXT("INTERNAL_ERROR"), OpName, FieldBase, ClassName, {}));
			continue;
		}

		// FunctionInput - set name and type
		if (UMaterialExpressionFunctionInput* FI = Cast<UMaterialExpressionFunctionInput>(Expr))
		{
			if (!Ref.IsEmpty()) FI->InputName = FName(*Ref);
			if (Obj->HasField(TEXT("sortPriority"))) FI->SortPriority = (int32)Obj->GetNumberField(TEXT("sortPriority"));
			FString Desc;
			if (Obj->TryGetStringField(TEXT("description"), Desc)) FI->Description = Desc;

			if (bHasInputType)
			{
				FI->InputType = ValidatedInputType;
				if (!InputAliasMessage.IsEmpty())
				{
					const FString WMsg = FString::Printf(TEXT("%s for input '%s'"), *InputAliasMessage, *Ref);
					LegacyWarnings.Add(WMsg);
					R.Warnings.Add(MakePolicyDiagnostic(
						TEXT("ALIAS_NORMALIZED"),
						OpName,
						FString::Printf(TEXT("%s.type"), *FieldBase),
						InputTypeStr,
						{ CanonicalInputType }
					));
				}
			}

			// Apply pre-validated previewValue (precheck above guarantees the
			// shape matches the declared input type, and emitted any alias
			// warnings). For the exotic addExpressions/class:FunctionInput
			// path without an explicit inputType, no precheck ran and
			// bPreviewApply stays false — defaultValue would have to be
			// routed via SetExpressionProperties instead (it has no DefaultValue
			// UProperty, so legacy behavior is PROPERTY_NOT_FOUND).
			if (bPreviewApply)
			{
				FI->PreviewValue = PreviewValueParsed;
				if (bPreviewImpliedUseAsDefault)
				{
					FI->bUsePreviewValueAsDefault = true;
				}
			}
			if (Obj->HasField(TEXT("useAsDefault")))
			{
				FI->bUsePreviewValueAsDefault = Obj->GetBoolField(TEXT("useAsDefault"));
			}
		}
		// FunctionOutput - set name
		else if (UMaterialExpressionFunctionOutput* FO = Cast<UMaterialExpressionFunctionOutput>(Expr))
		{
			if (!Ref.IsEmpty()) FO->OutputName = FName(*Ref);
			if (Obj->HasField(TEXT("sortPriority"))) FO->SortPriority = (int32)Obj->GetNumberField(TEXT("sortPriority"));
			FString Desc;
			if (Obj->TryGetStringField(TEXT("description"), Desc)) FO->Description = Desc;
		}
		// MaterialFunctionCall — bind the pre-loaded function. The G13 pre-check
		// above guarantees PreloadedCalledFunc is non-null when we reach here, so
		// no fallback or error handling is needed in this branch.
		else if (UMaterialExpressionMaterialFunctionCall* FuncCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
		{
			check(PreloadedCalledFunc);
			FuncCall->SetMaterialFunction(PreloadedCalledFunc);
			FuncCall->UpdateFromFunctionResource();

			if (!PreloadedAliasUsed.IsEmpty())
			{
				const FString WMsg = FString::Printf(
					TEXT("Normalized MaterialFunctionCall key '%s' to 'materialFunction' for ref '%s'"),
					*PreloadedAliasUsed, *Ref);
				LegacyWarnings.Add(WMsg);
				R.Warnings.Add(MakePolicyDiagnostic(
					TEXT("ALIAS_NORMALIZED"),
					OpName,
					FString::Printf(TEXT("%s.materialFunction"), *FieldBase),
					PreloadedAliasUsed,
					{ TEXT("materialFunction") }
				));
			}
		}
		// Parameter expressions - auto-set ParameterName
		else if (!Ref.IsEmpty())
		{
			if (UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
				SP->ParameterName = FName(*Ref);
			else if (UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
				VP->ParameterName = FName(*Ref);
			else if (UMaterialExpressionTextureSampleParameter2D* TP = Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
				TP->ParameterName = FName(*Ref);
		}

		// Properties (G1/G9/G14 fallback paths): collect property-level failures
		// from both the top-level object and the nested `properties{}` object,
		// then emit policy-shaped diagnostics with the right field-path prefix.
		// The expression node IS in R.Created — these are partial successes per
		// rule 12: the node exists, but the requested property assignment did not.
		auto AddPropertyNotFound = [&](const TArray<FString>& UnknownKeys, const FString& Prefix)
		{
			for (const FString& Key : UnknownKeys)
			{
				const FString FieldPath = FString::Printf(TEXT("%s[%d].%s%s"), *OpName, Idx, *Prefix, *Key);

				TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
				Skip->SetStringField(TEXT("op"), OpName);
				Skip->SetStringField(TEXT("field"), FieldPath);
				Skip->SetStringField(TEXT("reason"), TEXT("property not found on expression class"));
				R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

				R.Errors.Add(MakePolicyDiagnostic(
					TEXT("PROPERTY_NOT_FOUND"),
					OpName,
					FieldPath,
					Key,
					{}  // R2: empty allowed[]; future iteration can upgrade to exact UProperty names
				));
			}
		};
		auto AddAssetNotFound = [&](const TMap<FString, FString>& Misses, const FString& Prefix)
		{
			for (const auto& Pair : Misses)
			{
				const FString FieldPath = FString::Printf(TEXT("%s[%d].%s%s"), *OpName, Idx, *Prefix, *Pair.Key);

				TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
				Skip->SetStringField(TEXT("op"), OpName);
				Skip->SetStringField(TEXT("field"), FieldPath);
				Skip->SetStringField(TEXT("reason"), TEXT("referenced asset not found"));
				R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

				R.Errors.Add(MakePolicyDiagnostic(
					TEXT("NOT_FOUND"),
					OpName,
					FieldPath,
					Pair.Value,  // received = the asset path that failed to load
					{}
				));
			}
		};

		TArray<FString> TopLevelUnknownProps;
		TMap<FString, FString> TopLevelAssetMisses;
		SetExpressionProperties(Expr, Obj, &LegacyWarnings, &R.Errors, &TopLevelUnknownProps, &TopLevelAssetMisses);
		AddPropertyNotFound(TopLevelUnknownProps, TEXT(""));
		AddAssetNotFound(TopLevelAssetMisses, TEXT(""));

		const TSharedPtr<FJsonObject>* PropsObj;
		if (Obj->TryGetObjectField(TEXT("properties"), PropsObj))
		{
			TArray<FString> NestedUnknownProps;
			TMap<FString, FString> NestedAssetMisses;
			SetExpressionProperties(Expr, *PropsObj, &LegacyWarnings, &R.Errors, &NestedUnknownProps, &NestedAssetMisses);
			AddPropertyNotFound(NestedUnknownProps, TEXT("properties."));
			AddAssetNotFound(NestedAssetMisses, TEXT("properties."));
		}

		if (!Ref.IsEmpty()) ExpressionRefs.Add(Ref, Expr);
		FString UEName = Expr->GetName();
		if (!UEName.IsEmpty() && UEName != Ref) ExpressionRefs.Add(UEName, Expr);

		// G2: created record carries `op` and `userName` (formerly `userRef`).
		TSharedRef<FJsonObject> Record = MakeShareable(new FJsonObject());
		Record->SetStringField(TEXT("op"), OpName);
		Record->SetStringField(TEXT("userName"), Ref);
		Record->SetStringField(TEXT("assignedRef"), UEName);
		Record->SetStringField(TEXT("type"), ClassName);
		R.Created.Add(MakeShareable(new FJsonValueObject(Record)));

		Added++;
	}

	FString Msg = FString::Printf(TEXT("Added %d %s(s)"), Added, ItemNoun);
	if (LegacyWarnings.Num() > 0) Msg += TEXT(". Warnings: ") + FString::Join(LegacyWarnings, TEXT("; "));
	if (LegacyErrors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(LegacyErrors, TEXT("; "));
	R.Message = Msg;
	return R;
}

// ============================================================
// DO CONNECT EXPRESSIONS IN FUNCTION
// ============================================================

FNwiroIKMatResult FNwiroIKMaterialTools::DoConnectExpressionsInFunction(UMaterialFunction* Func, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	FNwiroIKMatResult R;
	R.bSuccess = true;  // helper-level execution; per-item failures live in Skipped+Errors

	const FString FuncPath = Func->GetPathName();

	TArray<FString> LegacyErrors;
	int32 Connected = 0;

	// All current refs available — used in NODE_REF_NOT_FOUND diagnostics.
	// ExpressionRefs is a static class member, so no `this` capture needed (and
	// `[this]` would not compile inside a static member function).
	auto BuildAllowedRefs = []()
	{
		TArray<FString> Out;
		for (const auto& Pair : ExpressionRefs)
		{
			if (Pair.Value.IsValid()) Out.Add(Pair.Key);
		}
		Out.Sort();
		return Out;
	};

	for (int32 Idx = 0; Idx < Items.Num(); ++Idx)
	{
		const TSharedPtr<FJsonObject>& Obj = Items[Idx]->AsObject();
		if (!Obj.IsValid()) continue;

		FString FromStr, ToStr;
		Obj->TryGetStringField(TEXT("from"), FromStr);
		Obj->TryGetStringField(TEXT("to"),   ToStr);

		const FString FieldBase = FString::Printf(TEXT("connect[%d]"), Idx);
		const FString FromField = FString::Printf(TEXT("%s.from"), *FieldBase);
		const FString ToField   = FString::Printf(TEXT("%s.to"),   *FieldBase);

		// Pin-syntax alias: AIs sometimes send `Ref:Pin` instead of canonical
		// `Ref.Pin`. Per policy rule 6, accept the alias, normalize to canonical
		// dot form, and emit an ALIAS_NORMALIZED warning naming the canonical.
		if (FromStr.Contains(TEXT(":")))
		{
			const FString Normalized = FromStr.Replace(TEXT(":"), TEXT("."));
			R.Warnings.Add(MakePolicyDiagnostic(
				TEXT("ALIAS_NORMALIZED"),
				TEXT("connect"),
				FromField,
				FromStr,
				{ Normalized },
				TEXT("Pin syntax uses dot (Ref.Pin), not colon (Ref:Pin). Normalized.")
			));
			FromStr = Normalized;
		}
		if (ToStr.Contains(TEXT(":")))
		{
			const FString Normalized = ToStr.Replace(TEXT(":"), TEXT("."));
			R.Warnings.Add(MakePolicyDiagnostic(
				TEXT("ALIAS_NORMALIZED"),
				TEXT("connect"),
				ToField,
				ToStr,
				{ Normalized },
				TEXT("Pin syntax uses dot (Ref.Pin), not colon (Ref:Pin). Normalized.")
			));
			ToStr = Normalized;
		}

		int32 FromDot, ToDot;
		FString FromRef, FromOutput, ToRef, ToInput;
		if (FromStr.FindChar('.', FromDot)) { FromRef = FromStr.Left(FromDot); FromOutput = FromStr.Mid(FromDot + 1); }
		else { FromRef = FromStr; FromOutput = TEXT(""); }
		if (ToStr.FindChar('.', ToDot))   { ToRef = ToStr.Left(ToDot);   ToInput  = ToStr.Mid(ToDot + 1); }
		else { ToRef = ToStr; ToInput = TEXT(""); }

		// Resolve source expression
		TWeakObjectPtr<UMaterialExpression>* FromExprPtr = ExpressionRefs.Find(FromRef);
		if (!FromExprPtr || !FromExprPtr->IsValid())
		{
			for (const TObjectPtr<UMaterialExpression>& E : Func->GetExpressionCollection().Expressions)
			{
				if (E && E->GetName() == FromRef)
				{
					ExpressionRefs.Add(FromRef, E);
					FromExprPtr = ExpressionRefs.Find(FromRef);
					break;
				}
			}
		}
		if (!FromExprPtr || !FromExprPtr->IsValid())
		{
			// G14: legacy SOURCE_EXPRESSION_NOT_FOUND → policy 8B NODE_REF_NOT_FOUND.
			const FString HumanMsg = FString::Printf(TEXT("Source expression not found: %s"), *FromRef);
			LegacyErrors.Add(HumanMsg);

			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), TEXT("connect"));
			Skip->SetStringField(TEXT("field"), FromField);
			Skip->SetStringField(TEXT("reason"), TEXT("source ref not found"));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

			R.Errors.Add(MakePolicyDiagnostic(
				TEXT("NODE_REF_NOT_FOUND"),
				TEXT("connect"),
				FromField,
				FromStr,
				BuildAllowedRefs()
			));
			continue;
		}

		// Resolve target expression
		TWeakObjectPtr<UMaterialExpression>* ToExprPtr = ExpressionRefs.Find(ToRef);
		if (!ToExprPtr || !ToExprPtr->IsValid())
		{
			for (const TObjectPtr<UMaterialExpression>& E : Func->GetExpressionCollection().Expressions)
			{
				if (E && E->GetName() == ToRef)
				{
					ExpressionRefs.Add(ToRef, E);
					ToExprPtr = ExpressionRefs.Find(ToRef);
					break;
				}
			}
		}
		if (!ToExprPtr || !ToExprPtr->IsValid())
		{
			// G14: legacy TARGET_EXPRESSION_NOT_FOUND → policy 8B NODE_REF_NOT_FOUND.
			const FString HumanMsg = FString::Printf(TEXT("Target expression not found: %s"), *ToRef);
			LegacyErrors.Add(HumanMsg);

			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), TEXT("connect"));
			Skip->SetStringField(TEXT("field"), ToField);
			Skip->SetStringField(TEXT("reason"), TEXT("target ref not found"));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

			R.Errors.Add(MakePolicyDiagnostic(
				TEXT("NODE_REF_NOT_FOUND"),
				TEXT("connect"),
				ToField,
				ToStr,
				BuildAllowedRefs()
			));
			continue;
		}

		bool bOk = UMaterialEditingLibrary::ConnectMaterialExpressions(FromExprPtr->Get(), FromOutput, ToExprPtr->Get(), ToInput);
		if (!bOk && IsUnnamedSingleOutputRequest(FromExprPtr->Get(), FromOutput))
		{
			// Unnamed single-output fallback (legacy bug #9 fix).
			if (TryConnectByOutputIndex(FromExprPtr->Get(), 0, ToExprPtr->Get(), ToInput))
			{
				bOk = true;
				R.Warnings.Add(MakePolicyDiagnostic(
					TEXT("ALIAS_NORMALIZED"),
					TEXT("connect"),
					FromField,
					FromOutput.IsEmpty() ? TEXT("(empty)") : *FromOutput,
					{},
					FString::Printf(TEXT("Connected %s -> %s via index 0 (source has a single unnamed output)"), *FromStr, *ToStr)
				));
			}
		}

		if (bOk)
		{
			Connected++;
			TSharedRef<FJsonObject> Mod = MakeShareable(new FJsonObject());
			Mod->SetStringField(TEXT("op"), TEXT("connect"));
			Mod->SetStringField(TEXT("from"), FromStr);
			Mod->SetStringField(TEXT("to"), ToStr);
			Mod->SetStringField(TEXT("assetPath"), FuncPath);
			R.Modified.Add(MakeShareable(new FJsonValueObject(Mod)));
			continue;
		}

		// Connect failed — discover why so we can emit the right 8B code per G14.
		// Strategy: check pin existence on each side; whatever's missing tells us
		// which code to emit. If both pins exist, the failure is type-related.
		const TArray<FString> AvailOutputs = GetExpressionOutputNames(FromExprPtr->Get());
		const TArray<FString> AvailInputs  = GetExpressionInputNames(ToExprPtr->Get());

		auto PinExists = [](const TArray<FString>& Pins, const FString& Name)
		{
			if (Name.IsEmpty()) return true;  // empty matches the default/first pin
			for (const FString& P : Pins)
			{
				if (P.Equals(Name, ESearchCase::IgnoreCase)) return true;
			}
			return false;
		};
		const bool bFromPinExists = PinExists(AvailOutputs, FromOutput);
		const bool bToPinExists   = PinExists(AvailInputs,  ToInput);

		if (!bFromPinExists)
		{
			const FString HumanMsg = FString::Printf(TEXT("Source output pin not found: %s on %s"), *FromOutput, *FromRef);
			LegacyErrors.Add(HumanMsg);

			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), TEXT("connect"));
			Skip->SetStringField(TEXT("field"), FromField);
			Skip->SetStringField(TEXT("reason"), TEXT("source output pin not found"));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

			R.Errors.Add(MakePolicyDiagnostic(
				TEXT("SOURCE_PIN_NOT_FOUND"),
				TEXT("connect"),
				FromField,
				FromStr,
				AvailOutputs
			));
		}
		else if (!bToPinExists)
		{
			const FString HumanMsg = FString::Printf(TEXT("Target input pin not found: %s on %s"), *ToInput, *ToRef);
			LegacyErrors.Add(HumanMsg);

			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), TEXT("connect"));
			Skip->SetStringField(TEXT("field"), ToField);
			Skip->SetStringField(TEXT("reason"), TEXT("target input pin not found"));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

			R.Errors.Add(MakePolicyDiagnostic(
				TEXT("TARGET_PIN_NOT_FOUND"),
				TEXT("connect"),
				ToField,
				ToStr,
				AvailInputs
			));
		}
		else
		{
			// Both pins resolved but UMaterialEditingLibrary refused — type mismatch.
			const FString HumanMsg = FString::Printf(TEXT("Pin type mismatch connecting %s -> %s"), *FromStr, *ToStr);
			LegacyErrors.Add(HumanMsg);

			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), TEXT("connect"));
			Skip->SetStringField(TEXT("field"), ToField);
			Skip->SetStringField(TEXT("reason"), TEXT("pin type mismatch"));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

			R.Errors.Add(MakePolicyDiagnostic(
				TEXT("PIN_TYPE_MISMATCH"),
				TEXT("connect"),
				ToField,
				ToStr,
				{}  // no useful copyable alternative for type mismatch
			));
		}
	}

	FString Msg = FString::Printf(TEXT("Connected %d/%d expression(s)"), Connected, Items.Num());
	if (LegacyErrors.Num() > 0) Msg += TEXT(". Errors: ") + FString::Join(LegacyErrors, TEXT("; "));
	R.Message = Msg;
	return R;
}

// ============================================================
// DO REMOVE EXPRESSIONS FROM FUNCTION
// ============================================================

FNwiroIKMatResult FNwiroIKMaterialTools::DoRemoveExpressionsFromFunction(UMaterialFunction* Func, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	FNwiroIKMatResult R;
	R.bSuccess = true;

	const FString FuncPath = Func->GetPathName();

	// All current refs available — used as `allowed` on NODE_REF_NOT_FOUND diagnostics.
	TArray<FString> AllowedRefs;
	for (const auto& Pair : ExpressionRefs)
	{
		if (Pair.Value.IsValid()) AllowedRefs.Add(Pair.Key);
	}
	AllowedRefs.Sort();

	int32 Removed = 0;
	for (int32 Idx = 0; Idx < Items.Num(); ++Idx)
	{
		const FString Ref = Items[Idx]->AsString();
		const FString FieldPath = FString::Printf(TEXT("deleteExpressions[%d]"), Idx);

		if (Ref.IsEmpty())
		{
			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), TEXT("deleteExpressions"));
			Skip->SetStringField(TEXT("field"), FieldPath);
			Skip->SetStringField(TEXT("reason"), TEXT("empty ref"));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

			R.Errors.Add(MakePolicyDiagnostic(
				TEXT("MISSING_REQUIRED_FIELD"),
				TEXT("deleteExpressions"),
				FieldPath,
				TEXT("empty"),
				{}
			));
			continue;
		}

		TWeakObjectPtr<UMaterialExpression>* ExprPtr = ExpressionRefs.Find(Ref);
		if (ExprPtr && ExprPtr->IsValid())
		{
			UMaterialExpression* Expr = ExprPtr->Get();
			Func->GetExpressionCollection().Expressions.Remove(Expr);
			Expr->MarkAsGarbage();
			ExpressionRefs.Remove(Ref);
			Removed++;

			TSharedRef<FJsonObject> Mod = MakeShareable(new FJsonObject());
			Mod->SetStringField(TEXT("op"), TEXT("deleteExpressions"));
			Mod->SetStringField(TEXT("ref"), Ref);
			Mod->SetStringField(TEXT("assetPath"), FuncPath);
			R.Modified.Add(MakeShareable(new FJsonValueObject(Mod)));
		}
		else
		{
			// G12: previously this silently no-op'd; now it produces a Skipped+Errors pair.
			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), TEXT("deleteExpressions"));
			Skip->SetStringField(TEXT("field"), FieldPath);
			Skip->SetStringField(TEXT("reason"), TEXT("expression not found"));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

			R.Errors.Add(MakePolicyDiagnostic(
				TEXT("NODE_REF_NOT_FOUND"),
				TEXT("deleteExpressions"),
				FieldPath,
				Ref,
				AllowedRefs
			));
		}
	}

	R.Message = FString::Printf(TEXT("Removed %d expression(s)"), Removed);
	return R;
}

// ============================================================
// DO REMOVE INPUTS FROM FUNCTION
// ============================================================
// Factored from the original inline block per §4.2.
// Populates Modified per successful removal; Skipped + Errors per item that
// did not resolve to a FunctionInput. bSuccess is always true (legacy semantics).
FNwiroIKMatResult FNwiroIKMaterialTools::DoRemoveInputsFromFunction(UMaterialFunction* Func, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	FNwiroIKMatResult R;
	R.bSuccess = true;

	const FString FuncPath = Func->GetPathName();

	// Build allowed list once for skipped diagnostics: current input refs.
	TArray<FString> AllowedInputs;
	for (const TObjectPtr<UMaterialExpression>& ExprPtr : Func->GetExpressionCollection().Expressions)
	{
		if (UMaterialExpressionFunctionInput* FI = Cast<UMaterialExpressionFunctionInput>(ExprPtr.Get()))
		{
			const FString N = FI->InputName.ToString();
			if (!N.IsEmpty()) AllowedInputs.Add(N);
		}
	}
	AllowedInputs.Sort();

	int32 Removed = 0;
	for (int32 Idx = 0; Idx < Items.Num(); ++Idx)
	{
		const FString InputName = Items[Idx]->AsString();
		const FString FieldPath = FString::Printf(TEXT("removeInputs[%d]"), Idx);

		TWeakObjectPtr<UMaterialExpression>* ExprPtr = ExpressionRefs.Find(InputName);
		if (ExprPtr && ExprPtr->IsValid() && ExprPtr->Get()->IsA<UMaterialExpressionFunctionInput>())
		{
			UMaterialExpression* Expr = ExprPtr->Get();
			Func->GetExpressionCollection().Expressions.Remove(Expr);
			Expr->MarkAsGarbage();
			ExpressionRefs.Remove(InputName);
			Removed++;

			TSharedRef<FJsonObject> Mod = MakeShareable(new FJsonObject());
			Mod->SetStringField(TEXT("op"), TEXT("removeInputs"));
			Mod->SetStringField(TEXT("ref"), InputName);
			Mod->SetStringField(TEXT("assetPath"), FuncPath);
			R.Modified.Add(MakeShareable(new FJsonValueObject(Mod)));
		}
		else
		{
			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), TEXT("removeInputs"));
			Skip->SetStringField(TEXT("field"), FieldPath);
			Skip->SetStringField(TEXT("reason"), TEXT("input not found"));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

			R.Errors.Add(MakePolicyDiagnostic(
				TEXT("NODE_REF_NOT_FOUND"),
				TEXT("removeInputs"),
				FieldPath,
				InputName,
				AllowedInputs
			));
		}
	}

	R.Message = FString::Printf(TEXT("Removed %d input(s)"), Removed);
	return R;
}

// ============================================================
// DO REMOVE OUTPUTS FROM FUNCTION
// ============================================================
FNwiroIKMatResult FNwiroIKMaterialTools::DoRemoveOutputsFromFunction(UMaterialFunction* Func, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	FNwiroIKMatResult R;
	R.bSuccess = true;

	const FString FuncPath = Func->GetPathName();

	TArray<FString> AllowedOutputs;
	for (const TObjectPtr<UMaterialExpression>& ExprPtr : Func->GetExpressionCollection().Expressions)
	{
		if (UMaterialExpressionFunctionOutput* FO = Cast<UMaterialExpressionFunctionOutput>(ExprPtr.Get()))
		{
			const FString N = FO->OutputName.ToString();
			if (!N.IsEmpty()) AllowedOutputs.Add(N);
		}
	}
	AllowedOutputs.Sort();

	int32 Removed = 0;
	for (int32 Idx = 0; Idx < Items.Num(); ++Idx)
	{
		const FString OutputName = Items[Idx]->AsString();
		const FString FieldPath = FString::Printf(TEXT("removeOutputs[%d]"), Idx);

		TWeakObjectPtr<UMaterialExpression>* ExprPtr = ExpressionRefs.Find(OutputName);
		if (ExprPtr && ExprPtr->IsValid() && ExprPtr->Get()->IsA<UMaterialExpressionFunctionOutput>())
		{
			UMaterialExpression* Expr = ExprPtr->Get();
			Func->GetExpressionCollection().Expressions.Remove(Expr);
			Expr->MarkAsGarbage();
			ExpressionRefs.Remove(OutputName);
			Removed++;

			TSharedRef<FJsonObject> Mod = MakeShareable(new FJsonObject());
			Mod->SetStringField(TEXT("op"), TEXT("removeOutputs"));
			Mod->SetStringField(TEXT("ref"), OutputName);
			Mod->SetStringField(TEXT("assetPath"), FuncPath);
			R.Modified.Add(MakeShareable(new FJsonValueObject(Mod)));
		}
		else
		{
			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), TEXT("removeOutputs"));
			Skip->SetStringField(TEXT("field"), FieldPath);
			Skip->SetStringField(TEXT("reason"), TEXT("output not found"));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));

			R.Errors.Add(MakePolicyDiagnostic(
				TEXT("NODE_REF_NOT_FOUND"),
				TEXT("removeOutputs"),
				FieldPath,
				OutputName,
				AllowedOutputs
			));
		}
	}

	R.Message = FString::Printf(TEXT("Removed %d output(s)"), Removed);
	return R;
}

// ============================================================
// DO DISCONNECT IN FUNCTION
// ============================================================
// Each item: { "to": "Ref" or "Ref.PinName" }.
// Modified per pin disconnected; Skipped + Errors for ref-not-found or
// pin-not-found, with `allowed[]` carrying the runtime pin list.
FNwiroIKMatResult FNwiroIKMaterialTools::DoDisconnectInFunction(UMaterialFunction* Func, const TArray<TSharedPtr<FJsonValue>>& Items)
{
	FNwiroIKMatResult R;
	R.bSuccess = true;

	const FString FuncPath = Func->GetPathName();

	TArray<FString> AllowedRefs;
	for (const auto& Pair : ExpressionRefs)
	{
		if (Pair.Value.IsValid()) AllowedRefs.Add(Pair.Key);
	}
	AllowedRefs.Sort();

	int32 Disconnected = 0;
	for (int32 Idx = 0; Idx < Items.Num(); ++Idx)
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (!Items[Idx]->TryGetObject(Obj))
		{
			const FString FieldPath = FString::Printf(TEXT("disconnect[%d]"), Idx);
			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), TEXT("disconnect"));
			Skip->SetStringField(TEXT("field"), FieldPath);
			Skip->SetStringField(TEXT("reason"), TEXT("expected object"));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));
			R.Errors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("disconnect"), FieldPath, TEXT("non-object"), { TEXT("object") }));
			continue;
		}

		FString ToStr;
		(*Obj)->TryGetStringField(TEXT("to"), ToStr);
		const FString FieldPath = FString::Printf(TEXT("disconnect[%d].to"), Idx);

		if (ToStr.IsEmpty())
		{
			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), TEXT("disconnect"));
			Skip->SetStringField(TEXT("field"), FieldPath);
			Skip->SetStringField(TEXT("reason"), TEXT("missing 'to'"));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));
			R.Errors.Add(MakePolicyDiagnostic(TEXT("MISSING_REQUIRED_FIELD"), TEXT("disconnect"), FieldPath, TEXT("missing"), { TEXT("to") }));
			continue;
		}

		// Pin-syntax alias: accept `Ref:Pin` and normalize to canonical `Ref.Pin`
		// per policy rule 6, with an ALIAS_NORMALIZED warning.
		if (ToStr.Contains(TEXT(":")))
		{
			const FString Normalized = ToStr.Replace(TEXT(":"), TEXT("."));
			R.Warnings.Add(MakePolicyDiagnostic(
				TEXT("ALIAS_NORMALIZED"),
				TEXT("disconnect"),
				FieldPath,
				ToStr,
				{ Normalized },
				TEXT("Pin syntax uses dot (Ref.Pin), not colon (Ref:Pin). Normalized.")
			));
			ToStr = Normalized;
		}

		int32 ToDot;
		FString ToRef = ToStr, ToInput;
		if (ToStr.FindChar('.', ToDot)) { ToRef = ToStr.Left(ToDot); ToInput = ToStr.Mid(ToDot + 1); }

		TWeakObjectPtr<UMaterialExpression>* ToExprPtr = ExpressionRefs.Find(ToRef);
		if (!ToExprPtr || !ToExprPtr->IsValid())
		{
			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), TEXT("disconnect"));
			Skip->SetStringField(TEXT("field"), FieldPath);
			Skip->SetStringField(TEXT("reason"), TEXT("target ref not found"));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));
			R.Errors.Add(MakePolicyDiagnostic(
				TEXT("NODE_REF_NOT_FOUND"),
				TEXT("disconnect"),
				FieldPath,
				ToStr,
				AllowedRefs
			));
			continue;
		}

		UMaterialExpression* ToExpr = ToExprPtr->Get();
		bool bFoundPin = false;
		for (int32 i = 0; ; i++)
		{
			FExpressionInput* Input = ToExpr->GetInput(i);
			if (!Input) break;
			const FString PinName = ToExpr->GetInputName(i).ToString();
			if ((ToInput.IsEmpty() && i == 0) || PinName.Equals(ToInput, ESearchCase::IgnoreCase))
			{
				Input->Expression = nullptr;
				Disconnected++;
				bFoundPin = true;

				TSharedRef<FJsonObject> Mod = MakeShareable(new FJsonObject());
				Mod->SetStringField(TEXT("op"), TEXT("disconnect"));
				Mod->SetStringField(TEXT("ref"), ToRef);
				Mod->SetStringField(TEXT("pin"), PinName.IsEmpty() ? FString::FromInt(i) : PinName);
				Mod->SetStringField(TEXT("assetPath"), FuncPath);
				R.Modified.Add(MakeShareable(new FJsonValueObject(Mod)));
				break;
			}
		}

		if (!bFoundPin)
		{
			const TArray<FString> AllowedPins = GetExpressionInputNames(ToExpr);
			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), TEXT("disconnect"));
			Skip->SetStringField(TEXT("field"), FieldPath);
			Skip->SetStringField(TEXT("reason"), TEXT("input pin not found on target"));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));
			R.Errors.Add(MakePolicyDiagnostic(
				TEXT("TARGET_PIN_NOT_FOUND"),
				TEXT("disconnect"),
				FieldPath,
				ToStr,
				AllowedPins
			));
		}
	}

	R.Message = FString::Printf(TEXT("Disconnected %d pin(s)"), Disconnected);
	return R;
}

// ============================================================
// DO SET METADATA ON FUNCTION
// ============================================================
// Per G6: validate all metadata fields atomically before applying any. On any
// invalid-type field, fail the whole metadata op without partial application.
// On success emit one Modified row per field actually written.
FNwiroIKMatResult FNwiroIKMaterialTools::DoSetMetadataOnFunction(UMaterialFunction* Func, const TSharedPtr<FJsonObject>& Meta)
{
	FNwiroIKMatResult R;
	R.bSuccess = true;

	if (!Meta.IsValid())
	{
		R.Message = TEXT("Metadata payload missing");
		return R;
	}

	const FString FuncPath = Func->GetPathName();

	// Phase 1: type-validate every requested field. Do not mutate anything yet.
	auto FieldMissingOrType = [&](const FString& Key, EJson Expected, const TCHAR* TypeName)
	{
		if (!Meta->HasField(Key)) return;
		if (!JsonFieldIsType(Meta, Key, Expected))
		{
			const FString FieldPath = FString::Printf(TEXT("setMetadata.%s"), *Key);
			TSharedRef<FJsonObject> Skip = MakeShareable(new FJsonObject());
			Skip->SetStringField(TEXT("op"), TEXT("setMetadata"));
			Skip->SetStringField(TEXT("field"), FieldPath);
			Skip->SetStringField(TEXT("reason"), FString::Printf(TEXT("expected %s"), TypeName));
			R.Skipped.Add(MakeShareable(new FJsonValueObject(Skip)));
			R.Errors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("setMetadata"), FieldPath, FString::Printf(TEXT("non-%s"), TypeName), { TypeName }));
		}
	};

	FieldMissingOrType(TEXT("exposeToLibrary"), EJson::Boolean, TEXT("boolean"));
	FieldMissingOrType(TEXT("description"),     EJson::String,  TEXT("string"));
	FieldMissingOrType(TEXT("category"),        EJson::String,  TEXT("string"));

	if (R.Errors.Num() > 0)
	{
		// Atomic: refuse to apply any field if any field is type-invalid.
		R.Message = TEXT("Metadata validation failed; no fields applied");
		return R;
	}

	// Phase 2: apply.
	if (Meta->HasField(TEXT("exposeToLibrary")))
	{
		bool bExpose = false;
		Meta->TryGetBoolField(TEXT("exposeToLibrary"), bExpose);
		Func->bExposeToLibrary = bExpose;

		TSharedRef<FJsonObject> Mod = MakeShareable(new FJsonObject());
		Mod->SetStringField(TEXT("op"), TEXT("setMetadata"));
		Mod->SetStringField(TEXT("field"), TEXT("exposeToLibrary"));
		Mod->SetBoolField(TEXT("value"), bExpose);
		Mod->SetStringField(TEXT("assetPath"), FuncPath);
		R.Modified.Add(MakeShareable(new FJsonValueObject(Mod)));
	}

	if (Meta->HasField(TEXT("description")))
	{
		FString Desc;
		Meta->TryGetStringField(TEXT("description"), Desc);
		Func->Description = Desc;

		TSharedRef<FJsonObject> Mod = MakeShareable(new FJsonObject());
		Mod->SetStringField(TEXT("op"), TEXT("setMetadata"));
		Mod->SetStringField(TEXT("field"), TEXT("description"));
		Mod->SetStringField(TEXT("assetPath"), FuncPath);
		R.Modified.Add(MakeShareable(new FJsonValueObject(Mod)));
	}

	if (Meta->HasField(TEXT("category")))
	{
		FString Cat;
		Meta->TryGetStringField(TEXT("category"), Cat);
		if (!Cat.IsEmpty())
		{
			Func->LibraryCategoriesText.Empty();
			TArray<FString> Parts;
			Cat.ParseIntoArray(Parts, TEXT("/"));
			for (const FString& Part : Parts)
				Func->LibraryCategoriesText.Add(FText::FromString(Part));
		}

		TSharedRef<FJsonObject> Mod = MakeShareable(new FJsonObject());
		Mod->SetStringField(TEXT("op"), TEXT("setMetadata"));
		Mod->SetStringField(TEXT("field"), TEXT("category"));
		Mod->SetStringField(TEXT("value"), Cat);
		Mod->SetStringField(TEXT("assetPath"), FuncPath);
		R.Modified.Add(MakeShareable(new FJsonValueObject(Mod)));
	}

	R.Message = TEXT("Metadata updated");
	return R;
}

// ============================================================
// DO CLEAR FUNCTION GRAPH
// ============================================================
// Always emits one Modified row stamped with the count of expressions that were
// cleared. Count may be 0 when the graph was already empty — that is a uniform
// success, not a Skipped item (C3: skipped is for actionable failures).
FNwiroIKMatResult FNwiroIKMaterialTools::DoClearFunctionGraph(UMaterialFunction* Func)
{
	FNwiroIKMatResult R;
	R.bSuccess = true;

	const FString FuncPath = Func->GetPathName();
	const int32 Count = Func->GetExpressionCollection().Expressions.Num();

	if (Count > 0)
	{
		for (TObjectPtr<UMaterialExpression>& ExprPtr : Func->GetExpressionCollection().Expressions)
		{
			if (ExprPtr) ExprPtr->MarkAsGarbage();
		}
		Func->GetExpressionCollection().Expressions.Empty();
	}

	TSharedRef<FJsonObject> Mod = MakeShareable(new FJsonObject());
	Mod->SetStringField(TEXT("op"), TEXT("clearGraph"));
	Mod->SetNumberField(TEXT("count"), Count);
	Mod->SetStringField(TEXT("assetPath"), FuncPath);
	R.Modified.Add(MakeShareable(new FJsonValueObject(Mod)));

	R.Message = (Count > 0)
		? FString::Printf(TEXT("Cleared %d expression(s) from graph"), Count)
		: TEXT("Graph was already empty");
	return R;
}

// G11: typed primitive validation per nested operation. For each item in `Op`'s
// array, if a key in `KeyTypes` is present, its value's EJson type must match.
// Missing keys are NOT flagged here (the existing required-any/required-all
// validator handles those). Variant-type keys (properties, default, value) are
// not in `KeyTypes` so they bypass type checking.
static void ValidateJsonArrayItemTypes(
	const TSharedPtr<FJsonObject>& Cmd,
	const FString& Op,
	const TMap<FString, EJson>& KeyTypes,
	TArray<TSharedPtr<FJsonValue>>& Errors)
{
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Cmd->TryGetArrayField(Op, Arr) || !Arr) return;

	auto TypeName = [](EJson T)
	{
		switch (T)
		{
			case EJson::String:  return TEXT("string");
			case EJson::Number:  return TEXT("number");
			case EJson::Boolean: return TEXT("boolean");
			case EJson::Object:  return TEXT("object");
			case EJson::Array:   return TEXT("array");
			default:             return TEXT("?");
		}
	};

	for (int32 Index = 0; Index < Arr->Num(); ++Index)
	{
		if (!(*Arr)[Index].IsValid() || (*Arr)[Index]->Type != EJson::Object) continue;
		const TSharedPtr<FJsonObject>& ItemObj = (*Arr)[Index]->AsObject();
		for (const auto& KT : KeyTypes)
		{
			// TryGetField avoids the FString/FSharedString key-type mismatch in UE 5.8
			// while remaining compatible with UE 5.7.
			TSharedPtr<FJsonValue> Found = ItemObj->TryGetField(KT.Key);
			if (!Found.IsValid()) continue;
			if (Found->Type != KT.Value)
			{
				const FString FieldPath = FString::Printf(TEXT("%s[%d].%s"), *Op, Index, *KT.Key);
				const TCHAR* TName = TypeName(KT.Value);
				Errors.Add(MakePolicyDiagnostic(
					TEXT("INVALID_TYPE"),
					Op,
					FieldPath,
					FString::Printf(TEXT("non-%s"), TName),
					{ TName }
				));
			}
		}
	}
}

// ============================================================
// EDIT MATERIAL FUNCTION (Policy 1.1)
// ============================================================
FString FNwiroIKMaterialTools::EditMaterialFunction(const FString& JsonCommand)
{
	// ============================================================
	// PHASE 1: Parse + top-level + nested schema validation (rule 14)
	// ============================================================
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("$"), TEXT("invalid-json"), { TEXT("object") }));
		return MakePolicyFailureResponse(TEXT(""), TEXT("Invalid JSON."), Errors);
	}

	const FString CallId = MakeCallId(Cmd);

	// G4: `path` is NOT in the allowed top-level keys. The MCP schema already
	// rejects it via additionalProperties:false; the wrapper does not accept it
	// as an alias either, matching the description's "Aliases NOT accepted" line.
	static const TSet<FString> AllowedTopLevel = {
		TEXT("_callId"), TEXT("assetPath"), TEXT("addInputs"), TEXT("addOutputs"),
		TEXT("addExpressions"), TEXT("connect"), TEXT("removeInputs"), TEXT("removeOutputs"),
		TEXT("deleteExpressions"), TEXT("disconnect"), TEXT("setMetadata"), TEXT("clearGraph")
	};
	static const TArray<FString> ArrayFields = {
		TEXT("addInputs"), TEXT("addOutputs"), TEXT("addExpressions"), TEXT("connect"),
		TEXT("removeInputs"), TEXT("removeOutputs"), TEXT("deleteExpressions"), TEXT("disconnect")
	};

	TArray<FString> AllowedTopLevelList = AllowedTopLevel.Array();
	AllowedTopLevelList.Sort();
	TArray<TSharedPtr<FJsonValue>> ValidationErrors;
	TArray<TSharedPtr<FJsonValue>> ValidationWarnings;

	for (const auto& Pair : Cmd->Values)
	{
		const FString Key(*Pair.Key);
		if (!AllowedTopLevel.Contains(Key))
		{
			ValidationErrors.Add(MakePolicyDiagnostic(TEXT("UNKNOWN_KEY"), TEXT("schema"), Key, Key, AllowedTopLevelList));
		}
	}

	// Framework-compatibility normalization (narrow exception per policy rule 6):
	// some agent frameworks stringify JSON arrays on the wire before they reach
	// the tool. When an array field arrives as a JSON-encoded string that parses
	// to an array, accept it, parse it in place, and emit ALIAS_NORMALIZED naming
	// the canonical type. The legacy edit_material_function schema already has
	// array-or-string compatibility for the same fields; we mirror it narrowly
	// here. Subsequent nested validation runs on the normalized array exactly
	// as if it had arrived canonical. Booleans/numbers/objects are NOT extended
	// this way — only the 8 array fields below.
	//
	// Fields whose string contents failed JSON-array parsing are tracked so the
	// later generic array-type check doesn't re-emit a redundant "non-array"
	// diagnostic for the same root cause.
	TSet<FString> ArrayParseFailedFields;
	auto NormalizeArrayField = [&](const FString& Key)
	{
		if (!Cmd->HasField(Key)) return;
		// TryGetField avoids the FString/FSharedString key-type mismatch in UE 5.8
		// while remaining compatible with UE 5.7.
		TSharedPtr<FJsonValue> Val = Cmd->TryGetField(Key);
		if (!Val.IsValid()) return;
		if (Val->Type == EJson::Array) return;     // canonical, nothing to do
		if (Val->Type != EJson::String) return;    // wrong type — leave for the standard INVALID_TYPE check

		const FString StringForm = Val->AsString();
		TArray<TSharedPtr<FJsonValue>> Parsed;
		TSharedRef<TJsonReader<>> ArrReader = TJsonReaderFactory<>::Create(StringForm);
		if (!FJsonSerializer::Deserialize(ArrReader, Parsed))
		{
			ValidationErrors.Add(MakePolicyDiagnostic(
				TEXT("INVALID_TYPE"),
				Key,
				Key,
				TEXT("string-not-parseable-as-array"),
				{ TEXT("array") }
			));
			ArrayParseFailedFields.Add(Key);
			return;
		}
		// Successfully parsed — replace in-place so downstream validation sees a real array.
		Cmd->SetArrayField(Key, Parsed);
		ValidationWarnings.Add(MakePolicyDiagnostic(
			TEXT("ALIAS_NORMALIZED"),
			Key,
			Key,
			TEXT("JSON-encoded string"),
			{ TEXT("array") },
			TEXT("Array field was received as a JSON-encoded string and normalized. Prefer sending a real JSON array.")
		));
	};
	for (const FString& ArrayField : ArrayFields)
	{
		NormalizeArrayField(ArrayField);
	}

	FString AssetPath;
	Cmd->TryGetStringField(TEXT("assetPath"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("MISSING_REQUIRED_FIELD"), TEXT("schema"), TEXT("assetPath"), TEXT("missing"), { TEXT("assetPath") }));
	}
	if (Cmd->HasField(TEXT("assetPath")) && !JsonFieldIsType(Cmd, TEXT("assetPath"), EJson::String))
	{
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("assetPath"), TEXT("non-string"), { TEXT("string") }));
	}
	if (Cmd->HasField(TEXT("_callId")) && !JsonFieldIsType(Cmd, TEXT("_callId"), EJson::String))
	{
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("_callId"), TEXT("non-string"), { TEXT("string") }));
	}
	// G7: per-sub-op `op` instead of generic "schema" for typed-property errors
	// on a specific sub-operation field.
	if (Cmd->HasField(TEXT("clearGraph")) && !JsonFieldIsType(Cmd, TEXT("clearGraph"), EJson::Boolean))
	{
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("clearGraph"), TEXT("clearGraph"), TEXT("non-boolean"), { TEXT("boolean") }));
	}
	if (Cmd->HasField(TEXT("setMetadata")) && !JsonFieldIsType(Cmd, TEXT("setMetadata"), EJson::Object))
	{
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("setMetadata"), TEXT("setMetadata"), TEXT("non-object"), { TEXT("object") }));
	}
	for (const FString& ArrayField : ArrayFields)
	{
		// Skip fields that NormalizeArrayField already diagnosed as
		// "string-not-parseable-as-array" — avoid emitting a redundant
		// "non-array" diagnostic for the same root cause.
		if (ArrayParseFailedFields.Contains(ArrayField)) continue;
		if (Cmd->HasField(ArrayField) && !JsonFieldIsType(Cmd, ArrayField, EJson::Array))
		{
			ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), ArrayField, ArrayField, TEXT("non-array"), { TEXT("array") }));
		}
	}

	// Nested key allowlists (rule 4A)
	const TSet<FString> AddInputKeys = {
		TEXT("name"), TEXT("type"), TEXT("sortPriority"), TEXT("description"),
		TEXT("x"), TEXT("y"), TEXT("ref"),
		TEXT("previewValue"), TEXT("useAsDefault"), TEXT("defaultValue")
	};
	const TSet<FString> AddOutputKeys = { TEXT("name"), TEXT("sortPriority"), TEXT("description"), TEXT("x"), TEXT("y"), TEXT("ref") };
	const TSet<FString> AddExpressionKeys = {
		TEXT("ref"), TEXT("name"), TEXT("type"), TEXT("class"), TEXT("x"), TEXT("y"), TEXT("properties"),
		TEXT("materialFunction"),
		TEXT("default"), TEXT("defaultValue"), TEXT("texture"), TEXT("value"), TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a"), TEXT("exponent"), TEXT("exponentIn")
	};
	const TSet<FString> ConnectKeys = { TEXT("from"), TEXT("to") };
	const TSet<FString> DisconnectKeys = { TEXT("to") };
	const TSet<FString> MetadataKeys = { TEXT("description"), TEXT("category"), TEXT("exposeToLibrary") };

	ValidateJsonArrayObjects(Cmd, TEXT("addInputs"), AddInputKeys, { TEXT("name"), TEXT("ref") }, { TEXT("type") }, ValidationErrors);
	ValidateJsonArrayObjects(Cmd, TEXT("addOutputs"), AddOutputKeys, { TEXT("name"), TEXT("ref") }, {}, ValidationErrors);
	ValidateJsonArrayObjects(Cmd, TEXT("addExpressions"), AddExpressionKeys, { TEXT("name"), TEXT("ref") }, { TEXT("type") }, ValidationErrors);
	ValidateJsonArrayObjects(Cmd, TEXT("connect"), ConnectKeys, {}, { TEXT("from"), TEXT("to") }, ValidationErrors);
	ValidateJsonArrayObjects(Cmd, TEXT("disconnect"), DisconnectKeys, {}, { TEXT("to") }, ValidationErrors);

	// G11: typed primitive validation for nested array items. Variant-type keys
	// (properties, default, value, defaultValue, texture) are excluded so the
	// existing flexibility is preserved.
	const TMap<FString, EJson> AddInputItemTypes = {
		{ TEXT("name"), EJson::String }, { TEXT("ref"), EJson::String },
		{ TEXT("type"), EJson::String }, { TEXT("description"), EJson::String },
		{ TEXT("sortPriority"), EJson::Number }, { TEXT("x"), EJson::Number }, { TEXT("y"), EJson::Number },
		{ TEXT("useAsDefault"), EJson::Boolean }
		// `previewValue` and `defaultValue` are intentionally untyped at the
		// validation layer — their accepted shape (number / array / boolean)
		// depends on the input's declared `type` and is enforced by the writer
		// with INVALID_TYPE / INVALID_VALUE diagnostics.
	};
	const TMap<FString, EJson> AddOutputItemTypes = {
		{ TEXT("name"), EJson::String }, { TEXT("ref"), EJson::String },
		{ TEXT("description"), EJson::String },
		{ TEXT("sortPriority"), EJson::Number }, { TEXT("x"), EJson::Number }, { TEXT("y"), EJson::Number }
	};
	const TMap<FString, EJson> AddExprItemTypes = {
		{ TEXT("name"), EJson::String }, { TEXT("ref"), EJson::String },
		{ TEXT("type"), EJson::String }, { TEXT("class"), EJson::String },
		{ TEXT("materialFunction"), EJson::String }, { TEXT("texture"), EJson::String },
		{ TEXT("properties"), EJson::Object },
		{ TEXT("x"), EJson::Number }, { TEXT("y"), EJson::Number },
		{ TEXT("r"), EJson::Number }, { TEXT("g"), EJson::Number }, { TEXT("b"), EJson::Number }, { TEXT("a"), EJson::Number },
		{ TEXT("exponent"), EJson::Number }, { TEXT("exponentIn"), EJson::Number }
		// `default`, `defaultValue`, `value` are intentionally untyped (variant).
	};
	const TMap<FString, EJson> ConnectItemTypes = {
		{ TEXT("from"), EJson::String }, { TEXT("to"), EJson::String }
	};
	const TMap<FString, EJson> DisconnectItemTypes = {
		{ TEXT("to"), EJson::String }
	};
	ValidateJsonArrayItemTypes(Cmd, TEXT("addInputs"),     AddInputItemTypes, ValidationErrors);
	ValidateJsonArrayItemTypes(Cmd, TEXT("addOutputs"),    AddOutputItemTypes, ValidationErrors);
	ValidateJsonArrayItemTypes(Cmd, TEXT("addExpressions"), AddExprItemTypes, ValidationErrors);
	ValidateJsonArrayItemTypes(Cmd, TEXT("connect"),       ConnectItemTypes, ValidationErrors);
	ValidateJsonArrayItemTypes(Cmd, TEXT("disconnect"),    DisconnectItemTypes, ValidationErrors);

	// String-array element checks for the simpler ops
	const TArray<TSharedPtr<FJsonValue>>* StringArr = nullptr;
	for (const FString& Op : { FString(TEXT("removeInputs")), FString(TEXT("removeOutputs")), FString(TEXT("deleteExpressions")) })
	{
		if (Cmd->TryGetArrayField(Op, StringArr) && StringArr)
		{
			for (int32 Index = 0; Index < StringArr->Num(); ++Index)
			{
				if (!(*StringArr)[Index].IsValid() || (*StringArr)[Index]->Type != EJson::String)
				{
					const FString Field = FString::Printf(TEXT("%s[%d]"), *Op, Index);
					ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), Op, Field, TEXT("non-string"), { TEXT("string") }));
				}
			}
		}
	}

	// setMetadata key allowlist + per-key type validation (G11). Type checks
	// happen here rather than inside DoSetMetadataOnFunction so the failure
	// happens pre-mutation (rule 14), consistent with the rest of Phase 1.
	const TSharedPtr<FJsonObject>* MetaObj = nullptr;
	if (Cmd->TryGetObjectField(TEXT("setMetadata"), MetaObj) && MetaObj)
	{
		TArray<FString> MetadataAllowed = MetadataKeys.Array();
		MetadataAllowed.Sort();
		for (const auto& Pair : (*MetaObj)->Values)
		{
			const FString Key(*Pair.Key);
			if (!MetadataKeys.Contains(Key))
			{
				ValidationErrors.Add(MakePolicyDiagnostic(TEXT("UNKNOWN_KEY"), TEXT("setMetadata"), FString::Printf(TEXT("setMetadata.%s"), *Key), Key, MetadataAllowed));
			}
		}
		if ((*MetaObj)->HasField(TEXT("exposeToLibrary")) && !JsonFieldIsType(*MetaObj, TEXT("exposeToLibrary"), EJson::Boolean))
		{
			ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("setMetadata"), TEXT("setMetadata.exposeToLibrary"), TEXT("non-boolean"), { TEXT("boolean") }));
		}
		if ((*MetaObj)->HasField(TEXT("description")) && !JsonFieldIsType(*MetaObj, TEXT("description"), EJson::String))
		{
			ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("setMetadata"), TEXT("setMetadata.description"), TEXT("non-string"), { TEXT("string") }));
		}
		if ((*MetaObj)->HasField(TEXT("category")) && !JsonFieldIsType(*MetaObj, TEXT("category"), EJson::String))
		{
			ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("setMetadata"), TEXT("setMetadata.category"), TEXT("non-string"), { TEXT("string") }));
		}
	}

	// Validate-before-mutate (rule 14): bail out if any schema issue.
	// ValidationWarnings (e.g. normalization warnings from NormalizeArrayField)
	// are forwarded into the failure response so the AI sees BOTH facts —
	// "your string array was normalized" AND "its contents were invalid".
	if (ValidationErrors.Num() > 0)
	{
		return MakePolicyFailureResponse(CallId, TEXT("Schema validation failed."), ValidationErrors, ValidationWarnings);
	}

	// ============================================================
	// PHASE 2: Load asset
	// ============================================================
	UMaterialFunction* Func = LoadMaterialFunction(AssetPath);
	if (!Func)
	{
		TArray<TSharedPtr<FJsonValue>> NotFoundErrors;
		NotFoundErrors.Add(MakePolicyDiagnostic(
			TEXT("NOT_FOUND"),
			TEXT("schema"),
			TEXT("assetPath"),
			AssetPath,
			{}
		));
		return MakePolicyFailureResponse(CallId, FString::Printf(TEXT("Material function not found: %s"), *AssetPath), NotFoundErrors, ValidationWarnings);
	}

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "EditMaterialFunction", "AI: Edit Material Function"), Func);

	ClearExpressionRefs();
	Func->Modify();

	// ============================================================
	// PHASE 3: Run operations through Do* helpers (G15)
	// ============================================================
	TArray<TSharedPtr<FJsonValue>> Created;
	TArray<TSharedPtr<FJsonValue>> Modified;
	TArray<TSharedPtr<FJsonValue>> Skipped;
	// Seed with Phase 1 normalization warnings so the response carries them
	// even on a successful run (e.g. string-encoded array → ALIAS_NORMALIZED).
	TArray<TSharedPtr<FJsonValue>> Warnings = ValidationWarnings;
	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<FString> Messages;

	auto Absorb = [&](const FString& Key, const FNwiroIKMatResult& R)
	{
		Messages.Add(FString::Printf(TEXT("[%s] %s"), *Key, *R.Message));
		Created.Append(R.Created);
		Modified.Append(R.Modified);
		Skipped.Append(R.Skipped);
		Warnings.Append(R.Warnings);
		Errors.Append(R.Errors);
	};

	// clearGraph (must run before pre-populate; see the original inline block)
	bool bClearGraph = false;
	Cmd->TryGetBoolField(TEXT("clearGraph"), bClearGraph);
	if (bClearGraph)
	{
		Absorb(TEXT("clearGraph"), DoClearFunctionGraph(Func));
	}
	else
	{
		// Pre-populate ExpressionRefs from existing graph so subsequent ops can
		// reference current expressions by their UE name or input/output name.
		for (const TObjectPtr<UMaterialExpression>& ExprPtr : Func->GetExpressionCollection().Expressions)
		{
			if (!ExprPtr) continue;
			UMaterialExpression* Expr = ExprPtr.Get();
			FString UEName = Expr->GetName();
			if (!UEName.IsEmpty()) ExpressionRefs.Add(UEName, Expr);
			if (UMaterialExpressionFunctionInput* FI = Cast<UMaterialExpressionFunctionInput>(Expr))
			{
				FString InputName = FI->InputName.ToString();
				if (!InputName.IsEmpty()) ExpressionRefs.Add(InputName, Expr);
			}
			else if (UMaterialExpressionFunctionOutput* FO = Cast<UMaterialExpressionFunctionOutput>(Expr))
			{
				FString OutputName = FO->OutputName.ToString();
				if (!OutputName.IsEmpty()) ExpressionRefs.Add(OutputName, Expr);
			}
		}
	}

	// addInputs — inject FunctionInput class, preserve original type as inputType
	const TArray<TSharedPtr<FJsonValue>>* AddInputsArr;
	if (Cmd->TryGetArrayField(TEXT("addInputs"), AddInputsArr) && AddInputsArr && AddInputsArr->Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> InputItems;
		for (const TSharedPtr<FJsonValue>& Val : *AddInputsArr)
		{
			const TSharedPtr<FJsonObject>* SrcObj;
			if (!Val->TryGetObject(SrcObj)) continue;
			TSharedRef<FJsonObject> Item = MakeShareable(new FJsonObject());
			for (auto& Pair : (*SrcObj)->Values) Item->SetField(Pair.Key, Pair.Value);
			FString OrigType;
			if ((*SrcObj)->TryGetStringField(TEXT("type"), OrigType))
				Item->SetStringField(TEXT("inputType"), OrigType);
			Item->SetStringField(TEXT("type"), TEXT("FunctionInput"));
			InputItems.Add(MakeShareable(new FJsonValueObject(Item)));
		}
		Absorb(TEXT("addInputs"), DoAddExpressionsToFunction(Func, InputItems, TEXT("input")));
	}

	// addOutputs — inject FunctionOutput class
	const TArray<TSharedPtr<FJsonValue>>* AddOutputsArr;
	if (Cmd->TryGetArrayField(TEXT("addOutputs"), AddOutputsArr) && AddOutputsArr && AddOutputsArr->Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> OutputItems;
		for (const TSharedPtr<FJsonValue>& Val : *AddOutputsArr)
		{
			const TSharedPtr<FJsonObject>* SrcObj;
			if (!Val->TryGetObject(SrcObj)) continue;
			TSharedRef<FJsonObject> Item = MakeShareable(new FJsonObject());
			for (auto& Pair : (*SrcObj)->Values) Item->SetField(Pair.Key, Pair.Value);
			Item->SetStringField(TEXT("type"), TEXT("FunctionOutput"));
			OutputItems.Add(MakeShareable(new FJsonValueObject(Item)));
		}
		Absorb(TEXT("addOutputs"), DoAddExpressionsToFunction(Func, OutputItems, TEXT("output")));
	}

	auto RunArrayOp = [&](const FString& Key, TFunction<FNwiroIKMatResult(const TArray<TSharedPtr<FJsonValue>>&)> OpFunc)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (Cmd->TryGetArrayField(Key, Arr) && Arr && Arr->Num() > 0)
		{
			Absorb(Key, OpFunc(*Arr));
		}
	};
	RunArrayOp(TEXT("addExpressions"),    [&](const auto& A) { return DoAddExpressionsToFunction(Func, A); });
	RunArrayOp(TEXT("connect"),           [&](const auto& A) { return DoConnectExpressionsInFunction(Func, A); });
	RunArrayOp(TEXT("deleteExpressions"), [&](const auto& A) { return DoRemoveExpressionsFromFunction(Func, A); });
	RunArrayOp(TEXT("removeInputs"),      [&](const auto& A) { return DoRemoveInputsFromFunction(Func, A); });
	RunArrayOp(TEXT("removeOutputs"),     [&](const auto& A) { return DoRemoveOutputsFromFunction(Func, A); });
	RunArrayOp(TEXT("disconnect"),        [&](const auto& A) { return DoDisconnectInFunction(Func, A); });

	if (Cmd->TryGetObjectField(TEXT("setMetadata"), MetaObj))
	{
		Absorb(TEXT("setMetadata"), DoSetMetadataOnFunction(Func, *MetaObj));
	}

	// Refresh after structural mutation (rule 15).
	// PreEditChange/PostEditChange retained for property change notification (orthogonal to undo registration via Modify()).
	Func->PreEditChange(nullptr);
	Func->PostEditChange();
	Func->MarkPackageDirty();
	UMaterialEditingLibrary::UpdateMaterialFunction(Func, nullptr);

	// ============================================================
	// PHASE 4: Build response envelope (rule 16, 16A)
	// ============================================================
	// New-schema success rule (G15): success ⇔ both Errors and Skipped are empty.
	// Any skipped requested-mutation or any structured error flips success:false,
	// matching policy rule 12 (partial success is visible).
	const bool bSuccess = (Errors.Num() == 0 && Skipped.Num() == 0);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetStringField(TEXT("policy_version"), TEXT("1.1"));
	Result->SetStringField(TEXT("_callId"), CallId);
	Result->SetStringField(TEXT("assetPath"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> MsgArr;
	for (const FString& M : Messages) MsgArr.Add(MakeShareable(new FJsonValueString(M)));
	Result->SetArrayField(TEXT("messages"), MsgArr);
	Result->SetArrayField(TEXT("created"),  Created);
	Result->SetArrayField(TEXT("modified"), Modified);
	Result->SetArrayField(TEXT("skipped"),  Skipped);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetArrayField(TEXT("errors"),   Errors);

	// refs map (back-compat convenience for callers that use it)
	if (Created.Num() > 0)
	{
		TSharedRef<FJsonObject> RefMap = MakeShareable(new FJsonObject());
		for (const TSharedPtr<FJsonValue>& V : Created)
		{
			if (!V.IsValid() || V->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject> O = V->AsObject();
			FString UserName;
			if (!O->TryGetStringField(TEXT("userName"), UserName))
				O->TryGetStringField(TEXT("userRef"), UserName);
			FString AssignedRef;
			O->TryGetStringField(TEXT("assignedRef"), AssignedRef);
			if (!UserName.IsEmpty() && !AssignedRef.IsEmpty())
				RefMap->SetStringField(UserName, AssignedRef);
		}
		Result->SetObjectField(TEXT("refs"), RefMap);
	}

	if (!bSuccess)
	{
		const FString ErrorSummary = (Created.Num() > 0 || Modified.Num() > 0)
			? TEXT("Some operations failed.")
			: TEXT("Material function edit failed.");
		Result->SetStringField(TEXT("error"), ErrorSummary);
	}

	return SerializeJsonObject(Result);
}

// ============================================================
// DELETE MATERIAL FUNCTION (Policy 1.1, M2 destructive)
// ============================================================
// Two-flag design:
//   - `preview` (default true) — controls preview vs execute (M2)
//   - `allowBrokenReferences` (default false) — confirms collateral damage (C1)
// Both default-safe. `preview:false` alone is NOT enough to break referencers;
// `allowBrokenReferences:true` is also required when referencers exist.
//
// QA invariants enforced by the structure of this function:
//   1. Preview branch never calls DeleteAsset / MarkPackageDirty.
//   2. Refusal branch never calls DeleteAsset.
//   3. allowBrokenReferences is consulted BEFORE the delete call, not after.
FString FNwiroIKMaterialTools::DeleteMaterialFunction(const FString& JsonCommand)
{
	// Phase 1: parse + validation
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("$"), TEXT("invalid-json"), { TEXT("object") }));
		return MakePolicyFailureResponse(TEXT(""), TEXT("Invalid JSON."), Errors);
	}

	const FString CallId = MakeCallId(Cmd);

	static const TSet<FString> AllowedTopLevel = {
		TEXT("_callId"), TEXT("assetPath"), TEXT("preview"), TEXT("allowBrokenReferences")
	};
	TArray<FString> AllowedList = AllowedTopLevel.Array();
	AllowedList.Sort();

	TArray<TSharedPtr<FJsonValue>> ValidationErrors;
	TArray<TSharedPtr<FJsonValue>> ValidationWarnings;
	for (const auto& Pair : Cmd->Values)
	{
		const FString Key(*Pair.Key);
		if (!AllowedTopLevel.Contains(Key))
		{
			ValidationErrors.Add(MakePolicyDiagnostic(TEXT("UNKNOWN_KEY"), TEXT("schema"), Key, Key, AllowedList));
		}
	}

	// Narrow framework-compatibility shim: some client tool-call frameworks
	// stringify primitive booleans before transport. Accept the exact JSON
	// strings "true" / "false" on `preview` and `allowBrokenReferences`,
	// normalize to real booleans, and emit a loud ALIAS_NORMALIZED warning.
	// Anything else (e.g. "yes", "1", arbitrary string) falls through to the
	// regular INVALID_TYPE check below. Real booleans are the canonical form.
	auto NormalizeStringBoolField = [&](const FString& Key)
	{
		if (!Cmd->HasField(Key)) return;
		// TryGetField avoids the FString/FSharedString key-type mismatch in UE 5.8
		// while remaining compatible with UE 5.7.
		TSharedPtr<FJsonValue> Val = Cmd->TryGetField(Key);
		if (!Val.IsValid() || Val->Type != EJson::String) return;
		const FString Str = Val->AsString();
		if (Str.Equals(TEXT("true"), ESearchCase::CaseSensitive))
		{
			Cmd->SetBoolField(Key, true);
		}
		else if (Str.Equals(TEXT("false"), ESearchCase::CaseSensitive))
		{
			Cmd->SetBoolField(Key, false);
		}
		else
		{
			return;
		}
		ValidationWarnings.Add(MakePolicyDiagnostic(
			TEXT("ALIAS_NORMALIZED"),
			TEXT("delete"),
			Key,
			TEXT("string boolean"),
			{ TEXT("boolean") },
			TEXT("Boolean field was received as a JSON string and normalized. Prefer sending a real JSON boolean.")
		));
	};
	NormalizeStringBoolField(TEXT("preview"));
	NormalizeStringBoolField(TEXT("allowBrokenReferences"));

	if (Cmd->HasField(TEXT("_callId")) && !JsonFieldIsType(Cmd, TEXT("_callId"), EJson::String))
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("_callId"), TEXT("non-string"), { TEXT("string") }));
	if (Cmd->HasField(TEXT("assetPath")) && !JsonFieldIsType(Cmd, TEXT("assetPath"), EJson::String))
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("assetPath"), TEXT("non-string"), { TEXT("string") }));
	if (Cmd->HasField(TEXT("preview")) && !JsonFieldIsType(Cmd, TEXT("preview"), EJson::Boolean))
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("preview"), TEXT("non-boolean"), { TEXT("boolean") }));
	if (Cmd->HasField(TEXT("allowBrokenReferences")) && !JsonFieldIsType(Cmd, TEXT("allowBrokenReferences"), EJson::Boolean))
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), TEXT("schema"), TEXT("allowBrokenReferences"), TEXT("non-boolean"), { TEXT("boolean") }));

	FString AssetPath;
	Cmd->TryGetStringField(TEXT("assetPath"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		ValidationErrors.Add(MakePolicyDiagnostic(TEXT("MISSING_REQUIRED_FIELD"), TEXT("schema"), TEXT("assetPath"), TEXT("missing"), { TEXT("assetPath") }));
	}

	if (ValidationErrors.Num() > 0)
	{
		return MakePolicyFailureResponse(CallId, TEXT("Schema validation failed."), ValidationErrors, ValidationWarnings);
	}

	// Phase 2: parse + validate assetPath BEFORE any registry/delete work.
	// `/Game/Foo/MF_A.MF_B` (mismatched object suffix) is rejected here — for a
	// destructive tool, silently retargeting to `MF_A` would be unacceptable.
	FString PackageName = AssetPath;
	FString SuffixAfterDot;
	{
		int32 DotIdx;
		if (PackageName.FindChar('.', DotIdx))
		{
			SuffixAfterDot = PackageName.Mid(DotIdx + 1);
			PackageName = PackageName.Left(DotIdx);
		}
	}

	FString PackageBasename;
	{
		int32 SlashIdx;
		if (!PackageName.FindLastChar('/', SlashIdx))
		{
			TArray<TSharedPtr<FJsonValue>> Errors;
			Errors.Add(MakePolicyDiagnostic(
				TEXT("INVALID_VALUE"),
				TEXT("schema"),
				TEXT("assetPath"),
				AssetPath,
				{},
				TEXT("Malformed assetPath: missing package path separator")
			));
			return MakePolicyFailureResponse(CallId, FString::Printf(TEXT("Malformed assetPath: %s"), *AssetPath), Errors, ValidationWarnings);
		}
		PackageBasename = PackageName.Mid(SlashIdx + 1);
	}

	if (!SuffixAfterDot.IsEmpty() && !SuffixAfterDot.Equals(PackageBasename, ESearchCase::CaseSensitive))
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakePolicyDiagnostic(
			TEXT("INVALID_VALUE"),
			TEXT("schema"),
			TEXT("assetPath"),
			AssetPath,
			{},
			FString::Printf(TEXT("Mismatched object suffix: '.%s' does not match basename '%s'"), *SuffixAfterDot, *PackageBasename)
		));
		return MakePolicyFailureResponse(CallId, FString::Printf(TEXT("Malformed assetPath: %s"), *AssetPath), Errors, ValidationWarnings);
	}

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();

	TArray<FAssetData> SelfDatas;
	AR.GetAssetsByPackageName(FName(*PackageName), SelfDatas);
	if (SelfDatas.Num() == 0)
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakePolicyDiagnostic(
			TEXT("NOT_FOUND"),
			TEXT("schema"),
			TEXT("assetPath"),
			AssetPath,
			{}
		));
		return MakePolicyFailureResponse(CallId, FString::Printf(TEXT("Material function not found: %s"), *AssetPath), Errors, ValidationWarnings);
	}

	// Verify the target is actually a UMaterialFunction (legacy tool would happily
	// delete any asset class via this tool — new schema refuses).
	const FString TargetClass = SelfDatas[0].AssetClassPath.GetAssetName().ToString();
	if (!TargetClass.Equals(TEXT("MaterialFunction"), ESearchCase::IgnoreCase))
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakePolicyDiagnostic(
			TEXT("INVALID_VALUE"),
			TEXT("schema"),
			TEXT("assetPath"),
			TargetClass,
			{ TEXT("MaterialFunction") },
			FString::Printf(TEXT("Asset at %s is a %s, not a MaterialFunction"), *AssetPath, *TargetClass)
		));
		return MakePolicyFailureResponse(CallId, FString::Printf(TEXT("Target is not a MaterialFunction: %s"), *AssetPath), Errors, ValidationWarnings);
	}

	// Canonical full-object form, echoed in every response field to keep all path
	// values in the response consistent (no mix of /Game/Foo/MF_Bar vs /Game/Foo/MF_Bar.MF_Bar).
	const FString CanonicalAssetPath = FString::Printf(TEXT("%s.%s"), *PackageName, *SelfDatas[0].AssetName.ToString());

	TArray<FAssetIdentifier> Referencers;
	AR.GetReferencers(FAssetIdentifier(FName(*PackageName)), Referencers);

	auto BuildBreakReferenceRow = [&AR](const FAssetIdentifier& Ref) -> TSharedRef<FJsonObject>
	{
		TSharedRef<FJsonObject> Row = MakeShareable(new FJsonObject());
		Row->SetStringField(TEXT("op"), TEXT("break_reference"));

		TArray<FAssetData> RefDatas;
		AR.GetAssetsByPackageName(Ref.PackageName, RefDatas);
		if (RefDatas.Num() > 0)
		{
			// Canonical object path for the referencer, same form as the deleted asset.
			const FString RefCanonical = FString::Printf(TEXT("%s.%s"), *Ref.PackageName.ToString(), *RefDatas[0].AssetName.ToString());
			Row->SetStringField(TEXT("assetPath"), RefCanonical);
			Row->SetStringField(TEXT("type"), RefDatas[0].AssetClassPath.GetAssetName().ToString());
		}
		else
		{
			// Registry data unavailable for the referencer — fall back to package path only.
			Row->SetStringField(TEXT("assetPath"), Ref.PackageName.ToString());
		}
		return Row;
	};

	// Resolve flags (defaults: preview=true, allowBrokenReferences=false).
	// Track whether allowBrokenReferences was *explicitly* sent so the diagnostic
	// can carry the right `received` value ("false" vs "missing").
	bool bPreview = true;
	Cmd->TryGetBoolField(TEXT("preview"), bPreview);
	const bool bHasAllowBrokenReferencesField = Cmd->HasField(TEXT("allowBrokenReferences"));
	bool bAllowBrokenReferences = false;
	Cmd->TryGetBoolField(TEXT("allowBrokenReferences"), bAllowBrokenReferences);

	// ============================================================
	// PREVIEW BRANCH — no mutation, no save (M2 default-safe)
	// ============================================================
	if (bPreview)
	{
		TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
		Result->SetBoolField(TEXT("success"), true);
		Result->SetBoolField(TEXT("preview"), true);
		Result->SetBoolField(TEXT("destructive"), true);
		Result->SetStringField(TEXT("policy_version"), TEXT("1.1"));
		Result->SetStringField(TEXT("_callId"), CallId);
		Result->SetStringField(TEXT("assetPath"), CanonicalAssetPath);

		TArray<TSharedPtr<FJsonValue>> WouldDelete;
		{
			TSharedRef<FJsonObject> Row = MakeShareable(new FJsonObject());
			Row->SetStringField(TEXT("assetPath"), CanonicalAssetPath);
			Row->SetStringField(TEXT("type"), TEXT("MaterialFunction"));
			WouldDelete.Add(MakeShareable(new FJsonValueObject(Row)));
		}
		Result->SetArrayField(TEXT("wouldDelete"), WouldDelete);

		TArray<TSharedPtr<FJsonValue>> WouldModify;
		for (const FAssetIdentifier& Ref : Referencers)
		{
			WouldModify.Add(MakeShareable(new FJsonValueObject(BuildBreakReferenceRow(Ref))));
		}
		Result->SetArrayField(TEXT("wouldModify"), WouldModify);

		TArray<TSharedPtr<FJsonValue>> Messages;
		Messages.Add(MakeShareable(new FJsonValueString(TEXT("Preview only. No mutation was performed."))));
		if (Referencers.Num() > 0)
		{
			Messages.Add(MakeShareable(new FJsonValueString(FString::Printf(
				TEXT("This asset has %d referencer(s). Pass allowBrokenReferences:true together with preview:false to proceed."),
				Referencers.Num()
			))));
		}
		Result->SetArrayField(TEXT("messages"), Messages);
		Result->SetArrayField(TEXT("created"),  MakeEmptyJsonArray());
		Result->SetArrayField(TEXT("modified"), MakeEmptyJsonArray());
		Result->SetArrayField(TEXT("skipped"),  MakeEmptyJsonArray());
		Result->SetArrayField(TEXT("warnings"), ValidationWarnings);  // string-bool normalization warnings (if any)
		Result->SetArrayField(TEXT("errors"),   MakeEmptyJsonArray());

		return SerializeJsonObject(Result);
	}

	// ============================================================
	// REFUSAL BRANCH — preview:false on referenced asset without allowBrokenReferences
	// No mutation. C1 default-fail-loud.
	// ============================================================
	if (Referencers.Num() > 0 && !bAllowBrokenReferences)
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakePolicyDiagnostic(
			TEXT("WOULD_BREAK_REFERENCES"),
			TEXT("delete"),
			TEXT("allowBrokenReferences"),
			bHasAllowBrokenReferencesField ? TEXT("false") : TEXT("missing"),
			{ TEXT("true") },
			FString::Printf(TEXT("Asset has %d referencer(s); allowBrokenReferences:true is required to delete."), Referencers.Num())
		));
		return MakePolicyFailureResponse(CallId, TEXT("Asset has referencers; allowBrokenReferences:true is required to delete."), Errors, ValidationWarnings);
	}

	// ============================================================
	// EXECUTE BRANCH — preview:false + (no referencers OR allowBrokenReferences:true)
	// ============================================================
	const bool bDeleted = UEditorAssetLibrary::DeleteAsset(PackageName);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("policy_version"), TEXT("1.1"));
	Result->SetStringField(TEXT("_callId"), CallId);
	Result->SetStringField(TEXT("assetPath"), CanonicalAssetPath);

	if (!bDeleted)
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakePolicyDiagnostic(
			TEXT("ASSET_SAVE_FAILED"),
			TEXT("delete"),
			TEXT("assetPath"),
			CanonicalAssetPath,
			{}
		));
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to delete: %s"), *CanonicalAssetPath));
		Result->SetArrayField(TEXT("messages"), MakeEmptyJsonArray());
		Result->SetArrayField(TEXT("created"),  MakeEmptyJsonArray());
		Result->SetArrayField(TEXT("modified"), MakeEmptyJsonArray());
		Result->SetArrayField(TEXT("skipped"),  MakeEmptyJsonArray());
		Result->SetArrayField(TEXT("warnings"), ValidationWarnings);  // string-bool normalization warnings (if any)
		Result->SetArrayField(TEXT("errors"),   Errors);
		return SerializeJsonObject(Result);
	}

	// Deletion succeeded.
	Result->SetBoolField(TEXT("success"), true);

	TArray<TSharedPtr<FJsonValue>> Modified;
	{
		// The deletion itself (rule 16: no `deleted[]` array; deletions go in modified[]).
		TSharedRef<FJsonObject> Row = MakeShareable(new FJsonObject());
		Row->SetStringField(TEXT("op"), TEXT("delete"));
		Row->SetStringField(TEXT("assetPath"), CanonicalAssetPath);
		Row->SetStringField(TEXT("type"), TEXT("MaterialFunction"));
		Modified.Add(MakeShareable(new FJsonValueObject(Row)));
	}
	for (const FAssetIdentifier& Ref : Referencers)
	{
		Modified.Add(MakeShareable(new FJsonValueObject(BuildBreakReferenceRow(Ref))));
	}
	Result->SetArrayField(TEXT("modified"), Modified);

	TArray<TSharedPtr<FJsonValue>> Warnings = ValidationWarnings;  // carry string-bool normalization warnings
	if (Referencers.Num() > 0)
	{
		Warnings.Add(MakePolicyDiagnostic(
			TEXT("BROKEN_REFERENCES"),
			TEXT("delete"),
			TEXT("assetPath"),
			CanonicalAssetPath,
			{},
			FString::Printf(TEXT("Deleted asset had %d referencer(s); those references are now broken."), Referencers.Num())
		));
	}
	Result->SetArrayField(TEXT("warnings"), Warnings);

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeShareable(new FJsonValueString(FString::Printf(TEXT("Deleted %s"), *CanonicalAssetPath))));
	Result->SetArrayField(TEXT("messages"), Messages);
	Result->SetArrayField(TEXT("created"), MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("skipped"), MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("errors"),  MakeEmptyJsonArray());

	return SerializeJsonObject(Result);
}
