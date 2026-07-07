#include "db.hpp"
#include "env.hpp"
#include <vector>

DbConfig db_config_from_env() {
    return {
        get_env("DB_HOST",     "localhost"),
        get_env("DB_PORT",     "3306"),
        get_env("DB_USER"),
        get_env("DB_PASSWORD"),
        get_env("DB_DATABASE"),
    };
}

void MySQLDatabase::connect() {
    conn_ = mysql_init(nullptr);
    if (!conn_) throw std::runtime_error("mysql_init failed");

    unsigned int port = static_cast<unsigned int>(std::stoul(cfg_.port));
    if (!mysql_real_connect(conn_,
                            cfg_.host.c_str(),
                            cfg_.user.c_str(),
                            cfg_.password.c_str(),
                            cfg_.database.c_str(),
                            port, nullptr, 0)) {
        std::string err = mysql_error(conn_);
        mysql_close(conn_);
        conn_ = nullptr;
        throw std::runtime_error("MySQL connect failed: " + err);
    }

    mysql_set_character_set(conn_, "utf8mb4");
    connected_ = true;
}

void MySQLDatabase::disconnect() {
    if (conn_) {
        mysql_close(conn_);
        conn_      = nullptr;
        connected_ = false;
    }
}

void MySQLDatabase::check_error(const std::string& ctx) {
    if (mysql_errno(conn_))
        throw std::runtime_error(ctx + ": " + mysql_error(conn_));
}

Result MySQLDatabase::query(const std::string& sql,
                            const std::vector<std::string>& params) {
    if (!connected_) throw std::runtime_error("Not connected to database");

    std::string stmt = sql;
    std::size_t pos  = 0;
    for (auto& p : params) {
        pos = stmt.find('?', pos);
        if (pos == std::string::npos) break;
        std::vector<char> buf(p.size() * 2 + 1);
        mysql_real_escape_string(conn_, buf.data(), p.c_str(),
                                 static_cast<unsigned long>(p.size()));
        stmt.replace(pos, 1, "'" + std::string(buf.data()) + "'");
        pos += p.size() + 2;
    }

    if (mysql_query(conn_, stmt.c_str()))
        check_error("query");

    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res && mysql_field_count(conn_) > 0)
        check_error("store_result");

    Result rows;
    if (!res) return rows;

    unsigned int n_fields = mysql_num_fields(res);
    MYSQL_FIELD* fields   = mysql_fetch_fields(res);

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        unsigned long* lengths = mysql_fetch_lengths(res);
        Row r;
        for (unsigned int i = 0; i < n_fields; ++i)
            r[fields[i].name] = row[i] ? std::string(row[i], lengths[i]) : "";
        rows.push_back(std::move(r));
    }

    mysql_free_result(res);
    return rows;
}

void MySQLDatabase::execute(const std::string& sql,
                            const std::vector<std::string>& params) {
    query(sql, params);
}

int64_t MySQLDatabase::last_insert_id() {
    return static_cast<int64_t>(mysql_insert_id(conn_));
}

void ShiftTracker::ensure_schema() {
    db_.execute(
        "CREATE TABLE IF NOT EXISTS sync_runs ("
        "  id           INT          NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "  run_at       DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  success      TINYINT(1)   NOT NULL DEFAULT 0,"
        "  total_shifts INT          NOT NULL DEFAULT 0,"
        "  created      INT          NOT NULL DEFAULT 0,"
        "  updated      INT          NOT NULL DEFAULT 0,"
        "  deleted      INT          NOT NULL DEFAULT 0,"
        "  change_ids   JSON         DEFAULT NULL,"
        "  error_msg    VARCHAR(512) DEFAULT NULL"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    );

    db_.execute(
        "CREATE TABLE IF NOT EXISTS shifts ("
        "  timegrip_id     VARCHAR(64)  NOT NULL PRIMARY KEY,"
        "  gcal_event_id   VARCHAR(255) DEFAULT NULL,"
        "  summary         VARCHAR(255) NOT NULL DEFAULT '',"
        "  start_dt        VARCHAR(32)  NOT NULL DEFAULT '',"
        "  end_dt          VARCHAR(32)  NOT NULL DEFAULT '',"
        "  all_day         TINYINT(1)   NOT NULL DEFAULT 0,"
        "  first_seen_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  last_seen_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  last_changed_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    );

    db_.execute(
        "CREATE TABLE IF NOT EXISTS shift_changes ("
        "  id            INT          NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "  changed_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  timegrip_id   VARCHAR(64)  NOT NULL,"
        "  change_type   VARCHAR(16)  NOT NULL,"
        "  gcal_event_id VARCHAR(255) DEFAULT NULL,"
        "  old_summary   VARCHAR(255) DEFAULT NULL,"
        "  old_start_dt  VARCHAR(32)  DEFAULT NULL,"
        "  old_end_dt    VARCHAR(32)  DEFAULT NULL,"
        "  new_summary   VARCHAR(255) DEFAULT NULL,"
        "  new_start_dt  VARCHAR(32)  DEFAULT NULL,"
        "  new_end_dt    VARCHAR(32)  DEFAULT NULL"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    );
}

std::map<std::string, ShiftSnapshot> ShiftTracker::fetch_snapshot(
        const std::string& from_date, const std::string& to_date) {
    auto rows = db_.query(
        "SELECT timegrip_id, gcal_event_id, summary, start_dt, end_dt, all_day"
        " FROM shifts WHERE start_dt >= ? AND start_dt <= ?",
        {from_date, to_date + "T23:59:59"});
    std::map<std::string, ShiftSnapshot> snap;
    for (auto& r : rows) {
        ShiftSnapshot s;
        s.gcal_event_id = r.at("gcal_event_id");
        s.summary       = r.at("summary");
        s.start         = r.at("start_dt");
        s.end           = r.at("end_dt");
        s.all_day       = r.at("all_day") == "1";
        snap[r.at("timegrip_id")] = s;
    }
    return snap;
}

int64_t ShiftTracker::begin_sync_run(int total_shifts) {
    db_.execute(
        "INSERT INTO sync_runs (total_shifts) VALUES (?)",
        {std::to_string(total_shifts)}
    );
    return db_.last_insert_id();
}

std::vector<int64_t> ShiftTracker::apply_changes(
        const std::vector<ShiftChange>& changes) {
    std::vector<int64_t> ids;
    ids.reserve(changes.size());

    for (auto& ch : changes) {
        if (ch.type == ShiftChange::Type::Unchanged) {
            db_.execute("UPDATE shifts SET last_seen_at = NOW() WHERE timegrip_id = ?",
                        {ch.timegrip_id});
            continue;
        }

        std::string type_str;
        switch (ch.type) {
            case ShiftChange::Type::Created:   type_str = "created"; break;
            case ShiftChange::Type::Updated:   type_str = "updated"; break;
            case ShiftChange::Type::Deleted:   type_str = "deleted"; break;
            case ShiftChange::Type::Unchanged: break;
        }

        if (ch.type != ShiftChange::Type::Deleted) {
            db_.execute(
                "INSERT INTO shifts"
                "  (timegrip_id, gcal_event_id, summary, start_dt, end_dt, all_day)"
                " VALUES (?, ?, ?, ?, ?, ?)"
                " ON DUPLICATE KEY UPDATE"
                "  gcal_event_id   = VALUES(gcal_event_id),"
                "  summary         = VALUES(summary),"
                "  start_dt        = VALUES(start_dt),"
                "  end_dt          = VALUES(end_dt),"
                "  all_day         = VALUES(all_day),"
                "  last_seen_at    = NOW(),"
                "  last_changed_at = NOW()",
                {ch.timegrip_id, ch.gcal_event_id, ch.summary,
                 ch.start, ch.end, ch.all_day ? "1" : "0"}
            );
        } else {
            db_.execute("DELETE FROM shifts WHERE timegrip_id = ?",
                        {ch.timegrip_id});
        }

        db_.execute(
            "INSERT INTO shift_changes"
            "  (timegrip_id, change_type, gcal_event_id,"
            "   old_summary, old_start_dt, old_end_dt,"
            "   new_summary, new_start_dt, new_end_dt)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            {ch.timegrip_id, type_str, ch.gcal_event_id,
             ch.old_summary, ch.old_start, ch.old_end,
             ch.summary,     ch.start,     ch.end}
        );
        ids.push_back(db_.last_insert_id());
    }

    return ids;
}

void ShiftTracker::finish_sync_run(int64_t run_id, bool success,
                                   int created, int updated, int deleted,
                                   const std::vector<int64_t>& change_ids,
                                   const std::string& error_msg) {
    std::string json = "[";
    for (size_t i = 0; i < change_ids.size(); ++i) {
        if (i) json += ",";
        json += std::to_string(change_ids[i]);
    }
    json += "]";

    db_.execute(
        "UPDATE sync_runs"
        " SET success=?, created=?, updated=?, deleted=?,"
        "     change_ids=?, error_msg=?"
        " WHERE id=?",
        {success ? "1" : "0",
         std::to_string(created), std::to_string(updated), std::to_string(deleted),
         json, error_msg, std::to_string(run_id)}
    );
}

void PeriodArchive::ensure_schema() {
    db_.execute(
        "CREATE TABLE IF NOT EXISTS pay_periods ("
        "  id                INT          NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "  period_start      DATE         NOT NULL,"
        "  period_end        DATE         NOT NULL,"
        "  pay_date          DATE         NOT NULL,"
        "  shift_count       INT          NOT NULL DEFAULT 0,"
        "  total_hours       DECIMAL(6,2) NOT NULL DEFAULT 0,"
        "  brutto_dkk        DECIMAL(10,2) NOT NULL DEFAULT 0,"
        "  net_estimated_dkk DECIMAL(10,2) NOT NULL DEFAULT 0,"
        "  fritvalg_dkk      DECIMAL(10,2) NOT NULL DEFAULT 0,"
        "  feriefri_dkk      DECIMAL(10,2) NOT NULL DEFAULT 0,"
        "  html_content      LONGTEXT     DEFAULT NULL,"
        "  locked            TINYINT(1)   NOT NULL DEFAULT 0,"
        "  generated_at      DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP"
        "                    ON UPDATE CURRENT_TIMESTAMP,"
        "  UNIQUE KEY (period_start)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    );
}

bool PeriodArchive::is_locked(int pay_month, int pay_year) {
    int sm = pay_month - 1, sy = pay_year;
    if (sm < 1) { sm = 12; --sy; }
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-16", sy, sm);
    auto rows = db_.query(
        "SELECT locked FROM pay_periods WHERE period_start = ? LIMIT 1",
        {buf}
    );
    return !rows.empty() && rows[0]["locked"] == "1";
}

void PeriodArchive::upsert_period(const PeriodData& d, bool lock) {
    char ps[16], pe[16], pd[16];
    snprintf(ps, sizeof(ps), "%04d-%02d-%02d",
             d.period_start_year, d.period_start_month, d.period_start_day);
    snprintf(pe, sizeof(pe), "%04d-%02d-%02d",
             d.period_end_year, d.period_end_month, d.period_end_day);
    int last = days_in_month(d.pay_month, d.pay_year);
    snprintf(pd, sizeof(pd), "%04d-%02d-%02d", d.pay_year, d.pay_month, last);

    auto dstr = [](double v) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", v);
        return std::string(buf);
    };

    db_.execute(
        "INSERT INTO pay_periods"
        "  (period_start, period_end, pay_date, shift_count, total_hours,"
        "   brutto_dkk, net_estimated_dkk, fritvalg_dkk, feriefri_dkk,"
        "   html_content, locked)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?)"
        " ON DUPLICATE KEY UPDATE"
        "  period_end        = VALUES(period_end),"
        "  pay_date          = VALUES(pay_date),"
        "  shift_count       = VALUES(shift_count),"
        "  total_hours       = VALUES(total_hours),"
        "  brutto_dkk        = VALUES(brutto_dkk),"
        "  net_estimated_dkk = VALUES(net_estimated_dkk),"
        "  fritvalg_dkk      = VALUES(fritvalg_dkk),"
        "  feriefri_dkk      = VALUES(feriefri_dkk),"
        "  html_content      = VALUES(html_content),"
        "  locked            = IF(locked=1, 1, VALUES(locked))",
        {ps, pe, pd,
         std::to_string(d.shift_count),
         dstr(d.total_hours),
         dstr(d.brutto_dkk),
         dstr(d.net_estimated_dkk),
         dstr(d.fritvalg_dkk),
         dstr(d.feriefri_dkk),
         d.html_content,
         lock ? "1" : "0"}
    );
}
