#!/usr/bin/env python3
"""FAUP - Frutiger Astro Pusher

Two channels:
  Astro Beta  (beta.json / AstroBeta.exe)   - your local test build, no activation code
  Frutiger Astro (latest.json / FrutigerAstro.exe) - what real copies run

Typical flow: push to beta, test your Astro Beta copy, then promote to production.

  python faup.py build [--beta]                  build to build/ only (publishes nothing)
  python faup.py beta 1.14.8 ["note" ...]       release to the Astro Beta channel (you test)
  python faup.py promote 1.14.8 ["note" ...]    release to the Frutiger Astro channel (everyone)
  python faup.py check [--beta]                  verify a channel's json against its exe

options: --no-push (commit only) · --allow-repo-change (move the whole channel elsewhere)
"""
import hashlib
import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path

HOME_REPO = "binjin-maker/FAUP"
ROOT = Path(__file__).resolve().parent
SRC = ROOT / "src"
C_FILE = SRC / "FrutigerMacro.c"
LIBS = ["user32", "gdi32", "comdlg32", "comctl32", "winmm", "dwmapi", "uxtheme", "wininet", "bcrypt", "shell32"]
KEEP_NOTES = 10

CHANNELS = {
    "prod": dict(json="latest.json", exe="FrutigerAstro.exe", zip="FrutigerAstro.zip", defines=[], rc="app.rc", label="Frutiger Astro"),
    "beta": dict(json="beta.json",   exe="AstroBeta.exe",     zip=None,                defines=["-DASTRO_BETA"], rc="app_beta.rc", label="Astro Beta"),
}

ZIP_README = "\r\n".join([
    "Frutiger Astro - Setup", "======================", "",
    "1. Unzip this.", "",
    "2. Rename FrutigerAstro.ex_ to FrutigerAstro.exe",
    "   (can't see the .ex_ part? File Explorer > View > Show > File name extensions)", "",
    "3. Turn off Smart App Control.",
    "   (Windows Security > App & browser control > Smart App Control settings > Off)",
    "   The EXE isn't code-signed, so Windows won't run it otherwise.", "",
    "4. Put FrutigerAstro.exe in a folder you can write to (Desktop, Documents,",
    "   a USB stick - not Program Files) and run it.", "",
    "5. First launch asks for your access code. New versions then install",
    "   themselves from the Update tab.", "",
])


def die(m): sys.exit("faup: " + m)


def run(cmd, cwd=ROOT, capture=False):
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=capture)
    except FileNotFoundError:
        die(f"{cmd[0]} isn't installed (or isn't on PATH)")
    if r.returncode:
        die(f"'{' '.join(cmd[:3])} ...' failed" + ((":\n" + r.stderr.decode(errors='replace').strip()) if capture else ""))
    return r.stdout if capture else b""


def git(*a): return run(["git", *a], capture=True).decode().strip()


def rel(p): return p.relative_to(ROOT).as_posix()


def vtuple(v):
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", v or "")
    if not m: die(f"versions look like 1.14.8, not {v!r}")
    return tuple(int(x) for x in m.groups())


def read_src():
    with open(C_FILE, encoding="utf-8", newline="") as f: return f.read()


def src_repo():
    m = re.search(r'updRepo\[\d+\]\s*=\s*L"([^"]+)"', read_src())
    return m.group(1) if m else die("can't find updRepo in src")


def src_version():
    m = re.search(r'#define APP_VERSION\s+"([^"]*)"', read_src())
    return m.group(1) if m else "?"


def set_version(v):
    s = read_src()
    s, a = re.subn(r'(#define APP_VERSION\s+)"[^"]*"', lambda m: m.group(1) + f'"{v}"', s)
    s, b = re.subn(r'(#define APP_VERSION_W\s+)L"[^"]*"', lambda m: m.group(1) + f'L"{v}"', s)
    if a != 1 or b != 1: die("can't find APP_VERSION / APP_VERSION_W in src")
    with open(C_FILE, "w", encoding="utf-8", newline="") as f: f.write(s)


def origin_repo():
    url = git("config", "--get", "remote.origin.url")
    m = re.search(r"github\.com[:/]([^/]+)/([^/]+?)(?:\.git)?/?$", url)
    return f"{m.group(1)}/{m.group(2)}" if m else die(f"origin ({url}) isn't a GitHub repo")


def load_json(p):
    if not p.exists(): return {}
    try: return json.loads(p.read_text(encoding="utf-8"))
    except ValueError as e: die(f"{p.name} isn't valid JSON ({e})")


def build(ch, out):
    if subprocess.run([sys.executable, "-m", "ziglang", "version"], capture_output=True).returncode:
        die("Zig isn't installed - run: pip install ziglang")
    out.parent.mkdir(exist_ok=True)
    run([sys.executable, "-m", "ziglang", "cc", "-target", "x86_64-windows-gnu", "-O2", "-s",
         *CHANNELS[ch]["defines"], "-o", str(out), "FrutigerMacro.c", CHANNELS[ch]["rc"],
         "-Wl,--subsystem,windows", *["-l" + l for l in LIBS]], cwd=SRC)
    data = out.read_bytes()
    if data[:2] != b"MZ": die("the build didn't produce a Windows EXE")
    return data


def notes_with(v, bullets, old):
    entry = "\n".join([v] + ["- " + b.strip().lstrip("-* ").strip() for b in bullets if b.strip()])
    blocks = [entry] + [b for b in old.split("\n\n") if b.strip()]
    return "\n\n".join(blocks[:KEEP_NOTES])


def publish(ch, v, bullets, push, allow_repo_change):
    c = CHANNELS[ch]
    jf = ROOT / "update" / c["json"]
    exe = ROOT / "update" / c["exe"]
    new = vtuple(v)
    if git("symbolic-ref", "--short", "HEAD") != "main":
        die("switch to the main branch first - copies only read main")
    repo, app_repo = origin_repo(), src_repo()
    if repo.lower() == app_repo.lower():
        repo = app_repo
    elif repo.lower() == HOME_REPO.lower() and not allow_repo_change:
        die(f'src updRepo is L"{app_repo}", so copies that install this would leave the {HOME_REPO} channel.\n'
            f'      Set it back to L"{HOME_REPO}", or pass --allow-repo-change if that is intentional.')
    else:
        print(f"warning: publishing to {repo} but this build checks {app_repo} - installs follow {app_repo} after")
    if push and git("ls-remote", "--heads", "origin", "main"):
        run(["git", "pull", "-q", "--ff-only", "origin", "main"])
    cur = load_json(jf)
    if cur.get("version") and new <= vtuple(cur["version"]):
        die(f"{v} must be higher than the published {c['label']} version {cur['version']}")

    set_version(v)
    print(f"building {c['label']} {v} ...")
    data = build(ch, exe)
    if v.encode("utf-16-le") not in data:
        die(f"the new EXE doesn't report {v} - check APP_VERSION_W")
    add = ["src", rel(exe)]
    if c["zip"]:
        zp = ROOT / "update" / c["zip"]
        with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("README.txt", ZIP_README)
            z.writestr("FrutigerAstro.ex_", data)
        add.append(rel(zp))
    run(["git", "add", *add])
    run(["git", "commit", "-q", "--allow-empty", "-m", f"{c['label']} {v}"])
    if push: run(["git", "push", "-q", "origin", "HEAD:main"])
    commit = git("rev-parse", "HEAD")

    info = {"version": v,
            "url": f"https://raw.githubusercontent.com/{repo}/{commit}/{rel(exe)}",
            "sha256": hashlib.sha256(data).hexdigest(),
            "notes": notes_with(v, bullets, cur.get("notes", ""))}
    with open(jf, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(info, indent=2, ensure_ascii=False) + "\n")
    run(["git", "add", rel(jf)])
    run(["git", "commit", "-q", "-m", f"Release {c['label']} {v}"])
    if push: run(["git", "push", "-q", "origin", "HEAD:main"])
    print(f"published {c['label']} {v}  (sha256 {info['sha256'][:16]}...)")
    print(f"  exe  {info['url']}")
    print("  copies update on next launch or Check for updates" if push else
          "  NOT pushed - run: git push  (both commits)")


def show(ref, path):
    for _ in range(2):
        r = subprocess.run(["git", "show", f"{ref}:{path}"], cwd=ROOT, capture_output=True)
        if r.returncode == 0: return r.stdout
        subprocess.run(["git", "fetch", "-q", "--depth", "1", "origin", ref], cwd=ROOT, capture_output=True)
    die(f"{path} isn't in commit {ref[:12]} - never pushed, or this clone can't fetch it")


def check(ch):
    c = CHANNELS[ch]
    info = load_json(ROOT / "update" / c["json"])
    if not info: die(f"{c['json']} doesn't exist yet - publish to {c['label']} first")
    for k in ("version", "url", "sha256"):
        if not info.get(k): die(f'{c["json"]} has no "{k}"')
    vtuple(info["version"])
    m = re.fullmatch(r"https://raw\.githubusercontent\.com/([^/]+/[^/]+)/([^/]+)/(.+)", info["url"])
    if not m: die("url must be a direct raw.githubusercontent.com link")
    slug, refc, path = m.groups()
    data = show(refc, path)
    bad = []
    if slug.lower() != origin_repo().lower(): bad.append(f"url points at {slug}, not {origin_repo()}")
    if not re.fullmatch(r"[0-9a-f]{40}", refc): bad.append(f"url uses '{refc}' not a commit hash - cache can go stale")
    if data[:2] != b"MZ": bad.append("file at url isn't an EXE")
    if hashlib.sha256(data).hexdigest() != info["sha256"].strip().lower(): bad.append("sha256 mismatch")
    if info["version"].encode("utf-16-le") not in data: bad.append(f"EXE at url doesn't report {info['version']}")
    print(f"{c['label']}: published {info['version']}  (source {src_version()}, checks {src_repo()})")
    print(f"  {info['url']}")
    if bad: die("problems:\n  - " + "\n  - ".join(bad))
    print("  ok: hash, EXE and version match")


def main(argv):
    flags = {a for a in argv if a.startswith("--")}
    args = [a for a in argv if not a.startswith("--")]
    if flags - {"--no-push", "--allow-repo-change", "--beta"}:
        die("unknown option - run: python faup.py")
    push, repo_ch = "--no-push" not in flags, "--allow-repo-change" in flags
    cmd = args[0] if args else "help"
    if cmd == "build":
        ch = "beta" if "--beta" in flags else "prod"
        data = build(ch, ROOT / "build" / CHANNELS[ch]["exe"])
        print(f"built {CHANNELS[ch]['label']}  v{src_version()}  {len(data):,} bytes  sha256 {hashlib.sha256(data).hexdigest()}")
    elif cmd == "beta" and len(args) >= 2:
        set_version(args[1])
        data = build("beta", ROOT / "build" / CHANNELS["beta"]["exe"])
        print(f"built Astro Beta {args[1]} -> build/{CHANNELS['beta']['exe']} ({len(data):,} bytes)")
        print("  local test build - run it directly; it is NOT published and never leaves your machine.")
    elif cmd in ("promote", "publish") and len(args) >= 2:
        publish("prod", args[1], args[2:], push, repo_ch)
    elif cmd == "check":
        if "--beta" in flags:
            print("Astro Beta is a local test build - nothing is published to verify. Build it with: faup.py beta <ver>")
        else:
            check("prod")
    else:
        print(__doc__.strip())


if __name__ == "__main__":
    main(sys.argv[1:])
