# Experiments

This document describes the experiment protocol used in the AIM 2026 paper.

## Summary

The full experiment matrix contains:

- **4 conditions**
- **2 telemetry rates**
- **3 repetitions**

Total:

- **24 runs**

## Common settings

- Run duration: **60 s**
- MQTT broker: **1883**
- WebSocket gateway: **8080**
- UI HTTP server: **8000**

UI replay/history size:

- **150 samples** for 50 Hz
- **300 samples** for 100 Hz

Recommended folder structure:

```text
exp/
  db/
  ui_logs/
  results/
```

## General workflow

For each run:

1. Start `robot_sim`
2. Start `telemetry_gateway` with the correct condition flags
3. Open the UI
4. Set **Last N samples**
5. Click **Connect**
6. Let the system run for **60 s**
7. Click **Export UI log**
8. Save the CSV with the expected file name

---

# A. Baseline

## 50 Hz

```bash
./robot_sim --host 127.0.0.1 --port 1883 --rate 50 --state-rate 50
```

```bash
./telemetry_gateway \
  --mqtt-host 127.0.0.1 \
  --mqtt-port 1883 \
  --db ./exp/db/baseline_r50_rep1.db \
  --ws-port 8080 \
  --ui-exp-tag baseline_r50_rep1
```

UI:

- `Last N samples = 150`
- click **Connect**
- run for **60 s**
- click **Export UI log**
- save as `ui_log_baseline_r50_rep1.csv`

## 100 Hz

```bash
./robot_sim --host 127.0.0.1 --port 1883 --rate 100 --state-rate 100
```

```bash
./telemetry_gateway \
  --mqtt-host 127.0.0.1 \
  --mqtt-port 1883 \
  --db ./exp/db/baseline_r100_rep1.db \
  --ws-port 8080 \
  --ui-exp-tag baseline_r100_rep1
```

UI:

- `Last N samples = 300`
- run for **60 s**
- export as `ui_log_baseline_r100_rep1.csv`

Repeat the same logic for:

- `rep2`
- `rep3`

---

# B. Delay

This condition injects **100 ms** of UI-facing delay.

## 50 Hz

```bash
./robot_sim --host 127.0.0.1 --port 1883 --rate 50 --state-rate 50
```

```bash
./telemetry_gateway \
  --mqtt-host 127.0.0.1 \
  --mqtt-port 1883 \
  --db ./exp/db/delay_r50_rep1.db \
  --ws-port 8080 \
  --ui-delay-ms 100 \
  --ui-exp-tag delay_r50_rep1
```

UI:

- `Last N samples = 150`
- run for **60 s**
- export as `ui_log_delay_r50_rep1.csv`

## 100 Hz

```bash
./robot_sim --host 127.0.0.1 --port 1883 --rate 100 --state-rate 100
```

```bash
./telemetry_gateway \
  --mqtt-host 127.0.0.1 \
  --mqtt-port 1883 \
  --db ./exp/db/delay_r100_rep1.db \
  --ws-port 8080 \
  --ui-delay-ms 100 \
  --ui-exp-tag delay_r100_rep1
```

UI:

- `Last N samples = 300`
- export as `ui_log_delay_r100_rep1.csv`

Repeat for `rep2` and `rep3`.

---

# C. Loss

This condition injects **5% packet loss** on the UI-facing delivery path.

## 50 Hz

```bash
./robot_sim --host 127.0.0.1 --port 1883 --rate 50 --state-rate 50
```

```bash
./telemetry_gateway \
  --mqtt-host 127.0.0.1 \
  --mqtt-port 1883 \
  --db ./exp/db/loss_r50_rep1.db \
  --ws-port 8080 \
  --ui-drop-pct 5 \
  --ui-exp-tag loss_r50_rep1
```

UI:

- `Last N samples = 150`
- export as `ui_log_loss_r50_rep1.csv`

## 100 Hz

```bash
./robot_sim --host 127.0.0.1 --port 1883 --rate 100 --state-rate 100
```

```bash
./telemetry_gateway \
  --mqtt-host 127.0.0.1 \
  --mqtt-port 1883 \
  --db ./exp/db/loss_r100_rep1.db \
  --ws-port 8080 \
  --ui-drop-pct 5 \
  --ui-exp-tag loss_r100_rep1
```

UI:

- `Last N samples = 300`
- export as `ui_log_loss_r100_rep1.csv`

Repeat for `rep2` and `rep3`.

---

# D. Disconnect + Replay

This is the main condition used in the paper.

It introduces:

- a cut starting at **20 s**
- a cut duration of **3 s**
- automatic UI reconnection
- replay/history request after reconnection

## 50 Hz

```bash
./robot_sim --host 127.0.0.1 --port 1883 --rate 50 --state-rate 50
```

```bash
./telemetry_gateway \
  --mqtt-host 127.0.0.1 \
  --mqtt-port 1883 \
  --db ./exp/db/disconnect_r50_rep1.db \
  --ws-port 8080 \
  --ui-cut-start-s 20 \
  --ui-cut-duration-s 3 \
  --ui-reconnect-mode close \
  --ui-exp-tag disconnect_r50_rep1
```

UI:

- `Last N samples = 150`
- click **Connect**
- run for **60 s**
- the connection should drop near **20 s**
- the UI should reconnect automatically
- the UI should request history
- export as `ui_log_disconnect_r50_rep1.csv`

## 100 Hz

```bash
./robot_sim --host 127.0.0.1 --port 1883 --rate 100 --state-rate 100
```

```bash
./telemetry_gateway \
  --mqtt-host 127.0.0.1 \
  --mqtt-port 1883 \
  --db ./exp/db/disconnect_r100_rep1.db \
  --ws-port 8080 \
  --ui-cut-start-s 20 \
  --ui-cut-duration-s 3 \
  --ui-reconnect-mode close \
  --ui-exp-tag disconnect_r100_rep1
```

UI:

- `Last N samples = 300`
- export as `ui_log_disconnect_r100_rep1.csv`

Repeat for `rep2` and `rep3`.

---

# Repetition map

Use these run tags:

- `baseline_r50_rep1`
- `baseline_r50_rep2`
- `baseline_r50_rep3`
- `baseline_r100_rep1`
- `baseline_r100_rep2`
- `baseline_r100_rep3`
- `delay_r50_rep1`
- `delay_r50_rep2`
- `delay_r50_rep3`
- `delay_r100_rep1`
- `delay_r100_rep2`
- `delay_r100_rep3`
- `loss_r50_rep1`
- `loss_r50_rep2`
- `loss_r50_rep3`
- `loss_r100_rep1`
- `loss_r100_rep2`
- `loss_r100_rep3`
- `disconnect_r50_rep1`
- `disconnect_r50_rep2`
- `disconnect_r50_rep3`
- `disconnect_r100_rep1`
- `disconnect_r100_rep2`
- `disconnect_r100_rep3`

---

# Analysis

After collecting all UI logs:

```bash
python3 analyze_ui_logs_with_latex.py \
  --input-dir ./exp/ui_logs \
  --output-dir ./exp/results \
  --pattern "ui_log_*.csv"
```

Generated outputs typically include:

- `aggregated_metrics.csv`
- `table_results_compact.tex`
- `table_summary_results.tex`
- `fig_disconnect_timeline.png`
- `fig_recovery_time_bar.png`
- `fig_replay_recovery_ratio_bar.png`
- `fig_staleness_boxplot.png`
