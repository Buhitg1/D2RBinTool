@echo off
rem Build d2datatables for the 2.4 game build (D2R.exe 69270 debug).
rem 69270 is a SINGLE data set (no base/parent split) and uses the default
rem (non-93854) calc-compiler path, so no build define is needed.  Output goes
rem to build_24\ so it can coexist with the 3.1.1 (build\) and 3.3
rem (build_93854\) builds.  NEVER edit build.bat / build_93854.bat.
setlocal
set "MSVC=C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.26100.0"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\winrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"
cd /d "%~dp0"
if not exist build_24 mkdir build_24
"%MSVC%\bin\Hostx64\x64\cl.exe" /nologo /std:c++17 /EHsc /O2 /W3 ^
   /Fe:build_24\d2datatables.exe /Fo:build_24\ ^
   src_24\main.cpp src_24\excel_engine.cpp src_24\tables.cpp src_24\idx_defaults.cpp src_24\calc_compiler.cpp src_24\decompiler.cpp /I src_24
if errorlevel 1 exit /b 1
echo Built build_24\d2datatables.exe
