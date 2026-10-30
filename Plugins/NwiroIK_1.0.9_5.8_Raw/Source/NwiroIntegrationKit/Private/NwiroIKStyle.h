// Copyright 2026 Nwiro. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"

class FNwiroIKStyle
{
public:
    static void Initialize();
    static void Shutdown();
    static TSharedPtr<ISlateStyle> Get() { return StyleInstance; }
    static FName GetStyleSetName() { return "NwiroIKStyle"; }

private:
    static TSharedPtr<FSlateStyleSet> StyleInstance;
};
