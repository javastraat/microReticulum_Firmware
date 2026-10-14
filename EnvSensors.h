// Copyright (C) 2026, Chad Attermann

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#ifdef HAS_ENV_SENSORS

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <microReticulum/Log.h>

// Minimal QMP6988 driver (M5Stack ENV III's pressure/temperature chip, fixed
// address 0x70). Algorithm ported from m5stack/M5Unit-ENV (MIT), via
// MeshCore's own port -- which already worked out the nRF52-specific gotcha
// below, so this keeps the same fix rather than rediscovering it:
// the chip pulls SDA low mid soft-reset (deadlocking this MCU's TWIM
// peripheral) and repeated-start reads hang it too, so every transfer here
// uses a full stop between the register-select write and the data read.
class QMP6988Sensor {
public:
    bool begin(TwoWire* wire, uint8_t addr = 0x70) {
        _wire = wire;
        _addr = addr;
        _wire->beginTransmission(_addr);
        if (_wire->endTransmission() != 0) return false;
        // Soft reset skipped -- see the class comment above.
        if (!loadCalibration()) return false;
        // NORMAL mode, IIR filter coeff 4, 8x P oversampling, 1x T oversampling
        writeReg(0xF1, 0x02);
        uint8_t ctrl = readByte(0xF4);
        ctrl = (ctrl & 0x1F) | (0x01 << 5);   // T osr = 1x
        ctrl = (ctrl & 0xE3) | (0x04 << 2);   // P osr = 8x
        ctrl = (ctrl & 0xFC) | 0x03;           // normal mode
        writeReg(0xF4, ctrl);
        delay(20);
        return true;
    }

    // Read chip, compute compensated values. Returns false on I2C error.
    bool update() {
        uint8_t buf[6];
        if (!readBytes(0xF7, buf, 6)) return false;
        uint32_t P_u = ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
        uint32_t T_u = ((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 8) | buf[5];
        int32_t dp = (int32_t)(P_u - 8388608UL);
        int32_t dt = (int32_t)(T_u - 8388608UL);
        int16_t tx = convTx02e(dt);
        int32_t px = getPressure02e(dp, tx);
        cTemp    = (float)tx / 256.0f;
        pressure = (float)px / 16.0f;
        return true;
    }

    float pressure = 0;  // Pa
    float cTemp    = 0;  // degC

private:
    TwoWire* _wire;
    uint8_t  _addr;

    struct {
        int32_t a0, b00, a1, a2;
        int64_t bt1, bt2, bp1, b11, bp2, b12, b21, bp3;
    } _ik;

    bool writeReg(uint8_t reg, uint8_t val) {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->write(val);
        return _wire->endTransmission() == 0;
    }

    uint8_t readByte(uint8_t reg) {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->endTransmission();  // full stop -- nRF52 TWIM hangs on repeated start
        _wire->requestFrom(_addr, (uint8_t)1);
        return _wire->available() ? _wire->read() : 0;
    }

    bool readBytes(uint8_t reg, uint8_t* buf, uint8_t len) {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        if (_wire->endTransmission() != 0) return false;  // full stop -- nRF52 TWIM hangs on repeated start
        _wire->requestFrom(_addr, len);
        for (uint8_t i = 0; i < len; i++) {
            if (!_wire->available()) return false;
            buf[i] = _wire->read();
        }
        return true;
    }

    bool loadCalibration() {
        uint8_t c[25];
        // Read calibration OTP one byte at a time (some I2C controllers
        // can't burst-read across this register range reliably).
        // Full stop+start instead of repeated start -- nRF52 TWIM hangs on Sr.
        for (int i = 0; i < 25; i++) {
            _wire->beginTransmission(_addr);
            _wire->write(0xA0 + i);
            if (_wire->endTransmission() != 0) return false;
            _wire->requestFrom(_addr, (uint8_t)1);
            if (!_wire->available()) return false;
            c[i] = _wire->read();
        }

        // Reconstruct raw signed coefficients (identical to M5Unit-ENV)
        int32_t a0  = (int32_t)((((uint32_t)c[18] << 12) | ((uint32_t)c[19] << 4) | (c[24] & 0x0F)) << 12) >> 12;
        int16_t a1  = (int16_t)(((uint16_t)c[20] << 8) | c[21]);
        int16_t a2  = (int16_t)(((uint16_t)c[22] << 8) | c[23]);
        int32_t b00 = (int32_t)((((uint32_t)c[0] << 12) | ((uint32_t)c[1] << 4) | ((c[24] & 0xF0) >> 4)) << 12) >> 12;
        int16_t bt1 = (int16_t)(((uint16_t)c[2]  << 8) | c[3]);
        int16_t bt2 = (int16_t)(((uint16_t)c[4]  << 8) | c[5]);
        int16_t bp1 = (int16_t)(((uint16_t)c[6]  << 8) | c[7]);
        int16_t b11 = (int16_t)(((uint16_t)c[8]  << 8) | c[9]);
        int16_t bp2 = (int16_t)(((uint16_t)c[10] << 8) | c[11]);
        int16_t b12 = (int16_t)(((uint16_t)c[12] << 8) | c[13]);
        int16_t b21 = (int16_t)(((uint16_t)c[14] << 8) | c[15]);
        int16_t bp3 = (int16_t)(((uint16_t)c[16] << 8) | c[17]);

        // Convert to integer kernel (identical magic constants as M5Unit-ENV)
        _ik.a0  = a0;
        _ik.b00 = b00;
        _ik.a1  = 3608L  * (int32_t)a1  - 1731677965L;
        _ik.a2  = 16889L * (int32_t)a2  - 87619360L;
        _ik.bt1 = 2982LL   * (int64_t)bt1 + 107370906LL;
        _ik.bt2 = 329854LL * (int64_t)bt2 + 108083093LL;
        _ik.bp1 = 19923LL  * (int64_t)bp1 + 1133836764LL;
        _ik.b11 = 2406LL   * (int64_t)b11 + 118215883LL;
        _ik.bp2 = 3079LL   * (int64_t)bp2 - 181579595LL;
        _ik.b12 = 6846LL   * (int64_t)b12 + 85590281LL;
        _ik.b21 = 13836LL  * (int64_t)b21 + 79333336LL;
        _ik.bp3 = 2915LL   * (int64_t)bp3 + 157155561LL;
        return true;
    }

    int16_t convTx02e(int32_t dt) {
        int64_t wk1 = (int64_t)_ik.a1 * (int64_t)dt;
        int64_t wk2 = ((int64_t)_ik.a2 * (int64_t)dt) >> 14;
        wk2 = (wk2 * (int64_t)dt) >> 10;
        wk2 = ((wk1 + wk2) / 32767L) >> 19;
        return (int16_t)((_ik.a0 + wk2) >> 4);
    }

    int32_t getPressure02e(int32_t dp, int16_t tx) {
        int64_t wk1, wk2, wk3;
        wk1  = (int64_t)_ik.bt1 * (int64_t)tx;
        wk2  = ((int64_t)_ik.bp1 * (int64_t)dp) >> 5;
        wk1 += wk2;
        wk2  = ((int64_t)_ik.bt2 * (int64_t)tx) >> 1;
        wk2  = (wk2 * (int64_t)tx) >> 8;
        wk3  = wk2;
        wk2  = ((int64_t)_ik.b11 * (int64_t)tx) >> 4;
        wk2  = (wk2 * (int64_t)dp) >> 1;
        wk3 += wk2;
        wk2  = ((int64_t)_ik.bp2 * (int64_t)dp) >> 13;
        wk2  = (wk2 * (int64_t)dp) >> 1;
        wk3 += wk2;
        wk1 += wk3 >> 14;
        wk2  = (int64_t)_ik.b12 * (int64_t)tx;
        wk2  = (wk2 * (int64_t)tx) >> 22;
        wk2  = (wk2 * (int64_t)dp) >> 1;
        wk3  = wk2;
        wk2  = ((int64_t)_ik.b21 * (int64_t)tx) >> 6;
        wk2  = (wk2 * (int64_t)dp) >> 23;
        wk2  = (wk2 * (int64_t)dp) >> 1;
        wk3 += wk2;
        wk2  = ((int64_t)_ik.bp3 * (int64_t)dp) >> 12;
        wk2  = (wk2 * (int64_t)dp) >> 23;
        wk2  = (wk2 * (int64_t)dp);
        wk3 += wk2;
        wk1 += wk3 >> 15;
        wk1 /= 32767L;
        wk1 >>= 11;
        wk1 += _ik.b00;
        return (int32_t)wk1;
    }
};

// Minimal BMP280 driver (M5Stack ENV I/II's pressure/temperature chip,
// address 0x76 or 0x77). Compensation formulas are Bosch's own published
// datasheet reference algorithm (the same one every open BMP280 driver,
// Adafruit's included, implements) -- reimplemented here directly instead
// of pulling in Adafruit_BMP280 + its BusIO/Unified-Sensor dependency chain,
// which was ~1KB too much for this board's flash partition.
class BMP280Sensor {
public:
    bool begin(TwoWire* wire, uint8_t addr) {
        _wire = wire;
        _addr = addr;
        if (readByte(0xD0) != 0x58) return false;  // CHIPID
        readCalibration();
        // Forced mode per read() call -- see update(). osrs_t=x1, osrs_p=x8 for now.
        writeReg(0xF4, 0x20);
        return true;
    }

    bool update() {
        // Trigger one forced-mode conversion: osrs_t=001, osrs_p=100, mode=01
        writeReg(0xF4, 0x25);
        delay(20);
        uint8_t buf[6];
        if (!readBytes(0xF7, buf, 6)) return false;
        int32_t adc_p = ((int32_t)buf[0] << 12) | ((int32_t)buf[1] << 4) | (buf[2] >> 4);
        int32_t adc_t = ((int32_t)buf[3] << 12) | ((int32_t)buf[4] << 4) | (buf[5] >> 4);

        int32_t var1 = ((((adc_t >> 3) - ((int32_t)_dig_T1 << 1))) * (int32_t)_dig_T2) >> 11;
        int32_t var2 = (((((adc_t >> 4) - (int32_t)_dig_T1) * ((adc_t >> 4) - (int32_t)_dig_T1)) >> 12) * (int32_t)_dig_T3) >> 14;
        int32_t t_fine = var1 + var2;
        cTemp = (float)((t_fine * 5 + 128) >> 8) / 100.0f;

        int64_t v1 = (int64_t)t_fine - 128000;
        int64_t v2 = v1 * v1 * (int64_t)_dig_P6;
        v2 = v2 + ((v1 * (int64_t)_dig_P5) << 17);
        v2 = v2 + (((int64_t)_dig_P4) << 35);
        v1 = ((v1 * v1 * (int64_t)_dig_P3) >> 8) + ((v1 * (int64_t)_dig_P2) << 12);
        v1 = (((((int64_t)1) << 47) + v1)) * ((int64_t)_dig_P1) >> 33;
        if (v1 == 0) return false;
        int64_t p = 1048576 - adc_p;
        p = (((p << 31) - v2) * 3125) / v1;
        v1 = (((int64_t)_dig_P9) * (p >> 13) * (p >> 13)) >> 25;
        v2 = (((int64_t)_dig_P8) * p) >> 19;
        p = ((p + v1 + v2) >> 8) + (((int64_t)_dig_P7) << 4);
        pressure = (float)p / 256.0f;  // Pa
        return true;
    }

    float pressure = 0;  // Pa
    float cTemp    = 0;  // degC

private:
    TwoWire* _wire;
    uint8_t  _addr;
    uint16_t _dig_T1; int16_t _dig_T2, _dig_T3;
    uint16_t _dig_P1; int16_t _dig_P2, _dig_P3, _dig_P4, _dig_P5, _dig_P6, _dig_P7, _dig_P8, _dig_P9;

    void writeReg(uint8_t reg, uint8_t val) {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->write(val);
        _wire->endTransmission();
    }

    uint8_t readByte(uint8_t reg) {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->endTransmission();  // full stop -- consistent with this file's other nRF52-safe drivers
        _wire->requestFrom(_addr, (uint8_t)1);
        return _wire->available() ? _wire->read() : 0;
    }

    bool readBytes(uint8_t reg, uint8_t* buf, uint8_t len) {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        if (_wire->endTransmission() != 0) return false;
        _wire->requestFrom(_addr, len);
        for (uint8_t i = 0; i < len; i++) {
            if (!_wire->available()) return false;
            buf[i] = _wire->read();
        }
        return true;
    }

    void readCalibration() {
        uint8_t c[24];
        readBytes(0x88, c, 24);
        _dig_T1 = (uint16_t)(c[0]  | (c[1]  << 8));
        _dig_T2 = (int16_t) (c[2]  | (c[3]  << 8));
        _dig_T3 = (int16_t) (c[4]  | (c[5]  << 8));
        _dig_P1 = (uint16_t)(c[6]  | (c[7]  << 8));
        _dig_P2 = (int16_t) (c[8]  | (c[9]  << 8));
        _dig_P3 = (int16_t) (c[10] | (c[11] << 8));
        _dig_P4 = (int16_t) (c[12] | (c[13] << 8));
        _dig_P5 = (int16_t) (c[14] | (c[15] << 8));
        _dig_P6 = (int16_t) (c[16] | (c[17] << 8));
        _dig_P7 = (int16_t) (c[18] | (c[19] << 8));
        _dig_P8 = (int16_t) (c[20] | (c[21] << 8));
        _dig_P9 = (int16_t) (c[22] | (c[23] << 8));
    }
};

// Minimal SHT30/31 driver (M5Stack ENV II/III's humidity/temperature chip,
// address 0x44). Uses the clock-stretching-DISABLED single-shot command
// (0x2400) and a fixed delay instead of relying on clock stretching to pace
// the read -- this file's QMP6988 driver already documents this MCU's TWIM
// peripheral being unreliable around stretched/repeated-start I2C
// transactions, so this avoids the same class of problem rather than
// risking it on a second chip.
class SHT3XSensor {
public:
    bool begin(TwoWire* wire, uint8_t addr = 0x44) {
        _wire = wire;
        _addr = addr;
        _wire->beginTransmission(_addr);
        return _wire->endTransmission() == 0;
    }

    bool update() {
        _wire->beginTransmission(_addr);
        _wire->write(0x24);
        _wire->write(0x00);  // high repeatability, clock stretching disabled
        if (_wire->endTransmission() != 0) return false;
        delay(16);  // worst-case high-repeatability conversion time
        _wire->requestFrom(_addr, (uint8_t)6);
        uint8_t buf[6];
        for (uint8_t i = 0; i < 6; i++) {
            if (!_wire->available()) return false;
            buf[i] = _wire->read();
        }
        if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) return false;
        uint16_t t_raw = ((uint16_t)buf[0] << 8) | buf[1];
        uint16_t h_raw = ((uint16_t)buf[3] << 8) | buf[4];
        temperature = -45.0f + 175.0f * ((float)t_raw / 65535.0f);
        humidity = 100.0f * ((float)h_raw / 65535.0f);
        return true;
    }

    float temperature = 0;
    float humidity = 0;

private:
    TwoWire* _wire;
    uint8_t  _addr;

    static uint8_t crc8(const uint8_t* data, uint8_t len) {
        uint8_t crc = 0xFF;
        for (uint8_t i = 0; i < len; i++) {
            crc ^= data[i];
            for (uint8_t b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
        return crc;
    }
};

// Minimal DHT12 driver (M5Stack ENV I's humidity/temperature chip, fixed
// address 0x5C). Register map is a flat 5-byte burst: humidity int, humidity
// dec, temp int (bit7 = sign), temp dec, checksum -- simple enough that it
// doesn't need a real library the way SHT3X/BMP280 do below.
class DHT12Sensor {
public:
    static const uint8_t ADDR = 0x5C;

    bool begin(TwoWire* wire) {
        _wire = wire;
        _wire->beginTransmission(ADDR);
        return _wire->endTransmission() == 0;
    }

    bool update() {
        uint8_t buf[5];
        _wire->beginTransmission(ADDR);
        _wire->write((uint8_t)0x00);
        if (_wire->endTransmission() != 0) return false;
        _wire->requestFrom((uint8_t)ADDR, (uint8_t)5);
        for (uint8_t i = 0; i < 5; i++) {
            if (!_wire->available()) return false;
            buf[i] = _wire->read();
        }
        uint8_t sum = (uint8_t)(buf[0] + buf[1] + buf[2] + buf[3]);
        if (sum != buf[4]) return false;
        humidity = buf[0] + buf[1] / 10.0f;
        float t = (buf[2] & 0x7F) + buf[3] / 10.0f;
        temperature = (buf[2] & 0x80) ? -t : t;
        return true;
    }

    float temperature = 0;
    float humidity = 0;

private:
    TwoWire* _wire;
};

// Auto-probes the Grove I2C header for whichever M5Stack ENV unit is
// plugged in -- ENV I (DHT12 + BMP280), ENV II (SHT30 + BMP280) or ENV III
// (SHT30 + QMP6988) -- and exposes a single combined reading regardless of
// which generation is attached.
namespace EnvSensors {

    inline bool humidity_found = false;   // SHT3X or DHT12
    inline bool pressure_found = false;   // QMP6988 or BMP280
    inline bool using_sht3x    = false;   // false => DHT12
    inline bool using_qmp6988  = false;   // false => BMP280

    inline SHT3XSensor     sht3x;
    inline BMP280Sensor    bmp280;
    inline QMP6988Sensor   qmp6988;
    inline DHT12Sensor     dht12;

    inline float temperature  = NAN;
    inline float humidity     = NAN;
    inline float pressure_hpa = NAN;
    inline float altitude_m   = NAN;

    inline void init() {
        RNS::trace("Looking for a Grove environment sensor...");
        Wire.begin();

        if (sht3x.begin(&Wire, 0x44)) {
            humidity_found = true;
            using_sht3x = true;
            RNS::trace("Found SHT30/31 humidity/temperature sensor");
        } else if (dht12.begin(&Wire)) {
            humidity_found = true;
            using_sht3x = false;
            RNS::trace("Found DHT12 humidity/temperature sensor");
        } else {
            RNS::trace("No humidity sensor found (SHT3X/DHT12)");
        }

        if (qmp6988.begin(&Wire, 0x70)) {
            pressure_found = true;
            using_qmp6988 = true;
            RNS::trace("Found QMP6988 pressure/temperature sensor");
        } else if (bmp280.begin(&Wire, 0x76) || bmp280.begin(&Wire, 0x77)) {
            pressure_found = true;
            using_qmp6988 = false;
            RNS::trace("Found BMP280 pressure/temperature sensor");
        } else {
            RNS::trace("No pressure sensor found (QMP6988/BMP280)");
        }
    }

    inline bool installed() { return humidity_found || pressure_found; }

    // Reads whichever sensor(s) were detected at init(). Returns false if
    // nothing is installed or every read failed. Temperature prefers the
    // humidity-sensor reading (SHT3X/DHT12) when both are present.
    inline bool read() {
        bool ok = false;

        if (humidity_found) {
            if (using_sht3x) {
                if (sht3x.update()) { temperature = sht3x.temperature; humidity = sht3x.humidity; ok = true; }
            } else if (dht12.update()) {
                temperature = dht12.temperature;
                humidity = dht12.humidity;
                ok = true;
            }
        }

        if (pressure_found) {
            bool pressure_ok = false;
            if (using_qmp6988) {
                if (qmp6988.update()) {
                    pressure_hpa = qmp6988.pressure / 100.0f;
                    if (!humidity_found) temperature = qmp6988.cTemp;
                    pressure_ok = true;
                }
            } else if (bmp280.update()) {
                pressure_hpa = bmp280.pressure / 100.0f;
                if (!humidity_found) temperature = bmp280.cTemp;
                pressure_ok = true;
            }
            if (pressure_ok) {
                altitude_m = 44330.0f * (1.0f - powf(pressure_hpa / 1013.25f, 0.1903f));
                ok = true;
            }
        }

        return ok;
    }

    inline const char* name() {
        if (humidity_found && pressure_found) return using_sht3x ? (using_qmp6988 ? "SHT30 + QMP6988" : "SHT30 + BMP280") : "DHT12 + BMP280";
        if (humidity_found) return using_sht3x ? "SHT30" : "DHT12";
        if (pressure_found) return using_qmp6988 ? "QMP6988" : "BMP280";
        return "none";
    }

}

#endif // HAS_ENV_SENSORS
