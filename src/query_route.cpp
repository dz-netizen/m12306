#include <string>

#include "m12306_common.h"

int main() {
	cgicc::Cgicc form;
	std::string username = m12306::get_form_value(form, "username");
	std::string from_city = m12306::get_form_value(form, "from_city");
	std::string to_city = m12306::get_form_value(form, "to_city");
	std::string seat_type = m12306::get_form_value(form, "seat_type");
	std::string date = m12306::get_form_value(form, "date");
	std::string time = m12306::get_form_value(form, "time");
	if (seat_type.empty()) seat_type = "二等座";
	if (date.empty()) date = "2026-05-01";
	if (time.empty()) time = "00:00";

	m12306::print_page_begin("Query Route");
	std::cout << "<h2>Query Route</h2>";
	std::cout << "<p><a href=\"/cgi-bin/home.cgi?username=" << m12306::html_escape(username)
			  << "\">Back Home</a></p>";

	if (from_city.empty() || to_city.empty()) {
		std::cout << "<p class=\"err\">from_city and to_city are required.</p>";
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

	std::cout << "<p>From: <b>" << m12306::html_escape(from_city)
			  << "</b> To: <b>" << m12306::html_escape(to_city)
			  << "</b>, Date: " << m12306::html_escape(date)
			  << ", Time >= " << m12306::html_escape(time)
			  << ", Seat Type: " << m12306::html_escape(seat_type) << "</p>";

	const char *direct_sql =
		"SELECT tp.train_id, s1.station_id, s1.station_name, s2.station_id, s2.station_name, "
		"       ts1.departure_time::text, ts2.arrival_time::text, tp.price::text, "
		"       COALESCE((SELECT si.remaining FROM seat_inventory si "
		"                 WHERE si.train_id=tp.train_id AND si.travel_date=$3::date AND si.seat_type=$5 "
		"                   AND si.from_station=tp.from_station AND si.to_station=tp.to_station), 5)::text AS left_seat "
		"FROM ticket_price tp "
		"JOIN station s1 ON s1.station_id=tp.from_station "
		"JOIN city c1 ON c1.city_id=s1.city_id "
		"JOIN station s2 ON s2.station_id=tp.to_station "
		"JOIN city c2 ON c2.city_id=s2.city_id "
		"JOIN train_station ts1 ON ts1.train_id=tp.train_id AND ts1.station_id=tp.from_station "
		"JOIN train_station ts2 ON ts2.train_id=tp.train_id AND ts2.station_id=tp.to_station "
		"WHERE c1.city_name=$1 AND c2.city_name=$2 AND c1.city_name<>c2.city_name "
		"  AND tp.seat_type=$5 "
		"  AND ts1.departure_time >= $4::time "
		"  AND COALESCE((SELECT si.remaining FROM seat_inventory si "
		"                 WHERE si.train_id=tp.train_id AND si.travel_date=$3::date AND si.seat_type=$5 "
		"                   AND si.from_station=tp.from_station AND si.to_station=tp.to_station), 5) > 0 "
		"ORDER BY tp.price ASC, "
		"  CASE WHEN ts2.arrival_time>=ts1.departure_time THEN ts2.arrival_time-ts1.departure_time "
		"       ELSE ts2.arrival_time + interval '24 hour' - ts1.departure_time END ASC, "
		"  ts1.departure_time ASC "
		"LIMIT 20";

	const char *params[5] = {from_city.c_str(), to_city.c_str(), date.c_str(), time.c_str(), seat_type.c_str()};
	PGresult *direct = PQexecParams(conn, direct_sql, 5, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(direct) != PGRES_TUPLES_OK) {
		std::cout << "<p class=\"err\">Direct query failed: "
				  << m12306::html_escape(PQresultErrorMessage(direct)) << "</p>";
		PQclear(direct);
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	std::cout << "<h3>Direct Trains</h3>";
	std::cout << "<table><tr><th>Train</th><th>From</th><th>To</th><th>Depart</th><th>Arrive</th><th>Price</th><th>Left</th><th>Book</th></tr>";
	int drows = PQntuples(direct);
	for (int i = 0; i < drows; ++i) {
		std::string left = PQgetvalue(direct, i, 8);
		std::cout << "<tr>"
				  << "<td>" << m12306::html_escape(PQgetvalue(direct, i, 0)) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(direct, i, 2)) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(direct, i, 4)) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(direct, i, 5)) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(direct, i, 6)) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(direct, i, 7)) << "</td>"
				  << "<td>" << m12306::html_escape(left) << "</td>";
		if (std::atoi(left.c_str()) > 0) {
			std::cout << "<td><a href=\"/cgi-bin/book.cgi?username=" << m12306::html_escape(username)
					  << "&train_id=" << m12306::html_escape(PQgetvalue(direct, i, 0))
					  << "&from_sid=" << m12306::html_escape(PQgetvalue(direct, i, 1))
					  << "&to_sid=" << m12306::html_escape(PQgetvalue(direct, i, 3))
					  << "&date=" << m12306::html_escape(date)
					  << "&seat_type=" << m12306::html_escape(seat_type) << "\">Book</a></td>";
		} else {
			std::cout << "<td>-</td>";
		}
		std::cout << "</tr>";
	}
	if (drows == 0) std::cout << "<tr><td colspan=\"8\">No direct routes.</td></tr>";
	std::cout << "</table>";
	PQclear(direct);

	const char *transfer_sql =
		"SELECT tp1.train_id, tp1.from_station::text, s1.station_name, tp1.to_station::text, sx1.station_name, cx1.city_name, "
		"       tp2.train_id, tp2.from_station::text, sx2.station_name, tp2.to_station::text, s2.station_name, "
		"       ts1.departure_time::text, tsx1.arrival_time::text, tsx2.departure_time::text, ts2.arrival_time::text, "
		"       (tp1.price + tp2.price)::text AS total_price, "
		"       LEAST("
		"         COALESCE((SELECT si.remaining FROM seat_inventory si WHERE si.train_id=tp1.train_id AND si.travel_date=$3::date AND si.seat_type=$5 AND si.from_station=tp1.from_station AND si.to_station=tp1.to_station),5),"
		"         COALESCE((SELECT si.remaining FROM seat_inventory si WHERE si.train_id=tp2.train_id AND si.travel_date=$3::date AND si.seat_type=$5 AND si.from_station=tp2.from_station AND si.to_station=tp2.to_station),5)"
		"       )::text AS left_total "
		"FROM ticket_price tp1 "
		"JOIN ticket_price tp2 ON tp1.seat_type=$5 AND tp2.seat_type=$5 AND tp1.train_id<>tp2.train_id "
		"JOIN station s1 ON s1.station_id=tp1.from_station "
		"JOIN city c1 ON c1.city_id=s1.city_id "
		"JOIN station sx1 ON sx1.station_id=tp1.to_station "
		"JOIN city cx1 ON cx1.city_id=sx1.city_id "
		"JOIN station sx2 ON sx2.station_id=tp2.from_station "
		"JOIN city cx2 ON cx2.city_id=sx2.city_id "
		"JOIN station s2 ON s2.station_id=tp2.to_station "
		"JOIN city c2 ON c2.city_id=s2.city_id "
		"JOIN train_station ts1 ON ts1.train_id=tp1.train_id AND ts1.station_id=tp1.from_station "
		"JOIN train_station tsx1 ON tsx1.train_id=tp1.train_id AND tsx1.station_id=tp1.to_station "
		"JOIN train_station tsx2 ON tsx2.train_id=tp2.train_id AND tsx2.station_id=tp2.from_station "
		"JOIN train_station ts2 ON ts2.train_id=tp2.train_id AND ts2.station_id=tp2.to_station "
		"WHERE c1.city_name=$1 AND c2.city_name=$2 AND c1.city_name<>c2.city_name "
		"  AND cx1.city_name=cx2.city_name "
		"  AND ts1.departure_time >= $4::time "
		"  AND ("
		"    (tp1.to_station=tp2.from_station AND (tsx2.departure_time - tsx1.arrival_time) BETWEEN interval '1 hour' AND interval '4 hour') OR "
		"    (tp1.to_station<>tp2.from_station AND (tsx2.departure_time - tsx1.arrival_time) BETWEEN interval '2 hour' AND interval '4 hour')"
		"  ) "
		"ORDER BY (tp1.price + tp2.price) ASC, ts1.departure_time ASC "
		"LIMIT 20";

	PGresult *transfer = PQexecParams(conn, transfer_sql, 5, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(transfer) != PGRES_TUPLES_OK) {
		std::cout << "<p class=\"err\">Transfer query failed: "
				  << m12306::html_escape(PQresultErrorMessage(transfer)) << "</p>";
		PQclear(transfer);
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	std::cout << "<h3>One-Transfer Routes</h3>";
	std::cout << "<table><tr><th>Leg1</th><th>Transfer City</th><th>Leg2</th><th>Times</th><th>Total Price</th><th>Total Left</th><th>Book</th></tr>";
	int trows = PQntuples(transfer);
	for (int i = 0; i < trows; ++i) {
		std::string leg1 = std::string(PQgetvalue(transfer, i, 0)) + " " + PQgetvalue(transfer, i, 2) + "->" + PQgetvalue(transfer, i, 4);
		std::string leg2 = std::string(PQgetvalue(transfer, i, 6)) + " " + PQgetvalue(transfer, i, 8) + "->" + PQgetvalue(transfer, i, 10);
		std::string times = std::string(PQgetvalue(transfer, i, 11)) + " / " + PQgetvalue(transfer, i, 12)
						  + " ; " + PQgetvalue(transfer, i, 13) + " / " + PQgetvalue(transfer, i, 14);
		std::string left_total = PQgetvalue(transfer, i, 16);
		std::cout << "<tr><td>" << m12306::html_escape(leg1) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(transfer, i, 5)) << "</td>"
				  << "<td>" << m12306::html_escape(leg2) << "</td>"
				  << "<td>" << m12306::html_escape(times) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(transfer, i, 15)) << "</td>"
				  << "<td>" << m12306::html_escape(left_total) << "</td>";
		if (std::atoi(left_total.c_str()) > 0) {
			std::cout << "<td><a href=\"/cgi-bin/book.cgi?username=" << m12306::html_escape(username)
					  << "&transfer=1"
					  << "&train1=" << m12306::html_escape(PQgetvalue(transfer, i, 0))
					  << "&from1=" << m12306::html_escape(PQgetvalue(transfer, i, 1))
					  << "&to1=" << m12306::html_escape(PQgetvalue(transfer, i, 3))
					  << "&train2=" << m12306::html_escape(PQgetvalue(transfer, i, 6))
					  << "&from2=" << m12306::html_escape(PQgetvalue(transfer, i, 7))
					  << "&to2=" << m12306::html_escape(PQgetvalue(transfer, i, 9))
					  << "&date=" << m12306::html_escape(date)
					  << "&seat_type=" << m12306::html_escape(seat_type) << "\">Book</a></td>";
		} else {
			std::cout << "<td>-</td>";
		}
		std::cout << "</tr>";
	}
	if (trows == 0) std::cout << "<tr><td colspan=\"7\">No transfer routes.</td></tr>";
	std::cout << "</table>";
	PQclear(transfer);

	std::cout << "<p><a href=\"/query_route.html?username=" << m12306::html_escape(username)
			  << "&from_city=" << m12306::html_escape(to_city)
			  << "&to_city=" << m12306::html_escape(from_city)
			  << "&seat_type=" << m12306::html_escape(seat_type)
			  << "&date=2026-05-02&time=00:00\">Return Trip Query</a></p>";

	PQfinish(conn);
	m12306::print_page_end();
	return 0;
}
