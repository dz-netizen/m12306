#ifndef M12306_COMMON_H
#define M12306_COMMON_H

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>

#include <cgicc/Cgicc.h>
#include <postgresql/libpq-fe.h>

namespace m12306 {

inline std::string html_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        switch (s[i]) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += s[i]; break;
        }
    }
    return out;
}

inline std::string get_form_value(cgicc::Cgicc &form, const std::string &key) {
    cgicc::form_iterator it = form.getElement(key);
    if (it != form.getElements().end() && !it->isEmpty()) {
        return **it;
    }
    return "";
}

inline const char *default_conninfo() {
    const char *conninfo_env = std::getenv("LAB2_DB_CONN");
    return (conninfo_env == NULL) ? "dbname=m12306 user=dbms password=dbms" : conninfo_env;
}

inline PGconn *connect_db() {
    PGconn *conn = PQconnectdb(default_conninfo());
    return conn;
}

inline void print_page_begin(const std::string &title) {
    std::cout << "Content-type:text/html\n\n";
    std::cout << "<!DOCTYPE html><html lang=\"zh-CN\"><head>"
                 "<meta charset=\"utf-8\">"
                 "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                 "<title>" << html_escape(title) << " - M12306</title>"
                 "<link rel=\"stylesheet\" href=\"/style.css\">"
                 "<style>"
                 ".container{width:100%;max-width:1000px;margin:20px auto;background:#fff;border-radius:12px;box-shadow:0 10px 40px rgba(0,0,0,0.15);overflow:hidden;}"
                 ".header{background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);color:#fff;padding:30px;text-align:center;}"
                 ".header h1{font-size:28px;font-weight:600;margin-bottom:8px;}"
                 ".content{padding:30px;}"
                 ".message{padding:12px 16px;border-radius:8px;margin-bottom:20px;font-size:14px;font-weight:500;}"
                 ".message.err{background:#ffebee;color:#c62828;border-left:4px solid #c62828;}"
                 ".message.ok{background:#e8f5e9;color:#2e7d32;border-left:4px solid #2e7d32;}"
                 ".nav-menu{list-style:none;margin:20px 0;}"
                 ".nav-menu li{margin-bottom:12px;}"
                 ".nav-menu a{display:block;padding:14px 16px;background:#f5f5f5;color:#667eea;text-decoration:none;border-radius:8px;font-weight:500;transition:all 0.3s ease;border-left:4px solid transparent;}"
                 ".nav-menu a:hover{background:#efefef;border-left-color:#667eea;padding-left:20px;}"
                 "table{width:100%;border-collapse:collapse;margin:20px 0;}"
                 "thead{background:#f5f5f5;}"
                 "th{padding:12px 14px;text-align:left;font-weight:600;font-size:13px;color:#333;border-bottom:2px solid #e0e0e0;}"
                 "td{padding:12px 14px;border-bottom:1px solid #f0f0f0;font-size:14px;}"
                 "tbody tr:hover{background:#f9f9f9;}"
                 ".logout-btn{margin-top:30px;padding-top:20px;border-top:1px solid #f0f0f0;text-align:center;}"
                 ".logout-btn a{display:inline-block;padding:10px 20px;background:#f5f5f5;color:#666;text-decoration:none;border-radius:8px;font-weight:500;transition:all 0.3s ease;}"
                 ".logout-btn a:hover{background:#e0e0e0;color:#333;}"
                 "a{color:#667eea;text-decoration:none;}"
                 "a:hover{color:#764ba2;}"
                 "</style></head><body><div class=\"container\"><div class=\"header\"><h1>M12306</h1><p>" << html_escape(title) << "</p></div><div class=\"content\">";
}

inline void print_page_end() {
    std::cout << "</div></div></body></html>";
}

inline bool exec_ok(PGconn *conn, const char *sql) {
    PGresult *res = PQexec(conn, sql);
    ExecStatusType st = PQresultStatus(res);
    bool ok = (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK);
    PQclear(res);
    return ok;
}

inline int get_user_id(PGconn *conn, const std::string &username) {
    const char *sql = "SELECT user_id FROM user_info WHERE username=$1";
    const char *params[1] = {username.c_str()};
    PGresult *res = PQexecParams(conn, sql, 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        return -1;
    }
    int uid = std::atoi(PQgetvalue(res, 0, 0));
    PQclear(res);
    return uid;
}

inline bool ensure_inventory(PGconn *conn, const std::string &train_id, const std::string &travel_date) {
    const char *sql =
        "INSERT INTO seat_inventory(train_id, travel_date, seat_type, from_station, to_station, remaining) "
        "SELECT tp.train_id, $2::date, tp.seat_type, tp.from_station, tp.to_station, 5 "
        "FROM ticket_price tp "
        "WHERE tp.train_id=$1 "
        "ON CONFLICT (train_id, travel_date, seat_type, from_station, to_station) DO NOTHING";
    const char *params[2] = {train_id.c_str(), travel_date.c_str()};
    PGresult *res = PQexecParams(conn, sql, 2, NULL, params, NULL, NULL, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

inline std::string tomorrow_date() {
    std::time_t now = std::time(NULL);
    std::tm tmv = *std::localtime(&now);
    tmv.tm_mday += 1;
    std::mktime(&tmv);

    char buf[11];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
    return std::string(buf);
}

inline bool parse_ymd(const std::string &s, int &y, int &m, int &d) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }

    y = std::atoi(s.substr(0, 4).c_str());
    m = std::atoi(s.substr(5, 2).c_str());
    d = std::atoi(s.substr(8, 2).c_str());

    std::tm tmv = {};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = m - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = 12;
    if (std::mktime(&tmv) == -1) return false;
    return (tmv.tm_year == y - 1900 && tmv.tm_mon == m - 1 && tmv.tm_mday == d);
}

inline bool is_after_today(const std::string &date) {
    int y = 0, m = 0, d = 0;
    if (!parse_ymd(date, y, m, d)) return false;

    std::time_t now = std::time(NULL);
    std::tm today = *std::localtime(&now);
    today.tm_hour = 0;
    today.tm_min = 0;
    today.tm_sec = 0;

    std::tm target = {};
    target.tm_year = y - 1900;
    target.tm_mon = m - 1;
    target.tm_mday = d;
    target.tm_hour = 0;
    target.tm_min = 0;
    target.tm_sec = 0;

    std::time_t t_today = std::mktime(&today);
    std::time_t t_target = std::mktime(&target);
    if (t_today == -1 || t_target == -1) return false;
    return t_target > t_today;
}

inline std::string qs(const std::string &username) {
    if (username.empty()) return "";
    return "?username=" + username;
}

inline bool is_admin(const std::string &username) {
    return username == "admin";
}

} // namespace m12306

#endif
