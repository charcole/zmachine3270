The included patch can be applied on top of MAME starting with ref 11782b47bd8fcc670ad259d7fe08de86a97f5eb5.

The driver is a mess as it was mainly just used for debugging the firmware and prototyping the network state machine so isn't possible to use as a terminal without quite a bit more work.

SDLC support was hacked in to the Z80 SIO driver and doesn't work exactly as it does in reality.

SoftDongle is a version of the ESP32 firmware that can run on a PC and be portable. Sends and recv data via network connection. Intended to feed into a future version of MAME.



BREAKING NEWS: Informer 207 376 support has been kindly implemented on the main branch of MAME by Dirk Best partly based on this code!
