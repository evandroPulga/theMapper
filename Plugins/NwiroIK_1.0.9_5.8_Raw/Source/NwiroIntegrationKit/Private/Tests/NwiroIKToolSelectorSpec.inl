// Copyright 2026 Nwiro. All Rights Reserved.
//
// NwiroIKToolSelectorSpec.inl — P0-G Phase 1 unit tests for the localllm
// ToolSelector. Included at FILE SCOPE at the bottom of NwiroIKBridge.cpp,
// AFTER `} // namespace NwiroToolSelector`, under #if WITH_DEV_AUTOMATION_TESTS.
//
// It is part of NwiroIKBridge.cpp's translation unit, so it legally calls the
// internal-linkage seams NwiroToolSelector::ResolveToolBudgetImpl and
// NwiroToolSelector::SelectToolsImpl without any export/header. Do NOT add this
// file to a module .cpp list and do NOT #include it anywhere but the single
// `#include "Tests/NwiroIKToolSelectorSpec.inl"` line in NwiroIKBridge.cpp.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

// ─────────────────────────────────────────────────────────────────────────────
// Test-local helpers (anonymous namespace — internal linkage, no ODR clash).
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
	// Build ONE MCP tool entry: { "name": <N>, "description": <Desc> } as a
	// TSharedPtr<FJsonValue> Object — the same shape DoSendPrompt deserializes
	// from GetToolDefinitions() (FNwiroIKMCPServer::GetEnabledToolDefinitionsJson()).
	TSharedPtr<FJsonValue> MakeTool(const FString& Name, const FString& Desc = FString())
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		if (!Desc.IsEmpty())
		{
			Obj->SetStringField(TEXT("description"), Desc);
		}
		return MakeShared<FJsonValueObject>(Obj);
	}

	// Synthetic ~220-tool registry. Includes the real pinlist verbs (so the pin
	// loop can find them) and the real Material group (so the B13 gate test has
	// targets), then pads with synthetic tool_NNN entries up to >= Count so the
	// registry exceeds any test Budget. Names are unique; lower index == earlier
	// insertion (used by the deterministic tie-break assertion).
	TArray<TSharedPtr<FJsonValue>> MakeSyntheticRegistry(int32 Count = 220)
	{
		TArray<TSharedPtr<FJsonValue>> Tools;

		// (a) The real pinned verbs first (indices 0..N-1) — must be present so
		//     the pin-floor / pin loop are exercised against real names.
		for (const FName& P : NwiroToolSelector::GetPinnedToolNames())
		{
			Tools.Add(MakeTool(P.ToString(), TEXT("pinned verb")));
		}

		// (b) The full Material category group, so B13 gating has real targets.
		TSet<FName> MaterialNames;
		NwiroToolSelector::ResolveCategoryGroup(
			NwiroToolSelector::EToolContextCategory::Material, MaterialNames);
		for (const FName& M : MaterialNames)
		{
			// create_material / edit_material are already in the pinlist; skip dups
			// so every registry name stays unique (NameToIdx is keyed by name).
			const FString S = M.ToString();
			bool bDup = false;
			for (const FName& P : NwiroToolSelector::GetPinnedToolNames())
			{
				if (P == M) { bDup = true; break; }
			}
			if (!bDup) { Tools.Add(MakeTool(S, TEXT("material tool"))); }
		}

		// (c) Pad with synthetic, BM25-inert tools up to Count. Distinct tokens
		//     ('synthetic','filler') that don't collide with query terms below.
		int32 N = 0;
		while (Tools.Num() < Count)
		{
			Tools.Add(MakeTool(
				FString::Printf(TEXT("synthetic_filler_tool_%03d"), N),
				TEXT("synthetic filler")));
			++N;
		}
		return Tools;
	}

	bool RegistryContainsName(const TArray<int32>& Keep,
		const TArray<TSharedPtr<FJsonValue>>& Reg, const FString& Name)
	{
		for (int32 Idx : Keep)
		{
			FString N;
			if (Reg.IsValidIndex(Idx) && Reg[Idx].IsValid()
				&& Reg[Idx]->Type == EJson::Object
				&& Reg[Idx]->AsObject()->TryGetStringField(TEXT("name"), N)
				&& N == Name)
			{
				return true;
			}
		}
		return false;
	}
} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Spec definition.
// ─────────────────────────────────────────────────────────────────────────────
// UE 5.7: EAutomationTestFlags is a scoped enum; the app-context mask is NO LONGER
// a member (EAutomationTestFlags::ApplicationContextMask) — it's the free constexpr
// EAutomationTestFlags_ApplicationContextMask (AutomationTest.h:143). ProductFilter
// IS still a real enum member, so it keeps the scoped form.
BEGIN_DEFINE_SPEC(FNwiroIKToolSelectorSpec, "NwiroIK.ToolSelector",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FNwiroIKToolSelectorSpec)

void FNwiroIKToolSelectorSpec::Define()
{
	using namespace NwiroToolSelector;

	// ── B11 — tier → cap table (pure ResolveToolBudgetImpl, env injected) ─────
	Describe("ResolveToolBudgetImpl (B11)", [this]()
	{
		It("maps native to 64 (NOT -1) with no override", [this]()
		{
			TestEqual(TEXT("native==64"), ResolveToolBudgetImpl(TEXT("native"), 0), 64);
		});
		It("maps emulated to 24 with no override", [this]()
		{
			TestEqual(TEXT("emulated==24"), ResolveToolBudgetImpl(TEXT("emulated"), 0), 24);
		});
		It("maps empty tier to 24 (no-warmup default)", [this]()
		{
			TestEqual(TEXT("empty==24"), ResolveToolBudgetImpl(TEXT(""), 0), 24);
		});
		It("maps none to 0 even when an override is set", [this]()
		{
			// The none -> 0 check MUST win over the override. This is the
			// behavioral contract that lets a none-tier strip be irreversible.
			TestEqual(TEXT("none==0 ignores override"),
				ResolveToolBudgetImpl(TEXT("none"), 999), 0);
		});
		It("is case-insensitive on the tier string", [this]()
		{
			TestEqual(TEXT("NONE==0"), ResolveToolBudgetImpl(TEXT("NONE"), 0), 0);
			TestEqual(TEXT("Native==64"), ResolveToolBudgetImpl(TEXT("Native"), 0), 64);
		});
		It("lets a positive override clamp native and emulated", [this]()
		{
			TestEqual(TEXT("override clamps native"),
				ResolveToolBudgetImpl(TEXT("native"), 12), 12);
			TestEqual(TEXT("override clamps emulated"),
				ResolveToolBudgetImpl(TEXT("emulated"), 12), 12);
		});
		It("ignores a non-positive override (0 == unset)", [this]()
		{
			TestEqual(TEXT("override 0 -> native default"),
				ResolveToolBudgetImpl(TEXT("native"), 0), 64);
		});
	});

	// ── B12 — selection mechanics (SelectToolsImpl, Category=Unknown) ─────────
	Describe("SelectToolsImpl mechanics (B12)", [this]()
	{
		It("raises the effective Budget to the pinlist floor when Budget<pinlist", [this]()
		{
			TArray<TSharedPtr<FJsonValue>> Reg = MakeSyntheticRegistry(220);
			const int32 PinCount = GetPinnedToolNames().Num();
			TArray<int32> Keep;
			// Budget below the pin count; the floor must raise kept to >= PinCount.
			// (Smoke test: relies on every pinned name being present in Reg.)
			SelectToolsImpl(TEXT("hello"), Reg, /*Budget=*/3,
				EToolContextCategory::Unknown, Keep);
			TestEqual(TEXT("kept == pin floor"), Keep.Num(), PinCount);
		});

		It("keeps exactly Budget when the registry exceeds Budget", [this]()
		{
			TArray<TSharedPtr<FJsonValue>> Reg = MakeSyntheticRegistry(220);
			TArray<int32> Keep;
			SelectToolsImpl(TEXT("hello"), Reg, /*Budget=*/24,
				EToolContextCategory::Unknown, Keep);
			TestEqual(TEXT("kept == Budget"), Keep.Num(), 24);
		});

		It("does full passthrough when the registry fits the Budget", [this]()
		{
			// Small registry (just the pins) with a large Budget => keep all,
			// in natural index order.
			TArray<TSharedPtr<FJsonValue>> Reg;
			for (const FName& P : GetPinnedToolNames()) { Reg.Add(MakeTool(P.ToString())); }
			TArray<int32> Keep;
			SelectToolsImpl(TEXT("hello"), Reg, /*Budget=*/100,
				EToolContextCategory::Unknown, Keep);
			TestEqual(TEXT("kept all"), Keep.Num(), Reg.Num());
			for (int32 i = 0; i < Reg.Num(); ++i)
			{
				TestEqual(TEXT("natural order"), Keep[i], i);
			}
		});

		It("breaks BM25 ties by lower index (deterministic)", [this]()
		{
			// With a query that matches no tool token, all candidates score 0 and
			// the sort MUST keep ascending index order among the kept tail.
			TArray<TSharedPtr<FJsonValue>> Reg = MakeSyntheticRegistry(220);
			TArray<int32> Keep;
			SelectToolsImpl(TEXT("zzz_no_match_query"), Reg, /*Budget=*/24,
				EToolContextCategory::Unknown, Keep);
			// The non-pinned kept indices (the BM25 tail) must be ascending.
			TSet<FName> PinSet(GetPinnedToolNames());
			TArray<int32> Tail;
			for (int32 Idx : Keep)
			{
				FString N; Reg[Idx]->AsObject()->TryGetStringField(TEXT("name"), N);
				if (!PinSet.Contains(FName(*N))) { Tail.Add(Idx); }
			}
			// Guard against a vacuous pass: Budget 24 - pins leaves a multi-entry
			// tail, so there MUST be >1 element to compare or the assertion is moot.
			TestTrue(TEXT("tail has multiple entries to compare"), Tail.Num() > 1);
			for (int32 i = 1; i < Tail.Num(); ++i)
			{
				TestTrue(TEXT("tail strictly ascending (tie-break)"),
					Tail[i] > Tail[i - 1]);
			}
		});

		It("includes the create_* creation-verb class via the pinlist", [this]()
		{
			TArray<TSharedPtr<FJsonValue>> Reg = MakeSyntheticRegistry(220);
			TArray<int32> Keep;
			SelectToolsImpl(TEXT("make me something"), Reg, /*Budget=*/24,
				EToolContextCategory::Unknown, Keep);
			TestTrue(TEXT("create_blueprint pinned-present"),
				RegistryContainsName(Keep, Reg, TEXT("create_blueprint")));
			TestTrue(TEXT("create_material pinned-present"),
				RegistryContainsName(Keep, Reg, TEXT("create_material")));
		});
	});

	// ── B13 — pure category functions, drift latch, and material gating ───────
	Describe("Category functions & gating (B13)", [this]()
	{
		It("CategoryName is the inverse of the enum for known categories", [this]()
		{
			TestEqual(TEXT("Material"),  FString(CategoryName(EToolContextCategory::Material)),  FString(TEXT("Material")));
			TestEqual(TEXT("Blueprint"), FString(CategoryName(EToolContextCategory::Blueprint)), FString(TEXT("Blueprint")));
			TestEqual(TEXT("Unknown"),   FString(CategoryName(EToolContextCategory::Unknown)),   FString(TEXT("Unknown")));
		});

		It("ResolveCategoryGroup yields a non-empty, expected set for Material", [this]()
		{
			TSet<FName> G;
			ResolveCategoryGroup(EToolContextCategory::Material, G);
			TestTrue(TEXT("Material group non-empty"), G.Num() > 0);
			TestTrue(TEXT("has create_material"), G.Contains(FName(TEXT("create_material"))));
			TestTrue(TEXT("has set_material_property"),
				G.Contains(FName(TEXT("set_material_property"))));
		});

		It("ResolveCategoryGroup(Unknown) is empty (cold-start => full set)", [this]()
		{
			TSet<FName> G;
			ResolveCategoryGroup(EToolContextCategory::Unknown, G);
			TestEqual(TEXT("Unknown group empty"), G.Num(), 0);
		});

		It("gates to material tools when Category=Material", [this]()
		{
			TArray<TSharedPtr<FJsonValue>> Reg = MakeSyntheticRegistry(220);
			TArray<int32> Keep;
			// Budget 40 (not 24): the Material group has more BM25-inert members
			// than 24-minus-pins tail slots, so at Budget 24 the tie-break could
			// deterministically-but-arbitrarily drop set_material_property (its
			// registry index depends on TSet<FName> hash-iteration order). A budget
			// wider than the whole Material group guarantees EVERY material tool
			// survives, making the "gated-in" assertion stable across runs.
			SelectToolsImpl(TEXT("tweak the surface shader"), Reg, /*Budget=*/40,
				EToolContextCategory::Material, Keep);

			// Every NON-pinned kept tool must be a Material-group member (the gate
			// restricts candidates to the Material set; pins are added on top).
			TSet<FName> PinSet(GetPinnedToolNames());
			TSet<FName> MatSet;
			ResolveCategoryGroup(EToolContextCategory::Material, MatSet);
			for (int32 Idx : Keep)
			{
				FString N; Reg[Idx]->AsObject()->TryGetStringField(TEXT("name"), N);
				const FName FN(*N);
				const bool bPinned = PinSet.Contains(FN);
				const bool bMaterial = MatSet.Contains(FN);
				TestTrue(TEXT("kept tool is pinned-or-material"), bPinned || bMaterial);
			}
			// And a representative material tool that ISN'T pinned should survive.
			TestTrue(TEXT("set_material_property gated-in"),
				RegistryContainsName(Keep, Reg, TEXT("set_material_property")));
			// A synthetic filler (non-material, non-pinned) must NOT survive.
			TestFalse(TEXT("filler gated-out"),
				RegistryContainsName(Keep, Reg, TEXT("synthetic_filler_tool_000")));
		});

		// ValidateToolNamesOnce drift — NOTE THE ONCE-LATCH IMPLICATION:
		// ValidateToolNamesOnce has a `static bool bValidated` that latches true
		// on the FIRST call in the process and short-circuits forever after. By
		// the time this spec runs, the production path (or an earlier B12/B13
		// SelectToolsImpl case above) has very likely ALREADY tripped that latch,
		// so we CANNOT assert that a drifted registry re-emits the warning — the
		// validator will no-op. We therefore assert only the OBSERVABLE,
		// latch-independent contract: SelectToolsImpl tolerates a registry that
		// is MISSING pinned/category names without crashing and without inventing
		// indices. (A real drift-warning assertion would require the once-latch to
		// be reset or seam-injected, which is out of P0-G Phase 1 scope.)
		It("tolerates pin/category drift (missing names) without crashing", [this]()
		{
			// A registry with NONE of the pinned names present (pure synthetic).
			TArray<TSharedPtr<FJsonValue>> Reg;
			for (int32 i = 0; i < 50; ++i)
			{
				Reg.Add(MakeTool(FString::Printf(TEXT("drift_tool_%02d"), i)));
			}
			TArray<int32> Keep;
			SelectToolsImpl(TEXT("anything"), Reg, /*Budget=*/24,
				EToolContextCategory::Material, Keep);
			// No pinned name resolves, Material names absent => candidates empty =>
			// kept is empty (gate found nothing, nothing pinned). Assert the exact
			// contract, then defensively confirm any (impossible) index is valid.
			TestEqual(TEXT("drifted registry keeps nothing"), Keep.Num(), 0);
			for (int32 Idx : Keep)
			{
				TestTrue(TEXT("kept index in range"), Reg.IsValidIndex(Idx));
			}
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
