@echo off
rem Builds Lull.exe with MSVC + Windows SDK (no other dependencies).
setlocal enabledelayedexpansion
cd /d "%~dp0"

if not defined VSCMD_VER (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
  if not defined VSPATH ( echo Visual Studio with C++ tools not found. & exit /b 1 )
  call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)

if not exist build\shaders mkdir build\shaders

echo [1/3] shaders
fxc /nologo /T vs_5_0 /E VSMain /O3 /Fh build\shaders\vs_full.h /Vn g_vs_full shaders\common.hlsl >nul || exit /b 1
fxc /nologo /T ps_5_0 /E PSBlit /O3 /Fh build\shaders\ps_blit.h /Vn g_ps_blit shaders\common.hlsl >nul || exit /b 1
for %%s in (0 1 2 3 4 5 6 7) do (
  fxc /nologo /T ps_5_0 /E PSMain /O3 /D SCENE=%%s /Fh build\shaders\ps_scene%%s.h /Vn g_ps_scene%%s shaders\scenes.hlsl >nul || exit /b 1
)
fxc /nologo /T ps_5_0 /E PSGridField /O3 /D SCENE=7 /Fh build\shaders\ps_gridfield.h /Vn g_ps_gridfield shaders\scenes.hlsl >nul || exit /b 1

echo [2/3] resources
set RCDEFS=
if exist res\lull.ico set RCDEFS=/DHAVE_ICON
rc /nologo %RCDEFS% /fo build\lull.res res\lull.rc || exit /b 1

echo [3/3] compile
cl /nologo /std:c++20 /O2 /MT /W3 /EHsc /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /Ibuild /Fobuild\ /Febuild\Lull.exe src\*.cpp build\lull.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO ^
   d3d11.lib dxgi.lib d2d1.lib dwrite.lib dcomp.lib dwmapi.lib windowscodecs.lib ^
   shell32.lib user32.lib gdi32.lib ole32.lib advapi32.lib winmm.lib shcore.lib uxtheme.lib || exit /b 1

echo Built build\Lull.exe
