#pragma once

#include <string_view>

// The remote connection belongs to A. Keep its heartbeat and mod handshake on A, while B supplies
// gameplay input after initialization has completed.
//
// 1.20.2+ split the connection into LOGIN -> CONFIGURATION -> PLAY, and moved several packets to
// new packages: custom payload and keep-alive live in ``common``, ping traffic has its own package
// since 1.21.9. Packets are classified by package prefix so a single binary keeps working on the
// old (play-only) and the new layout.
namespace packet_policy {
inline constexpr std::string_view gamePrefix = "net.minecraft.network.protocol.game.";
inline constexpr std::string_view configurationPrefix = "net.minecraft.network.protocol.configuration.";
inline constexpr std::string_view commonPrefix = "net.minecraft.network.protocol.common.";
inline constexpr std::string_view loginPrefix = "net.minecraft.network.protocol.login.";
inline constexpr std::string_view statusPrefix = "net.minecraft.network.protocol.status.";
inline constexpr std::string_view pingPrefix = "net.minecraft.network.protocol.ping.";
inline constexpr std::string_view handshakePrefix = "net.minecraft.network.protocol.handshake.";

inline bool hasPrefix(std::string_view name, std::string_view prefix) {
    return name.size() >= prefix.size() && name.substr(0, prefix.size()) == prefix;
}

inline bool isGamePacket(std::string_view name) {
    return hasPrefix(name, gamePrefix);
}

inline bool isConfigurationPacket(std::string_view name) {
    return hasPrefix(name, configurationPrefix);
}

inline bool isCommonPacket(std::string_view name) {
    return hasPrefix(name, commonPrefix);
}

inline bool isLoginPacket(std::string_view name) {
    return hasPrefix(name, loginPrefix);
}

inline bool isStatusPacket(std::string_view name) {
    return hasPrefix(name, statusPrefix) || hasPrefix(name, pingPrefix);
}

// Packets A owns: its heartbeat and its mod handshake must stay on A's real connection.
inline bool isOwnedByA(std::string_view name) {
    return name == "net.minecraft.network.protocol.game.ServerboundKeepAlivePacket" ||
           name == "net.minecraft.network.protocol.common.ServerboundKeepAlivePacket" ||
           name == "net.minecraft.network.protocol.game.ServerboundCustomPayloadPacket" ||
           name == "net.minecraft.network.protocol.common.ServerboundCustomPayloadPacket";
}

// A -> B downlink. While B is synchronizing only PLAY traffic was forwarded historically; the
// configuration phase additionally mirrors configuration and common packets so B can build the
// same registry view A has.
inline bool allowFromA(std::string_view name, bool bActive) {
    if (!bActive)
        return true;
    return !isGamePacket(name) || isOwnedByA(name);
}

// B -> A uplink. B never drives the remote server during login or configuration: those packets
// belong to A's handshake and are consumed by the proxy.
inline bool allowFromB(std::string_view name, bool bInPlay) {
    if (!bInPlay)
        return false;
    return isGamePacket(name) && !isOwnedByA(name);
}

inline bool isPlayPhasePayload(std::string_view name) {
    return isGamePacket(name) || isCommonPacket(name);
}
} // namespace packet_policy
