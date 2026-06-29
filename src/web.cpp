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

  .head-right{display:flex; align-items:center; gap:1rem;}
  .nav{display:flex; gap:.25rem; background:var(--surface); border:1px solid var(--border); border-radius:10px; padding:.25rem;}
  .navlink{padding:.35rem .75rem; border-radius:7px; font-size:.84rem; font-weight:600; color:var(--muted); text-decoration:none; transition:.15s;}
  .navlink:hover{color:var(--text);}
  .navlink.active{background:var(--surface-2); color:var(--text);}
  @media(max-width:560px){.tagline{display:none}}

  .panel{background:var(--surface); border:1px solid var(--border); border-radius:var(--radius); padding:1.1rem 1.2rem; margin-bottom:1rem;}
  .panel-title{font-size:.76rem; text-transform:uppercase; letter-spacing:.07em; color:var(--faint); margin-bottom:.95rem;}

  .chart{display:flex; align-items:flex-end; gap:.35rem; height:150px;}
  .chart .col{flex:1; display:flex; flex-direction:column; justify-content:flex-end; align-items:center; gap:.35rem; height:100%;}
  .bar-v{width:72%; min-height:3px; border-radius:5px 5px 0 0; background:linear-gradient(180deg,var(--accent),var(--accent-2));}
  .chart .col:hover .bar-v{filter:brightness(1.25);}
  .cl{font-size:.7rem; color:var(--faint); font-variant-numeric:tabular-nums;}

  .highlights{display:grid; grid-template-columns:repeat(auto-fit,minmax(165px,1fr)); gap:.8rem; margin-bottom:1rem;}
  .hl{background:var(--surface); border:1px solid var(--border); border-radius:var(--radius); padding:.9rem 1rem;}
  .hl-l{font-size:.7rem; text-transform:uppercase; letter-spacing:.05em; color:var(--faint);}
  .hl-n{font-size:1.05rem; font-weight:650; margin:.15rem 0; letter-spacing:-.01em;}
  .hl-d{font-size:.85rem; color:var(--muted);}

  .hrow{display:flex; align-items:center; justify-content:space-between; gap:.8rem; padding:.6rem 0; border-bottom:1px solid var(--border);}
  .hrow:last-child{border-bottom:none;}
  .hname{font-weight:600;}
  .chips{display:flex; flex-wrap:wrap; gap:.4rem; justify-content:flex-end;}
  .chip{font-size:.75rem; padding:.18rem .55rem; border-radius:999px; white-space:nowrap;
        background:var(--surface-2); border:1px solid var(--border); color:var(--muted);}
  .chip.streak{color:#ffd08a; border-color:rgba(255,208,138,.25);}
  .chip.trend.up{color:var(--ok); border-color:rgba(70,211,154,.3);}
  .chip.trend.down{color:var(--danger); border-color:rgba(255,107,107,.3);}

  /* clickable entry names linking to the detail page */
  .name a{color:inherit; text-decoration:none; border-bottom:1px dashed transparent; transition:.15s;}
  .name a:hover{color:var(--accent); border-bottom-color:rgba(124,140,255,.5);}
  .hname a{color:inherit; text-decoration:none;}
  .hname a:hover{color:var(--accent);}

  /* detail page */
  .detail-head{margin-bottom:1.2rem;}
  .back{color:var(--muted); font-size:.85rem; text-decoration:none;}
  .back:hover{color:var(--text);}
  .detail-title{font-size:1.9rem; font-weight:700; letter-spacing:-.02em; margin:.55rem 0 .55rem;}
  .dstats{display:grid; grid-template-columns:repeat(auto-fit,minmax(140px,1fr)); gap:.8rem; margin-bottom:1.4rem;}

  /* contribution-style calendar heatmap */
  .cal-scroll{overflow-x:auto; padding-bottom:.2rem;}
  .cal-months{display:flex; gap:3px; margin-bottom:4px;}
  .cal-m{width:13px; flex:0 0 13px; font-size:.62rem; color:var(--faint); white-space:nowrap; overflow:visible;}
  .cal{display:flex; gap:3px;}
  .cal-col{display:flex; flex-direction:column; gap:3px;}
  .cal-cell{width:13px; height:13px; flex:0 0 13px; border-radius:3px;
        background:var(--surface-2); border:1px solid var(--border);}
  .cal-cell.empty{background:transparent; border-color:transparent;}
  .cal-cell.l1{background:rgba(124,140,255,.28); border-color:transparent;}
  .cal-cell.l2{background:rgba(124,140,255,.5);  border-color:transparent;}
  .cal-cell.l3{background:rgba(124,140,255,.74); border-color:transparent;}
  .cal-cell.l4{background:var(--accent); border-color:transparent;}
  .cal-legend{display:flex; align-items:center; justify-content:flex-end; gap:.35rem;
        color:var(--faint); font-size:.72rem; margin-top:.6rem;}
  .cal-legend .cal-cell{width:11px; height:11px; flex:0 0 11px;}
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
  setInterval(function(){ if(!busy()) location.replace(location.pathname); }, 30000);

  // Drop the flash from the URL so a manual reload won't replay it, then fade it out.
  if(location.search) history.replaceState(null, "", location.pathname);
  var t = document.querySelector(".toast");
  if(t) setTimeout(function(){ t.classList.add("hide"); }, 3500);
})();
</script>
)HTML";

// Brand + nav, shared by the dashboard and the stats page. The sync indicator
// is a data-since span the client script ticks live.
static std::string render_header(const char* active) {
    const char* home  = std::string(active) == "home"  ? " active" : "";
    const char* stats = std::string(active) == "stats" ? " active" : "";
    return std::format(
        "<header><div class=\"brand\"><div class=\"logo\">⏱</div>"
        "<div><h1>overdue</h1><p class=\"tagline\">how long has it been?</p></div></div>"
        "<div class=\"head-right\">"
        "<nav class=\"nav\"><a href=\"/\" class=\"navlink{}\">Dashboard</a>"
        "<a href=\"/stats\" class=\"navlink{}\">Stats</a></nav>"
        "<div class=\"sync\">synced <span data-since=\"{}\">0s</span> ago</div>"
        "</div></header>\n",
        home, stats, to_epoch(now()));
}

// Closing markup shared by every page (footer + live-tick script + tags).
static std::string page_tail() {
    return std::string(
        "<footer>overdue · localhost dashboard · loopback only</footer>\n</div>\n")
        + PAGE_SCRIPT + "</body>\n</html>\n";
}

// Link to an entry's detail page; the name is the query, so url-encode it.
static std::string activity_url(const std::string& name) {
    return "/activity?name=" + url_encode(name);
}

static std::string hidden_input(const char* name, const std::string& value) {
    return std::format("<input type=\"hidden\" name=\"{}\" value=\"{}\">", name, html_escape(value));
}

// A form carrying the CSRF token, a preset entry name, and (optionally) the page
// to return to after the action — empty `next` falls back to the dashboard.
static std::string make_form(const std::string& token, const std::string& next,
                             const char* action, const std::string& name,
                             const std::string& inner) {
    return std::format(
        "<form class=\"row\" method=\"post\" action=\"{}\">{}{}{}{}</form>",
        action, hidden_input("token", token), hidden_input("name", name),
        next.empty() ? std::string() : hidden_input("next", next), inner);
}

// Progress bar (or a bare total) for entries that carry amounts / a target.
static std::string progress_bar(const Activity& a) {
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
}

// Shared manage controls (alarm/streak are habit-only; unit/target/delete for all).
static std::string manage_controls(const std::string& token, const std::string& next,
                                   const Activity& a, bool is_habit) {
    auto F = [&](const char* action, const std::string& inner) {
        return make_form(token, next, action, a.name, inner);
    };
    std::string m = "<details class=\"manage\"><summary>Manage ▾</summary><div class=\"manage-grid\">";
    if (is_habit) {
        m += F("/setalarm",
            "<input class=\"in sm\" name=\"dur\" placeholder=\"3d\"><button class=\"btn ghost\">Set alarm</button>");
        if (a.alert_after)
            m += F("/delalarm", "<button class=\"btn ghost\">Clear alarm</button>");
        m += F("/setstreak",
            "<input class=\"in sm\" name=\"streak\" placeholder=\"daily\"><button class=\"btn ghost\">Set streak</button>");
        if (a.streak)
            m += F("/delstreak", "<button class=\"btn ghost\">Clear streak</button>");
    }
    m += F("/setunit",
        "<input class=\"in sm\" name=\"unit\" placeholder=\"km\"><button class=\"btn ghost\">Set unit</button>");
    if (a.unit)
        m += F("/delunit", "<button class=\"btn ghost\">Clear unit</button>");
    m += F("/settarget",
        "<input class=\"in sm\" name=\"target\" placeholder=\"100\"><button class=\"btn ghost\">Set target</button>");
    if (a.target)
        m += F("/deltarget", "<button class=\"btn ghost\">Clear target</button>");
    // Delete always returns to the dashboard (the detail page would 404 after),
    // and confirms client-side. Message stays generic to avoid escaping names into JS.
    m += std::format(
        "<form class=\"row\" method=\"post\" action=\"/delete\" "
        "onsubmit=\"return confirm('Delete this entry and all its history?')\">{}{}"
        "<button class=\"btn danger\">Delete</button></form>",
        hidden_input("token", token), hidden_input("name", a.name));
    m += "</div></details>";
    return m;
}

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

    // Dashboard forms post here and bounce back to the dashboard (empty `next`).
    auto nform = [&](const char* action, const std::string& name, const std::string& inner) {
        return make_form(token, "", action, name, inner);
    };

    std::string out = PAGE_HEAD;

    if (flash)
        out += std::format("<div class=\"toast {}\">{}</div>\n",
                           flash->first ? "ok" : "err", html_escape(flash->second));

    out += render_header("home");

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
                "<div class=\"entry-head\"><div class=\"name\"><a href=\"{}\">{}</a></div><div class=\"badges\">{}</div></div>"
                "<div class=\"timer{}\" data-since=\"{}\">{}</div>"
                "<div class=\"sub\">last done {}</div>"
                "{}"
                "<div class=\"actions\">{}{}</div>"
                "{}</div>\n",
                od ? " over" : "",
                activity_url(a.name), html_escape(a.name), badges,
                od ? " over" : "", to_epoch(last_done(a)), format_elapsed(last_done(a)),
                format_datetime(last_done(a)),
                progress_bar(a),
                log_form, unlog_form,
                manage_controls(token, "", a, /*is_habit=*/true));
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
                "<div class=\"entry-head\"><div class=\"name\"><a href=\"{}\">{}</a></div><div class=\"badges\">{}</div></div>"
                "{}"
                "<div class=\"sub\">{}</div>"
                "{}"
                "<div class=\"actions\">{}</div>"
                "{}</div>\n",
                activity_url(a.name), html_escape(a.name), badges,
                timer,
                html_escape(sub),
                progress_bar(a),
                actions,
                manage_controls(token, "", a, /*is_habit=*/false));
        }
    }

    out += page_tail();
    return out;
}

static std::string render_stats_page(const std::filesystem::path& data_path) {
    Tracker tracker{data_path};
    const auto& all = tracker.all();
    auto gs      = compute_global(all);
    auto habits  = tracker.habits();
    auto tasks   = tracker.tasks(/*include_done=*/true);
    auto overdue = tracker.overdue_activities();

    std::string out = PAGE_HEAD;
    out += render_header("stats");

    // Overview
    out += "<div class=\"stats\">\n";
    out += std::format("<div class=\"stat\"><div class=\"n\">{}</div><div class=\"l\">Habits</div></div>\n", gs.habit_count);
    out += std::format("<div class=\"stat\"><div class=\"n\">{}</div><div class=\"l\">Total logs</div></div>\n", gs.total_logs);
    out += std::format("<div class=\"stat\"><div class=\"n\">{} / {}</div><div class=\"l\">Tasks done</div></div>\n", gs.task_done, gs.task_total);
    out += std::format("<div class=\"stat{}\"><div class=\"n\">{}</div><div class=\"l\">Overdue</div></div>\n",
                       overdue.empty() ? "" : " warn", overdue.size());
    out += "</div>\n";

    if (all.empty()) {
        out += "<p class=\"empty\">No data yet — <a href=\"/\">add a habit or task</a> to get started.</p>\n";
        out += page_tail();
        return out;
    }

    // Daily activity chart — log counts per local day over the last 14 days.
    {
        using namespace std::chrono;
        auto today = to_local_days(now());
        std::map<local_days, int> per_day;
        for (const auto& a : all)
            for (const auto& e : a.logs)
                per_day[to_local_days(e.when)]++;

        const int DAYS = 14;
        int maxc = 1;
        std::vector<std::pair<local_days, int>> series;
        for (int i = 0; i < DAYS; ++i) {
            auto d = today - days{DAYS - 1 - i};
            int c = 0;
            if (auto it = per_day.find(d); it != per_day.end()) c = it->second;
            series.push_back({d, c});
            maxc = std::max(maxc, c);
        }

        out += "<div class=\"panel\"><div class=\"panel-title\">Activity · last 14 days</div><div class=\"chart\">";
        for (const auto& [d, c] : series) {
            year_month_day ymd{d};
            out += std::format(
                "<div class=\"col\" title=\"{:04}-{:02}-{:02}: {} log(s)\">"
                "<div class=\"bar-v\" style=\"height:{:.0f}%\"></div>"
                "<div class=\"cl\">{}</div></div>",
                int(ymd.year()), unsigned(ymd.month()), unsigned(ymd.day()), c,
                100.0 * c / maxc, unsigned(ymd.day()));
        }
        out += "</div></div>\n";
    }

    // Highlights from the global aggregate
    {
        auto hcard = [](const char* label, const std::string& name, const std::string& detail) {
            return std::format("<div class=\"hl\"><div class=\"hl-l\">{}</div>"
                "<div class=\"hl-n\">{}</div><div class=\"hl-d\">{}</div></div>",
                label, html_escape(name), html_escape(detail));
        };
        std::string hl;
        if (gs.most_consistent)
            hl += hcard("Most consistent", gs.most_consistent->name,
                gs.most_consistent->avg_interval
                    ? "every " + format_duration(*gs.most_consistent->avg_interval) : "only 1 log");
        if (gs.most_neglected)
            hl += hcard("Most neglected", gs.most_neglected->name,
                gs.most_neglected->avg_interval
                    ? "every " + format_duration(*gs.most_neglected->avg_interval) : "only 1 log");
        if (gs.most_logged)
            hl += hcard("Most logged", gs.most_logged->name,
                std::format("{} logs", gs.most_logged->log_count));
        if (gs.best_streak && gs.best_streak->streak > 0 && gs.best_streak->streak_config)
            hl += hcard("Best streak", gs.best_streak->name,
                std::format("{} ({})", gs.best_streak->streak,
                    format_streak_label(*gs.best_streak->streak_config)));
        if (!hl.empty())
            out += "<div class=\"highlights\">" + hl + "</div>\n";
    }

    // Per-habit breakdown
    if (!habits.empty()) {
        out += "<div class=\"panel\"><div class=\"panel-title\">Habits</div>";
        for (const auto& a : habits) {
            std::string chips = std::format("<span class=\"chip\">{} logs</span>", a.logs.size());
            if (a.streak)
                chips += std::format("<span class=\"chip streak\">🔥 {} · {}</span>",
                    compute_streak(a), format_streak_label(*a.streak));
            if (auto iv = avg_interval(a))
                chips += std::format("<span class=\"chip\">every {}</span>", format_duration(*iv));
            if (auto qs = quantity_stats(a)) {
                std::string u = a.unit ? " " + *a.unit : "";
                chips += std::format("<span class=\"chip\">{}{} total</span>", format_amount(qs->total), u);
                double l = qs->last7, p = qs->prev7;
                if (l > 0 || p > 0) {
                    const char* cls = l > p ? "up" : (l < p ? "down" : "flat");
                    const char* arr = l > p ? "↑" : (l < p ? "↓" : "→");
                    chips += std::format("<span class=\"chip trend {}\">{} 7d {}{}</span>",
                        cls, arr, format_amount(l), u);
                }
            }
            out += std::format("<div class=\"hrow\"><div class=\"hname\"><a href=\"{}\">{}</a></div>"
                "<div class=\"chips\">{}</div></div>", activity_url(a.name), html_escape(a.name), chips);
        }
        out += "</div>\n";
    }

    // Tasks
    if (!tasks.empty()) {
        out += std::format("<div class=\"panel\"><div class=\"panel-title\">Tasks · {} done / {} total</div>",
            gs.task_done, gs.task_total);
        for (const auto& a : tasks) {
            std::string chip = a.completed_at
                ? std::format("<span class=\"chip\" style=\"color:var(--ok); border-color:rgba(70,211,154,.3)\">✓ {}</span>",
                    format_datetime(*a.completed_at))
                : std::format("<span class=\"chip\">pending <span data-since=\"{}\">{}</span></span>",
                    to_epoch(a.logs.front().when), format_elapsed(a.logs.front().when));
            out += std::format("<div class=\"hrow\"><div class=\"hname\"><a href=\"{}\">{}</a></div>"
                "<div class=\"chips\">{}</div></div>", activity_url(a.name), html_escape(a.name), chip);
        }
        out += "</div>\n";
    }

    out += page_tail();
    return out;
}

// GitHub-style contribution heatmap over the last ~6 months. Columns are weeks
// (oldest left), rows are Mon→Sun. Intensity tracks the day's amount when the
// activity logs quantities, otherwise the number of logs that day.
static std::string render_heatmap(const Activity& a) {
    using namespace std::chrono;

    struct DayAgg { int count = 0; double amount = 0; bool has_amount = false; };
    std::map<local_days, DayAgg> per;
    double best_amount = 0;
    bool any_amount = false;
    for (const auto& e : a.logs) {
        auto& x = per[to_local_days(e.when)];
        ++x.count;
        if (e.amount) { x.amount += *e.amount; x.has_amount = true; }
    }
    for (const auto& [d, x] : per) {
        best_amount = std::max(best_amount, x.amount);
        if (x.has_amount) any_amount = true;
    }

    auto level = [&](const DayAgg& x) -> int {
        if (x.count == 0) return 0;
        if (any_amount && best_amount > 0 && x.has_amount) {
            double r = x.amount / best_amount;
            if (r < 0.25) return 1;
            if (r < 0.50) return 2;
            if (r < 0.75) return 3;
            return 4;
        }
        return std::min(4, x.count);
    };

    const int WEEKS = 26;
    auto today      = to_local_days(now());
    auto end_week   = week_start(today);
    auto start_week = end_week - days{7 * (WEEKS - 1)};
    std::string unit = a.unit ? " " + *a.unit : "";

    static const char* MON[] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::string months, cols;
    int prev_month = -1;
    for (int w = 0; w < WEEKS; ++w) {
        auto col_start = start_week + days{7 * w};
        int mo = static_cast<int>(unsigned(year_month_day{col_start}.month()));
        // Label a column only when its month differs from the previous one; the
        // text overflows rightward across the (empty) following labels.
        months += (mo != prev_month)
            ? std::format("<span class=\"cal-m\">{}</span>", MON[mo])
            : "<span class=\"cal-m\"></span>";
        prev_month = mo;

        cols += "<div class=\"cal-col\">";
        for (int dow = 0; dow < 7; ++dow) {
            auto day = col_start + days{dow};
            if (day > today) { cols += "<div class=\"cal-cell empty\"></div>"; continue; }
            DayAgg x;
            if (auto it = per.find(day); it != per.end()) x = it->second;
            int lv = level(x);
            year_month_day ymd{day};
            std::string tip = std::format("{:04}-{:02}-{:02}",
                int(ymd.year()), unsigned(ymd.month()), unsigned(ymd.day()));
            if (x.count == 0) tip += " · no log";
            else {
                tip += std::format(" · {} log{}", x.count, x.count > 1 ? "s" : "");
                if (x.has_amount) tip += " · " + format_amount(x.amount) + unit;
            }
            cols += std::format("<div class=\"cal-cell{}\" title=\"{}\"></div>",
                lv ? std::format(" l{}", lv) : "", tip);
        }
        cols += "</div>";
    }

    std::string out = "<div class=\"panel\"><div class=\"panel-title\">Calendar · last 26 weeks</div>";
    out += "<div class=\"cal-scroll\"><div class=\"cal-months\">" + months
         + "</div><div class=\"cal\">" + cols + "</div></div>";
    out += "<div class=\"cal-legend\">Less"
           "<span class=\"cal-cell\"></span><span class=\"cal-cell l1\"></span>"
           "<span class=\"cal-cell l2\"></span><span class=\"cal-cell l3\"></span>"
           "<span class=\"cal-cell l4\"></span>More</div>";
    out += "</div>\n";
    return out;
}

static std::string render_activity_page(const std::filesystem::path& data_path,
                                        const std::string& token,
                                        const std::string& name,
                                        std::optional<Outcome> flash) {
    Tracker tracker{data_path};
    auto found = tracker.find(name);

    std::string out = PAGE_HEAD;
    if (flash)
        out += std::format("<div class=\"toast {}\">{}</div>\n",
                           flash->first ? "ok" : "err", html_escape(flash->second));
    out += render_header("");

    if (!found) {
        out += std::format(
            "<div class=\"detail-head\"><a class=\"back\" href=\"/\">← Dashboard</a></div>"
            "<p class=\"empty\">No activity named \"{}\". It may have been deleted.</p>\n",
            html_escape(name));
        out += page_tail();
        return out;
    }

    const Activity& a = *found;
    const bool is_habit = a.type == ActivityType::Habit;
    const std::string self = activity_url(a.name);
    const std::string unit = a.unit ? " " + *a.unit : "";

    bool od = false;
    for (const auto& o : tracker.overdue_activities())
        if (o.name == a.name) { od = true; break; }

    // Hero: back link, title, badges
    std::string badges;
    if (od) badges += "<span class=\"badge danger\">overdue</span>";
    if (is_habit && a.streak)
        badges += std::format("<span class=\"badge streak\">🔥 {} · {}</span>",
            compute_streak(a), format_streak_label(*a.streak));
    if (is_habit && a.alert_after)
        badges += std::format("<span class=\"badge\">⏰ {}</span>", format_duration(*a.alert_after));
    if (!is_habit)
        badges += a.completed_at ? "<span class=\"badge ok\">done</span>"
                                 : "<span class=\"badge\">task</span>";
    if (a.unit)   badges += std::format("<span class=\"badge\">unit: {}</span>", html_escape(*a.unit));
    if (a.target) badges += std::format("<span class=\"badge\">target: {}</span>", format_amount(*a.target));

    out += std::format(
        "<div class=\"detail-head\"><a class=\"back\" href=\"/\">← Dashboard</a>"
        "<div class=\"detail-title\">{}</div>"
        "<div class=\"badges\" style=\"justify-content:flex-start\">{}</div></div>\n",
        html_escape(a.name), badges);

    // Big live timer / status
    if (is_habit || !a.completed_at) {
        auto since = is_habit ? last_done(a) : a.logs.front().when;
        out += std::format("<div class=\"timer{}\" data-since=\"{}\">{}</div>",
            od ? " over" : "", to_epoch(since), format_elapsed(since));
        out += std::format("<div class=\"sub\" style=\"margin-bottom:1.3rem\">{} {}</div>\n",
            is_habit ? "last done" : "added", format_datetime(since));
    } else {
        out += "<div class=\"timer done\">✓ completed</div>";
        out += std::format("<div class=\"sub\" style=\"margin-bottom:1.3rem\">done {} · added {}</div>\n",
            format_datetime(*a.completed_at), format_datetime(a.logs.front().when));
    }

    // Stat cards (auto-fit, so any count looks tidy)
    {
        std::string cards = std::format(
            "<div class=\"stat\"><div class=\"n\">{}</div><div class=\"l\">Total logs</div></div>", a.logs.size());
        if (is_habit && a.streak)
            cards += std::format(
                "<div class=\"stat\"><div class=\"n\">🔥 {}</div><div class=\"l\">Current streak</div></div>",
                compute_streak(a));
        if (auto iv = avg_interval(a))
            cards += std::format(
                "<div class=\"stat\"><div class=\"n\">{}</div><div class=\"l\">Avg interval</div></div>",
                format_duration(*iv));
        if (auto qs = quantity_stats(a))
            cards += std::format(
                "<div class=\"stat\"><div class=\"n\">{}{}</div><div class=\"l\">Total logged</div></div>",
                format_amount(qs->total), unit);
        out += "<div class=\"dstats\">" + cards + "</div>\n";
    }

    // Progress toward a target
    if (a.target)
        out += "<div class=\"panel\"><div class=\"panel-title\">Progress</div>" + progress_bar(a) + "</div>\n";

    // Calendar heatmap
    out += render_heatmap(a);

    // Quantity breakdown
    if (auto qs = quantity_stats(a)) {
        auto cell = [](const char* l, const std::string& v) {
            return std::format("<div class=\"hl\"><div class=\"hl-l\">{}</div><div class=\"hl-n\">{}</div></div>", l, v);
        };
        std::string g;
        g += cell("Total",      format_amount(qs->total) + unit);
        g += cell("Avg / log",  format_amount(qs->avg_per_log) + unit);
        g += cell("Avg / day",  format_amount(qs->avg_per_day) + unit);
        g += cell("Best day",   format_amount(qs->best_day) + unit);
        g += cell("Max single", format_amount(qs->max_single) + unit);
        double l = qs->last7, p = qs->prev7;
        const char* arr = l > p ? "↑" : (l < p ? "↓" : "→");
        g += cell("Last 7 days", std::format("{} {}{}", arr, format_amount(l), unit));
        out += "<div class=\"panel\"><div class=\"panel-title\">Quantity</div>"
               "<div class=\"highlights\" style=\"margin-bottom:0\">" + g + "</div></div>\n";
    }

    // History — newest first, capped so a long log doesn't blow up the page
    {
        out += std::format("<div class=\"panel\"><div class=\"panel-title\">History · {} log{}</div>",
            a.logs.size(), a.logs.size() == 1 ? "" : "s");
        const size_t CAP = 30;
        size_t shown = 0;
        for (auto it = a.logs.rbegin(); it != a.logs.rend() && shown < CAP; ++it, ++shown) {
            std::string amt = it->amount
                ? std::format("<span class=\"chip\">{}{}</span>", format_amount(*it->amount), unit) : "";
            out += std::format(
                "<div class=\"hrow\"><div class=\"hname\" style=\"font-weight:500\">{}</div>"
                "<div class=\"chips\">{}</div></div>",
                html_escape(format_datetime(it->when)), amt);
        }
        if (a.logs.size() > CAP)
            out += std::format("<div class=\"hl-d\" style=\"margin-top:.7rem\">… and {} earlier</div>",
                a.logs.size() - CAP);
        out += "</div>\n";
    }

    // Actions — same controls as the dashboard, but they return here via `next`
    {
        std::string actions;
        if (is_habit) {
            actions += make_form(token, self, "/log", a.name,
                "<input class=\"in sm\" name=\"amount\" placeholder=\"amt\">"
                "<input class=\"in sm\" name=\"ago\" placeholder=\"ago\">"
                "<button class=\"btn primary\">Log</button>");
        } else if (!a.completed_at) {
            actions += make_form(token, self, "/done", a.name, "<button class=\"btn primary\">Mark done</button>");
            actions += make_form(token, self, "/log", a.name,
                "<input class=\"in sm\" name=\"amount\" placeholder=\"amt\"><button class=\"btn ghost\">Log progress</button>");
        }
        if (a.logs.size() > 1)
            actions += make_form(token, self, "/unlog", a.name, "<button class=\"btn ghost\">Unlog</button>");

        out += "<div class=\"panel\"><div class=\"panel-title\">Actions</div>"
               "<div class=\"actions\">" + actions + "</div>"
             + manage_controls(token, self, a, is_habit) + "</div>\n";
    }

    out += page_tail();
    return out;
}

void run_web(const std::filesystem::path& data_path, int port) {
    httplib::Server svr;
    const std::string token = make_token();
    std::mutex write_mtx; // serialize the load-modify-save in mutating handlers

    // Post/Redirect/Get: bounce back with a flash message. `next` lets a detail
    // page return to itself; we only honour our own paths to avoid open redirects.
    auto finish = [](httplib::Response& res, bool ok, const std::string& msg,
                     const std::string& next) {
        std::string base = (next == "/" || next.rfind("/activity?", 0) == 0) ? next : "/";
        char sep = base.find('?') == std::string::npos ? '?' : '&';
        res.status = 303;
        res.set_header("Location",
            std::format("{}{}{}={}", base, sep, ok ? "ok" : "err", url_encode(msg)));
    };

    // Reject any POST without the matching session token (blocks cross-site posts).
    auto guard = [&](const httplib::Request& req, httplib::Response& res) {
        if (req.get_param_value("token") == token) return true;
        res.status = 403;
        res.set_content("Forbidden: invalid or missing token. Reload the dashboard.", "text/plain");
        return false;
    };

    // Run a mutation under the lock and turn its Outcome into a redirect.
    auto run = [&](const httplib::Request& req, httplib::Response& res, auto&& fn) {
        std::string next = req.has_param("next") ? req.get_param_value("next") : "/";
        std::lock_guard<std::mutex> lock(write_mtx);
        try {
            Tracker t{data_path};
            auto [ok, msg] = fn(t);
            finish(res, ok, msg, next);
        } catch (const std::exception& e) {
            finish(res, false, std::string("Error: ") + e.what(), next);
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

    svr.Get("/stats", [&](const httplib::Request&, httplib::Response& res) {
        try {
            res.set_content(render_stats_page(data_path), "text/html; charset=utf-8");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::format("Error: {}", e.what()), "text/plain");
        }
    });

    svr.Get("/activity", [&](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.get_param_value("name");
        std::optional<Outcome> flash;
        if (req.has_param("ok"))  flash = Outcome{true,  req.get_param_value("ok")};
        else if (req.has_param("err")) flash = Outcome{false, req.get_param_value("err")};
        try {
            res.set_content(render_activity_page(data_path, token, name, flash),
                            "text/html; charset=utf-8");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::format("Error: {}", e.what()), "text/plain");
        }
    });

    svr.Post("/add", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(req, res, [&](Tracker& t) -> Outcome {
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
        run(req, res, [&](Tracker& t) -> Outcome {
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
        run(req, res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            return t.unlog(name)
                ? Outcome{true, "Cancelled last log for \"" + name + "\"."}
                : Outcome{false, "\"" + name + "\" not found or no log to restore."};
        });
    });

    svr.Post("/done", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(req, res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            return t.done(name)
                ? Outcome{true, "\"" + name + "\" completed."}
                : Outcome{false, "\"" + name + "\" not found or is not a task."};
        });
    });

    svr.Post("/delete", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(req, res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            return t.remove(name)
                ? Outcome{true, "Removed \"" + name + "\"."}
                : Outcome{false, "\"" + name + "\" not found."};
        });
    });

    svr.Post("/setalarm", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(req, res, [&](Tracker& t) -> Outcome {
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
        run(req, res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            return t.delalarm(name)
                ? Outcome{true, "Alarm removed for \"" + name + "\"."}
                : Outcome{false, "\"" + name + "\" not found."};
        });
    });

    svr.Post("/setstreak", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(req, res, [&](Tracker& t) -> Outcome {
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
        run(req, res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            return t.delstreak(name)
                ? Outcome{true, "Streak removed for \"" + name + "\"."}
                : Outcome{false, "\"" + name + "\" not found."};
        });
    });

    svr.Post("/setunit", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(req, res, [&](Tracker& t) -> Outcome {
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
        run(req, res, [&](Tracker& t) -> Outcome {
            std::string name = req.get_param_value("name");
            return t.delunit(name)
                ? Outcome{true, "Unit removed for \"" + name + "\"."}
                : Outcome{false, "\"" + name + "\" not found."};
        });
    });

    svr.Post("/settarget", [&](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) return;
        run(req, res, [&](Tracker& t) -> Outcome {
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
        run(req, res, [&](Tracker& t) -> Outcome {
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
