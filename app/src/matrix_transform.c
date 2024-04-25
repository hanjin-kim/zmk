/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/matrix_transform.h>
#include <zmk/matrix.h>
#include <dt-bindings/zmk/matrix_transform.h>

#define DT_DRV_COMPAT zmk_matrix_transform

struct zmk_matrix_transform_entry {
    uint16_t row;
    uint16_t column;
};

struct zmk_matrix_transform {
    const struct zmk_matrix_transform_entry *entries;
    size_t entries_len;
};

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_matrix_transform)

#define ZMK_KEYMAP_TRANSFORM_NODE DT_CHOSEN(zmk_matrix_transform)

#define TRANSFORM_ENTRY(i, n)                                                                      \
    {                                                                                              \
        .row = KT_ROW(DT_INST_PROP_BY_IDX(n, map, i)),                                             \
        .column = KT_COL(DT_INST_PROP_BY_IDX(n, map, i))                                           \
    }

#define MATRIX_TRANSFORM_INIT(n)                                                                   \
    static const struct zmk_matrix_transform_entry _CONCAT(zmk_transform_entries_,                 \
                                                           n)[DT_INST_PROP_LEN(n, map)] = {        \
        LISTIFY(DT_INST_PROP_LEN(n, map), TRANSFORM_ENTRY, (, ), n)};                              \
    const struct zmk_matrix_transform _CONCAT(zmk_matrix_transform_, DT_DRV_INST(n)) = {           \
        .entries = _CONCAT(zmk_transform_entries_, n),                                             \
        .entries_len = DT_INST_PROP_LEN(n, map),                                                   \
    };

DT_INST_FOREACH_STATUS_OKAY(MATRIX_TRANSFORM_INIT);

#elif DT_HAS_CHOSEN(zmk_kscan)

static struct zmk_matrix_transform_entry zmk_transform_entries_default[ZMK_KEYMAP_LEN] = {};

const struct zmk_matrix_transform zmk_matrix_transform_default = {
    .entries = zmk_transform_entries_default,
    .entries_len = ZMK_KEYMAP_LEN,
};

static int init_synth_matrix_transform(void) {
    for (int i = 0; i < ZMK_KEYMAP_LEN; i++) {
        zmk_transform_entries_default[i] = (struct zmk_matrix_transform_entry){
            .row = (i / ZMK_MATRIX_COLS), .column = (i % ZMK_MATRIX_COLS)};
    }

    return 0;
}

SYS_INIT(init_synth_matrix_transform, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#else

#error "Need a matrix tranform or compatible kscan selected to determine keymap size!"

#endif // DT_HAS_COMPAT_STATUS_OKAY(zmk_matrix_transform)

int32_t zmk_matrix_transform_row_column_to_position(zmk_matrix_transform_t mt, uint32_t row,
                                                    uint32_t column) {
#if DT_NODE_HAS_PROP(ZMK_KEYMAP_TRANSFORM_NODE, col_offset)
    column += DT_PROP(ZMK_KEYMAP_TRANSFORM_NODE, col_offset);
#endif

#if DT_NODE_HAS_PROP(ZMK_KEYMAP_TRANSFORM_NODE, row_offset)
    row += DT_PROP(ZMK_KEYMAP_TRANSFORM_NODE, row_offset);
#endif

#ifdef ZMK_KEYMAP_TRANSFORM_NODE

    for (int i = 0; i < mt->entries_len; i++) {
        const struct zmk_matrix_transform_entry *entry = &mt->entries[i];

        if (entry->row == row && entry->column == column) {
            return i;
        }
    }

    return -EINVAL;

#else

    return (row * ZMK_MATRIX_COLS) + column;

#endif /* ZMK_KEYMAP_TRANSFORM_NODE */
};