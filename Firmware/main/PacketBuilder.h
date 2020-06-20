#pragma once

#include <stdint.h>
#include <string.h>
#include <initializer_list>

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
	SDLCPacket(RawStream &Stream)
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
	static constexpr uint8_t FinalFlag = 0x10;

	RawStream &Raw;
};

class SDLCReadyToRecieve : public SDLCPacket
{
public:
	SDLCReadyToRecieve(SDLCInfoStream &Stream, bool bFinal = true)
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
	SDLCSetNormalResponseMode(SDLCInfoStream &Stream, bool bFinal = true)
		: SDLCPacket(Stream)
	{
		SendData(0x83 + (bFinal ? FinalFlag : 0));
	}
};

class SDLCRequestXID : public SDLCPacket
{
public:
	SDLCRequestXID(SDLCInfoStream &Stream, bool bFinal = true)
		: SDLCPacket(Stream)
	{
		SendData(0xAF + (bFinal ? FinalFlag : 0));
	}
};

class SDLCInfoPacket : public SDLCPacket
{
public:
	SDLCInfoPacket(SDLCInfoStream &Stream, bool bFinal = false)
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
	TranmissionPacket(SNAStream &Stream, uint8_t Destination, uint8_t Source, bool bExpedited = false, bool bFinal = false, int SequenceOverride = -1)
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
	RequestPacket(SNAStream &Stream, uint8_t Category, uint8_t Destination, uint8_t Source, bool bExpedited = false, bool bFinal = false, bool bChangeDirection = false, bool bStartOfBracket = true)
		: TranmissionPacket(Stream, Destination, Source, bExpedited, bFinal)
	{
		SendData((Category << 5) | 3);					   // Category + Start+end of chain
		SendData(0x80 + (bChangeDirection ? 0x20 : 0x00)); // Definitive response required
		SendData(bStartOfBracket ? 0xC0 : 0x40);		   // Start and end of bracket
	}
};

class PositiveResponsePacket : public TranmissionPacket
{
public:
	PositiveResponsePacket(SNAStream &Stream, uint8_t Category, uint8_t Destination, uint8_t Source, int SequenceOverride, bool bExpedited = false, bool bFinal = false)
		: TranmissionPacket(Stream, Destination, Source, bExpedited, bFinal, SequenceOverride)
	{
		SendData((Category << 5) | 0x83); // Category + Response
		SendData(0x80);					  // Definitive response + Positive
		SendData(0x00);
	}
};