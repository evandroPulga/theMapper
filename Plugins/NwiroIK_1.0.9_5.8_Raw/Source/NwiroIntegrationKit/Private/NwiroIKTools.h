// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// IK Rig Controller and IK Retargeter Controller APIs only exist in UE 5.7+.
// On older engines this entire tool set is compiled out and the MCP server
// does not list or dispatch these tools.
#define NWIRO_HAS_IK_TOOLS (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)

#if NWIRO_HAS_IK_TOOLS

/**
 * IK Rig, IK Retargeter, and Pose Search / Motion Matching tools.
 */
class FNwiroIKTools
{
public:
	// === IK Rig ===
	static FString CreateIKRig(const FString& JsonCommand);
	static FString ReadIKRig(const FString& JsonCommand);
	static FString AddIKGoal(const FString& JsonCommand);
	static FString AddIKSolver(const FString& JsonCommand);
	static FString AddRetargetChain(const FString& JsonCommand);

	// === IK Retargeter ===
	static FString CreateIKRetargeter(const FString& JsonCommand);
	static FString ReadIKRetargeter(const FString& JsonCommand);
	static FString SetChainMapping(const FString& JsonCommand);

	// === Pose Search / Motion Matching ===
	static FString CreatePoseSearchSchema(const FString& JsonCommand);
	static FString CreatePoseSearchDatabase(const FString& JsonCommand);
	static FString ReadPoseSearchDatabase(const FString& JsonCommand);
	static FString AddPoseSearchAnimation(const FString& JsonCommand);
};

#endif // NWIRO_HAS_IK_TOOLS
