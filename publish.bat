@echo off
rem  Double-click to publish a new Frutiger Astro version.
rem  Needs: Git, Python, and  pip install ziglang  (one-time).
cd /d "%~dp0"
echo Pulling latest...
git pull --ff-only
echo.
set /p VER=New version (e.g. 1.14.10):
set /p NOTE=What's new (one line):
echo.
python faup.py promote %VER% "%NOTE%"
echo.
pause
