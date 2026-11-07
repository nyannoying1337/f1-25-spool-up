@echo off
rem Tests the launch model and the flow against a fake game, then builds native\grid_start_2026.dll with MSVC (x64, static CRT). Run from anywhere.
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo build: vcvars64.bat not found; install the VS 2022 C++ build tools
  exit /b 1
)
call "%VCVARS%" >nul 2>&1 || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++20 /EHsc /O2 /MT /W4 /Fobuild\ /Febuild\model_test.exe test\model_test.cpp || exit /b 1
build\model_test.exe || exit /b 1
cl /nologo /std:c++20 /EHa /O1 /MT /W4 /wd4100 /wd4324 /permissive- /DUNICODE /D_UNICODE ^
   /Iinclude /Fobuild\ /Febuild\flow_test.exe test\flow_test.cpp /link /INCREMENTAL:NO user32.lib kernel32.lib || exit /b 1
build\flow_test.exe || exit /b 1
cl /nologo /std:c++20 /EHa /O2 /MT /W4 /wd4100 /wd4324 /permissive- /DNDEBUG /DUNICODE /D_UNICODE ^
   /Iinclude src\grid_start.cpp /Fobuild\ /Febuild\grid_start_2026.dll ^
   /LD /link /DLL /INCREMENTAL:NO /IMPLIB:build\grid_start_2026.lib user32.lib kernel32.lib || exit /b 1
copy /y build\grid_start_2026.dll grid_start_2026.dll >nul || exit /b 1
echo built %~dp0grid_start_2026.dll
