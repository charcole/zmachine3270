#include "crc.h"

static uint16_t CRCLookup[16][16];

void BuildCRCLookup()
{
	for (uint16_t DataNibble=0; DataNibble<16; DataNibble++)
	{
		for (uint16_t CRCNibble=0; CRCNibble<16; CRCNibble++)
		{
			uint16_t Data = DataNibble;
			uint16_t CRC = (CRCNibble << 12);
			for (uint8_t Step = 0; Step < 4; Step++)
			{
				uint16_t bLastBit = (CRC >> 15);
				uint16_t bDataBit = (Data & 1) ^ bLastBit;
				Data >>= 1;
				CRC = bDataBit + (CRC << 1);
				if (bDataBit)
				{
					CRC ^= (1<<5) + (1<<12);
				}
			}
			CRCLookup[CRCNibble][DataNibble]=CRC;
		}
	}
}

bool ValidatePacket(const uint8_t *Data, int Length)
{
	uint16_t CRC = 0xFFFF;
	while (Length--)
	{
		uint16_t Byte = *(Data++);
		CRC = (CRC << 4) ^ CRCLookup[CRC>>12][Byte & 0xF];
		Byte >>= 4;
		CRC = (CRC << 4) ^ CRCLookup[CRC>>12][Byte & 0xF];
	}
	return (CRC == 0x1D0F);
}