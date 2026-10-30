// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKSequencerTools.h"
#include "Factories/Factory.h"
#include "NwiroIKTransactionHelper.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
// MovieScene3DTransformSection included via track header
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Json.h"
#include "Misc/FrameRate.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroSequencer, Log, All);

static ULevelSequence* FindSequence(const FString& PathOrName)
{
	UObject* Asset = UEditorAssetLibrary::LoadAsset(PathOrName);
	if (ULevelSequence* Seq = Cast<ULevelSequence>(Asset)) return Seq;

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FARFilter Filter;
	Filter.ClassPaths.Add(ULevelSequence::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(TEXT("/Game"));
	TArray<FAssetData> Assets;
	ARM.Get().GetAssets(Filter, Assets);
	for (const FAssetData& A : Assets)
	{
		if (A.AssetName.ToString().Contains(PathOrName, ESearchCase::IgnoreCase))
		{
			return Cast<ULevelSequence>(A.GetAsset());
		}
	}
	return nullptr;
}

// ============================================================
// CREATE SEQUENCE
// ============================================================

FString FNwiroIKSequencerTools::CreateSequence(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Name = Cmd->GetStringField(TEXT("name"));
	FString Path = Cmd->GetStringField(TEXT("path"));
	int32 FPS = Cmd->HasField(TEXT("fps")) ? (int32)Cmd->GetNumberField(TEXT("fps")) : 30;

	if (Name.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"Missing 'name'\"}");
	if (Path.IsEmpty()) Path = TEXT("/Game/Cinematics");

	const FString FullPath = Path / Name;
	// Idempotent — return existing instead of dup-create crash.
	if (ULevelSequence* Existing = LoadObject<ULevelSequence>(nullptr, *(FullPath + TEXT(".") + Name)))
	{
		return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"existed\":true}"),
			*Name, *Existing->GetPathName());
	}

	// LevelSequenceFactoryNew lives in LevelSequenceEditor module — load dynamically to avoid hard dep.
	UClass* FactoryClass = LoadObject<UClass>(nullptr, TEXT("/Script/LevelSequenceEditor.LevelSequenceFactoryNew"));
	UFactory* Factory = FactoryClass ? NewObject<UFactory>(GetTransientPackage(), FactoryClass) : nullptr;

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateSequence", "AI: Create Sequence"));

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* NewAsset = AssetTools.CreateAsset(Name, Path, ULevelSequence::StaticClass(), Factory);
	ULevelSequence* Sequence = Cast<ULevelSequence>(NewAsset);
	if (!Sequence)
	{
		Tx.Cancel();
		return TEXT("{\"success\":false,\"error\":\"Failed to create LevelSequence. Ensure LevelSequenceEditor plugin is enabled.\"}");
	}
	Tx.AlsoModify(Sequence);

	Sequence->Initialize();
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (MovieScene)
	{
		Tx.AlsoModify(MovieScene);
		MovieScene->SetDisplayRate(FFrameRate(FPS, 1));
	}

	Sequence->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"name\":\"%s\",\"path\":\"%s\",\"fps\":%d}"),
		*Name, *Sequence->GetPathName(), FPS);
}

// ============================================================
// READ SEQUENCE
// ============================================================

FString FNwiroIKSequencerTools::ReadSequence(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path = Cmd->GetStringField(TEXT("path"));
	ULevelSequence* Sequence = FindSequence(Path);
	if (!Sequence)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Sequence not found: %s\"}"), *Path);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return TEXT("{\"success\":false,\"error\":\"No MovieScene\"}");

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Sequence->GetName());
	Result->SetStringField(TEXT("path"), Sequence->GetPathName());
	Result->SetNumberField(TEXT("fps"), (double)MovieScene->GetDisplayRate().Numerator);

	// Possessables (actor bindings)
	TArray<TSharedPtr<FJsonValue>> Bindings;
	for (int32 i = 0; i < MovieScene->GetPossessableCount(); i++)
	{
		const FMovieScenePossessable& Poss = MovieScene->GetPossessable(i);
		TSharedRef<FJsonObject> B = MakeShareable(new FJsonObject());
		B->SetStringField(TEXT("name"), Poss.GetName());
		B->SetStringField(TEXT("guid"), Poss.GetGuid().ToString());
		B->SetStringField(TEXT("class"), Poss.GetPossessedObjectClass() ? Poss.GetPossessedObjectClass()->GetName() : TEXT("None"));
		Bindings.Add(MakeShareable(new FJsonValueObject(B)));
	}
	Result->SetArrayField(TEXT("bindings"), Bindings);

	// Tracks (master tracks)
	TArray<TSharedPtr<FJsonValue>> Tracks;
	for (UMovieSceneTrack* Track : MovieScene->GetTracks())
	{
		if (!Track) continue;
		TSharedRef<FJsonObject> T = MakeShareable(new FJsonObject());
		T->SetStringField(TEXT("name"), Track->GetDisplayName().ToString());
		T->SetStringField(TEXT("class"), Track->GetClass()->GetName());
		T->SetNumberField(TEXT("sectionCount"), Track->GetAllSections().Num());
		Tracks.Add(MakeShareable(new FJsonValueObject(T)));
	}
	Result->SetArrayField(TEXT("masterTracks"), Tracks);

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// ADD SEQUENCE BINDING
// ============================================================

FString FNwiroIKSequencerTools::AddSequenceBinding(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SeqPath = Cmd->GetStringField(TEXT("sequence"));
	FString ActorName = Cmd->GetStringField(TEXT("actor"));

	ULevelSequence* Sequence = FindSequence(SeqPath);
	if (!Sequence)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Sequence not found: %s\"}"), *SeqPath);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return TEXT("{\"success\":false,\"error\":\"No MovieScene\"}");

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "AddSequenceBinding", "AI: Add Sequence Binding"), Sequence);
	Tx.AlsoModify(MovieScene);

	// Find actor in world
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return TEXT("{\"success\":false,\"error\":\"No editor world\"}");

	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetName() == ActorName || (*It)->GetActorLabel() == ActorName)
		{
			Actor = *It;
			break;
		}
	}
	if (!Actor)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Actor not found: %s\"}"), *ActorName);

	FGuid BindingID = MovieScene->AddPossessable(Actor->GetActorLabel(), Actor->GetClass());
	Sequence->BindPossessableObject(BindingID, *Actor, World);

	Sequence->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"actor\":\"%s\",\"bindingId\":\"%s\"}"),
		*Actor->GetActorLabel(), *BindingID.ToString());
}

// ============================================================
// ADD SEQUENCE TRACK
// ============================================================

FString FNwiroIKSequencerTools::AddSequenceTrack(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SeqPath = Cmd->GetStringField(TEXT("sequence"));
	FString BindingStr = Cmd->GetStringField(TEXT("binding"));
	FString TrackType = Cmd->GetStringField(TEXT("trackType"));

	ULevelSequence* Sequence = FindSequence(SeqPath);
	if (!Sequence) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Sequence not found: %s\"}"), *SeqPath);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return TEXT("{\"success\":false,\"error\":\"No MovieScene\"}");

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "AddSequenceTrack", "AI: Add Sequence Track"), Sequence);
	Tx.AlsoModify(MovieScene);

	FString TrackLower = TrackType.ToLower();
	// Audio and CameraCut live at sequence level — no binding GUID needed.
	const bool bSequenceLevel = TrackLower == TEXT("audio")
		|| TrackLower == TEXT("cameracut") || TrackLower == TEXT("camera_cut");

	FGuid BindingGuid;
	if (!bSequenceLevel)
	{
		if (!FGuid::Parse(BindingStr, BindingGuid) || !BindingGuid.IsValid())
		{
			for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
			{
				const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
				if (P.GetName().Equals(BindingStr, ESearchCase::IgnoreCase)) { BindingGuid = P.GetGuid(); break; }
			}
			if (!BindingGuid.IsValid())
			{
				for (int32 i = 0; i < MovieScene->GetSpawnableCount(); ++i)
				{
					const FMovieSceneSpawnable& S = MovieScene->GetSpawnable(i);
					if (S.GetName().Equals(BindingStr, ESearchCase::IgnoreCase)) { BindingGuid = S.GetGuid(); break; }
				}
			}
		}
		if (!BindingGuid.IsValid())
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"Binding not found: %s. Pass either the GUID returned by add_sequence_binding or the actor display name.\"}"), *BindingStr);
	}

	UMovieSceneTrack* Track = nullptr;
	if (TrackLower == TEXT("transform"))
	{
		Track = MovieScene->AddTrack<UMovieScene3DTransformTrack>(BindingGuid);
	}
	else if (TrackLower == TEXT("float"))
	{
		Track = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
	}
	else if (TrackLower == TEXT("bool"))
	{
		Track = MovieScene->AddTrack<UMovieSceneBoolTrack>(BindingGuid);
	}
	else if (TrackLower == TEXT("audio"))
	{
		Track = MovieScene->AddTrack<UMovieSceneAudioTrack>();
	}
	else if (TrackLower == TEXT("event"))
	{
		Track = MovieScene->AddTrack<UMovieSceneEventTrack>(BindingGuid);
	}
	else if (TrackLower == TEXT("cameracut") || TrackLower == TEXT("camera_cut"))
	{
		Track = MovieScene->AddTrack<UMovieSceneCameraCutTrack>();
	}
	else
	{
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Unknown track type: %s. Use: Transform, Float, Bool, Audio, Event, CameraCut\"}"), *TrackType);
	}

	if (!Track)
		return TEXT("{\"success\":false,\"error\":\"Failed to create track\"}");

	// Create initial section
	UMovieSceneSection* Section = Track->CreateNewSection();
	if (Section)
	{
		Track->AddSection(*Section);
		Section->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(150))); // 5 sec @ 30fps
	}

	Sequence->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"trackType\":\"%s\",\"trackClass\":\"%s\"}"),
		*TrackType, *Track->GetClass()->GetName());
}

// ============================================================
// ADD SEQUENCE KEYFRAME
// ============================================================

FString FNwiroIKSequencerTools::AddSequenceKeyframe(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SeqPath = Cmd->GetStringField(TEXT("sequence"));
	FString BindingStr = Cmd->GetStringField(TEXT("binding"));
	int32 Frame = (int32)Cmd->GetNumberField(TEXT("frame"));

	ULevelSequence* Sequence = FindSequence(SeqPath);
	if (!Sequence) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Sequence not found: %s\"}"), *SeqPath);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return TEXT("{\"success\":false,\"error\":\"No MovieScene\"}");

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "AddSequenceKeyframe", "AI: Add Sequence Keyframe"), Sequence);
	Tx.AlsoModify(MovieScene);

	// Resolve binding GUID — accept either GUID or actor display name.
	FGuid BindingGuid;
	if (!FGuid::Parse(BindingStr, BindingGuid) || !BindingGuid.IsValid())
	{
		for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
		{
			const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
			if (P.GetName().Equals(BindingStr, ESearchCase::IgnoreCase)) { BindingGuid = P.GetGuid(); break; }
		}
		if (!BindingGuid.IsValid())
			for (int32 i = 0; i < MovieScene->GetSpawnableCount(); ++i)
			{
				const FMovieSceneSpawnable& S = MovieScene->GetSpawnable(i);
				if (S.GetName().Equals(BindingStr, ESearchCase::IgnoreCase)) { BindingGuid = S.GetGuid(); break; }
			}
	}
	if (!BindingGuid.IsValid())
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Binding not found: %s\"}"), *BindingStr);

	// Find transform track for this binding
	const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
	if (!Binding)
		return TEXT("{\"success\":false,\"error\":\"Binding not found\"}");

	for (UMovieSceneTrack* Track : Binding->GetTracks())
	{
		UMovieScene3DTransformTrack* TransformTrack = Cast<UMovieScene3DTransformTrack>(Track);
		if (!TransformTrack) continue;

		for (UMovieSceneSection* Section : TransformTrack->GetAllSections())
		{
			// Get channels - Transform has 9 channels: LocX,LocY,LocZ,RotX,RotY,RotZ,ScaleX,ScaleY,ScaleZ
			FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
			TArrayView<FMovieSceneDoubleChannel*> Channels = Proxy.GetChannels<FMovieSceneDoubleChannel>();

			if (Channels.Num() >= 9)
			{
				double X = Cmd->HasField(TEXT("x")) ? Cmd->GetNumberField(TEXT("x")) : 0;
				double Y = Cmd->HasField(TEXT("y")) ? Cmd->GetNumberField(TEXT("y")) : 0;
				double Z = Cmd->HasField(TEXT("z")) ? Cmd->GetNumberField(TEXT("z")) : 0;

				FFrameNumber FrameNum(Frame);
				Channels[0]->AddCubicKey(FrameNum, X); // Location X
				Channels[1]->AddCubicKey(FrameNum, Y); // Location Y
				Channels[2]->AddCubicKey(FrameNum, Z); // Location Z

				if (Cmd->HasField(TEXT("pitch")) || Cmd->HasField(TEXT("yaw")) || Cmd->HasField(TEXT("roll")))
				{
					double Pitch = Cmd->HasField(TEXT("pitch")) ? Cmd->GetNumberField(TEXT("pitch")) : 0;
					double Yaw = Cmd->HasField(TEXT("yaw")) ? Cmd->GetNumberField(TEXT("yaw")) : 0;
					double Roll = Cmd->HasField(TEXT("roll")) ? Cmd->GetNumberField(TEXT("roll")) : 0;
					Channels[3]->AddCubicKey(FrameNum, Pitch);
					Channels[4]->AddCubicKey(FrameNum, Yaw);
					Channels[5]->AddCubicKey(FrameNum, Roll);
				}

				Sequence->MarkPackageDirty();

				return FString::Printf(TEXT("{\"success\":true,\"frame\":%d,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}"),
					Frame, X, Y, Z);
			}
		}
	}

	return TEXT("{\"success\":false,\"error\":\"No transform track found for this binding\"}");
}

// ============================================================
// SET SEQUENCE RANGE
// ============================================================

FString FNwiroIKSequencerTools::SetSequenceRange(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString SeqPath = Cmd->GetStringField(TEXT("sequence"));
	int32 StartFrame = (int32)Cmd->GetNumberField(TEXT("startFrame"));
	int32 EndFrame = (int32)Cmd->GetNumberField(TEXT("endFrame"));

	ULevelSequence* Sequence = FindSequence(SeqPath);
	if (!Sequence) return FString::Printf(TEXT("{\"success\":false,\"error\":\"Sequence not found: %s\"}"), *SeqPath);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return TEXT("{\"success\":false,\"error\":\"No MovieScene\"}");

	FNwiroIKTransactionHelper Tx(NSLOCTEXT("Nwiro", "SetSequenceRange", "AI: Set Sequence Range"), Sequence);
	Tx.AlsoModify(MovieScene);

	if (Cmd->HasField(TEXT("fps")))
	{
		int32 FPS = (int32)Cmd->GetNumberField(TEXT("fps"));
		MovieScene->SetDisplayRate(FFrameRate(FPS, 1));
	}

	MovieScene->SetPlaybackRange(FFrameNumber(StartFrame), (EndFrame - StartFrame));

	Sequence->MarkPackageDirty();

	return FString::Printf(TEXT("{\"success\":true,\"startFrame\":%d,\"endFrame\":%d}"), StartFrame, EndFrame);
}
