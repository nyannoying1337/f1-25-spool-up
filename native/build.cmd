@echo off
rem Tests the launch model and the flow against a fake game, then builds native\grid_start_2026.dll with zig's clang
rem (x86_64-windows-gnu, libc++ linked in; the same compiler Project Halo builds with). Run from anywhere.
rem zig: winget install zig.zig, or set ZIG to zig.exe.
setlocal
cd /d "%~dp0"
if not defined ZIG for /d %%d in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\zig.zig_*") do for /d %%e in ("%%d\zig-x86_64-windows-*") do if exist "%%e\zig.exe" set "ZIG=%%e\zig.exe"
if not defined ZIG (
  where zig >nul 2>&1 && set "ZIG=zig"
)
if not defined ZIG (
  echo build: zig not found; install it with "winget install zig.zig" or set ZIG
  exit /b 1
)
set "CXX="%ZIG%" c++ -target x86_64-windows-gnu -std=c++20 -fms-extensions -Wno-macro-redefined -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE -Iinclude"
if not exist build mkdir build
%CXX% -O2 -o build\model_test.exe test\model_test.cpp || exit /b 1
build\model_test.exe || exit /b 1
%CXX% -O1 -o build\flow_test.exe test\flow_test.cpp -luser32 -lkernel32 || exit /b 1
build\flow_test.exe || exit /b 1
rem -s: no symbols. The export table names the output file, so the DLL is always written as grid_start_2026.dll.
%CXX% -O2 -DNDEBUG -shared -s -o build\grid_start_2026.dll src\grid_start.cpp -luser32 -lkernel32 || exit /b 1
copy /y build\grid_start_2026.dll grid_start_2026.dll >nul || exit /b 1
echo built %~dp0grid_start_2026.dll
