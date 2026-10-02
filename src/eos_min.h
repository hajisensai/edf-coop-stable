// Minimal EOS SDK declarations used by EDF6DirectNet.
// Layouts copied from the official EOS SDK 1.15.5 headers (eos_p2p_types.h / eos_common.h);
// the game ships EOS SDK 1.16.1, which accepts these ApiVersions unchanged.
#pragma once
#include <cstddef>
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

// Lobby member attributes (eos_lobby_types.h). EDF.dll imports none of the member-attribute
// functions, so an attribute the plugin sets on its own member is invisible to the game.
typedef struct EOS_LobbyModificationHandle* EOS_HLobbyModification;
typedef struct EOS_LobbyDetailsHandle* EOS_HLobbyDetails;

struct EOS_Lobby_CreateLobbyOptionsHead {  // leading fields of EOS_Lobby_CreateLobbyOptions
    int32_t ApiVersion;
    EOS_ProductUserId LocalUserId;
};

struct EOS_Lobby_JoinLobbyOptionsHead {  // leading fields of EOS_Lobby_JoinLobbyOptions
    int32_t ApiVersion;
    EOS_HLobbyDetails LobbyDetailsHandle;
    EOS_ProductUserId LocalUserId;
};

struct EOS_Lobby_LobbyIdCallbackInfo {  // Create/Join/UpdateLobby callback info share this layout
    EOS_EResult ResultCode;
    void* ClientData;
    const char* LobbyId;
};

struct EOS_Lobby_UpdateLobbyModificationOptions {  // ApiVersion 1
    int32_t ApiVersion;
    EOS_ProductUserId LocalUserId;
    const char* LobbyId;
};

struct EOS_Lobby_UpdateLobbyOptions {  // ApiVersion 1
    int32_t ApiVersion;
    EOS_HLobbyModification LobbyModificationHandle;
};

struct EOS_Lobby_CopyLobbyDetailsHandleOptions {  // ApiVersion 1
    int32_t ApiVersion;
    const char* LobbyId;
    EOS_ProductUserId LocalUserId;
};

struct EOS_Lobby_AttributeData {  // ApiVersion 1
    int32_t ApiVersion;
    const char* Key;
    union {
        int64_t AsInt64;
        double AsDouble;
        EOS_Bool AsBool;
        const char* AsUtf8;
    } Value;
    int32_t ValueType;  // 0 bool, 1 int64, 2 double, 3 string
};

struct EOS_Lobby_Attribute {  // ApiVersion 1
    int32_t ApiVersion;
    EOS_Lobby_AttributeData* Data;
    int32_t Visibility;  // 0 public, 1 private
};

struct EOS_LobbyModification_AddMemberAttributeOptions {  // ApiVersion 1
    int32_t ApiVersion;
    const EOS_Lobby_AttributeData* Attribute;
    int32_t Visibility;
};

struct EOS_LobbyDetails_CopyMemberAttributeByKeyOptions {  // ApiVersion 1
    int32_t ApiVersion;
    EOS_ProductUserId TargetUserId;
    const char* AttrKey;
};

struct EOS_LobbyDetails_GetLobbyOwnerOptions {  // ApiVersion 1
    int32_t ApiVersion;
};

struct EOS_Lobby_AddNotifyLobbyMemberUpdateReceivedOptions {  // ApiVersion 1
    int32_t ApiVersion;
};

struct EOS_Lobby_LobbyMemberUpdateReceivedCallbackInfo {
    void* ClientData;
    const char* LobbyId;
    EOS_ProductUserId TargetUserId;
};

struct EOS_Lobby_PromoteMemberOptions {  // ApiVersion 1
    int32_t ApiVersion;
    const char* LobbyId;
    EOS_ProductUserId LocalUserId;
    EOS_ProductUserId TargetUserId;
};

// EOS_LobbyDetails_Info at ApiVersion 3 (SDK 1.16): EDF.dll reads it only at that version.
struct EOS_LobbyDetails_Info {
    int32_t ApiVersion;
    const char* LobbyId;
    EOS_ProductUserId LobbyOwnerUserId;
    int32_t PermissionLevel;
    uint32_t AvailableSlots;
    uint32_t MaxMembers;
    EOS_Bool bAllowInvites;
    const char* BucketId;
    EOS_Bool bAllowHostMigration;
    EOS_Bool bRTCRoomEnabled;
    EOS_Bool bAllowJoinById;
    EOS_Bool bRejoinAfterKickRequiresInvite;
    EOS_Bool bPresenceEnabled;
    const uint32_t* AllowedPlatformIds;
    uint32_t AllowedPlatformIdsCount;
};
constexpr int32_t EOS_LOBBYDETAILS_INFO_API_LATEST = 3;

struct EOS_LobbyDetails_CopyInfoOptions {  // ApiVersion 1
    int32_t ApiVersion;
};
struct EOS_LobbyDetails_GetAttributeCountOptions {  // ApiVersion 1
    int32_t ApiVersion;
};
struct EOS_LobbyDetails_CopyAttributeByIndexOptions {  // ApiVersion 1
    int32_t ApiVersion;
    uint32_t AttrIndex;
};

typedef struct EOS_LobbySearchHandle* EOS_HLobbySearch;
struct EOS_LobbySearch_FindOptions {  // ApiVersion 1
    int32_t ApiVersion;
    EOS_ProductUserId LocalUserId;
};
struct EOS_LobbySearch_FindCallbackInfo {
    EOS_EResult ResultCode;
    void* ClientData;
};
struct EOS_LobbySearch_GetSearchResultCountOptions {  // ApiVersion 1
    int32_t ApiVersion;
};
struct EOS_LobbySearch_CopySearchResultByIndexOptions {  // ApiVersion 1
    int32_t ApiVersion;
    uint32_t LobbyIndex;
};
struct EOS_LobbySearch_SetLobbyIdOptions {  // ApiVersion 1
    int32_t ApiVersion;
    const char* LobbyId;
};

#pragma pack(pop)

static_assert(offsetof(EOS_LobbyDetails_Info, MaxMembers) == 0x20, "EOS_LobbyDetails_Info");
static_assert(offsetof(EOS_LobbyDetails_Info, AllowedPlatformIdsCount) == 0x50, "EOS_LobbyDetails_Info");

using EOS_LobbySearch_OnFindCallback = void (*)(const EOS_LobbySearch_FindCallbackInfo*);
using PFN_EOS_LobbySearch_Find = void (*)(EOS_HLobbySearch, const EOS_LobbySearch_FindOptions*, void*,
                                          EOS_LobbySearch_OnFindCallback);
using PFN_EOS_LobbySearch_GetSearchResultCount = uint32_t (*)(EOS_HLobbySearch,
                                                              const EOS_LobbySearch_GetSearchResultCountOptions*);
using PFN_EOS_LobbySearch_CopySearchResultByIndex = EOS_EResult (*)(
    EOS_HLobbySearch, const EOS_LobbySearch_CopySearchResultByIndexOptions*, EOS_HLobbyDetails*);
using PFN_EOS_LobbySearch_SetLobbyId = EOS_EResult (*)(EOS_HLobbySearch, const EOS_LobbySearch_SetLobbyIdOptions*);
using PFN_EOS_LobbySearch_Release = void (*)(EOS_HLobbySearch);
using PFN_EOS_LobbyDetails_CopyInfo = EOS_EResult (*)(EOS_HLobbyDetails, const EOS_LobbyDetails_CopyInfoOptions*,
                                                      EOS_LobbyDetails_Info**);
using PFN_EOS_LobbyDetails_Info_Release = void (*)(EOS_LobbyDetails_Info*);
using PFN_EOS_LobbyDetails_GetAttributeCount = uint32_t (*)(EOS_HLobbyDetails,
                                                            const EOS_LobbyDetails_GetAttributeCountOptions*);
using PFN_EOS_LobbyDetails_CopyAttributeByIndex = EOS_EResult (*)(
    EOS_HLobbyDetails, const EOS_LobbyDetails_CopyAttributeByIndexOptions*, EOS_Lobby_Attribute**);

using EOS_Lobby_OnLobbyIdCallback = void (*)(const EOS_Lobby_LobbyIdCallbackInfo*);
using EOS_Lobby_OnLobbyMemberUpdateReceivedCallback = void (*)(const EOS_Lobby_LobbyMemberUpdateReceivedCallbackInfo*);
using PFN_EOS_Lobby_CreateLobby = void (*)(EOS_HLobby, const EOS_Lobby_CreateLobbyOptionsHead*, void*,
                                           EOS_Lobby_OnLobbyIdCallback);
using PFN_EOS_Lobby_JoinLobby = void (*)(EOS_HLobby, const EOS_Lobby_JoinLobbyOptionsHead*, void*,
                                         EOS_Lobby_OnLobbyIdCallback);
// LeaveLobby / DestroyLobby: only the call itself matters to us, so options and callback stay opaque.
using PFN_EOS_Lobby_LeaveOrDestroy = void (*)(EOS_HLobby, const void*, void*, void*);
using PFN_EOS_Lobby_UpdateLobbyModification = EOS_EResult (*)(EOS_HLobby, const EOS_Lobby_UpdateLobbyModificationOptions*,
                                                              EOS_HLobbyModification*);
using PFN_EOS_Lobby_UpdateLobby = void (*)(EOS_HLobby, const EOS_Lobby_UpdateLobbyOptions*, void*,
                                           EOS_Lobby_OnLobbyIdCallback);
using PFN_EOS_LobbyModification_AddMemberAttribute = EOS_EResult (*)(EOS_HLobbyModification,
                                                                     const EOS_LobbyModification_AddMemberAttributeOptions*);
using PFN_EOS_LobbyModification_Release = void (*)(EOS_HLobbyModification);
using PFN_EOS_Lobby_CopyLobbyDetailsHandle = EOS_EResult (*)(EOS_HLobby, const EOS_Lobby_CopyLobbyDetailsHandleOptions*,
                                                             EOS_HLobbyDetails*);
using PFN_EOS_LobbyDetails_CopyMemberAttributeByKey = EOS_EResult (*)(
    EOS_HLobbyDetails, const EOS_LobbyDetails_CopyMemberAttributeByKeyOptions*, EOS_Lobby_Attribute**);
// Its callback info (EOS_Lobby_PromoteMemberCallbackInfo) has the EOS_Lobby_LobbyIdCallbackInfo layout.
using PFN_EOS_Lobby_PromoteMember = void (*)(EOS_HLobby, const EOS_Lobby_PromoteMemberOptions*, void*,
                                             EOS_Lobby_OnLobbyIdCallback);
using PFN_EOS_LobbyDetails_GetLobbyOwner = EOS_ProductUserId (*)(EOS_HLobbyDetails,
                                                                 const EOS_LobbyDetails_GetLobbyOwnerOptions*);
struct EOS_LobbyDetails_GetMemberAttributeCountOptions {  // ApiVersion 1
    int32_t ApiVersion;
    EOS_ProductUserId TargetUserId;
};
struct EOS_LobbyDetails_GetMemberCountOptions {  // ApiVersion 1
    int32_t ApiVersion;
};
using PFN_EOS_LobbyDetails_GetMemberAttributeCount = uint32_t (*)(EOS_HLobbyDetails,
                                                                  const EOS_LobbyDetails_GetMemberAttributeCountOptions*);
using PFN_EOS_LobbyDetails_GetMemberCount = uint32_t (*)(EOS_HLobbyDetails, const EOS_LobbyDetails_GetMemberCountOptions*);
struct EOS_LobbyDetails_GetMemberByIndexOptions {  // ApiVersion 1
    int32_t ApiVersion;
    uint32_t MemberIndex;
};
using PFN_EOS_LobbyDetails_GetMemberByIndex = EOS_ProductUserId (*)(EOS_HLobbyDetails,
                                                                    const EOS_LobbyDetails_GetMemberByIndexOptions*);
struct EOS_LobbyDetails_CopyMemberAttributeByIndexOptions {  // ApiVersion 1
    int32_t ApiVersion;
    EOS_ProductUserId TargetUserId;
    uint32_t AttrIndex;
};
using PFN_EOS_LobbyDetails_CopyMemberAttributeByIndex = EOS_EResult (*)(
    EOS_HLobbyDetails, const EOS_LobbyDetails_CopyMemberAttributeByIndexOptions*, EOS_Lobby_Attribute**);
struct EOS_Lobby_KickMemberOptions {  // ApiVersion 1
    int32_t ApiVersion;
    const char* LobbyId;
    EOS_ProductUserId LocalUserId;
    EOS_ProductUserId TargetUserId;
};
using PFN_EOS_Lobby_KickMember = void (*)(EOS_HLobby, const EOS_Lobby_KickMemberOptions*, void*, void*);
using PFN_EOS_LobbyDetails_Release = void (*)(EOS_HLobbyDetails);
using PFN_EOS_Lobby_Attribute_Release = void (*)(EOS_Lobby_Attribute*);
using PFN_EOS_Lobby_AddNotifyMemberUpdate = EOS_NotificationId (*)(
    EOS_HLobby, const EOS_Lobby_AddNotifyLobbyMemberUpdateReceivedOptions*, void*,
    EOS_Lobby_OnLobbyMemberUpdateReceivedCallback);

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
