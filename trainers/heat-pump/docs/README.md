# Vesta Core HVAC Trainer Firmware (ESP32-S3 Build) ❄️🔥

This repository contains the **ESP32-S3 optimized** PlatformIO firmware for the **Vesta Core HVAC Trainer**. This code simulates a complete residential HVAC system, including refrigeration physics, electrical load dynamics, and fault injection capabilities.

This build is specifically tailored to leverage the hardware features of the ESP32-S3, providing a dual-interface for student interaction: a real-time web dashboard served over **Wi-Fi**, and a telemetry stream over **Bluetooth Low Energy (BLE)** for the native Android diagnostic app.

---

## 🚀 S3-Specific Optimizations

*   **Dual Core Architecture**: Ready to be expanded to leverage the dual-core architecture of the S3 for separating the physics engine from the web/BLE communication stack.
*   **Native USB-CDC**: Includes `build_flags` for enabling the native USB serial monitor on the S3 for faster debugging.
*   **Modern BLE Stack**: Utilizes the updated NimBLE library fully compatible with the ESP32-S3's Bluetooth 5 (LE) radio.

---

## 📁 Repository Layout

```text
├── src/
│   └── main.cpp      # Main application logic, physics engine, and server setup
├── data/
│   ├── index.html    # Student web portal (served via web server)
│   └── student.html  # Deprecated/old student portal
├── lib/              # Project-specific libraries
├── include/          # Project header files
├── platformio.ini    # PlatformIO project configuration for ESP32-S3
└── partitions.csv    # ESP32 flash partition layout
```

---

## 🛠️ Installation & Setup

### Prerequisites
*   **Visual Studio Code** with the **PlatformIO IDE** extension.
*   An **ESP32-S3** development board.

### Setup Instructions

1.  **Clone the repository:**
    ```bash
    git clone https://github.com/Randym1988/HVAC-trainer-S3-Build.git
    ```

2.  **Open in PlatformIO:**
    *   Open Visual Studio Code.
    *   Click the **PlatformIO icon** on the left sidebar.
    *   Click **Open Project** and select the cloned `HVAC-trainer-S3-Build` folder.

3.  **Build & Upload:**
    *   Connect your ESP32-S3 board to your computer.
    *   Click the **PlatformIO: Upload** button (the arrow icon) in the bottom status bar of VS Code.

---

## 📄 License
This project is proprietary and customized for Mitchell Media's Vesta Core Trainer platforms.
