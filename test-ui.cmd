@echo off
powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0tests\test_runner.ps1" %*
