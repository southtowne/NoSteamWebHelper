# NoSteamWebHelper
NoSteamWebHelper is a recreation of the original NoSteamWebHelper DLL, which the original author took down.

![GitHub Release Downloads](https://img.shields.io/github/downloads/southtowne/NoSteamWebHelper/total)

Original projects:  
https://github.com/Aetopia/NoSteamWebHelper (removed; source recovered from Wayback Machine)
https://github.com/xowny/NoSteamWebHelper-Reloaded

It keeps Steam's browser stack available while idle, then automatically parks steamwebhelper.exe while Steam is running a game, instead of suspending or killing it like the original did. An optional kill mode
build is also available for people who don't just want the processes gone outright.

# Usage
Simply follow the quick and easy steps below ↓

1. Download [Release.zip](https://github.com/southtowne/NoSteamWebHelper/releases/download/web/Release.zip).
2. Right-click & extract.
3. Close Steam completely.
4. Copy umpdc.dll from the park folder (or kill, if that's what you want) into your Steam install directory, next to steam.exe.
5. Start Steam normally.

# Optional Build instructions:

1. Prerequisites: Windows 10/11 x64, a mingw-w64 GCC toolchain on PATH (scoop install gcc, or MSYS2's UCRT64 shell with pacman -S mingw-w64-ucrt-x86_64-gcc).
2. Clone: git clone https://github.com/southtowne/NoSteamWebHelper.git
3. Build: run Build.cmd from the repo root.
4. Output: build\park\umpdc.dll and build\kill\umpdc.dll, both already named umpdc.dll.
5. Close Steam completely, copy whichever one you want next to steam.exe, then relaunch Steam.
