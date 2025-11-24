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

            // From your logs:
            // [1, 4, 0, 15]
            // [1, 5, 0, 15]
            // [1, 6, 0, 15]
            // [1, 7, 0, 12]
            // [1, 8, 0, 13]
            //
            // We'll treat:
            //   raw[1] = button id
            //   raw[2] = action type (currently 0 in your logs)
            const buttonId = raw[1];
            const actionType = raw[2] ?? 0;

            let actionStr = 'single';
            if (actionType === 1) actionStr = 'double';
            else if (actionType === 2) actionStr = 'hold';

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
};

export default {
    zigbeeModel: ['MACROPAD'],          // must match Basic cluster 'modelID'
    model: 'MACROPAD',
    vendor: 'STARKYDIY',
    description: 'Custom 16-button macropad (ESP32-C6)',
    fromZigbee: [
        fzLocal.macropad_button_event,
    ],
    toZigbee: [],
    exposes: [
        // Pre-declare actions (keep this exactly as you had it)
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
    ],
};
