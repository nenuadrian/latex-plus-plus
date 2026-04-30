# ── latex3d ──────────────────────────────────────────────────────────────────
# Standalone library: OpenType font + LaTeX → 3D mesh.
#
# Builds two ways:
#
#   1. In-tree (as a sub-project, e.g. of Hades World Online).
#      Parent project provides the freetype / glm / earcut_hpp / Catch2
#      targets via FetchContent; we detect that and reuse them.
#
#   2. Standalone:
#        cmake -S libs/latex3d -B build && cmake --build build
#      Pulls deps from system find_package(); falls back to FetchContent for
#      earcut_hpp (header-only) when not installed.

cmake_minimum_required(VERSION 3.18)
project(latex3d VERSION 0.1.0 LANGUAGES CXX
        DESCRIPTION "OpenType font + LaTeX -> 3D extruded mesh"
        HOMEPAGE_URL "")

option(LATEX3D_BUILD_TESTS "Build latex3d unit tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Dependencies ─────────────────────────────────────────────────────────────
# When in-tree, the parent has already populated these via FetchContent.
# When standalone, fall back to find_package / vendored FetchContent.
if(NOT TARGET freetype AND NOT TARGET Freetype::Freetype)
    find_package(Freetype REQUIRED)
endif()

if(NOT TARGET glm AND NOT TARGET glm::glm)
    # glm ships a CMake config on most distros, but not all (notably
    # plain Homebrew installs put headers under /opt/homebrew/include/glm
    # without a glmConfig.cmake). Try find_package; fall back to FetchContent.
    find_package(glm QUIET)
    if(NOT TARGET glm AND NOT TARGET glm::glm)
        include(FetchContent)
        FetchContent_Declare(glm
            GIT_REPOSITORY https://github.com/g-truc/glm.git
            GIT_TAG        1.0.1
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(glm)
    endif()
endif()

if(NOT TARGET earcut_hpp)
    # earcut_hpp is header-only; many distros don't package it. Try
    # find_package first, then fall back to FetchContent.
    find_package(earcut_hpp QUIET)
    if(NOT TARGET earcut_hpp)
        include(FetchContent)
        # SOURCE_SUBDIR is a non-existent path on purpose: it tells
        # FetchContent_MakeAvailable to populate the sources but skip the
        # add_subdirectory() step. earcut.hpp v2.2.4 ships a CMakeLists with
        # a pre-3.5 cmake_minimum_required that current CMake refuses; since
        # earcut is header-only we just want the include directory anyway,
        # so bypassing its CMakeLists is the cleanest path.
        FetchContent_Declare(earcut_hpp
            GIT_REPOSITORY https://github.com/mapbox/earcut.hpp.git
            GIT_TAG        v2.2.4
            GIT_SHALLOW    TRUE
            SOURCE_SUBDIR  cmake-skip)
        FetchContent_MakeAvailable(earcut_hpp)
        add_library(earcut_hpp INTERFACE)
        target_include_directories(earcut_hpp INTERFACE
            $<BUILD_INTERFACE:${earcut_hpp_SOURCE_DIR}/include>)
    endif()
endif()

# ── Library target ───────────────────────────────────────────────────────────
add_library(latex3d STATIC
    src/log.cpp
    src/math_table.cpp
    src/text3d.cpp
    src/layouter.cpp
    src/generator.cpp
)
add_library(latex3d::latex3d ALIAS latex3d)

target_include_directories(latex3d
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Resolve the actual freetype target name (FetchContent uses `freetype`,
# system find_package uses `Freetype::Freetype`).
if(TARGET freetype)
    set(_latex3d_freetype freetype)
else()
    set(_latex3d_freetype Freetype::Freetype)
endif()

# Same for glm.
if(TARGET glm)
    set(_latex3d_glm glm)
else()
    set(_latex3d_glm glm::glm)
endif()

# All link deps are wrapped in $<BUILD_INTERFACE:…> so they're only on the
# command line during *this* build tree. The installed export set must not
# reference targets the embedder didn't install (FetchContent'd glm /
# earcut_hpp aren't in any export set), so for INSTALL_INTERFACE we leave
# the deps to be resolved by the consumer's build via find_dependency() in
# the installed Config file. Clients that build latex3d as a subdirectory
# (the in-tree case) don't need the install path and just see the BUILD
# side of the generator expression.
target_link_libraries(latex3d
    PUBLIC  $<BUILD_INTERFACE:${_latex3d_glm}>
    PRIVATE $<BUILD_INTERFACE:${_latex3d_freetype}>
            $<BUILD_INTERFACE:earcut_hpp>
)

target_compile_features(latex3d PUBLIC cxx_std_17)

# ── Tests ────────────────────────────────────────────────────────────────────
# LATEX3D_BUILD_TESTS is the explicit opt-in. enable_testing() is idempotent
# and works whether we're standalone or included as a subdirectory — in-tree
# the parent has already enabled testing at the root scope, but a redundant
# call here is harmless and ensures `ctest --test-dir build` works in the
# standalone case too.
if(LATEX3D_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Install rules (standalone-only) ──────────────────────────────────────────
# Skipped when this project is included as a subdirectory of a larger build,
# since the embedder owns its own install layout.
if(CMAKE_PROJECT_NAME STREQUAL "latex3d")
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS latex3d EXPORT latex3dTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT latex3dTargets
        FILE latex3dTargets.cmake
        NAMESPACE latex3d::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/latex3d)

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/latex3dConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/latex3dConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/latex3d)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/latex3dConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/latex3dConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/latex3dConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/latex3d)
endif()
