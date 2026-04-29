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
		"WITH req AS ("
		"  SELECT rf.station_order AS req_from_order, rt.station_order AS req_to_order "
		"  FROM train_station rf "
		"  JOIN train_station rt ON rt.train_id=rf.train_id "
		"  WHERE rf.train_id=$1 AND rf.station_id=$4::int AND rt.station_id=$5::int"
		"), targets AS ("
		"  SELECT si.remaining "
		"  FROM seat_inventory si "
		"  JOIN train_station a ON a.train_id=si.train_id AND a.station_id=si.from_station "
		"  JOIN train_station b ON b.train_id=si.train_id AND b.station_id=si.to_station "
		"  JOIN req r ON TRUE "
		"  WHERE si.train_id=$1 AND si.travel_date=$2::date AND si.seat_type=$3 "
		"    AND r.req_from_order < r.req_to_order "
		"    AND a.station_order < r.req_to_order "
		"    AND r.req_from_order < b.station_order "
		"  FOR UPDATE"
		") "
		"SELECT COALESCE(MIN(remaining),0)::text, COUNT(*)::text FROM targets";
	const char *params[5] = {train_id.c_str(), date.c_str(), seat_type.c_str(), from_sid.c_str(), to_sid.c_str()};
	PGresult *res = PQexecParams(conn, sql, 5, NULL, params, NULL, NULL, 0);
	bool ok = false;
	if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1) {
		int min_remaining = std::atoi(PQgetvalue(res, 0, 0));
		int overlap_rows = std::atoi(PQgetvalue(res, 0, 1));
		ok = (overlap_rows > 0 && min_remaining > 0);
	}
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
		"WITH req AS ("
		"  SELECT rf.station_order AS req_from_order, rt.station_order AS req_to_order "
		"  FROM train_station rf "
		"  JOIN train_station rt ON rt.train_id=rf.train_id "
		"  WHERE rf.train_id=$1 AND rf.station_id=$4::int AND rt.station_id=$5::int"
		") "
		"UPDATE seat_inventory si "
		"SET remaining=remaining-1 "
		"FROM train_station a, train_station b, req r "
		"WHERE si.train_id=$1 AND si.travel_date=$2::date AND si.seat_type=$3 "
		"  AND a.train_id=si.train_id AND a.station_id=si.from_station "
		"  AND b.train_id=si.train_id AND b.station_id=si.to_station "
		"  AND r.req_from_order < r.req_to_order "
		"  AND a.station_order < r.req_to_order "
		"  AND r.req_from_order < b.station_order";
	const char *params[5] = {train_id.c_str(), date.c_str(), seat_type.c_str(), from_sid.c_str(), to_sid.c_str()};
	PGresult *res = PQexecParams(conn, sql, 5, NULL, params, NULL, NULL, 0);
	bool ok = false;
	if (PQresultStatus(res) == PGRES_COMMAND_OK) {
		int affected = std::atoi(PQcmdTuples(res));
		ok = (affected > 0);
	}
	PQclear(res);
	return ok;
}

static bool lock_user_booking_scope(PGconn *conn, int user_id) {
	std::string uid_s = std::to_string(user_id);
	const char *sql = "SELECT user_id FROM user_info WHERE user_id=$1::int FOR UPDATE";
	const char *params[1] = {uid_s.c_str()};
	PGresult *res = PQexecParams(conn, sql, 1, NULL, params, NULL, NULL, 0);
	bool ok = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1);
	PQclear(res);
	return ok;
}

static bool has_time_conflict_with_existing(PGconn *conn,
	int user_id,
	const std::string &train_id,
	const std::string &from_sid,
	const std::string &to_sid,
	const std::string &date) {
	std::string uid_s = std::to_string(user_id);
	const char *sql =
		"WITH new_leg AS ("
		"  SELECT ($5::date + tsf.departure_time) AS dep_ts, "
		"         ($5::date + tst.arrival_time "
		"          + CASE WHEN tst.arrival_time <= tsf.departure_time THEN interval '1 day' ELSE interval '0 day' END) AS arr_ts "
		"  FROM train_station tsf "
		"  JOIN train_station tst ON tst.train_id=tsf.train_id "
		"  WHERE tsf.train_id=$2 AND tsf.station_id=$3::int AND tst.station_id=$4::int"
		") "
		"SELECT EXISTS("
		"  SELECT 1 "
		"  FROM new_leg nl "
		"  JOIN orders o ON o.user_id=$1::int AND o.status='正常' "
		"  JOIN order_item oi ON oi.order_id=o.order_id "
		"  JOIN train_station esf ON esf.train_id=oi.train_id AND esf.station_id=oi.from_station "
		"  JOIN train_station est ON est.train_id=oi.train_id AND est.station_id=oi.to_station "
		"  WHERE (oi.travel_date + esf.departure_time) < nl.arr_ts "
		"    AND nl.dep_ts < (oi.travel_date + est.arrival_time "
		"      + CASE WHEN est.arrival_time <= esf.departure_time THEN interval '1 day' ELSE interval '0 day' END)"
		")";
	const char *params[5] = {uid_s.c_str(), train_id.c_str(), from_sid.c_str(), to_sid.c_str(), date.c_str()};
	PGresult *res = PQexecParams(conn, sql, 5, NULL, params, NULL, NULL, 0);
	bool conflict = false;
	if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1) {
		conflict = (std::string(PQgetvalue(res, 0, 0)) == "t");
	}
	PQclear(res);
	return conflict;
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
	if (!m12306::is_after_today(date)) {
		std::cout << "<p class=\"err\">Invalid travel date: booking is only allowed for dates after today.</p>";
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
		if (!lock_user_booking_scope(conn, user_id)) {
			m12306::exec_ok(conn, "ROLLBACK");
			std::cout << "<p class=\"err\">User lock failed.</p>";
			PQfinish(conn);
			m12306::print_page_end();
			return 1;
		}
		if (has_time_conflict_with_existing(conn, user_id, train_id, from_sid, to_sid, date)) {
			m12306::exec_ok(conn, "ROLLBACK");
			std::cout << "<p class=\"err\">Booking time conflicts with your existing valid order.</p>";
			PQfinish(conn);
			m12306::print_page_end();
			return 0;
		}
		if (!lock_and_check_inventory(conn, train_id, date, seat_type, from_sid, to_sid) ||
			!dec_inventory(conn, train_id, date, seat_type, from_sid, to_sid)) {
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
		if (PQresultStatus(it) != PGRES_COMMAND_OK) {
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
	if (!lock_user_booking_scope(conn, user_id)) {
		m12306::exec_ok(conn, "ROLLBACK");
		std::cout << "<p class=\"err\">User lock failed.</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}
	if (has_time_conflict_with_existing(conn, user_id, train1, from1, to1, date) ||
		has_time_conflict_with_existing(conn, user_id, train2, from2, to2, date)) {
		m12306::exec_ok(conn, "ROLLBACK");
		std::cout << "<p class=\"err\">Booking time conflicts with your existing valid order.</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 0;
	}
	if (!lock_and_check_inventory(conn, train1, date, seat_type, from1, to1) ||
		!lock_and_check_inventory(conn, train2, date, seat_type, from2, to2) ||
		!dec_inventory(conn, train1, date, seat_type, from1, to1) ||
		!dec_inventory(conn, train2, date, seat_type, from2, to2)) {
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

	if (!ok_items) {
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
