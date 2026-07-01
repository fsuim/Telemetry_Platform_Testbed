#!/usr/bin/env python3
"""
Unified analysis helper for telemetry-disconnection rate-sweep experiments.

This script replaces the previous two-step workflow:
  1) analyze_rate_sweep_with_baseline.py  -> reconnect-only vs replay tables
  2) analyze_rate_sweep.py                -> replay-only publication figures

It analyzes both:
  - ui_log_disconnect_replay_rXX_repY.csv
  - ui_log_reconnect_only_rXX_repY.csv

Main outputs:
  run_metrics.csv
  aggregated_metrics.csv
  validation_warnings.csv
  table_summary_results.tex
  table_disconnect_replay_rate_sweep.tex
  table_reconnect_only_vs_replay.tex
  fig_disconnect_replay_summary_panel.png
  fig_replay_recovery_ratio_sweep.png
  fig_recovery_time_disconnect_sweep.png
  fig_recovery_time_sweep.png
  fig_missing_recovered_samples_sweep.png
  fig_effective_live_rate_sweep.png
  fig_effective_live_rate_all_conditions.png
  fig_recovered_vs_missing.png
  fig_recovered_vs_missing.pdf
  fig_staleness_boxplot.png
  fig_unrecovered_context_comparison.png

Interpretation rule for the paper:
  The reconnect-only baseline verifies the absence of history reconstruction.
  Missing/unrecovered counts are condition-specific and should not be used to
  claim identical loss counts across conditions.
"""

from __future__ import annotations

import argparse
import math
import re
import shutil
from collections import Counter
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import matplotlib.pyplot as plt
import pandas as pd


# -----------------------------------------------------------------------------
# CLI and utilities
# -----------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Unified analysis for reconnect-only and replay-assisted UI logs."
    )
    parser.add_argument("--input-dir", required=True, help="Directory with exported UI CSV logs.")
    parser.add_argument("--output-dir", required=True, help="Directory for CSV/LaTeX/figure outputs.")
    parser.add_argument("--pattern", default="ui_log_*.csv", help="Glob pattern for UI logs.")
    parser.add_argument("--outage-s", type=int, default=3, help="Planned outage duration in seconds.")
    parser.add_argument(
        "--paper-output-dir",
        default=None,
        help="Optional directory where publication-ready figures/tables are also copied.",
    )
    parser.add_argument(
        "--staleness-all-conditions",
        action="store_true",
        help="If set, the staleness boxplot includes all conditions; by default it uses replay-assisted logs only.",
    )
    return parser.parse_args()


def safe_numeric(series: pd.Series) -> pd.Series:
    return pd.to_numeric(series, errors="coerce")


def condition_order_value(cond: object) -> int:
    order = {
        "disconnect_replay": 0,
        "reconnect_only": 1,
        "baseline": 2,
        "delay": 3,
        "loss": 4,
        "jitter": 5,
    }
    return order.get(str(cond), 99)


def pretty_condition(cond: object) -> str:
    mapping = {
        "reconnect_only": "Reconnect-only",
        "disconnect_replay": "Replay-assisted",
        "baseline": "Baseline",
        "delay": "Blocking delay",
        "loss": "Loss",
        "jitter": "Jitter",
    }
    return mapping.get(str(cond), str(cond).replace("_", " ").title())


def fmt_mean_std(mean_val, std_val, digits: int = 1, nan_text: str = "--") -> str:
    if pd.isna(mean_val):
        return nan_text
    if pd.isna(std_val):
        std_val = 0.0
    return f"{mean_val:.{digits}f} $\\pm$ {std_val:.{digits}f}"


def fmt_ratio(mean_val, std_val, digits: int = 2, nan_text: str = "--") -> str:
    return fmt_mean_std(mean_val, std_val, digits=digits, nan_text=nan_text)


def deterministic_offsets(n: int, width: float = 2.0) -> List[float]:
    if n <= 1:
        return [0.0] * n
    step = width / max(n - 1, 1)
    return [(-width / 2) + i * step for i in range(n)]


def configure_ieee_axes(ax) -> None:
    ax.tick_params(axis="both", labelsize=9)
    ax.xaxis.label.set_size(10)
    ax.yaxis.label.set_size(10)
    title = ax.get_title()
    if title:
        ax.set_title(title, fontsize=10)


# -----------------------------------------------------------------------------
# Metadata parsing
# -----------------------------------------------------------------------------

def parse_run_metadata(file_path: Path, df: Optional[pd.DataFrame] = None) -> Dict[str, object]:
    stem = file_path.stem.lower()
    run_tag = None
    if df is not None and "run_tag" in df.columns and not df["run_tag"].dropna().empty:
        run_tag = str(df["run_tag"].dropna().iloc[0]).strip().lower()

    text = f"{stem} {run_tag or ''}"

    # Current rate-sweep file-name formats.
    m = re.search(
        r"ui_log_(reconnect_only|disconnect_only|no_replay|disconnect_replay|disconnect)_r(\d+)_rep(\d+)",
        stem,
    )
    if m:
        raw_cond, rate_hz, rep = m.groups()
        if raw_cond in ("reconnect_only", "disconnect_only", "no_replay"):
            condition = "reconnect_only"
        else:
            condition = "disconnect_replay"
        return {
            "condition": condition,
            "rate_hz": int(rate_hz),
            "repetition": int(rep),
            "run_tag": run_tag or stem.replace("ui_log_", ""),
            "file_name": file_path.name,
        }

    # Generic compatibility with older files.
    m = re.search(
        r"ui_log_(baseline|delay|blocking_delay|loss|jitter)_r(\d+)_rep(\d+)",
        stem,
    )
    if m:
        raw_cond, rate_hz, rep = m.groups()
        condition = "delay" if raw_cond == "blocking_delay" else raw_cond
        return {
            "condition": condition,
            "rate_hz": int(rate_hz),
            "repetition": int(rep),
            "run_tag": run_tag or stem.replace("ui_log_", ""),
            "file_name": file_path.name,
        }

    cond_patterns = [
        (r"(?:^|[_\-\s])(reconnect_only|disconnect_only|no_replay)(?:$|[_\-\s])", "reconnect_only"),
        (r"(?:^|[_\-\s])(disconnect_replay|disconnect|replay)(?:$|[_\-\s])", "disconnect_replay"),
        (r"(?:^|[_\-\s])(baseline|c0)(?:$|[_\-\s])", "baseline"),
        (r"(?:^|[_\-\s])(delay|blocking_delay|c1)(?:$|[_\-\s])", "delay"),
        (r"(?:^|[_\-\s])(loss|c2)(?:$|[_\-\s])", "loss"),
        (r"(?:^|[_\-\s])(jitter|c4)(?:$|[_\-\s])", "jitter"),
    ]
    condition = None
    for pat, label in cond_patterns:
        if re.search(pat, text):
            condition = label
            break

    rate_hz = None
    m_rate = re.search(r"(?:^|[_\-\s])r(\d+)(?:$|[_\-\s])", text)
    if m_rate:
        rate_hz = int(m_rate.group(1))

    repetition = None
    m_rep = re.search(r"(?:^|[_\-\s])rep(\d+)(?:$|[_\-\s])", text)
    if m_rep:
        repetition = int(m_rep.group(1))

    return {
        "condition": condition,
        "rate_hz": rate_hz,
        "repetition": repetition,
        "run_tag": run_tag,
        "file_name": file_path.name,
    }


# -----------------------------------------------------------------------------
# Sequence and timing metrics
# -----------------------------------------------------------------------------

def infer_nominal_seq_step(seqs: Sequence[int]) -> int:
    if len(seqs) < 2:
        return 1
    diffs: List[int] = []
    prev = seqs[0]
    for cur in seqs[1:]:
        d = cur - prev
        if d > 0:
            diffs.append(d)
        prev = cur
    if not diffs:
        return 1
    step, _ = Counter(diffs).most_common(1)[0]
    return int(step) if step > 0 else 1


def compute_missing_ranges_with_step(seqs: Sequence[int], step: int) -> List[Tuple[int, int, int]]:
    ranges: List[Tuple[int, int, int]] = []
    if len(seqs) < 2:
        return ranges
    prev = seqs[0]
    for cur in seqs[1:]:
        d = cur - prev
        if d > step:
            approx_units = int(round(d / step))
            missing_count = max(approx_units - 1, 0)
            if missing_count > 0:
                start = prev + step
                end = prev + step * missing_count
                ranges.append((start, end, missing_count))
        prev = cur
    return ranges


def expand_missing_expected_values(ranges: Sequence[Tuple[int, int, int]], step: int) -> set:
    values = set()
    for start, end, _ in ranges:
        v = start
        while v <= end:
            values.add(v)
            v += step
    return values


def planned_disconnect_pair(df: pd.DataFrame, outage_s: int) -> Optional[Tuple[float, float]]:
    """Return (close_t, open_t) for the planned WebSocket disconnection.

    If several ws_close/ws_open pairs exist, select the pair whose duration is
    closest to the planned outage. This avoids using accidental startup or late
    reconnects as the experimental event.
    """
    if "event" not in df.columns or "wall_ms" not in df.columns:
        return None
    temp = df.copy().sort_values("wall_ms").reset_index(drop=True)
    temp["wall_ms"] = safe_numeric(temp["wall_ms"])
    close_idxs = temp.index[temp["event"] == "ws_close"].tolist()
    target_ms = outage_s * 1000.0
    candidates: List[Tuple[float, float, float]] = []
    for idx in close_idxs:
        close_t = temp.loc[idx, "wall_ms"]
        if pd.isna(close_t):
            continue
        after = temp.iloc[idx + 1 :]
        open_rows = after[after["event"] == "ws_open"]
        if open_rows.empty:
            continue
        open_t = safe_numeric(open_rows["wall_ms"]).dropna().iloc[0]
        duration = float(open_t - close_t)
        if duration <= 0:
            continue
        candidates.append((abs(duration - target_ms), float(close_t), float(open_t)))
    if not candidates:
        return None
    candidates.sort(key=lambda x: x[0])
    _, close_t, open_t = candidates[0]
    return close_t, open_t


def compute_recovery_time_ms(df: pd.DataFrame, outage_s: int) -> float:
    pair = planned_disconnect_pair(df, outage_s)
    if pair is None or "event" not in df.columns or "wall_ms" not in df.columns:
        return math.nan
    _, open_t = pair
    temp = df.copy().sort_values("wall_ms").reset_index(drop=True)
    temp["wall_ms"] = safe_numeric(temp["wall_ms"])
    if "source" not in temp.columns:
        temp["source"] = ""
    live_after_open = temp[
        (temp["event"] == "telemetry")
        & (temp["source"] == "live")
        & (temp["wall_ms"] >= open_t)
    ]
    if live_after_open.empty:
        return math.nan
    first_live_t = safe_numeric(live_after_open["wall_ms"]).dropna().iloc[0]
    return float(first_live_t - open_t)


# -----------------------------------------------------------------------------
# Per-file analysis
# -----------------------------------------------------------------------------

def analyze_one_file(file_path: Path, outage_s: int) -> Dict[str, object]:
    df = pd.read_csv(file_path)
    meta = parse_run_metadata(file_path, df)

    if "event" not in df.columns:
        raise ValueError(f"{file_path.name}: missing required column 'event'")
    if "source" not in df.columns:
        df["source"] = ""

    for col in ["wall_ms", "seq", "t_ns", "staleness_ms", "gap_from_prev", "count", "last"]:
        if col in df.columns:
            df[col] = safe_numeric(df[col])

    df = df.sort_values("wall_ms", na_position="last").reset_index(drop=True)
    live_df = df[(df["event"] == "telemetry") & (df["source"] == "live")].copy()
    replay_df = df[(df["event"] == "telemetry") & (df["source"] == "replay")].copy()

    live_seqs = safe_numeric(live_df.get("seq", pd.Series(dtype=float))).dropna().astype(int).tolist()
    replay_seqs = set(safe_numeric(replay_df.get("seq", pd.Series(dtype=float))).dropna().astype(int).tolist())
    nominal_step = infer_nominal_seq_step(live_seqs)

    missing_ranges = compute_missing_ranges_with_step(live_seqs, nominal_step)
    missing_expected = expand_missing_expected_values(missing_ranges, nominal_step)

    recovered_by_replay = len(missing_expected.intersection(replay_seqs))
    if meta.get("condition") == "reconnect_only":
        # In the baseline, replay is disabled. If replay rows exist, the raw
        # replay_count is kept for validation, but recovered context is reported as zero.
        recovered_by_replay = 0
        replay_recovery_ratio = 0.0 if missing_expected else math.nan
    else:
        replay_recovery_ratio = recovered_by_replay / len(missing_expected) if missing_expected else math.nan

    unrecovered_context = len(missing_expected) - recovered_by_replay if missing_expected else 0

    wall_ms = safe_numeric(df.get("wall_ms", pd.Series(dtype=float))).dropna()
    duration_s = float((wall_ms.iloc[-1] - wall_ms.iloc[0]) / 1000.0) if len(wall_ms) >= 2 else math.nan
    live_count = int(len(live_df))
    replay_count = int(len(replay_df))
    effective_live_rate = float(live_count / duration_s) if pd.notna(duration_s) and duration_s > 0 else math.nan

    staleness = live_df["staleness_ms"].dropna() if "staleness_ms" in live_df.columns else pd.Series(dtype=float)

    ia_mean = ia_std = ia_p95 = math.nan
    if len(live_df) >= 2 and "wall_ms" in live_df.columns:
        ia = live_df["wall_ms"].diff().dropna()
        if not ia.empty:
            ia_mean = float(ia.mean())
            ia_std = float(ia.std(ddof=1)) if len(ia) > 1 else 0.0
            ia_p95 = float(ia.quantile(0.95))

    history_loaded = int((df["event"] == "history_batch_loaded").sum())
    history_requests = int((df["event"] == "history_request").sum())

    warning_parts: List[str] = []
    if meta.get("condition") == "reconnect_only":
        if replay_count > 0 or history_requests > 0 or history_loaded > 0:
            warning_parts.append("reconnect_only contains replay/history events")
    if meta.get("condition") == "disconnect_replay":
        expected_window = None
        if meta.get("rate_hz") is not None:
            expected_window = int(meta["rate_hz"]) * outage_s
        if history_requests != 1:
            warning_parts.append(f"expected 1 history_request, found {history_requests}")
        if history_loaded != 1:
            warning_parts.append(f"expected 1 history_batch_loaded, found {history_loaded}")
        if expected_window is not None and replay_count != expected_window:
            warning_parts.append(f"expected {expected_window} replay rows, found {replay_count}")

    return {
        **meta,
        "nominal_seq_step": nominal_step,
        "duration_s": duration_s,
        "effective_live_rate": effective_live_rate,
        "n_rows_total": int(len(df)),
        "live_count": live_count,
        "replay_count": replay_count,
        "history_requests": history_requests,
        "history_loaded": history_loaded,
        "missing_samples": len(missing_expected),
        "live_missing_total": len(missing_expected),
        "gaps_count": len(missing_ranges),
        "largest_gap": max((mc for _, _, mc in missing_ranges), default=0),
        "recovered_by_replay": recovered_by_replay,
        "unrecovered_context": unrecovered_context,
        "replay_recovery_ratio": replay_recovery_ratio,
        "recovery_time_ms": compute_recovery_time_ms(df, outage_s),
        "relative_timestamp_offset_mean_ms": float(staleness.mean()) if not staleness.empty else math.nan,
        "relative_timestamp_offset_median_ms": float(staleness.median()) if not staleness.empty else math.nan,
        "relative_timestamp_offset_p95_ms": float(staleness.quantile(0.95)) if not staleness.empty else math.nan,
        "relative_timestamp_offset_max_ms": float(staleness.max()) if not staleness.empty else math.nan,
        # Backward-compatible aliases used by the previous generic script.
        "staleness_mean_ms": float(staleness.mean()) if not staleness.empty else math.nan,
        "staleness_median_ms": float(staleness.median()) if not staleness.empty else math.nan,
        "staleness_p95_ms": float(staleness.quantile(0.95)) if not staleness.empty else math.nan,
        "staleness_max_ms": float(staleness.max()) if not staleness.empty else math.nan,
        "inter_arrival_mean_ms": ia_mean,
        "inter_arrival_std_ms": ia_std,
        "inter_arrival_p95_ms": ia_p95,
        "validation_warning": "; ".join(warning_parts),
    }


def aggregate_runs(runs_df: pd.DataFrame) -> pd.DataFrame:
    metrics = [
        "duration_s",
        "effective_live_rate",
        "missing_samples",
        "live_missing_total",
        "recovered_by_replay",
        "unrecovered_context",
        "replay_recovery_ratio",
        "recovery_time_ms",
        "replay_count",
        "live_count",
        "history_requests",
        "history_loaded",
        "gaps_count",
        "largest_gap",
        "relative_timestamp_offset_mean_ms",
        "relative_timestamp_offset_median_ms",
        "relative_timestamp_offset_p95_ms",
        "relative_timestamp_offset_max_ms",
        "staleness_mean_ms",
        "staleness_median_ms",
        "staleness_p95_ms",
        "staleness_max_ms",
        "inter_arrival_mean_ms",
        "inter_arrival_std_ms",
        "inter_arrival_p95_ms",
    ]
    present = [m for m in metrics if m in runs_df.columns]
    grouped = runs_df.groupby(["condition", "rate_hz"], dropna=False)[present]
    agg = grouped.agg(["mean", "std", "count"]).reset_index()
    agg.columns = ["_".join([str(x) for x in col if x]) for col in agg.columns.to_flat_index()]
    agg["condition_order"] = agg["condition"].map(condition_order_value).fillna(99)
    return agg.sort_values(["condition_order", "rate_hz"]).drop(columns=["condition_order"])


def collect_live_staleness_points(files: Sequence[Path], runs_df: pd.DataFrame) -> pd.DataFrame:
    rows = []
    meta_by_file = {row["file_name"]: row for _, row in runs_df.iterrows()}
    for file_path in files:
        try:
            df = pd.read_csv(file_path)
        except Exception:
            continue
        if "event" not in df.columns or "source" not in df.columns or "staleness_ms" not in df.columns:
            continue
        df["staleness_ms"] = safe_numeric(df["staleness_ms"])
        live = df[(df["event"] == "telemetry") & (df["source"] == "live")].copy()
        if live.empty:
            continue
        meta = meta_by_file.get(file_path.name)
        if meta is None:
            continue
        live["condition"] = meta.get("condition")
        live["rate_hz"] = meta.get("rate_hz")
        live["file_name"] = file_path.name
        rows.append(live[["condition", "rate_hz", "file_name", "staleness_ms"]])
    return pd.concat(rows, ignore_index=True) if rows else pd.DataFrame(columns=["condition", "rate_hz", "file_name", "staleness_ms"])


# -----------------------------------------------------------------------------
# LaTeX tables
# -----------------------------------------------------------------------------

def write_summary_table(agg_df: pd.DataFrame, out_path: Path) -> None:
    lines = [
        r"\begin{table}[t]",
        r"\centering",
        r"\caption{Summary of interface-level behavior. Values are descriptive mean $\pm$ standard deviation across available repetitions.}",
        r"\label{tab:summary_results_updated}",
        r"\scriptsize",
        r"\begin{tabular}{llccc}",
        r"\hline",
        r"Condition & Rate (Hz) & Effective live rate (Hz) & Post-reopen stab. time (ms) & Replay ratio \\",
        r"\hline",
    ]
    tmp = agg_df.copy()
    tmp["condition_order"] = tmp["condition"].map(condition_order_value).fillna(99)
    tmp = tmp.sort_values(["condition_order", "rate_hz"])
    for _, row in tmp.iterrows():
        cond = row["condition"]
        rate = int(row["rate_hz"]) if pd.notna(row["rate_hz"]) else "--"
        eff = fmt_mean_std(row.get("effective_live_rate_mean"), row.get("effective_live_rate_std"), digits=1)
        rec = fmt_mean_std(row.get("recovery_time_ms_mean"), row.get("recovery_time_ms_std"), digits=1)
        rep = fmt_ratio(row.get("replay_recovery_ratio_mean"), row.get("replay_recovery_ratio_std"), digits=2)
        lines.append(f"{pretty_condition(cond)} & {rate} & {eff} & {rec} & {rep} " + r"\\")
    lines.extend([r"\hline", r"\end{tabular}", r"\end{table}"])
    out_path.write_text("\n".join(lines), encoding="utf-8")


def write_disconnect_replay_table(agg_df: pd.DataFrame, out_path: Path, outage_s: int) -> None:
    disc = agg_df[(agg_df["condition"] == "disconnect_replay") & (agg_df["rate_hz"].notna())].copy()
    disc = disc.sort_values("rate_hz")
    lines = [
        r"\begin{table}[t]",
        r"\centering",
        r"\caption{Disconnect/replay telemetry-rate sweep. The replay window is proportional to the induced outage duration. Values are descriptive mean $\pm$ standard deviation.}",
        r"\label{tab:disconnect_replay_rate_sweep}",
        r"\scriptsize",
        r"\begin{tabular}{lcccccc}",
        r"\hline",
        r"Rate (Hz) & Window & Eff. rate (Hz) & Missing & Recovered & Stab. time (ms) & Ratio \\",
        r"\hline",
    ]
    for _, row in disc.iterrows():
        rate = int(row["rate_hz"])
        replay_window = rate * outage_s
        eff = fmt_mean_std(row.get("effective_live_rate_mean"), row.get("effective_live_rate_std"), digits=1)
        missing = fmt_mean_std(row.get("missing_samples_mean"), row.get("missing_samples_std"), digits=1)
        recovered = fmt_mean_std(row.get("recovered_by_replay_mean"), row.get("recovered_by_replay_std"), digits=1)
        rec = fmt_mean_std(row.get("recovery_time_ms_mean"), row.get("recovery_time_ms_std"), digits=1)
        ratio = fmt_ratio(row.get("replay_recovery_ratio_mean"), row.get("replay_recovery_ratio_std"), digits=2)
        lines.append(f"{rate} & {replay_window} & {eff} & {missing} & {recovered} & {rec} & {ratio} " + r"\\")
    lines.extend([r"\hline", r"\end{tabular}", r"\end{table}"])
    out_path.write_text("\n".join(lines), encoding="utf-8")


def write_baseline_comparison_table(agg_df: pd.DataFrame, out_path: Path, outage_s: int) -> None:
    comp = agg_df[agg_df["condition"].isin(["reconnect_only", "disconnect_replay"])].copy()
    comp["condition_order_for_table"] = comp["condition"].map({"disconnect_replay": 0, "reconnect_only": 1}).fillna(9)
    comp = comp.sort_values(["rate_hz", "condition_order_for_table"])
    lines = [
        r"\begin{table*}[t]",
        r"\centering",
        r"\caption{Reconnect-only baseline versus replay-assisted recovery in the transient-disconnection experiment. Values are descriptive mean $\pm$ standard deviation; missing and unrecovered counts are condition-specific and are not used to claim identical loss counts across conditions.}",
        r"\label{tab:reconnect_only_vs_replay}",
        r"\scriptsize",
        r"\begin{tabular}{llcccccc}",
        r"\hline",
        r"Rate (Hz) & Condition & Window & Missing & Recovered & Unrecovered & Stab. time (ms) & Ratio \\",
        r"\hline",
    ]
    for _, row in comp.iterrows():
        rate = int(row["rate_hz"])
        cond = str(row["condition"])
        window = "--" if cond == "reconnect_only" else str(rate * outage_s)
        missing = fmt_mean_std(row.get("missing_samples_mean"), row.get("missing_samples_std"), digits=1)
        recovered = fmt_mean_std(row.get("recovered_by_replay_mean"), row.get("recovered_by_replay_std"), digits=1)
        unrecovered = fmt_mean_std(row.get("unrecovered_context_mean"), row.get("unrecovered_context_std"), digits=1)
        rec = fmt_mean_std(row.get("recovery_time_ms_mean"), row.get("recovery_time_ms_std"), digits=1)
        ratio = fmt_ratio(row.get("replay_recovery_ratio_mean"), row.get("replay_recovery_ratio_std"), digits=2)
        lines.append(f"{rate} & {pretty_condition(cond)} & {window} & {missing} & {recovered} & {unrecovered} & {rec} & {ratio} " + r"\\")
    lines.extend([r"\hline", r"\end{tabular}", r"\end{table*}"])
    out_path.write_text("\n".join(lines), encoding="utf-8")


# -----------------------------------------------------------------------------
# Figures
# -----------------------------------------------------------------------------

def disconnect_agg(agg_df: pd.DataFrame) -> pd.DataFrame:
    return agg_df[(agg_df["condition"] == "disconnect_replay") & (agg_df["rate_hz"].notna())].sort_values("rate_hz")


def disconnect_runs(runs_df: pd.DataFrame) -> pd.DataFrame:
    return runs_df[(runs_df["condition"] == "disconnect_replay") & (runs_df["rate_hz"].notna())].copy()


def plot_metric_sweep(
    runs_df: pd.DataFrame,
    agg_df: pd.DataFrame,
    metric: str,
    metric_mean: str,
    metric_std: str,
    ylabel: str,
    out_path: Path,
    title: Optional[str] = None,
) -> None:
    disc_runs = disconnect_runs(runs_df)
    disc_runs = disc_runs[disc_runs[metric].notna()].copy()
    disc_agg = disconnect_agg(agg_df)
    disc_agg = disc_agg[disc_agg[metric_mean].notna()].sort_values("rate_hz")
    if disc_agg.empty:
        return

    rates = disc_agg["rate_hz"].astype(float).tolist()
    means = disc_agg[metric_mean].astype(float).tolist()
    stds = disc_agg[metric_std].fillna(0).astype(float).tolist() if metric_std in disc_agg.columns else [0] * len(means)

    fig, ax = plt.subplots(figsize=(3.5, 2.6))
    ax.errorbar(rates, means, yerr=stds, marker="o", capsize=3, linewidth=1.2)

    for rate in rates:
        vals = disc_runs.loc[disc_runs["rate_hz"].astype(float) == rate, metric].dropna().tolist()
        offs = deterministic_offsets(len(vals), width=max(1.0, rate * 0.035))
        ax.plot([rate + off for off in offs], vals, "o", markersize=3, alpha=0.75)

    ax.set_xlabel("Rate (Hz)")
    ax.set_ylabel(ylabel)
    if title:
        ax.set_title(title)
    configure_ieee_axes(ax)
    fig.tight_layout(pad=0.4)
    fig.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)


def plot_missing_recovered_samples_sweep(agg_df: pd.DataFrame, out_path: Path) -> None:
    disc = disconnect_agg(agg_df)
    if disc.empty:
        return
    rates = disc["rate_hz"].astype(float)
    fig, ax = plt.subplots(figsize=(3.5, 2.6))
    ax.errorbar(
        rates,
        disc["missing_samples_mean"],
        yerr=disc["missing_samples_std"].fillna(0),
        marker="o",
        capsize=3,
        linewidth=1.2,
        label="Missing",
    )
    ax.errorbar(
        rates,
        disc["recovered_by_replay_mean"],
        yerr=disc["recovered_by_replay_std"].fillna(0),
        marker="s",
        capsize=3,
        linewidth=1.2,
        label="Recovered",
    )
    ax.set_xlabel("Rate (Hz)")
    ax.set_ylabel("Samples")
    ax.legend(fontsize=8, frameon=False)
    configure_ieee_axes(ax)
    fig.tight_layout(pad=0.4)
    fig.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)


def plot_effective_rate_all(agg_df: pd.DataFrame, out_path: Path) -> None:
    plot_df = agg_df.dropna(subset=["condition", "rate_hz", "effective_live_rate_mean"]).copy()
    if plot_df.empty:
        return
    plot_df["order"] = plot_df["condition"].map(condition_order_value).fillna(99)
    plot_df = plot_df.sort_values(["order", "rate_hz"])
    labels = [f"{pretty_condition(c)}\n{int(r)} Hz" for c, r in zip(plot_df["condition"], plot_df["rate_hz"])]
    values = plot_df["effective_live_rate_mean"].tolist()
    errs = plot_df.get("effective_live_rate_std", pd.Series([0] * len(plot_df))).fillna(0).tolist()

    fig_width = max(6.0, 0.55 * len(labels))
    fig, ax = plt.subplots(figsize=(fig_width, 3.2))
    ax.bar(range(len(values)), values, yerr=errs, capsize=2)
    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(labels, rotation=35, ha="right", fontsize=8)
    ax.set_ylabel("Effective live update rate (Hz)")
    configure_ieee_axes(ax)
    fig.tight_layout(pad=0.5)
    fig.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)


def plot_unrecovered_comparison(agg_df: pd.DataFrame, out_path: Path) -> None:
    comp = agg_df[agg_df["condition"].isin(["reconnect_only", "disconnect_replay"])].copy()
    comp = comp.dropna(subset=["rate_hz", "unrecovered_context_mean"])
    if comp.empty:
        return
    rates = sorted(comp["rate_hz"].dropna().unique())
    conditions = ["reconnect_only", "disconnect_replay"]
    x_positions = []
    values = []
    errors = []
    labels = []
    pos = 0
    for rate in rates:
        for cond in conditions:
            row = comp[(comp["rate_hz"] == rate) & (comp["condition"] == cond)]
            if row.empty:
                continue
            x_positions.append(pos)
            values.append(float(row["unrecovered_context_mean"].iloc[0]))
            std = row["unrecovered_context_std"].iloc[0] if "unrecovered_context_std" in row else 0.0
            errors.append(0.0 if pd.isna(std) else float(std))
            labels.append(f"{int(rate)} Hz\n{pretty_condition(cond)}")
            pos += 1
        pos += 0.75

    fig_width = max(6.5, 0.45 * len(labels))
    fig, ax = plt.subplots(figsize=(fig_width, 3.2))
    ax.bar(x_positions, values, yerr=errors, capsize=2)
    ax.set_xticks(x_positions)
    ax.set_xticklabels(labels, rotation=35, ha="right", fontsize=8)
    ax.set_ylabel("Unrecovered context (samples)")
    ax.tick_params(axis="y", labelsize=9)
    fig.tight_layout(pad=0.5)
    fig.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)


def plot_recovered_vs_missing(runs_df: pd.DataFrame, agg_df: pd.DataFrame, out_png: Path, out_pdf: Optional[Path] = None) -> None:
    disc_runs = disconnect_runs(runs_df)
    disc_runs = disc_runs[
        disc_runs["missing_samples"].notna() & disc_runs["recovered_by_replay"].notna()
    ].copy()
    disc_agg = disconnect_agg(agg_df)
    disc_agg = disc_agg[
        disc_agg["missing_samples_mean"].notna() & disc_agg["recovered_by_replay_mean"].notna()
    ].copy()
    if disc_runs.empty and disc_agg.empty:
        return

    fig, ax = plt.subplots(figsize=(3.5, 2.7))
    if not disc_runs.empty:
        for rate in sorted(disc_runs["rate_hz"].dropna().unique()):
            subset = disc_runs[disc_runs["rate_hz"] == rate].sort_values("repetition")
            ax.scatter(
                subset["missing_samples"],
                subset["recovered_by_replay"],
                s=18,
                alpha=0.65,
                label=f"{int(rate)} Hz",
            )
    if not disc_agg.empty:
        disc_agg = disc_agg.sort_values("rate_hz")
        for _, row in disc_agg.iterrows():
            rate = int(row["rate_hz"])
            x = float(row["missing_samples_mean"])
            y = float(row["recovered_by_replay_mean"])
            xerr = 0.0 if pd.isna(row.get("missing_samples_std")) else float(row.get("missing_samples_std"))
            yerr = 0.0 if pd.isna(row.get("recovered_by_replay_std")) else float(row.get("recovered_by_replay_std"))
            ax.errorbar(x, y, xerr=xerr, yerr=yerr, fmt="D", markersize=4.5, capsize=3, linewidth=1.0)
            ax.annotate(f"{rate}", xy=(x, y), xytext=(4, 4), textcoords="offset points", fontsize=8)

    ax.set_xlabel("Live-missing samples")
    ax.set_ylabel("Samples recovered through replay")
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)
    ax.grid(True, linewidth=0.3, alpha=0.35)
    handles, labels = ax.get_legend_handles_labels()
    if handles and len(handles) <= 6:
        ax.legend(handles, labels, title="Rate", fontsize=7, title_fontsize=8, loc="upper left", frameon=False)
    configure_ieee_axes(ax)
    fig.tight_layout(pad=0.4)
    fig.savefig(out_png, dpi=300, bbox_inches="tight")
    if out_pdf is not None:
        fig.savefig(out_pdf, bbox_inches="tight")
    plt.close(fig)


def plot_staleness_boxplot(all_live_points: pd.DataFrame, out_path: Path, all_conditions: bool = False) -> None:
    """Plot the relative timestamp-offset distribution used as Fig. 4.

    The paper inserts Fig. 4 as a single-column figure. Earlier versions used a
    wider figure size (6.0 x 3.2 in); when scaled down to one column in LaTeX,
    the tick labels and axis label appeared smaller than the other figures. The
    default replay-only version now uses the same single-column dimensions and
    font settings as the other paper figures.
    """
    data = all_live_points.dropna(subset=["condition", "rate_hz", "staleness_ms"]).copy()
    if not all_conditions:
        data = data[data["condition"] == "disconnect_replay"].copy()
    if data.empty:
        return

    keys = data[["condition", "rate_hz"]].drop_duplicates().copy()
    keys["order"] = keys["condition"].map(condition_order_value).fillna(99)
    keys = keys.sort_values(["order", "rate_hz"])

    labels = []
    series = []
    for _, row in keys.iterrows():
        subset = data[(data["condition"] == row["condition"]) & (data["rate_hz"] == row["rate_hz"])]
        vals = subset["staleness_ms"].dropna().tolist()
        if vals:
            if all_conditions:
                labels.append(f"{pretty_condition(row['condition'])}\n{int(row['rate_hz'])} Hz")
            else:
                # The caption already states that the plot is for replay-assisted
                # telemetry rates. Short labels avoid down-scaling and keep the
                # font visually consistent with Figs. 2 and 3.
                labels.append(f"{int(row['rate_hz'])} Hz")
            series.append(vals)
    if not series:
        return

    if all_conditions:
        # Multi-condition diagnostic plot: keep the wider canvas because it may
        # contain many categories.
        fig_width = max(6.0, 0.55 * len(series))
        fig, ax = plt.subplots(figsize=(fig_width, 3.2))
        ax.boxplot(series, labels=labels, showfliers=False)
        ax.tick_params(axis="x", labelrotation=35, labelsize=8)
        for tick in ax.get_xticklabels():
            tick.set_ha("right")
    else:
        # Paper Fig. 4: keep the same column width and font sizes used by the
        # other standalone figures, but use a slightly taller canvas so the full
        # rotated y-axis label, including the "(ms)" unit, is not clipped.
        # The LaTeX file should still include this figure with width=\columnwidth.
        fig, ax = plt.subplots(figsize=(3.5, 3.25))
        ax.boxplot(series, labels=labels, showfliers=False)
        ax.tick_params(axis="x", labelsize=9)
        ax.set_xlabel("Rate (Hz)")

    ax.set_ylabel("Relative timestamp offset at UI (ms)", labelpad=8)
    configure_ieee_axes(ax)

    if all_conditions:
        fig.tight_layout(pad=0.5)
    else:
        # Manual margins are more reliable than tight_layout alone for this
        # long, rotated label. They preserve the same font size while giving the
        # label enough vertical extent in the saved PNG.
        fig.subplots_adjust(left=0.24, right=0.98, bottom=0.18, top=0.97)

    fig.savefig(out_path, dpi=300, bbox_inches="tight", pad_inches=0.08)
    plt.close(fig)

def _panel_scatter_points(ax, runs_df: pd.DataFrame, metric: str) -> None:
    for rate in sorted(runs_df["rate_hz"].dropna().unique()):
        vals = runs_df.loc[runs_df["rate_hz"] == rate, metric].dropna().tolist()
        if not vals:
            continue
        offs = deterministic_offsets(len(vals), width=max(1.0, float(rate) * 0.035))
        ax.plot([float(rate) + off for off in offs], vals, "o", markersize=3, alpha=0.70)


def plot_disconnect_replay_summary_panel(runs_df: pd.DataFrame, agg_df: pd.DataFrame, out_path: Path) -> None:
    disc_runs = disconnect_runs(runs_df)
    disc_agg = disconnect_agg(agg_df)
    if disc_agg.empty:
        return
    rates = disc_agg["rate_hz"].astype(float)

    fig, axes = plt.subplots(2, 2, figsize=(6.9, 5.0))
    ax = axes[0, 0]
    ax.errorbar(
        rates,
        disc_agg["replay_recovery_ratio_mean"],
        yerr=disc_agg["replay_recovery_ratio_std"].fillna(0),
        marker="o",
        capsize=3,
        linewidth=1.2,
    )
    _panel_scatter_points(ax, disc_runs, "replay_recovery_ratio")
    ax.set_title("(a) Replay recovery ratio")
    ax.set_xlabel("Rate (Hz)")
    ax.set_ylabel("Replay ratio")
    configure_ieee_axes(ax)

    ax = axes[0, 1]
    ax.errorbar(
        rates,
        disc_agg["recovery_time_ms_mean"],
        yerr=disc_agg["recovery_time_ms_std"].fillna(0),
        marker="o",
        capsize=3,
        linewidth=1.2,
    )
    _panel_scatter_points(ax, disc_runs, "recovery_time_ms")
    ax.set_title("(b) Post-reopen stabilization")
    ax.set_xlabel("Rate (Hz)")
    ax.set_ylabel("Post-reopen stabilization (ms)")
    configure_ieee_axes(ax)

    ax = axes[1, 0]
    ax.errorbar(
        rates,
        disc_agg["missing_samples_mean"],
        yerr=disc_agg["missing_samples_std"].fillna(0),
        marker="o",
        capsize=3,
        linewidth=1.2,
        label="Missing",
    )
    ax.errorbar(
        rates,
        disc_agg["recovered_by_replay_mean"],
        yerr=disc_agg["recovered_by_replay_std"].fillna(0),
        marker="s",
        capsize=3,
        linewidth=1.2,
        label="Recovered",
    )
    ax.set_title("(c) Missing vs recovered")
    ax.set_xlabel("Rate (Hz)")
    ax.set_ylabel("Samples")
    ax.legend(fontsize=8, frameon=False)
    configure_ieee_axes(ax)

    ax = axes[1, 1]
    ax.errorbar(
        rates,
        disc_agg["effective_live_rate_mean"],
        yerr=disc_agg["effective_live_rate_std"].fillna(0),
        marker="o",
        capsize=3,
        linewidth=1.2,
    )
    _panel_scatter_points(ax, disc_runs, "effective_live_rate")
    ax.set_title("(d) Effective live rate")
    ax.set_xlabel("Rate (Hz)")
    ax.set_ylabel("Effective live rate (Hz)")
    configure_ieee_axes(ax)

    fig.tight_layout(pad=0.7)
    fig.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)


# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------

def copy_publication_outputs(output_dir: Path, paper_dir: Path) -> None:
    names = [
        "run_metrics.csv",
        "aggregated_metrics.csv",
        "validation_warnings.csv",
        "table_summary_results.tex",
        "table_disconnect_replay_rate_sweep.tex",
        "table_reconnect_only_vs_replay.tex",
        "fig_disconnect_replay_summary_panel.png",
        "fig_replay_recovery_ratio_sweep.png",
        "fig_recovery_time_disconnect_sweep.png",
        "fig_recovery_time_sweep.png",
        "fig_missing_recovered_samples_sweep.png",
        "fig_effective_live_rate_sweep.png",
        "fig_effective_live_rate_all_conditions.png",
        "fig_recovered_vs_missing.png",
        "fig_recovered_vs_missing.pdf",
        "fig_staleness_boxplot.png",
        "fig_unrecovered_context_comparison.png",
    ]
    paper_dir.mkdir(parents=True, exist_ok=True)
    for name in names:
        src = output_dir / name
        if src.exists():
            shutil.copy2(src, paper_dir / name)


def main() -> None:
    args = parse_args()
    input_dir = Path(args.input_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    files = sorted(input_dir.glob(args.pattern))
    if not files:
        raise SystemExit(f"No files found in {input_dir} with pattern {args.pattern}")

    rows = []
    skipped = []
    for file_path in files:
        try:
            row = analyze_one_file(file_path, outage_s=args.outage_s)
            if row.get("condition") is not None:
                rows.append(row)
            else:
                skipped.append((file_path.name, "could not infer condition from file name or run_tag"))
        except Exception as exc:
            skipped.append((file_path.name, str(exc)))

    if not rows:
        raise SystemExit("No valid UI logs could be analyzed.")

    runs_df = pd.DataFrame(rows).sort_values(["condition", "rate_hz", "repetition"], na_position="last")
    agg_df = aggregate_runs(runs_df)
    live_points = collect_live_staleness_points(files, runs_df)

    runs_df.to_csv(output_dir / "run_metrics.csv", index=False)
    agg_df.to_csv(output_dir / "aggregated_metrics.csv", index=False)

    warn_cols = ["file_name", "condition", "rate_hz", "repetition", "validation_warning", "replay_count", "history_requests", "history_loaded"]
    warnings = runs_df[runs_df["validation_warning"].astype(str).str.len() > 0][warn_cols]
    if warnings.empty:
        pd.DataFrame(columns=warn_cols).to_csv(output_dir / "validation_warnings.csv", index=False)
    else:
        warnings.to_csv(output_dir / "validation_warnings.csv", index=False)

    if skipped:
        pd.DataFrame(skipped, columns=["file_name", "error"]).to_csv(output_dir / "skipped_logs.csv", index=False)

    write_summary_table(agg_df, output_dir / "table_summary_results.tex")
    write_disconnect_replay_table(agg_df, output_dir / "table_disconnect_replay_rate_sweep.tex", outage_s=args.outage_s)
    write_baseline_comparison_table(agg_df, output_dir / "table_reconnect_only_vs_replay.tex", outage_s=args.outage_s)

    # Replay-assisted paper figures.
    plot_disconnect_replay_summary_panel(runs_df, agg_df, output_dir / "fig_disconnect_replay_summary_panel.png")
    plot_metric_sweep(
        runs_df,
        agg_df,
        metric="replay_recovery_ratio",
        metric_mean="replay_recovery_ratio_mean",
        metric_std="replay_recovery_ratio_std",
        ylabel="Replay recovery ratio",
        out_path=output_dir / "fig_replay_recovery_ratio_sweep.png",
    )
    plot_metric_sweep(
        runs_df,
        agg_df,
        metric="recovery_time_ms",
        metric_mean="recovery_time_ms_mean",
        metric_std="recovery_time_ms_std",
        ylabel="Post-reopen stabilization (ms)",
        out_path=output_dir / "fig_recovery_time_disconnect_sweep.png",
    )
    # Backward-compatible alias used by earlier project folders.
    if (output_dir / "fig_recovery_time_disconnect_sweep.png").exists():
        shutil.copy2(output_dir / "fig_recovery_time_disconnect_sweep.png", output_dir / "fig_recovery_time_sweep.png")

    plot_metric_sweep(
        runs_df,
        agg_df,
        metric="effective_live_rate",
        metric_mean="effective_live_rate_mean",
        metric_std="effective_live_rate_std",
        ylabel="Effective live rate (Hz)",
        out_path=output_dir / "fig_effective_live_rate_sweep.png",
    )
    plot_missing_recovered_samples_sweep(agg_df, output_dir / "fig_missing_recovered_samples_sweep.png")
    plot_recovered_vs_missing(
        runs_df,
        agg_df,
        out_png=output_dir / "fig_recovered_vs_missing.png",
        out_pdf=output_dir / "fig_recovered_vs_missing.pdf",
    )
    plot_staleness_boxplot(live_points, output_dir / "fig_staleness_boxplot.png", all_conditions=args.staleness_all_conditions)

    # Baseline/comparison figures.
    plot_unrecovered_comparison(agg_df, output_dir / "fig_unrecovered_context_comparison.png")
    plot_effective_rate_all(agg_df, output_dir / "fig_effective_live_rate_all_conditions.png")

    if args.paper_output_dir:
        copy_publication_outputs(output_dir, Path(args.paper_output_dir))

    print(f"Analyzed {len(runs_df)} files. Outputs written to: {output_dir}")
    if skipped:
        print(f"Skipped {len(skipped)} files. See skipped_logs.csv")
    if not warnings.empty:
        print("WARNING: Some logs contain validation warnings. See validation_warnings.csv")
    if args.paper_output_dir:
        print(f"Publication-ready files also copied to: {args.paper_output_dir}")


if __name__ == "__main__":
    main()
