#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <portaudio.h>
#include <thread>
#include <vector>
#include <json.hpp>
using json = nlohmann::json;
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "portaudio.lib")

#define SAMPLE_RATE 44100
#define FRAMES_PER_BUFFER 128 //буфер шобы задэржка была меньши
#define NUM_CHANNELS 1

std::vector<SOCKET> clients;
// абработка json сообщений
std::vector<std::string> voice_clients;

std::string handle_text_message(const std::string& message, const SOCKET& client_sock) {
    json request = json::parse(message); // Парсим входящий JSON
    std::string command = request["command"]; // Получаем команду

    json response;
    if (command == "voice_connect") {
        clients.push_back(client_sock);
        voice_clients.push_back(request["message"]);
        response = { {"command", "voice_connect"}, {"message", 1} };
    }
    if (command == "get_voice_clients") {
        response["command"] = command;
        std::string cl = "";
        for (const auto& i : voice_clients) {
            cl += i + "-=S=-"; // Корректное добавление строки
        }
        response["message"] = cl;
    }
    if (command == "disconnect") {
        //std::erase(voice_clients, request["message"]);
        response["command"] = command;
        response["message"] = "1";
    }

    return response.dump(); // Возвращаем JSON-ответ
}

void handle_client(SOCKET client_socket) {
    //абработка ашибак    
    PaStream* stream;
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
        return;
    }

    err = Pa_OpenDefaultStream(&stream, 0, NUM_CHANNELS, paInt16, SAMPLE_RATE, FRAMES_PER_BUFFER, nullptr, nullptr);
    if (err != paNoError) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
        Pa_Terminate();
        return;
    }

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
        Pa_CloseStream(stream);
        Pa_Terminate();
        return;
    }
    // отправка людим в вийсе
    while (true) {
        char buffer[FRAMES_PER_BUFFER * NUM_CHANNELS * 2];
        int bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);
        if (bytes_received <= 0) break;
        // если саабщении json та мы испальзуем одно а если не то аудио данние отсасываем
        if (bytes_received < FRAMES_PER_BUFFER * NUM_CHANNELS * 2) {
            std::string message(buffer, bytes_received);
            std::string response = handle_text_message(message, client_socket);
            send(client_socket, response.c_str(), response.size(), 0);
        }
        else {
            for (SOCKET other_client : clients) {
                if (other_client != client_socket) {
                    send(other_client, buffer, bytes_received, 0);
                }
            }
        }
    }
    printf("[DATA] : sending audio"); 

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    closesocket(client_socket);
}

int main() {
    // сокети иницианализиурем
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return 1;
    }

    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        std::cerr << "Socket creation failed." << std::endl;
        WSACleanup();
        return 1;
    }
    // параметри сервира
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(412);

    if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed." << std::endl;
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed." << std::endl;
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    std::cout << "Server started, waiting for clients..." << std::endl;
    while (true) {
        SOCKET client_sock = accept(server_socket, nullptr, nullptr);
        printf("[CLIENT] : connected [IP] : " + client_sock);
        if (client_sock == INVALID_SOCKET) {
            std::cerr << "Accept failed." << std::endl;
            continue;
        }
        //clients.push_back(client_sock);
        std::thread(handle_client, client_sock).detach();
    }

    closesocket(server_socket);
    WSACleanup();
    return 0;
    // закриваем серверь
}

// нада сделать базу данных на sqlite3, делаится оч просто 