CC=gcc

CFLAGS=-O2 -Wall -Wextra -std=c11 -pthread -I. -Isensors \
       -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE

LIB_COMMON=-lm -pthread -lrt
LIB_MQTT=-lmosquitto
LIB_PB=-lprotobuf-c
LIB_SQLITE=-lsqlite3

ROBOT_SRC=robot_main.c mqtt_client.c \
    sensors/imu_sensor.c sensors/tilt_sensor.c \
    sensors/motor1_sensor.c sensors/motor2_sensor.c \
    telemetry_cache.c telemetry_state_pub.c \
    robot_telemetry.pb-c.c

GW_SRC=telemetry_gateway_main.c telemetry_gateway.c \
    ws_server.c db_sqlite.c \
    mqtt_client.c robot_telemetry.pb-c.c

DUMP_SRC=state_dump.c robot_telemetry.pb-c.c

OUT_SIM=robot_sim
OUT_GW=telemetry_gateway
OUT_DUMP=state_dump

all: $(OUT_SIM) $(OUT_GW) $(OUT_DUMP)

$(OUT_SIM): $(ROBOT_SRC)
	$(CC) $(CFLAGS) $(ROBOT_SRC) -o $(OUT_SIM) $(LIB_MQTT) $(LIB_PB) $(LIB_COMMON)

$(OUT_GW): $(GW_SRC)
	$(CC) $(CFLAGS) $(GW_SRC) -o $(OUT_GW) $(LIB_MQTT) $(LIB_PB) $(LIB_SQLITE) $(LIB_COMMON)

$(OUT_DUMP): $(DUMP_SRC)
	$(CC) $(CFLAGS) $(DUMP_SRC) -o $(OUT_DUMP) $(LIB_MQTT) $(LIB_PB) $(LIB_COMMON)

clean:
	rm -f $(OUT_SIM) $(OUT_GW) $(OUT_DUMP)