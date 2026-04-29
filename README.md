# m12306

## 代码仓库

https://github.com/dz-netizen/m12306

---

## 代码概览

```
m12306
├── Makefile              // 编译
├── deploy_cgi.sh         // 部署 CGI 脚本
├── deploy_html.sh        // 部署 HTML 脚本
├── src
│   ├── m12306_common.h   // 头文件
│   ├── admin.cpp         // 管理员功能（需求9）
│   ├── book.cpp          // 订票逻辑（需求7）
│   ├── home.cpp          // 登录后首页
│   ├── login.cpp         // 登录处理
│   ├── orders.cpp        // 订单管理（需求8）
│   ├── query_route.cpp   // 两地查询（需求5/6，最复杂）
│   ├── query_train.cpp   // 单车次查询（需求4）
│   └── register.cpp      // 注册处理（需求3）
├── html
│   ├── m12306index.html  // 登录页面（系统入口）
│   ├── query_route.html  // 两地查询输入页（需求5/6）
│   ├── query_train.html  // 单车次查询输入页（需求4）
│   └── register.html     // 注册输入页（需求3）
├── db
│   ├── sql
│   │   ├── schema.sql       // 创建所有表（User / Train / Station / Orders 等）
│   │   │                    // 定义主键、外键、约束
│   │   └── load_data.sql    // 数据导入脚本
│   ├── data
│   │   ├── raw/train-2026-03 // 原始列车数据集
│   │   └── processed        // 预处理后的 .tbl 文件
│   └── scripts
│       └── generate_import_files.py    // 数据预处理脚本
└── build                 // CGI 编译产物
```

---

## 执行过程

### 1. 启动服务

```bash
sudo service apache2 start
sudo service postgresql start
```

### 2. 建立数据库

```bash
# 创建数据库 m12306（首次运行）
sudo -u postgres createdb m12306
```

### 3. 准备数据

将 train-2026-03 解压缩于 `db/data/raw/`，然后执行预处理脚本：

```bash
python3 db/scripts/generate_import_files.py
```

### 4. 初始化数据库

```bash
psql -d m12306 -f db/sql/schema.sql    # 创建表
psql -d m12306 -f db/sql/load_data.sql # 导入数据
```

### 5. 编译部署

```bash
make                # 编译 CGI
sh deploy_cgi.sh    # 部署 CGI
sh deploy_html.sh   # 部署 HTML
```

### 6. 访问系统

http://localhost:8080/m12306index.html
