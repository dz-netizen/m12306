#include <string>

#include "m12306_common.h"

int main() {
	cgicc::Cgicc form;
	std::string username = m12306::get_form_value(form, "username");

	m12306::print_page_begin("M12306 Home");
	std::cout << "<h2>M12306 Home</h2>";

	if (username.empty()) {
		std::cout << "<p class=\"err\">Please login first.</p>";
		std::cout << "<p><a href=\"/m12306index.html\">Login</a></p>";
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

	int uid = m12306::get_user_id(conn, username);
	PQfinish(conn);
	if (uid < 0) {
		std::cout << "<p class=\"err\">User does not exist. Please login again.</p>";
		std::cout << "<p><a href=\"/m12306index.html\">Login</a></p>";
		m12306::print_page_end();
		return 0;
	}

	std::cout << "<p class=\"ok\">Welcome, " << m12306::html_escape(username) << "</p>";
	std::cout << "<ul>";
	std::cout << "<li><a href=\"/query_train.html?username=" << m12306::html_escape(username)
			  << "\">Query Train</a></li>";
	std::cout << "<li><a href=\"/query_route.html?username=" << m12306::html_escape(username)
			  << "\">Query Route</a></li>";
	std::cout << "<li><a href=\"/cgi-bin/orders.cgi?username=" << m12306::html_escape(username)
			  << "\">Manage Orders</a></li>";
	if (m12306::is_admin(username)) {
		std::cout << "<li><a href=\"/cgi-bin/admin.cgi?username=admin\">Admin Panel</a></li>";
	}
	std::cout << "</ul>";
	std::cout << "<p><a href=\"/m12306index.html\">Logout</a></p>";

	m12306::print_page_end();
	return 0;
}
