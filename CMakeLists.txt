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

if (WIN32)
    add_executable(sbm_gui WIN32 src/gui_win.cpp)
    target_include_directories(sbm_gui PRIVATE include)
    target_link_libraries(sbm_gui PRIVATE user32 gdi32)
endif()

target_include_directories(sbm PRIVATE include)
target_link_libraries(sbm PRIVATE OpenSSL::Crypto)

if (WIN32 AND MINGW)
    target_link_libraries(sbm PRIVATE shell32)
endif()

if (MSVC)
    target_compile_options(sbm PRIVATE /W4)
    if (TARGET sbm_gui)
        target_compile_options(sbm_gui PRIVATE /W4)
    endif()
else()
    target_compile_options(sbm PRIVATE -Wall -Wextra -pedantic)
    if (TARGET sbm_gui)
        target_compile_options(sbm_gui PRIVATE -Wall -Wextra -pedantic)
    endif()
endif()
