#include <string>
#include <map>
#include <vector>

#include "m12306_common.h"

struct DirectSeatRow {
	std::string seat_type;
	std::string price;
	std::string left;
};

struct DirectGroup {
	std::string train_id;
	std::string from_sid;
	std::string from_name;
	std::string to_sid;
	std::string to_name;
	std::string depart;
	std::string arrive;
	std::map<std::string, DirectSeatRow> seat_map;
};

struct TransferSeatRow {
	std::string seat_type;
	std::string total_price;
	std::string left_total;
};

struct TransferGroup {
	std::string train1;
	std::string from1;
	std::string from1_name;
	std::string to1;
	std::string to1_name;
	std::string transfer_city;
	std::string train2;
	std::string from2;
	std::string from2_name;
	std::string to2;
	std::string to2_name;
	std::string depart1;
	std::string arrive1;
	std::string depart2;
	std::string arrive2;
	std::map<std::string, TransferSeatRow> seat_map;
};

static std::string short_time(const std::string &s) {
	if (s.size() >= 5) return s.substr(0, 5);
	return s;
}

static int parse_positive_int(const std::string &value, int fallback) {
	if (value.empty()) return fallback;
	int parsed = std::atoi(value.c_str());
	if (parsed <= 0) return fallback;
	return parsed;
}

static void render_transfer_panel(const std::vector<TransferGroup> &transfer_groups,
							  const std::string &username,
							  const std::string &date,
							  int transfer_page,
							  bool partial_response) {
	const size_t page_size = 20;
	std::vector<std::string> seat_order;
	seat_order.push_back("商务座");
	seat_order.push_back("一等座");
	seat_order.push_back("二等座");

	size_t total_groups = transfer_groups.size();
	size_t total_pages = total_groups == 0 ? 1 : (total_groups + page_size - 1) / page_size;
	if (transfer_page < 1) transfer_page = 1;
	if (static_cast<size_t>(transfer_page) > total_pages) transfer_page = static_cast<int>(total_pages);
	size_t start = static_cast<size_t>(transfer_page - 1) * page_size;
	size_t end = start + page_size;
	if (end > total_groups) end = total_groups;

	std::cout << "<div id=\"transfer-panel\">"
		  << "<h3>中转路线";
	if (total_groups > 0) {
		std::cout << " <span class=\"pager-hint\">第 " << transfer_page << " / " << total_pages << " 页</span>";
	}
	std::cout << "</h3>";

	std::cout << "<table><thead><tr><th>第一段</th><th>中转城市</th><th>第二段</th><th>时间</th><th>席别</th><th>总价</th><th>余票</th><th>购票</th></tr></thead><tbody>";
	if (total_groups == 0) {
		std::cout << "<tr><td colspan=\"8\">暂无中转路线。</td></tr>";
	} else {
		for (size_t gi = start; gi < end; ++gi) {
			const TransferGroup &g = transfer_groups[gi];
			std::string leg1 = g.train1 + " " + g.from1_name + "->" + g.to1_name;
			std::string leg2 = g.train2 + " " + g.from2_name + "->" + g.to2_name;
			for (size_t si = 0; si < seat_order.size(); ++si) {
				TransferSeatRow seat;
				seat.seat_type = seat_order[si];
				seat.total_price = "-";
				seat.left_total = "0";
				std::map<std::string, TransferSeatRow>::const_iterator it = g.seat_map.find(seat_order[si]);
				if (it != g.seat_map.end()) {
					seat = it->second;
				}
				std::cout << "<tr>";
				if (si == 0) {
					std::cout << "<td rowspan=\"3\">" << m12306::html_escape(leg1) << "</td>"
						  << "<td rowspan=\"3\">" << m12306::html_escape(g.transfer_city) << "</td>"
						  << "<td rowspan=\"3\">" << m12306::html_escape(leg2) << "</td>"
						  << "<td rowspan=\"3\"><div class=\"trip-times\">"
						  << "<div><span class=\"trip-label\">第一段</span><span class=\"trip-value\">" << m12306::html_escape(short_time(g.depart1)) << " → " << m12306::html_escape(short_time(g.arrive1)) << "</span></div>"
						  << "<div><span class=\"trip-label\">第二段</span><span class=\"trip-value\">" << m12306::html_escape(short_time(g.depart2)) << " → " << m12306::html_escape(short_time(g.arrive2)) << "</span></div>"
						  << "</div></td>";
				}
				std::cout << "<td>" << m12306::html_escape(seat.seat_type) << "</td>"
					  << "<td>" << m12306::html_escape(seat.total_price) << "</td>"
					  << "<td>" << m12306::html_escape(seat.left_total) << "</td>";
				if (seat.total_price != "-" && std::atoi(seat.left_total.c_str()) > 0) {
					std::cout << "<td><a href=\"/cgi-bin/book.cgi?username=" << m12306::html_escape(username)
						  << "&transfer=1"
						  << "&train1=" << m12306::html_escape(g.train1)
						  << "&from1=" << m12306::html_escape(g.from1)
						  << "&to1=" << m12306::html_escape(g.to1)
						  << "&train2=" << m12306::html_escape(g.train2)
						  << "&from2=" << m12306::html_escape(g.from2)
						  << "&to2=" << m12306::html_escape(g.to2)
						  << "&date=" << m12306::html_escape(date)
						  << "&seat_type=" << m12306::html_escape(seat.seat_type) << "\">购票</a></td>";
				} else {
					std::cout << "<td>-</td>";
				}
				std::cout << "</tr>";
			}
		}
	}
	std::cout << "</tbody></table>";

	if (total_groups > page_size) {
		std::cout << "<div class=\"pager\">";
		if (transfer_page > 1) {
			std::cout << "<a class=\"transfer-page-link\" data-page=\"" << (transfer_page - 1)
				  << "\" href=\"#\">上一页</a>";
		}
		if (transfer_page < static_cast<int>(total_pages)) {
			std::cout << "<a class=\"transfer-page-link\" data-page=\"" << (transfer_page + 1)
				  << "\" href=\"#\">下一页</a>";
		}
		std::cout << "</div>";
	}

	if (!partial_response) {
		std::cout << "<script>"
			  << "(function(){"
			  << "function bindPager(){"
			  << "var panel=document.getElementById('transfer-panel');"
			  << "if(!panel){return;}"
			  << "panel.querySelectorAll('a.transfer-page-link').forEach(function(link){"
			  << "link.addEventListener('click', function(ev){"
			  << "ev.preventDefault();"
			  << "var url=new URL(window.location.href);"
			  << "url.searchParams.set('transfer_page', link.dataset.page);"
			  << "url.searchParams.set('partial', '1');"
			  << "fetch(url.toString(), {credentials: 'same-origin'})"
			  << ".then(function(resp){return resp.text();})"
			  << ".then(function(html){panel.outerHTML = html; bindPager();});"
			  << "});"
			  << "});"
			  << "}"
			  << "bindPager();"
			  << "})();"
			  << "</script>";
	}
	std::cout << "</div>";
}

static bool load_transfer_groups(PGconn *conn,
							 const std::string &from_city,
							 const std::string &to_city,
							 const std::string &date,
							 const std::string &time,
							 int from_station_id_filter,
							 int to_station_id_filter,
							 std::vector<TransferGroup> &transfer_groups,
							 std::string &error_message) {
	const char *transfer_sql =
		"WITH transfer_candidates AS ("
		"  SELECT tp1.train_id, tp1.from_station::text, s1.station_name, tp1.to_station::text, sx1.station_name, cx1.city_name, "
		"         tp2.train_id, tp2.from_station::text, sx2.station_name, tp2.to_station::text, s2.station_name, "
		"         tp1.seat_type, "
		"         ts1.departure_time::text, tsx1.arrival_time::text, tsx2.departure_time::text, ts2.arrival_time::text, "
		"         (tp1.price + tp2.price)::text AS total_price, "
		"         LEAST("
		"           COALESCE((SELECT si.remaining FROM seat_inventory si WHERE si.train_id=tp1.train_id AND si.travel_date=$3::date AND si.seat_type=tp1.seat_type AND si.from_station=tp1.from_station AND si.to_station=tp1.to_station),5),"
		"           COALESCE((SELECT si.remaining FROM seat_inventory si WHERE si.train_id=tp2.train_id AND si.travel_date=$3::date AND si.seat_type=tp2.seat_type AND si.from_station=tp2.from_station AND si.to_station=tp2.to_station),5)"
		"         )::text AS left_total, "
		"         ts1.departure_time AS departure_time_val, "
		"         ROW_NUMBER() OVER (PARTITION BY tp1.train_id, tp2.train_id, tp1.seat_type "
		"                            ORDER BY (tp1.price + tp2.price) ASC) AS price_rank "
		"  FROM ticket_price tp1 "
		"  JOIN ticket_price tp2 ON tp1.seat_type=tp2.seat_type AND tp1.train_id<>tp2.train_id "
		"  JOIN station s1 ON s1.station_id=tp1.from_station "
		"  JOIN city c1 ON c1.city_id=s1.city_id "
		"  JOIN station sx1 ON sx1.station_id=tp1.to_station "
		"  JOIN city cx1 ON cx1.city_id=sx1.city_id "
		"  JOIN station sx2 ON sx2.station_id=tp2.from_station "
		"  JOIN city cx2 ON cx2.city_id=sx2.city_id "
		"  JOIN station s2 ON s2.station_id=tp2.to_station "
		"  JOIN city c2 ON c2.city_id=s2.city_id "
		"  JOIN train_station ts1 ON ts1.train_id=tp1.train_id AND ts1.station_id=tp1.from_station "
		"  JOIN train_station tsx1 ON tsx1.train_id=tp1.train_id AND tsx1.station_id=tp1.to_station "
		"  JOIN train_station tsx2 ON tsx2.train_id=tp2.train_id AND tsx2.station_id=tp2.from_station "
		"  JOIN train_station ts2 ON ts2.train_id=tp2.train_id AND ts2.station_id=tp2.to_station "
		"  WHERE c1.city_name=$1 AND c2.city_name=$2 AND c1.city_name<>c2.city_name "
		"    AND c1.city_name<>cx1.city_name "
		"    AND cx2.city_name<>c2.city_name "
		"    AND cx1.city_name=cx2.city_name "
		"    AND ts1.departure_time >= $4::time "
		"    AND ($5::int=0 OR tp1.from_station=$5::int) "
		"    AND ($6::int=0 OR tp2.to_station=$6::int) "
		"    AND ("
		"      (tp1.to_station=tp2.from_station AND (tsx2.departure_time - tsx1.arrival_time) BETWEEN interval '1 hour' AND interval '4 hour') OR "
		"      (tp1.to_station<>tp2.from_station AND (tsx2.departure_time - tsx1.arrival_time) BETWEEN interval '2 hour' AND interval '4 hour')"
		"    ) "
		") "
		"SELECT * FROM transfer_candidates WHERE price_rank = 1 "
		"ORDER BY departure_time_val ASC";

	std::string from_station_id_s = std::to_string(from_station_id_filter);
	std::string to_station_id_s = std::to_string(to_station_id_filter);
	const char *params[6] = {from_city.c_str(), to_city.c_str(), date.c_str(), time.c_str(), from_station_id_s.c_str(), to_station_id_s.c_str()};
	PGresult *transfer = PQexecParams(conn, transfer_sql, 6, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(transfer) != PGRES_TUPLES_OK) {
		error_message = PQresultErrorMessage(transfer);
		PQclear(transfer);
		return false;
	}

	int trows = PQntuples(transfer);
	std::map<std::string, size_t> transfer_index;
	for (int i = 0; i < trows; ++i) {
		std::string depart1 = PQgetvalue(transfer, i, 12);
		std::string arrive1 = PQgetvalue(transfer, i, 13);
		std::string depart2 = PQgetvalue(transfer, i, 14);
		std::string arrive2 = PQgetvalue(transfer, i, 15);
		std::string times = short_time(depart1) + "|" + short_time(arrive1) + "|"
						  + short_time(depart2) + "|" + short_time(arrive2);
		std::string key = std::string(PQgetvalue(transfer, i, 0)) + "|"
					 + PQgetvalue(transfer, i, 1) + "|"
					 + PQgetvalue(transfer, i, 3) + "|"
					 + PQgetvalue(transfer, i, 6) + "|"
					 + PQgetvalue(transfer, i, 7) + "|"
					 + PQgetvalue(transfer, i, 9) + "|"
					 + times;
		std::map<std::string, size_t>::iterator it = transfer_index.find(key);
		size_t idx;
		if (it == transfer_index.end()) {
			TransferGroup g;
			g.train1 = PQgetvalue(transfer, i, 0);
			g.from1 = PQgetvalue(transfer, i, 1);
			g.from1_name = PQgetvalue(transfer, i, 2);
			g.to1 = PQgetvalue(transfer, i, 3);
			g.to1_name = PQgetvalue(transfer, i, 4);
			g.transfer_city = PQgetvalue(transfer, i, 5);
			g.train2 = PQgetvalue(transfer, i, 6);
			g.from2 = PQgetvalue(transfer, i, 7);
			g.from2_name = PQgetvalue(transfer, i, 8);
			g.to2 = PQgetvalue(transfer, i, 9);
			g.to2_name = PQgetvalue(transfer, i, 10);
			g.depart1 = depart1;
			g.arrive1 = arrive1;
			g.depart2 = depart2;
			g.arrive2 = arrive2;
			transfer_groups.push_back(g);
			idx = transfer_groups.size() - 1;
			transfer_index[key] = idx;
		} else {
			idx = it->second;
		}
		TransferSeatRow seat;
		seat.seat_type = PQgetvalue(transfer, i, 11);
		seat.total_price = PQgetvalue(transfer, i, 16);
		seat.left_total = PQgetvalue(transfer, i, 17);
		transfer_groups[idx].seat_map[seat.seat_type] = seat;
	}

	PQclear(transfer);
	return true;
}

int main() {
	cgicc::Cgicc form;
	std::string username = m12306::get_form_value(form, "username");
	std::string from_city = m12306::get_form_value(form, "from_city");
	std::string to_city = m12306::get_form_value(form, "to_city");
	std::string from_station = m12306::get_form_value(form, "from_station");
	std::string to_station = m12306::get_form_value(form, "to_station");
	std::string date = m12306::get_form_value(form, "date");
	std::string time = m12306::get_form_value(form, "time");
	std::string transfer_page_value = m12306::get_form_value(form, "transfer_page");
	std::string partial_value = m12306::get_form_value(form, "partial");
	if (date.empty()) date = m12306::tomorrow_date();
	if (time.empty()) time = "00:00";
	int transfer_page = parse_positive_int(transfer_page_value, 1);
	bool partial_response = (partial_value == "1");
	if (partial_response) {
		std::cout << "Content-type:text/html\n\n";
	}

	if (from_city.empty() || to_city.empty()) {
		if (!partial_response) m12306::print_page_begin("查询线路");
		std::cout << "<p class=\"err\">from_city and to_city are required.</p>";
		m12306::print_page_end();
		return 0;
	}
	if (!m12306::is_after_today(date)) {
		if (!partial_response) m12306::print_page_begin("查询线路");
		std::cout << "<p class=\"err\">Invalid date: you can only book trains for dates after today.</p>";
		m12306::print_page_end();
		return 0;
	}

	PGconn *conn = m12306::connect_db();
	if (PQstatus(conn) != CONNECTION_OK) {
		if (!partial_response) m12306::print_page_begin("查询线路");
		std::cout << "<p class=\"err\">DB connection failed: "
				  << m12306::html_escape(PQerrorMessage(conn)) << "</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	const char *city_station_sql =
		"SELECT s.station_name "
		"FROM station s JOIN city c ON c.city_id=s.city_id "
		"WHERE c.city_name=$1 "
		"ORDER BY s.station_name";
	const char *from_city_param[1] = {from_city.c_str()};
	const char *to_city_param[1] = {to_city.c_str()};
	PGresult *from_station_res = PQexecParams(conn, city_station_sql, 1, NULL, from_city_param, NULL, NULL, 0);
	PGresult *to_station_res = PQexecParams(conn, city_station_sql, 1, NULL, to_city_param, NULL, NULL, 0);

	if (!partial_response) {
		m12306::print_page_begin("查询线路");
		std::cout << "<a href=\"/cgi-bin/home.cgi?username=" << m12306::html_escape(username)
				  << "\" class=\"back-link\">← 返回首页</a>";
		std::cout << "<div class=\"query-info-card\">"
				  << "<div class=\"info-item\"><label>出发城市</label><b>" << m12306::html_escape(from_city) << "</b></div>"
				  << "<div class=\"info-item\"><label>到达城市</label><b>" << m12306::html_escape(to_city) << "</b></div>"
				  << "<div class=\"info-item\"><label>出发日期</label><b>" << m12306::html_escape(date) << "</b></div>"
				  << "<div class=\"info-item\"><label>起始时间</label><b>≥ " << m12306::html_escape(time) << "</b></div>"
				  << "</div>";
		std::cout << "<form method=\"get\" action=\"/cgi-bin/query_route.cgi\" class=\"filter-form\">"
				  << "<input type=\"hidden\" name=\"username\" value=\"" << m12306::html_escape(username) << "\">"
				  << "<input type=\"hidden\" name=\"from_city\" value=\"" << m12306::html_escape(from_city) << "\">"
				  << "<input type=\"hidden\" name=\"to_city\" value=\"" << m12306::html_escape(to_city) << "\">"
				  << "<input type=\"hidden\" name=\"date\" value=\"" << m12306::html_escape(date) << "\">"
				  << "<input type=\"hidden\" name=\"time\" value=\"" << m12306::html_escape(time) << "\">";
		std::cout << "<label>出发站<select name=\"from_station\"><option value=\"\">(全部 "
				  << m12306::html_escape(from_city) << " 站)</option>";
		if (PQresultStatus(from_station_res) == PGRES_TUPLES_OK) {
			for (int i = 0; i < PQntuples(from_station_res); ++i) {
				std::string sname = PQgetvalue(from_station_res, i, 0);
				std::cout << "<option value=\"" << m12306::html_escape(sname) << "\"";
				if (sname == from_station) std::cout << " selected";
				std::cout << ">" << m12306::html_escape(sname) << "</option>";
			}
		}
		std::cout << "</select></label>";
		std::cout << "<label>到达站<select name=\"to_station\"><option value=\"\">(全部 "
				  << m12306::html_escape(to_city) << " 站)</option>";
		if (PQresultStatus(to_station_res) == PGRES_TUPLES_OK) {
			for (int i = 0; i < PQntuples(to_station_res); ++i) {
				std::string sname = PQgetvalue(to_station_res, i, 0);
				std::cout << "<option value=\"" << m12306::html_escape(sname) << "\"";
				if (sname == to_station) std::cout << " selected";
				std::cout << ">" << m12306::html_escape(sname) << "</option>";
			}
		}
		std::cout << "</select></label><button type=\"submit\">筛选</button></form>";
	}
	PQclear(from_station_res);
	PQclear(to_station_res);

	int from_station_id_filter = 0;
	int to_station_id_filter = 0;
	if (!from_station.empty()) {
		const char *sid_sql =
			"SELECT s.station_id::text "
			"FROM station s JOIN city c ON c.city_id=s.city_id "
			"WHERE c.city_name=$1 AND s.station_name=$2 "
			"LIMIT 1";
		const char *sid_params[2] = {from_city.c_str(), from_station.c_str()};
		PGresult *sid_res = PQexecParams(conn, sid_sql, 2, NULL, sid_params, NULL, NULL, 0);
		if (PQresultStatus(sid_res) != PGRES_TUPLES_OK || PQntuples(sid_res) == 0) {
			std::cout << "<p class=\"err\">Invalid from_station for selected city.</p>";
			PQclear(sid_res);
			PQfinish(conn);
			m12306::print_page_end();
			return 0;
		}
		from_station_id_filter = std::atoi(PQgetvalue(sid_res, 0, 0));
		PQclear(sid_res);
	}
	if (!to_station.empty()) {
		const char *sid_sql =
			"SELECT s.station_id::text "
			"FROM station s JOIN city c ON c.city_id=s.city_id "
			"WHERE c.city_name=$1 AND s.station_name=$2 "
			"LIMIT 1";
		const char *sid_params[2] = {to_city.c_str(), to_station.c_str()};
		PGresult *sid_res = PQexecParams(conn, sid_sql, 2, NULL, sid_params, NULL, NULL, 0);
		if (PQresultStatus(sid_res) != PGRES_TUPLES_OK || PQntuples(sid_res) == 0) {
			std::cout << "<p class=\"err\">Invalid to_station for selected city.</p>";
			PQclear(sid_res);
			PQfinish(conn);
			m12306::print_page_end();
			return 0;
		}
		to_station_id_filter = std::atoi(PQgetvalue(sid_res, 0, 0));
		PQclear(sid_res);
	}

	if (partial_response) {
		std::vector<TransferGroup> transfer_groups;
		std::string transfer_error;
		if (!load_transfer_groups(conn, from_city, to_city, date, time, from_station_id_filter, to_station_id_filter, transfer_groups, transfer_error)) {
			std::cout << "<div id=\"transfer-panel\"><p class=\"err\">Transfer query failed: "
				  << m12306::html_escape(transfer_error) << "</p></div>";
			PQfinish(conn);
			return 1;
		}
		render_transfer_panel(transfer_groups, username, date, transfer_page, true);
		PQfinish(conn);
		return 0;
	}

	const char *direct_sql =
		"SELECT tp.train_id, s1.station_id, s1.station_name, s2.station_id, s2.station_name, tp.seat_type, "
		"       ts1.departure_time::text, ts2.arrival_time::text, tp.price::text, "
		"       COALESCE((SELECT si.remaining FROM seat_inventory si "
		"                 WHERE si.train_id=tp.train_id AND si.travel_date=$3::date AND si.seat_type=tp.seat_type "
		"                   AND si.from_station=tp.from_station AND si.to_station=tp.to_station), 5)::text AS left_seat "
		"FROM ticket_price tp "
		"JOIN station s1 ON s1.station_id=tp.from_station "
		"JOIN city c1 ON c1.city_id=s1.city_id "
		"JOIN station s2 ON s2.station_id=tp.to_station "
		"JOIN city c2 ON c2.city_id=s2.city_id "
		"JOIN train_station ts1 ON ts1.train_id=tp.train_id AND ts1.station_id=tp.from_station "
		"JOIN train_station ts2 ON ts2.train_id=tp.train_id AND ts2.station_id=tp.to_station "
		"WHERE c1.city_name=$1 AND c2.city_name=$2 AND c1.city_name<>c2.city_name "
		"  AND ts1.departure_time >= $4::time "
		"  AND ($5::int=0 OR tp.from_station=$5::int) "
		"  AND ($6::int=0 OR tp.to_station=$6::int) "
		"  AND COALESCE((SELECT si.remaining FROM seat_inventory si "
		"                 WHERE si.train_id=tp.train_id AND si.travel_date=$3::date AND si.seat_type=tp.seat_type "
		"                   AND si.from_station=tp.from_station AND si.to_station=tp.to_station), 5) > 0 "
		"ORDER BY ts1.departure_time ASC, tp.price ASC, tp.seat_type ASC, "
		"  CASE WHEN ts2.arrival_time>=ts1.departure_time THEN ts2.arrival_time-ts1.departure_time "
		"       ELSE ts2.arrival_time + interval '24 hour' - ts1.departure_time END ASC";

	std::string from_station_id_s = std::to_string(from_station_id_filter);
	std::string to_station_id_s = std::to_string(to_station_id_filter);
	const char *params[6] = {from_city.c_str(), to_city.c_str(), date.c_str(), time.c_str(), from_station_id_s.c_str(), to_station_id_s.c_str()};
	PGresult *direct = PQexecParams(conn, direct_sql, 6, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(direct) != PGRES_TUPLES_OK) {
		std::cout << "<p class=\"err\">Direct query failed: "
				  << m12306::html_escape(PQresultErrorMessage(direct)) << "</p>";
		PQclear(direct);
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	std::cout << "<h3>直达列车</h3>";
	std::cout << "<table><thead><tr><th>车次</th><th>出发站</th><th>到达站</th><th>发车</th><th>到站</th><th>席别</th><th>票价</th><th>余票</th><th>购票</th></tr></thead><tbody>";
	int drows = PQntuples(direct);
	std::vector<std::string> seat_order;
	seat_order.push_back("商务座");
	seat_order.push_back("一等座");
	seat_order.push_back("二等座");
	std::vector<DirectGroup> direct_groups;
	std::map<std::string, size_t> direct_index;
	for (int i = 0; i < drows; ++i) {
		std::string key = std::string(PQgetvalue(direct, i, 0)) + "|"
						 + PQgetvalue(direct, i, 1) + "|"
						 + PQgetvalue(direct, i, 3) + "|"
						 + PQgetvalue(direct, i, 6) + "|"
						 + PQgetvalue(direct, i, 7);
		std::map<std::string, size_t>::iterator it = direct_index.find(key);
		size_t idx;
		if (it == direct_index.end()) {
			DirectGroup g;
			g.train_id = PQgetvalue(direct, i, 0);
			g.from_sid = PQgetvalue(direct, i, 1);
			g.from_name = PQgetvalue(direct, i, 2);
			g.to_sid = PQgetvalue(direct, i, 3);
			g.to_name = PQgetvalue(direct, i, 4);
			g.depart = PQgetvalue(direct, i, 6);
			g.arrive = PQgetvalue(direct, i, 7);
			direct_groups.push_back(g);
			idx = direct_groups.size() - 1;
			direct_index[key] = idx;
		} else {
			idx = it->second;
		}
		DirectSeatRow seat;
		seat.seat_type = PQgetvalue(direct, i, 5);
		seat.price = PQgetvalue(direct, i, 8);
		seat.left = PQgetvalue(direct, i, 9);
		direct_groups[idx].seat_map[seat.seat_type] = seat;
	}

	for (size_t gi = 0; gi < direct_groups.size(); ++gi) {
		DirectGroup &g = direct_groups[gi];
		for (size_t si = 0; si < seat_order.size(); ++si) {
			DirectSeatRow seat;
			seat.seat_type = seat_order[si];
			seat.price = "-";
			seat.left = "0";
			std::map<std::string, DirectSeatRow>::iterator it = g.seat_map.find(seat_order[si]);
			if (it != g.seat_map.end()) {
				seat = it->second;
			}
			std::cout << "<tr>";
			if (si == 0) {
				std::cout << "<td rowspan=\"3\">" << m12306::html_escape(g.train_id) << "</td>"
						  << "<td rowspan=\"3\">" << m12306::html_escape(g.from_name) << "</td>"
						  << "<td rowspan=\"3\">" << m12306::html_escape(g.to_name) << "</td>"
						  << "<td rowspan=\"3\">" << m12306::html_escape(g.depart) << "</td>"
						  << "<td rowspan=\"3\">" << m12306::html_escape(g.arrive) << "</td>";
			}
			std::cout << "<td>" << m12306::html_escape(seat.seat_type) << "</td>"
					  << "<td>" << m12306::html_escape(seat.price) << "</td>"
					  << "<td>" << m12306::html_escape(seat.left) << "</td>";
			if (seat.price != "-" && std::atoi(seat.left.c_str()) > 0) {
				std::cout << "<td><a href=\"/cgi-bin/book.cgi?username=" << m12306::html_escape(username)
						  << "&train_id=" << m12306::html_escape(g.train_id)
						  << "&from_sid=" << m12306::html_escape(g.from_sid)
						  << "&to_sid=" << m12306::html_escape(g.to_sid)
						  << "&date=" << m12306::html_escape(date)
						  << "&seat_type=" << m12306::html_escape(seat.seat_type) << "\">购票</a></td>";
			} else {
				std::cout << "<td>-</td>";
			}
			std::cout << "</tr>";
		}
	}
	if (direct_groups.empty()) std::cout << "<tr><td colspan=\"9\">暂无直达车次。</td></tr>";
	std::cout << "</tbody></table>";
	PQclear(direct);

	std::vector<TransferGroup> transfer_groups;
	std::string transfer_error;
	if (!load_transfer_groups(conn, from_city, to_city, date, time, from_station_id_filter, to_station_id_filter, transfer_groups, transfer_error)) {
		std::cout << "<p class=\"err\">Transfer query failed: "
				  << m12306::html_escape(transfer_error) << "</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}
	render_transfer_panel(transfer_groups, username, date, transfer_page, false);

	std::cout << "<p><a href=\"/query_route.html?username=" << m12306::html_escape(username)
			  << "&from_city=" << m12306::html_escape(to_city)
			  << "&to_city=" << m12306::html_escape(from_city)
			  << "&date=2026-05-02&time=00:00\">查询回程</a></p>";

	PQfinish(conn);
	m12306::print_page_end();
	return 0;
}
