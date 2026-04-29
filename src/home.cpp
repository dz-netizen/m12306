#include <string>

#include "m12306_common.h"

int main() {
	cgicc::Cgicc form;
	std::string username = m12306::get_form_value(form, "username");

	m12306::print_page_begin("首页");

	if (username.empty()) {
		std::cout << "<div class=\"message err\">请先登录</div>";
		std::cout << "<p><a href=\"/m12306index.html\">返回登录</a></p>";
		m12306::print_page_end();
		return 0;
	}

	PGconn *conn = m12306::connect_db();
	if (PQstatus(conn) != CONNECTION_OK) {
		std::cout << "<div class=\"message err\">数据库连接失败: "
				  << m12306::html_escape(PQerrorMessage(conn)) << "</div>";
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	int uid = m12306::get_user_id(conn, username);
	PQfinish(conn);
	if (uid < 0) {
		std::cout << "<div class=\"message err\">用户不存在，请重新登录</div>";
		std::cout << "<p><a href=\"/m12306index.html\">返回登录</a></p>";
		m12306::print_page_end();
		return 0;
	}

	std::cout << "<div class=\"message ok\">欢迎，" << m12306::html_escape(username) << "！</div>";
	std::cout << "<ul class=\"nav-menu\">";
	std::cout << "<li><a href=\"/query_train.html?username=" << m12306::html_escape(username)
			  << "\">🚆 查询列车信息</a></li>";
	std::cout << "<li><a href=\"/query_route.html?username=" << m12306::html_escape(username)
			  << "\">🗺️ 查询线路信息</a></li>";
	std::cout << "<li><a href=\"/cgi-bin/orders.cgi?username=" << m12306::html_escape(username)
			  << "\">📋 管理订单</a></li>";
	if (m12306::is_admin(username)) {
		std::cout << "<li><a href=\"/cgi-bin/admin.cgi?username=admin\">⚙️ 管理面板</a></li>";
	}
	std::cout << "</ul>";
	std::cout << "<div class=\"logout-btn\"><a href=\"/m12306index.html\">退 出 登 录</a></div>";

	m12306::print_page_end();
	return 0;
}
