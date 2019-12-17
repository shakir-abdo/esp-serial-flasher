/* Copyright 2020 Espressif Systems (Shanghai) PTE LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "serial_comm_prv.h"
#include "serial_comm.h"
#include "serial_io.h"
#include <stddef.h>


static uint32_t s_sequence_number = 0;

static const uint8_t DELIMITER = 0xc0;
static const uint8_t C0_REPLACEMENT[2] = {0xdb, 0xdc};
static const uint8_t BD_REPLACEMENT[2] = {0xdb, 0xdd};

static esp_loader_error_t check_response(command_t cmd, uint32_t *reg_value);


static inline esp_loader_error_t serial_read(uint8_t *buff, size_t size)
{
    return loader_port_serial_read(buff, size, loader_port_remaining_time());
}

static inline esp_loader_error_t serial_write(const uint8_t *buff, size_t size)
{
    return loader_port_serial_write(buff, size, loader_port_remaining_time());
}

static uint8_t compute_checksum(const uint8_t *data, uint32_t size)
{
    uint8_t checksum = 0xEF;

    while (size--) {
        checksum ^= *data++;
    }

    return checksum;
}


static esp_loader_error_t SLIP_receive_packet(uint8_t *buff, uint32_t buff_size)
{
    uint8_t ch;

    // Wait for delimiter
    do {
        esp_loader_error_t err = serial_read(&ch, 1);
        if (err != ESP_LOADER_SUCCESS) {
            return err;
        }
    } while (ch != DELIMITER);

    for (int i = 0; i < buff_size; i++) {
        RETURN_ON_ERROR( serial_read(&ch, 1)) ;

        if (ch == 0xdb) {
            RETURN_ON_ERROR( serial_read(&ch, 1) );
            if (ch == 0xdc) {
                buff[i] = 0xc0;
            } else if (ch == 0xdd) {
                buff[i] = 0xbd;
            } else {
                return ESP_LOADER_ERROR_INVALID_RESPONSE;
            }
        } else {
            buff[i] = ch;
        }
    }

    // Delimiter
    RETURN_ON_ERROR( serial_read(&ch, 1) );
    if (ch != DELIMITER) {
        return ESP_LOADER_ERROR_INVALID_RESPONSE;
    }

    return ESP_LOADER_SUCCESS;
}


static esp_loader_error_t SLIP_send(const uint8_t *data, uint32_t size)
{
    uint32_t to_write = 0;
    uint32_t written = 0;

    for (int i = 0; i < size; i++) {
        if (data[i] != 0xc0 && data[i] != 0xdb) {
            to_write++;
            continue;
        }

        if (to_write > 0) {
            RETURN_ON_ERROR( serial_write(&data[written], to_write) );
        }

        if (data[i] == 0xc0) {
            RETURN_ON_ERROR( serial_write(C0_REPLACEMENT, 2) );
        } else {
            RETURN_ON_ERROR( serial_write(BD_REPLACEMENT, 2) );
        }

        written = i + 1;
        to_write = 0;
    }

    if (to_write > 0) {
        RETURN_ON_ERROR( serial_write(&data[written], to_write) );
    }

    return ESP_LOADER_SUCCESS;
}


static esp_loader_error_t SLIP_send_delimiter(void)
{
    return serial_write(&DELIMITER, 1);
}


static esp_loader_error_t send_cmd(const void *cmd_data, uint32_t size, uint32_t *reg_value)
{
    command_t command = ((command_common_t *)cmd_data)->command;

    RETURN_ON_ERROR( SLIP_send_delimiter() );
    RETURN_ON_ERROR( SLIP_send((const uint8_t *)cmd_data, size) );
    RETURN_ON_ERROR( SLIP_send_delimiter() );

    return check_response(command, reg_value);
}


static esp_loader_error_t send_cmd_with_data(const void *cmd_data, size_t cmd_size,
        const void *data, size_t data_size)
{
    command_t command = ((command_common_t *)cmd_data)->command;

    RETURN_ON_ERROR( SLIP_send_delimiter() );
    RETURN_ON_ERROR( SLIP_send((const uint8_t *)cmd_data, cmd_size) );
    RETURN_ON_ERROR( SLIP_send(data, data_size) );
    RETURN_ON_ERROR( SLIP_send_delimiter() );

    return check_response(command, NULL);
}


static void log_loader_internal_error(error_code_t error)
{
    switch (error) {
        case INVALID_CRC:     loader_port_debug_print("INVALID_CRC"); break;
        case INVALID_COMMAND: loader_port_debug_print("INVALID_COMMAND"); break;
        case COMMAND_FAILED:  loader_port_debug_print("COMMAND_FAILED"); break;
        case FLASH_WRITE_ERR: loader_port_debug_print("FLASH_WRITE_ERR"); break;
        case FLASH_READ_ERR:  loader_port_debug_print("FLASH_READ_ERR"); break;
        case READ_LENGTH_ERR: loader_port_debug_print("READ_LENGTH_ERR"); break;
        case DEFLATE_ERROR:   loader_port_debug_print("DEFLATE_ERROR"); break;
        default:              loader_port_debug_print("UNKNOWN ERROR"); break;
    }
}


static esp_loader_error_t check_response(command_t cmd, uint32_t *reg_value)
{
    response_t response;
    esp_loader_error_t err;

    do {
        err = SLIP_receive_packet((uint8_t *)&response, sizeof(response_t));
        if (err != ESP_LOADER_SUCCESS) {
            return err;
        }
    } while ((response.direction != READ_DIRECTION) || (response.command != cmd));

    if (response.status == STATUS_FAILURE) {
        log_loader_internal_error(response.error);
        return ESP_LOADER_ERROR_INVALID_RESPONSE;
    }

    if (reg_value != NULL) {
        *reg_value = response.value;
    }

    return ESP_LOADER_SUCCESS;
}


esp_loader_error_t loader_flash_begin_cmd(uint32_t offset,
        uint32_t erase_size,
        uint32_t block_size,
        uint32_t blocks_to_write)
{
    begin_command_t begin_cmd = {
        .common = {
            .direction = 0,
            .command = FLASH_BEGIN,
            .size = 16,
            .checksum = 0
        },
        .erase_size = erase_size,
        .packet_count = blocks_to_write,
        .packet_size = block_size,
        .offset = offset
    };

    s_sequence_number = 0;

    return send_cmd(&begin_cmd, sizeof(begin_cmd), NULL);
}


esp_loader_error_t loader_flash_data_cmd(const uint8_t *data, uint32_t size)
{
    data_command_t data_cmd = {
        .common = {
            .direction = 0,
            .command = FLASH_DATA,
            .size = 16,
            .checksum = compute_checksum(data, size)
        },
        .data_size = size,
        .sequence_number = s_sequence_number++,
        .zero_0 = 0,
        .zero_1 = 0
    };

    return send_cmd_with_data(&data_cmd, sizeof(data_cmd), data, size);
}


esp_loader_error_t loader_flash_end_cmd(bool stay_in_loader)
{
    flash_end_command_t end_cmd = {
        .common = {
            .direction = 0,
            .command = FLASH_END,
            .size = 4,
            .checksum = 0
        },
        .stay_in_loader = stay_in_loader
    };

    return send_cmd(&end_cmd, sizeof(end_cmd), NULL);
}


esp_loader_error_t loader_sync_cmd(void)
{
    sync_command_t sync_cmd = {
        .common = {
            .direction = 0,
            .command = SYNC,
            .size = 36,
            .checksum = 0
        },
        .sync_sequence = {
            0x07, 0x07, 0x12, 0x20,
            0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
            0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
            0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
            0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
        }
    };

    return send_cmd(&sync_cmd, sizeof(sync_cmd), NULL);
}


esp_loader_error_t loader_mem_begin_cmd(uint32_t offset,
                                        uint32_t total_size,
                                        uint32_t packet_size,
                                        uint32_t packets_to_write)
{
    begin_command_t begin_cmd = {
        .common = {
            .direction = 0,
            .command = MEM_BEGIN,
            .size = 16,
            .checksum = 0
        },
        .erase_size = total_size,
        .packet_count = packets_to_write,
        .packet_size = packet_size,
        .offset = offset
    };

    s_sequence_number = 0;

    return send_cmd(&begin_cmd, sizeof(begin_cmd), NULL);
}


esp_loader_error_t loader_mem_data_cmd(uint8_t *data, uint32_t size)
{
    data_command_t data_cmd = {
        .common = {
            .direction = 0,
            .command = MEM_DATA,
            .size = 16,
            .checksum = compute_checksum(data, size)
        },
        .data_size = size,
        .sequence_number = s_sequence_number++,
        .zero_0 = 0,
        .zero_1 = 0
    };

    return send_cmd_with_data(&data_cmd, sizeof(data_cmd), data, size);
}


esp_loader_error_t loader_mem_end_cmd(bool stay_in_loader, uint32_t address)
{
    mem_end_command_t end_cmd = {
        .common = {
            .direction = 0,
            .command = MEM_END,
            .size = 8,
            .checksum = 0
        },
        .stay_in_loader = stay_in_loader,
        .entry_point_address = address
    };

    return send_cmd(&end_cmd, sizeof(end_cmd), NULL);
}


esp_loader_error_t loader_write_reg_cmd(uint32_t address, uint32_t value,
                                        uint32_t mask, uint32_t delay_us)
{
    write_reg_command_t write_cmd = {
        .common = {
            .direction = 0,
            .command = WRITE_REG,
            .size = 16,
            .checksum = 0
        },
        .address = address,
        .value = value,
        .mask = mask,
        .delay_us = delay_us
    };

    return send_cmd(&write_cmd, sizeof(write_cmd), NULL);
}


esp_loader_error_t loader_read_reg_cmd(uint32_t address, uint32_t *reg)
{
    read_reg_command_t read_cmd = {
        .common = {
            .direction = 0,
            .command = READ_REG,
            .size = 16,
            .checksum = 0
        },
        .address = address,
    };

    return send_cmd(&read_cmd, sizeof(read_cmd), reg);
}


esp_loader_error_t loader_spi_attach_cmd(uint32_t config)
{
    spi_attach_command_t attach_cmd = {
        .common = {
            .direction = 0,
            .command = SPI_ATTACH,
            .size = 8,
            .checksum = 0
        },
        .configuration = config,
        .zero = 0
    };

    return send_cmd(&attach_cmd, sizeof(attach_cmd), NULL);
}


__attribute__ ((weak)) void esp_loader_debug_print(const char *str)
{

}