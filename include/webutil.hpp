#pragma once
// Context-aware output-escaping and URL helpers for the web dashboard.
//
// These are deliberately kept free of httplib and of any server state so they
// can be unit-tested directly. web.cpp includes this header and every
// user-controlled value that reaches the page must pass through the helper that
// matches its *context*: text node, attribute value, or URL component. A single
// generic "html escape" is not correct for all three, so three helpers exist.

#include "activity.hpp" // normalize_tag

#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webutil {

// --- HTML escaping ---------------------------------------------------------

// Escape a value for an HTML *text node* (between tags): only `&`, `<`, `>`
// can change the parse there. Quotes are intentionally left as-is — they are
// legal text — which keeps ordinary punctuation readable.
inline std::string esc_text(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;";  break;
            case '>': out += "&gt;";  break;
            default:  out += c;
        }
    }
    return out;
}

// Escape a value destined for a double-quoted HTML *attribute*. In addition to
// the text-node set this neutralizes both quote styles so the value can never
// terminate the attribute and inject a new one (e.g. `" onmouseover="...`).
inline std::string esc_attr(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;
        }
    }
    return out;
}

// --- URL encoding ----------------------------------------------------------

// Percent-encode a value for use as a URL query-component (RFC 3986 unreserved
// set kept literal; everything else, including space, `&`, `=`, `?`, `%`, and
// all non-ASCII bytes, percent-encoded). Correct for both building links and
// redirect query strings.
inline std::string url_encode(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 0xf]; }
    }
    return out;
}

// Decode a percent-encoded query component. `+` is treated as a space
// (application/x-www-form-urlencoded), matching what browsers submit. Invalid
// escapes are passed through literally rather than dropped.
inline std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') out += ' ';
        else out += s[i];
    }
    return out;
}

// --- Links & canonical page URLs ------------------------------------------

// Detail-page link for an activity. The name is the query value, so it is
// url-encoded; the result is safe to drop straight into an href attribute.
inline std::string activity_url(const std::string& name) {
    return "/activity?name=" + url_encode(name);
}

// Split a raw query string (no leading '?') into decoded key/value pairs,
// preserving order. Values are url-decoded; keys are compared literally.
inline std::vector<std::pair<std::string, std::string>>
parse_query(std::string_view query) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t i = 0;
    while (i < query.size()) {
        std::size_t amp = query.find('&', i);
        std::string_view pair = query.substr(i, amp == std::string_view::npos
                                                    ? std::string_view::npos : amp - i);
        if (!pair.empty()) {
            std::size_t eq = pair.find('=');
            if (eq == std::string_view::npos)
                out.emplace_back(url_decode(pair), std::string());
            else
                out.emplace_back(url_decode(pair.substr(0, eq)),
                                 url_decode(pair.substr(eq + 1)));
        }
        if (amp == std::string_view::npos) break;
        i = amp + 1;
    }
    return out;
}

// The URL the page should settle on: current path plus its query with the
// transient flash params (`ok`, `err`) removed and every functional param
// (e.g. `name`, `tag`, `type`) preserved. Mirrors the browser-side
// canonicalUrl() the dashboard script uses for both flash-cleanup and the
// periodic refresh, so the deterministic C++ tests exercise the same logic.
inline std::string canonical_url(std::string_view path, std::string_view query) {
    std::string rebuilt;
    for (auto& [k, v] : parse_query(query)) {
        if (k == "ok" || k == "err") continue;
        if (!rebuilt.empty()) rebuilt += '&';
        rebuilt += url_encode(k);
        rebuilt += '=';
        rebuilt += url_encode(v);
    }
    std::string out(path);
    if (!rebuilt.empty()) { out += '?'; out += rebuilt; }
    return out;
}

// --- Tags ------------------------------------------------------------------

// Split a free-form tag field (as typed into one web input) into individual
// tags. Commas *and* whitespace separate, matching how a user naturally types
// "health, morning" or "health morning". Normalization/de-duplication is left
// to the domain layer (Tracker/clean_tags) so behavior stays identical to the
// CLI; this only tokenizes.
inline std::vector<std::string> split_tags(std::string_view field) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        std::string n = normalize_tag(cur);
        if (!n.empty()) out.push_back(n);
        cur.clear();
    };
    for (char c : field) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') flush();
        else cur += c;
    }
    flush();
    return out;
}

// Build a dashboard URL carrying the active filters, so links and redirect
// targets keep the current view. Tags are url-encoded; an empty filter set
// yields a bare "/".
inline std::string dashboard_url(const std::vector<std::string>& tags,
                                 std::string_view type) {
    std::string q;
    if (type == "habit" || type == "task") {
        q += "type=";
        q += url_encode(type);
    }
    for (const auto& t : tags) {
        if (!q.empty()) q += '&';
        q += "tag=";
        q += url_encode(t);
    }
    return q.empty() ? "/" : "/?" + q;
}

// Display-only tag chips for a card/detail header. Each chip links to the
// dashboard filtered by that single tag. Text is escaped as a text node and the
// href as a URL component.
inline std::string render_tag_chips(const std::vector<std::string>& tags) {
    if (tags.empty()) return {};
    std::string out = "<div class=\"chips tags\">";
    for (const auto& t : tags)
        out += "<a class=\"chip tag\" href=\"" + esc_attr(dashboard_url({t}, ""))
             + "\">#" + esc_text(t) + "</a>";
    out += "</div>";
    return out;
}

// True when `next` is a safe same-origin redirect target: a rooted path that is
// not protocol-relative ("//host"). Used to reject open-redirect attempts while
// still allowing filtered dashboards ("/?tag=..") and detail pages.
inline bool is_safe_next(std::string_view next) {
    return next.size() >= 1 && next.front() == '/' &&
           !(next.size() >= 2 && next[1] == '/');
}

} // namespace webutil
