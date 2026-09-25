@echo off
setlocal enabledelayedexpansion
:: ============================================================
:: Build <Projet>.xex avec le toolchain XDK standalone
:: (sdk\extracted\XDK - aucun VS2010 requis)
:: Usage : build.bat [Projet]      (defaut : Plex360)
::         build.bat XboxHello
:: ============================================================
set "PROJ=%~1"
if "%PROJ%"=="" set "PROJ=Plex360"

set "ROOT=%~dp0"
set "XEDK=%ROOT%sdk\extracted\XDK"
set "XBIN=%XEDK%\bin\win32"
set "OBJDIR=%ROOT%build\Release\obj\%PROJ%"
set "BINDIR=%ROOT%build\Release\bin"

if not exist "%XBIN%\cl.exe" (
    echo [ERREUR] Toolchain XDK introuvable dans %XBIN%
    echo Extrais d'abord le SDK ^(voir README.md^).
    exit /b 1
)
if not exist "%ROOT%%PROJ%\src" (
    echo [ERREUR] Projet %PROJ% introuvable ^(%ROOT%%PROJ%\src absent^).
    exit /b 1
)

:: CRT headers publics = TechPreview\Jul12Compiler\include\xbox
:: (equiv. des VC\include de VS2010, absents de include\xbox nu)
set "INCLUDE=%XEDK%\include\xbox;%XEDK%\TechPreview\Jul12Compiler\include\xbox;%ROOT%%PROJ%\src;%ROOT%%PROJ%\src\tls;%ROOT%%PROJ%\src\tls\SSL;%ROOT%%PROJ%\src\tls\SSL\inc"
set "LIB=%XEDK%\lib\xbox"

if not exist "%OBJDIR%" mkdir "%OBJDIR%"
if not exist "%BINDIR%" mkdir "%BINDIR%"

set "LIBS=xapilib.lib d3d9.lib d3dx9.lib xgraphics.lib xnet.lib xonline.lib xaudio2.lib xmedia2.lib xmcore.lib xboxkrnl.lib"
set "OBJS="

echo [1/3] Compile %PROJ% ^(cl Xenon PPC^)...
for %%f in ("%ROOT%%PROJ%\src\*.cpp") do (
    echo   ^> %%~nxf
    "%XBIN%\cl.exe" /nologo /c /MT /O2 /Oi /Ot /DNDEBUG /D_XBOX /W3 /Fo"%OBJDIR%\%%~nf.obj" "%%f"
    if errorlevel 1 goto :fail
    set "OBJS=!OBJS! "%OBJDIR%\%%~nf.obj""
)

:: XboxTLS + BearSSL (optionnel : seulement si src\tls existe)
if exist "%ROOT%%PROJ%\src\tls" (
    echo [1b/3] Compile XboxTLS + BearSSL...
    for %%f in ("%ROOT%%PROJ%\src\tls\*.cpp") do (
        echo   ^> %%~nxf
        "%XBIN%\cl.exe" /nologo /c /MT /O2 /Oi /Ot /DNDEBUG /D_XBOX /W3 /Fo"%OBJDIR%\%%~nf.obj" "%%f"
        if errorlevel 1 goto :fail
        set "OBJS=!OBJS! "%OBJDIR%\%%~nf.obj""
    )
    :: BearSSL : batch par sous-dossier (les .c partagent le meme dossier d'obj)
    for /d %%d in ("%ROOT%%PROJ%\src\tls\SSL\*") do (
        if exist "%%d\*.c" (
            echo   ^> SSL\%%~nxd
            "%XBIN%\cl.exe" /nologo /c /MT /O2 /Oi /Ot /DNDEBUG /D_XBOX /W1 /Fo"%OBJDIR%\\" "%%d\*.c"
            if errorlevel 1 goto :fail
            for %%f in ("%%d\*.c") do set "OBJS=!OBJS! "%OBJDIR%\%%~nf.obj""
        )
    )
    for %%f in ("%ROOT%%PROJ%\src\tls\SSL\*.c") do (
        echo   ^> %%~nxf
        "%XBIN%\cl.exe" /nologo /c /MT /O2 /Oi /Ot /DNDEBUG /D_XBOX /W1 /Fo"%OBJDIR%\%%~nf.obj" "%%f"
        if errorlevel 1 goto :fail
        set "OBJS=!OBJS! "%OBJDIR%\%%~nf.obj""
    )
)

echo [2/3] Link ^(PPCBE^)...
:: response file : ~300 objets depassent la limite de ligne de commande
set "RSP=%OBJDIR%\link.rsp"
>"%RSP%" echo /nologo /MACHINE:PPCBE /XEX:NO /STACK:262144,262144 /OUT:"%BINDIR%\%PROJ%.exe"
>>"%RSP%" echo %LIBS%
for %%f in ("%ROOT%%PROJ%\src\*.cpp") do >>"%RSP%" echo "%OBJDIR%\%%~nf.obj"
if exist "%ROOT%%PROJ%\src\tls" (
    for %%f in ("%ROOT%%PROJ%\src\tls\*.cpp") do >>"%RSP%" echo "%OBJDIR%\%%~nf.obj"
    for /d %%d in ("%ROOT%%PROJ%\src\tls\SSL\*") do (
        for %%f in ("%%d\*.c") do >>"%RSP%" echo "%OBJDIR%\%%~nf.obj"
    )
    for %%f in ("%ROOT%%PROJ%\src\tls\SSL\*.c") do >>"%RSP%" echo "%OBJDIR%\%%~nf.obj"
)
"%XBIN%\link.exe" @"%RSP%"
if errorlevel 1 goto :fail

echo [3/3] %PROJ%.exe -^> %PROJ%.xex ^(imagexex^)...
"%XBIN%\imagexex.exe" /NOLOGO /IN:"%BINDIR%\%PROJ%.exe" /OUT:"%BINDIR%\%PROJ%.xex"
if errorlevel 1 goto :fail

:: config.ini a cote du xex si present dans le projet
if exist "%ROOT%%PROJ%\config.ini" copy /y "%ROOT%%PROJ%\config.ini" "%BINDIR%\config.ini" >nul

echo.
echo [OK] %BINDIR%\%PROJ%.xex
goto :eof

:fail
echo [ERREUR] Build echoue.
exit /b 1
