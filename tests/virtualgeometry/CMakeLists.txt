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
    "${VG}/virtualgeometrypages.cpp"
    "${VG}/virtualgeometrypagecache.cpp"
    "${VG}/virtualgeometryselection.cpp"
    "${VG}/virtualgeometrygpu.cpp"
    "${VG}/virtualgeometryruntime.cpp"
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
