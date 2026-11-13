# FAUP: notes for Claude

This repo is the live update system for Frutiger Astro. Every installed copy reads its channel's JSON on `main` and installs the EXE it points to, so a mistake here reaches every user.

- To publish anything, follow **README.md → "Publishing as Claude (or any AI agent)"** step by step, including the checks.
- Two channels: `faup.py beta <ver> "note"` (your test build, Astro Beta) and `faup.py promote <ver> "note"` (production, Frutiger Astro). Usually beta first, then promote. Verify with `python3 faup.py check` / `check --beta` plus the `curl` in step 6.
- `updRepo` in `src/FrutigerMacro.c` must stay `L"binjin-maker/FAUP"`.
- Never force-push, rewrite history, reuse or lower a version (per channel), or point `url` at `main`.
- `src/` is public. The access code lives only as a SHA-256 hash — never commit the plaintext code. Ask Binjin before committing anything else that shouldn't be public.
- The only destructive behaviour is the app's **local** three-wrong-codes self-delete of its own exe. There is no remote terminate and must never be one.
- Frutiger Servers may push **display-only announcements**: plain text the user sees and dismisses. The client only ever *renders* server content as a banner — it must never execute it, act on it, change settings from it, or treat it as a command. No remote destructive or control action, ever.
- Telemetry is install-id + version + heartbeat/event only (`beat()`): events are launch, activate, ping, working (a macro/instruct is running — state only, never its content), and offline (sent on close). Ping is every 10s. It is ON by default via the baked `DEFAULT_SERVERS_URL` in `src/FrutigerMacro.c`; ini `[Servers] Url=` overrides it, and beta (`ASTRO_BETA`) sends nothing. The install id is a random opaque value, not tied to identity. Never add collection of user files, Instruct text, or PC data — the heartbeat payload stays id+app+version+event.
- Release notes are shown to users in *What's new*. Keep them short and leave out repo names.
- Send EXEs to Binjin as `.ex_` files (browsers block `.exe` downloads).
