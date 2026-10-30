// Copyright 2026 Nwiro. All Rights Reserved.

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKMCPAnnotationsSpec,
	"NwiroIK.MCP.ToolAnnotations",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKMCPAnnotationsSpec::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Request = MakeShareable(new FJsonObject);
	Request->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Request->SetNumberField(TEXT("id"), 1);
	Request->SetStringField(TEXT("method"), TEXT("tools/list"));
	Request->SetObjectField(TEXT("params"), MakeShareable(new FJsonObject));

	const TSharedPtr<FJsonObject> Response = FNwiroIKMCPServer::ProcessJsonRpc(Request);
	if (!TestTrue(TEXT("tools/list returns a response"), Response.IsValid())) return false;

	const TSharedPtr<FJsonObject>* Result = nullptr;
	if (!TestTrue(TEXT("tools/list response contains result"),
		Response->TryGetObjectField(TEXT("result"), Result) && Result && (*Result).IsValid()))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
	if (!TestTrue(TEXT("tools/list result contains tools"),
		(*Result)->TryGetArrayField(TEXT("tools"), Tools) && Tools))
	{
		return false;
	}
	TestTrue(TEXT("tools/list is non-empty"), Tools->Num() > 0);

	TMap<FString, bool> ReadOnlyByName;
	for (const TSharedPtr<FJsonValue>& ToolValue : *Tools)
	{
		if (!TestTrue(TEXT("tool entry is an object"),
			ToolValue.IsValid() && ToolValue->Type == EJson::Object))
		{
			continue;
		}

		const TSharedPtr<FJsonObject> Tool = ToolValue->AsObject();
		FString ToolName;
		if (!TestTrue(TEXT("tool entry has a name"),
			Tool.IsValid() && Tool->TryGetStringField(TEXT("name"), ToolName) && !ToolName.IsEmpty()))
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* Annotations = nullptr;
		const FString AnnotationLabel = FString::Printf(TEXT("%s has annotations"), *ToolName);
		if (!TestTrue(*AnnotationLabel,
			Tool->TryGetObjectField(TEXT("annotations"), Annotations)
				&& Annotations && (*Annotations).IsValid()))
		{
			continue;
		}

		bool bReadOnly = false;
		bool bDestructive = false;
		bool bIdempotent = false;
		TestTrue(*FString::Printf(TEXT("%s has readOnlyHint"), *ToolName),
			(*Annotations)->TryGetBoolField(TEXT("readOnlyHint"), bReadOnly));
		TestTrue(*FString::Printf(TEXT("%s has destructiveHint"), *ToolName),
			(*Annotations)->TryGetBoolField(TEXT("destructiveHint"), bDestructive));
		TestTrue(*FString::Printf(TEXT("%s has idempotentHint"), *ToolName),
			(*Annotations)->TryGetBoolField(TEXT("idempotentHint"), bIdempotent));

		TestEqual(*FString::Printf(TEXT("%s destructive classification is conservative"), *ToolName),
			bDestructive, !bReadOnly);
		TestEqual(*FString::Printf(TEXT("%s idempotence matches read-only classification"), *ToolName),
			bIdempotent, bReadOnly);
		ReadOnlyByName.Add(ToolName, bReadOnly);
	}

	auto ExpectReadOnly = [this, &ReadOnlyByName](const TCHAR* ToolName, bool bExpected)
	{
		const bool* Actual = ReadOnlyByName.Find(ToolName);
		const FString PresenceLabel = FString::Printf(TEXT("tools/list contains %s"), ToolName);
		if (!TestTrue(*PresenceLabel, Actual != nullptr)) return;
		TestEqual(*FString::Printf(TEXT("%s readOnlyHint"), ToolName), *Actual, bExpected);
	};

	ExpectReadOnly(TEXT("find_blueprints"), true);
	ExpectReadOnly(TEXT("find_assets"), true);
	ExpectReadOnly(TEXT("read_blueprint"), true);
	ExpectReadOnly(TEXT("list_voices_elevenlabs"), true);

	ExpectReadOnly(TEXT("create_blueprint"), false);
	ExpectReadOnly(TEXT("edit_blueprint"), false);
	ExpectReadOnly(TEXT("render_blueprint_graph"), false);
	ExpectReadOnly(TEXT("clear_graph"), false);
	ExpectReadOnly(TEXT("execute_python"), false);

	// These names look read-only, but their implementations have side effects.
	ExpectReadOnly(TEXT("bp_get_compile_errors"), false);
	ExpectReadOnly(TEXT("get_asset_thumbnail"), false);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNwiroIKMCPToolEventPayloadSpec,
	"NwiroIK.MCP.ToolEventPayload",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNwiroIKMCPToolEventPayloadSpec::RunTest(const FString& Parameters)
{
	const FString Input = TEXT("{\"name\":\"BP_ToolVis_CX\",\"path\":\"/Game/HarnessTest\"}");
	const TSharedPtr<FJsonObject> Start =
		MakeMcpToolStartEventData(TEXT("create_blueprint"), TEXT("42"), Input);

	TestTrue(TEXT("start payload is valid"), Start.IsValid());
	TestEqual(TEXT("start payload name"), Start->GetStringField(TEXT("name")), TEXT("create_blueprint"));
	TestEqual(TEXT("start payload id"), Start->GetStringField(TEXT("id")), TEXT("42"));
	TestEqual(TEXT("start payload preserves arguments"), Start->GetStringField(TEXT("input")), Input);

	const FString Output = TEXT("{\"success\":true,\"assetPath\":\"/Game/HarnessTest/BP_ToolVis_CX\"}");
	const TSharedPtr<FJsonObject> End =
		MakeMcpToolEndEventData(TEXT("create_blueprint"), TEXT("42"), Output, true);

	TestTrue(TEXT("end payload is valid"), End.IsValid());
	TestEqual(TEXT("end payload name"), End->GetStringField(TEXT("name")), TEXT("create_blueprint"));
	TestEqual(TEXT("end payload id"), End->GetStringField(TEXT("id")), TEXT("42"));
	TestEqual(TEXT("end payload preserves result"), End->GetStringField(TEXT("output")), Output);
	TestTrue(TEXT("end payload records success"), End->GetBoolField(TEXT("ok")));

	const FString BoundaryPayload = FString::ChrN(MaxMcpToolUiPayloadChars, TEXT('b'));
	TestEqual(TEXT("payload at display cap is unchanged"),
		TruncateMcpToolUiPayload(BoundaryPayload), BoundaryPayload);

	const FString LargeInput = FString::ChrN(MaxMcpToolUiPayloadChars + 512, TEXT('i'));
	const TSharedPtr<FJsonObject> LargeStart =
		MakeMcpToolStartEventData(TEXT("read_blueprint"), TEXT("43"), LargeInput);
	const FString DisplayInput = LargeStart->GetStringField(TEXT("input"));
	TestEqual(TEXT("large input is capped including suffix"),
		DisplayInput.Len(), MaxMcpToolUiPayloadChars);
	TestTrue(TEXT("large input records original length"), DisplayInput.EndsWith(
		FString::Printf(TEXT("...[truncated, %d chars total]"), LargeInput.Len())));

	const FString LargeOutput = FString::ChrN(MaxMcpToolUiPayloadChars + 1024, TEXT('o'));
	const TSharedPtr<FJsonObject> LargeEnd =
		MakeMcpToolEndEventData(TEXT("read_blueprint"), TEXT("43"), LargeOutput, true);
	const FString DisplayOutput = LargeEnd->GetStringField(TEXT("output"));
	TestEqual(TEXT("large output is capped including suffix"),
		DisplayOutput.Len(), MaxMcpToolUiPayloadChars);
	TestTrue(TEXT("large output records original length"), DisplayOutput.EndsWith(
		FString::Printf(TEXT("...[truncated, %d chars total]"), LargeOutput.Len())));

	return true;
}
