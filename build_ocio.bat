@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d C:\Users\songt\Desktop\Render\External\OpenColorIO\build
cmake -G "Ninja" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DOCIO_BUILD_APPS=OFF -DOCIO_BUILD_DOCS=OFF -DOCIO_BUILD_TESTS=OFF -DOCIO_BUILD_GPU_TESTS=OFF -DOCIO_BUILD_PYGLUE=OFF -DOCIO_BUILD_NUKE=OFF -DOCIO_USE_SSE=ON -DCMAKE_INSTALL_PREFIX=../install ../upstream