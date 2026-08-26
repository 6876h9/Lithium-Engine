#include "network/network_manager.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>

bool NetworkManager::initialize() {
    if (enet_initialize() != 0) {
        std::cerr << "An error occurred while initializing ENet.\n";
        return false;
    }
    return true;
}

void NetworkManager::shutdown() {
    disconnect();
    enet_deinitialize();
}

bool NetworkManager::host_server(uint16_t port) {
    disconnect();
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;
    
    // Up to 32 clients, 2 channels, 0 incoming/outgoing bandwidth
    client_or_server = enet_host_create(&address, 32, 2, 0, 0);
    if (client_or_server == nullptr) {
        std::cerr << "An error occurred while trying to create an ENet server host.\n";
        return false;
    }
    mode = NetworkMode::Server;
    std::cout << "Server started on port " << port << std::endl;
    return true;
}

bool NetworkManager::connect_to_server(const std::string& host, uint16_t port) {
    disconnect();
    
    client_or_server = enet_host_create(nullptr, 1, 2, 0, 0);
    if (client_or_server == nullptr) {
        std::cerr << "An error occurred while trying to create an ENet client host.\n";
        return false;
    }
    
    ENetAddress address;
    enet_address_set_host(&address, host.c_str());
    address.port = port;
    
    server_peer = enet_host_connect(client_or_server, &address, 2, 0);
    if (server_peer == nullptr) {
        std::cerr << "No available peers for initiating an ENet connection.\n";
        return false;
    }
    
    ENetEvent event;
    // Wait up to 5 seconds for connection success
    if (enet_host_service(client_or_server, &event, 5000) > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
        std::cout << "Connection to server succeeded.\n";
        mode = NetworkMode::Client;
        return true;
    } else {
        enet_peer_reset(server_peer);
        server_peer = nullptr;
        std::cerr << "Connection to server failed.\n";
        return false;
    }
}

void NetworkManager::disconnect() {
    if (mode == NetworkMode::Client && server_peer) {
        enet_peer_disconnect(server_peer, 0);
        // Wait for disconnect
        ENetEvent event;
        bool disconnected = false;
        while (enet_host_service(client_or_server, &event, 1000) > 0) {
            switch (event.type) {
                case ENET_EVENT_TYPE_RECEIVE:
                    enet_packet_destroy(event.packet);
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    std::cout << "Disconnected from server.\n";
                    disconnected = true;
                    break;
                default:
                    break;
            }
        }
        if (!disconnected) {
            enet_peer_reset(server_peer);
        }
        server_peer = nullptr;
    }
    
    if (client_or_server) {
        enet_host_destroy(client_or_server);
        client_or_server = nullptr;
    }
    
    connected_clients.clear();
    mode = NetworkMode::Offline;
}

void NetworkManager::update() {
    if (!client_or_server) return;

    ENetEvent event;
    // Non-blocking poll
    while (enet_host_service(client_or_server, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                if (mode == NetworkMode::Server) {
                    std::cout << "A new client connected.\n";
                    connected_clients.push_back(event.peer);
                }
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                if (event.packet->dataLength == sizeof(NetworkPacketTransform)) {
                    NetworkPacketTransform* transform = reinterpret_cast<NetworkPacketTransform*>(event.packet->data);
                    if (transform->packet_type == 0) {
                        received_transforms.push_back(*transform);
                    }
                }
                enet_packet_destroy(event.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                if (mode == NetworkMode::Server) {
                    std::cout << "A client disconnected.\n";
                    auto it = std::find(connected_clients.begin(), connected_clients.end(), event.peer);
                    if (it != connected_clients.end()) {
                        connected_clients.erase(it);
                    }
                } else if (mode == NetworkMode::Client) {
                    std::cout << "Server disconnected.\n";
                    disconnect();
                }
                event.peer->data = NULL;
                break;
            }
            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }
}

void NetworkManager::broadcast_transform(uint32_t actor_id, const Vector3& pos, const Vector3& rot, const Vector3& scale) {
    if (mode == NetworkMode::Offline || !client_or_server) return;

    NetworkPacketTransform packet_data;
    packet_data.packet_type = 0;
    packet_data.actor_id = actor_id;
    packet_data.position = pos;
    packet_data.rotation = rot;
    packet_data.scale = scale;

    ENetPacket* packet = enet_packet_create(&packet_data, sizeof(NetworkPacketTransform), ENET_PACKET_FLAG_UNSEQUENCED);

    if (mode == NetworkMode::Server) {
        enet_host_broadcast(client_or_server, 0, packet);
    } else if (mode == NetworkMode::Client && server_peer) {
        enet_peer_send(server_peer, 0, packet);
    }
}
