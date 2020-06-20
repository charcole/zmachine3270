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

enum ESDLCPacketType
{
	InformationPacket = 0x00,
	RecieveReady = 0x01,
	RecieveNotReady = 0x05,
	Reject = 0x09,
	UnnumberedInfo = 0x03,
	RequestInitializationMode = 0x07,
	DisconnectMode = 0x0F,
	RequestDisconnect = 0x43,
	UnnumberedACK = 0x63,
	FrameReject = 0x87,
	XID = 0xAF,
	Configure = 0xC7,
	Test = 0xE3,
	Beacon = 0xEF,
	Unknown = 0xFF
};

enum ETransmissionHeaderType
{
	FMData = 0x00,
	NetworkHeader = 0x20,
	DataFlowControl = 0x40,
	SessionControl = 0x60
};

enum ESenseDataReason
{
	UserSenseDataOnly = 0x00,
	RequestReject = 0x08,
	RequestError = 0x10,
	StateError = 0x20,
	ReqHdrUsageError = 0x40,
	PathError = 0x80
};

enum ERequestRejectReason
{
	ResourceNotAvailable,
	InterventionRequired,
	MissingPassword,
	InvalidPassword,
	SessionLimitExceeded,
	ResourceUnknown,
	ResourceNotAvailable2,
	InvalidContentsID,
	ModeInconsistency,
	PermissionRejected,
	BracketRaceError,
	ProcedureNotSupported,
	NAUContention,
	NAUNotAuthorized,
	EndUserNotAuthorized,
	MissingRequesterID,
	Break,
	UnsufficientResource,
	BracketBidReject,
	BracketBidReject2,
	FunctionActive,
	FunctionActive2,
	LinkOrLinkResourceInactive,
	LinkProcedureinProcess,
	RTRNotRequired,
	RequestSequenceError
};

struct ParsedSDLC
{
	ESDLCPacketType Type;
	uint8_t Address;
	uint8_t RecieveCount;
	uint8_t SendCount;
	bool bPollOrFinal;
	bool bRecieveCountValid;
	bool bSendCountValid;
};

struct ParsedFID2
{
	bool bStartOfBIU;
	bool bEndOfBIU;
	bool bODAI;
	bool bExpidited;
	uint8_t Destination;
	uint8_t Origin;
	uint16_t Sequence;
};

struct ParsedRU
{
	ETransmissionHeaderType HeaderType;
	uint8_t ResponseType;
	bool bHasFMHeader;
	bool bBypassQueues;
	bool bPAC;
};

struct ParsedRequest
{
	bool bStartOfChain;
	bool bEndOfChain;
	bool bBeginBracket;
	bool bEndBracket;
	bool bChangeDirection;
	bool bEnciphered;
	bool bPadded;
	bool bConditionalEndBracket;
	bool bCodeSelection;
};

struct ParsedResponce
{
	bool bPositiveResponse;
};

struct ParsedSenseData
{
	ESenseDataReason Reason;
	uint8_t SubReason;
	uint16_t SpecificInfo;
};

class ParsedPacket
{
public:
	ParsedPacket()
	{
		memset(this, 0, sizeof(*this));
		StartOfData = -1;
	}
	
	void Parse(const uint8_t *PacketData, int PacketSize)
	{
		EndOfData = PacketSize; // Ignore CRC, already checked
		if (PacketSize > 2)
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
							RU.bBypassQueues = ((PacketData[9]&0x02) == 0);
							RU.bPAC = ((PacketData[9]&0x01) != 0);
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

	void Dump(const uint8_t* Data)
	{
		if (bSDLCValid)
		{
			printf("Packet for/to %02x\n", SDLC.Address);
			switch (SDLC.Type)
			{
				case InformationPacket: printf("Information"); break;
				case RecieveReady: printf("Recieve Ready"); break;
				case RecieveNotReady: printf("Recieve Not Ready"); break;
				case Reject: printf("Reject"); break;
				case UnnumberedInfo: printf("Unnumbered Info"); break;
				case RequestInitializationMode: printf("Request Initialization Mode"); break;
				case DisconnectMode: printf("Disconnect Mode"); break;
				case RequestDisconnect: printf("Request Disconnect"); break;
				case UnnumberedACK: printf("Unnumbered ACK"); break;
				case FrameReject: printf("Frame Reject"); break;
				case XID: printf("XID"); break;
				case Configure: printf("Configure"); break;
				case Test: printf("Test"); break;
				case Beacon: printf("Beacon"); break;
				default: printf("Unknown"); break;
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
					case FMData: printf("   FM data\n"); break;
					case NetworkHeader: printf("   Network header\n"); break;
					case DataFlowControl: printf("   Data flow control\n"); break;
					case SessionControl: printf("   Session control\n"); break;
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
				case UserSenseDataOnly: printf("    User Sense Data Only\n"); break;
				case RequestReject:
					printf("    Request Reject\n");
					switch(Sense.SubReason)
					{
						case ResourceNotAvailable: printf("     Resource Not Available\n"); break;
						case InterventionRequired: printf("     Intervention Required\n"); break;
						case MissingPassword: printf("     Missing Password\n"); break;
						case InvalidPassword: printf("     Invalid Password\n"); break;
						case SessionLimitExceeded: printf("     Session Limit Exceeded\n"); break;
						case ResourceUnknown: printf("     Resource Unknown\n"); break;
						case ResourceNotAvailable2: printf("     Resource Not Available\n"); break;
						case InvalidContentsID: printf("     Invalid Contents ID\n"); break;
						case ModeInconsistency: printf("     Mode Inconsistency\n"); break;
						case PermissionRejected: printf("     Permission Rejected\n"); break;
						case BracketRaceError: printf("     Bracket Race Error\n"); break;
						case ProcedureNotSupported: printf("     Procedure Not Supported\n"); break;
						case NAUContention: printf("     NAU Contention\n"); break;
						case NAUNotAuthorized: printf("     NAU Not Authorized\n"); break;
						case EndUserNotAuthorized: printf("     End User Not Authorized\n"); break;
						case MissingRequesterID: printf("     Missing Requester ID\n"); break;
						case Break: printf("     Break\n"); break;
						case UnsufficientResource: printf("     Unsufficient Resource\n"); break;
						case BracketBidReject: printf("     Bracket Bid Reject\n"); break;
						case BracketBidReject2: printf("     Bracket Bid Reject\n"); break;
						case FunctionActive: printf("     Function Active\n"); break;
						case FunctionActive2: printf("     Function Active\n"); break;
						case LinkOrLinkResourceInactive: printf("     Link or Link Resource Inactive\n"); break;
						case LinkProcedureinProcess: printf("     Link Procedure in Process\n"); break;
						case RTRNotRequired: printf("     RTR Not Required\n"); break;
						case RequestSequenceError: printf("     Request Sequence Error\n"); break;
						default:   printf("     Error code: %02x\n", Sense.SubReason);
					}
					break;
				case RequestError: printf("    Request Error\n"); break;
				case StateError: printf("    State Error\n"); break;
				case ReqHdrUsageError: printf("    Request Header Usage Error\n"); break;
				case PathError: printf("    Path Error\n"); break;
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

	ParsedSDLC SDLC;
	ParsedFID2 FID2;
	ParsedRU RU;
	ParsedRequest Req;
	ParsedResponce Res;
	ParsedSenseData Sense;
	int StartOfData;
	int EndOfData;
	bool bSDLCValid;
	bool bFID2Valid;
	bool bRequestValid;
	bool bResponseValid;
	bool bSenseValid;
};

class RawStream
{
public:
	void SendByte(uint8_t Byte)
	{
		SendRaw(Byte);
		History[Head & (kHistorySize - 1)] = Byte;
		Head++;
	}

	void Resend(int Start, int End)
	{
		for (int Current = Start; Current < End; Current++)
		{
			SendRaw(History[Current]);
		}
	}

	int Tell() const
	{
		return Head;
	}

	int GetData(uint8_t *Destination) const // For debug
	{
		memcpy(Destination, History, Head);
		return Head;
	}

	void Reset()
	{
		Head = 0;
	}

private:
	void SendRaw(uint8_t Byte)
	{
		//printf("Sending: %02x\n", Byte);
	}

	static constexpr int kHistorySize = 4096;
	uint8_t History[kHistorySize];
	int Head = 0;
};

class SDLCInfoStream : public RawStream
{
public:
	void SetRecieveCount(uint8_t NewCount)
	{
		RecieveCount = NewCount;
	}

	uint8_t RecieveCount = 0;
	uint8_t SendCount = 0;
};

class SNAStream : public SDLCInfoStream
{
public:
	void SetSequence(uint16_t NewSequence)
	{
		Sequence = NewSequence;
	}

	uint16_t Sequence = 0;
};

class SDLCPacket
{
public:
	SDLCPacket(RawStream& Stream)
		: Raw(Stream)
	{
		SendData(Address);
	}
	
	void SendData(uint8_t Byte)
	{
		
		Raw.SendByte(Byte);
	}

	void SendData(std::initializer_list<uint8_t> List)
	{
		for (uint8_t Byte : List)
		{
			SendData(Byte);
		}
	}

	~SDLCPacket()
	{
	}

protected:
	static constexpr uint8_t Address = 0x40;
	static constexpr uint8_t Flag = 0x7E;
	static constexpr uint8_t FinalFlag = 0x10;

	RawStream& Raw;
};

class SDLCReadyToRecieve : public SDLCPacket
{
public:
	SDLCReadyToRecieve(SDLCInfoStream& Stream, bool bFinal = true)
		: SDLCPacket(Stream)
	{
		uint8_t Counts = 0x01 + (bFinal ? FinalFlag : 0);
		Counts |= (Stream.RecieveCount << 5) & 0xE0;
		SendData(Counts);
	}
};

class SDLCSetNormalResponseMode : public SDLCPacket
{
public:
	SDLCSetNormalResponseMode(SDLCInfoStream& Stream, bool bFinal = true)
		: SDLCPacket(Stream)
	{
		SendData(0x83 + (bFinal ? FinalFlag : 0));
	}
};

class SDLCRequestXID : public SDLCPacket
{
public:
	SDLCRequestXID(SDLCInfoStream& Stream, bool bFinal = true)
		: SDLCPacket(Stream)
	{
		SendData(0xAF + (bFinal ? FinalFlag : 0));
	}
};

class SDLCInfoPacket : public SDLCPacket
{
public:
	SDLCInfoPacket(SDLCInfoStream& Stream, bool bFinal = false)
		: SDLCPacket(Stream)
	{
		uint8_t Counts = (bFinal ? FinalFlag : 0);
		Counts |= (Stream.RecieveCount << 5) & 0xE0;
		Counts |= (Stream.SendCount << 1) & 0x0E;
		Stream.SendCount++;
		SendData(Counts);
	}
};

class TranmissionPacket : public SDLCInfoPacket
{
public:
	TranmissionPacket(SNAStream& Stream, uint8_t Destination, uint8_t Source, bool bExpedited = false, bool bFinal = false, int SequenceOverride = -1)
		: SDLCInfoPacket(Stream, bFinal)
	{
		SendData(TransHeader + (bExpedited ? ExpiditedFlag : 0));
		SendData(0); // Reserved
		SendData(Destination);
		SendData(Source);
		if (SequenceOverride >= 0)
		{
			SendData((SequenceOverride >> 8) & 0xFF);
			SendData((SequenceOverride >> 0) & 0xFF);
		}
		else
		{
			SendData((Stream.Sequence >> 8) & 0xFF);
			SendData((Stream.Sequence >> 0) & 0xFF);
			if (!bExpedited)
			{
				Stream.Sequence++;
			}
		}
	}

private:
	static constexpr uint8_t TransHeader = 0x2C; // FID 2 + whole BIU
	static constexpr uint8_t ExpiditedFlag = 0x01;
};

class RequestPacket : public TranmissionPacket
{
public:
	RequestPacket(SNAStream& Stream, uint8_t Category, uint8_t Destination, uint8_t Source, bool bExpedited = false, bool bFinal = false, bool bChangeDirection = false, bool bStartOfBracket = true)
		: TranmissionPacket(Stream, Destination, Source, bExpedited, bFinal)
	{
		SendData((Category << 5) | 3); // Category + Start+end of chain
		SendData(0x80 + (bChangeDirection?0x20:0x00)); // Definitive response required
		SendData(bStartOfBracket ? 0xC0 : 0x40); // Start and end of bracket
	}
};

class PositiveResponsePacket : public TranmissionPacket
{
public:
	PositiveResponsePacket(SNAStream& Stream, uint8_t Category, uint8_t Destination, uint8_t Source, int SequenceOverride, bool bExpedited = false, bool bFinal = false)
		: TranmissionPacket(Stream, Destination, Source, bExpedited, bFinal, SequenceOverride)
	{
		SendData((Category << 5) | 0x83); // Category + Response
		SendData(0x80); // Definitive response + Positive
		SendData(0x00);
	}
};

char EBCDICToASCII[256];
char ASCIIToEBCDIC[256];

void Map(char ASCII, char EBCDIC)
{
	EBCDICToASCII[(uint8_t)EBCDIC] = ASCII;
	ASCIIToEBCDIC[(uint8_t)ASCII] = EBCDIC;
}

void Map(char ASCIIStart, char ASCIIEnd, char EBCDIC)
{
	for (char ASCII = ASCIIStart; ASCII <= ASCIIEnd; ASCII++)
	{
		Map(ASCII, EBCDIC++);
	}
}

void BuildTextTables()
{
	memset(EBCDICToASCII, ' ', sizeof (EBCDICToASCII));
	memset(ASCIIToEBCDIC, 0x40, sizeof (ASCIIToEBCDIC));

	Map('a', 'i', 0x81);
	Map('j', 'r', 0x91);
	Map('s', 'z', 0xA2);
	Map('A', 'I', 0xC1);
	Map('J', 'R', 0xD1);
	Map('S', 'Z', 0xE2);
	Map('0', '9', 0xF0);
	Map('\n', 0x15);
	Map(' ', 0x40);
	Map(0xA2, 0x4A);
	Map('.', 0x4B);
	Map('<', 0x4C);
	Map('(', 0x4D);
	Map('+', 0x4E);
	Map('|', 0x4F);
	Map('&', 0x50);
	Map('!', 0x5A);
	Map('$', 0x5B);
	Map('*', 0x5C);
	Map(')', 0x5D);
	Map(';', 0x5E);
	Map(0xAC, 0x5F);
	Map('-', 0x60);
	Map('/', 0x61);
	Map(0xA6, 0x6A);
	Map(',', 0x6B);
	Map('%', 0x6C);
	Map('_', 0x6D);
	Map('>', 0x6E);
	Map('?', 0x6F);
	Map(':', 0x7A);
	Map('#', 0x7B);
	Map('@', 0x7C);
	Map('\'', 0x7D);
	Map('=', 0x7E);
	Map('"', 0x7F);
}

class NetworkState
{
	enum EState
	{
		StateXID,
		StateNrm,
		StateActPU,
		StateActLU,
		StateBind,
		StateDataReset,
		StateSendScreen,
		StateWaitForInput,
		StateRespond
	};

public:
	NetworkState()
	{
		BuildTextTables();
	}

	int GenerateNewPackets(uint8_t* Buffer)
	{
		Stream.Reset();
		if (bSendReadyToRecieve)
		{
			SDLCReadyToRecieve RR(Stream);
			bSendReadyToRecieve = false;
			return Stream.GetData(Buffer);
		}
		switch (State)
		{
			case StateXID:
			{
				SDLCRequestXID XID(Stream);
				State = StateNrm;
				break;
			}
			case StateNrm:
			{
				SDLCSetNormalResponseMode SetNrmRes(Stream);
				State = StateActPU;
				break;
			}
			case StateActPU:
			{
				Stream.SetSequence(0x1568);
				{
					RequestPacket ACTPU(Stream, 3, 0, 0, true);
					ACTPU.SendData({0x11, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A});
				}
				bSendReadyToRecieve = true;
				State = StateActLU;
				break;
			}
			case StateActLU:
			{
				Stream.SetSequence(0x1569);
				{
					RequestPacket ACTLU(Stream, 3, 2, 0, true);
					ACTLU.SendData({0x0D, 0x01, 0x01});
				}
				bSendReadyToRecieve = true;
				State = StateBind;
				break;
			}
			case StateBind:
			{
				Stream.SetSequence(0x0);
				{
					RequestPacket Bind(Stream, 3, 2, 1);
					Bind.SendData({
							0x31, 0x01, 0x03, 0x03, 0xA1, 0xA1, 0x30, 0x80, 0x00,
							0x01, 0x85, 0x85, 0x0A, 0x00, 0x02, 0x11, 0x00, 0x00,
							0xB1, 0x00, 0xC0, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
							0x04, 0xD1, 0xE2, 0xC9, 0xD6, 0x00
							});
				}
				bSendReadyToRecieve = true;
				State = StateDataReset;
				break;
			}
			case StateDataReset:
			{
				{
					RequestPacket DataReset(Stream, 3, 2, 1);
					DataReset.SendData(0xA0);
				}
				bSendReadyToRecieve = true;
				State = StateSendScreen;
				break;
			}
			case StateSendScreen:
			{
				{
					RequestPacket DataStream3270(Stream, 0, 2, 1);
					DataStream3270.SendData({0xF1, 0xD3, 0x11, 0x5C, 0xF0, 0x1D, 0xF0});
					const char* Message = "You did it! ";
					while (*Message)
					{
						DataStream3270.SendData(ASCIIToEBCDIC[(uint8_t)*Message++]);
					}
					DataStream3270.SendData({0x6E, 0x40, 0x1D, 0x40, 0x13, 0x11, 0x5D, 0x7F, 0x1D, 0xF0});
				}
				bSendReadyToRecieve = true;
				State = StateWaitForInput;
				break;
			}
			case StateWaitForInput:
			{
				SDLCReadyToRecieve RR(Stream);
				break;
			}
			case StateRespond:
			{
				{
					// TODO: ERI?
					PositiveResponsePacket Response(Stream, 0, 2, 1, ProcessedPacket);
					Response.SendData({0x7d, 0x5d, 0xc3, 0x11, 0x5d, 0xc2, 0xf4});
				}
				{
					RequestPacket DataReset(Stream, 0, 2, 1, false, false, false, false);
				}
				bSendReadyToRecieve = true;
				State = StateWaitForInput;
				break;
			}
		}
		return Stream.GetData(Buffer);
	}

	void ProcessPacket(const ParsedPacket& Packet, const uint8_t* Data)
	{
		if (Packet.bSDLCValid)
		{
			if (Packet.SDLC.bSendCountValid)
			{
				Stream.SetRecieveCount(Packet.SDLC.RecieveCount);
			}
		}
		if (Packet.bRequestValid && Packet.Req.bChangeDirection)
		{
			if (Packet.FID2.Sequence != ProcessedPacket)
			{
				State = StateRespond;
				if (Packet.bResponseValid || Packet.bRequestValid)
				{
					//Stream.SetSequence(Packet.FID2.Sequence + 1);
					ProcessedPacket = Packet.FID2.Sequence;
				}
			}
		}
	}

private:
	EState State = StateXID;
	SNAStream Stream;
	uint16_t ProcessedPacket = 0xFFFF;
	bool bSendReadyToRecieve = false;
};

NetworkState Network;

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
			MAX_MESSAGE_LENGTH = 128
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

uint16_t CRCLookup[16][16]; // 512 bytes

void BuildCRCLookup()
{
	for (uint16_t DataNibble=0; DataNibble<16; DataNibble++)
	{
		for (uint16_t CRCNibble=0; CRCNibble<16; CRCNibble++)
		{
			uint16_t Data = DataNibble;
			uint16_t CRC = (CRCNibble << 12);
			for (uint8_t Step = 0; Step < 4; Step++)
			{
				uint16_t bLastBit = (CRC >> 15);
				uint16_t bDataBit = (Data & 1) ^ bLastBit;
				Data >>= 1;
				CRC = bDataBit + (CRC << 1);
				if (bDataBit)
				{
					CRC ^= (1<<5) + (1<<12);
				}
			}
			CRCLookup[CRCNibble][DataNibble]=CRC;
		}
	}
}

bool ValidatePacket(uint8_t *Data, size_t Length)
{
	uint16_t CRC = 0xFFFF;
	while (Length--)
	{
		uint16_t Byte = *(Data++);
		CRC = (CRC << 4) ^ CRCLookup[CRC>>12][Byte & 0xF];
		Byte >>= 4;
		CRC = (CRC << 4) ^ CRCLookup[CRC>>12][Byte & 0xF];
	}
	return (CRC == 0x1D0F);
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

	BuildCRCLookup();

	vTaskDelay(10000);

	while (!bQuitTasks)
	{
		uint8_t SendBuffer[128];
		int SendSize = Network.GenerateNewPackets(SendBuffer);
		
		printf("Sending packet of size %d\n", SendSize);
		for (int i=0; i<SendSize; i++)
		{
			printf("%02x ", SendBuffer[i]);
		}
		printf("\n");

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
		printf("Send done\n");

		TickType_t WaitTime = 1000; // Start waiting for response, first long delay then anything else we might pick up
		uint32_t Packet[2];
		while (xQueueReceive(RecvEventQueue, Packet, WaitTime))
		{
			const int LengthOfPacket = Packet[1] - Packet[0];
			uint8_t PacketData[256];
			
			bool bGoodPacket = false;
			if (LengthOfPacket < sizeof(PacketData))
			{
				for (int PacketIndex = 0; PacketIndex < LengthOfPacket; PacketIndex++)
				{
					PacketData[PacketIndex] = RingBuff[(Packet[0]++) & RING_BUF_MASK];
				}
				bGoodPacket = (ValidatePacket(PacketData, LengthOfPacket));
			}
			
			if (bGoodPacket)
			{
				printf("Recieved good packet of length %d\n", LengthOfPacket);
				for (int i=0; i<LengthOfPacket; i++)
				{
					printf("%02x ", PacketData[i]);
				}
				printf("\n");

				ParsedPacket ProcessedPacket;
				ProcessedPacket.Parse(PacketData, LengthOfPacket - 2);
				ProcessedPacket.Dump(PacketData);
				
				Network.ProcessPacket(ProcessedPacket, PacketData);
			}
			else
			{
				printf("Recieved bad packet of length %d :(\n", LengthOfPacket);
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
