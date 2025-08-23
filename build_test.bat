@echo off
setlocal
rem Build ExtDll.dll if not already built
if not exist ExtDll.dll (
    echo Building ExtDll.dll...
    cl /nologo /W4 /O2 /EHsc /DUNICODE /D_UNICODE /std:c++17 /MD /LD ExtDll.cpp /Fe:ExtDll.dll /link /DEF:ExtDll.def winhttp.lib || goto :eof
)

echo Building TestGetCard.exe...
cl /nologo /W4 /O2 /EHsc TestGetCard.cpp /Fe:TestGetCard.exe || goto :eof

echo.
echo Running test (DLL must be alongside EXE or on PATH)...
copy /Y ExtDll.dll TestGetCard.exe.local 1>nul 2>nul
TestGetCard.exe ExtDll.dll
endlocal

