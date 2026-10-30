// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FNwiroIKModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	// Handle for OnEnginePreExit — Slate-owned panel teardown runs there (while
	// Slate is guaranteed alive) instead of relying on ShutdownModule ordering.
	FDelegateHandle EnginePreExitHandle;
};
