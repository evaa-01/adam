cmake_minimum_required(VERSION 3.20)

# Runtime: compiles the N64Recomp output (..\RecompiledFuncs) and links it with N64ModernRuntime
# (ultramodern + librecomp), RT64 (graphics) and RecompFrontend (launcher, settings menus, SDL input).
# Build with clang-cl + Ninja from a VS developer environment: see build.ps1.

project(AdamRecomp LANGUAGES C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-everything /W4")
    # Source paths end up in the exe (assert messages, __FILE__): make them relative to the folder holding adam and
    # its dependencies, so a shared exe doesn't show the build machine's folders.
    get_filename_component(ADAM_SOURCE_ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)
    file(TO_NATIVE_PATH "${ADAM_SOURCE_ROOT}" ADAM_SOURCE_ROOT_NATIVE)
    set(ADAM_PREFIX_MAP "/clang:-ffile-prefix-map=${ADAM_SOURCE_ROOT}/= /clang:-ffile-prefix-map=${ADAM_SOURCE_ROOT_NATIVE}\\=")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${ADAM_PREFIX_MAP}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ADAM_PREFIX_MAP}")
endif()

# The C/C++ runtime goes into adam.exe (/MT) for every target, so players don't need the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Same as Zelda64Recomp: avoid vcredist issues with the constexpr mutex constructor
add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(BUILD_SHARED_LIBS OFF)

# N64ModernRuntime lives next to this project (..\N64ModernRuntime)
set(N64MODERNRUNTIME_DIR "${CMAKE_SOURCE_DIR}/../../N64ModernRuntime" CACHE PATH "N64ModernRuntime checkout")
add_subdirectory(${N64MODERNRUNTIME_DIR} ${CMAKE_BINARY_DIR}/N64ModernRuntime)

# RT64 renderer (..\rt64 next to this project, pinned to upstream 8bd2cf1, which has plume for RecompFrontend).
set(RT64_DIR "${CMAKE_SOURCE_DIR}/../../rt64" CACHE PATH "RT64 checkout")
set(RT64_STATIC TRUE)
add_compile_definitions(HLSL_CPU NOMINMAX)
add_subdirectory(${RT64_DIR} ${CMAKE_BINARY_DIR}/rt64)
target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/rt64/src)
# RT64's bundled SDL 2.26.3 defines _m_prefetch by hand (old-clang workaround), which newer clang
# rejects as a builtin redefinition. Its include guard makes SDL skip it.
add_compile_definitions(__PRFCHWINTRIN_H)
# Newer clang makes this an error; imgui passes an uninitialized char with length 0 (never read).
target_compile_options(rt64 PRIVATE -Wno-uninitialized-const-pointer)

# SDL2 2.30.3 (as Banjo: Recompiled): window, input and audio. RecompFrontend's headers expect sdl2_SOURCE_DIR.
# RT64 still links its own 2.26.3 import library; both load the same SDL2.dll (this one) at runtime.
include(FetchContent)
FetchContent_Declare(sdl2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.3/SDL2-devel-2.30.3-VC.zip)
FetchContent_MakeAvailable(sdl2)

# Shader compiler settings RT64's build_*_shader functions read (copied from Banjo: Recompiled's CMakeLists);
# RecompFrontend compiles its two UI shaders with them.
set(DXC "${RT64_DIR}/src/contrib/dxc/bin/x64/dxc.exe")
set(DXC_COMMON_OPTS "-I${PROJECT_SOURCE_DIR}/src")
set(DXC_DXIL_OPTS "-Wno-ignored-attributes")
set(DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
set(DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T ps_6_0" "-D DYNAMIC_RENDER_PARAMS")
set(DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T vs_6_0" "-D DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
set(DXC_CS_OPTS "${DXC_COMMON_OPTS}" "-E" "CSMain" "-T cs_6_0")
set(DXC_GS_OPTS "${DXC_COMMON_OPTS}" "-E" "GSMain" "-T gs_6_0")
set(DXC_RT_OPTS "${DXC_COMMON_OPTS}" "-D" "RT_SHADER" "-T" "lib_6_3" "-fspv-target-env=vulkan1.1spirv1.4" "-fspv-extension=SPV_KHR_ray_tracing" "-fspv-extension=SPV_EXT_descriptor_indexing")

# RecompFrontend (..\RecompFrontend next to this project, N64Recomp's launcher/menu/input library; upstream main b1a1477).
set(RECOMPFRONTEND_DIR "${CMAKE_SOURCE_DIR}/../../RecompFrontend" CACHE PATH "RecompFrontend checkout")
set(RECOMP_FRONTEND_N64MODERNRUNTIME_PATH ${N64MODERNRUNTIME_DIR})
set(RECOMP_FRONTEND_RT64_PATH ${RT64_DIR})
add_subdirectory(${RECOMPFRONTEND_DIR} ${CMAKE_BINARY_DIR}/RecompFrontend)
# runtime\include\ui_funcs.h: stand-in for a game header recompui expects (see runtime\patches\recompfrontend-ui-funcs.patch).
target_include_directories(recompui PRIVATE ${CMAKE_SOURCE_DIR}/include)

set(RECOMPILED_DIR "${CMAKE_SOURCE_DIR}/../RecompiledFuncs")

# -DADAM_LIVE=ON builds the live shell: no recompiled game code in the exe; it's recompiled from the player's ROM at
# game start (src\live.cpp, inputs copied to live\ next to the exe). Build it in its own folder (build.ps1 -Live).
option(ADAM_LIVE "Build the live shell (game code recompiled from the ROM at startup)" OFF)

if (NOT ADAM_LIVE)
# RecompiledFuncs - the N64Recomp output (generated, not in git)
add_library(RecompiledFuncs STATIC)

target_compile_options(RecompiledFuncs PRIVATE
    -fno-strict-aliasing
    -Wno-unused-variable
    -Wno-implicit-function-declaration
    -Wno-everything
)

target_include_directories(RecompiledFuncs PRIVATE
    ${N64MODERNRUNTIME_DIR}/ultramodern/include
    ${N64MODERNRUNTIME_DIR}/librecomp/include
    ${N64MODERNRUNTIME_DIR}/N64Recomp/include
)

file(GLOB FUNC_C_SOURCES ${RECOMPILED_DIR}/*.c)
file(GLOB FUNC_CXX_SOURCES ${RECOMPILED_DIR}/*.cpp)
target_sources(RecompiledFuncs PRIVATE ${FUNC_C_SOURCES} ${FUNC_CXX_SOURCES})
endif()

# RSP microcode recompiled by RSPRecomp (rsp\*.toml -> rsp\*.cpp; generated, not in git)
set(RSP_DIR "${CMAKE_SOURCE_DIR}/../rsp")

# Main executable
add_executable(adam src/main.cpp src/input.cpp src/audio.cpp src/config.cpp src/rsp_interp.cpp src/launcher_art.cpp src/subtitles.cpp
    src/embedded_files_unpack.cpp ${CMAKE_BINARY_DIR}/embedded_files.cpp)

# Files built into the exe (src\embedded_files.h): the menus' assets\, SDL's controller database and, in the live shell,
# live\symbols.toml + live\config.toml. embedded_files.cpp pulls each one in with clang's #embed; a trailing 0 keeps
# empty files legal (and text null-terminated), so the size is sizeof - 1. adam.exe unpacks them into data\ at startup.
file(GLOB_RECURSE ADAM_ASSET_FILES CONFIGURE_DEPENDS RELATIVE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/assets/*")
set(ADAM_EMBEDDED "")   # list of "<published path>|<source file>|<unpack 0/1>"
foreach (file ${ADAM_ASSET_FILES})
    list(APPEND ADAM_EMBEDDED "${file}|${CMAKE_SOURCE_DIR}/${file}|1")
endforeach()
list(APPEND ADAM_EMBEDDED "recompcontrollerdb.txt|${CMAKE_SOURCE_DIR}/recompcontrollerdb.txt|0")
if (ADAM_LIVE)
    list(APPEND ADAM_EMBEDDED "live/symbols.toml|${CMAKE_SOURCE_DIR}/../symbols.toml|1")
    list(APPEND ADAM_EMBEDDED "live/config.toml|${CMAKE_SOURCE_DIR}/../config.toml|1")
endif()
set(ADAM_EMBED_ARRAYS "")
set(ADAM_EMBED_ENTRIES "")
set(ADAM_EMBED_SOURCES "")
set(index 0)
foreach (entry ${ADAM_EMBEDDED})
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 published)
    list(GET parts 1 source)
    list(GET parts 2 unpack)
    get_filename_component(source "${source}" ABSOLUTE)
    if (unpack)
        set(unpack true)
    else()
        set(unpack false)
    endif()
    string(APPEND ADAM_EMBED_ARRAYS "static const unsigned char file${index}[] = {\n#embed \"${source}\" suffix(,)\n0 };\n")
    string(APPEND ADAM_EMBED_ENTRIES "    { \"${published}\", file${index}, sizeof(file${index}) - 1, ${unpack} },\n")
    list(APPEND ADAM_EMBED_SOURCES "${source}")
    math(EXPR index "${index} + 1")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/embedded_files.cpp" @ONLY CONTENT
"// Generated by runtime/CMakeLists.txt: the files built into adam.exe (see src/embedded_files.h).
#include \"embedded_files.h\"

${ADAM_EMBED_ARRAYS}
namespace adam {
const EmbeddedFile embedded_files[] = {
${ADAM_EMBED_ENTRIES}};
const size_t embedded_file_count = ${index};
}
")
# Rebuild when an embedded file changes (the compiler's dependency list may not include #embed files).
set_source_files_properties(${CMAKE_BINARY_DIR}/embedded_files.cpp PROPERTIES
    OBJECT_DEPENDS "${ADAM_EMBED_SOURCES}" COMPILE_OPTIONS "-Wno-c23-extensions")
target_include_directories(adam PRIVATE ${CMAKE_SOURCE_DIR}/src)
# The recompiled audio microcode (generated from the ROM) only goes into the normal build; the live shell interprets it.
if (NOT ADAM_LIVE)
    target_sources(adam PRIVATE ${RSP_DIR}/aspMain.cpp)
    target_include_directories(adam PRIVATE ${RECOMPILED_DIR}) # recomp_overlays.inl
endif()
# rsp_interp.cpp decodes RSP instructions with rabbitizer, like RSPRecomp.
target_include_directories(adam PRIVATE
    ${N64MODERNRUNTIME_DIR}/N64Recomp/lib/rabbitizer/include
    ${N64MODERNRUNTIME_DIR}/N64Recomp/lib/rabbitizer/cplusplus/include
)

target_include_directories(adam PRIVATE
    ${sdl2_SOURCE_DIR}/include
    ${N64MODERNRUNTIME_DIR}/ultramodern/include
    ${N64MODERNRUNTIME_DIR}/N64Recomp/include
    ${RT64_DIR}/src
    ${RT64_DIR}/src/contrib
    ${RT64_DIR}/src/contrib/hlslpp/include
    ${RT64_DIR}/src/contrib/dxc/inc
    ${RT64_DIR}/src/rhi
    ${RT64_DIR}/src/render
    ${RT64_DIR}/src/contrib/nativefiledialog-extended/src/include
    ${RECOMPFRONTEND_DIR}/recompinput/include
    ${RECOMPFRONTEND_DIR}/recompui/include
    ${RECOMPFRONTEND_DIR}/recompui/src
)
target_link_directories(adam PRIVATE ${sdl2_SOURCE_DIR}/lib/x64)

# -march=nehalem (as Zelda64Recomp): the recompiled RSP vector code (librecomp/rsp_vu_impl.hpp) needs SSE4.1.
target_compile_options(adam PRIVATE -march=nehalem -fno-strict-aliasing -fms-extensions)

if (MSVC)
    # Same as Zelda64Recomp: identical code folding breaks function patching
    target_link_options(adam PRIVATE /OPT:NOICF)
    # Only the .pdb's file name in the exe, not the build folder.
    target_link_options(adam PRIVATE "/PDBALTPATH:%_PDB%")
endif()

# RecompiledFuncs must come first: it and librecomp both define the 64-bit division helpers (__divdi3_recomp...).
if (NOT ADAM_LIVE)
    target_link_libraries(adam PRIVATE RecompiledFuncs)
endif()
target_link_libraries(adam PRIVATE recompui recompinput librecomp ultramodern rt64 nfd SDL2 Winmm.lib)
if (ADAM_LIVE)
    # N64Recomp's live recompiler (LiveRecomp + its symbol-file loader) comes with librecomp.
    # N64Recomp's symbol-file loader (Context::from_symbol_file) is in its src\config.cpp, which upstream only
    # compiles into the command-line tools.
    target_sources(adam PRIVATE src/live.cpp ${N64MODERNRUNTIME_DIR}/N64Recomp/src/config.cpp)
    target_include_directories(adam PRIVATE ${N64MODERNRUNTIME_DIR}/N64Recomp/src)
    target_compile_definitions(adam PRIVATE ADAM_LIVE)
    target_link_libraries(adam PRIVATE LiveRecomp N64Recomp tomlplusplus::tomlplusplus fmt)
endif()

# Next to the exe: SDL2.dll (2.30.3) and RT64's shader compiler DLLs (RT64 loads those from the exe's folder).
# Everything else the exe needs is built into it (see embedded_files.cpp above).
add_custom_command(TARGET adam POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
        "${RT64_DIR}/src/contrib/dxc/bin/x64/dxil.dll"
        "${RT64_DIR}/src/contrib/dxc/bin/x64/dxcompiler.dll"
        $<TARGET_FILE_DIR:adam>)
