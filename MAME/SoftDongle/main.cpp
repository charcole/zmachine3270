#include "zops.h"
#include "FrontEnd.h"
#include "TN3270.h"
#include "crc.h"
#include "3270.h"
#include "Network.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DUMP_PACKETS 1

static NetworkState Network;

void* GameTask(void *Args)
{
	FrontEnd SelectionScreen;
	int CurrentGame = SelectionScreen.Show(&GScreen);
	while (CurrentGame < 0)
	{
		if (CurrentGame == -1)
		{
			TN3270::Run();
		}
		CurrentGame = SelectionScreen.Show(&GScreen);
	}
	FILE* f = fopen("game.z", "rb");
	if (!f)
	{
		printf("Couldn't find \"game.z\". Can be almost any Z3,Z4 or Z5 game. Quitting\n");
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	int GameSize = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* GameData = malloc(GameSize);
	fread(GameData, GameSize, 1, f);
	fclose(f);
	zopsMain((char*)GameData);
	free(GameData);
	return NULL;
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

int CurrentPacketByte = 0x80;
int CurrentPacketIndex = 0;
uint8_t PacketData[4096];
int RunCount = 0;

bool RecvBit(bool bBit)
{
	if (bBit)
	{
		RunCount++;
	}
	else
	{
		int RunLength = RunCount;
		RunCount = 0;

		if (RunLength >= 6)
		{
			CurrentPacketByte = 0x80;
			CurrentPacketIndex = 0;
			return (RunLength == 6);
		}
		else if (RunLength >= 5)
		{
			// If 5 then needs to be dropped
			return false;
		}
	}

	bool bFlush = ((CurrentPacketByte & 1) != 0);
	CurrentPacketByte >>= 1;
	CurrentPacketByte |= (bBit ? 0x80 : 0);
	if (bFlush)
	{
		if (CurrentPacketIndex < sizeof(PacketData))
		{
			PacketData[CurrentPacketIndex] = CurrentPacketByte;
		}
		CurrentPacketIndex++;
		CurrentPacketByte = 0x80;
	}
	return false;
}

int main(int argc, char** argv)
{
	if (argc != 3)
	{
		printf("Usage 3270.out ip_addr port\n");
		return 1;
	}

	int Socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Socket == -1)
    {
        printf("Failed to create socket\n");
        return 1;
    }

	int port = atoi(argv[2]);
	printf("Connecting to %s:%d\n", argv[1], port);

    struct sockaddr_in DestAddr;
    DestAddr.sin_addr.s_addr = inet_addr(argv[1]);
    DestAddr.sin_family = AF_INET;
    DestAddr.sin_port = htons(port);

    if (connect(Socket, (struct sockaddr *)&DestAddr, sizeof(DestAddr)) == -1)
    {
        close(Socket);
        printf("Failed to connect socket\n");
        return 1;
    }

	BuildCRCLookup();
	BuildTextTables();

	pthread_t GameThread;
	void *Ret;
	pthread_create(&GameThread, NULL, GameTask, NULL);

	while (true)
	{
		uint8_t SendBuffer[2048+1024];
		bool bWaitForReply = false;
		int SendSize = Network.GenerateNewPackets(SendBuffer, bWaitForReply);

#if DUMP_PACKETS
		printf("Sending packet of size %d:", SendSize);
		for (int i=0; i<SendSize; i++)
		{
			printf(" %02x", SendBuffer[i]);
		}
		printf("\n");
#endif

		CBitStream A(SendBuffer, SendSize);
		CBitStream B;

		A.Flush();
		CalculateCRC(B, A);
		ZeroBitAndFlagInsertion(A, B);

		int NumBitsToSend = 0;
		const uint8_t* BytesToSend = A.ToBytes(NumBitsToSend);
		send(Socket, BytesToSend, (NumBitsToSend+7)/8, 0);

		fd_set RecvFdSet;
		FD_ZERO(&RecvFdSet);
		FD_SET(Socket, &RecvFdSet);
		timeval TimeVal;
		TimeVal.tv_sec=1;
		TimeVal.tv_usec=0;
		int SelectReturn = select(Socket + 1, &RecvFdSet, nullptr, nullptr, &TimeVal);
		if (SelectReturn > 0 && FD_ISSET(Socket, &RecvFdSet))
		{
			uint8_t RX[2048];
			int NumRX = recv(Socket, RX, sizeof(RX), 0);
			if (NumRX == 0)
			{
				printf("recv returned zero. Connection closed?\n");
				return 1;
			}
			else if (NumRX < 0)
			{
				printf("recv error: %d errno:%d\n", NumRX, errno);
				return 1;
			}
			CBitStream ReceivedBits(RX, NumRX);
			while (!ReceivedBits.Finished())
			{
				bool Bit = ReceivedBits.ReadBit();
				int LengthOfPacket = CurrentPacketIndex;
				if (RecvBit(Bit))
				{
					if (LengthOfPacket < sizeof(PacketData))
					{
#if DUMP_PACKETS
						printf("Recieved packet of length %d:", LengthOfPacket);
						for (int i=0; i<LengthOfPacket; i++)
						{
							printf(" %02x", PacketData[i]);
						}
						printf("\n");
#endif

						PacketParser ProcessedPacket;
						ProcessedPacket.Parse(PacketData, LengthOfPacket);
#if DUMP_PACKETS
						ProcessedPacket.Dump(PacketData);
#endif
						Network.ProcessPacket(ProcessedPacket, PacketData);
					}
					else
					{
						printf("Packet over max length %d :(\n", LengthOfPacket);
					}
				}
			}
		}
	}

	pthread_join(GameThread, &Ret);
        
	close(Socket);

	return 0;
}
