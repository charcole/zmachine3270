#pragma once

#include "3270.h"

class PacketParser
{
public:
    PacketParser();
    void Parse(const uint8_t *PacketData, int PacketSize);
    void Dump(const uint8_t *Data);

    ParsedSDLC SDLC;
    ParsedFID2 FID2;
    ParsedRU RU;
    ParsedRequest Req;
    ParsedResponce Res;
    ParsedSenseData Sense;
    int StartOfData;
    int EndOfData;
    bool bPacketGood;
    bool bSDLCValid;
    bool bFID2Valid;
    bool bRequestValid;
    bool bResponseValid;
    bool bSenseValid;
};
