// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

/**
 * Editor utility tools: screenshot, log reading, PIE control.
 */

// Custom output device to capture recent log entries
class FNwiroIKLogCapture : public FOutputDevice
{
public:
	static FNwiroIKLogCapture& Get();

	void Register();
	void Unregister();

	TArray<FString> GetRecentLines(int32 Count, const FString& SeverityFilter = TEXT(""), const FString& CategoryFilter = TEXT("")) const;

protected:
	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;

private:
	mutable FCriticalSection Lock;
	TArray<FString> Lines;
	static const int32 MaxLines = 1000;
};

class FNwiroIKEditorTools
{
public:
	// Capture editor viewport screenshot
	static FString TakeScreenshot(const FString& JsonCommand);

	// Read recent output log entries
	static FString ReadLog(const FString& JsonCommand);

	// Start Play in Editor
	static FString PlayInEditor(const FString& JsonCommand);

	// Stop Play in Editor
	static FString StopPIE(const FString& JsonCommand);
};
