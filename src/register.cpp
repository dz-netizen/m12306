#include <string>

#include "m12306_common.h"

static bool exists_value(PGconn *conn, const char *sql, const std::string &value) {
	const char *params[1] = {value.c_str()};
	PGresult *res = PQexecParams(conn, sql, 1, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		PQclear(res);
		return true;
	}
	bool exists = (std::atoi(PQgetvalue(res, 0, 0)) > 0);
	PQclear(res);
	return exists;
}

int main() {
	cgicc::Cgicc form;
	std::string username = m12306::get_form_value(form, "username");
	std::string phone = m12306::get_form_value(form, "phone");
	std::string name = m12306::get_form_value(form, "name");
	std::string password = m12306::get_form_value(form, "password");
	if (password.empty()) {
		m12306::print_page_begin("M12306 Register");
		std::cout << "<h2>M12306 Register</h2>";
		std::cout << "<p class=\"err\">password is required.</p>";
		std::cout << "<p><a href=\"/register.html\">Back</a></p>";
		m12306::print_page_end();
		return 0;
	}

	m12306::print_page_begin("M12306 Register");
	std::cout << "<h2>M12306 Register</h2>";

	if (username.empty() || phone.empty() || name.empty()) {
		std::cout << "<p class=\"err\">username/phone/name are required.</p>";
		std::cout << "<p><a href=\"/register.html\">Back</a></p>";
		m12306::print_page_end();
		return 0;
	}

	PGconn *conn = m12306::connect_db();
	if (PQstatus(conn) != CONNECTION_OK) {
		std::cout << "<p class=\"err\">Database connection failed: "
				  << m12306::html_escape(PQerrorMessage(conn)) << "</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	if (exists_value(conn, "SELECT COUNT(*) FROM user_info WHERE username=$1", username)) {
		const char *upd_sql = "UPDATE user_info SET password=$2 WHERE username=$1";
		const char *upd_params[2] = {username.c_str(), password.c_str()};
		PGresult *upd = PQexecParams(conn, upd_sql, 2, NULL, upd_params, NULL, NULL, 0);
		if (PQresultStatus(upd) == PGRES_COMMAND_OK) {
			std::cout << "<p class=\"ok\">Username exists. Password has been updated.</p>";
			std::cout << "<p><a href=\"/m12306index.html\">Go Login</a></p>";
		} else {
			std::cout << "<p class=\"err\">Password update failed: "
					  << m12306::html_escape(PQresultErrorMessage(upd)) << "</p>";
			std::cout << "<p><a href=\"/register.html\">Back</a></p>";
		}
		PQclear(upd);
		PQfinish(conn);
		m12306::print_page_end();
		return 0;
	}

	if (exists_value(conn, "SELECT COUNT(*) FROM user_info WHERE phone=$1", phone)) {
		std::cout << "<p class=\"err\">Phone already exists.</p>";
		std::cout << "<p><a href=\"/register.html\">Back</a></p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 0;
	}

	const char *sql =
		"INSERT INTO user_info(username, phone, name, password) "
		"VALUES($1,$2,$3,$4)";
	const char *params[4] = {username.c_str(), phone.c_str(), name.c_str(), password.c_str()};
	PGresult *res = PQexecParams(conn, sql, 4, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		std::cout << "<p class=\"err\">Register failed: "
				  << m12306::html_escape(PQresultErrorMessage(res)) << "</p>";
	} else {
		std::cout << "<p class=\"ok\">Register success. Please login now.</p>";
		std::cout << "<p><a href=\"/m12306index.html\">Go Login</a></p>";
	}

	PQclear(res);
	PQfinish(conn);
	m12306::print_page_end();
	return 0;
}
