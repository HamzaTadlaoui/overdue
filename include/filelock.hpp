#pragma once
// Advisory cross-process file lock (Unix/Linux, flock(2)).
//
// A mutation to data.json is a load-modify-save transaction that can race with
// a second overdue process (a CLI command, the web server, a second server).
// An in-process mutex cannot see those other processes; an advisory lock on a
// sibling lock file can. The lock is held on a stable, never-renamed lock file
// (data.json.lock) rather than on data.json itself, because the atomic save
// replaces data.json's inode via rename and a lock tied to the old inode would
// not be seen by a writer that opened the new one.
//
// The lock is advisory: it only coordinates code that also takes it (all of
// overdue's write paths do). It is released automatically when the descriptor
// is closed, including on crash, so a killed process never leaves it stuck.

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

// Exclusive advisory lock, released by RAII. Move-only.
class FileLock {
public:
    // Blocking acquire: waits until the lock is free. Throws std::runtime_error
    // (a clear failure, never a silent continue) if the lock file cannot be
    // opened or flock fails for any reason other than an interrupting signal.
    explicit FileLock(const std::filesystem::path& lock_path) {
        fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ < 0)
            throw std::runtime_error("Cannot open lock file " + lock_path.string() +
                                     ": " + std::strerror(errno));
        int rc;
        do { rc = ::flock(fd_, LOCK_EX); } while (rc < 0 && errno == EINTR);
        if (rc < 0) {
            int e = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("Cannot lock " + lock_path.string() + ": " +
                                     std::strerror(e));
        }
    }

    // Non-blocking acquire used by tests and by callers that must not stall:
    // returns nullopt when the lock is currently held by someone else.
    static std::optional<FileLock> try_acquire(const std::filesystem::path& lock_path) {
        int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0)
            throw std::runtime_error("Cannot open lock file " + lock_path.string() +
                                     ": " + std::strerror(errno));
        int rc;
        do { rc = ::flock(fd, LOCK_EX | LOCK_NB); } while (rc < 0 && errno == EINTR);
        if (rc < 0) {
            int e = errno;
            ::close(fd);
            if (e == EWOULDBLOCK) return std::nullopt;
            throw std::runtime_error("Cannot lock " + lock_path.string() + ": " +
                                     std::strerror(e));
        }
        return FileLock(fd);
    }

    ~FileLock() { reset(); }

    FileLock(FileLock&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    FileLock& operator=(FileLock&& o) noexcept {
        if (this != &o) { reset(); fd_ = o.fd_; o.fd_ = -1; }
        return *this;
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    // Conventional sibling lock path for a data file: "<data>.lock".
    static std::filesystem::path lock_path_for(const std::filesystem::path& data_path) {
        std::filesystem::path p = data_path;
        p += ".lock";
        return p;
    }

private:
    explicit FileLock(int fd) : fd_(fd) {}
    void reset() {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
            fd_ = -1;
        }
    }
    int fd_ = -1;
};
