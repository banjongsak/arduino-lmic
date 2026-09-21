/*******************************************************************************
 * ESP32 micro-climate station -> LoRaWAN (AS923, Thailand)
 *
 * Reads air temperature and relative humidity from a DHT22 (AM2302) sensor
 * and sends them as a 6-byte uplink on LoRaWAN using OTAA.
 *
 * Region: AS923-1, the band used in Thailand (920 - 925 MHz).
 *         This LMIC version has no built-in AS923 support, so the sketch is
 *         built with CFG_eu868 (the dynamic-channel EU stack) and the EU
 *         channels are replaced at runtime by the AS923 channel plan.
 *         See the README in this folder for the required
 *         project_config/lmic_project_config.h settings.
 *
 * Hardware (default pin map = TTGO LoRa32 / Heltec style wiring):
 *   SX1276/RFM95   ESP32
 *   NSS   -------- GPIO18
 *   RST   -------- GPIO14
 *   DIO0  -------- GPIO26
 *   DIO1  -------- GPIO33
 *   DIO2  -------- GPIO32   (not used by LoRaWAN, may be left unconnected)
 *   SCK   -------- GPIO5
 *   MISO  -------- GPIO19
 *   MOSI  -------- GPIO27
 *   DHT22 data --- GPIO4    (with a 10k pull-up to 3V3)
 *
 * Libraries: this LMIC library + "DHT sensor library" by Adafruit
 *            (and its dependency "Adafruit Unified Sensor").
 *
 * Keep the duty cycle / fair-use policy in mind: one uplink every 5 minutes
 * or slower is a good choice for a climate station.
 *******************************************************************************/

#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>
#include <DHT.h>

// -------------------------------------------------------------------------
// 1. Sensor configuration
// -------------------------------------------------------------------------
#define DHT_PIN   4
#define DHT_TYPE  DHT22        // use DHT11 if that is the sensor you have

DHT dht(DHT_PIN, DHT_TYPE);

// Optional battery monitoring through a resistor divider (2 x 100k).
// Set BATTERY_PIN to -1 to disable it; the battery field is then sent as 0.
#define BATTERY_PIN        35
#define BATTERY_DIVIDER    2.0f    // (R1 + R2) / R2

// -------------------------------------------------------------------------
// 2. LoRaWAN OTAA keys - copy these from your network server
//    (The Things Network / ChirpStack / a Thai private network)
// -------------------------------------------------------------------------

// AppEUI (JoinEUI), least-significant byte first
static const u1_t PROGMEM APPEUI[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
void os_getArtEui (u1_t* buf) { memcpy_P(buf, APPEUI, 8); }

// DevEUI, least-significant byte first
static const u1_t PROGMEM DEVEUI[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
void os_getDevEui (u1_t* buf) { memcpy_P(buf, DEVEUI, 8); }

// AppKey, most-significant byte first (copy as shown on the console)
static const u1_t PROGMEM APPKEY[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
void os_getDevKey (u1_t* buf) { memcpy_P(buf, APPKEY, 16); }

// -------------------------------------------------------------------------
// 3. Radio pin map
// -------------------------------------------------------------------------
const lmic_pinmap lmic_pins = {
    .nss  = 18,
    .rxtx = LMIC_UNUSED_PIN,
    .rst  = 14,
    .dio  = {26, 33, 32},
};

// -------------------------------------------------------------------------
// 4. Application settings
// -------------------------------------------------------------------------
const unsigned TX_INTERVAL = 300;   // seconds between uplinks (5 minutes)
const u1_t     LORA_PORT   = 1;
const dr_t     DATA_RATE   = DR_SF9; // SF9BW125, a good default for AS923

static osjob_t sendjob;
static uint8_t payload[6];

// -------------------------------------------------------------------------
// AS923 (Thailand) channel plan, applied on top of the EU868 stack.
// Channel 0 and 1 are the two join channels every AS923 gateway listens on.
// -------------------------------------------------------------------------
static void setupChannelsAS923() {
    // Remove the EU868 channels that LMIC installs by default.
    for (int ch = 0; ch < 9; ch++) {
        LMIC_disableChannel(ch);
    }

    // AS923-1 channels, DR0 (SF12) .. DR5 (SF7), all in band 0.
    LMIC_setupChannel(0, 923200000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI); // join
    LMIC_setupChannel(1, 923400000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI); // join
    LMIC_setupChannel(2, 923600000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
    LMIC_setupChannel(3, 923800000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
    LMIC_setupChannel(4, 924000000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
    LMIC_setupChannel(5, 924200000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
    LMIC_setupChannel(6, 924400000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
    LMIC_setupChannel(7, 924600000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);

    // RX2 for AS923 is 923.2 MHz / SF10BW125 (DR2).
    LMIC.dn2Freq = 923200000;
    LMIC.dn2Dr   = DR_SF10;
}

// -------------------------------------------------------------------------
// Read the sensors and build the payload
// -------------------------------------------------------------------------
static bool buildPayload() {
    float humidity    = dht.readHumidity();
    float temperature = dht.readTemperature();   // degrees Celsius

    if (isnan(humidity) || isnan(temperature)) {
        Serial.println(F("DHT read failed, skipping this uplink"));
        return false;
    }

    uint16_t battery_mv = 0;
    if (BATTERY_PIN >= 0) {
        // ESP32 ADC is 12 bit over a nominal 3.3 V range.
        float volts = (analogRead(BATTERY_PIN) / 4095.0f) * 3.3f * BATTERY_DIVIDER;
        battery_mv  = (uint16_t)(volts * 1000.0f);
    }

    int16_t  t100 = (int16_t)roundf(temperature * 100.0f);  // 0.01 degC
    uint16_t h100 = (uint16_t)roundf(humidity * 100.0f);    // 0.01 %RH

    payload[0] = (uint8_t)(t100 >> 8);
    payload[1] = (uint8_t)(t100 & 0xFF);
    payload[2] = (uint8_t)(h100 >> 8);
    payload[3] = (uint8_t)(h100 & 0xFF);
    payload[4] = (uint8_t)(battery_mv >> 8);
    payload[5] = (uint8_t)(battery_mv & 0xFF);

    Serial.print(F("T = "));       Serial.print(temperature, 2);
    Serial.print(F(" C, RH = "));  Serial.print(humidity, 2);
    Serial.print(F(" %, Vbat = ")); Serial.print(battery_mv);
    Serial.println(F(" mV"));
    return true;
}

static void do_send(osjob_t* j) {
    if (LMIC.opmode & OP_TXRXPEND) {
        Serial.println(F("Radio busy, uplink not queued"));
        return;
    }

    if (buildPayload()) {
        // Unconfirmed uplink: cheaper on air time and easier on the gateway.
        LMIC_setTxData2(LORA_PORT, payload, sizeof(payload), 0);
        Serial.println(F("Uplink queued"));
    } else {
        // Sensor error: try again at the next interval.
        os_setTimedCallback(&sendjob, os_getTime() + sec2osticks(TX_INTERVAL), do_send);
    }
}

// -------------------------------------------------------------------------
// LMIC event handling
// -------------------------------------------------------------------------
void onEvent(ev_t ev) {
    Serial.print(os_getTime());
    Serial.print(F(": "));
    switch (ev) {
        case EV_JOINING:
            Serial.println(F("EV_JOINING"));
            break;
        case EV_JOINED:
            Serial.println(F("EV_JOINED"));
            // Link ADR requests from the network can re-enable other channels,
            // so re-apply the AS923 plan after the join.
            setupChannelsAS923();
            // Disable link check mode; ADR stays on to save battery.
            LMIC_setLinkCheckMode(0);
            do_send(&sendjob);
            break;
        case EV_JOIN_FAILED:
            Serial.println(F("EV_JOIN_FAILED"));
            break;
        case EV_REJOIN_FAILED:
            Serial.println(F("EV_REJOIN_FAILED"));
            break;
        case EV_TXCOMPLETE:
            Serial.println(F("EV_TXCOMPLETE (sent, RX windows done)"));
            if (LMIC.txrxFlags & TXRX_ACK) {
                Serial.println(F("Received ack"));
            }
            if (LMIC.dataLen) {
                Serial.print(F("Downlink received, "));
                Serial.print(LMIC.dataLen);
                Serial.println(F(" byte(s)"));
            }
            os_setTimedCallback(&sendjob, os_getTime() + sec2osticks(TX_INTERVAL), do_send);
            break;
        case EV_TXSTART:
            Serial.println(F("EV_TXSTART"));
            break;
        case EV_LINK_DEAD:
            Serial.println(F("EV_LINK_DEAD"));
            break;
        case EV_LINK_ALIVE:
            Serial.println(F("EV_LINK_ALIVE"));
            break;
        default:
            Serial.print(F("Unknown event: "));
            Serial.println((unsigned)ev);
            break;
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println(F("ESP32 micro-climate station - LoRaWAN AS923 (Thailand)"));

    dht.begin();
    if (BATTERY_PIN >= 0) {
        analogReadResolution(12);
        analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
    }

    // SPI pins of the TTGO LoRa32 / Heltec boards: SCK, MISO, MOSI, SS.
    SPI.begin(5, 19, 27, 18);

    os_init();
    LMIC_reset();

    // Clock error margin: ESP32 boards often need this to catch RX windows.
    LMIC_setClockError(MAX_CLOCK_ERROR * 1 / 100);

    setupChannelsAS923();

    LMIC_setAdrMode(1);
    LMIC_setDrTxpow(DATA_RATE, 14);   // AS923 in Thailand: 14 dBm (25 mW ERP)

    // Start joining; the first uplink is sent from the EV_JOINED event.
    LMIC_startJoining();
}

void loop() {
    os_runloop_once();
}
