#pragma once
#include "http.hpp"
#include "timegrip.hpp"
#include <map>
#include <string>
#include <vector>

struct GoogleTokens {
    std::string access_token;
    std::string refresh_token;
};

// Headless if refresh_token is set; otherwise launches browser OAuth2 flow.
GoogleTokens google_auth(const std::string& client_id,
                         const std::string& client_secret,
                         const std::string& refresh_token = "");

std::string get_or_create_calendar(const std::string& access_token,
                                   const std::string& calendar_name);

struct CalendarEvent {
    std::string id;
    std::string summary;
    std::string description;
    std::string start;
    std::string end;
    bool        all_day = false;
    std::string timegrip_id;
};

std::vector<CalendarEvent> fetch_managed_events(const std::string& access_token,
                                                const std::string& calendar_id,
                                                const std::string& from_date,
                                                const std::string& to_date);

// DB snapshot of a single synced shift.
struct ShiftSnapshot {
    std::string gcal_event_id;
    std::string summary;
    std::string start;
    std::string end;
    bool        all_day = false;
};

struct ShiftChange {
    enum class Type { Created, Updated, Deleted, Unchanged };
    Type        type;
    std::string timegrip_id;
    std::string gcal_event_id;
    // current state (empty for Deleted)
    std::string summary;
    std::string start;
    std::string end;
    bool        all_day   = false;
    // previous state (populated for Updated/Deleted)
    std::string old_summary;
    std::string old_start;
    std::string old_end;
};

struct SyncResult {
    int created   = 0;
    int updated   = 0;
    int deleted   = 0;
    int unchanged = 0;
    std::vector<ShiftChange> changes;
};

// protect_before: events starting before this date (YYYY-MM-DD) are never touched.
SyncResult sync_calendar(const std::string& access_token,
                         const std::string& calendar_id,
                         const Timetable&   timetable,
                         const FunctionMap& func_map,
                         std::map<std::string, ShiftSnapshot> snapshot,
                         const std::string& protect_before,
                         const std::string& from_date,
                         const std::string& to_date);
