# Xbox D3D8 Compatibility Layer
#   _WIN32 -> backed by Direct3D 11 (d3d8_*.c, d3d8_combiners.c, d3d8_vsh.c)
#   POSIX  -> backed by OpenGL 3.3 core via SDL2 + libepoxy (d3d8_gl.c)

if(WIN32 AND NOT XBOXRECOMP_WIN_GL)
    add_library(xbox_d3d8 STATIC
        d3d8_device.c
        d3d8_resources.c
        d3d8_shaders.c
        d3d8_states.c
        d3d8_combiners.c
        d3d8_vsh.c
    )
    target_link_libraries(xbox_d3d8 PUBLIC platform d3d11 dxgi dxguid d3dcompiler)

    # Offline smoke test for the pure format layer (no D3D11 device needed).
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3d8_smoke)
elseif(APPLE OR XBOXRECOMP_WIN_GL OR ANDROID OR SWITCH)
    # Nothing to build: a title that brings its own D3D (JSRF's is linked into
    # its executable) reaches the GPU through the pushbuffer and nv2a_gl, and
    # the OpenGL D3D8 below wants SDL2 and libepoxy, which a Mac only has
    # through a package manager -- the Intel Homebrew's x86_64 copies, which
    # an arm64 build cannot link -- and an iPhone not at all. The headers
    # stay available.
    add_library(xbox_d3d8 INTERFACE)
    target_include_directories(xbox_d3d8 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(xbox_d3d8 INTERFACE platform)
    return()
else()
    add_library(xbox_d3d8 STATIC
        d3d8_gl.c
    )
    find_package(SDL2 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EPOXY REQUIRED epoxy)
    target_include_directories(xbox_d3d8 PRIVATE ${EPOXY_INCLUDE_DIRS})
    target_link_libraries(xbox_d3d8 PUBLIC platform SDL2::SDL2 ${EPOXY_LIBRARIES})
endif()

target_include_directories(xbox_d3d8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_d3d8 PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_d3d8 PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
