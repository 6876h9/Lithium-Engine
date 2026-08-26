#pragma once
#include <enet/enet.h>
#include <string>
#include <vector>
#include <cstdint>
#include "core/math.hpp"

enum class NetworkMode {
    Offline,
    Server,
    Client
};

#pragma pack(push, 1)
struct NetworkPacketTransform {
    uint8_t packet_type = 0; // 0 for Transform
    uint32_t actor_id;
    Vector3 position;
    Vector3 rotation;
    Vector3 scale;
};
#pragma pack(pop)

class NetworkManager {
public:
    static NetworkManager& get() {
        static NetworkManager instance;
        return instance;
    }

    bool initialize();
    void shutdown();
    void update();

    bool host_server(uint16_t port);
    bool connect_to_server(const std::string& host, uint16_t port);
    void disconnect();

    NetworkMode get_mode() const { return mode; }
    
    void broadcast_transform(uint32_t actor_id, const Vector3& pos, const Vector3& rot, const Vector3& scale);
    
    const std::vector<NetworkPacketTransform>& get_received_transforms() const { return received_transforms; }
    void clear_received_transforms() { received_transforms.clear(); }

private:
    NetworkManager() = default;
    ~NetworkManager() = default;

    ENetHost* client_or_server = nullptr;
    ENetPeer* server_peer = nullptr;
    std::vector<ENetPeer*> connected_clients;
    NetworkMode mode = NetworkMode::Offline;

    std::vector<NetworkPacketTransform> received_transforms;
};
