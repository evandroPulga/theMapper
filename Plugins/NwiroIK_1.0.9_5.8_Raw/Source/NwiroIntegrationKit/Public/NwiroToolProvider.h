// Copyright 2026 Nwiro. All Rights Reserved.
//
// Nwiro Tool Provider SDK — v1 (header-only)
// ------------------------------------------------------------------------------
// Vendor this SINGLE header into your own UE5 plugin to expose your C++ methods
// as Nwiro MCP tools. It depends ONLY on engine Core (CoreMinimal + the engine's
// Modular Features registry). You take NO compile-time or link-time dependency on
// Nwiro: if neither Nwiro product is installed, your RegisterModularFeature() call
// is a harmless no-op and your plugin compiles and runs exactly as before.
//
// Both Nwiro products (Nwiro Pro and Nwiro Integration Kit) enumerate providers
// registered under INwiroToolProvider::GetFeatureName() at session start,
// namespace their tools as `plugin.<your-id>.<tool>`, and route dispatch back to
// Execute().
//
// Usage (in your editor module's StartupModule / ShutdownModule):
//
//     // MyProvider implements INwiroToolProvider
//     IModularFeatures::Get().RegisterModularFeature(
//         INwiroToolProvider::GetFeatureName(), &MyProviderInstance);
//     // ... and on shutdown:
//     IModularFeatures::Get().UnregisterModularFeature(
//         INwiroToolProvider::GetFeatureName(), &MyProviderInstance);

#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"
#include "Features/IModularFeatures.h"

/**
 * Risk classification for a tool — drives Nwiro's approval / permission UX.
 * If a provider omits it, the host treats the tool as the strictest class.
 */
enum class ENwiroToolRisk : uint8
{
	ReadOnly,    // no side effects (queries, inspection)
	Mutating,    // changes project/editor state, generally reversible
	Destructive, // deletes or otherwise irreversible
};

/**
 * One tool a provider exposes. `Name` is the LEAF name only — Nwiro assigns the
 * authoritative namespace prefix (`plugin.<verified-id>.`) so third parties
 * cannot squat reserved names.
 */
struct FNwiroToolDef
{
	/** Leaf tool name, lowercase [a-z0-9_], e.g. "build_road". */
	FString Name;

	/** Human/LLM-facing description of what the tool does. */
	FString Description;

	/** JSON-Schema object (as a string) describing the arguments.
	 *  Empty => treated as a no-argument tool. */
	FString InputSchemaJson;

	/** Risk class — drives the approval UX. Defaults to Mutating. */
	ENwiroToolRisk Risk = ENwiroToolRisk::Mutating;

	/** Run Execute() on the game thread (default, safe for UObject/editor work).
	 *  Set false only if your handler is fully thread-safe. */
	bool bGameThread = true;
};

/**
 * Implement this interface and register the instance as a modular feature under
 * INwiroToolProvider::GetFeatureName(). Keep the instance alive while registered.
 */
class INwiroToolProvider : public IModularFeature
{
public:
	/** Modular-feature name both Nwiro products look up. Must match exactly. */
	static FName GetFeatureName() { return FName(TEXT("NwiroToolProvider")); }

	/** Contract version. Bumped when the interface changes incompatibly. */
	static constexpr int32 ApiVersion = 1;

	virtual ~INwiroToolProvider() = default;

	/** Contract version this provider was built against. The host skips providers
	 *  built against a NEWER contract than it understands. */
	virtual int32 GetNwiroToolApiVersion() const { return ApiVersion; }

	/** Stable identity of the owning plugin (lowercase, dots allowed),
	 *  e.g. "acme.roads". Becomes the authority segment: `plugin.<id>.<tool>`.
	 *  Reserved heads (nwiro/ue/system/internal/debug/mcp) are rejected. */
	virtual FString GetProviderId() const = 0;

	/** Friendly name for the settings UI. Defaults to the provider id. */
	virtual FString GetDisplayName() const { return GetProviderId(); }

	/** Fill OutTools with the tools this provider exposes. Called at session
	 *  start — keep it cheap and deterministic. */
	virtual void GetToolDefinitions(TArray<FNwiroToolDef>& OutTools) const = 0;

	/** Run a tool by its LEAF name (as given in GetToolDefinitions). Returns a
	 *  JSON result string. Invoked on the game thread unless the def opted out. */
	virtual FString Execute(const FString& ToolName, const FString& ArgsJson) = 0;
};
