// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Niagara VFX tools: create systems, read parameters.
 */
class FNwiroIKNiagaraTools
{
public:
	static FString CreateNiagaraSystem(const FString& JsonCommand);
	static FString ReadNiagaraSystem(const FString& JsonCommand);
	static FString SetNiagaraParameter(const FString& JsonCommand);
	static FString AddNiagaraEmitter(const FString& JsonCommand);
};
