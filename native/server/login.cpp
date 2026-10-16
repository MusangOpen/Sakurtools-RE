#include "internal.h"
#include "world_snapshot.h"

namespace proxy_server {

bool beginSnapshot(JNIEnv* env) {
    std::lock_guard<std::recursive_mutex> lock(server.dispatchMutex);
    if (server.stopping.load() || server.handoff.state() != LoginHandoff::State::Capturing)
        return JNI_FALSE;
    // A's earlier packet tasks have run on the render thread. The snapshot includes
    // them; packets dispatched after this boundary remain buffered as live deltas.
    clearPendingPackets(env);
    return JNI_TRUE;
}

void writeToB(JNIEnv* env, jobject packet) {
    jobject ch;
    {
        std::lock_guard<std::mutex> l(server.clientMutex);
        ch = server.clientChannel ? env->NewLocalRef(server.clientChannel) : nullptr;
    }
    if (!ch)
        return;
    env->CallObjectMethod(ch, server.refs.channelWriteAndFlushMid, packet);
    if (env->ExceptionCheck())
        LogAndClearException(env, "writeAndFlush");
    env->DeleteLocalRef(ch);
}

bool uuidToBytes(JNIEnv* env, jobject uuid, unsigned char out[16]) {
    if (!uuid || !server.refs.uuidGetMsbMid || !server.refs.uuidGetLsbMid)
        return false;
    jlong msb = env->CallLongMethod(uuid, server.refs.uuidGetMsbMid);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    jlong lsb = env->CallLongMethod(uuid, server.refs.uuidGetLsbMid);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    for (int i = 0; i < 8; ++i)
        out[i] = (unsigned char)((msb >> ((7 - i) * 8)) & 0xFF);
    for (int i = 0; i < 8; ++i)
        out[8 + i] = (unsigned char)((lsb >> ((7 - i) * 8)) & 0xFF);
    return true;
}

void ensureARealUuid(JNIEnv* env) {
    if (server.aPlayer.ready && server.aPlayer.name)
        return;
    if (!server.refs.minecraftCls || !server.refs.mcGetInstanceMid || !server.refs.mcGetUserMid)
        return;
    jobject mc = env->CallStaticObjectMethod(server.refs.minecraftCls, server.refs.mcGetInstanceMid);
    if (!mc || env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }
    jobject user = env->CallObjectMethod(mc, server.refs.mcGetUserMid);
    env->DeleteLocalRef(mc);
    if (!user || env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }

    if (!server.aPlayer.ready && server.refs.userGetProfileIdMid) {
        jobject uuid = env->CallObjectMethod(user, server.refs.userGetProfileIdMid);
        if (uuid && !env->ExceptionCheck()) {
            unsigned char bytes[16];
            if (uuidToBytes(env, uuid, bytes)) {
                server.aPlayer.uuid = env->NewGlobalRef(uuid);
                std::memcpy(server.aPlayer.uuidBytes, bytes, 16);
                server.aPlayer.ready = true;
                LogTo("mirror: cached A's real UUID for tab-list mirroring");
            }
            env->DeleteLocalRef(uuid);
        } else if (env->ExceptionCheck())
            env->ExceptionClear();
    }
    if (!server.aPlayer.name && server.refs.userGetGameProfileMid && server.refs.gameProfileGetNameMid) {
        jobject profile = env->CallObjectMethod(user, server.refs.userGetGameProfileMid);
        if (profile && !env->ExceptionCheck()) {
            jstring nm = (jstring)env->CallObjectMethod(profile, server.refs.gameProfileGetNameMid);
            if (nm && !env->ExceptionCheck()) {
                server.aPlayer.name = (jstring)env->NewGlobalRef(nm);
                const char* utf = env->GetStringUTFChars(nm, nullptr);
                LogTo("team-mirror: cached A's name = '%s'", utf ? utf : "?");
                if (utf)
                    env->ReleaseStringUTFChars(nm, utf);
                env->DeleteLocalRef(nm);
            } else if (env->ExceptionCheck())
                env->ExceptionClear();
            env->DeleteLocalRef(profile);
        } else if (env->ExceptionCheck())
            env->ExceptionClear();
    }
    env->DeleteLocalRef(user);
}

void sendSelfInfoToB(JNIEnv* env) {
    jobject offlineUuid = server.bPlayer.uuid;
    jstring name = server.bPlayer.name;
    if (!offlineUuid || !name) {
        LogTo("self-info: B identity not ready, skipping push");
        return;
    }
    jobject profile = nullptr;
    if (server.refs.gameProfileCls && server.refs.gameProfileCtor)
        profile = env->NewObject(server.refs.gameProfileCls, server.refs.gameProfileCtor, offlineUuid, name);
    if (!profile || env->ExceptionCheck()) {
        LogAndClearException(env, "self-info/profile");
        if (profile)
            env->DeleteLocalRef(profile);
        return;
    }
    // Keep B's profile (name/UUID, and its skin on 1.20.1) but list it only once: A's players own
    // the TAB entries, so the mirrored entry stays unlisted.
    jobject packet = buildPlayerInfoUpdate(env, kPiuAddPlayer | kPiuGameMode | kPiuListed | kPiuLatency,
                                           offlineUuid, profile, JNI_FALSE, 0, nullptr, nullptr);
    env->DeleteLocalRef(profile);
    if (!packet)
        return;
    writeToB(env, packet);
    env->DeleteLocalRef(packet);
    LogTo("self-info: pushed B's own player-info to B (unlisted, A's skin)");
}

bool reconstructAndSendLoginToB(JNIEnv* env, jobject ch) {
    jobject mc = env->CallStaticObjectMethod(server.refs.minecraftCls, server.refs.mcGetInstanceMid);
    jobject target;
    {
        std::lock_guard<std::mutex> lock(server.targetMutex);
        target = server.targetConnection ? env->NewLocalRef(server.targetConnection) : nullptr;
    }
    if (!mc || !target || env->ExceptionCheck()) {
        if (mc)
            env->DeleteLocalRef(mc);
        if (target)
            env->DeleteLocalRef(target);
        LogAndClearException(env, "snapshot/target");
        return false;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(server.dispatchMutex);
        clearPendingPackets(env);
        server.handoff.begin();
        server.clientState.store(ClientState::Syncing, std::memory_order_release);
    }
    auto started = std::chrono::steady_clock::now();
    std::vector<jobject> packets;
    bool captured = CaptureWorldSnapshot(env, mc, target, beginSnapshot, packets);
    env->DeleteLocalRef(mc);
    env->DeleteLocalRef(target);
    if (!captured || env->ExceptionCheck()) {
        LogAndClearException(env, "snapshot/capture");
        for (jobject packet : packets)
            env->DeleteGlobalRef(packet);
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(server.dispatchMutex);
    bool ok = !server.stopping.load() && server.handoff.state() == LoginHandoff::State::Capturing;
    const size_t count = packets.size();
    for (jobject packet : packets) {
        if (ok) {
            jobject outgoing = nullptr;
            ok = prepareTeamPacket(env, packet, outgoing);
            if (ok && outgoing) ok = WriteSnapshotPacket(env, ch, outgoing);
            if (outgoing) env->DeleteLocalRef(outgoing);
        }
        env->DeleteGlobalRef(packet);
    }
    if (!ok) {
        LogAndClearException(env, "snapshot/write");
        clearPendingPackets(env);
        server.handoff.fail();
        return false;
    }
    LogTo("mid-login: snapshot queued packets=%d elapsed=%lldms", (int)count,
          (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
              .count());
    return true;
}

// A's Render thread waits on this gate until B is far enough along to receive A's join stream.
// On 1.20.2+ that moment is the start of B's configuration phase, because the configuration payload
// itself comes from A connecting.
void releaseMainGate(JNIEnv*) {
    {
        std::lock_guard<std::mutex> lock(server.gateMutex);
        server.clientConnected = true;
    }
    server.gateChanged.notify_all();
    LogTo("login: released A's Render thread — A now connects and its live join stream feeds B");
}

// B reached PLAY. Send its own TAB entry, then either hand over A's world snapshot (mid-session) or
// simply start forwarding A's play stream.
bool EnterPlayPhase(JNIEnv* env, jobject ch) {
    sendSelfInfoToB(env);
    if (!server.midSession.load(std::memory_order_acquire)) {
        server.clientState.store(ClientState::Play, std::memory_order_release);
        return true;
    }
    if (!ch)
        return false;
    LogTo("mid-login: rebuilding login for B from A's state");
    server.clientState.store(ClientState::Syncing, std::memory_order_release);
    if (!reconstructAndSendLoginToB(env, ch)) {
        LogTo("mid-login: reconstruction failed; closing B instead of entering empty PLAY");
        env->CallObjectMethod(ch, server.refs.channelCloseMid);
        if (env->ExceptionCheck())
            LogAndClearException(env, "mid-login/close");
        return false;
    }
    {
        std::lock_guard<std::recursive_mutex> dispatch(server.dispatchMutex);
        if (server.stopping.load())
            return false;
        if (!server.handoff.snapshotSent()) {
            server.handoff.fail();
            BServer_CheckLoginTimeout(env);
            return false;
        }
        server.clientState.store(ClientState::AwaitReady, std::memory_order_release);
        std::vector<jobject> pending;
        pending.swap(server.pendingPackets);
        for (jobject packet : pending) {
            BServer_ForwardToB(env, packet);
            env->DeleteGlobalRef(packet);
        }
    }
    jobject channel;
    {
        std::lock_guard<std::mutex> lock(server.clientMutex);
        channel = server.clientChannel ? env->NewLocalRef(server.clientChannel) : nullptr;
    }
    if (channel) {
        jclass type = env->GetObjectClass(channel);
        jmethodID flush = env->GetMethodID(type, "flush", "()Lio/netty/channel/Channel;");
        if (flush)
            env->CallObjectMethod(channel, flush);
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(type);
        env->DeleteLocalRef(channel);
    }
    LogTo("mid-login: snapshot sent; waiting for B initialization acknowledgement");
    return true;
}

void completeLogin(JNIEnv* env, jobject hello) {

    if (!server.refs.helloPacketNameFid) {
        LogTo("login: no hello.name field");
        return;
    }
    jstring jname = (jstring)env->GetObjectField(hello, server.refs.helloPacketNameFid);
    if (!jname) {
        LogTo("login: hello.name is null");
        return;
    }
    const char* utfName = env->GetStringUTFChars(jname, nullptr);
    LogTo("login: B says name='%s'", utfName ? utfName : "?");

    std::string composite = std::string("OfflinePlayer:") + (utfName ? utfName : "");
    if (utfName)
        env->ReleaseStringUTFChars(jname, utfName);
    jbyteArray arr = env->NewByteArray((jsize)composite.size());
    env->SetByteArrayRegion(arr, 0, (jsize)composite.size(), reinterpret_cast<const jbyte*>(composite.data()));
    jobject uuid = env->CallStaticObjectMethod(server.refs.uuidCls, server.refs.uuidNameUuidFromBytesMid, arr);
    env->DeleteLocalRef(arr);
    if (!uuid || env->ExceptionCheck()) {
        LogAndClearException(env, "login: nameUUIDFromBytes");
        env->DeleteLocalRef(jname);
        return;
    }
    LogTo("login: computed offline UUID");

    if (!server.refs.gameProfileCtor) {
        LogTo("login: no GameProfile ctor");
        env->DeleteLocalRef(uuid);
        env->DeleteLocalRef(jname);
        return;
    }
    jobject profile = env->NewObject(server.refs.gameProfileCls, server.refs.gameProfileCtor, uuid, jname);
    if (!profile || env->ExceptionCheck()) {
        LogAndClearException(env, "login: GameProfile ctor");
        env->DeleteLocalRef(uuid);
        env->DeleteLocalRef(jname);
        return;
    }

    if (!server.refs.loginFinishedPacketCtor) {
        LogTo("login: no LoginFinished ctor");
        return;
    }
    jobject lfp = env->NewObject(server.refs.loginFinishedPacketCls, server.refs.loginFinishedPacketCtor, profile);
    env->DeleteLocalRef(profile);
    if (!lfp || env->ExceptionCheck()) {
        LogAndClearException(env, "login: LoginFinished ctor");
        return;
    }

    writeToB(env, lfp);
    env->DeleteLocalRef(lfp);
    LogTo("login: sent ClientboundGameProfilePacket to B");

    {
        std::lock_guard<std::recursive_mutex> dispatch(server.dispatchMutex);
        server.teams = TeamState{};
        if (server.bPlayer.name) env->DeleteGlobalRef(server.bPlayer.name);
        server.bPlayer.name = static_cast<jstring>(env->NewGlobalRef(jname));
        if (server.bPlayer.uuid) env->DeleteGlobalRef(server.bPlayer.uuid);
        server.bPlayer.uuid = static_cast<jobject>(env->NewGlobalRef(uuid));
        unsigned char bBytes[16];
        if (uuidToBytes(env, uuid, bBytes)) {
            std::memcpy(server.bPlayer.uuidBytes, bBytes, 16);
            server.bPlayer.ready = true;
        }
        ensureARealUuid(env);
    }

    jobject ch;
    {
        std::lock_guard<std::mutex> l(server.clientMutex);
        ch = server.clientChannel ? env->NewLocalRef(server.clientChannel) : nullptr;
    }
    if (!ch) {
        env->DeleteLocalRef(uuid);
        env->DeleteLocalRef(jname);
        return;
    }

    if (server.refs.configurationPhase) {
        // 1.20.2+: B answers the login phase with ServerboundLoginAcknowledgedPacket and only then
        // switch the codecs. The acknowledgement arrives while the channel still runs LOGIN, which
        // is why the switch happens in the packet handler rather than here.
        server.configurationStarted = false;
        server.clientState.store(ClientState::Configuring, std::memory_order_release);
        LogTo("login: B is in LOGIN; waiting for the login acknowledgement before CONFIGURATION");
    } else {
        applyProtocolState(env, ch, server.refs.protoPlay);
        LogTo("login: protocol switched to PLAY");
        if (server.midSession.load(std::memory_order_acquire)) {
            EnterPlayPhase(env, ch);
        } else {
            server.clientState.store(ClientState::Play, std::memory_order_release);
            releaseMainGate(env);
        }
    }
    env->DeleteLocalRef(ch);
    env->DeleteLocalRef(uuid);
    env->DeleteLocalRef(jname);
}

} // namespace proxy_server
