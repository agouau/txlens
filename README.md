# TxLens

**See where your data goes.**

One of the hardest parts of an advanced persistent threat (APT) is realizing that your computer has been compromised in the first place. An attacker may quietly maintain access and send data out while everything appears normal.

TxLens is a simple, low-cost tool that makes outgoing network activity visible. It runs on an ESP32-S3 with a touchscreen, acting as a small Wi-Fi router between your computer and your existing network. It shows where the connected device sends data and how much it sends, giving you a view of that traffic outside the computer itself.

TxLens helps you notice unfamiliar destinations and unexpected uploads. It provides visibility into network activity; it cannot determine on its own whether a connection is malicious or detect every kind of compromise.

## A look at the interface

The touchscreen keeps the focus on outgoing traffic, with destination details and device settings close at hand.

| Start screen | Live traffic #1 | Live traffic #2 |
| :---: | :---: | :---: |
| <img src="assets/screenshots/1-0.png" alt="TxLens start screen with connection status" width="200"> | <img src="assets/screenshots/1-1.png" alt="Live outgoing traffic graphs by destination" width="200"> | <img src="assets/screenshots/1-2.png" alt="Traffic list showing domain names and IP addresses" width="200"> |

**TX Traffic** shows destination IP addresses or domain names, when available, alongside live graphs covering the last 60 seconds. It makes bursts of outgoing activity easy to spot.

| Destination details | Settings | Device status |
| :---: | :---: | :---: |
| <img src="assets/screenshots/1-3.png" alt="Destination details showing upload volume, ISP, location and reputation information" width="200"> | <img src="assets/screenshots/2.png" alt="Settings screen with a QR code for Wi-Fi configuration" width="200"> | <img src="assets/screenshots/3.png" alt="Device status showing resource usage, SD logging and network information" width="200"> |

**Destination details** show recent upload volume, ISP, organization, ASN and location, with optional AbuseIPDB reputation checks. **Settings** provides access to Wi-Fi configuration, while **Status** shows resource usage, network information and logging status. An optional microSD card stores per-minute traffic logs as CSV files.

## Get started

An **ESP32-S3 with 8 MB of PSRAM and more than 4 MB of flash** is recommended, along with a touchscreen. A **320 × 480** display is a good starting point. Higher resolutions can work, but keeping the display within roughly **480 × 480** is preferable: larger displays increase rendering work and can slow the interface. Upstream Wi-Fi needs to support 2.4 GHz.

1. Clone this repository or download it using GitHub's **Code - Download ZIP**.
2. Open the project in **Visual Studio Code** with the Espressif ESP-IDF extension and an **ESP-IDF 6.1** environment. A Dev Container configuration is also included.
3. Adapt the project for your board, then build and flash using the ESP-IDF extension's controls. If embedded development is unfamiliar, an AI coding tool can help.

The current firmware includes a specific board's display, touch and pin configuration, and defaults to 16 MB of flash. Other boards need corresponding changes to the hardware settings, drivers and flash configuration. Start with [`main/app_config.h`](main/app_config.h) and `sdkconfig.defaults*`.

## Collaborate

I'm available for cybersecurity projects and consulting. If you have a project in mind, I'd be happy to discuss how I can help.

Contact me at **[tx-lens@outlook.com](mailto:tx-lens@outlook.com)**.
