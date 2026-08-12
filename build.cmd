@echo off
setlocal EnableDelayedExpansion

set "INPUT_DIR=src\web\assets"
set "OUTPUT_DIR=gen"

if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

for /R "%INPUT_DIR%" %%F in (*) do (
    set "FILE=%%F"
    set "RELATIVE=!FILE:%CD%\%INPUT_DIR%\=!"

    echo Generating !RELATIVE!

    python3 gen-asset-module.py ^
        --path "!RELATIVE!" ^
        "!FILE!" ^
        > "%OUTPUT_DIR%\!RELATIVE!.c"
)

echo Assets generated

wsl.exe -e bash -lc "source ~/ps5tools/env.sh && make -f Makefile ps5_nzb_downloader.elf"

if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

wsl.exe -e bash -lc "file ps5_nzb_downloader.elf"

endlocal