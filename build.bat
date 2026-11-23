@echo off
rem Builds Nitro.exe and NitroHook.dll into build\dist.
rem Needs Visual Studio 2022 with "Desktop development with C++" and CMake.
cmake -S "%~dp0" -B "%~dp0build" -A x64 || goto :error
cmake --build "%~dp0build" --config Release || goto :error
echo.
echo Done. Files are in "%~dp0build\dist".
echo Copy Lossless.dll from your Lossless Scaling folder next to Nitro.exe.
goto :eof
:error
echo Build failed.
exit /b 1
