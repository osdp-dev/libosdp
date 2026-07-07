/*
 * Copyright (c) 2024-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <Arduino.h>
#include <osdp.hpp>

OSDP::PeripheralDevice pd;
osdp_pd_info_t info_pd = {};
static const struct osdp_pd_cap pd_cap[] = {
    { OSDP_PD_CAP_READER_LED_CONTROL, 1, 1 },
    { OSDP_PD_CAP_READER_AUDIBLE_OUTPUT, 1, 1 },
    { static_cast<uint8_t>(-1), 0, 0 } /* Sentinel */
};
static struct osdp_channel pd_channel = {};

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

void init_pd_info()
{
    info_pd.name = "pd[101]";
    info_pd.baud_rate = 9600;
    info_pd.address = 101;
    info_pd.flags = 0;
    info_pd.id.version = 1;
    info_pd.id.model = 153;
    info_pd.id.vendor_code = 31337;
    info_pd.id.serial_number = 0x01020304;
    info_pd.id.firmware_version = 0x0A0B0C0D;
    info_pd.cap = pd_cap;
    info_pd.scbk = nullptr;

    pd_channel.recv = serial1_recv_func;
    pd_channel.send = serial1_send_func;
}

int pd_command_handler(void *data, struct osdp_cmd *cmd)
{
    (void)(data);

    Serial.println("Received a command!");
    return 0;
}

void setup()
{
    Serial.begin(115200);
    Serial1.begin(115200);

    pd.logger_init("osdp::pd", OSDP_LOG_DEBUG, NULL);

    init_pd_info();
    pd.setup(&pd_channel, &info_pd);

    pd.set_command_callback(pd_command_handler, nullptr);
}

void loop()
{
    pd.refresh();
    delay(1000);
}
