# FAUP

**Frutiger Astro Pusher**: the update channel for Frutiger Astro (the build without the Learn tab). Every installed copy checks this repo for new versions, and `faup.py` builds and publishes them.

Made by **DOOG unincorporated**.

- [Install](#install)
- [How a copy updates itself](#how-a-copy-updates-itself)
- [Publishing an update](#publishing-an-update)
- [Publishing as Claude (or any AI agent)](#publishing-as-claude-or-any-ai-agent)
- [latest.json](#latestjson) · [Rules that matter](#rules-that-matter) · [Testing first](#testing-a-build-before-everyone-gets-it) · [Troubleshooting](#troubleshooting)

## Install

1. Download [`update/FrutigerAstro.zip`](https://github.com/binjin-maker/FAUP/raw/main/update/FrutigerAstro.zip), unzip it, and rename `FrutigerAstro.ex_` to `FrutigerAstro.exe`.
2. Turn off Smart App Control (*Windows Security > App & browser control > Smart App Control settings > Off*). The EXE isn't code-signed, so Windows won't run it otherwise.
3. Put it in a folder you can write to (Desktop, Documents, a USB stick, **not** Program Files) and run it.

That's the only manual install. From then on, new versions arrive through the app's **Update** tab.

While it's running, the app shows an envelope on the taskbar and in Alt+Tab, and the EXE file itself keeps the eye icon. If you **pin** it to the taskbar, Windows draws the pin with the EXE's icon (the eye). To make the pin an envelope too, right-click the pin, right-click *FrutigerAstro* in the menu, choose *Properties > Change Icon*, and pick the envelope. It's the second icon inside the EXE.

## How a copy updates itself

The **Update** tab button carries a little status light, so you can see the state from any tab. The tab itself shows a big status light, the installed version, one button, and *What's new* (the changelog of the last 10 releases).

| Light | Headline | Button |
|---|---|---|
| Green | You're up to date | Check for updates |
| Amber | Update available (version line shows `1.14.6 → 1.14.7`) | Install update |
| Blue | Checking for updates… / Downloading update… (with a progress bar) / Installing update… | Checking… / Updating… |
| Red | You're offline / Couldn't check for updates / Update didn't install (a line underneath says why) | Check for updates or Install update |

1. **Check.** Every launch, and every press of **Check for updates**, the app reads
   `https://raw.githubusercontent.com/binjin-maker/FAUP/main/update/latest.json`
2. **Compare.** If that file's `version` is higher than the app's own, the light turns amber (so does the little light on the Update tab itself), and the status bar says *Update X is ready — open the Update tab to install it*.
3. **Download and verify.** **Install update** downloads the file at `url` and shows its progress. The file is rejected unless it starts like a Windows EXE, is at least 20 KB, and its SHA-256 matches `sha256`. If any check fails, nothing on disk changes.
4. **Swap.** The download is saved as `FrutigerAstro.exe.new`, the running EXE is renamed to `.old`, and the new one takes its place. The app then restarts with *Updated to vX — enjoy!* in the status bar and deletes the `.old` file on its next launch.

Updates never install by themselves; the user presses Install update. Settings in `FrutigerMacro.ini` (hotkeys, presets, Instruct values) and `.fmac` macro files are never touched.

## What's in here

```
FAUP/
├── README.md                this file
├── CLAUDE.md                short rules for AI agents (points here)
├── faup.py                  build + publish tool
├── src/
│   ├── FrutigerMacro.c      the whole app (Win32, one file)
│   ├── icon.ico             the eye: the EXE's own icon in File Explorer
│   ├── mail.ico             the envelope: what the running app shows on the taskbar and in Alt+Tab
│   └── app.rc, app.manifest resources (icons + manifest)
└── update/
    ├── latest.json          the file every copy reads
    ├── FrutigerAstro.exe    the current build (what latest.json points at)
    └── FrutigerAstro.zip    first-time download: README.txt + FrutigerAstro.ex_
```

## Publishing an update

### One-time setup

- **Python 3** and **Git**
- **Zig** (the compiler), installed through pip: `pip install ziglang`. It cross-compiles the Windows EXE from Windows, macOS or Linux, so you don't need Visual Studio or MinGW.
- A clone you can push from: `git clone https://github.com/binjin-maker/FAUP`

### The one command

Make your changes in `src/FrutigerMacro.c`, then, from the repo folder:

```
python faup.py publish 1.14.8 "Magic mode skips spaces now" "Fixed the typo slider"
```

Each quoted note becomes one bullet in the app's *What's new* box. Notes are optional, but users see them, so write them for users. `publish` runs these steps in order and stops at the first problem:

1. Makes sure you're on `main` and that `updRepo` in the source still says `binjin-maker/FAUP`.
2. Pulls the latest `main` and checks that the new version is higher than the one in `latest.json`.
3. Writes the version into both `APP_VERSION` and `APP_VERSION_W` in the source.
4. Builds the EXE with Zig and checks that it really reports the new version.
5. Copies it to `update/FrutigerAstro.exe`, rebuilds `update/FrutigerAstro.zip`, then commits (*FrutigerAstro 1.14.8*) and pushes.
6. Writes `latest.json` with the version, a link to the EXE **pinned to the commit from step 5**, the EXE's SHA-256 and the notes, then commits (*Release 1.14.8*) and pushes.

Copies pick the release up on their next launch or **Check for updates**. GitHub can take up to about 5 minutes to start serving the new `latest.json`.

### Other commands

| Command | What it does |
|---|---|
| `python faup.py build` | Builds `build/FrutigerAstro.exe` from `src/` and prints its version and SHA-256. Publishes nothing, so use it to try a change first. |
| `python faup.py check` | Re-checks the live release: `latest.json` is valid, `url` is pinned to a commit in this repo, and that EXE matches `sha256` and reports the right version. Works in shallow clones. |
| `python faup.py publish ... --no-push` | Makes both commits but doesn't push them. Run `git push` afterwards and push **both**, or copies will try to download a commit GitHub doesn't have. |
| `python faup.py publish ... --allow-repo-change` | Only for moving the channel (see *Rules*). Without it, `publish` refuses a build whose `updRepo` isn't `binjin-maker/FAUP`. |

### Publishing by hand (without faup.py)

1. Raise the version in **both** lines at the top of the updater section of `src/FrutigerMacro.c`:
   ```c
   #define APP_VERSION   "1.14.8"
   #define APP_VERSION_W L"1.14.8"
   ```
2. Build it from inside `src/`:
   ```
   python -m ziglang cc -target x86_64-windows-gnu -O2 -s -o FrutigerAstro.exe FrutigerMacro.c app.rc -Wl,--subsystem,windows -luser32 -lgdi32 -lcomdlg32 -lcomctl32 -lwinmm -ldwmapi -luxtheme -lwininet -lbcrypt -lshell32
   ```
3. Move the EXE to `update/FrutigerAstro.exe`, commit and push. Copy that commit's full hash (`git rev-parse HEAD`).
4. Get the EXE's SHA-256 with `certutil -hashfile update\FrutigerAstro.exe SHA256` on Windows, or `sha256sum update/FrutigerAstro.exe` elsewhere.
5. Fill in `update/latest.json` (format below), then commit and push.

## Publishing as Claude (or any AI agent)

Binjin usually asks for a change in plain words ("make Restart do X", "push this update") or sends a full `FrutigerMacro.c`. Follow these steps in order. Every step has a check. If a check fails, stop and tell him what failed instead of working around it.

**1. Get push access and clone.** You need permission to push to `binjin-maker/FAUP`. In a Claude Code session, add the repo with push access before cloning. Anywhere else, ask Binjin. Clone with some history:
```
git clone --depth 50 https://github.com/binjin-maker/FAUP && cd FAUP
git config user.name >/dev/null || git config user.name "Claude"
git config user.email >/dev/null || git config user.email "noreply@anthropic.com"
```

**2. Install the compiler.**
```
pip install ziglang          # add --break-system-packages if pip says the environment is externally managed
python3 -m ziglang version   # check: prints a version number
```

**3. Read the current release.**
```
python3 faup.py check        # check: ends with "ok: hash, EXE and version all match"
```
Note the published version. The next version adds 1 to the last number (1.14.7 → 1.14.8) unless Binjin asks for something else.

**4. Make the change** in `src/FrutigerMacro.c`, or replace the file with the one Binjin sent. Then confirm three things:
- `grep -n 'updRepo\[' src/FrutigerMacro.c` still shows `L"binjin-maker/FAUP"`. A file Binjin sends may come from another channel. If it points somewhere else, change it back. `publish` refuses to run otherwise.
- You didn't hand-edit `APP_VERSION` / `APP_VERSION_W`. `publish` sets both.
- `src/` is public. If the change includes code that shouldn't be public, stop and ask Binjin.

**5. Build and test.**
```
python3 faup.py build        # check: last line starts with "built build/FrutigerAstro.exe"
```
Compiler errors print above that line. For UI changes, run the build under Wine first (see [Testing](#testing-a-build-before-everyone-gets-it)).

**6. Publish.**
```
python3 faup.py publish 1.14.8 "One short note per change, written for users"
```
Check: it ends with `published 1.14.8` and the pinned EXE link. A line saying `This repository moved` is just GitHub noting a different letter case in the clone URL, and is harmless.

**7. Verify from the outside.**
```
python3 faup.py check
curl -s "https://raw.githubusercontent.com/binjin-maker/FAUP/main/update/latest.json?t=$RANDOM"
```
The JSON must show the new version. If it still shows the old one, wait a minute and retry, since GitHub's cache can take up to about 5 minutes. Then download its `url` and compare the hash: `curl -s -o /tmp/fa.exe "<url>" && sha256sum /tmp/fa.exe` must equal `sha256`.

**8. Report back.** Tell Binjin the version number. Installed copies show it on their next launch or after **Check for updates**. If he wants the EXE itself, send `update/FrutigerAstro.exe` renamed to `FrutigerAstro.ex_`, because browsers often block `.exe` downloads.

**Never:**
- force-push or rewrite `main`'s history, because `latest.json` links to exact commits
- publish a version that isn't higher than the current one
- point `url` at `main`, or rename or move anything in `update/`
- store the EXE in Git LFS, or make the repo private
- commit `build/` (it's in `.gitignore`)

**If a push fails halfway** (someone pushed in between): run `git pull --rebase origin main`, then run the same `publish` command again. `latest.json` is written only after the EXE commit is pushed, so retrying is safe.

### Where things are in the source

| What | Where in `src/FrutigerMacro.c` |
|---|---|
| Version, update repo, update download/verify/swap | the `/* ---------- updater ---------- */` section (`APP_VERSION`, `updRepo`, `CheckThread`, `InstallThread`) |
| Update tab drawing (light, headline, progress, What's new) | `drawUpdatePage`, `fillNotes`, `updSync` |
| Typing engine (Humanizer, typos, pauses) | `/* ---------- Instruct / Humanizer ---------- */` |
| Instruct buttons (Type, Stop, Restart, Clear, Magic) | `startType`, `stopAll`, `restartType`, `magicStart` and the `WM_COMMAND` cases in `WndProc` |
| Layout of every tab | `buildUI` (controls) and `renderPages` (panels and labels) |
| Taskbar envelope vs EXE eye | `IDI_MAIL` in `WinMain` and `src/app.rc` |

## `latest.json`

```json
{
  "version": "1.14.8",
  "url": "https://raw.githubusercontent.com/binjin-maker/FAUP/<commit hash>/update/FrutigerAstro.exe",
  "sha256": "<64 hex characters>",
  "notes": "1.14.8\n- Magic mode skips spaces now\n\n1.14.7\n- Simpler Update tab"
}
```

| Field | Rules |
|---|---|
| `version` | `major.minor.patch`. A copy only installs a version **higher** than its own. |
| `url` | A direct `raw.githubusercontent.com` link to the EXE, pinned to a commit hash rather than `main` (see below). A `github.com/.../blob/...` link downloads a web page, which the app rejects. |
| `sha256` | The EXE's SHA-256 in hex. The app refuses the download if it doesn't match. An empty value skips the check entirely, so always fill it in. |
| `notes` | Shown under *What's new*. `\n` starts a new line, lines starting with `- ` become bullets, and `*` and backticks are stripped. Users read this, so leave out repo names and other technical details. |

## Rules that matter

- **Keep the repo public.** Copies download anonymously, and a private repo looks like "nothing published" to them.
- **Keep the branch named `main`, and keep `update/latest.json` where it is.** Every copy has `binjin-maker/FAUP/main/update/latest.json` baked in. Renaming or moving the repo can cut every existing copy off from updates. If you ever have to move, change `updRepo` in the source, publish that build here one last time with `--allow-repo-change`, and then move.
- **Versions only go up.** To roll back, publish the old code again under a higher number. For example: `git checkout <old commit> -- src`, then `python faup.py publish 1.14.9 "Rolled back to 1.14.7"`.
- **The EXE must report the version in `latest.json`.** If it reports a lower one, every copy that installs it sees "Update available" again forever. `faup.py` sets both version lines and verifies the build to prevent this.
- **Why `url` is pinned to a commit:** GitHub's CDN caches `main` links for a few minutes. With `url` pointing at `main/update/FrutigerAstro.exe`, a copy could receive the *old* EXE alongside the *new* hash and fail the safety check. A commit link always serves the same bytes.
- **A missing `latest.json` looks like "You're up to date"** to users, so a broken channel doesn't nag anyone. If copies never see a release, run `python faup.py check` and the `curl` from step 7 above.

## Testing a build before everyone gets it

**On Windows:** `python faup.py build` gives you an EXE you can run straight away.

**On Linux with Wine** (handy for AI agents in a sandbox):
```
apt-get install -y wine64 xvfb xdotool imagemagick
Xvfb :99 -screen 0 1600x1000x24 & export DISPLAY=:99
wine build/FrutigerAstro.exe &
sleep 10 && import -window root shot.png      # the window title is "FrutigerMacro"
```
Use `xdotool` to click tabs and buttons, then look at the screenshot.

**A whole update, on one PC:** point a single copy at a separate repo by adding this to the `FrutigerMacro.ini` next to it:
```ini
[Update]
Repo=binjin-maker/FAUP-test
```
Then publish to that repo from a clone whose `origin` is `FAUP-test`. `faup.py` warns that the build checks a different repo, which is expected here. Delete the line to go back to FAUP.

## Troubleshooting

What the Update tab says, and what to do:

| Shown | Meaning |
|---|---|
| **You're offline** — *Connect to the internet, then check again.* | No connection (DNS or connect failed). Everything else in the app still works. |
| **Couldn't check for updates** — *The update server didn't answer.* | The network blocks `raw.githubusercontent.com` (school/work firewall), or it timed out. |
| **Couldn't check for updates** — *The update server is busy.* | GitHub returned an error, such as a rate limit. Try later. |
| **Update didn't install** — *The download didn't finish.* | The connection dropped, or `url` points at a commit that was never pushed. Run `python faup.py check`. |
| **Update didn't install** — *The download came through damaged.* | The file at `url` isn't an EXE (for example a web page link instead of a raw link). |
| **Update didn't install** — *The download didn't pass its safety check.* | `sha256` doesn't match the file at `url`. Run `python faup.py check`. |
| **Update didn't install** — *Couldn't save the update…* | The app is in a protected folder like Program Files. Move it somewhere writable. |
| **Update didn't install** — *Couldn't swap in the update…* | Another copy is running from the same file, or the folder is read-only. Close it and retry. |
| Status bar: *Stop recording, playing or typing before updating.* | Finish what the app is doing, then press Install update again. |

Failed installs never change the EXE on disk, so retrying is always safe.
