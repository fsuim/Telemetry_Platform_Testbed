# Telemetry_Platform_Testbed build
#
# Sources live under src/, public headers under include/, object files under build/,
# and executable binaries under bin/.

CC ?= gcc
PROTOC_C ?= protoc-c

SRC_DIR := src
INC_DIR := include
BUILD_DIR := build
BIN_DIR := bin
PROTO_DIR := proto

CPPFLAGS := -I$(INC_DIR) -I$(INC_DIR)/sensors -I$(INC_DIR)/generated
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -pthread -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS ?=

LIB_COMMON := -lm -pthread -lrt
LIB_MQTT := -lmosquitto
LIB_PB := -lprotobuf-c
LIB_SQLITE := -lsqlite3

COMMON_PROTO_SRC := $(SRC_DIR)/generated/robot_telemetry.pb-c.c

ROBOT_SRC := \
	$(SRC_DIR)/apps/robot_main.c \
	$(SRC_DIR)/core/mqtt_client.c \
	$(SRC_DIR)/core/telemetry_cache.c \
	$(SRC_DIR)/core/telemetry_state_pub.c \
	$(SRC_DIR)/sensors/imu_sensor.c \
	$(SRC_DIR)/sensors/tilt_sensor.c \
	$(SRC_DIR)/sensors/motor1_sensor.c \
	$(SRC_DIR)/sensors/motor2_sensor.c \
	$(COMMON_PROTO_SRC)

GATEWAY_SRC := \
	$(SRC_DIR)/apps/telemetry_gateway_main.c \
	$(SRC_DIR)/core/telemetry_gateway.c \
	$(SRC_DIR)/core/ws_server.c \
	$(SRC_DIR)/core/db_sqlite.c \
	$(SRC_DIR)/core/mqtt_client.c \
	$(COMMON_PROTO_SRC)

DUMP_SRC := \
	$(SRC_DIR)/apps/state_dump.c \
	$(COMMON_PROTO_SRC)

ROBOT_OBJ := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(ROBOT_SRC))
GATEWAY_OBJ := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(GATEWAY_SRC))
DUMP_OBJ := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(DUMP_SRC))

OUT_SIM := $(BIN_DIR)/robot_sim
OUT_GATEWAY := $(BIN_DIR)/telemetry_gateway
OUT_DUMP := $(BIN_DIR)/state_dump

GENERATED_C := $(SRC_DIR)/generated/robot_telemetry.pb-c.c
GENERATED_H := $(INC_DIR)/generated/robot_telemetry.pb-c.h
PROTO_FILE := $(PROTO_DIR)/robot_telemetry.proto

.PHONY: all clean distclean proto layout

all: $(OUT_SIM) $(OUT_GATEWAY) $(OUT_DUMP)

$(OUT_SIM): $(ROBOT_OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@ $(LIB_MQTT) $(LIB_PB) $(LIB_COMMON)

$(OUT_GATEWAY): $(GATEWAY_OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@ $(LIB_MQTT) $(LIB_PB) $(LIB_SQLITE) $(LIB_COMMON)

$(OUT_DUMP): $(DUMP_OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@ $(LIB_MQTT) $(LIB_PB) $(LIB_COMMON)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BIN_DIR) $(BUILD_DIR):
	mkdir -p $@

# Regenerate protobuf-c files after editing proto/robot_telemetry.proto.
# Generated .c and .h are intentionally kept in separate source/include trees.
proto:
	mkdir -p $(SRC_DIR)/generated $(INC_DIR)/generated $(BUILD_DIR)/protobuf
	$(PROTOC_C) --proto_path=$(PROTO_DIR) --c_out=$(BUILD_DIR)/protobuf $(PROTO_FILE)
	cp $(BUILD_DIR)/protobuf/robot_telemetry.pb-c.c $(GENERATED_C)
	cp $(BUILD_DIR)/protobuf/robot_telemetry.pb-c.h $(GENERATED_H)

layout:
	@echo "Source files:      $(SRC_DIR)/"
	@echo "Header files:      $(INC_DIR)/"
	@echo "Generated sources: $(SRC_DIR)/generated/ and $(INC_DIR)/generated/"
	@echo "Object files:      $(BUILD_DIR)/"
	@echo "Binaries:          $(BIN_DIR)/"

clean:
	rm -rf $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)
	touch $(BUILD_DIR)/.gitkeep
	rm -f $(OUT_SIM) $(OUT_GATEWAY) $(OUT_DUMP)

# Also remove generated experiment/runtime artifacts created during local runs.
distclean: clean
	rm -rf exp/db exp/ui_logs exp/results

-include $(sort $(ROBOT_OBJ:.o=.d) $(GATEWAY_OBJ:.o=.d) $(DUMP_OBJ:.o=.d))
