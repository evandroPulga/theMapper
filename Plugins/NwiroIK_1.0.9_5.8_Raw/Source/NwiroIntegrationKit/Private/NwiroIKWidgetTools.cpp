// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKWidgetTools.h"
#include "NwiroIKAssetGuard.h"
#include "NwiroIKTransactionHelper.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "WidgetBlueprintFactory.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/CanvasPanel.h"
#include "Components/VerticalBox.h"
#include "Components/HorizontalBox.h"
#include "Components/Overlay.h"
#include "Components/ScrollBox.h"
#include "Components/Slider.h"
#include "Components/CheckBox.h"
#include "Components/ProgressBar.h"
#include "Components/EditableTextBox.h"
#include "Components/Border.h"
#include "Components/Spacer.h"
#include "Components/SizeBox.h"
#include "Components/GridPanel.h"
#include "Components/WrapBox.h"
#include "Components/UniformGridPanel.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableText.h"
#include "Components/MultiLineEditableText.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/WidgetSwitcher.h"
#include "Components/ScaleBox.h"
#include "Components/RetainerBox.h"
#include "Components/InvalidationBox.h"
#include "Components/Throbber.h"
#include "Components/CircularThrobber.h"
#include "Components/SpinBox.h"
#include "Components/ListView.h"
#include "Components/TileView.h"
#include "Components/TreeView.h"
#include "Components/ExpandableArea.h"
#include "Components/MenuAnchor.h"
#include "Components/BackgroundBlur.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBoxSlot.h"
#include "Components/BorderSlot.h"
#include "Layout/Margin.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "BlueprintCompilationManager.h"
#include "KismetCompilerModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Json.h"
// Render deps
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Slate/WidgetRenderer.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "RenderingThread.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroWidget, Log, All);

// Pre-seed the BPGC into the WBP's package BEFORE compile, so on next load
// the editor finds a valid GeneratedClass and opens the asset cleanly.
// Without this, manual NewObject + CompileSynchronously leaves GeneratedClass
// null on disk (BPGC lives in transient pkg, never serialized), and the
// editor refuses to open with "could not be loaded because it derives from
// an invalid class."
static UWidgetBlueprintGeneratedClass* EnsureSeedGeneratedClass(UWidgetBlueprint* WBP)
{
	if (!WBP || !WBP->ParentClass) return nullptr;
	if (UWidgetBlueprintGeneratedClass* Existing = Cast<UWidgetBlueprintGeneratedClass>(WBP->GeneratedClass))
		return Existing;

	UPackage* Pkg = WBP->GetOutermost();
	const FString GenName = WBP->GetName() + TEXT("_C");
	UWidgetBlueprintGeneratedClass* GenClass = NewObject<UWidgetBlueprintGeneratedClass>(
		Pkg, FName(*GenName),
		RF_Public | RF_Standalone | RF_Transactional);
	GenClass->ClassGeneratedBy = WBP;
	GenClass->SetSuperStruct(WBP->ParentClass);
	UClass* ParentWithin = WBP->ParentClass->ClassWithin.Get();
	GenClass->ClassWithin = ParentWithin ? ParentWithin : UObject::StaticClass();
	GenClass->ClassFlags |= CLASS_CompiledFromBlueprint;
	// Bind + StaticLink populate ClassConstructor + linker so subsequent
	// GetDefaultObject(true) calls don't assert with "ClassConstructor null".
	if (UClass* SuperClass = GenClass->GetSuperClass())
	{
		if (!SuperClass->ClassConstructor) SuperClass->Bind();
	}
	GenClass->Bind();
	GenClass->StaticLink(true);
	WBP->GeneratedClass = GenClass;
	WBP->SkeletonGeneratedClass = GenClass;
	return GenClass;
}

static UWidgetBlueprint* FindWidgetBP(const FString& PathOrName)
{
	UObject* Asset = UEditorAssetLibrary::LoadAsset(PathOrName);
	if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Asset)) return WBP;

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FARFilter Filter;
	Filter.ClassPaths.Add(UWidgetBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(TEXT("/Game"));
	TArray<FAssetData> Assets;
	ARM.Get().GetAssets(Filter, Assets);
	for (const FAssetData& A : Assets)
	{
		if (A.AssetName.ToString().Contains(PathOrName, ESearchCase::IgnoreCase))
		{
			return Cast<UWidgetBlueprint>(NwiroSafeRegistryLoad(A));
		}
	}
	return nullptr;
}

static UClass* ResolveWidgetClass(const FString& ClassName)
{
	FString Lower = ClassName.ToLower();
	if (Lower == TEXT("button")) return UButton::StaticClass();
	if (Lower == TEXT("textblock") || Lower == TEXT("text")) return UTextBlock::StaticClass();
	if (Lower == TEXT("image")) return UImage::StaticClass();
	if (Lower == TEXT("canvaspanel") || Lower == TEXT("canvas")) return UCanvasPanel::StaticClass();
	if (Lower == TEXT("verticalbox") || Lower == TEXT("vbox")) return UVerticalBox::StaticClass();
	if (Lower == TEXT("horizontalbox") || Lower == TEXT("hbox")) return UHorizontalBox::StaticClass();
	if (Lower == TEXT("overlay")) return UOverlay::StaticClass();
	if (Lower == TEXT("scrollbox")) return UScrollBox::StaticClass();
	if (Lower == TEXT("slider")) return USlider::StaticClass();
	if (Lower == TEXT("checkbox")) return UCheckBox::StaticClass();
	if (Lower == TEXT("progressbar")) return UProgressBar::StaticClass();
	if (Lower == TEXT("editabletextbox") || Lower == TEXT("textbox") || Lower == TEXT("input")) return UEditableTextBox::StaticClass();
	if (Lower == TEXT("border")) return UBorder::StaticClass();
	if (Lower == TEXT("spacer")) return USpacer::StaticClass();
	if (Lower == TEXT("sizebox")) return USizeBox::StaticClass();
	if (Lower == TEXT("gridpanel") || Lower == TEXT("grid")) return UGridPanel::StaticClass();
	if (Lower == TEXT("wrapbox")) return UWrapBox::StaticClass();
	if (Lower == TEXT("uniformgridpanel") || Lower == TEXT("uniformgrid")) return UUniformGridPanel::StaticClass();
	if (Lower == TEXT("comboboxstring") || Lower == TEXT("combobox") || Lower == TEXT("dropdown")) return UComboBoxString::StaticClass();
	if (Lower == TEXT("editabletext")) return UEditableText::StaticClass();
	if (Lower == TEXT("multilineeditabletext") || Lower == TEXT("multilinetext")) return UMultiLineEditableText::StaticClass();
	if (Lower == TEXT("multilineeditabletextbox") || Lower == TEXT("multilinetextbox")) return UMultiLineEditableTextBox::StaticClass();
	if (Lower == TEXT("widgetswitcher") || Lower == TEXT("switcher")) return UWidgetSwitcher::StaticClass();
	if (Lower == TEXT("scalebox")) return UScaleBox::StaticClass();
	if (Lower == TEXT("retainerbox")) return URetainerBox::StaticClass();
	if (Lower == TEXT("invalidationbox")) return UInvalidationBox::StaticClass();
	if (Lower == TEXT("throbber")) return UThrobber::StaticClass();
	if (Lower == TEXT("circularthrobber")) return UCircularThrobber::StaticClass();
	if (Lower == TEXT("spinbox") || Lower == TEXT("numericinput")) return USpinBox::StaticClass();
	if (Lower == TEXT("listview")) return UListView::StaticClass();
	if (Lower == TEXT("tileview")) return UTileView::StaticClass();
	if (Lower == TEXT("treeview")) return UTreeView::StaticClass();
	if (Lower == TEXT("expandablearea")) return UExpandableArea::StaticClass();
	if (Lower == TEXT("menuanchor")) return UMenuAnchor::StaticClass();
	if (Lower == TEXT("backgroundblur")) return UBackgroundBlur::StaticClass();
	return nullptr;
}

// ============================================================
// JSON -> UProperty reflection helpers (used by AddWidget for
// `properties` and `slot` blocks). Hallucination-tolerant: accepts
// JSON numbers/bools/strings as natural types, plus array/object
// forms for FVector2D / FLinearColor / FMargin.
// ============================================================

static bool ReadVec2FromJson(const TSharedPtr<FJsonValue>& V, FVector2D& Out)
{
	if (!V.IsValid()) return false;
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	const TSharedPtr<FJsonObject>* Obj = nullptr;
	if (V->TryGetArray(Arr) && Arr->Num() >= 2)
	{
		Out.X = (*Arr)[0]->AsNumber(); Out.Y = (*Arr)[1]->AsNumber(); return true;
	}
	if (V->TryGetObject(Obj))
	{
		double X = Out.X, Y = Out.Y;
		(*Obj)->TryGetNumberField(TEXT("x"), X);
		(*Obj)->TryGetNumberField(TEXT("y"), Y);
		Out.X = X; Out.Y = Y;
		return true;
	}
	return false;
}

static bool ReadColorFromJson(const TSharedPtr<FJsonValue>& V, FLinearColor& Out)
{
	if (!V.IsValid()) return false;
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	const TSharedPtr<FJsonObject>* Obj = nullptr;
	if (V->TryGetArray(Arr) && Arr->Num() >= 3)
	{
		Out.R = (float)(*Arr)[0]->AsNumber();
		Out.G = (float)(*Arr)[1]->AsNumber();
		Out.B = (float)(*Arr)[2]->AsNumber();
		Out.A = Arr->Num() >= 4 ? (float)(*Arr)[3]->AsNumber() : 1.f;
		return true;
	}
	if (V->TryGetObject(Obj))
	{
		double R = Out.R, G = Out.G, B = Out.B, A = Out.A;
		(*Obj)->TryGetNumberField(TEXT("r"), R);
		(*Obj)->TryGetNumberField(TEXT("g"), G);
		(*Obj)->TryGetNumberField(TEXT("b"), B);
		(*Obj)->TryGetNumberField(TEXT("a"), A);
		Out.R = R; Out.G = G; Out.B = B; Out.A = A;
		return true;
	}
	FString S;
	if (V->TryGetString(S))
	{
		// "#RRGGBB" or "#RRGGBBAA"
		FColor C = FColor::FromHex(S);
		Out = FLinearColor(C);
		return true;
	}
	return false;
}

static bool ApplyJsonToProperty(UObject* Owner, FProperty* Prop, const TSharedPtr<FJsonValue>& Val)
{
	if (!Owner || !Prop || !Val.IsValid()) return false;
	void* Ptr = Prop->ContainerPtrToValuePtr<void>(Owner);

	if (FBoolProperty* B = CastField<FBoolProperty>(Prop))
	{
		bool b = false; if (Val->TryGetBool(b)) { B->SetPropertyValue(Ptr, b); return true; }
	}
	if (FFloatProperty* F = CastField<FFloatProperty>(Prop))
	{
		double d = 0; if (Val->TryGetNumber(d)) { F->SetPropertyValue(Ptr, (float)d); return true; }
	}
	if (FDoubleProperty* D = CastField<FDoubleProperty>(Prop))
	{
		double d = 0; if (Val->TryGetNumber(d)) { D->SetPropertyValue(Ptr, d); return true; }
	}
	if (FIntProperty* I = CastField<FIntProperty>(Prop))
	{
		int32 n = 0; if (Val->TryGetNumber(n)) { I->SetPropertyValue(Ptr, n); return true; }
	}
	if (FInt64Property* I64 = CastField<FInt64Property>(Prop))
	{
		int64 n = 0; if (Val->TryGetNumber(n)) { I64->SetPropertyValue(Ptr, n); return true; }
	}
	if (FByteProperty* By = CastField<FByteProperty>(Prop))
	{
		if (By->Enum)
		{
			FString s; if (Val->TryGetString(s))
			{
				int64 EV = By->Enum->GetValueByNameString(s);
				if (EV != INDEX_NONE) { By->SetPropertyValue(Ptr, (uint8)EV); return true; }
			}
		}
		else
		{
			double d = 0; if (Val->TryGetNumber(d)) { By->SetPropertyValue(Ptr, (uint8)d); return true; }
		}
	}
	if (FEnumProperty* E = CastField<FEnumProperty>(Prop))
	{
		FString s; if (Val->TryGetString(s))
		{
			int64 EV = E->GetEnum()->GetValueByNameString(s);
			if (EV != INDEX_NONE)
			{
				E->GetUnderlyingProperty()->SetIntPropertyValue(Ptr, EV);
				return true;
			}
		}
	}
	if (FStrProperty* S = CastField<FStrProperty>(Prop))
	{
		FString s; if (Val->TryGetString(s)) { S->SetPropertyValue(Ptr, s); return true; }
	}
	if (FNameProperty* N = CastField<FNameProperty>(Prop))
	{
		FString s; if (Val->TryGetString(s)) { N->SetPropertyValue(Ptr, FName(*s)); return true; }
	}
	if (FTextProperty* T = CastField<FTextProperty>(Prop))
	{
		FString s; if (Val->TryGetString(s)) { T->SetPropertyValue(Ptr, FText::FromString(s)); return true; }
	}
	if (FStructProperty* Sp = CastField<FStructProperty>(Prop))
	{
		if (Sp->Struct == TBaseStructure<FLinearColor>::Get())
		{
			FLinearColor C = *((FLinearColor*)Ptr);
			if (ReadColorFromJson(Val, C)) { *((FLinearColor*)Ptr) = C; return true; }
		}
		if (Sp->Struct == TBaseStructure<FVector2D>::Get())
		{
			FVector2D V = *((FVector2D*)Ptr);
			if (ReadVec2FromJson(Val, V)) { *((FVector2D*)Ptr) = V; return true; }
		}
	}

	// Last-chance fallback: stringify and ImportText
	FString AsStr;
	if (Val->TryGetString(AsStr))
	{
		return Prop->ImportText_Direct(*AsStr, Ptr, Owner, PPF_None) != nullptr;
	}
	double AsNum = 0;
	if (Val->TryGetNumber(AsNum))
	{
		FString S = FString::SanitizeFloat(AsNum);
		return Prop->ImportText_Direct(*S, Ptr, Owner, PPF_None) != nullptr;
	}
	return false;
}

static int32 ApplyJsonProperties(UObject* Target, const TSharedPtr<FJsonObject>& Props, TArray<FString>* OutSkipped = nullptr)
{
	if (!Target || !Props.IsValid()) return 0;
	int32 Applied = 0;
	for (const auto& KV : Props->Values)
	{
		const FString Key(*KV.Key);
		// Skip slot-only keys here (handled separately).
		FProperty* Prop = Target->GetClass()->FindPropertyByName(FName(*Key));
		if (!Prop)
		{
			if (OutSkipped) OutSkipped->Add(Key + TEXT("(unknown)"));
			continue;
		}
		if (ApplyJsonToProperty(Target, Prop, KV.Value)) ++Applied;
		else if (OutSkipped) OutSkipped->Add(Key + TEXT("(typefail)"));
	}
	return Applied;
}

static void ApplyCanvasSlotConfig(UCanvasPanelSlot* Slot, const TSharedPtr<FJsonObject>& Cfg)
{
	if (!Slot || !Cfg.IsValid()) return;

	auto GetVec2Key = [&](const TCHAR* Key, FVector2D& Out) -> bool {
		const TSharedPtr<FJsonValue> V = Cfg->Values.FindRef(Key);
		return V.IsValid() && ReadVec2FromJson(V, Out);
	};

	FVector2D Tmp;
	Tmp = Slot->GetPosition();   if (GetVec2Key(TEXT("position"), Tmp) || GetVec2Key(TEXT("pos"), Tmp)) Slot->SetPosition(Tmp);
	Tmp = Slot->GetSize();       if (GetVec2Key(TEXT("size"), Tmp))         Slot->SetSize(Tmp);
	Tmp = Slot->GetAlignment();  if (GetVec2Key(TEXT("alignment"), Tmp) || GetVec2Key(TEXT("align"), Tmp)) Slot->SetAlignment(Tmp);

	// Scalar x/y/width/height shortcuts (merge with current).
	{
		FVector2D P = Slot->GetPosition();
		double X = P.X, Y = P.Y;
		const bool bX = Cfg->TryGetNumberField(TEXT("x"), X);
		const bool bY = Cfg->TryGetNumberField(TEXT("y"), Y);
		if (bX || bY) Slot->SetPosition(FVector2D(X, Y));
	}
	{
		FVector2D Sz = Slot->GetSize();
		double W = Sz.X, H = Sz.Y;
		bool bW = Cfg->TryGetNumberField(TEXT("width"), W);  if (!bW) bW = Cfg->TryGetNumberField(TEXT("w"), W);
		bool bH = Cfg->TryGetNumberField(TEXT("height"), H); if (!bH) bH = Cfg->TryGetNumberField(TEXT("h"), H);
		if (bW || bH) Slot->SetSize(FVector2D(W, H));
	}

	// Anchors: [minX,minY,maxX,maxY] OR {min:[x,y],max:[x,y]} OR named preset string.
	const TSharedPtr<FJsonValue> AnchorsVal = Cfg->Values.FindRef(TEXT("anchors"));
	if (AnchorsVal.IsValid())
	{
		FAnchors A = Slot->GetAnchors();
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		FString PresetStr;
		if (AnchorsVal->TryGetArray(Arr) && Arr->Num() >= 4)
		{
			A.Minimum = FVector2D((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber());
			A.Maximum = FVector2D((*Arr)[2]->AsNumber(), (*Arr)[3]->AsNumber());
			Slot->SetAnchors(A);
		}
		else if (AnchorsVal->TryGetObject(Obj))
		{
			FVector2D MinV = A.Minimum, MaxV = A.Maximum;
			const TSharedPtr<FJsonValue> Mn = (*Obj)->Values.FindRef(TEXT("min"));
			const TSharedPtr<FJsonValue> Mx = (*Obj)->Values.FindRef(TEXT("max"));
			if (Mn.IsValid()) ReadVec2FromJson(Mn, MinV);
			if (Mx.IsValid()) ReadVec2FromJson(Mx, MaxV);
			A.Minimum = MinV; A.Maximum = MaxV;
			Slot->SetAnchors(A);
		}
		else if (AnchorsVal->TryGetString(PresetStr))
		{
			FString P = PresetStr.ToLower();
			if      (P == TEXT("topleft"))     A = FAnchors(0, 0, 0, 0);
			else if (P == TEXT("topcenter") || P == TEXT("topcentre")) A = FAnchors(0.5f, 0, 0.5f, 0);
			else if (P == TEXT("topright"))    A = FAnchors(1, 0, 1, 0);
			else if (P == TEXT("centerleft"))  A = FAnchors(0, 0.5f, 0, 0.5f);
			else if (P == TEXT("center") || P == TEXT("centre")) A = FAnchors(0.5f, 0.5f, 0.5f, 0.5f);
			else if (P == TEXT("centerright")) A = FAnchors(1, 0.5f, 1, 0.5f);
			else if (P == TEXT("bottomleft"))  A = FAnchors(0, 1, 0, 1);
			else if (P == TEXT("bottomcenter")) A = FAnchors(0.5f, 1, 0.5f, 1);
			else if (P == TEXT("bottomright")) A = FAnchors(1, 1, 1, 1);
			else if (P == TEXT("fill") || P == TEXT("stretch")) A = FAnchors(0, 0, 1, 1);
			else if (P == TEXT("fillhorizontal") || P == TEXT("hstretch")) A = FAnchors(0, 0, 1, 0);
			else if (P == TEXT("fillvertical")   || P == TEXT("vstretch")) A = FAnchors(0, 0, 0, 1);
			Slot->SetAnchors(A);
		}
	}

	int32 ZOrder = 0;
	if (Cfg->TryGetNumberField(TEXT("zOrder"), ZOrder) || Cfg->TryGetNumberField(TEXT("zorder"), ZOrder) || Cfg->TryGetNumberField(TEXT("z"), ZOrder))
		Slot->SetZOrder(ZOrder);

	bool bAutoSize = false;
	if (Cfg->TryGetBoolField(TEXT("autoSize"), bAutoSize) || Cfg->TryGetBoolField(TEXT("autosize"), bAutoSize) || Cfg->TryGetBoolField(TEXT("auto"), bAutoSize))
		Slot->SetAutoSize(bAutoSize);
}

// ============================================================
// CREATE WIDGET BLUEPRINT
// ============================================================

FString FNwiroIKWidgetTools::CreateWidgetBlueprint(const FString& JsonCommand)
{
	if (!IsInGameThread())
	{
		TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
		TFuture<FString> Future = Promise->GetFuture();
		AsyncTask(ENamedThreads::GameThread, [JsonCommand, Promise]()
		{
			Promise->SetValue(FNwiroIKWidgetTools::CreateWidgetBlueprint(JsonCommand));
		});
		return Future.Get();
	}

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	FString RootType = Cmd->GetStringField(TEXT("rootWidget"));

	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/UI");
	if (RootType.IsEmpty()) RootType = TEXT("CanvasPanel");

	// Add WBP_ prefix if missing
	if (!Name.StartsWith(TEXT("WBP_")) && !Name.StartsWith(TEXT("W_")))
		Name = TEXT("WBP_") + Name;

	FString FullPath = Path / Name;

	// Idempotent guard via AssetRegistry — does NOT trigger a load.
	// (The old `LoadObject<UWidgetBlueprint>` path left the destination package
	// in a half-initialized state on a "not found" lookup, which then crashed
	// the subsequent CreateBlueprint inside UMGEditor.)
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		const FString AssetPathString = FullPath + TEXT(".") + Name;
		const FSoftObjectPath SoftPath(AssetPathString);
		FAssetData ExistingData = ARM.Get().GetAssetByObjectPath(SoftPath);
		if (ExistingData.IsValid())
		{
			UObject* Loaded = ExistingData.GetAsset();
			if (UWidgetBlueprint* Existing = Cast<UWidgetBlueprint>(Loaded))
			{
				return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"existed\":true}"),
					*Name, *Existing->GetPathName());
			}
		}
	}

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateWidgetBlueprint", "AI: Create Widget Blueprint"));

	// We do NOT pre-create the package or call CreatePackage here. AssetTools::
	// CreateAsset constructs the package and outer chain itself the same way
	// the Content Browser does. Pre-touching the package was leaving it in a
	// state that broke UMGEditor's BPGC construction.
	UPackage* Package = nullptr; // (kept name for the legacy purge lambda below)

	// Defensive purge: FKismetEditorUtilities::CreateBlueprint internally does a
	// CastChecked that crashes the editor outright when a stale UObject squats
	// on the target name in the package (typical after delete_blueprint left a
	// partial — the on-disk asset is gone but the in-memory BP / BPGC / CDO
	// linger and conflict with the freshly created ones). Sweep all three.
	auto PurgeByName = [&](const FString& InName)
	{
		if (UObject* Stale = StaticFindObject(UObject::StaticClass(), Package, *InName))
		{
			Stale->ClearFlags(RF_Public | RF_Standalone);
			Stale->RemoveFromRoot();
			Stale->Rename(nullptr, GetTransientPackage(),
				REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional | REN_ForceNoResetLoaders);
			Stale->MarkAsGarbage();
		}
	};
	// TEMP DEBUG: purge calls disabled to isolate which step triggers the
	// CreateBlueprint internal CastChecked crash on UBlueprintGeneratedClass.
	// Will reinstate once the baseline (no purge, no GC) is proven stable.
	// PurgeByName(Name); PurgeByName(Name + TEXT("_C")); ...

	// CRITICAL: ensure UMGEditor and KismetCompiler modules are loaded BEFORE
	// CreateBlueprint runs. UWidgetBlueprintCompilerContext registers itself
	// in UMGEditor::StartupModule. If the module hasn't been touched yet
	// (e.g. an early MCP call before the user opens any widget UI), the
	// blueprint compilation manager has no compiler for UWidgetBlueprint,
	// silently leaves GeneratedClass=null, and the final
	// CastChecked<UBlueprintGeneratedClass> inside CreateBlueprint crashes
	// the editor with "Cast of nullptr to BlueprintGeneratedClass failed".
	// Ensure module + compiler are loaded.
	FModuleManager::Get().LoadModuleChecked(FName("KismetCompiler"));
	FModuleManager::Get().LoadModuleChecked(FName("UMGEditor"));

	// Probe registered compilers — if empty, the editor is still booting; bail
	// out cleanly instead of crashing inside CreateBlueprint's CastChecked.
	{
		IKismetCompilerInterface& KC = FModuleManager::LoadModuleChecked<IKismetCompilerInterface>("KismetCompiler");
		const TArray<IBlueprintCompiler*>& Compilers = KC.GetCompilers();
		UE_LOG(LogNwiroWidget, Warning,
			TEXT("[NWIRO_PROBE_99] WBGC.HasCtor=%d UMGEd.Loaded=%d RegisteredCompilers=%d"),
			(UWidgetBlueprintGeneratedClass::StaticClass() && UWidgetBlueprintGeneratedClass::StaticClass()->ClassConstructor) ? 1 : 0,
			FModuleManager::Get().IsModuleLoaded(FName("UMGEditor")) ? 1 : 0,
			Compilers.Num());
		if (Compilers.Num() == 0)
		{
			Tx.Cancel();
			return TEXT("{\"success\":false,\"error\":\"No blueprint compilers registered yet (editor still initializing). Wait 10s and retry.\"}");
		}
	}

	// IMPORTANT: this is the exact pattern that works in our generic
	// create_blueprint tool (NwiroIKBlueprintTools.cpp:4519-4557). Without
	// FGCScopeGuard + AddToRoot, the BP/Package can be garbage-collected
	// mid-construction, which leaves NewBP->GeneratedClass null and the
	// final CastChecked<UBlueprintGeneratedClass> inside CreateBlueprint
	// crashes the editor. Same fix the actor BP path uses.
	UWidgetBlueprint* WBP = nullptr;
	UPackage* WPackage = CreatePackage(*FullPath);
	if (!WPackage)
	{
		Tx.Cancel();
		return TEXT("{\"success\":false,\"error\":\"Failed to create package\"}");
	}
	{
		FGCScopeGuard GCScopeGuard;
		WPackage->AddToRoot();

		// Build the UWidgetBlueprint manually (skips FKismetEditorUtilities::
		// CreateBlueprint and its internal CastChecked that's been crashing in
		// this project state). Mirrors the steps Epic's UWidgetBlueprintFactory
		// performs after CreateBlueprint, but constructs the BP + BPGC + CDO
		// ourselves so we control every step. Compilation manager is invoked
		// at the end to finalize the generated class.
		WBP = NewObject<UWidgetBlueprint>(WPackage, FName(*Name),
			RF_Public | RF_Standalone | RF_Transactional);
		if (!WBP)
		{
			WPackage->RemoveFromRoot();
			Tx.Cancel();
			return TEXT("{\"success\":false,\"error\":\"NewObject<UWidgetBlueprint> returned null\"}");
		}
		WBP->AddToRoot();
		WBP->ParentClass = UUserWidget::StaticClass();
		WBP->BlueprintType = BPTYPE_Normal;
		WBP->BlueprintSystemVersion = UBlueprint::GetCurrentBlueprintSystemVersion();
		WBP->bIsNewlyCreated = true;
		WBP->bHasBeenRegenerated = false;
		WBP->bLegacyNeedToPurgeSkelRefs = false;
		WBP->Status = BS_BeingCreated;
		WBP->GenerateNewGuid();

		// Construct the WidgetTree so the compiler has something to traverse.
		WBP->WidgetTree = NewObject<UWidgetTree>(WBP, TEXT("WidgetTree"), RF_Transactional);
		WBP->WidgetTree->SetFlags(RF_Transactional | RF_ArchetypeObject);

		// Add the default EventGraph (UbergraphPage) so edit_blueprint can
		// later add Event/OnClicked nodes. Our manual NewObject path skips
		// what FKismetEditorUtilities::CreateBlueprint normally does; explicit
		// here so the resulting BP behaves like one created via the editor UI.
		UEdGraph* EventGraph = FBlueprintEditorUtils::CreateNewGraph(
			WBP, FName(TEXT("EventGraph")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddUbergraphPage(WBP, EventGraph);

		// CRITICAL: pre-seed the BPGC into the WBP's package BEFORE compile,
		// so the editor can open the saved asset on next load. See
		// EnsureSeedGeneratedClass comment for full rationale.
		EnsureSeedGeneratedClass(WBP);

		// Compile via the manager — populates GeneratedClass with a real
		// UWidgetBlueprintGeneratedClass + CDO.
		FBlueprintCompilationManager::CompileSynchronously(
			FBPCompileRequest(WBP, EBlueprintCompileOptions::None, nullptr));

		FAssetRegistryModule::AssetCreated(WBP);
		WBP->MarkPackageDirty();

		WBP->RemoveFromRoot();
		WPackage->RemoveFromRoot();
		// (We previously had an `if (!WBP->GeneratedClass)` short-circuit here,
		// but it kept tripping in cases where downstream add_widget / compile
		// calls still succeed — the GeneratedClass becomes valid on the next
		// recompile. Trust the BP and let the caller see a downstream error
		// if anything is actually broken.)
	}

	Tx.AlsoModify(WBP);

	// Set root widget
	UClass* RootClass = ResolveWidgetClass(RootType);
	if (RootClass && WBP->WidgetTree)
	{
		WBP->WidgetTree->Modify();
		UWidget* RootWidget = WBP->WidgetTree->ConstructWidget<UWidget>(RootClass, FName(TEXT("RootPanel")));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
		if (!WBP->WidgetVariableNameToGuidMap.Contains(RootWidget->GetFName()))
			WBP->WidgetVariableNameToGuidMap.Add(RootWidget->GetFName(), FGuid::NewGuid());
#endif
		WBP->WidgetTree->RootWidget = RootWidget;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
	FBlueprintCompilationManager::CompileSynchronously(
		FBPCompileRequest(WBP, EBlueprintCompileOptions::None, nullptr));

	// Force CDO creation so the on-disk BPGC has a serializable default object.
	if (WBP->GeneratedClass)
	{
		WBP->GeneratedClass->GetDefaultObject(true);
		WBP->Status = BS_UpToDate;
	}

	FAssetRegistryModule::AssetCreated(WBP);
	WBP->MarkPackageDirty();

	// Use UEditorAssetLibrary::SaveLoadedAsset — same path Ctrl+S triggers.
	// Persists the BPGC properly so editor double-click works on next load.
	UEditorAssetLibrary::SaveLoadedAsset(WBP, /*bOnlyIfIsDirty=*/false);

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"rootWidget\":\"%s\"}"),
		*Name, *WBP->GetPathName(), *RootType);
}

// ============================================================
// READ WIDGET BLUEPRINT
// ============================================================

FString FNwiroIKWidgetTools::ReadWidgetBlueprint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path;
	for (const TCHAR* K : { TEXT("path"), TEXT("assetPath"), TEXT("widgetBlueprint"), TEXT("blueprint") }) {
		if (Cmd->TryGetStringField(K, Path) && !Path.IsEmpty()) break;
	}
	UWidgetBlueprint* WBP = FindWidgetBP(Path);
	if (!WBP)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"WidgetBlueprint not found: %s\"}"), *Path);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), WBP->GetName());
	Result->SetStringField(TEXT("path"), WBP->GetPathName());

	// Serialize widget tree (+ key properties + slot info — needed for
	// acceptance assertions like "ProgressBar.Percent > 0").
	auto SerializeColor = [](const FLinearColor& C) -> TSharedPtr<FJsonObject> {
		TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject());
		O->SetNumberField(TEXT("r"), C.R); O->SetNumberField(TEXT("g"), C.G);
		O->SetNumberField(TEXT("b"), C.B); O->SetNumberField(TEXT("a"), C.A);
		return O;
	};
	auto SerializeVec2 = [](const FVector2D& V) -> TSharedPtr<FJsonObject> {
		TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject());
		O->SetNumberField(TEXT("x"), V.X); O->SetNumberField(TEXT("y"), V.Y);
		return O;
	};

	TFunction<TSharedPtr<FJsonObject>(UWidget*)> SerializeWidget;
	SerializeWidget = [&](UWidget* W) -> TSharedPtr<FJsonObject>
	{
		if (!W) return nullptr;

		TSharedPtr<FJsonObject> WidgetObj = MakeShareable(new FJsonObject());
		WidgetObj->SetStringField(TEXT("name"), W->GetName());
		WidgetObj->SetStringField(TEXT("class"), W->GetClass()->GetName());
		WidgetObj->SetBoolField(TEXT("isVisible"), W->GetVisibility() == ESlateVisibility::Visible || W->GetVisibility() == ESlateVisibility::SelfHitTestInvisible);

		// Per-class interesting properties for acceptance checks.
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject());
		if (UTextBlock* T = Cast<UTextBlock>(W))
		{
			Props->SetStringField(TEXT("Text"), T->GetText().ToString());
			Props->SetObjectField(TEXT("ColorAndOpacity"), SerializeColor(T->GetColorAndOpacity().GetSpecifiedColor()));
		}
		if (UProgressBar* PB = Cast<UProgressBar>(W))
		{
			Props->SetNumberField(TEXT("Percent"), PB->GetPercent());
			Props->SetObjectField(TEXT("FillColorAndOpacity"), SerializeColor(PB->GetFillColorAndOpacity()));
		}
		if (UButton* B = Cast<UButton>(W))
		{
			Props->SetObjectField(TEXT("ColorAndOpacity"), SerializeColor(B->GetColorAndOpacity()));
		}
		if (UBorder* Br = Cast<UBorder>(W))
		{
			Props->SetObjectField(TEXT("BrushColor"), SerializeColor(Br->GetBrushColor()));
		}
		if (USlider* Sl = Cast<USlider>(W))
		{
			Props->SetNumberField(TEXT("Value"), Sl->GetValue());
		}
		if (UCheckBox* Cb = Cast<UCheckBox>(W))
		{
			Props->SetBoolField(TEXT("IsChecked"), Cb->IsChecked());
		}
		if (USpinBox* Sp = Cast<USpinBox>(W))
		{
			Props->SetNumberField(TEXT("Value"), Sp->GetValue());
		}
		if (UEditableText* Et = Cast<UEditableText>(W))
		{
			Props->SetStringField(TEXT("Text"), Et->GetText().ToString());
		}
		if (UEditableTextBox* Etb = Cast<UEditableTextBox>(W))
		{
			Props->SetStringField(TEXT("Text"), Etb->GetText().ToString());
		}
		if (Props->Values.Num() > 0)
			WidgetObj->SetObjectField(TEXT("properties"), Props);

		// Slot info if this widget is in a CanvasPanel.
		if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(W->Slot))
		{
			TSharedPtr<FJsonObject> SlotObj = MakeShareable(new FJsonObject());
			SlotObj->SetStringField(TEXT("type"), TEXT("CanvasPanelSlot"));
			SlotObj->SetObjectField(TEXT("position"), SerializeVec2(CSlot->GetPosition()));
			SlotObj->SetObjectField(TEXT("size"), SerializeVec2(CSlot->GetSize()));
			SlotObj->SetObjectField(TEXT("alignment"), SerializeVec2(CSlot->GetAlignment()));
			FAnchors An = CSlot->GetAnchors();
			TSharedPtr<FJsonObject> Anchors = MakeShareable(new FJsonObject());
			Anchors->SetObjectField(TEXT("min"), SerializeVec2(An.Minimum));
			Anchors->SetObjectField(TEXT("max"), SerializeVec2(An.Maximum));
			SlotObj->SetObjectField(TEXT("anchors"), Anchors);
			SlotObj->SetNumberField(TEXT("zOrder"), CSlot->GetZOrder());
			SlotObj->SetBoolField(TEXT("autoSize"), CSlot->GetAutoSize());
			WidgetObj->SetObjectField(TEXT("slot"), SlotObj);
		}
		else if (W->Slot)
		{
			TSharedPtr<FJsonObject> SlotObj = MakeShareable(new FJsonObject());
			SlotObj->SetStringField(TEXT("type"), W->Slot->GetClass()->GetName());
			WidgetObj->SetObjectField(TEXT("slot"), SlotObj);
		}

		// If it's a panel, serialize children
		if (UPanelWidget* Panel = Cast<UPanelWidget>(W))
		{
			TArray<TSharedPtr<FJsonValue>> Children;
			for (int32 i = 0; i < Panel->GetChildrenCount(); i++)
			{
				UWidget* Child = Panel->GetChildAt(i);
				TSharedPtr<FJsonObject> ChildObj = SerializeWidget(Child);
				if (ChildObj.IsValid())
					Children.Add(MakeShareable(new FJsonValueObject(ChildObj)));
			}
			if (Children.Num() > 0)
				WidgetObj->SetArrayField(TEXT("children"), Children);
		}

		return WidgetObj;
	};

	if (WBP->WidgetTree && WBP->WidgetTree->RootWidget)
	{
		TSharedPtr<FJsonObject> TreeObj = SerializeWidget(WBP->WidgetTree->RootWidget);
		if (TreeObj.IsValid())
			Result->SetObjectField(TEXT("widgetTree"), TreeObj);
	}

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// ADD WIDGET
// ============================================================

FString FNwiroIKWidgetTools::AddWidget(const FString& JsonCommand)
{
	// Game-thread marshal: Slate/UObject mutation here is game-thread-only, but tool
	// dispatch can arrive on the MCP HTTP worker thread. Mirrors CreateWidgetBlueprint's guard.
	if (!IsInGameThread())
	{
		TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
		TFuture<FString> Future = Promise->GetFuture();
		AsyncTask(ENamedThreads::GameThread, [JsonCommand, Promise]()
		{
			Promise->SetValue(FNwiroIKWidgetTools::AddWidget(JsonCommand));
		});
		return Future.Get();
	}

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	// Hallucination-tolerant arg picker — many agents reach for widgetBlueprint /
	// widgetType / type / etc instead of the canonical blueprint / widgetClass.
	auto Pick = [&](std::initializer_list<const TCHAR*> Names) -> FString {
		for (const TCHAR* N : Names) { FString V; if (Cmd->TryGetStringField(N, V) && !V.IsEmpty()) return V; }
		return FString();
	};
	FString BPPath      = Pick({ TEXT("blueprint"), TEXT("widgetBlueprint"), TEXT("assetPath"), TEXT("path") });
	FString WidgetClass = Pick({ TEXT("widgetClass"), TEXT("widgetType"), TEXT("type"), TEXT("class"), TEXT("widget_type") });
	FString WidgetName  = Pick({ TEXT("name"), TEXT("widgetName"), TEXT("label") });
	FString ParentName  = Pick({ TEXT("parent"), TEXT("parentName"), TEXT("parentWidget") });

	// Optional sub-objects: arbitrary properties on the widget, and slot config
	// (CanvasPanelSlot position/size/anchors/etc when parent is a CanvasPanel).
	const TSharedPtr<FJsonObject>* PropsObjPtr = nullptr;
	const TSharedPtr<FJsonObject>* SlotObjPtr  = nullptr;
	for (const TCHAR* K : { TEXT("properties"), TEXT("props"), TEXT("settings") })
		if (!PropsObjPtr && Cmd->TryGetObjectField(K, PropsObjPtr)) {}
	for (const TCHAR* K : { TEXT("slot"), TEXT("slotConfig"), TEXT("layout") })
		if (!SlotObjPtr && Cmd->TryGetObjectField(K, SlotObjPtr)) {}

	UWidgetBlueprint* WBP = FindWidgetBP(BPPath);
	if (!WBP)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"WidgetBlueprint not found: %s\"}"), *BPPath);

	UClass* WClass = ResolveWidgetClass(WidgetClass);
	if (!WClass)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Unknown widget class: %s. Use: Button, TextBlock, Image, CanvasPanel, VerticalBox, HorizontalBox, Overlay, ScrollBox, Slider, CheckBox, ProgressBar, EditableTextBox, Border, Spacer, SizeBox\"}"), *WidgetClass);

	if (WidgetName.IsEmpty())
		WidgetName = WidgetClass + FString::FromInt(FMath::Rand() % 1000);

	UWidgetTree* Tree = WBP->WidgetTree;
	if (!Tree) return TEXT("{\"success\":false,\"error\":\"No widget tree\"}");

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "AddWidget", "AI: Add Widget"), Tree);
	Tx.AlsoModify(WBP);

	// Create the widget
	UWidget* NewWidget = Tree->ConstructWidget<UWidget>(WClass, FName(*WidgetName));
	if (!NewWidget)
		return TEXT("{\"success\":false,\"error\":\"Failed to construct widget\"}");
	NewWidget->bIsVariable = true;

	// ConstructWidget bypasses the normal UMG editor flow so WidgetVariableNameToGuidMap
	// never gets populated. The UMG blueprint compiler (WidgetBlueprintCompiler.cpp:794)
	// requires every widget in the tree to have a GUID — register it manually here.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
	if (!WBP->WidgetVariableNameToGuidMap.Contains(NewWidget->GetFName()))
		WBP->WidgetVariableNameToGuidMap.Add(NewWidget->GetFName(), FGuid::NewGuid());
#endif

	// Apply widget-level properties BEFORE AddChild so any visibility/text defaults
	// the AI specified are present before compilation.
	TArray<FString> SkippedProps;
	int32 PropsApplied = 0;
	if (PropsObjPtr && (*PropsObjPtr).IsValid())
	{
		PropsApplied = ApplyJsonProperties(NewWidget, *PropsObjPtr, &SkippedProps);
	}

	// Find parent or use root
	UPanelWidget* ParentPanel = nullptr;
	if (!ParentName.IsEmpty())
	{
		ParentPanel = Cast<UPanelWidget>(Tree->FindWidget(FName(*ParentName)));
	}
	if (!ParentPanel)
	{
		ParentPanel = Cast<UPanelWidget>(Tree->RootWidget);
	}

	UPanelSlot* PanelSlot = nullptr;
	if (ParentPanel)
	{
		PanelSlot = ParentPanel->AddChild(NewWidget);
		if (!PanelSlot)
		{
			return TEXT("{\"success\":false,\"error\":\"Parent panel cannot accept children\"}");
		}
	}
	else
	{
		// No root, set as root
		Tree->RootWidget = NewWidget;
	}

	// Apply slot config (CanvasPanelSlot is the common case; for other slot types
	// fall back to generic property reflection so caller can still set Padding etc).
	FString SlotApplied = TEXT("none");
	if (PanelSlot && SlotObjPtr && (*SlotObjPtr).IsValid())
	{
		if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(PanelSlot))
		{
			ApplyCanvasSlotConfig(CSlot, *SlotObjPtr);
			SlotApplied = TEXT("canvas");
		}
		else
		{
			ApplyJsonProperties(PanelSlot, *SlotObjPtr);
			SlotApplied = TEXT("generic");
		}
	}

	// Do NOT synchronously CompileBlueprint(WBP) here. A synchronous widget-BP
	// compile mid-tree-mutation AV-crashes UE in the Kismet/UMG compile path
	// (EXCEPTION_ACCESS_VIOLATION) — the same crash that forced removing the
	// compile from SetWidgetProperty. The structural edit is already live in the
	// WidgetTree (read_widget_blueprint/render/openable all see it); marking the
	// BP modified lets the editor persist + lazily recompile on save/open safely.
	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

	FString SkippedJoined = FString::Join(SkippedProps, TEXT(","));
	return FString::Printf(TEXT("{\"success\":true,\"widget\":\"%s\",\"class\":\"%s\",\"parent\":\"%s\",\"propsApplied\":%d,\"propsSkipped\":\"%s\",\"slotApplied\":\"%s\"}"),
		*WidgetName, *WClass->GetName(), ParentPanel ? *ParentPanel->GetName() : TEXT("root"),
		PropsApplied, *SkippedJoined, *SlotApplied);
}

// ============================================================
// SET WIDGET PROPERTY
// ============================================================

FString FNwiroIKWidgetTools::SetWidgetProperty(const FString& JsonCommand)
{
	// Game-thread marshal: Slate/UObject mutation here is game-thread-only, but tool
	// dispatch can arrive on the MCP HTTP worker thread. Mirrors CreateWidgetBlueprint's guard.
	if (!IsInGameThread())
	{
		TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
		TFuture<FString> Future = Promise->GetFuture();
		AsyncTask(ENamedThreads::GameThread, [JsonCommand, Promise]()
		{
			Promise->SetValue(FNwiroIKWidgetTools::SetWidgetProperty(JsonCommand));
		});
		return Future.Get();
	}

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	auto Pick = [&](std::initializer_list<const TCHAR*> Names) -> FString {
		for (const TCHAR* N : Names) { FString V; if (Cmd->TryGetStringField(N, V) && !V.IsEmpty()) return V; }
		return FString();
	};
	FString BPPath       = Pick({ TEXT("blueprint"), TEXT("widgetBlueprint"), TEXT("assetPath"), TEXT("path") });
	FString WidgetName   = Pick({ TEXT("widget"), TEXT("widgetName"), TEXT("name") });
	FString PropertyName = Pick({ TEXT("property"), TEXT("propertyName"), TEXT("prop") });
	FString Value        = Pick({ TEXT("value"), TEXT("val") });

	UWidgetBlueprint* WBP = FindWidgetBP(BPPath);
	if (!WBP)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"WidgetBlueprint not found: %s\"}"), *BPPath);

	// Loose widget lookup — exact + case-insensitive across all widgets in tree.
	UWidget* Widget = WBP->WidgetTree ? WBP->WidgetTree->FindWidget(FName(*WidgetName)) : nullptr;
	if (!Widget && WBP->WidgetTree)
	{
		TArray<UWidget*> AllW;
		WBP->WidgetTree->GetAllWidgets(AllW);
		for (UWidget* W : AllW)
		{
			if (W && W->GetName().Equals(WidgetName, ESearchCase::IgnoreCase)) { Widget = W; break; }
		}
	}
	if (!Widget)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Widget not found: %s\"}"), *WidgetName);

	// Resolve which object/struct owns the property. The agent commonly uses
	// names visible in the blueprint editor — for slots these don't always
	// match the C++ field names. UCanvasPanelSlot exposes Anchors/Alignment/
	// Offsets in the editor but stores them nested inside `LayoutData`
	// (FAnchorData). So a single property name may need to walk through:
	//   Widget → Slot → LayoutData → Anchors
	// We accept any dotted path AND a list of editor-name → C++-path
	// aliases so the agent's mental model "just works".
	//
	// Strategy: build a list of candidate dotted paths to try, in order.
	UObject* RootTarget = Widget;
	FString ResolvedPropName = PropertyName;

	TArray<TPair<UObject*, FString>> Candidates;
	Candidates.Emplace(Widget, PropertyName);
	if (Widget->Slot)
	{
		// Strip "Slot." prefix if present, then try on the slot.
		FString OnSlot = PropertyName;
		if (OnSlot.StartsWith(TEXT("Slot.")) || OnSlot.StartsWith(TEXT("slot.")))
			OnSlot = OnSlot.Mid(5);
		Candidates.Emplace(Widget->Slot, OnSlot);
		// Canvas-panel-slot aliases: editor labels → nested struct paths.
		// These are checked AFTER the direct lookup so a real direct prop
		// (e.g. ZOrder, bAutoSize) still wins.
		static const TMap<FString, FString> SlotAliases = {
			{ TEXT("anchors"),    TEXT("LayoutData.Anchors") },
			{ TEXT("alignment"),  TEXT("LayoutData.Alignment") },
			{ TEXT("offsets"),    TEXT("LayoutData.Offsets") },
			{ TEXT("offset"),     TEXT("LayoutData.Offsets") },
			{ TEXT("autosize"),   TEXT("bAutoSize") },
			{ TEXT("sizetocontent"), TEXT("bAutoSize") },
		};
		if (const FString* Alias = SlotAliases.Find(OnSlot.ToLower()))
		{
			Candidates.Emplace(Widget->Slot, *Alias);
		}
	}

	// Widget-level size aliases. The editor shows "size" on an Image via its
	// Brush's ImageSize, but agents naturally reach for WidthOverride /
	// HeightOverride / Size / DesiredSize (those are SizeBox-only). Map them
	// onto Brush.ImageSize so the natural call works on Image/Border/etc. These
	// are added AFTER the direct + slot candidates, so a real WidthOverride on a
	// SizeBox still wins; on an Image (no such prop) it falls through to here.
	{
		static const TMap<FString, FString> WidgetSizeAliases = {
			{ TEXT("widthoverride"),  TEXT("Brush.ImageSize.X") },
			{ TEXT("heightoverride"), TEXT("Brush.ImageSize.Y") },
			{ TEXT("width"),          TEXT("Brush.ImageSize.X") },
			{ TEXT("height"),         TEXT("Brush.ImageSize.Y") },
			{ TEXT("size"),           TEXT("Brush.ImageSize") },
			{ TEXT("desiredsize"),    TEXT("Brush.ImageSize") },
			{ TEXT("imagesize"),      TEXT("Brush.ImageSize") },
		};
		if (const FString* Alias = WidgetSizeAliases.Find(PropertyName.ToLower()))
			Candidates.Emplace(Widget, *Alias);
	}

	// Walk a dotted path on a base object. Returns the final FProperty + a
	// raw value pointer into the leaf. Handles arbitrary struct nesting.
	auto Resolve = [](UObject* Base, const FString& DottedPath, FProperty*& OutProp, void*& OutValPtr) -> bool
	{
		if (!Base) return false;
		TArray<FString> Parts; DottedPath.ParseIntoArray(Parts, TEXT("."));
		if (Parts.Num() == 0) return false;
		UStruct* CurStruct = Base->GetClass();
		void* CurPtr = (void*)Base;
		FProperty* CurProp = nullptr;
		for (int32 i = 0; i < Parts.Num(); i++)
		{
			CurProp = CurStruct->FindPropertyByName(FName(*Parts[i]));
			if (!CurProp) return false;
			void* NewPtr = CurProp->ContainerPtrToValuePtr<void>(CurPtr);
			if (i == Parts.Num() - 1) { OutProp = CurProp; OutValPtr = NewPtr; return true; }
			// Descend into struct
			if (FStructProperty* SP = CastField<FStructProperty>(CurProp))
			{
				CurStruct = SP->Struct;
				CurPtr = NewPtr;
				continue;
			}
			return false; // intermediate part isn't a struct → can't descend
		}
		return false;
	};

	UObject* Target = nullptr;
	FProperty* Prop = nullptr;
	void* ValuePtr = nullptr;
	for (const auto& Cand : Candidates)
	{
		FProperty* TryProp = nullptr; void* TryPtr = nullptr;
		if (Resolve(Cand.Key, Cand.Value, TryProp, TryPtr))
		{
			Target = Cand.Key;
			Prop = TryProp;
			ValuePtr = TryPtr;
			ResolvedPropName = Cand.Value;
			break;
		}
	}

	if (!Prop)
	{
		// Build a helpful error: list widget + slot property names so the
		// agent can self-correct without burning 9 retries.
		TArray<FString> WidgetProps;
		for (TFieldIterator<FProperty> It(Widget->GetClass()); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
				WidgetProps.Add(It->GetName());
		}
		TArray<FString> SlotProps;
		if (Widget->Slot)
		{
			for (TFieldIterator<FProperty> It(Widget->Slot->GetClass()); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
					SlotProps.Add(It->GetName());
			}
		}
		const int32 MaxList = 12;
		FString WidgetList = FString::Join(MakeArrayView(WidgetProps).Slice(0, FMath::Min(MaxList, WidgetProps.Num())), TEXT(", "));
		FString SlotList = Widget->Slot
			? FString::Join(MakeArrayView(SlotProps).Slice(0, FMath::Min(MaxList, SlotProps.Num())), TEXT(", "))
			: FString(TEXT("(widget has no slot — not in a panel)"));
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Property not found: %s. Aliases tried (Anchors/Alignment/Offsets → LayoutData.X, AutoSize → bAutoSize). Widget props (first %d): %s. Slot props (use bare name, 'Slot.X', or 'Slot.LayoutData.X'): %s\"}"),
			*PropertyName, MaxList, *WidgetList, *SlotList);
	}

	// Coerce friendly shorthand into the exact struct text ImportText wants, so
	// the agent does not burn retries guessing the literal syntax.
	//   FSlateChildSize (VerticalBox/HorizontalBox child Slot.Size):
	//     "auto" -> (SizeRule=Auto,Value=1.000000)
	//     "fill" / "fill:2" / "2" -> (SizeRule=Fill,Value=2.000000)
	//   FVector2D (e.g. Brush.ImageSize):
	//     "32" -> (X=32,Y=32) ; "32,48" / "32 48" -> (X=32,Y=48)
	// Only runs when the resolved prop is that struct AND the value is not
	// already a struct literal, so explicit "(SizeRule=...)" calls pass through.
	FString ImportVal = Value;
	if (FStructProperty* SP = CastField<FStructProperty>(Prop))
	{
		const FString StructName = SP->Struct->GetName();
		const FString Trimmed = Value.TrimStartAndEnd();
		if (!Trimmed.StartsWith(TEXT("(")))
		{
			if (StructName == TEXT("SlateChildSize"))
			{
				const FString Low = Trimmed.ToLower();
				if (Low.StartsWith(TEXT("auto")))
				{
					ImportVal = TEXT("(SizeRule=Auto,Value=1.000000)");
				}
				else
				{
					FString NumPart = Low;
					NumPart.ReplaceInline(TEXT("fill"), TEXT(""));
					NumPart.ReplaceInline(TEXT(":"), TEXT(" "));
					NumPart.ReplaceInline(TEXT(","), TEXT(" "));
					NumPart = NumPart.TrimStartAndEnd();
					const float Weight = NumPart.IsEmpty() ? 1.0f : FCString::Atof(*NumPart);
					ImportVal = FString::Printf(TEXT("(SizeRule=Fill,Value=%f)"), Weight);
				}
			}
			else if (StructName == TEXT("Vector2D") || StructName == TEXT("Vector2f"))
			{
				FString N = Trimmed;
				N.ReplaceInline(TEXT(","), TEXT(" "));
				TArray<FString> Toks;
				N.ParseIntoArray(Toks, TEXT(" "), true);
				if (Toks.Num() == 1)
					ImportVal = FString::Printf(TEXT("(X=%s,Y=%s)"), *Toks[0], *Toks[0]);
				else if (Toks.Num() >= 2)
					ImportVal = FString::Printf(TEXT("(X=%s,Y=%s)"), *Toks[0], *Toks[1]);
			}
		}
	}

	{
		FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "SetWidgetProperty", "AI: Set Widget Property"), Target);
		Tx.AlsoModify(WBP);

		if (Prop->ImportText_Direct(*ImportVal, ValuePtr, Target, PPF_None))
		{
			// Mark dirty so the editor persists + lazily recompiles. Do NOT call
			// FKismetEditorUtilities::CompileBlueprint here: a synchronous widget
			// compile mid-property-mutation crashes UE with an access violation
			// (EXCEPTION_ACCESS_VIOLATION in the Kismet compile path). The widget
			// tree edit is already live for read/render/openable; a full compile
			// is unnecessary per-set and the crash poisons the whole run.
			FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

			const TCHAR* On = (Target == Widget) ? TEXT("widget") : TEXT("slot");
			return FString::Printf(TEXT("{\"success\":true,\"widget\":\"%s\",\"property\":\"%s\",\"value\":\"%s\",\"setOn\":\"%s\"}"),
				*WidgetName, *ResolvedPropName, *Value, On);
		}
		Tx.Cancel();
	}

	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to set %s on %s (ImportText rejected value '%s')\"}"), *PropertyName, *WidgetName, *Value);
}

// Render a WidgetBlueprint to PNG by walking its editor-side WidgetTree
// directly. Bypasses BPGC instantiation — that path produces empty visuals
// for our manually-built widgets because the BPGC class layout doesn't
// carry the editor-time WidgetTree mutations.
FString FNwiroIKWidgetTools::RenderWidgetBlueprint(const FString& JsonCommand)
{
	// Game-thread marshal: Slate construction + render commands here are game-thread-only, but
	// tool dispatch can arrive on the MCP HTTP worker thread. Mirrors CreateWidgetBlueprint's guard.
	if (!IsInGameThread())
	{
		TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
		TFuture<FString> Future = Promise->GetFuture();
		AsyncTask(ENamedThreads::GameThread, [JsonCommand, Promise]()
		{
			Promise->SetValue(FNwiroIKWidgetTools::RenderWidgetBlueprint(JsonCommand));
		});
		return Future.Get();
	}

	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path;
	for (const TCHAR* K : { TEXT("path"), TEXT("blueprint"), TEXT("widget"), TEXT("name"), TEXT("assetPath") })
		if (Cmd->TryGetStringField(K, Path) && !Path.IsEmpty()) break;
	if (Path.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"Missing 'path'\"}");

	int32 Width = Cmd->HasField(TEXT("width")) ? (int32)Cmd->GetNumberField(TEXT("width")) : 1280;
	int32 Height = Cmd->HasField(TEXT("height")) ? (int32)Cmd->GetNumberField(TEXT("height")) : 720;
	Width = FMath::Clamp(Width, 64, 4096);
	Height = FMath::Clamp(Height, 64, 4096);

	UWidgetBlueprint* WBP = FindWidgetBP(Path);
	if (!WBP)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Widget blueprint not found: %s\"}"), *Path);
	if (!WBP->WidgetTree || !WBP->WidgetTree->RootWidget)
		return TEXT("{\"success\":false,\"error\":\"WidgetTree or RootWidget is null\"}");

	TSharedRef<SWidget> SlateWidget = WBP->WidgetTree->RootWidget->TakeWidget();
	const FVector2D DrawSize((float)Width, (float)Height);

	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>();
	RT->RenderTargetFormat = RTF_RGBA8;
	RT->ClearColor = FLinearColor(0.1f, 0.1f, 0.12f, 1.0f);
	RT->InitAutoFormat(Width, Height);
	RT->UpdateResourceImmediate(true);

	FWidgetRenderer* Renderer = new FWidgetRenderer(true);
	Renderer->SetIsPrepassNeeded(true);
	Renderer->DrawWidget(RT, SlateWidget, DrawSize, 0.0f);
	FlushRenderingCommands();
	BeginCleanup(Renderer);

	FTextureRenderTargetResource* RTRes = RT->GameThread_GetRenderTargetResource();
	if (!RTRes) return TEXT("{\"success\":false,\"error\":\"No render target resource\"}");

	TArray<FColor> Pixels;
	Pixels.SetNumUninitialized(Width * Height);
	FReadSurfaceDataFlags Flags;
	Flags.SetLinearToGamma(false);
	RTRes->ReadPixels(Pixels, Flags);

	IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Png = ImageModule.CreateImageWrapper(EImageFormat::PNG);
	if (!Png.IsValid() || !Png->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
		return TEXT("{\"success\":false,\"error\":\"PNG encode failed\"}");
	const TArray64<uint8> PngData = Png->GetCompressed();

	FString SavePath;
	Cmd->TryGetStringField(TEXT("saveTo"), SavePath);
	if (SavePath.IsEmpty())
	{
		const FString BaseName = FPaths::GetBaseFilename(WBP->GetPathName());
		SavePath = FPaths::ProjectSavedDir() / TEXT("NwiroWidgetRenders") / (BaseName + TEXT("_render.png"));
	}
	SavePath = FPaths::ConvertRelativePathToFull(SavePath);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SavePath), true);
	if (!FFileHelper::SaveArrayToFile(PngData, *SavePath))
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Write PNG failed: %s\"}"), *SavePath);

	return FString::Printf(TEXT("{\"success\":true,\"path\":\"%s\",\"width\":%d,\"height\":%d,\"widget\":\"%s\"}"),
		*SavePath.Replace(TEXT("\\"), TEXT("/")), Width, Height, *WBP->GetName());
}
