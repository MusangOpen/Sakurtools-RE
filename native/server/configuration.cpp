// Configuration-phase handoff (Minecraft 1.20.2 and newer).
//
// Since 1.20.2 a client only reaches PLAY after LOGIN -> CONFIGURATION -> PLAY. The proxy plays the
// server role for B, so it has to drive that phase itself:
//
//   * mid-session (A is already in game)  - the configuration payload is rebuilt from A's live
//     RegistryAccess through RegistrySynchronization, exactly like a vanilla server does.
//   * A not connected yet                 - A's own configuration stream is mirrored to B and the
//     switch to PLAY happens when B acknowledges the forwarded finish-configuration packet.
//
// 1.21.9 replaces the ConnectionProtocol channel attribute with ProtocolInfo objects, so switching
// state means installing fresh PacketEncoder/PacketDecoder handlers. The legacy path (1.20.1) keeps
// using the attribute and setProtocol, see transport.cpp.
#include "classfile.h"
#include "internal.h"
#include "random_name.h"
#include <vector>

namespace proxy_server {
namespace {

jclass g_contextClass = nullptr;
jclass g_registrySinkClass = nullptr;

// GameProtocols$Context.hasInfiniteMaterials() selects the item-component codec for serverbound play
// packets. The vanilla client always reports true here (ClientConfigurationPacketListenerImpl$1), and
// the proxy decodes what that client encoded, so it mirrors the client.
jboolean JNICALL Native_PlayContext_hasInfiniteMaterials(JNIEnv*, jobject) {
    return JNI_TRUE;
}

// BiConsumer<ResourceKey, List> sink used by RegistrySynchronization.packRegistries: every call
// becomes one ClientboundRegistryDataPacket for B.
void JNICALL Native_RegistrySink_accept(JNIEnv* env, jobject, jobject key, jobject entries) {
    if (!key || !entries || !server.refs.registryDataPacketCls || !server.refs.registryDataPacketCtor)
        return;
    jobject packet = env->NewObject(server.refs.registryDataPacketCls, server.refs.registryDataPacketCtor, key, entries);
    if (!packet || env->ExceptionCheck()) {
        LogAndClearException(env, "configuration/registry packet");
        if (packet)
            env->DeleteLocalRef(packet);
        return;
    }
    writeToB(env, packet);
    env->DeleteLocalRef(packet);
}

std::string internalNameOf(JNIEnv* env, jclass type) {
    jclass clsCls = env->FindClass("java/lang/Class");
    jmethodID getName = env->GetMethodID(clsCls, "getName", "()Ljava/lang/String;");
    env->DeleteLocalRef(clsCls);
    if (!getName)
        return {};
    auto name = static_cast<jstring>(env->CallObjectMethod(type, getName));
    if (!name || env->ExceptionCheck()) {
        env->ExceptionClear();
        return {};
    }
    const char* utf = env->GetStringUTFChars(name, nullptr);
    std::string result = utf ? utf : "";
    if (utf)
        env->ReleaseStringUTFChars(name, utf);
    env->DeleteLocalRef(name);
    for (char& ch : result)
        if (ch == '.')
            ch = '/';
    return result;
}

// Defines a class that implements one interface method natively. Both the play-phase context and
// the registry sink are single-method interfaces, so no Java bytecode bodies are needed.
jclass defineSingleMethodImpl(JNIEnv* env, jobject loader, jclass iface, const char* method, const char* desc,
                              const void* function) {
    std::string internal = internalNameOf(env, iface);
    if (internal.empty())
        return nullptr;
    std::string name = MakeInternalName(GetTrampolinePackage(), GenerateRandomClassName(2, 3));
    ClassBuilder cb(name, "java/lang/Object", 52);
    cb.addInterface(internal);
    cb.addNativeMethod(method, desc, ACC_PUBLIC | ACC_NATIVE);
    std::vector<u1> bytes = cb.build();
    jclass defined = env->DefineClass(name.c_str(), loader, reinterpret_cast<const jbyte*>(bytes.data()),
                                      static_cast<jsize>(bytes.size()));
    if (!defined) {
        LogAndClearException(env, "configuration/DefineImpl");
        return nullptr;
    }
    JNINativeMethod binding{const_cast<char*>(method), const_cast<char*>(desc), const_cast<void*>(function)};
    if (env->RegisterNatives(defined, &binding, 1) != 0) {
        LogAndClearException(env, "configuration/RegisterImpl");
        env->DeleteLocalRef(defined);
        return nullptr;
    }
    jclass global = static_cast<jclass>(env->NewGlobalRef(defined));
    env->DeleteLocalRef(defined);
    return global;
}

bool ensureImplementationClasses(JNIEnv* env, jobject loader) {
    if (!g_contextClass && server.refs.gameContextCls) {
        g_contextClass = defineSingleMethodImpl(env, loader, server.refs.gameContextCls, "hasInfiniteMaterials", "()Z",
                                                reinterpret_cast<const void*>(&Native_PlayContext_hasInfiniteMaterials));
    }
    if (!g_registrySinkClass && server.refs.biConsumerCls) {
        g_registrySinkClass = defineSingleMethodImpl(env, loader, server.refs.biConsumerCls, "accept",
                                                     "(Ljava/lang/Object;Ljava/lang/Object;)V",
                                                     reinterpret_cast<const void*>(&Native_RegistrySink_accept));
    }
    return g_contextClass != nullptr && g_registrySinkClass != nullptr;
}

jobject staticProtocolInfo(JNIEnv* env, jclass owner, jfieldID field) {
    if (!owner || !field)
        return nullptr;
    jobject value = env->GetStaticObjectField(owner, field);
    if (!value || env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return value;
}

// A's live registry access; the play-phase codecs need it to serialize registry-dependent payloads.
jobject registryAccessOfA(JNIEnv* env) {
    if (!server.refs.minecraftCls || !server.refs.mcGetInstanceMid || !server.refs.mcGetConnectionMid)
        return nullptr;
    jobject mc = env->CallStaticObjectMethod(server.refs.minecraftCls, server.refs.mcGetInstanceMid);
    if (!mc || env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    jobject listener = env->CallObjectMethod(mc, server.refs.mcGetConnectionMid);
    env->DeleteLocalRef(mc);
    if (!listener || env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    jobject access = env->CallObjectMethod(listener, server.refs.cplRegistryAccessMid);
    env->DeleteLocalRef(listener);
    if (!access || env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    // 1.20.1 only exposes the unfrozen view; frozen() is unnecessary when it already is frozen.
    jclass accessCls = env->GetObjectClass(access);
    jmethodID freeze = env->GetMethodID(accessCls, "freeze", "()Lnet/minecraft/core/RegistryAccess$Frozen;");
    if (!freeze && env->ExceptionCheck())
        env->ExceptionClear();
    env->DeleteLocalRef(accessCls);
    if (freeze) {
        jobject frozen = env->CallObjectMethod(access, freeze);
        if (frozen && !env->ExceptionCheck()) {
            env->DeleteLocalRef(access);
            return frozen;
        }
        if (env->ExceptionCheck())
            env->ExceptionClear();
    }
    return access;
}

// Binds GameProtocols' play templates to A's registries. SERVERBOUND_TEMPLATE additionally needs a
// GameProtocols$Context instance.
bool bindPlayProtocolInfos(JNIEnv* env, jobject& inbound, jobject& outbound) {
    inbound = nullptr;
    outbound = nullptr;
    if (!server.refs.gameProtocolsCls || !server.refs.simpleUnboundBindMid || !server.refs.unboundBindMid ||
        !server.refs.registryBufDecoratorMid)
        return false;
    jobject access = registryAccessOfA(env);
    if (!access) {
        LogTo("configuration: A's registry access unavailable; cannot bind PLAY codecs");
        return false;
    }
    jobject decorator =
        env->CallStaticObjectMethod(server.refs.registryFriendlyBufCls, server.refs.registryBufDecoratorMid, access);
    if (!decorator || env->ExceptionCheck()) {
        LogAndClearException(env, "configuration/decorator");
        env->DeleteLocalRef(access);
        return false;
    }
    jobject clientbound = staticProtocolInfo(env, server.refs.gameProtocolsCls, server.refs.gameClientboundTemplateFid);
    if (clientbound) {
        outbound = env->CallObjectMethod(clientbound, server.refs.simpleUnboundBindMid, decorator);
        if (env->ExceptionCheck()) {
            LogAndClearException(env, "configuration/bindClientbound");
            outbound = nullptr;
        }
        env->DeleteLocalRef(clientbound);
    }
    jobject serverbound = staticProtocolInfo(env, server.refs.gameProtocolsCls, server.refs.gameServerboundTemplateFid);
    jobject context = nullptr;
    if (serverbound && g_contextClass)
        context = env->AllocObject(g_contextClass);
    if (serverbound && context) {
        inbound = env->CallObjectMethod(serverbound, server.refs.unboundBindMid, decorator, context);
        if (env->ExceptionCheck()) {
            LogAndClearException(env, "configuration/bindServerbound");
            inbound = nullptr;
        }
    }
    if (context)
        env->DeleteLocalRef(context);
    if (serverbound)
        env->DeleteLocalRef(serverbound);
    env->DeleteLocalRef(decorator);
    env->DeleteLocalRef(access);
    return outbound != nullptr;
}

bool replaceCodec(JNIEnv* env, jobject pipeline, const char* primary, const char* alternate, jclass handlerCls,
                  jmethodID ctor, jobject info) {
    if (!info)
        return true;
    jobject handler = env->NewObject(handlerCls, ctor, info);
    if (!handler || env->ExceptionCheck()) {
        LogAndClearException(env, "configuration/codec");
        if (handler)
            env->DeleteLocalRef(handler);
        return false;
    }
    bool replaced = false;
    const char* names[] = {primary, alternate};
    for (const char* name : names) {
        if (!name)
            continue;
        jstring from = env->NewStringUTF(name);
        jstring to = env->NewStringUTF(name);
        env->CallObjectMethod(pipeline, server.refs.pipelineReplaceMid, from, to, handler);
        env->DeleteLocalRef(from);
        env->DeleteLocalRef(to);
        if (!env->ExceptionCheck()) {
            replaced = true;
            break;
        }
        env->ExceptionClear();
    }
    env->DeleteLocalRef(handler);
    if (!replaced)
        LogTo("configuration: no '%s' handler in pipeline", primary);
    return replaced;
}

bool installModernCodecs(JNIEnv* env, jobject channel, jobject inbound, jobject outbound) {
    jobject pipeline = env->CallObjectMethod(channel, server.refs.channelPipelineMid);
    if (!pipeline || env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    bool ok = true;
    if (inbound)
        ok = replaceCodec(env, pipeline, "decoder", "inbound_config", server.refs.packetDecoderCls,
                          server.refs.packetDecoderCtor, inbound) &&
             ok;
    if (outbound)
        ok = replaceCodec(env, pipeline, "encoder", "outbound_config", server.refs.packetEncoderCls,
                          server.refs.packetEncoderCtor, outbound) &&
             ok;
    env->DeleteLocalRef(pipeline);
    return ok;
}

bool buildEnabledFeaturesPacket(JNIEnv* env, jobject& packet) {
    packet = nullptr;
    if (!server.refs.enabledFeaturesPacketCls || !server.refs.enabledFeaturesPacketCtor || !server.refs.minecraftCls ||
        !server.refs.mcGetInstanceMid || !server.refs.mcGetConnectionMid || !server.refs.cplEnabledFeaturesMid ||
        !server.refs.featureFlagsRegistryFid || !server.refs.featureFlagRegistryToNamesMid)
        return false;
    jobject mc = env->CallStaticObjectMethod(server.refs.minecraftCls, server.refs.mcGetInstanceMid);
    if (!mc || env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    jobject listener = env->CallObjectMethod(mc, server.refs.mcGetConnectionMid);
    env->DeleteLocalRef(mc);
    if (!listener || env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    jobject flags = env->CallObjectMethod(listener, server.refs.cplEnabledFeaturesMid);
    env->DeleteLocalRef(listener);
    if (!flags || env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    jobject registry = env->GetStaticObjectField(server.refs.minecraftCls, server.refs.featureFlagsRegistryFid);
    if (!registry || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(flags);
        return false;
    }
    jobject names = env->CallObjectMethod(registry, server.refs.featureFlagRegistryToNamesMid, flags);
    env->DeleteLocalRef(registry);
    env->DeleteLocalRef(flags);
    if (!names || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (names)
            env->DeleteLocalRef(names);
        return false;
    }
    packet = env->NewObject(server.refs.enabledFeaturesPacketCls, server.refs.enabledFeaturesPacketCtor, names);
    env->DeleteLocalRef(names);
    if (!packet || env->ExceptionCheck()) {
        LogAndClearException(env, "configuration/enabledFeatures");
        if (packet)
            env->DeleteLocalRef(packet);
        packet = nullptr;
        return false;
    }
    return true;
}

jobject buildTagsPacket(JNIEnv* env) {
    if (!server.refs.updateTagsPacketCls || !server.refs.updateTagsPacketCtor || !server.refs.hashMapCls ||
        !server.refs.registryAccessRegistriesMid || !server.refs.serializeTagsToNetworkMid)
        return nullptr;
    jobject access = registryAccessOfA(env);
    if (!access)
        return nullptr;
    jobject map = env->NewObject(server.refs.hashMapCls, server.refs.hashMapCtor);
    jobject stream = map ? env->CallObjectMethod(access, server.refs.registryAccessRegistriesMid) : nullptr;
    jobject iterator = stream ? env->CallObjectMethod(stream, server.refs.streamIteratorMid) : nullptr;
    if (!map || !iterator || env->ExceptionCheck()) {
        LogAndClearException(env, "configuration/tags iterator");
        if (iterator)
            env->DeleteLocalRef(iterator);
        if (stream)
            env->DeleteLocalRef(stream);
        if (map)
            env->DeleteLocalRef(map);
        env->DeleteLocalRef(access);
        return nullptr;
    }
    int count = 0;
    while (env->CallBooleanMethod(iterator, server.refs.iteratorHasNextMid) == JNI_TRUE) {
        jobject entry = env->CallObjectMethod(iterator, server.refs.iteratorNextMid);
        if (!entry || env->ExceptionCheck()) {
            env->ExceptionClear();
            break;
        }
        jobject key = env->CallObjectMethod(entry, server.refs.registryEntryKeyMid);
        jobject registry = env->CallObjectMethod(entry, server.refs.registryEntryValueMid);
        if (key && registry && !env->ExceptionCheck()) {
            jboolean networkable = env->CallStaticBooleanMethod(server.refs.registrySynchronizationCls,
                                                                server.refs.isNetworkableMid, key);
            if (networkable == JNI_TRUE) {
                jobject payload = env->CallStaticObjectMethod(server.refs.tagSerializationCls,
                                                              server.refs.serializeTagsToNetworkMid, registry);
                if (payload && !env->ExceptionCheck()) {
                    env->CallObjectMethod(map, server.refs.mapPutMid, key, payload);
                    ++count;
                }
                if (payload)
                    env->DeleteLocalRef(payload);
            }
        }
        if (env->ExceptionCheck())
            env->ExceptionClear();
        if (registry)
            env->DeleteLocalRef(registry);
        if (key)
            env->DeleteLocalRef(key);
        env->DeleteLocalRef(entry);
    }
    env->DeleteLocalRef(iterator);
    env->DeleteLocalRef(stream);
    env->DeleteLocalRef(access);
    jobject packet = env->NewObject(server.refs.updateTagsPacketCls, server.refs.updateTagsPacketCtor, map);
    env->DeleteLocalRef(map);
    if (!packet || env->ExceptionCheck()) {
        LogAndClearException(env, "configuration/tags packet");
        if (packet)
            env->DeleteLocalRef(packet);
        return nullptr;
    }
    LogTo("configuration: mirrored tags for %d registries", count);
    return packet;
}

} // namespace

bool applyProtocolState(JNIEnv* env, jobject channel, jobject protocolValue) {
    if (!channel || !protocolValue)
        return false;
    if (!server.refs.modernNetworking) {
        setProtocolState(env, channel, protocolValue);
        return true;
    }
    jobject inbound = nullptr;
    jobject outbound = nullptr;
    if (env->IsSameObject(protocolValue, server.refs.protoHandshaking)) {
        inbound = staticProtocolInfo(env, server.refs.handshakeProtocolsCls, server.refs.handshakeServerboundFid);
    } else if (env->IsSameObject(protocolValue, server.refs.protoStatus)) {
        inbound = staticProtocolInfo(env, server.refs.statusProtocolsCls, server.refs.statusServerboundFid);
        outbound = staticProtocolInfo(env, server.refs.statusProtocolsCls, server.refs.statusClientboundFid);
    } else if (env->IsSameObject(protocolValue, server.refs.protoLogin)) {
        inbound = staticProtocolInfo(env, server.refs.loginProtocolsCls, server.refs.loginServerboundFid);
        outbound = staticProtocolInfo(env, server.refs.loginProtocolsCls, server.refs.loginClientboundFid);
    } else if (server.refs.protoConfiguration && env->IsSameObject(protocolValue, server.refs.protoConfiguration)) {
        inbound = staticProtocolInfo(env, server.refs.configProtocolsCls, server.refs.configServerboundFid);
        outbound = staticProtocolInfo(env, server.refs.configProtocolsCls, server.refs.configClientboundFid);
    } else {
        jobject mcLoader = GetMinecraftClassLoader(env, g_jvmti);
        if (mcLoader) {
            ensureImplementationClasses(env, mcLoader);
            env->DeleteGlobalRef(mcLoader);
        }
        if (!bindPlayProtocolInfos(env, inbound, outbound)) {
            LogAndClearException(env, "configuration/play protocol");
            return false;
        }
        LogTo("configuration: PLAY codecs bound to A's registries");
    }
    bool ok = installModernCodecs(env, channel, inbound, outbound);
    if (inbound)
        env->DeleteLocalRef(inbound);
    if (outbound)
        env->DeleteLocalRef(outbound);
    LogAndClearException(env, "configuration/apply protocol");
    return ok;
}

bool beginConfigurationPhase(JNIEnv* env, jobject channel) {
    if (!server.refs.configurationPhase)
        return false;
    return applyProtocolState(env, channel, server.refs.protoConfiguration);
}

bool synthesizeConfigurationPhase(JNIEnv* env, jobject channel) {
    if (!server.refs.configurationPhase)
        return false;
    bool ok = true;
    jobject features = nullptr;
    if (buildEnabledFeaturesPacket(env, features)) {
        writeToB(env, features);
        env->DeleteLocalRef(features);
    } else {
        LogTo("configuration: enabled features unavailable; B keeps default flags");
    }

    jobject access = registryAccessOfA(env);
    jobject consumer = nullptr;
    if (access && g_registrySinkClass && server.refs.registryAccessSerializationContextMid &&
        server.refs.nbtOpsInstanceFid && server.refs.packRegistriesMid) {
        consumer = env->AllocObject(g_registrySinkClass);
        jobject nbtOps = env->GetStaticObjectField(server.refs.nbtOpsCls, server.refs.nbtOpsInstanceFid);
        jobject ops = nullptr;
        if (nbtOps && server.refs.registryAccessSerializationContextMid)
            ops = env->CallObjectMethod(access, server.refs.registryAccessSerializationContextMid, nbtOps);
        if ((!ops || env->ExceptionCheck()) && nbtOps && server.refs.registryOpsCreateMid) {
            if (env->ExceptionCheck())
                env->ExceptionClear();
            ops = env->CallStaticObjectMethod(server.refs.registryOpsCls, server.refs.registryOpsCreateMid, nbtOps,
                                              access);
        }
        jobject emptySet = nullptr;
        jclass collections = env->FindClass("java/util/Collections");
        if (collections) {
            jmethodID empty = env->GetStaticMethodID(collections, "emptySet", "()Ljava/util/Set;");
            if (empty)
                emptySet = env->CallStaticObjectMethod(collections, empty);
            env->DeleteLocalRef(collections);
        }
        if (ops && emptySet && !env->ExceptionCheck()) {
            // Vanilla asks for known packs first; an empty set simply means "send everything".
            env->CallStaticVoidMethod(server.refs.registrySynchronizationCls, server.refs.packRegistriesMid, ops,
                                      access, emptySet, consumer);
            if (env->ExceptionCheck()) {
                LogAndClearException(env, "configuration/packRegistries");
                ok = false;
            }
        } else {
            LogAndClearException(env, "configuration/packRegistries setup");
            ok = false;
        }
        if (emptySet)
            env->DeleteLocalRef(emptySet);
        if (ops)
            env->DeleteLocalRef(ops);
        if (nbtOps)
            env->DeleteLocalRef(nbtOps);
    } else {
        LogTo("configuration: registry synchronization unavailable");
        ok = false;
    }
    if (consumer)
        env->DeleteLocalRef(consumer);
    if (access)
        env->DeleteLocalRef(access);

    jobject tags = buildTagsPacket(env);
    if (tags) {
        writeToB(env, tags);
        env->DeleteLocalRef(tags);
    }

    if (server.refs.finishConfigurationPacketCls) {
        jclass cls = server.refs.finishConfigurationPacketCls;
        jmethodID ctor = env->GetMethodID(cls, "<init>", "()V");
        if (env->ExceptionCheck())
            env->ExceptionClear();
        if (ctor) {
            jobject finish = env->NewObject(cls, ctor);
            if (finish && !env->ExceptionCheck()) {
                writeToB(env, finish);
                env->DeleteLocalRef(finish);
            } else {
                LogAndClearException(env, "configuration/finish packet");
                ok = false;
            }
        }
    }
    LogTo("configuration: synthesized configuration phase for B (ok=%s)", ok ? "true" : "false");
    return ok;
}

bool isFinishConfigurationPacket(JNIEnv* env, jobject packet) {
    return env && packet && server.refs.serverboundFinishConfigurationCls &&
           env->IsInstanceOf(packet, server.refs.serverboundFinishConfigurationCls) == JNI_TRUE;
}

bool isClientboundFinishConfigurationPacket(JNIEnv* env, jobject packet) {
    return env && packet && server.refs.finishConfigurationPacketCls &&
           env->IsInstanceOf(packet, server.refs.finishConfigurationPacketCls) == JNI_TRUE;
}

} // namespace proxy_server
