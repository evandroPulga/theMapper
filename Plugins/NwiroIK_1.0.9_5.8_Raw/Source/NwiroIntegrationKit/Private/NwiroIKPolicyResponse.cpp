// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKPolicyResponse.h"
#include "Misc/Guid.h"

namespace NwiroIKPolicy
{

TArray<TSharedPtr<FJsonValue>> MakeEmptyJsonArray()
{
	return TArray<TSharedPtr<FJsonValue>>();
}

void SetStringArrayField(const TSharedRef<FJsonObject>& Obj, const FString& Field, const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FString& Value : Values)
	{
		Arr.Add(MakeShareable(new FJsonValueString(Value)));
	}
	Obj->SetArrayField(Field, Arr);
}

TSharedPtr<FJsonValue> MakePolicyDiagnostic(
	const FString& Code,
	const FString& Op,
	const FString& Field,
	const FString& Received,
	const TArray<FString>& Allowed,
	const FString& Message
)
{
	TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject());
	Obj->SetStringField(TEXT("code"), Code);
	Obj->SetStringField(TEXT("op"), Op);
	Obj->SetStringField(TEXT("field"), Field);
	Obj->SetStringField(TEXT("received"), Received);
	SetStringArrayField(Obj, TEXT("allowed"), Allowed);
	if (!Message.IsEmpty())
	{
		Obj->SetStringField(TEXT("message"), Message);
	}
	return MakeShareable(new FJsonValueObject(Obj));
}

FString SerializeJsonObject(const TSharedRef<FJsonObject>& Obj)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);
	return Out;
}

FString MakeCallId(const TSharedPtr<FJsonObject>& Cmd)
{
	FString CallId;
	if (Cmd.IsValid())
	{
		Cmd->TryGetStringField(TEXT("_callId"), CallId);
	}
	return CallId.IsEmpty() ? FGuid::NewGuid().ToString(EGuidFormats::Digits) : CallId;
}

FString MakePolicyFailureResponse(
	const FString& CallId,
	const FString& Error,
	const TArray<TSharedPtr<FJsonValue>>& Errors,
	const TArray<TSharedPtr<FJsonValue>>& Warnings
)
{
	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), false);
	Result->SetStringField(TEXT("policy_version"), TEXT("1.1"));
	Result->SetStringField(TEXT("_callId"), CallId.IsEmpty() ? FGuid::NewGuid().ToString(EGuidFormats::Digits) : CallId);
	Result->SetStringField(TEXT("error"), Error);
	Result->SetArrayField(TEXT("messages"), MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("created"), MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("modified"), MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("skipped"), MakeEmptyJsonArray());
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetArrayField(TEXT("errors"), Errors);
	return SerializeJsonObject(Result);
}

bool JsonFieldIsType(const TSharedPtr<FJsonObject>& Obj, const FString& Key, EJson Type)
{
	// TryGetField avoids the FString/FSharedString key-type mismatch in UE 5.8
	// while remaining compatible with UE 5.7.
	TSharedPtr<FJsonValue> Found = Obj->TryGetField(Key);
	return Found.IsValid() && Found->Type == Type;
}

void ValidateJsonArrayObjects(
	const TSharedPtr<FJsonObject>& Cmd,
	const FString& Op,
	const TSet<FString>& AllowedKeys,
	const TArray<FString>& RequiredAny,
	const TArray<FString>& RequiredAll,
	TArray<TSharedPtr<FJsonValue>>& Errors
)
{
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Cmd->TryGetArrayField(Op, Arr) || !Arr)
	{
		return;
	}

	TArray<FString> AllowedList = AllowedKeys.Array();
	AllowedList.Sort();

	for (int32 Index = 0; Index < Arr->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Item = (*Arr)[Index];
		if (!Item.IsValid() || Item->Type != EJson::Object)
		{
			const FString Field = FString::Printf(TEXT("%s[%d]"), *Op, Index);
			Errors.Add(MakePolicyDiagnostic(TEXT("INVALID_TYPE"), Op, Field, TEXT("non-object"), { TEXT("object") }));
			continue;
		}

		const TSharedPtr<FJsonObject>& ItemObj = Item->AsObject();
		for (const auto& Pair : ItemObj->Values)
		{
			const FString Key(*Pair.Key);
			if (!AllowedKeys.Contains(Key))
			{
				const FString Field = FString::Printf(TEXT("%s[%d].%s"), *Op, Index, *Key);
				Errors.Add(MakePolicyDiagnostic(TEXT("UNKNOWN_KEY"), Op, Field, Key, AllowedList));
			}
		}

		for (const FString& Required : RequiredAll)
		{
			if (!ItemObj->HasField(Required))
			{
				const FString Field = FString::Printf(TEXT("%s[%d].%s"), *Op, Index, *Required);
				Errors.Add(MakePolicyDiagnostic(TEXT("MISSING_REQUIRED_FIELD"), Op, Field, TEXT("missing"), { Required }));
			}
		}

		if (RequiredAny.Num() > 0)
		{
			bool bHasAny = false;
			for (const FString& Required : RequiredAny)
			{
				if (ItemObj->HasField(Required))
				{
					bHasAny = true;
					break;
				}
			}
			if (!bHasAny)
			{
				const FString Field = FString::Printf(TEXT("%s[%d]"), *Op, Index);
				Errors.Add(MakePolicyDiagnostic(TEXT("MISSING_REQUIRED_FIELD"), Op, Field, TEXT("missing"), RequiredAny));
			}
		}
	}
}

TSharedPtr<FJsonValue> ConvertDiagnosticForPolicy(const TSharedPtr<FJsonValue>& Value, const FString& DefaultOp)
{
	if (!Value.IsValid() || Value->Type != EJson::Object)
	{
		return MakePolicyDiagnostic(TEXT("INTERNAL_ERROR"), DefaultOp, TEXT("diagnostic"), TEXT("invalid"), {});
	}

	const TSharedPtr<FJsonObject>& In = Value->AsObject();
	FString Code = In->HasField(TEXT("code")) ? In->GetStringField(TEXT("code")) : TEXT("INTERNAL_ERROR");
	FString Op = DefaultOp;
	In->TryGetStringField(TEXT("op"), Op);
	if (Op == DefaultOp)
	{
		In->TryGetStringField(TEXT("operation"), Op);
	}

	FString Field;
	In->TryGetStringField(TEXT("field"), Field);
	FString Received;
	In->TryGetStringField(TEXT("received"), Received);
	if (Received.IsEmpty())
	{
		In->TryGetStringField(TEXT("value"), Received);
	}
	FString Message;
	In->TryGetStringField(TEXT("message"), Message);

	TArray<FString> Allowed;
	const TArray<TSharedPtr<FJsonValue>>* AllowedIn = nullptr;
	if (In->TryGetArrayField(TEXT("allowed"), AllowedIn) && AllowedIn)
	{
		for (const TSharedPtr<FJsonValue>& Item : *AllowedIn)
		{
			if (Item.IsValid())
			{
				Allowed.Add(Item->AsString());
			}
		}
	}
	return MakePolicyDiagnostic(Code, Op, Field, Received, Allowed, Message);
}

} // namespace NwiroIKPolicy
