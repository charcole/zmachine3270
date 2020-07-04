#include "Network.h"
#include "Screen.h"

NetworkState::NetworkState()
{
}

int NetworkState::GenerateNewPackets(uint8_t *Buffer, bool &bWaitForReply)
{
    Stream.Reset();
    if (bSendReadyToRecieve || WaitForSequence != -1)
    {
        SDLCReadyToRecieve RR(Stream);
        bSendReadyToRecieve = false;
        bWaitForReply = true;
        return Stream.GetData(Buffer);
    }
    bWaitForReply = false;
    switch (State)
    {
    case StateXID:
    {
        SDLCRequestXID XID(Stream);
        State = StateNrm;
        bWaitForReply = true;
        break;
    }
    case StateNrm:
    {
        SDLCSetNormalResponseMode SetNrmRes(Stream);
        State = StateActPU;
        bWaitForReply = true;
        break;
    }
    case StateActPU:
    {
        Stream.SetSequence(0x1568);
        {
            WaitForSequence = Stream.Sequence;
            RequestPacket ACTPU(Stream, 3, 0, 0, true, true, true);
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
            WaitForSequence = Stream.Sequence;
            RequestPacket ACTLU(Stream, 3, 2, 0, true, true, true);
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
            WaitForSequence = Stream.Sequence;
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
            WaitForSequence = Stream.Sequence;
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
            bool bStartOfChain=false;
            bool bEndOfChain=false;
            if (CurrentLine == 0)
            {
                Num3270Packets = GScreen.SerializeScreen3270();
                bStartOfChain=true;
            }
            int PacketSize = 0;
            const char* PacketData = GScreen.GetScreen3270Packet(CurrentLine++, PacketSize);
            if (CurrentLine >= Num3270Packets)
            {
                CurrentLine = 0;
                State = StateWaitForInput;
                bEndOfChain=true;
            }
            WaitForSequence = Stream.Sequence;
            RequestPacket DataStream3270(Stream, 0, 2, 1, bStartOfChain, bEndOfChain);
            while (PacketSize--)
            {
                DataStream3270.SendData(*(PacketData++));
            }
        }
        bSendReadyToRecieve = true;
        break;
    }
    case StateWaitForInput:
    {
        SDLCReadyToRecieve RR(Stream);
        bWaitForReply = true;
        break;
    }
    case StateRespond:
    {
        {
            // TODO: ERI?
            PositiveResponsePacket Response(Stream, 0, 2, 1, ProcessedPacket);
            Response.SendData({0x7d, 0x5d, 0xc3, 0x11, 0x5d, 0xc2, 0xf4});
        }
        State = StateRespond2;
        break;
    }
    case StateRespond2:
    {
        {
            WaitForSequence = Stream.Sequence;
            RequestPacket DataReset(Stream, 0, 2, 1, true, true, false, false, false, false);
        }
        bSendReadyToRecieve = true;
        State = StateSendScreen;
        break;
    }
    }
    return Stream.GetData(Buffer);
}

void NetworkState::ProcessPacket(const PacketParser &Packet, const uint8_t *Data)
{
    if (!Packet.bPacketGood)
    {
        printf("Bad packet: CRC mismatch (State:%d)", State);
        bSendReadyToRecieve = true;
        return;
    }
    if (Packet.bSDLCValid)
    {
        if (Packet.SDLC.bRecieveCountValid)
        {
            // Recieve count should match next send count
            if (Packet.SDLC.RecieveCount != (Stream.SendCount & 7))
            {
                // Not got last message so resend?
                if (bSendReadyToRecieve)
                {
                    // Just sent something. Shouldn't be recieving anything yet.
                    // Hasn't processed what we just sent yet or we're out of sync?
                    return;
                }
                printf("RecvCount (%d) != SendCount (%d) so resending last message (%d:%d:%d)\n", Packet.SDLC.RecieveCount, Stream.SendCount & 7, State, CurrentLine, bSendReadyToRecieve);
                WaitForSequence=-1;
                Stream.SendCount--;
                if (CurrentLine > 0)
                {
                    CurrentLine--;
                }
                else if (State == StateWaitForInput)
                {
                    State = StateSendScreen;
                    CurrentLine = Num3270Packets - 1;
                }
                else if (State != StateRespond)
                {
                    State = (EState)(State - 1);
                }
                return;
            }
        }
        if (Packet.SDLC.bSendCountValid)
        {
            // Recieved a numbered packet so increase recieve count
            Stream.IncreaseRecieveCount();
        }
    }
    if (Packet.bFID2Valid && WaitForSequence != -1)
    {
        if (Packet.FID2.Sequence == WaitForSequence)
        {
            WaitForSequence = -1;
        }
        else
        {
            printf("Recieved out of sequence packet %d != %d (State:%d)\n", Packet.FID2.Sequence, WaitForSequence, State);
        }
    }
    if (Packet.bResponseValid && !Packet.Res.bPositiveResponse)
    {
        printf("Recieved negative response (State:%d)", State);
        Packet.Dump(Data);
    }
    if (Packet.bRequestValid && Packet.Req.bChangeDirection)
    {
        //if (Packet.FID2.Sequence != ProcessedPacket)
        {
            State = StateRespond;
            if (Packet.bResponseValid || Packet.bRequestValid)
            {
                //Stream.SetSequence(Packet.FID2.Sequence + 1);
                ProcessedPacket = Packet.FID2.Sequence;

                GScreen.Process3270Reply(&Data[Packet.StartOfData], Packet.EndOfData - Packet.StartOfData);
            }
        }
    }
}
