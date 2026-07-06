#pragma once
#include "http.hpp"
#include <map>
#include <string>
#include <vector>

struct Absence {
    std::string type_caption;
};

struct ShiftDetail {
    std::string start_time;
    std::string end_time;
    int         function_id = 0;
};

struct Shift {
    long long              worktime_id = 0;
    std::string            start_time;
    std::string            end_time;
    int                    duration    = 0;  // minutes
    int                    pause       = 0;  // minutes (start_pause or actual_pause)
    std::vector<ShiftDetail> details;
    bool                   has_absence = false;
    Absence                absence;
};

struct Day {
    std::string         date;   // "DD-MM-YYYY" from API
    std::vector<Shift>  shifts;
};

struct Week {
    int              week = 0;
    int              year = 0;
    std::vector<Day> days;
};

struct Timetable {
    std::vector<Week> weeks;
};

using FunctionMap = std::map<std::string, std::string>;  // id → name

Timetable   fetch_timetable(HttpClient& session, int weeks_ahead = 12, int weeks_back = 0);
FunctionMap fetch_function_names(HttpClient& session);
