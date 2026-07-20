#pragma once
#include "activity.hpp"
#include <filesystem>
#include <stdexcept>
#include <vector>

// Thrown when a save would overwrite data that another process changed after we
// loaded it (optimistic-concurrency conflict). Carries both revisions so the
// caller can report a clear, specific error instead of silently clobbering.
class StaleWriteError : public std::runtime_error {
public:
    StaleWriteError(long long expected, long long found)
        : std::runtime_error(
              "Data changed on disk since it was loaded (expected revision " +
              std::to_string(expected) + ", found " + std::to_string(found) +
              ") — refusing to overwrite newer data. Reload and retry."),
          expected_revision(expected), found_revision(found) {}
    long long expected_revision;
    long long found_revision;
};

// Result of a load: the activities plus the file's revision. Legacy files with
// no revision metadata load with revision 0, which is also the revision of an
// absent file, so a first save cleanly upgrades them.
struct DataFile {
    std::vector<Activity> activities;
    long long revision = 0;
    // True when the data was recovered rather than read cleanly: some entries
    // were skipped, the envelope metadata was malformed, or the file came from a
    // newer format version. The next write should back up the original first.
    bool repaired = false;
};

class Storage {
public:
    // Load activities and the current revision. Throws on an unparseable file so
    // a later save cannot turn recoverable corruption into total data loss.
    static DataFile load(const std::filesystem::path& path);

    // Atomic, cross-process-safe, revision-checked commit.
    //  * Acquires the advisory file lock for the whole check-and-write.
    //  * Fails with StaleWriteError if the on-disk revision no longer matches
    //    `base_revision` (someone else wrote in the meantime).
    //  * If `backup_original` is set, copies the existing file to "<path>.bak"
    //    before overwriting it (used when the loaded data was repaired), and
    //    fails rather than overwrite if that backup can't be made.
    //  * Otherwise writes the data with revision `base_revision + 1` via a temp
    //    file + rename, and returns the new revision.
    static long long save(const std::filesystem::path& path,
                          const std::vector<Activity>& activities,
                          long long base_revision, bool backup_original = false);

    // The current on-disk revision without loading everything (0 if the file is
    // absent or legacy). Throws if the file exists but cannot be parsed.
    static long long current_revision(const std::filesystem::path& path);
};
