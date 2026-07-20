#include "test_harness.hpp"
#include "temp_dir.hpp"
#include "filelock.hpp"

#include <optional>
#include <stdexcept>

// These use flock across *separate open descriptions* of the same lock file. On
// Unix that contends even within one process, so contention is fully
// deterministic here — no sleeps or timing.

TEST(filelock_acquire_and_release) {
    TempDir d;
    auto lp = FileLock::lock_path_for(d.data());
    {
        FileLock lock(lp);            // blocking acquire succeeds on a free lock
        // While held, a non-blocking acquire from a second description must fail.
        CHECK(!FileLock::try_acquire(lp).has_value());
    }
    // After the scope, the lock is released and can be taken again.
    CHECK(FileLock::try_acquire(lp).has_value());
}

TEST(filelock_try_acquire_reports_contention) {
    TempDir d;
    auto lp = FileLock::lock_path_for(d.data());
    auto first = FileLock::try_acquire(lp);
    CHECK(first.has_value());
    auto second = FileLock::try_acquire(lp);
    CHECK(!second.has_value());       // contended -> nullopt, not an exception
}

TEST(filelock_released_after_exception) {
    TempDir d;
    auto lp = FileLock::lock_path_for(d.data());
    try {
        FileLock lock(lp);
        throw std::runtime_error("boom");   // unwind while holding the lock
    } catch (const std::runtime_error&) {
        // swallowed
    }
    // RAII must have released the lock during unwinding.
    CHECK(FileLock::try_acquire(lp).has_value());
}

TEST(filelock_move_transfers_ownership) {
    TempDir d;
    auto lp = FileLock::lock_path_for(d.data());
    std::optional<FileLock> held;
    {
        FileLock lock(lp);
        held.emplace(std::move(lock)); // ownership moves out of the scope
    }
    // Still held by `held`, so contention persists past the inner scope.
    CHECK(!FileLock::try_acquire(lp).has_value());
    held.reset();
    CHECK(FileLock::try_acquire(lp).has_value());
}
