@echo off
setlocal
pushd "%~dp0"

set OUT_DIR=..\..\build\renaro_model_playground
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

cl.exe /nologo /EHsc /std:c++17 /MD /utf-8 ^
  /I..\.. /I..\..\backends ^
  main.cpp ^
  llama_backend.cpp ^
  ..\..\backends\imgui_impl_dx11.cpp ^
  ..\..\backends\imgui_impl_win32.cpp ^
  ..\..\imgui*.cpp ^
  /Fe:"%OUT_DIR%\renaro_model_playground.exe" ^
  /Fo:"%OUT_DIR%\" ^
  /link d3d11.lib d3dcompiler.lib dxgi.lib windowscodecs.lib comdlg32.lib ole32.lib gdi32.lib dwmapi.lib winhttp.lib psapi.lib

if errorlevel 1 goto :failed
echo Built %OUT_DIR%\renaro_model_playground.exe
popd
exit /b 0

:failed
set BUILD_ERROR=%errorlevel%
popd
exit /b %BUILD_ERROR%
