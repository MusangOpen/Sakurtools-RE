#include "internal.h"
#include "packet_policy.h"
#include "relay_handler.h"
#include "status_response.h"

namespace proxy_server {

void routeToA(JNIEnv* env, jobject packet) {
    jobject target;
    {
        std::lock_guard<std::mutex> l(server.targetMutex);
        target = server.targetConnection ? env->NewLocalRef(server.targetConnection) : nullptr;
    }
    if (!target)
        return;
    if (!server.refs.connectionSendMid) {
        env->DeleteLocalRef(target);
        return;
    }

    RelayFilter_MarkBypass(env, packet);
    env->CallVoidMethod(target, server.refs.connectionSendMid, packet);
    if (env->ExceptionCheck())
        LogAndClearException(env, "routeToA");
    env->DeleteLocalRef(target);
}

void mirrorPlayerInfoUpdateToB(JNIEnv* env, jobject ch, jobject packet) {
    if (!server.refs.playerInfoUpdatePacketCls || !server.refs.listSizeMid || !server.refs.listGetMid ||
        !server.refs.piuEntriesMidA || !server.refs.piEntryProfileIdMid)
        return;
    if (!server.aPlayer.ready || !server.bPlayer.ready || !server.bPlayer.uuid || !server.bPlayer.name)
        return;
    if (!env->IsInstanceOf(packet, server.refs.playerInfoUpdatePacketCls))
        return;

    jobject listA = env->CallObjectMethod(packet, server.refs.piuEntriesMidA);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        listA = nullptr;
    }
    jobject listB = server.refs.piuEntriesMidB ? env->CallObjectMethod(packet, server.refs.piuEntriesMidB) : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        listB = nullptr;
    }
    jint sizeA = -1, sizeB = -1;
    if (listA) {
        sizeA = env->CallIntMethod(listA, server.refs.listSizeMid);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            sizeA = -1;
        }
    }
    if (listB) {
        sizeB = env->CallIntMethod(listB, server.refs.listSizeMid);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            sizeB = -1;
        }
    }
    jobject entries = (sizeB > sizeA) ? listB : listA;
    jint nEntries = (sizeB > sizeA) ? sizeB : sizeA;

    jobject aEntry = nullptr;
    for (jint i = 0; i < nEntries && !aEntry; ++i) {
        jobject e = env->CallObjectMethod(entries, server.refs.listGetMid, i);
        if (!e || env->ExceptionCheck()) {
            env->ExceptionClear();
            if (e)
                env->DeleteLocalRef(e);
            continue;
        }
        jobject euuid = env->CallObjectMethod(e, server.refs.piEntryProfileIdMid);
        if (euuid && !env->ExceptionCheck()) {
            unsigned char eb[16];
            if (uuidToBytes(env, euuid, eb) && std::memcmp(eb, server.aPlayer.uuidBytes, 16) == 0)
                aEntry = env->NewLocalRef(e);
            env->DeleteLocalRef(euuid);
        } else if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(e);
    }
    if (listA)
        env->DeleteLocalRef(listA);
    if (listB)
        env->DeleteLocalRef(listB);
    if (!aEntry)
        return;

    jobject gameMode = server.refs.piEntryGameModeMid ? env->CallObjectMethod(aEntry, server.refs.piEntryGameModeMid)
                                                      : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        gameMode = nullptr;
    }
    jint latency = 0;
    if (server.refs.piEntryLatencyMid) {
        latency = env->CallIntMethod(aEntry, server.refs.piEntryLatencyMid);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            latency = 0;
        }
    }
    env->DeleteLocalRef(aEntry);

    // B keeps A's game mode, latency and display name but is never added as a second visible entry.
    jobject profile = env->NewObject(server.refs.gameProfileCls, server.refs.gameProfileCtor, server.bPlayer.uuid,
                                     server.bPlayer.name);
    if (!profile || env->ExceptionCheck()) {
        LogAndClearException(env, "mirror/profile");
        if (gameMode)
            env->DeleteLocalRef(gameMode);
        return;
    }
    jobject mirrored = buildPlayerInfoUpdate(env, kPiuGameMode | kPiuListed | kPiuLatency, server.bPlayer.uuid,
                                             profile, JNI_FALSE, latency, gameMode, nullptr);
    env->DeleteLocalRef(profile);
    if (gameMode)
        env->DeleteLocalRef(gameMode);
    if (!mirrored)
        return;
    env->CallObjectMethod(ch, server.refs.channelWriteAndFlushMid, mirrored);
    if (env->ExceptionCheck())
        LogAndClearException(env, "mirror: writeAndFlush");
    env->DeleteLocalRef(mirrored);
    LogTo("mirror: single-entry PlayerInfoUpdate to B (latency=%d)", latency);
}

void writeClientPacket(JNIEnv* env, jobject ch, jobject packet) {

    if (server.refs.customPayloadPacketCls && env->IsInstanceOf(packet, server.refs.customPayloadPacketCls)) {
        static std::atomic<int> dropped{0};
        int n = dropped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || (n & 0x3FF) == 0)
            LogTo("ForwardToB: skipping ClientboundCustomPayloadPacket (mod channel, count=%d)", n);
        return;
    }
    jobject outgoing = nullptr;
    if (!prepareTeamPacket(env, packet, outgoing)) {
        LogAndClearException(env, "ForwardToB/teams");
        env->CallObjectMethod(ch, server.refs.channelCloseMid);
        LogAndClearException(env, "ForwardToB/close");
        return;
    }
    if (!outgoing) return;
    env->CallObjectMethod(ch, server.refs.channelWriteAndFlushMid, outgoing);
    env->DeleteLocalRef(outgoing);
    if (env->ExceptionCheck()) {
        LogAndClearException(env, "ForwardToB/writeOne");
        env->CallObjectMethod(ch, server.refs.channelCloseMid);
        LogAndClearException(env, "ForwardToB/close");
        return;
    }
    mirrorPlayerInfoUpdateToB(env, ch, packet);
}

bool forwardBundleExpanded(JNIEnv* env, jobject ch, jobject packet) {
    if (!server.refs.bundlePacketCls || !server.refs.bundleSubPacketsMid || !server.refs.iterableIteratorMid ||
        !server.refs.iteratorHasNextMid || !server.refs.iteratorNextMid)
        return false;
    if (!env->IsInstanceOf(packet, server.refs.bundlePacketCls))
        return false;

    jobject iterable = env->CallObjectMethod(packet, server.refs.bundleSubPacketsMid);
    if (env->ExceptionCheck() || !iterable) {
        LogAndClearException(env, "bundle.subPackets");
        return true;
    }
    jobject it = env->CallObjectMethod(iterable, server.refs.iterableIteratorMid);
    env->DeleteLocalRef(iterable);
    if (env->ExceptionCheck() || !it) {
        LogAndClearException(env, "bundle.iterator");
        return true;
    }

    int forwarded = 0, nulls = 0;
    while (true) {
        jboolean more = env->CallBooleanMethod(it, server.refs.iteratorHasNextMid);
        if (env->ExceptionCheck()) {
            LogAndClearException(env, "bundle.iter.hasNext");
            break;
        }
        if (!more)
            break;

        jobject sub = env->CallObjectMethod(it, server.refs.iteratorNextMid);
        if (env->ExceptionCheck()) {
            LogAndClearException(env, "bundle.iter.next");
            break;
        }
        if (!sub) {
            ++nulls;
            continue;
        }

        if (!forwardBundleExpanded(env, ch, sub)) {
            writeClientPacket(env, ch, sub);
        }
        env->DeleteLocalRef(sub);
        ++forwarded;
    }
    env->DeleteLocalRef(it);
    if (nulls > 0)
        LogTo("bundle: forwarded=%d, null-subs=%d (skipped)", forwarded, nulls);
    return true;
}

void handleClientPacket(JNIEnv* env, jobject ctx, jobject msg) {
    jobject ch = env->CallObjectMethod(ctx, server.refs.contextChannelMid);
    if (!ch || env->ExceptionCheck())
        return;
    std::string cls = JniClassName(env, msg);
    ClientState state = server.clientState.load(std::memory_order_acquire);
    if (state == ClientState::AwaitHandshake || state == ClientState::AwaitLogin)
        LogTo("BServer: RX %s (state=%d)", cls.c_str(), (int)state);

    if (server.refs.intentPacketCls && env->IsInstanceOf(msg, server.refs.intentPacketCls)) {
        jobject nextProto = server.refs.protoLogin;
        if (server.refs.intentionPacketIntentFid) {
            jobject intent = env->GetObjectField(msg, server.refs.intentionPacketIntentFid);
            if (intent) {
                if (env->IsSameObject(intent, server.refs.protoStatus))
                    nextProto = server.refs.protoStatus;
                else
                    nextProto = server.refs.protoLogin;
                env->DeleteLocalRef(intent);
            }
        }
        setProtocolState(env, ch, nextProto);
        if (nextProto != server.refs.protoStatus) {
            bool occupied = false;
            {
                std::lock_guard<std::mutex> l(server.clientMutex);
                occupied = server.clientChannel && !env->IsSameObject(ch, server.clientChannel);
                if (!occupied) {
                    if (!server.clientChannel)
                        server.clientChannel = env->NewGlobalRef(ch);
                    if (!server.clientChannel)
                        return;
                    server.clientState.store(ClientState::AwaitLogin, std::memory_order_release);
                }
            }
            if (occupied) {
                env->CallObjectMethod(ch, server.refs.channelCloseMid);
                return;
            }
        }
        LogTo("BServer: intention → %s", nextProto == server.refs.protoStatus ? "STATUS" : "LOGIN");
        return;
    }

    if (server.refs.statusRequestPacketCls && env->IsInstanceOf(msg, server.refs.statusRequestPacketCls)) {
        jobject response = server.refs.statusClassLoader
                               ? BuildStatusResponse(env, server.refs.statusClassLoader, currentServerMotd(env))
                               : nullptr;
        if (!response && server.refs.statusResponse)
            response = env->NewLocalRef(server.refs.statusResponse);
        if (!response) {
            LogTo("STATUS: closing query because response initialization failed");
            env->CallObjectMethod(ch, server.refs.channelCloseMid);
            return;
        }
        env->CallObjectMethod(ch, server.refs.channelWriteAndFlushMid, response);
        if (env->ExceptionCheck())
            LogAndClearException(env, "STATUS/write response");
        else
            LogTo("STATUS: replied MOTD '%s'", kLanMotd);
        return;
    }

    if (server.refs.pingRequestPacketCls && env->IsInstanceOf(msg, server.refs.pingRequestPacketCls)) {
        jlong t = 0;
        if (server.refs.pingRequestPacketTimeFid)
            t = env->GetLongField(msg, server.refs.pingRequestPacketTimeFid);
        if (server.refs.pongResponsePacketCtor) {
            jobject pong = env->NewObject(server.refs.pongResponsePacketCls, server.refs.pongResponsePacketCtor, t);
            if (pong && !env->ExceptionCheck())
                env->CallObjectMethod(ch, server.refs.channelWriteAndFlushMid, pong);
            LogTo("BServer: replied Pong(%lld)", (long long)t);
        }
        return;
    }

    {
        std::lock_guard<std::mutex> l(server.clientMutex);
        if (!server.clientChannel || !env->IsSameObject(ch, server.clientChannel))
            return;
    }
    if (state == ClientState::AwaitLogin && server.refs.helloPacketCls &&
        env->IsInstanceOf(msg, server.refs.helloPacketCls)) {
        completeLogin(env, msg);
        return;
    }

    if (server.clientState.load(std::memory_order_acquire) == ClientState::Configuring) {
        // 1.20.2+ configuration phase. B's own configuration answers belong to its connection, not
        // to A's, so only the two protocol switches are acted upon.
        if (!server.configurationStarted && server.refs.loginAckPacketCls &&
            env->IsInstanceOf(msg, server.refs.loginAckPacketCls)) {
            std::lock_guard<std::recursive_mutex> dispatch(server.dispatchMutex);
            server.configurationStarted = true;
            beginConfigurationPhase(env, ch);
            LogTo("BServer: B acknowledged the login phase; channel switched to CONFIGURATION");
            if (server.midSession.load(std::memory_order_acquire)) {
                if (!synthesizeConfigurationPhase(env, ch)) {
                    LogTo("BServer: configuration handoff failed; closing B");
                    env->CallObjectMethod(ch, server.refs.channelCloseMid);
                    LogAndClearException(env, "configuration/close");
                }
            } else {
                // A's own configuration stream is mirrored to B from here on; release A's Render
                // thread so A can connect and produce it.
                releaseMainGate(env);
            }
            return;
        }
        if (isFinishConfigurationPacket(env, msg)) {
            std::lock_guard<std::recursive_mutex> dispatch(server.dispatchMutex);
            if (applyProtocolState(env, ch, server.refs.protoPlay)) {
                LogTo("BServer: B acknowledged configuration; switching channel to PLAY");
                EnterPlayPhase(env, ch);
            } else {
                LogTo("BServer: PLAY codecs unavailable after configuration; closing B");
                env->CallObjectMethod(ch, server.refs.channelCloseMid);
                LogAndClearException(env, "configuration/close");
            }
            return;
        }
        if (packet_policy::isConfigurationPacket(cls) || packet_policy::isCommonPacket(cls) ||
            packet_policy::isLoginPacket(cls))
            return;
    }

    std::lock_guard<std::recursive_mutex> dispatch(server.dispatchMutex);
    if (cls == "net.minecraft.network.protocol.game.ServerboundAcceptTeleportationPacket" &&
        server.refs.teleportAckId) {
        jint id = env->CallIntMethod(msg, server.refs.teleportAckId);
        if (env->ExceptionCheck())
            return;
        if (server.handoff.acknowledge(id)) {
            if (server.clientState.load() == ClientState::AwaitReady &&
                server.handoff.state() == LoginHandoff::State::Active) {
                server.clientState.store(ClientState::Play, std::memory_order_release);
                LogTo("mid-login: B acknowledged world snapshot; control handed to B");
            }
            return;
        }
    }

    if (server.clientState.load(std::memory_order_acquire) == ClientState::Play) {
        if (packet_policy::allowFromB(cls, true)) {
            routeToA(env, msg);
        }
    }
}

} // namespace proxy_server

using namespace proxy_server;

void BServer_ForwardToB(JNIEnv* env, jobject packet) {
    if (!packet)
        return;
    std::lock_guard<std::recursive_mutex> dispatch(server.dispatchMutex);
    ClientState state = server.clientState.load(std::memory_order_acquire);
    if (server.stopping.load() || (state != ClientState::Play && state != ClientState::Syncing &&
                                   state != ClientState::AwaitReady && state != ClientState::Configuring))
        return;

    std::string cls = JniClassName(env, packet);
    // While B is in its configuration phase, A's configuration and common traffic is mirrored too:
    // those packets carry the registries B needs before PLAY.
    const bool configuring = state == ClientState::Configuring;
    bool forwardable = packet_policy::isGamePacket(cls) ||
                       (configuring && (packet_policy::isConfigurationPacket(cls) || packet_policy::isCommonPacket(cls)));
    if (!forwardable)
        return;
    if (configuring && isClientboundFinishConfigurationPacket(env, packet))
        LogTo("BServer: mirroring A's finish-configuration; waiting for B's acknowledgement");
    if (state == ClientState::Syncing) {
        if (server.handoff.state() != LoginHandoff::State::Capturing)
            return;
        jobject ref = server.pendingPackets.size() < kMaxPendingPackets ? env->NewGlobalRef(packet) : nullptr;
        if (ref)
            server.pendingPackets.push_back(ref);
        else {
            server.handoff.fail();
            clearPendingPackets(env);
            LogTo("mid-login: live packet buffer exhausted; snapshot cancelled");
        }
        return;
    }

    jobject ch;
    {
        std::lock_guard<std::mutex> l(server.clientMutex);
        ch = server.clientChannel ? env->NewLocalRef(server.clientChannel) : nullptr;
    }
    if (!ch)
        return;

    if (!forwardBundleExpanded(env, ch, packet)) {
        writeClientPacket(env, ch, packet);
    }

    env->DeleteLocalRef(ch);
}
