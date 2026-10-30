// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class SWebBrowser;

// macOS IME path intentionally NOT implemented as custom plugin code.
// Decision: macOS IME composition relies on the cross-platform UE5 API
// SWebBrowser::BindInputMethodSystem() (already called from FNwiroIKPanel
// ::OnSpawnTab), which routes through Epic's FCEFImeHandler. Anything
// beyond that — NSTextInputClient firstResponder swapping, app-activation
// observers, deferred NSView discovery — is an engine-level concern that
// should not live in the plugin. If a Mac-side IME gap surfaces in a
// real customer report, escalate to an engine patch / Epic PR rather
// than re-introducing a custom .mm bridge here.

// --- Windows bridge (class-based API; impl in NwiroIKImeBridge.cpp) ---
#if PLATFORM_WINDOWS

// UE 5.7 inlines IWindowsMessageHandler inside WindowsApplication.h
// (no separate IWindowsMessageHandler.h — that was a UE 5.0-5.5 convention
// that Epic reversed). Older UE versions also expose IWindowsMessageHandler
// via this header, so this include is forward- and backward-compatible.
#include "Windows/WindowsApplication.h"

/**
 * Bridges Windows IME messages (WM_IME_*) from the Slate top-level window
 * to the embedded CEF child HWND.
 *
 * Slate's FWindowsApplication intercepts WM_IME_* at the parent window and
 * never forwards them to the CEF child HWND. Result: JS compositionstart/end
 * never fire, CJK users can't type in the embedded chat input.
 *
 * This handler re-routes those messages to the CEF browser HWND via
 * PostMessageW (for value-param composition messages) or SendMessageW (for
 * WM_IME_SETCONTEXT which carries an HIMC handle and requires synchronous
 * delivery to avoid use-after-free).
 *
 * CEF HWND is discovered via EnumChildWindows looking for the well-known
 * "Chrome_RenderWidgetHostHWND" window class — no CEF header includes needed.
 * Result is cached and re-validated with IsWindow() to avoid per-message walk
 * cost during active composition.
 */
class FNwiroIKImeBridge : public IWindowsMessageHandler
{
public:
	// Virtual destructor — IWindowsMessageHandler is polymorphic and this
	// class adds non-trivial members (TWeakPtr, HWND). Silences MSVC C4265
	// and guarantees correct cleanup if ever destroyed through a base ptr.
	virtual ~FNwiroIKImeBridge() = default;

	void Attach(TSharedPtr<SWebBrowser> InBrowser);
	void Detach();

	// IWindowsMessageHandler
	virtual bool ProcessMessage(HWND Hwnd, uint32 Msg, WPARAM WParam, LPARAM LParam, int32& OutResult) override;

private:
	HWND GetCefHwnd() const;

	TWeakPtr<SWebBrowser> WeakBrowser;
	mutable HWND CachedCefHwnd = nullptr;
};

#endif // PLATFORM_WINDOWS
