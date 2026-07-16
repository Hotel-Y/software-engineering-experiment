cmake_minimum_required(VERSION 3.16)

# 项目统一使用 C++17；关闭编译器扩展，保证代码在 MSVC 与 MinGW 下行为一致。
project(SimpleBackupManager VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 命令行程序的 AES-GCM、PBKDF2 和安全随机数由本地 OpenSSL 开发包提供。
set(OPENSSL_ROOT_DIR "${CMAKE_SOURCE_DIR}/third_party/openssl/mingw64")
find_package(OpenSSL REQUIRED)

# sbm 是核心命令行程序，集中承载备份、还原、校验、打包和解包功能。
add_executable(sbm
    src/main.cpp
    src/BackupManager.cpp
    src/ArchiveManager.cpp
    src/Manifest.cpp
    src/FileUtils.cpp
)

if (WIN32)
    # sbm_gui 只负责收集参数和展示结果，实际业务通过子进程调用 sbm.exe 完成。
    add_executable(sbm_gui WIN32 src/gui_win.cpp)
    target_include_directories(sbm_gui PRIVATE include)
    target_link_libraries(sbm_gui PRIVATE user32 gdi32)
endif()

target_include_directories(sbm PRIVATE include)
target_link_libraries(sbm PRIVATE OpenSSL::Crypto)

# MinGW 下 CommandLineToArgvW 所在的 shell32 需要显式链接。
if (WIN32 AND MINGW)
    target_link_libraries(sbm PRIVATE shell32)
endif()

# 两类编译器都开启较严格的警告，尽早暴露类型转换和可移植性问题。
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
