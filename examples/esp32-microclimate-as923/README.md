# ESP32 micro-climate station on LoRaWAN AS923 (Thailand)

Reads temperature and humidity with a DHT22 and sends them over LoRaWAN
(OTAA join) every 5 minutes.

## 1. Band setting

Thailand uses **AS923-1** (920-925 MHz). This LMIC version has no AS923 code,
but AS923 uses the same *dynamic channel* stack as EU868, so the sketch is
built as EU868 and the channel list is replaced at runtime
(`setupChannelsAS923()` in the sketch).

Edit `project_config/lmic_project_config.h` in the library folder:

```c
#define CFG_eu868 1
#define CFG_sx1276_radio 1
// #define LMIC_USE_INTERRUPTS
```

Make sure `CFG_us915` is **not** defined.

Channels used: 923.2, 923.4, 923.6, 923.8, 924.0, 924.2, 924.4, 924.6 MHz.
Join channels: 923.2 and 923.4 MHz. RX2: 923.2 MHz, SF10BW125.
TX power is set to 14 dBm, which matches the Thai NBTC limit (25 mW ERP)
for this band.

## 2. Wiring (TTGO LoRa32 / Heltec style)

| SX1276 / RFM95 | ESP32  |
|----------------|--------|
| NSS            | GPIO18 |
| RST            | GPIO14 |
| DIO0           | GPIO26 |
| DIO1           | GPIO33 |
| DIO2           | GPIO32 |
| SCK            | GPIO5  |
| MISO           | GPIO19 |
| MOSI           | GPIO27 |

DHT22 data pin -> GPIO4, with a 10k pull-up resistor to 3V3.
Optional battery divider (two 100k resistors) -> GPIO35.

If your board wires the radio differently, change `lmic_pins` and the
`SPI.begin(...)` call in the sketch.

## 3. Libraries

* this LMIC library
* "DHT sensor library" by Adafruit
* "Adafruit Unified Sensor"

## 4. Keys

Register the device on your network server (The Things Network, ChirpStack,
or a Thai operator), choose **OTAA**, then copy into the sketch:

* `APPEUI` (JoinEUI) - least significant byte first
* `DEVEUI` - least significant byte first
* `APPKEY` - as shown on the console (most significant byte first)

## 5. Payload format (6 bytes)

| Bytes | Field       | Encoding                  |
|-------|-------------|---------------------------|
| 0-1   | temperature | int16, big endian, x100 C |
| 2-3   | humidity    | uint16, big endian, x100 %|
| 4-5   | battery     | uint16, big endian, mV    |

TTN v3 payload formatter (JavaScript):

```javascript
function decodeUplink(input) {
  var b = input.bytes;
  var t = (b[0] << 8) | b[1];
  if (t & 0x8000) { t -= 0x10000; }   // signed
  return {
    data: {
      temperature: t / 100,
      humidity: ((b[2] << 8) | b[3]) / 100,
      battery: ((b[4] << 8) | b[5]) / 1000
    }
  };
}
```

## 6. Notes

* Do not send faster than about once a minute. Keep to the fair-use policy
  of your network (TTN: 30 s of air time per device per day).
* For battery powered stations, add deep sleep between uplinks. That needs
  the LoRaWAN session (ABP style or saved OTAA session) to be kept in RTC
  memory, because LMIC loses its state on reset.
* Put the DHT22 in a ventilated radiation shield, out of direct sun, so the
  readings are real air temperature.
