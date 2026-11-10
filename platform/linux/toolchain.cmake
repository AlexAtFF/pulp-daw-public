# PULP on Linux (platform/linux/README.md). build.sh passes it to CMake:
#   cmake -S . -B build/linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=platform/linux/toolchain.cmake
# It puts pulp_compat.h (the Microsoft C-runtime calls the engine makes) in front of every C++ file, puts the small <windows.h>
# that the MP3 damage test includes on the include path, and defines _DEBUG in Debug builds, as MSVC does.
set(PULP_LINUX "${CMAKE_CURRENT_LIST_DIR}")
set(CMAKE_CXX_FLAGS_INIT "-include \"${PULP_LINUX}/pulp_compat.h\" -pthread")
set(CMAKE_CXX_FLAGS_DEBUG_INIT "-g -D_DEBUG")
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES "${PULP_LINUX}/include")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-pthread")
