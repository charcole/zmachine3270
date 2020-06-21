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
    void SerializeScreen3270(char* Data);
    void GetCursorPosition(int& X, int& Y);

private:
    void ConditionalScroll();

    struct FLine
    {
        char Col[NUM_COLS];
    };
    FLine Row[NUM_ROWS];

    int TopLine = 0;
    int CursorRow = 0;
    int CursorCol = 0;

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

#ifdef __cplusplus
}
#endif