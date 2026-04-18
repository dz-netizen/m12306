#include <cstdlib>
#include <string>

#include "m12306_common.h"

static bool fetch_leg(PGconn *conn,
	const std::string &train_id,
	const std::string &from_sid,
	const std::string &to_sid,
	const std::string &seat_type,
	double &fare,
	std::string &from_name,
	std::string &to_name) {
	const char *sql =
		"SELECT tp.price::text, sf.station_name, st.station_name "
		"FROM ticket_price tp "
		"JOIN station sf ON sf.station_id=tp.from_station "
		"JOIN station st ON st.station_id=tp.to_station "
		"WHERE tp.train_id=$1 AND tp.from_station=$2::int AND tp.to_station=$3::int AND tp.seat_type=$4";
	const char *params[4] = {train_id.c_str(), from_sid.c_str(), to_sid.c_str(), seat_type.c_str()};
	PGresult *res = PQexecParams(conn, sql, 4, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
		PQclear(res);
		return false;
	}
	fare = std::atof(PQgetvalue(res, 0, 0));
	from_name = PQgetvalue(res, 0, 1);
	to_name = PQgetvalue(res, 0, 2);
	PQclear(res);
	return true;
}

static bool lock_and_check_inventory(PGconn *conn,
	const std::string &train_id,
	const std::string &date,
	const std::string &seat_type,
	const std::string &from_sid,
	const std::string &to_sid) {
	const char *sql =
		"SELECT remaining FROM seat_inventory "
		"WHERE train_id=$1 AND travel_date=$2::date AND seat_type=$3 AND from_station=$4::int AND to_station=$5::int FOR UPDATE";
	const char *params[5] = {train_id.c_str(), date.c_str(), seat_type.c_str(), from_sid.c_str(), to_sid.c_str()};
	PGresult *res = PQexecParams(conn, sql, 5, NULL, params, NULL, NULL, 0);
	bool ok = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0 && std::atoi(PQgetvalue(res, 0, 0)) > 0);
	PQclear(res);
	return ok;
}

static bool dec_inventory(PGconn *conn,
	const std::string &train_id,
	const std::string &date,
	const std::string &seat_type,
	const std::string &from_sid,
	const std::string &to_sid) {
	const char *sql =
		"UPDATE seat_inventory SET remaining=remaining-1 "
		"WHERE train_id=$1 AND travel_date=$2::date AND seat_type=$3 AND from_station=$4::int AND to_station=$5::int";
	const char *params[5] = {train_id.c_str(), date.c_str(), seat_type.c_str(), from_sid.c_str(), to_sid.c_str()};
	PGresult *res = PQexecParams(conn, sql, 5, NULL, params, NULL, NULL, 0);
	bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
	PQclear(res);
	return ok;
}

int main() {
	cgicc::Cgicc form;
	std::string username = m12306::get_form_value(form, "username");
	std::string date = m12306::get_form_value(form, "date");
	std::string seat_type = m12306::get_form_value(form, "seat_type");
	std::string confirm = m12306::get_form_value(form, "confirm");
	std::string transfer = m12306::get_form_value(form, "transfer");

	std::string train_id = m12306::get_form_value(form, "train_id");
	std::string from_sid = m12306::get_form_value(form, "from_sid");
	std::string to_sid = m12306::get_form_value(form, "to_sid");

	std::string train1 = m12306::get_form_value(form, "train1");
	std::string from1 = m12306::get_form_value(form, "from1");
	std::string to1 = m12306::get_form_value(form, "to1");
	std::string train2 = m12306::get_form_value(form, "train2");
	std::string from2 = m12306::get_form_value(form, "from2");
	std::string to2 = m12306::get_form_value(form, "to2");

	if (seat_type.empty()) seat_type = "二等座";

	m12306::print_page_begin("Book Ticket");
	std::cout << "<h2>Book Ticket</h2>";
	std::cout << "<p><a href=\"/cgi-bin/home.cgi?username=" << m12306::html_escape(username)
			  << "\">Back Home</a></p>";

	bool is_transfer = (transfer == "1");
	if (username.empty() || date.empty() ||
		(!is_transfer && (train_id.empty() || from_sid.empty() || to_sid.empty())) ||
		(is_transfer && (train1.empty() || from1.empty() || to1.empty() || train2.empty() || from2.empty() || to2.empty()))) {
		std::cout << "<p class=\"err\">Missing booking parameters.</p>";
		m12306::print_page_end();
		return 0;
	}

	PGconn *conn = m12306::connect_db();
	if (PQstatus(conn) != CONNECTION_OK) {
		std::cout << "<p class=\"err\">DB connection failed: "
				  << m12306::html_escape(PQerrorMessage(conn)) << "</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	int user_id = m12306::get_user_id(conn, username);
	if (user_id < 0) {
		std::cout << "<p class=\"err\">Invalid user.</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 0;
	}

	if (!is_transfer) {
		m12306::ensure_inventory(conn, train_id, date);

		double fare = 0.0;
		std::string from_name, to_name;
		if (!fetch_leg(conn, train_id, from_sid, to_sid, seat_type, fare, from_name, to_name)) {
			std::cout << "<p class=\"err\">Cannot find fare for this segment/seat.</p>";
			PQfinish(conn);
			m12306::print_page_end();
			return 0;
		}

		double service_fee = 5.0;
		double total = fare + service_fee;

		if (confirm != "1") {
			std::cout << "<p>Train: <b>" << m12306::html_escape(train_id) << "</b></p>";
			std::cout << "<p>Route: " << m12306::html_escape(from_name) << " -> " << m12306::html_escape(to_name)
					  << " , Date: " << m12306::html_escape(date) << "</p>";
			std::cout << "<p>Seat: " << m12306::html_escape(seat_type)
					  << " , Fare: " << fare << " , Fee: " << service_fee
					  << " , Total: <b>" << total << "</b></p>";
			std::cout << "<p><a href=\"/cgi-bin/book.cgi?username=" << m12306::html_escape(username)
					  << "&train_id=" << m12306::html_escape(train_id)
					  << "&from_sid=" << m12306::html_escape(from_sid)
					  << "&to_sid=" << m12306::html_escape(to_sid)
					  << "&date=" << m12306::html_escape(date)
					  << "&seat_type=" << m12306::html_escape(seat_type)
					  << "&confirm=1\">Confirm Booking</a></p>";
			m12306::print_page_end();
			PQfinish(conn);
			return 0;
		}

		m12306::exec_ok(conn, "BEGIN");
		if (!lock_and_check_inventory(conn, train_id, date, seat_type, from_sid, to_sid)) {
			m12306::exec_ok(conn, "ROLLBACK");
			std::cout << "<p class=\"err\">No inventory left.</p>";
			PQfinish(conn);
			m12306::print_page_end();
			return 0;
		}

		std::string total_s = std::to_string(total);
		std::string uid_s = std::to_string(user_id);
		const char *ord_sql =
			"INSERT INTO orders(user_id,total_price,status) VALUES($1::int,$2::numeric,'正常') RETURNING order_id";
		const char *op[2] = {uid_s.c_str(), total_s.c_str()};
		PGresult *or1 = PQexecParams(conn, ord_sql, 2, NULL, op, NULL, NULL, 0);
		if (PQresultStatus(or1) != PGRES_TUPLES_OK || PQntuples(or1) == 0) {
			PQclear(or1);
			m12306::exec_ok(conn, "ROLLBACK");
			std::cout << "<p class=\"err\">Create order failed.</p>";
			PQfinish(conn);
			m12306::print_page_end();
			return 1;
		}
		std::string order_id = PQgetvalue(or1, 0, 0);
		PQclear(or1);

		std::string fare_s = std::to_string(fare);
		const char *item_sql =
			"INSERT INTO order_item(order_id,train_id,from_station,to_station,seat_type,price,travel_date) "
			"VALUES($1::int,$2,$3::int,$4::int,$5,$6::numeric,$7::date)";
		const char *itp[7] = {order_id.c_str(), train_id.c_str(), from_sid.c_str(), to_sid.c_str(), seat_type.c_str(), fare_s.c_str(), date.c_str()};
		PGresult *it = PQexecParams(conn, item_sql, 7, NULL, itp, NULL, NULL, 0);
		if (PQresultStatus(it) != PGRES_COMMAND_OK || !dec_inventory(conn, train_id, date, seat_type, from_sid, to_sid)) {
			PQclear(it);
			m12306::exec_ok(conn, "ROLLBACK");
			std::cout << "<p class=\"err\">Booking failed in transaction.</p>";
			PQfinish(conn);
			m12306::print_page_end();
			return 1;
		}
		PQclear(it);
		m12306::exec_ok(conn, "COMMIT");

		std::cout << "<p class=\"ok\">Booking success. Order ID: " << m12306::html_escape(order_id) << "</p>";
		std::cout << "<p><a href=\"/cgi-bin/orders.cgi?username=" << m12306::html_escape(username)
				  << "\">View Orders</a></p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 0;
	}

	// Transfer booking flow
	m12306::ensure_inventory(conn, train1, date);
	m12306::ensure_inventory(conn, train2, date);

	double fare1 = 0.0, fare2 = 0.0;
	std::string from_name1, to_name1, from_name2, to_name2;
	if (!fetch_leg(conn, train1, from1, to1, seat_type, fare1, from_name1, to_name1) ||
		!fetch_leg(conn, train2, from2, to2, seat_type, fare2, from_name2, to_name2)) {
		std::cout << "<p class=\"err\">Cannot find fare for transfer legs.</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 0;
	}

	double service_fee = 10.0;
	double total = fare1 + fare2 + service_fee;

	if (confirm != "1") {
		std::cout << "<p>Transfer Booking (2 legs):</p>";
		std::cout << "<p>Leg1: <b>" << m12306::html_escape(train1) << "</b> "
				  << m12306::html_escape(from_name1) << " -> " << m12306::html_escape(to_name1)
				  << " , Fare: " << fare1 << "</p>";
		std::cout << "<p>Leg2: <b>" << m12306::html_escape(train2) << "</b> "
				  << m12306::html_escape(from_name2) << " -> " << m12306::html_escape(to_name2)
				  << " , Fare: " << fare2 << "</p>";
		std::cout << "<p>Date: " << m12306::html_escape(date)
				  << " , Seat: " << m12306::html_escape(seat_type)
				  << " , Fee: " << service_fee
				  << " , Total: <b>" << total << "</b></p>";
		std::cout << "<p><a href=\"/cgi-bin/book.cgi?username=" << m12306::html_escape(username)
				  << "&transfer=1"
				  << "&train1=" << m12306::html_escape(train1)
				  << "&from1=" << m12306::html_escape(from1)
				  << "&to1=" << m12306::html_escape(to1)
				  << "&train2=" << m12306::html_escape(train2)
				  << "&from2=" << m12306::html_escape(from2)
				  << "&to2=" << m12306::html_escape(to2)
				  << "&date=" << m12306::html_escape(date)
				  << "&seat_type=" << m12306::html_escape(seat_type)
				  << "&confirm=1\">Confirm Transfer Booking</a></p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 0;
	}

	m12306::exec_ok(conn, "BEGIN");
	if (!lock_and_check_inventory(conn, train1, date, seat_type, from1, to1) ||
		!lock_and_check_inventory(conn, train2, date, seat_type, from2, to2)) {
		m12306::exec_ok(conn, "ROLLBACK");
		std::cout << "<p class=\"err\">No inventory left for one transfer leg.</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 0;
	}

	std::string total_s = std::to_string(total);
	std::string uid_s = std::to_string(user_id);
	const char *ord_sql =
		"INSERT INTO orders(user_id,total_price,status) VALUES($1::int,$2::numeric,'正常') RETURNING order_id";
	const char *op[2] = {uid_s.c_str(), total_s.c_str()};
	PGresult *or1 = PQexecParams(conn, ord_sql, 2, NULL, op, NULL, NULL, 0);
	if (PQresultStatus(or1) != PGRES_TUPLES_OK || PQntuples(or1) == 0) {
		PQclear(or1);
		m12306::exec_ok(conn, "ROLLBACK");
		std::cout << "<p class=\"err\">Create transfer order failed.</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}
	std::string order_id = PQgetvalue(or1, 0, 0);
	PQclear(or1);

	const char *item_sql =
		"INSERT INTO order_item(order_id,train_id,from_station,to_station,seat_type,price,travel_date) "
		"VALUES($1::int,$2,$3::int,$4::int,$5,$6::numeric,$7::date)";
	std::string fare1_s = std::to_string(fare1);
	std::string fare2_s = std::to_string(fare2);
	const char *it1[7] = {order_id.c_str(), train1.c_str(), from1.c_str(), to1.c_str(), seat_type.c_str(), fare1_s.c_str(), date.c_str()};
	const char *it2[7] = {order_id.c_str(), train2.c_str(), from2.c_str(), to2.c_str(), seat_type.c_str(), fare2_s.c_str(), date.c_str()};
	PGresult *r_it1 = PQexecParams(conn, item_sql, 7, NULL, it1, NULL, NULL, 0);
	PGresult *r_it2 = PQexecParams(conn, item_sql, 7, NULL, it2, NULL, NULL, 0);
	bool ok_items = (PQresultStatus(r_it1) == PGRES_COMMAND_OK && PQresultStatus(r_it2) == PGRES_COMMAND_OK);
	PQclear(r_it1);
	PQclear(r_it2);

	if (!ok_items ||
		!dec_inventory(conn, train1, date, seat_type, from1, to1) ||
		!dec_inventory(conn, train2, date, seat_type, from2, to2)) {
		m12306::exec_ok(conn, "ROLLBACK");
		std::cout << "<p class=\"err\">Transfer booking failed in transaction.</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	m12306::exec_ok(conn, "COMMIT");
	std::cout << "<p class=\"ok\">Transfer booking success. Order ID: " << m12306::html_escape(order_id) << "</p>";
	std::cout << "<p><a href=\"/cgi-bin/orders.cgi?username=" << m12306::html_escape(username)
			  << "\">View Orders</a></p>";

	PQfinish(conn);
	m12306::print_page_end();
	return 0;
}
