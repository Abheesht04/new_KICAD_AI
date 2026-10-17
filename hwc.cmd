@echo off
setlocal EnableExtensions
set "ROOT=%~dp0"
set "BUILD_DIR=%ROOT%build"
set "CONFIG=Debug"
if defined HWC_CONFIG set "CONFIG=%HWC_CONFIG%"

echo [HWC] Configuring the C23 project...
cmake -S "%ROOT%" -B "%BUILD_DIR%" -DHWCOMPILER_BUILD_TESTS=ON
if errorlevel 1 exit /b 1

echo [HWC] Building target hwc (%CONFIG%)...
cmake --build "%BUILD_DIR%" --config %CONFIG% --target hwc copper-core --parallel
if errorlevel 1 exit /b 1

if exist "%BUILD_DIR%\%CONFIG%\hwc.exe" (
  set "HWC_EXE=%BUILD_DIR%\%CONFIG%\hwc.exe"
) else if exist "%BUILD_DIR%\hwc.exe" (
  set "HWC_EXE=%BUILD_DIR%\hwc.exe"
) else (
  echo [HWC] Build completed but hwc.exe was not found.
  exit /b 2
)

pushd "%ROOT%"
"%HWC_EXE%" %*
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
