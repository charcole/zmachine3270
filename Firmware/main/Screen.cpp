#include "Screen.h"
#include "3270.h"
#include <string.h>

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
            ConditionalScroll();
        }
        return;
    }
    Row[CursorRow].Col[CursorCol++] = Char;
    if (CursorCol >= NUM_COLS)
    {
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
    Print(Input);
    Print('\n');
}

void Screen::SerializeScreen3270(char *Data)
{
    while (!bSuspended)
    {
        vTaskDelay(5);
    }
    int CurrentLine = TopLine;
    for (int Line = 0; Line < NUM_ROWS; Line++)
    {
        for (int Col = 0; Col < NUM_COLS; Col++)
        {
            char Char = Row[CurrentLine].Col[Col];
            *(Data++) = ASCIIToEBCDIC[Char & 0xFF];
        }
        CurrentLine++;
        if (CurrentLine >= NUM_ROWS)
        {
            CurrentLine = 0;
        }
    }
}

void Screen::GetCursorPosition(int& X, int& Y)
{
    X = CursorCol;
    Y = (CursorRow - TopLine);
    if (Y<0)
    {
        Y+=NUM_ROWS;
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