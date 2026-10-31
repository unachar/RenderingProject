@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d C:\Users\songt\Desktop\Render\External\KTX-Software\build
cmake --build . --config Release --target install