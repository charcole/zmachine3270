#pragma once

#include <stdint.h>

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

extern char EBCDICToASCII[256];
extern char ASCIIToEBCDIC[256];

void BuildTextTables();
