@echo off
cd "%~dp0"
if not exist build\park mkdir build\park
if not exist build\kill mkdir build\kill
gcc -municode -s -nostdlib -shared src\Library.c -lkernel32 -luser32 -lshell32 -ladvapi32 -lwtsapi32 -lntdll -o build\park\umpdc.dll
gcc -municode -s -nostdlib -shared -DKILL_MODE src\Library.c -lkernel32 -luser32 -lshell32 -ladvapi32 -lwtsapi32 -lntdll -o build\kill\umpdc.dll
