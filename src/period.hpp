#pragma once
#include <string>

inline int days_in_month(int mo, int y) {
    static const int t[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return t[mo];
}

struct PeriodData {
    // Period bounds (lønperiode: 16th of prev month → 15th of pay_month)
    int period_start_day,   period_start_month,   period_start_year;
    int period_end_day,     period_end_month,     period_end_year;
    int pay_month, pay_year; // pay_date = last day of pay_month/pay_year

    int    shift_count       = 0;
    double total_hours       = 0.0;
    double brutto_dkk        = 0.0;
    double net_estimated_dkk = 0.0;
    double fritvalg_dkk      = 0.0;
    double feriefri_dkk      = 0.0;

    std::string section_html; // <section>…</section> fragment (transient, not stored in DB)
    std::string html_content; // complete standalone <!DOCTYPE html>…</html> (stored in DB)
};
