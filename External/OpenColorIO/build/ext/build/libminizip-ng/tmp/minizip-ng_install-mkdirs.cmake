# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/libminizip-ng/src/minizip-ng_install")
  file(MAKE_DIRECTORY "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/libminizip-ng/src/minizip-ng_install")
endif()
file(MAKE_DIRECTORY
  "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/libminizip-ng/src/minizip-ng_install-build"
  "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/libminizip-ng"
  "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/libminizip-ng/tmp"
  "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/libminizip-ng/src/minizip-ng_install-stamp"
  "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/libminizip-ng/src"
  "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/libminizip-ng/src/minizip-ng_install-stamp"
)

set(configSubDirs Debug;Release;MinSizeRel;RelWithDebInfo)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/libminizip-ng/src/minizip-ng_install-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/songt/Desktop/Render/External/OpenColorIO/build/ext/build/libminizip-ng/src/minizip-ng_install-stamp${cfgdir}") # cfgdir has leading slash
endif()
