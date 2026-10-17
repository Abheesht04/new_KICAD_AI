@echo off
setlocal EnableExtensions
set "COPPER_PROJECT_ROOT=%~dp0"
pushd "%COPPER_PROJECT_ROOT%"
if /I "%~1"=="studio" goto studio
cmake -S . -B build -DHWCOMPILER_BUILD_TESTS=ON
if errorlevel 1 goto fail
cmake --build build --config Release --target hwc copper-core --parallel
if errorlevel 1 goto fail
if exist "build\Release\copper-core.exe" set "COPPER_CORE=%COPPER_PROJECT_ROOT%build\Release\copper-core.exe"
if exist "build\copper-core.exe" set "COPPER_CORE=%COPPER_PROJECT_ROOT%build\copper-core.exe"
py -3 tools\copper.py %*
set "COPPER_RC=%ERRORLEVEL%"
popd
exit /b %COPPER_RC%
:studio
cmake -S . -B build-desktop -DCOPPER_BUILD_DESKTOP=ON
if errorlevel 1 goto fail
cmake --build build-desktop --config Release --target copper-studio --parallel
if errorlevel 1 goto fail
if exist "build-desktop\desktop\Release\copper-studio.exe" (
 "build-desktop\desktop\Release\copper-studio.exe"
) else (
 "build-desktop\desktop\copper-studio.exe"
)
set "COPPER_RC=%ERRORLEVEL%"
popd
exit /b %COPPER_RC%
:fail
popd
exit /b 1
