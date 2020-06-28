#ifdef __cplusplus
extern "C"
{
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

struct Screen
{
public:
    enum
    {
        NUM_ROWS = 24,
        NUM_COLS = 80
    };

    Screen();
    void Print(const char* String);
    void Print(char Char);
    void ProvideInput(const char* Input);
    void ReadInput(char *Input, int MaxLength);
    void GetCursorPosition(int& X, int& Y);
    void SetCursorPosition(int X, int Y);
    
    int SerializeScreen3270();
    const char* GetScreen3270Packet(int PacketNum, int& PacketSize)
    {
        PacketSize = Packets[PacketNum].Length;
        return Packets[PacketNum].Start;
    }

private:
    void AddScreenAddress(char* &Data, int Col, int Row);
    void WriteMultiple(char* &Data, int& Col, int Row, char EBDIC, int Run);
    void WriteScreenData(char* &Data, int Col, int Row, const char* ASCIIData, int NumData);
    void ConditionalScroll();

    struct FLine
    {
        char Col[NUM_COLS];
    };
    FLine Row[NUM_ROWS];

    struct FPacket
    {
        char* Start;
        int Length;
    };
    FPacket Packets[NUM_ROWS];

    char Serialized3270Data[NUM_ROWS * (NUM_COLS + 16)]; // Space for header + cursor setting

    int TopLine = 0;
    int CursorRow = 0;
    int CursorCol = 0;
    bool bWordwrap = false;

    volatile char* WaitingInput = nullptr;
    volatile TaskHandle_t TaskHandle;
    volatile bool bSuspended = false;
};

extern Screen GScreen;

extern "C"
{
#endif

void ScreenPrint(const char* String);
void ScreenPrintChar(char Char);
void ScreenReadInput(char* Input, int MaxLength);
void ScreenGetCursor(int* CursorX, int* CursorY);
void ScreenSetCursor(int CursorX, int CursorY);

#ifdef __cplusplus
}
#endif