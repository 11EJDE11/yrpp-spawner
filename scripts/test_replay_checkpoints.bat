@echo off
setlocal
call "%~dp0run_vsdevcmd.bat" -arch=x86
if errorlevel 1 exit /b %errorlevel%
cd /d "%~dp0.."
if not exist "Debug\CheckpointTests" mkdir "Debug\CheckpointTests"
cl /nologo /c /O2 /DMINIZ_NO_STDIO /DMINIZ_NO_TIME /DMINIZ_NO_ARCHIVE_APIS /DMINIZ_NO_ZLIB_APIS /DMINIZ_NO_ZLIB_COMPATIBLE_NAMES src\Vendor\miniz\miniz.c /FoDebug\CheckpointTests\miniz.obj
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /EHsc /O2 /Gy /DNOMINMAX /DSYR_VER=2 /D_CRT_SECURE_NO_WARNINGS /I src /I YRpp tests\replay-checkpoints.cpp src\Replay\ReplayFile.cpp src\Replay\ReplayStream.cpp Debug\CheckpointTests\miniz.obj /FoDebug\CheckpointTests\ /FeDebug\CheckpointTests\replay-checkpoints.exe /link /OPT:REF
if errorlevel 1 exit /b %errorlevel%
Debug\CheckpointTests\replay-checkpoints.exe Debug\CheckpointTests
