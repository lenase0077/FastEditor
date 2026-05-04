@echo off
setlocal enabledelayedexpansion

echo Compilando FastEditor...

set PATH=%PATH%;C:\Program Files\CodeBlocks\MinGW\bin

echo [1/3] Compilando recurso de icono...
windres --preprocessor=cpp resource.rc -O coff -o resource.res
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: No se pudo compilar el recurso (icono)
    pause
    exit /b 1
)

echo [2/3] Compilando codigo...
g++ -std=c++14 -Iimgui main.cpp imgui/imgui.cpp imgui/imgui_demo.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp imgui/imgui_impl_win32.cpp imgui/imgui_impl_opengl3.cpp resource.res -lopengl32 -lgdi32 -ldwmapi -lcomdlg32 -mwindows -o FastEditor.exe
if %ERRORLEVEL% NEQ 0 (
    echo ERROR DE COMPILACION
    pause
    exit /b 1
)

echo [3/3] Compilacion OK! Ejecutando...
start FastEditor.exe
