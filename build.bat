@echo off
rem Build d2datatables with MSVC - the 3.1.1 variant (game build 91923, D2R.exe).
rem The toolchain paths are set explicitly so that vcvars64.bat (which shells out
rem to reg.exe) is not required.
setlocal
set "MSVC=C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.26100.0"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\winrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"
cd /d "%~dp0"
if not exist build mkdir build
"%MSVC%\bin\Hostx64\x64\cl.exe" /nologo /std:c++17 /EHsc /O2 /W3 ^
   /Fe:build\d2datatables.exe /Fo:build\ ^
   src\main.cpp src\excel_engine.cpp src\tables.cpp src\idx_defaults.cpp src\calc_compiler.cpp src\decompiler.cpp /I src
if errorlevel 1 exit /b 1
echo Built build\d2datatables.exe
