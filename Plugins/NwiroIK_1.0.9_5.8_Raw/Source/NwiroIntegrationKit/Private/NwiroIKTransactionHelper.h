// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ScopedTransaction.h"
#include "UObject/Object.h"

/**
 * RAII helper that bundles an Unreal editor transaction with one or more
 * UObject::Modify() calls. The transaction stays open for the lifetime of
 * the helper; on scope exit it is closed (or cancelled) and the bundled
 * mutations appear as a single compound undo step in the editor's
 * transaction buffer.
 *
 * Why this exists:
 *   AI-driven tool calls mutate UObjects in bursty sequences with no natural
 *   save points. Unwrapped mutations do not register with GEditor->Trans,
 *   so Ctrl+Z is a no-op. Eagerly calling SaveLoadedAsset after each
 *   mutation also persists partial state to disk; on crash or normal
 *   close the project is left in an unrecoverable broken state. This
 *   helper enforces the canonical UE5 transaction pattern at every
 *   mutation site so both live-session undo and crash-recovery work.
 *
 * Usage:
 *
 *   {
 *       FNwiroIKTransactionHelper Tx(
 *           NSLOCTEXT("Nwiro", "CreateStruct", "AI: Create Struct"),
 *           Asset);
 *
 *       Asset->SomeProperty = NewValue;
 *       Tx.AlsoModify(OtherAsset);   // include more objects in the same undo step
 *       OtherAsset->OtherProperty = OtherValue;
 *
 *       // Mark dirty so the editor knows the package needs saving.
 *       // Do NOT call SaveLoadedAsset() here — let the editor's save flow
 *       // (Ctrl+S, save-on-close prompt) handle persistence.
 *       Asset->MarkPackageDirty();
 *   }   // Tx destructor closes the FScopedTransaction.
 *
 * IMPORTANT — Do NOT call UEditorAssetLibrary::SaveLoadedAsset() within
 * (or immediately after) the helper's scope on the same mutation path.
 * Eager saves defeat the entire purpose of the transaction wrapper and
 * re-create the crash-corruption vector this helper exists to eliminate.
 * Mark the package dirty and rely on the editor's normal save flow.
 */
class FNwiroIKTransactionHelper
{
public:
	/** Begin a new transaction and register the initial UObject for undo. */
	explicit FNwiroIKTransactionHelper(const FText& Description, UObject* InitialObject = nullptr)
		: Transaction(Description)
	{
		if (InitialObject)
		{
			InitialObject->Modify();
		}
	}

	/** Register an additional UObject with the active transaction (for multi-object mutations). */
	void AlsoModify(UObject* Object)
	{
		if (Object)
		{
			Object->Modify();
		}
	}

	/** Cancel the transaction so it does NOT appear on the undo stack. Use only on failure paths. */
	void Cancel()
	{
		Transaction.Cancel();
	}

private:
	FScopedTransaction Transaction;

	// Non-copyable, non-movable — RAII discipline
	FNwiroIKTransactionHelper(const FNwiroIKTransactionHelper&) = delete;
	FNwiroIKTransactionHelper& operator=(const FNwiroIKTransactionHelper&) = delete;
	FNwiroIKTransactionHelper(FNwiroIKTransactionHelper&&) = delete;
	FNwiroIKTransactionHelper& operator=(FNwiroIKTransactionHelper&&) = delete;
};
