#include "test_harness.hpp"
#include "temp_dir.hpp"
#include "storage.hpp"

#include <fstream>
#include <iterator>
#include <string>

// Build a minimal one-activity vector for save tests.
static std::vector<Activity> one(const std::string& name) {
    Activity a;
    a.name = name;
    a.type = ActivityType::Habit;
    a.logs = {LogEntry{now(), std::nullopt}};
    return {a};
}

static void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p);
    f << contents;
}

// --- Envelope round-trip + revision advance --------------------------------

TEST(storage_absent_file_loads_empty_rev0) {
    TempDir d;
    DataFile df = Storage::load(d.data());
    CHECK(df.activities.empty());
    CHECK_EQ(df.revision, 0LL);
}

TEST(storage_first_save_writes_revision_1) {
    TempDir d;
    long long rev = Storage::save(d.data(), one("run"), /*base=*/0);
    CHECK_EQ(rev, 1LL);
    DataFile df = Storage::load(d.data());
    CHECK_EQ(df.revision, 1LL);
    CHECK_EQ(df.activities.size(), size_t{1});
    CHECK_EQ(df.activities[0].name, std::string("run"));
}

TEST(storage_each_save_advances_revision) {
    TempDir d;
    long long r1 = Storage::save(d.data(), one("run"), 0);
    long long r2 = Storage::save(d.data(), one("run"), r1);
    long long r3 = Storage::save(d.data(), one("run"), r2);
    CHECK_EQ(r1, 1LL);
    CHECK_EQ(r2, 2LL);
    CHECK_EQ(r3, 3LL);
    CHECK_EQ(Storage::current_revision(d.data()), 3LL);
}

// --- Stale-write conflict detection ----------------------------------------

TEST(storage_stale_write_conflicts_not_both_win) {
    TempDir d;
    // Establish revision 1 that two writers both load.
    Storage::save(d.data(), one("run"), 0);
    long long baseA = Storage::load(d.data()).revision; // 1
    long long baseB = Storage::load(d.data()).revision; // 1

    // Writer A commits first, based on revision 1 -> becomes 2.
    long long afterA = Storage::save(d.data(), one("run-A"), baseA);
    CHECK_EQ(afterA, 2LL);

    // Writer B, still based on the same original revision 1, must be rejected
    // rather than silently overwrite A's newer data.
    CHECK_THROWS(Storage::save(d.data(), one("run-B"), baseB), StaleWriteError);

    // A's write survived; B did not clobber it.
    DataFile df = Storage::load(d.data());
    CHECK_EQ(df.activities.size(), size_t{1});
    CHECK_EQ(df.activities[0].name, std::string("run-A"));
    CHECK_EQ(df.revision, 2LL);
}

TEST(storage_stale_writer_can_retry_after_reload) {
    TempDir d;
    Storage::save(d.data(), one("run"), 0);        // rev 1
    Storage::save(d.data(), one("run2"), 1);       // rev 2 (someone else wrote)
    CHECK_THROWS(Storage::save(d.data(), one("mine"), 1), StaleWriteError);
    // Reload to the current revision, then the retry succeeds.
    long long base = Storage::load(d.data()).revision;
    long long rev = Storage::save(d.data(), one("mine"), base);
    CHECK_EQ(rev, 3LL);
}

// --- Legacy compatibility ---------------------------------------------------

TEST(storage_loads_legacy_bare_array) {
    TempDir d;
    // Original format: a top-level array, logs as bare unix numbers, no revision.
    write_file(d.data(),
        R"([{"name":"floss","type":"habit","logs":[1700000000,1700086400],"tags":["Dental"]}])");
    DataFile df = Storage::load(d.data());
    CHECK_EQ(df.revision, 0LL);
    CHECK_EQ(df.activities.size(), size_t{1});
    CHECK_EQ(df.activities[0].name, std::string("floss"));
    CHECK_EQ(df.activities[0].logs.size(), size_t{2});
    // Tags normalized on load.
    CHECK_EQ(df.activities[0].tags.size(), size_t{1});
    CHECK_EQ(df.activities[0].tags[0], std::string("dental"));
}

TEST(storage_loads_legacy_single_object) {
    TempDir d;
    write_file(d.data(), R"({"name":"solo","type":"habit","logs":[1700000000]})");
    DataFile df = Storage::load(d.data());
    CHECK_EQ(df.activities.size(), size_t{1});
    CHECK_EQ(df.activities[0].name, std::string("solo"));
    CHECK_EQ(df.revision, 0LL);
}

TEST(storage_legacy_upgrades_to_envelope_on_save) {
    TempDir d;
    write_file(d.data(),
        R"([{"name":"floss","type":"habit","logs":[1700000000]}])");
    DataFile df = Storage::load(d.data());
    CHECK_EQ(df.revision, 0LL);
    // First save from the legacy base (0) upgrades cleanly to revision 1.
    long long rev = Storage::save(d.data(), df.activities, df.revision);
    CHECK_EQ(rev, 1LL);
    // Re-read confirms the envelope now carries a revision.
    CHECK_EQ(Storage::current_revision(d.data()), 1LL);
}

// --- Corrupt-file preservation ----------------------------------------------

TEST(storage_corrupt_file_throws_on_load) {
    TempDir d;
    write_file(d.data(), "{ this is not valid json ][");
    CHECK_THROWS(Storage::load(d.data()), std::runtime_error);
}

TEST(storage_corrupt_file_not_overwritten) {
    TempDir d;
    const std::string corrupt = "{ not valid json ][";
    write_file(d.data(), corrupt);
    // A save attempt re-reads the revision first; a corrupt file must abort the
    // save (throw) rather than clobber it.
    CHECK_THROWS(Storage::save(d.data(), one("run"), 0), std::runtime_error);
    // The original corrupt bytes are still on disk, untouched.
    std::ifstream f(d.data());
    std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK_EQ(got, corrupt);
}

// --- Strict envelope validation --------------------------------------------

TEST(storage_envelope_activities_only_loads) {
    TempDir d;
    // An envelope identified solely by "activities" (no version/revision) is
    // still a valid envelope and loads at revision 0.
    write_file(d.data(),
        R"({"activities":[{"name":"run","type":"habit","logs":[1700000000]}]})");
    DataFile df = Storage::load(d.data());
    CHECK_EQ(df.revision, 0LL);
    CHECK_EQ(df.activities.size(), size_t{1});
    CHECK_EQ(df.activities[0].name, std::string("run"));
}

TEST(storage_malformed_envelope_activities_not_array_throws) {
    TempDir d;
    // Envelope markers present but "activities" is not an array: must be rejected
    // as a malformed envelope, never reinterpreted as a legacy single activity.
    write_file(d.data(), R"({"version":2,"revision":1,"activities":"nope"})");
    CHECK_THROWS(Storage::load(d.data()), std::runtime_error);
}

TEST(storage_envelope_marker_without_activities_throws) {
    TempDir d;
    // A "revision" marker with no activities array is a broken envelope, not a
    // legacy activity object named nothing.
    write_file(d.data(), R"({"revision":5})");
    CHECK_THROWS(Storage::load(d.data()), std::runtime_error);
}

TEST(storage_unsupported_future_version_rejected) {
    TempDir d;
    write_file(d.data(), R"({"version":999,"revision":1,"activities":[]})");
    CHECK_THROWS(Storage::load(d.data()), std::runtime_error);
}

TEST(storage_envelope_bad_version_type_throws) {
    TempDir d;
    write_file(d.data(), R"({"version":"two","activities":[]})");
    CHECK_THROWS(Storage::load(d.data()), std::runtime_error);
}

TEST(storage_envelope_bad_revision_type_throws) {
    TempDir d;
    write_file(d.data(), R"({"version":2,"revision":"x","activities":[]})");
    CHECK_THROWS(Storage::load(d.data()), std::runtime_error);
}

TEST(storage_envelope_negative_revision_throws) {
    TempDir d;
    write_file(d.data(), R"({"version":2,"revision":-3,"activities":[]})");
    CHECK_THROWS(Storage::load(d.data()), std::runtime_error);
}

TEST(storage_malformed_envelope_not_overwritten) {
    TempDir d;
    // A structurally invalid (but syntactically valid JSON) envelope must never
    // be clobbered by a save: current_revision re-parses it, throws, and aborts.
    const std::string bad = R"({"version":2,"activities":{"not":"an array"}})";
    write_file(d.data(), bad);
    CHECK_THROWS(Storage::current_revision(d.data()), std::runtime_error);
    CHECK_THROWS(Storage::save(d.data(), one("run"), 0), std::runtime_error);
    std::ifstream f(d.data());
    std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK_EQ(got, bad);
}

TEST(storage_future_version_not_overwritten) {
    TempDir d;
    const std::string future = R"({"version":999,"revision":2,"activities":[]})";
    write_file(d.data(), future);
    // Refuse to downgrade/overwrite data written by a newer version.
    CHECK_THROWS(Storage::save(d.data(), one("run"), 2), std::runtime_error);
    std::ifstream f(d.data());
    std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK_EQ(got, future);
}

// --- Adversarial values survive a round-trip -------------------------------

TEST(storage_preserves_adversarial_name_unit_tags) {
    TempDir d;
    Activity a;
    a.name = "A&B <script>alert(1)</script>";
    a.type = ActivityType::Habit;
    a.unit = "\" onmouseover=\"x";
    a.tags = {"café", "東京"};
    a.logs = {LogEntry{now(), 5.0}};
    Storage::save(d.data(), {a}, 0);

    DataFile df = Storage::load(d.data());
    CHECK_EQ(df.activities.size(), size_t{1});
    const Activity& b = df.activities[0];
    CHECK_EQ(b.name, a.name);
    CHECK(b.unit.has_value());
    CHECK_EQ(*b.unit, *a.unit);
    CHECK_EQ(b.tags.size(), size_t{2});
}
