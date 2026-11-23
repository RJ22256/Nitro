@echo off
rem Builds Nitro (64-bit app + 32/64-bit hooks) into build\dist.
rem Needs Visual Studio 2022 with "Desktop development with C++" and CMake.
cmake -S "%~dp0" -B "%~dp0build" -A x64 || goto :error
cmake --build "%~dp0build" --config Release || goto :error
cmake -S "%~dp0" -B "%~dp0build32" -A Win32 || goto :error
cmake --build "%~dp0build32" --config Release || goto :error
copy /y "%~dp0build32\dist\*" "%~dp0build\dist\" >nul
if not exist "%~dp0build\dist\Fonts" mkdir "%~dp0build\dist\Fonts"
copy /y "%~dp0fonts\*" "%~dp0build\dist\Fonts\" >nul
echo.
echo Done. Files are in "%~dp0build\dist".
goto :eof
:error
echo Build failed.
exit /b 1
