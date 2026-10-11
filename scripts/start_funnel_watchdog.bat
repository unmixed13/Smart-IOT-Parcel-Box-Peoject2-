@echo off
rem Starts the Funnel watchdog in a hidden window. Run as the user that is logged in to Tailscale.
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0funnel_watchdog.ps1" -Port 8888
