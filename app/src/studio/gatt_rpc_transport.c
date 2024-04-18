/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <sys/types.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/ring_buffer.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/studio/rpc.h>

#include "uuid.h"

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

static bool handling_rx = false;

static void rpc_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);

    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);

    LOG_INF("RPC Notifications %s", notif_enabled ? "enabled" : "disabled");
}

static ssize_t read_rpc_resp(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                             uint16_t len, uint16_t offset) {

    LOG_DBG("Read response for length %d at offset %d", len, offset);
    return 0;
}

static ssize_t write_rpc_req(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                             uint16_t len, uint16_t offset, uint8_t flags) {
    if (handling_rx) {
        uint32_t copied = 0;
        struct ring_buf *rpc_buf = zmk_rpc_get_rx_buf();
        do {
            uint8_t *buffer;
            uint32_t claim_len = ring_buf_put_claim(rpc_buf, &buffer, len - copied);

            if (claim_len > 0) {
                memcpy(buffer, ((uint8_t *)buf) + copied, claim_len);
                copied += claim_len;
            }

            ring_buf_put_finish(rpc_buf, claim_len);
        } while (len - copied > 0);

        zmk_rpc_rx_notify();
    }

    return len;
}

BT_GATT_SERVICE_DEFINE(
    rpc_interface, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(ZMK_STUDIO_BT_SERVICE_UUID)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_STUDIO_BT_RPC_CHRC_UUID),
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT, read_rpc_resp,
                           write_rpc_req, NULL),
    BT_GATT_CCC(rpc_ccc_cfg_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

static int gatt_start_rx() {
    handling_rx = true;
    return 0;
}

static int gatt_stop_rx(void) {
    handling_rx = false;
    return 0;
}

static void notif_rpc_tx_cb(struct k_work *work) {
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    struct ring_buf *tx_buf = zmk_rpc_get_tx_buf();

    if (!conn) {
        LOG_WRN("No active connection for queued data, dropping");
        ring_buf_reset(tx_buf);
        return;
    }

    uint8_t notify_size = 23; // Default MTU size unless negotiated higher
    struct bt_conn_info conn_info;
    if (bt_conn_get_info(conn, &conn_info) >= 0) {
        notify_size = conn_info.le.data_len->tx_max_len;
    }

    uint8_t notify_bytes[notify_size];

    while (ring_buf_size_get(tx_buf) > 0) {
        uint8_t added = 0;
        while (added < notify_size && ring_buf_size_get(tx_buf) > 0) {
            uint8_t *buf;
            int len = ring_buf_get_claim(tx_buf, &buf, notify_size - added);

            if (len > 0) {
                memcpy(notify_bytes + added, buf, len);
            }

            added += len;
            ring_buf_get_finish(tx_buf, len);
        }

        struct bt_gatt_notify_params notify_params = {
            .attr = &rpc_interface.attrs[1],
            .data = notify_bytes,
            .len = added,
        };

        uint8_t notify_attempts = 5;
        do {
            int err = bt_gatt_notify_cb(conn, &notify_params);
            if (err >= 0) {
                break;
            }

            LOG_WRN("Failed to notify the response %d", err);
            k_sleep(K_MSEC(200));
        } while (notify_attempts-- > 0);
    }

    bt_conn_unref(conn);
}

static K_WORK_DEFINE(notify_tx_work, notif_rpc_tx_cb);

struct gatt_write_state {
    uint8_t notify_size;
    uint8_t pending_notify;
};

static void gatt_tx_notify(struct ring_buf *tx_buf, size_t added, bool msg_done, void *user_data) {
    struct gatt_write_state *state = (struct gatt_write_state *)user_data;

    state->pending_notify += added;

    if (msg_done || state->pending_notify > state->notify_size) {
        k_work_submit(&notify_tx_work);
        state->pending_notify = 0;
    }
}

static struct gatt_write_state tx_state = {};

static void *gatt_tx_user_data(void) {
    memset(&tx_state, sizeof(tx_state), 0);

    return &tx_state;
}

ZMK_RPC_TRANSPORT(gatt, ZMK_TRANSPORT_BLE, gatt_start_rx, gatt_stop_rx, gatt_tx_user_data,
                  gatt_tx_notify);
