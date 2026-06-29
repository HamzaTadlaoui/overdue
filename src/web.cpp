#include "web.hpp"
#include "tracker.hpp"
#include "stats.hpp"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <sys/wait.h>
#include <unistd.h>

using Outcome = std::pair<bool, std::string>; // {ok, message}

// Activity names and units are user-controlled and rendered into the page, so
// escape them even though the server only ever binds to loopback.
static std::string html_escape(const std::string& s) {
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

// Percent-encode for putting a flash message into a redirect query string.
static std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 0xf]; }
    }
    return out;
}

// Absolute unix seconds, so the browser can tick the elapsed timer live.
static long long to_epoch(const std::chrono::system_clock::time_point& tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

static std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Random token tying every form to this server instance, so another site open
// in the same browser can't drive these endpoints (the loopback bind alone
// doesn't stop cross-site POSTs).
static std::string make_token() {
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    static const char* hex = "0123456789abcdef";
    std::string t;
    t.reserve(32);
    for (int i = 0; i < 32; ++i) t += hex[dist(rd)];
    return t;
}

// Hand off to the desktop's default browser, mirroring the notify() helper in
// main.cpp: fork, then exec so we never block the server thread.
static void open_browser(const std::string& url) {
    pid_t pid = fork();
    if (pid == 0) {
        execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
        _exit(127);
    } else if (pid > 0) {
        waitpid(pid, nullptr, 0);
    }
}

static const char* PAGE_HEAD = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>overdue</title>
<style>
  :root{
    --bg:#0b0d11; --bg2:#0e1116; --surface:#14181f; --surface-2:#171c25;
    --border:rgba(255,255,255,.07); --border-strong:rgba(255,255,255,.12);
    --text:#e8eaf0; --muted:#9aa2b1; --faint:#6b7383;
    --accent:#7c8cff; --accent-2:#5b6cff; --ok:#46d39a; --danger:#ff6b6b;
    --radius:14px; color-scheme:dark;
  }
  *{box-sizing:border-box}
  body{
    margin:0; min-height:100vh; color:var(--text);
    font:15px/1.55 ui-sans-serif,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
    background:
      radial-gradient(900px 520px at 82% -10%, rgba(124,140,255,.10), transparent 60%),
      radial-gradient(700px 500px at -10% 8%, rgba(70,211,154,.06), transparent 55%),
      var(--bg);
  }
  .wrap{max-width:760px; margin:0 auto; padding:2.2rem 1.3rem 4rem;}
  a{color:var(--accent)}

  header{display:flex; align-items:center; justify-content:space-between; gap:1rem; margin-bottom:1.7rem;}
  .brand{display:flex; align-items:center; gap:.8rem;}
  .logo{width:42px; height:42px; display:grid; place-items:center; border-radius:12px;
        background:linear-gradient(145deg,var(--accent),var(--accent-2)); font-size:1.25rem;
        box-shadow:0 6px 20px rgba(91,108,255,.35);}
  h1{margin:0; font-size:1.45rem; letter-spacing:-.02em;}
  .tagline{margin:0; color:var(--muted); font-size:.85rem;}
  .sync{color:var(--faint); font-size:.8rem; font-variant-numeric:tabular-nums;}

  .stats{display:grid; grid-template-columns:repeat(4,1fr); gap:.8rem; margin-bottom:1.7rem;}
  .stat{background:var(--surface); border:1px solid var(--border); border-radius:var(--radius); padding:.85rem 1rem;}
  .stat .n{font-size:1.6rem; font-weight:700; letter-spacing:-.02em; font-variant-numeric:tabular-nums;}
  .stat .l{color:var(--muted); font-size:.72rem; text-transform:uppercase; letter-spacing:.06em;}
  .stat.warn{border-color:rgba(255,107,107,.4); background:linear-gradient(180deg,rgba(255,107,107,.08),transparent);}
  .stat.warn .n{color:var(--danger);}
  @media(max-width:560px){.stats{grid-template-columns:repeat(2,1fr);}}

  .addbar{display:flex; gap:.7rem; margin-bottom:1.4rem; flex-wrap:wrap;}
  .addbar details{flex:1; min-width:180px;}
  .addbar summary{list-style:none; cursor:pointer; user-select:none; font-weight:600; font-size:.9rem;
        background:var(--surface); border:1px solid var(--border); border-radius:12px; padding:.6rem .9rem; transition:.15s;}
  .addbar summary::-webkit-details-marker{display:none;}
  .addbar summary:hover{border-color:var(--border-strong); background:var(--surface-2);}
  .addbar details[open] summary{border-color:var(--accent);}
  .addform{background:var(--surface); border:1px solid var(--border); border-radius:12px;
        margin-top:.5rem; padding:.9rem 1rem; display:grid; gap:.6rem;}
  .fld{display:grid; gap:.25rem; font-size:.78rem; color:var(--muted);}

  .section-title{font-size:.8rem; text-transform:uppercase; letter-spacing:.08em; color:var(--faint); margin:1.6rem .2rem .8rem;}

  .entry{background:var(--surface); border:1px solid var(--border); border-radius:var(--radius);
        padding:1.05rem 1.15rem; margin-bottom:.85rem; transition:.18s;}
  .entry:hover{border-color:var(--border-strong);}
  .entry.over{border-color:rgba(255,107,107,.45); background:linear-gradient(180deg,rgba(255,107,107,.06),transparent 60%);}
  .entry-head{display:flex; align-items:flex-start; justify-content:space-between; gap:.6rem;}
  .name{font-weight:650; font-size:1.08rem; letter-spacing:-.01em;}
  .badges{display:flex; gap:.4rem; flex-wrap:wrap; justify-content:flex-end;}
  .badge{font-size:.72rem; padding:.18rem .5rem; border-radius:999px; white-space:nowrap;
        background:var(--surface-2); border:1px solid var(--border); color:var(--muted);}
  .badge.streak{color:#ffd08a; border-color:rgba(255,208,138,.25);}
  .badge.danger{color:var(--danger); border-color:rgba(255,107,107,.4); background:rgba(255,107,107,.08); font-weight:600;}
  .badge.ok{color:var(--ok); border-color:rgba(70,211,154,.3);}

  .timer{font-size:1.7rem; font-weight:700; letter-spacing:-.01em; margin:.35rem 0 .15rem;
        font-variant-numeric:tabular-nums; color:var(--accent);}
  .timer.over{color:var(--danger); animation:pulse 2s ease-in-out infinite;}
  .timer.done{color:var(--ok); font-size:1.15rem;}
  @keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}
  .sub{color:var(--muted); font-size:.85rem;}

  .bar{height:7px; border-radius:999px; background:var(--surface-2); overflow:hidden;
        margin:.7rem 0 .35rem; border:1px solid var(--border);}
  .bar-fill{height:100%; border-radius:999px; background:linear-gradient(90deg,var(--accent-2),var(--accent));}
  .bar-label{color:var(--muted); font-size:.8rem;}

  .actions{display:flex; flex-wrap:wrap; gap:.45rem; align-items:center; margin-top:.85rem;}
  form.row{display:inline-flex; gap:.4rem; align-items:center; margin:0;}
  .in{background:var(--bg2); border:1px solid var(--border-strong); color:var(--text);
        border-radius:9px; padding:.4rem .55rem; font-size:.88rem; transition:.15s;}
  .in:focus{outline:none; border-color:var(--accent); box-shadow:0 0 0 3px rgba(124,140,255,.18);}
  .in.sm{width:5.4rem;}
  .btn{border:1px solid var(--border-strong); border-radius:9px; padding:.42rem .8rem; font-size:.85rem;
        font-weight:600; cursor:pointer; transition:.15s; color:#fff; background:var(--surface-2);}
  .btn:hover{transform:translateY(-1px);}
  .btn.primary{background:linear-gradient(145deg,var(--accent),var(--accent-2)); border-color:transparent;
        box-shadow:0 4px 14px rgba(91,108,255,.3);}
  .btn.ghost{background:var(--surface-2); color:var(--muted);}
  .btn.ghost:hover{color:var(--text);}
  .btn.danger{background:rgba(255,107,107,.14); border-color:rgba(255,107,107,.35); color:#ff9a9a;}
  .btn.danger:hover{background:rgba(255,107,107,.22);}

  details.manage{margin-top:.7rem;}
  details.manage>summary{list-style:none; cursor:pointer; color:var(--faint); font-size:.8rem;}
  details.manage>summary::-webkit-details-marker{display:none;}
  details.manage>summary:hover{color:var(--muted);}
  .manage-grid{display:flex; flex-wrap:wrap; gap:.5rem; margin-top:.7rem; padding-top:.8rem; border-top:1px solid var(--border);}

  .toast{position:fixed; top:1rem; left:50%; transform:translateX(-50%); z-index:50;
        padding:.7rem 1.1rem; border-radius:11px; font-size:.9rem; font-weight:500;
        border:1px solid var(--border-strong); box-shadow:0 10px 30px rgba(0,0,0,.4);
        backdrop-filter:blur(8px); transition:opacity .4s,transform .4s;}
  .toast.ok{background:rgba(70,211,154,.16); color:#9df0cb; border-color:rgba(70,211,154,.4);}
  .toast.err{background:rgba(255,107,107,.16); color:#ffb4b4; border-color:rgba(255,107,107,.4);}
  .toast.hide{opacity:0; transform:translateX(-50%) translateY(-12px);}

  .empty{color:var(--faint); font-style:italic; padding:.5rem .2rem;}
  footer{margin-top:2.4rem; color:var(--faint); font-size:.78rem; text-align:center;}
</style>
</head>
<body>
<div class="wrap">
)HTML";

static const char* PAGE_SCRIPT = R"HTML(<script>
(function(){
  function fmt(s){
    s = Math.max(0, Math.floor(s));
    var d=Math.floor(s/86400), h=Math.floor(s%86400/3600), m=Math.floor(s%3600/60), x=s%60;
    if(d>0) return d+"d "+h+"h "+m+"m "+x+"s";
    if(h>0) return h+"h "+m+"m "+x+"s";
    if(m>0) return m+"m "+x+"s";
    return x+"s";
  }
  function tick(){
    var now = Date.now()/1000;
    document.querySelectorAll("[data-since]").forEach(function(el){
      el.textContent = fmt(now - parseFloat(el.dataset.since));
    });
  }
  tick(); setInterval(tick, 1000);

  // Don't yank the page out from under someone who's typing or mid-action.
  function busy(){
    var a = document.activeElement;
    if(a && /^(INPUT|SELECT|TEXTAREA)$/.test(a.tagName)) return true;
    if(document.querySelector("details[open]")) return true;
    if(window.getSelection && String(window.getSelection())) return true;
    return false;
  }
  setInterval(function(){ if(!busy()) location.replace("/"); }, 30000);

  // Drop the flash from the URL so a manual reload won't replay it, then fade it out.
  if(location.search) history.replaceState(null, "", location.pathname);
  var t = document.querySelector(".toast");
  if(t) setTimeout(function(){ t.classList.add("hide"); }, 3500);
})();
</script>
)HTML";

static std::string render_page(const std::filesystem::path& data_path,
                               const std::string& token,
                               std::optional<Outcome> flash) {
    // Reload on every request so the page reflects edits from the CLI or other tabs.
    Tracker tracker{data_path};
    auto habits  = tracker.habits();
    auto tasks   = tracker.tasks(/*include_done=*/true);
    auto overdue = tracker.overdue_activities();
    auto gs      = compute_global(tracker.all());

    std::unordered_set<std::string> overdue_names;
    for (const auto& a : overdue) overdue_names.insert(a.name);

    std::string tok = html_escape(token);

    // A form carrying the CSRF token plus a preset entry name.
    auto nform = [&](const char* action, const std::string& name, const std::string& inner) {
        return std::format(
            "<form class=\"row\" method=\"post\" action=\"{}\">"
            "<input type=\"hidden\" name=\"token\" value=\"{}\">"
            "<input type=\"hidden\" name=\"name\" value=\"{}\">{}</form>",
            action, tok, html_escape(name), inner);
    };

    // Progress bar (or a bare total) for entries that carry amounts / a target.
    auto progress_html = [&](const Activity& a) -> std::string {
        auto qs = quantity_stats(a);
        double total = qs ? qs->total : 0.0;
        std::string unit = a.unit ? " " + *a.unit : "";
        if (a.target) {
            double pct = *a.target > 0 ? 100.0 * total / *a.target : 0.0;
            double fill = std::min(100.0, std::max(0.0, pct));
            return std::format(
                "<div class=\"bar\"><div class=\"bar-fill\" style=\"width:{:.1f}%\"></div></div>"
                "<div class=\"bar-label\">{} / {}{} · {:.0f}%</div>",
                fill, format_amount(total), format_amount(*a.target), unit, pct);
        }
        if (qs)
            return std::format("<div class=\"bar-label\">{}{} total</div>", format_amount(total), unit);
        return "";
    };

    // Shared manage controls (alarm/streak are habit-only; unit/target/delete for all)
    auto manage_block = [&](const Activity& a, bool is_habit) {
        std::string m = "<details class=\"manage\"><summary>Manage ▾</summary><div class=\"manage-grid\">";
        if (is_habit) {
            m += nform("/setalarm", a.name,
                "<input class=\"in sm\" name=\"dur\" placeholder=\"3d\"><button class=\"btn ghost\">Set alarm</button>");
            if (a.alert_after)
                m += nform("/delalarm", a.name, "<button class=\"btn ghost\">Clear alarm</button>");
            m += nform("/setstreak", a.name,
                "<input class=\"in sm\" name=\"streak\" placeholder=\"daily\"><button class=\"btn ghost\">Set streak</button>");
            if (a.streak)
                m += nform("/delstreak", a.name, "<button class=\"btn ghost\">Clear streak</button>");
        }
        m += nform("/setunit", a.name,
            "<input class=\"in sm\" name=\"unit\" placeholder=\"km\"><button class=\"btn ghost\">Set unit</button>");
        if (a.unit)
            m += nform("/delunit", a.name, "<button class=\"btn ghost\">Clear unit</button>");
        m += nform("/settarget", a.name,
            "<input class=\"in sm\" name=\"target\" placeholder=\"100\"><button class=\"btn ghost\">Set target</button>");
        if (a.target)
            m += nform("/deltarget", a.name, "<button class=\"btn ghost\">Clear target</button>");
        // Delete confirms client-side; the message stays generic to avoid escaping names into JS.
        m += std::format(
            "<form class=\"row\" method=\"post\" action=\"/delete\" "
            "onsubmit=\"return confirm('Delete this entry and all its history?')\">"
            "<input type=\"hidden\" name=\"token\" value=\"{}\">"
            "<input type=\"hidden\" name=\"name\" value=\"{}\">"
            "<button class=\"btn danger\">Delete</button></form>",
            tok, html_escape(a.name));
        m += "</div></details>";
        return m;
    };

    std::string out = PAGE_HEAD;

    if (flash)
        out += std::format("<div class=\"toast {}\">{}</div>\n",
                           flash->first ? "ok" : "err", html_escape(flash->second));

    // Header: brand + a live "updated Xs ago" indicator the JS keeps ticking.
    out += std::format(
        "<header><div class=\"brand\"><div class=\"logo\">⏱</div>"
        "<div><h1>overdue</h1><p class=\"tagline\">how long has it been?</p></div></div>"
        "<div class=\"sync\">synced <span data-since=\"{}\">0s</span> ago</div></header>\n",
        to_epoch(now()));

    // Summary stats
    out += "<div class=\"stats\">\n";
    out += std::format("<div class=\"stat\"><div class=\"n\">{}</div><div class=\"l\">Habits</div></div>\n", gs.habit_count);
    out += std::format("<div class=\"stat\"><div class=\"n\">{}</div><div class=\"l\">Total logs</div></div>\n", gs.total_logs);
    out += std::format("<div class=\"stat\"><div class=\"n\">{} / {}</div><div class=\"l\">Tasks done</div></div>\n", gs.task_done, gs.task_total);
    out += std::format("<div class=\"stat{}\"><div class=\"n\">{}</div><div class=\"l\">Overdue</div></div>\n",
                       overdue.empty() ? "" : " warn", overdue.size());
    out += "</div>\n";

    // Add forms (collapsible)
    out += "<div class=\"addbar\">\n";
    out += std::format(
        "<details><summary>+ New habit</summary>"
        "<form class=\"addform\" method=\"post\" action=\"/add\">"
        "<input type=\"hidden\" name=\"token\" value=\"{}\">"
        "<input type=\"hidden\" name=\"type\" value=\"habit\">"
        "<label class=\"fld\">Name<input class=\"in\" name=\"name\" required></label>"
        "<label class=\"fld\">Alarm after (e.g. 3d, 12h)<input class=\"in\" name=\"alarm\"></label>"
        "<label class=\"fld\">Streak (daily / weekly / 3d)<input class=\"in\" name=\"streak\"></label>"
        "<label class=\"fld\">Unit (e.g. km)<input class=\"in\" name=\"unit\"></label>"
        "<label class=\"fld\">Target<input class=\"in\" name=\"target\"></label>"
        "<button class=\"btn primary\" type=\"submit\">Add habit</button></form></details>\n", tok);
    out += std::format(
        "<details><summary>+ New task</summary>"
        "<form class=\"addform\" method=\"post\" action=\"/add\">"
        "<input type=\"hidden\" name=\"token\" value=\"{}\">"
        "<input type=\"hidden\" name=\"type\" value=\"task\">"
        "<label class=\"fld\">Name<input class=\"in\" name=\"name\" required></label>"
        "<label class=\"fld\">Unit (e.g. pages)<input class=\"in\" name=\"unit\"></label>"
        "<label class=\"fld\">Target<input class=\"in\" name=\"target\"></label>"
        "<button class=\"btn primary\" type=\"submit\">Add task</button></form></details>\n", tok);
    out += "</div>\n";

    // Habits
    out += "<div class=\"section-title\">Habits</div>\n";
    if (habits.empty()) {
        out += "<p class=\"empty\">No habits yet — add one above.</p>\n";
    } else {
        for (const auto& a : habits) {
            bool od = overdue_names.contains(a.name);

            std::string badges;
            if (od) badges += "<span class=\"badge danger\">overdue</span>";
            if (a.streak)
                badges += std::format("<span class=\"badge streak\">🔥 {} · {}</span>",
                                      compute_streak(a), format_streak_label(*a.streak));
            if (a.alert_after)
                badges += std::format("<span class=\"badge\">⏰ {}</span>", format_duration(*a.alert_after));

            std::string log_form = nform("/log", a.name,
                "<input class=\"in sm\" name=\"amount\" placeholder=\"amt\">"
                "<input class=\"in sm\" name=\"ago\" placeholder=\"ago\">"
                "<button class=\"btn primary\">Log</button>");
            std::string unlog_form = a.logs.size() > 1
                ? nform("/unlog", a.name, "<button class=\"btn ghost\">Unlog</button>") : "";

            out += std::format(
                "<div class=\"entry{}\">"
                "<div class=\"entry-head\"><div class=\"name\">{}</div><div class=\"badges\">{}</div></div>"
                "<div class=\"timer{}\" data-since=\"{}\">{}</div>"
                "<div class=\"sub\">last done {}</div>"
                "{}"
                "<div class=\"actions\">{}{}</div>"
                "{}</div>\n",
                od ? " over" : "",
                html_escape(a.name), badges,
                od ? " over" : "", to_epoch(last_done(a)), format_elapsed(last_done(a)),
                format_datetime(last_done(a)),
                progress_html(a),
                log_form, unlog_form,
                manage_block(a, /*is_habit=*/true));
        }
    }

    // Tasks
    out += "<div class=\"section-title\">Tasks</div>\n";
    if (tasks.empty()) {
        out += "<p class=\"empty\">No tasks yet — add one above.</p>\n";
    } else {
        for (const auto& a : tasks) {
            std::string badges, timer, sub;
            if (a.completed_at) {
                badges = "<span class=\"badge ok\">done</span>";
                timer  = "<div class=\"timer done\">✓ completed</div>";
                sub    = std::format("done {} · added {}",
                    format_datetime(*a.completed_at), format_datetime(a.logs.front().when));
            } else {
                timer = std::format("<div class=\"timer\" data-since=\"{}\">{}</div>",
                    to_epoch(a.logs.front().when), format_elapsed(a.logs.front().when));
                sub = std::format("pending · added {}", format_datetime(a.logs.front().when));
            }

            std::string actions;
            if (!a.completed_at) {
                actions += nform("/done", a.name, "<button class=\"btn primary\">Mark done</button>");
                actions += nform("/log", a.name,
                    "<input class=\"in sm\" name=\"amount\" placeholder=\"amt\"><button class=\"btn ghost\">Log progress</button>");
            }
            if (a.logs.size() > 1)
                actions += nform("/unlog", a.name, "<button class=\"btn ghost\">Unlog</button>");

            out += std::format(
                "<div class=\"entry\">"
                "<div class=\"entry-head\"><div class=\"name\">{}</div><div class=\"badges\">{}</div></div>"
                "{}"
                "<div class=\"sub\">{}</div>"
                "{}"
                "<div class=\"actions\">{}</div>"
                "{}</div>\n",
                html_escape(a.name), badges,
                timer,
                html_escape(sub),
                progress_html(a),
                actions,
                manage_block(a, /*is_habit=*/false));
        }
    }

    out += "<footer>overdue · localhost dashboard · loopback only</footer>\n</div>\n";
    out += PAGE_SCRIPT;
    out += "</body>\n</html>\n";
    return out;
}

void run_web(const std::filesystem::path& data_path, int port) {
    httplib::Server svr;
    const std::string token = make_token();
    std::mutex write_mtx; // serialize the load-modify-save in mutating handlers

    // Redirect back to the dashboard with a flash message (Post/Redirect/Get).
    auto finish = [](httplib::Response& res, bool ok, const std::string& msg) {
        res.status = 303;
        res.set_header("Location", std::format("/?{}={}", ok ? "ok" : "err", url_encode(msg)));
    };

    // Reject any POST without the matching session token (blocks cross-site posts).
    auto guard = [&](const httplib::Request& req, httplib::Response& res) {
        if (req.get_param_value("token") == token) return true;
        res.status = 403;
        res.set_content("Forbidden: invalid or missing token. Reload the dashboard.", "text/plain");
        return false;
    };

    // Run a mutation under the lock and turn its Outcome into a redirect.
    auto run = [&](httplib::Response& res, auto&& fn) {
        std::lock_guard<std::mutex> lock(write_mtx);
        try {
            Tracker t{data_path};
            auto [ok, msg] = fn(t);
            finish(res, ok, msg);
        } catch (const std::exception& e) {
            finish(res, false, std::string("Error: ") + e.what());
        }
    };

    auto opt_field = [](const httplib::Request& req, const char* key) -> std::optional<std::string> {
        if (!req.has_param(key)) return std::nullopt;
        auto v = trim(req.get_param_value(key));
        if (v.empty()) return std::nullopt;
        return v;
    };

    svr.Get("/", [&](const httplib::Request& req, httplib::Response& res) {
        std::optional<Outcome> flash;
        if (req.has_param("ok"))  flash = Outcome{true,  req.get_param_value("ok")};
        else if (req.has_param("err")) flash = Outcome{false, req.get_param_value("err")};
        try {
            res.set_content(render_page(data_path, token, flash), "text/html; charset=utf-8");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::format("Error: {}", e.what()), "text/plain");
        }
    });

    svr.Post("/add", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(res, [&](Tracker& t) -> Outcome {
            std::string name = trim(req.get_param_value("name"));
            if (name.empty()) return {false, "Name is required."};
            bool is_task = req.get_param_value("type") == "task";

            std::optional<std::string> unit = opt_field(req, "unit");
            std::optional<double> target;
            if (auto v = opt_field(req, "target")) {
                target = parse_amount(*v);
                if (!target) return {false, "Invalid target — expected a non-negative number."};
            }
            if (is_task)
                return t.addtask(name, unit, target)
                    ? Outcome{true, "Added task \"" + name + "\"."}
                    : Outcome{false, "\"" + name + "\" already exists."};

            std::optional<long long> alarm;
            if (auto v = opt_field(req, "alarm")) {
                alarm = parse_duration(*v);
                if (!alarm) return {false, "Invalid alarm duration — try 3d, 12h, 30m."};
            }
            std::optional<StreakConfig> streak;
            if (auto v = opt_field(req, "streak")) {
                streak = parse_streak(*v);
                if (!streak) return {false, "Invalid streak — try daily, weekly, monthly, 3d."};
            }
            return t.add(name, alarm, streak, unit, target)
                ? Outcome{true, "Now tracking \"" + name + "\"."}
                : Outcome{false, "\"" + name + "\" is already tracked."};
        });
    });

    svr.Post("/log", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            std::optional<double> amount;
            if (auto v = opt_field(req, "amount")) {
                amount = parse_amount(*v);
                if (!amount) return {false, "Invalid amount — expected a non-negative number."};
            }
            std::optional<std::chrono::system_clock::time_point> when;
            if (auto v = opt_field(req, "ago")) {
                auto d = parse_duration(*v);
                if (!d) return {false, "Invalid duration — try 2h, 1d6h."};
                when = now() - std::chrono::seconds{*d};
            }
            return t.log(name, when, amount)
                ? Outcome{true, "Logged \"" + name + "\"."}
                : Outcome{false, "\"" + name + "\" not found."};
        });
    });

    svr.Post("/unlog", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            return t.unlog(name)
                ? Outcome{true, "Cancelled last log for \"" + name + "\"."}
                : Outcome{false, "\"" + name + "\" not found or no log to restore."};
        });
    });

    svr.Post("/done", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            return t.done(name)
                ? Outcome{true, "\"" + name + "\" completed."}
                : Outcome{false, "\"" + name + "\" not found or is not a task."};
        });
    });

    svr.Post("/delete", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            return t.remove(name)
                ? Outcome{true, "Removed \"" + name + "\"."}
                : Outcome{false, "\"" + name + "\" not found."};
        });
    });

    svr.Post("/setalarm", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            auto v = opt_field(req, "dur");
            if (!v) return {false, "Enter a duration like 3d or 12h."};
            auto d = parse_duration(*v);
            if (!d) return {false, "Invalid duration — try 3d, 12h, 30m."};
            return t.setalarm(name, *d)
                ? Outcome{true, std::format("Alarm for \"{}\" set after {}.", name, format_duration(*d))}
                : Outcome{false, "\"" + name + "\" not found."};
        });
    });

    svr.Post("/delalarm", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            return t.delalarm(name)
                ? Outcome{true, "Alarm removed for \"" + name + "\"."}
                : Outcome{false, "\"" + name + "\" not found."};
        });
    });

    svr.Post("/setstreak", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            auto v = opt_field(req, "streak");
            if (!v) return {false, "Enter a streak like daily or 3d."};
            auto sc = parse_streak(*v);
            if (!sc) return {false, "Invalid streak — try daily, weekly, monthly, 3d."};
            return t.setstreak(name, *sc)
                ? Outcome{true, std::format("Streak for \"{}\" set ({}).", name, format_streak_label(*sc))}
                : Outcome{false, "\"" + name + "\" not found."};
        });
    });

    svr.Post("/delstreak", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            return t.delstreak(name)
                ? Outcome{true, "Streak removed for \"" + name + "\"."}
                : Outcome{false, "\"" + name + "\" not found."};
        });
    });

    svr.Post("/setunit", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            auto v = opt_field(req, "unit");
            if (!v) return {false, "Enter a unit like km or pages."};
            return t.setunit(name, *v)
                ? Outcome{true, std::format("Unit for \"{}\" set to {}.", name, *v)}
                : Outcome{false, "\"" + name + "\" not found."};
        });
    });

    svr.Post("/delunit", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            return t.delunit(name)
                ? Outcome{true, "Unit removed for \"" + name + "\"."}
                : Outcome{false, "\"" + name + "\" not found."};
        });
    });

    svr.Post("/settarget", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            auto v = opt_field(req, "target");
            if (!v) return {false, "Enter a target number."};
            auto target = parse_amount(*v);
            if (!target) return {false, "Invalid target — expected a non-negative number."};
            return t.settarget(name, *target)
                ? Outcome{true, std::format("Target for \"{}\" set to {}.", name, format_amount(*target))}
                : Outcome{false, "\"" + name + "\" not found."};
        });
    });

    svr.Post("/deltarget", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            return t.deltarget(name)
                ? Outcome{true, "Target removed for \"" + name + "\"."}
                : Outcome{false, "\"" + name + "\" not found."};
        });
    });

    std::string url = std::format("http://127.0.0.1:{}/", port);
    std::println("overdue dashboard running at {}", url);
    std::println("Press Ctrl+C to stop.");
    std::fflush(stdout); // surface the URL even when stdout is redirected/piped

    // Open the browser once the listener is actually accepting connections, so
    // the first request doesn't race ahead of bind() and get refused.
    std::thread opener([&] {
        for (int i = 0; i < 250 && !svr.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (svr.is_running()) open_browser(url);
    });
    opener.detach();

    // Loopback only — never expose the tracker on the network.
    if (!svr.listen("127.0.0.1", port))
        std::println(stderr, "Failed to bind to 127.0.0.1:{}. Is the port already in use?", port);
}
