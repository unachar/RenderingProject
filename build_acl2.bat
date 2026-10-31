@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d C:\Users\songt\Desktop\Render\External\ACL\upstream
python make.py -compiler vs2022 -config Release -cpu x64 -build