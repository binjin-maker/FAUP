#!/usr/bin/env python3
"""FAUP - Frutiger Astro Pusher

  python faup.py build                               build build/FrutigerAstro.exe from src/ (publishes nothing)
  python faup.py publish 1.14.8 ["note" ...]        release a new version to every copy's Update tab
  python faup.py check                               verify update/latest.json against the EXE it points at

publish options:
  --no-push              make both commits but leave them for you to git push
  --allow-repo-change    publish a build whose updRepo isn't binjin-maker/FAUP (moves every copy to that repo)
"""
import hashlib
import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path

CHANNEL = "binjin-maker/FAUP"
ROOT = Path(__file__).resolve().parent
SRC = ROOT / "src"
C_FILE = SRC / "FrutigerMacro.c"
BUILD = ROOT / "build" / "FrutigerAstro.exe"
EXE = ROOT / "update" / "FrutigerAstro.exe"
ZIP = ROOT / "update" / "FrutigerAstro.zip"
JSON_FILE = ROOT / "update" / "latest.json"
LIBS = ["user32", "gdi32", "comdlg32", "comctl32", "winmm", "dwmapi", "uxtheme", "wininet", "bcrypt", "shell32"]
KEEP_NOTES = 10

ZIP_README = "\r\n".join([
    "Frutiger Astro - Setup",
    "======================",
    "",
    "1. Unzip this.",
    "",
    "2. Rename FrutigerAstro.ex_ to FrutigerAstro.exe",
    "   (can't see the .ex_ part? File Explorer > View > Show > File name extensions)",
    "",
    "3. Turn off Smart App Control.",
    "   (Windows Security > App & browser control > Smart App Control settings > Off)",
    "   The EXE isn't code-signed, so Windows won't run it otherwise.",
    "",
    "4. Put FrutigerAstro.exe in a folder you can write to (Desktop, Documents,",
    "   a USB stick - not Program Files) and run it.",
    "",
    "5. That's the only manual install. When a new version is out, the light on the",
    "   Update tab turns amber: press Install update and it restarts into the new one.",
    "",
    "Made by DOOG unincorporated.",
    "",
])


def die(msg):
    sys.exit("faup: " + msg)


def run(cmd, cwd=ROOT, capture=False):
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=capture)
    except FileNotFoundError:
        die(f"{cmd[0]} isn't installed (or isn't on PATH)")
    if r.returncode:
        err = r.stderr.decode(errors="replace").strip() if capture else ""
        die(f"'{' '.join(cmd[:3])} ...' failed" + (f":\n{err}" if err else ""))
    return r.stdout if capture else b""


def git(*args):
    return run(["git", *args], capture=True).decode().strip()


def rel(p):
    return p.relative_to(ROOT).as_posix()


def vtuple(v):
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", v or "")
    if not m:
        die(f"versions look like 1.14.8, not {v!r}")
    return tuple(int(x) for x in m.groups())


def read_src():
    with open(C_FILE, encoding="utf-8", newline="") as f:
        return f.read()


def src_repo():
    m = re.search(r'updRepo\[\d+\]\s*=\s*L"([^"]+)"', read_src())
    if not m:
        die("can't find updRepo in src/FrutigerMacro.c")
    return m.group(1)


def src_version():
    m = re.search(r'#define APP_VERSION\s+"([^"]*)"', read_src())
    return m.group(1) if m else "?"


def set_version(v):
    s = read_src()
    s, a = re.subn(r'(#define APP_VERSION\s+)"[^"]*"', lambda m: m.group(1) + f'"{v}"', s)
    s, b = re.subn(r'(#define APP_VERSION_W\s+)L"[^"]*"', lambda m: m.group(1) + f'L"{v}"', s)
    if a != 1 or b != 1:
        die("can't find APP_VERSION and APP_VERSION_W in src/FrutigerMacro.c")
    with open(C_FILE, "w", encoding="utf-8", newline="") as f:
        f.write(s)


def origin_repo():
    url = git("config", "--get", "remote.origin.url")
    m = re.search(r"github\.com[:/]([^/]+)/([^/]+?)(?:\.git)?/?$", url)
    if not m:
        die(f"origin ({url}) isn't a GitHub repo, so there's no raw link to point copies at")
    return f"{m.group(1)}/{m.group(2)}"


def load_json():
    if not JSON_FILE.exists():
        return {}
    try:
        return json.loads(JSON_FILE.read_text(encoding="utf-8"))
    except ValueError as e:
        die(f"update/latest.json isn't valid JSON ({e})")


def build():
    if subprocess.run([sys.executable, "-m", "ziglang", "version"], capture_output=True).returncode:
        die("Zig isn't installed - run: pip install ziglang")
    BUILD.parent.mkdir(exist_ok=True)
    run([sys.executable, "-m", "ziglang", "cc", "-target", "x86_64-windows-gnu", "-O2", "-s",
         "-o", str(BUILD), "FrutigerMacro.c", "app.rc", "-Wl,--subsystem,windows",
         *["-l" + lib for lib in LIBS]], cwd=SRC)
    data = BUILD.read_bytes()
    if data[:2] != b"MZ":
        die("the build didn't produce a Windows EXE")
    return data


def notes_with(v, bullets, old):
    entry = "\n".join([v] + ["- " + b.strip().lstrip("-* ").strip() for b in bullets if b.strip()])
    blocks = [entry] + [b for b in old.split("\n\n") if b.strip()]
    return "\n\n".join(blocks[:KEEP_NOTES])


def publish(v, bullets, push, allow_repo_change):
    new = vtuple(v)
    if git("symbolic-ref", "--short", "HEAD") != "main":
        die("switch to the main branch first - copies only read main")
    repo, app_repo = origin_repo(), src_repo()
    if repo.lower() == app_repo.lower():
        repo = app_repo
    elif repo.lower() == CHANNEL.lower() and not allow_repo_change:
        die(f'src/FrutigerMacro.c has updRepo = L"{app_repo}", so every copy that installs this build would\n'
            f'      stop getting updates from FAUP. Set it back to L"{CHANNEL}" (or pass --allow-repo-change\n'
            f"      if you're moving the channel on purpose).")
    else:
        print(f"warning: publishing to {repo}, but this build checks {app_repo} for updates - copies that\n"
              f"         install it follow {app_repo} afterwards (unless their FrutigerMacro.ini sets Repo=)")
    if push and git("ls-remote", "--heads", "origin", "main"):
        run(["git", "pull", "-q", "--ff-only", "origin", "main"])
    cur = load_json()
    if cur.get("version") and new <= vtuple(cur["version"]):
        die(f"{v} has to be higher than the published {cur['version']} - copies only install higher versions")

    set_version(v)
    print(f"building {v} ...")
    data = build()
    if v.encode("utf-16-le") not in data:
        die(f"the new EXE doesn't report {v} - check APP_VERSION_W in src/FrutigerMacro.c")
    EXE.parent.mkdir(exist_ok=True)
    EXE.write_bytes(data)
    with zipfile.ZipFile(ZIP, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("README.txt", ZIP_README)
        z.writestr("FrutigerAstro.ex_", data)
    run(["git", "add", "src", rel(EXE), rel(ZIP)])
    run(["git", "commit", "-q", "--allow-empty", "-m", f"FrutigerAstro {v}"])
    if push:
        run(["git", "push", "-q", "origin", "HEAD:main"])
    commit = git("rev-parse", "HEAD")

    info = {
        "version": v,
        "url": f"https://raw.githubusercontent.com/{repo}/{commit}/{rel(EXE)}",
        "sha256": hashlib.sha256(data).hexdigest(),
        "notes": notes_with(v, bullets, cur.get("notes", "")),
    }
    with open(JSON_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(info, indent=2, ensure_ascii=False) + "\n")
    run(["git", "add", rel(JSON_FILE)])
    run(["git", "commit", "-q", "-m", f"Release {v}"])
    if push:
        run(["git", "push", "-q", "origin", "HEAD:main"])

    print(f"published {v}  ({len(data):,} bytes, sha256 {info['sha256'][:16]}...)")
    print(f"  exe   {info['url']}")
    print("  copies see it on their next launch or Check for updates" if push else
          "  not pushed yet - run: git push   (both commits, or copies will fail to download it)")


def show(ref, path):
    r = subprocess.run(["git", "show", f"{ref}:{path}"], cwd=ROOT, capture_output=True)
    if r.returncode:
        subprocess.run(["git", "fetch", "-q", "--depth", "1", "origin", ref], cwd=ROOT, capture_output=True)
        r = subprocess.run(["git", "show", f"{ref}:{path}"], cwd=ROOT, capture_output=True)
    if r.returncode:
        die(f"{path} isn't in commit {ref[:12]} - it was never pushed, or this clone can't fetch it")
    return r.stdout


def check():
    info = load_json()
    if not info:
        die("update/latest.json doesn't exist yet - publish something first")
    for k in ("version", "url", "sha256"):
        if not info.get(k):
            die(f'latest.json has no "{k}"')
    vtuple(info["version"])
    m = re.fullmatch(r"https://raw\.githubusercontent\.com/([^/]+/[^/]+)/([^/]+)/(.+)", info["url"])
    if not m:
        die("url has to be a direct https://raw.githubusercontent.com/... link")
    slug, ref, path = m.groups()
    data = show(ref, path)
    problems = []
    if slug.lower() != origin_repo().lower():
        problems.append(f"url points at {slug}, not this repo ({origin_repo()})")
    if not re.fullmatch(r"[0-9a-f]{40}", ref):
        problems.append(f"url uses '{ref}' instead of a commit hash - GitHub's cache can serve a stale EXE")
    if data[:2] != b"MZ":
        problems.append("the file at url isn't an EXE - copies will reject it")
    if hashlib.sha256(data).hexdigest() != info["sha256"].strip().lower():
        problems.append("sha256 doesn't match the EXE at url - copies will refuse the download")
    if info["version"].encode("utf-16-le") not in data:
        problems.append(f"the EXE at url doesn't report {info['version']} - copies would keep reinstalling it")
    print(f"published  {info['version']}   (source is at {src_version()}, app checks {src_repo()})")
    print(f"exe        {info['url']}")
    if problems:
        die("problems:\n  - " + "\n  - ".join(problems))
    print("ok: hash, EXE and version all match")


def main(argv):
    flags = {a for a in argv if a.startswith("--")}
    args = [a for a in argv if not a.startswith("--")]
    unknown = flags - {"--no-push", "--allow-repo-change"}
    if unknown:
        die(f"unknown option {sorted(unknown)[0]} - run: python faup.py help")
    cmd = args[0] if args else "help"
    if cmd == "build":
        data = build()
        print(f"built {rel(BUILD)}  v{src_version()}  {len(data):,} bytes  sha256 {hashlib.sha256(data).hexdigest()}")
    elif cmd == "publish" and len(args) >= 2:
        publish(args[1], args[2:], "--no-push" not in flags, "--allow-repo-change" in flags)
    elif cmd == "check":
        check()
    else:
        print(__doc__.strip())


if __name__ == "__main__":
    main(sys.argv[1:])
