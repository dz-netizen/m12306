# m12306

## 代码仓库

https://github.com/dz-netizen/m12306

---

## 代码概览

```cpp
Lab2
├── db
│   ├── build
│   │   ├── load_data.sql   // 数据导入脚本
│   │   └── schema.sql      
│   │       // - 创建所有表（User / Train / Station / Orders 等）
│   │       // - 定义主键、外键、约束
│   │       // - 是整个系统的数据基础（核心）
│   └── data
│       ├── preprocess
|       |   ├── generate_import_files.py    //数据预处理脚本
│       │   └── output  // 数据预处理结果  
│       └── train-2026-03    // 原始列车数据集       
└── setup
    ├── code
    |   ├── m12306_common.h //头文件
    |   ├── Makefile    //编译
    |   ├── run_cgi.sh  //便捷执行脚本
    │   ├── admin.cpp   // 管理员功能（需求9）
    │   ├── book.cpp    // 订票逻辑（需求7）  
    │   ├── home.cpp    // 登录后首页
    │   ├── login.cpp   // 登录处理
    │   ├── orders.cpp  // 订单管理（需求8）
    │   ├── query_route.cpp // 两地查询（需求5/6，最复杂）
    │   ├── query_train.cpp // 单车次查询（需求4）
    │   └── register.cpp    // 注册处理（需求3）
    └── vm-apache
        └── htdocs(html)
            ├── run_html.sh         //便捷执行脚本
            ├── m12306index.html    // 登录页面（系统入口）
            ├── query_route.html    // 两地查询输入页（需求5/6）
            ├── query_train.html    // 单车次查询输入页（需求4）
            └── register.html       // 注册输入页（需求3）
```

---

## schema

---

## 执行过程

- 开启apache2服务
`$sudo service apache2 start`

- 开启postgresql服务
`$sudo service postgresql start`

- 建立数据库m12306


- 将train-2026-03解压缩于`/Lab2/db/data/`

- 执行 `generate_import_files.py`


--- 
- 执行`schema.sql`和`load_data.sql`
`$psql -d <db> -f db/build/schema.sql`
`$psql -d <db> -f db/build/load_data.sql`

注意需要在对应目录下执行该命令，比如示例中为Lab2

---

- cgi
`$ sh run_cgi.sh`

- html
`sh run_html.sh`

- 查看 http://localhost:8080/m12306index.html
