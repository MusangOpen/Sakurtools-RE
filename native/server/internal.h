#pragma once

// Private server interfaces. Other modules include only b_server.h.
#include "b_server.h"
#include "jni_lookup.h"
#include "state.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace proxy_server {

void setProtocolState(JNIEnv* env, jobject channel, jobject protoValue);
bool defineInitClass(JNIEnv* env, jobject mcLoader);
bool defineHandlerClass(JNIEnv* env, jobject mcLoader);
bool defineMainGateClass(JNIEnv* env, jobject mcLoader);
bool startLanAnnouncement(JNIEnv* env, jobject mcLoader);
bool bindServer(JNIEnv* env, jobject mcLoader);
std::u16string currentServerMotd(JNIEnv* env);
bool refreshMidSessionRefs(JNIEnv* env, jobject mcLoader);
bool cacheJavaRefs(JNIEnv* env, jobject mcLoader);
void clearPendingPackets(JNIEnv* env);
void closeARemoteConnection(JNIEnv* env);
bool uuidToBytes(JNIEnv* env, jobject uuid, unsigned char out[16]);
void writeToB(JNIEnv* env, jobject packet);
void completeLogin(JNIEnv* env, jobject hello);
void releaseMainGate(JNIEnv* env);
// B reached PLAY: pushes B's own TAB entry and starts the mid-session snapshot when needed.
bool EnterPlayPhase(JNIEnv* env, jobject channel);
bool prepareTeamPacket(JNIEnv* env, jobject packet, jobject& output);
void handleClientPacket(JNIEnv* env, jobject ctx, jobject msg);

// configuration.cpp: 1.20.2+ configuration-phase handoff, used by both login paths.
bool applyProtocolState(JNIEnv* env, jobject channel, jobject protocolValue);
bool beginConfigurationPhase(JNIEnv* env, jobject channel);
bool synthesizeConfigurationPhase(JNIEnv* env, jobject channel);
bool isFinishConfigurationPacket(JNIEnv* env, jobject packet);
bool isClientboundFinishConfigurationPacket(JNIEnv* env, jobject packet);

// player_info.cpp: builds a one-entry PlayerInfoUpdate packet without hand-encoding the body.
// Action bits follow the enum ordinals: ADD_PLAYER, UPDATE_GAME_MODE, UPDATE_LISTED, UPDATE_LATENCY,
// UPDATE_DISPLAY_NAME.
inline constexpr unsigned kPiuAddPlayer = 1u << 0;
inline constexpr unsigned kPiuGameMode = 1u << 2;
inline constexpr unsigned kPiuListed = 1u << 3;
inline constexpr unsigned kPiuLatency = 1u << 4;
inline constexpr unsigned kPiuDisplayName = 1u << 5;
jobject buildPlayerInfoUpdate(JNIEnv* env, unsigned actions, jobject uuid, jobject profile, jboolean listed,
                              jint latency, jobject gameMode, jobject displayName);

} // namespace proxy_server
