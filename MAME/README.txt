The included patch can be applied on top of MAME starting with ref 11782b47bd8fcc670ad259d7fe08de86a97f5eb5.

The driver is a mess as it was mainly just used for debugging the firmware and prototyping the network state machine so isn't possible to use as a terminal without quite a bit more work.

SDLC support was hacked in to the Z80 SIO driver and doesn't work exactly as it does in reality.
