# FAUP

**Frutiger Astro Pusher**: the update channel for Frutiger Astro (the build without the Learn tab). Every copy of the app checks this repo for new versions, and `faup.py` builds and pushes them.

Made by **DOOG unincorporated**.

## Install

1. Download [`update/FrutigerAstro.zip`](https://github.com/binjin-maker/FAUP/raw/main/update/FrutigerAstro.zip), unzip it, and rename `FrutigerAstro.ex_` to `FrutigerAstro.exe`.
2. Turn off Smart App Control (*Windows Security > App & browser control > Smart App Control settings > Off*). The EXE isn't code-signed, so Windows won't run it otherwise.
3. Put it in a folder you can write to (Desktop, Documents, a USB stick, **not** Program Files) and run it.

That's the only manual install. From then on, new versions arrive through the app's **Update** tab.

While it's running, the app shows an envelope on the taskbar and in Alt+Tab, and the EXE file itself keeps the eye icon. If you **pin** it to the taskbar, Windows draws the pin with the EXE's icon (the eye). To make the pin an envelope too, right-click the pin, right-click *FrutigerAstro* in the menu, choose *Properties > Change Icon*, and pick the envelope. It's the second icon inside the EXE.

## How a copy updates itself

1. **Check.** On launch (when *Check on launch* is ticked, which is the default) and whenever you press **Check now**, the app reads
   `https://raw.githubusercontent.com/binjin-maker/FAUP/main/update/latest.json`
2. **Compare.** If that file's `version` is higher than the app's own, the Update tab gets an orange dot and the status bar says *Update vX is available*. The tab shows the release notes under *What's new*.
3. **Download and verify.** **Install update** downloads the file at `url`. It's rejected unless it starts like a Windows EXE, is at least 20 KB, and its SHA-256 matches `sha256`. If any check fails, nothing on disk changes.
4. **Swap.** The download is saved as `FrutigerAstro.exe.new`, the running EXE is renamed to `.old`, and the new one takes its place. The app restarts with *Updated to vX — enjoy!* in the status bar and deletes the `.old` file on its next launch.

With *Auto-install updates* ticked, step 3 starts on its own as soon as an update is found (unless you're recording, playing or typing). Settings in `FrutigerMacro.ini` (hotkeys, presets, Instruct values) and your `.fmac` files are never touched. **Open GitHub page** opens this repo.

## What's in here

```
FAUP/
├── faup.py                  build + publish tool
├── src/
│   ├── FrutigerMacro.c      the whole app
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
python faup.py publish 1.14.7 "Magic mode skips spaces now" "Fixed the typo slider"
```

Each quoted note becomes one bullet in the app's *What's new* box. `publish` runs these steps in order and stops at the first problem:

1. Pulls the latest `main` and makes sure the new version is higher than the one in `latest.json`.
2. Writes the version into both `APP_VERSION` and `APP_VERSION_W` in the source.
3. Builds the EXE with Zig and checks that it really reports the new version.
4. Copies it to `update/FrutigerAstro.exe`, rebuilds `update/FrutigerAstro.zip`, then commits (*FrutigerAstro 1.14.7*) and pushes.
5. Writes `latest.json` with the version, a link to the EXE **pinned to the commit from step 4**, the EXE's SHA-256 and the notes (the last 10 releases are kept), then commits (*Release 1.14.7*) and pushes.

Copies pick the release up on their next launch or **Check now**. GitHub can take up to about 5 minutes to start serving the new `latest.json`.

### Other commands

| Command | What it does |
|---|---|
| `python faup.py build` | Builds `build/FrutigerAstro.exe` from `src/` and prints its version and SHA-256. Publishes nothing, so use it to try a change first. |
| `python faup.py check` | Re-checks the live release: `latest.json` is valid, `url` points at this repo, and the EXE in that commit matches `sha256` and reports the right version. |
| `python faup.py publish ... --no-push` | Makes both commits but doesn't push them. Run `git push` afterwards and push **both**, or copies will try to download a commit GitHub doesn't have. |

### Publishing by hand (without faup.py)

1. Raise the version in **both** lines at the top of the updater section of `src/FrutigerMacro.c`:
   ```c
   #define APP_VERSION   "1.14.7"
   #define APP_VERSION_W L"1.14.7"
   ```
2. Build it from inside `src/`:
   ```
   python -m ziglang cc -target x86_64-windows-gnu -O2 -s -o FrutigerAstro.exe FrutigerMacro.c app.rc -Wl,--subsystem,windows -luser32 -lgdi32 -lcomdlg32 -lcomctl32 -lwinmm -ldwmapi -luxtheme -lwininet -lbcrypt -lshell32
   ```
3. Move the EXE to `update/FrutigerAstro.exe`, commit and push. Copy that commit's full hash (`git rev-parse HEAD`, or from the commit's page on GitHub).
4. Get the EXE's SHA-256 with `certutil -hashfile update\FrutigerAstro.exe SHA256` on Windows, or `sha256sum update/FrutigerAstro.exe` elsewhere.
5. Fill in `update/latest.json` (format below), then commit and push.

## `latest.json`

```json
{
  "version": "1.14.7",
  "url": "https://raw.githubusercontent.com/binjin-maker/FAUP/<commit hash>/update/FrutigerAstro.exe",
  "sha256": "<64 hex characters>",
  "notes": "1.14.7\n- Magic mode skips spaces now\n- Fixed the typo slider\n\n1.14.6\n- First release from FAUP"
}
```

| Field | Rules |
|---|---|
| `version` | `major.minor.patch`. A copy only installs a version **higher** than its own. |
| `url` | A direct `raw.githubusercontent.com` link to the EXE, pinned to a commit hash rather than `main` (see below). A `github.com/.../blob/...` link downloads a web page, which the app rejects. If it's empty, the app reports that the release has no .exe attached. |
| `sha256` | The EXE's SHA-256 in hex. The app refuses the download if it doesn't match. An empty value skips the check entirely, so always fill it in. |
| `notes` | Shown under *What's new*. `\n` starts a new line, lines starting with `- ` become bullets, and `*` and backticks are stripped. |

## Rules that matter

- **Keep the repo public.** Copies download anonymously, so a private repo looks like "no update has been published" to them.
- **Keep the branch named `main` and keep `update/latest.json` where it is.** Every copy has `binjin-maker/FAUP/main/update/latest.json` baked in. Renaming or moving the repo can cut every existing copy off from updates. If you ever have to move, change `updRepo` in the source, publish that build here one last time, and then move.
- **Versions only go up.** To roll back, publish the old code again under a higher number. For example, `git checkout <old commit> -- src` and then `python faup.py publish 1.14.9 "Rolled back to 1.14.7"`.
- **The EXE must report the version in `latest.json`.** If it reports a lower one, every copy that installs it sees "update available" again and, with Auto-install on, reinstalls it in a loop. `faup.py` sets both version lines and verifies the build to prevent this.
- **Why `url` is pinned to a commit:** GitHub's CDN caches `main` links for a few minutes. With `url` pointing at `main/update/FrutigerAstro.exe`, a copy could receive the *old* EXE alongside the *new* hash and fail the checksum. A commit link always serves the same bytes.

## Testing a build on one PC first

`python faup.py build` gives you an EXE you can run straight away. To test the whole update flow without touching everyone else, point a single copy at a separate repo by adding this to the `FrutigerMacro.ini` next to it:

```ini
[Update]
Repo=binjin-maker/FAUP-test
```

Then publish to that repo from a clone of this one whose `origin` is `FAUP-test`. `faup.py` warns that only copies with that line will see the release. The Update tab's *Source* line shows which repo a copy is reading. Delete the line to go back to FAUP.

## Troubleshooting

What the Update tab's *Connection* line or the status bar says:

| Message | What it means |
|---|---|
| Offline — no internet connection | The PC is offline. Everything else in the app still works. |
| Couldn't reach GitHub (network error N) — maybe a school/work firewall | The network blocks `raw.githubusercontent.com`. |
| Online — but no update has been published to the repo yet | `latest.json` wasn't found: nothing has been published, the repo is private, or the branch isn't `main`. |
| Online — GitHub rate-limited the check | Too many checks from one network. Wait a bit. |
| Online — update server answered HTTP N | GitHub returned some other error. Try again later. |
| Update download failed | `url` is wrong or points at a commit that was never pushed. Run `python faup.py check`. |
| The downloaded file isn't a valid EXE | `url` is a web page link instead of a raw link. |
| Update failed its checksum check | `sha256` doesn't match the file at `url`. Run `python faup.py check` to see why. |
| Couldn't write the update next to the EXE | The app is in a protected folder like Program Files. Move it somewhere writable. |
| Couldn't move the running EXE aside | Another copy is running from the same file, or the folder is read-only. Close it and retry. |
| The new release has no .exe attached | `url` is empty in `latest.json`. |
| Stop recording/playing/typing before updating | Finish what the app is doing, then press Install update again. |

Failed installs never change the EXE on disk, so retrying is always safe.
