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

static bool lookup_city_id(PGconn *conn,
				   const std::string &city_name,
				   int &city_id,
				   std::string &error_message) {
	// 先把城市名换成 city_id，后续站点和票价查询都能直接用整数过滤。
	const char *city_sql =
		"SELECT city_id "
		"FROM city "
		"WHERE city_name=$1 "
		"LIMIT 1";
	const char *params[1] = {city_name.c_str()};
	PGresult *res = PQexecParams(conn, city_sql, 1, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		error_message = PQresultErrorMessage(res);
		PQclear(res);
		return false;
	}
	if (PQntuples(res) == 0) {
		error_message = "Unknown city: " + city_name;
		PQclear(res);
		return false;
	}
	city_id = std::atoi(PQgetvalue(res, 0, 0));
	PQclear(res);
	return true;
}

static void render_transfer_panel(const std::vector<TransferGroup> &transfer_groups,
							  const std::string &username,
							  const std::string &date,
							  int transfer_page,
							  bool partial_response) {
	// 中转结果单独分页，partial_response 只返回面板片段给前端异步替换。
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

// 中转候选在 SQL 里一次性算出，再在 C++ 里整理成可展示的分组结构。
static bool load_transfer_groups(PGconn *conn,
						 int from_city_id,
						 int to_city_id,
							 const std::string &date,
							 const std::string &time,
							 int from_station_id_filter,
							 int to_station_id_filter,
							 std::vector<TransferGroup> &transfer_groups,
							 std::string &error_message) {
	// 按出发城市、到达城市和时间窗口筛选可用的中转组合。

	  // 这段 SQL 先分别找出第一程和第二程，再按中转城市、席别和时间窗组合成候选换乘方案。
  const char *transfer_sql =
      /*
       * first_leg:
       * 先筛选第一程：出发城市 -> 换乘城市。
       */
      "WITH first_leg AS ("
      "  SELECT "
      "    tp.train_id, "
      "    tp.from_station, "
      "    tp.to_station, "
      "    tp.seat_type, "
      "    tp.price, "
      "    from_station.station_name AS from_station_name, "
      "    transfer_station.station_name AS transfer_station_name, "
      "    transfer_city.city_name AS transfer_city_name, "
      "    from_stop.departure_time, "
      "    transfer_arrive_stop.arrival_time, "
      "    from_stop.station_order AS from_order, "
      "    transfer_arrive_stop.station_order AS to_order "

      "  FROM ticket_price tp "

	"  JOIN station from_station ON from_station.station_id=tp.from_station "//出发站ID关联车站表，拿到车站名称和所属城市ID
      "  JOIN station transfer_station ON transfer_station.station_id=tp.to_station "
      "  JOIN city transfer_city ON transfer_city.city_id=transfer_station.city_id "

      "  JOIN train_station from_stop "//关联发车站的车次时刻表记录，拿到发车时间和站序
      "    ON from_stop.train_id=tp.train_id "
      "   AND from_stop.station_id=tp.from_station "

      "  JOIN train_station transfer_arrive_stop "//关联换乘站的车次时刻表记录，拿到到达时间和站序
      "    ON transfer_arrive_stop.train_id=tp.train_id "
      "   AND transfer_arrive_stop.station_id=tp.to_station "

		// 筛选条件：出发城市、换乘城市、时间窗口、可选的出发站 
	"  WHERE from_station.city_id=$1::int "
	"    AND transfer_station.city_id<>$1::int "
	"    AND transfer_station.city_id<>$2::int "
      "    AND from_stop.departure_time >= $4::time "
      "    AND ($5::int=0 OR tp.from_station=$5::int) "
      "    AND transfer_arrive_stop.station_order > from_stop.station_order"
      "), "

      /*
       * second_leg:
       * 再筛选第二程：换乘城市 -> 到达城市。
       */
      "second_leg AS ("
      "  SELECT "
      "    tp.train_id, "
      "    tp.from_station, "
      "    tp.to_station, "
      "    tp.seat_type, "
      "    tp.price, "
      "    transfer_station.station_name AS transfer_station_name, "
      "    to_station.station_name AS to_station_name, "
      "    transfer_city.city_name AS transfer_city_name, "
      "    transfer_depart_stop.departure_time, "
      "    to_stop.arrival_time, "
      "    transfer_depart_stop.station_order AS from_order, "
      "    to_stop.station_order AS to_order "

      "  FROM ticket_price tp "

	"  JOIN station transfer_station ON transfer_station.station_id=tp.from_station "//换乘站ID关联车站表，拿到车站名称和所属城市ID
	"  JOIN city transfer_city ON transfer_city.city_id=transfer_station.city_id "
	"  JOIN station to_station ON to_station.station_id=tp.to_station "

      "  JOIN train_station transfer_depart_stop "//关联换乘站的车次时刻表记录，拿到发车时间和站序
      "    ON transfer_depart_stop.train_id=tp.train_id "
      "   AND transfer_depart_stop.station_id=tp.from_station "

      "  JOIN train_station to_stop "//关联到达站的车次时刻表记录，拿到到达时间和站序
      "    ON to_stop.train_id=tp.train_id "
      "   AND to_stop.station_id=tp.to_station "

	// 筛选条件：到达城市、换乘城市、可选的到达站、换乘站必须在第一程的到达站之后
	"  WHERE to_station.city_id=$2::int "
	"    AND transfer_station.city_id<>$1::int "
	"    AND transfer_station.city_id<>$2::int "
      "    AND ($6::int=0 OR tp.to_station=$6::int) "
      "    AND to_stop.station_order > transfer_depart_stop.station_order"
      "), "

      /*
       * transfer_candidates:
       * 组合第一程和第二程。
       * 要求：
       * 1. 两程席别一致
       * 2. 两程不是同一车次
       * 3. 换乘城市一致
       * 4. 同站换乘至少 1 小时，不同站换乘至少 2 小时
       * 5. 最长换乘时间不超过 4 小时
       */
      "transfer_candidates AS ("
      "  SELECT "
      "    first_leg.*, "
      "    second_leg.train_id AS second_train_id, "
      "    second_leg.from_station AS second_from_station, "
      "    second_leg.transfer_station_name AS second_from_station_name, "
      "    second_leg.to_station AS second_to_station, "
      "    second_leg.to_station_name, "
      "    second_leg.departure_time AS second_departure_time, "
      "    second_leg.arrival_time AS second_arrival_time, "
      "    second_leg.price AS second_price, "
      "    (first_leg.price + second_leg.price) AS total_price, "
      "    CASE "
      "      WHEN second_leg.departure_time >= first_leg.arrival_time "
      "      THEN second_leg.departure_time - first_leg.arrival_time "
      "      ELSE second_leg.departure_time + interval '24 hour' - first_leg.arrival_time "
      "    END AS transfer_duration "

      "  FROM first_leg "

      "  JOIN second_leg "//关联第二程，拿到第二程的车次、站点、时间和价格
      "    ON second_leg.seat_type=first_leg.seat_type "
      "   AND second_leg.train_id<>first_leg.train_id "
      "   AND second_leg.transfer_city_name=first_leg.transfer_city_name "
      "), "

      /*
       * available_candidates:
       * 关联两段库存。
       * 没有库存记录时，默认余票为 5。
       */
      "available_candidates AS ("
      "  SELECT "
      "    candidate.*, "
      "    LEAST("
      "      COALESCE(first_inventory.remaining, 5), "
      "      COALESCE(second_inventory.remaining, 5)"
      "    ) AS left_total "

      "  FROM transfer_candidates candidate "

		// 关联第一程库存记录，拿到第一程的余票
      "  LEFT JOIN seat_inventory first_inventory "
      "    ON first_inventory.train_id=candidate.train_id "
      "   AND first_inventory.travel_date=$3::date "
      "   AND first_inventory.seat_type=candidate.seat_type "
      "   AND first_inventory.from_station=candidate.from_station "
      "   AND first_inventory.to_station=candidate.to_station "

	  // 关联第二程库存记录，拿到第二程的余票
      "  LEFT JOIN seat_inventory second_inventory "
      "    ON second_inventory.train_id=candidate.second_train_id "
      "   AND second_inventory.travel_date=$3::date "
      "   AND second_inventory.seat_type=candidate.seat_type "
      "   AND second_inventory.from_station=candidate.second_from_station "
      "   AND second_inventory.to_station=candidate.second_to_station "

	  // 筛选条件：满足换乘时间要求，并且两段的最小余票数大于 0。
      "  WHERE candidate.transfer_duration BETWEEN interval '1 hour' AND interval '4 hour' "
      "    AND ("
      "      candidate.to_station=candidate.second_from_station "
      "      OR candidate.transfer_duration >= interval '2 hour'"
      "    ) "
      "    AND LEAST("
      "      COALESCE(first_inventory.remaining, 5), "
      "      COALESCE(second_inventory.remaining, 5)"
      "    ) > 0"
      "), "

      /*
       * ranked_candidates:
       * 同一组 车次1 + 车次2 + 席别 只保留总价最低的方案。
       */
      "ranked_candidates AS ("
      "  SELECT "
      "    available_candidates.*, "
      "    ROW_NUMBER() OVER ("
      "      PARTITION BY train_id, second_train_id, seat_type "
      "      ORDER BY total_price ASC"
      "    ) AS price_rank "
      "  FROM available_candidates"
      ") "

      "SELECT "
      "  train_id, "
      "  from_station::text, "
      "  from_station_name, "
      "  to_station::text, "
      "  transfer_station_name, "
      "  transfer_city_name, "
      "  second_train_id, "
      "  second_from_station::text, "
      "  second_from_station_name, "
      "  second_to_station::text, "
      "  to_station_name, "
      "  seat_type, "
      "  departure_time::text, "
      "  arrival_time::text, "
      "  second_departure_time::text, "
      "  second_arrival_time::text, "
      "  total_price::text, "
      "  left_total::text, "
      "  departure_time AS departure_time_val, "
      "  price_rank "

      "FROM ranked_candidates "
      "WHERE price_rank=1 "
      "ORDER BY departure_time_val ASC";


	std::string from_station_id_s = std::to_string(from_station_id_filter);
	std::string to_station_id_s = std::to_string(to_station_id_filter);
	std::string from_city_id_s = std::to_string(from_city_id);
	std::string to_city_id_s = std::to_string(to_city_id);
	const char *params[6] = {from_city_id_s.c_str(), to_city_id_s.c_str(), date.c_str(), time.c_str(), from_station_id_s.c_str(), to_station_id_s.c_str()};
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

	// 城市参数缺失或日期非法时，直接返回错误页或错误片段。
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

	// 先渲染直达结果，再渲染中转结果，保持页面结构清晰。
	PGconn *conn = m12306::connect_db();
	if (PQstatus(conn) != CONNECTION_OK) {
		if (!partial_response) m12306::print_page_begin("查询线路");
		std::cout << "<p class=\"err\">DB connection failed: "
				  << m12306::html_escape(PQerrorMessage(conn)) << "</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	int from_city_id = 0;
	int to_city_id = 0;
	std::string city_error;
	if (!lookup_city_id(conn, from_city, from_city_id, city_error) || !lookup_city_id(conn, to_city, to_city_id, city_error)) {
		if (!partial_response) m12306::print_page_begin("查询线路");
		std::cout << "<p class=\"err\">" << m12306::html_escape(city_error) << "</p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 0;
	}

	// 加载并渲染直达车次结果。
	const char *city_station_sql =
		"SELECT s.station_name "
		"FROM station s "
		"WHERE s.city_id=$1::int "
		"ORDER BY s.station_name";

	std::string from_city_id_s = std::to_string(from_city_id);
	std::string to_city_id_s = std::to_string(to_city_id);
	const char *from_city_param[1] = {from_city_id_s.c_str()};
	const char *to_city_param[1] = {to_city_id_s.c_str()};
	// 读取出发城市下的全部车站名称，填充筛选下拉框。
	PGresult *from_station_res = PQexecParams(conn, city_station_sql, 1, NULL, from_city_param, NULL, NULL, 0);
	// 读取到达城市下的全部车站名称，填充筛选下拉框。
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

		// 将出发站名称解析成 station_id，用于后续精确过滤。
		const char *sid_sql =
			"SELECT s.station_id::text "
			"FROM station s "
			"WHERE s.city_id=$1::int AND s.station_name=$2 "
			"LIMIT 1";
		
			const char *sid_params[2] = {from_city_id_s.c_str(), from_station.c_str()};
		PGresult *sid_res = PQexecParams(conn, sid_sql, 2, NULL, sid_params, NULL, NULL, 0);
		if (PQresultStatus(sid_res) != PGRES_TUPLES_OK || PQntuples(sid_res) == 0) {
			std::cout << "<p class=\"err\">Invalid from_station for selected city.</p>";
			PQclear(sid_res);
			PQfinish(conn);
			m12306::print_page_end();
			return 0;
		}
		from_station_id_filter = std::atoi(PQgetvalue(sid_res, 0, 0));
		// 直达车次按车次和时间聚合后，再按席别补全余票和购票入口。
		PQclear(sid_res);
	}
	if (!to_station.empty()) {

		// 将到达站名称解析成 station_id，用于后续精确过滤。
		const char *sid_sql =
			"SELECT s.station_id::text "
			"FROM station s "
			"WHERE s.city_id=$1::int AND s.station_name=$2 "
			"LIMIT 1";

		const char *sid_params[2] = {to_city_id_s.c_str(), to_station.c_str()};
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
		if (!load_transfer_groups(conn, from_city_id, to_city_id, date, time, from_station_id_filter, to_station_id_filter, transfer_groups, transfer_error)) {
			std::cout << "<div id=\"transfer-panel\"><p class=\"err\">Transfer query failed: "
				  << m12306::html_escape(transfer_error) << "</p></div>";
			PQfinish(conn);
			return 1;
		}
		render_transfer_panel(transfer_groups, username, date, transfer_page, true);
		PQfinish(conn);
		return 0;
	}


	  // 这段 SQL 直接筛出满足出发城市、到达城市、起始时间和可选站点条件的直达车次。
  const char *direct_sql =
      /*
       * matched_route:
       * 先筛选出满足出发城市、到达城市、发车时间、可选站点条件的直达票价记录。
       */
      "WITH matched_route AS ("
      "  SELECT "
      "    tp.train_id, "
      "    tp.from_station, "
      "    tp.to_station, "
      "    tp.seat_type, "
      "    tp.price, "
      "    from_station.station_name AS from_station_name, "
      "    to_station.station_name AS to_station_name, "
      "    from_stop.departure_time, "
      "    to_stop.arrival_time "

      "  FROM ticket_price tp "

	"  JOIN station from_station "
	"    ON from_station.station_id=tp.from_station "//出发站ID关联车站表，拿到车站名称和所属城市ID

	"  JOIN station to_station "
	"    ON to_station.station_id=tp.to_station "//到达站ID关联车站表，拿到车站名称和所属城市ID

      "  JOIN train_station from_stop "
      "    ON from_stop.train_id=tp.train_id "
      "   AND from_stop.station_id=tp.from_station "//关联发车站的车次时刻表记录，拿到发车时间

      "  JOIN train_station to_stop "
      "    ON to_stop.train_id=tp.train_id "
      "   AND to_stop.station_id=tp.to_station "//关联到达站的车次时刻表记录，拿到到达时间

	"  WHERE from_station.city_id=$1::int "
	"    AND to_station.city_id=$2::int "
	"    AND from_station.city_id<>to_station.city_id "
      "    AND from_stop.departure_time >= $4::time "
      "    AND ($5::int=0 OR tp.from_station=$5::int) "//可选的出发站过滤，如果没有指定站点则不过滤
      "    AND ($6::int=0 OR tp.to_station=$6::int) "//可选的到达站过滤，如果没有指定站点则不过滤
      "    AND to_stop.station_order > from_stop.station_order"//确保到达站在出发站之后
      ") "

      /* =======================================================================
	  主查询：在满足条件的直达区间里，关联库存表拿到余票信息，并只返回有余票的车次。 */
      "SELECT "
      "  route.train_id, "
      "  route.from_station, "
      "  route.from_station_name, "
      "  route.to_station, "
      "  route.to_station_name, "
      "  route.seat_type, "
      "  route.departure_time::text, "
      "  route.arrival_time::text, "
      "  route.price::text, "
      "  COALESCE(inventory.remaining, 5)::text AS left_seat "

      "FROM matched_route route "

      "LEFT JOIN seat_inventory inventory "//关联库存表，拿到余票信息
      "  ON inventory.train_id=route.train_id "
      " AND inventory.travel_date=$3::date "
      " AND inventory.seat_type=route.seat_type "
      " AND inventory.from_station=route.from_station "
      " AND inventory.to_station=route.to_station "

      /* 只返回有余票的车次 */
      "WHERE COALESCE(inventory.remaining, 5) > 0 "//如果没有库存记录，默认有 5 张票

      // 排序：
      "ORDER BY "
      "  route.departure_time ASC, "// 发车时间早的优先
      "  route.price ASC, "			// 价格低的优先
      "  route.seat_type ASC, "		// 席别排序
      "  CASE "						// 行程时间短的优先，跨天到达时补 24 小时
      "    WHEN route.arrival_time >= route.departure_time "
      "    THEN route.arrival_time - route.departure_time "
      "    ELSE route.arrival_time + interval '24 hour' - route.departure_time "
      "  END ASC";


	std::string from_station_id_s = std::to_string(from_station_id_filter);
	std::string to_station_id_s = std::to_string(to_station_id_filter);
	const char *params[6] = {from_city_id_s.c_str(), to_city_id_s.c_str(), date.c_str(), time.c_str(), from_station_id_s.c_str(), to_station_id_s.c_str()};
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
	if (!load_transfer_groups(conn, from_city_id, to_city_id, date, time, from_station_id_filter, to_station_id_filter, transfer_groups, transfer_error)) {
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
