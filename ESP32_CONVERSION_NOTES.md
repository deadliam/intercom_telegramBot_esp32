# ESP32 WROOM-32D Conversion Notes

## Overview
This document outlines the changes made to convert the NodeMCU (ESP8266) Telegram Intercom code to work with ESP32 WROOM-32D.

## Hardware Pin Mapping

| Function | NodeMCU (ESP8266) | ESP32 WROOM-32D | Notes |
|----------|-------------------|-----------------|-------|
| Call Detection | A0 | GPIO36 (ADC1_CH0) | Analog input only |
| Camera Button | D5 | GPIO18 | Digital I/O |
| Door Button | D6 | GPIO19 | Digital I/O |
| Built-in LED | GPIO16 | GPIO2 | Status indication |

## Library Changes

### Includes Updated
- `ESP8266WiFi.h` → `WiFi.h`
- `ESP8266WebServer.h` → `WebServer.h`
- `ESP8266mDNS.h` → `ESPmDNS.h`
- `coredecls.h` → `esp_system.h`

### Function Replacements
- `ESP.getResetReason()` → `esp_reset_reason()`
- `ESP.getChipId()` → `ESP.getEfuseMac()`
- `ESP.getHeapFragmentation()` → Not available (set to 0)
- `ESP.getMaxFreeBlockSize()` → `ESP.getMaxAllocHeap()`
- `ESP.eraseConfig()` → Not needed (WiFiManager handles this)
- `ENC_TYPE_NONE` → `WIFI_AUTH_OPEN`

### Watchdog Changes
- ESP32 handles watchdog automatically
- Removed manual `ESP.wdtFeed()` calls
- Removed `ESP.wdtEnable()/ESP.wdtDisable()`

## ESP32-Specific Optimizations

### ADC Configuration
- Added `analogReadResolution(10)` for ESP8266 compatibility (0-1023 range)
- ESP32 default is 12-bit (0-4095), but 10-bit maintains compatibility

### Memory Management
- ESP32 has more RAM than ESP8266 (320KB vs 80KB)
- Fragmentation monitoring disabled (not available on ESP32)
- Memory thresholds can be adjusted higher if needed

## Wiring Connections

### ESP32 WROOM-32D Pinout
```
                    ESP32 WROOM-32D
                   ┌─────────────────┐
                   │                 │
            3V3 ───┤ 3V3         GND ├─── GND
             EN ───┤ EN          D23 ├─── 
          SENSOR ───┤ VP(36)      D22 ├─── 
                   │ VN(39)      TX0 ├─── 
                   │ D34         RX0 ├─── 
                   │ D35         D21 ├─── 
                   │ D32         D19 ├─── DOOR BUTTON
                   │ D33         D18 ├─── CAMERA BUTTON
                   │ D25          D5 ├─── 
                   │ D26         TX2 ├─── 
                   │ D27         RX2 ├─── 
                   │ D14          D4 ├─── 
                   │ D12          D2 ├─── LED (Built-in)
                   │ D13         D15 ├─── 
                   │ GND         GND ├─── GND
                   │ VIN         3V3 ├─── 3V3
                   └─────────────────┘
```

### Connection Details
- **Call Detection**: Connect to GPIO36 (VP) - This is an analog-only pin
- **Camera Button**: Connect optocoupler to GPIO18
- **Door Button**: Connect optocoupler to GPIO19
- **Power**: 3.3V or 5V via VIN pin
- **Ground**: Connect all grounds together

## Important Notes

### Power Requirements
- ESP32 consumes more power than ESP8266 (160-240mA vs 80mA)
- Ensure adequate power supply (minimum 500mA recommended)
- Consider power management for battery applications

### WiFi Compatibility
- ESP32 supports both 2.4GHz and 5GHz (depending on variant)
- WiFiManager library works the same way
- Better WiFi range and stability compared to ESP8266

### Memory Advantages
- 320KB RAM vs 80KB on ESP8266
- More stable operation with complex applications
- Better handling of large web pages and JSON responses

### Programming
- Use ESP32 board package in Arduino IDE
- Select "ESP32 Dev Module" or "ESP32 Wrover Module"
- Upload speed: 921600 baud (or lower if issues occur)
- Flash frequency: 80MHz
- Partition scheme: Default 4MB with spiffs

## Testing Checklist

- [ ] Compile successfully with ESP32 board package
- [ ] WiFi connection and configuration portal
- [ ] Telegram bot connectivity
- [ ] Call detection on GPIO36
- [ ] Camera button activation on GPIO18
- [ ] Door button activation on GPIO19
- [ ] Web interface accessibility
- [ ] OTA updates functionality
- [ ] Serial commands working
- [ ] Memory monitoring and cleanup

## Troubleshooting

### Common Issues
1. **Compilation errors**: Ensure ESP32 board package is installed
2. **GPIO36 not working**: This pin is input-only, cannot be used as output
3. **WiFi connection issues**: Check power supply stability
4. **Upload failures**: Try lower baud rates (115200)
5. **Memory issues**: ESP32 has more RAM, adjust thresholds if needed

### Board Package Installation
1. File → Preferences → Additional Board Manager URLs
2. Add: `https://dl.espressif.com/dl/package_esp32_index.json`
3. Tools → Board → Boards Manager → Search "ESP32" → Install

## Performance Improvements

### ESP32 Advantages
- Dual-core processor (240MHz)
- More GPIO pins available
- Hardware encryption support
- Better analog-to-digital converter
- More stable WiFi connection
- Support for Bluetooth (if needed in future)

The conversion maintains full compatibility with the original functionality while taking advantage of ESP32's improved capabilities.
