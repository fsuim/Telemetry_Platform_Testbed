#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <mosquitto.h>

typedef struct {
    struct mosquitto *mosq;
} mqtt_client_t;

int  mqtt_client_connect(mqtt_client_t *c, const char *client_id, const char *host, int port);
void mqtt_client_close(mqtt_client_t *c);

#endif