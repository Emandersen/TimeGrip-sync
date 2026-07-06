#include "gcal.hpp"
#include <algorithm>
#include <cstring>
#include <curl/curl.h>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <regex>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

using json = nlohmann::json;

static const std::string TIMEZONE      = "Europe/Copenhagen";
static const std::string TOKEN_URI     = "https://oauth2.googleapis.com/token";
static const std::string CALENDAR_API  = "https://www.googleapis.com/calendar/v3";
static const std::string AUTH_URI      = "https://accounts.google.com/o/oauth2/auth";

class GCalClient {
public:
    explicit GCalClient(std::string token) : token_(std::move(token)) {}

    json get(const std::string& url, const Params& params = {}) {
        return request("GET", url, params, "");
    }

    json post(const std::string& url, const json& body) {
        return request("POST", url, {}, body.dump());
    }

    json patch(const std::string& url, const json& body) {
        return request("PATCH", url, {}, body.dump());
    }

    void del(const std::string& url) {
        request("DELETE", url, {}, "");
    }

private:
    std::string token_;

    json request(const std::string& method, const std::string& url,
                 const Params& params, const std::string& body) {
        HttpClient client;
        client.set_user_agent("timegrip-sync/1.0");

        std::string full_url = url;
        if (!params.empty()) {
            full_url += '?';
            bool first = true;
            for (auto& [k, v] : params) {
                if (!first) full_url += '&';
                full_url += k + "=" + v;
                first = false;
            }
        }

        Headers hdrs = {{"Authorization", "Bearer " + token_}};

        if (method == "GET") {
            return do_curl(method, full_url, body, hdrs);
        }
        if (method == "DELETE") {
            return do_curl(method, full_url, "", hdrs);
        }
        return do_curl(method, full_url, body, hdrs);
    }

    json do_curl(const std::string& method, const std::string& url,
                 const std::string& body, const Headers& headers) {
        CURL* c = curl_easy_init();
        if (!c) throw std::runtime_error("curl_easy_init failed");

        struct Guard { CURL* h; ~Guard() { curl_easy_cleanup(h); } } g{c};

        std::string resp_body;
        auto write_cb = [](char* p, size_t s, size_t n, void* ud) -> size_t {
            static_cast<std::string*>(ud)->append(p, s * n);
            return s * n;
        };

        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                         static_cast<size_t(*)(char*, size_t, size_t, void*)>(write_cb));
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp_body);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);

        curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        for (auto& [k, v] : headers)
            hdrs = curl_slist_append(hdrs, (k + ": " + v).c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);

        if (method == "POST") {
            curl_easy_setopt(c, CURLOPT_POST, 1L);
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        } else if (method == "PATCH") {
            curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PATCH");
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        } else if (method == "DELETE") {
            curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
        }

        CURLcode rc = curl_easy_perform(c);
        curl_slist_free_all(hdrs);

        if (rc != CURLE_OK)
            throw std::runtime_error(std::string("curl failed: ") +
                                     curl_easy_strerror(rc));

        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);

        if (status == 204 || resp_body.empty()) return nullptr;
        if (status >= 400)
            throw HttpError(status, "API error " + std::to_string(status) +
                                    ": " + resp_body);

        return json::parse(resp_body);
    }
};

static int listen_on_random_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed");
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;  // OS picks port
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0)
        throw std::runtime_error("bind() failed");
    if (listen(fd, 1) < 0) throw std::runtime_error("listen() failed");
    return fd;
}

static int get_port(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    getsockname(fd, (sockaddr*)&addr, &len);
    return ntohs(addr.sin_port);
}

static std::string accept_code(int server_fd) {
    int client = accept(server_fd, nullptr, nullptr);
    if (client < 0) throw std::runtime_error("accept() failed");

    char buf[4096] = {};
    recv(client, buf, sizeof(buf) - 1, 0);

    const char* resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n\r\n"
        "<html><body><h2>Authorization successful — you can close this tab.</h2></body></html>";
    send(client, resp, strlen(resp), 0);
    close(client);

    // Extract code= from GET request line: "GET /?code=XXX&... HTTP/1.1"
    std::cmatch m;
    std::regex re(R"(code=([^& \r\n]+))");
    if (!std::regex_search(buf, m, re))
        throw std::runtime_error("OAuth callback did not contain a code");
    return m[1].str();
}

// Headless if refresh_token is set; otherwise launches browser OAuth2 flow.
GoogleTokens google_auth(const std::string& client_id,
                         const std::string& client_secret,
                         const std::string& refresh_token) {
    HttpClient client;

    if (!refresh_token.empty()) {
        auto r = client.post_form(TOKEN_URI, {
            {"client_id",     client_id},
            {"client_secret", client_secret},
            {"refresh_token", refresh_token},
            {"grant_type",    "refresh_token"},
        });
        if (r.status != 200)
            throw HttpError(r.status, "Token refresh failed: " + r.body);

        auto j = json::parse(r.body);
        return {j.at("access_token").get<std::string>(), refresh_token};
    }

    int server_fd = listen_on_random_port();
    int port = get_port(server_fd);
    std::string redirect_uri = "http://localhost:" + std::to_string(port) + "/";

    std::string auth_url = AUTH_URI
        + "?client_id="     + client_id
        + "&redirect_uri="  + redirect_uri
        + "&response_type=code"
        + "&scope=https://www.googleapis.com/auth/calendar"
        + "&access_type=offline"
        + "&prompt=consent";

    std::cout << "\nPlease visit this URL to authorise:\n" << auth_url << "\n\n";
    (void)system(("xdg-open '" + auth_url + "' 2>/dev/null &").c_str());

    std::string code = accept_code(server_fd);
    close(server_fd);

    auto r = client.post_form(TOKEN_URI, {
        {"code",          code},
        {"client_id",     client_id},
        {"client_secret", client_secret},
        {"redirect_uri",  redirect_uri},
        {"grant_type",    "authorization_code"},
    });
    if (r.status != 200)
        throw HttpError(r.status, "Code exchange failed: " + r.body);

    auto j = json::parse(r.body);
    GoogleTokens tokens;
    tokens.access_token  = j.at("access_token").get<std::string>();
    tokens.refresh_token = j.value("refresh_token", "");

    if (!tokens.refresh_token.empty())
        std::cout << "\n  Refresh token (save to .env as GOOGLE_REFRESH_TOKEN):\n"
                  << "  " << tokens.refresh_token << "\n\n";

    return tokens;
}

std::string get_or_create_calendar(const std::string& access_token,
                                   const std::string& calendar_name) {
    GCalClient gc(access_token);

    auto list = gc.get(CALENDAR_API + "/users/me/calendarList");
    for (auto& item : list.value("items", json::array())) {
        if (item.value("summary", "") == calendar_name)
            return item.at("id").get<std::string>();
    }

    auto created = gc.post(CALENDAR_API + "/calendars", {
        {"summary",  calendar_name},
        {"timeZone", TIMEZONE},
    });
    std::cout << "  Created calendar '" << calendar_name << "'\n";
    return created.at("id").get<std::string>();
}

std::vector<CalendarEvent> fetch_managed_events(const std::string& access_token,
                                                const std::string& calendar_id,
                                                const std::string& from_date,
                                                const std::string& to_date) {
    GCalClient gc(access_token);
    std::vector<CalendarEvent> events;
    std::string page_token;

    do {
        Params params = {
            {"timeMin",                 from_date + "T00:00:00Z"},
            {"timeMax",                 to_date   + "T23:59:59Z"},
            {"privateExtendedProperty", "source=timegrip"},
            {"singleEvents",            "true"},
            {"maxResults",              "250"},
        };
        if (!page_token.empty()) params["pageToken"] = page_token;

        auto resp = gc.get(CALENDAR_API + "/calendars/" + calendar_id + "/events",
                           params);

        for (auto& item : resp.value("items", json::array())) {
            CalendarEvent ev;
            ev.id          = item.value("id", "");
            ev.summary     = item.value("summary", "");
            ev.description = item.value("description", "");

            auto& s = item["start"];
            if (s.contains("date")) {
                ev.start   = s["date"].get<std::string>();
                ev.all_day = true;
            } else {
                ev.start = s.value("dateTime", "");
            }
            auto& e = item["end"];
            ev.end = e.contains("date") ? e["date"].get<std::string>()
                                        : e.value("dateTime", "");

            ev.timegrip_id = item.value(
                json::json_pointer("/extendedProperties/private/timegrip_id"), "");

            if (!ev.timegrip_id.empty())
                events.push_back(std::move(ev));
        }

        page_token = resp.value("nextPageToken", "");
    } while (!page_token.empty());

    return events;
}

static const std::vector<std::string> VACATION_TYPES = {"ferie", "fritimer"};
static const std::vector<std::string> SKIP_TYPES     = {"ph-reduktion", "ph-reduction"};

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static bool contains_any(const std::string& s,
                          const std::vector<std::string>& needles) {
    auto sl = to_lower(s);
    for (auto& n : needles)
        if (sl.find(n) != std::string::npos) return true;
    return false;
}

static std::string parse_time(const std::string& iso) {
    auto t = iso.find('T');
    if (t != std::string::npos && iso.size() > t + 5)
        return iso.substr(t + 1, 5);
    return "?";
}

// Extract "YYYY-MM-DD" from "DD-MM-YYYY" or ISO string.
static std::string parse_date(const std::string& s) {
    if (s.size() >= 10 && s[4] == '-') return s.substr(0, 10);
    if (s.size() == 10 && s[2] == '-' && s[5] == '-')
        return s.substr(6, 4) + "-" + s.substr(3, 2) + "-" + s.substr(0, 2);
    if (s.size() > 10 && s[4] == '-') return s.substr(0, 10);
    return s;
}

static std::string format_duration(int minutes) {
    int h = minutes / 60, m = minutes % 60;
    if (m) return std::to_string(h) + "h " + std::to_string(m) + "m";
    return std::to_string(h) + "h";
}

static json build_event_body(const Shift& s, const std::string& day_date,
                             const FunctionMap& func_map,
                             const std::string& timegrip_id) {
    json ext = {{"source", "timegrip"}, {"timegrip_id", timegrip_id}};

    if (s.has_absence && !s.worktime_id) {
        // Pure all-day absence
        return {
            {"summary",     s.absence.type_caption + " — guaranteed no work"},
            {"start",       {{"date", day_date}}},
            {"end",         {{"date", day_date}}},
            {"extendedProperties", {{"private", ext}}},
        };
    }

    auto start_t  = parse_time(s.start_time);
    auto end_t    = parse_time(s.end_time);
    auto date_str = parse_date(s.start_time.empty() ? day_date : s.start_time);

    std::string summary = "Netto \xe2\x80\x94 " + start_t + "\xe2\x80\x93" + end_t;

    std::string desc = format_duration(s.duration);
    if (s.pause) desc += " \xc2\xb7 " + std::to_string(s.pause) + "m break";
    if (!s.details.empty()) {
        auto fid = std::to_string(s.details[0].function_id);
        auto it  = func_map.find(fid);
        if (it != func_map.end()) desc += " \xc2\xb7 " + it->second;
    }

    return {
        {"summary",     summary},
        {"description", desc},
        {"start",       {{"dateTime", date_str + "T" + start_t + ":00"},
                         {"timeZone", TIMEZONE}}},
        {"end",         {{"dateTime", date_str + "T" + end_t   + ":00"},
                         {"timeZone", TIMEZONE}}},
        {"extendedProperties", {{"private", ext}}},
    };
}

// Strip timezone offset/Z so "2026-07-08T16:00:00+02:00" == "2026-07-08T16:00:00".
static std::string norm_dt(const std::string& dt) {
    if (dt.size() > 19 && dt[10] == 'T') return dt.substr(0, 19);
    return dt;
}

static bool events_differ(const CalendarEvent& existing, const json& desired) {
    if (existing.summary != desired.value("summary", "")) return true;
    if (existing.description != desired.value("description", "")) return true;

    std::string d_start = desired.contains("start")
        ? (desired["start"].contains("dateTime")
               ? desired["start"]["dateTime"].get<std::string>()
               : desired["start"].value("date", ""))
        : "";
    std::string d_end = desired.contains("end")
        ? (desired["end"].contains("dateTime")
               ? desired["end"]["dateTime"].get<std::string>()
               : desired["end"].value("date", ""))
        : "";

    if (norm_dt(existing.start) != norm_dt(d_start)) return true;
    if (norm_dt(existing.end)   != norm_dt(d_end))   return true;
    return false;
}

// protect_before: events starting before this date (YYYY-MM-DD) are never touched.
SyncResult sync_calendar(const std::string& access_token,
                         const std::string& calendar_id,
                         const Timetable&   timetable,
                         const FunctionMap& func_map,
                         const std::string& from_date,
                         const std::string& to_date) {
    GCalClient gc(access_token);

    std::map<std::string, json> desired;

    for (auto& week : timetable.weeks) {
        for (auto& day : week.days) {
            std::string day_date = parse_date(day.date);
            for (auto& shift : day.shifts) {
                std::string caption_lc = shift.has_absence
                    ? to_lower(shift.absence.type_caption) : "";

                if (contains_any(caption_lc, SKIP_TYPES)) continue;

                std::string tid;
                if (shift.has_absence && (!shift.worktime_id ||
                        contains_any(caption_lc, VACATION_TYPES))) {
                    tid = "absence_" + day_date + "_" + caption_lc;
                } else if (shift.worktime_id) {
                    tid = std::to_string(shift.worktime_id);
                } else if (!shift.start_time.empty()) {
                    // No worktime_id — use date+start as stable composite key
                    tid = "shift_" + day_date + "_" + parse_time(shift.start_time);
                } else {
                    continue;
                }

                desired[tid] = build_event_body(shift, day_date, func_map, tid);
            }
        }
    }

    auto existing_vec = fetch_managed_events(access_token, calendar_id,
                                             from_date, to_date);
    std::map<std::string, CalendarEvent> existing;
    for (auto& ev : existing_vec)
        existing[ev.timegrip_id] = ev;

    SyncResult result;
    std::string cal_path = CALENDAR_API + "/calendars/" + calendar_id + "/events";

    auto extract_dt = [](const json& body, bool all_day) -> std::pair<std::string,std::string> {
        std::string s, e;
        if (body.contains("start"))
            s = all_day ? body["start"].value("date","") : body["start"].value("dateTime","");
        if (body.contains("end"))
            e = all_day ? body["end"].value("date","")   : body["end"].value("dateTime","");
        return {s, e};
    };

    for (auto& [tid, body] : desired) {
        bool all_day = body.contains("start") && body["start"].contains("date");

        if (existing.find(tid) == existing.end()) {
            auto resp = gc.post(cal_path, body);
            ++result.created;

            auto [s, e] = extract_dt(body, all_day);
            result.changes.push_back({
                ShiftChange::Type::Created,
                tid,
                resp.is_null() ? "" : resp.value("id", ""),
                body.value("summary", ""), s, e, all_day,
                {}, {}, {}
            });
        } else if (events_differ(existing.at(tid), body)) {
            auto& ev = existing.at(tid);
            auto patched = body;
            patched["id"] = ev.id;
            gc.patch(cal_path + "/" + ev.id, patched);
            ++result.updated;

            auto [s, e] = extract_dt(body, all_day);
            result.changes.push_back({
                ShiftChange::Type::Updated,
                tid, ev.id,
                body.value("summary", ""), s, e, all_day,
                ev.summary, ev.start, ev.end
            });
        }
    }

    for (auto& [tid, ev] : existing) {
        if (desired.find(tid) == desired.end()) {
            gc.del(cal_path + "/" + ev.id);
            ++result.deleted;

            result.changes.push_back({
                ShiftChange::Type::Deleted,
                tid, ev.id,
                {}, {}, {}, ev.all_day,
                ev.summary, ev.start, ev.end
            });
        }
    }

    return result;
}
