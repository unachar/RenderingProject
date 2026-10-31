@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d C:\Users\songt\Desktop\Render\External\basis_universal
rmdir /s /q build_transcoder 2>nul
mkdir build_transcoder
cd build_transcoder
cmake -G "Ninja" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=../install -DBASISU_BUILD_TOOLS=OFF -DBASISU_BUILD_TESTS=OFF -DBASISU_EXAMPLES=OFF -DBASISU_ZSTD=OFF ../upstream