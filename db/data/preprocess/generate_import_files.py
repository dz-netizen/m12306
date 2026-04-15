#!/usr/bin/env python3
"""Generate COPY-ready .tbl files from train-2026-03 raw text dataset.

Outputs are written to: db/data/preprocess/output
"""

from __future__ import annotations

import csv
import re
from pathlib import Path
from typing import Dict, List, Optional, Tuple

BASE_DIR = Path(__file__).resolve().parent
RAW_DIR = (BASE_DIR / "../train-2026-03").resolve()
OUT_DIR = (BASE_DIR / "output").resolve()

STATIONS_FILE = RAW_DIR / "stations.txt"
TRAIN_FILE_RE = re.compile(r"^G\d+\.txt$")


def clean_text(s: str) -> str:
    return s.strip().replace("\u3000", " ").strip()


def parse_single_price(token: str) -> Optional[float]:
    t = clean_text(token)
    if t in {"", "-"}:
        return None
    t = t.replace("￥", "").replace("元", "").strip()
    try:
        return float(t)
    except ValueError:
        return None


def parse_fare_triplet(cell: str) -> Dict[str, Optional[float]]:
    # Column example: ￥116/￥185/￥404  -> 二等座/一等座/商务座 (cumulative fare)
    text = clean_text(cell)
    fares: Dict[str, Optional[float]] = {"二等座": None, "一等座": None, "商务座": None}
    if not text or text == "-/-/-":
        return fares

    parts = [clean_text(x) for x in text.split("/")]
    while len(parts) < 3:
        parts.append("-")

    fares["二等座"] = parse_single_price(parts[0])
    fares["一等座"] = parse_single_price(parts[1])
    fares["商务座"] = parse_single_price(parts[2])
    return fares


def parse_time(cell: str) -> Optional[str]:
    text = clean_text(cell)
    if text in {"", "-", "始发站", "终点站"}:
        return None
    m = re.match(r"^(\d{1,2}:\d{2})$", text)
    if not m:
        return None
    hh, mm = text.split(":")
    return f"{int(hh):02d}:{mm}"


def write_tbl(path: Path, header: List[str], rows: List[List[object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f, delimiter="|", lineterminator="\n")
        writer.writerow(header)
        for row in rows:
            writer.writerow(row)


def load_station_mapping() -> Tuple[Dict[str, str], Dict[str, int], Dict[str, int], List[List[object]], List[List[object]]]:
    # station_to_city: station_name -> city_name
    station_to_city: Dict[str, str] = {}

    with STATIONS_FILE.open("r", encoding="utf-8") as f:
        reader = csv.reader(f, skipinitialspace=True)
        header = next(reader, None)
        if header is None:
            raise RuntimeError("stations.txt is empty")

        for row in reader:
            if len(row) < 2:
                continue
            city = clean_text(row[0])
            station = clean_text(row[1])
            if not city or not station:
                continue
            station_to_city[station] = city

    # Build deterministic IDs based on sorted names
    city_names = sorted(set(station_to_city.values()))
    city_id_map = {name: idx + 1 for idx, name in enumerate(city_names)}

    station_names = sorted(station_to_city.keys())
    station_id_map = {name: idx + 1 for idx, name in enumerate(station_names)}

    city_rows = [[city_id_map[name], name] for name in city_names]
    station_rows = [
        [station_id_map[st], st, city_id_map[station_to_city[st]]]
        for st in station_names
    ]

    return station_to_city, city_id_map, station_id_map, city_rows, station_rows


def parse_train_file(path: Path) -> List[Tuple[int, str, Optional[str], Optional[str], Dict[str, Optional[float]]]]:
    rows: List[Tuple[int, str, Optional[str], Optional[str], Dict[str, Optional[float]]]] = []
    with path.open("r", encoding="utf-8") as f:
        reader = csv.reader(f, skipinitialspace=True)
        next(reader, None)  # header
        for raw in reader:
            if len(raw) < 9:
                continue

            order_str = clean_text(raw[0])
            station_name = clean_text(raw[1])
            arr = parse_time(raw[2])
            dep = parse_time(raw[4])
            fares_cum = parse_fare_triplet(raw[8])

            if not order_str.isdigit() or not station_name:
                continue

            rows.append((int(order_str), station_name, arr, dep, fares_cum))

    rows.sort(key=lambda x: x[0])
    # Start station often has '-' fare; treat missing cumulative fare as 0.
    if rows:
        o, s, a, d, fares = rows[0]
        fares = dict(fares)
        for seat in ("二等座", "一等座", "商务座"):
            if fares.get(seat) is None:
                fares[seat] = 0.0
        rows[0] = (o, s, a, d, fares)
    return rows


def main() -> None:
    if not RAW_DIR.exists():
        raise RuntimeError(f"Raw directory not found: {RAW_DIR}")
    if not STATIONS_FILE.exists():
        raise RuntimeError(f"stations mapping not found: {STATIONS_FILE}")

    (
        station_to_city,
        city_id_map,
        station_id_map,
        city_rows,
        station_rows,
    ) = load_station_mapping()

    train_rows: List[List[object]] = []
    train_station_rows: List[List[object]] = []
    ticket_price_rows: List[List[object]] = []

    missing_stations: Dict[str, int] = {}

    train_files = sorted(p for p in RAW_DIR.iterdir() if p.is_file() and TRAIN_FILE_RE.match(p.name))

    for tf in train_files:
        train_id = tf.stem
        parsed = parse_train_file(tf)
        if len(parsed) < 2:
            continue

        train_rows.append([train_id])

        # Build per-train station sequence with existing station ids only.
        seq: List[Tuple[int, int, Optional[str], Optional[str], Dict[str, Optional[float]]]] = []
        seen_station_ids = set()
        for order, st_name, arr, dep, fares in parsed:
            sid = station_id_map.get(st_name)
            if sid is None:
                missing_stations[st_name] = missing_stations.get(st_name, 0) + 1
                continue
            if sid in seen_station_ids:
                # Some train files may contain repeated same station names.
                # Keep the first occurrence to satisfy UNIQUE(train_id, station_id).
                continue
            seen_station_ids.add(sid)
            seq.append((order, sid, arr, dep, fares))
            train_station_rows.append([train_id, sid, order, arr or "", dep or ""])

        if len(seq) < 2:
            continue

        # Interval fares from cumulative fares.
        for i in range(len(seq) - 1):
            _order_i, sid_i, _arr_i, _dep_i, fares_i = seq[i]
            for j in range(i + 1, len(seq)):
                _order_j, sid_j, _arr_j, _dep_j, fares_j = seq[j]
                for seat in ("二等座", "一等座", "商务座"):
                    cum_i = fares_i.get(seat)
                    cum_j = fares_j.get(seat)
                    if cum_i is None or cum_j is None:
                        continue
                    diff = round(float(cum_j) - float(cum_i), 2)
                    if diff < 0:
                        continue
                    ticket_price_rows.append([train_id, sid_i, sid_j, seat, f"{diff:.2f}"])

    # Deduplicate trains, train_station and ticket rows.
    train_rows = [list(x) for x in sorted({tuple(r) for r in train_rows})]
    train_station_rows = [list(x) for x in sorted({tuple(r) for r in train_station_rows}, key=lambda r: (r[0], int(r[2])))]
    ticket_price_rows = [list(x) for x in sorted({tuple(r) for r in ticket_price_rows}, key=lambda r: (r[0], int(r[1]), int(r[2])))]

    write_tbl(OUT_DIR / "city.tbl", ["city_id", "city_name"], city_rows)
    write_tbl(OUT_DIR / "station.tbl", ["station_id", "station_name", "city_id"], station_rows)
    write_tbl(OUT_DIR / "train.tbl", ["train_id"], train_rows)
    write_tbl(
        OUT_DIR / "train_station.tbl",
        ["train_id", "station_id", "station_order", "arrival_time", "departure_time"],
        train_station_rows,
    )
    write_tbl(
        OUT_DIR / "ticket_price.tbl",
        ["train_id", "from_station", "to_station", "seat_type", "price"],
        ticket_price_rows,
    )

    summary = [
        f"raw_dir={RAW_DIR}",
        f"train_files={len(train_files)}",
        f"city_rows={len(city_rows)}",
        f"station_rows={len(station_rows)}",
        f"train_rows={len(train_rows)}",
        f"train_station_rows={len(train_station_rows)}",
        f"ticket_price_rows={len(ticket_price_rows)}",
        f"missing_station_count={len(missing_stations)}",
    ]
    if missing_stations:
        top_missing = sorted(missing_stations.items(), key=lambda kv: kv[1], reverse=True)[:20]
        summary.append("top_missing_stations=" + ", ".join(f"{k}:{v}" for k, v in top_missing))

    (OUT_DIR / "summary.txt").write_text("\n".join(summary) + "\n", encoding="utf-8")
    print("\n".join(summary))


if __name__ == "__main__":
    main()
