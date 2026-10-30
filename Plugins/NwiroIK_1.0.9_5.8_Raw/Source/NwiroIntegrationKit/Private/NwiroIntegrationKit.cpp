// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIK.h"
#include "NwiroIKMCPServer.h"
#include "NwiroIKPanel.h"
#include "NwiroIKStyle.h"
#include "NwiroIKEditorTools.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CoreDelegates.h"
#include "Json.h"

#define LOCTEXT_NAMESPACE "FNwiroIKIntegrationKitModule"

// ============================================================
// Config — Saved/NwiroIntegrationKit/config.json
// ============================================================

static FString GetConfigPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NwiroIntegrationKit"), TEXT("config.json"));
}

static TSharedPtr<FJsonObject> LoadConfig()
{
	FString Content;
	if (FFileHelper::LoadFileToString(Content, *GetConfigPath()))
	{
		TSharedPtr<FJsonObject> Json;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
		if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
		{
			return Json;
		}
	}
	return MakeShareable(new FJsonObject);
}

static void SaveConfig(TSharedPtr<FJsonObject> Json)
{
	FString ConfigPath = GetConfigPath();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*FPaths::GetPath(ConfigPath));

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
	FFileHelper::SaveStringToFile(Output, *ConfigPath);
}

static int32 LoadSavedPort()
{
	auto Json = LoadConfig();
	return Json->HasField(TEXT("mcpPort")) ? (int32)Json->GetNumberField(TEXT("mcpPort")) : 5353;
}

void SaveMCPConfig(int32 Port)
{
	auto Json = LoadConfig();
	Json->SetNumberField(TEXT("mcpPort"), Port);
	SaveConfig(Json);
}

// ============================================================
// Secrets
// ============================================================

FString LoadNwiroSecret(const FString& Key)
{
	auto Json = LoadConfig();
	if (Json->HasTypedField<EJson::Object>(TEXT("secrets")))
	{
		auto Secrets = Json->GetObjectField(TEXT("secrets"));
		if (Secrets->HasField(Key))
		{
			return Secrets->GetStringField(Key);
		}
	}
	FString EnvKey = FPlatformMisc::GetEnvironmentVariable(*FString::Printf(TEXT("NWIRO_%s"), *Key.ToUpper()));
	return EnvKey;
}

void SaveNwiroSecret(const FString& Key, const FString& Value)
{
	auto Json = LoadConfig();
	TSharedPtr<FJsonObject> Secrets;
	if (Json->HasTypedField<EJson::Object>(TEXT("secrets")))
	{
		Secrets = Json->GetObjectField(TEXT("secrets"));
	}
	else
	{
		Secrets = MakeShareable(new FJsonObject);
	}

	if (Value.IsEmpty())
	{
		Secrets->RemoveField(Key);
		Json->SetObjectField(TEXT("secrets"), Secrets);
		SaveConfig(Json);
		return;
	}
	Secrets->SetStringField(Key, Value);
	Json->SetObjectField(TEXT("secrets"), Secrets);
	SaveConfig(Json);
}

// ============================================================
// Module — MCP server only (no chat panel, no browser widget)
// ============================================================

void FNwiroIKModule::StartupModule()
{
	FNwiroIKStyle::Initialize();
	FNwiroIKPanel::Initialize();

	// Tear down Slate-owned panel state from OnEnginePreExit, which fires before
	// core modules shut down while Slate is still alive — robust against the
	// module-shutdown-vs-Slate-teardown ordering that caused the exit crash.
	// FNwiroIKPanel::Shutdown() is idempotent, so the ShutdownModule() call is a
	// harmless fallback.
	EnginePreExitHandle = FCoreDelegates::OnEnginePreExit.AddStatic(&FNwiroIKPanel::Shutdown);

	const int32 Port = LoadSavedPort();
	FNwiroIKMCPServer::Start(Port);
	FNwiroIKLogCapture::Get().Register();

	UE_LOG(LogTemp, Log, TEXT("Nwiro Integration Kit: MCP server started on port %d (209 tools)"), Port);
	// adapter-reliability-w8: build-identity tag emitted once on plugin
	// init so support can verify (via grep) which patch waves are
	// compiled into the running DLL. ABSENCE of this line in a user's
	// log = stale C++ binary, regardless of UI bundle freshness.
	// Bump the tag when shipping a new wave.
	UE_LOG(LogTemp, Log,
		TEXT("Nwiro IK: build-id w8 (env-helpers + bytes-counter + version-path + mcp-pre-flight + effortlevel-pattern + stall-detector)"));
}

void FNwiroIKModule::ShutdownModule()
{
	if (EnginePreExitHandle.IsValid())
	{
		FCoreDelegates::OnEnginePreExit.Remove(EnginePreExitHandle);
		EnginePreExitHandle.Reset();
	}
	FNwiroIKLogCapture::Get().Unregister();
	FNwiroIKMCPServer::Stop();
	FNwiroIKPanel::Shutdown(); // idempotent fallback if OnEnginePreExit already ran
	FNwiroIKStyle::Shutdown();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FNwiroIKModule, NwiroIntegrationKit)
