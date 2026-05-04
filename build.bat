@echo off
echo Compilando FastEditor...
g++ -std=c++14 -Iimgui main.cpp imgui/imgui.cpp imgui/imgui_demo.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp imgui/imgui_impl_win32.cpp imgui/imgui_impl_opengl3.cpp -lopengl32 -lgdi32 -ldwmapi -lcomdlg32 -mwindows -o FastEditor.exe
if %ERRORLEVEL% NEQ 0 (
    echo ERROR DE COMPILACION
    pause
) else (
    echo Compilacion OK. Ejecutando...
    start FastEditor.exe
)
