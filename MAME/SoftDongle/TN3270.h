#pragma once

#include <stdint.h>

class TN3270
{
public:
    
    static void Run();

private:

	bool Connect();
	void Disconnect();
	void Process();
	uint8_t RecvByte();
	void SendFlush();
	void SendByte(uint8_t Byte);

private:

	uint8_t DataStream[4096];
	uint8_t* DataStreamPtr = DataStream;
	uint8_t RX[256];
	uint8_t TX[256];
	int RecvPtr = 0;
	int RecvEnd = 0;
	int SendPtr = 0;
	int SocketFD = -1;
	bool bError = false;
};
