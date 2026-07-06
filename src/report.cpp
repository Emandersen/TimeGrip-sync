#include "report.hpp"
#include "env.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
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

static int days_in_month(int mo, int y) {
    static const int t[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return t[mo];
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

    // Evening supplement on weekdays only
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
.pay-badge{font-size:.75rem;background:rgba(255,255,255,.18);padding:.2rem .6rem;border-radius:4px;white-space:nowrap}
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

void generate_report(const std::string& out_path,
                     const Timetable& tt,
                     const FunctionMap& /*func_map*/,
                     const PayConfig& cfg) {
    using PeriodKey = std::pair<int,int>;  // {pay_month, pay_year}

    struct Entry {
        int day, mo, yr, wday;
        const Shift* shift;
        ShiftPayInfo pay;
    };

    std::map<PeriodKey, std::vector<Entry>> periods;

    for (auto& w : tt.weeks) {
        for (auto& day : w.days) {
            int d, mo, y;
            if (!parse_dmyyyy(day.date, d, mo, y)) continue;
            auto key  = lonperiode_key(d, mo, y);
            int  wday = day_of_week(d, mo, y);
            for (auto& s : day.shifts) {
                Entry e{d, mo, y, wday, &s, calc_shift_pay(s, d, mo, y, cfg)};
                periods[key].push_back(std::move(e));
            }
        }
    }

    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M UTC", &utc);

    std::ofstream f(out_path);
    if (!f) throw std::runtime_error("Cannot write report: " + out_path);

    f << "<!DOCTYPE html>\n<html lang=\"da\">\n<head>\n"
      << "<meta charset=\"UTF-8\">\n"
      << "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
      << "<title>Timegrip L\xc3\xb8noversigt</title>\n"
      << "<style>" << CSS << "</style>\n"
      << "</head>\n<body>\n"
      << "<header><h1>Timegrip L\xc3\xb8noversigt</h1>"
      << "<span class=\"updated\">Opdateret " << ts << "</span></header>\n"
      << "<main>\n";

    // Newest period first
    std::vector<PeriodKey> keys;
    for (auto& [k, _] : periods) keys.push_back(k);
    std::sort(keys.begin(), keys.end(), [](auto& a, auto& b){ return a > b; });

    for (auto& key : keys) {
        auto& entries = periods[key];
        int pay_mo = key.first, pay_yr = key.second;
        int start_mo = pay_mo - 1, start_yr = pay_yr;
        if (start_mo < 1) { start_mo = 12; --start_yr; }

        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b){
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

        double am      = gross * cfg.am_bidrag_pct / 100.0;
        double aft_am  = gross - am;
        double pension = aft_am * cfg.employee_pension_pct / 100.0;
        double tax     = (aft_am - pension) * cfg.tax_pct / 100.0;
        double net     = aft_am - pension - tax - cfg.atp_dkk - cfg.klub_dkk;
        double fritvalg = gross * cfg.fritvalg_pct / 100.0;
        double feriefri = gross * cfg.feriefri_pct / 100.0;
        double employer_pension = aft_am * cfg.employer_pension_pct / 100.0;

        int pay_last_day = days_in_month(pay_mo, pay_yr);

        f << "<section class=\"period\">\n"
          << "<div class=\"ph\"><h2>16. " << DK_MONTHS[start_mo]
          << (start_yr != pay_yr ? (" " + std::to_string(start_yr)) : std::string{})
          << " \xe2\x80\x93 15. " << DK_MONTHS[pay_mo] << " " << pay_yr << "</h2>"
          << "<span class=\"pay-badge\">Udbetaling " << pay_last_day
          << ". " << DK_MONTHS[pay_mo] << "</span></div>\n"
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

        f << "<table>\n<thead><tr>"
          << "<th>Dato</th><th>Dag</th><th>Tid</th><th>Timer</th>"
          << "<th>Till\xc3\xa6g</th><th class=\"num\">Brutto</th>"
          << "</tr></thead>\n<tbody>\n";

        for (auto& e : entries) {
            const Shift& s = *e.shift;
            const ShiftPayInfo& p = e.pay;

            if (p.is_absence && !s.worktime_id) {
                f << "<tr><td>" << e.day << " " << DK_MONTHS[e.mo] << "</td>"
                  << "<td>" << DK_DAYS[e.wday] << "</td>"
                  << "<td colspan=\"4\"><span class=\"tag t-abs\">"
                  << s.absence.type_caption << "</span></td></tr>\n";
                continue;
            }
            if (s.duration <= 0) continue;

            std::string tags;
            if (e.wday == 0)        tags += "<span class=\"tag t-son\">s\xc3\xb8n</span> ";
            else if (e.wday == 6)   tags += "<span class=\"tag t-lor\">l\xc3\xb8r</span> ";
            if (p.evening_h > 0.01) tags += "<span class=\"tag t-aft\">aft</span>";
            if (tags.empty()) tags = "<span style=\"color:#ccc\">\xe2\x80\x93</span>";

            f << "<tr>"
              << "<td>" << e.day << " " << DK_MONTHS[e.mo] << "</td>"
              << "<td>" << DK_DAYS[e.wday] << "</td>"
              << "<td>" << fmt_time(s.start_time) << "\xe2\x80\x93" << fmt_time(s.end_time) << "</td>"
              << "<td>" << fmt_h(p.worked_h) << "</td>"
              << "<td>" << tags << "</td>"
              << "<td class=\"num\">" << fmt_dkk(p.gross) << "</td>"
              << "</tr>\n";
        }

        f << "</tbody></table>\n";

        f << "<details class=\"breakdown\"><summary>L\xc3\xb8nberegning</summary>\n"
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
          <<     "<td>+" << fmt_dkk(employer_pension) << "</td></tr>\n"
          << "</table></details>\n"
          << "</section>\n";
    }

    if (periods.empty())
        f << "<p style=\"padding:2rem;color:#888\">Ingen vagter i perioden.</p>\n";

    f << "</main>\n</body>\n</html>\n";

    if (!f) throw std::runtime_error("Write error for report: " + out_path);
}
