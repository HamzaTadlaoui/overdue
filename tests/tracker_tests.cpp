#include "test_harness.hpp"
#include "temp_dir.hpp"
#include "tracker.hpp"
#include "storage.hpp"

#include <string>

// Exercises the full Tracker mutation path (load-modify-save with the advisory
// lock and revision check), plus the tag domain operations the web reuses.

// --- Tag domain behavior (shared by CLI and web) ---------------------------

TEST(tracker_addtag_normalizes_and_dedupes) {
    TempDir d;
    Tracker t{d.data()};
    CHECK(t.add("run"));
    CHECK(t.addtag("run", "  Health "));
    CHECK(t.addtag("run", "health"));   // duplicate after normalization
    CHECK(t.addtag("run", "Morning"));
    auto a = t.find("run");
    CHECK(a.has_value());
    // Stored lowercased, de-duplicated and sorted.
    CHECK_EQ(a->tags.size(), size_t{2});
    CHECK_EQ(a->tags[0], std::string("health"));
    CHECK_EQ(a->tags[1], std::string("morning"));
}

TEST(tracker_deltag_removes_normalized) {
    TempDir d;
    Tracker t{d.data()};
    CHECK(t.add("run", std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                {"health", "morning"}));
    CHECK(t.deltag("run", " HEALTH "));         // matches after normalization
    CHECK(!t.deltag("run", "absent"));          // not present -> false
    auto a = t.find("run");
    CHECK_EQ(a->tags.size(), size_t{1});
    CHECK_EQ(a->tags[0], std::string("morning"));
}

TEST(tracker_tags_persist_across_reload) {
    TempDir d;
    {
        Tracker t{d.data()};
        CHECK(t.add("run"));
        CHECK(t.addtag("run", "Fitness"));
    }
    // A fresh Tracker (fresh load) must see the persisted, normalized tag.
    Tracker t2{d.data()};
    auto a = t2.find("run");
    CHECK(a.has_value());
    CHECK_EQ(a->tags.size(), size_t{1});
    CHECK_EQ(a->tags[0], std::string("fitness"));
}

TEST(tracker_tag_filter_matches_any) {
    TempDir d;
    Tracker t{d.data()};
    t.add("a", std::nullopt, std::nullopt, std::nullopt, std::nullopt, {"work"});
    t.add("b", std::nullopt, std::nullopt, std::nullopt, std::nullopt, {"home"});
    t.add("c", std::nullopt, std::nullopt, std::nullopt, std::nullopt, {"work", "urgent"});
    int matches = 0;
    for (const auto& act : t.all())
        if (has_tag(act, "work") || has_tag(act, "urgent")) ++matches;
    CHECK_EQ(matches, 2);   // a and c
}

// --- Cross-process-style concurrency ---------------------------------------

TEST(tracker_two_writers_same_base_conflict) {
    TempDir d;
    // Seed the file so both trackers load the same starting revision.
    { Tracker seed{d.data()}; CHECK(seed.add("run")); }

    // Two independent Trackers model two processes: each loads the same revision.
    Tracker a{d.data()};
    Tracker b{d.data()};

    // A commits first.
    CHECK(a.log("run"));
    // B is now based on a stale revision; its save must raise a conflict rather
    // than silently erase A's log.
    CHECK_THROWS(b.log("run"), StaleWriteError);

    // A's mutation survived: reload and count logs (seed + A's log = 2).
    Tracker check{d.data()};
    auto ra = check.find("run");
    CHECK(ra.has_value());
    CHECK_EQ(ra->logs.size(), size_t{2});
}

TEST(tracker_lock_released_after_conflict) {
    TempDir d;
    { Tracker seed{d.data()}; CHECK(seed.add("run")); }
    Tracker a{d.data()};
    Tracker b{d.data()};
    CHECK(a.log("run"));                       // rev advances
    CHECK_THROWS(b.log("run"), StaleWriteError); // b conflicts and throws

    // If the conflict had leaked the advisory lock, this fresh writer would
    // block forever; instead it acquires cleanly and commits.
    Tracker c{d.data()};
    CHECK(c.log("run"));
    Tracker check{d.data()};
    CHECK_EQ(check.find("run")->logs.size(), size_t{3});
}

TEST(tracker_sequential_saves_advance) {
    TempDir d;
    Tracker t{d.data()};
    CHECK(t.add("run"));      // rev 1
    CHECK(t.log("run"));      // rev 2
    CHECK(t.log("run"));      // rev 3
    // Same instance keeps committing without conflict because it tracks its own
    // advancing revision.
    CHECK_EQ(Storage::current_revision(d.data()), 3LL);
}
