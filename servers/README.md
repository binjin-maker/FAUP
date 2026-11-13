# Frutiger Servers

A tiny self-hosted dashboard for the Frutiger series. It **only listens**: copies of Frutiger Astro send a heartbeat (install id + app + version + an event — `launch`, `activate`, `ping` every 10s, `working`, or `offline`) after they're activated, and it shows them. It never receives files, typed text, macro content, or anything else off a PC, and it has no way to run anything on a copy, change it, or delete it.

## Run

Just run `FrutigerServers.exe` (Windows, no install). The whole panel is **in the window** — copy list, announcements, and the emergency recall builder — no browser needed. Data is stored next to the exe in `frutiger_servers.dat`. (A read-only web view is still served at `http://127.0.0.1:8787/` if you ever want it.)

Build it yourself from `frutiger_servers.c`:
```
python -m ziglang cc -target x86_64-windows-gnu -O2 -s -o FrutigerServers.exe frutiger_servers.c servers.rc \
  -Wl,--subsystem,windows -lws2_32 -luser32 -lgdi32 -lshell32 -lcomctl32
```

## What it shows

- **Active-user count**, total installs, and each copy with a coloured bullet — **green Online**, **amber Working** (busy with a macro/instruct), **black Offline** — a last-seen time, and a small **activity sparkline** of its recent state. Offline copies stay listed, so counts persist. Give any copy a nickname for your own reference.
- **Announcements** — send a display-only message to everyone or one online copy. It shows on the copy as a banner until the user dismisses it. The client only ever *renders* it; it never runs anything from the server.
- **Emergency · recall & notice** — builds the `faup.py` command to recall a bad version (shades those copies with your message until they update to the fix) or set a changelog notice. Recalls are issued by publishing to the update channel, so they're authenticated — this panel just composes the command; there's no live "disable" button, by design.

## Access

Copies on the **same network find it automatically** — it binds `0.0.0.0` and answers UDP discovery (`FRUTIGER-DISCOVER` on `:8788`), so there's nothing to set up for your own machines. The read-only web view honours `FS_HOST`, `FS_PORT`, and an optional `FS_TOKEN` (with a token set, the web view requires `?key=TOKEN`). Keep the token private; the panel is for you only.

## Point copies at it

On the same LAN, nothing to do. For copies out on the internet, set the endpoint in the `FrutigerMacro.ini` next to a copy's exe:
```ini
[Servers]
Url=http://YOUR-HOST:8787/beat
```
Heartbeats are on by default and only run after a copy is activated.
