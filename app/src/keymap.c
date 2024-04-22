/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <drivers/behavior.h>
#include <zephyr/sys/util.h>
#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/sensors.h>
#include <zmk/virtual_key_position.h>

#include <zmk/ble.h>
#if ZMK_BLE_IS_CENTRAL
#include <zmk/split/bluetooth/central.h>
#endif

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/sensor_event.h>

static zmk_keymap_layers_state_t _zmk_keymap_layer_state = 0;
static uint8_t _zmk_keymap_layer_default = 0;

#define DT_DRV_COMPAT zmk_keymap

#if !DT_NODE_EXISTS(DT_DRV_INST(0))

#error "Keymap node not found, check a keymap is available and is has compatible = "zmk,keymap" set"

#endif

#define TRANSFORMED_LAYER(node)                                                                    \
    { LISTIFY(DT_PROP_LEN(node, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), node) }

#if ZMK_KEYMAP_HAS_SENSORS
#define _TRANSFORM_SENSOR_ENTRY(idx, layer)                                                        \
    {                                                                                              \
        .behavior_dev = DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(layer, sensor_bindings, idx)),            \
        .param1 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(layer, sensor_bindings, idx, param1), (0),    \
                              (DT_PHA_BY_IDX(layer, sensor_bindings, idx, param1))),               \
        .param2 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(layer, sensor_bindings, idx, param2), (0),    \
                              (DT_PHA_BY_IDX(layer, sensor_bindings, idx, param2))),               \
    }

#define SENSOR_LAYER(node)                                                                         \
    COND_CODE_1(                                                                                   \
        DT_NODE_HAS_PROP(node, sensor_bindings),                                                   \
        ({LISTIFY(DT_PROP_LEN(node, sensor_bindings), _TRANSFORM_SENSOR_ENTRY, (, ), node)}),      \
        ({}))

#endif /* ZMK_KEYMAP_HAS_SENSORS */

#define LAYER_NAME(node) DT_PROP_OR(node, display_name, DT_PROP_OR(node, label, NULL))

// State

// When a behavior handles a key position "down" event, we record the layer state
// here so that even if that layer is deactivated before the "up", event, we
// still send the release event to the behavior in that layer also.
static uint32_t zmk_keymap_active_behavior_layer[ZMK_KEYMAP_LEN];

static struct zmk_behavior_binding zmk_keymap[ZMK_KEYMAP_LAYERS_LEN][ZMK_KEYMAP_LEN] = {
    DT_INST_FOREACH_CHILD_SEP(0, TRANSFORMED_LAYER, (, ))};

static const char *zmk_keymap_layer_names[ZMK_KEYMAP_LAYERS_LEN] = {
    DT_INST_FOREACH_CHILD_SEP(0, LAYER_NAME, (, ))};

#if ZMK_KEYMAP_HAS_SENSORS

static struct zmk_behavior_binding
    zmk_sensor_keymap[ZMK_KEYMAP_LAYERS_LEN][ZMK_KEYMAP_SENSORS_LEN] = {
        DT_INST_FOREACH_CHILD_SEP(0, SENSOR_LAYER, (, ))};

#endif /* ZMK_KEYMAP_HAS_SENSORS */

static inline int set_layer_state(uint8_t layer, bool state) {
    int ret = 0;
    if (layer >= ZMK_KEYMAP_LAYERS_LEN) {
        return -EINVAL;
    }

    // Default layer should *always* remain active
    if (layer == _zmk_keymap_layer_default && !state) {
        return 0;
    }

    zmk_keymap_layers_state_t old_state = _zmk_keymap_layer_state;
    WRITE_BIT(_zmk_keymap_layer_state, layer, state);
    // Don't send state changes unless there was an actual change
    if (old_state != _zmk_keymap_layer_state) {
        LOG_DBG("layer_changed: layer %d state %d", layer, state);
        ret = raise_layer_state_changed(layer, state);
        if (ret < 0) {
            LOG_WRN("Failed to raise layer state changed (%d)", ret);
        }
    }

    return ret;
}

uint8_t zmk_keymap_layer_default(void) { return _zmk_keymap_layer_default; }

zmk_keymap_layers_state_t zmk_keymap_layer_state(void) { return _zmk_keymap_layer_state; }

bool zmk_keymap_layer_active_with_state(uint8_t layer, zmk_keymap_layers_state_t state_to_test) {
    // The default layer is assumed to be ALWAYS ACTIVE so we include an || here to ensure nobody
    // breaks up that assumption by accident
    return (state_to_test & (BIT(layer))) == (BIT(layer)) || layer == _zmk_keymap_layer_default;
};

bool zmk_keymap_layer_active(uint8_t layer) {
    return zmk_keymap_layer_active_with_state(layer, _zmk_keymap_layer_state);
};

uint8_t zmk_keymap_highest_layer_active(void) {
    for (uint8_t layer = ZMK_KEYMAP_LAYERS_LEN - 1; layer > 0; layer--) {
        if (zmk_keymap_layer_active(layer)) {
            return layer;
        }
    }
    return zmk_keymap_layer_default();
}

int zmk_keymap_layer_activate(uint8_t layer) { return set_layer_state(layer, true); };

int zmk_keymap_layer_deactivate(uint8_t layer) { return set_layer_state(layer, false); };

int zmk_keymap_layer_toggle(uint8_t layer) {
    if (zmk_keymap_layer_active(layer)) {
        return zmk_keymap_layer_deactivate(layer);
    }

    return zmk_keymap_layer_activate(layer);
};

int zmk_keymap_layer_to(uint8_t layer) {
    for (int i = ZMK_KEYMAP_LAYERS_LEN - 1; i >= 0; i--) {
        zmk_keymap_layer_deactivate(i);
    }

    zmk_keymap_layer_activate(layer);

    return 0;
}

bool is_active_layer(uint8_t layer, zmk_keymap_layers_state_t layer_state) {
    return (layer_state & BIT(layer)) == BIT(layer) || layer == _zmk_keymap_layer_default;
}

const char *zmk_keymap_layer_name(uint8_t layer) {
    if (layer >= ZMK_KEYMAP_LAYERS_LEN) {
        return NULL;
    }

    return zmk_keymap_layer_names[layer];
}

const struct zmk_behavior_binding *zmk_keymap_get_layer_binding_at_idx(uint8_t layer,
                                                                       uint8_t binding_idx) {
    if (layer >= ZMK_KEYMAP_LAYERS_LEN) {
        return NULL;
    }

    if (binding_idx >= ZMK_KEYMAP_LEN) {
        return NULL;
    }

    return &zmk_keymap[layer][binding_idx];
}

#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)

#define PENDING_ARRAY_SIZE DIV_ROUND_UP(ZMK_KEYMAP_LEN, 8)

static uint8_t zmk_keymap_layer_pending_changes[ZMK_KEYMAP_LAYERS_LEN][PENDING_ARRAY_SIZE];

#endif // IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)

int zmk_keymap_set_layer_binding_at_idx(uint8_t layer, uint8_t binding_idx,
                                        struct zmk_behavior_binding binding) {
    if (layer >= ZMK_KEYMAP_LAYERS_LEN || binding_idx >= ZMK_KEYMAP_LEN) {
        return -EINVAL;
    }

#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)
    uint8_t *pending = zmk_keymap_layer_pending_changes[layer];

    WRITE_BIT(pending[binding_idx / 8], binding_idx % 8, 1);
#endif // IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)

    // TODO: Need a mutex to protect access to the keymap data?
    memcpy(&zmk_keymap[layer][binding_idx], &binding, sizeof(binding));

    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)

#define PENDING_ARRAY_SIZE DIV_ROUND_UP(ZMK_KEYMAP_LEN, 8)

static uint8_t zmk_keymap_layer_pending_changes[ZMK_KEYMAP_LAYERS_LEN][PENDING_ARRAY_SIZE];

struct zmk_behavior_binding_setting {
    zmk_behavior_local_id_t behavior_local_id;
    uint32_t param1;
    uint32_t param2;
} __packed;

int zmk_keymap_save_changes(void) {
    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
        uint8_t *pending = zmk_keymap_layer_pending_changes[l];

        for (int kp = 0; kp < ZMK_KEYMAP_LEN; kp++) {
            if (pending[kp / 8] & BIT(kp % 8)) {
                LOG_DBG("Pending save for layer %d at key position %d", l, kp);

                struct zmk_behavior_binding *binding = &zmk_keymap[l][kp];
                struct zmk_behavior_binding_setting binding_setting = {
                    .behavior_local_id = zmk_behavior_get_local_id(binding->behavior_dev),
                    .param1 = binding->param1,
                    .param2 = binding->param2,
                };

                // We can skip any trailing zero params, regardless of the behavior
                // and if those params are meaningful.
                size_t len = sizeof(binding_setting);
                if (binding_setting.param2 == 0) {
                    len -= 4;

                    if (binding_setting.param1 == 0) {
                        len -= 4;
                    }
                }

                char setting_name[20];
                sprintf(setting_name, "keymap/l/%d/%d", l, kp);

                settings_save_one(setting_name, &binding_setting, len);
            }
        }

        *pending = 0;
    }

    return 0;
}

int zmk_keymap_discard_changes(void) { return settings_load_subtree("keymap/l"); }

#else

int zmk_keymap_save_changes(void) { return -ENOTSUP; }

int zmk_keymap_discard_changes(void) { return -ENOTSUP; }

#endif // IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)

int invoke_locally(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event,
                   bool pressed) {
    if (pressed) {
        return behavior_keymap_binding_pressed(binding, event);
    } else {
        return behavior_keymap_binding_released(binding, event);
    }
}

int zmk_keymap_apply_position_state(uint8_t source, int layer, uint32_t position, bool pressed,
                                    int64_t timestamp) {
    // We want to make a copy of this, since it may be converted from
    // relative to absolute before being invoked
    struct zmk_behavior_binding binding = zmk_keymap[layer][position];
    const struct device *behavior;
    struct zmk_behavior_binding_event event = {
        .layer = layer,
        .position = position,
        .timestamp = timestamp,
    };

    LOG_DBG("layer: %d position: %d, binding name: %s", layer, position, binding.behavior_dev);

    behavior = zmk_behavior_get_binding(binding.behavior_dev);

    if (!behavior) {
        LOG_WRN("No behavior assigned to %d on layer %d", position, layer);
        return 1;
    }

    int err = behavior_keymap_binding_convert_central_state_dependent_params(&binding, event);
    if (err) {
        LOG_ERR("Failed to convert relative to absolute behavior binding (err %d)", err);
        return err;
    }

    enum behavior_locality locality = BEHAVIOR_LOCALITY_CENTRAL;
    err = behavior_get_locality(behavior, &locality);
    if (err) {
        LOG_ERR("Failed to get behavior locality %d", err);
        return err;
    }

    switch (locality) {
    case BEHAVIOR_LOCALITY_CENTRAL:
        return invoke_locally(&binding, event, pressed);
    case BEHAVIOR_LOCALITY_EVENT_SOURCE:
#if ZMK_BLE_IS_CENTRAL
        if (source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
            return invoke_locally(&binding, event, pressed);
        } else {
            return zmk_split_bt_invoke_behavior(source, &binding, event, pressed);
        }
#else
        return invoke_locally(&binding, event, pressed);
#endif
    case BEHAVIOR_LOCALITY_GLOBAL:
#if ZMK_BLE_IS_CENTRAL
        for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
            zmk_split_bt_invoke_behavior(i, &binding, event, pressed);
        }
#endif
        return invoke_locally(&binding, event, pressed);
    }

    return -ENOTSUP;
}

int zmk_keymap_position_state_changed(uint8_t source, uint32_t position, bool pressed,
                                      int64_t timestamp) {
    if (pressed) {
        zmk_keymap_active_behavior_layer[position] = _zmk_keymap_layer_state;
    }
    for (int layer = ZMK_KEYMAP_LAYERS_LEN - 1; layer >= _zmk_keymap_layer_default; layer--) {
        if (zmk_keymap_layer_active_with_state(layer, zmk_keymap_active_behavior_layer[position])) {
            int ret = zmk_keymap_apply_position_state(source, layer, position, pressed, timestamp);
            if (ret > 0) {
                LOG_DBG("behavior processing to continue to next layer");
                continue;
            } else if (ret < 0) {
                LOG_DBG("Behavior returned error: %d", ret);
                return ret;
            } else {
                return ret;
            }
        }
    }

    return -ENOTSUP;
}

#if ZMK_KEYMAP_HAS_SENSORS
int zmk_keymap_sensor_event(uint8_t sensor_index,
                            const struct zmk_sensor_channel_data *channel_data,
                            size_t channel_data_size, int64_t timestamp) {
    bool opaque_response = false;

    for (int layer = ZMK_KEYMAP_LAYERS_LEN - 1; layer >= 0; layer--) {
        struct zmk_behavior_binding *binding = &zmk_sensor_keymap[layer][sensor_index];

        LOG_DBG("layer: %d sensor_index: %d, binding name: %s", layer, sensor_index,
                binding->behavior_dev);

        const struct device *behavior = zmk_behavior_get_binding(binding->behavior_dev);
        if (!behavior) {
            LOG_DBG("No behavior assigned to %d on layer %d", sensor_index, layer);
            continue;
        }

        struct zmk_behavior_binding_event event = {
            .layer = layer,
            .position = ZMK_VIRTUAL_KEY_POSITION_SENSOR(sensor_index),
            .timestamp = timestamp,
        };

        int ret = behavior_sensor_keymap_binding_accept_data(
            binding, event, zmk_sensors_get_config_at_index(sensor_index), channel_data_size,
            channel_data);

        if (ret < 0) {
            LOG_WRN("behavior data accept for behavior %s returned an error (%d). Processing to "
                    "continue to next layer",
                    binding->behavior_dev, ret);
            continue;
        }

        enum behavior_sensor_binding_process_mode mode =
            (!opaque_response && layer >= _zmk_keymap_layer_default &&
             zmk_keymap_layer_active(layer))
                ? BEHAVIOR_SENSOR_BINDING_PROCESS_MODE_TRIGGER
                : BEHAVIOR_SENSOR_BINDING_PROCESS_MODE_DISCARD;

        ret = behavior_sensor_keymap_binding_process(binding, event, mode);

        if (ret == ZMK_BEHAVIOR_OPAQUE) {
            LOG_DBG("sensor event processing complete, behavior response was opaque");
            opaque_response = true;
        } else if (ret < 0) {
            LOG_DBG("Behavior returned error: %d", ret);
            return ret;
        }
    }

    return 0;
}

#endif /* ZMK_KEYMAP_HAS_SENSORS */

int keymap_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos_ev;
    if ((pos_ev = as_zmk_position_state_changed(eh)) != NULL) {
        return zmk_keymap_position_state_changed(pos_ev->source, pos_ev->position, pos_ev->state,
                                                 pos_ev->timestamp);
    }

#if ZMK_KEYMAP_HAS_SENSORS
    const struct zmk_sensor_event *sensor_ev;
    if ((sensor_ev = as_zmk_sensor_event(eh)) != NULL) {
        return zmk_keymap_sensor_event(sensor_ev->sensor_index, sensor_ev->channel_data,
                                       sensor_ev->channel_data_size, sensor_ev->timestamp);
    }
#endif /* ZMK_KEYMAP_HAS_SENSORS */

    return -ENOTSUP;
}

ZMK_LISTENER(keymap, keymap_listener);
ZMK_SUBSCRIPTION(keymap, zmk_position_state_changed);

#if ZMK_KEYMAP_HAS_SENSORS
ZMK_SUBSCRIPTION(keymap, zmk_sensor_event);
#endif /* ZMK_KEYMAP_HAS_SENSORS */

#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)

static int keymap_handle_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;

    LOG_DBG("Setting Keymap setting %s", name);

    if (settings_name_steq(name, "l", &next) && next) {
        char *endptr;
        uint8_t layer = strtoul(next, &endptr, 10);
        if (*endptr != '/') {
            LOG_WRN("Invalid layer number: %s with endptr %s", next, endptr);
            return -EINVAL;
        }

        uint8_t key_position = strtoul(endptr + 1, &endptr, 10);

        if (*endptr != '\0') {
            LOG_WRN("Invalid key_position number: %s with endptr %s", next, endptr);
            return -EINVAL;
        }

        if (len > sizeof(struct zmk_behavior_binding_setting)) {
            LOG_ERR("Too large binding setting size (got %d expected %d)", len,
                    sizeof(struct zmk_behavior_binding_setting));
            return -EINVAL;
        }

        if (layer >= ZMK_KEYMAP_LAYERS_LEN) {
            LOG_WRN("Layer %d is larger than max of %d", layer, ZMK_KEYMAP_LAYERS_LEN);
            return -EINVAL;
        }

        if (key_position >= ZMK_KEYMAP_LEN) {
            LOG_WRN("Key position %d is larger than max of %d", key_position, ZMK_KEYMAP_LEN);
            return -EINVAL;
        }

        struct zmk_behavior_binding_setting binding_setting = {0};
        int err = read_cb(cb_arg, &binding_setting, len);
        if (err <= 0) {
            LOG_ERR("Failed to handle keymap binding from settings (err %d)", err);
            return err;
        }

        const char *name =
            zmk_behavior_find_behavior_name_from_local_id(binding_setting.behavior_local_id);

        if (!name) {
            LOG_WRN("Loaded device %d from settings but no device found by that local ID",
                    binding_setting.behavior_local_id);
            return -ENODEV;
        }

        zmk_keymap[layer][key_position] = (struct zmk_behavior_binding){
            .behavior_dev = name,
            .param1 = binding_setting.param1,
            .param2 = binding_setting.param2,
        };
    }

    return 0;
};

SETTINGS_STATIC_HANDLER_DEFINE(keymap, "keymap", NULL, keymap_handle_set, NULL, NULL);

int keymap_init(void) {
    settings_subsys_init();

    settings_load_subtree("keymap");

    return 0;
}

SYS_INIT(keymap_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif // IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)
