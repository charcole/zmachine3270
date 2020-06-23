#include <stdio.h>
#include <string.h>
#include "PacketParser.h"
#include "crc.h"

PacketParser::PacketParser()
{
    memset(this, 0, sizeof(*this));
    StartOfData = -1;
}

void PacketParser::Parse(const uint8_t *PacketData, int PacketSize)
{
    bPacketGood = ValidatePacket(PacketData, PacketSize);
    PacketSize -= 2;        // Drop CRC
    
    EndOfData = PacketSize; // Ignore CRC, already checked
    if (PacketSize >= 2)
    {
        bSDLCValid = true;
        SDLC.Address = PacketData[0];
        SDLC.RecieveCount = (PacketData[1] >> 5) & 7;
        SDLC.SendCount = (PacketData[1] >> 1) & 7;
        SDLC.bPollOrFinal = (PacketData[1] >> 4) & 1;
        if ((PacketData[1] & 1) == 0)
        {
            SDLC.Type = InformationPacket;
            SDLC.bRecieveCountValid = SDLC.bSendCountValid = true;

            if (PacketSize > 7)
            {
                if ((PacketData[2] & 0xF0) == 0x20)
                {
                    bFID2Valid = true;
                    FID2.bStartOfBIU = ((PacketData[2] & 0x08) != 0);
                    FID2.bEndOfBIU = ((PacketData[2] & 0x04) != 0);
                    FID2.bODAI = ((PacketData[2] & 0x02) != 0);
                    FID2.bExpidited = ((PacketData[2] & 0x01) != 0);
                    FID2.Destination = PacketData[4];
                    FID2.Origin = PacketData[5];
                    FID2.Sequence = (((int)PacketData[6] << 8) | PacketData[7]);

                    if (PacketSize > 10)
                    {
                        bResponseValid = ((PacketData[8] & 0x80) != 0);
                        bRequestValid = ((PacketData[8] & 0x80) == 0);
                        RU.HeaderType = (ETransmissionHeaderType)(PacketData[8] & 0x60);
                        RU.bHasFMHeader = ((PacketData[8] & 0x08) != 0);
                        RU.ResponseType = (PacketData[9] & 0xA0);
                        RU.bBypassQueues = ((PacketData[9] & 0x02) == 0);
                        RU.bPAC = ((PacketData[9] & 0x01) != 0);
                        if (bRequestValid)
                        {
                            RU.ResponseType |= (PacketData[9] & 0x10);
                            Req.bStartOfChain = ((PacketData[8] & 0x02) != 0);
                            Req.bEndOfChain = ((PacketData[8] & 0x01) != 0);
                            Req.bBeginBracket = ((PacketData[10] & 0x80) != 0);
                            Req.bEndBracket = ((PacketData[10] & 0x40) != 0);
                            Req.bChangeDirection = ((PacketData[10] & 0x20) != 0);
                            Req.bCodeSelection = ((PacketData[10] & 0x08) != 0);
                            Req.bEnciphered = ((PacketData[10] & 0x04) != 0);
                            Req.bPadded = ((PacketData[10] & 0x02) != 0);
                            Req.bConditionalEndBracket = ((PacketData[10] & 0x01) != 0);
                        }
                        else if (bResponseValid)
                        {
                            Res.bPositiveResponse = ((PacketData[9] & 0x10) == 0);
                        }
                        StartOfData = 11;

                        if (PacketData[8] & 0x04)
                        {
                            bSenseValid = true;
                            Sense.Reason = (ESenseDataReason)PacketData[11];
                            Sense.SubReason = PacketData[12];
                            Sense.SpecificInfo = (((int)PacketData[13] << 8) | PacketData[14]);
                            StartOfData = 15;
                        }
                    }
                }
            }
        }
        else
        {
            uint8_t PacketType = PacketData[1];
            StartOfData = 2;
            switch (PacketType & 0xF)
            {
            case RecieveReady:
            case RecieveNotReady:
            case Reject:
                SDLC.Type = (ESDLCPacketType)(PacketType & 0xF);
                SDLC.bRecieveCountValid = true;
                break;
            default:
                switch (PacketType & 0xEF)
                {
                case UnnumberedInfo:
                case RequestInitializationMode:
                case DisconnectMode:
                case RequestDisconnect:
                case UnnumberedACK:
                case FrameReject:
                case XID:
                case Configure:
                case Test:
                case Beacon:
                    SDLC.Type = (ESDLCPacketType)(PacketType & 0xEF);
                    break;
                default:
                    SDLC.Type = Unknown;
                    break;
                }
                break;
            }
        }
    }
}

void PacketParser::Dump(const uint8_t *Data)
{
    if (!bPacketGood)
    {
        printf("Bad packet. CRC mismatch\n");
        return;
    }
    if (bSDLCValid)
    {
        printf("Packet for/to %02x\n", SDLC.Address);
        switch (SDLC.Type)
        {
        case InformationPacket:
            printf("Information");
            break;
        case RecieveReady:
            printf("Recieve Ready");
            break;
        case RecieveNotReady:
            printf("Recieve Not Ready");
            break;
        case Reject:
            printf("Reject");
            break;
        case UnnumberedInfo:
            printf("Unnumbered Info");
            break;
        case RequestInitializationMode:
            printf("Request Initialization Mode");
            break;
        case DisconnectMode:
            printf("Disconnect Mode");
            break;
        case RequestDisconnect:
            printf("Request Disconnect");
            break;
        case UnnumberedACK:
            printf("Unnumbered ACK");
            break;
        case FrameReject:
            printf("Frame Reject");
            break;
        case XID:
            printf("XID");
            break;
        case Configure:
            printf("Configure");
            break;
        case Test:
            printf("Test");
            break;
        case Beacon:
            printf("Beacon");
            break;
        default:
            printf("Unknown");
            break;
        }
        if (SDLC.bRecieveCountValid)
        {
            printf(" Nr=%d", SDLC.RecieveCount);
        }
        if (SDLC.bSendCountValid)
        {
            printf(" Ns=%d", SDLC.SendCount);
        }
        printf(" PollOrFinal=%d\n", SDLC.bPollOrFinal);
    }

    if (bFID2Valid)
    {
        printf(" FID2\n");
        printf(" Destination: %02x\n", FID2.Destination);
        printf(" Origin: %02x\n", FID2.Origin);
        printf(" Sequence: %04x\n", FID2.Sequence);
        if (FID2.bStartOfBIU)
        {
            printf("  Start of BIU\n");
        }
        if (FID2.bEndOfBIU)
        {
            printf("  End of BIU\n");
        }
        if (FID2.bODAI)
        {
            printf("  ODAI?\n");
        }
        if (FID2.bExpidited)
        {
            printf("  Expedited\n");
        }

        if (bRequestValid || bResponseValid)
        {
            printf("  %s\n", bRequestValid ? "Request" : "Response");
            switch (RU.HeaderType)
            {
            case FMData:
                printf("   FM data\n");
                break;
            case NetworkHeader:
                printf("   Network header\n");
                break;
            case DataFlowControl:
                printf("   Data flow control\n");
                break;
            case SessionControl:
                printf("   Session control\n");
                break;
            }
            if (RU.bHasFMHeader)
            {
                printf("   FM header follows\n");
            }
            printf("   Response type: %02x\n", RU.ResponseType);
            printf("   %s TC queues\n", RU.bBypassQueues ? "Bypass" : "Enqueue in");
            printf("   %sPAC\n", RU.bPAC ? "" : "!");
            if (bRequestValid)
            {
                if (Req.bStartOfChain)
                {
                    printf("   Start of chain\n");
                }
                if (Req.bEndOfChain)
                {
                    printf("   End of chain\n");
                }
                if (Req.bBeginBracket)
                {
                    printf("   Begin Bracket\n");
                }
                if (Req.bEndBracket)
                {
                    printf("   End Bracket\n");
                }
                if (Req.bChangeDirection)
                {
                    printf("   Change Direction\n");
                }
                printf("   Code Selection: %d\n", Req.bCodeSelection ? 1 : 0);
                if (Req.bEnciphered)
                {
                    printf("   RU is enciphered\n");
                }
                if (Req.bPadded)
                {
                    printf("   RU is padded\n");
                }
                if (Req.bConditionalEndBracket)
                {
                    printf("   Conditional End Bracket\n");
                }
            }
            else if (bResponseValid)
            {
                printf("   %s response\n", Res.bPositiveResponse ? "Positive" : "Negative");
            }
        }
    }

    if (bSenseValid)
    {
        switch (Sense.Reason)
        {
        case UserSenseDataOnly:
            printf("    User Sense Data Only\n");
            break;
        case RequestReject:
            printf("    Request Reject\n");
            switch (Sense.SubReason)
            {
            case ResourceNotAvailable:
                printf("     Resource Not Available\n");
                break;
            case InterventionRequired:
                printf("     Intervention Required\n");
                break;
            case MissingPassword:
                printf("     Missing Password\n");
                break;
            case InvalidPassword:
                printf("     Invalid Password\n");
                break;
            case SessionLimitExceeded:
                printf("     Session Limit Exceeded\n");
                break;
            case ResourceUnknown:
                printf("     Resource Unknown\n");
                break;
            case ResourceNotAvailable2:
                printf("     Resource Not Available\n");
                break;
            case InvalidContentsID:
                printf("     Invalid Contents ID\n");
                break;
            case ModeInconsistency:
                printf("     Mode Inconsistency\n");
                break;
            case PermissionRejected:
                printf("     Permission Rejected\n");
                break;
            case BracketRaceError:
                printf("     Bracket Race Error\n");
                break;
            case ProcedureNotSupported:
                printf("     Procedure Not Supported\n");
                break;
            case NAUContention:
                printf("     NAU Contention\n");
                break;
            case NAUNotAuthorized:
                printf("     NAU Not Authorized\n");
                break;
            case EndUserNotAuthorized:
                printf("     End User Not Authorized\n");
                break;
            case MissingRequesterID:
                printf("     Missing Requester ID\n");
                break;
            case Break:
                printf("     Break\n");
                break;
            case UnsufficientResource:
                printf("     Unsufficient Resource\n");
                break;
            case BracketBidReject:
                printf("     Bracket Bid Reject\n");
                break;
            case BracketBidReject2:
                printf("     Bracket Bid Reject\n");
                break;
            case FunctionActive:
                printf("     Function Active\n");
                break;
            case FunctionActive2:
                printf("     Function Active\n");
                break;
            case LinkOrLinkResourceInactive:
                printf("     Link or Link Resource Inactive\n");
                break;
            case LinkProcedureinProcess:
                printf("     Link Procedure in Process\n");
                break;
            case RTRNotRequired:
                printf("     RTR Not Required\n");
                break;
            case RequestSequenceError:
                printf("     Request Sequence Error\n");
                break;
            default:
                printf("     Error code: %02x\n", Sense.SubReason);
            }
            break;
        case RequestError:
            printf("    Request Error\n");
            break;
        case StateError:
            printf("    State Error\n");
            break;
        case ReqHdrUsageError:
            printf("    Request Header Usage Error\n");
            break;
        case PathError:
            printf("    Path Error\n");
            break;
        }
        printf("     Specific Info: %04x\n", Sense.SpecificInfo);
    }

    if (StartOfData >= 0 && StartOfData < EndOfData)
    {
        printf("    Data:");
        for (int DataIndex = StartOfData; DataIndex < EndOfData; DataIndex++)
        {
            printf(" %02x", Data[DataIndex]);
        }
        printf("\n");
    }
}
