// Minimal EOS SDK declarations used by EDF6DirectNet.
// Layouts copied from the official EOS SDK 1.15.5 headers (eos_p2p_types.h / eos_common.h);
// the game ships EOS SDK 1.16.1, which accepts these ApiVersions unchanged.
#pragma once
#include <cstdint>

#pragma pack(push, 8)

typedef struct EOS_ProductUserIdDetails* EOS_ProductUserId;
typedef struct EOS_P2PHandle* EOS_HP2P;
typedef struct EOS_PlatformHandle* EOS_HPlatform;
typedef int32_t EOS_Bool;
typedef int32_t EOS_EResult;
typedef uint64_t EOS_NotificationId;

constexpr EOS_EResult EOS_Success = 0;
constexpr EOS_EResult EOS_NoConnection = 1;
constexpr EOS_EResult EOS_InvalidParameters = 10;
constexpr EOS_EResult EOS_NotFound = 18;
constexpr EOS_EResult EOS_LimitExceeded = 22;

constexpr uint32_t EOS_P2P_MAX_PACKET_SIZE = 1170;
constexpr int EOS_PRODUCTUSERID_MAX_LENGTH = 32;
constexpr int EOS_P2P_SOCKETID_SOCKETNAME_SIZE = 33;

struct EOS_P2P_SocketId {
    int32_t ApiVersion;  // 1
    char SocketName[EOS_P2P_SOCKETID_SOCKETNAME_SIZE];
};

enum EOS_EPacketReliability : int32_t {
    EOS_PR_UnreliableUnordered = 0,
    EOS_PR_ReliableUnordered = 1,
    EOS_PR_ReliableOrdered = 2,
};

struct EOS_P2P_SendPacketOptions {  // ApiVersion 3
    int32_t ApiVersion;
    EOS_ProductUserId LocalUserId;
    EOS_ProductUserId RemoteUserId;
    const EOS_P2P_SocketId* SocketId;
    uint8_t Channel;
    uint32_t DataLengthBytes;
    const void* Data;
    EOS_Bool bAllowDelayedDelivery;
    EOS_EPacketReliability Reliability;
    EOS_Bool bDisableAutoAcceptConnection;
};

struct EOS_P2P_ReceivePacketOptions {  // ApiVersion 2
    int32_t ApiVersion;
    EOS_ProductUserId LocalUserId;
    uint32_t MaxDataSizeBytes;
    const uint8_t* RequestedChannel;
};

struct EOS_P2P_AddNotifyOptions {  // Established / Interrupted / Closed, ApiVersion 1
    int32_t ApiVersion;
    EOS_ProductUserId LocalUserId;
    const EOS_P2P_SocketId* SocketId;
};

enum EOS_ENetworkConnectionType : int32_t {
    EOS_NCT_NoConnection = 0,
    EOS_NCT_DirectConnection = 1,
    EOS_NCT_RelayedConnection = 2,
};

struct EOS_P2P_OnPeerConnectionEstablishedInfo {
    void* ClientData;
    EOS_ProductUserId LocalUserId;
    EOS_ProductUserId RemoteUserId;
    const EOS_P2P_SocketId* SocketId;
    int32_t ConnectionType;  // 0 new, 1 reconnection
    EOS_ENetworkConnectionType NetworkType;
};

struct EOS_P2P_OnPeerConnectionInterruptedInfo {
    void* ClientData;
    EOS_ProductUserId LocalUserId;
    EOS_ProductUserId RemoteUserId;
    const EOS_P2P_SocketId* SocketId;
};

struct EOS_P2P_OnRemoteConnectionClosedInfo {
    void* ClientData;
    EOS_ProductUserId LocalUserId;
    EOS_ProductUserId RemoteUserId;
    const EOS_P2P_SocketId* SocketId;
    int32_t Reason;
};

struct EOS_P2P_QueryNATTypeOptions {  // ApiVersion 1
    int32_t ApiVersion;
};

struct EOS_P2P_OnQueryNATTypeCompleteInfo {
    EOS_EResult ResultCode;
    void* ClientData;
    int32_t NATType;  // 0 unknown, 1 open, 2 moderate, 3 strict
};

struct EOS_P2P_SetRelayControlOptions {  // ApiVersion 1
    int32_t ApiVersion;
    int32_t RelayControl;  // 0 no relays, 1 allow, 2 force
};

struct EOS_P2P_SetPortRangeOptions {  // ApiVersion 1
    int32_t ApiVersion;
    uint16_t Port;
    uint16_t MaxAdditionalPortsToTry;
};

struct EOS_P2P_SetPacketQueueSizeOptions {  // ApiVersion 1
    int32_t ApiVersion;
    uint64_t IncomingPacketQueueMaxSizeBytes;
    uint64_t OutgoingPacketQueueMaxSizeBytes;
};

struct EOS_P2P_GetPacketQueueInfoOptions {  // ApiVersion 1
    int32_t ApiVersion;
};

struct EOS_P2P_PacketQueueInfo {
    uint64_t IncomingPacketQueueMaxSizeBytes;
    uint64_t IncomingPacketQueueCurrentSizeBytes;
    uint64_t IncomingPacketQueueCurrentPacketCount;
    uint64_t OutgoingPacketQueueMaxSizeBytes;
    uint64_t OutgoingPacketQueueCurrentSizeBytes;
    uint64_t OutgoingPacketQueueCurrentPacketCount;
};

struct EOS_P2P_AddNotifyIncomingPacketQueueFullOptions {  // ApiVersion 1
    int32_t ApiVersion;
};

struct EOS_P2P_OnIncomingPacketQueueFullInfo {
    void* ClientData;
    uint64_t PacketQueueMaxSizeBytes;
    uint64_t PacketQueueCurrentSizeBytes;
    EOS_ProductUserId OverflowPacketLocalUserId;
    uint8_t OverflowPacketChannel;
    uint32_t OverflowPacketSizeBytes;
};

struct EOS_P2P_PeerConnectionOptions {  // AcceptConnection / CloseConnection, ApiVersion 1
    int32_t ApiVersion;
    EOS_ProductUserId LocalUserId;
    EOS_ProductUserId RemoteUserId;
    const EOS_P2P_SocketId* SocketId;
};

struct EOS_P2P_CloseConnectionsOptions {  // ApiVersion 1
    int32_t ApiVersion;
    EOS_ProductUserId LocalUserId;
    const EOS_P2P_SocketId* SocketId;
};

typedef struct EOS_LobbyHandle* EOS_HLobby;

struct EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions {  // ApiVersion 1
    int32_t ApiVersion;
};

struct EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo {
    void* ClientData;
    const char* LobbyId;
    EOS_ProductUserId TargetUserId;
    int32_t CurrentStatus;  // 0 joined, 1 left, 2 disconnected, 3 kicked, 4 promoted, 5 closed
};

#pragma pack(pop)

using EOS_Lobby_OnLobbyMemberStatusReceivedCallback = void (*)(const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo*);
using PFN_EOS_P2P_AcceptConnection = EOS_EResult (*)(EOS_HP2P, const EOS_P2P_PeerConnectionOptions*);
using PFN_EOS_P2P_CloseConnection = EOS_EResult (*)(EOS_HP2P, const EOS_P2P_PeerConnectionOptions*);
using PFN_EOS_P2P_CloseConnections = EOS_EResult (*)(EOS_HP2P, const EOS_P2P_CloseConnectionsOptions*);
using PFN_EOS_Lobby_AddNotifyMemberStatus = EOS_NotificationId (*)(
    EOS_HLobby, const EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions*, void*,
    EOS_Lobby_OnLobbyMemberStatusReceivedCallback);

using EOS_P2P_OnPeerConnectionEstablishedCallback = void (*)(const EOS_P2P_OnPeerConnectionEstablishedInfo*);
using EOS_P2P_OnPeerConnectionInterruptedCallback = void (*)(const EOS_P2P_OnPeerConnectionInterruptedInfo*);
using EOS_P2P_OnRemoteConnectionClosedCallback = void (*)(const EOS_P2P_OnRemoteConnectionClosedInfo*);
using EOS_P2P_OnQueryNATTypeCompleteCallback = void (*)(const EOS_P2P_OnQueryNATTypeCompleteInfo*);
using EOS_P2P_OnIncomingPacketQueueFullCallback = void (*)(const EOS_P2P_OnIncomingPacketQueueFullInfo*);

using PFN_EOS_Platform_GetP2PInterface = EOS_HP2P (*)(EOS_HPlatform);
using PFN_EOS_P2P_SendPacket = EOS_EResult (*)(EOS_HP2P, const EOS_P2P_SendPacketOptions*);
using PFN_EOS_P2P_ReceivePacket = EOS_EResult (*)(EOS_HP2P, const EOS_P2P_ReceivePacketOptions*, EOS_ProductUserId*,
                                                  EOS_P2P_SocketId*, uint8_t*, void*, uint32_t*);
using PFN_EOS_P2P_AddNotifyEstablished = EOS_NotificationId (*)(EOS_HP2P, const EOS_P2P_AddNotifyOptions*, void*,
                                                                EOS_P2P_OnPeerConnectionEstablishedCallback);
using PFN_EOS_P2P_AddNotifyInterrupted = EOS_NotificationId (*)(EOS_HP2P, const EOS_P2P_AddNotifyOptions*, void*,
                                                                EOS_P2P_OnPeerConnectionInterruptedCallback);
using PFN_EOS_P2P_AddNotifyClosed = EOS_NotificationId (*)(EOS_HP2P, const EOS_P2P_AddNotifyOptions*, void*,
                                                           EOS_P2P_OnRemoteConnectionClosedCallback);
using PFN_EOS_P2P_AddNotifyQueueFull = EOS_NotificationId (*)(EOS_HP2P,
                                                              const EOS_P2P_AddNotifyIncomingPacketQueueFullOptions*,
                                                              void*, EOS_P2P_OnIncomingPacketQueueFullCallback);
using PFN_EOS_P2P_QueryNATType = void (*)(EOS_HP2P, const EOS_P2P_QueryNATTypeOptions*, void*,
                                          EOS_P2P_OnQueryNATTypeCompleteCallback);
using PFN_EOS_P2P_SetRelayControl = EOS_EResult (*)(EOS_HP2P, const EOS_P2P_SetRelayControlOptions*);
using PFN_EOS_P2P_SetPortRange = EOS_EResult (*)(EOS_HP2P, const EOS_P2P_SetPortRangeOptions*);
using PFN_EOS_P2P_SetPacketQueueSize = EOS_EResult (*)(EOS_HP2P, const EOS_P2P_SetPacketQueueSizeOptions*);
using PFN_EOS_P2P_GetPacketQueueInfo = EOS_EResult (*)(EOS_HP2P, const EOS_P2P_GetPacketQueueInfoOptions*,
                                                      EOS_P2P_PacketQueueInfo*);
using PFN_EOS_ProductUserId_ToString = EOS_EResult (*)(EOS_ProductUserId, char*, int32_t*);
using PFN_EOS_ProductUserId_FromString = EOS_ProductUserId (*)(const char*);
using PFN_EOS_EResult_ToString = const char* (*)(EOS_EResult);
