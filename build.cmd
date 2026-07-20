@echo off
rem Builds LeniaWallpaper.exe (Release) into build\ using the VS 2022 toolchain.
rem No CMake needed; CMakeLists.txt is provided as an alternative.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -no_logo
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
rc /nologo /fo build\app.res src\app.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /W4 /permissive- /EHsc /MT ^
   /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /I src /Fo:build\ /Fe:build\LeniaWallpaper.exe ^
   src\main.cpp src\config.cpp src\renderer.cpp src\scheduler.cpp ^
   src\species.cpp src\tray.cpp src\util.cpp src\wallpaper_window.cpp ^
   build\app.res ^
   /link /SUBSYSTEM:WINDOWS d2d1.lib dwrite.lib ole32.lib
if errorlevel 1 exit /b 1
copy /Y fonts\Digital7Italic.ttf build\Digital7Italic.ttf >nul
echo.
echo Built build\LeniaWallpaper.exe
