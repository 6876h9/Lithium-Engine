cmake_minimum_required(VERSION 3.16)
project(CustomEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Platform dependency discovery
#
# Linux uses the system SDL2/OpenGL. Windows (MinGW cross-build) uses the
# prebuilt SDL2 and Embree packages staged under thirdparty/, because neither is
# discoverable through a Linux find_package when cross-compiling.
# ---------------------------------------------------------------------------
if(WIN32)
    set(SDL2_WIN_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/SDL2_windows)
    set(EMBREE_WIN_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/embree_windows)

    if(NOT EXISTS "${SDL2_WIN_DIR}/lib/libSDL2.dll.a")
        message(FATAL_ERROR "Windows SDL2 not found at ${SDL2_WIN_DIR}. See docs/Windows_Build_Guide.md")
    endif()
    if(NOT EXISTS "${EMBREE_WIN_DIR}/lib/embree4.lib")
        message(FATAL_ERROR "Windows Embree not found at ${EMBREE_WIN_DIR}. See docs/Windows_Build_Guide.md")
    endif()

    # Both forms: the SDL2 subdirectory so bare "#include <SDL.h>" resolves (which is
    # what the ImGui SDL2 backend uses, and what pkg-config gives on Linux via
    # -I/usr/include/SDL2), plus the parent so "#include <SDL2/SDL.h>" works too.
    set(SDL2_INCLUDE_DIRS "${SDL2_WIN_DIR}/include/SDL2" "${SDL2_WIN_DIR}/include")
    # SDL2main must precede SDL2, and mingw32 must precede SDL2main - the linker
    # resolves left to right and SDL2main provides the WinMain that wraps main().
    set(SDL2_LIBRARIES mingw32 "${SDL2_WIN_DIR}/lib/libSDL2main.a" "${SDL2_WIN_DIR}/lib/libSDL2.dll.a")
    set(OPENGL_INCLUDE_DIR "")
    set(OPENGL_LIBRARIES opengl32)

    set(EMBREE_INCLUDE_DIR "${EMBREE_WIN_DIR}/include")
    set(EMBREE_LIBRARIES "${EMBREE_WIN_DIR}/lib/embree4.lib")

    # Windows equivalents of the POSIX libs linked on Linux. dl/pthread do not exist;
    # ws2_32 is required by enet, and the rest are SDL2's own Win32 dependencies.
    set(PLATFORM_LIBS ws2_32 winmm imm32 setupapi version ole32 oleaut32 cfgmgr32 shell32 user32 gdi32 advapi32)
    set(EMBREE_LINK_DIR "")
else()
    find_package(SDL2 REQUIRED)
    find_package(OpenGL REQUIRED)

    set(EMBREE_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/embree)
    set(EMBREE_LIBRARIES embree4 tbb)
    set(PLATFORM_LIBS dl pthread m)
    set(EMBREE_LINK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/embree)
endif()

# Setup ImGui directories
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui)

# Include directories
include_directories(
    include
    ${SDL2_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imnodes
)

# Source files
set(SOURCES
    src/core/input_map.cpp
    src/main.cpp
    src/core/engine.cpp
    src/core/platform.cpp
    src/core/window.cpp
    src/core/resource_manager.cpp
    src/core/mesh_resource.cpp
    src/core/texture_resource.cpp
    src/core/browser.cpp
    src/core/editor.cpp
    src/core/noise.cpp
    src/core/model_importer.cpp
    src/core/mesh_simplifier.cpp
    src/core/asset_database.cpp
    src/core/scene_serializer.cpp
    src/core/selftest.cpp
    src/core/visual_script_editor.cpp
    src/renderer/renderer.cpp
    src/renderer/tesla.cpp
    src/renderer/material_shader.cpp
    src/renderer/lightmapper.cpp
    src/world/actor.cpp
    src/world/component.cpp
    src/world/static_mesh_component.cpp
    src/world/animation_player.cpp
    src/world/physics_attribute.cpp
    src/world/character_controller_component.cpp
    src/world/joint_component.cpp
    src/world/ui_canvas_component.cpp
    src/world/lod_group_component.cpp
    src/world/nav_agent_component.cpp
    src/world/terrain_component.cpp
    src/world/particle_emitter_component.cpp
    src/core/threading/task_graph.cpp
    src/world/cminus_component.cpp
    src/world/cpp_script_component.cpp
    src/world/light_components.cpp
    src/world/audio_component.cpp
    src/world/sprite_actor.cpp
    src/world/camera_component.cpp
    src/scripting/cminus_interpreter.cpp
    src/scripting/cminus_builtins.cpp
    src/core/material_graph.cpp
    src/core/external_editor.cpp
    src/scripting/lua_api.cpp
    src/world/lua_script_component.cpp
    src/audio/miniaudio_impl.cpp
    src/audio/audio_engine.cpp
    src/audio/audio_bus.cpp
    src/physics/physics_engine.cpp
    src/navigation/navmesh.cpp
    src/network/network_manager.cpp

    # ImGui sources
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMGUI_DIR}/ImGuizmo.cpp
    
    # ImNodes
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imnodes/imnodes.cpp
)

# Fetch Assimp
include(FetchContent)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# assimp defaults to building every importer it has (~80 formats, including heavy
# CAD/industry ones like IFC and STEP). Those dominate build time and peak memory -
# the IFC reader alone is large enough to exhaust RAM on a 8GB machine when several
# translation units compile in parallel - and the engine never loads any of them.
#
# Restrict to the formats a game project actually imports. This also cuts the binary
# size noticeably.
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
foreach(fmt OBJ FBX GLTF COLLADA PLY STL 3DS)
    set(ASSIMP_BUILD_${fmt}_IMPORTER ON CACHE BOOL "" FORCE)
endforeach()
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG v5.3.1
)
FetchContent_MakeAvailable(assimp)

# assimp's DefaultIOStream.cpp picks its stat() variant from __MSVCRT_VERSION__, which
# MinGW does not predefine. Left at 0 it falls through to the _fstat32/_stat32 branch,
# and MinGW's import libraries have no _fstat32 - producing an undefined __imp__fstat32
# at link time. Selecting the 64-bit CRT makes it take the _fstat64 path, which MinGW
# does provide.
if(WIN32 AND MINGW)
    target_compile_definitions(assimp PRIVATE __MSVCRT_VERSION__=0x0700)
endif()

# Fetch JoltPhysics
#
# The instruction set has to be pinned to what the oldest target CPU actually has.
# Jolt defaults LZCNT/TZCNT/F16C/FMADD to ON, and leaving them on while AVX is off is
# not a coherent baseline: those four arrived with Haswell, so on a Sandy Bridge or
# Ivy Bridge machine the build succeeds and then dies with SIGILL the first time a
# shape is built (Jolt's BVH encoder packs node bounds as half floats via the F16C
# vcvtps2ph, which such a CPU does not have).
set(USE_SSE4_1 ON CACHE BOOL "" FORCE)
set(USE_SSE4_2 ON CACHE BOOL "" FORCE)
set(USE_AVX2 OFF CACHE BOOL "" FORCE)
set(USE_AVX OFF CACHE BOOL "" FORCE)
set(USE_AVX512 OFF CACHE BOOL "" FORCE)
set(USE_LZCNT OFF CACHE BOOL "" FORCE)
set(USE_TZCNT OFF CACHE BOOL "" FORCE)
set(USE_F16C OFF CACHE BOOL "" FORCE)
set(USE_FMADD OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG        v5.0.0
    SOURCE_SUBDIR  "Build"
)
FetchContent_MakeAvailable(JoltPhysics)

# Jolt compiles itself with -Werror. Jolt v5.0.0 predates GCC's current
# -Wmaybe-uninitialized analysis, which now fires inside SoftBodyMotionProperties
# and turns a third-party warning into a failed build of this engine. Downgrade
# -Werror for Jolt's own target only - warnings on engine code stay fatal.
if (TARGET Jolt AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(Jolt PRIVATE -Wno-error)
endif()

# Fetch Lua (gameplay scripting)
#
# The upstream repo ships no CMakeLists - it is built by a hand-written makefile -
# so the library target is declared here. lua.c and luac.c are the standalone
# interpreter and bytecode compiler; both define their own main() and must be
# excluded or they collide with the engine's.
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        v5.4.8
)
FetchContent_MakeAvailable(lua)

file(GLOB LUA_SOURCES ${lua_SOURCE_DIR}/*.c)
list(REMOVE_ITEM LUA_SOURCES ${lua_SOURCE_DIR}/lua.c ${lua_SOURCE_DIR}/luac.c)
add_library(lua_static STATIC ${LUA_SOURCES})
target_include_directories(lua_static PUBLIC ${lua_SOURCE_DIR})
# Compiled as C++ so that a Lua error longjmp-ing past engine frames is raised as a
# C++ exception instead. Lua's own error handling is built on setjmp/longjmp, which
# would otherwise skip every destructor between the error and the protected call.
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
if(UNIX)
    # Enables dlopen-based package.loadlib. LUA_USE_LINUX would also pull in readline,
    # which only the standalone interpreter needs and which is not linked here.
    target_compile_definitions(lua_static PRIVATE LUA_USE_POSIX LUA_USE_DLOPEN)
endif()

# Fetch ENet (Networking)
FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG v1.3.18
)
FetchContent_MakeAvailable(enet)

# Executable - Engine
add_executable(Lithium_Engine ${SOURCES})
target_include_directories(Lithium_Engine PRIVATE include src ${EMBREE_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/miniaudio ${enet_SOURCE_DIR}/include)
target_link_directories(Lithium_Engine PRIVATE include/imgui ${EMBREE_LINK_DIR})
target_link_libraries(Lithium_Engine
    ${SDL2_LIBRARIES}
    ${OPENGL_LIBRARIES}
    assimp
    Jolt
    enet
    lua_static
    ${EMBREE_LIBRARIES}
    ${PLATFORM_LIBS}
)
if(WIN32)
    # Windows resolves DLLs from the executable's own directory, so there is no
    # RPATH equivalent to set - the DLLs are copied beside the .exe instead.
    #
    # WIN32_EXECUTABLE builds against the GUI subsystem rather than the console one,
    # so launching the engine does not also open a stray black console window.
    # Diagnostics already go to LithiumEngine_Startup.log, not to stdout.
    set_target_properties(Lithium_Engine PROPERTIES ENABLE_EXPORTS TRUE WIN32_EXECUTABLE TRUE)
else()
    set_target_properties(Lithium_Engine PROPERTIES
        ENABLE_EXPORTS TRUE
        BUILD_WITH_INSTALL_RPATH TRUE
        # $ORIGIN is resolved by the dynamic loader relative to the executable itself.
        # An absolute build-machine path here (which is what this used to be) bakes
        # /home/<builder>/... into the binary, so it loads on the build machine and
        # nowhere else - fatal for a distributed build.
        INSTALL_RPATH "$ORIGIN/lib:$ORIGIN"
    )
endif()

# Executable - Game
add_executable(Lithium_Game ${SOURCES})
target_compile_definitions(Lithium_Game PRIVATE STANDALONE_GAME)
target_include_directories(Lithium_Game PRIVATE include src ${EMBREE_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/miniaudio ${enet_SOURCE_DIR}/include)
target_link_directories(Lithium_Game PRIVATE include/imgui ${EMBREE_LINK_DIR})
target_link_libraries(Lithium_Game
    ${SDL2_LIBRARIES}
    ${OPENGL_LIBRARIES}
    assimp
    Jolt
    enet
    lua_static
    ${EMBREE_LIBRARIES}
    ${PLATFORM_LIBS}
)
if(WIN32)
    # Windows resolves DLLs from the executable's own directory, so there is no
    # RPATH equivalent to set - the DLLs are copied beside the .exe instead.
    set_target_properties(Lithium_Game PROPERTIES ENABLE_EXPORTS TRUE WIN32_EXECUTABLE TRUE)
else()
    set_target_properties(Lithium_Game PROPERTIES
        ENABLE_EXPORTS TRUE
        BUILD_WITH_INSTALL_RPATH TRUE
        # $ORIGIN is resolved by the dynamic loader relative to the executable itself.
        # An absolute build-machine path here (which is what this used to be) bakes
        # /home/<builder>/... into the binary, so it loads on the build machine and
        # nowhere else - fatal for a distributed build.
        INSTALL_RPATH "$ORIGIN/lib:$ORIGIN"
    )
endif()

# Runtime libraries that ship with the engine, per platform. Only the file list and
# the destination sub-directory are decided here; the copy command itself is built
# inside the loop below, because it needs $<TARGET_FILE_DIR:${target}> and ${target}
# does not exist yet at this point.
#
# Windows resolves DLLs from the executable's own directory (there is no RPATH), so
# they go beside the .exe; Linux uses lib/, which is what $ORIGIN/lib points at.
if(WIN32)
    set(LITHIUM_RUNTIME_LIBS
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/embree_windows/bin/embree4.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/embree_windows/bin/tbb12.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/embree_windows/bin/tbbmalloc.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/SDL2_windows/bin/SDL2.dll")
    set(LITHIUM_RUNTIME_LIB_SUBDIR "")
else()
    set(LITHIUM_RUNTIME_LIBS
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/embree/libembree4.so.4"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/embree/libsycl.so.8"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/embree/libtbb.so.12"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/embree/libur_loader.so.0")
    set(LITHIUM_RUNTIME_LIB_SUBDIR "/lib")
endif()

# Ship engine-bundled default assets (the default HDRI environment map, and
# the DamagedHelmet sample mesh/texture used by the Blank Template's default
# scene) next to both executables. Uses a standalone ALL target rather than
# POST_BUILD on Lithium_Engine/Lithium_Game: POST_BUILD only re-runs when that
# specific target actually relinks, so if these files ever go missing (e.g.
# a stray "rm") without any source change, a normal `cmake --build` would
# silently skip restoring them. An ALL custom target with no OUTPUT re-runs
# copy_if_different on every build invocation regardless.
# Assets copied individually rather than by copy_directory. Each one's .meta
# sidecar has to travel with it: the GUIDs are authored against the source tree by
# `Lithium_Engine --index-assets` and committed, and if the build output did not
# inherit them the engine would mint fresh ones there on first run. Every clean
# build would then issue different GUIDs and every scene reference would break -
# the precise failure the sidecars exist to prevent. Directories copied whole
# already carry their sidecars, so they are not listed here.
set(LITHIUM_INDIVIDUAL_ASSETS
    Content/DamagedHelmet.mesh
    Content/Sofa_01.bin
    Content/Sofa_01.gltf
    Content/Textures/DamagedHelmet_embedded0.jpg
    Content/Textures/WoodFloor.jpg
    EngineContent/DefaultSky.hdr
    EngineContent/NightSky.hdr
)

foreach(target Lithium_Engine Lithium_Game)
    # One copy_if_exists invocation per sidecar, tolerating the ones a fresh
    # checkout has not indexed yet.
    set(${target}_meta_commands "")
    foreach(asset ${LITHIUM_INDIVIDUAL_ASSETS})
        list(APPEND ${target}_meta_commands
            COMMAND ${CMAKE_COMMAND}
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/${asset}.meta
                -DDST=$<TARGET_FILE_DIR:${target}>/${asset}.meta
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_if_exists.cmake
        )
    endforeach()

    add_custom_target(${target}_content ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/EngineContent"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/Content/Textures"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/EngineContent/DefaultSky.hdr"
            "$<TARGET_FILE_DIR:${target}>/EngineContent/DefaultSky.hdr"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/DamagedHelmet.mesh"
            "$<TARGET_FILE_DIR:${target}>/Content/DamagedHelmet.mesh"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Textures/DamagedHelmet_embedded0.jpg"
            "$<TARGET_FILE_DIR:${target}>/Content/Textures/DamagedHelmet_embedded0.jpg"
        # CC0 (public domain) assets from Poly Haven used by the Reflections Showcase:
        # a night sky HDRI, a wood floor texture, and a sofa model with its textures.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/EngineContent/NightSky.hdr"
            "$<TARGET_FILE_DIR:${target}>/EngineContent/NightSky.hdr"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Textures/WoodFloor.jpg"
            "$<TARGET_FILE_DIR:${target}>/Content/Textures/WoodFloor.jpg"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Sofa_01.gltf"
            "$<TARGET_FILE_DIR:${target}>/Content/Sofa_01.gltf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/Sofa_01.bin"
            "$<TARGET_FILE_DIR:${target}>/Content/Sofa_01.bin"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/textures"
            "$<TARGET_FILE_DIR:${target}>/Content/textures"
        # The sample game. Its scene names the script by a path relative to the
        # working directory, so the two have to sit beside the executable together.
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/Content/LithiumTest"
            "$<TARGET_FILE_DIR:${target}>/Content/LithiumTest"
        # The CC0 sky library is a few hundred MB, so it is mirrored with
        # copy_directory_if_different rather than copy_directory: an unconditional
        # copy would re-write every HDRI on every single build.
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/EngineContent/Skies"
            "$<TARGET_FILE_DIR:${target}>/EngineContent/Skies"
        # Embree and its SYCL/TBB runtime are not present on a normal Linux install,
        # so they ship beside the binary. RUNPATH is $ORIGIN/lib, which means the
        # loader finds them relative to the executable wherever it is unpacked.
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>${LITHIUM_RUNTIME_LIB_SUBDIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${LITHIUM_RUNTIME_LIBS}
            "$<TARGET_FILE_DIR:${target}>${LITHIUM_RUNTIME_LIB_SUBDIR}"
        ${${target}_meta_commands}
        DEPENDS ${target}
    )
endforeach()

# Install executable and thirdparty libraries
install(TARGETS Lithium_Engine Lithium_Game DESTINATION bin)
install(DIRECTORY thirdparty/embree/ DESTINATION lib/embree FILES_MATCHING PATTERN "*.so*")
install(FILES EngineContent/DefaultSky.hdr DESTINATION bin/EngineContent)

# --- CPack Installer Configuration ---
set(CPACK_PACKAGE_NAME "Lithium_Engine")
set(CPACK_PACKAGE_VENDOR "YourCompany")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lithium Engine - Custom C++ Game Engine")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_VERSION_MAJOR "1")
set(CPACK_PACKAGE_VERSION_MINOR "0")
set(CPACK_PACKAGE_VERSION_PATCH "0")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Lithium_Engine")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Developer")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libsdl2-2.0-0, libgl1, libassimp5")
include(CPack)
