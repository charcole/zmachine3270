#include "Wikipedia.h"
#include "Screen.h"

#include <stdio.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_log.h"

#define TAG "Wiki"

class CSimplisticWikiParser
{
public:
    CSimplisticWikiParser()
    {
        Reset();
        InString = false;
        bEscape = false;
    }

    void Reset()
    {
        OutReset();
        CloseCheck = false;
        SelfCloseCheck = false;
        bIsComment = false;
        NumNewLines = 2;
        InAngledBrackets = 0;
        InCurlyBrackets = 0;
        InSquareBrackets = 0;
        TagDepth = 0;
    }

    void ProcessChunk(const char *String, int Length)
    {
        const char* EndString = String + Length;
        while (String < EndString)
        {
            if (!bEscape && *String == '"')
            {
                InString = !InString;
                if (InString)
                {
                    Reset();
                }
                String++;
                continue;
            }
            if (!InString)
            {
                String++;
                continue;
            }
            switch (*String)
            {
            case '\\':
                bEscape = true;
                break;
            case '<':
                InAngledBrackets++;
                CloseCheck = true;
                TagDepth++;
                break;
            case '>':
                InAngledBrackets--;
                if (SelfCloseCheck || bIsComment)
                    TagDepth--;
                bIsComment = false;
                break;
            case '/':
                if (InAngledBrackets == 1)
                {
                    if (CloseCheck)
                        TagDepth -= 2;
                    else
                        SelfCloseCheck = true;
                }
                else
                {
                    ProcessChar('/');
                }
                break;
            case '!':
                if (InAngledBrackets == 1 && CloseCheck)
                {
                    bIsComment = true;
                }
                else
                {
                    ProcessChar('!');
                }
                break;
            case '{':
                InCurlyBrackets++;
                break;
            case '}':
                InCurlyBrackets--;
                break;
            case '[':
                InSquareBrackets++;
                OutStorePos();
                break;
            case ']':
                InSquareBrackets--;
                break;
            case '\'':
                break;
            case '|':
                if (InSquareBrackets)
                    OutRestorePos();
                break;
            default:
                ProcessChar(*String);
                break;
            }
            String++;
        }
    }

    const char *GetOutput()
    {
        OutWrite('\0');
        OutBuffer[sizeof(OutBuffer) - 1] = '\0';
        return OutBuffer;
    }

private:
    void ProcessChar(char Char)
    {
        if (!TagDepth && !InAngledBrackets && !InCurlyBrackets)
        {
            if (bEscape && Char == 'n')
            {
                if (NumNewLines < 2)
                {
                    OutWrite('\n');
                    NumNewLines++;
                }
            }
            else
            {
                OutWrite(Char);
                NumNewLines = 0;
            }
        }
        bEscape = false;
        CloseCheck = false;
        SelfCloseCheck = false;
    }

    void OutWrite(char OutChar)
    {
        if (Write < OutBuffer + sizeof(OutBuffer))
        {
            *(Write++) = OutChar;
        }
    }

    void OutReset()
    {
        Write = OutBuffer;
    }

    void OutStorePos()
    {
        StartWrite = Write;
    }

    void OutRestorePos()
    {
        Write = StartWrite;
    }

    bool CloseCheck = false;
    bool SelfCloseCheck = false;
    bool bIsComment = false;
    bool InString = false;
    bool bEscape = false;
    int NumNewLines = 2;
    int InAngledBrackets = 0;
    int InCurlyBrackets = 0;
    int InSquareBrackets = 0;
    int TagDepth = 0;
    char OutBuffer[80 * 23];
    char *Write = nullptr;
    char *StartWrite = nullptr;
};

static esp_err_t
_http_event_handle(esp_http_client_event_t *evt)
{
    CSimplisticWikiParser* Parser = (CSimplisticWikiParser*)evt->user_data;
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER");
            printf("%.*s", evt->data_len, (char*)evt->data);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            Parser->ProcessChunk((const char*)evt->data, evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}

void Wikipedia::Run()
{
    CSimplisticWikiParser WikiParser;
    char SearchTerm[80];
    char SearchURL[128 + sizeof(SearchTerm)];
    strcpy(SearchTerm, "IBM 3270");

    while (SearchTerm[0] && strcasecmp(SearchTerm, "exit") != 0)
    {
        bool bSuccess = true;

        char* Term = SearchTerm;
        while (*Term)
        {
            if (*Term == ' ')
            {
                *Term = '_';
            }
            Term++;
        }

        esp_http_client_config_t RequestConfig = {};
        sprintf(SearchURL, "https://en.wikipedia.org/w/api.php?action=query&prop=revisions&rvprop=content&rvsection=0&titles=%s&format=json", SearchTerm);
        ESP_LOGI(TAG, "URL = %s", SearchURL);
        RequestConfig.url = SearchURL;
        RequestConfig.event_handler = _http_event_handle;
        RequestConfig.user_data = &WikiParser;

        esp_http_client_handle_t Client = esp_http_client_init(&RequestConfig);

        int Row = 1;
        GScreen.Clear();
        GScreen.SetCursorPosition(0, Row);

        if (esp_http_client_perform(Client) == ESP_OK)
        {
            ESP_LOGI(TAG, "Status = %d, content_length = %d",
                     esp_http_client_get_status_code(Client),
                     esp_http_client_get_content_length(Client));

            if (esp_http_client_get_status_code(Client) == 200)
            {
                const char *Output = WikiParser.GetOutput();
                if (Output[0])
                {
                    while (*Output && Row != 23)
                    {
                        GScreen.Print(*(Output++));
                        int Column = 0;
                        GScreen.GetCursorPosition(Column, Row);
                    }
                    bSuccess = true;
                }
            }

            if (!bSuccess)
            {
                GScreen.Print("An error occurred\n");
            }
            GScreen.SetCursorPosition(1, 0);
            GScreen.Print("                               Wikipedia  Browser");
            GScreen.SetCursorPosition(0, 23);
            GScreen.Print("SEARCH:");
            GScreen.ReadInput(SearchTerm, sizeof(SearchTerm));
        }

        esp_http_client_cleanup(Client);
    }
}
