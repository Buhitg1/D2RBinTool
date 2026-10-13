@echo off
rem Build d2datatables for the 3.3 game build (D2R_93854_RAC_China.exe, build 93854).
rem Identical to build.bat but defines D2DT_BUILD_93854, which selects the
rem 93854 variants of the changed descriptors (charstats/levels/monpet/
rem superuniques), the moncalc code provider and the moncalc calc family.
rem Output goes to build_93854\ so both variants can coexist.
setlocal
set "MSVC=C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.26100.0"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\winrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"
cd /d "%~dp0"
if not exist build_93854 mkdir build_93854
"%MSVC%\bin\Hostx64\x64\cl.exe" /nologo /std:c++17 /EHsc /O2 /W3 /DD2DT_BUILD_93854 ^
   /Fe:build_93854\d2datatables.exe /Fo:build_93854\ ^
   src\main.cpp src\excel_engine.cpp src\tables.cpp src\idx_defaults.cpp src\calc_compiler.cpp src\decompiler.cpp /I src
if errorlevel 1 exit /b 1
echo Built build_93854\d2datatables.exe
