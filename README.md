# Artifact Evaluation Guide: MQTT + Protobuf-C Telemetry Testbed

This repository contains a **lightweight, reproducible telemetry testbed** used in our paper **submitted** to the [Fusion2026](https://www.ntnu.edu/fusion2026) conference.  

## What you will test

You will run three components:

1. **`robot_sim`** — publishes telemetry to an MQTT broker:
   - **RAW sensor topics** (binary payloads on separate topics)
   - **Snapshot** (a consolidated Protobuf message published periodically)

2. **`telemetry_gateway`** — subscribes to snapshots, **logs to SQLite (WAL)**, and exposes a **WebSocket** stream (JSON) for the UI:
   - live stream
   - “last N” replay from SQLite

3. **`ui/`** — a static web page served over HTTP that connects to the gateway WebSocket and visualizes telemetry.

---

## System Requirements

### Tested environment
- **Windows 11 + WSL2 + Ubuntu**
- Local broker, simulator, gateway and UI on the same machine (loopback)

### Dependencies (Ubuntu / WSL2)
- `build-essential` (gcc/make)
- Mosquitto: `mosquitto`, `libmosquitto-dev`, `mosquitto-clients`
- Protobuf-C: `protobuf-c-compiler`, `libprotobuf-c-dev`
- SQLite: `sqlite3`, `libsqlite3-dev`
- Python 3 + pip (for analysis/plots)

---

## Installation (Ubuntu / WSL2)

Recommended “combo” command:

```bash
sudo apt update
sudo apt install -y build-essential libmosquitto-dev mosquitto mosquitto-clients protobuf-c-compiler libprotobuf-c-dev libsqlite3-dev sqlite3
```

Python (usually already present on Ubuntu). For plotting, you’ll likely want:

```bash
python3 -m pip install --user matplotlib
```

> If `analyze_gwstats.py` requires additional packages, install as needed (see “Information pending” at the end).

---

## Build

From repository root:

```bash
make
```

Expected binaries:
- `./robot_sim`
- `./telemetry_gateway`

---

## Quick Start (WSL2 + Windows 11) — Step by step

Open **two WSL terminals** (plus one optional terminal for serving the UI).

### 1) Start the MQTT broker (Mosquitto)

Option A (service):

```bash
sudo service mosquitto start
```

Option B (foreground verbose):

```bash
mosquitto -v
```

### 2) Run the simulator (`robot_sim`) — Terminal 1

**Required example:**

```bash
./robot_sim --host 127.0.0.1 --port 1883 --rate 50
```

This publishes:
- RAW sensor topics (binary)
- Snapshot topic (Protobuf)

### 3) Run the gateway (`telemetry_gateway`) — Terminal 2

**Required example:**

```bash
./telemetry_gateway --mqtt-host 127.0.0.1 --mqtt-port 1883 --db telemetry.db --ws-port 8080
```

What to expect:
- A SQLite DB file `telemetry.db` appears (WAL mode may create `telemetry.db-wal` / `telemetry.db-shm` while running)
- The gateway listens for WebSocket connections on port `8080`

### 4) Serve the UI over HTTP — Terminal 3

```bash
cd ui
python3 -m http.server 8000
```

### 5) Open the UI in your browser (Windows or WSL)

- Open: `http://localhost:8000`
- In the UI, connect to: `ws://localhost:8080`

What to expect:
- Connection indicator changes to “connected”
- Telemetry values update live
- If the UI has a “history” / “last N” option, it should fetch from SQLite via the gateway

---

## Sanity Checks (recommended)

### A) Confirm the simulator is publishing RAW data

```bash
mosquitto_sub -t "/robot/sensors/imu0/acc" -C 1 | wc -c
```

Interpretation:
- This prints the byte size of a single message from the RAW IMU accel topic.
- A non-zero number confirms: **broker reachable + simulator publishing + topic correct**.

### B) Confirm the snapshot stream exists (Protobuf payload)

```bash
mosquitto_sub -t "/robot/v1/telemetry/state" -C 1 | wc -c
```

Interpretation:
- Confirms a consolidated snapshot is being published on the snapshot topic.

### C) Confirm the gateway WebSocket is listening

```bash
ss -ltnp | grep 8080 || true
```

---

## Architecture and Topics

### Snapshot vs RAW topics

- **RAW topics:** sensor-specific binary payloads published on separate MQTT topics (minimal overhead, deterministic layout).
- **Snapshot:** a consolidated **Protobuf** message (“photo of system state”) published periodically on a single topic. This is what the gateway logs to SQLite and replays to the UI.

### End-to-end dataflow

`robot_sim` → **MQTT (Mosquitto)** → `telemetry_gateway` → **SQLite + WebSocket (JSON)** → `ui/`

### Default ports

| Component | Purpose | Default |
|---|---|---:|
| Mosquitto | MQTT broker | 1883 |
| telemetry_gateway | WebSocket server | 8080 |
| python http.server | UI HTTP server | 8000 |

### Main MQTT topics

**Snapshot**
- `/robot/v1/telemetry/state`

**RAW (examples / main set)**
- `/robot/sensors/imu0/acc`
- `/robot/sensors/imu0/gyro`
- `/robot/sensors/tilt0/tilt`
- `/robot/sensors/motor1/tics`
- `/robot/sensors/motor1/rpm`
- `/robot/sensors/motor1/temperature`
- `/robot/sensors/motor1/voltage_power_stage`
- `/robot/sensors/motor1/current_power_stage`
- `/robot/sensors/motor2/...` (same pattern as motor1)

> If your local checkout differs (topic names, number of motors/sensors), see “Information pending” at the end and adjust accordingly.

---

## Reproducing the Experiment from the Paper Submission (`run_sweep.sh`)

This repository includes an experiment driver script that runs a sweep over snapshot rates and SQLite batching, logs gateway stats, and generates CSV + figures.

### Prerequisites
- Broker running (Mosquitto)
- Binaries built (`make`)
- You can create folders in the repo directory (`logs/`, `db/`, `results/`)

### What the script does (faithful to the provided `run_sweep.sh`)
- Duration per configuration: **60 seconds**
- RAW sensors fixed at: **50 Hz** (`SENSOR_RATE=50`)
- Snapshot rate sweep (`--state-rate`): **20, 50, 100, 200 Hz**
- SQLite batch sweep (`--batch`): **1 and 50**
- Creates output folders: `logs/`, `db/`, `results/`
- For each configuration:
  - Runs `telemetry_gateway` in background with `--stats` and logs stderr to a file
  - Runs `robot_sim` for 60 seconds with the chosen snapshot rate
  - Stops the gateway and saves DB + log
- After the sweep:
  - Runs `python3 analyze_gwstats.py ...` to produce CSV + figures in `results/`

### Run it

```bash
chmod +x run_sweep.sh
./run_sweep.sh
```

### Output locations and naming

For each `(batch, rate)` pair:

- Database:
  - `db/run_b${B}_r${R}.db`
  - Examples: `db/run_b1_r20.db`, `db/run_b50_r200.db`

- Gateway stats log:
  - `logs/gw_b${B}_r${R}.log`
  - Examples: `logs/gw_b1_r20.log`, `logs/gw_b50_r200.log`

After analysis:

- `results/` will contain:
  - `*.csv` (aggregated metrics)
  - figure files (commonly `*.png` / `*.pdf` / `*.svg`, depending on the script)

### Typical extracted metrics 
Depending on the analysis implementation, typical metrics include:
- achieved snapshot RX throughput (messages per second)
- SQLite insert rate (rows per second)
- commit/transaction timing (especially batch=1 vs batch=50)
- payload size (bytes per snapshot)
- backlog/queue growth indicators (if logged)
- storage cost (bytes/sample; DB size vs number of samples)

### Python plotting
At minimum:

```bash
python3 -m pip install --user matplotlib
```

If needed, install additional dependencies required by `analyze_gwstats.py`.

---

## Troubleshooting

### Mosquitto won’t start / port 1883 is in use

```bash
sudo service mosquitto status
ss -ltnp | grep 1883 || true
sudo lsof -i :1883
```

If you started `mosquitto -v` in a terminal, stop it with `Ctrl+C`.

### UI does not connect to WebSocket (`ws://localhost:8080`)
- Confirm gateway is running:
  ```bash
  ss -ltnp | grep 8080 || true
  ```
- Confirm you are using the correct URL in the UI: `ws://localhost:8080`
- If opening the UI in Windows while services run in WSL2, `localhost` usually works; if not, try using the WSL2 IP for debugging.

### No telemetry appears
- Check snapshot topic:
  ```bash
  mosquitto_sub -t "/robot/v1/telemetry/state" -C 1 | wc -c
  ```
- Check a RAW topic:
  ```bash
  mosquitto_sub -t "/robot/sensors/imu0/acc" -C 1 | wc -c
  ```

### Permission / paths (`db/`, `logs/`, `results/`)
```bash
mkdir -p db logs results
```

If you cloned into `/mnt/c/...`, consider moving the repo into your WSL home directory for fewer filesystem/permission edge-cases.

### Stopping processes
- Foreground processes: `Ctrl+C`
- Background gateway in scripts: use `kill <PID>` if needed
- Quick “find and kill” (use carefully):
  ```bash
  pgrep -a telemetry_gateway || true
  pgrep -a robot_sim || true
  ```

### Useful live diagnostics
- Tail logs:
  ```bash
  tail -f logs/*.log
  ```
- Inspect MQTT topics:
  ```bash
  mosquitto_sub -t "/robot/#" -v
  ```

---

## Reproducibility Notes

- **Primary target environment:** WSL2 + Ubuntu on Windows 11, local broker and processes.
- **Python:** recommend 3.10+ (Ubuntu default versions are fine).
- **Scope:** this setup is designed for controlled local runs. Performance characteristics will differ on real networks (latency/jitter/loss).

---

## Information pending (placeholders — please do not assume)

If any of these differ in your checkout, they should be clarified/updated for artifact evaluation:

- **Exact CLI defaults used by `run_sweep.sh`:**
  - The script runs `./telemetry_gateway --db ... --batch ... --stats` without explicitly setting `--mqtt-host/--mqtt-port/--ws-port`.
  - The script runs `./robot_sim --rate ... --state-rate ...` without explicitly setting `--host/--port`.
  - If your binaries do not default to `127.0.0.1:1883` and `ws-port=8080`, update the script or pass explicit flags.
- **Python dependencies for `analyze_gwstats.py`:**
  - Besides `matplotlib`, confirm whether it needs `numpy`, `pandas`, etc., and provide `requirements.txt` if available.
- **Final authoritative RAW topic list and payload sizes:**
  - This README lists the main topics; confirm any additions/removals in your simulator build.
- **Expected figure/CSV filenames in `results/`:**
  - Document the exact filenames once stabilized.
