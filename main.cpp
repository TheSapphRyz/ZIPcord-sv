#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <portaudio.h>
#include <thread>
#include <vector>
#include <json.hpp>
#include <mutex>

using json = nlohmann::json;

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "portaudio.lib")

#define SAMPLE_RATE 44100
#define FRAMES_PER_BUFFER 128
#define NUM_CHANNELS 1
#define MAX_BUFFER_SIZE 1048576 // 1MB

std::vector<SOCKET> clients;
std::mutex clientsMutex;
std::vector<std::string> voice_clients;
std::unordered_map<SOCKET, std::string> client_usernames;

void broadcastMessage(const std::string& msg, SOCKET senderSocket, bool voiceOnly = false) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    std::cout << "Broadcasting message, size: " << msg.size() << ", voiceOnly: " << (voiceOnly ? "true" : "false") << "\n";
    for (size_t i = 0; i < clients.size(); ++i) {
        SOCKET client = clients[i];
        if (client != senderSocket && client != INVALID_SOCKET) {
            bool inVoiceChat = client_usernames.count(client) && std::find(voice_clients.begin(), voice_clients.end(), client_usernames[client]) != voice_clients.end();
            if (!voiceOnly || inVoiceChat) {
                int total_sent = 0;
                int bytes_to_send = static_cast<int>(msg.size());
                const char* data = msg.c_str();
                std::cout << "Sending to client " << client << "\n";
                while (total_sent < bytes_to_send) {
                    int send_result = send(client, data + total_sent, bytes_to_send - total_sent, 0);
                    if (send_result == SOCKET_ERROR) {
                        std::cerr << "Failed to send to client " << client << ": " << WSAGetLastError() << "\n";
                        break;
                    }
                    total_sent += send_result;
                }
                if (total_sent == bytes_to_send) {
                    std::cout << "Successfully sent " << (voiceOnly ? "audio" : "message") << " to client " << client << ", size: " << msg.size() << "\n";
                }
                else {
                    std::cerr << "Incomplete send to client " << client << ", sent: " << total_sent << ", expected: " << bytes_to_send << "\n";
                }
            }
            else if (voiceOnly) {
                std::cout << "Skipped client " << client << " (not in voice chat)\n";
            }
        }
    }
}

// Обработка текстовых сообщений (JSON)
std::string handle_text_message(const std::string& message, SOCKET client_sock) {
    json request;
    try {
        request = json::parse(message);
        std::cout << "Parsed JSON: " << message << "\n";
    }
    catch (const std::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << " - Raw data: ";
        for (size_t i = 0; i < message.size() && i < 50; ++i) {
            std::cerr << (int)(unsigned char)message[i] << " ";
        }
        std::cerr << std::endl;
        return json{ {"status", "error"}, {"message", "Invalid JSON"} }.dump();
    }

    json response;
    std::string command = request["type"];
    if (command == "voice_connect") {
        std::lock_guard<std::mutex> lock(clientsMutex);
        clients.push_back(client_sock);
        voice_clients.push_back(request["message"]);
        client_usernames[client_sock] = request["message"];
        response = { {"type", "voice_connect"}, {"message", 1} };
    }
    else if (command == "get_voice_clients") {
        std::lock_guard<std::mutex> lock(clientsMutex);
        response["type"] = command;
        std::string cl = "";
        for (const auto& i : voice_clients) cl += i + "-=S=-";
        response["message"] = cl;
    }
    else if (command == "disconnect") {
        std::lock_guard<std::mutex> lock(clientsMutex);
        auto it = std::remove(voice_clients.begin(), voice_clients.end(), request["message"]);
        voice_clients.erase(it, voice_clients.end());
        response["command"] = command;
        response["message"] = "1";
    }
    else if (command == "chat_msg") {
        std::string username = request.contains("username") ? request["username"].get<std::string>() : "unknown";
        std::string msg_text = request.contains("message") ? request["message"].get<std::string>() : "";
        std::string time = request.contains("time") ? request["time"].get<std::string>() : "unknown";
        json r = { {"type", "chat_msg"}, {"username", username}, {"message", msg_text}, {"time", time} };
        std::cout << "Broadcasting chat message: " << r.dump() << "\n";
        for (SOCKET i : clients) {
            if (i != client_sock) {
                send(i, r.dump().c_str(), r.dump().size(), 0);
            }
        }
        response = { {"type", "chat_req"}, {"message", "ok"} };
        std::cout << "Chat message from " << username << ": " << msg_text << "\n";
    }
    return response.dump();
}

// Обработка клиента
void handle_client(SOCKET client_socket) {
    std::vector<char> buffer(MAX_BUFFER_SIZE);
    while (true) {
        int bytes_received = recv(client_socket, buffer.data(), buffer.size(), 0);
        if (bytes_received <= 0) {
            std::cerr << "Client disconnected or error: " << WSAGetLastError() << std::endl;
            break;
        }

        // Проверяем, является ли сообщение JSON (текстовым)
        bool is_json = false;
        if (bytes_received > 1 && buffer[0] == '{' && buffer[bytes_received - 1] == '}') {
            is_json = true;
        }

        if (is_json) {
            // Обработка текстового сообщения (JSON)
            std::string message(buffer.data(), bytes_received);
            std::string response = handle_text_message(message, client_socket);
            if (!response.empty()) {
                send(client_socket, response.c_str(), response.size(), 0);
            }
        }
        else {
            // Обработка аудиоданных (бинарные данные)
            std::lock_guard<std::mutex> lock(clientsMutex);
            for (SOCKET other_client : clients) {
                if (other_client != client_socket) {
                    send(other_client, buffer.data(), bytes_received, 0);
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(clientsMutex);
    auto it = std::find(clients.begin(), clients.end(), client_socket);
    if (it != clients.end()) {
        clients.erase(it);
    }
    closesocket(client_socket);
}

int main() {
    // Инициализация Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return 1;
    }

    // Инициализация PortAudio
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
        WSACleanup();
        return 1;
    }

    // Создание сокета
    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        std::cerr << "Socket creation failed." << std::endl;
        Pa_Terminate();
        WSACleanup();
        return 1;
    }

    // Настройка адреса сервера
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(412);

    // Привязка сокета
    if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed." << std::endl;
        closesocket(server_socket);
        Pa_Terminate();
        WSACleanup();
        return 1;
    }

    // Прослушивание сокета
    if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed." << std::endl;
        closesocket(server_socket);
        Pa_Terminate();
        WSACleanup();
        return 1;
    }

    std::cout << "Server started, waiting for clients..." << std::endl;

    // Основной цикл сервера
    while (true) {
        SOCKET client_sock = accept(server_socket, nullptr, nullptr);
        if (client_sock == INVALID_SOCKET) {
            std::cerr << "Accept failed." << std::endl;
            continue;
        }

        std::lock_guard<std::mutex> lock(clientsMutex);
        clients.push_back(client_sock);
        std::thread(handle_client, client_sock).detach();
    }

    // Завершение работы
    closesocket(server_socket);
    Pa_Terminate();
    WSACleanup();
    return 0;
}
