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

#ifdef HAVE_MYSQL

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
        "  total_shifts INT          NOT NULL DEFAULT 0,"
        "  created      INT          NOT NULL DEFAULT 0,"
        "  updated      INT          NOT NULL DEFAULT 0,"
        "  deleted      INT          NOT NULL DEFAULT 0,"
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
        "  id           INT          NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "  sync_run_id  INT          NOT NULL,"
        "  changed_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  timegrip_id  VARCHAR(64)  NOT NULL,"
        "  change_type  VARCHAR(16)  NOT NULL,"
        "  old_summary  VARCHAR(255) DEFAULT NULL,"
        "  old_start_dt VARCHAR(32)  DEFAULT NULL,"
        "  old_end_dt   VARCHAR(32)  DEFAULT NULL,"
        "  new_summary  VARCHAR(255) DEFAULT NULL,"
        "  new_start_dt VARCHAR(32)  DEFAULT NULL,"
        "  new_end_dt   VARCHAR(32)  DEFAULT NULL,"
        "  FOREIGN KEY (sync_run_id) REFERENCES sync_runs(id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    );
}

int64_t ShiftTracker::begin_sync_run(int total_shifts) {
    db_.execute(
        "INSERT INTO sync_runs (total_shifts) VALUES (?)",
        {std::to_string(total_shifts)}
    );
    return db_.last_insert_id();
}

void ShiftTracker::finish_sync_run(int64_t run_id,
                                   int created, int updated, int deleted,
                                   const std::string& error_msg) {
    db_.execute(
        "UPDATE sync_runs SET created=?, updated=?, deleted=?, error_msg=? WHERE id=?",
        {std::to_string(created), std::to_string(updated), std::to_string(deleted),
         error_msg, std::to_string(run_id)}
    );
}

void ShiftTracker::apply_changes(int64_t run_id,
                                  const std::vector<ShiftChange>& changes) {
    for (auto& ch : changes) {
        std::string type_str;
        switch (ch.type) {
            case ShiftChange::Type::Created: type_str = "created"; break;
            case ShiftChange::Type::Updated: type_str = "updated"; break;
            case ShiftChange::Type::Deleted: type_str = "deleted"; break;
        }

        if (ch.type != ShiftChange::Type::Deleted) {
            // Upsert current state into the shifts snapshot table.
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
            // Remove from the live snapshot — history stays in shift_changes.
            db_.execute(
                "DELETE FROM shifts WHERE timegrip_id = ?",
                {ch.timegrip_id}
            );
        }

        // Append to the immutable changelog.
        db_.execute(
            "INSERT INTO shift_changes"
            "  (sync_run_id, timegrip_id, change_type,"
            "   old_summary, old_start_dt, old_end_dt,"
            "   new_summary, new_start_dt, new_end_dt)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            {std::to_string(run_id), ch.timegrip_id, type_str,
             ch.old_summary, ch.old_start, ch.old_end,
             ch.summary,     ch.start,     ch.end}
        );
    }
}

#endif  // HAVE_MYSQL
