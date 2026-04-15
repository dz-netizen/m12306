#include <string>

#include "m12306_common.h"

int main() {
	cgicc::Cgicc form;
	std::string username = m12306::get_form_value(form, "username");
	std::string view_user = m12306::get_form_value(form, "view_user");

	m12306::print_page_begin("Admin Panel");
	std::cout << "<h2>Admin Panel</h2>";
	std::cout << "<p><a href=\"/m12306index.html\">LogOut</a></p>";

	if (!m12306::is_admin(username)) {
		std::cout << "<p class=\"err\">Forbidden: admin only.</p>";
		m12306::print_page_end();
		return 0;
	}

	PGconn *conn = m12306::connect_db();
	if (PQstatus(conn) != CONNECTION_OK) {
		std::cout << "<p class=\"err\">DB connection failed.</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	PGresult *r1 = PQexec(conn, "SELECT COUNT(*)::text FROM orders WHERE status='正常'");
	PGresult *r2 = PQexec(conn, "SELECT COALESCE(SUM(total_price),0)::text FROM orders WHERE status='正常'");
	std::string total_orders = (PQresultStatus(r1) == PGRES_TUPLES_OK) ? PQgetvalue(r1, 0, 0) : "0";
	std::string total_price = (PQresultStatus(r2) == PGRES_TUPLES_OK) ? PQgetvalue(r2, 0, 0) : "0";
	PQclear(r1);
	PQclear(r2);

	std::cout << "<p><b>Total valid orders:</b> " << m12306::html_escape(total_orders)
			  << " , <b>Total valid revenue:</b> " << m12306::html_escape(total_price) << "</p>";

	PGresult *top = PQexec(conn,
		"SELECT oi.train_id, COUNT(*)::text cnt "
		"FROM order_item oi JOIN orders o ON o.order_id=oi.order_id "
		"WHERE o.status='正常' GROUP BY oi.train_id ORDER BY cnt::int DESC LIMIT 10");
	std::cout << "<h3>Top 10 Hot Trains</h3><table><tr><th>Train</th><th>Count</th></tr>";
	if (PQresultStatus(top) == PGRES_TUPLES_OK) {
		for (int i = 0; i < PQntuples(top); ++i) {
			std::cout << "<tr><td>" << m12306::html_escape(PQgetvalue(top, i, 0))
					  << "</td><td>" << m12306::html_escape(PQgetvalue(top, i, 1)) << "</td></tr>";
		}
	}
	std::cout << "</table>";
	PQclear(top);

	PGresult *users = PQexec(conn, "SELECT username, name, phone FROM user_info ORDER BY user_id");
	std::cout << "<h3>Registered Users</h3><table><tr><th>Username</th><th>Name</th><th>Phone</th><th>Orders</th></tr>";
	if (PQresultStatus(users) == PGRES_TUPLES_OK) {
		for (int i = 0; i < PQntuples(users); ++i) {
			std::string u = PQgetvalue(users, i, 0);
			std::cout << "<tr><td>" << m12306::html_escape(u)
					  << "</td><td>" << m12306::html_escape(PQgetvalue(users, i, 1))
					  << "</td><td>" << m12306::html_escape(PQgetvalue(users, i, 2))
					  << "</td><td><a href=\"/cgi-bin/admin.cgi?username=admin&view_user="
					  << m12306::html_escape(u) << "\">View</a></td></tr>";
		}
	}
	std::cout << "</table>";
	PQclear(users);

	if (!view_user.empty()) {
		std::cout << "<h3>Orders of " << m12306::html_escape(view_user) << "</h3>";
		const char *sql =
			"SELECT o.order_id::text, o.status, o.total_price::text, o.create_time::date::text "
			"FROM orders o JOIN user_info u ON u.user_id=o.user_id "
			"WHERE u.username=$1 ORDER BY o.order_id DESC";
		const char *p[1] = {view_user.c_str()};
		PGresult *vo = PQexecParams(conn, sql, 1, NULL, p, NULL, NULL, 0);
		std::cout << "<table><tr><th>Order ID</th><th>Status</th><th>Total</th><th>Date</th></tr>";
		if (PQresultStatus(vo) == PGRES_TUPLES_OK) {
			for (int i = 0; i < PQntuples(vo); ++i) {
				std::cout << "<tr><td>" << m12306::html_escape(PQgetvalue(vo, i, 0))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(vo, i, 1))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(vo, i, 2))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(vo, i, 3))
						  << "</td></tr>";
			}
		}
		std::cout << "</table>";
		PQclear(vo);
	}

	PQfinish(conn);
	m12306::print_page_end();
	return 0;
}
