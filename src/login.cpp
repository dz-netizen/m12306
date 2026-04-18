#include <string>

#include "m12306_common.h"

int main() {
	cgicc::Cgicc form;
	std::string username = m12306::get_form_value(form, "username");
	std::string password = m12306::get_form_value(form, "password");

	if (username.empty() || password.empty()) {
		m12306::print_page_begin("M12306 Login");
		std::cout << "<h2>M12306 Login</h2>";
		std::cout << "<p class=\"err\">Username and password cannot be empty.</p>";
		std::cout << "<p><a href=\"/m12306index.html\">Back</a></p>";
		m12306::print_page_end();
		return 0;
	}

	PGconn *conn = m12306::connect_db();
	if (PQstatus(conn) != CONNECTION_OK) {
		m12306::print_page_begin("M12306 Login");
		std::cout << "<h2>M12306 Login</h2>";
		std::cout << "<p class=\"err\">Database connection failed: "
				  << m12306::html_escape(PQerrorMessage(conn)) << "</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	// Ensure fixed admin user exists.
	m12306::exec_ok(conn,
		"INSERT INTO user_info(username, phone, name, password) "
		"VALUES('admin','00000000000','Admin','admin') "
		"ON CONFLICT (username) DO NOTHING");

	const char *sql = "SELECT user_id, name, password FROM user_info WHERE username=$1";
	const char *params[1] = {username.c_str()};
	PGresult *res = PQexecParams(conn, sql, 1, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		m12306::print_page_begin("M12306 Login");
		std::cout << "<h2>M12306 Login</h2>";
		std::cout << "<p class=\"err\">Query failed: "
				  << m12306::html_escape(PQresultErrorMessage(res)) << "</p>";
		PQclear(res);
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	if (PQntuples(res) == 0) {
		m12306::print_page_begin("M12306 Login");
		std::cout << "<h2>M12306 Login</h2>";
		std::cout << "<p class=\"err\">User not found. Please register first.</p>";
		std::cout << "<p><a href=\"/register.html\">Go Register</a></p>";
	} else {
		std::string db_password = PQgetvalue(res, 0, 2);
		if (db_password != password) {
			m12306::print_page_begin("M12306 Login");
			std::cout << "<h2>M12306 Login</h2>";
			std::cout << "<p class=\"err\">Password incorrect.</p>";
			std::cout << "<p>Please go to register page and re-register to change password.</p>";
			std::cout << "<p><a href=\"/register.html\">Go Register</a></p>";
			PQclear(res);
			PQfinish(conn);
			m12306::print_page_end();
			return 0;
		}

		if (m12306::is_admin(username)) {
			PQclear(res);
			PQfinish(conn);
			std::cout << "Status: 302 Found\r\n";
			std::cout << "Location: /cgi-bin/admin.cgi?username=admin\r\n\r\n";
			return 0;
		}

		m12306::print_page_begin("M12306 Login");
		std::cout << "<h2>M12306 Login</h2>";
		std::string display_name = PQgetvalue(res, 0, 1);
		std::cout << "<p class=\"ok\">Welcome, " << m12306::html_escape(display_name)
				  << " (" << m12306::html_escape(username) << ")</p>";
		std::cout << "<p><a href=\"/cgi-bin/home.cgi?username=" << m12306::html_escape(username)
				  << "\">Enter Home</a></p>";
	}

	PQclear(res);
	PQfinish(conn);
	m12306::print_page_end();
	return 0;
}
