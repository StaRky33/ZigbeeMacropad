//Put this in zigbee2mqtt/external_converters
//Then reboot z2m in any way and wait at most 1 minute

const fzLocal = {
    custom_brightness: {
        cluster: 'manuSpecificStarkyDIY',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.data.brightness !== undefined) {
                return { brightness: msg.data.brightness };
            }
        },
    },
};

const tzLocal = {
    custom_brightness: {
        key: ['brightness'],
        convertSet: async (entity, key, value, meta) => {
            await entity.write('manuSpecificStarkyDIY', { brightness: value });
            return { state: { brightness: value } };
        },
    },
};

export default {
    zigbeeModel: ['MACROPAD'],
    model: 'MACROPAD',
    vendor: 'STARKYDIY',
    description: 'ESP32-C6 Macropad with custom brightness cluster',
    fromZigbee: [fzLocal.custom_brightness],
    toZigbee: [tzLocal.custom_brightness],
    exposes: [{ name: 'brightness', property: 'brightness', type: 'numeric', access: 7, unit: '%', value_min: 0, value_max: 100 }],
};
