// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Level Sequencer tools: create sequences, bindings, tracks, keyframes.
 */
class FNwiroIKSequencerTools
{
public:
	static FString CreateSequence(const FString& JsonCommand);
	static FString ReadSequence(const FString& JsonCommand);
	static FString AddSequenceBinding(const FString& JsonCommand);
	static FString AddSequenceTrack(const FString& JsonCommand);
	static FString AddSequenceKeyframe(const FString& JsonCommand);
	static FString SetSequenceRange(const FString& JsonCommand);
};
