// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKImeBridge.h"

#if PLATFORM_WINDOWS

#include "SWebBrowser.h"
#include "Framework/Application/SlateApplication.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <imm.h>
#include "Windows/HideWindowsPlatformTypes.h"

// Dedicated log category so customer reports can grep for it easily.
DEFINE_LOG_CATEGORY_STATIC(LogNwiroIme, Log, All);

// Experimental: rewrite IME composition attributes to suppress Chromium's
// default yellow background highlight on ATTR_TARGET_CONVERTED segments.
// Default OFF — enable via Build.cs or compile define only after confirming
// (via the diagnostic log below) that the bridge is actually active on the
// target setup. See council report yellow-highlight-fix-options.json.
#ifndef NWIRO_IK_REWRITE_IME_ATTRS
#define NWIRO_IK_REWRITE_IME_ATTRS 0
#endif

namespace
{
	struct FCefHwndSearch { HWND Result = nullptr; };

	// EnumChildWindows callback. Walks the Slate top-level window's child HWNDs
	// looking for the CEF browser. CEF on Windows registers its render-host
	// window with class name "Chrome_RenderWidgetHostHWND" — we match that
	// (or the parent "CefBrowserWindow" container as a fallback) by class name
	// so we don't need to depend on the CEF SDK headers at compile time.
	//
	// Returns BOOL (= int): non-zero to continue enumeration, zero to stop.
	// We use 1/0 literals instead of TRUE/FALSE macros because
	// HideWindowsPlatformTypes.h has UN-defined those macros by the time
	// this namespace block is compiled (UE hides Win32 macros to keep its
	// internal symbol table clean). BOOL is `typedef int BOOL` per windef.h.
	static BOOL CALLBACK FindCefChildProc(HWND Child, LPARAM Param)
	{
		TCHAR ClassName[256] = {};
		GetClassNameW(Child, ClassName, 256);
		if (FCString::Stricmp(ClassName, TEXT("Chrome_RenderWidgetHostHWND")) == 0 ||
			FCString::Stricmp(ClassName, TEXT("CefBrowserWindow")) == 0)
		{
			reinterpret_cast<FCefHwndSearch*>(Param)->Result = Child;
			return 0; // stop enumeration (FALSE-equivalent)
		}
		return 1; // continue enumeration (TRUE-equivalent)
	}
}

void FNwiroIKImeBridge::Attach(TSharedPtr<SWebBrowser> InBrowser)
{
	WeakBrowser = InBrowser;
	CachedCefHwnd = nullptr;
}

void FNwiroIKImeBridge::Detach()
{
	WeakBrowser.Reset();
	CachedCefHwnd = nullptr;
}

bool FNwiroIKImeBridge::ProcessMessage(HWND Hwnd, uint32 Msg, WPARAM WParam, LPARAM LParam, int32& OutResult)
{
	switch (Msg)
	{
	case WM_IME_SETCONTEXT:
	case WM_IME_STARTCOMPOSITION:
	case WM_IME_COMPOSITION:
	case WM_IME_ENDCOMPOSITION:
	case WM_IME_CHAR:
	case WM_IME_NOTIFY:
		break;
	default:
		return false; // not an IME message — let Slate keep handling
	}

	HWND CefHwnd = GetCefHwnd();

	// Diagnostic: surface bridge activity for WM_IME_COMPOSITION specifically —
	// this is the message the yellow-highlight bug attaches to. Customer
	// support can grep their UE Output Log for "NwiroIme:" and tell us
	// whether the bridge found a CEF HWND in their OSR setup or not.
	// Two competing theories on the yellow highlight depend on this answer:
	//   • Bridge inactive (CefHwnd=null) → yellow originates in
	//     FCEFTextInputMethodContext::ImeSetComposition (engine source path)
	//   • Bridge active (CefHwnd=non-null) → yellow originates from
	//     ATTR_TARGET_CONVERTED bytes forwarded via PostMessageW path.
	// Logging is intentionally conditional on WM_IME_COMPOSITION to avoid
	// spamming the log on every IME state change.
	if (Msg == WM_IME_COMPOSITION)
	{
		UE_LOG(LogNwiroIme, Log,
			TEXT("WM_IME_COMPOSITION CefHwnd=%p LParam=0x%08llx (GCS_COMPSTR=%d GCS_RESULTSTR=%d GCS_COMPATTR=%d)"),
			(void*)CefHwnd,
			(unsigned long long)LParam,
			(LParam & GCS_COMPSTR) ? 1 : 0,
			(LParam & GCS_RESULTSTR) ? 1 : 0,
			(LParam & GCS_COMPATTR) ? 1 : 0);
	}

	if (!CefHwnd)
	{
		// CEF browser HWND not yet created (panel just spawned) — let Slate
		// handle the message normally. Next composition attempt will retry.
		return false;
	}

#if NWIRO_IK_REWRITE_IME_ATTRS
	// Experimental: rewrite ATTR_TARGET_CONVERTED → ATTR_INPUT in the
	// composition attribute string so Chromium renders the actively-converting
	// segment with a plain underline instead of the default yellow background.
	// Behind a compile flag — only enable after the diagnostic log confirms
	// the bridge is active on the affected user's setup.
	//
	// Recursion guard: ImmSetCompositionStringW synchronously sends
	// WM_IME_COMPOSITION back through the same window, which would re-enter
	// ProcessMessage and loop forever. thread_local flag breaks the loop.
	thread_local bool bRewritingAttrs = false;
	if (Msg == WM_IME_COMPOSITION && !bRewritingAttrs && (LParam & GCS_COMPATTR))
	{
		// Read the IMC that Chromium will read — CefHwnd, NOT the Slate Hwnd.
		// Each window can have its own HIMC association; if Slate and CEF
		// don't share, modifying Slate's IMC has no effect on what Chromium
		// sees when it calls ImmGetContext(CefHwnd) in its own pump.
		HIMC hImc = ImmGetContext(CefHwnd);
		if (hImc)
		{
			const LONG AttrSize = ImmGetCompositionStringW(hImc, GCS_COMPATTR, nullptr, 0);
			if (AttrSize > 0)
			{
				TArray<BYTE> Attrs;
				Attrs.SetNumUninitialized(AttrSize);
				ImmGetCompositionStringW(hImc, GCS_COMPATTR, Attrs.GetData(), AttrSize);

				int32 NumRewritten = 0;
				for (int32 i = 0; i < AttrSize; ++i)
				{
					if (Attrs[i] == /*ATTR_TARGET_CONVERTED*/ 2)
					{
						Attrs[i] = /*ATTR_INPUT*/ 0;
						++NumRewritten;
					}
				}

				if (NumRewritten > 0)
				{
					// SCS_CHANGEATTR rewrites the attribute array only —
					// the composition string and clause info are left alone.
					// DWORD cast: ImmSetCompositionStringW takes unsigned
					// lengths; AttrSize is LONG. Cast suppresses C4245.
					bRewritingAttrs = true;
					ImmSetCompositionStringW(hImc, SCS_CHANGEATTR,
						Attrs.GetData(), static_cast<DWORD>(AttrSize),
						nullptr, 0);
					bRewritingAttrs = false;

					UE_LOG(LogNwiroIme, Verbose,
						TEXT("Rewrote %d/%d ATTR_TARGET_CONVERTED bytes to ATTR_INPUT"),
						NumRewritten, AttrSize);
				}
			}
			ImmReleaseContext(CefHwnd, hImc);
		}
	}
#endif // NWIRO_IK_REWRITE_IME_ATTRS

	if (Msg == WM_IME_SETCONTEXT)
	{
		// wParam carries an HIMC (Input Method Context) handle. Async delivery
		// via PostMessage can race with Win32 releasing the handle.
		// Sequence:
		//   1. Tell Slate's own HWND to suppress its IME UI (lParam=0).
		//   2. Synchronously hand full IME display rights to CEF with the
		//      real lParam — CEF processes immediately, no race.
		DefWindowProc(Hwnd, WM_IME_SETCONTEXT, WParam, 0);
		SendMessageW(CefHwnd, WM_IME_SETCONTEXT, WParam, LParam);
		OutResult = 0;
		return true;
	}

	// Composition messages carry value-type params (composition strings,
	// attribute lists). Async PostMessage is safe and avoids deadlocks —
	// CEF processes the message on its own browser thread loop, and
	// SendMessage from here would block waiting for CEF's pump to drain.
	PostMessageW(CefHwnd, Msg, WParam, LParam);
	OutResult = 0;
	return true; // swallow from Slate — CEF owns it now
}

HWND FNwiroIKImeBridge::GetCefHwnd() const
{
	if (!FSlateApplication::IsInitialized()) return nullptr;

	// Cache hit — fast path during active composition.
	if (CachedCefHwnd && IsWindow(CachedCefHwnd))
	{
		return CachedCefHwnd;
	}
	CachedCefHwnd = nullptr;

	TSharedPtr<SWebBrowser> Browser = WeakBrowser.Pin();
	if (!Browser.IsValid())
	{
		return nullptr;
	}

	TSharedPtr<SWindow> TopWindow = FSlateApplication::Get().FindWidgetWindow(Browser.ToSharedRef());
	if (!TopWindow.IsValid())
	{
		return nullptr;
	}

	TSharedPtr<FGenericWindow> NativeWindow = TopWindow->GetNativeWindow();
	if (!NativeWindow.IsValid())
	{
		return nullptr;
	}

	HWND SlateHwnd = reinterpret_cast<HWND>(NativeWindow->GetOSWindowHandle());
	if (!SlateHwnd)
	{
		return nullptr;
	}

	FCefHwndSearch Search;
	EnumChildWindows(SlateHwnd, FindCefChildProc, reinterpret_cast<LPARAM>(&Search));
	CachedCefHwnd = Search.Result;
	return CachedCefHwnd;
}

#endif // PLATFORM_WINDOWS
