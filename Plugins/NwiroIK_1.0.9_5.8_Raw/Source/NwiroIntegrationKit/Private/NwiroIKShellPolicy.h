// Copyright 2026 Nwiro. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * Shell command policy engine (Phase 1 of the council-gated shell feature).
 * Pure classification — NO execution here. Used by the ACP permission gate and,
 * authoritatively, again at the terminal/create exec site (council #2: enforce at
 * the exec site, not only at request_permission).
 *
 * Council guardrail #2/#4: pattern matching is best-effort, NOT a sandbox. It
 * normalizes + unwraps wrapper shells (cmd /c, bash -lc, sh -c, pwsh -Command)
 * before classifying, treats obfuscation (EncodedCommand / pipe-to-interpreter)
 * as Block, and fails CLOSED (unknown ⇒ stricter).
 */
namespace NwiroIKShellPolicy
{
	enum class ERisk : uint8
	{
		Allow,            // ordinary command — still needs the normal per-command Allow click
		ConfirmHighRisk,  // suspicious — requires typed/elevated confirmation
		Block             // forbidden — denied server-side regardless of user approval
	};

	struct FAssessment
	{
		ERisk Risk = ERisk::Block;          // fail-closed default
		FString Reason;                      // human-facing reason (for the modal / logs)
		FString DisplayCommand;              // unwrapped command to show the user (original case)
		bool bRequiresTypedConfirmation = false;
	};

	/** Classify a raw command string. Never executes. */
	FAssessment Assess(const FString& RawCommand);

	/** Stable identity for binding an approval to the exact command (council #2). */
	FString NormalizedHash(const FString& RawCommand);

	const TCHAR* RiskToString(ERisk Risk);
}
