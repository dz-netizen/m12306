#include <string>

#include "m12306_common.h"

static bool inc_inventory(PGconn *conn,
	const char *train_id,
	const char *date,
	const char *seat_type,
	const char *from_sid,
	const char *to_sid) {
	const char *sql =
		"WITH req AS ("
		"  SELECT rf.station_order AS req_from_order, rt.station_order AS req_to_order "
		"  FROM train_station rf "
		"  JOIN train_station rt ON rt.train_id=rf.train_id "
		"  WHERE rf.train_id=$1 AND rf.station_id=$4::int AND rt.station_id=$5::int) "
		"UPDATE seat_inventory si "
		"SET remaining=remaining+1 "
		"FROM train_station ts_from, train_station ts_to, req r "
		"WHERE si.train_id=$1 AND si.travel_date=$2::date AND si.seat_type=$3 "
		"  AND ts_from.train_id=si.train_id AND ts_from.station_id=si.from_station "
		"  AND ts_to.train_id=si.train_id AND ts_to.station_id=si.to_station "
		"  AND r.req_from_order < r.req_to_order "
		"  AND ts_from.station_order < r.req_to_order "
		"  AND r.req_from_order < ts_to.station_order";
	const char *params[5] = {train_id, date, seat_type, from_sid, to_sid};
	PGresult *res = PQexecParams(conn, sql, 5, NULL, params, NULL, NULL, 0);
	bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK && std::atoi(PQcmdTuples(res)) > 0);
	PQclear(res);
	return ok;
}

int main() {
	cgicc::Cgicc form;
	std::string username = m12306::get_form_value(form, "username");
	std::string date_from = m12306::get_form_value(form, "date_from");
	std::string date_to = m12306::get_form_value(form, "date_to");
	std::string action = m12306::get_form_value(form, "action");
	std::string order_id = m12306::get_form_value(form, "order_id");
	if (date_from.empty()) date_from = "2026-01-01";
	if (date_to.empty()) date_to = "2026-12-31";

	m12306::print_page_begin("订单管理");
	std::cout << "<p><a href=\"/cgi-bin/home.cgi?username=" << m12306::html_escape(username)
			  << "\" class=\"back-link\">← \u8fd4\u56de\u9996\u9875</a></p>";

	if (username.empty()) {
		std::cout << "<div class=\"message err\">用户名\u662f\u5fc5\u586b\u9879\u3002</div>";
		m12306::print_page_end();
		return 0;
	}

	PGconn *conn = m12306::connect_db();
	if (PQstatus(conn) != CONNECTION_OK) {
		std::cout << "<div class=\"message err\">数\u636e\u5e93\u8fde\u63a5\u5931\u8d25\u3002</div>";
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}
	int uid = m12306::get_user_id(conn, username);
	if (uid < 0) {
		std::cout << "<div class=\"message err\">用\u6237\u4e0d\u5b58\u5728\u3002</div>";
		PQfinish(conn);
		m12306::print_page_end();
		return 0;
	}

	std::string uid_s = std::to_string(uid);

	if (action == "cancel" && !order_id.empty()) {
		m12306::exec_ok(conn, "BEGIN");
		// 锁定订单行，确认这笔订单属于当前用户且仍然是正常状态。
		const char *lock_sql = "SELECT status FROM orders WHERE order_id=$1::int AND user_id=$2::int FOR UPDATE";
		const char *lp[2] = {order_id.c_str(), uid_s.c_str()};
		PGresult *lk = PQexecParams(conn, lock_sql, 2, NULL, lp, NULL, NULL, 0);
		bool can_cancel = (PQresultStatus(lk) == PGRES_TUPLES_OK && PQntuples(lk) > 0
						&& std::string(PQgetvalue(lk, 0, 0)) == "正常");
		PQclear(lk);
		if (can_cancel) {
			// 取出订单明细，逐段把与该行程重叠的库存区间回滚。
			const char *item_sql = "SELECT train_id, seat_type, from_station, to_station, travel_date::text FROM order_item WHERE order_id=$1::int";
			const char *ip[1] = {order_id.c_str()};
			PGresult *it = PQexecParams(conn, item_sql, 1, NULL, ip, NULL, NULL, 0);
			bool restored = (PQresultStatus(it) == PGRES_TUPLES_OK);
			if (restored) {
				for (int i = 0; i < PQntuples(it); ++i) {
					if (!inc_inventory(conn,
						PQgetvalue(it, i, 0),
						PQgetvalue(it, i, 4),
						PQgetvalue(it, i, 1),
						PQgetvalue(it, i, 2),
						PQgetvalue(it, i, 3))) {
						restored = false;
						break;
					}
				}
			}
			PQclear(it);
			if (!restored) {
				m12306::exec_ok(conn, "ROLLBACK");
				std::cout << "<div class=\"message err\">订单取消失败。</div>";
			} else {
				PGresult *oc = PQexecParams(conn,
					"UPDATE orders SET status='取消' WHERE order_id=$1::int AND user_id=$2::int",
					2, NULL, lp, NULL, NULL, 0);
				bool status_updated = (PQresultStatus(oc) == PGRES_COMMAND_OK);
				PQclear(oc);
				if (status_updated) {
					m12306::exec_ok(conn, "COMMIT");
					std::cout << "<div class=\"message ok\">订单已取消: " << m12306::html_escape(order_id) << "</div>";
				} else {
					m12306::exec_ok(conn, "ROLLBACK");
					std::cout << "<div class=\"message err\">订单取消失败。</div>";
				}
			}
		} else {
			m12306::exec_ok(conn, "ROLLBACK");
			std::cout << "<div class=\"message err\">订\u5355\u65e0\u6cd5\u53d6\u6d88\u3002</div>";
		}
	}


	std::cout << "<form method=\"get\" action=\"/cgi-bin/orders.cgi\" class=\"filter-form\">"
			  << "<input type=\"hidden\" name=\"username\" value=\"" << m12306::html_escape(username) << "\">"
			  << "<label>起始日期 <input type=\"date\" name=\"date_from\" value=\"" << m12306::html_escape(date_from) << "\"></label>"
			  << "<label>结束日期 <input type=\"date\" name=\"date_to\" value=\"" << m12306::html_escape(date_to) << "\"></label>"
			  << "<button type=\"submit\">筛选</button></form>";

	// 列出用户订单，并把首末程、列车号、起止站和时间拼成一行摘要。
const char *list_sql =
    // =========================
    // 主查询：订单基础信息
    // =========================
    "SELECT o.order_id::text, o.create_time::date::text, "
    "       COALESCE(train_info.train_ids,'-') AS train_ids, "   // 拼接整单经过的车次链
    "       COALESCE(seat_info.seat_types,'-') AS seat_types, "   // 拼接整单席别链
    "       COALESCE(sf.station_name,'-') AS from_station, "      // 起点站名称
    "       COALESCE(st.station_name,'-') AS to_station, "        // 终点站名称
    "       COALESCE(tsf.departure_time::text,'-') AS depart_time, " // 首段出发时间
    "       COALESCE(tst.arrival_time::text,'-') AS arrive_time, "   // 末段到达时间
    "       o.total_price::text, o.status "

    "FROM orders o "

    // 聚合订单内所有车次（用于展示路径）
    "LEFT JOIN LATERAL ("
    "  SELECT string_agg(oi.train_id, '->' ORDER BY oi.id) AS train_ids "
    "  FROM order_item oi WHERE oi.order_id=o.order_id"
    ") train_info ON TRUE "

    //聚合席别信息（与车次一一对应）
    "LEFT JOIN LATERAL ("
    "  SELECT string_agg(oi.seat_type, '->' ORDER BY oi.id) AS seat_types "
    "  FROM order_item oi WHERE oi.order_id=o.order_id"
    ") seat_info ON TRUE "


    // 找订单第一段的起点站（用于展示 from_statio
    "LEFT JOIN LATERAL ("
    "  SELECT oi.from_station "
    "  FROM order_item oi WHERE oi.order_id=o.order_id "
    "  ORDER BY oi.id ASC LIMIT 1"
    ") first_leg ON TRUE "

    // 找订单最后一段的终点站（用于展示 to_station）

    "LEFT JOIN LATERAL ("
    "  SELECT oi.to_station "
    "  FROM order_item oi WHERE oi.order_id=o.order_id "
    "  ORDER BY oi.id DESC LIMIT 1"
    ") last_leg ON TRUE "


    // 首段车次 + 起点站（用于查 departure_time）
    "LEFT JOIN LATERAL ("
    "  SELECT oi.train_id, oi.from_station "
    "  FROM order_item oi WHERE oi.order_id=o.order_id "
    "  ORDER BY oi.id ASC LIMIT 1"
    ") first_seg ON TRUE "

    // 末段车次 + 终点站（用于查 arrival_time）
    "LEFT JOIN LATERAL ("
    "  SELECT oi.train_id, oi.to_station "
    "  FROM order_item oi WHERE oi.order_id=o.order_id "
    "  ORDER BY oi.id DESC LIMIT 1"
    ") last_seg ON TRUE "

    // 站点维表：把 station_id -> station_name
    "LEFT JOIN station sf ON sf.station_id=first_leg.from_station "
    "LEFT JOIN station st ON st.station_id=last_leg.to_station "

    // 8. 车站时刻表：拿首段出发时间 & 末段到达时间
    "LEFT JOIN train_station tsf "
    "  ON tsf.train_id=first_seg.train_id "
    " AND tsf.station_id=first_seg.from_station "

    "LEFT JOIN train_station tst "
    "  ON tst.train_id=last_seg.train_id "
    " AND tst.station_id=last_seg.to_station "

    // 过滤条件：用户 + 时间范围
    "WHERE o.user_id=$1::int "
    "  AND o.create_time::date BETWEEN $2::date AND $3::date "
    // 最近订单优先
    "ORDER BY o.order_id DESC";
	const char *lp2[3] = {uid_s.c_str(), date_from.c_str(), date_to.c_str()};
	PGresult *ls = PQexecParams(conn, list_sql, 3, NULL, lp2, NULL, NULL, 0);
	if (PQresultStatus(ls) != PGRES_TUPLES_OK) {
		std::cout << "<div class=\"message err\">订\u5355\u67e5\u8be2\u5931\u8d25\u3002</div>";
		PQclear(ls);
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	std::cout << "<table><tr><th>\u8ba2\u5355\u53f7</th><th>\u65e5\u671f</th><th>\u5217\u8f66\u53f7</th><th>\u5ea7\u4f4d\u7c7b\u7a0b</th><th>\u51fa\u53d1</th><th>\u6b62</th><th>\u8d77\u7a0b</th><th>\u5230\u7a0b</th><th>\u603b\u4ef7</th><th>\u72b6\u6001</th><th>\u64cd\u4f5c</th></tr>";
	for (int i = 0; i < PQntuples(ls); ++i) {
		std::string oid = PQgetvalue(ls, i, 0);
		std::string st = PQgetvalue(ls, i, 9);
		std::cout << "<tr><td>" << m12306::html_escape(oid) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(ls, i, 1)) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(ls, i, 2)) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(ls, i, 3)) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(ls, i, 4)) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(ls, i, 5)) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(ls, i, 6)) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(ls, i, 7)) << "</td>"
				  << "<td>" << m12306::html_escape(PQgetvalue(ls, i, 8)) << "</td>"
				  << "<td>" << m12306::html_escape(st) << "</td><td>";
		std::cout << "<a href=\"/cgi-bin/orders.cgi?username=" << m12306::html_escape(username)
				  << "&action=detail&order_id=" << m12306::html_escape(oid)
				  << "&date_from=" << m12306::html_escape(date_from)
				  << "&date_to=" << m12306::html_escape(date_to)
				  << "\">\u8be6\u60c5</a>";
		if (st == "\u6b63\u5e38") {
			std::cout << " | <a href=\"/cgi-bin/orders.cgi?username=" << m12306::html_escape(username)
					  << "&action=cancel&order_id=" << m12306::html_escape(oid)
					  << "&date_from=" << m12306::html_escape(date_from)
					  << "&date_to=" << m12306::html_escape(date_to)
					  << "\">\u53d6\u6d88</a>";
		}
		std::cout << "</td></tr>";
	}
	if (PQntuples(ls) == 0) std::cout << "<tr><td colspan=\"11\">\u6ca1\u6709\u8ba2\u5355\u3002</td></tr>";
	std::cout << "</table>";

	if (action == "detail" && !order_id.empty()) {
		// 订单详情页需要把每一段的站点、时间和价格展开显示。
		const char *detail_sql =
		// =========================
		// 主查询：订单明细的基础信息
		// =========================
			"SELECT oi.id::text, oi.train_id, sf.station_name, st.station_name, "
			"       oi.seat_type, oi.price::text, oi.travel_date::text, "
			"       COALESCE(ts1.departure_time::text,'-') AS depart_time, "
			"       COALESCE(ts2.arrival_time::text,'-') AS arrive_time "
			"FROM order_item oi "
			"JOIN orders o ON o.order_id=oi.order_id "
			"JOIN station sf ON sf.station_id=oi.from_station "
			"JOIN station st ON st.station_id=oi.to_station "
			"LEFT JOIN train_station ts1 ON ts1.train_id=oi.train_id AND ts1.station_id=oi.from_station "//找到每段的出发时间
			"LEFT JOIN train_station ts2 ON ts2.train_id=oi.train_id AND ts2.station_id=oi.to_station "//找到每段的到达时间
			"WHERE oi.order_id=$1::int AND o.user_id=$2::int "
			"ORDER BY oi.id";
			
		const char *dp[2] = {order_id.c_str(), uid_s.c_str()};
		PGresult *dr = PQexecParams(conn, detail_sql, 2, NULL, dp, NULL, NULL, 0);
		if (PQresultStatus(dr) == PGRES_TUPLES_OK && PQntuples(dr) > 0) {
			std::cout << "<h3>订单详情：" << m12306::html_escape(order_id) << "</h3>";
			std::cout << "<table><tr><th>程段</th><th>列车号</th><th>出发站</th><th>到达站</th><th>乘车日期</th><th>出发时间</th><th>到达时间</th><th>座位类型</th><th>价格</th></tr>";
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
