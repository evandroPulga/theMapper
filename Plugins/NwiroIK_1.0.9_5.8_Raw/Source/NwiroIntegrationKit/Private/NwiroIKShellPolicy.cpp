// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroIKShellPolicy.h"
#include "Misc/SecureHash.h"
#include "Internationalization/Regex.h"
#include <initializer_list>

// Phase-1 v2 — hardened per council review (request_changes). Key posture:
//   * ALLOWLIST-DRIVEN, fail-closed: unknown commands default to ConfirmHighRisk,
//     never Allow. Allow only for a small known-safe introspective set with no
//     metacharacters and no out-of-project/secret path references.
//   * Delete detection is TOKENIZED + flag-aggregated (covers split flags
//     `rm -r -f`, long options `--recursive --force`, PowerShell aliases/prefixes
//     `ri -Rec -For`, cmd `del /s /q`).
//   * Dynamic-eval bootstraps (pipe-to-interpreter, $(...)/<(...)/backtick feeding
//     curl/wget/base64/certutil) are BLOCK, not merely high-risk.
//   * EncodedCommand detection is scoped to a powershell/pwsh head only (no more
//     bogus `-e` matches). Over-deep/over-nested wrappers fail closed to Block.
// Best-effort, NOT a sandbox (council #2): the exec site adds CWD scoping +
// per-command approval; this is one defense-in-depth layer.

namespace NwiroIKShellPolicy
{
	const TCHAR* RiskToString(ERisk Risk)
	{
		switch (Risk)
		{
			case ERisk::Allow:           return TEXT("allow");
			case ERisk::ConfirmHighRisk: return TEXT("confirm_high_risk");
			default:                     return TEXT("block");
		}
	}

	namespace
	{
		FString Collapse(const FString& In)
		{
			FString Out; Out.Reserve(In.Len());
			bool bPrevSpace = false;
			for (TCHAR C : In)
			{
				const bool bSpace = (C == TEXT(' ') || C == TEXT('\t') || C == TEXT('\n') || C == TEXT('\r'));
				if (bSpace) { if (!bPrevSpace && !Out.IsEmpty()) Out.AppendChar(TEXT(' ')); bPrevSpace = true; }
				else { Out.AppendChar(C); bPrevSpace = false; }
			}
			return Out.TrimStartAndEnd();
		}

		FString Unquote(const FString& In)
		{
			FString S = In.TrimStartAndEnd();
			if (S.Len() >= 2)
			{
				const TCHAR F = S[0];
				if ((F == TEXT('"') || F == TEXT('\'')) && S[S.Len() - 1] == F) return S.Mid(1, S.Len() - 2);
			}
			return S;
		}

		bool AnyContains(const FString& Low, std::initializer_list<const TCHAR*> Needles)
		{
			for (const TCHAR* N : Needles) { if (Low.Contains(N)) return true; }
			return false;
		}

		bool RegexHit(const FString& S, const FString& Pattern)
		{
			FRegexPattern Pat(Pattern);
			FRegexMatcher M(Pat, S);
			return M.FindNext();
		}

		bool HasNonAscii(const FString& S)
		{
			for (TCHAR C : S) { if (C > 127) return true; }
			return false;
		}

		/** Strip leading `VAR=value ` env-assignments so `FOO=1 bash -c ...` still unwraps. */
		FString StripLeadingEnv(const FString& In)
		{
			FString S = In;
			while (true)
			{
				// Match a single leading NAME=value token followed by a space.
				if (!RegexHit(S, TEXT("^[A-Za-z_][A-Za-z0-9_]*=\\S* "))) break;
				int32 Sp; if (!S.FindChar(TEXT(' '), Sp)) break;
				S = S.Mid(Sp + 1).TrimStart();
			}
			return S;
		}

		/** Program basename, lowercased, without path or .exe. */
		FString ProgBasename(const FString& Tok)
		{
			FString P = Unquote(Tok).ToLower();
			P.ReplaceInline(TEXT("%comspec%"), TEXT("cmd"));
			int32 Slash;
			if (P.FindLastChar(TEXT('/'), Slash)) P = P.Mid(Slash + 1);
			if (P.FindLastChar(TEXT('\\'), Slash)) P = P.Mid(Slash + 1);
			if (P.EndsWith(TEXT(".exe"))) P = P.LeftChop(4);
			return P;
		}

		/** Unwrap wrapper shells to the real inner command. Flags opaque/over-nested. */
		FString Unwrap(const FString& In, bool& bOutEncoded, bool& bOutTooNested)
		{
			FString Cmd = Collapse(In);
			int32 Depth = 0;
			for (; Depth < 4; ++Depth)
			{
				Cmd = StripLeadingEnv(Cmd);
				const FString Low = Cmd.ToLower();
				// Derive the wrapper head from the FIRST token only — passing the whole
				// line to ProgBasename() basenamed on a '/' inside the PAYLOAD and broke
				// wrapper detection entirely (council critical).
				FString HeadTok = Cmd; { int32 Sp; if (Cmd.FindChar(TEXT(' '), Sp)) HeadTok = Cmd.Left(Sp); }
				const FString Head = ProgBasename(HeadTok);
				const bool bPwshHead = (Head == TEXT("powershell") || Head == TEXT("pwsh"));
				// EncodedCommand only meaningful for powershell/pwsh (avoids bogus `-e` hits).
				if (bPwshHead && (Low.Contains(TEXT("-encodedcommand")) || RegexHit(Low, TEXT("\\s-enc?\\s")) || RegexHit(Low, TEXT("\\s-e(c|nc|ncodedcommand)?\\s"))))
				{
					bOutEncoded = true; return Cmd;
				}
				static const TCHAR* const Wrappers[] = {
					TEXT("cmd"), TEXT("powershell"), TEXT("pwsh"), TEXT("bash"), TEXT("sh"), TEXT("zsh")
				};
				bool bIsWrapper = false;
				for (const TCHAR* W : Wrappers) { if (Head == W) { bIsWrapper = true; break; } }
				if (!bIsWrapper) break;
				static const TCHAR* const Flags[] = { TEXT(" /c "), TEXT(" /k "), TEXT(" -c "), TEXT(" -lc "), TEXT(" -command "), TEXT(" -command:") };
				int32 Best = INDEX_NONE; int32 BestLen = 0;
				for (const TCHAR* Flag : Flags)
				{
					const int32 Idx = Low.Find(Flag, ESearchCase::IgnoreCase, ESearchDir::FromStart);
					if (Idx != INDEX_NONE && (Best == INDEX_NONE || Idx < Best)) { Best = Idx; BestLen = FCString::Strlen(Flag); }
				}
				if (Best == INDEX_NONE) break;
				Cmd = Unquote(Cmd.Mid(Best + BestLen));
			}
			// Still looks like a wrapper after the depth cap ⇒ can't reliably inspect.
			if (Depth >= 4)
			{
				FString HeadTok = Cmd; { int32 Sp; if (Cmd.FindChar(TEXT(' '), Sp)) HeadTok = Cmd.Left(Sp); }
				const FString Head = ProgBasename(HeadTok);
				for (const TCHAR* W : { TEXT("cmd"), TEXT("powershell"), TEXT("pwsh"), TEXT("bash"), TEXT("sh"), TEXT("zsh") })
				{
					if (Head == W) { bOutTooNested = true; break; }
				}
			}
			return Cmd;
		}

		bool IsDeleteVerb(const FString& Prog)
		{
			static const TCHAR* const Verbs[] = { TEXT("rm"), TEXT("del"), TEXT("erase"), TEXT("rd"), TEXT("rmdir"), TEXT("remove-item"), TEXT("ri"), TEXT("unlink") };
			for (const TCHAR* V : Verbs) { if (Prog == V) return true; }
			return false;
		}

		/** Aggregate recursive/force across ALL flag tokens (covers split flags, long
		 *  options, PowerShell prefixes, cmd switches). */
		void DeleteFlags(const TArray<FString>& Toks, const FString& Prog, bool& bRec, bool& bForce)
		{
			bRec = (Prog == TEXT("rd") || Prog == TEXT("rmdir")); // inherently recursive
			bForce = false;
			for (int32 i = 1; i < Toks.Num(); ++i)
			{
				const FString T = Toks[i].ToLower();
				if (T == TEXT("/s")) { bRec = true; continue; }
				if (T == TEXT("/q") || T == TEXT("/f")) { bForce = true; continue; }
				if (T.StartsWith(TEXT("--")))
				{
					if (T.StartsWith(TEXT("--rec"))) bRec = true;
					if (T.StartsWith(TEXT("--for"))) bForce = true;
					continue;
				}
				if (T.StartsWith(TEXT("-")) && T.Len() > 1)
				{
					const FString Word = T.Mid(1);
					if (Word.StartsWith(TEXT("rec"))) { bRec = true; continue; }   // -Recurse / -rec
					if (Word.StartsWith(TEXT("for"))) { bForce = true; continue; } // -Force / -for
					// bundled short flags: -rf, -fr, -r, -f, -vrf …
					bool bAlpha = true; for (TCHAR C : Word) { if (!FChar::IsAlpha(C)) { bAlpha = false; break; } }
					if (bAlpha && Word.Len() <= 8)
					{
						if (Word.Contains(TEXT("r"))) bRec = true;
						if (Word.Contains(TEXT("f"))) bForce = true;
					}
				}
			}
		}

		/** Small allowlist of known-safe, read-only/introspective commands. */
		bool IsAllowlisted(const FString& Prog, const TArray<FString>& Toks)
		{
			static const TCHAR* const Safe[] = {
				TEXT("ls"), TEXT("dir"), TEXT("pwd"), TEXT("echo"), TEXT("whoami"), TEXT("hostname"),
				TEXT("where"), TEXT("which"), TEXT("cat"), TEXT("type"), TEXT("head"), TEXT("tail"),
				TEXT("date"), TEXT("tree"), TEXT("wc"), TEXT("clear"), TEXT("cls")
			};
			for (const TCHAR* S : Safe) { if (Prog == S) return true; }
			// git: only known read-only subcommands.
			if (Prog == TEXT("git") && Toks.Num() >= 2)
			{
				const FString Sub = Toks[1].ToLower();
				// Read-only subcommands only — branch/remote/config can MUTATE, so they
				// fall through to ConfirmHighRisk (council).
				static const TCHAR* const SafeGit[] = { TEXT("status"), TEXT("diff"), TEXT("log"), TEXT("show"), TEXT("rev-parse"), TEXT("describe") };
				for (const TCHAR* G : SafeGit) { if (Sub == G) return true; }
			}
			// version probes only.
			if ((Prog == TEXT("node") || Prog == TEXT("npm") || Prog == TEXT("python") || Prog == TEXT("python3") || Prog == TEXT("pip") || Prog == TEXT("dotnet"))
				&& Toks.Num() == 2 && (Toks[1] == TEXT("--version") || Toks[1] == TEXT("-v") || Toks[1] == TEXT("version")))
			{
				return true;
			}
			return false;
		}
	}

	FString NormalizedHash(const FString& RawCommand)
	{
		bool bEnc = false, bNested = false;
		const FString Norm = Unwrap(RawCommand, bEnc, bNested).ToLower();
		// ASCII MD5 is adequate here: non-ASCII commands are flagged HighRisk by Assess,
		// and approval binding also keys on toolCallId, so hash collisions can't be
		// leveraged. (council low finding — acceptable for ASCII shells.)
		return FMD5::HashAnsiString(*Norm);
	}

	FAssessment Assess(const FString& RawCommand)
	{
		FAssessment A;
		bool bEncoded = false, bTooNested = false;
		const FString Display = Unwrap(RawCommand, bEncoded, bTooNested);
		A.DisplayCommand = Display;
		const FString Low = Display.ToLower();

		if (Display.IsEmpty()) { A.Risk = ERisk::Block; A.Reason = TEXT("empty command"); return A; }
		if (bEncoded)   { A.Risk = ERisk::Block; A.Reason = TEXT("obfuscated/encoded command (-EncodedCommand) cannot be inspected"); return A; }
		if (bTooNested) { A.Risk = ERisk::Block; A.Reason = TEXT("over-nested wrapper cannot be reliably inspected"); return A; }

		TArray<FString> Toks; Display.ParseIntoArrayWS(Toks);
		const FString Prog = Toks.Num() > 0 ? ProgBasename(Toks[0]) : FString();

		// ── BLOCK ─────────────────────────────────────────────────────────────
		// Delete family — tokenized + flag-aggregated.
		if (IsDeleteVerb(Prog))
		{
			bool bRec = false, bForce = false;
			DeleteFlags(Toks, Prog, bRec, bForce);
			if (bRec && bForce) { A.Risk = ERisk::Block; A.Reason = TEXT("recursive + forced delete"); return A; }
			if (bRec)           { A.Risk = ERisk::ConfirmHighRisk; A.Reason = TEXT("recursive delete"); A.bRequiresTypedConfirmation = true; return A; }
		}
		// Disk / filesystem destruction.
		if (AnyContains(Low, { TEXT("mkfs"), TEXT("diskpart"), TEXT("format c:"), TEXT("format /"), TEXT(">/dev/sd"), TEXT("> /dev/sd"), TEXT("of=/dev/") }))
		{ A.Risk = ERisk::Block; A.Reason = TEXT("disk/filesystem destruction"); return A; }
		// System control / fork bomb / registry destruction.
		if (Prog == TEXT("shutdown") || Prog == TEXT("reboot") || Prog == TEXT("halt") || Prog == TEXT("poweroff")
			|| Low.Contains(TEXT(":(){")) || Low.Contains(TEXT("reg delete")))
		{ A.Risk = ERisk::Block; A.Reason = TEXT("system control / destructive op"); return A; }
		// Dynamic-eval / obfuscation bootstrap.
		if (AnyContains(Low, { TEXT("| sh"), TEXT("|sh"), TEXT("| bash"), TEXT("|bash"), TEXT("| python"), TEXT("|python"),
				TEXT("| node"), TEXT("|node"), TEXT("| pwsh"), TEXT("|pwsh"), TEXT("| powershell"), TEXT("|powershell"),
				TEXT("| cmd"), TEXT("|cmd"), TEXT("| zsh"), TEXT("|zsh"), TEXT("iex "), TEXT("iex("), TEXT("invoke-expression") }))
		{ A.Risk = ERisk::Block; A.Reason = TEXT("pipe-to-interpreter / dynamic eval"); return A; }
		{
			const bool bSubst = AnyContains(Low, { TEXT("$("), TEXT("<("), TEXT("`") });
			const bool bFetchDecode = AnyContains(Low, { TEXT("curl"), TEXT("wget"), TEXT("iwr"), TEXT("invoke-webrequest"), TEXT("base64"), TEXT("certutil") });
			if (bSubst && bFetchDecode) { A.Risk = ERisk::Block; A.Reason = TEXT("eval bootstrap: substitution feeding fetch/decode"); return A; }
			if (AnyContains(Low, { TEXT("base64 -d"), TEXT("base64 --decode"), TEXT("certutil -decode"), TEXT("certutil /decode") }) && Low.Contains(TEXT("|")))
			{ A.Risk = ERisk::Block; A.Reason = TEXT("decode piped into a shell"); return A; }
		}

		// ── Precompute high-risk signals ──────────────────────────────────────
		const bool bMeta = AnyContains(Low, { TEXT("&&"), TEXT("||"), TEXT(";"), TEXT("|"), TEXT("$("), TEXT("`"), TEXT("<("), TEXT(">"), TEXT(">>") });
		const bool bSecret = AnyContains(Low, { TEXT(".ssh"), TEXT(".aws"), TEXT(".gnupg"), TEXT("id_rsa"), TEXT("id_ed25519"),
			TEXT("credentials"), TEXT(".npmrc"), TEXT("git-credentials"), TEXT(".kube"), TEXT(".docker") }) || RegexHit(Low, TEXT("\\.env($|[ /\\\\.\"'])"));
		const bool bOutside = AnyContains(Low, { TEXT("~/"), TEXT("~\\"), TEXT("$home"), TEXT("$env:"), TEXT("%userprofile%"), TEXT("/etc/"),
			TEXT("/usr/"), TEXT("/root"), TEXT("c:\\windows"), TEXT("c:/windows"), TEXT("..\\"), TEXT("../") });
		const bool bEnvIndirect = RegexHit(Low, TEXT("\\$\\{?[a-z_]")) || Low.Contains(TEXT("%")) || Low.Contains(TEXT("$env:"));

		// ── ALLOW (only known-safe, clean) ────────────────────────────────────
		if (!bMeta && !bSecret && !bOutside && !HasNonAscii(Display) && IsAllowlisted(Prog, Toks))
		{ A.Risk = ERisk::Allow; A.Reason = TEXT("allowlisted read-only command"); return A; }

		// ── CONFIRM HIGH-RISK (everything else — fail-closed default) ──────────
		A.Risk = ERisk::ConfirmHighRisk;
		A.bRequiresTypedConfirmation = true;
		if (bMeta)                                A.Reason = TEXT("command chaining / redirection / subshell");
		else if (RegexHit(Low, TEXT("\\b(curl|wget|nc|ncat|telnet|ssh|scp|sftp|ftp|invoke-webrequest|iwr)\\b"))) A.Reason = TEXT("network tool");
		else if (RegexHit(Low, TEXT("\\b(apt|apt-get|brew|choco|winget|gem|cargo|go)\\s+install")) || RegexHit(Low, TEXT("\\b(npm|yarn|pnpm)\\b.*(\\s-g\\b|global|install)")) || RegexHit(Low, TEXT("\\bpip3?\\s+install"))) A.Reason = TEXT("package manager / global install");
		else if (bSecret)                         A.Reason = TEXT("references credential/secret path");
		else if (bOutside)                        A.Reason = TEXT("references path outside the project");
		else if (HasNonAscii(Display))            A.Reason = TEXT("non-ASCII in command (possible homoglyph)");
		else if (bEnvIndirect)                    A.Reason = TEXT("environment-variable indirection");
		else                                      A.Reason = TEXT("command not on the read-only allowlist (default-deny)");
		return A;
	}
}
