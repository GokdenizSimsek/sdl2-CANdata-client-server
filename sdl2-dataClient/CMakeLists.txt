cmake_minimum_required(VERSION 3.10)

project(sdl2-dataClient)

# Find SDL2
find_package(SDL2 CONFIG REQUIRED)

# Find Poco
find_package(Poco REQUIRED COMPONENTS Net Util Foundation)

# Create executable
add_executable(sdl2-dataClient sdl2-dataClient.cpp)

# Link libraries
target_link_libraries(sdl2-dataClient PRIVATE SDL2::SDL2 SDL2::SDL2main Poco::Net Poco::Util Poco::Foundation)

# Set C++ standard
set_target_properties(sdl2-dataClient PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# Add compile options
add_compile_options("/utf-8")
