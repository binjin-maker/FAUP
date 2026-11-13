# FAUP: notes for Claude

This repo is the live update channel for Frutiger Astro. Every installed copy reads `update/latest.json` on `main` and installs the EXE it points to, so a mistake here reaches every user.

- To publish anything, follow **README.md → "Publishing as Claude (or any AI agent)"** step by step, including the checks.
- Publish only with `python3 faup.py publish <version> "note"`, and verify with `python3 faup.py check` plus the `curl` in step 7.
- `updRepo` in `src/FrutigerMacro.c` must stay `L"binjin-maker/FAUP"`.
- Never force-push, rewrite history, reuse or lower a version, or point `url` at `main`.
- `src/` is public. Ask Binjin before committing anything that shouldn't be.
- Release notes are shown to users in the app's *What's new* box. Keep them short and leave out repo names.
- Send EXEs to Binjin as `.ex_` files (browsers block `.exe` downloads).
