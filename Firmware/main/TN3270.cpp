#include "TN3270.h"
#include "Screen.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include <stdlib.h>
#include <stdio.h>

// Can be run with MVS Turnkey 4 but is quite janky.
// Turnkey 4 expects newer terminal so sends invalid fields
// PF24 is used to recieve more input if available
// So on splash screen press PF24 to get to login
// Try login
// Will show error message
// PA1 will get you to command prompt
// logon HERC03
// Menu will be a bit jank
// Erase input
// 1
// RFE seems to work fine from here
// PF7/PF8 scroll

enum
{
    EOR = 0xEF,
    SE = 0xF0,
    SB = 0xFA,
    WILL = 0xFB,
    WONT = 0xFC,
    DO = 0xFD,
    DONT = 0xFE,
    IAC = 0xFF,
};

enum
{
    TRANSMIT_BINARY = 0x00,
    TERMINAL_TYPE = 0x18,
    END_OF_RECORD = 0x19,
};

enum
{
    TERMINAL_TYPE_IS = 0x00,
    TERMINAL_TYPE_SEND = 0x01,
};

void TN3270::Run()
{
    TN3270 Terminal;
    printf("Attempting to connect via TN3270\n");
    if (Terminal.Connect())
    {
        Terminal.Process();
        Terminal.Disconnect();
    }
}

bool TN3270::Connect()
{
    SocketFD = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (SocketFD == -1)
    {
        printf("Failed to create socket\n");
        return false;
    }

    struct sockaddr_in DestAddr;
    DestAddr.sin_addr.s_addr = inet_addr("192.168.1.11");
    DestAddr.sin_family = AF_INET;
    DestAddr.sin_port = htons(3270);

    if (connect(SocketFD, (struct sockaddr *)&DestAddr, sizeof(DestAddr)) == -1)
    {
        close(SocketFD);
        printf("Failed to connect socket\n");
        return false;
    }

    printf("Connected\n");

    return true;
}

void TN3270::Disconnect()
{
    shutdown(SocketFD, SHUT_RDWR);
    close(SocketFD);

    printf("Disconnected\n");
}

void TN3270::Process()
{
    while (!bError)
    {
        uint8_t Char = RecvByte();
        if (Char == IAC)
        {
            uint8_t Verb = RecvByte();
            if (Verb != IAC)
            {
                if (Verb == DO)
                {
                    uint8_t Option = RecvByte();
                    SendByte(IAC);
                    switch (Option)
                    {
                    case TRANSMIT_BINARY:
                    case TERMINAL_TYPE:
                    case END_OF_RECORD:
                        SendByte(WILL);
                        break;
                    default:
                        printf("Unknown option %d\n", Option);
                        SendByte(WONT);
                        break;
                    }
                    SendByte(Option);
                    SendFlush();
                }
                else if (Verb == WILL)
                {
                    uint8_t Option = RecvByte();
                    SendByte(IAC);
                    switch (Option)
                    {
                    case TRANSMIT_BINARY:
                    case END_OF_RECORD:
                        SendByte(DO);
                        break;
                    default:
                        printf("Unknown option %d\n", Option);
                        SendByte(DONT);
                        break;
                    }
                    SendByte(Option);
                    SendFlush();
                }
                else if (Verb == SB)
                {
                    uint8_t Option = RecvByte();
                    switch (Option)
                    {
                    case TERMINAL_TYPE:
                    {
                        uint8_t SubOption = RecvByte();
                        if (SubOption == TERMINAL_TYPE_SEND)
                        {
                            SendByte(IAC);
                            SendByte(SB);
                            SendByte(TERMINAL_TYPE);
                            SendByte(TERMINAL_TYPE_IS);
                            SendByte('I');
                            SendByte('B');
                            SendByte('M');
                            SendByte('-');
                            SendByte('3');
                            SendByte('2');
                            SendByte('7');
                            SendByte('8');
                            SendByte('-');
                            SendByte('2');
                            SendByte(IAC);
                            SendByte(SE);
                            SendFlush();
                        }
                        else
                        {
                            printf("Unknown terminal type suboption %d\n", SubOption);
                        }
                        break;
                    }
                    default:
                        printf("Unknown option (SB) %d\n", Option);
                        break;
                    }
                }
                else if (Verb == EOR)
                {
                    printf("Providing some data for the screen\n");
                    if (DataStreamPtr > DataStream && DataStreamPtr < DataStream + sizeof(DataStream))
                    {
                        GScreen.SetRawStream((const char *)DataStream, DataStreamPtr - DataStream);
                    }
                    DataStreamPtr = DataStream;

                    bool bHasCancelledInput = false;
                    while (true)
                    {
                        int InputLength = GScreen.ReadInput((char *)DataStream, sizeof(DataStream), true, 20);
                        if (InputLength > 0) // Allow PF24 to be ignored
                        {
                            printf("Got some input\n");
                            for (int InputIndex = 0; InputIndex < InputLength; InputIndex++)
                            {
                                if (DataStream[InputIndex] == IAC)
                                {
                                    SendByte(IAC); // Escape 0xFF
                                }
                                SendByte(DataStream[InputIndex]);
                            }
                            SendByte(IAC);
                            SendByte(EOR);
                            SendFlush();
                            break;
                        }
                        else if (InputLength == 0)
                        {
                            printf("Has cancelled the input\n");
                            break;
                        }
                        else if (InputLength == -1 && !bHasCancelledInput) // Timeout, check if we have any more data to recv
                        {
                            fd_set RecvFdSet;
                            FD_ZERO(&RecvFdSet);
                            FD_SET(SocketFD, &RecvFdSet);
                            timeval TimeVal;
                            TimeVal.tv_sec=0;
                            TimeVal.tv_usec=0;
                            int SelectReturn = select(SocketFD + 1, &RecvFdSet, nullptr, nullptr, &TimeVal);
                            if (SelectReturn > 0 && FD_ISSET(SocketFD, &RecvFdSet)) // More data to read
                            {
                                printf("Cancelling input as we have some more data pending\n");
                                GScreen.CancelInput();
                                bHasCancelledInput = true;
                            }
                        }
                    }
                }
                continue;
            }
        }
        if (DataStreamPtr < DataStream + sizeof(DataStream))
        {
            *(DataStreamPtr++) = Char;
        }
    }
    GScreen.SetRawStream(nullptr, 0);
}

uint8_t TN3270::RecvByte()
{
    if (RecvPtr >= RecvEnd)
    {
        RecvPtr = 0;
        RecvEnd = recv(SocketFD, RX, sizeof(RX), 0);
        bError |= (RecvEnd < 0);
    }
    return RX[RecvPtr++];
}

void TN3270::SendFlush()
{
    if (SendPtr > 0)
    {
        bError |= (send(SocketFD, TX, SendPtr, 0) < 0);
        SendPtr = 0;
    }
}

void TN3270::SendByte(uint8_t Byte)
{
    TX[SendPtr++] = Byte;
    if (SendPtr >= sizeof(TX))
    {
        SendFlush();
    }
}
