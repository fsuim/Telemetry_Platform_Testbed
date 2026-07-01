#!/usr/bin/env bash
set -euo pipefail

# v4 strict automated reconnect-only baseline runner.
# Run from repository root.
# Full run:
#   REPS=10 RATES="25 50 75 100 150" bash experiments/disconnect_replay_rate_sweep/run_reconnect_only_matrix_auto.sh
# Single run:
#   HEADLESS=0 REPS=10 RATES="25" START_REP=1 END_REP=1 bash experiments/disconnect_replay_rate_sweep/run_reconnect_only_matrix_auto.sh

SCRIPT_VERSION="v5_strict_wait_gateway_2026-06-22"
STUDY_ID=${STUDY_ID:-reconnect_only_rate_sweep_auto_v5_strict_wait_gateway}
RATES=${RATES:-"25 50 75 100 150"}
REPS=${REPS:-10}
START_REP=${START_REP:-1}
END_REP=${END_REP:-$REPS}
RUN_DURATION_S=${RUN_DURATION_S:-60}
POST_RUN_WAIT_S=${POST_RUN_WAIT_S:-8}
MIN_LIVE_FRACTION=${MIN_LIVE_FRACTION:-0.50}
CUT_START_S=${CUT_START_S:-20}
CUT_DURATION_S=${CUT_DURATION_S:-3}
MQTT_HOST=${MQTT_HOST:-127.0.0.1}
MQTT_PORT=${MQTT_PORT:-1883}
WS_PORT=${WS_PORT:-8080}
UI_HTTP_PORT=${UI_HTTP_PORT:-8000}
UI_DIR=${UI_DIR:-./ui}
DB_DIR=${DB_DIR:-./exp/db}
UI_LOG_DIR=${UI_LOG_DIR:-./exp/ui_logs}
HEADLESS=${HEADLESS:-1}
START_UI_SERVER=${START_UI_SERVER:-1}
AUTOMATION_SCRIPT=${AUTOMATION_SCRIPT:-experiments/disconnect_replay_rate_sweep/ui_automation/playwright_export_ui_log.py}
ROBOT_BIN=${ROBOT_BIN:-./bin/robot_sim}
GATEWAY_BIN=${GATEWAY_BIN:-./bin/telemetry_gateway}

UI_HOST_SELECTOR=${UI_HOST_SELECTOR:-}
UI_PORT_SELECTOR=${UI_PORT_SELECTOR:-}
UI_PATH_SELECTOR=${UI_PATH_SELECTOR:-}
UI_LAST_N_SELECTOR=${UI_LAST_N_SELECTOR:-}
UI_CONNECT_SELECTOR=${UI_CONNECT_SELECTOR:-}
UI_EXPORT_SELECTOR=${UI_EXPORT_SELECTOR:-}

mkdir -p "$DB_DIR" "$UI_LOG_DIR"

if [[ ! -x "$ROBOT_BIN" ]]; then
  echo "ERROR: robot simulator not found or not executable: $ROBOT_BIN"
  echo "Run 'make' from the repository root, or set ROBOT_BIN=/path/to/robot_sim."
  exit 1
fi
if [[ ! -x "$GATEWAY_BIN" ]]; then
  echo "ERROR: telemetry gateway not found or not executable: $GATEWAY_BIN"
  echo "Run 'make' from the repository root, or set GATEWAY_BIN=/path/to/telemetry_gateway."
  exit 1
fi
if [[ ! -f "$AUTOMATION_SCRIPT" ]]; then
  echo "ERROR: automation script not found: $AUTOMATION_SCRIPT"
  exit 1
fi

port_is_open() {
  local host="$1"
  local port="$2"
  python3 - <<PY >/dev/null 2>&1
import socket, sys
host = "${host}"
port = int("${port}")
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(0.25)
try:
    s.connect((host, port))
    sys.exit(0)
except OSError:
    sys.exit(1)
finally:
    try:
        s.close()
    except Exception:
        pass
PY
}

wait_port_open() {
  local host="$1"
  local port="$2"
  local timeout_s="${3:-10}"
  local start_ts now_ts
  start_ts=$(date +%s)
  while true; do
    if port_is_open "$host" "$port"; then
      return 0
    fi
    now_ts=$(date +%s)
    if (( now_ts - start_ts >= timeout_s )); then
      return 1
    fi
    sleep 0.25
  done
}

wait_port_closed() {
  local host="$1"
  local port="$2"
  local timeout_s="${3:-10}"
  local start_ts now_ts
  start_ts=$(date +%s)
  while true; do
    if ! port_is_open "$host" "$port"; then
      return 0
    fi
    now_ts=$(date +%s)
    if (( now_ts - start_ts >= timeout_s )); then
      return 1
    fi
    sleep 0.25
  done
}

terminate_pid() {
  local name="$1"
  local pid="${2:-}"
  if [[ -z "$pid" ]]; then return 0; fi
  if ! kill -0 "$pid" 2>/dev/null; then return 0; fi
  kill "$pid" 2>/dev/null || true
  for _ in $(seq 1 30); do
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid" 2>/dev/null || true
      return 0
    fi
    sleep 0.2
  done
  echo "WARNING: ${name} PID ${pid} did not exit after TERM; sending KILL"
  kill -9 "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

cleanup_run() {
  terminate_pid "robot_sim" "${ROBOT_PID:-}"
  terminate_pid "telemetry_gateway" "${GATEWAY_PID:-}"
  ROBOT_PID=""
  GATEWAY_PID=""
  # Give the WebSocket port a short time to become unavailable before the next run.
  wait_port_closed "127.0.0.1" "$WS_PORT" 5 || true
}

cleanup_all() {
  cleanup_run || true
  if [[ -n "${UI_SERVER_PID:-}" ]] && kill -0 "$UI_SERVER_PID" 2>/dev/null; then kill "$UI_SERVER_PID" 2>/dev/null || true; fi
}
trap cleanup_all EXIT INT TERM

if [[ "$START_UI_SERVER" == "1" ]]; then
  if [[ ! -d "$UI_DIR" ]]; then
    echo "ERROR: UI_DIR not found: $UI_DIR"
    exit 1
  fi
  echo "Starting UI HTTP server on port ${UI_HTTP_PORT} from ${UI_DIR}"
  python3 -m http.server "$UI_HTTP_PORT" --directory "$UI_DIR" > "${DB_DIR}/${STUDY_ID}_ui_http.log" 2>&1 &
  UI_SERVER_PID=$!
  sleep 1
else
  echo "START_UI_SERVER=0: assuming UI server already running at http://localhost:${UI_HTTP_PORT}"
fi

cat <<INFO
============================================================
Automated reconnect-only baseline sweep — ${SCRIPT_VERSION}
Study ID: $STUDY_ID
Rates: $RATES
Repetitions: ${START_REP}..${END_REP} of ${REPS}
Run duration: ${RUN_DURATION_S}s + ${POST_RUN_WAIT_S}s post-run wait
WebSocket cut: start=${CUT_START_S}s, duration=${CUT_DURATION_S}s
UI URL: http://localhost:${UI_HTTP_PORT}
UI logs: $UI_LOG_DIR
Replay/history window: 0 samples for reconnect-only
Strict rule: any telemetry,replay row makes the run invalid.
============================================================
INFO

for rate in $RATES; do
  for rep in $(seq "$START_REP" "$END_REP"); do
    run_tag="reconnect_only_r${rate}_rep${rep}"
    db_path="${DB_DIR}/${run_tag}.db"
    expected_log="${UI_LOG_DIR}/ui_log_${run_tag}.csv"

    echo
    echo "------------------------------------------------------------"
    echo "Run tag: ${run_tag}"
    echo "Nominal telemetry rate: ${rate} Hz"
    echo "Expected UI log: ${expected_log}"
    echo "Database: ${db_path}"
    echo "------------------------------------------------------------"

    # Ensure the previous run has completely released the WebSocket port before starting a new gateway.
    cleanup_run || true
    if port_is_open "127.0.0.1" "$WS_PORT"; then
      echo "ERROR: WebSocket port ${WS_PORT} is still open before starting ${run_tag}."
      echo "Check for a stale telemetry_gateway process, for example:"
      echo "  ps aux | grep telemetry_gateway"
      echo "  fuser -n tcp ${WS_PORT}"
      exit 10
    fi

    rm -f "$db_path" "$expected_log"

    "$ROBOT_BIN" --host "$MQTT_HOST" --port "$MQTT_PORT" --rate "$rate" --state-rate "$rate" > "${DB_DIR}/${run_tag}_robot.log" 2>&1 &
    ROBOT_PID=$!
    sleep 1

    "$GATEWAY_BIN" \
      --mqtt-host "$MQTT_HOST" \
      --mqtt-port "$MQTT_PORT" \
      --db "$db_path" \
      --ws-port "$WS_PORT" \
      --ui-cut-start-s "$CUT_START_S" \
      --ui-cut-duration-s "$CUT_DURATION_S" \
      --ui-reconnect-mode close \
      --ui-exp-tag "$run_tag" > "${DB_DIR}/${run_tag}_gateway.log" 2>&1 &
    GATEWAY_PID=$!

    # Wait until the gateway is actually listening before opening the UI.
    # Without this guard, the browser may enter a ws_error/ws_close loop if the next run starts
    # before the previous gateway has released the port or before the new gateway is ready.
    if ! wait_port_open "127.0.0.1" "$WS_PORT" 10; then
      echo "ERROR: telemetry_gateway did not open WebSocket port ${WS_PORT} for ${run_tag}."
      echo "--- gateway log tail ---"
      tail -n 80 "${DB_DIR}/${run_tag}_gateway.log" || true
      echo "--- robot log tail ---"
      tail -n 40 "${DB_DIR}/${run_tag}_robot.log" || true
      cleanup_run || true
      exit 11
    fi
    sleep 0.5

    headless_arg=""
    if [[ "$HEADLESS" == "1" ]]; then headless_arg="--headless"; fi

    selector_args=()
    [[ -n "$UI_HOST_SELECTOR" ]] && selector_args+=(--host-selector "$UI_HOST_SELECTOR")
    [[ -n "$UI_PORT_SELECTOR" ]] && selector_args+=(--port-selector "$UI_PORT_SELECTOR")
    [[ -n "$UI_PATH_SELECTOR" ]] && selector_args+=(--path-selector "$UI_PATH_SELECTOR")
    [[ -n "$UI_LAST_N_SELECTOR" ]] && selector_args+=(--last-n-selector "$UI_LAST_N_SELECTOR")
    [[ -n "$UI_CONNECT_SELECTOR" ]] && selector_args+=(--connect-selector "$UI_CONNECT_SELECTOR")
    [[ -n "$UI_EXPORT_SELECTOR" ]] && selector_args+=(--export-selector "$UI_EXPORT_SELECTOR")

    python3 "$AUTOMATION_SCRIPT" \
      --url "http://localhost:${UI_HTTP_PORT}" \
      --host "localhost" \
      --port "$WS_PORT" \
      --path "/" \
      --last-n 0 \
      --wait-s "$((RUN_DURATION_S + POST_RUN_WAIT_S))" \
      --output "$expected_log" \
      --block-history-requests \
      $headless_arg \
      "${selector_args[@]}"

    cleanup_run

    if [[ ! -f "$expected_log" ]]; then
      echo "ERROR: expected UI log was not created: $expected_log"
      exit 1
    fi

    live_rows=$(awk -F, '$2=="telemetry" && $3=="live" {c++} END {print c+0}' "$expected_log")
    replay_rows=$(awk -F, '$2=="telemetry" && $3=="replay" {c++} END {print c+0}' "$expected_log")
    ws_open_rows=$(awk -F, '$2=="ws_open" {c++} END {print c+0}' "$expected_log")
    history_requests=$(awk -F, '$2=="history_request" {c++} END {print c+0}' "$expected_log")
    history_loaded=$(awk -F, '$2=="history_batch_loaded" {c++} END {print c+0}' "$expected_log")
    min_live_rows=$(python3 - <<PY
rate=${rate}
run_duration=${RUN_DURATION_S}
cut_duration=${CUT_DURATION_S}
fraction=${MIN_LIVE_FRACTION}
print(int(rate * max(1, run_duration - cut_duration) * fraction))
PY
)

    echo "Validation: ws_open=${ws_open_rows}, live_rows=${live_rows}, replay_rows=${replay_rows}, history_request=${history_requests}, history_loaded=${history_loaded}, min_live_rows=${min_live_rows}"

    if [[ "$ws_open_rows" -lt 2 ]]; then
      echo "ERROR: invalid run ${run_tag}: expected at least two ws_open events."
      exit 2
    fi
    if [[ "$live_rows" -lt "$min_live_rows" ]]; then
      echo "ERROR: invalid run ${run_tag}: too few live telemetry rows (${live_rows} < ${min_live_rows})."
      exit 3
    fi
    if [[ "$replay_rows" -gt 0 || "$history_requests" -gt 0 || "$history_loaded" -gt 0 ]]; then
      echo "ERROR: invalid reconnect-only run ${run_tag}: history/replay was still triggered."
      echo "The baseline must contain no history_request, no history_batch_loaded and no telemetry,replay rows."
      echo "Use this v5 package and verify that the output banner says: ${SCRIPT_VERSION}."
      exit 4
    fi
  done
done

echo
echo "Automated reconnect-only baseline complete. Suggested combined analysis:"
echo "python3 experiments/disconnect_replay_rate_sweep/analyze_rate_sweep_with_baseline.py --input-dir ./exp/ui_logs --output-dir ./exp/results/reconnect_only_vs_replay --pattern 'ui_log_*.csv'"
