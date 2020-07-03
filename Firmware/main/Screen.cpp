#include "Screen.h"
#include "3270.h"
#include <string.h>

enum
{
    WriteCommand = 0xF1,
    WCC = 0xC3,

    SetBufferAddress = 0x11,
    SetCursor = 0x13,
    RepeatToAddress = 0x3C,

    StartField = 0x1D,
    ProtectedField = 0xF0,
    EditableField = 0x40,

    SetAttribute = 0x28,               // Not supported
    FieldAttribute3270 = 0xC0,         // Not supported
    ResetCharacterAttributes = 0x00,   // Not supported
    SetAttributeExtended = 0x29,       // Not supported
    ExtendedHighlightAttribute = 0x41, // Not supported
    HighlightReverseVideo = 0xF2,      // Not supported

    NUL = 0x00
};

Screen GScreen;

Screen::Screen()
{
    for (int Line = 0; Line < NUM_ROWS; Line++)
    {
        memset(&Row[Line], ' ', sizeof(Row[0]));
    }
}

void Screen::Print(const char *String)
{
    while (String[0])
    {
        Print(String[0]);
        String++;
    }
}

void Screen::Print(char Char)
{
    if (Char < 0x20 || Char >= 0x7F) // Control codes
    {
        if (Char == '\n')
        {
            bWordwrap = false;
            ConditionalScroll();
        }
        return;
    }
    if (bWordwrap)
    {
        bWordwrap = false;
        if (Char == ' ')
        {
            return;
        }
        else
        {
            int LastRow = CursorRow - 1;
            if (LastRow < 0)
            {
                LastRow += NUM_ROWS;
            }
            int LastCol = NUM_COLS - 1;
            int MaxLookback = NUM_COLS / 2;
            while (LastCol > MaxLookback && Row[LastRow].Col[LastCol] != ' ')
            {
                LastCol--;
            }
            if (LastCol > MaxLookback)
            {
                LastCol++;
                while (LastCol < NUM_COLS)
                {
                    Row[CursorRow].Col[CursorCol++] = Row[LastRow].Col[LastCol];
                    Row[LastRow].Col[LastCol++] = ' ';
                }
            }
            else
            {
                int LastCol = NUM_COLS - 1;
                Row[CursorRow].Col[CursorCol++] = Row[LastRow].Col[LastCol];
                Row[LastRow].Col[LastCol] = '-';
            }
        }
    }
    Row[CursorRow].Col[CursorCol++] = Char;
    if (CursorCol >= NUM_COLS)
    {
        bWordwrap = (Char != ' ');
        ConditionalScroll();
    }
}

void Screen::ProvideInput(const char* Input)
{
    strcpy((char*)WaitingInput, Input);
    bSuspended = false;
    xTaskNotifyGive(TaskHandle);
}

void Screen::ReadInput(char* Input, int MaxLength)
{
    WaitingInput = Input;
    TaskHandle = xTaskGetCurrentTaskHandle();
    bSuspended = true;
    ulTaskNotifyTake( pdTRUE, portMAX_DELAY );
    Print(' ');
    Print(Input);
    Print('\n');
    Print('\n');
}

void Screen::AddScreenAddress(char* &Data, int Col, int Row)
{
    uint16_t ScreenAddress = Row * NUM_COLS + Col;
    if (ScreenAddress >= NUM_COLS * NUM_ROWS)
    {
        ScreenAddress -= NUM_COLS * NUM_ROWS;
    }
    ScreenAddress = ((ScreenAddress & 0x0FC0) << 2) | (ScreenAddress & 0x3F);
    ScreenAddress |= 0x4040;
    *(Data++) = ((ScreenAddress >> 8) & 0xFF);
    *(Data++) = (ScreenAddress & 0xFF);
}

void Screen::WriteMultiple(char* &Data, int& Col, int Row, char EBDIC, int Run)
{
    Col += Run;
    if (Run <= 4)
    {
        for (int i = 0; i < Run; i++)
        {
            *(Data++) = EBDIC;
        }
    }
    else
    {
        *(Data++) = RepeatToAddress;
        AddScreenAddress(Data, Col, Row);
        *(Data++) = EBDIC;
    }
}

void Screen::WriteScreenData(char* &Data, int Col, int Row, const char* ASCIIData, int NumData)
{
    int Run = 0;
    char LastChar = 0;
    while (NumData--)
    {
       char EBDIC = ASCIIToEBCDIC[(uint8_t)(*(ASCIIData++))];
       if (EBDIC != LastChar)
       {
           WriteMultiple(Data, Col, Row, LastChar, Run);
           Run = 0;
       }
       LastChar = EBDIC;
       Run++;
    }
    WriteMultiple(Data, Col, Row, LastChar, Run);
}

int Screen::SerializeScreen3270()
{
    while (!bSuspended)
    {
        vTaskDelay(5);
    }

    int NumPackets = 0;
    char* Data = Serialized3270Data;
    
    Packets[NumPackets].Start = Data;

    *(Data++) = WriteCommand;
    *(Data++) = WCC;
    *(Data++) = SetBufferAddress;
    AddScreenAddress(Data, 0, 0);

    constexpr int MaxPacketSize = 240;
    char* EndOfPacket = Serialized3270Data + MaxPacketSize;

    int CurrentLine = TopLine;
    for (int Line = 0; Line < NUM_ROWS; Line++)
    {
        char* BeforeLine = Data;
        if (Line == 0)
        {
            *(Data++) = StartField;
            *(Data++) = ProtectedField + 8;
            WriteScreenData(Data, 1, Line, &Row[CurrentLine].Col[1], sizeof(Row[CurrentLine].Col) - 2);
            *(Data++) = StartField;
            *(Data++) = ProtectedField;
        }
        else
        {
            WriteScreenData(Data, 0, Line, Row[CurrentLine].Col, sizeof(Row[CurrentLine].Col));

            if (CurrentLine == CursorRow)
            {
                *(Data++) = SetBufferAddress;
                AddScreenAddress(Data, CursorCol, Line);
                *(Data++) = StartField;
                *(Data++) = EditableField;
                *(Data++) = SetCursor;
                *(Data++) = RepeatToAddress;
                AddScreenAddress(Data, NUM_COLS - 1, Line);
                *(Data++) = NUL;
                *(Data++) = StartField;
                *(Data++) = ProtectedField;
            }
        }
        if (Data > EndOfPacket)
        {
            Packets[NumPackets].Length = BeforeLine - Packets[NumPackets].Start;
            NumPackets++;
            Packets[NumPackets].Start = BeforeLine;
            EndOfPacket = BeforeLine + MaxPacketSize;
        }
        CurrentLine++;
        if (CurrentLine >= NUM_ROWS)
        {
            CurrentLine = 0;
        }
    }
    Packets[NumPackets].Length = Data - Packets[NumPackets].Start;
    NumPackets++;
    return NumPackets;
}

void Screen::GetCursorPosition(int& X, int& Y)
{
    X = CursorCol;
    Y = CursorRow - TopLine;
    if (Y < 0)
    {
        Y += NUM_ROWS;
    }
}

void Screen::SetCursorPosition(int X, int Y)
{
    bWordwrap = false;
    CursorCol = X;
    CursorRow = Y + TopLine;
    if (CursorRow >= NUM_ROWS)
    {
        CursorRow -= NUM_ROWS;
    }
}

void Screen::ConditionalScroll()
{
    CursorCol = 0;
    CursorRow++;
    if (CursorRow >= NUM_ROWS)
    {
        CursorRow = 0;
    }
    memset(&Row[CursorRow], ' ', sizeof(Row[0]));
    if (CursorRow == TopLine)
    {
        TopLine++;
        if (TopLine >= NUM_ROWS)
        {
            TopLine = 0;
        }
    }
}

void ScreenPrint(const char* String)
{
    GScreen.Print(String);
}

void ScreenPrintChar(char Char)
{
    GScreen.Print(Char);
}

void ScreenReadInput(char* Input, int MaxLength)
{
    GScreen.ReadInput(Input, MaxLength);
}

void ScreenGetCursor(int* CursorX, int* CursorY)
{
    GScreen.GetCursorPosition(*CursorX, *CursorY);
}

void ScreenSetCursor(int CursorX, int CursorY)
{
    GScreen.SetCursorPosition(CursorX, CursorY);
}