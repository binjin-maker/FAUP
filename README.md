# FAUP

**Frutiger Astro Pusher** — the build-and-release system for Frutiger Astro. It cross-compiles the app, publishes updates to two channels, and every installed copy updates itself from here.

- [Install](#install) · [Activation](#activation) · [How updates work](#how-updates-work)
- [The two channels](#the-two-channels-frutiger-astro--astro-beta)
- [What a copy sends — and never sends](#what-a-copy-sends--and-never-sends) · [Frutiger Servers](#frutiger-servers)
- [Publishing](#publishing) · [Publishing as an AI agent](#publishing-as-claude-or-any-ai-agent)
- [latest.json](#latestjson) · [Rules](#rules-that-matter) · [Troubleshooting](#troubleshooting)

## Install

1. Download [`update/FrutigerAstro.zip`](https://github.com/binjin-maker/FAUP/raw/main/update/FrutigerAstro.zip), unzip, rename `FrutigerAstro.ex_` to `FrutigerAstro.exe`.
2. Turn off Smart App Control (*Windows Security > App & browser control > Smart App Control settings > Off*). The EXE isn't code-signed.
3. Put it in a folder you can write to (Desktop, Documents, a USB stick — **not** Program Files) and run it.

While running, the app shows an envelope on the taskbar; the EXE file keeps the eye icon.

## Activation

The first launch asks for an **access code**. Enter it and the copy activates once — it won't ask again on that machine. The code is yours to hand out; it ships as a SHA-256 hash, so it isn't readable in the binary.

**Three wrong codes removes the copy.** It deletes *only its own .exe* — your macros, settings, and every other file are untouched — and exists to stop someone brute-forcing a copy they weren't given. A fresh, un-activated copy holds no data, so nothing of yours is ever lost. The activation screen says this up front.

> Distribution note: the gate lives in the app, so every copy that updates to an activation-aware build asks for the code on its next launch. Make sure anyone you've given a copy to has the code before you promote such a build.

## How updates work

The **Update** tab shows a status light, the installed version, a changelog (*What's new*), one button, and two toggles — *Auto-check* and *Auto-install*, both on by default. The button also carries a small light so you can read the state from any tab.

1. On launch and on **Check for updates**, the copy reads its channel's JSON on `main`.
2. If the published version is higher, the light turns amber and *What's new* shows the notes.
3. **Install update** downloads the EXE, confirms it's a real EXE and that its SHA-256 matches, then swaps it in and restarts. Any failed check changes nothing on disk.
4. Settings (`FrutigerMacro.ini`) and `.fmac` macros are never touched by an update.

## The two channels: Frutiger Astro & Astro Beta

| | **Frutiger Astro** (production) | **Astro Beta** (your test build) |
|---|---|---|
| Who runs it | everyone you give a code to | just you, locally |
| Activation code | required | none |
| Telemetry | yes (see below) | none |
| Where it lives | published to this repo | **local only** — built to `build/`, never published |
| Updates | from `update/latest.json` | none (you run the newest local build) |
| Built by | `faup.py promote` | `faup.py beta` |

The flow: make a change → `faup.py beta <ver>` builds your local `AstroBeta.exe` → run and test it → when happy, `faup.py promote <ver>` ships the *same* version to Frutiger Astro. Beta is never published, so a copy you didn't hand out can't download an un-activated build.

## What a copy sends — and never sends

Frutiger Astro (not Beta) sends a small **heartbeat** so you can see how many copies are active, what version they run, and whether one is busy. A heartbeat is only ever: a random install id, the app name, the version, and an event — `launch`, `activate`, `ping` (every 10s), `working` (a macro/instruct is running — the *state* only, never its content), or `offline` (sent on close).

It **never** sends your files, anything typed into Instruct, macros, folder names, usernames, or anything else off the PC. Heartbeats are **on by default** and start **after** a copy is activated: a copy finds the dashboard on its own by UDP LAN discovery (broadcasts `FRUTIGER-DISCOVER` to `:8788`), falling back to the baked localhost URL. Set `[Servers] Url=` in the ini to point a copy at a specific endpoint (use this for copies out on the internet). Astro Beta sends nothing at all.

There is **no remote control.** Frutiger Servers only listens — it cannot send anything back to a copy, and nothing (not Frutiger Servers, not FAUP) can delete or disable a copy remotely. The only self-delete is the local three-wrong-codes one above, triggered by the person at that keyboard.

## Frutiger Servers

`servers/frutiger_servers.c` is a self-hosted, native Win32 dashboard (`FrutigerServers.exe` — no installs). A standalone Python version (`servers/frutiger_servers.py`) is kept for reference.

It shows your **active-user count**, total installs, versions in the wild, and each copy with a coloured bullet — **green Online**, **amber Working** (busy with a macro/instruct), **black Offline** — plus a last-seen time and a small **activity sparkline** of its recent state. Offline copies stay listed, so your counts persist. You can give any copy a nickname for your own reference, and push a plain-text **display-only announcement** that copies show as a dismissable banner (never a command — a copy only ever *renders* it).

Copies on the **same network connect automatically** — the dashboard binds `0.0.0.0` and answers UDP discovery, so there's nothing to configure. For copies out on the internet, point them at a reachable endpoint by adding to the `FrutigerMacro.ini` next to their exe:
```ini
[Servers]
Url=http://YOUR-HOST:8787/beat
```

## Publishing

One-time: **Python 3**, **Git**, and **Zig** (`pip install ziglang` — it cross-compiles the Windows EXE from any OS). Clone: `git clone https://github.com/binjin-maker/FAUP`.

```
python faup.py beta 1.14.9 "Trying a new splash"      # -> Astro Beta, you test
python faup.py promote 1.14.9 "New splash"            # -> Frutiger Astro, everyone
```

Each quoted note is one *What's new* bullet (write them for users). `beta`/`promote` both: pull main, check the new version is higher than that channel's published one, stamp `APP_VERSION`, build (Zig), verify the EXE reports that version, commit + push the EXE, then write the channel JSON with the EXE link **pinned to that commit** and push again.

Other commands:

| Command | Does |
|---|---|
| `python faup.py build [--beta]` | build to `build/` only, print version + SHA-256. Nothing published. |
| `python faup.py check [--beta]` | re-verify the live channel: JSON valid, url pinned to a commit in this repo, EXE matches the hash and reports the right version. Works in shallow clones. |
| `... --no-push` | make both commits, don't push. Then `git push` **both**. |
| `... --allow-repo-change` | only when deliberately moving a channel to another repo. |

### Publishing by hand

Bump **both** `APP_VERSION` and `APP_VERSION_W` in `src/FrutigerMacro.c`, build from `src/` with the flags in `faup.py` (add `-DASTRO_BETA` for the Beta build), move the EXE to `update/`, commit, push, then fill `update/latest.json` (or `beta.json`) with the version, a `raw.githubusercontent.com` link **pinned to that commit**, the EXE's SHA-256 (`certutil -hashfile … SHA256`), and notes. Commit and push.

## `latest.json`

```json
{
  "version": "1.14.9",
  "url": "https://raw.githubusercontent.com/binjin-maker/FAUP/<commit hash>/update/FrutigerAstro.exe",
  "sha256": "<64 hex chars>",
  "notes": "1.14.9\n- New splash\n\n1.14.8\n- ..."
}
```

| Field | Rule |
|---|---|
| `version` | `major.minor.patch`. A copy installs only a **higher** version than its own. |
| `url` | direct `raw.githubusercontent.com` link to the EXE, pinned to a **commit hash** (not `main`). A `.../blob/...` link is a web page and is rejected. |
| `sha256` | the EXE's hash; a mismatch makes copies refuse the download. Never leave it empty. |
| `notes` | shown in *What's new*. `\n` = newline, `- ` = bullet. Users read this — keep it plain. |

## Rules that matter

- **Keep the repo public** (copies download anonymously) and the **branch `main`**; every copy has `binjin-maker/FAUP/main/update/<json>` baked in.
- **Versions only go up** per channel. To roll back, re-publish old code under a higher number.
- **The EXE must report the version in its JSON** — `faup.py` stamps and verifies this, so copies don't reinstall forever.
- **`url` is pinned to a commit** because GitHub caches `main` links for minutes; a commit link always serves the same bytes.
- A missing JSON reads as "up to date", so a broken channel never nags anyone.

## Publishing as Claude (or any AI agent)

Binjin usually asks in plain words, or hands over a `FrutigerMacro.c`. Follow these in order; if a check fails, stop and say so rather than working around it.

1. **Clone with push access** (`git clone --depth 50 https://github.com/binjin-maker/FAUP`); set `git config user.name` / `user.email` if unset.
2. **`pip install ziglang`** (add `--break-system-packages` if needed); confirm `python3 -m ziglang version`.
3. **`python3 faup.py check`** and `check --beta` — note the current versions. Next version = +1 on the last number unless told otherwise.
4. **Make the change** in `src/FrutigerMacro.c` (or drop in Binjin's file). Then confirm: `grep updRepo` still shows `L"binjin-maker/FAUP"`; you did not hand-edit `APP_VERSION` (faup stamps it); nothing private is going into the public `src/`.
5. **Beta first:** `python3 faup.py beta <ver> "note"`, then test under Wine (below) if the change is visual. When it's good, **`python3 faup.py promote <ver> "note"`**.
6. **Verify live:** `python3 faup.py check`, then `curl -s "https://raw.githubusercontent.com/binjin-maker/FAUP/main/update/latest.json?t=$RANDOM"` shows the new version (GitHub's cache can lag ~5 min); download its `url` and confirm the hash.
7. **Report the version to Binjin.** Send any EXE as a `.ex_` file (browsers block `.exe`).

**Never:** force-push or rewrite `main`; reuse or lower a version; point `url` at `main`; make the repo private; use Git LFS for the EXE; commit `build/`. If a push races someone, `git pull --rebase origin main` and re-run the same command — the JSON is written only after the EXE commit lands, so retrying is safe.

Testing a build under Wine (sandbox-friendly):
```
apt-get install -y wine64 xvfb xdotool imagemagick
Xvfb :99 -screen 0 1600x1000x24 & export DISPLAY=:99
wine build/FrutigerAstro.exe & sleep 10 && import -window root shot.png   # window title: "Frutiger Astro"
```

### Where things are in `src/FrutigerMacro.c`

| What | Where |
|---|---|
| Version, update repo/channel, download/verify/swap | `/* updater */` — `APP_VERSION`, `updRepo`, `UPD_JSON`, `CheckThread`, `InstallThread` |
| Beta vs production split | `#ifdef ASTRO_BETA` (sets name + channel, compiles out activation + telemetry) |
| Activation gate + self-delete | `runActivation`, `codeOk`, `selfDestruct` (production only) |
| Heartbeat + LAN discovery | `beat`, `BeatThread`, `discoverServer`, `resolveServerUrl` (production only; on by default) |
| Update tab drawing | `drawUpdatePage`, `fillNotes`, `updSync` |
| Splash | `runSplash`, `splashFrame` |
| Opens-on-Recorder / minimize-to-tray | `WinMain` (`showPage(PG_MACRO)`), `FrameProc` `WM_SIZE`, `trayAdd`/`trayRestore` |
| Instruct (fixed) vs Winlan (humanizer) tabs | `buildPage`, `tog[]`/`TG_*`, page enum `PG_INSTR`/`PG_WINLAN` |
| Humanizer auto-go + review breaks | `HState`, `hDelay`/`hPause`, `reviewBreak`, `goSkip`/`inBreak`, `ID_GO` |
| Diff-fixer ("Fix target") | `startDiffFix`, `DiffThread`, `clipGet`/`clipSet`, LCS walk |
| Humanized playback / Pause (F7) | `PlayThread` (`playHumanize`), `paused` in `waitUntil`, `HK_N+1` hotkey |
| Breakers panel (night mode, tray, splash, bubbles, confirm) | `runBreakers`, `BreakerProc`, `brk[]`/`BRK_*`, `loadBreakers`/`saveBreakers` |

## Troubleshooting

| Update tab shows | Meaning |
|---|---|
| **You're offline** | no connection; everything else still works |
| **Couldn't check for updates** | `raw.githubusercontent.com` blocked (firewall) or GitHub errored; try later |
| **Update didn't install — didn't finish** | connection dropped, or `url`'s commit was never pushed — run `faup.py check` |
| **…came through damaged** | file at `url` isn't an EXE (a `blob` link instead of raw) |
| **…didn't pass its safety check** | `sha256` doesn't match the EXE at `url` |
| **…couldn't save / swap in** | app is in a protected folder, or another copy is open — move it somewhere writable and retry |

Failed installs never change the EXE on disk, so retrying is always safe.
