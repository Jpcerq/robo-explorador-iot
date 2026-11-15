@echo off
chcp 65001 >nul
cd /d "%~dp0"
title Lampada - iniciar banco
set "PYTHONIOENCODING=utf-8"
set "PY=%LOCALAPPDATA%\Programs\Python\Python311\python.exe"
if not exist "%PY%" set "PY=py"
echo Iniciando... o progresso fica em iniciar_banco.log
"%PY%" -u iniciar_banco.py > iniciar_banco.log 2>&1
type iniciar_banco.log
echo.
pause
