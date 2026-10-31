@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d C:\Users\songt\Desktop\Render\External\KTX-Software
rmdir /s /q build 2>nul
mkdir build
cd build
cmake -G "Ninja" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=../install -DKTX_FEATURE_TOOLS=OFF -DKTX_FEATURE_TESTS=OFF ../upstream