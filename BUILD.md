# OptiX AI Denoiser GUI (native C++ / Win32)

Single executable, no external libraries, static CRT (no VC++ redistributable needed).
It runs Denoiser.exe once per image, so the CLI is not modified.

## Build (Visual Studio 2019/2022 with "Desktop development with C++")
Open "x64 Native Tools Command Prompt for VS" in this folder:

    cmake -S . -B build -A x64
    cmake --build build --config Release

Output: build\Release\OptiXDenoiserGUI.exe

Alternative: in Visual Studio use File > Open > Folder, select this folder,
choose the x64-Release configuration, then Build > Build All.

## Run
Copy Denoiser.exe next to OptiXDenoiserGUI.exe (or into a "Denoiser" subfolder).
It is picked up automatically; otherwise set the path in the Engine field.
Settings are stored in %APPDATA%\OptiXDenoiserGUI\settings.ini.

## Metadata
Version, publisher and copyright live in src/version.h and res/app.rc
(CompanyName, LegalCopyright, FileVersion, ProductVersion). Replace res/app.ico to change the icon.

## Licensing note
The Denoiser CLI is MIT licensed (c) Declan Russell. If you ship Denoiser.exe with
this GUI, include its LICENSE file alongside it.
