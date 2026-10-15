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
#include <cmath>
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
        if (readByte(0xD1) != 0x5C) return false;   // CHIP_ID; an SHTC3 shares this address
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
// Also drives the BME280 (chip ID 0x60), the same part plus a humidity
// sensor: humidity calibration, ctrl_hum and the extra two data bytes.
class BMP280Sensor {
public:
    bool begin(TwoWire* wire, uint8_t addr) {
        _wire = wire;
        _addr = addr;
        uint8_t id = readByte(0xD0);               // CHIPID
        if (id != 0x58 && id != 0x60) return false;
        is_bme280 = id == 0x60;
        readCalibration();
        // Forced mode per read() call -- see update(). osrs_t=x1, osrs_p=x8 for now.
        if (is_bme280) writeReg(0xF2, 0x01);       // humidity x1; takes effect with the next ctrl_meas write
        writeReg(0xF4, 0x20);
        return true;
    }

    bool update() {
        // Trigger one forced-mode conversion: osrs_t=001, osrs_p=100, mode=01
        if (is_bme280) writeReg(0xF2, 0x01);
        writeReg(0xF4, 0x25);
        delay(20);
        uint8_t buf[8];
        if (!readBytes(0xF7, buf, is_bme280 ? 8 : 6)) return false;
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

        if (is_bme280) {                           // Bosch's integer humidity compensation
            int32_t adc_h = ((int32_t)buf[6] << 8) | buf[7];
            int32_t v = t_fine - 76800;
            v = (((((adc_h << 14) - (((int32_t)_dig_H4) << 20) - (((int32_t)_dig_H5) * v)) + 16384) >> 15) *
                 (((((((v * (int32_t)_dig_H6) >> 10) * (((v * (int32_t)_dig_H3) >> 11) + 32768)) >> 10) + 2097152) * (int32_t)_dig_H2 + 8192) >> 14));
            v = v - (((((v >> 15) * (v >> 15)) >> 7) * (int32_t)_dig_H1) >> 4);
            v = v < 0 ? 0 : v > 419430400 ? 419430400 : v;
            humidity = (float)(v >> 12) / 1024.0f;
        }
        return true;
    }

    float pressure = 0;  // Pa
    float cTemp    = 0;  // degC
    float humidity = 0;  // %RH, BME280 only
    bool  is_bme280 = false;

private:
    TwoWire* _wire;
    uint8_t  _addr;
    uint16_t _dig_T1; int16_t _dig_T2, _dig_T3;
    uint16_t _dig_P1; int16_t _dig_P2, _dig_P3, _dig_P4, _dig_P5, _dig_P6, _dig_P7, _dig_P8, _dig_P9;
    uint8_t  _dig_H1 = 0, _dig_H3 = 0; int16_t _dig_H2 = 0, _dig_H4 = 0, _dig_H5 = 0; int8_t _dig_H6 = 0;

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
        if (is_bme280) {
            uint8_t h[7];
            _dig_H1 = readByte(0xA1);
            readBytes(0xE1, h, 7);
            _dig_H2 = (int16_t)(h[0] | (h[1] << 8));
            _dig_H3 = h[2];
            _dig_H4 = (int16_t)(((int16_t)(int8_t)h[3] << 4) | (h[4] & 0x0F));
            _dig_H5 = (int16_t)(((int16_t)(int8_t)h[5] << 4) | (h[4] >> 4));
            _dig_H6 = (int8_t)h[6];
        }
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
        // Read the status register: only an SHT3x answers it with a valid CRC
        // (an SHT4x or INA226 can sit at the same address).
        _wire->beginTransmission(_addr);
        _wire->write(0xF3);
        _wire->write(0x2D);
        if (_wire->endTransmission() != 0) return false;
        delay(1);
        uint8_t b[3];
        if (_wire->requestFrom(_addr, (uint8_t)3) != 3) return false;
        for (uint8_t i = 0; i < 3; i++) b[i] = _wire->read();
        return crc8(b, 2) == b[2];
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

    static uint8_t crc8(const uint8_t* data, uint8_t len) {   // Sensirion: poly 0x31, init 0xFF
        uint8_t crc = 0xFF;
        for (uint8_t i = 0; i < len; i++) {
            crc ^= data[i];
            for (uint8_t b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
        return crc;
    }

private:
    TwoWire* _wire;
    uint8_t  _addr;
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
        if (_wire->endTransmission() != 0) return false;
        // A valid reading (checksum and ranges) tells it apart from an
        // LPS22HB, which can sit at the same address.
        if (update()) return true;
        delay(50);
        return update();
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
        if (buf[0] > 100 || buf[1] > 9 || buf[3] > 9) return false;
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

// ---------------------------------------------------------------------------
// The drivers below cover the other common Grove/Qwiic weather sensors. Each
// begin() checks the chip's identity, because several share an address with
// a chip above: SHT4x and SHT30 (0x44), SHTC3 and QMP6988 (0x70), LPS22HB and
// DHT12 (0x5C), and the Bosch parts at 0x76/0x77 (told apart by chip ID).
// Same full-stop register reads as above. Formulas are the datasheet ones.
// ---------------------------------------------------------------------------
namespace EnvI2C {
    inline bool cmd16(TwoWire* w, uint8_t a, uint16_t c) {
        w->beginTransmission(a); w->write((uint8_t)(c >> 8)); w->write((uint8_t)c);
        return w->endTransmission() == 0;
    }
    inline bool readN(TwoWire* w, uint8_t a, uint8_t* b, uint8_t n) {
        if (w->requestFrom(a, n) != n) return false;
        for (uint8_t i = 0; i < n; i++) b[i] = w->read();
        return true;
    }
    inline bool readRegs(TwoWire* w, uint8_t a, uint8_t reg, uint8_t* b, uint8_t n) {
        w->beginTransmission(a); w->write(reg);
        if (w->endTransmission() != 0) return false;   // full stop -- nRF52 TWIM hangs on repeated start
        return readN(w, a, b, n);
    }
    inline bool writeReg(TwoWire* w, uint8_t a, uint8_t reg, uint8_t v) {
        w->beginTransmission(a); w->write(reg); w->write(v);
        return w->endTransmission() == 0;
    }
    inline bool ack(TwoWire* w, uint8_t a) { w->beginTransmission(a); return w->endTransmission() == 0; }
}

// SHT40/41/45 (0x44, 0x45 or 0x46): serial number with CRC as identity.
class SHT4XSensor {
public:
    bool begin(TwoWire* wire, uint8_t addr) {
        _w = wire; _a = addr;
        uint8_t b[6];
        if (!EnvI2C::ack(_w, _a)) return false;
        _w->beginTransmission(_a); _w->write(0x89);   // read serial number
        if (_w->endTransmission() != 0) return false;
        delay(2);
        if (!EnvI2C::readN(_w, _a, b, 6)) return false;
        return SHT3XSensor::crc8(b, 2) == b[2] && SHT3XSensor::crc8(b + 3, 2) == b[5];
    }
    bool update() {
        uint8_t b[6];
        _w->beginTransmission(_a); _w->write(0xFD);   // high precision
        if (_w->endTransmission() != 0) return false;
        delay(10);
        if (!EnvI2C::readN(_w, _a, b, 6)) return false;
        if (SHT3XSensor::crc8(b, 2) != b[2] || SHT3XSensor::crc8(b + 3, 2) != b[5]) return false;
        temperature = -45.0f + 175.0f * (float)(((uint16_t)b[0] << 8) | b[1]) / 65535.0f;
        humidity    = -6.0f + 125.0f * (float)(((uint16_t)b[3] << 8) | b[4]) / 65535.0f;
        humidity    = humidity < 0 ? 0 : humidity > 100 ? 100 : humidity;
        return true;
    }
    float temperature = 0, humidity = 0;
private:
    TwoWire* _w; uint8_t _a;
};

// SHTC3 (0x70): ID register 0xEFC8, low bits xxxx 1xxx xx00 0111.
class SHTC3Sensor {
public:
    bool begin(TwoWire* wire, uint8_t addr = 0x70) {
        _w = wire; _a = addr;
        uint8_t b[3];
        if (!EnvI2C::cmd16(_w, _a, 0x3517)) return false;   // wake up
        delay(1);
        if (!EnvI2C::cmd16(_w, _a, 0xEFC8)) return false;   // read ID
        if (!EnvI2C::readN(_w, _a, b, 3) || SHT3XSensor::crc8(b, 2) != b[2]) return false;
        uint16_t id = ((uint16_t)b[0] << 8) | b[1];
        EnvI2C::cmd16(_w, _a, 0xB098);                       // sleep
        return (id & 0x083F) == 0x0807;
    }
    bool update() {
        uint8_t b[6];
        if (!EnvI2C::cmd16(_w, _a, 0x3517)) return false;
        delay(1);
        if (!EnvI2C::cmd16(_w, _a, 0x7866)) return false;   // normal mode, T first, no clock stretching
        delay(13);
        bool ok = EnvI2C::readN(_w, _a, b, 6) && SHT3XSensor::crc8(b, 2) == b[2] && SHT3XSensor::crc8(b + 3, 2) == b[5];
        EnvI2C::cmd16(_w, _a, 0xB098);
        if (!ok) return false;
        temperature = -45.0f + 175.0f * (float)(((uint16_t)b[0] << 8) | b[1]) / 65536.0f;
        humidity    = 100.0f * (float)(((uint16_t)b[3] << 8) | b[4]) / 65536.0f;
        return true;
    }
    float temperature = 0, humidity = 0;
private:
    TwoWire* _w; uint8_t _a;
};

// AHT10 / AHT20 (0x38): calibrated bit in the status byte, after the init
// command if needed (0xBE on the AHT20, 0xE1 on the AHT10).
class AHTSensor {
public:
    bool begin(TwoWire* wire, uint8_t addr = 0x38) {
        _w = wire; _a = addr;
        uint8_t st;
        if (!EnvI2C::ack(_w, _a)) return false;
        delay(40);
        if (!EnvI2C::readN(_w, _a, &st, 1)) return false;
        if (!(st & 0x08)) {
            _w->beginTransmission(_a); _w->write(0xBE); _w->write(0x08); _w->write(0x00);
            if (_w->endTransmission() != 0) { _w->beginTransmission(_a); _w->write(0xE1); _w->write(0x08); _w->write(0x00); _w->endTransmission(); }
            delay(10);
            if (!EnvI2C::readN(_w, _a, &st, 1) || !(st & 0x08)) return false;
        }
        return true;
    }
    bool update() {
        uint8_t b[6];
        _w->beginTransmission(_a); _w->write(0xAC); _w->write(0x33); _w->write(0x00);   // trigger
        if (_w->endTransmission() != 0) return false;
        delay(80);
        for (uint8_t i = 0; i < 5; i++) {
            if (!EnvI2C::readN(_w, _a, b, 6)) return false;
            if (!(b[0] & 0x80)) break;              // not busy
            delay(10);
        }
        if (b[0] & 0x80) return false;
        uint32_t h = ((uint32_t)b[1] << 12) | ((uint32_t)b[2] << 4) | (b[3] >> 4);
        uint32_t t = ((uint32_t)(b[3] & 0x0F) << 16) | ((uint32_t)b[4] << 8) | b[5];
        humidity    = h * 100.0f / 1048576.0f;
        temperature = t * 200.0f / 1048576.0f - 50.0f;
        return true;
    }
    float temperature = 0, humidity = 0;
private:
    TwoWire* _w; uint8_t _a;
};

// LPS22HB (0x5C or 0x5D): WHO_AM_I 0xB1. One-shot conversions.
class LPS22HBSensor {
public:
    bool begin(TwoWire* wire, uint8_t addr) {
        _w = wire; _a = addr;
        uint8_t id = 0;
        if (!EnvI2C::readRegs(_w, _a, 0x0F, &id, 1) || id != 0xB1) return false;
        EnvI2C::writeReg(_w, _a, 0x10, 0x00);       // CTRL_REG1: power down, one-shot use
        return true;
    }
    bool update() {
        if (!EnvI2C::writeReg(_w, _a, 0x11, 0x11)) return false;   // CTRL_REG2: ONE_SHOT, register auto-increment
        uint8_t c = 0x01;
        for (uint8_t i = 0; i < 10 && (c & 0x01); i++) { delay(10); if (!EnvI2C::readRegs(_w, _a, 0x11, &c, 1)) return false; }
        if (c & 0x01) return false;
        uint8_t b[5];
        if (!EnvI2C::readRegs(_w, _a, 0x28, b, 5)) return false;
        int32_t p = (int32_t)(((uint32_t)b[2] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[0] << 8)) >> 8;   // 24-bit signed
        pressure_hpa = p / 4096.0f;
        temperature  = (int16_t)(((uint16_t)b[4] << 8) | b[3]) / 100.0f;
        return true;
    }
    float temperature = 0, pressure_hpa = 0;
private:
    TwoWire* _w; uint8_t _a;
};

// BMP085 / BMP180 (0x77): chip ID 0x55. Datasheet integer algorithm, ultra
// high resolution (oss = 3).
class BMP180Sensor {
public:
    bool begin(TwoWire* wire, uint8_t addr = 0x77) {
        _w = wire; _a = addr;
        uint8_t id = 0, c[22];
        if (!EnvI2C::readRegs(_w, _a, 0xD0, &id, 1) || id != 0x55) return false;
        if (!EnvI2C::readRegs(_w, _a, 0xAA, c, 22)) return false;
        auto s = [&](int i) { return (int16_t)(((uint16_t)c[i] << 8) | c[i + 1]); };
        ac1 = s(0); ac2 = s(2); ac3 = s(4); ac4 = (uint16_t)s(6); ac5 = (uint16_t)s(8); ac6 = (uint16_t)s(10);
        b1 = s(12); b2 = s(14); mb = s(16); mc = s(18); md = s(20);
        return true;
    }
    bool update() {
        const uint8_t oss = 3;
        uint8_t b[3];
        if (!EnvI2C::writeReg(_w, _a, 0xF4, 0x2E)) return false;
        delay(5);
        if (!EnvI2C::readRegs(_w, _a, 0xF6, b, 2)) return false;
        int32_t ut = ((int32_t)b[0] << 8) | b[1];
        if (!EnvI2C::writeReg(_w, _a, 0xF4, 0x34 + (oss << 6))) return false;
        delay(26);
        if (!EnvI2C::readRegs(_w, _a, 0xF6, b, 3)) return false;
        int32_t up = ((((int32_t)b[0] << 16) | ((int32_t)b[1] << 8) | b[2]) >> (8 - oss));

        int32_t x1 = ((ut - (int32_t)ac6) * (int32_t)ac5) >> 15;
        if (x1 + md == 0) return false;
        int32_t x2 = ((int32_t)mc << 11) / (x1 + md);
        int32_t b5 = x1 + x2;
        temperature = ((b5 + 8) >> 4) / 10.0f;

        int32_t b6 = b5 - 4000;
        x1 = ((int32_t)b2 * ((b6 * b6) >> 12)) >> 11;
        x2 = ((int32_t)ac2 * b6) >> 11;
        int32_t x3 = x1 + x2;
        int32_t b3 = ((((int32_t)ac1 * 4 + x3) << oss) + 2) / 4;
        x1 = ((int32_t)ac3 * b6) >> 13;
        x2 = ((int32_t)b1 * ((b6 * b6) >> 12)) >> 16;
        x3 = ((x1 + x2) + 2) >> 2;
        uint32_t b4 = ((uint32_t)ac4 * (uint32_t)(x3 + 32768)) >> 15;
        if (b4 == 0) return false;
        uint32_t b7 = ((uint32_t)up - b3) * (uint32_t)(50000 >> oss);
        int32_t p = b7 < 0x80000000UL ? (int32_t)((b7 * 2) / b4) : (int32_t)((b7 / b4) * 2);
        x1 = (p >> 8) * (p >> 8);
        x1 = (x1 * 3038) >> 16;
        x2 = (-7357 * p) >> 16;
        p = p + ((x1 + x2 + 3791) >> 4);
        pressure = (float)p;                        // Pa
        return true;
    }
    float temperature = 0, pressure = 0;
private:
    TwoWire* _w; uint8_t _a;
    int16_t ac1, ac2, ac3, b1, b2, mb, mc, md; uint16_t ac4, ac5, ac6;
};

// BME680 (0x76/0x77): chip ID 0x61. Temperature, humidity and pressure from
// Bosch's floating-point compensation; the gas heater is left off.
class BME680Sensor {
public:
    bool begin(TwoWire* wire, uint8_t addr) {
        _w = wire; _a = addr;
        uint8_t id = 0, c1[25], c2[16];
        if (!EnvI2C::readRegs(_w, _a, 0xD0, &id, 1) || id != 0x61) return false;
        if (!EnvI2C::readRegs(_w, _a, 0x89, c1, 25) || !EnvI2C::readRegs(_w, _a, 0xE1, c2, 16)) return false;
        auto u16 = [](const uint8_t* c, int lo) { return (uint16_t)(c[lo] | (c[lo + 1] << 8)); };
        // c1 holds 0x89..0xA1, c2 holds 0xE1..0xF0
        t1 = u16(c2, 8);  t2 = (int16_t)u16(c1, 1);  t3 = (int8_t)c1[3];                  // 0xE9/EA, 0x8A/8B, 0x8C
        p1 = u16(c1, 5);  p2 = (int16_t)u16(c1, 7);  p3 = (int8_t)c1[9];                  // 0x8E/8F, 0x90/91, 0x92
        p4 = (int16_t)u16(c1, 11); p5 = (int16_t)u16(c1, 13); p7 = (int8_t)c1[15]; p6 = (int8_t)c1[16];   // 0x94/95, 0x96/97, 0x98, 0x99
        p8 = (int16_t)u16(c1, 19); p9 = (int16_t)u16(c1, 21); p10 = c1[23];               // 0x9C/9D, 0x9E/9F, 0xA0
        h1 = (uint16_t)((c2[2] << 4) | (c2[1] & 0x0F));                                    // 0xE3, 0xE2
        h2 = (uint16_t)((c2[0] << 4) | (c2[1] >> 4));                                      // 0xE1, 0xE2
        h3 = (int8_t)c2[3]; h4 = (int8_t)c2[4]; h5 = (int8_t)c2[5]; h6 = c2[6]; h7 = (int8_t)c2[7];   // 0xE4..0xE8
        EnvI2C::writeReg(_w, _a, 0x71, 0x00);       // ctrl_gas_1: no gas measurement
        return true;
    }
    bool update() {
        EnvI2C::writeReg(_w, _a, 0x72, 0x01);       // ctrl_hum: humidity x1
        if (!EnvI2C::writeReg(_w, _a, 0x74, (0x02 << 5) | (0x05 << 2) | 0x01)) return false;   // T x2, P x16, forced
        uint8_t st = 0;
        for (uint8_t i = 0; i < 10; i++) { delay(10); if (!EnvI2C::readRegs(_w, _a, 0x1D, &st, 1)) return false; if (st & 0x80) break; }
        if (!(st & 0x80)) return false;             // no new data
        uint8_t b[8];
        if (!EnvI2C::readRegs(_w, _a, 0x1F, b, 8)) return false;
        uint32_t pa = ((uint32_t)b[0] << 12) | ((uint32_t)b[1] << 4) | (b[2] >> 4);
        uint32_t ta = ((uint32_t)b[3] << 12) | ((uint32_t)b[4] << 4) | (b[5] >> 4);
        uint16_t ha = ((uint16_t)b[6] << 8) | b[7];

        float v1 = ((ta / 16384.0f) - (t1 / 1024.0f)) * t2;
        float v2 = ((ta / 131072.0f) - (t1 / 8192.0f)) * ((ta / 131072.0f) - (t1 / 8192.0f)) * (t3 * 16.0f);
        float tf = v1 + v2;
        temperature = tf / 5120.0f;

        v1 = (tf / 2.0f) - 64000.0f;
        v2 = v1 * v1 * (p6 / 131072.0f);
        v2 = v2 + (v1 * p5 * 2.0f);
        v2 = (v2 / 4.0f) + (p4 * 65536.0f);
        v1 = (((p3 * v1 * v1) / 16384.0f) + (p2 * v1)) / 524288.0f;
        v1 = (1.0f + (v1 / 32768.0f)) * p1;
        if (v1 == 0) return false;
        float pc = 1048576.0f - pa;
        pc = ((pc - (v2 / 4096.0f)) * 6250.0f) / v1;
        v1 = (p9 * pc * pc) / 2147483648.0f;
        v2 = pc * (p8 / 32768.0f);
        float v3 = (pc / 256.0f) * (pc / 256.0f) * (pc / 256.0f) * (p10 / 131072.0f);
        pressure = pc + (v1 + v2 + v3 + (p7 * 128.0f)) / 16.0f;   // Pa

        float tc = temperature;
        v1 = ha - ((h1 * 16.0f) + ((h3 / 2.0f) * tc));
        v2 = v1 * ((h2 / 262144.0f) * (1.0f + ((h4 / 16384.0f) * tc) + ((h5 / 1048576.0f) * tc * tc)));
        float h = v2 + ((h6 / 16384.0f) + ((h7 / 2097152.0f) * tc)) * v2 * v2;
        humidity = h < 0 ? 0 : h > 100 ? 100 : h;
        return true;
    }
    float temperature = 0, humidity = 0, pressure = 0;
private:
    TwoWire* _w; uint8_t _a;
    uint16_t t1, p1, h1, h2; int16_t t2, p2, p4, p5, p8, p9; int8_t t3, p3, p6, p7, h3, h4, h5, h7; uint8_t p10, h6;
};

// Minimal driver for TI current/power monitors on the same bus: INA219,
// INA226, INA260 and INA3221 (3 channels). They share the 0x40-0x45 address
// range, so the chip is told apart by its ID registers (TI manufacturer ID
// 0x5449 at 0xFE plus die ID at 0xFF; the INA219 has neither and is known by
// its power-on config value). Current is worked out from the shunt voltage,
// so no calibration register is needed -- only the shunt resistance, 0.1 ohm
// on the usual breakout modules (ENV_INA_SHUNT_OHMS to change it; the INA260
// has its own 2 mohm shunt inside). Same full-stop transfers as above.
#ifndef ENV_INA_SHUNT_OHMS
  #define ENV_INA_SHUNT_OHMS 0.1f
#endif
class INASensor {
public:
    enum Kind : uint8_t { NONE, INA219, INA226, INA260, INA3221 };

    bool begin(TwoWire* wire, uint8_t addr) {
        _wire = wire;
        _addr = addr;
        kind = NONE;
        _wire->beginTransmission(_addr);
        if (_wire->endTransmission() != 0) return false;
        uint16_t mfr = 0, die = 0, cfg = 0;
        if (read16(0xFE, mfr) && mfr == 0x5449 && read16(0xFF, die)) {
            if ((die & 0xFFF0) == 0x2260) kind = INA226;
            else if ((die & 0xFFF0) == 0x2270) kind = INA260;
            else if (die == 0x3220) kind = INA3221;
        } else if (read16(0x00, cfg) && cfg == 0x399F) kind = INA219;   // power-on default
        return kind != NONE;
    }

    uint8_t channels() const { return kind == INA3221 ? 3 : kind == NONE ? 0 : 1; }
    const char* name() const { return kind == INA219 ? "INA219" : kind == INA226 ? "INA226" : kind == INA260 ? "INA260" : kind == INA3221 ? "INA3221" : "none"; }

    // One channel (0 for single-channel chips). Volts on the bus side, amps
    // through the shunt (negative = flowing the other way).
    bool update(uint8_t ch, float& volts, float& amps) {
        uint16_t sh = 0, bus = 0;
        switch (kind) {
            case INA219:
                if (!read16(0x01, sh) || !read16(0x02, bus)) return false;
                volts = (bus >> 3) * 0.004f;
                amps  = (int16_t)sh * 10e-6f / ENV_INA_SHUNT_OHMS;
                return true;
            case INA226:
                if (!read16(0x01, sh) || !read16(0x02, bus)) return false;
                volts = bus * 1.25e-3f;
                amps  = (int16_t)sh * 2.5e-6f / ENV_INA_SHUNT_OHMS;
                return true;
            case INA260:
                if (!read16(0x01, sh) || !read16(0x02, bus)) return false;
                volts = bus * 1.25e-3f;
                amps  = (int16_t)sh * 1.25e-3f;          // current register, built-in shunt
                return true;
            case INA3221:
                if (ch > 2 || !read16(0x01 + 2 * ch, sh) || !read16(0x02 + 2 * ch, bus)) return false;
                volts = ((int16_t)bus >> 3) * 8e-3f;
                amps  = ((int16_t)sh >> 3) * 40e-6f / ENV_INA_SHUNT_OHMS;
                return true;
            default:
                return false;
        }
    }

    Kind kind = NONE;

private:
    TwoWire* _wire;
    uint8_t  _addr;

    bool read16(uint8_t reg, uint16_t& v) {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        if (_wire->endTransmission() != 0) return false;  // full stop -- nRF52 TWIM hangs on repeated start
        _wire->requestFrom(_addr, (uint8_t)2);
        if (_wire->available() < 2) return false;
        v = ((uint16_t)_wire->read() << 8) | _wire->read();
        return true;
    }
};

// Auto-probes the Grove I2C header for temperature / humidity / pressure
// sensors -- the M5Stack ENV units (ENV I = DHT12 + BMP280, ENV II = SHT30 +
// BMP280, ENV III = SHT30 + QMP6988) or any of the single chips above -- and
// exposes one combined reading: humidity from the best humidity chip found,
// pressure from the best pressure chip, temperature from the humidity chip
// (else the pressure chip). Also finds up to two INA power monitors (see
// INASensor), e.g. one on the solar panel and one on the battery.
namespace EnvSensors {

    // Humidity and pressure sources, in order of preference.
    enum Hum : uint8_t { H_NONE, H_SHT4X, H_SHT3X, H_SHTC3, H_BME680, H_BME280, H_AHT, H_DHT12 };
    enum Prs : uint8_t { P_NONE, P_BME280, P_BME680, P_BMP280, P_QMP6988, P_LPS22HB, P_BMP180 };

    inline bool humidity_found = false;
    inline bool pressure_found = false;
    inline Hum  hum_src = H_NONE;
    inline Prs  prs_src = P_NONE;

    inline SHT3XSensor     sht3x;
    inline SHT4XSensor     sht4x;
    inline SHTC3Sensor     shtc3;
    inline AHTSensor       aht;
    inline DHT12Sensor     dht12;
    inline BMP280Sensor    bmp280;               // BMP280 or BME280
    inline BME680Sensor    bme680;
    inline QMP6988Sensor   qmp6988;
    inline LPS22HBSensor   lps22hb;
    inline BMP180Sensor    bmp180;

    inline float temperature  = NAN;
    inline float humidity     = NAN;
    inline float pressure_hpa = NAN;
    inline float altitude_m   = NAN;

    // Every chip found, with its own last reading (NAN where it does not
    // measure that), for pages that show the chips one by one. read() fills it.
    enum Chip : uint8_t { C_SHT4X, C_SHT3X, C_SHTC3, C_AHT, C_DHT12, C_BME280, C_BME680, C_BMP280, C_QMP6988, C_LPS22HB, C_BMP180 };
    struct ChipReading { Chip id; float t, h, p_hpa; bool ok; };
    inline ChipReading chips[11];
    inline uint8_t     chip_n = 0;
    inline const char* chip_name(Chip c) {
        static const char* n[] = {"SHT4x", "SHT30", "SHTC3", "AHT10/20", "DHT12", "BME280", "BME680", "BMP280", "QMP6988", "LPS22HB", "BMP180"};
        return n[c];
    }

    // Power monitors: up to 2 chips, up to 4 channels in total (an INA3221
    // counts 3). read_power() refreshes volts/amps/ok of every channel.
    struct PowerChannel { uint8_t dev, ch; float volts, amps; bool ok; };
    inline INASensor    ina[2];
    inline uint8_t      ina_addr[2] = {0, 0};
    inline uint8_t      ina_count = 0;
    inline PowerChannel power[4];
    inline uint8_t      power_n = 0;

    inline const char* hum_name(Hum h) {
        static const char* n[] = {"none", "SHT4x", "SHT30", "SHTC3", "BME680", "BME280", "AHT10/20", "DHT12"};
        return n[h];
    }
    inline const char* prs_name(Prs p) {
        static const char* n[] = {"none", "BME280", "BME680", "BMP280", "QMP6988", "LPS22HB", "BMP180"};
        return n[p];
    }

    inline void init() {
        RNS::trace("Looking for Grove environment sensors...");
        Wire.begin();
        bool claimed[128] = {false};               // addresses already taken by a chip found below
        bool f_sht4x = false, f_sht3x = false, f_shtc3 = false, f_aht = false, f_dht12 = false;
        bool f_bme280 = false, f_bmp280 = false, f_bme680 = false, f_qmp = false, f_lps = false, f_bmp180 = false;

        // 0x44 / 0x45: SHT30 (status CRC) before SHT4x (serial CRC); an INA226 can sit here too
        for (uint8_t a : {0x44, 0x45}) {
            if (!f_sht3x && sht3x.begin(&Wire, a)) { f_sht3x = claimed[a] = true; continue; }
            if (!f_sht4x && sht4x.begin(&Wire, a)) f_sht4x = claimed[a] = true;
        }
        // 0x70: QMP6988 (chip ID) before SHTC3 (ID register)
        if (qmp6988.begin(&Wire, 0x70)) f_qmp = claimed[0x70] = true;
        else if (shtc3.begin(&Wire, 0x70)) f_shtc3 = claimed[0x70] = true;
        // 0x5C / 0x5D: LPS22HB (WHO_AM_I) before DHT12 (checksum, 0x5C only)
        if (lps22hb.begin(&Wire, 0x5C)) f_lps = claimed[0x5C] = true;
        else if (dht12.begin(&Wire)) f_dht12 = claimed[0x5C] = true;
        if (!f_lps && lps22hb.begin(&Wire, 0x5D)) f_lps = claimed[0x5D] = true;
        // 0x76 / 0x77: Bosch parts, told apart by chip ID
        for (uint8_t a : {0x76, 0x77}) {
            if (!f_bme680 && bme680.begin(&Wire, a)) { f_bme680 = claimed[a] = true; continue; }
            if (!f_bme280 && !f_bmp280 && bmp280.begin(&Wire, a)) { (bmp280.is_bme280 ? f_bme280 : f_bmp280) = claimed[a] = true; continue; }
            if (a == 0x77 && !f_bmp180 && bmp180.begin(&Wire, a)) f_bmp180 = claimed[a] = true;
        }
        // 0x38: AHT10 / AHT20
        if (aht.begin(&Wire, 0x38)) f_aht = claimed[0x38] = true;

        auto add = [](Chip c) { chips[chip_n++] = ChipReading{c, NAN, NAN, NAN, false}; };
        if (f_sht4x) add(C_SHT4X);
        if (f_sht3x) add(C_SHT3X);
        if (f_shtc3) add(C_SHTC3);
        if (f_aht) add(C_AHT);
        if (f_dht12) add(C_DHT12);
        if (f_bme280) add(C_BME280);
        if (f_bme680) add(C_BME680);
        if (f_bmp280) add(C_BMP280);
        if (f_qmp) add(C_QMP6988);
        if (f_lps) add(C_LPS22HB);
        if (f_bmp180) add(C_BMP180);

        hum_src = f_sht4x ? H_SHT4X : f_sht3x ? H_SHT3X : f_shtc3 ? H_SHTC3 : f_bme680 ? H_BME680 : f_bme280 ? H_BME280 : f_aht ? H_AHT : f_dht12 ? H_DHT12 : H_NONE;
        prs_src = f_bme280 ? P_BME280 : f_bme680 ? P_BME680 : f_bmp280 ? P_BMP280 : f_qmp ? P_QMP6988 : f_lps ? P_LPS22HB : f_bmp180 ? P_BMP180 : P_NONE;
        humidity_found = hum_src != H_NONE;
        pressure_found = prs_src != P_NONE;
        RNS::tracef("Environment sensors: humidity %s, pressure %s", hum_name(hum_src), prs_name(prs_src));

        for (uint8_t a = 0x40; a <= 0x45 && ina_count < 2; a++) {
            if (claimed[a]) continue;                // an SHT30/SHT4x lives there
            if (ina[ina_count].begin(&Wire, a)) {
                ina_addr[ina_count++] = a;
                RNS::tracef("Found %s power monitor at 0x%02x", ina[ina_count - 1].name(), a);
            }
        }
        for (uint8_t d = 0; d < ina_count; d++)
            for (uint8_t c = 0; c < ina[d].channels() && power_n < 4; c++) { power[power_n].dev = d; power[power_n].ch = c; power_n++; }
    }

    inline bool installed() { return humidity_found || pressure_found; }

    // Reads every chip found (each once) into chips[], then takes humidity
    // from the chosen humidity chip, pressure from the chosen pressure chip,
    // and temperature from the humidity chip when there is one. Returns false
    // if nothing is installed or the chosen chips' reads all failed.
    inline bool read() {
        for (uint8_t i = 0; i < chip_n; i++) {
            ChipReading& c = chips[i]; bool ok = false;
            c.t = c.h = c.p_hpa = NAN;
            switch (c.id) {
                case C_SHT4X:   if ((ok = sht4x.update())) { c.t = sht4x.temperature; c.h = sht4x.humidity; } break;
                case C_SHT3X:   if ((ok = sht3x.update())) { c.t = sht3x.temperature; c.h = sht3x.humidity; } break;
                case C_SHTC3:   if ((ok = shtc3.update())) { c.t = shtc3.temperature; c.h = shtc3.humidity; } break;
                case C_AHT:     if ((ok = aht.update()))   { c.t = aht.temperature;   c.h = aht.humidity; }   break;
                case C_DHT12:   if ((ok = dht12.update())) { c.t = dht12.temperature; c.h = dht12.humidity; } break;
                case C_BME280:  if ((ok = bmp280.update())) { c.t = bmp280.cTemp; c.h = bmp280.humidity; c.p_hpa = bmp280.pressure / 100.0f; } break;
                case C_BMP280:  if ((ok = bmp280.update())) { c.t = bmp280.cTemp; c.p_hpa = bmp280.pressure / 100.0f; } break;
                case C_BME680:  if ((ok = bme680.update())) { c.t = bme680.temperature; c.h = bme680.humidity; c.p_hpa = bme680.pressure / 100.0f; } break;
                case C_QMP6988: if ((ok = qmp6988.update())) { c.t = qmp6988.cTemp; c.p_hpa = qmp6988.pressure / 100.0f; } break;
                case C_LPS22HB: if ((ok = lps22hb.update())) { c.t = lps22hb.temperature; c.p_hpa = lps22hb.pressure_hpa; } break;
                case C_BMP180:  if ((ok = bmp180.update()))  { c.t = bmp180.temperature; c.p_hpa = bmp180.pressure / 100.0f; } break;
            }
            c.ok = ok;
        }
        static const Chip hum_chip[] = {C_SHT4X /*unused*/, C_SHT4X, C_SHT3X, C_SHTC3, C_BME680, C_BME280, C_AHT, C_DHT12};
        static const Chip prs_chip[] = {C_BME280 /*unused*/, C_BME280, C_BME680, C_BMP280, C_QMP6988, C_LPS22HB, C_BMP180};
        auto find = [](Chip id) -> const ChipReading* { for (uint8_t i = 0; i < chip_n; i++) if (chips[i].id == id && chips[i].ok) return &chips[i]; return nullptr; };
        bool ok = false;
        const ChipReading* hc = hum_src != H_NONE ? find(hum_chip[hum_src]) : nullptr;
        const ChipReading* pc = prs_src != P_NONE ? find(prs_chip[prs_src]) : nullptr;
        if (hc) { humidity = hc->h; temperature = hc->t; ok = true; }
        if (pc) {
            pressure_hpa = pc->p_hpa;
            altitude_m = 44330.0f * (1.0f - powf(pressure_hpa / 1013.25f, 0.1903f));
            if (!hc) temperature = pc->t;
            ok = true;
        }
        return ok;
    }

    // Label for one power channel: "INA226", or "INA3221/2" for its channel 2.
    inline const char* power_label(uint8_t i, char* buf, size_t n) {
        const INASensor& s = ina[power[i].dev];
        if (s.channels() > 1) snprintf(buf, n, "%s/%u", s.name(), (unsigned)power[i].ch + 1); else snprintf(buf, n, "%s", s.name());
        return buf;
    }

    // Reads every power channel. An INA3221 input with nothing connected
    // (bus under 0.1 V) is marked not ok, so it is left off pages.
    inline bool read_power() {
        bool any = false;
        for (uint8_t i = 0; i < power_n; i++) {
            PowerChannel& p = power[i];
            p.ok = ina[p.dev].update(p.ch, p.volts, p.amps) && !(ina[p.dev].kind == INASensor::INA3221 && p.volts < 0.1f);
            any |= p.ok;
        }
        return any;
    }

    // "SHT30 + QMP6988", "BME280" (one chip doing both), "AHT10/20", ...
    inline const char* name() {
        static char b[24];
        bool same = (hum_src == H_BME280 && prs_src == P_BME280) || (hum_src == H_BME680 && prs_src == P_BME680);
        if (humidity_found && pressure_found && !same) snprintf(b, sizeof b, "%s + %s", hum_name(hum_src), prs_name(prs_src));
        else if (humidity_found) snprintf(b, sizeof b, "%s", hum_name(hum_src));
        else if (pressure_found) snprintf(b, sizeof b, "%s", prs_name(prs_src));
        else return "none";
        return b;
    }

}

#endif // HAS_ENV_SENSORS
