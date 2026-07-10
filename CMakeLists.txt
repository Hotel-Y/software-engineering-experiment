cmake_minimum_required(VERSION 3.16)

project(SimpleBackupManager VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(OPENSSL_ROOT_DIR "${CMAKE_SOURCE_DIR}/third_party/openssl/mingw64")
find_package(OpenSSL REQUIRED)

add_executable(sbm
    src/main.cpp
    src/BackupManager.cpp
    src/ArchiveManager.cpp
    src/Manifest.cpp
    src/FileUtils.cpp
)

target_include_directories(sbm PRIVATE include)
target_link_libraries(sbm PRIVATE OpenSSL::Crypto)

if (MSVC)
    target_compile_options(sbm PRIVATE /W4)
else()
    target_compile_options(sbm PRIVATE -Wall -Wextra -pedantic)
endif()
