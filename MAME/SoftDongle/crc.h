#pragma once

#include <stdint.h>

void BuildCRCLookup();
bool ValidatePacket(const uint8_t *Data, int Length);