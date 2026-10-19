cmake_minimum_required(VERSION 3.20)
project(VirtualGeometryPhase1Tests LANGUAGES CXX)
enable_testing()
set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(VG "${ROOT}/source/virtualgeometry")
set(MESHOPT "${ROOT}/External/meshoptimizer/src")
add_executable(virtualgeometry_tests
    "${ROOT}/tests/virtualgeometry_tests.cpp"
    "${ROOT}/tests/virtualgeometry_runtime_tests.cpp"
    "${ROOT}/tests/virtualgeometry_selection_tests.cpp"
    "${ROOT}/tests/virtualgeometry_gpu_tests.cpp"
    "${ROOT}/tests/virtualgeometry_page_tests.cpp"
    "${ROOT}/tests/virtualgeometry_resident_selection_tests.cpp"
    "${ROOT}/tests/virtualgeometry_residency_tests.cpp"
    "${VG}/virtualgeometryresidency.cpp"
    "${ROOT}/tests/virtualgeometry_page_io_tests.cpp"
    "${VG}/virtualgeometrypageio.cpp"
    "${VG}/virtualgeometrypagestress.cpp"
    "${VG}/virtualgeometrypages.cpp"
    "${VG}/virtualgeometrycompression.cpp"
    "${ROOT}/tests/virtualgeometry_compression_tests.cpp"
    "${VG}/virtualgeometrypagecache.cpp"
    "${VG}/virtualgeometryselection.cpp"
    "${VG}/virtualgeometrygpu.cpp"
    "${VG}/virtualgeometryruntime.cpp"
    "${VG}/virtualgeometryimport.cpp"
    "${ROOT}/source/rendering/meshlets/meshletbuilder.cpp"
    "${VG}/virtualgeometrybuilder.cpp"
    "${VG}/virtualgeometrygeometry.cpp"
    "${VG}/virtualgeometryhierarchy.cpp"
    "${VG}/virtualgeometryvalidator.cpp"
    "${VG}/virtualgeometryserializer.cpp"
    "${VG}/virtualgeometrymetadata.cpp"
    "${VG}/virtualgeometryreport.cpp"
    "${MESHOPT}/allocator.cpp"
    "${MESHOPT}/clusterizer.cpp"
    "${MESHOPT}/meshletutils.cpp"
    "${MESHOPT}/partition.cpp"
    "${MESHOPT}/indexgenerator.cpp"
    "${MESHOPT}/simplifier.cpp"
    "${MESHOPT}/spatialorder.cpp")
target_include_directories(virtualgeometry_tests PRIVATE "${ROOT}/source")
target_compile_features(virtualgeometry_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(virtualgeometry_tests PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(virtualgeometry_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME virtualgeometry_phase1 COMMAND virtualgeometry_tests)
add_test(NAME virtualgeometry_million COMMAND virtualgeometry_tests --million)
set_tests_properties(virtualgeometry_million PROPERTIES LABELS stress)
# Keep filesystem fixtures in the writable build tree. Windows sandbox temp
# directories can reject the atomic file publication used by the cache tests.
foreach(test_name virtualgeometry_phase1 virtualgeometry_million)
    set(test_temp "${CMAKE_CURRENT_BINARY_DIR}/test-temp/${test_name}")
    file(MAKE_DIRECTORY "${test_temp}")
    set_tests_properties(${test_name} PROPERTIES
        ENVIRONMENT "TEMP=${test_temp};TMP=${test_temp};TMPDIR=${test_temp}")
endforeach()

add_executable(virtualgeometry_shadow_selection_tests
    "${ROOT}/tests/virtualgeometry_shadow_selection_tests.cpp"
    "${ROOT}/tests/virtualgeometry_selection_tests.cpp"
    "${VG}/virtualgeometryselection.cpp")
target_include_directories(virtualgeometry_shadow_selection_tests PRIVATE "${ROOT}/source")
target_compile_features(virtualgeometry_shadow_selection_tests PRIVATE cxx_std_20)
add_test(NAME virtualgeometry_shadow_selection COMMAND virtualgeometry_shadow_selection_tests)

add_executable(renderfeature_tests "${ROOT}/tests/renderfeature_tests.cpp")
target_include_directories(renderfeature_tests PRIVATE "${ROOT}/source")
target_compile_features(renderfeature_tests PRIVATE cxx_std_17)
add_test(NAME renderfeature_policy COMMAND renderfeature_tests)

# Explicit opt-in target: the normal CPU test build does not compile or run D3D12 parity.
if(WIN32)
    add_executable(virtualgeometry_decoder_parity_tests EXCLUDE_FROM_ALL
        "${ROOT}/tests/virtualgeometry_decoder_parity_tests.cpp"
        "${VG}/virtualgeometrycompression.cpp")
    target_include_directories(virtualgeometry_decoder_parity_tests PRIVATE "${ROOT}/source")
    target_compile_features(virtualgeometry_decoder_parity_tests PRIVATE cxx_std_20)
    target_link_libraries(virtualgeometry_decoder_parity_tests PRIVATE d3d12 dxgi)
    set(PARITY_SHADER "${CMAKE_CURRENT_BINARY_DIR}/VirtualGeometryDecoderParity.cso")
    set(PARITY_DXC "${ROOT}/packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native/bin/x64/dxc.exe")
    add_custom_command(OUTPUT "${PARITY_SHADER}"
        COMMAND "${PARITY_DXC}" -T cs_6_5 -E main -O3 -Fo "${PARITY_SHADER}"
            "${ROOT}/tests/virtualgeometry_decoder_parity_cs.hlsl"
        DEPENDS "${ROOT}/tests/virtualgeometry_decoder_parity_cs.hlsl"
            "${ROOT}/shader/hlsl/VGGeometryDecode.hlsli"
        VERBATIM)
    add_custom_target(virtualgeometry_decoder_parity_shader DEPENDS "${PARITY_SHADER}")
    add_dependencies(virtualgeometry_decoder_parity_tests virtualgeometry_decoder_parity_shader)
endif()
