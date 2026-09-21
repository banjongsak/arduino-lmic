/*******************************************************************************
 * ESP32 outdoor garden climate station -> LoRaWAN AS923 (Thailand)
 *
 * Built for weather-proof hardware:
 *   - SHT31-D temperature / humidity probe (I2C, PTFE filter cap)
 *   - SX1276 / RFM95W radio, 923 MHz, outdoor antenna
 *   - LiFePO4 battery + solar panel
 *   - deep sleep between uplinks, so one charge lasts many cloudy days
 *
 * How the sleep works:
 *   LMIC loses everything on reset, so after the first OTAA join the session
 *   (DevAddr, network key, app key, frame counter) is copied into RTC memory.
 *   That memory survives deep sleep, so after waking the sketch restores the
 *   session with LMIC_setSession() and sends at once - no re-join, no extra
 *   air time, much less battery used.
 *
 * Wiring (TTGO LoRa32 / Heltec style, change if your board differs):
 *   SX1276      ESP32            SHT31-D      ESP32
 *   NSS  ------ GPIO18           SDA   ------ GPIO21
 *   RST  ------ GPIO14           SCL   ------ GPIO22
 *   DIO0 ------ GPIO26           VIN   ------ GPIO25 (sensor power, optional)
 *   DIO1 ------ GPIO33           GND   ------ GND
 *   SCK  ------ GPIO5
 *   MISO ------ GPIO19
 *   MOSI ------ GPIO27
 *   Battery divider (2 x 100k) -- GPIO35
 *
 * Libraries: this LMIC library, "Adafruit SHT31 Library",
 *            "Adafruit BusIO", "Adafruit Unified Sensor".
 *
 * Band: AS923-1 (Thailand). Build the library as CFG_eu868 - see README.
 *******************************************************************************/

#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>

// -------------------------------------------------------------------------
// 1. Settings you may want to change
// -------------------------------------------------------------------------
#define SLEEP_MINUTES      10       // uplink interval; 10 min suits solar power
#define LORA_PORT          1
#define TX_DATARATE        DR_SF9   // SF9BW125
#define TX_POWER_DBM       14       // NBTC limit for this band (25 mW ERP)

#define SHT31_ADDRESS      0x44     // 0x45 if the ADDR pad is tied high
#define SENSOR_POWER_PIN   25       // drives the sensor VIN; -1 = always on
#define BATTERY_PIN        35       // -1 to disable battery monitoring
#define BATTERY_DIVIDER    2.0f     // (R1 + R2) / R2

// Give up and sleep if the radio has not finished within this many seconds.
#define TX_TIMEOUT_SEC     120

// -------------------------------------------------------------------------
// 2. OTAA keys from your network server (TTN / ChirpStack / Thai operator)
// -------------------------------------------------------------------------
static const u1_t PROGMEM APPEUI[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
void os_getArtEui (u1_t* buf) { memcpy_P(buf, APPEUI, 8); }

static const u1_t PROGMEM DEVEUI[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
void os_getDevEui (u1_t* buf) { memcpy_P(buf, DEVEUI, 8); }

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
// 4. State kept across deep sleep (RTC memory survives, RAM does not)
// -------------------------------------------------------------------------
RTC_DATA_ATTR static bool     rtc_joined      = false;
RTC_DATA_ATTR static u4_t     rtc_netid       = 0;
RTC_DATA_ATTR static devaddr_t rtc_devaddr    = 0;
RTC_DATA_ATTR static u1_t     rtc_nwkKey[16];
RTC_DATA_ATTR static u1_t     rtc_artKey[16];
RTC_DATA_ATTR static u4_t     rtc_seqnoUp     = 0;
RTC_DATA_ATTR static u4_t     rtc_bootCount   = 0;

Adafruit_SHT31 sht31 = Adafruit_SHT31();

static osjob_t  sendjob;
static uint8_t  payload[7];
static bool     txDone       = false;
static uint32_t txStartMs    = 0;

// -------------------------------------------------------------------------
// AS923 (Thailand) channel plan, laid over the EU868 dynamic-channel stack
// -------------------------------------------------------------------------
static void setupChannelsAS923() {
    for (int ch = 0; ch < 9; ch++) {
        LMIC_disableChannel(ch);
    }
    LMIC_setupChannel(0, 923200000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI); // join
    LMIC_setupChannel(1, 923400000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI); // join
    LMIC_setupChannel(2, 923600000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
    LMIC_setupChannel(3, 923800000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
    LMIC_setupChannel(4, 924000000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
    LMIC_setupChannel(5, 924200000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
    LMIC_setupChannel(6, 924400000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);
    LMIC_setupChannel(7, 924600000, DR_RANGE_MAP(DR_SF12, DR_SF7), BAND_CENTI);

    LMIC.dn2Freq = 923200000;   // RX2: 923.2 MHz
    LMIC.dn2Dr   = DR_SF10;     // RX2: SF10BW125
}

// -------------------------------------------------------------------------
// Sensor reading. Returns false if the probe did not answer.
// -------------------------------------------------------------------------
static bool readSensors(float &temperature, float &humidity, uint16_t &battery_mv) {
    if (SENSOR_POWER_PIN >= 0) {
        pinMode(SENSOR_POWER_PIN, OUTPUT);
        digitalWrite(SENSOR_POWER_PIN, HIGH);
        delay(20);                 // SHT31 needs a few ms after power-up
    }

    Wire.begin(21, 22);
    if (!sht31.begin(SHT31_ADDRESS)) {
        Serial.println(F("SHT31 not found - check wiring and address"));
        return false;
    }

    // Two readings: the first one after power-up is sometimes stale.
    sht31.readTemperature();
    delay(20);
    temperature = sht31.readTemperature();
    humidity    = sht31.readHumidity();

    if (SENSOR_POWER_PIN >= 0) {
        digitalWrite(SENSOR_POWER_PIN, LOW);   // sensor off again, saves power
    }

    if (isnan(temperature) || isnan(humidity)) {
        Serial.println(F("SHT31 read failed"));
        return false;
    }

    battery_mv = 0;
    if (BATTERY_PIN >= 0) {
        analogReadResolution(12);
        analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
        uint32_t raw = 0;
        for (int i = 0; i < 8; i++) {          // average, the ESP32 ADC is noisy
            raw += analogRead(BATTERY_PIN);
        }
        float volts = ((raw / 8.0f) / 4095.0f) * 3.3f * BATTERY_DIVIDER;
        battery_mv  = (uint16_t)(volts * 1000.0f);
    }
    return true;
}

static void buildPayload(float temperature, float humidity, uint16_t battery_mv) {
    int16_t  t100 = (int16_t)roundf(temperature * 100.0f);   // 0.01 degC
    uint16_t h100 = (uint16_t)roundf(humidity * 100.0f);     // 0.01 %RH

    payload[0] = (uint8_t)(t100 >> 8);
    payload[1] = (uint8_t)(t100 & 0xFF);
    payload[2] = (uint8_t)(h100 >> 8);
    payload[3] = (uint8_t)(h100 & 0xFF);
    payload[4] = (uint8_t)(battery_mv >> 8);
    payload[5] = (uint8_t)(battery_mv & 0xFF);
    payload[6] = (uint8_t)(rtc_bootCount & 0xFF);            // simple health counter

    Serial.printf("T=%.2f C  RH=%.2f %%  Vbat=%u mV  boot=%u\n",
                  temperature, humidity, battery_mv, rtc_bootCount);
}

// -------------------------------------------------------------------------
// Deep sleep
// -------------------------------------------------------------------------
static void goToSleep() {
    // Remember the frame counter so the network does not drop our next uplink.
    rtc_seqnoUp = LMIC.seqnoUp;

    Serial.printf("Sleeping for %d minutes\n", SLEEP_MINUTES);
    Serial.flush();

    // Put the radio in its lowest power state.
    LMIC_shutdown();
    digitalWrite(lmic_pins.rst, LOW);
    SPI.end();

    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_MINUTES * 60ULL * 1000000ULL);
    esp_deep_sleep_start();
}

static void do_send(osjob_t* j) {
    float    temperature, humidity;
    uint16_t battery_mv;

    if (!readSensors(temperature, humidity, battery_mv)) {
        // Bad reading: do not waste air time, just sleep and try next cycle.
        goToSleep();
        return;
    }

    buildPayload(temperature, humidity, battery_mv);
    LMIC_setTxData2(LORA_PORT, payload, sizeof(payload), 0);  // unconfirmed
    txStartMs = millis();
    Serial.println(F("Uplink queued"));
}

// -------------------------------------------------------------------------
// LMIC events
// -------------------------------------------------------------------------
void onEvent(ev_t ev) {
    switch (ev) {
        case EV_JOINING:
            Serial.println(F("Joining..."));
            break;

        case EV_JOINED: {
            Serial.println(F("Joined"));

            // Save the session so later wake-ups skip the join.
            u4_t netid = 0;
            devaddr_t devaddr = 0;
            u1_t nwkKey[16], artKey[16];
            LMIC_getSessionKeys(&netid, &devaddr, nwkKey, artKey);

            rtc_netid   = netid;
            rtc_devaddr = devaddr;
            memcpy(rtc_nwkKey, nwkKey, 16);
            memcpy(rtc_artKey, artKey, 16);
            rtc_joined  = true;

            setupChannelsAS923();       // ADR from the join may have changed them
            LMIC_setLinkCheckMode(0);
            LMIC_setDrTxpow(TX_DATARATE, TX_POWER_DBM);
            do_send(&sendjob);
            break;
        }

        case EV_JOIN_FAILED:
        case EV_REJOIN_FAILED:
            Serial.println(F("Join failed - sleeping, will retry next cycle"));
            goToSleep();
            break;

        case EV_TXCOMPLETE:
            Serial.println(F("TX complete"));
            if (LMIC.dataLen) {
                Serial.printf("Downlink: %u byte(s) on port %u\n",
                              LMIC.dataLen, LMIC.frame[LMIC.dataBeg - 1]);
            }
            txDone = true;
            break;

        default:
            break;
    }
}

// -------------------------------------------------------------------------
// setup / loop
// -------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    rtc_bootCount++;
    Serial.printf("\nGarden climate station - boot %u\n", rtc_bootCount);

    SPI.begin(5, 19, 27, 18);       // SCK, MISO, MOSI, SS

    os_init();
    LMIC_reset();

    // ESP32 clocks drift in the heat; this margin keeps the RX windows open.
    LMIC_setClockError(MAX_CLOCK_ERROR * 1 / 100);

    setupChannelsAS923();
    LMIC_setAdrMode(0);             // fixed DR: ADR is not useful when we sleep
    LMIC_setLinkCheckMode(0);
    LMIC_setDrTxpow(TX_DATARATE, TX_POWER_DBM);

    if (rtc_joined) {
        // Wake-up after sleep: restore the saved session and send right away.
        Serial.println(F("Restoring saved session"));
        LMIC_setSession(rtc_netid, rtc_devaddr, rtc_nwkKey, rtc_artKey);
        setupChannelsAS923();       // setSession resets the channel list
        LMIC_setDrTxpow(TX_DATARATE, TX_POWER_DBM);
        LMIC.seqnoUp = rtc_seqnoUp;
        do_send(&sendjob);
    } else {
        // First boot after power-up or after erasing RTC memory.
        LMIC_startJoining();
    }
}

void loop() {
    os_runloop_once();

    if (txDone) {
        goToSleep();
    }

    // Safety net: never stay awake forever if the radio gets stuck.
    if (txStartMs && (millis() - txStartMs > TX_TIMEOUT_SEC * 1000UL)) {
        Serial.println(F("TX timeout - sleeping anyway"));
        goToSleep();
    }
}
