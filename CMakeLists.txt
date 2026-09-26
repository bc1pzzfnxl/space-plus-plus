cmake_minimum_required(VERSION 3.20)
project(space_plus_plus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# SDL2: pkg-config on Unix and MinGW, CMake package (vcpkg) on MSVC Windows.
if(WIN32 AND NOT MINGW)
  find_package(SDL2 CONFIG REQUIRED)
  set(SDL2_LIBS SDL2::SDL2)
  if(TARGET SDL2::SDL2main)
    list(APPEND SDL2_LIBS SDL2::SDL2main)
  endif()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  set(SDL2_LIBS PkgConfig::SDL2)
endif()

find_package(GLEW REQUIRED)
find_package(OpenGL REQUIRED)
find_package(glm CONFIG QUIET)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC ${SDL2_LIBS})

add_executable(blackhole src/main.cpp)
# Optional preset: flags used when the app is launched without arguments,
# e.g. -DBLACKHOLE_DEFAULT_ARGS="--polish --anim" (see README build variants).
set(BLACKHOLE_DEFAULT_ARGS "" CACHE STRING "CLI flags used when none are given")
if(BLACKHOLE_DEFAULT_ARGS)
  target_compile_definitions(blackhole PRIVATE
    BLACKHOLE_DEFAULT_ARGS="${BLACKHOLE_DEFAULT_ARGS}")
endif()
if(MSVC)
  target_compile_options(blackhole PRIVATE /W4)
else()
  target_compile_options(blackhole PRIVATE -Wall -Wextra)
endif()
target_link_libraries(blackhole PRIVATE
  imgui GLEW::GLEW OpenGL::GL ${SDL2_LIBS})
if(glm_FOUND)
  target_link_libraries(blackhole PRIVATE glm::glm)
endif()
