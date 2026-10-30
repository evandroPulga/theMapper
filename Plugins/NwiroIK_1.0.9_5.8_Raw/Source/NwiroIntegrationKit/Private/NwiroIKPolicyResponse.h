// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Json.h"

// Shared policy 1.1 response/envelope helpers per rule 16A.
//
// This is the NwiroIntegrationKit copy of the helper. The Nwiro plugin maintains
// its own copy at Nwiro/Private/NwiroPolicyResponse.h (namespace NwiroPolicy)
// because the two plugins are independent and do not depend on each other. The
// two copies MUST expose the same API surface and produce structurally identical
// responses for matched inputs. See `Design Documents/material-function-tools-new-schema-migration.md`
// §4.1 for the drift discipline.
namespace NwiroIKPolicy
{
	// Construct an empty JSON value array. Convenience for envelope fields that
	// must be present even when empty (messages, created, modified, skipped,
	// warnings, errors).
	TArray<TSharedPtr<FJsonValue>> MakeEmptyJsonArray();

	// Set a string-array JSON field on an object.
	void SetStringArrayField(const TSharedRef<FJsonObject>& Obj, const FString& Field, const TArray<FString>& Values);

	// Build a policy 1.1 diagnostic object with the canonical shape:
	//   { code, op, field, received, allowed[, message] }
	// `allowed` may be empty when there is no useful copyable replacement value
	// (PERMISSION_DENIED, PROTECTED_PATH, etc.). `Message` is optional context.
	TSharedPtr<FJsonValue> MakePolicyDiagnostic(
		const FString& Code,
		const FString& Op,
		const FString& Field,
		const FString& Received,
		const TArray<FString>& Allowed,
		const FString& Message = TEXT("")
	);

	// Serialize a JSON object using the condensed print policy.
	FString SerializeJsonObject(const TSharedRef<FJsonObject>& Obj);

	// Echo `_callId` from input if present and non-empty; otherwise mint a
	// fresh GUID. Per rule 16C.
	FString MakeCallId(const TSharedPtr<FJsonObject>& Cmd);

	// Build a policy 1.1 failure envelope:
	//   { success:false, policy_version:"1.1", _callId, error, messages[], created[],
	//     modified[], skipped[], warnings[], errors[] }
	// `Errors` and `Warnings` are the structured diagnostic arrays. `Error` is the
	// short human-readable top-level summary string (rule 8A).
	FString MakePolicyFailureResponse(
		const FString& CallId,
		const FString& Error,
		const TArray<TSharedPtr<FJsonValue>>& Errors,
		const TArray<TSharedPtr<FJsonValue>>& Warnings = TArray<TSharedPtr<FJsonValue>>()
	);

	// True when `Key` exists on `Obj` and its value is of the requested EJson type.
	bool JsonFieldIsType(const TSharedPtr<FJsonObject>& Obj, const FString& Key, EJson Type);

	// Validate that the array field `Op` on `Cmd` is an array of objects whose
	// keys are all in `AllowedKeys`, that each object has at least one of the
	// keys in `RequiredAny` (when non-empty), and all of the keys in `RequiredAll`.
	// Appends UNKNOWN_KEY / INVALID_TYPE / MISSING_REQUIRED_FIELD diagnostics to
	// `Errors` per rule 4A. Caller decides whether to short-circuit before mutation.
	void ValidateJsonArrayObjects(
		const TSharedPtr<FJsonObject>& Cmd,
		const FString& Op,
		const TSet<FString>& AllowedKeys,
		const TArray<FString>& RequiredAny,
		const TArray<FString>& RequiredAll,
		TArray<TSharedPtr<FJsonValue>>& Errors
	);

	// Convert a legacy diagnostic JSON value (older { code, message, operation,
	// ref, field, value, allowed } shape) into the policy 1.1 shape, defaulting
	// missing fields. Used by wrappers that consume legacy tool output.
	TSharedPtr<FJsonValue> ConvertDiagnosticForPolicy(const TSharedPtr<FJsonValue>& Value, const FString& DefaultOp);
}
