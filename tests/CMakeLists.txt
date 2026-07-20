# One test binary aggregating every suite; CTest registers granular cases
# against it by passing a name-substring filter. Tests link only overdue_core
# (no httplib) and exercise the extracted helpers and domain/storage behavior in
# isolated temp directories — they never touch the user's real config or data.
add_executable(overdue_tests
    test_main.cpp
    webutil_tests.cpp
    storage_tests.cpp
    tracker_tests.cpp
    filelock_tests.cpp
)
target_link_libraries(overdue_tests PRIVATE overdue_core)
target_include_directories(overdue_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(overdue_tests PRIVATE ${OVERDUE_WARNINGS})

# Each add_test runs the shared binary filtered to one area, so a failure points
# straight at the affected concern in CTest output.
foreach(suite webutil storage tracker filelock)
    add_test(NAME ${suite} COMMAND overdue_tests ${suite})
endforeach()
