#include "Network.h"
#include "Screen.h"

NetworkState::NetworkState()
{
}

int NetworkState::GenerateNewPackets(uint8_t *Buffer, bool &bWaitForReply)
{
    Stream.Reset();
    if (bSendReadyToRecieve)
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
            if (CurrentLine == 0)
            {
                Num3270Packets = GScreen.SerializeScreen3270();
            }
            int PacketSize = 0;
            const char* PacketData = GScreen.GetScreen3270Packet(CurrentLine++, PacketSize);
            if (CurrentLine >= Num3270Packets)
            {
                CurrentLine = 0;
                State = StateWaitForInput;
            }
            RequestPacket DataStream3270(Stream, 0, 2, 1);
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
            RequestPacket DataReset(Stream, 0, 2, 1, false, false, false, false);
        }
        bSendReadyToRecieve = true;
        State = StateDataReset;
        break;
    }
    }
    return Stream.GetData(Buffer);
}

void NetworkState::ProcessPacket(const PacketParser &Packet, const uint8_t *Data)
{
    if (!Packet.bPacketGood)
    {
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
                printf("RecvCount (%d) != SendCount (%d) so resending last message (%d:%d:%d)\n", Packet.SDLC.RecieveCount, Stream.SendCount & 7, State, CurrentLine, bSendReadyToRecieve);
                if (bSendReadyToRecieve)
                {
                    // Just sent something. Shouldn't be recieving anything yet.
                    // Hasn't processed what we just sent yet or we're out of sync?
                    return;
                }
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
    if (Packet.bRequestValid && Packet.Req.bChangeDirection)
    {
        if (Packet.FID2.Sequence != ProcessedPacket)
        {
            State = StateRespond;
            if (Packet.bResponseValid || Packet.bRequestValid)
            {
                //Stream.SetSequence(Packet.FID2.Sequence + 1);
                ProcessedPacket = Packet.FID2.Sequence;

                if (Data[Packet.StartOfData] == 0x7D) // ENTER key
                {
                    char InputString[128];
                    if (Packet.EndOfData - Packet.StartOfData - 6 < sizeof(InputString) - 1)
                    {
                        int DataOffset = Packet.StartOfData + 3; // Skip AID and cursor address
                        if (Data[DataOffset] == 0x11) // Set Buffer Address always seems to follow
                        {
                            DataOffset += 3; // SBA + Address
                            char *Input = InputString;
                            for (int TextIndex = DataOffset; TextIndex < Packet.EndOfData; TextIndex++)
                            {
                                *(Input++) = EBCDICToASCII[Data[TextIndex]];
                            }
                            *(Input++) = '\0';
                            printf("Input: %s\n", InputString);
                            GScreen.ProvideInput(InputString);
                        }
                    }
                }
            }
        }
    }
}
