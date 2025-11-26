//Put this in zigbee2mqtt/external_converters
//Then reboot z2m in any way and wait at most 1 minute
// Custom converter for your ESP32-C6 MACROPAD
// Decodes 'raw' frames on cluster manuSpecificAssaDoorLock into an "action" field.

import {presets as e, access as ea} from 'zigbee-herdsman-converters/lib/exposes';

const fzLocal = {
    macropad_button_event: {
        // Matches your logs:
        // type 'raw', cluster 'manuSpecificAssaDoorLock'
        cluster: 'manuSpecificAssaDoorLock',
        type: ['raw'],
        convert: (model, msg, publish, options, meta) => {
            let raw = msg.data;

            // 3 possible shapes:
            // 1) Buffer  -> convert to array
            // 2) Plain array
            // 3) Object {data:[..], type:'Buffer'} (older style)
            if (raw && raw.data && Array.isArray(raw.data)) {
                // Case 3
                raw = raw.data;
            } else if (Buffer.isBuffer(raw)) {
                // Case 1
                raw = Array.from(raw);
            }

            if (!Array.isArray(raw) || raw.length < 3) {
                // Debug log to confirm converter is being called
                meta.logger.debug(
                    `MACROPAD: raw converter got unsupported msg.data = ${JSON.stringify(msg.data)}`
                );
                return {};
            }

            // From logs:
            // [x, x, 0, 15, 1]
            const buttonId = raw[3];
            const actionType = raw[4] ?? 0;

            let actionStr = 'default';
            if (actionType === 1) actionStr = 'single';
            else if (actionType === 2) actionStr = 'double';
			else if (actionType === 3) actionStr = 'hold';

            const action = `button_${buttonId}_${actionStr}`;

            meta.logger.info(
                `MACROPAD: decoded raw=${JSON.stringify(raw)} -> action=${action}`
            );

            return {
                action,          // main field for automations
                button: buttonId,
                action_type: actionStr,
            };
        },
    },
	
	macropad_levels_attrs: {
        // Listen to attribute reports or read responses on 0xFC00
        cluster: 'manuSpecificAssaDoorLock',
        type: ['readResponse', 'attributeReport'],
        convert: (model, msg, publish, options, meta) => {
            const result = {};
            const data = msg.data || {};

            // msg.data keys can be numbers or strings, handle both
            const getAttr = (id) => data[id] ?? data[String(id)];

            const brightness = getAttr(0x0A00);

            if (brightness !== undefined) result.current_brightness = brightness;
            return result;
        },
    },
};
const tzLocal = {
    macropad_current_brightness: {
        key: ['current_brightness'],
        convertSet: async (entity, key, value, meta) => {
            const v = Math.max(0, Math.min(100, Number(value)));
            await entity.write(
                'manuSpecificAssaDoorLock',
                {0x0A00: {value: v, type: 0x20}}, // 0x20 = uint8
                // no manufacturerCode needed if your cluster isn't manufacturer-specific in that sense
            );
            return {state: {current_brightness: v}};
        },
        convertGet: async (entity, key, meta) => {
            await entity.read(
                'manuSpecificAssaDoorLock',
                [0x0A00],
            );
        },
    },
};

export default {
    zigbeeModel: ['MACROPAD'],          // must match Basic cluster 'modelID'
    model: 'MACROPAD',
    vendor: 'STARKYDIY',
    description: 'Custom 16-button macropad (ESP32-C6)',
    fromZigbee: [
        fzLocal.macropad_button_event,
        fzLocal.macropad_levels_attrs,
    ],
    toZigbee: [
        tzLocal.macropad_current_brightness,
    ],
    exposes: [
        e.action([
            'button_0_single', 'button_1_single', 'button_2_single', 'button_3_single',
            'button_4_single', 'button_5_single', 'button_6_single', 'button_7_single',
            'button_8_single', 'button_9_single', 'button_10_single', 'button_11_single',
            'button_12_single', 'button_13_single', 'button_14_single', 'button_15_single',
			'button_0_double', 'button_1_double', 'button_2_double', 'button_3_double',
            'button_4_double', 'button_5_double', 'button_6_double', 'button_7_double',
            'button_8_double', 'button_9_double', 'button_10_double', 'button_11_double',
            'button_12_double', 'button_13_double', 'button_14_double', 'button_15_double',
			'button_0_hold', 'button_1_hold', 'button_2_hold', 'button_3_hold',
            'button_4_hold', 'button_5_hold', 'button_6_hold', 'button_7_hold',
            'button_8_hold', 'button_9_hold', 'button_10_hold', 'button_11_hold',
            'button_12_hold', 'button_13_hold', 'button_14_hold', 'button_15_hold',			
        ]),

        // NEW: config brighntess
        {
            type: 'numeric',
            name: 'current_brightness',
            label: 'Brightness',
            property: 'current_brightness',
            access: 7,  // state + set + get
            category: 'config',
            value_min: 0,
            value_max: 100,
			unit:'%',
            description: 'Global brightness used for click feedbacks',
        },
    ],
};