#pragma once
#include "period.hpp"
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using Row    = std::map<std::string, std::string>;
using Result = std::vector<Row>;

class Database {
public:
    virtual ~Database() = default;

    virtual void connect()    = 0;
    virtual void disconnect() = 0;

    virtual Result  query(const std::string& sql,
                          const std::vector<std::string>& params = {}) = 0;
    virtual void    execute(const std::string& sql,
                            const std::vector<std::string>& params = {}) = 0;

    bool is_connected() const { return connected_; }

protected:
    bool connected_ = false;
};

struct DbConfig {
    std::string host     = "localhost";
    std::string port     = "3306";
    std::string user;
    std::string password;
    std::string database;
};

#ifdef HAVE_MYSQL

#include "gcal.hpp"
#include <mysql/mysql.h>

class MySQLDatabase : public Database {
public:
    explicit MySQLDatabase(DbConfig cfg) : cfg_(std::move(cfg)) {}
    ~MySQLDatabase() override { if (connected_) disconnect(); }

    void connect() override;
    void disconnect() override;
    Result  query(const std::string& sql,
                  const std::vector<std::string>& params = {}) override;
    void    execute(const std::string& sql,
                    const std::vector<std::string>& params = {}) override;

    int64_t last_insert_id();

private:
    DbConfig cfg_;
    MYSQL*   conn_ = nullptr;

    void check_error(const std::string& context);
};

class ShiftTracker {
public:
    explicit ShiftTracker(MySQLDatabase& db) : db_(db) {}

    void ensure_schema();
    int64_t begin_sync_run(int total_shifts);
    std::vector<int64_t> apply_changes(const std::vector<ShiftChange>& changes);
    void finish_sync_run(int64_t run_id, bool success,
                         int created, int updated, int deleted,
                         const std::vector<int64_t>& change_ids,
                         const std::string& error_msg = "");

private:
    MySQLDatabase& db_;
};

class PeriodArchive {
public:
    explicit PeriodArchive(MySQLDatabase& db) : db_(db) {}

    void ensure_schema();
    bool is_locked(int pay_month, int pay_year);
    void upsert_period(const PeriodData& data, bool lock);

private:
    MySQLDatabase& db_;
};

#endif  // HAVE_MYSQL

DbConfig db_config_from_env();
