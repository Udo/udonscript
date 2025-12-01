cmake_minimum_required(VERSION 3.15)
project(udonscript VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/tmp)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/tmp)

# For multi-config generators (like Visual Studio)
foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_SOURCE_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_SOURCE_DIR}/tmp)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_SOURCE_DIR}/tmp)
endforeach()

# Core library sources
set(UDONSCRIPT_CORE_SOURCES
    src/core/types.cpp
    src/core/udonscript.cpp
    src/core/udonscript-builtins.cpp
    src/core/udonscript-helpers.cpp
)

# Create static library
add_library(udonscript_core STATIC ${UDONSCRIPT_CORE_SOURCES})
target_include_directories(udonscript_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

# Set intermediate files to go to tmp/
set_target_properties(udonscript_core PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/tmp
)

# Find all program files in src/programs/
file(GLOB PROGRAM_SOURCES ${CMAKE_SOURCE_DIR}/src/programs/*.cpp)

# Create an executable for each program
foreach(PROGRAM_SOURCE ${PROGRAM_SOURCES})
    get_filename_component(PROGRAM_NAME ${PROGRAM_SOURCE} NAME_WE)
    add_executable(${PROGRAM_NAME} ${PROGRAM_SOURCE})
    target_link_libraries(${PROGRAM_NAME} PRIVATE udonscript_core)
    target_include_directories(${PROGRAM_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    
    # Set output directory for executables
    set_target_properties(${PROGRAM_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
    )
endforeach()

# Compiler warnings
if(MSVC)
    target_compile_options(udonscript_core PRIVATE /W4)
else()
    target_compile_options(udonscript_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
