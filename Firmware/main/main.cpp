// (c) Charlie Cole 2018

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <algorithm>
extern "C"
{
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/rmt.h"
#include "driver/spi_slave.h"
#include "driver/timer.h"
#include "driver/ledc.h"
#include "nvs_flash.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
};
#include "webpage.h"

#define ARRAY_NUM(x) (sizeof(x)/sizeof(x[0]))

#define EXAMPLE_ESP_WIFI_SSID      "TALKTALK4153C8_24Ghz"
#define EXAMPLE_ESP_WIFI_PASS      "EX4T6H46"

#define STORAGE_NAMESPACE "settingsns"

#define OUT_SDLCDATA  			(GPIO_NUM_13)
#define OUT_SDLCCLOCK			(GPIO_NUM_12)
#define IN_SDLCCLOCK			(GPIO_NUM_14)
#define IN_SDLCREADY			(GPIO_NUM_27)
#define IN_SDLCRECV				(GPIO_NUM_26)
#define IN_SDLCRECV2			(GPIO_NUM_25)

static const char *LogTag = "informer";

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t wifi_event_group;

struct PacketToSend
{
	uint32_t NumBits;
	const uint8_t* Data;
};

static xQueueHandle SendEventQueue = nullptr;
static TaskHandle_t RecvTask = nullptr;

struct Configuration
{
	int32_t Dummy;
};

Configuration Config;
		
volatile bool bQuitTasks = false;
volatile bool bDoneQuitTask1 = false;
volatile bool bDoneQuitTask2 = false;

static void ClearSettings()
{
	Config =
	{
		0
	};
}

static esp_err_t LoadSettings(void)
{
	nvs_handle my_handle;
	esp_err_t err;
				
	err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &my_handle);
	printf("Opening namespace %d\n", err);
	if (err == ESP_OK)
	{
		size_t required_size = 0;
		err = nvs_get_blob(my_handle, "settings", NULL, &required_size);
		printf("Get blob %d %d\n", err, required_size);
		
		if (err == ESP_OK && required_size == sizeof(Config))
		{
			err = nvs_get_blob(my_handle, "settings", &Config, &required_size);
			printf("Get blob(2) %d %d\n", err, required_size);
		}

		nvs_close(my_handle);
	}

	return err;
}

static esp_err_t SaveSettings(void)
{
	nvs_handle my_handle;
	esp_err_t err;

	err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
	printf("Opening namespace %d\n", err);
	if (err == ESP_OK)
	{
		err = nvs_set_blob(my_handle, "settings", &Config, sizeof(Config));
		printf("Set blob %d\n", err);

		if (err == ESP_OK)
		{
			err = nvs_commit(my_handle);
			printf("Commit %d\n", err);
		}

		nvs_close(my_handle);
	}

	return err;
}

class CBitStream
{
	public:
		CBitStream()
		{
			Reset();
		}

		CBitStream(const uint8_t* Data, int NumBytes)
		{
			memcpy(Bytes, Data, NumBytes);
			TotalBits = 8 * NumBytes;
			CurrentByte = NumBytes;
			CurrentMask = 1;
		}

		void Reset()
		{
			TotalBits = 0;
			CurrentByte = 0;
			CurrentMask = 1;
			Bytes[CurrentByte] = 0;
		}

		void Flush()
		{
			// Ready for reading
			CurrentByte = 0;
			CurrentMask = 1;
		}

		bool Finished() const
		{
			return TotalBits == 0;
		}

		bool ReadBit()
		{
			bool bBit = ((Bytes[CurrentByte] & CurrentMask) != 0);
			TotalBits--;
			if (CurrentMask == 0x80)
			{
				CurrentMask = 1;
				CurrentByte++;
			}
			else
			{
				CurrentMask <<= 1;
			}
			return bBit;
		}

		void WriteBit(bool bBit)
		{
			if (bBit)
			{
				Bytes[CurrentByte] |= CurrentMask;
			}
			TotalBits++;
			if (CurrentMask == 0x80)
			{
				CurrentMask = 1;
				CurrentByte++;
				Bytes[CurrentByte] = 0;
			}
			else
			{
				CurrentMask <<= 1;
			}
		}

		const uint8_t* ToBytes(int& NumBits)
		{
			NumBits = TotalBits;
			return Bytes;
		}

	private:
		enum
		{
			MAX_MESSAGE_LENGTH = 64
		};

		int32_t TotalBits;
		int32_t CurrentByte;
		uint8_t CurrentMask;
		uint8_t Bytes[MAX_MESSAGE_LENGTH];
};

void CalculateCRC(CBitStream& Dest, CBitStream& Source)
{
	uint16_t CRC = 0xFFFF;

	Dest.Reset();
	while (!Source.Finished())
	{
		uint16_t bData = (Source.ReadBit() ? 1: 0);
		uint16_t bLastBit = (CRC >> 15);
		uint16_t bDataBit = (bData ^ bLastBit);
		CRC = bDataBit + (CRC << 1);
		if (bDataBit)
		{
			CRC ^= (1<<5) + (1<<12);
		};
		Dest.WriteBit(bData);
	}
	for (int BitIndex = 0; BitIndex < 16; BitIndex++)
	{
		Dest.WriteBit(!(CRC >> 15));
		CRC <<= 1;
	}
	Dest.Flush();
}

void InsertFlags(CBitStream& Dest)
{
	Dest.WriteBit(0);
	Dest.WriteBit(1);
	Dest.WriteBit(1);
	Dest.WriteBit(1);
	Dest.WriteBit(1);
	Dest.WriteBit(1);
	Dest.WriteBit(1);
	Dest.WriteBit(0);
}

void ZeroBitAndFlagInsertion(CBitStream& Dest, CBitStream& Source)
{
	uint8_t NumBits = 0;

	Dest.Reset();
	InsertFlags(Dest);
	while (!Source.Finished())
	{
		bool bData = Source.ReadBit();
		Dest.WriteBit(bData);
		if (bData)
		{
			NumBits++;
			if (NumBits == 5)
			{
				Dest.WriteBit(0);
				NumBits = 0;
			}
		}
		else
		{
			NumBits = 0;
		}
	}
	InsertFlags(Dest);
	Dest.Flush();
}

void WifiStartListening(void *pvParameters)
{
	static char WifiBuffer[2048];
	static char OutBuffer[4096];
	int Sock = socket(PF_INET, SOCK_STREAM, 0);
	
	sockaddr_in SockAddrIn;	
	memset(&SockAddrIn, 0, sizeof(SockAddrIn));
	SockAddrIn.sin_family = AF_INET;
	SockAddrIn.sin_port = htons(80);
	SockAddrIn.sin_addr.s_addr = htonl(INADDR_ANY);
	bind(Sock, (struct sockaddr *)&SockAddrIn, sizeof(SockAddrIn));
	
	listen(Sock, 5);

	bool bUpdated = false;
	while (!bUpdated)
	{
		sockaddr_in ClientSockAddrIn;
		socklen_t ClientSockAddrLen = sizeof(ClientSockAddrIn);
		int ClientSock = accept(Sock, (sockaddr*)&ClientSockAddrIn, &ClientSockAddrLen);
	
		if (ClientSock != -1)
		{
			int Recieved = recv(ClientSock, &WifiBuffer, sizeof(WifiBuffer) - 1, 0);
			if (Recieved > 0)
			{
				WifiBuffer[Recieved] = 0;

				const char* ExpectedRequests[] =
				{
					"GET /",
					"POST / HTTP/",
					"POST /index.html HTTP/"
				};

				bool bReturnIndex = false;
				bool bStartRecieving = false;
				for (int i = 0; i < ARRAY_NUM(ExpectedRequests); i++)
				{
					int Len = strlen(ExpectedRequests[i]);
					if (Recieved >= Len && strncmp(WifiBuffer, ExpectedRequests[i], Len) == 0)
					{
						if (strncmp(WifiBuffer, "POST", 4) == 0)
							bStartRecieving = true;
						else
							bReturnIndex = true;
						break;
					}
				}

				if (bStartRecieving)
				{
					bQuitTasks = true;
					while (!bDoneQuitTask1)
					{
						vTaskDelay(10);
					}
					while (!bDoneQuitTask2)
					{
						vTaskDelay(10);
					}

					int Length = 0;
					const char* ContentLength = strstr(WifiBuffer, "Content-Length: ");
					if (ContentLength)
					{
						Length = atoi(ContentLength + strlen("Content-Length: "));
					}

					esp_err_t ErrorCode = ESP_OK;
					bool bWaitingForStart = true;
					esp_ota_handle_t UpdateHandle = 0 ;
					const esp_partition_t *UpdatePartition = esp_ota_get_next_update_partition(nullptr);
					ESP_LOGI("ota", "partition:%p", UpdatePartition);

					if (Length > 0)
					{

						while (Length > 0)
						{
							Recieved = recv(ClientSock, &WifiBuffer, sizeof(WifiBuffer) - 1, 0);
							if (Recieved <= 0)
								break;

							WifiBuffer[Recieved] = 0;
							if (bWaitingForStart)
							{
								const char *Start = strstr(WifiBuffer, "LGV_FIRM");
								if (Start)
								{
									bWaitingForStart = false;
    								ErrorCode = esp_ota_begin(UpdatePartition, OTA_SIZE_UNKNOWN, &UpdateHandle);
									ESP_LOGI("ota", "ota begin:%d", ErrorCode);

									const char* StartOfFirmware = Start + strlen("LGV_FIRM");
									int Afterwards = (WifiBuffer + Recieved) - StartOfFirmware;
									if (Afterwards > 0)
									{
										if (ErrorCode == ESP_OK)
										{
											ErrorCode = esp_ota_write(UpdateHandle, StartOfFirmware, Afterwards);
											ESP_LOGI("ota", "ota write (a):%d", ErrorCode);
										}
									}
								}
							}
							else if (ErrorCode == ESP_OK)
							{
								ErrorCode = esp_ota_write(UpdateHandle, WifiBuffer, Recieved);
								ESP_LOGI("ota", "ota write (b):%d", ErrorCode);
							}
							Length -= Recieved;
						}
					}

					if (ErrorCode == ESP_OK)
					{
						ErrorCode = esp_ota_end(UpdateHandle);
						ESP_LOGI("ota", "ota end:%d", ErrorCode);
					}

					if (ErrorCode == ESP_OK)
					{
						ErrorCode = esp_ota_set_boot_partition(UpdatePartition);
						ESP_LOGI("ota", "set boot:%d", ErrorCode);
					}
        
					ESP_LOGI("ota", "error code:%d", ErrorCode);
					ESP_LOGI("ota", "length:%d", Length);
					ESP_LOGI("ota", "waiting for start:%d", bWaitingForStart);

					bool bSuccess = (ErrorCode == ESP_OK && Length == 0 && !bWaitingForStart);

					const char* UpdatedResponse = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n<!doctype html><title>Firmware Update</title><style>*{box-sizing: border-box;}body{margin: 0;}#main{display: flex; min-height: calc(100vh - 40vh);}#main > article{flex: 1;}#main > nav, #main > aside{flex: 0 0 20vw;}#main > nav{order: -1;}header, footer, article, nav, aside{padding: 1em;}header, footer{height: 20vh;}</style><body> <header> <center><h1>Firmware Update</h1></center> </header> <div id=\"main\"> <nav></nav> <article><p>Firmware update successful. Rebooting...</p></article> <aside></aside> </div></body>";
					const char* NotUpdatedResponse = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n<!doctype html><title>Firmware Update</title><style>*{box-sizing: border-box;}body{margin: 0;}#main{display: flex; min-height: calc(100vh - 40vh);}#main > article{flex: 1;}#main > nav, #main > aside{flex: 0 0 20vw;}#main > nav{order: -1;}header, footer, article, nav, aside{padding: 1em;}header, footer{height: 20vh;}</style><body> <header> <center><h1>Firmware Update</h1></center> </header> <div id=\"main\"> <nav></nav> <article><p>Update failed.</p></article> <aside></aside> </div></body>";
					const char* Response = bSuccess ? UpdatedResponse : NotUpdatedResponse;

					send(ClientSock, Response, strlen(Response), 0);

					bUpdated = bSuccess;
				}
				else
				{
					const char *Dummy = strstr(WifiBuffer, "dummy=");
					if (Dummy)
					{
						Config.Dummy=atoi(Dummy + strlen("dummy="));
					}

					const char* GoodResponse = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n" WEBPAGE_STRING;
					const char* BadResponse = "HTTP/1.0 404 NOT FOUND\r\nContent-Type: text/html\r\n\r\n<!doctype html><title>Not Found</title><body>Page not found :(</body>";
					const char* Response = bReturnIndex ? GoodResponse : BadResponse;

					snprintf(OutBuffer, sizeof(OutBuffer), Response, Config.Dummy);

					send(ClientSock, OutBuffer, strlen(OutBuffer), 0);
					
					if (Dummy)
					{
						SaveSettings();
					}
				}
			}

			close(ClientSock);
		}
		else
		{
			vTaskDelay(100);
		}
	}
	
	vTaskDelay(1000);
	esp_wifi_stop();
	vTaskDelay(1000);
	esp_wifi_deinit();
	vTaskDelay(1000);
	esp_restart();
}

#define RING_BUF_SIZE	1024 // Should give almost 2 seconds buffering
#define RING_BUF_MASK	(RING_BUF_SIZE-1)
#define INVALID_PACKET	~0u

uint8_t RingBuff[RING_BUF_SIZE];
uint32_t RecvCurrentByte = 0;
uint32_t RingBufIndex = 0;
uint32_t RunCount = 0;
uint32_t StartPacket = INVALID_PACKET;
static xQueueHandle RecvEventQueue = NULL;

void CPU0Task(void *pvParameters)
{
	ESP_LOGI(LogTag, "CPU0Task started\n");

	uint8_t Msg[] = { 0x40, 0xBF };

	CBitStream A(Msg, sizeof(Msg));
	CBitStream B;

	A.Flush();
	CalculateCRC(B, A);
	ZeroBitAndFlagInsertion(A, B);

	int NumBitsToSend = 0;
	const uint8_t* BytesToSend = A.ToBytes(NumBitsToSend);

	while (!bQuitTasks)
	{
		printf("Sending packet\n");
		// Queue our packet to be sent
		PacketToSend SendPacket;
		SendPacket.NumBits = NumBitsToSend;
		SendPacket.Data = BytesToSend;
		xQueueSend(SendEventQueue, &SendPacket, portMAX_DELAY);
		// Wait until it's been sent
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		printf("Send done\n");

		TickType_t WaitTime = 1000; // Start waiting for response, first long delay then anything else we might pick up
		uint32_t Packet[2];
		while (xQueueReceive(RecvEventQueue, Packet, WaitTime))
		{
			printf("Recieved packet %d to %d\n", Packet[0], Packet[1]);
			while (Packet[0] < Packet[1])
			{
				printf("%02x\n", RingBuff[Packet[0] & RING_BUF_MASK]);
				Packet[0]++;
			}
			WaitTime = 0;
        }
	}

	bDoneQuitTask1 = true;

	while (true)
	{
		vTaskDelay(100000);
	}
}

IRAM_ATTR void RecvCallback(void* UserData)
{
	bool bBit = gpio_get_level(IN_SDLCRECV2);
			
	if (bBit)
	{
		RunCount++;
	}
	else
	{
		int RunLength = RunCount;
		RunCount = 0;
		
		if (RunLength >= 5)
		{
			if (RunLength > 6) // Should never happen
			{
				StartPacket = INVALID_PACKET;
			}
			else if (RunLength == 6) // It's the flags!
			{
				uint32_t EndPacket = RingBufIndex;
				if (StartPacket != INVALID_PACKET && StartPacket != EndPacket)
				{
					uint32_t Packet[2] = { StartPacket, EndPacket };
					xQueueSendFromISR(RecvEventQueue, Packet, NULL);
				}
				StartPacket = RingBufIndex;
				RecvCurrentByte = 0x80;
			}
			// If 5 then needs to be dropped
			return;
		}
	}

	bool bFlush = ((RecvCurrentByte & 1) != 0);
	RecvCurrentByte >>= 1;
	RecvCurrentByte |= (bBit ? 0x80 : 0);
	if (bFlush)
	{
		RingBuff[(RingBufIndex++) & RING_BUF_MASK] = RecvCurrentByte;
		RecvCurrentByte = 0x80;
	}
}

void InitializeGPIO()
{
	gpio_config_t io_conf;
	io_conf.intr_type = GPIO_INTR_DISABLE;
	io_conf.mode = GPIO_MODE_INPUT;
	io_conf.pin_bit_mask = BIT(IN_SDLCRECV2);
	io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
	io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
	gpio_config(&io_conf);

	gpio_install_isr_service(0);//ESP_INTR_FLAG_IRAM);
	gpio_isr_handler_add(IN_SDLCCLOCK, RecvCallback, nullptr);

	gpio_set_pull_mode(IN_SDLCCLOCK, GPIO_PULLUP_ONLY);
	gpio_set_intr_type(IN_SDLCCLOCK, GPIO_INTR_POSEDGE); // TODO: Might be negative edge (change with SPI mode)
	gpio_intr_enable(IN_SDLCCLOCK);
}

void InitializeClock()
{
	ledc_channel_config_t ledc_ch_config = {};
	ledc_ch_config.channel  = LEDC_CHANNEL_0;
	ledc_ch_config.duty = 2;
	ledc_ch_config.gpio_num = OUT_SDLCCLOCK;
	ledc_ch_config.speed_mode = LEDC_HIGH_SPEED_MODE;
	ledc_ch_config.timer_sel = LEDC_TIMER_0;

	ledc_timer_config_t ledc_time_config = {};
	ledc_time_config.duty_resolution = LEDC_TIMER_2_BIT;
	ledc_time_config.freq_hz = 9600;
	ledc_time_config.speed_mode = LEDC_HIGH_SPEED_MODE;
	ledc_time_config.timer_num = LEDC_TIMER_0;
    
	ledc_timer_config(&ledc_time_config);
    ledc_channel_config(&ledc_ch_config);
}

void InitializeSPI()
{
	spi_bus_config_t buscfg = {};
	buscfg.mosi_io_num = IN_SDLCRECV;
	buscfg.miso_io_num = OUT_SDLCDATA;
	buscfg.sclk_io_num = IN_SDLCCLOCK;

	spi_slave_interface_config_t slvcfg = {};
	slvcfg.mode = 0; // TODO: Might be 1. Seems to work either way
	slvcfg.spics_io_num = IN_SDLCREADY;
	slvcfg.queue_size = 2; // Make sure we can have a flags transaction in flight at all times
	slvcfg.flags = SPI_SLAVE_BIT_LSBFIRST;

	// Always ready to send (connection is half duplex)
	gpio_set_pull_mode(IN_SDLCRECV, GPIO_PULLUP_ONLY);
	gpio_set_pull_mode(IN_SDLCREADY, GPIO_PULLDOWN_ONLY);

	//Initialize SPI slave interface
	spi_slave_initialize(HSPI_HOST, &buscfg, &slvcfg, 2);
}

void CPU1Task(void *pvParameters)
{
	ESP_LOGI(LogTag, "CPU1Task started\n");

	InitializeClock();
	InitializeGPIO();
	InitializeSPI();	

	uint8_t Flags[32]; // Needs to fill at least 1ms. 32 bytes is about 50ms at 4800 baud
	memset(Flags, 0x7E, sizeof(Flags));
			
	spi_slave_transaction_t FlagsTransaction = {};
	FlagsTransaction.length = sizeof(Flags);
	FlagsTransaction.tx_buffer = Flags;
	FlagsTransaction.rx_buffer = nullptr;

	while (!bQuitTasks)
	{
		PacketToSend Packet;
		if (xQueueReceive(SendEventQueue, &Packet, 0))
		{
			spi_slave_transaction_t MsgTransaction = {};
			MsgTransaction.length = Packet.NumBits;
			MsgTransaction.tx_buffer = Packet.Data;
			MsgTransaction.rx_buffer = nullptr;

			spi_slave_queue_trans(HSPI_HOST, &MsgTransaction, portMAX_DELAY);
			while (true)
			{
				spi_slave_queue_trans(HSPI_HOST, &FlagsTransaction, portMAX_DELAY);

				spi_slave_transaction_t* RecievedTransaction = nullptr;
				spi_slave_get_trans_result(HSPI_HOST, &RecievedTransaction, 0);
				if (RecievedTransaction == &MsgTransaction)
				{
					xTaskNotifyGive(RecvTask);
					break;
				}
			}
		}
		else
		{
			spi_slave_queue_trans(HSPI_HOST, &FlagsTransaction, portMAX_DELAY);			
		}
	}
	
	bDoneQuitTask2 = true;

	while (true)
	{
		vTaskDelay(100000);
	}
}

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int WIFI_CONNECTED_BIT = BIT0;

static const char *TAG = "simple wifi";

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        ESP_LOGI(TAG, "got ip:%s",
                 ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "station:" MACSTR " join, AID=%d",
                 MAC2STR(event->event_info.sta_connected.mac),
                 event->event_info.sta_connected.aid);
        break;
    case SYSTEM_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "station:" MACSTR "leave, AID=%d",
                 MAC2STR(event->event_info.sta_disconnected.mac),
                 event->event_info.sta_disconnected.aid);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

void wifi_init_sta()
{
    wifi_event_group = xEventGroupCreate();

    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL) );

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	
	wifi_config_t wifi_config;
    memset((void *)&wifi_config, 0, sizeof(wifi_config_t));
    memcpy(wifi_config.sta.ssid, EXAMPLE_ESP_WIFI_SSID,sizeof(EXAMPLE_ESP_WIFI_SSID));
    memcpy(wifi_config.sta.password, EXAMPLE_ESP_WIFI_PASS, sizeof(EXAMPLE_ESP_WIFI_PASS));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");
    ESP_LOGI(TAG, "connect to ap SSID:%s password:%s",
             EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
}

extern "C" void app_main(void)
{
	//Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

	if (LoadSettings() != ESP_OK)
	{
		ClearSettings();
	}

	printf("Waiting for connection to the wifi network...\n ");
	xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
	printf("Connected\n\n");

	SendEventQueue = xQueueCreate(4, sizeof(PacketToSend));
	RecvEventQueue = xQueueCreate(16, 2 * sizeof(uint32_t));

	xTaskCreatePinnedToCore(&CPU0Task, "CPU0Task", 8192, NULL, 5, &RecvTask, 0);
	xTaskCreatePinnedToCore(&CPU1Task, "CPU1Task", 8192, NULL, 5, NULL, 1);
	xTaskCreatePinnedToCore(&WifiStartListening, "WifiConfig", 8192, NULL, 5, NULL, 0);
}
