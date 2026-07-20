cmake_minimum_required(VERSION 3.20)
project(overdue VERSION 0.1.0 LANGUAGES CXX)

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

set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE   OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(httplib)

# Warning flags applied only to project-owned targets (never the fetched
# dependencies). Not treated as errors, so third-party headers can't break us.
set(OVERDUE_WARNINGS -Wall -Wextra -Wpedantic)

# Opt-in AddressSanitizer + UndefinedBehaviorSanitizer build. Applied to project
# targets defined below (the header-only dependencies compile nothing). Requires
# the sanitizer runtime libraries to be installed; the CI sanitize job uses it.
option(OVERDUE_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(OVERDUE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# Domain + storage core, shared by the CLI/web binary and the test binary. Kept
# free of httplib so the tests don't have to pull in the web server.
add_library(overdue_core STATIC
    src/tracker.cpp
    src/storage.cpp
    src/config.cpp
)
target_include_directories(overdue_core PUBLIC include)
target_link_libraries(overdue_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(overdue_core PRIVATE ${OVERDUE_WARNINGS})

add_executable(overdue
    src/main.cpp
    src/web.cpp
)
target_link_libraries(overdue PRIVATE overdue_core httplib::httplib)
target_compile_options(overdue PRIVATE ${OVERDUE_WARNINGS})

install(TARGETS overdue RUNTIME DESTINATION bin)

enable_testing()
add_subdirectory(tests)
