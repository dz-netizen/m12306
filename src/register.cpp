#include <string>
#include <cctype>

#include "m12306_common.h"

static bool exists_value(PGconn *conn, const char *sql, const std::string &value) {
	const char *params[1] = {value.c_str()};
	PGresult *res = PQexecParams(conn, sql, 1, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		PQclear(res);
		return true;
	}
	bool exists = (std::atoi(PQgetvalue(res, 0, 0)) > 0);
	PQclear(res);
	return exists;
}

static bool is_valid_phone(const std::string &phone) {
	if (phone.size() != 11) return false;
	for (size_t i = 0; i < phone.size(); ++i) {
		if (!std::isdigit(static_cast<unsigned char>(phone[i]))) return false;
	}
	return true;
}

int main() {
	cgicc::Cgicc form;
	std::string username = m12306::get_form_value(form, "username");
	std::string phone = m12306::get_form_value(form, "phone");
	std::string name = m12306::get_form_value(form, "name");
	std::string password = m12306::get_form_value(form, "password");
	if (password.empty()) {
		m12306::print_page_begin("注册");
		std::cout << "<div class=\"message err\">密码不能为空</div>";
		std::cout << "<p><a href=\"/register.html\">返回</a></p>";
		m12306::print_page_end();
		return 0;
	}

	m12306::print_page_begin("注册");

	if (username.empty() || phone.empty() || name.empty()) {
		std::cout << "<div class=\"message err\">用户名/手机号/真实姓名的需要填写</div>";
		std::cout << "<p><a href=\"/register.html\">返回</a></p>";
		m12306::print_page_end();
		return 0;
	}
	if (!is_valid_phone(phone)) {
		std::cout << "<div class=\"message err\">手机号格式错误：哪必须是11位数字</div>";
		std::cout << "<p><a href=\"/register.html\">返回</a></p>";
		m12306::print_page_end();
		return 0;
	}

	if (username.empty() || phone.empty() || name.empty()) {
		std::cout << "<div class=\"message err\">数据库连接失败</div>";
		// This is an error - moving the connection check
	}

	PGconn *conn = m12306::connect_db();
	if (PQstatus(conn) != CONNECTION_OK) {
		std::cout << "<div class=\"message err\">数据库连接失败: "
				  << m12306::html_escape(PQerrorMessage(conn)) << "</div>";
		PQfinish(conn);
		m12306::print_page_end();
		return 1;
	}

	if (exists_value(conn, "SELECT COUNT(*) FROM user_info WHERE username=$1", username)) {
		const char *upd_sql = "UPDATE user_info SET password=$2 WHERE username=$1";
		const char *upd_params[2] = {username.c_str(), password.c_str()};
		PGresult *upd = PQexecParams(conn, upd_sql, 2, NULL, upd_params, NULL, NULL, 0);
		if (PQresultStatus(upd) == PGRES_COMMAND_OK) {
			std::cout << "<div class=\"message ok\">用户已存在，密码已更新。</div>";
			std::cout << "<p><a href=\"/m12306index.html\">前往登录</a></p>";
		} else {
			std::cout << "<div class=\"message err\">密码更新失败: "
					  << m12306::html_escape(PQresultErrorMessage(upd)) << "</div>";
			std::cout << "<p><a href=\"/register.html\">返回</a></p>";
		}
		PQclear(upd);
		PQfinish(conn);
		m12306::print_page_end();
		return 0;
	}

	if (exists_value(conn, "SELECT COUNT(*) FROM user_info WHERE phone=$1", phone)) {
		std::cout << "<div class=\"message err\">手机号已经存在</div>";
		std::cout << "<p><a href=\"/register.html\">返回</a></p>";
		PQfinish(conn);
		m12306::print_page_end();
		return 0;
	}

	const char *sql =
		"INSERT INTO user_info(username, phone, name, password) "
		"VALUES($1,$2,$3,$4)";
	const char *params[4] = {username.c_str(), phone.c_str(), name.c_str(), password.c_str()};
	PGresult *res = PQexecParams(conn, sql, 4, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		std::cout << "<div class=\"message err\">注册失败: "
				  << m12306::html_escape(PQresultErrorMessage(res)) << "</div>";
	} else {
		std::cout << "<div class=\"message ok\">注册成功。请登录</div>";
		std::cout << "<p><a href=\"/m12306index.html\">前往登录</a></p>";
	}

	PQclear(res);
	PQfinish(conn);
	m12306::print_page_end();
	return 0;
}
