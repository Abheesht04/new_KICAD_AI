@echo off
setlocal EnableExtensions
rem Builds pcba (and the copper-core backend) if needed, then runs it with your arguments.
set "ROOT=%~dp0"
set "BUILD_DIR=%ROOT%build"
set "CONFIG=Release"
if defined PCBA_CONFIG set "CONFIG=%PCBA_CONFIG%"

if not exist "%BUILD_DIR%\CMakeCache.txt" (
  cmake -S "%ROOT%." -B "%BUILD_DIR%" -DHWCOMPILER_BUILD_TESTS=ON 1>&2
  if errorlevel 1 exit /b 70
)
cmake --build "%BUILD_DIR%" --config %CONFIG% --target pcba copper-core --parallel 1>&2
if errorlevel 1 exit /b 70

if exist "%BUILD_DIR%\%CONFIG%\pcba.exe" (
  "%BUILD_DIR%\%CONFIG%\pcba.exe" %*
) else (
  "%BUILD_DIR%\pcba.exe" %*
)
exit /b %ERRORLEVEL%
