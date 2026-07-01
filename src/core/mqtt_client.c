#include "mqtt_client.h"
#include <stdio.h>
#include <stdlib.h>

int mqtt_client_connect(mqtt_client_t *c, const char *client_id, const char *host, int port){
    c->mosq = mosquitto_new(client_id, true, NULL);
    if(!c->mosq){
        fprintf(stderr, "mosquitto_new failed\n");
        return -1;
    }

    mosquitto_reconnect_delay_set(c->mosq, 1, 10, true);

    int rc = mosquitto_connect(c->mosq, host, port, 60);
    if(rc != MOSQ_ERR_SUCCESS){
        fprintf(stderr, "mosquitto_connect(%s:%d) failed: %s\n", host, port, mosquitto_strerror(rc));
        mosquitto_destroy(c->mosq);
        c->mosq = NULL;
        return -2;
    }

    rc = mosquitto_loop_start(c->mosq);
    if(rc != MOSQ_ERR_SUCCESS){
        fprintf(stderr, "mosquitto_loop_start failed: %s\n", mosquitto_strerror(rc));
        mosquitto_disconnect(c->mosq);
        mosquitto_destroy(c->mosq);
        c->mosq = NULL;
        return -3;
    }

    return 0;
}

void mqtt_client_close(mqtt_client_t *c){
    if(!c || !c->mosq) return;
    mosquitto_loop_stop(c->mosq, true);
    mosquitto_disconnect(c->mosq);
    mosquitto_destroy(c->mosq);
    c->mosq = NULL;
}