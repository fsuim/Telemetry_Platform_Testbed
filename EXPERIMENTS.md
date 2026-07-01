# Experiment protocol: reconnect-only versus replay-assisted recovery

This document describes the current reproducible experiment for the telemetry platform.

The goal is to measure interface-level telemetry continuity during a planned gateway-to-UI WebSocket disconnection and to compare two post-disconnection recovery modes:

1. `reconnect_only`: the UI reconnects to the live stream, but no history/replay request is allowed.
2. `disconnect_replay`: the UI reconnects and requests a recent persisted replay window.

The experiment isolates the UI-facing path. Telemetry is still received and persisted by the gateway while the browser UI is disconnected, so replay can be evaluated using already stored samples.

## 1. Experimental design

| Parameter | Value |
|---|---|
| Recovery modes | `reconnect_only`, `disconnect_replay` |
| Telemetry rates | `25`, `50`, `75`, `100`, `150` Hz |
| Repetitions | `10` per rate per mode |
| Total runs | `2 × 5 × 10 = 100` |
| Run duration | `60` s |
| WebSocket cut start | `20` s |
| WebSocket cut duration | `3` s |
| Post-run wait | `8` s |
| MQTT broker | `127.0.0.1:1883` |
| WebSocket gateway | `localhost:8080` |
| UI HTTP server | `http://localhost:8000` |

Replay-window size is proportional to the planned outage duration:

| Rate | Replay window |
|---:|---:|
| 25 Hz | 75 samples |
| 50 Hz | 150 samples |
| 75 Hz | 225 samples |
| 100 Hz | 300 samples |
| 150 Hz | 450 samples |

The reconnect-only baseline uses `Last N = 0` and blocks history/replay requests. Therefore, replay-recovered samples and replay recovery ratio are zero by definition in this mode.

## 2. Metrics

The analysis computes metrics from exported browser/UI CSV logs:

| Metric | Meaning |
|---|---|
| `effective_live_rate` | Live telemetry samples observed by the UI divided by observation duration. |
| `live_missing_total` | Sequence identifiers missing from the UI live stream inside the disconnect-and-recovery target interval. |
| `recovered_by_replay` | Live-missing sequence identifiers later observed through replay. |
| `unrecovered_context` | Live-missing sequence identifiers not recovered through replay. |
| `replay_recovery_ratio` | `recovered_by_replay / live_missing_total`. |
| `recovery_time_ms` | Time from WebSocket reopening to stable live delivery. |
| `relative_timestamp_offset_*` | UI timestamp minus gateway timestamp, interpreted as a relative freshness diagnostic. |

The sequence step is inferred per run. The analysis does not assume that sequence identifiers always increment by one.

## 3. Prepare the environment

Install system dependencies:

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  mosquitto \
  mosquitto-clients \
  libmosquitto-dev \
  protobuf-c-compiler \
  libprotobuf-c-dev \
  sqlite3 \
  libsqlite3-dev \
  python3 \
  python3-pip
```

Install Python dependencies:

```bash
python3 -m pip install --user pandas matplotlib playwright
python3 -m playwright install chromium
```

On WSL/Ubuntu, install browser dependencies if needed:

```bash
python3 -m playwright install-deps chromium
```

Build the C binaries:

```bash
make clean
make
```

The build writes object files to `build/` and executable binaries to `bin/`:

```text
bin/robot_sim
bin/telemetry_gateway
bin/state_dump
```

The generated protobuf-c files are already included. Run `make proto` only after editing `proto/robot_telemetry.proto`.

Start Mosquitto:

```bash
sudo service mosquitto start
```

Create output directories:

```bash
mkdir -p exp/db exp/ui_logs exp/results/reconnect_only_vs_replay paper
```

## 4. Run the automated matrix

Run from the repository root.

### 4.1 Reconnect-only baseline

```bash
REPS=10 RATES="25 50 75 100 150" \
bash experiments/disconnect_replay_rate_sweep/run_reconnect_only_matrix_auto.sh
```

This script performs all reconnect-only runs. For each run it:

1. starts `robot_sim` at the selected rate;
2. starts `telemetry_gateway` with a planned WebSocket cut at 20 s for 3 s;
3. opens the browser UI with Playwright;
4. forces `Last N = 0`;
5. blocks history/replay requests;
6. exports the UI log to `exp/ui_logs/`;
7. validates that no replay rows or history requests are present.

Expected output files follow this pattern:

```text
exp/ui_logs/ui_log_reconnect_only_r25_rep1.csv
exp/ui_logs/ui_log_reconnect_only_r25_rep2.csv
...
exp/ui_logs/ui_log_reconnect_only_r150_rep10.csv
```

### 4.2 Replay-assisted condition

```bash
REPS=10 RATES="25 50 75 100 150" \
bash experiments/disconnect_replay_rate_sweep/run_disconnect_replay_matrix_auto.sh
```

This script performs all replay-assisted runs. For each run it:

1. starts `robot_sim` at the selected rate;
2. starts `telemetry_gateway` with the same planned WebSocket cut;
3. opens the browser UI with Playwright;
4. sets `Last N = rate_hz × 3`;
5. waits for disconnection, reconnection, and automatic history loading;
6. exports the UI log to `exp/ui_logs/`;
7. validates the number of replay rows and history events.

Expected output files follow this pattern:

```text
exp/ui_logs/ui_log_disconnect_replay_r25_rep1.csv
exp/ui_logs/ui_log_disconnect_replay_r25_rep2.csv
...
exp/ui_logs/ui_log_disconnect_replay_r150_rep10.csv
```

## 5. Rerun a subset

Use `RATES`, `START_REP`, and `END_REP` to rerun only part of the matrix.

Example: rerun only replay-assisted 150 Hz repetition 7 with a visible browser:

```bash
HEADLESS=0 REPS=10 RATES="150" START_REP=7 END_REP=7 \
bash experiments/disconnect_replay_rate_sweep/run_disconnect_replay_matrix_auto.sh
```

Example: rerun reconnect-only 25 Hz repetitions 1 to 3:

```bash
REPS=10 RATES="25" START_REP=1 END_REP=3 \
bash experiments/disconnect_replay_rate_sweep/run_reconnect_only_matrix_auto.sh
```

## 6. Runner configuration reference

Both automated scripts accept the following environment variables:

| Variable | Default | Meaning |
|---|---:|---|
| `STUDY_ID` | script-specific | Prefix used in some process logs |
| `RATES` | `25 50 75 100 150` | Space-separated telemetry rates |
| `REPS` | `10` | Number of repetitions per rate |
| `START_REP` | `1` | First repetition to run |
| `END_REP` | `$REPS` | Last repetition to run |
| `RUN_DURATION_S` | `60` | Duration of telemetry collection |
| `POST_RUN_WAIT_S` | `8` | Extra wait before UI-log export |
| `MIN_LIVE_FRACTION` | `0.50` | Minimum live-row validation fraction |
| `CUT_START_S` | `20` | Planned cut start in seconds |
| `CUT_DURATION_S` | `3` | Planned cut duration in seconds |
| `MQTT_HOST` | `127.0.0.1` | MQTT broker host |
| `MQTT_PORT` | `1883` | MQTT broker port |
| `WS_PORT` | `8080` | WebSocket port |
| `UI_HTTP_PORT` | `8000` | UI HTTP server port |
| `UI_DIR` | `./ui` | UI directory served by Python HTTP server |
| `DB_DIR` | `./exp/db` | Databases and process logs |
| `UI_LOG_DIR` | `./exp/ui_logs` | Exported browser/UI CSV logs |
| `HEADLESS` | `1` | `1` = headless browser, `0` = visible browser |
| `START_UI_SERVER` | `1` | `1` = runner starts UI server |
| `ROBOT_BIN` | `./bin/robot_sim` | Path to the simulator binary used by the runners |
| `GATEWAY_BIN` | `./bin/telemetry_gateway` | Path to the gateway binary used by the runners |

Selectors can be overridden if the UI markup changes:

```bash
UI_HOST_SELECTOR="#host" \
UI_PORT_SELECTOR="#port" \
UI_PATH_SELECTOR="#path" \
UI_LAST_N_SELECTOR="#lastN" \
UI_CONNECT_SELECTOR="#connectBtn" \
UI_EXPORT_SELECTOR="#exportBtn" \
bash experiments/disconnect_replay_rate_sweep/run_disconnect_replay_matrix_auto.sh
```

## 7. Analyze logs and generate figures/tables

After all UI logs are available, run:

```bash
python3 experiments/disconnect_replay_rate_sweep/analyze_rate_sweep_with_baseline.py \
  --input-dir ./exp/ui_logs \
  --output-dir ./exp/results/reconnect_only_vs_replay \
  --paper-output-dir ./paper \
  --pattern "ui_log_*.csv"
```

Expected console output:

```text
Analyzed 100 files. Outputs written to: exp/results/reconnect_only_vs_replay
WARNING: Some logs contain validation warnings. See validation_warnings.csv
Publication-ready files also copied to: ./paper
```

The warning line appears only when at least one log has a validation warning. Check `validation_warnings.csv` before using the aggregated results.

Main output files:

```text
aggregated_metrics.csv
run_metrics.csv
validation_warnings.csv
table_reconnect_only_vs_replay.tex
table_disconnect_replay_rate_sweep.tex
table_summary_results.tex
fig_disconnect_replay_summary_panel.png
fig_effective_live_rate_sweep.png
fig_missing_recovered_samples_sweep.png
fig_recovered_vs_missing.pdf
fig_recovered_vs_missing.png
fig_recovery_time_sweep.png
fig_replay_recovery_ratio_sweep.png
fig_staleness_boxplot.png
fig_unrecovered_context_comparison.png
```

## 8. Validation checks

### Reconnect-only

The reconnect-only runner requires:

- at least two `ws_open` events;
- enough live telemetry rows;
- zero `telemetry,replay` rows;
- zero `history_request` rows;
- zero `history_batch_loaded` rows.

### Replay-assisted

The replay-assisted runner requires:

- at least two `ws_open` events;
- enough live telemetry rows;
- exactly one `history_request` row;
- exactly one `history_batch_loaded` row;
- replay rows equal to `rate_hz × CUT_DURATION_S`.

If a validation error occurs, inspect:

```text
exp/db/<run_tag>_gateway.log
exp/db/<run_tag>_robot.log
```

Then rerun the affected `rate` and `rep` only.

## 9. Manual protocol for one run

The automated scripts are preferred. Use the manual protocol only for debugging or extending the experiment.

### 9.1 Reconnect-only, 50 Hz, rep 1

Terminal 1:

```bash
./bin/robot_sim --host 127.0.0.1 --port 1883 --rate 50 --state-rate 50
```

Terminal 2:

```bash
./bin/telemetry_gateway \
  --mqtt-host 127.0.0.1 \
  --mqtt-port 1883 \
  --db ./exp/db/reconnect_only_r50_rep1.db \
  --ws-port 8080 \
  --ui-cut-start-s 20 \
  --ui-cut-duration-s 3 \
  --ui-reconnect-mode close \
  --ui-exp-tag reconnect_only_r50_rep1
```

Terminal 3:

```bash
python3 -m http.server 8000 --directory ./ui
```

Browser:

```text
http://localhost:8000
Host = localhost
Port = 8080
Path = /
Last N samples = 0
```

Connect, keep the UI open for the full run, do not request history, then export:

```text
exp/ui_logs/ui_log_reconnect_only_r50_rep1.csv
```

### 9.2 Replay-assisted, 50 Hz, rep 1

Use the same simulator and UI server, but start the gateway with:

```bash
./bin/telemetry_gateway \
  --mqtt-host 127.0.0.1 \
  --mqtt-port 1883 \
  --db ./exp/db/disconnect_replay_r50_rep1.db \
  --ws-port 8080 \
  --ui-cut-start-s 20 \
  --ui-cut-duration-s 3 \
  --ui-reconnect-mode close \
  --ui-exp-tag disconnect_replay_r50_rep1
```

Browser:

```text
Last N samples = 150
```

Connect, keep the UI open through the planned cut and reconnection, wait for history loading, then export:

```text
exp/ui_logs/ui_log_disconnect_replay_r50_rep1.csv
```

For other rates, replace the simulator rate, gateway `--ui-exp-tag`, database path, exported file name, and replay window according to the table in Section 1.

## 10. Reproducibility checklist

Before reporting results, record:

- repository commit or ZIP version;
- operating system and whether WSL was used;
- Mosquitto version;
- Python version;
- Playwright version;
- exact `RATES`, `REPS`, `CUT_START_S`, `CUT_DURATION_S`, `RUN_DURATION_S`, and `POST_RUN_WAIT_S`;
- exact analysis command;
- whether `validation_warnings.csv` is empty;
- any failed or manually repeated runs.

A result set is reproducible when the raw UI logs in `exp/ui_logs/`, the generated `run_metrics.csv`, the generated `aggregated_metrics.csv`, and the analysis command are all preserved together.

## 11. Cleaning and restarting

Remove generated experiment artifacts:

```bash
rm -rf exp/db exp/ui_logs exp/results/reconnect_only_vs_replay paper
mkdir -p exp/db exp/ui_logs exp/results/reconnect_only_vs_replay paper
```

Stop stale processes if needed:

```bash
ps aux | grep -E 'robot_sim|telemetry_gateway|http.server' | grep -v grep
fuser -n tcp 8080
fuser -n tcp 8000
```

Rebuild:

```bash
make clean
make
```
