# Build file for MiniVectorDB.  The database only needs a C++17 compiler and
# the C++ standard library, there are no third party dependencies.
cmake_minimum_required(VERSION 3.10)
project(minivectordb CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(minivectordb
  src/base.cpp
  src/value.cpp
  src/sql.cpp
  src/storage.cpp
  src/btree.cpp
  src/condition.cpp
  src/engine.cpp
  src/executor.cpp
  src/server.cpp
  src/main.cpp)

target_include_directories(minivectordb PRIVATE src)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(minivectordb PRIVATE -O2 -Wall -Wextra)
  # The distance kernel selects an AVX2 implementation at runtime, the rest of
  # the binary stays portable.
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-msse2" MVDB_HAS_SSE2)
  if(MVDB_HAS_SSE2)
    target_compile_options(minivectordb PRIVATE -msse2)
  endif()
endif()

install(TARGETS minivectordb RUNTIME DESTINATION bin)