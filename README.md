# Telemetry_Platform_Testbed

A lightweight telemetry platform for **interface-level evaluation of supervisory telemetry systems** under degraded communication conditions.

This repository accompanies our IECON 2026 paper on **Replay-Assisted Observability Assessment of Industrial Robotic Telemetry Interfaces Under Communication Impairments**. It is based on [`Telemetry_Testbed`](https://github.com/fsuim/Telemetry_Testbed), but the **experimental design, impairment scenarios, UI logging workflow, and evaluation purpose are different** in this version.

## Overview

The platform has three main parts:

- **`robot_sim`**  
  Publishes telemetry snapshots to MQTT.

- **`telemetry_gateway`**  
  Subscribes to MQTT, stores telemetry in SQLite, and exposes:
  - a **live WebSocket stream**
  - a **history/replay interface**

- **`ui/`**  
  Browser-based supervisory interface used to:
  - monitor live telemetry
  - request recent history
  - export UI-side CSV logs

Data path:

```text
robot_sim -> MQTT broker -> telemetry_gateway -> SQLite + WebSocket -> browser UI
```

## What this repository is for

This testbed is designed to evaluate what the **supervisory interface actually observes** when communication is degraded.

The experiments used in the paper focus on:

- **Baseline**
- **Delay**
- **Loss**
- **Disconnect + Replay**

The paper reports interface-facing metrics such as:

- effective live update rate
- recovery time after reconnection
- replay recovery ratio
- relative staleness

## Related repositories

- This repository: `Telemetry_Platform_Testbed`
- Base repository: [`Telemetry_Testbed`](https://github.com/fsuim/Telemetry_Testbed)

## Requirements

Tested in:

- Windows 11
- WSL2
- Ubuntu

Main dependencies:

- `mosquitto`
- `libmosquitto-dev`
- `sqlite3`
- `libsqlite3-dev`
- `protobuf-c-compiler`
- `libprotobuf-c-dev`
- Python 3
- `pandas`
- `matplotlib`

Example installation on Ubuntu / WSL2:

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

python3 -m pip install --user pandas matplotlib
```

## Build

Clone and build:

```bash
git clone https://github.com/fsuim/Telemetry_Platform_Testbed.git
cd Telemetry_Platform_Testbed
make
```

Expected binaries:

- `./robot_sim`
- `./telemetry_gateway`

## Quick start

### 1. Start the MQTT broker

```bash
sudo service mosquitto start
```

### 2. Start the simulator

Example at 50 Hz:

```bash
./robot_sim --host 127.0.0.1 --port 1883 --rate 50 --state-rate 50
```

### 3. Start the gateway

```bash
./telemetry_gateway \
  --mqtt-host 127.0.0.1 \
  --mqtt-port 1883 \
  --db telemetry.db \
  --ws-port 8080
```

### 4. Start the UI

```bash
cd ui
python3 -m http.server 8000
```

Open:

```text
http://localhost:8000
```

Use:

- **Host:** `localhost`
- **Port:** `8080`
- **Path:** `/`

Then click **Connect**.

## Experiment protocol

The paper uses:

- **4 conditions**
- **2 telemetry rates**
- **3 repetitions**

Total:

- **24 runs**

Conditions:

1. Baseline
2. Delay
3. Loss
4. Disconnect + Replay

Rates:

- 50 Hz
- 100 Hz

Each run lasts:

- **60 seconds**

Replay/history setting in the UI:

- **150 samples** for 50 Hz
- **300 samples** for 100 Hz

Full step-by-step instructions are in [`EXPERIMENTS.md`](./EXPERIMENTS.md).

## Analyze exported UI logs

After collecting the UI CSV logs, run:

```bash
python3 analyze_ui_logs_with_latex.py \
  --input-dir ./exp/ui_logs \
  --output-dir ./exp/results \
  --pattern "ui_log_*.csv"
```

Typical outputs:

- aggregated CSV metrics
- LaTeX tables
- PNG figures used in the paper

Examples:

- `aggregated_metrics.csv`
- `table_results_compact.tex`
- `table_summary_results.tex`
- `fig_recovery_time_bar.png`
- `fig_replay_recovery_ratio_bar.png`
- `fig_staleness_boxplot.png`

## Repository purpose

Compared with the original `Telemetry_Testbed`, this repository is intended specifically for:

- controlled communication impairment experiments
- supervisory UI logging
- replay-assisted recovery analysis
- paper-oriented figure/table generation

## Troubleshooting

### UI does not connect

Check that:

- Mosquitto is running
- `telemetry_gateway` is listening on port `8080`
- the browser uses `localhost:8080`

### No telemetry appears

Check the MQTT snapshot stream:

```bash
mosquitto_sub -t "/robot/v1/telemetry/state" -C 1 | wc -c
```

### Replay does not happen after disconnect

Make sure the disconnect condition is started with:

- `--ui-cut-start-s 20`
- `--ui-cut-duration-s 3`
- `--ui-reconnect-mode close`

and that the UI remains open for the full run.

## License

Please follow the repository license and cite the corresponding paper if you reuse this setup.
