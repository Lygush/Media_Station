@echo off
rem Пересборка шейдеров дымки (.qsb) после правки mist.frag / mist.vert.
rem qsb.exe лежит в папке bin Qt, например C:\Qt\6.8.0\msvc2022_64\bin\qsb.exe
rem Использование: scripts\build_shaders.bat [путь\к\qsb.exe]
setlocal
set QSB=%~1
if "%QSB%"=="" set QSB=qsb.exe
cd /d "%~dp0..\assets\shaders\mist" || exit /b 1
for %%S in (mist.vert mist.frag) do (
    "%QSB%" --glsl "100 es,120,150" --hlsl 50 --msl 12 -o %%S.qsb %%S || exit /b 1
)
echo OK
