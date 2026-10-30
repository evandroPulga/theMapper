// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKPanel.h"
#include "NwiroIKBridge.h"
#include "NwiroIKImeBridge.h"
#include "NwiroIKStyle.h"
#include "LevelEditor.h"
#include "SWebBrowser.h"
#include "ToolMenus.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/ConfigCacheIni.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsApplication.h"
#endif

// Persisted toolbar-button visibility (per-project ini). Default ON.
static const TCHAR* ToolbarIniSection = TEXT("NwiroIK.UI");
static const TCHAR* ToolbarIniKey = TEXT("ShowToolbarButton");

static const FName NwiroTabName("NwiroIntegrationKit");
static const FText NwiroTabDisplay = FText::FromString("Nwiro Integration Kit");
static const FText NwiroTabDesc = FText::FromString("Nwiro Integration Kit");
static const FString FrontendURL = TEXT("https://app-integration.nwiro.ai");

TSharedPtr<FNwiroIKPanel> FNwiroIKPanel::Instance;
TSharedPtr<SWebBrowser> FNwiroIKPanel::WebBrowserWidget = nullptr;

void FNwiroIKPanel::Initialize()
{
	if (!Instance.IsValid())
	{
		Instance = MakeShareable(new FNwiroIKPanel);
		Instance->RegisterMenus();
	}
}

void FNwiroIKPanel::Shutdown()
{
	if (Instance.IsValid())
	{
		// ShutdownModule() runs AFTER FSlateApplication::Shutdown() on the
		// editor-quit path, so FSlateApplication::Get() would fire the
		// `check(CurrentApplication.IsValid())` assertion at
		// SlateApplication.h:321. Confirmed by customer crash UECC-Windows-
		// 4CA6C097478451D285AD12AFD6FE0BE3 — call stack named this exact
		// function. Every Slate-touching path below is guarded with
		// FSlateApplication::IsInitialized(); the same guard is mirrored in
		// OnTabClosed_Lambda for the editor-quit-with-panel-open path.
#if PLATFORM_WINDOWS
		// Covers quit-with-panel-open: OnTabClosed never fires, so without this
		// the IME bridge stays registered with FWindowsApplication and the next
		// IME message crashes through a dangling pointer into freed memory.
		//
		// FSlateApplication::IsInitialized() guard: confirmed-crash fix for
		// SlateApplication.h:321 assertion `CurrentApplication.IsValid()`.
		// At editor-quit Slate may already be torn down before this Shutdown()
		// runs (UE module shutdown ordering for PostEngineInit modules is not
		// guaranteed relative to Slate teardown). Reported in two independent
		// user reports on UE 5.7 and 5.8 — not a 5.8-only regression.
		if (Instance->ImeBridge.IsValid())
		{
			if (FSlateApplication::IsInitialized())
			{
				FWindowsApplication* WinApp = static_cast<FWindowsApplication*>(
					FSlateApplication::Get().GetPlatformApplication().Get());
				if (WinApp)
				{
					WinApp->RemoveMessageHandler(*Instance->ImeBridge);
				}
			}
			// Detach + Reset are pure SharedPtr/internal ops with no Slate
			// dependency — always run them. Only RemoveMessageHandler needs Slate
			// and is guarded by the inner IsInitialized() check above.
			Instance->ImeBridge->Detach();
			Instance->ImeBridge.Reset();
		}
#endif
#if PLATFORM_MAC
		// TODO(v1.0.6): Symmetric Mac IME bridge teardown — currently no Mac
		// quit-with-panel-open path exists, so Mac users hit the same dangling-
		// pointer crash class on editor quit if the panel is open. Pending
		// identification of Mac-side IME handler API (likely via the bridge in
		// NwiroIKImeBridgeMac.mm). See council critic finding #3.
#endif
		// Unbind UE's text input method system on editor-quit path.
		// Mirrors OnTabClosed_Lambda — Shutdown fires when quitting with
		// the panel still open, OnTabClosed fires when the user closes the
		// tab manually. Both need the unbind to prevent dangling-context
		// crash on subsequent async IME focus events.
		//
		// Fix Point E: defensive Slate-availability guard.
		// SWebBrowser::UnbindInputMethodSystem internals are unverifiable
		// (Epic GitHub mirror is auth-gated), so we cannot prove the call
		// path doesn't reach FSlateApplication::Get(). If Slate is already
		// torn down at this point, the bound ITextInputMethodSystem* came
		// from Slate originally and is invalid anyway — skipping the unbind
		// is correct cleanup, not a leak. One extra clause; zero downside
		// when Slate is alive (guard evaluates true, identical behavior).
		// Scope note: this guard catches the Slate-gone teardown class only;
		// it does NOT protect against widget-internal CEF IME state corruption.
		if (WebBrowserWidget.IsValid() && FSlateApplication::IsInitialized())
		{
			WebBrowserWidget->UnbindInputMethodSystem();
		}
		WebBrowserWidget.Reset();
		// Precautionary: FGlobalTabmanager is owned by Slate; guard against
		// edge-case teardown ordering when Shutdown() runs after Slate has been
		// destroyed. Not the confirmed crash site (that is the FSlateApplication
		// usage above), but the same defensive principle applies.
		// Unregister BEFORE resetting Instance — CreateRaw registered the
		// spawner with Instance.Get() as the target; resetting first would
		// leave a dangling delegate target until unregister completes.
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(NwiroTabName);
		}
		// Mirrors OnTabClosed_Lambda: null the bridge singleton so any
		// post-teardown async path that checks UNwiroIKBridge::Instance
		// sees nullptr instead of a stale pointer into freed memory.
		UNwiroIKBridge::Instance = nullptr;
		Instance.Reset();
	}
}

void FNwiroIKPanel::OpenPanel()
{
	FGlobalTabmanager::Get()->TryInvokeTab(NwiroTabName);
}

bool FNwiroIKPanel::IsToolbarButtonEnabled()
{
	bool bShow = true; // default ON
	if (GConfig)
	{
		GConfig->GetBool(ToolbarIniSection, ToolbarIniKey, bShow, GEditorPerProjectIni);
	}
	return bShow;
}

void FNwiroIKPanel::SetToolbarButtonEnabled(bool bEnabled)
{
	if (GConfig)
	{
		GConfig->SetBool(ToolbarIniSection, ToolbarIniKey, bEnabled, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}
	// Rebuild the level-editor toolbar so the dynamic entry re-runs and the
	// button appears/disappears without an editor restart.
	if (UToolMenus* ToolMenus = UToolMenus::Get())
	{
		ToolMenus->RefreshAllWidgets();
	}
}

void FNwiroIKPanel::AddLog(const FString& Message)
{
}

void FNwiroIKPanel::RegisterMenus()
{
	UToolMenu* WindowMenu = UToolMenus::Get()->ExtendMenu("MainFrame.MainMenu.Window");
	// Place Nwiro under its own "Assistant" category instead of "Get Content"
	// so it's easier to find and visually separate from marketplace tabs.
	FToolMenuSection* ContentSectionPtr = WindowMenu->FindSection("NwiroAssistant");
	if (!ContentSectionPtr)
	{
		ContentSectionPtr = &WindowMenu->AddSection("NwiroAssistant",
			NSLOCTEXT("MainAppMenu", "NwiroAssistantHeader", "Assistant"));
	}
	ContentSectionPtr->AddMenuEntry(
		"OpenNwiroIKTab",
		NwiroTabDisplay,
		NwiroTabDesc,
		FSlateIcon(FNwiroIKStyle::GetStyleSetName(), "NwiroIK.Logo"),
		FUIAction(FExecuteAction::CreateStatic(&FNwiroIKPanel::OpenPanel), FCanExecuteAction())
	);

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(NwiroTabName,
		FOnSpawnTab::CreateRaw(Instance.Get(), &FNwiroIKPanel::OnSpawnTab))
		.SetDisplayName(NwiroTabDisplay)
		.SetTooltipText(NwiroTabDesc)
		.SetAutoGenerateMenuEntry(false);

	// Level-editor toolbar button. UE5's main toolbar is UToolMenus-based;
	// legacy FExtender hooks no longer render there, so we extend the toolbar's
	// "User" section directly. A dynamic entry lets the show/hide toggle take
	// effect on RefreshAllWidgets() without an editor restart.
	if (UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.User"))
	{
		FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("NwiroIntegrationKit");
		Section.AddDynamicEntry("NwiroIKToolbarButton", FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
		{
			// User can hide the button from the plugin's settings (default ON).
			if (!IsToolbarButtonEnabled()) return;

			InSection.AddEntry(FToolMenuEntry::InitToolBarButton(
				"OpenNwiroIK",
				FUIAction(FExecuteAction::CreateStatic(&FNwiroIKPanel::OpenPanel)),
				NwiroTabDisplay,
				NwiroTabDesc,
				FSlateIcon(FNwiroIKStyle::GetStyleSetName(), "NwiroIK.ToolbarIcon")));
		}));
	}
}

void FNwiroIKPanel::CreateBrowserWidget()
{
	WebBrowserWidget = SNew(SWebBrowser)
		.InitialURL(FrontendURL)
		.ShowControls(false)
		.ShowAddressBar(false)
		.ShowErrorMessage(true)
		.SupportsTransparency(false)
		.OnBeforePopup_Lambda([](FString URL, FString Frame)
		{
			FPlatformProcess::LaunchURL(*URL, nullptr, nullptr);
			return true;
		});
}

TSharedRef<SDockTab> FNwiroIKPanel::OnSpawnTab(const FSpawnTabArgs& SpawnTabArgs)
{
	CreateBrowserWidget();

#if PLATFORM_WINDOWS
	// Wire up the IME message bridge so CJK input (Pinyin, Bopomofo, Japanese
	// MS-IME etc.) reaches the embedded CEF browser. Without this, Slate's
	// FWindowsApplication eats WM_IME_* at the parent HWND and JS-side
	// compositionstart/end never fires inside the embedded React app.
	ImeBridge = MakeShareable(new FNwiroIKImeBridge());
	ImeBridge->Attach(WebBrowserWidget);
	{
		FWindowsApplication* WinApp = static_cast<FWindowsApplication*>(
			FSlateApplication::Get().GetPlatformApplication().Get());
		if (WinApp)
		{
			WinApp->AddMessageHandler(*ImeBridge);
		}
	}
#endif

	SAssignNew(PanelDock, SDockTab)
		.OnTabClosed_Lambda([WeakThis = AsWeak()](TSharedRef<SDockTab>)
		{
			TSharedPtr<FNwiroIKPanel> Pin = WeakThis.Pin();
			if (!Pin.IsValid())
			{
				return;
			}

			// Tab-close usually fires while Slate is alive (user clicks X on
			// the tab), but on the editor-quit-with-panel-open path,
			// FNwiroIKPanel::Shutdown() does Instance.Reset(), which destroys
			// PanelDock and can fire this callback during Slate teardown
			// depending on UE version. The WeakThis.Pin() guard above plus the
			// FSlateApplication::IsInitialized() checks below keep this safe.
#if PLATFORM_WINDOWS
			// Mirror of Shutdown() cleanup — needed here for the normal
			// user-closes-the-tab flow (Shutdown() only fires on module
			// unload / editor quit).
			//
			// IsInitialized() guard: lower trigger probability here than the
			// Shutdown() path (tab close usually happens while Slate is alive),
			// but the same crash pattern applies. Defensive consistency.
			if (Pin->ImeBridge.IsValid() && FSlateApplication::IsInitialized())
			{
				FWindowsApplication* WinApp = static_cast<FWindowsApplication*>(
					FSlateApplication::Get().GetPlatformApplication().Get());
				if (WinApp)
				{
					WinApp->RemoveMessageHandler(*Pin->ImeBridge);
				}
				// Detach + Reset are SharedPtr ops; when Slate is gone this block
				// is skipped and the bridge is released by its TSharedPtr dtor.
				Pin->ImeBridge->Detach();
				Pin->ImeBridge.Reset();
			}
#endif
			// Unbind UE's text input method system before resetting the browser.
			// Without this, ITextInputMethodSystem keeps a dangling context
			// pointer to the destroyed browser and crashes on the next async
			// IME focus event. Paired with BindInputMethodSystem in OnSpawnTab.
			//
			// Fix Point E: defensive Slate-availability guard — see Shutdown()
			// for full rationale. Tab-close typically precedes module teardown
			// so trigger probability is lower than Shutdown(), but the same
			// crash class applies. One-line cost.
			// Scope note: this guard catches the Slate-gone teardown class only;
			// it does NOT protect against widget-internal CEF IME state corruption.
			if (WebBrowserWidget.IsValid() && FSlateApplication::IsInitialized())
			{
				WebBrowserWidget->UnbindInputMethodSystem();
			}
			WebBrowserWidget.Reset();
			UNwiroIKBridge::Instance = nullptr;
			Pin->PanelDock.Reset();
		})
		.TabRole(NomadTab)
		[
			WebBrowserWidget.ToSharedRef()
		];

	// Bind the bridge
	if (!UNwiroIKBridge::Instance)
	{
		UNwiroIKBridge::Instance = NewObject<UNwiroIKBridge>();
	}
	WebBrowserWidget->BindUObject(TEXT("bridge"), UNwiroIKBridge::Instance);

	// Wire UE's text input method system into the CEF browser so CJK IMEs
	// (Bopomofo, Pinyin, Japanese MS-IME, Korean Hangul) work inside the
	// embedded chat input. UE 5.6's SWebBrowser uses CEF in off-screen
	// rendering (OSR) mode — there is no native child HWND on Windows or
	// NSView on macOS for IME messages to target. Instead, UE already
	// provides FCEFImeHandler + FCEFTextInputMethodContext which forward
	// to CefBrowserHost::ImeSetComposition / ImeCommitText. The bind call
	// below is the documented entry point that activates this whole chain
	// — without it, FCEFImeHandler::TextInputMethodSystem stays null and
	// every IME focus event is silently dropped on the InitContext check.
	// Customer-verified working with Bopomofo on UE 5.6 Windows. macOS
	// relies on this same call (no custom .mm bridge here by design — see
	// NwiroIKImeBridge.h header comment).
	if (ITextInputMethodSystem* Ims = FSlateApplication::Get().GetTextInputMethodSystem())
	{
		WebBrowserWidget->BindInputMethodSystem(Ims);
	}

	return PanelDock.ToSharedRef();
}
