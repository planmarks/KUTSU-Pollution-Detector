@echo off
rem ===========================================================================
rem KUTSU Hub - Windows launcher. Double-click to start; close the window to stop.
rem Needs Python 3.7+ (python.org). On Windows 7 use Python 3.8.x, the last
rem release that supports it. Tick "Add python.exe to PATH" when installing.
rem Extra options go after the script name, e.g.  --port 8081 --retention-days 90
rem ===========================================================================
cd /d "%~dp0"
title KUTSU Hub
where py >nul 2>nul && (py -3 kutsu_hub.py %* & goto :end)
where python >nul 2>nul && (python kutsu_hub.py %* & goto :end)
echo Python 3 was not found. Install it from https://www.python.org/downloads/
echo (Windows 7: Python 3.8) and tick "Add python.exe to PATH".
:end
pause
