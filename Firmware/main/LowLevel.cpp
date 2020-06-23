#include <stdio.h>
#include <string.h>
#include "crc.h"
#include "Network.h"
extern "C"
{
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "driver/timer.h"
#include "driver/ledc.h"
#include "esp_log.h"
};

#define OUT_SDLCDATA  			(GPIO_NUM_13)
#define OUT_SDLCCLOCK			(GPIO_NUM_12)
#define IN_SDLCCLOCK			(GPIO_NUM_14)
#define IN_SDLCREADY			(GPIO_NUM_27)
#define IN_SDLCRECV				(GPIO_NUM_26)
#define IN_SDLCRECV2			(GPIO_NUM_25)

#define RING_BUF_SIZE	1024 // Should give almost 2 seconds buffering
#define RING_BUF_MASK	(RING_BUF_SIZE-1)
#define INVALID_PACKET	~0u

extern volatile bool bQuitTasks;
extern volatile bool bDoneQuitTask1;
extern volatile bool bDoneQuitTask2;

static xQueueHandle SendEventQueue = nullptr;
static TaskHandle_t RecvTask = nullptr;
static const char *LogTag = "informer";
static NetworkState Network;

static DRAM_ATTR uint8_t RecvRingBuff[RING_BUF_SIZE];
static DRAM_ATTR uint32_t RecvCurrentByte = 0;
static DRAM_ATTR uint32_t RingBufIndex = 0;
static DRAM_ATTR uint32_t RunCount = 0;
static DRAM_ATTR uint32_t StartPacket = INVALID_PACKET;
static DRAM_ATTR xQueueHandle RecvEventQueue = NULL;

struct PacketToSend
{
	uint32_t NumBits;
	const uint8_t* Data;
};

// Packet building
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
			MAX_MESSAGE_LENGTH = (2048+1024)
		};

		int32_t TotalBits;
		int32_t CurrentByte;
		uint8_t CurrentMask;
		uint8_t Bytes[MAX_MESSAGE_LENGTH];
};

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
		}
		Dest.WriteBit(bData);
	}
	for (int BitIndex = 0; BitIndex < 16; BitIndex++)
	{
		Dest.WriteBit(!(CRC >> 15));
		CRC <<= 1;
	}
	Dest.Flush();
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

// Messaging
void MessageTask(void *pvParameters)
{
	ESP_LOGI(LogTag, "MessageTask started\n");

	while (!bQuitTasks)
	{
		uint8_t SendBuffer[2048+1024];
		bool bWaitForReply = false;
		int SendSize = Network.GenerateNewPackets(SendBuffer, bWaitForReply);
		
		/*printf("Sending packet of size %d:", SendSize);
		for (int i=0; i<SendSize; i++)
		{
			printf(" %02x", SendBuffer[i]);
		}
		printf("\n");*/

		CBitStream A(SendBuffer, SendSize);
		CBitStream B;

		A.Flush();
		CalculateCRC(B, A);
		ZeroBitAndFlagInsertion(A, B);

		int NumBitsToSend = 0;
		const uint8_t* BytesToSend = A.ToBytes(NumBitsToSend);

		// Queue our packet to be sent
		PacketToSend SendPacket;
		SendPacket.NumBits = NumBitsToSend;
		SendPacket.Data = BytesToSend;
		xQueueSend(SendEventQueue, &SendPacket, portMAX_DELAY);
		// Wait until it's been sent
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

		TickType_t WaitTime = (bWaitForReply ? 1000 : 0); // Start waiting for response, first long delay then anything else we might pick up
		uint32_t Packet[2];
		while (xQueueReceive(RecvEventQueue, Packet, WaitTime))
		{
			const int LengthOfPacket = Packet[1] - Packet[0];
			uint8_t PacketData[1024];
			
			if (LengthOfPacket < sizeof(PacketData))
			{
				for (int PacketIndex = 0; PacketIndex < LengthOfPacket; PacketIndex++)
				{
					PacketData[PacketIndex] = RecvRingBuff[(Packet[0]++) & RING_BUF_MASK];
				}
				
                /*printf("Recieved packet of length %d:", LengthOfPacket);
				for (int i=0; i<LengthOfPacket; i++)
				{
					printf(" %02x", PacketData[i]);
				}
				printf("\n");*/

				PacketParser ProcessedPacket;
				ProcessedPacket.Parse(PacketData, LengthOfPacket);
				ProcessedPacket.Dump(PacketData);
				
				Network.ProcessPacket(ProcessedPacket, PacketData);
			}
			else
			{
				printf("Packet over max length %d :(\n", LengthOfPacket);
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

// Low level recieve
IRAM_ATTR void RecvCallback(void* UserData)
{
    bool bBit ((GPIO.in & (1 << IN_SDLCRECV2)) != 0);

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
		RecvRingBuff[(RingBufIndex++) & RING_BUF_MASK] = RecvCurrentByte;
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

	gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
	gpio_isr_handler_add(IN_SDLCCLOCK, RecvCallback, nullptr);

	gpio_set_pull_mode(IN_SDLCCLOCK, GPIO_PULLUP_ONLY);
	gpio_set_intr_type(IN_SDLCCLOCK, GPIO_INTR_POSEDGE); // TODO: Might be negative edge (change with SPI mode)
	gpio_intr_enable(IN_SDLCCLOCK);
}

// Low level send
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

void IRAM_ATTR SendTask(void *pvParameters)
{
    // Sends flags unless we have a message to send. Done via SPI
	ESP_LOGI(LogTag, "SendTask started\n");

	InitializeGPIO();
	InitializeSPI();	

	uint8_t Flags[16]; // Needs to fill at least 1ms. 16 bytes is about 13ms at 9600 baud
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

// Setup

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

void CreateSendRecieveTasks()
{
	BuildCRCLookup();
	BuildTextTables();

    SendEventQueue = xQueueCreate(4, sizeof(PacketToSend));
	RecvEventQueue = xQueueCreate(16, 2 * sizeof(uint32_t));
	
    InitializeClock();

	xTaskCreatePinnedToCore(&MessageTask, "MessageTask", 2*8192, NULL, 5, &RecvTask, 0);
	xTaskCreatePinnedToCore(&SendTask, "SendTask", 8192, NULL, 5, NULL, 1);
}
