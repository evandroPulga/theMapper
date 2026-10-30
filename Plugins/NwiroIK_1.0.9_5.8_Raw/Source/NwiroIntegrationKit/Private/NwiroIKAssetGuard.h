// Copyright 2026 Nwiro. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/UObjectGlobals.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/PackageName.h"

// Guard against the "Cannot replace existing object of a different class" fatal
// (UObjectGlobals StaticAllocateObject): creating an asset where an object of a
// DIFFERENT class already lives at the same object path crashes the editor.
// Returns a JSON error string if such a conflict exists; empty string => safe.
// Discovered via fuzz: create_struct then create_enum at the same name+path.
inline FString NwiroCheckCreateConflict(const FString& PackagePath, const FString& AssetName, const UClass* DesiredClass)
{
	if (AssetName.IsEmpty()) return FString();
	// Reject names containing characters illegal in UE object names: they crash
	// downstream FName / package creation and corrupt object-path lookups (so the
	// collision check below cannot even find a conflicting asset). (fuzz: sql-inject name)
	{
		const FString InvalidChars = TEXT("\"' ,;.:/\\*?<>|\n\r\t");
		int32 _Idx;
		for (const TCHAR Ch : AssetName)
			if (Ch < 0x20 || InvalidChars.FindChar(Ch, _Idx))
				return TEXT("{\"success\":false,\"error\":\"Invalid asset name: contains characters not allowed in an asset name (quotes, spaces, punctuation, or path separators).\"}");
	}
	const FString ObjectPath = PackagePath + TEXT(".") + AssetName;
	const UObject* Existing = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath);
	if (Existing && DesiredClass && !Existing->IsA(DesiredClass))
	{
		return FString::Printf(
			TEXT("{\"success\":false,\"error\":\"An asset of a different type ('%s') already exists at '%s'. Pick a different name or delete the existing asset first.\"}"),
			*Existing->GetClass()->GetName(), *ObjectPath);
	}
	return FString();
}

// Safe asset-registry load. The dominant editor crash in this plugin is GetAsset()
// force-loading a compile-failed Blueprint whose native parent class is not in memory:
// the generated class / CDO is null and the load derefs null (read 0x8) -> ACCESS_VIOLATION.
// For an already-loaded asset, or any non-Blueprint type, this is a passthrough to GetAsset().
// For an UNLOADED Blueprint we first verify the native parent class is resolvable WITHOUT
// loading (asset-registry tag + FindObject); if it is not, we refuse the load and return null
// instead of crashing. Callers already null-check the result. (See the ListGASAssets fix.)
inline UObject* NwiroSafeRegistryLoad(const FAssetData& A)
{
	if (A.IsAssetLoaded()) return A.GetAsset();
	const FString AssetClass = A.AssetClassPath.GetAssetName().ToString();
	if (AssetClass.Contains(TEXT("Blueprint")))
	{
		FString NativeParent;
		if (A.GetTagValue(TEXT("NativeParentClass"), NativeParent) && !NativeParent.IsEmpty())
		{
			// Tag is in UE export-text form, e.g.
			// "/Script/CoreUObject.Class'/Script/LyraGame.LyraCharacter'".
			// ExportTextPathToObjectPath extracts the bare object path; a plain
			// "/Script/Mod.Class" (no wrapper) passes through unchanged.
			const FString ClassPath = FPackageName::ExportTextPathToObjectPath(NativeParent);
			if (!ClassPath.IsEmpty() && !FindObject<UClass>(nullptr, *ClassPath))
				return nullptr; // native parent not in memory -> would null-CDO-crash; refuse
		}
	}
	return A.GetAsset();
}
