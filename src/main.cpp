#include "adfs.hpp"
#include "db.hpp"
#include "env.hpp"
#include "gcal.hpp"
#include "timegrip.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

static const std::string VERSION      = "1.0.0";
static const int         DEFAULT_WEEKS = 12;

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
        "  --version      Print version and exit\n"
        "  --help         Show this help\n"
        "\n"
        "Environment variables (set in .env or shell):\n"
        "  SALLING_EMAIL          Timegrip login email\n"
        "  SALLING_PASSWORD       Timegrip login password\n"
        "  GOOGLE_CLIENT_ID       Google OAuth2 client ID\n"
        "  GOOGLE_CLIENT_SECRET   Google OAuth2 client secret\n"
        "  GOOGLE_REFRESH_TOKEN   OAuth2 refresh token (obtained on first run)\n"
        "\n"
        "  SYNC_AHEAD_WEEKS       Weeks ahead to sync (default: 12, overridden by --weeks)\n"
        "  SYNC_LOOKBACK_WEEKS    Weeks behind today that can be changed (default: 4)\n"
        "\n"
        "  DB_HOST / DB_PORT / DB_USER / DB_PASSWORD / DB_DATABASE\n"
        "                         MySQL connection (optional, for future use)\n";
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

    int  weeks   = DEFAULT_WEEKS;
    bool dry_run = false;
    bool weeks_from_cli = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { usage(argv[0]); return 0; }
        if (arg == "--version")             { std::cout << VERSION << "\n"; return 0; }
        if (arg == "--dry-run")             { dry_run = true; continue; }
        if (arg == "--weeks" && i + 1 < argc) {
            weeks = std::stoi(argv[++i]);
            if (weeks < 1 || weeks > 52) {
                std::cerr << "error: --weeks must be 1-52\n";
                return 1;
            }
            weeks_from_cli = true;
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
        auto timetable = fetch_timetable(*session, weeks, lookback);
        auto func_map  = fetch_function_names(*session);

        int total_shifts = 0;
        for (auto& w : timetable.weeks)
            for (auto& d : w.days)
                total_shifts += static_cast<int>(d.shifts.size());

        std::string from_date = weeks_offset_str(-lookback);
        std::string to_date   = weeks_offset_str(weeks);
        std::cout << "  " << total_shifts << " shifts fetched ("
                  << from_date << " → " << to_date << ")\n";

        if (dry_run) {
            std::cout << "\n[dry-run] Timetable:\n";
            const char* MON_NAMES[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};
            for (auto& w : timetable.weeks) {
                std::cout << "\nWeek " << w.week << " · " << w.year << "\n";
                for (auto& d : w.days) {
                    // Parse DD-MM-YYYY
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
            return 0;
        }

        std::cout << "\nGoogle Calendar:\n";
        auto tokens = google_auth(
            require_env("GOOGLE_CLIENT_ID"),
            require_env("GOOGLE_CLIENT_SECRET"),
            get_env("GOOGLE_REFRESH_TOKEN"));

        auto calendar_id = get_or_create_calendar(tokens.access_token,
                                                  CALENDAR_NAME);

        std::cout << "  Syncing events…\n";
        auto result = sync_calendar(tokens.access_token, calendar_id,
                                    timetable, func_map,
                                    from_date, to_date, from_date);

        std::cout << "\n✓ Done — "
                  << result.created   << " created · "
                  << result.updated   << " updated · "
                  << result.deleted   << " deleted · "
                  << result.unchanged << " unchanged\n";

        // Database tracking (optional — only runs when DB_HOST is set)
#ifdef HAVE_MYSQL
        if (!get_env("DB_HOST").empty()) {
            std::cout << "\nDatabase:\n";
            try {
                MySQLDatabase db(db_config_from_env());
                db.connect();
                ShiftTracker tracker(db);
                tracker.ensure_schema();

                auto run_id = tracker.begin_sync_run(total_shifts);
                tracker.apply_changes(run_id, result.changes);
                tracker.finish_sync_run(run_id,
                                        result.created,
                                        result.updated,
                                        result.deleted);

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
