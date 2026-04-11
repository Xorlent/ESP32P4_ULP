/**
 * @file ulp_programs.h
 * @brief Catalogue of pre-built LP core binaries bundled in this library.
 */

#pragma once

#include "ulp_gpio_wakeup.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *data;
    size_t         size;
} ulp_program_desc_t;

static inline const ulp_program_desc_t *ulp_program_get(uint32_t id)
{
    static const ulp_program_desc_t table[] = {
        /* 0 – NONE         */ { NULL,                    0                         },
        /* 1 – GPIO_WAKEUP  */ { ulp_gpio_wakeup_bin,     sizeof(ulp_gpio_wakeup_bin) },
    };
    const size_t table_len = sizeof(table) / sizeof(table[0]);
    if (id >= table_len) return NULL;
    return &table[id];
}

#ifdef __cplusplus
}
#endif
