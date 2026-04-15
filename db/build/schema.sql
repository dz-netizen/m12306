-- =========================
-- 1. 城市表
-- =========================
DROP TABLE IF EXISTS Order_Item CASCADE;
DROP TABLE IF EXISTS Orders CASCADE;
DROP TABLE IF EXISTS Seat_Inventory CASCADE;
DROP TABLE IF EXISTS Ticket_Price CASCADE;
DROP TABLE IF EXISTS Train_Station CASCADE;
DROP TABLE IF EXISTS Train CASCADE;
DROP TABLE IF EXISTS Station CASCADE;
DROP TABLE IF EXISTS City CASCADE;
DROP TABLE IF EXISTS User_Info CASCADE;

CREATE TABLE City (
    city_id SERIAL PRIMARY KEY,
    city_name VARCHAR(50) NOT NULL UNIQUE
);

-- =========================
-- 2. 车站表
-- =========================
CREATE TABLE Station (
    station_id SERIAL PRIMARY KEY,
    station_name VARCHAR(20) NOT NULL,
    city_id INT NOT NULL REFERENCES City(city_id),
    UNIQUE(station_name, city_id)
);

-- =========================
-- 3. 列车表
-- =========================
CREATE TABLE Train (
    train_id VARCHAR(10) PRIMARY KEY
);

-- =========================
-- 4. 列车经停信息
-- =========================
CREATE TABLE Train_Station (
    id SERIAL PRIMARY KEY,
    train_id VARCHAR(10) REFERENCES Train(train_id) ON DELETE CASCADE,
    station_id INT REFERENCES Station(station_id),

    station_order INT NOT NULL,
    arrival_time TIME,
    departure_time TIME,

    UNIQUE(train_id, station_order),
    UNIQUE(train_id, station_id)
);

-- =========================
-- 5. 票价表（区间票价）
-- =========================
CREATE TABLE Ticket_Price (
    train_id VARCHAR(10),
    from_station INT,
    to_station INT,
    seat_type VARCHAR(20),

    price DECIMAL(10,2) NOT NULL CHECK (price >= 0),

    PRIMARY KEY(train_id, from_station, to_station, seat_type),

    FOREIGN KEY(train_id, from_station)
        REFERENCES Train_Station(train_id, station_id)
        ON DELETE CASCADE,

    FOREIGN KEY(train_id, to_station)
        REFERENCES Train_Station(train_id, station_id)
        ON DELETE CASCADE,

    CHECK (from_station <> to_station)
);

-- =========================
-- 6. 座位库存（关键表）
-- =========================
CREATE TABLE Seat_Inventory (
    train_id VARCHAR(10),
    travel_date DATE,
    seat_type VARCHAR(20),

    from_station INT,
    to_station INT,

    remaining INT NOT NULL CHECK (remaining >= 0),

    PRIMARY KEY(train_id, travel_date, seat_type, from_station, to_station),

    FOREIGN KEY(train_id, from_station)
        REFERENCES Train_Station(train_id, station_id)
        ON DELETE CASCADE,

    FOREIGN KEY(train_id, to_station)
        REFERENCES Train_Station(train_id, station_id)
        ON DELETE CASCADE
);

-- =========================
-- 7. 用户表
-- =========================
CREATE TABLE User_Info (
    user_id SERIAL PRIMARY KEY,
    username VARCHAR(20) NOT NULL UNIQUE,
    phone VARCHAR(11) NOT NULL UNIQUE,
    name VARCHAR(20) NOT NULL,
    password VARCHAR(100) NOT NULL
);

-- =========================
-- 8. 订单表
-- =========================
CREATE TABLE Orders (
    order_id SERIAL PRIMARY KEY,
    user_id INT REFERENCES User_Info(user_id),

    total_price DECIMAL(10,2) NOT NULL CHECK (total_price >= 0),
    status VARCHAR(10) NOT NULL CHECK (status IN ('正常', '取消')),

    create_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- =========================
-- 9. 订单明细（支持换乘）
-- =========================
CREATE TABLE Order_Item (
    id SERIAL PRIMARY KEY,
    order_id INT REFERENCES Orders(order_id) ON DELETE CASCADE,

    train_id VARCHAR(10),
    from_station INT,
    to_station INT,

    seat_type VARCHAR(20),
    price DECIMAL(10,2),

    travel_date DATE,

    FOREIGN KEY(train_id, from_station)
        REFERENCES Train_Station(train_id, station_id),

    FOREIGN KEY(train_id, to_station)
        REFERENCES Train_Station(train_id, station_id)
);

-- =========================
-- 10. 索引优化（建议加）
-- =========================
CREATE INDEX idx_station_city ON Station(city_id);
CREATE INDEX idx_train_station_train ON Train_Station(train_id);
CREATE INDEX idx_inventory_query 
    ON Seat_Inventory(train_id, travel_date, seat_type);

CREATE INDEX idx_orders_user ON Orders(user_id);
CREATE INDEX idx_order_item_order ON Order_Item(order_id);

-- =========================
-- Data Import Next Step
-- =========================
-- After schema creation, run:
--   psql -d <your_db_name> -f db/build/load_data.sql

