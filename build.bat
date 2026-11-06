@echo off
rem Configure and build notita (Release) from a plain cmd prompt.
rem   build.bat                -> builds into .\build
rem   set NOTITA_BUILD_DIR=... -> builds somewhere else
rem Uses Ninja if it is on PATH, otherwise NMake (always present with MSVC).
setlocal
set "ROOT=%~dp0"
if defined NOTITA_BUILD_DIR (set "BUILD=%NOTITA_BUILD_DIR%") else (set "BUILD=%ROOT%build")
where cl >nul 2>nul
if errorlevel 1 (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
where cl >nul 2>nul
if errorlevel 1 (
  if not defined VSDIR ( echo Could not find Visual Studio with the C++ tools. & exit /b 1 )
  call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
)
where cl >nul 2>nul
if errorlevel 1 ( echo MSVC environment setup failed. & exit /b 1 )
set "GEN=NMake Makefiles"
where ninja >nul 2>nul
if not errorlevel 1 set "GEN=Ninja"
if not exist "%BUILD%\CMakeCache.txt" (
  cmake -S "%ROOT%." -B "%BUILD%" -G "%GEN%" -DCMAKE_BUILD_TYPE=Release || exit /b 1
) else (
  cmake -S "%ROOT%." -B "%BUILD%" -DNOTITA_DEPS_ONLY=OFF || exit /b 1
)
cmake --build "%BUILD%" || exit /b 1
echo.
echo Built: %BUILD%\notita.exe   tests: %BUILD%\notita_tests.exe   bench: %BUILD%\notita_bench.exe
