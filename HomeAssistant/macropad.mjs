//Put this in zigbee2mqtt/external_converters
//Then reboot z2m in any way and wait at most 1 minute
const CUSTOM_CLUSTER_ID = 0xFC00;   // same as in your C code
const ATTR_BRIGHTNESS_ID = 0x0001;  // same as in your C code
// ---------------------------------------------------------------------------

const fzLocal = {
    custom_brightness: {
        cluster: CUSTOM_CLUSTER_ID,
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg) => {
            if (msg.data[ATTR_BRIGHTNESS_ID] !== undefined) {
                return { brightness: msg.data[ATTR_BRIGHTNESS_ID] };
            }
        },
    },
};

const tzLocal = {
    custom_brightness: {
        key: ['brightness'],
        convertSet: async (entity, key, value, meta) => {
            // Each attribute must include both dataType and value
            const payload = [
                { attribute: ATTR_BRIGHTNESS_ID, value, dataType: 0x20 }, // 0x20 = uint8
            ];

            await entity.write(CUSTOM_CLUSTER_ID, payload, {
                manufacturerCode: 0x1234, // STARKYDIY manufacturer code
            });

            return { state: { brightness: value } };
        },
    },
};
/*
const payload = [
    { attribute: ATTR_BRIGHTNESS_ID, value, dataType: 0x20 },
];
await entity.write(CUSTOM_CLUSTER_ID, payload, { manufacturerCode: 0x1234 });
*/
export default {
    zigbeeModel: ['MACROPAD'],
    model: 'MACROPAD',
    vendor: 'STARKYDIY',
    description: 'ESP32-C6 Macropad with custom brightness cluster',
	documentation: 'https://github.com/StaRky33/ZigbeeMacropad',
    fromZigbee: [fzLocal.custom_brightness],
    toZigbee: [tzLocal.custom_brightness],
    exposes: [
        {
            name: 'brightness',
            property: 'brightness',
            type: 'numeric',
            access: 7, // read/write/report
            unit: '%',
            value_min: 0,
            value_max: 255,
            description: 'Custom brightness level',
        },
    ],
};
