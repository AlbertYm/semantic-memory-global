@echo off
setlocal
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0Install-SemanticMemoryV2.ps1" -Action Install -PayloadManifestPath "%~dp0payload\payload-manifest.json"
