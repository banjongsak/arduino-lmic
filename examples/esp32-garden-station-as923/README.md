# Garden climate station - ESP32 + SHT31 + LoRaWAN AS923 (Thailand)

Outdoor version of the micro-climate station. It uses weather-proof parts,
deep sleep for solar power, and keeps the LoRaWAN session in RTC memory so it
joins the network only once.

## Hardware

| Part | Choice | Note |
|---|---|---|
| Board | TTGO LoRa32 (923 MHz) or RAK Wisblock | ESP32 + SX1276 |
| Sensor | **SHT31-D probe with PTFE filter cap** | ±0.2 °C, ±2 %RH, survives rain and dew |
| Shield | White multi-plate radiation shield | Without it readings run 5-10 °C too high |
| Box | IP65/IP67 box + IP68 cable glands + vent plug | Glands point down; add silica gel |
| Antenna | 3-5 dBi outdoor fiberglass, 923 MHz | Seal the connector with self-amalgamating tape |
| Battery | LiFePO4 18650/32650 | Better than Li-ion in Thai heat |
| Power | 5-10 W solar panel + charge board | Deep sleep keeps it running through rainy days |
| Surge | Gas-discharge arrester on the antenna line | Recommended for pole mounting |

## Wiring

| SX1276 | ESP32 | | SHT31-D | ESP32 |
|---|---|---|---|---|
| NSS  | GPIO18 | | SDA | GPIO21 |
| RST  | GPIO14 | | SCL | GPIO22 |
| DIO0 | GPIO26 | | VIN | GPIO25 (switched power, optional) |
| DIO1 | GPIO33 | | GND | GND |
| SCK  | GPIO5  | | | |
| MISO | GPIO19 | | | |
| MOSI | GPIO27 | | | |

Battery divider (two 100k resistors) -> GPIO35.
SHT31 I2C address is 0x44, or 0x45 if the ADDR pad is pulled high.

## Band setting

Edit `project_config/lmic_project_config.h`:

```c
#define CFG_eu868 1
#define CFG_sx1276_radio 1
```

AS923 uses the same dynamic-channel stack as EU868, so the sketch builds as
EU868 and swaps in the AS923 channels at run time: 923.2 - 924.6 MHz,
join on 923.2 / 923.4, RX2 = 923.2 MHz SF10, TX power 14 dBm.

## Libraries

* this LMIC library
* Adafruit SHT31 Library
* Adafruit BusIO
* Adafruit Unified Sensor

## How deep sleep works here

1. First boot: normal OTAA join. The session keys (NetID, DevAddr, NwkSKey,
   AppSKey) are copied into RTC memory.
2. Send the uplink, then `esp_deep_sleep_start()` for `SLEEP_MINUTES`.
3. On wake, RTC memory still holds the session, so `LMIC_setSession()`
   restores it and the frame counter, and the device sends immediately -
   no join, very little air time, very little battery.

Because of this, ADR is turned off and a fixed data rate (SF9) is used.
ADR cannot work properly when the device sleeps between every uplink.

**Important on the network server:** turn **off** "frame counter checks"
strict mode, or make sure the counter is stored. The sketch restores the
counter itself, so normally this is fine, but a full power loss (RTC memory
cleared) makes the device join again and the counter restarts.

Deep sleep current is about 20-40 µA on a TTGO board with the radio in
sleep. Remove or cut the power LED and the on-board regulator's quiescent
load if you want to go lower.

## Payload (7 bytes)

| Bytes | Field | Encoding |
|---|---|---|
| 0-1 | temperature | int16, big endian, x100 °C |
| 2-3 | humidity | uint16, big endian, x100 %RH |
| 4-5 | battery | uint16, big endian, mV |
| 6 | boot counter | uint8, wraps at 255 (health check) |

TTN v3 decoder:

```javascript
function decodeUplink(input) {
  var b = input.bytes;
  var t = (b[0] << 8) | b[1];
  if (t & 0x8000) { t -= 0x10000; }
  return {
    data: {
      temperature: t / 100,
      humidity: ((b[2] << 8) | b[3]) / 100,
      battery: ((b[4] << 8) | b[5]) / 1000,
      boot: b[6]
    }
  };
}
```

## Field tips

* Mount the shield 1.2-1.5 m above ground, over grass, not over concrete.
* Put fine mesh over shield openings so ants do not nest inside.
* Spray the ESP32 board with conformal coating before closing the box.
* 10 minutes between uplinks is a good balance of data and battery. Do not
  go below 1 minute, and keep to your network's fair-use policy.
