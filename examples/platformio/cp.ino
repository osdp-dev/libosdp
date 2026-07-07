/*
 * Copyright (c) 2024-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <Arduino.h>
#include <osdp.hpp>

OSDP::ControlPanel cp;
osdp_pd_info_t pd_info[] = {
    {},
};
static struct osdp_channel cp_channel = {};

int serial1_send_func(void *data, uint8_t *buf, int len)
{
    (void)(data);

    int sent = 0;
    for (int i = 0; i < len; i++) {
        if (Serial1.write(buf[i])) {
            sent++;
        } else {
            break;
        }
    }
    return sent;
}

int serial1_recv_func(void *data, uint8_t *buf, int len)
{
    (void)(data);

    int read = 0;
    while (Serial1.available() && read < len) {
        buf[read] = Serial1.read();
        read++;
    }
    return read;
}

void init_cp_info()
{
    pd_info[0].name = "pd[101]";
    pd_info[0].baud_rate = 115200;
    pd_info[0].address = 101;
    pd_info[0].flags = 0;
    pd_info[0].id.version = 0;
    pd_info[0].id.model = 0;
    pd_info[0].id.vendor_code = 0;
    pd_info[0].id.serial_number = 0;
    pd_info[0].id.firmware_version = 0;
    pd_info[0].cap = nullptr;
    pd_info[0].scbk = nullptr;

    cp_channel.recv = serial1_recv_func;
    cp_channel.send = serial1_send_func;
}

int event_handler(void *data, int pd, struct osdp_event *event)
{
    (void)(data);

    Serial.println("Received an event!");
    return 0;
}

void setup()
{
    Serial.begin(115200);
    Serial1.begin(115200);

    cp.logger_init("osdp::cp", OSDP_LOG_DEBUG, NULL);

    init_cp_info();
    cp.setup(&cp_channel, 1, pd_info);
    cp.set_event_callback(event_handler, nullptr);
}

void loop()
{
    cp.refresh();
    delay(1000);
}
