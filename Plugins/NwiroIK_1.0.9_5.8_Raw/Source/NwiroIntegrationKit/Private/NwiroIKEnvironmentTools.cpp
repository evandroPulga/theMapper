// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKEnvironmentTools.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SplineComponent.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/Light.h"
#include "Components/LightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/PhysicsConstraintActor.h"
#include "EngineUtils.h"
#include "EditorAssetLibrary.h"
#include "Json.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroEnv, Log, All);

static UWorld* GetEdWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

// Get existing USplineComponent or attach a new one — LLM-friendly so add_spline_point on a
// plain actor doesn't fail with "no spline component". Mutating attach is gated by bAutoAttach.
static USplineComponent* GetOrAddSpline(AActor* Actor, bool bAutoAttach = true)
{
	if (!Actor) return nullptr;
	USplineComponent* S = Actor->FindComponentByClass<USplineComponent>();
	if (S) return S;
	if (!bAutoAttach) return nullptr;
	S = NewObject<USplineComponent>(Actor, TEXT("SplineComponent"));
	if (!S) return nullptr;
	S->RegisterComponent();
	Actor->AddInstanceComponent(S);
	if (!Actor->GetRootComponent()) Actor->SetRootComponent(S);
	else S->AttachToComponent(Actor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
	S->ClearSplinePoints();
	return S;
}

static AActor* FindActorByName(UWorld* World, const FString& Name)
{
	if (!World || Name.IsEmpty()) return nullptr;
	// 4-tier fuzzy match — handles spawn_actor uniqueness suffixes and LLM hallucinated casing.
	AActor* Exact = nullptr;
	AActor* CaseInsensitive = nullptr;
	AActor* StartsWith = nullptr;
	AActor* Contains = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		const FString N = A->GetName();
		const FString L = A->GetActorLabel();
		if (N == Name || L == Name) { Exact = A; break; }
		if (!CaseInsensitive && (N.Equals(Name, ESearchCase::IgnoreCase) || L.Equals(Name, ESearchCase::IgnoreCase))) CaseInsensitive = A;
		if (!StartsWith && (N.StartsWith(Name, ESearchCase::IgnoreCase) || L.StartsWith(Name, ESearchCase::IgnoreCase))) StartsWith = A;
		if (!Contains && (N.Contains(Name, ESearchCase::IgnoreCase) || L.Contains(Name, ESearchCase::IgnoreCase))) Contains = A;
	}
	return Exact ? Exact : (CaseInsensitive ? CaseInsensitive : (StartsWith ? StartsWith : Contains));
}

// ============================================================
// SET POST PROCESS
// ============================================================

FString FNwiroIKEnvironmentTools::SetPostProcess(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GetEdWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	APostProcessVolume* PPV = nullptr;
	if (!ActorName.IsEmpty())
	{
		PPV = Cast<APostProcessVolume>(FindActorByName(World, ActorName));
	}
	if (!PPV)
	{
		for (TActorIterator<APostProcessVolume> It(World); It; ++It) { PPV = *It; break; }
	}
	// Auto-spawn an unbound PPV if none exists — most LLM-friendly default.
	if (!PPV)
	{
		FActorSpawnParameters Sp;
		Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		PPV = World->SpawnActor<APostProcessVolume>(APostProcessVolume::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Sp);
		if (PPV) { PPV->bUnbound = true; PPV->SetActorLabel(TEXT("NwiroPostProcessVolume")); }
	}
	if (!PPV) return TEXT("{\"success\":false,\"error\":\"PostProcessVolume not found and could not auto-spawn one\"}");

	FPostProcessSettings& S = PPV->Settings;

	if (Cmd->HasField(TEXT("bloom_intensity"))) { S.bOverride_BloomIntensity = true; S.BloomIntensity = (float)Cmd->GetNumberField(TEXT("bloom_intensity")); }
	if (Cmd->HasField(TEXT("auto_exposure_min"))) { S.bOverride_AutoExposureMinBrightness = true; S.AutoExposureMinBrightness = (float)Cmd->GetNumberField(TEXT("auto_exposure_min")); }
	if (Cmd->HasField(TEXT("auto_exposure_max"))) { S.bOverride_AutoExposureMaxBrightness = true; S.AutoExposureMaxBrightness = (float)Cmd->GetNumberField(TEXT("auto_exposure_max")); }
	if (Cmd->HasField(TEXT("vignette_intensity"))) { S.bOverride_VignetteIntensity = true; S.VignetteIntensity = (float)Cmd->GetNumberField(TEXT("vignette_intensity")); }
	if (Cmd->HasField(TEXT("saturation"))) { float Sat = (float)Cmd->GetNumberField(TEXT("saturation")); S.bOverride_ColorSaturation = true; S.ColorSaturation = FVector4(Sat, Sat, Sat, Sat); }
	if (Cmd->HasField(TEXT("contrast"))) { float Con = (float)Cmd->GetNumberField(TEXT("contrast")); S.bOverride_ColorContrast = true; S.ColorContrast = FVector4(Con, Con, Con, Con); }
	if (Cmd->HasField(TEXT("infinite_extent"))) PPV->bUnbound = Cmd->GetBoolField(TEXT("infinite_extent"));

	PPV->MarkPackageDirty();
	// TODO: PPV is a level actor; persistence needs UEditorLevelLibrary::SaveCurrentLevel on the level package.
	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\"}"), *PPV->GetActorLabel());
}

// ============================================================
// SET FOG
// ============================================================

FString FNwiroIKEnvironmentTools::SetFog(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	UWorld* World = GetEdWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AExponentialHeightFog* Fog = nullptr;
	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	if (!ActorName.IsEmpty())
		Fog = Cast<AExponentialHeightFog>(FindActorByName(World, ActorName));
	else
	{
		for (TActorIterator<AExponentialHeightFog> It(World); It; ++It) { Fog = *It; break; }
	}

	// LLM-friendliness: auto-spawn an ExponentialHeightFog if the level
	// doesn't have one yet. Agents naturally call set_fog without first
	// adding the actor.
	if (!Fog && ActorName.IsEmpty())
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Fog = World->SpawnActor<AExponentialHeightFog>(AExponentialHeightFog::StaticClass(),
			FVector(0, 0, 200), FRotator::ZeroRotator, Params);
		if (Fog) Fog->SetActorLabel(TEXT("NwiroExponentialHeightFog"));
	}

	if (!Fog || !Fog->GetComponent()) return TEXT("{\"success\":false,\"error\":\"ExponentialHeightFog not found and auto-spawn failed\"}");

	UExponentialHeightFogComponent* C = Fog->GetComponent();
	if (Cmd->HasField(TEXT("density"))) C->FogDensity = (float)Cmd->GetNumberField(TEXT("density"));
	if (Cmd->HasField(TEXT("height_falloff"))) C->FogHeightFalloff = (float)Cmd->GetNumberField(TEXT("height_falloff"));
	if (Cmd->HasField(TEXT("start_distance"))) C->StartDistance = (float)Cmd->GetNumberField(TEXT("start_distance"));
	if (Cmd->HasField(TEXT("volumetric"))) C->bEnableVolumetricFog = Cmd->GetBoolField(TEXT("volumetric"));

	C->MarkRenderStateDirty();
	Fog->MarkPackageDirty();
	// TODO: Fog actor lives in the level; persistence needs UEditorLevelLibrary::SaveCurrentLevel.
	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\"}"), *Fog->GetActorLabel());
}

// ============================================================
// SET SKY ATMOSPHERE
// ============================================================

FString FNwiroIKEnvironmentTools::SetSkyAtmosphere(const FString& JsonCommand)
{
	// SkyAtmosphere is configured via its component properties using reflection
	// For now, return a helper message suggesting execute_python or set_actor_property
	return TEXT("{\"success\":true,\"message\":\"Use set_actor_property on your SkyAtmosphere actor to configure atmosphere settings. Key properties: RayleighScattering, MieScattering, GroundAlbedo.\"}");
}

// ============================================================
// SET LIGHT PROPERTIES
// ============================================================

FString FNwiroIKEnvironmentTools::SetLightProperties(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	if (ActorName.IsEmpty()) ActorName = Cmd->GetStringField(TEXT("name"));
	UWorld* World = GetEdWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	ULightComponent* Light = Actor->FindComponentByClass<ULightComponent>();
	if (!Light) return TEXT("{\"success\":false,\"error\":\"No light component found on actor\"}");

	if (Cmd->HasField(TEXT("intensity"))) Light->SetIntensity((float)Cmd->GetNumberField(TEXT("intensity")));
	if (Cmd->HasField(TEXT("color")))
	{
		const TSharedPtr<FJsonObject>* ColorObj;
		if (Cmd->TryGetObjectField(TEXT("color"), ColorObj))
		{
			float CR = (*ColorObj)->HasField(TEXT("r")) ? (float)(*ColorObj)->GetNumberField(TEXT("r")) : 1.0f;
			float CG = (*ColorObj)->HasField(TEXT("g")) ? (float)(*ColorObj)->GetNumberField(TEXT("g")) : 1.0f;
			float CB = (*ColorObj)->HasField(TEXT("b")) ? (float)(*ColorObj)->GetNumberField(TEXT("b")) : 1.0f;
			Light->SetLightColor(FLinearColor(CR, CG, CB));
		}
	}
	if (Cmd->HasField(TEXT("temperature"))) { Light->bUseTemperature = true; Light->SetTemperature((float)Cmd->GetNumberField(TEXT("temperature"))); }
	if (Cmd->HasField(TEXT("cast_shadows"))) Light->SetCastShadows(Cmd->GetBoolField(TEXT("cast_shadows")));
	if (Cmd->HasField(TEXT("source_radius")))
	{
		if (UPointLightComponent* PL = Cast<UPointLightComponent>(Light))
			PL->SourceRadius = (float)Cmd->GetNumberField(TEXT("source_radius"));
	}
	if (Cmd->HasField(TEXT("attenuation_radius")))
	{
		if (UPointLightComponent* PL = Cast<UPointLightComponent>(Light))
			PL->SetAttenuationRadius((float)Cmd->GetNumberField(TEXT("attenuation_radius")));
	}
	if (Cmd->HasField(TEXT("inner_cone_angle")))
	{
		if (USpotLightComponent* SL = Cast<USpotLightComponent>(Light))
			SL->SetInnerConeAngle((float)Cmd->GetNumberField(TEXT("inner_cone_angle")));
	}
	if (Cmd->HasField(TEXT("outer_cone_angle")))
	{
		if (USpotLightComponent* SL = Cast<USpotLightComponent>(Light))
			SL->SetOuterConeAngle((float)Cmd->GetNumberField(TEXT("outer_cone_angle")));
	}

	Light->MarkRenderStateDirty();
	Actor->MarkPackageDirty();
	// TODO: Light actor lives in the level; persistence needs UEditorLevelLibrary::SaveCurrentLevel.
	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"lightType\":\"%s\"}"),
		*Actor->GetActorLabel(), *Light->GetClass()->GetName());
}

// ============================================================
// SET PHYSICS SIMULATION
// ============================================================

FString FNwiroIKEnvironmentTools::SetPhysicsSimulation(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	UPrimitiveComponent* Prim = Actor->FindComponentByClass<UPrimitiveComponent>();
	if (!Prim) return TEXT("{\"success\":false,\"error\":\"No primitive component\"}");

	if (Cmd->HasField(TEXT("simulate"))) Prim->SetSimulatePhysics(Cmd->GetBoolField(TEXT("simulate")));
	if (Cmd->HasField(TEXT("gravity"))) Prim->SetEnableGravity(Cmd->GetBoolField(TEXT("gravity")));
	if (Cmd->HasField(TEXT("mass")))
	{
		Prim->SetMassOverrideInKg(NAME_None, (float)Cmd->GetNumberField(TEXT("mass")));
	}
	if (Cmd->HasField(TEXT("linear_damping"))) Prim->SetLinearDamping((float)Cmd->GetNumberField(TEXT("linear_damping")));
	if (Cmd->HasField(TEXT("angular_damping"))) Prim->SetAngularDamping((float)Cmd->GetNumberField(TEXT("angular_damping")));

	Actor->MarkPackageDirty();
	// TODO: physics actor lives in the level; persistence needs UEditorLevelLibrary::SaveCurrentLevel.
	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\"}"), *Actor->GetActorLabel());
}

// ============================================================
// SET COLLISION PROFILE
// ============================================================

FString FNwiroIKEnvironmentTools::SetCollisionProfile(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	FString Profile = Cmd->GetStringField(TEXT("profile"));

	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	UPrimitiveComponent* Prim = Actor->FindComponentByClass<UPrimitiveComponent>();
	if (!Prim) return TEXT("{\"success\":false,\"error\":\"No primitive component\"}");

	Prim->SetCollisionProfileName(FName(*Profile));
	Actor->MarkPackageDirty();
	// TODO: actor lives in the level; persistence needs UEditorLevelLibrary::SaveCurrentLevel.
	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"profile\":\"%s\"}"), *Actor->GetActorLabel(), *Profile);
}

// ============================================================
// ADD PHYSICS CONSTRAINT
// ============================================================

FString FNwiroIKEnvironmentTools::AddPhysicsConstraint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Actor1Name = Cmd->GetStringField(TEXT("actor1"));
	FString Actor2Name = Cmd->GetStringField(TEXT("actor2"));
	FString ConstraintType = Cmd->GetStringField(TEXT("type"));

	UWorld* World = GetEdWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AActor* A1 = FindActorByName(World, Actor1Name);
	AActor* A2 = FindActorByName(World, Actor2Name);
	if (!A1) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor1 not found: %s\"}"), *Actor1Name);
	if (!A2) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor2 not found: %s\"}"), *Actor2Name);

	FVector MidPoint = (A1->GetActorLocation() + A2->GetActorLocation()) * 0.5f;
	FActorSpawnParameters Params;
	APhysicsConstraintActor* ConstraintActor = World->SpawnActor<APhysicsConstraintActor>(MidPoint, FRotator::ZeroRotator, Params);
	if (!ConstraintActor) return TEXT("{\"success\":false,\"error\":\"Failed to spawn constraint actor\"}");

	UPhysicsConstraintComponent* Constraint = ConstraintActor->GetConstraintComp();
	Constraint->ConstraintActor1 = A1;
	Constraint->ConstraintActor2 = A2;

	FString TypeLower = ConstraintType.ToLower();
	if (TypeLower == TEXT("fixed"))
	{
		Constraint->SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Locked, 0);
		Constraint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Locked, 0);
		Constraint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Locked, 0);
	}
	else if (TypeLower == TEXT("hinge"))
	{
		Constraint->SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Free, 0);
		Constraint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Locked, 0);
		Constraint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Locked, 0);
	}
	else if (TypeLower == TEXT("ballsocket") || TypeLower == TEXT("ball"))
	{
		Constraint->SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0);
		Constraint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Free, 0);
		Constraint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Free, 0);
		Constraint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Free, 0);
	}
	// default: free

	Constraint->InitComponentConstraint();
	return FString::Printf(TEXT("{\"success\":true,\"constraint\":\"%s\",\"type\":\"%s\",\"actor1\":\"%s\",\"actor2\":\"%s\"}"),
		*ConstraintActor->GetActorLabel(), *ConstraintType, *Actor1Name, *Actor2Name);
}

// ============================================================
// GET PHYSICS INFO
// ============================================================

FString FNwiroIKEnvironmentTools::GetPhysicsInfo(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	UPrimitiveComponent* Prim = Actor->FindComponentByClass<UPrimitiveComponent>();
	if (!Prim) return TEXT("{\"success\":false,\"error\":\"No primitive component\"}");

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetBoolField(TEXT("simulatePhysics"), Prim->IsSimulatingPhysics());
	Result->SetBoolField(TEXT("gravityEnabled"), Prim->IsGravityEnabled());
	Result->SetNumberField(TEXT("mass"), Prim->GetMass());
	Result->SetNumberField(TEXT("linearDamping"), Prim->GetLinearDamping());
	Result->SetNumberField(TEXT("angularDamping"), Prim->GetAngularDamping());
	Result->SetStringField(TEXT("collisionProfile"), Prim->GetCollisionProfileName().ToString());

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// CREATE SPLINE ACTOR
// ============================================================

FString FNwiroIKEnvironmentTools::CreateSplineActor(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Label = Cmd->GetStringField(TEXT("label"));
	if (Label.IsEmpty()) Label = TEXT("SplineActor");

	UWorld* World = GetEdWorld();
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass());
	if (!Actor) return TEXT("{\"success\":false,\"error\":\"Failed to spawn actor\"}");

	Actor->SetActorLabel(Label);
	USplineComponent* Spline = NewObject<USplineComponent>(Actor, TEXT("SplineComponent"));
	Spline->RegisterComponent();
	Actor->AddInstanceComponent(Spline);
	Actor->SetRootComponent(Spline);
	Spline->ClearSplinePoints();

	// Add initial points
	const TArray<TSharedPtr<FJsonValue>>* Points;
	if (Cmd->TryGetArrayField(TEXT("points"), Points))
	{
		for (const TSharedPtr<FJsonValue>& PVal : *Points)
		{
			const TSharedPtr<FJsonObject>& PObj = PVal->AsObject();
			if (!PObj.IsValid()) continue;
			double X = PObj->HasField(TEXT("x")) ? PObj->GetNumberField(TEXT("x")) : 0;
			double Y = PObj->HasField(TEXT("y")) ? PObj->GetNumberField(TEXT("y")) : 0;
			double Z = PObj->HasField(TEXT("z")) ? PObj->GetNumberField(TEXT("z")) : 0;
			Spline->AddSplinePoint(FVector(X, Y, Z), ESplineCoordinateSpace::World, true);
		}
	}
	else
	{
		// Default 2-point spline
		Spline->AddSplinePoint(FVector(0, 0, 0), ESplineCoordinateSpace::World, true);
		Spline->AddSplinePoint(FVector(500, 0, 0), ESplineCoordinateSpace::World, true);
	}

	Spline->UpdateSpline();

	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"pointCount\":%d}"),
		*Label, Spline->GetNumberOfSplinePoints());
}

// ============================================================
// ADD SPLINE POINT
// ============================================================

FString FNwiroIKEnvironmentTools::AddSplinePoint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	USplineComponent* Spline = GetOrAddSpline(Actor, /*bAutoAttach=*/true);
	if (!Spline) return TEXT("{\"success\":false,\"error\":\"No spline component (and could not attach one)\"}");

	double X = Cmd->HasField(TEXT("x")) ? Cmd->GetNumberField(TEXT("x")) : 0;
	double Y = Cmd->HasField(TEXT("y")) ? Cmd->GetNumberField(TEXT("y")) : 0;
	double Z = Cmd->HasField(TEXT("z")) ? Cmd->GetNumberField(TEXT("z")) : 0;

	Spline->AddSplinePoint(FVector(X, Y, Z), ESplineCoordinateSpace::World, true);
	Spline->UpdateSpline();

	return FString::Printf(TEXT("{\"success\":true,\"pointCount\":%d}"), Spline->GetNumberOfSplinePoints());
}

// ============================================================
// SET SPLINE POINT
// ============================================================

FString FNwiroIKEnvironmentTools::SetSplinePoint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	int32 Index = (int32)Cmd->GetNumberField(TEXT("index"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	USplineComponent* Spline = GetOrAddSpline(Actor, /*bAutoAttach=*/true);
	if (!Spline) return TEXT("{\"success\":false,\"error\":\"No spline component (and could not attach one)\"}");
	if (Index < 0 || Index >= Spline->GetNumberOfSplinePoints())
		return TEXT("{\"success\":false,\"error\":\"Index out of range\"}");

	double X = Cmd->HasField(TEXT("x")) ? Cmd->GetNumberField(TEXT("x")) : Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World).X;
	double Y = Cmd->HasField(TEXT("y")) ? Cmd->GetNumberField(TEXT("y")) : Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World).Y;
	double Z = Cmd->HasField(TEXT("z")) ? Cmd->GetNumberField(TEXT("z")) : Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World).Z;

	Spline->SetLocationAtSplinePoint(Index, FVector(X, Y, Z), ESplineCoordinateSpace::World, true);
	Spline->UpdateSpline();

	return FString::Printf(TEXT("{\"success\":true,\"index\":%d}"), Index);
}

// ============================================================
// REMOVE SPLINE POINT
// ============================================================

FString FNwiroIKEnvironmentTools::RemoveSplinePoint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	int32 Index = (int32)Cmd->GetNumberField(TEXT("index"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	USplineComponent* Spline = GetOrAddSpline(Actor, /*bAutoAttach=*/true);
	if (!Spline) return TEXT("{\"success\":false,\"error\":\"No spline component (and could not attach one)\"}");

	Spline->RemoveSplinePoint(Index, true);
	Spline->UpdateSpline();
	return FString::Printf(TEXT("{\"success\":true,\"pointCount\":%d}"), Spline->GetNumberOfSplinePoints());
}

// ============================================================
// GET SPLINE INFO
// ============================================================

FString FNwiroIKEnvironmentTools::GetSplineInfo(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	USplineComponent* Spline = GetOrAddSpline(Actor, /*bAutoAttach=*/true);
	if (!Spline) return TEXT("{\"success\":false,\"error\":\"No spline component (and could not attach one)\"}");

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("pointCount"), Spline->GetNumberOfSplinePoints());
	Result->SetNumberField(TEXT("length"), Spline->GetSplineLength());
	Result->SetBoolField(TEXT("closed"), Spline->IsClosedLoop());

	TArray<TSharedPtr<FJsonValue>> Pts;
	for (int32 i = 0; i < Spline->GetNumberOfSplinePoints(); i++)
	{
		FVector Loc = Spline->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
		TSharedRef<FJsonObject> P = MakeShareable(new FJsonObject());
		P->SetNumberField(TEXT("index"), i);
		P->SetNumberField(TEXT("x"), Loc.X);
		P->SetNumberField(TEXT("y"), Loc.Y);
		P->SetNumberField(TEXT("z"), Loc.Z);
		Pts.Add(MakeShareable(new FJsonValueObject(P)));
	}
	Result->SetArrayField(TEXT("points"), Pts);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// SET SPLINE CLOSED
// ============================================================

FString FNwiroIKEnvironmentTools::SetSplineClosed(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	bool bClosed = Cmd->GetBoolField(TEXT("closed"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	USplineComponent* Spline = GetOrAddSpline(Actor, /*bAutoAttach=*/true);
	if (!Spline) return TEXT("{\"success\":false,\"error\":\"No spline component (and could not attach one)\"}");

	Spline->SetClosedLoop(bClosed, true);
	Spline->UpdateSpline();
	return FString::Printf(TEXT("{\"success\":true,\"closed\":%s}"), bClosed ? TEXT("true") : TEXT("false"));
}

// ============================================================
// SET SPLINE POINT TYPE
// ============================================================

FString FNwiroIKEnvironmentTools::SetSplinePointType(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString ActorName = Cmd->GetStringField(TEXT("actor"));
	int32 Index = (int32)Cmd->GetNumberField(TEXT("index"));
	FString Type = Cmd->GetStringField(TEXT("type"));
	UWorld* World = GetEdWorld();
	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	USplineComponent* Spline = GetOrAddSpline(Actor, /*bAutoAttach=*/true);
	if (!Spline) return TEXT("{\"success\":false,\"error\":\"No spline component (and could not attach one)\"}");

	ESplinePointType::Type PointType = ESplinePointType::Curve;
	FString TypeLower = Type.ToLower();
	if (TypeLower == TEXT("linear")) PointType = ESplinePointType::Linear;
	else if (TypeLower == TEXT("constant")) PointType = ESplinePointType::Constant;
	else if (TypeLower == TEXT("curveclamped") || TypeLower == TEXT("clamped")) PointType = ESplinePointType::CurveClamped;

	Spline->SetSplinePointType(Index, PointType, true);
	Spline->UpdateSpline();
	return FString::Printf(TEXT("{\"success\":true,\"index\":%d,\"type\":\"%s\"}"), Index, *Type);
}
