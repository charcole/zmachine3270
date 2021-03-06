// (c) Charlie Cole 2018

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <algorithm>
extern "C"
{
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
};
#include "webpage.h"
#include "zops.h"
#include "FrontEnd.h"
#include "TN3270.h"
#include "Wikipedia.h"
#include "SSH.h"

#ifndef CONFIG_WL_SECTOR_SIZE
#define CONFIG_WL_SECTOR_SIZE 4096
#endif

#define ARRAY_NUM(x) (sizeof(x)/sizeof(x[0]))

#define EXAMPLE_ESP_WIFI_SSID      "TALKTALK4153C8_24Ghz"
#define EXAMPLE_ESP_WIFI_PASS      "EX4T6H46"

#define STORAGE_NAMESPACE "settingsns"

#define TOTAL_GAME_FLASH_SIZE		(768*1024)

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t wifi_event_group;

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

struct Configuration
{
	int32_t Dummy;
};

Configuration Config;
		
volatile bool bQuitTasks = false;
volatile bool bDoneQuitTask1 = false;
volatile bool bDoneQuitTask2 = false;

extern void CreateSendRecieveTasks();

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
					"POST /index.html HTTP/",
					"POST /game.html HTTP/"
				};

				bool bReturnIndex = false;
				bool bStartRecieving = false;
				bool bUploadingGame = false;
				ESP_LOGI("ota", "got:%s", WifiBuffer);
				for (int i = 0; i < ARRAY_NUM(ExpectedRequests); i++)
				{
					ESP_LOGI("ota", "testing:%s", ExpectedRequests[i]);
					int Len = strlen(ExpectedRequests[i]);
					if (Recieved >= Len && strncmp(WifiBuffer, ExpectedRequests[i], Len) == 0)
					{
						ESP_LOGI("ota", "matched:%s", ExpectedRequests[i]);
						if (strncmp(WifiBuffer, "POST", 4) == 0)
						{
							bStartRecieving = true;
							bUploadingGame = (i == 3);
						}
						else
						{
							bReturnIndex = true;
						}
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
					const esp_partition_t *UpdatePartition = nullptr;
					
					if (bUploadingGame)
					{
						UpdatePartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "games");
					}
					else
					{
					 	UpdatePartition = esp_ota_get_next_update_partition(nullptr);
					}
					
					ESP_LOGI("ota", "partition:%p", UpdatePartition);

					if (UpdatePartition && Length > 0)
					{
						size_t UploadOffset = 0;
						while (Length > 0)
						{
							Recieved = recv(ClientSock, &WifiBuffer, sizeof(WifiBuffer) - 1, 0);
							if (Recieved <= 0)
								break;

							WifiBuffer[Recieved] = 0;
							if (bWaitingForStart)
							{
								const char *Start = strstr(WifiBuffer, bUploadingGame?"LGV_DATA":"LGV_FIRM");
								if (Start)
								{
									bWaitingForStart = false;
									if (bUploadingGame) // Erase flash
									{
										ErrorCode = esp_partition_erase_range(UpdatePartition, 0, TOTAL_GAME_FLASH_SIZE);
									}
									else
									{
										ErrorCode = esp_ota_begin(UpdatePartition, OTA_SIZE_UNKNOWN, &UpdateHandle);
									}
									ESP_LOGI("ota", "ota begin:%d", ErrorCode);

									const char* StartOfFirmware = Start + strlen("LGV_FIRM");
									int Afterwards = (WifiBuffer + Recieved) - StartOfFirmware;
									if (Afterwards > 0)
									{
										if (ErrorCode == ESP_OK)
										{
											if (bUploadingGame)
											{
												ErrorCode = esp_partition_write(UpdatePartition, UploadOffset, StartOfFirmware, Afterwards);
												UploadOffset += Afterwards;
											}
											else
											{
												ErrorCode = esp_ota_write(UpdateHandle, StartOfFirmware, Afterwards);
											}
											ESP_LOGI("ota", "ota write (a):%d", ErrorCode);
										}
									}
								}
							}
							else if (ErrorCode == ESP_OK)
							{
								if (bUploadingGame)
								{
									int SizeOfGameData = TOTAL_GAME_FLASH_SIZE;
									int TruncatedRecieved = Recieved;
									if (UploadOffset + TruncatedRecieved > SizeOfGameData)
									{
										ESP_LOGI("ota", "throwing away end of buffer (%d bytes)", Recieved + UploadOffset - SizeOfGameData);
										TruncatedRecieved = SizeOfGameData - UploadOffset;
									}
									ErrorCode = esp_partition_write(UpdatePartition, UploadOffset, WifiBuffer, TruncatedRecieved);
									UploadOffset += TruncatedRecieved;
								}
								else
								{
									ErrorCode = esp_ota_write(UpdateHandle, WifiBuffer, Recieved);
								}
								ESP_LOGI("ota", "ota write (b):%d UploadOffset:%d Received:%d", ErrorCode, UploadOffset, Recieved);
							}
							Length -= Recieved;
						}
					}

					if (!bUploadingGame)
					{
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

					FATFS *FatFSInfo;
					DWORD FreeClusters;
					FRESULT FreeError = f_getfree("0:", &FreeClusters, &FatFSInfo);
					int FreeSize = -1024;
					if (FreeError == FR_OK)
					{
						DWORD FreeSectors = FreeClusters * FatFSInfo->csize;
						FreeSize = FreeSectors * CONFIG_WL_SECTOR_SIZE;
					}

					snprintf(OutBuffer, sizeof(OutBuffer), Response, Config.Dummy, FreeSize / 1024);

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

void GameTask(void *pvParameters)
{
	// # To write to the partition
	// Tool at ~/esp/esp-idf/components/partition_table/parttool2.py (2 is own name for newer github version)
	// parttool.py --port "/dev/tty.SLAB_USBtoUART" write_partition --partition-name=games --input "GameData.bin"
	// Or use update_game.sh to update via WiFi
	const void *GameData = nullptr;
	const esp_partition_t* GamesPartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "games");
	spi_flash_mmap_handle_t FlashHandle = 0;
	if (GamesPartition)
	{
		constexpr int GameSize = 256 * 1024;
		FrontEnd SelectionScreen;
		int CurrentGame = SelectionScreen.Show(&GScreen);
		while (CurrentGame < 0)
		{
			if (CurrentGame == -1)
			{
				TN3270::Run();
			}
			else if (CurrentGame == -2)
			{
				Wikipedia::Run();
			}
			else
			{
				SSH::Run();
			}
			CurrentGame = SelectionScreen.Show(&GScreen);
		}
		int CurGameOffset = CurrentGame * GameSize;
		int CurGameSize = GameSize;
		if (CurrentGame == 0 || CurrentGame == 3) // Z3 games
		{
			CurGameSize /= 2;
			if (CurrentGame == 3)
				CurGameOffset = CurGameSize;
		}
		if (esp_partition_mmap(GamesPartition, CurGameOffset, CurGameSize, SPI_FLASH_MMAP_DATA, &GameData, &FlashHandle) == ESP_OK)
		{
    		ESP_LOGI(TAG, "Playing game from partition");
		}
	}
	zopsMain((const char*)GameData);
	if (FlashHandle)
	{
		spi_flash_munmap(FlashHandle);
	}
}

void MountFAT()
{
	ESP_LOGI(TAG, "Mounting FAT filesystem");
    esp_vfs_fat_mount_config_t mount_config;
	mount_config.max_files = 16;
	mount_config.format_if_mount_failed = true;
	mount_config.allocation_unit_size = CONFIG_WL_SECTOR_SIZE;
	esp_err_t err = esp_vfs_fat_spiflash_mount("/spiflash", "storage", &mount_config, &s_wl_handle);
	if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS (%s)", esp_err_to_name(err));
        return;
    }
    ESP_LOGE(TAG, "Mounted FATFS at /spiflash");
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

	MountFAT();

	printf("Waiting for connection to the wifi network...\n ");
	xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
	printf("Connected\n\n");
	
	xTaskCreatePinnedToCore(&WifiStartListening, "WifiConfig", 8192, NULL, 5, NULL, 0);
	xTaskCreatePinnedToCore(&GameTask, "GameTask", 8192, NULL, 5, NULL, 0);
	CreateSendRecieveTasks();
}
