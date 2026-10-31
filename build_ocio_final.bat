@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d C:\Users\songt\Desktop\Render\External\OpenColorIO
rmdir /s /q build 2>nul
mkdir build
cd build
cmake -G "Ninja" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DOCIO_BUILD_APPS=OFF -DOCIO_BUILD_DOCS=OFF -DOCIO_BUILD_TESTS=OFF -DOCIO_BUILD_GPU_TESTS=OFF -DOCIO_BUILD_PYGLUE=OFF -DOCIO_BUILD_NUKE=OFF -DOCIO_BUILD_PYTHON=OFF -DCMAKE_INSTALL_PREFIX=../install -DCMAKE_POLICY_DEFAULT_CMP0063=NEW ../upstream