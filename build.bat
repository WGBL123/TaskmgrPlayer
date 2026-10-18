@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul
title TaskmgrPlayer 一键编译

rem ============================================================
rem  TaskmgrPlayer 一键编译脚本
rem  用法:
rem    build.bat                 编译 Release x64
rem    build.bat Debug           编译 Debug x64
rem    build.bat Release run     编译完成后立即运行
rem  可选环境变量:
rem    OpenCVRoot=C:\opencv      指定 OpenCV 根目录
rem ============================================================

pushd "%~dp0" >nul 2>&1
if errorlevel 1 goto :fail
set "ROOT=%CD%"
if not "%ROOT:~-1%"=="\" set "ROOT=%ROOT%\"

set "CONFIG=Release"
set "RUN="

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="debug" (
    set "CONFIG=Debug"
) else if /i "%~1"=="release" (
    set "CONFIG=Release"
) else if /i "%~1"=="run" (
    set "RUN=1"
) else (
    echo [ERROR] 未知参数: %~1
    echo 用法: build.bat [Debug^|Release] [run]
    goto :fail
)
shift
goto parse_args
:args_done

rem ---------- 1. 查找 MSBuild ----------
set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe 2^>nul`) do (
        if not defined MSBUILD set "MSBUILD=%%I"
    )
)
if not defined MSBUILD if exist "%VSWHERE%" (
    rem 旧版 vswhere 不支持 -find 时的兜底
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -property installationPath 2^>nul`) do (
        if exist "%%I\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%%I\MSBuild\Current\Bin\MSBuild.exe"
    )
)
if not defined MSBUILD (
    for /f "delims=" %%I in ('where msbuild 2^>nul') do (
        if not defined MSBUILD set "MSBUILD=%%I"
    )
)
if not defined MSBUILD goto :no_msbuild

rem ---------- 1b. 选择平台工具集 ----------
rem 工程默认 v142；VS2022 未安装 v142 时自动退回 v143。
set "TOOLSET="
if exist "%VSWHERE%" (
    set "VSVER="
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -property installationVersion 2^>nul`) do set "VSVER=%%I"
    if defined VSVER if "!VSVER:~0,2!"=="17" (
        set "V142="
        for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.v142.x86.x64 -property installationPath 2^>nul`) do set "V142=%%I"
        if not defined V142 set "TOOLSET=/p:PlatformToolset=v143"
    )
)

rem ---------- 2. 查找 OpenCV ----------
set "CVROOT="
if defined OpenCVRoot call :try_cv "%OpenCVRoot%"
if not defined CVROOT if defined OPENCV_DIR call :try_cv "%OPENCV_DIR%"
if not defined CVROOT (
    for %%D in (
        "C:\opencv"
        "C:\opencv\build"
        "C:\Program Files\OpenCV\opencv"
        "C:\Program Files\OpenCV"
        "C:\OpenCV"
        "%USERPROFILE%\opencv"
        "%USERPROFILE%\Downloads\opencv"
        "%USERPROFILE%\Desktop\opencv"
    ) do (
        if not defined CVROOT call :try_cv "%%~D"
    )
)

if not defined CVROOT (
    echo.
    echo [WARN] 没有自动找到 OpenCV。
    echo 请下载 OpenCV 4.x Windows 版，或输入已安装的 OpenCV 根目录。
    echo 根目录下应包含: build\include\opencv2\opencv.hpp
    echo 例如: C:\opencv
    echo.
    set "CVINPUT="
    set /p "CVINPUT=OpenCV 根目录: "
    if defined CVINPUT (
        set "CVINPUT=!CVINPUT:"=!"
        call :try_cv "!CVINPUT!"
    )
    if not defined CVROOT goto :no_opencv
)

set "CVLIBDIR="
for %%V in (vc17 vc16 vc15 vc14) do (
    if not defined CVLIBDIR if exist "%CVROOT%\build\x64\%%V\lib\opencv_world*.lib" (
        set "CVLIBDIR=%CVROOT%\build\x64\%%V\lib"
    )
)
if not defined CVLIBDIR goto :no_lib

set "CVLIBREL="
set "CVLIBDBG="
for /f "delims=" %%L in ('dir /b /a-d "%CVLIBDIR%\opencv_world*.lib" 2^>nul') do (
    set "N=%%~nL"
    if /i "!N:~-1!"=="d" (
        set "CVLIBDBG=%%L"
    ) else (
        set "CVLIBREL=%%L"
    )
)

if /i "%CONFIG%"=="Debug" (
    set "CVLIB=!CVLIBDBG!"
) else (
    set "CVLIB=!CVLIBREL!"
)
if not defined CVLIB goto :no_lib

set "CVLIBNAME=!CVLIB:.lib=!"
if /i "!CVLIBNAME:~-1!"=="d" set "CVLIBNAME=!CVLIBNAME:~0,-1!"

rem OpenCV 的 DLL 目录通常与 lib 目录同级
set "CVBIN="
for %%P in ("%CVLIBDIR%\..\bin") do (
    if exist "%%~fP\opencv_*.dll" set "CVBIN=%%~fP"
)

rem ---------- 3. 编译 ----------
echo.
echo ============================================================
echo   编译 TaskmgrPlayer  %CONFIG% ^| x64
echo   MSBuild : %MSBUILD%
echo   OpenCV  : %CVROOT%
echo   库目录  : %CVLIBDIR%
echo   库名称  : %CVLIBNAME%
echo ============================================================
echo.

"%MSBUILD%" "%ROOT%TaskmgrPlayer.sln" /nologo /m /v:minimal ^
    /p:Configuration=%CONFIG% /p:Platform=x64 ^
    "/p:OpenCVRoot=%CVROOT%" "/p:OpenCVLibDir=%CVLIBDIR%" "/p:OpenCVLibName=%CVLIBNAME%" %TOOLSET%
if errorlevel 1 goto :build_failed

rem ---------- 4. 找到 exe ----------
set "EXE="
if exist "%ROOT%x64\%CONFIG%\TaskmgrPlayer.exe" set "EXE=%ROOT%x64\%CONFIG%\TaskmgrPlayer.exe"
if not defined EXE if exist "%ROOT%TaskmgrPlayer\x64\%CONFIG%\TaskmgrPlayer.exe" set "EXE=%ROOT%TaskmgrPlayer\x64\%CONFIG%\TaskmgrPlayer.exe"
if not defined EXE (
    for /r "%ROOT%" %%E in (TaskmgrPlayer.exe) do (
        if not defined EXE (
            echo %%~dpE | findstr /i /c:"\x64\%CONFIG%\" >nul && set "EXE=%%E"
        )
    )
)
if not defined EXE goto :no_exe

rem ---------- 5. 组装运行目录 ----------
set "DIST=%ROOT%dist\%CONFIG%"
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%" >nul 2>&1

copy /y "%EXE%" "%DIST%" >nul || goto :copy_failed
if exist "%ROOT%x64\%CONFIG%\TaskmgrPlayer.pdb" copy /y "%ROOT%x64\%CONFIG%\TaskmgrPlayer.pdb" "%DIST%" >nul

if exist "%ROOT%TaskmgrPlayer\config.cfg" copy /y "%ROOT%TaskmgrPlayer\config.cfg" "%DIST%" >nul
if exist "%ROOT%TaskmgrPlayer\audio.wav" copy /y "%ROOT%TaskmgrPlayer\audio.wav" "%DIST%" >nul

set "VIDEO="
if exist "%ROOT%TaskmgrPlayer\BadApple.flv" set "VIDEO=%ROOT%TaskmgrPlayer\BadApple.flv"
if not defined VIDEO (
    for %%F in (flv mp4 avi mkv mov webm wmv m4v) do (
        if not defined VIDEO (
            for %%G in ("%ROOT%TaskmgrPlayer\*.%%F") do (
                if exist "%%~G" set "VIDEO=%%~G"
            )
        )
    )
)
if defined VIDEO copy /y "!VIDEO!" "%DIST%" >nul

if exist "%ROOT%TaskmgrPlayer\ffmpeg.exe" copy /y "%ROOT%TaskmgrPlayer\ffmpeg.exe" "%DIST%" >nul
if not exist "%DIST%\ffmpeg.exe" if exist "%ROOT%ffmpeg.exe" copy /y "%ROOT%ffmpeg.exe" "%DIST%" >nul

if defined CVBIN (
    set "WANTDLL=%CVLIBNAME%.dll"
    if /i "%CONFIG%"=="Debug" set "WANTDLL=%CVLIBNAME%d.dll"
    if exist "!CVBIN!\!WANTDLL!" copy /y "!CVBIN!\!WANTDLL!" "%DIST%" >nul
    if exist "!CVBIN!\opencv_videoio_ffmpeg*.dll" copy /y "!CVBIN!\opencv_videoio_ffmpeg*.dll" "%DIST%" >nul
    if exist "!CVBIN!\opencv_ffmpeg*.dll" copy /y "!CVBIN!\opencv_ffmpeg*.dll" "%DIST%" >nul
) else (
    echo [WARN] 没有找到 OpenCV 的 bin 目录，请手动把 opencv_world*.dll
    echo        和 opencv_videoio_ffmpeg*.dll 复制到 %DIST%
)

echo.
echo ============================================================
echo   编译完成，输出目录:
echo   %DIST%
echo ============================================================
dir /b "%DIST%"
echo.

if defined RUN (
    echo 正在启动 TaskmgrPlayer...
    start "" "%DIST%\TaskmgrPlayer.exe"
)

echo 按任意键退出...
pause >nul
exit /b 0

rem ============================================================
rem  子程序: 尝试把参数识别为 OpenCV 根目录
rem  支持:
rem    C:\opencv
rem    C:\opencv\build
rem ============================================================
:try_cv
if "%~1"=="" exit /b
set "CAND=%~1"
for %%P in ("%CAND%") do set "CAND=%%~fP"

if exist "%CAND%\build\include\opencv2\opencv.hpp" (
    set "CVROOT=%CAND%"
    exit /b
)
if exist "%CAND%\include\opencv2\opencv.hpp" (
    for %%P in ("%CAND%\..") do set "CVROOT=%%~fP"
    exit /b
)
if exist "%CAND%\opencv\build\include\opencv2\opencv.hpp" (
    set "CVROOT=%CAND%\opencv"
    exit /b
)
exit /b

rem ============================================================
rem  错误提示
rem ============================================================
:no_msbuild
echo.
echo [ERROR] 没有找到 MSBuild。
echo 请安装 Visual Studio 2019 或 2022，并勾选:
echo   “使用 C++ 的桌面开发”
echo 下载: https://visualstudio.microsoft.com/zh-hans/downloads/
goto :fail

:no_opencv
echo.
echo [ERROR] OpenCV 路径无效。
echo 根目录下必须包含 build\include\opencv2\opencv.hpp
echo 下载: https://opencv.org/releases/
goto :fail

:no_lib
echo.
echo [ERROR] 找不到 %CONFIG% 需要的 OpenCV 库。
echo 查找目录: %CVLIBDIR%
echo 请确认安装的是 OpenCV Windows 版（含 build\x64\vc15\lib）。
goto :fail

:build_failed
echo.
echo [ERROR] 编译失败，请把上面的错误信息发给开发者。
goto :fail

:no_exe
echo.
echo [ERROR] 编译似乎成功，但没有找到 TaskmgrPlayer.exe。
echo 请检查上面的 MSBuild 输出。
goto :fail

:copy_failed
echo.
echo [ERROR] 复制 exe 到 dist 失败。
echo 目标目录: %DIST%
goto :fail

:fail
echo.
echo 按任意键退出...
pause >nul
exit /b 1
