#include "dptx/StreamHistoryStore.h"
#include "dptx/Utils.h"

#include <algorithm>
#include <cctype>
#include <mysql/mysql.h>
#include <sstream>
#include <utility>

namespace dptx {

namespace {

std::pair<std::string, unsigned int> split_endpoint(const std::string& endpoint) {
    auto pos = endpoint.rfind(':');
    if (pos == std::string::npos) {
        return {endpoint, 3306};
    }
    return {endpoint.substr(0, pos), static_cast<unsigned int>(std::stoul(endpoint.substr(pos + 1)))};
}

std::string mysql_escape(MYSQL* mysql, const std::string& value) {
    std::string out;
    out.resize(value.size() * 2 + 1);
    unsigned long len = mysql_real_escape_string(mysql, out.data(), value.data(), static_cast<unsigned long>(value.size()));
    out.resize(len);
    return out;
}

} // namespace

StreamHistoryStore::StreamHistoryStore(const AppConfig& config, std::string direction)
    : config_(config), direction_(std::move(direction)) {
    std::transform(direction_.begin(), direction_.end(), direction_.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (direction_ != "rx") {
        direction_ = "tx";
    }
}

bool StreamHistoryStore::enabled() const {
    return config_.db_enabled;
}

std::string StreamHistoryStore::last_error() const {
    return last_error_;
}

void* StreamHistoryStore::connect() {
    MYSQL* mysql = mysql_init(nullptr);
    if (!mysql) {
        last_error_ = "mysql_init failed";
        return nullptr;
    }

    auto [host, port] = split_endpoint(config_.db_endpoint);
    if (!mysql_real_connect(
            mysql,
            host.c_str(),
            config_.db_user.c_str(),
            config_.db_password.c_str(),
            config_.db_name.c_str(),
            port,
            nullptr,
            0)) {
        last_error_ = mysql_error(mysql);
        mysql_close(mysql);
        return nullptr;
    }
    mysql_set_character_set(mysql, "utf8mb4");
    return mysql;
}

std::string StreamHistoryStore::table_name() const {
    std::string table;
    if (direction_ == "rx") {
        table = config_.db_rx_stream_table.empty() ? "rx_stream_history" : config_.db_rx_stream_table;
    } else {
        table = config_.db_tx_stream_table.empty()
            ? (config_.db_stream_table.empty() ? "tx_stream_history" : config_.db_stream_table)
            : config_.db_tx_stream_table;
    }
    table.erase(std::remove_if(table.begin(), table.end(), [](unsigned char c) {
        return !(std::isalnum(c) || c == '_');
    }), table.end());
    return table.empty() ? (direction_ == "rx" ? "rx_stream_history" : "tx_stream_history") : table;
}

bool StreamHistoryStore::ensure_table(void* handle) {
    auto* mysql = static_cast<MYSQL*>(handle);
    std::ostringstream sql;
    sql << "CREATE TABLE IF NOT EXISTS `" << table_name() << "` ("
        << "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        << "name VARCHAR(255) NOT NULL,"
        << "mode VARCHAR(32) NOT NULL,"
        << "rate_mbps INT NOT NULL DEFAULT 0,"
        << "config_json LONGTEXT NOT NULL,"
        << "saved_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP"
        << ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    if (mysql_query(mysql, sql.str().c_str()) != 0) {
        last_error_ = mysql_error(mysql);
        return false;
    }
    return true;
}

bool StreamHistoryStore::save_streams(const std::vector<StreamInfo>& streams) {
    if (!enabled()) {
        return true;
    }

    MYSQL* mysql = static_cast<MYSQL*>(connect());
    if (!mysql) {
        return false;
    }
    if (!ensure_table(mysql)) {
        mysql_close(mysql);
        return false;
    }

    for (const auto& stream : streams) {
        const std::string direction = stream.direction.empty() ? "tx" : stream.direction;
        if (direction != direction_) {
            continue;
        }
        auto name = mysql_escape(mysql, stream.name);
        auto mode = mysql_escape(mysql, stream.mode);
        auto config_json = mysql_escape(mysql, stream.config_json.empty() ? "{}" : stream.config_json);
        std::ostringstream sql;
        sql << "INSERT INTO `" << table_name() << "` "
            << "(name, mode, rate_mbps, config_json) VALUES ('"
            << name << "','" << mode << "'," << stream.target_mbps << ",'"
            << config_json << "')";
        if (mysql_query(mysql, sql.str().c_str()) != 0) {
            last_error_ = mysql_error(mysql);
            mysql_close(mysql);
            return false;
        }
    }

    mysql_close(mysql);
    return true;
}

std::vector<HistoryStreamInfo> StreamHistoryStore::load_streams(int limit) {
    std::vector<HistoryStreamInfo> streams;
    if (!enabled()) {
        return streams;
    }

    MYSQL* mysql = static_cast<MYSQL*>(connect());
    if (!mysql) {
        return streams;
    }
    if (!ensure_table(mysql)) {
        mysql_close(mysql);
        return streams;
    }

    std::ostringstream sql;
    sql << "SELECT id,name,mode,rate_mbps,config_json,DATE_FORMAT(saved_at,'%Y-%m-%d %H:%i:%s') "
        << "FROM `" << table_name() << "` ORDER BY saved_at DESC, id DESC LIMIT " << limit;
    if (mysql_query(mysql, sql.str().c_str()) != 0) {
        last_error_ = mysql_error(mysql);
        mysql_close(mysql);
        return streams;
    }

    MYSQL_RES* result = mysql_store_result(mysql);
    if (!result) {
        last_error_ = mysql_error(mysql);
        mysql_close(mysql);
        return streams;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        HistoryStreamInfo item;
        item.id = row[0] ? std::stol(row[0]) : 0;
        item.name = row[1] ? row[1] : "";
        item.direction = direction_;
        item.mode = row[2] ? row[2] : "";
        item.rate_mbps = row[3] ? std::stoi(row[3]) : 0;
        item.config_json = row[4] ? row[4] : "{}";
        item.saved_at = row[5] ? row[5] : "";
        streams.push_back(std::move(item));
    }

    mysql_free_result(result);
    mysql_close(mysql);
    return streams;
}

std::optional<HistoryStreamInfo> StreamHistoryStore::load_stream(long id) {
    if (!enabled()) {
        return std::nullopt;
    }

    MYSQL* mysql = static_cast<MYSQL*>(connect());
    if (!mysql) {
        return std::nullopt;
    }
    if (!ensure_table(mysql)) {
        mysql_close(mysql);
        return std::nullopt;
    }

    std::ostringstream sql;
    sql << "SELECT id,name,mode,rate_mbps,config_json,DATE_FORMAT(saved_at,'%Y-%m-%d %H:%i:%s') "
        << "FROM `" << table_name() << "` WHERE id=" << id << " LIMIT 1";
    if (mysql_query(mysql, sql.str().c_str()) != 0) {
        last_error_ = mysql_error(mysql);
        mysql_close(mysql);
        return std::nullopt;
    }

    MYSQL_RES* result = mysql_store_result(mysql);
    if (!result) {
        last_error_ = mysql_error(mysql);
        mysql_close(mysql);
        return std::nullopt;
    }

    std::optional<HistoryStreamInfo> item;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (row) {
        HistoryStreamInfo value;
        value.id = row[0] ? std::stol(row[0]) : 0;
        value.name = row[1] ? row[1] : "";
        value.direction = direction_;
        value.mode = row[2] ? row[2] : "";
        value.rate_mbps = row[3] ? std::stoi(row[3]) : 0;
        value.config_json = row[4] ? row[4] : "{}";
        value.saved_at = row[5] ? row[5] : "";
        item = value;
    }

    mysql_free_result(result);
    mysql_close(mysql);
    return item;
}

bool StreamHistoryStore::delete_stream(long id) {
    if (!enabled()) {
        return false;
    }

    MYSQL* mysql = static_cast<MYSQL*>(connect());
    if (!mysql) {
        return false;
    }
    if (!ensure_table(mysql)) {
        mysql_close(mysql);
        return false;
    }

    std::ostringstream sql;
    sql << "DELETE FROM `" << table_name() << "` WHERE id=" << id << " LIMIT 1";
    if (mysql_query(mysql, sql.str().c_str()) != 0) {
        last_error_ = mysql_error(mysql);
        mysql_close(mysql);
        return false;
    }

    const auto affected = mysql_affected_rows(mysql);
    mysql_close(mysql);
    if (affected == 0) {
        last_error_ = "history stream not found";
        return false;
    }
    return true;
}

} // namespace dptx
