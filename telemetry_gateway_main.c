#include "telemetry_gateway.h"

#include <mosquitto.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void on_sig(int sig){
    (void)sig;
    g_running = 0;
}

static void usage(const char *p){
    fprintf(stderr,
        "Usage: %s [--mqtt-host HOST] [--mqtt-port PORT] [--db PATH] [--ws-port PORT] [--batch N] [--stats] [--stats-ms MS]\n"
        "Defaults: mqtt-host=127.0.0.1 mqtt-port=1883 db=telemetry.db ws-port=8080 batch=50 stats=off stats-ms=1000\n",
        p);
}

int main(int argc, char **argv){
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    const char *mqtt_host = "127.0.0.1";
    int mqtt_port = 1883;
    const char *db_path = "telemetry.db";
    int ws_port = 8080;
    int batch = 50;
    int stats = 0;
    int stats_ms = 1000;
    int ui_delay_ms = 0;
    int ui_jitter_ms = 0;
    int ui_drop_pct = 0;
    int ui_cut_start_s = 0;
    int ui_cut_duration_s = 0;
    const char *ui_reconnect_mode = "close";
    const char *ui_exp_tag = "default";

    for(int i=1; i<argc; i++){
        if(strcmp(argv[i], "--mqtt-host")==0 && i+1<argc){
            mqtt_host = argv[++i];
        }else if(strcmp(argv[i], "--mqtt-port")==0 && i+1<argc){
            mqtt_port = atoi(argv[++i]);
        }else if(strcmp(argv[i], "--db")==0 && i+1<argc){
            db_path = argv[++i];
        }else if(strcmp(argv[i], "--ws-port")==0 && i+1<argc){
            ws_port = atoi(argv[++i]);
        }else if(strcmp(argv[i], "--batch")==0 && i+1<argc){
            batch = atoi(argv[++i]);
        }else if(strcmp(argv[i], "--stats")==0){
            stats = 1;
        }else if(strcmp(argv[i], "--stats-ms")==0 && i+1<argc){
            stats_ms = atoi(argv[++i]);
        }else if(strcmp(argv[i], "--ui-delay-ms")==0 && i+1<argc){
            ui_delay_ms = atoi(argv[++i]);
        }else if(strcmp(argv[i], "--ui-jitter-ms")==0 && i+1<argc){
            ui_jitter_ms = atoi(argv[++i]);
        }else if(strcmp(argv[i], "--ui-drop-pct")==0 && i+1<argc){
            ui_drop_pct = atoi(argv[++i]);
        }else if(strcmp(argv[i], "--ui-cut-start-s")==0 && i+1<argc){
            ui_cut_start_s = atoi(argv[++i]);
        }else if(strcmp(argv[i], "--ui-cut-duration-s")==0 && i+1<argc){
            ui_cut_duration_s = atoi(argv[++i]);
        }else if(strcmp(argv[i], "--ui-reconnect-mode")==0 && i+1<argc){
            ui_reconnect_mode = argv[++i];
        }else if(strcmp(argv[i], "--ui-exp-tag")==0 && i+1<argc){
            ui_exp_tag = argv[++i];
        }else if(strcmp(argv[i], "--help")==0){
            usage(argv[0]);
            return 0;
        }else{
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    // Pass experiment knobs to the gateway without changing the public API.
    // (telemetry_gateway.c reads these env vars at startup)
    if(batch <= 0) batch = 50;
    if(stats_ms <= 0) stats_ms = 1000;
    if(ui_delay_ms < 0) ui_delay_ms = 0;
    if(ui_jitter_ms < 0) ui_jitter_ms = 0;
    if(ui_drop_pct < 0) ui_drop_pct = 0;
    if(ui_drop_pct > 100) ui_drop_pct = 100;
    if(ui_cut_start_s < 0) ui_cut_start_s = 0;
    if(ui_cut_duration_s < 0) ui_cut_duration_s = 0;
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", batch);
        setenv("GW_BATCH", buf, 1);
        snprintf(buf, sizeof(buf), "%d", stats_ms);
        setenv("GW_STATS_MS", buf, 1);
        setenv("GW_STATS", stats ? "1" : "0", 1);

        snprintf(buf, sizeof(buf), "%d", ui_delay_ms);
        setenv("WS_DELAY_MS", buf, 1);
        snprintf(buf, sizeof(buf), "%d", ui_jitter_ms);
        setenv("WS_JITTER_MS", buf, 1);
        snprintf(buf, sizeof(buf), "%d", ui_drop_pct);
        setenv("WS_DROP_PCT", buf, 1);
        snprintf(buf, sizeof(buf), "%d", ui_cut_start_s);
        setenv("WS_CUT_START_S", buf, 1);
        snprintf(buf, sizeof(buf), "%d", ui_cut_duration_s);
        setenv("WS_CUT_DURATION_S", buf, 1);
        setenv("WS_RECONNECT_MODE", (ui_reconnect_mode && ui_reconnect_mode[0]) ? ui_reconnect_mode : "close", 1);
        setenv("WS_EXP_TAG", (ui_exp_tag && ui_exp_tag[0]) ? ui_exp_tag : "default", 1);
    }

    mosquitto_lib_init();

    telemetry_gateway_t *gw = NULL;
    if(telemetry_gateway_start(&gw, mqtt_host, mqtt_port, db_path, ws_port) != 0){
        fprintf(stderr, "Failed to start telemetry_gateway\n");
        mosquitto_lib_cleanup();
        return 2;
    }

    printf("telemetry_gateway running\n");
    printf("  MQTT: %s:%d topic=/robot/v1/telemetry/state (protobuf)\n", mqtt_host, mqtt_port);
    printf("  DB:   %s\n", db_path);
    printf("  WS:   ws://localhost:%d\n", ws_port);
    printf("  EXP:  batch=%d stats=%s stats-ms=%d\n", batch, stats ? "on" : "off", stats_ms);
    printf("Press Ctrl+C to stop.\n");

    while(g_running){
        usleep(200000);
    }

    telemetry_gateway_stop(gw);
    mosquitto_lib_cleanup();
    printf("Stopped.\n");
    return 0;
}