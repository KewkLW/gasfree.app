# Cross-compilation toolchain for Windows x64 from Linux using clang-cl + lld-link + xwin.
#
# Prerequisites:
#   1. clang-cl, llvm-lib, llvm-rc, llvm-mt (LLVM/Clang on Linux)
#   2. lld-link (system `lld` package, or rustup's bundled `gcc-ld/lld-link`)
#   3. Windows CRT + SDK splatted via xwin:
#        xwin --arch x86_64 --accept-license splat \
#          --use-winsysroot-style --preserve-ms-arch-notation \
#          --output ~/.cache/xwin
#
# Usage:
#   cmake -B build-win64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-win64-clang-cl.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_CROSSCOMPILING TRUE)

# Locate xwin sysroot
if(NOT DEFINED XWIN_DIR OR XWIN_DIR STREQUAL "")
    if(DEFINED ENV{XWIN_DIR} AND NOT "$ENV{XWIN_DIR}" STREQUAL "")
        set(XWIN_DIR "$ENV{XWIN_DIR}")
    else()
        set(XWIN_DIR "$ENV{HOME}/.cache/xwin")
    endif()
endif()
if(NOT EXISTS "${XWIN_DIR}/VC" AND NOT EXISTS "${XWIN_DIR}/Windows Kits")
    message(FATAL_ERROR
        "xwin sysroot not found at '${XWIN_DIR}'. Run:\n"
        "  xwin --arch x86_64 --accept-license splat --use-winsysroot-style --preserve-ms-arch-notation --output \"${XWIN_DIR}\"\n"
        "or pass -DXWIN_DIR=/path/to/xwin.")
endif()

# Locate clang-cl & LLVM binutils
find_program(CLANG_CL_EXECUTABLE NAMES clang-cl REQUIRED)
find_program(LLVM_LIB_EXECUTABLE NAMES llvm-lib REQUIRED)
find_program(LLVM_RC_EXECUTABLE NAMES llvm-rc)
find_program(LLVM_MT_EXECUTABLE NAMES llvm-mt)

# Locate lld-link (system PATH first, then rustup's bundled lld-link)
file(GLOB _RUSTUP_LLD_CANDIDATES
    "$ENV{HOME}/.rustup/toolchains/stable-*/lib/rustlib/*/bin/gcc-ld"
    "$ENV{HOME}/.rustup/toolchains/*/lib/rustlib/*/bin/gcc-ld"
)
find_program(LLD_LINK_EXECUTABLE NAMES lld-link
    HINTS ${_RUSTUP_LLD_CANDIDATES}
    REQUIRED
)

set(CMAKE_C_COMPILER "${CLANG_CL_EXECUTABLE}")
set(CMAKE_CXX_COMPILER "${CLANG_CL_EXECUTABLE}")
set(CMAKE_LINKER "${LLD_LINK_EXECUTABLE}")
set(CMAKE_AR "${LLVM_LIB_EXECUTABLE}")
if(LLVM_RC_EXECUTABLE)
    set(CMAKE_RC_COMPILER "${LLVM_RC_EXECUTABLE}")
endif()
if(LLVM_MT_EXECUTABLE)
    set(CMAKE_MT "${LLVM_MT_EXECUTABLE}")
endif()

set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

# Pass /winsysroot to both compiler and lld-link so all MSVC CRT + Windows SDK
# headers and libraries resolve cleanly with zero extra env vars.
# Note: clang-cl takes "/winsysroot <dir>" (space), whereas lld-link takes "/winsysroot:<dir>" (colon).
# Suppress Steinberg VST3 SDK's #pragma pack noise under clang-cl.
set(_XWIN_FLAGS "-m64 /winsysroot \"${XWIN_DIR}\" -Wno-unused-command-line-argument -Wno-pragma-pack")
set(CMAKE_C_FLAGS_INIT "${_XWIN_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${_XWIN_FLAGS} /EHsc")

set(_XWIN_LINK_FLAGS "/winsysroot:\"${XWIN_DIR}\" /machine:x64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_XWIN_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_XWIN_LINK_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_XWIN_LINK_FLAGS}")

# Static release MSVC runtime (/MT) so output .clap / .vst3 DLLs need no VCRUNTIME140
# redistributable and xwin doesn't need non-redistributable debug CRT libs (libcmtd.lib).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)
set(CMAKE_TRY_COMPILE_CONFIGURATION "Release")

# Never search host Linux /usr/lib for Windows libraries
set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
