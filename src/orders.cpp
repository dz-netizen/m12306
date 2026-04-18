#include <string>

#include "m12306_common.h"

int main() {
	cgicc::Cgicc form;
	std::string username = m12306::get_form_value(form, "username");
	std::string date_from = m12306::get_form_value(form, "date_from");
	std::string date_to = m12306::get_form_value(form, "date_to");
	std::string action = m12306::get_form_value(form, "action");
	std::string order_id = m12306::get_form_value(form, "order_id");
	if (date_from.empty()) date_from = "2026-01-01";
	if (date_to.empty()) date_to = "2026-12-31";

	m12306::print_page_begin("Orders");
	std::cout << "<h2>Order Management</h2>";
	std::cout << "<p><a href=\"/cgi-bin/home.cgi?username=" << m12306::html_escape(username)
			  << "\">Back Home</a></p>";

	if (username.empty()) {
		std::cout << "<p class=\"err\">username required.</p>";
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
	int uid = m12306::get_user_id(conn, username);
	if (uid < 0) {
		std::cout << "<p class=\"err\">user not found.</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 0;
	}

	std::string uid_s = std::to_string(uid);

	if (action == "cancel" && !order_id.empty()) {
		m12306::exec_ok(conn, "BEGIN");
		const char *lock_sql = "SELECT status FROM orders WHERE order_id=$1::int AND user_id=$2::int FOR UPDATE";
		const char *lp[2] = {order_id.c_str(), uid_s.c_str()};
		PGresult *lk = PQexecParams(conn, lock_sql, 2, NULL, lp, NULL, NULL, 0);
		bool can_cancel = (PQresultStatus(lk) == PGRES_TUPLES_OK && PQntuples(lk) > 0
						&& std::string(PQgetvalue(lk, 0, 0)) == "正常");
		PQclear(lk);
		if (can_cancel) {
			const char *item_sql = "SELECT train_id, seat_type, from_station, to_station, travel_date::text FROM order_item WHERE order_id=$1::int";
			const char *ip[1] = {order_id.c_str()};
			PGresult *it = PQexecParams(conn, item_sql, 1, NULL, ip, NULL, NULL, 0);
			if (PQresultStatus(it) == PGRES_TUPLES_OK) {
				for (int i = 0; i < PQntuples(it); ++i) {
					const char *up_sql =
						"UPDATE seat_inventory SET remaining=remaining+1 "
						"WHERE train_id=$1 AND seat_type=$2 AND from_station=$3::int AND to_station=$4::int AND travel_date=$5::date";
					const char *up[5] = {
						PQgetvalue(it, i, 0), PQgetvalue(it, i, 1), PQgetvalue(it, i, 2),
						PQgetvalue(it, i, 3), PQgetvalue(it, i, 4)
					};
					PGresult *u = PQexecParams(conn, up_sql, 5, NULL, up, NULL, NULL, 0);
					PQclear(u);
				}
			}
			PQclear(it);
			PGresult *oc = PQexecParams(conn,
				"UPDATE orders SET status='取消' WHERE order_id=$1::int AND user_id=$2::int",
				2, NULL, lp, NULL, NULL, 0);
			PQclear(oc);
			m12306::exec_ok(conn, "COMMIT");
			std::cout << "<p class=\"ok\">Order canceled: " << m12306::html_escape(order_id) << "</p>";
		} else {
			m12306::exec_ok(conn, "ROLLBACK");
			std::cout << "<p class=\"err\">Order not cancelable.</p>";
		}
	}


	std::cout << "<form method=\"get\" action=\"/cgi-bin/orders.cgi\">"
			  << "<input type=\"hidden\" name=\"username\" value=\"" << m12306::html_escape(username) << "\">"
			  << "From: <input type=\"date\" name=\"date_from\" value=\"" << m12306::html_escape(date_from) << "\">"
			  << "To: <input type=\"date\" name=\"date_to\" value=\"" << m12306::html_escape(date_to) << "\">"
			  << "<button type=\"submit\">Filter</button></form>";

	const char *list_sql =
		"SELECT o.order_id::text, o.create_time::date::text, "
		"       COALESCE(sf.station_name,'-') AS from_station, "
		"       COALESCE(st.station_name,'-') AS to_station, "
		"       o.total_price::text, o.status "
		"FROM orders o "
		"LEFT JOIN LATERAL ("
		"  SELECT oi.from_station FROM order_item oi WHERE oi.order_id=o.order_id ORDER BY oi.id ASC LIMIT 1"
		") first_leg ON TRUE "
		"LEFT JOIN LATERAL ("
		"  SELECT oi.to_station FROM order_item oi WHERE oi.order_id=o.order_id ORDER BY oi.id DESC LIMIT 1"
		") last_leg ON TRUE "
		"LEFT JOIN station sf ON sf.station_id=first_leg.from_station "
		"LEFT JOIN station st ON st.station_id=last_leg.to_station "
		"WHERE o.user_id=$1::int AND o.create_time::date BETWEEN $2::date AND $3::date "
		"ORDER BY o.order_id DESC";
	const char *lp2[3] = {uid_s.c_str(), date_from.c_str(), date_to.c_str()};
	PGresult *ls = PQexecParams(conn, list_sql, 3, NULL, lp2, NULL, NULL, 0);
	if (PQresultStatus(ls) != PGRES_TUPLES_OK) {
		std::cout << "<p class=\"err\">Order query failed.</p>";
		PQclear(ls);
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	std::cout << "<table><tr><th>Order ID</th><th>Date</th><th>From</th><th>To</th><th>Total</th><th>Status</th><th>Action</th></tr>";
	for (int i = 0; i < PQntuples(ls); ++i) {
		std::string oid = PQgetvalue(ls, i, 0);
		std::string st = PQgetvalue(ls, i, 5);
		std::cout << "<tr><td>" << m12306::html_escape(oid) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(ls, i, 1)) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(ls, i, 2)) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(ls, i, 3)) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(ls, i, 4)) << "</td>"
				  << "<td>" << m12306::html_escape(st) << "</td><td>";
		std::cout << "<a href=\"/cgi-bin/orders.cgi?username=" << m12306::html_escape(username)
				  << "&action=detail&order_id=" << m12306::html_escape(oid)
				  << "&date_from=" << m12306::html_escape(date_from)
				  << "&date_to=" << m12306::html_escape(date_to)
				  << "\">Detail</a>";
		if (st == "正常") {
			std::cout << " | <a href=\"/cgi-bin/orders.cgi?username=" << m12306::html_escape(username)
					  << "&action=cancel&order_id=" << m12306::html_escape(oid)
					  << "&date_from=" << m12306::html_escape(date_from)
					  << "&date_to=" << m12306::html_escape(date_to)
					  << "\">Cancel</a>";
		}
		std::cout << "</td></tr>";
	}
	if (PQntuples(ls) == 0) std::cout << "<tr><td colspan=\"7\">No orders.</td></tr>";
	std::cout << "</table>";

	if (action == "detail" && !order_id.empty()) {
		const char *detail_sql =
			"SELECT oi.id::text, oi.train_id, sf.station_name, st.station_name, "
			"       oi.seat_type, oi.price::text, oi.travel_date::text, "
			"       COALESCE(ts1.departure_time::text,'-') AS depart_time, "
			"       COALESCE(ts2.arrival_time::text,'-') AS arrive_time "
			"FROM order_item oi "
			"JOIN orders o ON o.order_id=oi.order_id "
			"JOIN station sf ON sf.station_id=oi.from_station "
			"JOIN station st ON st.station_id=oi.to_station "
			"LEFT JOIN train_station ts1 ON ts1.train_id=oi.train_id AND ts1.station_id=oi.from_station "
			"LEFT JOIN train_station ts2 ON ts2.train_id=oi.train_id AND ts2.station_id=oi.to_station "
			"WHERE oi.order_id=$1::int AND o.user_id=$2::int "
			"ORDER BY oi.id";
		const char *dp[2] = {order_id.c_str(), uid_s.c_str()};
		PGresult *dr = PQexecParams(conn, detail_sql, 2, NULL, dp, NULL, NULL, 0);
		if (PQresultStatus(dr) == PGRES_TUPLES_OK && PQntuples(dr) > 0) {
			std::cout << "<h3>Order Detail: " << m12306::html_escape(order_id) << "</h3>";
			std::cout << "<table><tr><th>Leg</th><th>Train</th><th>From</th><th>To</th><th>Date</th><th>Depart</th><th>Arrive</th><th>Seat</th><th>Price</th></tr>";
			for (int i = 0; i < PQntuples(dr); ++i) {
				std::cout << "<tr><td>" << m12306::html_escape(PQgetvalue(dr, i, 0))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(dr, i, 1))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(dr, i, 2))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(dr, i, 3))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(dr, i, 6))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(dr, i, 7))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(dr, i, 8))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(dr, i, 4))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(dr, i, 5))
						  << "</td></tr>";
			}
			std::cout << "</table>";
		} else {
			std::cout << "<p class=\"err\">Order detail not found.</p>";
		}
		PQclear(dr);
	}

	PQclear(ls);
	PQfinish(conn);
	m12306::print_page_end();
	return 0;
}
