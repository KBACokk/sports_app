cmake_minimum_required(VERSION 3.24)
project(sports_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW REQUIRED glfw3)

# Dear ImGui docking branch
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
)

# ImPlot
FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG master
)

# cpp-httplib
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG master
)

# nlohmann/json
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG master
)

# portable-file-dialogs
FetchContent_Declare(
    pfd
    GIT_REPOSITORY https://github.com/samhocevar/portable-file-dialogs.git
    GIT_TAG main
)

# stb
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
)

FetchContent_MakeAvailable(imgui implot httplib json pfd stb)

add_library(imgui_lib
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${GLFW_INCLUDE_DIRS}
)

target_link_libraries(imgui_lib PUBLIC
    ${GLFW_LIBRARIES}
    OpenGL::GL
)

add_library(implot_lib
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)

target_include_directories(implot_lib PUBLIC
    ${implot_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
)

target_link_libraries(implot_lib PUBLIC imgui_lib)

add_executable(sports_app
    src/main.cpp
)

target_include_directories(sports_app PRIVATE
    src
    ${httplib_SOURCE_DIR}
    ${json_SOURCE_DIR}/single_include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${implot_SOURCE_DIR}
    ${pfd_SOURCE_DIR}
    ${stb_SOURCE_DIR}
    ${GLFW_INCLUDE_DIRS}
)

target_link_libraries(sports_app PRIVATE
    imgui_lib
    implot_lib
    ${GLFW_LIBRARIES}
    OpenGL::GL
)
