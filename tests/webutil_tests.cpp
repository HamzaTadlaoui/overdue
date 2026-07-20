#include "test_harness.hpp"
#include "webutil.hpp"

#include <string>

using namespace webutil;

// The adversarial values from the task brief, reused across cases.
static const std::string kScript = "<script>alert(1)</script>";
static const std::string kAttrBreak = "\" onmouseover=\"alert(1)";
static const std::string kAmp = "A&B";
static const std::string kUnicode = "café / 東京";
static const std::string kPunct = "< > & \" ' / ? = %";

// --- HTML text-node escaping ----------------------------------------------

TEST(webutil_text_escapes_markup) {
    std::string out = esc_text(kScript);
    CHECK(out.find("<script>") == std::string::npos);
    CHECK(out.find("<") == std::string::npos);
    CHECK(out.find(">") == std::string::npos);
    CHECK_EQ(out, std::string("&lt;script&gt;alert(1)&lt;/script&gt;"));
}

TEST(webutil_text_escapes_ampersand) {
    CHECK_EQ(esc_text(kAmp), std::string("A&amp;B"));
}

TEST(webutil_text_keeps_unicode_intact) {
    // Legitimate Unicode must pass through unchanged (only markup is escaped).
    CHECK_EQ(esc_text(kUnicode), kUnicode);
}

// --- HTML attribute escaping -----------------------------------------------

TEST(webutil_attr_neutralizes_quote_breakout) {
    // Placed inside title="{}", the escaped value must not contain a bare quote
    // that could close the attribute and add an event handler.
    std::string out = esc_attr(kAttrBreak);
    CHECK(out.find('"') == std::string::npos);
    CHECK_EQ(out, std::string("&quot; onmouseover=&quot;alert(1)"));
}

TEST(webutil_attr_escapes_single_quote_and_markup) {
    std::string out = esc_attr("' <b> & \"");
    CHECK(out.find('"') == std::string::npos);
    CHECK(out.find('\'') == std::string::npos);
    CHECK(out.find('<') == std::string::npos);
    CHECK_EQ(out, std::string("&#39; &lt;b&gt; &amp; &quot;"));
}

TEST(webutil_attr_keeps_unicode_intact) {
    CHECK_EQ(esc_attr(kUnicode), kUnicode);
}

// --- URL query encoding ----------------------------------------------------

TEST(webutil_url_encode_reserved_and_space) {
    // Space, and all reserved punctuation, must be percent-encoded.
    std::string out = url_encode(kPunct);
    CHECK(out.find(' ') == std::string::npos);
    CHECK(out.find('<') == std::string::npos);
    CHECK(out.find('&') == std::string::npos);
    CHECK(out.find('=') == std::string::npos);
    CHECK(out.find('?') == std::string::npos);
    CHECK(out.find('%') != std::string::npos);
}

TEST(webutil_url_roundtrip_unicode) {
    CHECK_EQ(url_decode(url_encode(kUnicode)), kUnicode);
}

TEST(webutil_url_roundtrip_adversarial) {
    for (const auto& s : {kScript, kAttrBreak, kAmp, kPunct, kUnicode})
        CHECK_EQ(url_decode(url_encode(s)), s);
}

TEST(webutil_url_encode_unreserved_untouched) {
    CHECK_EQ(url_encode("Abc-190_x.y~z"), std::string("Abc-190_x.y~z"));
}

// --- Activity links (canonical page URL) -----------------------------------

TEST(webutil_activity_url_encodes_name) {
    std::string u = activity_url(kScript);
    CHECK(u.rfind("/activity?name=", 0) == 0);
    CHECK(u.find("<script>") == std::string::npos);
    // The encoded name must decode back to the original activity.
    std::string q = u.substr(std::string("/activity?name=").size());
    CHECK_EQ(url_decode(q), kScript);
}

TEST(webutil_activity_url_unicode_resolves) {
    std::string name = "morning café run";
    std::string u = activity_url(name);
    std::string q = u.substr(std::string("/activity?name=").size());
    CHECK_EQ(url_decode(q), name);
}

// --- Task 1: canonical refresh / flash-cleanup URL --------------------------

TEST(webutil_canonical_url_strips_only_flash_params) {
    // ok/err are transient and removed; name is functional and preserved.
    std::string q = "name=" + url_encode("morning run") + "&ok=Logged";
    std::string c = canonical_url("/activity", q);
    CHECK(c.rfind("/activity?", 0) == 0);
    CHECK(c.find("ok=") == std::string::npos);
    CHECK(c.find("err=") == std::string::npos);
    // name survives and still points at the same activity.
    auto params = parse_query(c.substr(c.find('?') + 1));
    bool found = false;
    for (auto& [k, v] : params)
        if (k == "name") { found = true; CHECK_EQ(v, std::string("morning run")); }
    CHECK(found);
}

TEST(webutil_canonical_url_preserves_name_after_flash) {
    // Simulates the activity page after an action: ?name=<enc>&ok=<msg>.
    std::string name = kScript; // adversarial, reserved chars
    std::string q = "name=" + url_encode(name) + "&ok=" + url_encode("Logged \"x\".");
    std::string c = canonical_url("/activity", q);
    CHECK(c.find("ok=") == std::string::npos);
    auto params = parse_query(c.substr(c.find('?') + 1));
    bool found = false;
    for (auto& [k, v] : params)
        if (k == "name") { found = true; CHECK_EQ(v, name); }
    CHECK(found);
}

TEST(webutil_canonical_url_preserves_filter_params) {
    std::string q = "type=task&tag=work&tag=admin&err=Nope";
    std::string c = canonical_url("/", q);
    CHECK(c.find("err=") == std::string::npos);
    CHECK(c.find("type=task") != std::string::npos);
    CHECK(c.find("tag=work") != std::string::npos);
    CHECK(c.find("tag=admin") != std::string::npos);
}

TEST(webutil_canonical_url_no_query_stays_bare_path) {
    CHECK_EQ(canonical_url("/", ""), std::string("/"));
    CHECK_EQ(canonical_url("/", "ok=hi"), std::string("/"));
}

// --- Tags: parsing, filter URLs, chips -------------------------------------

TEST(webutil_split_tags_normalizes_and_dedupes_on_split) {
    auto v = split_tags("Health, morning  health");
    // Splits on comma and whitespace, normalizes (lowercase/trim). De-dup is the
    // domain layer's job, so raw split may repeat; here order is preserved.
    CHECK(v.size() >= 2);
    CHECK_EQ(v[0], std::string("health"));
    CHECK_EQ(v[1], std::string("morning"));
}

TEST(webutil_split_tags_ignores_empties) {
    auto v = split_tags("  ,, , ");
    CHECK(v.empty());
}

TEST(webutil_dashboard_url_builds_filters) {
    CHECK_EQ(dashboard_url({}, ""), std::string("/"));
    CHECK_EQ(dashboard_url({}, "habit"), std::string("/?type=habit"));
    std::string u = dashboard_url({"work", "café"}, "task");
    CHECK(u.rfind("/?", 0) == 0);
    CHECK(u.find("type=task") != std::string::npos);
    CHECK(u.find("tag=work") != std::string::npos);
    // Unicode tag is encoded and round-trips.
    auto params = parse_query(u.substr(2));
    bool found_cafe = false;
    for (auto& [k, val] : params)
        if (k == "tag" && val == "café") found_cafe = true;
    CHECK(found_cafe);
}

TEST(webutil_render_tag_chips_escapes_text_and_href) {
    auto html = render_tag_chips({kScript});
    CHECK(html.find("<script>") == std::string::npos);   // label escaped as text
    CHECK(html.find("&lt;script&gt;") != std::string::npos);
    // href carries an encoded filter URL, never a raw quote/space.
    auto hpos = html.find("href=\"");
    CHECK(hpos != std::string::npos);
    auto hend = html.find('"', hpos + 6);
    std::string href = html.substr(hpos + 6, hend - (hpos + 6));
    CHECK(href.find(' ') == std::string::npos);
    CHECK(href.find('<') == std::string::npos);
}

TEST(webutil_render_tag_chips_empty_is_empty) {
    CHECK(render_tag_chips({}).empty());
}

// --- Open-redirect guard ----------------------------------------------------

TEST(webutil_is_safe_next_allows_local_paths) {
    CHECK(is_safe_next("/"));
    CHECK(is_safe_next("/?tag=work&type=task"));
    CHECK(is_safe_next("/activity?name=x"));
}

TEST(webutil_is_safe_next_rejects_offsite) {
    CHECK(!is_safe_next("//evil.example.com"));
    CHECK(!is_safe_next("https://evil.example.com"));
    CHECK(!is_safe_next("evil"));
    CHECK(!is_safe_next(""));
}
