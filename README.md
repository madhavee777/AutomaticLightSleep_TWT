# ESP32-C6 Wi-Fi 6 TWT & MQTT Low Power Sensor

## Overview
This repository contains an advanced, ultra-low-power IoT sensor template designed for the **ESP32-C6**. It leverages **Wi-Fi 6 (802.11ax) Target Wake Time (TWT)** and **FreeRTOS Automatic Light Sleep** to maintain a persistent connection to an MQTT broker (HiveMQ) while drawing sub-milliamp current during sleep intervals. 

This project bridges the gap between networking protocols and aggressive hardware power management, solving common race conditions and asynchronous blocking issues found in commercial IoT development.

## Core Architecture & Technical Solutions

### 1. Two-Stage Power Management Boot Sequence
If maximum power saving is enabled too early, the ESP32 antenna shuts off before the router can assign an IP address via DHCP. This firmware uses an Event-Driven sequence:
1. **Boot:** Wi-Fi Power Save is disabled (`WIFI_PS_NONE`) for a lightning-fast IP acquisition and MQTT TCP handshake.
2. **Lock:** FreeRTOS `EventGroups` halt the application until the MQTT `CONNECTED` event fires.
3. **Sleep:** Only after the connection is entirely secure does the firmware engage `WIFI_PS_MAX_MODEM` and negotiate the Wi-Fi 6 TWT sleep contract.

### 2. Wi-Fi 6 Target Wake Time (TWT)
Instead of disconnecting from the network (which requires a high-power reconnection later), the ESP32 negotiates an exact sleep schedule with the Wi-Fi 6 Access Point. 
* The router buffers incoming traffic.
* The ESP32 briefly wakes its RF modem for ~65ms (`min_wake_dura`) to grab data.
* *Note: The Access Point dictates the final TWT interval. The firmware must dynamically accept the router's enforced sleep window (e.g., 292 seconds).*

### 3. Asynchronous MQTT & QoS 0 "Fire and Forget"
A common pitfall in RTOS IoT design is additive blocking. Using MQTT QoS 1 (At Least Once) requires the ESP32 to wait for a `PUBACK` receipt. If the network drops packets, the task blocks indefinitely, breaking the strict 15-second Light Sleep cadence and causing duplicate data payloads. 
* **The Solution:** This architecture implements **QoS 0** (Fire and Forget) for the telemetry payload. The sensor pushes the payload to the network buffer and immediately yields to `vTaskDelay`, allowing the FreeRTOS Idle Task to instantly drop the CPU into Light Sleep.

## Hardware & Environment
* **Target:** ESP32-C6 Development Board
* **Framework:** ESP-IDF (v5.2+)
* **Prerequisites:** A Wi-Fi 6 (802.11ax) router is required for TWT functionality. (Code defaults to standard DTIM Light Sleep on older routers).

## Build & Run Instructions
1. Configure the environment for the ESP32-C6:
   ```bash
   idf.py set-target esp32c6

2. Build the firmware and flash it to the board:
   ```bash
   idf.py build flash monitor