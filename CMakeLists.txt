cmake_minimum_required(VERSION 3.20)
project(dayssince VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(dayssince
    src/main.cpp
    src/tracker.cpp
    src/storage.cpp
)

target_include_directories(dayssince PRIVATE include)
target_link_libraries(dayssince PRIVATE nlohmann_json::nlohmann_json)

install(TARGETS dayssince RUNTIME DESTINATION bin)
