#include "Network.h"

NetworkState::NetworkState()
{
}

int NetworkState::GenerateNewPackets(uint8_t *Buffer)
{
    Stream.Reset();
    if (bSendReadyToRecieve)
    {
        SDLCReadyToRecieve RR(Stream);
        bSendReadyToRecieve = false;
        return Stream.GetData(Buffer);
    }
    switch (State)
    {
    case StateXID:
    {
        SDLCRequestXID XID(Stream);
        State = StateNrm;
        break;
    }
    case StateNrm:
    {
        SDLCSetNormalResponseMode SetNrmRes(Stream);
        State = StateActPU;
        break;
    }
    case StateActPU:
    {
        Stream.SetSequence(0x1568);
        {
            RequestPacket ACTPU(Stream, 3, 0, 0, true);
            ACTPU.SendData({0x11, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A});
        }
        bSendReadyToRecieve = true;
        State = StateActLU;
        break;
    }
    case StateActLU:
    {
        Stream.SetSequence(0x1569);
        {
            RequestPacket ACTLU(Stream, 3, 2, 0, true);
            ACTLU.SendData({0x0D, 0x01, 0x01});
        }
        bSendReadyToRecieve = true;
        State = StateBind;
        break;
    }
    case StateBind:
    {
        Stream.SetSequence(0x0);
        {
            RequestPacket Bind(Stream, 3, 2, 1);
            Bind.SendData({0x31, 0x01, 0x03, 0x03, 0xA1, 0xA1, 0x30, 0x80, 0x00,
                           0x01, 0x85, 0x85, 0x0A, 0x00, 0x02, 0x11, 0x00, 0x00,
                           0xB1, 0x00, 0xC0, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                           0x04, 0xD1, 0xE2, 0xC9, 0xD6, 0x00});
        }
        bSendReadyToRecieve = true;
        State = StateDataReset;
        break;
    }
    case StateDataReset:
    {
        {
            RequestPacket DataReset(Stream, 3, 2, 1);
            DataReset.SendData(0xA0);
        }
        bSendReadyToRecieve = true;
        State = StateSendScreen;
        break;
    }
    case StateSendScreen:
    {
        {
            RequestPacket DataStream3270(Stream, 0, 2, 1);
            DataStream3270.SendData({0xF1, 0xD3, 0x11, 0x5C, 0xF0, 0x1D, 0xF0});
            const char *Message = "Talk to me > ";
            while (*Message)
            {
                DataStream3270.SendData(ASCIIToEBCDIC[(uint8_t)*Message++]);
            }
            DataStream3270.SendData({0x6E, 0x40, 0x1D, 0x40, 0x13, 0x11, 0x5D, 0x7F, 0x1D, 0xF0});
        }
        bSendReadyToRecieve = true;
        State = StateWaitForInput;
        break;
    }
    case StateWaitForInput:
    {
        SDLCReadyToRecieve RR(Stream);
        break;
    }
    case StateRespond:
    {
        {
            // TODO: ERI?
            PositiveResponsePacket Response(Stream, 0, 2, 1, ProcessedPacket);
            Response.SendData({0x7d, 0x5d, 0xc3, 0x11, 0x5d, 0xc2, 0xf4});
        }
        {
            RequestPacket DataReset(Stream, 0, 2, 1, false, false, false, false);
        }
        bSendReadyToRecieve = true;
        State = StateWaitForInput;
        break;
    }
    }
    return Stream.GetData(Buffer);
}

void NetworkState::ProcessPacket(const PacketParser &Packet, const uint8_t *Data)
{
    if (!Packet.bPacketGood)
    {
        return;
    }
    if (Packet.bSDLCValid)
    {
        if (Packet.SDLC.bSendCountValid)
        {
            Stream.SetRecieveCount(Packet.SDLC.RecieveCount);
        }
    }
    if (Packet.bRequestValid && Packet.Req.bChangeDirection)
    {
        if (Packet.FID2.Sequence != ProcessedPacket)
        {
            State = StateRespond;
            if (Packet.bResponseValid || Packet.bRequestValid)
            {
                //Stream.SetSequence(Packet.FID2.Sequence + 1);
                ProcessedPacket = Packet.FID2.Sequence;
            }
        }
    }
}
