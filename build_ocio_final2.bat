@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d C:\Users\songt\Desktop\Render\External\OpenColorIO\build
cmake --build . --config Release --target install