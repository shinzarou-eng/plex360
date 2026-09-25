@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d C:\Users\fbdyl\Downloads\xbox\pctest
cl /nologo /EHsc /I. test.cpp ..\Plex360\src\xmlmini.cpp ..\Plex360\src\plex.cpp /Fe:test.exe
if errorlevel 1 exit /b 1
test.exe
