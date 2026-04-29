#include <string>
#include <vector>

#include "m12306_common.h"

int main() {
	cgicc::Cgicc form;
	std::string username = m12306::get_form_value(form, "username");
	std::string train_id = m12306::get_form_value(form, "train_id");
	std::string date = m12306::get_form_value(form, "date");
	if (date.empty()) date = m12306::tomorrow_date();

	m12306::print_page_begin("Query Train");
	std::cout << "<h2>Query Train</h2>";
	std::cout << "<p><a href=\"/cgi-bin/home.cgi?username=" << m12306::html_escape(username)
			  << "\">Back Home</a></p>";

	if (train_id.empty()) {
		std::cout << "<p class=\"err\">train_id is required.</p>";
		m12306::print_page_end();
		return 0;
	}
	if (!m12306::is_after_today(date)) {
		std::cout << "<p class=\"err\">Invalid date: you can only book trains for dates after today.</p>";
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

	m12306::ensure_inventory(conn, train_id, date);
	const char *seat_sql =
		"SELECT seat_type FROM ("
		"  SELECT DISTINCT seat_type "
		"  FROM ticket_price "
		"  WHERE train_id=$1"
		") t "
		"ORDER BY CASE t.seat_type "
		"  WHEN '商务座' THEN 1 "
		"  WHEN '一等座' THEN 2 "
		"  WHEN '二等座' THEN 3 "
		"  ELSE 99 END, t.seat_type";
	const char *seat_params[1] = {train_id.c_str()};
	PGresult *seat_res = PQexecParams(conn, seat_sql, 1, NULL, seat_params, NULL, NULL, 0);
	if (PQresultStatus(seat_res) != PGRES_TUPLES_OK) {
		std::cout << "<p class=\"err\">Seat-type query failed: "
				  << m12306::html_escape(PQresultErrorMessage(seat_res)) << "</p>";
		PQclear(seat_res);
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	std::vector<std::string> seat_types;
	for (int i = 0; i < PQntuples(seat_res); ++i) {
		seat_types.push_back(PQgetvalue(seat_res, i, 0));
	}
	PQclear(seat_res);
	if (seat_types.empty()) {
		std::cout << "<p class=\"err\">No seat types found for this train.</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 0;
	}

	const char *sql =
		"WITH start_station AS ("
		"  SELECT station_id AS start_sid FROM train_station WHERE train_id=$1 ORDER BY station_order LIMIT 1"
		") "
		"SELECT ts.station_order, c.city_name, s.station_name, "
		"       COALESCE(ts.arrival_time::text,'-') AS arr, COALESCE(ts.departure_time::text,'-') AS dep, "
		"       COALESCE(tp.price::text,'-') AS fare, "
		"       CASE WHEN ts.station_order=1 THEN 0 ELSE COALESCE(("
		"         SELECT MIN(si.remaining) "
		"         FROM seat_inventory si "
		"         JOIN train_station a ON a.train_id=si.train_id AND a.station_id=si.from_station "
		"         JOIN train_station b ON b.train_id=si.train_id AND b.station_id=si.to_station "
		"         WHERE si.train_id=$1 AND si.travel_date=$2::date AND si.seat_type=$3 "
		"           AND a.station_order>=1 AND b.station_order=ts.station_order"
		"       ), 5) END AS left_seat, "
		"       ts.station_id "
		"FROM train_station ts "
		"JOIN station s ON s.station_id=ts.station_id "
		"JOIN city c ON c.city_id=s.city_id "
		"LEFT JOIN start_station ss ON TRUE "
		"LEFT JOIN ticket_price tp ON tp.train_id=ts.train_id AND tp.from_station=ss.start_sid "
		"  AND tp.to_station=ts.station_id AND tp.seat_type=$3 "
		"WHERE ts.train_id=$1 "
		"ORDER BY ts.station_order";

	std::vector<std::string> station_sid;
	std::vector<std::string> station_name;
	std::vector<std::string> station_order;
	bool loaded_stations = false;
	int stops = 0;

	std::cout << "<p>Train: <b>" << m12306::html_escape(train_id) << "</b>, Date: "
			  << m12306::html_escape(date) << ", Seat Type: all</p>";

	for (size_t si = 0; si < seat_types.size(); ++si) {
		const std::string &seat_type = seat_types[si];
		const char *params[3] = {train_id.c_str(), date.c_str(), seat_type.c_str()};
		PGresult *res = PQexecParams(conn, sql, 3, NULL, params, NULL, NULL, 0);
		if (PQresultStatus(res) != PGRES_TUPLES_OK) {
			std::cout << "<p class=\"err\">Query failed for seat type "
					  << m12306::html_escape(seat_type) << ": "
					  << m12306::html_escape(PQresultErrorMessage(res)) << "</p>";
			PQclear(res);
			PQfinish(conn);
			m12306::print_page_end();
			return 1;
		}

		int rows = PQntuples(res);
		if (rows > stops) stops = rows;

		if (!loaded_stations && rows > 0) {
			for (int i = 0; i < rows; ++i) {
				station_order.push_back(PQgetvalue(res, i, 0));
				station_name.push_back(PQgetvalue(res, i, 2));
				station_sid.push_back(PQgetvalue(res, i, 7));
			}
			loaded_stations = true;
		}

		std::cout << "<h3>Seat Type: " << m12306::html_escape(seat_type) << "</h3>";
		std::cout << "<table><tr>"
				  << "<th>Order</th><th>City</th><th>Station</th><th>Arrival</th><th>Departure</th>"
				  << "<th>Fare</th><th>Left</th><th>Book(From Start)</th></tr>";

		std::string start_sid = (rows > 0) ? PQgetvalue(res, 0, 7) : "";
		for (int i = 0; i < rows; ++i) {
			std::string order = PQgetvalue(res, i, 0);
			std::string sid = PQgetvalue(res, i, 7);
			std::string fare = PQgetvalue(res, i, 5);
			int left = std::atoi(PQgetvalue(res, i, 6));

			std::cout << "<tr>"
					  << "<td>" << m12306::html_escape(order) << "</td>"
					  << "<td>" << m12306::html_escape(PQgetvalue(res, i, 1)) << "</td>"
					  << "<td>" << m12306::html_escape(PQgetvalue(res, i, 2)) << "</td>"
					  << "<td>" << m12306::html_escape(PQgetvalue(res, i, 3)) << "</td>"
					  << "<td>" << m12306::html_escape(PQgetvalue(res, i, 4)) << "</td>"
					  << "<td>" << m12306::html_escape(fare) << "</td>"
					  << "<td>" << left << "</td>";

			if (i > 0 && left > 0 && fare != "-") {
				std::cout << "<td><a href=\"/cgi-bin/book.cgi?username=" << m12306::html_escape(username)
						  << "&train_id=" << m12306::html_escape(train_id)
						  << "&from_sid=" << m12306::html_escape(start_sid)
						  << "&to_sid=" << m12306::html_escape(sid)
						  << "&date=" << m12306::html_escape(date)
						  << "&seat_type=" << m12306::html_escape(seat_type) << "\">Book</a></td>";
			} else {
				std::cout << "<td>-</td>";
			}
			std::cout << "</tr>";
		}
		std::cout << "</table>";
		PQclear(res);
	}

	std::cout << "<p>Stops: " << stops << "</p>";

	if (station_sid.size() >= 2) {
		std::cout << "<h3>Book Any Segment</h3>";
		std::cout << "<p>Select any start/end station (start must be before end).</p>";
		std::cout << "<form id=\"segment_form\" method=\"get\" action=\"/cgi-bin/book.cgi\">"
				  << "<input type=\"hidden\" name=\"username\" value=\"" << m12306::html_escape(username) << "\">"
				  << "<input type=\"hidden\" name=\"train_id\" value=\"" << m12306::html_escape(train_id) << "\">"
				  << "<input type=\"hidden\" name=\"date\" value=\"" << m12306::html_escape(date) << "\">";

		std::cout << "From: <select id=\"from_sid\" name=\"from_sid\">";
		for (size_t i = 0; i + 1 < station_sid.size(); ++i) {
			std::string sid = station_sid[i];
			std::string station = station_name[i];
			std::string order = station_order[i];
			std::cout << "<option value=\"" << m12306::html_escape(sid) << "\" data-order=\""
					  << m12306::html_escape(order) << "\">"
					  << m12306::html_escape(order + std::string(". ") + station) << "</option>";
		}
		std::cout << "</select> ";

		std::cout << "To: <select id=\"to_sid\" name=\"to_sid\">";
		for (size_t i = 1; i < station_sid.size(); ++i) {
			std::string sid = station_sid[i];
			std::string station = station_name[i];
			std::string order = station_order[i];
			std::cout << "<option value=\"" << m12306::html_escape(sid) << "\" data-order=\""
					  << m12306::html_escape(order) << "\">"
					  << m12306::html_escape(order + std::string(". ") + station) << "</option>";
		}
		std::cout << "</select> ";

		std::cout << "Seat Type: <select name=\"seat_type\">";
		for (size_t i = 0; i < seat_types.size(); ++i) {
			std::cout << "<option value=\"" << m12306::html_escape(seat_types[i]) << "\">"
					  << m12306::html_escape(seat_types[i]) << "</option>";
		}
		std::cout << "</select> ";
		std::cout << "<button type=\"submit\">Book Segment</button>";
		std::cout << "</form>";

		std::cout << "<script>"
				  "(function(){"
				  "const fs=document.getElementById('from_sid');"
				  "const ts=document.getElementById('to_sid');"
				  "const form=document.getElementById('segment_form');"
				  "function valid(){"
				  "const fo=parseInt(fs.options[fs.selectedIndex].dataset.order||'0',10);"
				  "const to=parseInt(ts.options[ts.selectedIndex].dataset.order||'0',10);"
				  "return fo<to;"
				  "}"
				  "form.addEventListener('submit',function(e){"
				  "if(!valid()){e.preventDefault();alert('Start station must be before end station.');}"
				  "});"
				  "})();"
				  "</script>";
	}

	PQfinish(conn);
	m12306::print_page_end();
	return 0;
}
