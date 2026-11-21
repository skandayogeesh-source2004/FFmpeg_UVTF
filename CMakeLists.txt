
cmake_minimum_required(VERSION 3.20)

project(FFmpeg_UVTF LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(FFMPEG REQUIRED)

add_executable(uvtf
    app/main.cpp
    input/FileInput.cpp
    input/RTPInput.cpp
    input/SyntheticInput.cpp
    output/RTPOutput.cpp
    rendering/D3D11Renderer.cpp
)

target_include_directories(uvtf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/input"
    "${CMAKE_CURRENT_SOURCE_DIR}/output"
    "${CMAKE_CURRENT_SOURCE_DIR}/rendering"
    ${FFMPEG_INCLUDE_DIRS}
)

target_link_directories(uvtf PRIVATE
    ${FFMPEG_LIBRARY_DIRS}
)

target_link_libraries(uvtf PRIVATE
    ${FFMPEG_LIBRARIES}
    Threads::Threads
    d3d11
    dxgi
    user32
)

if(MSVC)
    target_compile_options(uvtf PRIVATE
        /W4
        /permissive-
    )
endif()
