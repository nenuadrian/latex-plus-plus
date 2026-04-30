# ── latex3d unit tests ──────────────────────────────────────────────────────
# Pin the LaTeX layouter contract: symbol substitution, fraction layout,
# script positioning, group balancing. Runs against a fake font-metrics
# provider so the test doesn't need FreeType or a TTF — pure layout logic.

if(NOT TARGET Catch2::Catch2WithMain)
    # Try a system-installed Catch2 v3 first; fall back to FetchContent so
    # `cmake -S libs/latex3d -B build` works on a fresh machine without
    # requiring the user to brew/apt install catch2 first.
    find_package(Catch2 3 QUIET)
    if(NOT TARGET Catch2::Catch2WithMain)
        include(FetchContent)
        FetchContent_Declare(Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG        v3.5.4
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(Catch2)
    endif()
endif()

add_executable(test_latex3d_layouter test_layouter.cpp)
target_link_libraries(test_latex3d_layouter PRIVATE
    latex3d
    Catch2::Catch2WithMain)

add_test(NAME Latex3DLayouter COMMAND test_latex3d_layouter)
