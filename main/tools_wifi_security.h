#pragma once

#include <stdbool.h>
#include <stdint.h>

void tools_wifi_negotiate(uint8_t *data, int len, uint8_t **output_data,
                          int *output_len, bool *need_free);
int tools_wifi_encrypt(uint8_t iv8, uint8_t *data, int len);
int tools_wifi_decrypt(uint8_t iv8, uint8_t *data, int len);
uint16_t tools_wifi_checksum(uint8_t iv8, uint8_t *data, int len);
int tools_wifi_security_init(void);
void tools_wifi_security_deinit(void);
