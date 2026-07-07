#include "adfs.hpp"
#include "db.hpp"
#include "env.hpp"
#include "gcal.hpp"
#include "period.hpp"
#include "report.hpp"
#include "timegrip.hpp"

#include <openssl/evp.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

static const std::string VERSION       = "1.0.0";
static const int         DEFAULT_WEEKS = 12;

// PBKDF2-SHA256, 100 000 iterations → 64-char lowercase hex
static std::string compute_pbkdf2_hex(const std::string& password,
                                      const std::string& salt) {
    unsigned char out[32];
    PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                      reinterpret_cast<const unsigned char*>(salt.c_str()),
                      static_cast<int>(salt.size()),
                      100000, EVP_sha256(), 32, out);
    char hex[65];
    for (int i = 0; i < 32; ++i)
        snprintf(hex + i * 2, 3, "%02x", out[i]);
    return hex;
}

static void today_dmy(int& d, int& mo, int& y) {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    d  = utc.tm_mday;
    mo = utc.tm_mon + 1;
    y  = utc.tm_year + 1900;
}

static void usage(const char* prog) {
    std::cout <<
        "timegrip-sync " << VERSION << "\n"
        "\n"
        "Usage:\n"
        "  " << prog << " [options]\n"
        "\n"
        "Options:\n"
        "  --weeks N      Weeks ahead to sync (default: " << DEFAULT_WEEKS << ")\n"
        "  --dry-run      Fetch and display shifts without writing to calendar\n"
        "  --report DIR   Generate pay report files into directory DIR\n"
        "  --save         Write shift tracking and period archive to DB\n"
        "  --version      Print version and exit\n"
        "  --help         Show this help\n"
        "\n"
        "Environment variables (set in .env or shell):\n"
        "  TIMEGRIP_BASE_URL      Base URL of your Timegrip instance\n"
        "  SALLING_EMAIL          Timegrip login email\n"
        "  SALLING_PASSWORD       Timegrip login password\n"
        "  GOOGLE_CLIENT_ID       Google OAuth2 client ID\n"
        "  GOOGLE_CLIENT_SECRET   Google OAuth2 client secret\n"
        "  GOOGLE_REFRESH_TOKEN   OAuth2 refresh token (obtained on first run)\n"
        "  CALENDAR_NAME          Google Calendar name to sync into\n"
        "\n"
        "  SYNC_AHEAD_WEEKS       Weeks ahead to sync (default: 12, overridden by --weeks)\n"
        "  SYNC_LOOKBACK_WEEKS    Weeks behind today that can be changed (default: 4)\n"
        "\n"
        "  HOURLY_RATE            Base hourly rate (DKK)\n"
        "  EVENING_SUPPLEMENT     Evening supplement (DKK/hr)\n"
        "  SATURDAY_SUPPLEMENT    Saturday supplement (DKK/hr)\n"
        "  SUNDAY_SUPPLEMENT      Sunday supplement (DKK/hr)\n"
        "  AM_BIDRAG_PCT          AM-bidrag percentage\n"
        "  TAX_PCT                Income tax percentage\n"
        "  EMPLOYEE_PENSION_PCT   Employee pension percentage\n"
        "  EMPLOYER_PENSION_PCT   Employer pension percentage\n"
        "  ATP_DKK                ATP per pay period (DKK)\n"
        "  KLUB_DKK               Union/club fee per pay period (DKK)\n"
        "  FRITVALG_PCT           Fritvalg savings percentage\n"
        "  FERIEFRI_PCT           Feriefri percentage\n"
        "\n"
        "  REPORT_PASSWORD        Password for the web report (gate.php)\n"
        "  REPORT_SALT            Salt used when hashing the report password\n"
        "\n"
        "  DB_HOST / DB_PORT / DB_USER / DB_PASSWORD / DB_DATABASE\n"
        "                         MySQL connection (used with --save)\n";
}

static std::string weeks_offset_str(int weeks) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    t += weeks * 7 * 24 * 3600;
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

int main(int argc, char* argv[]) {
    load_dotenv();

    int         weeks          = DEFAULT_WEEKS;
    bool        dry_run        = false;
    bool        weeks_from_cli = false;
    bool        save_to_db     = false;
    std::string report_dir;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { usage(argv[0]); return 0; }
        if (arg == "--version")             { std::cout << VERSION << "\n"; return 0; }
        if (arg == "--dry-run")             { dry_run = true; continue; }
        if (arg == "--save")                { save_to_db = true; continue; }
        if (arg == "--weeks" && i + 1 < argc) {
            weeks = std::stoi(argv[++i]);
            if (weeks < 1 || weeks > 52) {
                std::cerr << "error: --weeks must be 1-52\n";
                return 1;
            }
            weeks_from_cli = true;
            continue;
        }
        if (arg == "--report" && i + 1 < argc) {
            report_dir = argv[++i];
            continue;
        }
        std::cerr << "error: unknown option: " << arg << "\n";
        usage(argv[0]);
        return 1;
    }

    if (!weeks_from_cli) {
        auto ahead_env = get_env("SYNC_AHEAD_WEEKS");
        if (!ahead_env.empty()) {
            int v = std::stoi(ahead_env);
            if (v >= 1 && v <= 52) weeks = v;
        }
    }

    try {
        std::cout << "timegrip-sync " << VERSION << "\n\n";

        std::cout << "Timegrip:\n";
        std::cout << "  Authenticating with ADFS…\n";
        auto session = adfs_login(require_env("SALLING_EMAIL"),
                                  require_env("SALLING_PASSWORD"));
        std::cout << "  ✓ Timegrip session ready\n";

        int lookback = 4;
        auto lb_env = get_env("SYNC_LOOKBACK_WEEKS");
        if (!lb_env.empty()) {
            int v = std::stoi(lb_env);
            if (v >= 0 && v <= 52) lookback = v;
        }

        std::cout << "  Fetching timetable…\n";
        const std::string base_url = require_env("TIMEGRIP_BASE_URL");
        auto timetable = fetch_timetable(*session, base_url, weeks, lookback);
        auto func_map  = fetch_function_names(*session, base_url);

        int total_shifts = 0;
        for (auto& w : timetable.weeks)
            for (auto& d : w.days)
                total_shifts += static_cast<int>(d.shifts.size());

        std::string from_date = weeks_offset_str(-lookback);
        std::string to_date   = weeks_offset_str(weeks);
        std::cout << "  " << total_shifts << " shifts fetched ("
                  << from_date << " → " << to_date << ")\n";

        auto cfg = pay_config_from_env();

        auto do_report = [&]() {
            if (report_dir.empty()) return;

            std::cout << "\nReport:\n  Computing periods…\n";
            auto periods = compute_periods(timetable, func_map, cfg);

            std::string pbkdf2_hex;
            auto report_pw = get_env("REPORT_PASSWORD");
            if (!report_pw.empty())
                pbkdf2_hex = compute_pbkdf2_hex(report_pw,
                                                require_env("REPORT_SALT"));

#ifdef HAVE_MYSQL
            if (save_to_db && !get_env("DB_HOST").empty()) {
                try {
                    MySQLDatabase db(db_config_from_env());
                    db.connect();
                    PeriodArchive archive(db);
                    archive.ensure_schema();

                    int td, tmo, ty;
                    today_dmy(td, tmo, ty);

                    int saved = 0, skipped = 0;
                    for (auto& pd : periods) {
                        if (archive.is_locked(pd.pay_month, pd.pay_year)) {
                            ++skipped;
                            continue;
                        }
                        bool lock = (ty > pd.pay_year)
                                 || (ty == pd.pay_year && tmo > pd.pay_month)
                                 || (ty == pd.pay_year && tmo == pd.pay_month
                                     && td >= days_in_month(pd.pay_month, pd.pay_year));
                        archive.upsert_period(pd, lock);
                        ++saved;
                    }
                    std::cout << "  " << saved << " period(s) archived";
                    if (skipped) std::cout << ", " << skipped << " locked/skipped";
                    std::cout << "\n";
                } catch (const std::exception& e) {
                    std::cerr << "  DB warning (period archive): " << e.what() << "\n";
                }
            }
#endif

            std::filesystem::create_directories(report_dir);

            auto db_cfg = db_config_from_env();
            if (!pbkdf2_hex.empty()) {
                generate_gate_files(report_dir, db_cfg, pbkdf2_hex);
                std::cout << "  ✓ gate.php + .htaccess written to " << report_dir << "\n";
            } else {
                generate_report(report_dir + "/report.html", timetable, func_map, cfg);
                std::cout << "  ✓ report.html written to " << report_dir << "\n";
            }
        };

        if (dry_run) {
            std::cout << "\n[dry-run] Timetable:\n";
            const char* MON_NAMES[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};
            for (auto& w : timetable.weeks) {
                std::cout << "\nWeek " << w.week << " · " << w.year << "\n";
                for (auto& d : w.days) {
                    int day_n = 0, mon_n = 0;
                    if (d.date.size() == 10 && d.date[2] == '-') {
                        day_n = std::stoi(d.date.substr(0, 2));
                        mon_n = std::stoi(d.date.substr(3, 2));
                    }
                    std::string date_str = std::to_string(day_n) + " " +
                                           (mon_n >= 1 && mon_n <= 12
                                                ? MON_NAMES[mon_n - 1] : "?");
                    if (d.shifts.empty()) {
                        std::cout << "  " << date_str << "  off\n";
                        continue;
                    }
                    for (auto& s : d.shifts) {
                        if (s.has_absence && !s.worktime_id) {
                            std::cout << "  " << date_str << "  🏖  "
                                      << s.absence.type_caption
                                      << " — guaranteed no work\n";
                        } else {
                            auto st = s.start_time.size() > 16
                                          ? s.start_time.substr(11, 5) : "?";
                            auto et = s.end_time.size() > 16
                                          ? s.end_time.substr(11, 5) : "?";
                            auto fid = !s.details.empty()
                                           ? std::to_string(s.details[0].function_id)
                                           : "";
                            auto fit = func_map.find(fid);
                            std::cout << "  " << date_str << "  "
                                      << st << "–" << et
                                      << "  (" << s.duration / 60 << "h)";
                            if (fit != func_map.end())
                                std::cout << "  " << fit->second;
                            std::cout << "\n";
                        }
                    }
                }
            }
            do_report();
            return 0;
        }

        std::cout << "\nGoogle Calendar:\n";
        auto tokens = google_auth(
            require_env("GOOGLE_CLIENT_ID"),
            require_env("GOOGLE_CLIENT_SECRET"),
            get_env("GOOGLE_REFRESH_TOKEN"));

        auto calendar_id = get_or_create_calendar(tokens.access_token,
                                                  require_env("CALENDAR_NAME"));

        std::cout << "  Syncing events…\n";
        auto result = sync_calendar(tokens.access_token, calendar_id,
                                    timetable, func_map,
                                    from_date, to_date, from_date);

        std::cout << "\n✓ Done — "
                  << result.created   << " created · "
                  << result.updated   << " updated · "
                  << result.deleted   << " deleted · "
                  << result.unchanged << " unchanged\n";

        do_report();

#ifdef HAVE_MYSQL
        if (save_to_db && !get_env("DB_HOST").empty()) {
            std::cout << "\nDatabase:\n";
            try {
                MySQLDatabase db(db_config_from_env());
                db.connect();
                ShiftTracker tracker(db);
                tracker.ensure_schema();

                auto run_id    = tracker.begin_sync_run(total_shifts);
                auto change_ids = tracker.apply_changes(result.changes);
                tracker.finish_sync_run(run_id, true,
                                        result.created,
                                        result.updated,
                                        result.deleted,
                                        change_ids);

                std::cout << "  " << result.changes.size()
                          << " change(s) recorded (run #"
                          << run_id << ")\n";
            } catch (const std::exception& e) {
                std::cerr << "  DB warning: " << e.what() << "\n";
            }
        }
#endif

    } catch (const HttpError& e) {
        std::cerr << "HTTP error " << e.status << ": " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
