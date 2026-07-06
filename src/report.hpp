#pragma once
#include "timegrip.hpp"
#include <string>

struct PayConfig {
    double hourly_rate          = 0.0;
    double evening_supplement   = 0.0;     // DKK/hr for hours after evening_cutoff
    double saturday_supplement  = 0.0;     // DKK/hr for all Saturday hours
    double sunday_supplement    = 0.0;     // DKK/hr for all Sunday hours
    int    evening_cutoff_hour  = 18;      // 24h hour when evening rate kicks in
    double am_bidrag_pct        = 8.0;
    double tax_pct              = 0.0;     // bikort, no personfradrag
    double employee_pension_pct = 0.0;
    double employer_pension_pct = 0.0;     // employer-paid, shown informatively
    double atp_dkk              = 0.0;     // per lønperiode
    double klub_dkk             = 0.0;     // per lønperiode
    double fritvalg_pct         = 0.0;     // opsparet, paid out separately
    double feriefri_pct         = 0.0;     // feriefri til fritvalg
    int    advance_notice_days  = 14;
};

PayConfig pay_config_from_env();

void generate_report(const std::string& out_path,
                     const Timetable& tt,
                     const FunctionMap& func_map,
                     const PayConfig& cfg);
