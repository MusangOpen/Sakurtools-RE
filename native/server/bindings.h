#pragma once

#include "proxy.h"

namespace proxy_server {

// Global JNI references and IDs are retained for the resident DLL's lifetime.
// Session shutdown releases connections and players, but keeps these bindings.
struct JavaBindings {
    jclass initClass = nullptr;
    jmethodID initCtor = nullptr;
    jclass handlerClass = nullptr;
    jmethodID handlerCtor = nullptr;
    jclass mainGateClass = nullptr;
    jmethodID mainGateCtor = nullptr;
    jclass connectionCls = nullptr;
    jmethodID connectionConfigureSerMid = nullptr;
    jmethodID connectionSendMid = nullptr;
    jfieldID connectionAttrProtocolFid = nullptr;
    jobject protoHandshaking = nullptr;
    jobject protoLogin = nullptr;
    jobject protoPlay = nullptr;
    jobject protoStatus = nullptr;
    jobject statusResponse = nullptr;
    jobject statusClassLoader = nullptr;
    jmethodID mcGetCurrentServerMid = nullptr;
    jfieldID serverDataNameFid = nullptr;
    jfieldID serverDataAddressFid = nullptr;
    jmethodID contextChannelMid = nullptr;
    jclass pongResponsePacketCls = nullptr;
    jmethodID pongResponsePacketCtor = nullptr;
    jclass statusRequestPacketCls = nullptr;
    jclass pingRequestPacketCls = nullptr;
    jfieldID pingRequestPacketTimeFid = nullptr;
    jfieldID intentionPacketIntentFid = nullptr;
    jobject flowServerbound = nullptr;
    jobject flowClientbound = nullptr;
    jclass channelCls = nullptr;
    jmethodID channelPipelineMid = nullptr;
    jmethodID channelWriteAndFlushMid = nullptr;
    jmethodID channelAttrMid = nullptr;
    jmethodID channelConfigMid = nullptr;
    jmethodID channelCloseMid = nullptr;
    jfieldID connectionChannelFid = nullptr;
    jmethodID configSetOptionMid = nullptr;
    jobject tcpNoDelayOption = nullptr;
    jobject booleanTrue = nullptr;
    jclass pipelineCls = nullptr;
    jmethodID pipelineAddLastMid = nullptr;
    jmethodID pipelineRemoveNameMid = nullptr;
    jmethodID pipelineGetHandlerMid = nullptr;
    jclass attributeCls = nullptr;
    jmethodID attributeSetMid = nullptr;
    jclass bundlerInfoCls = nullptr;
    jfieldID bundlerProviderFid = nullptr;
    jclass gameProfileCls = nullptr;
    jmethodID gameProfileCtor = nullptr;
    jclass minecraftCls = nullptr;
    jmethodID mcGetInstanceMid = nullptr;
    jmethodID mcGetProfilePropsMid = nullptr;
    jfieldID mcGameModeFid = nullptr;
    jclass friendlyBufCls = nullptr;
    jmethodID friendlyBufCtor = nullptr;
    jmethodID fbbWriteByteMid = nullptr;
    jmethodID fbbWriteBooleanMid = nullptr;
    jmethodID fbbWriteVarIntMid = nullptr;
    jmethodID fbbWriteUUIDMid = nullptr;
    jmethodID fbbWriteUtfMid = nullptr;
    jmethodID fbbWriteGpPropsMid = nullptr;
    jclass unpooledCls = nullptr;
    jmethodID unpooledBufferMid = nullptr;
    jclass playerInfoUpdatePacketCls = nullptr;
    jmethodID playerInfoUpdatePacketBufCtor = nullptr;
    jmethodID playerInfoUpdatePacketWriteMid = nullptr;
    jclass playerInfoEntryClsUnused = nullptr;
    jmethodID playerInfoUpdatePacketCtor = nullptr; // (EnumSet, Collection)V
    jfieldID playerInfoUpdatePacketEntriesFid = nullptr;
    jclass playerInfoActionCls = nullptr; // ClientboundPlayerInfoUpdatePacket$Action
    jmethodID enumSetNoneOfMid = nullptr;
    jmethodID collectionAddMid = nullptr;
    jmethodID piuEntriesMidA = nullptr;
    jmethodID piuEntriesMidB = nullptr;
    jclass piEntryCls = nullptr;
    jmethodID piEntryProfileIdMid = nullptr;
    jmethodID piEntryGameModeMid = nullptr;
    jmethodID piEntryLatencyMid = nullptr;
    jmethodID piEntryDisplayNameMid = nullptr;
    jmethodID gameTypeGetIdMid = nullptr;
    jmethodID fbbWriteComponentMid = nullptr;
    jmethodID listSizeMid = nullptr;
    jmethodID listGetMid = nullptr;
    jmethodID byteBufGetByteMid = nullptr;
    jclass customPayloadPacketCls = nullptr;
    jclass setPlayerTeamPacketCls = nullptr;
    jmethodID setPlayerTeamCtor = nullptr;
    jfieldID setPlayerTeamParametersFid = nullptr;
    jfieldID setPlayerTeamMethodFid = nullptr;
    jfieldID setPlayerTeamNameFid = nullptr;
    jfieldID setPlayerTeamPlayersFid = nullptr;
    jmethodID userGetGameProfileMid = nullptr;
    jmethodID gameProfileGetNameMid = nullptr;
    jclass userCls = nullptr;
    jmethodID userGetProfileIdMid = nullptr;
    jmethodID mcGetUserMid = nullptr;
    jmethodID uuidGetMsbMid = nullptr;
    jmethodID uuidGetLsbMid = nullptr;
    jclass loginFinishedPacketCls = nullptr;
    jmethodID loginFinishedPacketCtor = nullptr;
    jclass helloPacketCls = nullptr;
    jfieldID helloPacketNameFid = nullptr;
    jclass intentPacketCls = nullptr;
    jclass uuidCls = nullptr;
    jmethodID uuidNameUuidFromBytesMid = nullptr;
    jclass bundlePacketCls = nullptr;
    jmethodID bundleSubPacketsMid = nullptr;
    jmethodID iterableIteratorMid = nullptr;
    jmethodID iteratorHasNextMid = nullptr;
    jmethodID iteratorNextMid = nullptr;
    jmethodID mcGetConnectionMid = nullptr;
    jmethodID cplGetConnectionMid = nullptr;
    jmethodID teleportAckId = nullptr;

    // --- 1.21.9+ networking: codecs are ProtocolInfo objects, not ConnectionProtocol attrs. ---
    // modernNetworking is true when the runtime exposes ProtocolInfo plus PacketEncoder/
    // PacketDecoder(ProtocolInfo); 1.20.1 keeps the protocol attribute + setProtocol path.
    bool modernNetworking = false;
    bool configurationPhase = false;
    jclass protocolInfoCls = nullptr;
    jclass packetEncoderCls = nullptr;
    jmethodID packetEncoderCtor = nullptr;
    jclass packetDecoderCls = nullptr;
    jmethodID packetDecoderCtor = nullptr;
    jclass simpleUnboundProtocolCls = nullptr;
    jmethodID simpleUnboundBindMid = nullptr;
    jclass unboundProtocolCls = nullptr;
    jmethodID unboundBindMid = nullptr;
    jclass registryFriendlyBufCls = nullptr;
    jmethodID registryBufDecoratorMid = nullptr;
    jmethodID connectionConfigureSerializationModernMid = nullptr; // (ChannelPipeline, PacketFlow, Z,
                                                                   // BandwidthDebugMonitor)V
    jmethodID pipelineReplaceMid = nullptr; // (String, String, ChannelHandler)ChannelPipeline
    jclass gameProtocolsCls = nullptr;
    jfieldID gameServerboundTemplateFid = nullptr;
    jfieldID gameClientboundTemplateFid = nullptr;
    jclass loginProtocolsCls = nullptr;
    jfieldID loginServerboundFid = nullptr;
    jfieldID loginClientboundFid = nullptr;
    jclass configProtocolsCls = nullptr;
    jfieldID configServerboundFid = nullptr;
    jfieldID configClientboundFid = nullptr;
    jclass handshakeProtocolsCls = nullptr;
    jfieldID handshakeServerboundFid = nullptr;
    jclass statusProtocolsCls = nullptr;
    jfieldID statusServerboundFid = nullptr;
    jfieldID statusClientboundFid = nullptr;

    // --- 1.20.2+ configuration phase ---
    jobject protoConfiguration = nullptr;
    jclass loginAckPacketCls = nullptr; // ServerboundLoginAcknowledgedPacket: the phase switch trigger
    jclass registryDataPacketCls = nullptr;
    jmethodID registryDataPacketCtor = nullptr; // (ResourceKey, List)V
    jclass enabledFeaturesPacketCls = nullptr;
    jmethodID enabledFeaturesPacketCtor = nullptr; // (Set)V
    jclass finishConfigurationPacketCls = nullptr;
    jclass serverboundFinishConfigurationCls = nullptr;
    jfieldID featureFlagsRegistryFid = nullptr;  // FeatureFlags.REGISTRY
    jmethodID featureFlagRegistryToNamesMid = nullptr; // toNames(FeatureFlagSet)Set
    jobject intentLogin = nullptr;               // ClientIntent.LOGIN (1.20.2+ handshake enum)
    jobject intentStatus = nullptr;              // ClientIntent.STATUS

    // --- packet construction helpers that changed between 1.20.1 and 1.21.10 ---
    bool hasCommonSpawnInfo = false; // 1.20.2+ ClientboundLoginPacket carries CommonPlayerSpawnInfo
    jclass positionMoveRotationCls = nullptr;
    jmethodID positionMoveRotationOfEntityMid = nullptr; // static of(Entity)
    jclass respawnDataCls = nullptr;
    jmethodID respawnDataOfMid = nullptr; // static of(ResourceKey, BlockPos, F, F)
    jmethodID levelGetRespawnDataMid = nullptr;
    jclass entityTypeCls = nullptr;
    jfieldID entityTypePlayerFid = nullptr;
    jclass addEntityPacketCls = nullptr;
    jmethodID addEntityPacketCtor = nullptr; // (I UUID DDDFF EntityType I Vec3 D)V
    jclass addPlayerPacketCls = nullptr;     // 1.20.1 only; 1.21.9 folds players into AddEntity
    jclass playerInfoEntryCls = nullptr;
    jmethodID playerInfoEntryCtorModern = nullptr; // (UUID, GameProfile, Z, I, GameType, Component, Z, I, Data)V
    jmethodID playerInfoEntryCtorLegacy = nullptr; // (UUID, GameProfile, Z, I, GameType, Component, Data)V
    jmethodID playerInfoEntryGameModeMid = nullptr;
    jmethodID playerInfoEntryLatencyMid = nullptr;
    jmethodID playerInfoEntryDisplayNameMid = nullptr;
    jmethodID cplEnabledFeaturesMid = nullptr; // ClientPacketListener.enabledFeatures()FeatureFlagSet
    jmethodID cplRegistryAccessMid = nullptr;  // ClientPacketListener.registryAccess()RegistryAccess

    // --- registry/tag synchronization used to rebuild A's configuration view for B ---
    jclass gameContextCls = nullptr;      // GameProtocols$Context
    jmethodID gameContextInfiniteMid = nullptr;
    jclass biConsumerCls = nullptr;       // java.util.function.BiConsumer
    jclass enumSetCls = nullptr;          // java.util.EnumSet
    jclass tagSerializationCls = nullptr; // net.minecraft.tags.TagNetworkSerialization
    jmethodID featureFlagToNamesMid = nullptr;
    jclass nbtOpsCls = nullptr;
    jfieldID nbtOpsInstanceFid = nullptr;
    jmethodID registryAccessSerializationContextMid = nullptr; // createSerializationContext(DynamicOps)RegistryOps
    jclass registryOpsCls = nullptr;                           // fallback binder for the same value
    jmethodID registryOpsCreateMid = nullptr;
    jclass registrySynchronizationCls = nullptr;
    jmethodID packRegistriesMid = nullptr;      // packRegistries(DynamicOps, RegistryAccess, Set, BiConsumer)V
    jmethodID isNetworkableMid = nullptr;       // isNetworkable(ResourceKey)Z
    jclass updateTagsPacketCls = nullptr;       // ClientboundUpdateTagsPacket
    jmethodID updateTagsPacketCtor = nullptr;
    jmethodID serializeTagsToNetworkMid = nullptr; // TagNetworkSerialization.serializeToNetwork(Registry)NetworkPayload
    jmethodID registryAccessRegistriesMid = nullptr; // RegistryAccess.registries()Stream
    jmethodID registryEntryKeyMid = nullptr;
    jmethodID registryEntryValueMid = nullptr;
    jmethodID streamIteratorMid = nullptr;
    jclass hashMapCls = nullptr;
    jmethodID hashMapCtor = nullptr;
    jmethodID mapPutMid = nullptr;
};

} // namespace proxy_server
