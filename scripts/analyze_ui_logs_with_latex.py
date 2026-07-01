import argparse
import math
import re
from collections import Counter
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import matplotlib.pyplot as plt
import pandas as pd


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze UI telemetry logs, generate plots, and export LaTeX tables."
    )
    parser.add_argument("--input-dir", type=str, required=True)
    parser.add_argument("--output-dir", type=str, required=True)
    parser.add_argument("--pattern", type=str, default="*.csv")
    return parser.parse_args()


def safe_numeric(series: pd.Series) -> pd.Series:
    return pd.to_numeric(series, errors="coerce")


def parse_run_metadata(file_path: Path, df: pd.DataFrame) -> Dict[str, Optional[str]]:
    stem = file_path.stem.lower()

    run_tag = None
    if "run_tag" in df.columns and not df["run_tag"].dropna().empty:
        run_tag = str(df["run_tag"].dropna().iloc[0]).strip().lower()

    text = f"{stem} {run_tag or ''}"

    m = re.search(
        r"ui_log_(baseline|delay|loss|disconnect|disconnect_replay|jitter)_r(\d+)_rep(\d+)",
        stem,
    )
    if m:
        raw_cond, rate_hz, rep = m.groups()
        condition = "disconnect_replay" if raw_cond in ("disconnect", "disconnect_replay") else raw_cond
        return {
            "condition": condition,
            "rate_hz": int(rate_hz),
            "repetition": int(rep),
            "run_tag": run_tag,
            "file_name": file_path.name,
        }

    condition = None
    cond_patterns = [
        (r"(?:^|[_\-\s])(baseline|c0)(?:$|[_\-\s])", "baseline"),
        (r"(?:^|[_\-\s])(delay|c1)(?:$|[_\-\s])", "delay"),
        (r"(?:^|[_\-\s])(loss|c2)(?:$|[_\-\s])", "loss"),
        (r"(?:^|[_\-\s])(disconnect|reconnect|c3)(?:$|[_\-\s])", "disconnect_replay"),
        (r"(?:^|[_\-\s])(jitter|c4)(?:$|[_\-\s])", "jitter"),
    ]
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


def infer_nominal_seq_step(seqs: List[int]) -> int:
    if len(seqs) < 2:
        return 1

    diffs = []
    prev = seqs[0]
    for cur in seqs[1:]:
        d = cur - prev
        if d > 0:
            diffs.append(d)
        prev = cur

    if not diffs:
        return 1

    counts = Counter(diffs)
    step, _ = counts.most_common(1)[0]
    return int(step) if step > 0 else 1


def compute_missing_ranges_with_step(seqs: List[int], step: int) -> List[Tuple[int, int, int]]:
    ranges = []
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


def expand_missing_expected_values(ranges: List[Tuple[int, int, int]], step: int) -> set:
    values = set()
    for start, end, _ in ranges:
        if end >= start:
            v = start
            while v <= end:
                values.add(v)
                v += step
    return values


def compute_recovery_time_ms(df: pd.DataFrame) -> float:
    if "event" not in df.columns or "wall_ms" not in df.columns:
        return math.nan

    temp = df.copy()
    temp["wall_ms"] = safe_numeric(temp["wall_ms"])
    temp = temp.sort_values("wall_ms").reset_index(drop=True)

    close_idxs = temp.index[temp["event"] == "ws_close"].tolist()
    if not close_idxs:
        return math.nan

    recoveries = []

    for idx in close_idxs:
        close_t = temp.loc[idx, "wall_ms"]
        after = temp.iloc[idx + 1 :]

        open_rows = after[after["event"] == "ws_open"]
        if open_rows.empty:
            continue
        open_idx = open_rows.index[0]
        open_t = temp.loc[open_idx, "wall_ms"]

        if (open_t - close_t) > 10000:
            continue

        after_open = temp.loc[open_idx + 1 :]

        hist_rows = after_open[
            (after_open["event"] == "history_request")
            & (safe_numeric(after_open["wall_ms"]) - open_t <= 5000)
        ]
        if hist_rows.empty:
            continue

        live_rows = after_open[
            (after_open["event"] == "telemetry") & (after_open["source"] == "live")
        ]
        if live_rows.empty:
            continue

        first_live_t = safe_numeric(live_rows["wall_ms"]).dropna().iloc[0]
        recoveries.append(first_live_t - open_t)

    if not recoveries:
        return math.nan

    return float(sum(recoveries) / len(recoveries))


def analyze_one_file(file_path: Path) -> Dict[str, object]:
    df = pd.read_csv(file_path)
    meta = parse_run_metadata(file_path, df)

    for col in ["wall_ms", "seq", "t_ns", "staleness_ms", "gap_from_prev", "count", "last"]:
        if col in df.columns:
            df[col] = safe_numeric(df[col])

    if "event" not in df.columns:
        raise ValueError(f"{file_path.name}: missing required column 'event'")
    if "source" not in df.columns:
        df["source"] = ""

    df = df.sort_values("wall_ms", na_position="last").reset_index(drop=True)

    live_df = df[(df["event"] == "telemetry") & (df["source"] == "live")].copy()
    replay_df = df[(df["event"] == "telemetry") & (df["source"] == "replay")].copy()

    live_seqs = safe_numeric(live_df["seq"]).dropna().astype(int).tolist()
    replay_seqs = set(safe_numeric(replay_df["seq"]).dropna().astype(int).tolist())

    nominal_step = infer_nominal_seq_step(live_seqs)

    staleness = live_df["staleness_ms"].dropna() if "staleness_ms" in live_df.columns else pd.Series(dtype=float)
    staleness_mean = float(staleness.mean()) if not staleness.empty else math.nan
    staleness_median = float(staleness.median()) if not staleness.empty else math.nan
    staleness_p95 = float(staleness.quantile(0.95)) if not staleness.empty else math.nan
    staleness_max = float(staleness.max()) if not staleness.empty else math.nan

    missing_ranges = compute_missing_ranges_with_step(live_seqs, nominal_step)
    missing_expected = expand_missing_expected_values(missing_ranges, nominal_step)

    gaps_count = len(missing_ranges)
    missing_samples = len(missing_expected)
    largest_gap = max((mc for _, _, mc in missing_ranges), default=0)

    inter_arrival_mean_ms = math.nan
    inter_arrival_std_ms = math.nan
    inter_arrival_p95_ms = math.nan
    if len(live_df) >= 2:
        ia = live_df["wall_ms"].diff().dropna()
        if not ia.empty:
            inter_arrival_mean_ms = float(ia.mean())
            inter_arrival_std_ms = float(ia.std(ddof=1)) if len(ia) > 1 else 0.0
            inter_arrival_p95_ms = float(ia.quantile(0.95))

    recovery_time_ms = compute_recovery_time_ms(df)

    recovered_by_replay = len(missing_expected.intersection(replay_seqs))
    replay_recovery_ratio = (
        recovered_by_replay / len(missing_expected)
        if len(missing_expected) > 0
        else math.nan
    )

    live_count = int(len(live_df))
    replay_count = int(len(replay_df))
    history_requests = int((df["event"] == "history_request").sum()) if "event" in df.columns else 0

    wall_ms = safe_numeric(df["wall_ms"]).dropna()
    duration_s = float((wall_ms.iloc[-1] - wall_ms.iloc[0]) / 1000.0) if len(wall_ms) >= 2 else math.nan

    effective_live_rate = (
        float(live_count / duration_s)
        if pd.notna(duration_s) and duration_s > 0
        else math.nan
    )

    return {
        **meta,
        "nominal_seq_step": nominal_step,
        "duration_s": duration_s,
        "effective_live_rate": effective_live_rate,
        "n_rows_total": int(len(df)),
        "live_count": live_count,
        "replay_count": replay_count,
        "history_requests": history_requests,
        "staleness_mean_ms": staleness_mean,
        "staleness_median_ms": staleness_median,
        "staleness_p95_ms": staleness_p95,
        "staleness_max_ms": staleness_max,
        "gaps_count": gaps_count,
        "missing_samples": missing_samples,
        "largest_gap": largest_gap,
        "inter_arrival_mean_ms": inter_arrival_mean_ms,
        "inter_arrival_std_ms": inter_arrival_std_ms,
        "inter_arrival_p95_ms": inter_arrival_p95_ms,
        "recovery_time_ms": recovery_time_ms,
        "recovered_by_replay": recovered_by_replay,
        "live_missing_total": len(missing_expected),
        "replay_recovery_ratio": replay_recovery_ratio,
    }


def aggregate_runs(runs_df: pd.DataFrame) -> pd.DataFrame:
    group_cols = ["condition", "rate_hz"]
    metric_cols = [
        "nominal_seq_step",
        "duration_s",
        "effective_live_rate",
        "staleness_mean_ms",
        "staleness_median_ms",
        "staleness_p95_ms",
        "staleness_max_ms",
        "gaps_count",
        "missing_samples",
        "largest_gap",
        "inter_arrival_mean_ms",
        "inter_arrival_std_ms",
        "inter_arrival_p95_ms",
        "recovery_time_ms",
        "recovered_by_replay",
        "live_missing_total",
        "replay_recovery_ratio",
        "live_count",
        "replay_count",
    ]

    agg_dict = {col: ["mean", "std"] for col in metric_cols}
    grouped = runs_df.groupby(group_cols, dropna=False).agg(agg_dict)
    grouped.columns = ["_".join([c for c in tup if c]) for tup in grouped.columns]
    grouped = grouped.reset_index()
    return grouped


def prettify_condition(cond: str) -> str:
    mapping = {
        "baseline": "Baseline",
        "delay": "Delay",
        "loss": "Loss",
        "disconnect_replay": "Disconnect+Replay",
        "jitter": "Jitter",
        None: "Unknown",
    }
    return mapping.get(cond, str(cond).replace("_", " ").title())


def label_from_row(row: pd.Series) -> str:
    cond = str(row.get("condition", "unknown"))
    cond_map = {
        "baseline": "baseline",
        "delay": "delay",
        "loss": "loss",
        "disconnect_replay": "disc+replay",
        "jitter": "jitter",
    }
    cond_short = cond_map.get(cond, cond)
    rate = row.get("rate_hz", None)
    if pd.notna(rate):
        return f"{cond_short}\n{int(rate)} Hz"
    return cond_short


def save_staleness_boxplot(all_live_points: pd.DataFrame, out_path: Path) -> None:
    if all_live_points.empty:
        return

    order_df = (
        all_live_points[["condition", "rate_hz"]]
        .drop_duplicates()
        .sort_values(["condition", "rate_hz"], na_position="last")
        .reset_index(drop=True)
    )

    labels = []
    data = []

    for _, row in order_df.iterrows():
        cond = row["condition"]
        rate = row["rate_hz"]
        sub = all_live_points[
            (all_live_points["condition"] == cond) & (all_live_points["rate_hz"] == rate)
        ]["staleness_ms"].dropna()
        if len(sub) == 0:
            continue
        labels.append(label_from_row(row))
        data.append(sub.tolist())

    if not data:
        return

    plt.figure(figsize=(12, 6))
    plt.boxplot(data, labels=labels, showfliers=False)
    plt.ylabel("Staleness at UI (ms)")
    plt.title("Data Staleness by Condition and Telemetry Rate")
    plt.xticks(rotation=25, ha="right")
    plt.tight_layout()
    plt.savefig(out_path, dpi=200)
    plt.close()


def save_barplot(
    agg_df: pd.DataFrame,
    value_col: str,
    title: str,
    ylabel: str,
    out_path: Path,
) -> None:
    if agg_df.empty or value_col not in agg_df.columns:
        return

    plot_df = agg_df.dropna(subset=["condition", "rate_hz"]).sort_values(["condition", "rate_hz"]).copy()
    if plot_df.empty:
        return

    labels = [label_from_row(r) for _, r in plot_df.iterrows()]
    values = plot_df[value_col].tolist()

    plt.figure(figsize=(12, 6))
    plt.bar(range(len(values)), values)
    plt.xticks(range(len(values)), labels, rotation=25, ha="right")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.tight_layout()
    plt.savefig(out_path, dpi=200)
    plt.close()


def save_replay_recovery_ratio_plot(
    agg_df: pd.DataFrame,
    runs_df: pd.DataFrame,
    out_path: Path,
) -> None:
    if agg_df.empty or "replay_recovery_ratio_mean" not in agg_df.columns:
        return

    plot_df = agg_df[
        (agg_df["condition"] == "disconnect_replay")
        & (agg_df["rate_hz"].notna())
        & (agg_df["replay_recovery_ratio_mean"].notna())
    ].copy()

    if plot_df.empty:
        return

    plot_df = plot_df.sort_values("rate_hz")

    labels = [f"{int(rate)} Hz" for rate in plot_df["rate_hz"]]
    means = plot_df["replay_recovery_ratio_mean"].tolist()
    errs = plot_df["replay_recovery_ratio_std"].fillna(0).tolist() if "replay_recovery_ratio_std" in plot_df.columns else [0.0] * len(means)

    x = list(range(len(means)))

    fig, ax = plt.subplots(figsize=(3.0, 2.3))
    bars = ax.bar(
        x,
        means,
        yerr=errs,
        capsize=3,
        width=0.50,
        error_kw={"elinewidth": 1.2, "capthick": 1.2},
    )

    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=8)
    ax.set_xlabel("Telemetry rate", fontsize=8)
    ax.set_ylabel("Replay recovery ratio", fontsize=8)
    ax.tick_params(axis="y", labelsize=8)

    ymax = max(m + e for m, e in zip(means, errs))
    ax.set_ylim(0, max(0.22, ymax * 1.20 + 0.01))

    for bar, mean_val, err_val in zip(bars, means, errs):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            mean_val + err_val + 0.005,
            f"{mean_val:.2f}",
            ha="center",
            va="bottom",
            fontsize=7,
        )

    raw_df = runs_df[
        (runs_df["condition"] == "disconnect_replay")
        & (runs_df["rate_hz"].notna())
        & (runs_df["replay_recovery_ratio"].notna())
    ].copy()

    if not raw_df.empty:
        raw_df = raw_df.sort_values(["rate_hz", "repetition"], na_position="last")
        offsets = [-0.06, 0.0, 0.06, -0.03, 0.03]
        for i, rate in enumerate(plot_df["rate_hz"]):
            vals = raw_df.loc[raw_df["rate_hz"] == rate, "replay_recovery_ratio"].tolist()
            xs = [i + offsets[j % len(offsets)] for j in range(len(vals))]
            ax.plot(xs, vals, "o", markersize=3)

    fig.tight_layout(pad=0.4)
    fig.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)


def save_disconnect_timeline(input_file: Path, out_path: Path) -> None:
    df = pd.read_csv(input_file)
    if "wall_ms" not in df.columns or "event" not in df.columns:
        return

    df["wall_ms"] = safe_numeric(df["wall_ms"])
    df = df.sort_values("wall_ms").reset_index(drop=True)
    if df["wall_ms"].dropna().empty:
        return

    t0 = df["wall_ms"].dropna().iloc[0]
    df["t_rel_s"] = (df["wall_ms"] - t0) / 1000.0

    live = df[(df["event"] == "telemetry") & (df["source"] == "live")].copy()
    replay = df[(df["event"] == "telemetry") & (df["source"] == "replay")].copy()
    closes = df[df["event"] == "ws_close"].copy()
    opens = df[df["event"] == "ws_open"].copy()
    hist = df[df["event"] == "history_request"].copy()

    plt.figure(figsize=(11, 4))
    if not live.empty:
        plt.scatter(live["t_rel_s"], [1] * len(live), s=8, label="live telemetry")
    if not replay.empty:
        plt.scatter(replay["t_rel_s"], [2] * len(replay), s=8, label="replay telemetry")
    if not closes.empty:
        plt.scatter(closes["t_rel_s"], [3] * len(closes), s=30, marker="x", label="ws_close")
    if not opens.empty:
        plt.scatter(opens["t_rel_s"], [4] * len(opens), s=30, marker="o", label="ws_open")
    if not hist.empty:
        plt.scatter(hist["t_rel_s"], [5] * len(hist), s=30, marker="^", label="history_request")

    plt.yticks([1, 2, 3, 4, 5], ["live", "replay", "close", "open", "history"])
    plt.xlabel("Time since run start (s)")
    plt.title(f"Representative Timeline: {input_file.stem}")
    plt.legend(loc="best")
    plt.tight_layout()
    plt.savefig(out_path, dpi=200)
    plt.close()


def collect_live_staleness_points(files: List[Path], runs_df: pd.DataFrame) -> pd.DataFrame:
    rows = []
    meta_by_file = {r["file_name"]: r for _, r in runs_df.iterrows()}

    for f in files:
        df = pd.read_csv(f)
        if "event" not in df.columns or "source" not in df.columns or "staleness_ms" not in df.columns:
            continue

        df["staleness_ms"] = safe_numeric(df["staleness_ms"])
        live = df[(df["event"] == "telemetry") & (df["source"] == "live")].copy()
        if live.empty:
            continue

        meta = meta_by_file.get(f.name, {})
        live["condition"] = meta.get("condition")
        live["rate_hz"] = meta.get("rate_hz")
        rows.append(live[["condition", "rate_hz", "staleness_ms"]])

    if not rows:
        return pd.DataFrame(columns=["condition", "rate_hz", "staleness_ms"])

    return pd.concat(rows, ignore_index=True)


def fmt_mean_std(mean_val, std_val, digits=1, nan_text="--"):
    if pd.isna(mean_val):
        return nan_text
    if pd.isna(std_val):
        return f"{mean_val:.{digits}f}"
    return f"{mean_val:.{digits}f} $\\pm$ {std_val:.{digits}f}"


def fmt_ratio(mean_val, std_val, digits=2, nan_text="--"):
    if pd.isna(mean_val):
        return nan_text
    if pd.isna(std_val):
        return f"{mean_val:.{digits}f}"
    return f"{mean_val:.{digits}f} $\\pm$ {std_val:.{digits}f}"


def build_paper_table_df(agg_df: pd.DataFrame) -> pd.DataFrame:
    rows = []
    plot_df = agg_df.dropna(subset=["condition", "rate_hz"]).sort_values(["condition", "rate_hz"]).copy()

    for _, row in plot_df.iterrows():
        rows.append({
            "Condition": prettify_condition(row["condition"]),
            "Rate (Hz)": int(row["rate_hz"]) if pd.notna(row["rate_hz"]) else "--",
            "Staleness median (ms)": fmt_mean_std(
                row.get("staleness_median_ms_mean"),
                row.get("staleness_median_ms_std"),
                digits=1,
            ),
            "Staleness p95 (ms)": fmt_mean_std(
                row.get("staleness_p95_ms_mean"),
                row.get("staleness_p95_ms_std"),
                digits=1,
            ),
            "Missing samples": fmt_mean_std(
                row.get("missing_samples_mean"),
                row.get("missing_samples_std"),
                digits=1,
            ),
            "Recovery time (ms)": fmt_mean_std(
                row.get("recovery_time_ms_mean"),
                row.get("recovery_time_ms_std"),
                digits=1,
            ),
            "Replay recovery ratio": fmt_ratio(
                row.get("replay_recovery_ratio_mean"),
                row.get("replay_recovery_ratio_std"),
                digits=2,
            ),
        })

    return pd.DataFrame(rows)


def build_summary_table_df(agg_df: pd.DataFrame) -> pd.DataFrame:
    rows = []
    plot_df = agg_df.dropna(subset=["condition", "rate_hz"]).sort_values(["condition", "rate_hz"]).copy()

    for _, row in plot_df.iterrows():
        cond = row["condition"]
        rows.append({
            "Condition": "Disc.+Replay" if cond == "disconnect_replay" else prettify_condition(cond),
            "Nominal rate (Hz)": int(row["rate_hz"]) if pd.notna(row["rate_hz"]) else "--",
            "Effective live rate (Hz)": fmt_mean_std(
                row.get("effective_live_rate_mean"),
                row.get("effective_live_rate_std"),
                digits=1,
            ),
            "Recovery time (ms)": (
                fmt_mean_std(
                    row.get("recovery_time_ms_mean"),
                    row.get("recovery_time_ms_std"),
                    digits=1,
                )
                if cond == "disconnect_replay" else "--"
            ),
            "Replay ratio": (
                fmt_ratio(
                    row.get("replay_recovery_ratio_mean"),
                    row.get("replay_recovery_ratio_std"),
                    digits=2,
                )
                if cond == "disconnect_replay" else "--"
            ),
        })

    return pd.DataFrame(rows)


def write_latex_table(
    table_df: pd.DataFrame,
    out_path: Path,
    caption: str,
    label: str,
    compact: bool = False,
) -> None:
    if compact:
        align = "llccccc"
        header = (
            "\\begin{table}[t]\n"
            "\\centering\n"
            f"\\caption{{{caption}}}\n"
            f"\\label{{{label}}}\n"
            "\\scriptsize\n"
            "\\begin{tabular}{" + align + "}\n"
            "\\hline\n"
            "Condition & Rate & Med. stale & P95 stale & Missing & Recovery & Replay ratio \\\\\n"
            "& (Hz) & (ms) & (ms) & samples & (ms) & \\\\\n"
            "\\hline\n"
        )
        body_lines = []
        for _, row in table_df.iterrows():
            body_lines.append(
                f"{row['Condition']} & "
                f"{row['Rate (Hz)']} & "
                f"{row['Staleness median (ms)']} & "
                f"{row['Staleness p95 (ms)']} & "
                f"{row['Missing samples']} & "
                f"{row['Recovery time (ms)']} & "
                f"{row['Replay recovery ratio']} \\\\"
            )
        footer = "\n\\hline\n\\end{tabular}\n\\end{table}\n"
        out_path.write_text(header + "\n".join(body_lines) + footer, encoding="utf-8")
        return

    align = "llccccc"
    header = (
        "\\begin{table*}[t]\n"
        "\\centering\n"
        f"\\caption{{{caption}}}\n"
        f"\\label{{{label}}}\n"
        "\\small\n"
        "\\begin{tabular}{" + align + "}\n"
        "\\hline\n"
        "Condition & Rate (Hz) & Staleness median (ms) & Staleness p95 (ms) & Missing samples & Recovery time (ms) & Replay recovery ratio \\\\\n"
        "\\hline\n"
    )

    body_lines = []
    for _, row in table_df.iterrows():
        body_lines.append(
            f"{row['Condition']} & "
            f"{row['Rate (Hz)']} & "
            f"{row['Staleness median (ms)']} & "
            f"{row['Staleness p95 (ms)']} & "
            f"{row['Missing samples']} & "
            f"{row['Recovery time (ms)']} & "
            f"{row['Replay recovery ratio']} \\\\"
        )

    footer = "\n\\hline\n\\end{tabular}\n\\end{table*}\n"
    out_path.write_text(header + "\n".join(body_lines) + footer, encoding="utf-8")


def write_latex_summary_table(table_df: pd.DataFrame, out_path: Path) -> None:
    header = (
        "\\begin{table}[t]\n"
        "\\centering\n"
        "\\caption{Summary of interface-level behavior across evaluated communication conditions. "
        "Reported values are mean $\\pm$ standard deviation across three repeated runs.}\n"
        "\\label{tab:results_main}\n"
        "\\scriptsize\n"
        "\\begin{tabular}{@{}llccc@{}}\n"
        "\\hline\n"
        "Condition & Nominal & Effective live & Recovery & Replay \\\\\n"
        " & rate (Hz) & rate (Hz) & time (ms) & ratio \\\\\n"
        "\\hline\n"
    )

    body_lines = []
    for _, row in table_df.iterrows():
        body_lines.append(
            f"{row['Condition']} & "
            f"{row['Nominal rate (Hz)']} & "
            f"{row['Effective live rate (Hz)']} & "
            f"{row['Recovery time (ms)']} & "
            f"{row['Replay ratio']} \\\\"
        )

    footer = "\n\\hline\n\\end{tabular}\n\\end{table}\n"
    out_path.write_text(header + "\n".join(body_lines) + footer, encoding="utf-8")


def main() -> None:
    args = parse_args()

    input_dir = Path(args.input_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    files = sorted(input_dir.glob(args.pattern))
    if not files:
        raise SystemExit(f"No CSV files found in {input_dir} matching {args.pattern}")

    run_results = []
    failed = []

    for file_path in files:
        try:
            res = analyze_one_file(file_path)
            run_results.append(res)
            print(f"[OK] {file_path.name}")
        except Exception as e:
            failed.append((file_path.name, str(e)))
            print(f"[FAIL] {file_path.name}: {e}")

    if not run_results:
        raise SystemExit("No runs were successfully analyzed.")

    runs_df = pd.DataFrame(run_results)
    runs_df = runs_df.sort_values(
        ["condition", "rate_hz", "repetition", "file_name"],
        na_position="last",
    ).reset_index(drop=True)

    agg_df = aggregate_runs(runs_df)

    runs_csv = output_dir / "run_metrics.csv"
    agg_csv = output_dir / "aggregated_metrics.csv"
    fail_csv = output_dir / "failed_files.csv"

    runs_df.to_csv(runs_csv, index=False)
    agg_df.to_csv(agg_csv, index=False)

    if failed:
        pd.DataFrame(failed, columns=["file_name", "error"]).to_csv(fail_csv, index=False)

    live_points = collect_live_staleness_points(files, runs_df)

    save_staleness_boxplot(
        live_points,
        output_dir / "fig_staleness_boxplot.png",
    )

    save_barplot(
        agg_df,
        "missing_samples_mean",
        "Missing Live Samples by Condition and Telemetry Rate",
        "Missing samples (mean across repetitions)",
        output_dir / "fig_missing_samples_bar.png",
    )

    save_replay_recovery_ratio_plot(
        agg_df,
        runs_df,
        output_dir / "fig_replay_recovery_ratio_bar.png",
    )

    save_barplot(
        agg_df,
        "recovery_time_ms_mean",
        "Recovery Time After Reconnection",
        "Recovery time (ms)",
        output_dir / "fig_recovery_time_bar.png",
    )

    disconnect_candidates = [
        f for f in files
        if re.search(r"(disconnect|reconnect|c3)", f.stem.lower())
    ]
    if disconnect_candidates:
        try:
            save_disconnect_timeline(
                disconnect_candidates[0],
                output_dir / "fig_disconnect_timeline.png",
            )
        except Exception as e:
            print(f"[WARN] Timeline figure failed: {e}")

    table_df = build_paper_table_df(agg_df)
    table_df.to_csv(output_dir / "table_results.csv", index=False)

    write_latex_table(
        table_df,
        output_dir / "table_results.tex",
        caption="Aggregated interface-level observability metrics across communication conditions. Reported values are mean $\\pm$ standard deviation across repeated runs.",
        label="tab:results_main_full",
        compact=False,
    )

    write_latex_table(
        table_df,
        output_dir / "table_results_compact.tex",
        caption="Compact summary of the main observability metrics across evaluated conditions. Reported values are mean $\\pm$ standard deviation across repeated runs.",
        label="tab:results_compact",
        compact=True,
    )

    summary_df = build_summary_table_df(agg_df)
    summary_df.to_csv(output_dir / "table_summary_results.csv", index=False)

    write_latex_summary_table(
        summary_df,
        output_dir / "table_summary_results.tex",
    )

    print("\nOutputs written to:")
    print(f"  {runs_csv}")
    print(f"  {agg_csv}")
    print(f"  {output_dir / 'table_results.csv'}")
    print(f"  {output_dir / 'table_results.tex'}")
    print(f"  {output_dir / 'table_results_compact.tex'}")
    print(f"  {output_dir / 'table_summary_results.csv'}")
    print(f"  {output_dir / 'table_summary_results.tex'}")
    print(f"  {output_dir / 'fig_staleness_boxplot.png'}")
    print(f"  {output_dir / 'fig_missing_samples_bar.png'}")
    print(f"  {output_dir / 'fig_replay_recovery_ratio_bar.png'}")
    print(f"  {output_dir / 'fig_recovery_time_bar.png'}")
    if disconnect_candidates:
        print(f"  {output_dir / 'fig_disconnect_timeline.png'}")
    if failed:
        print(f"  {fail_csv}")


if __name__ == "__main__":
    main()