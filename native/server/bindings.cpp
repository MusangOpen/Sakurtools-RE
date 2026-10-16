#include "internal.h"
#include "mapped_method.h"
#include "status_response.h"
#include "world_snapshot.h"

namespace proxy_server {

std::u16string currentServerMotd(JNIEnv* env) {
    std::u16string name, address;
    if (env->PushLocalFrame(8) != JNI_OK) {
        env->ExceptionClear();
        return FormatServerMotd(name, address);
    }
    if (server.refs.minecraftCls && server.refs.mcGetInstanceMid && server.refs.mcGetCurrentServerMid) {
        jobject mc = env->CallStaticObjectMethod(server.refs.minecraftCls, server.refs.mcGetInstanceMid);
        if (mc && !env->ExceptionCheck()) {
            jobject data = env->CallObjectMethod(mc, server.refs.mcGetCurrentServerMid);
            if (data && !env->ExceptionCheck()) {
                auto read = [&](jfieldID field) -> std::u16string {
                    if (!field || env->ExceptionCheck())
                        return {};
                    jstring value = static_cast<jstring>(env->GetObjectField(data, field));
                    if (!value || env->ExceptionCheck())
                        return {};
                    const jsize length = env->GetStringLength(value);
                    const jchar* chars = env->GetStringChars(value, nullptr);
                    if (!chars)
                        return {};
                    std::u16string result(chars, chars + length);
                    env->ReleaseStringChars(value, chars);
                    return result;
                };
                name = read(server.refs.serverDataNameFid);
                address = read(server.refs.serverDataAddressFid);
            }
        }
    }
    if (env->ExceptionCheck())
        env->ExceptionClear();
    env->PopLocalFrame(nullptr);
    return FormatServerMotd(name, address);
}

bool cacheStatusResponse(JNIEnv* env, jobject mcLoader) {
    server.refs.statusClassLoader = env->NewGlobalRef(mcLoader);
    if (!server.refs.statusClassLoader) {
        env->ExceptionClear();
        return false;
    }
    jobject response = BuildStatusResponse(env, mcLoader, currentServerMotd(env));
    if (response) {
        server.refs.statusResponse = env->NewGlobalRef(response);
        env->DeleteLocalRef(response);
    }
    if (!server.refs.statusResponse) {
        LogTo("STATUS: response unavailable; gameplay listener will still start");
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }
    return server.refs.statusResponse != nullptr;
}

bool refreshMidSessionRefs(JNIEnv* env, jobject mcLoader) {
    if (!InstallWorldSnapshot(env, mcLoader))
        return false;
    jclass teleport =
        LoadClassInLoader(env, mcLoader, "net.minecraft.network.protocol.game.ServerboundAcceptTeleportationPacket");
    if (!teleport)
        return false;
    server.refs.teleportAckId = FindMappedMethod(env, teleport, "getId", "m_133795_", "()I");
    env->DeleteLocalRef(teleport);
    if (!server.refs.teleportAckId) {
        LogAndClearException(env, "snapshot/teleport acknowledgement");
        return false;
    }

    if (!server.refs.minecraftCls)
        return false;
    server.refs.mcGetConnectionMid = FindMappedMethod(env, server.refs.minecraftCls, "getConnection", "m_91403_",
                                                      "()Lnet/minecraft/client/multiplayer/ClientPacketListener;");
    if (!server.refs.mcGetConnectionMid) {
        LogAndClearException(env, "mid-session/Minecraft.getConnection");
        return false;
    }
    jclass listener = loadOrFind(env, mcLoader, "net.minecraft.client.multiplayer.ClientPacketListener",
                                 "Lnet/minecraft/client/multiplayer/ClientPacketListener;");
    if (!listener)
        return false;
    server.refs.cplGetConnectionMid =
        FindMappedMethod(env, listener, "getConnection", "m_104910_", "()Lnet/minecraft/network/Connection;");
    if (!server.refs.cplGetConnectionMid) {
        LogAndClearException(env, "mid-session/ClientPacketListener.getConnection");
        return false;
    }
    // 1.20.2+ exposes the client's enabled feature flags on the play listener; the configuration
    // handoff replays them to B so its registries match A's.
    server.refs.cplEnabledFeaturesMid = findMethodByDescriptor(
        listener, "()Lnet/minecraft/world/flag/FeatureFlagSet;", false);
    server.refs.cplRegistryAccessMid =
        findMethodByDescriptor(listener, "()Lnet/minecraft/core/RegistryAccess$Frozen;", false);
    if (!server.refs.cplRegistryAccessMid) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        server.refs.cplRegistryAccessMid =
            findMethodByDescriptor(listener, "()Lnet/minecraft/core/RegistryAccess;", false);
    }
    env->DeleteLocalRef(listener);
    return server.refs.mcGetInstanceMid && server.refs.connectionChannelFid && server.refs.channelPipelineMid;
}

static bool cacheProtocolBindings(JNIEnv* env, jobject mcLoader) {
    jclass connCls =
        loadOrFind(env, mcLoader, "net.minecraft.network.Connection", "Lnet/minecraft/network/Connection;");
    if (!connCls)
        return false;
    server.refs.connectionCls = static_cast<jclass>(env->NewGlobalRef(connCls));
    // 1.21.9+ takes four arguments (the boolean selects memory-connection handler names);
    // 1.20.1 only has the two-argument form.
    server.refs.connectionConfigureSerializationModernMid = findMethodByDescriptor(
        connCls,
        "(Lio/netty/channel/ChannelPipeline;Lnet/minecraft/network/protocol/PacketFlow;Z"
        "Lnet/minecraft/network/BandwidthDebugMonitor;)V",
        true);
    server.refs.connectionConfigureSerMid = server.refs.connectionConfigureSerializationModernMid
        ? nullptr
        : findMethodByDescriptor(
              connCls, "(Lio/netty/channel/ChannelPipeline;Lnet/minecraft/network/protocol/PacketFlow;)V", true);
    server.refs.connectionSendMid =
        findMethodByDescriptor(connCls, "(Lnet/minecraft/network/protocol/Packet;)V", false);
    server.refs.connectionAttrProtocolFid = findFieldByDescriptor(connCls, "Lio/netty/util/AttributeKey;", true);

    server.refs.connectionChannelFid = findFieldByDescriptor(connCls, "Lio/netty/channel/Channel;", false);
    env->DeleteLocalRef(connCls);

    jclass bundlerCls = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.BundlerInfo",
                                   "Lnet/minecraft/network/protocol/BundlerInfo;");
    if (bundlerCls) {
        server.refs.bundlerInfoCls = static_cast<jclass>(env->NewGlobalRef(bundlerCls));
        server.refs.bundlerProviderFid = findFieldByDescriptor(bundlerCls, "Lio/netty/util/AttributeKey;", true);
        env->DeleteLocalRef(bundlerCls);
    } else {
        LogTo("BServer: BundlerInfo not found (bundle attr not set — may cause issues on modern servers)");
    }

    jclass protoCls = loadOrFind(env, mcLoader, "net.minecraft.network.ConnectionProtocol",
                                 "Lnet/minecraft/network/ConnectionProtocol;");
    if (protoCls) {
        auto readEnum = [&](const char* n) -> jobject {
            jfieldID f = env->GetStaticFieldID(protoCls, n, "Lnet/minecraft/network/ConnectionProtocol;");
            if (!f)
                return nullptr;
            jobject v = env->GetStaticObjectField(protoCls, f);
            return v ? env->NewGlobalRef(v) : nullptr;
        };
        server.refs.protoHandshaking = readEnum("HANDSHAKING");
        server.refs.protoLogin = readEnum("LOGIN");
        server.refs.protoPlay = readEnum("PLAY");
        server.refs.protoStatus = readEnum("STATUS");
        server.refs.protoConfiguration = readEnum("CONFIGURATION");
        server.refs.configurationPhase = server.refs.protoConfiguration != nullptr;
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(protoCls);
    }

    jclass flowCls = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.PacketFlow",
                                "Lnet/minecraft/network/protocol/PacketFlow;");
    if (!flowCls)
        return false;
    jfieldID fSB = env->GetStaticFieldID(flowCls, "SERVERBOUND", "Lnet/minecraft/network/protocol/PacketFlow;");
    jfieldID fCB = env->GetStaticFieldID(flowCls, "CLIENTBOUND", "Lnet/minecraft/network/protocol/PacketFlow;");
    if (fSB)
        server.refs.flowServerbound = env->NewGlobalRef(env->GetStaticObjectField(flowCls, fSB));
    if (fCB)
        server.refs.flowClientbound = env->NewGlobalRef(env->GetStaticObjectField(flowCls, fCB));
    env->DeleteLocalRef(flowCls);
    return true;
}

// 1.21.9 replaced the ConnectionProtocol attribute with per-state ProtocolInfo objects: switching
// state now means installing PacketEncoder/PacketDecoder for the target protocol instead of
// calling setProtocol on the existing codec handlers.
static void cacheModernNetworkingBindings(JNIEnv* env, jobject mcLoader) {
    server.refs.protocolInfoCls = static_cast<jclass>(env->NewGlobalRef(
        loadOrFind(env, mcLoader, "net.minecraft.network.ProtocolInfo", "Lnet/minecraft/network/ProtocolInfo;")));
    jclass enc =
        loadOrFind(env, mcLoader, "net.minecraft.network.PacketEncoder", "Lnet/minecraft/network/PacketEncoder;");
    if (enc) {
        server.refs.packetEncoderCls = static_cast<jclass>(env->NewGlobalRef(enc));
        server.refs.packetEncoderCtor = env->GetMethodID(enc, "<init>", "(Lnet/minecraft/network/ProtocolInfo;)V");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(enc);
    }
    jclass dec =
        loadOrFind(env, mcLoader, "net.minecraft.network.PacketDecoder", "Lnet/minecraft/network/PacketDecoder;");
    if (dec) {
        server.refs.packetDecoderCls = static_cast<jclass>(env->NewGlobalRef(dec));
        server.refs.packetDecoderCtor = env->GetMethodID(dec, "<init>", "(Lnet/minecraft/network/ProtocolInfo;)V");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(dec);
    }

    jclass simple = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.SimpleUnboundProtocol",
                               "Lnet/minecraft/network/protocol/SimpleUnboundProtocol;");
    if (simple) {
        server.refs.simpleUnboundProtocolCls = static_cast<jclass>(env->NewGlobalRef(simple));
        server.refs.simpleUnboundBindMid = findMethodByDescriptor(
            simple, "(Ljava/util/function/Function;)Lnet/minecraft/network/ProtocolInfo;", false);
        env->DeleteLocalRef(simple);
    }
    jclass unbound = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.UnboundProtocol",
                                "Lnet/minecraft/network/protocol/UnboundProtocol;");
    if (unbound) {
        server.refs.unboundProtocolCls = static_cast<jclass>(env->NewGlobalRef(unbound));
        server.refs.unboundBindMid = findMethodByDescriptor(
            unbound, "(Ljava/util/function/Function;Ljava/lang/Object;)Lnet/minecraft/network/ProtocolInfo;", false);
        env->DeleteLocalRef(unbound);
    }
    jclass rfb = loadOrFind(env, mcLoader, "net.minecraft.network.RegistryFriendlyByteBuf",
                            "Lnet/minecraft/network/RegistryFriendlyByteBuf;");
    if (rfb) {
        server.refs.registryFriendlyBufCls = static_cast<jclass>(env->NewGlobalRef(rfb));
        server.refs.registryBufDecoratorMid =
            findMethodByDescriptor(rfb, "(Lnet/minecraft/core/RegistryAccess;)Ljava/util/function/Function;", true);
        env->DeleteLocalRef(rfb);
    }

    struct FieldSpec {
        const char* field;
        jfieldID* out;
    };
    auto cacheProtocols = [&](const char* dotted, const char* sig, jclass* outCls, FieldSpec* fields, int count) {
        jclass cls = loadOrFind(env, mcLoader, dotted, sig);
        if (!cls) {
            if (env->ExceptionCheck())
                env->ExceptionClear();
            return;
        }
        *outCls = static_cast<jclass>(env->NewGlobalRef(cls));
        for (int i = 0; i < count; ++i) {
            jfieldID fid = env->GetStaticFieldID(cls, fields[i].field, "Lnet/minecraft/network/ProtocolInfo;");
            if (fid)
                *(fields[i].out) = fid;
            else if (env->ExceptionCheck())
                env->ExceptionClear();
        }
        env->DeleteLocalRef(cls);
    };

    FieldSpec loginFields[] = {{"SERVERBOUND", &server.refs.loginServerboundFid},
                               {"CLIENTBOUND", &server.refs.loginClientboundFid}};
    cacheProtocols("net.minecraft.network.protocol.login.LoginProtocols",
                   "Lnet/minecraft/network/protocol/login/LoginProtocols;", &server.refs.loginProtocolsCls,
                   loginFields, 2);
    FieldSpec configFields[] = {{"SERVERBOUND", &server.refs.configServerboundFid},
                                {"CLIENTBOUND", &server.refs.configClientboundFid}};
    cacheProtocols("net.minecraft.network.protocol.configuration.ConfigurationProtocols",
                   "Lnet/minecraft/network/protocol/configuration/ConfigurationProtocols;",
                   &server.refs.configProtocolsCls, configFields, 2);
    FieldSpec handshakeFields[] = {{"SERVERBOUND", &server.refs.handshakeServerboundFid}};
    cacheProtocols("net.minecraft.network.protocol.handshake.HandshakeProtocols",
                   "Lnet/minecraft/network/protocol/handshake/HandshakeProtocols;",
                   &server.refs.handshakeProtocolsCls, handshakeFields, 1);
    FieldSpec statusFields[] = {{"SERVERBOUND", &server.refs.statusServerboundFid},
                                {"CLIENTBOUND", &server.refs.statusClientboundFid}};
    cacheProtocols("net.minecraft.network.protocol.status.StatusProtocols",
                   "Lnet/minecraft/network/protocol/status/StatusProtocols;", &server.refs.statusProtocolsCls,
                   statusFields, 2);

    jclass game = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.game.GameProtocols",
                             "Lnet/minecraft/network/protocol/game/GameProtocols;");
    if (game) {
        server.refs.gameProtocolsCls = static_cast<jclass>(env->NewGlobalRef(game));
        auto field = [&](const char* name, const char* sig) -> jfieldID {
            jfieldID fid = env->GetStaticFieldID(game, name, sig);
            if (!fid && env->ExceptionCheck())
                env->ExceptionClear();
            return fid;
        };
        server.refs.gameServerboundTemplateFid =
            field("SERVERBOUND_TEMPLATE", "Lnet/minecraft/network/protocol/UnboundProtocol;");
        server.refs.gameClientboundTemplateFid =
            field("CLIENTBOUND_TEMPLATE", "Lnet/minecraft/network/protocol/SimpleUnboundProtocol;");
        env->DeleteLocalRef(game);
    }

    server.refs.modernNetworking = server.refs.protocolInfoCls && server.refs.packetEncoderCls &&
                                   server.refs.packetEncoderCtor && server.refs.packetDecoderCls &&
                                   server.refs.packetDecoderCtor && server.refs.configProtocolsCls &&
                                   server.refs.loginProtocolsCls && server.refs.statusProtocolsCls &&
                                   server.refs.handshakeProtocolsCls && server.refs.registryBufDecoratorMid &&
                                   server.refs.simpleUnboundBindMid && server.refs.unboundBindMid;
    if (env->ExceptionCheck())
        env->ExceptionClear();
    LogTo("BServer: networking model=%s configurationPhase=%s",
          server.refs.modernNetworking ? "ProtocolInfo" : "ConnectionProtocol",
          server.refs.configurationPhase ? "yes" : "no");
}

// Configuration-phase and hand-mirrored packet bindings used by the 1.20.2+ login handoff.
static void cacheConfigurationBindings(JNIEnv* env, jobject mcLoader) {
    jclass regData = loadOrFind(env, mcLoader,
                                "net.minecraft.network.protocol.configuration.ClientboundRegistryDataPacket",
                                "Lnet/minecraft/network/protocol/configuration/ClientboundRegistryDataPacket;");
    if (regData) {
        server.refs.registryDataPacketCls = static_cast<jclass>(env->NewGlobalRef(regData));
        server.refs.registryDataPacketCtor =
            findMethodByDescriptor(regData, "(Lnet/minecraft/resources/ResourceKey;Ljava/util/List;)V", false);
        env->DeleteLocalRef(regData);
    }
    jclass features =
        loadOrFind(env, mcLoader,
                   "net.minecraft.network.protocol.configuration.ClientboundUpdateEnabledFeaturesPacket",
                   "Lnet/minecraft/network/protocol/configuration/ClientboundUpdateEnabledFeaturesPacket;");
    if (features) {
        server.refs.enabledFeaturesPacketCls = static_cast<jclass>(env->NewGlobalRef(features));
        server.refs.enabledFeaturesPacketCtor = findMethodByDescriptor(features, "(Ljava/util/Set;)V", false);
        env->DeleteLocalRef(features);
    }
    jclass finish =
        loadOrFind(env, mcLoader, "net.minecraft.network.protocol.configuration.ClientboundFinishConfigurationPacket",
                   "Lnet/minecraft/network/protocol/configuration/ClientboundFinishConfigurationPacket;");
    if (finish) {
        server.refs.finishConfigurationPacketCls = static_cast<jclass>(env->NewGlobalRef(finish));
        env->DeleteLocalRef(finish);
    }
    jclass ack =
        loadOrFind(env, mcLoader, "net.minecraft.network.protocol.configuration.ServerboundFinishConfigurationPacket",
                   "Lnet/minecraft/network/protocol/configuration/ServerboundFinishConfigurationPacket;");
    if (ack) {
        server.refs.serverboundFinishConfigurationCls = static_cast<jclass>(env->NewGlobalRef(ack));
        env->DeleteLocalRef(ack);
    }
    // 1.20.2+: the client acknowledges the login phase before the codecs may switch to CONFIGURATION.
    jclass loginAck = loadOrFind(env, mcLoader,
                                 "net.minecraft.network.protocol.login.ServerboundLoginAcknowledgedPacket",
                                 "Lnet/minecraft/network/protocol/login/ServerboundLoginAcknowledgedPacket;");
    if (loginAck) {
        server.refs.loginAckPacketCls = static_cast<jclass>(env->NewGlobalRef(loginAck));
        env->DeleteLocalRef(loginAck);
    }

    jclass flags =
        loadOrFind(env, mcLoader, "net.minecraft.world.flag.FeatureFlags", "Lnet/minecraft/world/flag/FeatureFlags;");
    if (flags) {
        server.refs.featureFlagsRegistryFid =
            env->GetStaticFieldID(flags, "REGISTRY", "Lnet/minecraft/world/flag/FeatureFlagRegistry;");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(flags);
    }
    jclass registry = loadOrFind(env, mcLoader, "net.minecraft.world.flag.FeatureFlagRegistry",
                                 "Lnet/minecraft/world/flag/FeatureFlagRegistry;");
    if (registry) {
        server.refs.featureFlagRegistryToNamesMid =
            findMethodByDescriptor(registry, "(Lnet/minecraft/world/flag/FeatureFlagSet;)Ljava/util/Set;", false);
        env->DeleteLocalRef(registry);
    }
}

// Helpers for packets whose constructors changed after 1.20.1.
static void cachePacketConstructionBindings(JNIEnv* env, jobject mcLoader) {
    jclass pmr = loadOrFind(env, mcLoader, "net.minecraft.world.entity.PositionMoveRotation",
                            "Lnet/minecraft/world/entity/PositionMoveRotation;");
    if (pmr) {
        server.refs.positionMoveRotationCls = static_cast<jclass>(env->NewGlobalRef(pmr));
        server.refs.positionMoveRotationOfEntityMid = findMethodByDescriptor(
            pmr, "(Lnet/minecraft/world/entity/Entity;)Lnet/minecraft/world/entity/PositionMoveRotation;", true);
        env->DeleteLocalRef(pmr);
    }
    jclass respawn = loadOrFind(env, mcLoader, "net.minecraft.world.level.storage.LevelData$RespawnData",
                                "Lnet/minecraft/world/level/storage/LevelData$RespawnData;");
    if (respawn) {
        server.refs.respawnDataCls = static_cast<jclass>(env->NewGlobalRef(respawn));
        server.refs.respawnDataOfMid = findMethodByDescriptor(
            respawn,
            "(Lnet/minecraft/resources/ResourceKey;Lnet/minecraft/core/BlockPos;FF)"
            "Lnet/minecraft/world/level/storage/LevelData$RespawnData;",
            true);
        env->DeleteLocalRef(respawn);
    }
    jclass levelCls = loadOrFind(env, mcLoader, "net.minecraft.world.level.Level", "Lnet/minecraft/world/level/Level;");
    if (levelCls) {
        server.refs.levelGetRespawnDataMid =
            findMethodByDescriptor(levelCls, "()Lnet/minecraft/world/level/storage/LevelData$RespawnData;", false);
        env->DeleteLocalRef(levelCls);
    }
    jclass typeCls =
        loadOrFind(env, mcLoader, "net.minecraft.world.entity.EntityType", "Lnet/minecraft/world/entity/EntityType;");
    if (typeCls) {
        server.refs.entityTypeCls = static_cast<jclass>(env->NewGlobalRef(typeCls));
        server.refs.entityTypePlayerFid =
            env->GetStaticFieldID(typeCls, "PLAYER", "Lnet/minecraft/world/entity/EntityType;");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(typeCls);
    }
    jclass addEntity = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.game.ClientboundAddEntityPacket",
                                  "Lnet/minecraft/network/protocol/game/ClientboundAddEntityPacket;");
    if (addEntity) {
        server.refs.addEntityPacketCls = static_cast<jclass>(env->NewGlobalRef(addEntity));
        // 1.21.9+ spawns every entity (players included) through this packet; the ServerEntity-based
        // constructors are server-only, so the client-side snapshot uses the raw field constructor.
        server.refs.addEntityPacketCtor = findMethodByDescriptor(
            addEntity,
            "(ILjava/util/UUID;DDDFFLnet/minecraft/world/entity/EntityType;ILnet/minecraft/world/phys/Vec3;D)V",
            false);
        env->DeleteLocalRef(addEntity);
    }
    jclass spawnInfo = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.game.CommonPlayerSpawnInfo",
                                  "Lnet/minecraft/network/protocol/game/CommonPlayerSpawnInfo;");
    if (spawnInfo) {
        server.refs.hasCommonSpawnInfo = true;
        env->DeleteLocalRef(spawnInfo);
    }
    jclass addPlayer = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.game.ClientboundAddPlayerPacket",
                                  "Lnet/minecraft/network/protocol/game/ClientboundAddPlayerPacket;");
    if (addPlayer) {
        server.refs.addPlayerPacketCls = static_cast<jclass>(env->NewGlobalRef(addPlayer));
        env->DeleteLocalRef(addPlayer);
    }
    jclass entryCls = loadOrFind(env, mcLoader,
                                 "net.minecraft.network.protocol.game.ClientboundPlayerInfoUpdatePacket$Entry",
                                 "Lnet/minecraft/network/protocol/game/ClientboundPlayerInfoUpdatePacket$Entry;");
    if (entryCls) {
        server.refs.playerInfoEntryCls = static_cast<jclass>(env->NewGlobalRef(entryCls));
        server.refs.playerInfoEntryCtorModern = findMethodByDescriptor(
            entryCls,
            "(Ljava/util/UUID;Lcom/mojang/authlib/GameProfile;ZILnet/minecraft/world/level/GameType;"
            "Lnet/minecraft/network/chat/Component;ZILnet/minecraft/network/chat/RemoteChatSession$Data;)V",
            false);
        server.refs.playerInfoEntryCtorLegacy = findMethodByDescriptor(
            entryCls,
            "(Ljava/util/UUID;Lcom/mojang/authlib/GameProfile;ZILnet/minecraft/world/level/GameType;"
            "Lnet/minecraft/network/chat/Component;Lnet/minecraft/network/chat/RemoteChatSession$Data;)V",
            false);
        server.refs.playerInfoEntryGameModeMid =
            findMethodByDescriptor(entryCls, "()Lnet/minecraft/world/level/GameType;", false);
        static const char* const kLatencyExcl[] = {"hashCode"};
        server.refs.playerInfoEntryLatencyMid =
            findMethodByDescriptorExcept(entryCls, "()I", false, kLatencyExcl, 1);
        server.refs.playerInfoEntryDisplayNameMid =
            findMethodByDescriptor(entryCls, "()Lnet/minecraft/network/chat/Component;", false);
        env->DeleteLocalRef(entryCls);
    }
}

// Registry and tag synchronization helpers. A 1.21.10 server rebuilds the configuration payload
// from its own RegistryAccess through RegistrySynchronization; the mid-session handoff does the
// same with the registries A already received.
static void cacheRegistrySyncBindings(JNIEnv* env, jobject mcLoader) {
    jclass context = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.game.GameProtocols$Context",
                                "Lnet/minecraft/network/protocol/game/GameProtocols$Context;");
    if (context) {
        server.refs.gameContextCls = static_cast<jclass>(env->NewGlobalRef(context));
        server.refs.gameContextInfiniteMid =
            findMethodByDescriptor(context, "()Z", false);
        env->DeleteLocalRef(context);
    }
    jclass consumer = env->FindClass("java/util/function/BiConsumer");
    if (consumer) {
        server.refs.biConsumerCls = static_cast<jclass>(env->NewGlobalRef(consumer));
        env->DeleteLocalRef(consumer);
    }
    jclass enumSet = env->FindClass("java/util/EnumSet");
    if (enumSet) {
        server.refs.enumSetCls = static_cast<jclass>(env->NewGlobalRef(enumSet));
        env->DeleteLocalRef(enumSet);
    }

    jclass nbtOps = loadOrFind(env, mcLoader, "net.minecraft.nbt.NbtOps", "Lnet/minecraft/nbt/NbtOps;");
    if (nbtOps) {
        server.refs.nbtOpsCls = static_cast<jclass>(env->NewGlobalRef(nbtOps));
        server.refs.nbtOpsInstanceFid = env->GetStaticFieldID(nbtOps, "INSTANCE", "Lnet/minecraft/nbt/NbtOps;");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(nbtOps);
    }
    jclass access = loadOrFind(env, mcLoader, "net.minecraft.core.RegistryAccess", "Lnet/minecraft/core/RegistryAccess;");
    if (access) {
        server.refs.registryAccessSerializationContextMid = findMethodByDescriptor(
            access,
            "(Lcom/mojang/serialization/DynamicOps;)Lnet/minecraft/resources/RegistryOps;", false);
        server.refs.registryAccessRegistriesMid = findMethodByDescriptor(access, "()Ljava/util/stream/Stream;", false);
        env->DeleteLocalRef(access);
    }
    jclass ops = loadOrFind(env, mcLoader, "net.minecraft.resources.RegistryOps", "Lnet/minecraft/resources/RegistryOps;");
    if (ops) {
        server.refs.registryOpsCls = static_cast<jclass>(env->NewGlobalRef(ops));
        // RegistrySynchronization.packRegistries needs a RegistryOps; the instance factory lives on
        // the access object, the static one on RegistryOps.
        server.refs.registryOpsCreateMid = findMethodByDescriptor(
            ops,
            "(Lcom/mojang/serialization/DynamicOps;Lnet/minecraft/core/HolderLookup$Provider;)"
            "Lnet/minecraft/resources/RegistryOps;",
            true);
        env->DeleteLocalRef(ops);
    }
    jclass entry = loadOrFind(env, mcLoader, "net.minecraft.core.RegistryAccess$RegistryEntry",
                              "Lnet/minecraft/core/RegistryAccess$RegistryEntry;");
    if (entry) {
        server.refs.registryEntryKeyMid = findMethodByDescriptor(entry, "()Lnet/minecraft/resources/ResourceKey;", false);
        server.refs.registryEntryValueMid = findMethodByDescriptor(entry, "()Lnet/minecraft/core/Registry;", false);
        env->DeleteLocalRef(entry);
    }
    jclass sync = loadOrFind(env, mcLoader, "net.minecraft.core.RegistrySynchronization",
                             "Lnet/minecraft/core/RegistrySynchronization;");
    if (sync) {
        server.refs.registrySynchronizationCls = static_cast<jclass>(env->NewGlobalRef(sync));
        server.refs.packRegistriesMid = findMethodByDescriptor(
            sync,
            "(Lcom/mojang/serialization/DynamicOps;Lnet/minecraft/core/RegistryAccess;Ljava/util/Set;"
            "Ljava/util/function/BiConsumer;)V",
            true);
        server.refs.isNetworkableMid =
            findMethodByDescriptor(sync, "(Lnet/minecraft/resources/ResourceKey;)Z", true);
        env->DeleteLocalRef(sync);
    }
    jclass tags =
        loadOrFind(env, mcLoader, "net.minecraft.tags.TagNetworkSerialization", "Lnet/minecraft/tags/TagNetworkSerialization;");
    if (tags) {
        server.refs.tagSerializationCls = static_cast<jclass>(env->NewGlobalRef(tags));
        server.refs.serializeTagsToNetworkMid = findMethodByDescriptor(
            tags, "(Lnet/minecraft/core/Registry;)Lnet/minecraft/tags/TagNetworkSerialization$NetworkPayload;", true);
        env->DeleteLocalRef(tags);
    }
    jclass updateTags = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.common.ClientboundUpdateTagsPacket",
                                   "Lnet/minecraft/network/protocol/common/ClientboundUpdateTagsPacket;");
    if (updateTags) {
        server.refs.updateTagsPacketCls = static_cast<jclass>(env->NewGlobalRef(updateTags));
        server.refs.updateTagsPacketCtor = findMethodByDescriptor(updateTags, "(Ljava/util/Map;)V", false);
        env->DeleteLocalRef(updateTags);
    }
    jclass stream = env->FindClass("java/util/stream/Stream");
    if (stream) {
        server.refs.streamIteratorMid =
            env->GetMethodID(stream, "iterator", "()Ljava/util/Iterator;");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(stream);
    }
    jclass map = env->FindClass("java/util/HashMap");
    if (map) {
        server.refs.hashMapCls = static_cast<jclass>(env->NewGlobalRef(map));
        server.refs.hashMapCtor = env->GetMethodID(map, "<init>", "()V");
        server.refs.mapPutMid = env->GetMethodID(map, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(map);
    }
}

static bool cacheChannelBindings(JNIEnv* env, jobject mcLoader) {
    jclass chCls = loadOrFind(env, mcLoader, "io.netty.channel.Channel", "Lio/netty/channel/Channel;");
    if (!chCls)
        return false;
    server.refs.channelCls = static_cast<jclass>(env->NewGlobalRef(chCls));
    server.refs.channelPipelineMid = env->GetMethodID(chCls, "pipeline", "()Lio/netty/channel/ChannelPipeline;");
    server.refs.channelWriteAndFlushMid =
        env->GetMethodID(chCls, "writeAndFlush", "(Ljava/lang/Object;)Lio/netty/channel/ChannelFuture;");
    server.refs.channelAttrMid =
        env->GetMethodID(chCls, "attr", "(Lio/netty/util/AttributeKey;)Lio/netty/util/Attribute;");
    server.refs.channelConfigMid = env->GetMethodID(chCls, "config", "()Lio/netty/channel/ChannelConfig;");
    server.refs.channelCloseMid = env->GetMethodID(chCls, "close", "()Lio/netty/channel/ChannelFuture;");
    if (env->ExceptionCheck())
        env->ExceptionClear();
    env->DeleteLocalRef(chCls);

    jclass contextCls = LoadClassInLoader(env, mcLoader, "io.netty.channel.ChannelHandlerContext");
    if (!contextCls)
        return false;
    server.refs.contextChannelMid = env->GetMethodID(contextCls, "channel", "()Lio/netty/channel/Channel;");
    env->DeleteLocalRef(contextCls);
    if (!server.refs.contextChannelMid || env->ExceptionCheck())
        return false;

    jclass cfgCls = loadOrFind(env, mcLoader, "io.netty.channel.ChannelConfig", "Lio/netty/channel/ChannelConfig;");
    if (cfgCls) {
        server.refs.configSetOptionMid =
            env->GetMethodID(cfgCls, "setOption", "(Lio/netty/channel/ChannelOption;Ljava/lang/Object;)Z");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(cfgCls);
    }
    jclass optCls = loadOrFind(env, mcLoader, "io.netty.channel.ChannelOption", "Lio/netty/channel/ChannelOption;");
    if (optCls) {
        jfieldID f = env->GetStaticFieldID(optCls, "TCP_NODELAY", "Lio/netty/channel/ChannelOption;");
        if (f) {
            jobject v = env->GetStaticObjectField(optCls, f);
            if (v)
                server.refs.tcpNoDelayOption = env->NewGlobalRef(v);
        }
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(optCls);
    }
    jclass boolCls = env->FindClass("java/lang/Boolean");
    if (boolCls) {
        jfieldID f = env->GetStaticFieldID(boolCls, "TRUE", "Ljava/lang/Boolean;");
        if (f) {
            jobject v = env->GetStaticObjectField(boolCls, f);
            if (v)
                server.refs.booleanTrue = env->NewGlobalRef(v);
        }
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(boolCls);
    }

    jclass pipCls = loadOrFind(env, mcLoader, "io.netty.channel.ChannelPipeline", "Lio/netty/channel/ChannelPipeline;");
    if (!pipCls)
        return false;
    server.refs.pipelineCls = static_cast<jclass>(env->NewGlobalRef(pipCls));
    server.refs.pipelineAddLastMid = env->GetMethodID(
        pipCls, "addLast", "(Ljava/lang/String;Lio/netty/channel/ChannelHandler;)Lio/netty/channel/ChannelPipeline;");
    server.refs.pipelineRemoveNameMid =
        env->GetMethodID(pipCls, "remove", "(Ljava/lang/String;)Lio/netty/channel/ChannelHandler;");
    server.refs.pipelineGetHandlerMid =
        env->GetMethodID(pipCls, "get", "(Ljava/lang/String;)Lio/netty/channel/ChannelHandler;");
    server.refs.pipelineReplaceMid = env->GetMethodID(
        pipCls, "replace", "(Ljava/lang/String;Ljava/lang/String;Lio/netty/channel/ChannelHandler;)"
                           "Lio/netty/channel/ChannelPipeline;");
    if (env->ExceptionCheck())
        env->ExceptionClear();
    env->DeleteLocalRef(pipCls);

    jclass attrCls = loadOrFind(env, mcLoader, "io.netty.util.Attribute", "Lio/netty/util/Attribute;");
    if (!attrCls)
        return false;
    server.refs.attributeCls = static_cast<jclass>(env->NewGlobalRef(attrCls));
    server.refs.attributeSetMid = env->GetMethodID(attrCls, "set", "(Ljava/lang/Object;)V");
    env->DeleteLocalRef(attrCls);
    return true;
}

static void cachePlayerBindings(JNIEnv* env, jobject mcLoader) {
    jclass gpCls = loadOrFind(env, mcLoader, "com.mojang.authlib.GameProfile", "Lcom/mojang/authlib/GameProfile;");
    if (gpCls) {
        server.refs.gameProfileCls = static_cast<jclass>(env->NewGlobalRef(gpCls));
        server.refs.gameProfileCtor = env->GetMethodID(gpCls, "<init>", "(Ljava/util/UUID;Ljava/lang/String;)V");

        server.refs.gameProfileGetNameMid = env->GetMethodID(gpCls, "getName", "()Ljava/lang/String;");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(gpCls);
    }

    jclass mcCls = loadOrFind(env, mcLoader, "net.minecraft.client.Minecraft", "Lnet/minecraft/client/Minecraft;");
    if (mcCls) {
        server.refs.minecraftCls = static_cast<jclass>(env->NewGlobalRef(mcCls));
        server.refs.mcGetInstanceMid = findMethodByDescriptor(mcCls, "()Lnet/minecraft/client/Minecraft;", true);
        // getProfileProperties() disappeared in 1.21.9; the skin push then falls back to profile only.
        server.refs.mcGetProfilePropsMid =
            findMethodByDescriptor(mcCls, "()Lcom/mojang/authlib/properties/PropertyMap;", false);
        server.refs.mcGetUserMid = findMethodByDescriptor(mcCls, "()Lnet/minecraft/client/User;", false);
        server.refs.mcGameModeFid =
            findFieldByDescriptor(mcCls, "Lnet/minecraft/client/multiplayer/MultiPlayerGameMode;", false);
        server.refs.mcGetCurrentServerMid =
            env->GetMethodID(mcCls, "getCurrentServer", "()Lnet/minecraft/client/multiplayer/ServerData;");
        if (!server.refs.mcGetCurrentServerMid) {
            env->ExceptionClear();
            server.refs.mcGetCurrentServerMid =
                env->GetMethodID(mcCls, "m_91089_", "()Lnet/minecraft/client/multiplayer/ServerData;");
        }
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(mcCls);
    }
    jclass serverData = LoadClassInLoader(env, mcLoader, "net.minecraft.client.multiplayer.ServerData");
    if (serverData) {
        auto field = [&](const char* named, const char* srg) {
            jfieldID id = env->GetFieldID(serverData, named, "Ljava/lang/String;");
            if (!id) {
                env->ExceptionClear();
                id = env->GetFieldID(serverData, srg, "Ljava/lang/String;");
            }
            if (env->ExceptionCheck())
                env->ExceptionClear();
            return id;
        };
        server.refs.serverDataNameFid = field("name", "f_105362_");
        server.refs.serverDataAddressFid = field("ip", "f_105363_");
        env->DeleteLocalRef(serverData);
    }
    jclass userCls = loadOrFind(env, mcLoader, "net.minecraft.client.User", "Lnet/minecraft/client/User;");
    if (userCls) {
        server.refs.userCls = static_cast<jclass>(env->NewGlobalRef(userCls));
        server.refs.userGetProfileIdMid = findMethodByDescriptor(userCls, "()Ljava/util/UUID;", false);

        server.refs.userGetGameProfileMid =
            findMethodByDescriptor(userCls, "()Lcom/mojang/authlib/GameProfile;", false);
        env->DeleteLocalRef(userCls);
    }
}

static void cacheBufferBindings(JNIEnv* env, jobject mcLoader) {
    jclass fbbCls =
        loadOrFind(env, mcLoader, "net.minecraft.network.FriendlyByteBuf", "Lnet/minecraft/network/FriendlyByteBuf;");
    if (fbbCls) {
        server.refs.friendlyBufCls = static_cast<jclass>(env->NewGlobalRef(fbbCls));
        server.refs.friendlyBufCtor = env->GetMethodID(fbbCls, "<init>", "(Lio/netty/buffer/ByteBuf;)V");

        server.refs.fbbWriteByteMid = env->GetMethodID(fbbCls, "writeByte", "(I)Lio/netty/buffer/ByteBuf;");
        server.refs.fbbWriteBooleanMid = env->GetMethodID(fbbCls, "writeBoolean", "(Z)Lio/netty/buffer/ByteBuf;");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        server.refs.fbbWriteVarIntMid =
            findMethodByDescriptor(fbbCls, "(I)Lnet/minecraft/network/FriendlyByteBuf;", false);
        server.refs.fbbWriteUUIDMid =
            findMethodByDescriptor(fbbCls, "(Ljava/util/UUID;)Lnet/minecraft/network/FriendlyByteBuf;", false);
        server.refs.fbbWriteUtfMid =
            findMethodByDescriptor(fbbCls, "(Ljava/lang/String;I)Lnet/minecraft/network/FriendlyByteBuf;", false);
        server.refs.fbbWriteGpPropsMid =
            findMethodByDescriptor(fbbCls, "(Lcom/mojang/authlib/properties/PropertyMap;)V", false);
        env->DeleteLocalRef(fbbCls);
    }
    jclass unpCls = loadOrFind(env, mcLoader, "io.netty.buffer.Unpooled", "Lio/netty/buffer/Unpooled;");
    if (unpCls) {
        server.refs.unpooledCls = static_cast<jclass>(env->NewGlobalRef(unpCls));
        server.refs.unpooledBufferMid = env->GetStaticMethodID(unpCls, "buffer", "()Lio/netty/buffer/ByteBuf;");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(unpCls);
    }
}

static void cachePlayerPacketBindings(JNIEnv* env, jobject mcLoader) {
    jclass piuCls = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.game.ClientboundPlayerInfoUpdatePacket",
                               "Lnet/minecraft/network/protocol/game/ClientboundPlayerInfoUpdatePacket;");
    if (piuCls) {
        server.refs.playerInfoUpdatePacketCls = static_cast<jclass>(env->NewGlobalRef(piuCls));
        server.refs.playerInfoUpdatePacketBufCtor =
            env->GetMethodID(piuCls, "<init>", "(Lnet/minecraft/network/FriendlyByteBuf;)V");

        static const char* const kPacketWriteExcl[] = {"<init>"};
        server.refs.playerInfoUpdatePacketWriteMid = findMethodByDescriptorExcept(
            piuCls, "(Lnet/minecraft/network/FriendlyByteBuf;)V", false, kPacketWriteExcl, 1);
        server.refs.playerInfoUpdatePacketCtor =
            findMethodByDescriptor(piuCls, "(Ljava/util/EnumSet;Ljava/util/Collection;)V", false);
        server.refs.playerInfoUpdatePacketEntriesFid =
            findFieldByDescriptor(piuCls, "Ljava/util/List;", false);

        jmethodID listMids[2] = {nullptr, nullptr};
        int nList = findMethodsByDescriptor(piuCls, "()Ljava/util/List;", false, listMids, 2);
        server.refs.piuEntriesMidA = listMids[0];
        server.refs.piuEntriesMidB = listMids[1];
        LogTo("  piu: %d ()List accessor(s) cached", nList);
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(piuCls);
    }

    jclass piEntryCls =
        loadOrFind(env, mcLoader, "net.minecraft.network.protocol.game.ClientboundPlayerInfoUpdatePacket$Entry",
                   "Lnet/minecraft/network/protocol/game/ClientboundPlayerInfoUpdatePacket$Entry;");
    if (piEntryCls) {
        server.refs.piEntryCls = static_cast<jclass>(env->NewGlobalRef(piEntryCls));
        server.refs.piEntryProfileIdMid = findMethodByDescriptor(piEntryCls, "()Ljava/util/UUID;", false);
        server.refs.piEntryGameModeMid =
            findMethodByDescriptor(piEntryCls, "()Lnet/minecraft/world/level/GameType;", false);
        static const char* const kHashExcl[] = {"hashCode"};
        server.refs.piEntryLatencyMid = findMethodByDescriptorExcept(piEntryCls, "()I", false, kHashExcl, 1);
        server.refs.piEntryDisplayNameMid =
            findMethodByDescriptor(piEntryCls, "()Lnet/minecraft/network/chat/Component;", false);
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(piEntryCls);
    }

    jclass gameTypeCls =
        loadOrFind(env, mcLoader, "net.minecraft.world.level.GameType", "Lnet/minecraft/world/level/GameType;");
    if (gameTypeCls) {
        static const char* const kIdExcl[] = {"ordinal", "hashCode"};
        server.refs.gameTypeGetIdMid = findMethodByDescriptorExcept(gameTypeCls, "()I", false, kIdExcl, 2);
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(gameTypeCls);
    }

    if (server.refs.friendlyBufCls) {
        server.refs.fbbWriteComponentMid = findMethodByDescriptor(
            server.refs.friendlyBufCls,
            "(Lnet/minecraft/network/chat/Component;)Lnet/minecraft/network/FriendlyByteBuf;", false);
    }

    jclass listCls = env->FindClass("java/util/List");
    if (listCls) {
        server.refs.listSizeMid = env->GetMethodID(listCls, "size", "()I");
        server.refs.listGetMid = env->GetMethodID(listCls, "get", "(I)Ljava/lang/Object;");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(listCls);
    }

    jclass actionCls = loadOrFind(env, mcLoader,
                                  "net.minecraft.network.protocol.game.ClientboundPlayerInfoUpdatePacket$Action",
                                  "Lnet/minecraft/network/protocol/game/ClientboundPlayerInfoUpdatePacket$Action;");
    if (actionCls) {
        server.refs.playerInfoActionCls = static_cast<jclass>(env->NewGlobalRef(actionCls));
        env->DeleteLocalRef(actionCls);
    }
    jclass enumSetCls = env->FindClass("java/util/EnumSet");
    if (enumSetCls) {
        server.refs.enumSetNoneOfMid =
            env->GetStaticMethodID(enumSetCls, "noneOf", "(Ljava/lang/Class;)Ljava/util/EnumSet;");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(enumSetCls);
    }
    jclass collectionCls = env->FindClass("java/util/Collection");
    if (collectionCls) {
        server.refs.collectionAddMid = env->GetMethodID(collectionCls, "add", "(Ljava/lang/Object;)Z");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(collectionCls);
    }

    {
        jclass bbCls = loadOrFind(env, mcLoader, "io.netty.buffer.ByteBuf", "Lio/netty/buffer/ByteBuf;");
        if (bbCls) {
            server.refs.byteBufGetByteMid = env->GetMethodID(bbCls, "getByte", "(I)B");
            if (env->ExceptionCheck())
                env->ExceptionClear();
            env->DeleteLocalRef(bbCls);
        }
    }

    jclass cpp = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.game.ClientboundCustomPayloadPacket",
                            "Lnet/minecraft/network/protocol/game/ClientboundCustomPayloadPacket;");
    if (!cpp) {
        // 1.20.2+ moved custom payloads to the common protocol package (mailbox plugins on 1.21.9).
        if (env->ExceptionCheck())
            env->ExceptionClear();
        cpp = loadOrFind(env, mcLoader,
                         "net.minecraft.network.protocol.common.ClientboundCustomPayloadPacket",
                         "Lnet/minecraft/network/protocol/common/ClientboundCustomPayloadPacket;");
    }
    if (cpp) {
        server.refs.customPayloadPacketCls = static_cast<jclass>(env->NewGlobalRef(cpp));
        env->DeleteLocalRef(cpp);
    }

    jclass sptCls = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.game.ClientboundSetPlayerTeamPacket",
                               "Lnet/minecraft/network/protocol/game/ClientboundSetPlayerTeamPacket;");
    if (sptCls) {
        server.refs.setPlayerTeamPacketCls = static_cast<jclass>(env->NewGlobalRef(sptCls));
        server.refs.setPlayerTeamCtor = env->GetMethodID(sptCls, "<init>",
            "(Ljava/lang/String;ILjava/util/Optional;Ljava/util/Collection;)V");
        if (env->ExceptionCheck()) env->ExceptionClear();
        server.refs.setPlayerTeamParametersFid = findFieldByDescriptor(sptCls, "Ljava/util/Optional;", false);
        server.refs.setPlayerTeamMethodFid = findFieldByDescriptor(sptCls, "I", false);
        server.refs.setPlayerTeamNameFid = findFieldByDescriptor(sptCls, "Ljava/lang/String;", false);
        server.refs.setPlayerTeamPlayersFid = findFieldByDescriptor(sptCls, "Ljava/util/Collection;", false);
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(sptCls);
    }

    jclass uuidClsL = env->FindClass("java/util/UUID");
    if (uuidClsL) {
        server.refs.uuidGetMsbMid = env->GetMethodID(uuidClsL, "getMostSignificantBits", "()J");
        server.refs.uuidGetLsbMid = env->GetMethodID(uuidClsL, "getLeastSignificantBits", "()J");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(uuidClsL);
    }
    if (env->ExceptionCheck())
        env->ExceptionClear();
}

static void cacheLoginStatusBindings(JNIEnv* env, jobject mcLoader) {
    // 1.20.2 renamed ClientboundGameProfilePacket to ClientboundLoginFinishedPacket.
    jclass lfp = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.login.ClientboundLoginFinishedPacket",
                            "Lnet/minecraft/network/protocol/login/ClientboundLoginFinishedPacket;");
    if (!lfp) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        lfp = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.login.ClientboundGameProfilePacket",
                         "Lnet/minecraft/network/protocol/login/ClientboundGameProfilePacket;");
    }
    if (lfp) {
        server.refs.loginFinishedPacketCls = static_cast<jclass>(env->NewGlobalRef(lfp));

        server.refs.loginFinishedPacketCtor = findMethodByDescriptor(lfp, "(Lcom/mojang/authlib/GameProfile;)V", false);
        env->DeleteLocalRef(lfp);
    }

    jclass hello = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.login.ServerboundHelloPacket",
                              "Lnet/minecraft/network/protocol/login/ServerboundHelloPacket;");
    if (hello) {
        server.refs.helloPacketCls = static_cast<jclass>(env->NewGlobalRef(hello));
        server.refs.helloPacketNameFid = findFieldByDescriptor(hello, "Ljava/lang/String;", false);
        env->DeleteLocalRef(hello);
    }

    jclass intent = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.handshake.ClientIntentionPacket",
                               "Lnet/minecraft/network/protocol/handshake/ClientIntentionPacket;");
    if (intent) {
        server.refs.intentPacketCls = static_cast<jclass>(env->NewGlobalRef(intent));
        // 1.20.2+ carries a ClientIntent enum instead of a ConnectionProtocol enum.
        server.refs.intentionPacketIntentFid =
            findFieldByDescriptor(intent, "Lnet/minecraft/network/protocol/handshake/ClientIntent;", false);
        if (!server.refs.intentionPacketIntentFid)
            server.refs.intentionPacketIntentFid =
                findFieldByDescriptor(intent, "Lnet/minecraft/network/ConnectionProtocol;", false);
        env->DeleteLocalRef(intent);
    }
    jclass clientIntent =
        loadOrFind(env, mcLoader, "net.minecraft.network.protocol.handshake.ClientIntent",
                   "Lnet/minecraft/network/protocol/handshake/ClientIntent;");
    if (clientIntent) {
        auto readIntent = [&](const char* name) -> jobject {
            jfieldID fid = env->GetStaticFieldID(clientIntent, name,
                                                 "Lnet/minecraft/network/protocol/handshake/ClientIntent;");
            if (!fid)
                return nullptr;
            jobject value = env->GetStaticObjectField(clientIntent, fid);
            return value ? env->NewGlobalRef(value) : nullptr;
        };
        server.refs.intentLogin = readIntent("LOGIN");
        server.refs.intentStatus = readIntent("STATUS");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(clientIntent);
    }

    jclass sReq = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.status.ServerboundStatusRequestPacket",
                             "Lnet/minecraft/network/protocol/status/ServerboundStatusRequestPacket;");
    if (sReq) {
        server.refs.statusRequestPacketCls = static_cast<jclass>(env->NewGlobalRef(sReq));
        env->DeleteLocalRef(sReq);
    }

    // 1.21.9 moved ping traffic out of the status protocol into its own package.
    jclass pReq = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.ping.ServerboundPingRequestPacket",
                             "Lnet/minecraft/network/protocol/ping/ServerboundPingRequestPacket;");
    if (!pReq) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        pReq = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.status.ServerboundPingRequestPacket",
                          "Lnet/minecraft/network/protocol/status/ServerboundPingRequestPacket;");
    }
    if (pReq) {
        server.refs.pingRequestPacketCls = static_cast<jclass>(env->NewGlobalRef(pReq));
        server.refs.pingRequestPacketTimeFid = findFieldByDescriptor(pReq, "J", false);
        env->DeleteLocalRef(pReq);
    }

    jclass pong = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.ping.ClientboundPongResponsePacket",
                             "Lnet/minecraft/network/protocol/ping/ClientboundPongResponsePacket;");
    if (!pong) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        pong = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.status.ClientboundPongResponsePacket",
                          "Lnet/minecraft/network/protocol/status/ClientboundPongResponsePacket;");
    }
    if (pong) {
        server.refs.pongResponsePacketCls = static_cast<jclass>(env->NewGlobalRef(pong));
        server.refs.pongResponsePacketCtor = findMethodByDescriptor(pong, "(J)V", false);
        env->DeleteLocalRef(pong);
    }
}

static void cacheBundleBindings(JNIEnv* env, jobject mcLoader) {
    jclass cbp = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.game.ClientboundBundlePacket",
                            "Lnet/minecraft/network/protocol/game/ClientboundBundlePacket;");
    if (cbp) {
        server.refs.bundlePacketCls = static_cast<jclass>(env->NewGlobalRef(cbp));
        jclass bp = loadOrFind(env, mcLoader, "net.minecraft.network.protocol.BundlePacket",
                               "Lnet/minecraft/network/protocol/BundlePacket;");
        if (bp) {
            server.refs.bundleSubPacketsMid = findMethodByDescriptor(bp, "()Ljava/lang/Iterable;", false);
            env->DeleteLocalRef(bp);
        }
        env->DeleteLocalRef(cbp);
    } else {
        LogTo("BServer: ClientboundBundlePacket not found — bundles won't be expanded");
    }
    jclass iterableCls = env->FindClass("java/lang/Iterable");
    if (iterableCls) {
        server.refs.iterableIteratorMid = env->GetMethodID(iterableCls, "iterator", "()Ljava/util/Iterator;");
        env->DeleteLocalRef(iterableCls);
    }
    jclass iteratorCls = env->FindClass("java/util/Iterator");
    if (iteratorCls) {
        server.refs.iteratorHasNextMid = env->GetMethodID(iteratorCls, "hasNext", "()Z");
        server.refs.iteratorNextMid = env->GetMethodID(iteratorCls, "next", "()Ljava/lang/Object;");
        env->DeleteLocalRef(iteratorCls);
    }
    if (env->ExceptionCheck())
        env->ExceptionClear();

    jclass uuidCls = env->FindClass("java/util/UUID");
    if (uuidCls) {
        server.refs.uuidCls = static_cast<jclass>(env->NewGlobalRef(uuidCls));
        server.refs.uuidNameUuidFromBytesMid =
            env->GetStaticMethodID(uuidCls, "nameUUIDFromBytes", "([B)Ljava/util/UUID;");
        env->DeleteLocalRef(uuidCls);
    }
}

bool cacheJavaRefs(JNIEnv* env, jobject mcLoader) {
    if (!cacheProtocolBindings(env, mcLoader))
        return false;
    if (!cacheChannelBindings(env, mcLoader))
        return false;
    cachePlayerBindings(env, mcLoader);
    cacheBufferBindings(env, mcLoader);
    cachePlayerPacketBindings(env, mcLoader);
    cacheLoginStatusBindings(env, mcLoader);
    cacheBundleBindings(env, mcLoader);
    cacheModernNetworkingBindings(env, mcLoader);
    cacheConfigurationBindings(env, mcLoader);
    cachePacketConstructionBindings(env, mcLoader);
    cacheRegistrySyncBindings(env, mcLoader);

    LogTo("cacheJavaRefs: configureSer=%p configureSerModern=%p send=%p attrProtoFid=%p bundlerFid=%p "
          "protoHS=%p protoLOGIN=%p protoCONFIG=%p protoPLAY=%p flowSB=%p pipMid=%p replace=%p "
          "attrSet=%p gpCtor=%p lfpCtor=%p helloName=%p uuidFromBytes=%p",
          (void*)server.refs.connectionConfigureSerMid, (void*)server.refs.connectionConfigureSerializationModernMid,
          (void*)server.refs.connectionSendMid, (void*)server.refs.connectionAttrProtocolFid,
          (void*)server.refs.bundlerProviderFid, (void*)server.refs.protoHandshaking, (void*)server.refs.protoLogin,
          (void*)server.refs.protoConfiguration, (void*)server.refs.protoPlay, (void*)server.refs.flowServerbound,
          (void*)server.refs.channelPipelineMid, (void*)server.refs.pipelineReplaceMid, (void*)server.refs.attributeSetMid,
          (void*)server.refs.gameProfileCtor, (void*)server.refs.loginFinishedPacketCtor,
          (void*)server.refs.helloPacketNameFid, (void*)server.refs.uuidNameUuidFromBytesMid);
    if (env->ExceptionCheck())
        env->ExceptionClear();

    cacheStatusResponse(env, mcLoader);
    return server.refs.connectionSendMid && (server.refs.connectionConfigureSerMid ||
                                             server.refs.connectionConfigureSerializationModernMid) &&
           server.refs.flowServerbound && server.refs.channelPipelineMid && server.refs.pipelineAddLastMid &&
           server.refs.channelWriteAndFlushMid && server.refs.channelAttrMid && server.refs.attributeSetMid;
}

} // namespace proxy_server
