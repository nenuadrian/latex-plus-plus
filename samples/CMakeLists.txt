# ── latex3d sample renderer ──────────────────────────────────────────────────
# Tiny tool that turns the library's MeshData into PNG thumbnails so the
# README can showcase what the engine produces. Off-by-default — opt in with
# -DLATEX3D_BUILD_SAMPLES=ON, then build the `latex3d_samples` target to
# regenerate everything in docs/samples/.

include(FetchContent)

# stb_image_write — public-domain single-header PNG writer.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake-skip)
FetchContent_MakeAvailable(stb)

add_executable(latex3d_render_samples render_samples.cpp)
target_include_directories(latex3d_render_samples PRIVATE ${stb_SOURCE_DIR})
target_link_libraries(latex3d_render_samples PRIVATE latex3d)
# stb_image_write triggers some -Wmissing-field-initializers warnings on
# strict toolchains; suppress only on the sample TU.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(latex3d_render_samples PRIVATE
        -Wno-missing-field-initializers)
endif()

# ── Math font ────────────────────────────────────────────────────────────────
# Required: pass a math .otf via -DLATEX3D_SAMPLES_FONT=/path/to/font.otf.
# We deliberately do NOT auto-download a font: every candidate (Fira Math,
# Latin Modern Math, STIX Two Math) ships under different release artefacts
# and tag conventions, and a wrong guess gives an opaque clone failure that
# wastes more time than just picking a font once.
#
# Sources (any one works — they all carry an OpenType MATH table):
#   • Fira Math:        https://github.com/firamath/firamath/releases
#   • STIX Two Math:    https://github.com/stipub/stixfonts/raw/master/fonts/static_otf/STIXTwoMath-Regular.otf
#   • Latin Modern:     https://www.gust.org.pl/projects/e-foundry/lm-math
#
# If you're working in the Hades tree, the in-tree font path is:
#   data/fonts/FiraMath-Regular.otf

set(_samples_out_dir "${CMAKE_CURRENT_SOURCE_DIR}/../docs/samples")
file(MAKE_DIRECTORY "${_samples_out_dir}")

if(NOT LATEX3D_SAMPLES_FONT)
    message(STATUS "latex3d samples: LATEX3D_SAMPLES_FONT not set — "
                   "`latex3d_samples` target NOT created. Re-configure with "
                   "-DLATEX3D_SAMPLES_FONT=/path/to/math.otf to enable it "
                   "(font sources listed in samples/CMakeLists.txt).")
elseif(NOT EXISTS "${LATEX3D_SAMPLES_FONT}")
    message(FATAL_ERROR "latex3d samples: LATEX3D_SAMPLES_FONT="
                        "${LATEX3D_SAMPLES_FONT} does not exist on disk. "
                        "Pass an absolute path to a real .otf/.ttf file.")
else()
    message(STATUS "latex3d samples: using ${LATEX3D_SAMPLES_FONT}")
    add_custom_target(latex3d_samples
        COMMAND $<TARGET_FILE:latex3d_render_samples>
                "${LATEX3D_SAMPLES_FONT}"
                "${_samples_out_dir}"
        DEPENDS latex3d_render_samples
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        COMMENT "Rendering latex3d sample PNGs into docs/samples/"
        VERBATIM)
endif()
