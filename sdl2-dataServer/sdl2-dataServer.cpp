#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Exception.h>

#include <iostream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <mutex>
#include <atomic>
#include <windows.h>

using namespace Poco::Net;
using namespace Poco;
using namespace std;

// === CAN frame struct ===
struct can_frame {
    uint32_t can_id;
    uint8_t can_dlc;
    uint8_t data[8];
};

// === Global değişkenler ===
HANDLE serialHandle = INVALID_HANDLE_VALUE;
std::mutex frameMutex;
can_frame steeringFrame = { 0x4B7, 8, {0} };
can_frame propulsionFrame = { 0x4B3, 8, {0} };
std::atomic<bool> hasSteering = false;
std::atomic<bool> hasPropulsion = false;

// === Seri port aç ===
bool openSerialPort(const std::string& portName, DWORD baudRate = CBR_115200) {
    serialHandle = CreateFileA(portName.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (serialHandle == INVALID_HANDLE_VALUE) {
        cerr << "Serial port could not be opened: " << portName << endl;
        return false;
    }

    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(serialHandle, &dcb)) return false;

    dcb.BaudRate = baudRate;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;

    if (!SetCommState(serialHandle, &dcb)) return false;

    cout << "Serial port opened: " << portName << " @ " << baudRate << " baud" << endl;
    return true;
}

// === CAN frame'i yazdır ve seriport'a gönder ===
void sendFrameOverSerial(const can_frame& frame) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << frame.can_id << "#";
    for (int i = 0; i < frame.can_dlc; ++i) {
        oss << std::setw(2) << std::setfill('0') << (int)frame.data[i];
    }
    oss << "\n";

    std::string str = oss.str();
    DWORD written;
    WriteFile(serialHandle, str.c_str(), str.size(), &written, NULL);
    cout << "[SENT] " << str;
}

// === CAN string'ten struct'a çevir ===
bool parseCANFrameString(const std::string& msg, can_frame& frame) {
    auto sep = msg.find('#');
    if (sep == std::string::npos) return false;

    try {
        frame.can_id = std::stoul(msg.substr(0, sep), nullptr, 16);
        std::string data = msg.substr(sep + 1);
        frame.can_dlc = static_cast<uint8_t>(min((int)data.length() / 2, 8));

        for (int i = 0; i < frame.can_dlc; ++i) {
            frame.data[i] = static_cast<uint8_t>(std::stoul(data.substr(i * 2, 2), nullptr, 16));
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

// === 100 ms'de bir steering + propulsion gönderici ===
void periodicSender() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::lock_guard<std::mutex> lock(frameMutex);
        if (hasSteering.load()) {
            sendFrameOverSerial(steeringFrame);
        }
        if (hasPropulsion.load()) {
            sendFrameOverSerial(propulsionFrame);
        }
    }
}

// === WebSocket handler ===
class WebSocketRequestHandler : public HTTPRequestHandler {
public:
    void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override {
        try {
            WebSocket ws(request, response);
            cout << "WebSocket client connected." << endl;

            char buffer[1024];
            int flags, n;

            while (true) {
                n = ws.receiveFrame(buffer, sizeof(buffer), flags);
                if (n <= 0) break;

                std::string message(buffer, n);
                if (message.empty() || message == "ping") continue;

                can_frame frame;
                if (parseCANFrameString(message, frame)) {
                    std::lock_guard<std::mutex> lock(frameMutex);
                    if (frame.can_id == 0x4B7) {
                        steeringFrame = frame;
                        hasSteering = true;
                    }
                    else if (frame.can_id == 0x4B3) {
                        propulsionFrame = frame;
                        hasPropulsion = true;
                    }
                }
            }
            cout << "WebSocket client disconnected." << endl;
        }
        catch (const Exception& ex) {
            cerr << "WebSocket error: " << ex.displayText() << endl;
        }
    }
};

// === Factory ===
class RequestHandlerFactory : public HTTPRequestHandlerFactory {
public:
    HTTPRequestHandler* createRequestHandler(const HTTPServerRequest&) override {
        return new WebSocketRequestHandler;
    }
};

// === main ===
int main() {
    try {
        if (!openSerialPort("COM5", CBR_115200)) {
            cerr << "Could not open COM5" << endl;
            return 1;
        }

        std::thread t(periodicSender);
        t.detach();

        UInt16 port = 8000;
        ServerSocket socket(port);
        HTTPServer server(new RequestHandlerFactory, socket, new HTTPServerParams);

        server.start();
        cout << "Server started on port " << port << endl;

        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        server.stop();
        CloseHandle(serialHandle);
    }
    catch (const Exception& ex) {
        cerr << "Server error: " << ex.displayText() << endl;
    }

    return 0;
}
