# ESP32 UART to Bluetooth SPP Bridge (BTProxy) User Manual

**Firmware Version:** 1.0

## 1. Overview

This firmware turns an ESP32 module into a transparent bridge between a UART (Serial) interface and a classic Bluetooth device using the Serial Port Profile (SPP). It allows a device connected via UART to communicate with a remote Bluetooth device as if they were connected by a serial cable.

The firmware has two modes of operation:
*   **Proxy Mode:** The default mode. The ESP32 automatically connects to a pre-configured remote Bluetooth device and transparently forwards all data between the UART and Bluetooth connection.
*   **Command Mode:** An interactive mode that allows you to configure the remote Bluetooth device's settings (MAC address and PIN) via the UART interface.

## 2. Hardware Requirements

*   An **ESP32 development board** with classic Bluetooth support (most standard ESP32 modules are suitable).
*   The built-in LED on GPIO 2 acts as a status indicator for the firmware.
*   The "Boot" button (connected to GPIO 0) is used to switch between Proxy and Command modes.

### LED indicator states

*   **Rapid flashing (after power-up):** The device is offering a short window to enter Command Mode. Press the Boot button while the LED is flashing quickly.
*   **Solid on:** Command Mode is active and the device is waiting for commands over UART.
*   **Off:** Proxy Mode is active and the device is idle.
*   **Brief flickers while in Proxy Mode:** Data is being relayed between UART and Bluetooth.

## 3. UART (Serial Port) Settings

To communicate with the ESP32 module (especially in Command Mode), you need to configure your serial terminal or host device with the following parameters:

*   **Baud Rate:** 115200
*   **Data Bits:** 8
*   **Parity:** None
*   **Stop Bits:** 1
*   **Flow Control:** None

## 4. Configuration Steps

To set up the connection to your target Bluetooth device, you need to enter Command Mode and issue a few commands.

### Step 1: Enter Command Mode

1.  Connect the ESP32 to your computer via USB.
2.  Open a serial terminal with the settings specified in section 3.
3.  Wait until the built-in LED starts flashing rapidly right after boot and, while it is flashing, press the **"Boot" button** (GPIO 0) on the ESP32 board. Pressing the **"Boot" button** (GPIO 0) when ESP32 is connected to a remote BT device will put it into command mode as well.
4.  The device will report the mode change in the serial terminal:
    ```
    %ESP32_BTP% CMD MODE: ENABLED
    ```
    And the built-in LED become solid on. Now the device is ready to accept commands.

### Step 2: Scan for Devices (Optional but Recommended)

To find the MAC address of your target Bluetooth device, use the `SCAN` command.

```
SCAN
```

The ESP32 will perform a 10-second scan for nearby classic Bluetooth devices and list their MAC addresses, names, and services.

Example output:
```
%ESP32_BTP% INFO: Scanning, please wait...
%ESP32_BTP%  SCAN: found 01:23:45:67:89:AB
...
%ESP32_BTP% INFO: Found devices:
%ESP32_BTP% INFO: Address: 01:23:45:67:89:AB  Name: [MyBTDevice]  RSSI: -55
%ESP32_BTP% INFO:        Services found: 1
%ESP32_BTP% INFO:        channel 1 (SPP)
```
Copy the MAC address of your target device (e.g., `01:23:45:67:89:AB`).

### Step 3: Configure Connection Settings

Use the `SETADDR` and `SETPIN` commands to store the remote device's credentials.

1.  **Set the MAC Address:**
    ```
    SETADDR XX:XX:XX:XX:XX:XX
    ```
    Replace `XX:XX:XX:XX:XX:XX` with the address you found in the scan.
    Example: `SETADDR 01:23:45:67:89:AB`

2.  **Set the PIN Code:**
    ```
    SETPIN YYYY
    ```
    Replace `YYYY` with the PIN code of the remote device.
    Example: `SETPIN 1234`

The settings are saved automatically to the ESP32's non-volatile memory.

### Step 4: Verify Configuration (Optional)

Use the `INFO` command to check the currently saved settings.

```
INFO
```

### Step 5: Exit Command Mode

To start the proxy, exit command mode by pressing the **"Boot" button** again. The device will report the mode change:

```
%ESP32_BTP% CMD MODE: DISABLED
```

Alternatively, you can simply reset the ESP32. The device will now automatically attempt to connect to the configured remote device.

## 5. Command Reference

All commands must be terminated with a newline character (press Enter).

*   `HELP`: Prints the list of available commands.
*   `SCAN`: Scans for nearby classic Bluetooth devices.
*   `SETADDR <address>`: Sets and saves the MAC address of the remote device.
*   `SETPIN <pin>`: Sets and saves the PIN code for the remote device.
*   `INFO`: Displays the current saved configuration.
*   `CLEAR`: Resets the configuration to default (blank) values.

If you enter an unknown command, the device will show an error message.
```
%ESP32_BTP% ERROR: unknown command [your_command], type HELP for help
```

