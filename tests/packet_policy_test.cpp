#include "packet_policy.h"
#include <cstdio>

// Routing ownership must stay version independent: 1.20.1 keeps keep-alive and custom payload in the
// game package, 1.20.2+ moved them to the common package and 1.21.9 moved ping into its own package.
int main() {
    struct Case {
        const char* name;
        bool fromAWhileBActive;
        bool fromBInPlay;
    };
    const Case cases[] = {
        {"net.minecraft.network.protocol.game.ServerboundMovePlayerPacket$PosRot", false, true},
        {"net.minecraft.network.protocol.game.ServerboundAcceptTeleportationPacket", false, true},
        {"net.minecraft.network.protocol.game.ServerboundKeepAlivePacket", true, false},
        {"net.minecraft.network.protocol.common.ServerboundKeepAlivePacket", true, false},
        {"net.minecraft.network.protocol.game.ServerboundCustomPayloadPacket", true, false},
        {"net.minecraft.network.protocol.common.ServerboundCustomPayloadPacket", true, false},
        {"net.minecraft.network.protocol.game.ServerboundPongPacket", false, true},
        {"net.minecraft.network.protocol.common.ServerboundPongPacket", true, false},
        {"net.minecraft.network.protocol.login.ServerboundHelloPacket", true, false},
        {"net.minecraft.network.protocol.login.ServerboundLoginAcknowledgedPacket", true, false},
        {"net.minecraft.network.protocol.configuration.ServerboundClientInformationPacket", true, false},
        {"net.minecraft.network.protocol.configuration.ServerboundFinishConfigurationPacket", true, false},
        {"net.minecraft.network.protocol.handshake.ClientIntentionPacket", true, false},
        {"net.minecraft.network.protocol.status.ServerboundStatusRequestPacket", true, false},
        {"net.minecraft.network.protocol.ping.ServerboundPingRequestPacket", true, false},
        {"example.UnknownPacket", true, false},
        {"", true, false},
    };
    for (const auto& item : cases) {
        if (!packet_policy::allowFromA(item.name, false) ||
            packet_policy::allowFromA(item.name, true) != item.fromAWhileBActive ||
            packet_policy::allowFromB(item.name, true) != item.fromBInPlay ||
            packet_policy::allowFromB(item.name, false)) {
            std::fprintf(stderr, "FAIL: routing ownership for %s\n", item.name);
            return 1;
        }
    }

    // Configuration traffic is mirrored to B while it syncs, play traffic always is.
    if (!packet_policy::isConfigurationPacket("net.minecraft.network.protocol.configuration."
                                              "ClientboundRegistryDataPacket") ||
        !packet_policy::isCommonPacket("net.minecraft.network.protocol.common.ClientboundUpdateTagsPacket") ||
        packet_policy::isPlayPhasePayload("net.minecraft.network.protocol.configuration."
                                          "ClientboundFinishConfigurationPacket")) {
        std::fprintf(stderr, "FAIL: phase classification\n");
        return 1;
    }
    std::puts("PASS: A/B routing ownership across 1.20.1 and 1.21.10 layouts");
    return 0;
}
