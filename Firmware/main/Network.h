#pragma once

#include "PacketBuilder.h"
#include "PacketParser.h"

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
	NetworkState();
	int GenerateNewPackets(uint8_t* Buffer);
	void ProcessPacket(const PacketParser& Packet, const uint8_t* Data);

private:
	EState State = StateXID;
	SNAStream Stream;
	uint16_t ProcessedPacket = 0xFFFF;
	bool bSendReadyToRecieve = false;
};
