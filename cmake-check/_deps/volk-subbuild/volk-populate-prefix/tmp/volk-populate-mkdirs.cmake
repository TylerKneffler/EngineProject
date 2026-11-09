# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "C:/Users/tyler/source/repos/EngineProject/cmake-check/_deps/volk-src")
  file(MAKE_DIRECTORY "C:/Users/tyler/source/repos/EngineProject/cmake-check/_deps/volk-src")
endif()
file(MAKE_DIRECTORY
  "C:/Users/tyler/source/repos/EngineProject/cmake-check/_deps/volk-build"
  "C:/Users/tyler/source/repos/EngineProject/cmake-check/_deps/volk-subbuild/volk-populate-prefix"
  "C:/Users/tyler/source/repos/EngineProject/cmake-check/_deps/volk-subbuild/volk-populate-prefix/tmp"
  "C:/Users/tyler/source/repos/EngineProject/cmake-check/_deps/volk-subbuild/volk-populate-prefix/src/volk-populate-stamp"
  "C:/Users/tyler/source/repos/EngineProject/cmake-check/_deps/volk-subbuild/volk-populate-prefix/src"
  "C:/Users/tyler/source/repos/EngineProject/cmake-check/_deps/volk-subbuild/volk-populate-prefix/src/volk-populate-stamp"
)

set(configSubDirs Debug)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/tyler/source/repos/EngineProject/cmake-check/_deps/volk-subbuild/volk-populate-prefix/src/volk-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/tyler/source/repos/EngineProject/cmake-check/_deps/volk-subbuild/volk-populate-prefix/src/volk-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
