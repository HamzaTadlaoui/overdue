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

// --- Envelope detection + best-effort recovery -----------------------------

TEST(storage_envelope_activities_only_loads) {
    TempDir d;
    // An envelope identified solely by "activities" (no version/revision) is
    // still a valid envelope and loads cleanly at revision 0.
    write_file(d.data(),
        R"({"activities":[{"name":"run","type":"habit","logs":[1700000000]}]})");
    DataFile df = Storage::load(d.data());
    CHECK_EQ(df.revision, 0LL);
    CHECK(!df.repaired);
    CHECK_EQ(df.activities.size(), size_t{1});
    CHECK_EQ(df.activities[0].name, std::string("run"));
}

TEST(storage_recovers_with_malformed_metadata) {
    TempDir d;
    // version/revision are null (malformed metadata) but the activities are fine:
    // recover them, default revision to 0, and flag the load as repaired.
    write_file(d.data(),
        R"({"version":null,"revision":null,"activities":[{"name":"run","type":"habit","logs":[1700000000]}]})");
    DataFile df = Storage::load(d.data());
    CHECK_EQ(df.activities.size(), size_t{1});
    CHECK_EQ(df.activities[0].name, std::string("run"));
    CHECK_EQ(df.revision, 0LL);
    CHECK(df.repaired);           // metadata was malformed -> repaired
}

TEST(storage_recovers_with_wrong_metadata_types) {
    TempDir d;
    // version is a string, revision is a string number: tolerate both, recovering
    // the revision value where possible, and flag repaired.
    write_file(d.data(),
        R"({"version":"two","revision":"7","activities":[{"name":"run","type":"habit","logs":[1700000000]}]})");
    DataFile df = Storage::load(d.data());
    CHECK_EQ(df.activities.size(), size_t{1});
    CHECK_EQ(df.revision, 7LL);   // "7" recovered tolerantly
    CHECK(df.repaired);
}

TEST(storage_negative_revision_defaults_and_repairs) {
    TempDir d;
    write_file(d.data(),
        R"({"version":2,"revision":-3,"activities":[{"name":"run","type":"habit","logs":[1700000000]}]})");
    DataFile df = Storage::load(d.data());
    CHECK_EQ(df.activities.size(), size_t{1});
    CHECK_EQ(df.revision, 0LL);   // negative -> default 0
    CHECK(df.repaired);
}

TEST(storage_partial_recovery_skips_invalid) {
    TempDir d;
    // Two valid activities among invalid ones (no name, no logs, non-object).
    // The valid ones are recovered; the rest skipped; the load is flagged repaired.
    write_file(d.data(), R"({
        "version":2, "revision":4,
        "activities":[
            {"name":"good1","type":"habit","logs":[1700000000]},
            {"type":"habit","logs":[1700000000]},
            {"name":"noLogs","type":"habit","logs":[]},
            42,
            {"name":"good2","type":"task","logs":[1700000100]}
        ]
    })");
    DataFile df = Storage::load(d.data());
    CHECK_EQ(df.revision, 4LL);
    CHECK_EQ(df.activities.size(), size_t{2});
    CHECK_EQ(df.activities[0].name, std::string("good1"));
    CHECK_EQ(df.activities[1].name, std::string("good2"));
    CHECK(df.repaired);           // entries were skipped
}

TEST(storage_future_version_recovers_activities) {
    TempDir d;
    // A newer format version whose activities structure is still understandable:
    // read the activities conservatively, keep the revision, flag repaired so the
    // original is backed up before any rewrite (no silent downgrade).
    write_file(d.data(),
        R"({"version":99,"revision":5,"activities":[{"name":"run","type":"habit","logs":[1700000000]}]})");
    DataFile df = Storage::load(d.data());
    CHECK_EQ(df.activities.size(), size_t{1});
    CHECK_EQ(df.activities[0].name, std::string("run"));
    CHECK_EQ(df.revision, 5LL);
    CHECK(df.repaired);
}

// --- Hard failures: nothing recoverable, never overwritten -----------------

TEST(storage_envelope_activities_not_array_throws) {
    TempDir d;
    // Envelope markers present but "activities" is not an array: there is no data
    // to recover, so refuse (and thereby preserve the file) rather than treat it
    // as a legacy single activity.
    write_file(d.data(), R"({"version":2,"revision":1,"activities":"nope"})");
    CHECK_THROWS(Storage::load(d.data()), std::runtime_error);
}

TEST(storage_envelope_marker_without_activities_throws) {
    TempDir d;
    // A "revision" marker with no activities array yields nothing recoverable.
    write_file(d.data(), R"({"revision":5})");
    CHECK_THROWS(Storage::load(d.data()), std::runtime_error);
}

TEST(storage_unrecoverable_envelope_not_overwritten) {
    TempDir d;
    // Structurally invalid (but syntactically valid JSON) envelope: current_revision
    // re-parses it, throws, and aborts the save, leaving the file untouched.
    const std::string bad = R"({"version":2,"activities":{"not":"an array"}})";
    write_file(d.data(), bad);
    CHECK_THROWS(Storage::current_revision(d.data()), std::runtime_error);
    CHECK_THROWS(Storage::save(d.data(), one("run"), 0), std::runtime_error);
    std::ifstream f(d.data());
    std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK_EQ(got, bad);
}

// --- Backup before rewriting recovered data --------------------------------

TEST(storage_backup_before_rewrite_preserves_original) {
    TempDir d;
    const std::string orig =
        R"({"version":2,"revision":3,"activities":[{"name":"old","type":"habit","logs":[1700000000]}]})";
    write_file(d.data(), orig);
    DataFile df = Storage::load(d.data());

    // Explicitly request a backup on save (what Tracker does for repaired loads).
    long long rev = Storage::save(d.data(), one("new"), df.revision, /*backup=*/true);
    CHECK_EQ(rev, 4LL);

    // The backup holds the exact original bytes.
    auto bak = d.data(); bak += ".bak";
    CHECK(std::filesystem::exists(bak));
    std::ifstream bf(bak);
    std::string backed((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
    CHECK_EQ(backed, orig);

    // The live file now holds the new, upgraded data.
    DataFile after = Storage::load(d.data());
    CHECK_EQ(after.activities.size(), size_t{1});
    CHECK_EQ(after.activities[0].name, std::string("new"));
    CHECK_EQ(after.revision, 4LL);
}

TEST(storage_future_version_backed_up_then_upgraded) {
    TempDir d;
    const std::string future = R"({"version":99,"revision":2,"activities":[{"name":"run","type":"habit","logs":[1700000000]}]})";
    write_file(d.data(), future);
    DataFile df = Storage::load(d.data());
    CHECK(df.repaired);                 // future version flags repair
    CHECK_EQ(df.revision, 2LL);

    // Saving recovered data from a future file backs up the original (no silent
    // downgrade) and then writes our format.
    long long rev = Storage::save(d.data(), df.activities, df.revision, /*backup=*/true);
    CHECK_EQ(rev, 3LL);
    auto bak = d.data(); bak += ".bak";
    std::ifstream bf(bak);
    std::string backed((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
    CHECK_EQ(backed, future);           // newer-format original preserved verbatim
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
