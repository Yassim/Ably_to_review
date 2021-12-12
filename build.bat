@echo on

echo off

if NOT DEFINED VCINSTALLDIR (
    echo "test USING VISUAL STUDIO 2017"
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvarsall.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 10.0.17763.0
        echo "USING VISUAL STUDIO 2017"
    )
)

if NOT DEFINED VCINSTALLDIR (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio 15.0\VC\vcvarsall.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio 15.0\VC\vcvarsall.bat" x86
        echo "USING VISUAL STUDIO 15"
    )
)

if NOT DEFINED VCINSTALLDIR (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" x86
        echo "USING VISUAL STUDIO 14"
    )
)

if NOT DEFINED VCINSTALLDIR (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio 13.0\VC\vcvarsall.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio 13.0\VC\vcvarsall.bat" x86
        echo "USING VISUAL STUDIO 13"
    )
)

if NOT DEFINED VCINSTALLDIR (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" x86
        echo "USING VISUAL STUDIO 12"
    )
)

if NOT DEFINED VCINSTALLDIR (
    echo "No compatible visual studio found! run vcvarsall.bat first!"
    goto :end
)

cl.exe Ably.cc /nologo /O2 /EHsc /std:c++17 /W4 /link /out:Ably.exe

:end
pause