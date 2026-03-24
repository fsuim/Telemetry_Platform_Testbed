#ifndef TELEMETRY_GATEWAY_H
#define TELEMETRY_GATEWAY_H

typedef struct telemetry_gateway telemetry_gateway_t;

int telemetry_gateway_start(telemetry_gateway_t **out,
                           const char *mqtt_host, int mqtt_port,
                           const char *db_path,
                           int ws_port);

void telemetry_gateway_stop(telemetry_gateway_t *g);

#endif