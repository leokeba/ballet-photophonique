#pragma once

// Gledopto GL-C-611WL, "Elite 5P-EXMU" (docs/DESIGN.md section 3): a classic
// ESP32 (D0WD-V3, 4 MB flash, no PSRAM), five low-side PWM outputs, a LAN8720
// Ethernet PHY, a PDM microphone. Pins from Gledopto's manual and WLED's
// "Gledopto Series With Ethernet" board.

namespace board {

// The output terminals, in the order printed on the case: V+ IO18 IO16 IO12
// IO4 IO2. GPIO 12 and 2 are strapping pins, held low by the board at reset.
constexpr int kOutputs[5] = {18, 16, 12, 4, 2};

// The Function button (WLED: on/off, colour; 10 s: reset). Not used yet.
constexpr int kButton = 17;
// The spare terminal for DIY use. Not used yet.
constexpr int kSpare = 13;

// Ethernet: a LAN8720 over RMII (fixed: TXD0 19, TX_EN 21, TXD1 22, RXD0 25,
// RXD1 26, CRS_DV 27), at address 1, MDC 23, MDIO 33, its 50 MHz oscillator
// powered by GPIO 5 and clocking GPIO 0: TesserKIT's
// tesserkit::boards::gledopto(), which main.cpp passes to Network.

// PDM microphone on I2S. Not used.
constexpr int kMicData = 32;
constexpr int kMicClock = 15;

}  // namespace board
