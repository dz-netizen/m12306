#ifndef M12306_COMMON_H
#define M12306_COMMON_H

#include <cstdlib>
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
    std::cout << "<!doctype html><html><head><meta charset=\"utf-8\">"
                 "<title>" << html_escape(title) << "</title>"
                 "<style>"
                 "body{font-family:Verdana,sans-serif;background:#f4f7fb;margin:24px;color:#111;}"
                 "table{border-collapse:collapse;background:#fff;width:100%;max-width:1100px;}"
                 "th,td{border:1px solid #d7d7d7;padding:8px;text-align:left;}"
                 "th{background:#e9f1ff;}"
                 "input,select{padding:6px;margin:4px 8px 8px 0;}"
                 "button{padding:7px 12px;}"
                 ".card{background:#fff;border:1px solid #ddd;padding:16px;max-width:1100px;}"
                 ".err{color:#b00020;font-weight:700;}"
                 ".ok{color:#006400;font-weight:700;}"
                 "a{color:#0b57d0;text-decoration:none;}"
                 "a:hover{text-decoration:underline;}"
                 "</style></head><body>";
    std::cout << "<div class=\"card\">";
}

inline void print_page_end() {
    std::cout << "</div></body></html>";
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

inline std::string qs(const std::string &username) {
    if (username.empty()) return "";
    return "?username=" + username;
}

inline bool is_admin(const std::string &username) {
    return username == "admin";
}

} // namespace m12306

#endif
