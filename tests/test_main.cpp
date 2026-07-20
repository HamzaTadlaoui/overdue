#include "test_harness.hpp"

// Single entry point for the whole suite. An optional substring argument runs
// only the matching tests, which is how CTest registers granular test cases
// against this one binary.
int main(int argc, char** argv) {
    return th::run_all(argc > 1 ? argv[1] : "");
}
