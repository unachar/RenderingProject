# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/pybind11/src/pybind11_install")
  file(MAKE_DIRECTORY "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/pybind11/src/pybind11_install")
endif()
file(MAKE_DIRECTORY
  "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/pybind11/src/pybind11_install-build"
  "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/pybind11"
  "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/pybind11/tmp"
  "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/pybind11/src/pybind11_install-stamp"
  "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/pybind11/src"
  "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/pybind11/src/pybind11_install-stamp"
)

set(configSubDirs Debug;Release;MinSizeRel;RelWithDebInfo)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/pybind11/src/pybind11_install-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/pybind11/src/pybind11_install-stamp${cfgdir}") # cfgdir has leading slash
endif()
