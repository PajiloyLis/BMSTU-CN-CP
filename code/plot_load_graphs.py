#!/usr/bin/env python3
"""
Build load-test plots from load_ramp.csv.

Plots:
1) latency_ms vs concurrency
2) rps vs concurrency
3) transfer_bytes_sec vs concurrency
4) per-connection speed (transfer_bytes_sec / concurrency) vs concurrency

All plots are black-and-white with distinct line styles and markers
for worker counts 4, 8, 16.
"""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path
from typing import Dict, List

import matplotlib.pyplot as plt

plt.rcParams["font.family"] = "DejaVu Sans"

LINE_STYLES = {4: "-", 8: "--", 16: "-."}
MARKERS = {4: "o", 8: "s", 16: "^"}
WORKER_ORDER = [4, 8, 16]


def _to_float(value: str) -> float:
    if value is None:
        return math.nan
    value = value.strip()
    if not value or value.lower() == "nan":
        return math.nan
    try:
        return float(value)
    except ValueError:
        return math.nan


def _to_int(value: str, default: int = -1) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _mean(values: List[float]) -> float:
    valid = [v for v in values if not math.isnan(v)]
    if not valid:
        return math.nan
    return sum(valid) / len(valid)


def load_rows(csv_path: Path, tool_filter: str) -> tuple[Dict[int, List[dict]], Dict[int, List[float]]]:
    grouped: Dict[int, List[dict]] = {w: [] for w in WORKER_ORDER}
    error_points: Dict[int, List[float]] = {w: [] for w in WORKER_ORDER}
    accum: Dict[int, Dict[int, Dict[str, List[float]]]] = {w: {} for w in WORKER_ORDER}

    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            tool = (row.get("tool") or "").strip().lower()
            if tool_filter and tool != tool_filter.lower():
                continue

            workers = _to_int(row.get("workers"), default=-1)
            if workers not in grouped:
                continue

            concurrency = _to_float(row.get("concurrency", ""))
            latency_ms = _to_float(row.get("latency_ms", ""))
            rps = _to_float(row.get("rps", ""))
            transfer_bps = _to_float(row.get("transfer_bytes_sec", ""))

            if math.isnan(concurrency):
                continue

            c_key = int(round(concurrency))
            if c_key not in accum[workers]:
                accum[workers][c_key] = {
                    "latency_ms": [],
                    "rps": [],
                    "transfer_bps": [],
                    "per_conn_bps": [],
                }

            per_conn_bps = (
                transfer_bps / concurrency
                if (not math.isnan(transfer_bps) and concurrency > 0)
                else math.nan
            )
            accum[workers][c_key]["latency_ms"].append(latency_ms)
            accum[workers][c_key]["rps"].append(rps)
            accum[workers][c_key]["transfer_bps"].append(transfer_bps)
            accum[workers][c_key]["per_conn_bps"].append(per_conn_bps)

            stop_reason = (row.get("stop_reason") or "").strip().lower()
            if stop_reason in {"first_socket_error", "first_failed_request"}:
                error_points[workers].append(concurrency)

    for workers in WORKER_ORDER:
        for c_key in sorted(accum[workers].keys()):
            grouped[workers].append(
                {
                    "concurrency": float(c_key),
                    "latency_ms": _mean(accum[workers][c_key]["latency_ms"]),
                    "rps": _mean(accum[workers][c_key]["rps"]),
                    "transfer_bps": _mean(accum[workers][c_key]["transfer_bps"]),
                    "per_conn_bps": _mean(accum[workers][c_key]["per_conn_bps"]),
                }
            )
    return grouped, error_points


def _plot_metric(
    grouped: Dict[int, List[dict]],
    metric_key: str,
    ylabel: str,
    title: str,
    out_path_png: Path,
    out_path_pdf: Path,
    y_scale: float = 1.0,
) -> None:
    plt.figure(figsize=(9, 6))

    for workers in WORKER_ORDER:
        rows = grouped.get(workers, [])
        xs = [r["concurrency"] for r in rows if not math.isnan(r[metric_key])]
        ys = [r[metric_key] / y_scale for r in rows if not math.isnan(r[metric_key])]
        if not xs:
            continue

        plt.plot(
            xs,
            ys,
            color="black",
            linestyle=LINE_STYLES[workers],
            marker=MARKERS[workers],
            markersize=5,
            linewidth=1.6,
            label=f"{workers} {('воркера' if workers == 4 else 'воркеров')}",
        )

    plt.xlabel("Число клиентов")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.grid(True, linestyle=":", linewidth=0.8, color="0.6")
    plt.legend()
    plt.tight_layout()
    # Save with tight bounding box so PDF is cropped by content.
    plt.savefig(out_path_png, dpi=180, bbox_inches="tight", pad_inches=0.05)
    plt.savefig(out_path_pdf, bbox_inches="tight", pad_inches=0.05)
    plt.close()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build load plots from load_ramp.csv"
    )
    parser.add_argument(
        "--csv",
        default="load_ramp.csv",
        help="Path to input CSV (default: load_ramp.csv)",
    )
    parser.add_argument(
        "--out-dir",
        default="plots",
        help="Output directory for generated plots (default: plots)",
    )
    parser.add_argument(
        "--tool",
        default="wrk",
        help="Filter CSV rows by tool value (default: wrk)",
    )
    args = parser.parse_args()

    csv_path = Path(args.csv).resolve()
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    if not csv_path.exists():
        raise SystemExit(f"CSV not found: {csv_path}")

    grouped, error_points = load_rows(csv_path, tool_filter=args.tool)

    _plot_metric(
        grouped=grouped,
        metric_key="latency_ms",
        ylabel="Среднее время обработки запроса, мс",
        title="Зависимость среднего времени обработки запроса от числа клиентов",
        out_path_png=out_dir / "latency_vs_clients.png",
        out_path_pdf=out_dir / "latency_vs_clients.pdf",
    )
    _plot_metric(
        grouped=grouped,
        metric_key="rps",
        ylabel="Запросов в секунду (RPS)",
        title="Зависимость RPS от числа клиентов",
        out_path_png=out_dir / "rps_vs_clients.png",
        out_path_pdf=out_dir / "rps_vs_clients.pdf",
    )
    _plot_metric(
        grouped=grouped,
        metric_key="transfer_bps",
        ylabel="Средняя скорость передачи, МБ/с",
        title="Зависимость средней скорости передачи от числа клиентов",
        out_path_png=out_dir / "transfer_vs_clients.png",
        out_path_pdf=out_dir / "transfer_vs_clients.pdf",
        y_scale=1024.0 * 1024.0,
    )
    _plot_metric(
        grouped=grouped,
        metric_key="per_conn_bps",
        ylabel="Скорость передачи на одно соединение, КБ/с",
        title="Зависимость скорости передачи на одно соединение от числа клиентов",
        out_path_png=out_dir / "per_connection_speed_vs_clients.png",
        out_path_pdf=out_dir / "per_connection_speed_vs_clients.pdf",
        y_scale=1024.0,
    )

    print(f"Plots saved to: {out_dir}")
    print("Среднее число клиентов, при котором появляется первая ошибка:")
    for workers in WORKER_ORDER:
        vals = error_points.get(workers, [])
        if vals:
            print(f"  {workers} воркера: {sum(vals)/len(vals):.2f} (n={len(vals)})")
        else:
            print(f"  {workers} воркера: ошибок не зафиксировано")


if __name__ == "__main__":
    main()
