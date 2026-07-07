#include "timegrip.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

// Parse a JSON value that may be number or numeric string.
template<typename T>
static T jnum(const json& j, const std::string& key, T def) {
    if (!j.contains(key)) return def;
    auto& v = j[key];
    if (v.is_null())   return def;
    if (v.is_number()) return v.get<T>();
    if (v.is_string()) {
        auto s = v.get<std::string>();
        if (s.empty()) return def;
        try {
            if constexpr (std::is_floating_point_v<T>)
                return static_cast<T>(std::stod(s));
            else
                return static_cast<T>(std::stoll(s));
        } catch (...) { return def; }
    }
    return def;
}

static std::string iso_now(int offset_weeks = 0) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    t += offset_weeks * 7 * 24 * 3600;
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    if (offset_weeks == 0)
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT00:00:00.000", &tm);
    else
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

Timetable fetch_timetable(HttpClient& session, const std::string& base_url,
                          int weeks_ahead, int weeks_back) {
    auto r = session.get(base_url + "/webapi/?func=MyWorktimes", {
        {"from_date", iso_now(-weeks_back)},
        {"to_date",   iso_now(weeks_ahead)},
    });
    if (r.status != 200)
        throw HttpError(r.status, "MyWorktimes returned " + std::to_string(r.status));

    auto j = json::parse(r.body);

    if (j.contains("isSuccess") && j["isSuccess"] == false)
        throw std::runtime_error("MyWorktimes API error: " +
            j.value("errors", json::array())[0].get<std::string>());

    Timetable tt;
    for (auto& jw : j.at("weeks")) {
        Week w;
        w.week = jnum<int>(jw, "week", 0);
        w.year = jnum<int>(jw, "year", 0);

        for (auto& jd : jw.value("days", json::array())) {
            Day d;
            d.date = jd.value("date", "");

            for (auto& js : jd.value("shifts", json::array())) {
                Shift s;
                s.worktime_id = jnum<long long>(js, "worktime_id", 0LL);
                s.start_time  = js.value("start_time",  "");
                s.end_time    = js.value("end_time",    "");
                s.duration    = jnum<int>(js, "duration", 0);

                int pause = jnum<int>(js, "start_pause", 0);
                if (pause == 0) pause = jnum<int>(js, "actual_pause", 0);
                s.pause = pause;

                if (js.contains("details")) {
                    for (auto& jdet : js["details"]) {
                        ShiftDetail det;
                        det.start_time   = jdet.value("start_time",   "");
                        det.end_time     = jdet.value("end_time",     "");
                        det.function_id  = jnum<int>(jdet, "function_id", 0);
                        s.details.push_back(std::move(det));
                    }
                }

                if (js.contains("absence") && !js["absence"].is_null()) {
                    s.has_absence           = true;
                    s.absence.type_caption  = js["absence"].value("type_caption", "Absence");
                }

                d.shifts.push_back(std::move(s));
            }
            w.days.push_back(std::move(d));
        }
        tt.weeks.push_back(std::move(w));
    }
    return tt;
}

FunctionMap fetch_function_names(HttpClient& session, const std::string& base_url) {
    auto r = session.get(base_url + "/webapi/?func=LoadSetting");
    if (r.status != 200)
        throw HttpError(r.status, "LoadSetting returned " + std::to_string(r.status));

    auto j = json::parse(r.body);
    FunctionMap m;
    for (auto& f : j.at("LoadSetting")[0].at("jobFunctions"))
        m[std::to_string(jnum<int>(f, "id", 0))] = f.value("name", "");
    return m;
}
