# 🧠 Zigbee Macropad (ESP32-C6)

A compact **16-key Zigbee macropad** powered by the an **ESP32-C6**, designed for integration with **Home Assistant (ZIGBEE2MQTT)**.  
It provides tactile mechanical key input, configurable LED feedback, and a fully 3D-printed case.
It gives 3 sorts of inputs, single click, double clicks and long click to add into any automation to control device remotely.

---

## ⚙️ Hardware

| Component | Description |
|------------|-------------|
| **MCU** | nanoESP32-C6-N8 |
| **Switches** | 16 x Cherry MX Red (linear) |
| **Battery** | 2 x EBL 1.5 V lithium-ion rechargeable AA cell |
| **Buttons** | 6 mm tactile push button (BOOT / Reset) |
| **Power switch** | 13 mm by 8.5 mm on/off slide switch |
| **BOOST Converter** | MT3608 DC-DC Boost converter (3.1v to 3.3v) |
| **Magnets** | 4 x 10 mm × 2 mm neodymium discs |
| **Threaded inserts** | 4 x M2.5 heat-set brass inserts |
| **Screws** | 4 x M2.5 × 5 mm machine screws |
| **Case** | 3D-printed PLA enclosure (about 79g with cap switches and supports) |

---

## 🧩 3D-Printed Parts

Designed and customized in **Tinkercad**, printed with **Centauri Carbon and white PLA**.  
Printed at 0.2 mm layer height, 15–20% infill, no supports required.

### 🔗 Remixed Models
- [16 Keys Macropad](https://www.printables.com/model/140766-16-keys-macropad)  
- [AA Battery Holder for Dupont Jumper](https://www.printables.com/model/380920-aa-battery-holder-for-dupont-jumper)  
- [Simple Cherry MX Keycap](https://www.printables.com/model/118708-simple-cherry-mx-keycap)

---

## 💻 Code

Development began in **Arduino IDE**, later migrated to **VS Code with ESP-IDF v5.3.4** for Zigbee and multitasking support.

### 🧠 Software Stack
- **Framework:** ESP-IDF v5.3.4  
- **Zigbee SDK:** [Espressif ESP-Zigbee-SDK](https://github.com/espressif/esp-zigbee-sdk)  
- **Zigbee Role:** Router / End Device  
- **Endpoint Type:** HA Dimmable Light (only exposes brightness to Home Assistant) WIP

### 🔧 Functionality
- 16 GPIO-connected keys with **single**, **double**, and **long press** detection
- **LED feedback** color for each type of press  
- **LED feedback** brightness controlled via Zigbee “brightness” attribute  
- **BOOT button** triggers Zigbee factory reset and new pairing mode  
- **Blinking red LED** indicates pairing state  
- **Debounce and ISR-driven** button logic for reliability  

---

## 🗺️ Circuit Schematic
![Circuit Schematics](Pictures/circuitSchematics.png)
---

## 🧰 Build Instructions

### 🪛 1. Print and Prepare the Case
- Print all parts. Only the remix part is present in github, everything else can be found on printables.
- Verify that magnets, switches, and PCB fit snugly before final assembly.
- Insert **M2.5 heat-set inserts** into the designated mounting points using a soldering iron at ~200 °C.

### ⚡ 2. Mount the Components
- **Cherry MX switches**: press-fit into the 16-slot plate, solder to perfboard or PCB or with simple wires.
- **nanoESP32-C6-N8**: simply clip in place with any fixation.
- **6 mm BOOT button**: mount to a small hole on the side (for Zigbee reset). Add some glue from glue gun.
- **Power switch**: connect inline with battery’s.
- **Battery holder**: Press fit battery inside. The EBL battery have a builtin BMS so I glue them in their slot with the micro usb accessible outside.
- **Magnets**: press-fit into the lid and base for a secure snap fit. You can add super glue to make sure they don't move.

### 🔋 3. Wiring Overview
| Connection | Description |
|-------------|-------------|
| **GPIO 2,3,4,5 and 18,19,20,21** | Follow the wiring diagram.|
| **GPIO 9** | BOOT / Reset button. Connect to one side of the 6mm button and the other to any GND. |
| **3.3 V & GND** | Power rails for ESP32-C6 and key pull-ups. |
| **Battery pack** | 3 x 1.5 V Li-ion cell inline. Minus to Vin- and Plus to Vin+ of MT3608 (reach 4.5V with no cutout and goes to 0 when empty). |
| **MT3608** | Vin+ and Vin- to battery.Vout+ and Vout- to 3.3V and GND on ESP32 |
| **On/Off switch** | Inline with battery lead. |

> 💡 Use internal pull-ups on all button pins; connect switches to **GND**.

### 🔧 4. Flash the Firmware
1. Install **ESP-IDF v5.3.4** (or newer).  
2. Clone or copy the project to your workspace.  
3. Build and flash:
   ```bash
   idf.py set-target esp32c6
   idf.py build
   idf.py flash monitor
The device will start blinking red to indicate Zigbee pairing mode.

---

## 🔗 Pair with Home Assistant

Add macropad.mjs file to config/zigbee2mqtt/external_converters (if folder does not exist create it).
In Home Assistant, open Settings → Devices & Services → Zigbee2MQTT → Permit join.
Test button clicks (single, double, hold) — LED flashes will reflect the button type.
You can now create an automation based on the button clicks.

---

## ✨ Features Summary

🔘 16 mechanical switches with multiple click detection
💡 LED feedback brightness linked to Zigbee brightness setting
🔄 BOOT button triggers factory reset and re-pairing
🔴 Blinking red LED during pairing
🧱 Modular, 3D-printed enclosure with magnets and inserts
🪫 Battery powered with on/off switch

---

## 🧾 License

This project is released under the MIT License.
Remixed 3D models remain under their respective creator licenses (see linked Printables pages).

---

## 📸 Gallery

//TODO Add pictures
