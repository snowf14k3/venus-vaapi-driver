// SPDX-License-Identifier: MIT
#ifndef VENUS_BIT_WRITER_H
#define VENUS_BIT_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct venus_bit_writer {
    /* H.264 syntax is written most-significant bit first into the RBSP. */
    uint8_t *data;
    size_t capacity;
    size_t bit_count;
    bool overflow;
};

void venus_bits_init(struct venus_bit_writer *writer, uint8_t *data,
                     size_t capacity);
void venus_bits_write(struct venus_bit_writer *writer, uint32_t value,
                      unsigned int bits);
void venus_bits_write_ue(struct venus_bit_writer *writer, uint32_t value);
void venus_bits_write_se(struct venus_bit_writer *writer, int32_t value);
void venus_bits_finish_rbsp(struct venus_bit_writer *writer);
size_t venus_bits_size(const struct venus_bit_writer *writer);
bool venus_bits_ok(const struct venus_bit_writer *writer);

#endif
