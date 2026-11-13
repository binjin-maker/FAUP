# Frutiger Servers

A tiny self-hosted dashboard for the Frutiger series. It **only listens**: copies of Frutiger Astro send a heartbeat (install id + app + version + a launch/ping event) after they're activated, and it shows them. It never receives files, typed text, or anything else off a PC, and it has no way to run anything on a copy, change it, or delete it.

## Run

Just run `FrutigerServers.exe` (Windows, no install). The whole panel is **in the window** — copy list, announcements, and the emergency recall builder — no browser needed. Data is stored next to the exe in `frutiger_servers.dat`. (A read-only web view is still served at `http://127.0.0.1:8787/` if you ever want it.)

Build it yourself from `frutiger_servers.c`:
```
python -m ziglang cc -target x86_64-windows-gnu -O2 -s -o FrutigerServers.exe frutiger_servers.c servers.rc \
  -Wl,--subsystem,windows -lws2_32 -luser32 -lgdi32 -lshell32 -lcomctl32
```

## What it shows

- **Active-user count**, total installs, and each copy as **Online / Offline** with a last-seen time (offline copies stay listed, so counts persist). Give any copy a nickname for your own reference.
- **Announcements** — send a display-only message to everyone or one online copy. It shows on the copy as a banner until the user dismisses it. The client only ever *renders* it; it never runs anything from the server.
- **Emergency · recall & notice** — builds the `faup.py` command to recall a bad version (shades those copies with your message until they update to the fix) or set a changelog notice. Recalls are issued by publishing to the update channel, so they're authenticated — this panel just composes the command; there's no live "disable" button, by design.

## Access

Binds to `127.0.0.1` (localhost) only by default. To reach it from elsewhere, set env vars before launch: `FS_HOST` (e.g. `0.0.0.0`), `FS_PORT`, and `FS_TOKEN` — with a token set, the dashboard requires `?key=TOKEN`. Keep the token private; the panel is for you only.

## Point copies at it

In the `FrutigerMacro.ini` next to a copy's exe:
```ini
[Servers]
Url=http://YOUR-HOST:8787/beat
```
Telemetry is off until this is set, and only runs after a copy is activated.
