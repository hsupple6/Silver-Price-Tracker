# MCU Silver Price Tracker

This project is a custom embedded system built around an ESP32 and an ILI9341 TFT display, developed entirely from scratch. The ESP32 handles system logic and SPI communication with the display, with firmware written to manage initialization, drawing routines, and peripheral control.

In addition to the electronics and software, all mechanical components were custom designed in CAD, including both the internal structural shell and the external sheet metal enclosure. The enclosure was designed to precisely fit the electronics while prioritizing durability, clean assembly, and a compact form factor.

This repository documents a complete hardware–software integration project, combining embedded programming, electrical interfacing, and mechanical design into a single, cohesive system.

# Spec Sheet

- NODE MCU ESP32S
- ILI 9341 320 x 240 SPI Display
- 12mm 12v Momentary Switch
- 3.3V Regulator Module

- PLA
- 0.62" Aluminum Sheet

# Manufacturing Process

Laser Cut Aluminum Sheet to Size, use Sheet Brake to Bend Metal Parts

3D Print Main Shell Assembly

Solder all Connections Between Esp32 and Display According to Wiring Diagram
QC All Connections

Upload Code Using USB-TTY UART Connector from Arduino IDE

Assemble using either AA Battery Tray or USB-C Cable
