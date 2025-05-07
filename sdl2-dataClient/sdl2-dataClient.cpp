#include <SDL2/SDL.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Exception.h>

#include <iostream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>

using namespace Poco::Net;
using namespace Poco;
using namespace std;

static double steering = 0;
static double power1 = 0;

std::mutex wsMutex;
std::atomic<bool> running(true);

double mapRangeSteering(int x) {
    return -50 + ((double)(x + 32768) * 100 / 65535);
}

double mapRangePower(int x) {
    return 100 - (((x + 32768) / 65535.0) * 100);
}

string createCANFrameString(uint32_t can_id, const uint8_t* data, int dlc) {
    stringstream ss;
    ss << hex << uppercase << setw(3) << setfill('0') << can_id << "#";
    for (int i = 0; i < dlc; ++i) {
        ss << hex << setw(2) << setfill('0') << (int)data[i];
    }
    return ss.str();
}

WebSocket* connectWebSocket() {
    while (true) {
        try {
            HTTPClientSession session("127.0.0.1", 8000);
            HTTPRequest request(HTTPRequest::HTTP_GET, "/", HTTPMessage::HTTP_1_1);
            HTTPResponse response;
            WebSocket* ws = new WebSocket(session, request, response);
            cout << "WebSocket connected." << endl;
            return ws;
        }
        catch (Poco::Exception& ex) {
            cerr << "WebSocket connect failed: " << ex.displayText() << " - retrying in 3s..." << endl;
            this_thread::sleep_for(chrono::seconds(3));
        }
    }
}

void websocketPinger(WebSocket*& ws) {
    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        try {
            std::lock_guard<std::mutex> lock(wsMutex);
            ws->sendFrame("ping", 4, WebSocket::FRAME_TEXT);
        }
        catch (...) {
            cerr << "WebSocket connection lost (ping failed). Reconnecting..." << endl;
            try {
                ws->shutdown();
                delete ws;
            }
            catch (...) {}
            ws = connectWebSocket();
        }
    }
}

int main(int argc, char* argv[]) {
    WebSocket* ws = nullptr;
    SDL_Joystick* joystick = nullptr;

    while (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) < 0) {
        cerr << "SDL couldn't start: " << SDL_GetError() << ", retrying in 3s..." << endl;
        this_thread::sleep_for(chrono::seconds(3));
    }

    SDL_JoystickEventState(SDL_ENABLE);
    ws = connectWebSocket();
    thread pinger(websocketPinger, std::ref(ws));

    double prevSteering = -999, prevPower1 = -999;

    while (running.load()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            else if (event.type == SDL_JOYDEVICEADDED && !joystick) {
                joystick = SDL_JoystickOpen(event.jdevice.which);
                if (joystick) cout << "Joystick connected: " << SDL_JoystickName(joystick) << endl;
            }
            else if (event.type == SDL_JOYDEVICEREMOVED && joystick) {
                cout << "Joystick disconnected." << endl;
                SDL_JoystickClose(joystick);
                joystick = nullptr;
            }
        }

        if (!joystick && SDL_NumJoysticks() > 0) {
            joystick = SDL_JoystickOpen(0);
            if (joystick) cout << "Joystick reconnected: " << SDL_JoystickName(joystick) << endl;
        }

        if (joystick) {
            steering = mapRangeSteering(SDL_JoystickGetAxis(joystick, 0));
            power1 = mapRangePower(SDL_JoystickGetAxis(joystick, 2));

            if (steering != prevSteering || power1 != prevPower1) {
                prevSteering = steering;
                prevPower1 = power1;

                uint8_t propulsionData[8] = { 0 };
                propulsionData[0] = 0x01;
                propulsionData[1] = static_cast<uint8_t>(power1);
                string propulsionMsg = createCANFrameString(0x4B3, propulsionData, 8);
                cout << "[SEND] Propulsion: " << propulsionMsg << endl;

                uint8_t steeringData[8] = { 0 };
                steeringData[0] = static_cast<uint8_t>((steering + 50) * 255.0 / 100.0);
                string steeringMsg = createCANFrameString(0x4B7, steeringData, 8);
                cout << "[SEND] Steering:   " << steeringMsg << endl;

                try {
                    std::lock_guard<std::mutex> lock(wsMutex);
                    ws->sendFrame(propulsionMsg.data(), propulsionMsg.size(), WebSocket::FRAME_TEXT);
                    ws->sendFrame(steeringMsg.data(), steeringMsg.size(), WebSocket::FRAME_TEXT);
                }
                catch (Poco::Exception& ex) {
                    cerr << "WebSocket send failed: " << ex.displayText() << endl;
                    try {
                        ws->shutdown(); delete ws;
                    }
                    catch (...) {}
                    ws = connectWebSocket();
                }
            }
        }
        SDL_Delay(40);
    }

    if (joystick) SDL_JoystickClose(joystick);
    SDL_Quit();

    if (ws) {
        try { ws->shutdown(); delete ws; }
        catch (...) {}
    }

    pinger.join();
    return 0;
}
