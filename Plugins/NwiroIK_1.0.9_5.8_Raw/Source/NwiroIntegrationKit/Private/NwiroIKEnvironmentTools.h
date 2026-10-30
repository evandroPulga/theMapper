// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Environment, Physics, and Spline tools.
 */
class FNwiroIKEnvironmentTools
{
public:
	// === Environment ===
	static FString SetPostProcess(const FString& JsonCommand);
	static FString SetFog(const FString& JsonCommand);
	static FString SetSkyAtmosphere(const FString& JsonCommand);
	static FString SetLightProperties(const FString& JsonCommand);

	// === Physics ===
	static FString SetPhysicsSimulation(const FString& JsonCommand);
	static FString SetCollisionProfile(const FString& JsonCommand);
	static FString AddPhysicsConstraint(const FString& JsonCommand);
	static FString GetPhysicsInfo(const FString& JsonCommand);

	// === Spline ===
	static FString CreateSplineActor(const FString& JsonCommand);
	static FString AddSplinePoint(const FString& JsonCommand);
	static FString SetSplinePoint(const FString& JsonCommand);
	static FString RemoveSplinePoint(const FString& JsonCommand);
	static FString GetSplineInfo(const FString& JsonCommand);
	static FString SetSplineClosed(const FString& JsonCommand);
	static FString SetSplinePointType(const FString& JsonCommand);
};
