#include <string>

#include "m12306_common.h"

int main() {
	cgicc::Cgicc form;
	std::string username = m12306::get_form_value(form, "username");
	std::string view_user = m12306::get_form_value(form, "view_user");

	m12306::print_page_begin("管\u7406\u9762\u677f");
	std::cout << "<p><a href=\"/m12306index.html\" class=\"back-link\">← \u9000\u51fa\u767b\u5f55</a></p>";

	if (!m12306::is_admin(username)) {
		std::cout << "<div class=\"message err\">\u7981\u6b62\u4e0a\u4e0a\uff1a\u4ec5\u7ba1\u7406\u5458\u53ef\u8bbf\u95ee</div>";
		m12306::print_page_end();
		return 0;
	}

	PGconn *conn = m12306::connect_db();
	if (PQstatus(conn) != CONNECTION_OK) {
		std::cout << "<div class=\"message err\">\u6570\u636e\u5e93\u8fde\u63a5\u5931\u8d25\u3002</div>";
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	PGresult *r1 = PQexec(conn, "SELECT COUNT(*)::text FROM orders WHERE status='正常'");
	PGresult *r2 = PQexec(conn, "SELECT COALESCE(SUM(total_price),0)::numeric::text FROM orders WHERE status='正常'");
	std::string total_orders = (PQresultStatus(r1) == PGRES_TUPLES_OK) ? PQgetvalue(r1, 0, 0) : "0";
	std::string total_price = (PQresultStatus(r2) == PGRES_TUPLES_OK) ? PQgetvalue(r2, 0, 0) : "0.00";
	PQclear(r1);
	PQclear(r2);

	std::cout << "<div class=\"admin-stats\">"
			  << "<div class=\"stat\"><div class=\"stat-label\">有效订单总数</div><div class=\"stat-value\">" << m12306::html_escape(total_orders) << "</div></div>"
			  << "<div class=\"stat\"><div class=\"stat-label\">有效收入总计</div><div class=\"stat-value\">¥" << m12306::html_escape(total_price) << "</div></div>"
			  << "</div>";

	// 计算出每个车次的有效订单数，并按订单数排序取前10。
	PGresult *top = PQexec(conn,
		"SELECT t.train_id, COALESCE(h.cnt, 0)::text AS cnt "
		"FROM train t "
		"LEFT JOIN ("
		"  SELECT oi.train_id, COUNT(*)::int AS cnt "//统计每个车次的订单数
		"  FROM order_item oi JOIN orders o ON o.order_id=oi.order_id "
		"  WHERE o.status='正常' "
		"  GROUP BY oi.train_id"
		") h ON h.train_id=t.train_id "
		"ORDER BY COALESCE(h.cnt, 0) DESC, t.train_id ASC "
		"LIMIT 10");
	std::cout << "<h3>线上热门列车 TOP 10</h3><table><tr><th>列车号</th><th>订票数</th></tr>";
	if (PQresultStatus(top) == PGRES_TUPLES_OK) {
		for (int i = 0; i < PQntuples(top); ++i) {
			std::cout << "<tr><td>" << m12306::html_escape(PQgetvalue(top, i, 0))
					  << "</td><td>" << m12306::html_escape(PQgetvalue(top, i, 1)) << "</td></tr>";
		}
	}
	std::cout << "</table>";
	PQclear(top);

	PGresult *users = PQexec(conn, "SELECT username, name, phone FROM user_info ORDER BY user_id");
	std::cout << "<h3>\u5df2\u6ce8\u518c\u7528\u6237</h3><table><tr><th>\u7528\u6237\u540d</th><th>\u771f\u5b9e\u59d3\u540d</th><th>\u624b\u673a\u53f7</th><th>\u8ba2\u5355</th></tr>";
	if (PQresultStatus(users) == PGRES_TUPLES_OK) {
		for (int i = 0; i < PQntuples(users); ++i) {
			std::string u = PQgetvalue(users, i, 0);
			std::cout << "<tr><td>" << m12306::html_escape(u)
					  << "</td><td>" << m12306::html_escape(PQgetvalue(users, i, 1))
					  << "</td><td>" << m12306::html_escape(PQgetvalue(users, i, 2))
					  << "</td><td><a href=\"/cgi-bin/admin.cgi?username=admin&view_user="
					  << m12306::html_escape(u) << "\">\u67e5\u770b</a></td></tr>";
		}
	}
	std::cout << "</table>";
	PQclear(users);

	if (!view_user.empty()) {
		std::cout << "<h3>" << m12306::html_escape(view_user) << " \u7684\u8ba2\u5355\u8be6\u60c5</h3>";
const char *sql =
// =========================
// 主查询：订单列表的基础信息
// =========================
    "SELECT o.order_id::text, o.status, o.total_price::text, o.create_time::date::text, "
    "       COALESCE(train_info.train_ids,'-') AS train_ids, "
    "       COALESCE(seat_info.seat_types,'-') AS seat_types, "
    "       COALESCE(tsf.departure_time::text,'-') AS depart_time, "
    "       COALESCE(tst.arrival_time::text,'-') AS arrive_time "

    "FROM orders o "

	// 通过 user_id 关联用户（按用户名查订单）
    "JOIN user_info u ON u.user_id=o.user_id "

	// 聚合所有 train_id，按顺序拼接成一条路径
    "LEFT JOIN LATERAL ("
    "  SELECT string_agg(oi.train_id, '->' ORDER BY oi.id) AS train_ids "
    "  FROM order_item oi WHERE oi.order_id=o.order_id"
    ") train_info ON TRUE "
    
    // 聚合所有座位类型，按顺序拼接成一条路径
    "LEFT JOIN LATERAL ("
    "  SELECT string_agg(oi.seat_type, '->' ORDER BY oi.id) AS seat_types "
    "  FROM order_item oi WHERE oi.order_id=o.order_id"
    ") seat_info ON TRUE "


	// 取第一段行程（用于出发站）
    "LEFT JOIN LATERAL ("
    "  SELECT oi.train_id, oi.from_station "
    "  FROM order_item oi WHERE oi.order_id=o.order_id ORDER BY oi.id ASC LIMIT 1"
    ") first_seg ON TRUE "
    
	// 取最后一段行程（用于到达站）
    "LEFT JOIN LATERAL ("
    "  SELECT oi.train_id, oi.to_station "
    "  FROM order_item oi WHERE oi.order_id=o.order_id ORDER BY oi.id DESC LIMIT 1"
    ") last_seg ON TRUE "
    
	// 查第一段出发时间
    "LEFT JOIN train_station tsf "
    "ON tsf.train_id=first_seg.train_id AND tsf.station_id=first_seg.from_station "
    
	// 查最后一段到达时间
    "LEFT JOIN train_station tst "
    "ON tst.train_id=last_seg.train_id AND tst.station_id=last_seg.to_station "
    
	// 过滤条件：按用户名查订单
    "WHERE u.username=$1 "
    
	// 按订单ID倒序（最新订单在前）
    "ORDER BY o.order_id DESC";


		const char *p[1] = {view_user.c_str()};
		PGresult *vo = PQexecParams(conn, sql, 1, NULL, p, NULL, NULL, 0);
		std::cout << "<table><tr><th>Order ID</th><th>Status</th><th>Total</th><th>Date</th><th>Train ID</th><th>Seat Type</th><th>Depart</th><th>Arrive</th></tr>";
		if (PQresultStatus(vo) == PGRES_TUPLES_OK) {
			for (int i = 0; i < PQntuples(vo); ++i) {
				std::cout << "<tr><td>" << m12306::html_escape(PQgetvalue(vo, i, 0))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(vo, i, 1))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(vo, i, 2))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(vo, i, 3))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(vo, i, 4))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(vo, i, 5))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(vo, i, 6))
						  << "</td><td>" << m12306::html_escape(PQgetvalue(vo, i, 7))
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
