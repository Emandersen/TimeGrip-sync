#include "report.hpp"
#include "env.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

PayConfig pay_config_from_env() {
    auto get_d = [](const char* key, double def) -> double {
        auto s = get_env(key);
        if (s.empty()) return def;
        try { return std::stod(s); } catch (...) { return def; }
    };
    auto get_i = [](const char* key, int def) -> int {
        auto s = get_env(key);
        if (s.empty()) return def;
        try { return std::stoi(s); } catch (...) { return def; }
    };
    PayConfig c;
    c.hourly_rate          = get_d("HOURLY_RATE",          0.0);
    c.evening_supplement   = get_d("EVENING_SUPPLEMENT",   0.0);
    c.saturday_supplement  = get_d("SATURDAY_SUPPLEMENT",  0.0);
    c.sunday_supplement    = get_d("SUNDAY_SUPPLEMENT",    0.0);
    c.evening_cutoff_hour  = get_i("EVENING_CUTOFF_HOUR",  18);
    c.am_bidrag_pct        = get_d("AM_BIDRAG_PCT",        8.0);
    c.tax_pct              = get_d("TAX_PCT",              0.0);
    c.employee_pension_pct = get_d("EMPLOYEE_PENSION_PCT", 0.0);
    c.employer_pension_pct = get_d("EMPLOYER_PENSION_PCT", 0.0);
    c.atp_dkk              = get_d("ATP_DKK",              0.0);
    c.klub_dkk             = get_d("KLUB_DKK",             0.0);
    c.fritvalg_pct         = get_d("FRITVALG_PCT",         0.0);
    c.feriefri_pct         = get_d("FERIEFRI_PCT",         0.0);
    c.advance_notice_days  = get_i("ADVANCE_NOTICE_DAYS",  14);
    return c;
}

static bool parse_dmyyyy(const std::string& s, int& d, int& mo, int& y) {
    if (s.size() != 10 || s[2] != '-' || s[5] != '-') return false;
    try {
        d  = std::stoi(s.substr(0, 2));
        mo = std::stoi(s.substr(3, 2));
        y  = std::stoi(s.substr(6, 4));
        return true;
    } catch (...) { return false; }
}

// 0=Sun 1=Mon … 6=Sat
static int day_of_week(int d, int mo, int y) {
    std::tm t{};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_isdst = -1;
    mktime(&t);
    return t.tm_wday;
}

// "YYYY-MM-DDTHH:MM:…" → minutes since midnight, -1 on fail
static int hhmm_to_min(const std::string& s) {
    if (s.size() < 16) return -1;
    try { return std::stoi(s.substr(11,2)) * 60 + std::stoi(s.substr(14,2)); }
    catch (...) { return -1; }
}

// Lønperiode key {pay_month, pay_year}: period is [16th prev, 15th pay_month]
static std::pair<int,int> lonperiode_key(int d, int mo, int y) {
    if (d >= 16) {
        int pm = mo + 1, py = y;
        if (pm > 12) { pm = 1; ++py; }
        return {pm, py};
    }
    return {mo, y};
}

static const char* DK_MONTHS[] = {
    "","januar","februar","marts","april","maj","juni",
    "juli","august","september","oktober","november","december"
};
static const char* DK_DAYS[] = {"s\xc3\xb8n","man","tir","ons","tor","fre","l\xc3\xb8r"};

struct ShiftPayInfo {
    double worked_h    = 0;
    double evening_h   = 0;
    double base_pay    = 0;
    double evening_pay = 0;
    double weekend_pay = 0;
    double gross       = 0;
    bool   is_absence  = false;
    bool   is_vacation = false;
};

static ShiftPayInfo calc_shift_pay(const Shift& s, int d, int mo, int y,
                                    const PayConfig& cfg) {
    ShiftPayInfo p;
    if (s.has_absence) {
        std::string cap = s.absence.type_caption;
        for (auto& c : cap) c = static_cast<char>(tolower(c));
        p.is_absence = true;
        if (cap.find("ferie") != std::string::npos ||
            cap.find("vacation") != std::string::npos)
            p.is_vacation = true;
        if (!s.worktime_id || s.duration == 0) return p;
    }
    if (s.duration <= 0) return p;

    int worked_min = s.duration - s.pause;
    if (worked_min <= 0) worked_min = s.duration;
    p.worked_h = worked_min / 60.0;
    p.base_pay = p.worked_h * cfg.hourly_rate;

    int wday = day_of_week(d, mo, y);
    if (wday == 0)
        p.weekend_pay = p.worked_h * cfg.sunday_supplement;
    else if (wday == 6)
        p.weekend_pay = p.worked_h * cfg.saturday_supplement;

    if (wday >= 1 && wday <= 5) {
        int start_min  = hhmm_to_min(s.start_time);
        int end_min    = hhmm_to_min(s.end_time);
        int cutoff_min = cfg.evening_cutoff_hour * 60;
        if (start_min >= 0 && end_min > cutoff_min) {
            int eve_raw = end_min - std::max(start_min, cutoff_min);
            p.evening_h   = std::min(eve_raw / 60.0, p.worked_h);
            p.evening_pay = p.evening_h * cfg.evening_supplement;
        }
    }

    p.gross = p.base_pay + p.weekend_pay + p.evening_pay;
    return p;
}

static std::string fmt_dkk(double v) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(0) << std::round(v);
    std::string num = ss.str();
    bool neg = (!num.empty() && num[0] == '-');
    if (neg) num = num.substr(1);
    std::string out;
    int cnt = 0;
    for (int i = static_cast<int>(num.size()) - 1; i >= 0; --i, ++cnt) {
        if (cnt > 0 && cnt % 3 == 0) out = "." + out;
        out = num[i] + out;
    }
    return (neg ? "-" : "") + out + "\xc2\xa0kr";
}

static std::string fmt_h(double h) {
    int total = static_cast<int>(std::round(h * 60));
    int hh = total / 60, mm = total % 60;
    std::ostringstream ss;
    ss << hh << "t";
    if (mm) ss << std::setw(2) << std::setfill('0') << mm << "m";
    return ss.str();
}

static std::string fmt_time(const std::string& iso) {
    return iso.size() >= 16 ? iso.substr(11, 5) : "?";
}

static const char* CSS = R"css(
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,BlinkMacSystemFont,sans-serif;background:#f0f2f5;color:#1a1a1a;line-height:1.5}
header{background:#1a237e;color:#fff;padding:1.25rem 2rem;display:flex;align-items:baseline;gap:1.5rem;flex-wrap:wrap}
header h1{font-size:1.35rem;font-weight:700;letter-spacing:-.01em}
.updated{font-size:.82rem;opacity:.65}
main{max-width:980px;margin:1.75rem auto;padding:0 1rem;display:flex;flex-direction:column;gap:1.5rem}
.period{background:#fff;border-radius:10px;overflow:hidden;box-shadow:0 1px 4px rgba(0,0,0,.09)}
.ph{background:#1a237e;color:#fff;padding:.85rem 1.2rem;display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:.5rem}
.ph h2{font-size:1.05rem;font-weight:600}
.pay-badge{font-size:.95rem;font-weight:700;background:rgba(255,255,255,.22);padding:.3rem .85rem;border-radius:6px;white-space:nowrap;letter-spacing:.01em}
.stats{display:flex;flex-wrap:wrap;border-bottom:1px solid #ebebeb}
.stat{flex:1;min-width:110px;padding:.75rem 1.2rem;border-right:1px solid #ebebeb}
.stat:last-child{border-right:none}
.stat .lbl{font-size:.7rem;text-transform:uppercase;letter-spacing:.05em;color:#777;display:block;margin-bottom:.15rem}
.stat .val{font-size:1.05rem;font-weight:700;color:#1a237e}
.stat .val.net{color:#1b5e20}
table{width:100%;border-collapse:collapse;font-size:.875rem}
th{background:#f7f8fa;font-weight:500;color:#555;font-size:.72rem;text-transform:uppercase;letter-spacing:.05em;padding:.45rem .9rem;text-align:left;border-bottom:2px solid #e8e8e8}
td{padding:.45rem .9rem;border-bottom:1px solid #f2f2f2;vertical-align:middle}
tr:last-child td{border-bottom:none}
tr:hover td{background:#fafbff}
.num{text-align:right;font-variant-numeric:tabular-nums}
.tag{display:inline-block;font-size:.7rem;padding:.1rem .4rem;border-radius:3px;font-weight:600;vertical-align:middle;margin-right:2px}
.t-aft{background:#fff3e0;color:#bf360c}
.t-lor{background:#ede7f6;color:#4527a0}
.t-son{background:#fce4ec;color:#880e4f}
.t-abs{background:#e8f5e9;color:#1b5e20;font-weight:400}
details.breakdown{border-top:1px solid #ebebeb}
details.breakdown summary{cursor:pointer;padding:.7rem 1.2rem;font-size:.83rem;font-weight:600;color:#1a237e;user-select:none;list-style:none}
details.breakdown summary::-webkit-details-marker{display:none}
details.breakdown summary::before{content:"▶ "}
details[open].breakdown summary::before{content:"▼ "}
.btable{width:100%;border-collapse:collapse;font-size:.83rem;margin:.25rem 0 .75rem}
.btable td{padding:.3rem 1.2rem;border-bottom:1px solid #f2f2f2}
.btable td:last-child{text-align:right;font-variant-numeric:tabular-nums}
.btable .sep td{border-top:2px solid #ddd;font-weight:700;border-bottom:none}
.btable .info td{color:#888;font-size:.78rem;font-style:italic}
)css";

struct PeriodEntry {
    int day, mo, yr, wday;
    const Shift* shift;
    ShiftPayInfo pay;
};

static std::string render_period_section(
        int pay_mo, int pay_yr,
        std::vector<PeriodEntry>& entries,
        const PayConfig& cfg,
        PeriodData& out) {

    int start_mo = pay_mo - 1, start_yr = pay_yr;
    if (start_mo < 1) { start_mo = 12; --start_yr; }

    std::sort(entries.begin(), entries.end(), [](const PeriodEntry& a, const PeriodEntry& b){
        if (a.yr != b.yr) return a.yr < b.yr;
        if (a.mo != b.mo) return a.mo < b.mo;
        return a.day < b.day;
    });

    double gross = 0, total_h = 0;
    int    vac_days = 0, n_shifts = 0;
    for (auto& e : entries) {
        if (e.pay.is_absence && !e.shift->worktime_id) {
            if (e.pay.is_vacation) ++vac_days;
            continue;
        }
        if (e.shift->duration <= 0) continue;
        gross   += e.pay.gross;
        total_h += e.pay.worked_h;
        ++n_shifts;
    }

    double am             = gross * cfg.am_bidrag_pct / 100.0;
    double aft_am         = gross - am;
    double pension        = aft_am * cfg.employee_pension_pct / 100.0;
    double tax            = (aft_am - pension) * cfg.tax_pct / 100.0;
    double net            = aft_am - pension - tax - cfg.atp_dkk - cfg.klub_dkk;
    double fritvalg       = gross * cfg.fritvalg_pct / 100.0;
    double feriefri       = gross * cfg.feriefri_pct / 100.0;
    double employer_pen   = aft_am * cfg.employer_pension_pct / 100.0;
    int    pay_last_day   = days_in_month(pay_mo, pay_yr);

    out.period_start_day   = 16; out.period_start_month = start_mo; out.period_start_year = start_yr;
    out.period_end_day     = 15; out.period_end_month   = pay_mo;   out.period_end_year   = pay_yr;
    out.pay_month          = pay_mo;
    out.pay_year           = pay_yr;
    out.shift_count        = n_shifts;
    out.total_hours        = total_h;
    out.brutto_dkk         = gross;
    out.net_estimated_dkk  = net;
    out.fritvalg_dkk       = fritvalg;
    out.feriefri_dkk       = feriefri;

    std::ostringstream s;
    s << "<section class=\"period\">\n"
      << "<div class=\"ph\"><h2>16. " << DK_MONTHS[start_mo];
    if (start_yr != pay_yr) s << " " << start_yr;
    s << " \xe2\x80\x93 15. " << DK_MONTHS[pay_mo] << " " << pay_yr << "</h2>"
      << "<span class=\"pay-badge\">Udbetaling " << pay_last_day
      << ". " << DK_MONTHS[pay_mo] << " " << pay_yr << "</span></div>\n"
      << "<div class=\"stats\">"
      << "<div class=\"stat\"><span class=\"lbl\">Vagter</span>"
      << "<span class=\"val\">" << n_shifts << "</span></div>"
      << "<div class=\"stat\"><span class=\"lbl\">Timer</span>"
      << "<span class=\"val\">" << fmt_h(total_h) << "</span></div>"
      << "<div class=\"stat\"><span class=\"lbl\">Bruttol\xc3\xb8n</span>"
      << "<span class=\"val\">" << fmt_dkk(gross) << "</span></div>"
      << "<div class=\"stat\"><span class=\"lbl\">Est. netto</span>"
      << "<span class=\"val net\">" << fmt_dkk(net) << "</span></div>"
      << "<div class=\"stat\"><span class=\"lbl\">Fridage</span>"
      << "<span class=\"val\">" << vac_days << "</span></div>"
      << "</div>\n";

    s << "<table>\n<thead><tr>"
      << "<th>Dato</th><th>Dag</th><th>Tid</th><th>Timer</th>"
      << "<th>Till\xc3\xa6g</th><th class=\"num\">Brutto</th>"
      << "</tr></thead>\n<tbody>\n";

    for (auto& e : entries) {
        const Shift& sh = *e.shift;
        const ShiftPayInfo& p = e.pay;

        if (p.is_absence && !sh.worktime_id) {
            s << "<tr><td>" << e.day << " " << DK_MONTHS[e.mo] << "</td>"
              << "<td>" << DK_DAYS[e.wday] << "</td>"
              << "<td colspan=\"4\"><span class=\"tag t-abs\">"
              << sh.absence.type_caption << "</span></td></tr>\n";
            continue;
        }
        if (sh.duration <= 0) continue;

        std::string tags;
        if (e.wday == 0)        tags += "<span class=\"tag t-son\">s\xc3\xb8n</span> ";
        else if (e.wday == 6)   tags += "<span class=\"tag t-lor\">l\xc3\xb8r</span> ";
        if (p.evening_h > 0.01) tags += "<span class=\"tag t-aft\">aft</span>";
        if (tags.empty()) tags = "<span style=\"color:#ccc\">\xe2\x80\x93</span>";

        s << "<tr>"
          << "<td>" << e.day << " " << DK_MONTHS[e.mo] << "</td>"
          << "<td>" << DK_DAYS[e.wday] << "</td>"
          << "<td>" << fmt_time(sh.start_time) << "\xe2\x80\x93" << fmt_time(sh.end_time) << "</td>"
          << "<td>" << fmt_h(p.worked_h) << "</td>"
          << "<td>" << tags << "</td>"
          << "<td class=\"num\">" << fmt_dkk(p.gross) << "</td>"
          << "</tr>\n";
    }

    s << "</tbody></table>\n";

    s << "<details class=\"breakdown\"><summary>L\xc3\xb8nberegning</summary>\n"
      << "<table class=\"btable\">\n"
      << "<tr><td>Bruttol\xc3\xb8n</td><td>" << fmt_dkk(gross) << "</td></tr>\n"
      << "<tr><td>AM-bidrag (" << cfg.am_bidrag_pct << "&#37;)</td>"
      <<     "<td>\xe2\x88\x92" << fmt_dkk(am) << "</td></tr>\n"
      << "<tr><td>Medarbejderpension (" << cfg.employee_pension_pct << "&#37;)</td>"
      <<     "<td>\xe2\x88\x92" << fmt_dkk(pension) << "</td></tr>\n"
      << "<tr><td>A-skat (" << cfg.tax_pct << "&#37;, bikort)</td>"
      <<     "<td>\xe2\x88\x92" << fmt_dkk(tax) << "</td></tr>\n"
      << "<tr><td>ATP</td><td>\xe2\x88\x92" << fmt_dkk(cfg.atp_dkk) << "</td></tr>\n"
      << "<tr><td>Klub Scottie</td><td>\xe2\x88\x92" << fmt_dkk(cfg.klub_dkk) << "</td></tr>\n"
      << "<tr class=\"sep\"><td>Estimeret udbetaling</td><td>" << fmt_dkk(net) << "</td></tr>\n"
      << "<tr class=\"info\"><td>Fritvalgsopsparing (" << cfg.fritvalg_pct << "&#37;, udbetales separat)</td>"
      <<     "<td>+" << fmt_dkk(fritvalg) << "</td></tr>\n"
      << "<tr class=\"info\"><td>Feriefri til fritvalg (" << cfg.feriefri_pct << "&#37;)</td>"
      <<     "<td>+" << fmt_dkk(feriefri) << "</td></tr>\n"
      << "<tr class=\"info\"><td>Arbejdsgiverpension (" << cfg.employer_pension_pct << "&#37;)</td>"
      <<     "<td>+" << fmt_dkk(employer_pen) << "</td></tr>\n"
      << "</table></details>\n"
      << "</section>\n";

    return s.str();
}

static std::string doc_header(const std::string& title, const std::string& ts) {
    std::ostringstream s;
    s << "<!DOCTYPE html>\n<html lang=\"da\">\n<head>\n"
      << "<meta charset=\"UTF-8\">\n"
      << "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
      << "<title>" << title << "</title>\n"
      << "<style>" << CSS << "</style>\n"
      << "</head>\n<body>\n"
      << "<header><h1>" << title << "</h1>"
      << "<span class=\"updated\">Opdateret " << ts << "</span></header>\n"
      << "<main>\n";
    return s.str();
}

static std::string doc_footer() {
    return "</main>\n</body>\n</html>\n";
}

std::vector<PeriodData> compute_periods(const Timetable& tt,
                                        const FunctionMap& /*func_map*/,
                                        const PayConfig& cfg) {
    using PeriodKey = std::pair<int,int>;

    std::map<PeriodKey, std::vector<PeriodEntry>> period_map;

    for (auto& w : tt.weeks) {
        for (auto& day : w.days) {
            int d, mo, y;
            if (!parse_dmyyyy(day.date, d, mo, y)) continue;
            auto key  = lonperiode_key(d, mo, y);
            int  wday = day_of_week(d, mo, y);
            for (auto& s : day.shifts) {
                PeriodEntry e{d, mo, y, wday, &s, calc_shift_pay(s, d, mo, y, cfg)};
                period_map[key].push_back(std::move(e));
            }
        }
    }

    std::vector<PeriodKey> keys;
    for (auto& [k, _] : period_map) keys.push_back(k);
    std::sort(keys.begin(), keys.end());

    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M UTC", &utc);

    std::vector<PeriodData> result;
    result.reserve(keys.size());

    for (auto& key : keys) {
        PeriodData pd;
        std::string section = render_period_section(
            key.first, key.second, period_map[key], cfg, pd);

        pd.section_html = section;

        std::string title = "L\xc3\xb8noversigt \xe2\x80\x93 ";
        title += std::string(DK_MONTHS[pd.pay_month]) + " " + std::to_string(pd.pay_year);
        pd.html_content = doc_header(title, ts) + section + doc_footer();

        result.push_back(std::move(pd));
    }

    return result;
}

void generate_report(const std::string& out_path,
                     const Timetable& tt,
                     const FunctionMap& func_map,
                     const PayConfig& cfg) {
    auto periods = compute_periods(tt, func_map, cfg);

    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M UTC", &utc);

    std::ofstream f(out_path);
    if (!f) throw std::runtime_error("Cannot write report: " + out_path);

    f << doc_header("Timegrip L\xc3\xb8noversigt", ts);

    if (periods.empty()) {
        f << "<p style=\"padding:2rem;color:#888\">Ingen vagter i perioden.</p>\n";
    } else {
        // Newest first for the combined report
        for (int i = static_cast<int>(periods.size()) - 1; i >= 0; --i)
            f << periods[i].section_html;
    }

    f << doc_footer();

    if (!f) throw std::runtime_error("Write error for report: " + out_path);
}

static const char* GATE_TEMPLATE = R"GATE(<?php
$pw_hash     = '__HASH__';
$salt        = '__SALT__';
$cookie_name = 'loen_auth';
$cookie_ttl  = 365 * 24 * 3600;
$valid_token = hash_hmac('sha256', 'loen_v1', $pw_hash);

if (isset($_GET['logout'])) {
    setcookie($cookie_name, '', time() - 3600, '/');
    header('Location: /');
    exit;
}

$error = false;
if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_POST['password'])) {
    $attempt = hash_pbkdf2('sha256', $_POST['password'], $salt, 100000);
    if (hash_equals($pw_hash, $attempt)) {
        setcookie($cookie_name, $valid_token, time() + $cookie_ttl, '/', '', false, true);
        header('Location: ' . strtok($_SERVER['REQUEST_URI'], '?'));
        exit;
    }
    $error = true;
}

if (!isset($_COOKIE[$cookie_name]) || !hash_equals($valid_token, $_COOKIE[$cookie_name])) {
?><!DOCTYPE html>
<html lang="da">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lønoversigt</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:#f0f2f5;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:1rem}
.card{background:#fff;border-radius:14px;box-shadow:0 4px 24px rgba(0,0,0,.10);padding:2rem 2rem 2.5rem;width:100%;max-width:360px}
.brand{background:#1a237e;border-radius:10px;padding:1.2rem 1rem;text-align:center;margin-bottom:1.75rem}
.brand h1{color:#fff;font-size:1.25rem;font-weight:700;letter-spacing:-.01em}
.brand p{color:rgba(255,255,255,.6);font-size:.8rem;margin-top:.2rem}
label{display:block;font-size:.85rem;font-weight:600;color:#444;margin-bottom:.5rem}
input[type=password]{display:block;width:100%;padding:.85rem 1rem;border:2px solid #e0e0e0;border-radius:9px;font-size:1rem;outline:none;transition:border-color .15s}
input[type=password]:focus{border-color:#1a237e}
.err{background:#fce4ec;color:#b71c1c;border-radius:7px;padding:.65rem 1rem;font-size:.85rem;margin-bottom:1rem}
button{display:block;width:100%;margin-top:1.1rem;padding:.9rem;background:#1a237e;color:#fff;border:none;border-radius:9px;font-size:1rem;font-weight:600;cursor:pointer;transition:background .15s}
button:hover{background:#283593}
button:active{background:#0d1757}
</style>
</head>
<body>
<div class="card">
  <div class="brand">
    <h1>Lønoversigt</h1>
    <p>emandersen.dk</p>
  </div>
  <?php if ($error): ?>
  <div class="err">Forkert adgangskode — prøv igen</div>
  <?php endif; ?>
  <form method="post" autocomplete="on">
    <label for="pw">Adgangskode</label>
    <input type="password" id="pw" name="password"
           autocomplete="current-password"
           autofocus placeholder="••••••••">
    <button type="submit">Log ind</button>
  </form>
</div>
</body>
</html>
<?php
    exit;
}

$db_host = '__DB_HOST__';
$db_user = '__DB_USER__';
$db_pass = '__DB_PASS__';
$db_name = '__DB_NAME__';

try {
    $pdo = new PDO("mysql:host=$db_host;dbname=$db_name;charset=utf8mb4",
                   $db_user, $db_pass,
                   [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
                    PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC]);
} catch (PDOException $e) {
    http_response_code(503);
    echo '<p style="font-family:sans-serif;padding:2rem;color:#c62828">Database ikke tilgængelig. Prøv igen senere.</p>';
    exit;
}

if (isset($_GET['m'])) {
    $ps = $_GET['m'];
    if (!preg_match('/^\d{4}-\d{2}-\d{2}$/', $ps)) { http_response_code(400); exit; }
    $stmt = $pdo->prepare('SELECT html_content FROM loen_periods WHERE period_start = ? LIMIT 1');
    $stmt->execute([$ps]);
    $row = $stmt->fetch();
    if (!$row) { http_response_code(404); echo '<p style="font-family:sans-serif;padding:2rem">Periode ikke fundet.</p>'; exit; }
    echo $row['html_content'];
    exit;
}

$stmt = $pdo->query(
    'SELECT period_start, period_end, pay_date, shift_count,'
    .' total_hours, brutto_dkk, net_estimated_dkk, locked'
    .' FROM loen_periods ORDER BY pay_date ASC'
);
$rows = $stmt->fetchAll();
$today = date('Y-m-d');

$months = ['','januar','februar','marts','april','maj','juni',
           'juli','august','september','oktober','november','december'];

function fmt_date_da($ymd, $months) {
    [$y,$m,$d] = explode('-', $ymd);
    return intval($d).'. '.$months[intval($m)].' '.$y;
}
function fmt_period_da($ps, $pe, $months) {
    [$sy,$sm,$sd] = explode('-', $ps);
    [$ey,$em,$ed] = explode('-', $pe);
    $s = intval($sd).'. '.$months[intval($sm)];
    if ($sy !== $ey) $s .= ' '.$sy;
    return $s.' – '.intval($ed).'. '.$months[intval($em)].' '.$ey;
}
?><!DOCTYPE html>
<html lang="da">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lønoversigt</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:#f0f2f5;color:#1a1a1a}
header{background:#1a237e;color:#fff;padding:1.1rem 1.5rem;display:flex;align-items:center;gap:1rem}
header h1{font-size:1.2rem;font-weight:700;flex:1}
.logout{color:rgba(255,255,255,.65);font-size:.82rem;text-decoration:none}
.logout:hover{color:#fff}
.cards{max-width:700px;margin:1.5rem auto;padding:0 1rem;display:flex;flex-direction:column;gap:.85rem}
.card{background:#fff;border-radius:10px;box-shadow:0 1px 4px rgba(0,0,0,.09);
      padding:1rem 1.25rem;display:flex;align-items:center;justify-content:space-between;
      gap:1rem;flex-wrap:wrap;text-decoration:none;color:inherit;transition:box-shadow .15s}
.card:hover{box-shadow:0 3px 14px rgba(0,0,0,.14)}
.card.current{border:2px solid #1a237e;background:#f0f4ff}
.card-left{}
.card-period{font-weight:600;font-size:.95rem;color:#1a1a1a}
.card-pay{font-size:1rem;font-weight:700;color:#1a237e;margin-top:.2rem}
.card-pay.past{color:#555;font-weight:600}
.locked-badge{display:inline-block;font-size:.72rem;background:#e8f5e9;color:#2e7d32;
              padding:.1rem .5rem;border-radius:3px;margin-left:.5rem;font-weight:600;vertical-align:middle}
.card-stats{display:flex;gap:1.25rem;flex-wrap:wrap;font-size:.88rem;color:#555}
.card-stats .val{font-weight:700;color:#1a237e}
.card-stats .val.net{color:#1b5e20}
</style>
</head>
<body>
<header>
  <h1>Lønoversigt</h1>
  <a class="logout" href="?logout">Log ud</a>
</header>
<div class="cards">
<?php foreach ($rows as $r):
    $is_past = $r['pay_date'] < $today;
    $cls = 'card' . ($r['locked'] ? ' locked' : '');
?>
<a class="<?= $cls ?>" data-pay="<?= htmlspecialchars($r['pay_date']) ?>"
   href="?m=<?= urlencode($r['period_start']) ?>">
  <div class="card-left">
    <div class="card-period">
      <?= htmlspecialchars(fmt_period_da($r['period_start'], $r['period_end'], $months)) ?>
      <?php if ($r['locked']): ?><span class="locked-badge">betalt</span><?php endif; ?>
    </div>
    <div class="card-pay <?= $is_past ? 'past' : '' ?>">
      Udbetaling <?= htmlspecialchars(fmt_date_da($r['pay_date'], $months)) ?>
    </div>
  </div>
  <div class="card-stats">
    <span><?= intval($r['shift_count']) ?> vagter</span>
    <span><span class="val"><?= number_format($r['total_hours'],1,',','.') ?>t</span></span>
    <span>Brutto <span class="val"><?= number_format($r['brutto_dkk'],0,',','.') ?>&nbsp;kr</span></span>
    <span>Netto <span class="val net"><?= number_format($r['net_estimated_dkk'],0,',','.') ?>&nbsp;kr</span></span>
  </div>
</a>
<?php endforeach; ?>
<?php if (empty($rows)): ?>
<p style="padding:2rem;color:#888;text-align:center">Ingen lønperioder endnu.</p>
<?php endif; ?>
</div>
<script>
(function(){
  var today = new Date().toISOString().slice(0,10);
  var cards = Array.from(document.querySelectorAll('.card[data-pay]'));
  var current = cards.find(function(c){ return c.dataset.pay >= today; });
  if (current) {
    current.classList.add('current');
    current.scrollIntoView({block:'start', behavior:'smooth'});
  }
})();
</script>
</body>
</html>
)GATE";

// pbkdf2_hash: 64-char hex PBKDF2-SHA256 of REPORT_PASSWORD; does nothing if empty.
void generate_gate_files(const std::string& out_dir,
                         const DbConfig& db_cfg,
                         const std::string& pbkdf2_hash) {
    if (out_dir.empty() || pbkdf2_hash.empty()) return;

    std::filesystem::create_directories(out_dir);

    std::string php = GATE_TEMPLATE;
    auto replace_all = [](std::string& s,
                          const std::string& from,
                          const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all(php, "__HASH__",    pbkdf2_hash);
    replace_all(php, "__DB_HOST__", db_cfg.host);
    replace_all(php, "__DB_USER__", db_cfg.user);
    replace_all(php, "__DB_PASS__", db_cfg.password);
    replace_all(php, "__DB_NAME__", db_cfg.database);

    {
        std::ofstream gf(out_dir + "/gate.php");
        if (!gf) throw std::runtime_error("Cannot write gate.php to " + out_dir);
        gf << php;
        if (!gf) throw std::runtime_error("Write error for gate.php");
    }
    {
        std::ofstream hf(out_dir + "/.htaccess");
        if (!hf) throw std::runtime_error("Cannot write .htaccess to " + out_dir);
        hf << "DirectoryIndex gate.php\n";
    }
}
