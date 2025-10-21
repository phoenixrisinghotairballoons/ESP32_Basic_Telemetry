# Remote Control with 2.13" E-Ink (LoRa RFM95) — ESP32 (DOIT DevKit V1)

Short README to get the sketch compiling and running. It targets the **DOIT ESP32 DEVKIT V1** with a 2.13" E-Ink display and an **RFM95 (LoRa 915 MHz)** radio, using priority-first relay control with deterministic ACKs and snappier OFF response.

![Telemetry site from the October 11 flight](./october10flight.jpeg)

---

## What this project does

- Sends commands: `RELAY<n>_ON#<seq>` / `RELAY<n>_OFF#<seq>`
- Expects controller reply: `ACK:<seq>,0x<bitmap>` (bit0 = R1, bit1 = R2, …)
- Adds a quick duplicate resend (~70 ms) to beat collisions (especially on OFF)
- Draws telemetry + states on a 2.13" E-Ink (SSD1680/SSD1675 drivers)
- Optional buzzer alerts on temperature / altitude delta thresholds

---

## Prerequisites

- **Arduino IDE** (or Arduino CLI / PlatformIO)
- **Board:** DOIT ESP32 DEVKIT V1
- **Radio:** RFM95 (LoRa, 915 MHz default in code)
- **Display:** 2.13" E-Ink supported by Adafruit EPD (SSD1680 or SSD1675)

---

## Arduino IDE — ESP32 board setup (DOIT ESP32)

1. **Add ESP32 boards URL**  
   *File → Preferences → Additional Boards Manager URLs*:  
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`

2. **Install ESP32 core**  
   *Tools → Board → Boards Manager…* → search “**ESP32**” → install **ESP32 by Espressif Systems**.

3. **Select board**  
   *Tools → Board*: **DOIT ESP32 DEVKIT V1**

4. **Recommended Tools settings**
   - Upload Speed: 921600 (drop to 460800 if unstable)
   - Flash Frequency: 80 MHz
   - Partition Scheme: “Default 4MB with spiffs” (or the default on your core)

5. **Choose the correct serial Port** for your ESP32.

---

## Libraries to install (all boards)

Install via **Arduino Library Manager** unless noted:

- **RadioHead** (by AirSpayce) — for `RH_RF95` (RFM95)
- **Adafruit EPD**
- **Adafruit GFX Library**
- **Adafruit BusIO**  
*(SPI and Wire come with the ESP32 core.)*

> If your 2.13" panel uses a different controller, set `USE_SSD1680` accordingly.

---

## Hardware & pin map (as in the sketch)

> ⚠️ On ESP32, the `A*` aliases map to specific GPIOs depending on the core/variant. If your board variant doesn’t define `A0/A1/A2`, replace them with the exact GPIO numbers you’ve wired and update the defines.

| Function         | Pin (from code) |
|------------------|------------------|
| Button 1         | `A1` (INPUT_PULLUP) |
| Button 2         | `A0` (INPUT_PULLUP) |
| Piezo buzzer     | `12` |
| RFM95 CS         | `8` |
| RFM95 INT (DIO0) | `3` |
| EPD CS           | `A2` |
| EPD DC           | `6` |
| EPD RESET        | `5` |
| EPD BUSY         | `10` |
| EPD SRAM CS      | `-1` (unused) |

---

## Build & upload

1. Open the sketch in Arduino IDE.
2. Ensure the **board**, **port**, and **libraries** are set as above.
3. Click **Verify** then **Upload**.

---

## Region & radio settings

- Default frequency is **915.0 MHz** in code:
  ```cpp
  rf95.setFrequency(915.0);
