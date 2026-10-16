// PlayerInfoUpdate helpers.
//
// The proxy used to hand-encode the packet body into a FriendlyByteBuf and wrap it with the
// FriendlyByteBuf constructor. That encoding changed repeatedly (1.20.5 added list order and the
// "show hat" flag, 1.21.9 changed the profile payload), so the port builds the packet from its
// public constructor and entry records instead: the wire format is then whatever the running
// version expects.
#include "internal.h"

namespace proxy_server {
namespace {

jobject actionConstant(JNIEnv* env, const char* name) {
    if (!server.refs.playerInfoActionCls)
        return nullptr;
    jfieldID field = env->GetStaticFieldID(server.refs.playerInfoActionCls, name,
                                           "Lnet/minecraft/network/protocol/game/"
                                           "ClientboundPlayerInfoUpdatePacket$Action;");
    if (!field) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return nullptr;
    }
    return env->GetStaticObjectField(server.refs.playerInfoActionCls, field);
}

} // namespace

jobject buildPlayerInfoUpdate(JNIEnv* env, unsigned actions, jobject uuid, jobject profile, jboolean listed,
                              jint latency, jobject gameMode, jobject displayName) {
    if (!server.refs.playerInfoUpdatePacketCls || !server.refs.playerInfoUpdatePacketCtor ||
        !server.refs.playerInfoEntryCls || !server.refs.playerInfoUpdatePacketEntriesFid || !uuid || !profile)
        return nullptr;
    const bool modernEntry = server.refs.playerInfoEntryCtorModern != nullptr;
    if (!modernEntry && !server.refs.playerInfoEntryCtorLegacy)
        return nullptr;

    jobject entry = nullptr;
    if (modernEntry) {
        // showHat / listOrder / chat session keep their defaults.
        entry = env->NewObject(server.refs.playerInfoEntryCls, server.refs.playerInfoEntryCtorModern, uuid, profile,
                               listed, latency, gameMode, displayName, JNI_FALSE, (jint)0, (jobject) nullptr);
    } else {
        entry = env->NewObject(server.refs.playerInfoEntryCls, server.refs.playerInfoEntryCtorLegacy, uuid, profile,
                               listed, latency, gameMode, displayName, (jobject) nullptr);
    }
    if (!entry || env->ExceptionCheck()) {
        LogAndClearException(env, "player-info/entry");
        if (entry)
            env->DeleteLocalRef(entry);
        return nullptr;
    }

    jobject actionSet = nullptr;
    if (server.refs.playerInfoActionCls && server.refs.enumSetCls && server.refs.enumSetNoneOfMid &&
        server.refs.collectionAddMid) {
        actionSet = env->CallStaticObjectMethod(server.refs.enumSetCls, server.refs.enumSetNoneOfMid,
                                                server.refs.playerInfoActionCls);
        if (actionSet && !env->ExceptionCheck()) {
            struct {
                const char* name;
                unsigned bit;
            } wanted[] = {{"ADD_PLAYER", 1u << 0},
                          {"UPDATE_GAME_MODE", 1u << 2},
                          {"UPDATE_LISTED", 1u << 3},
                          {"UPDATE_LATENCY", 1u << 4},
                          {"UPDATE_DISPLAY_NAME", 1u << 5}};
            for (const auto& item : wanted) {
                if ((actions & item.bit) == 0)
                    continue;
                jobject constant = actionConstant(env, item.name);
                if (!constant)
                    continue;
                env->CallBooleanMethod(actionSet, server.refs.collectionAddMid, constant);
                env->DeleteLocalRef(constant);
                if (env->ExceptionCheck())
                    break;
            }
        }
    }
    if (!actionSet || env->ExceptionCheck()) {
        LogAndClearException(env, "player-info/actions");
        if (actionSet)
            env->DeleteLocalRef(actionSet);
        env->DeleteLocalRef(entry);
        return nullptr;
    }

    jclass collections = env->FindClass("java/util/Collections");
    jobject emptyList = nullptr;
    if (collections) {
        jmethodID empty = env->GetStaticMethodID(collections, "emptyList", "()Ljava/util/List;");
        if (empty)
            emptyList = env->CallStaticObjectMethod(collections, empty);
        env->DeleteLocalRef(collections);
    }
    if (env->ExceptionCheck())
        env->ExceptionClear();

    jobject packet = nullptr;
    if (emptyList)
        packet = env->NewObject(server.refs.playerInfoUpdatePacketCls, server.refs.playerInfoUpdatePacketCtor,
                                actionSet, emptyList);
    if (packet && !env->ExceptionCheck()) {
        // The constructor filters the incoming collection by action, so the list is set afterwards.
        jclass arrayList = env->FindClass("java/util/ArrayList");
        jmethodID ctor = arrayList ? env->GetMethodID(arrayList, "<init>", "()V") : nullptr;
        jmethodID add = arrayList ? env->GetMethodID(arrayList, "add", "(Ljava/lang/Object;)Z") : nullptr;
        jobject list = (arrayList && ctor && !env->ExceptionCheck()) ? env->NewObject(arrayList, ctor) : nullptr;
        if (list && add && !env->ExceptionCheck()) {
            env->CallBooleanMethod(list, add, entry);
            if (!env->ExceptionCheck())
                env->SetObjectField(packet, server.refs.playerInfoUpdatePacketEntriesFid, list);
        }
        if (arrayList)
            env->DeleteLocalRef(arrayList);
        if (list)
            env->DeleteLocalRef(list);
    }
    env->DeleteLocalRef(entry);
    if (emptyList)
        env->DeleteLocalRef(emptyList);
    env->DeleteLocalRef(actionSet);
    if (env->ExceptionCheck()) {
        LogAndClearException(env, "player-info/build");
        if (packet) {
            env->DeleteLocalRef(packet);
            packet = nullptr;
        }
    }
    return packet;
}

} // namespace proxy_server
