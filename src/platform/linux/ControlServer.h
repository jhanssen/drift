#pragma once

// WebSocket control endpoint — protocol v0 of the editor/runtime channel
// (DESIGN.md §5.3). Listens on 127.0.0.1:<port>; messages are JSON texts:
//
//   -> {"id":1,"method":"describe"}
//   <- {"id":1,"result":{"protocol":1,"scene":...,"animated":...,
//                        "parameters":[{name,type,value,min?,max?,step?,
//                                       label?,hint?},...]}}
//   -> {"id":2,"method":"set","params":{"name":"p","value":V}}
//   <- {"id":2,"result":{}}  or  {"id":2,"error":"..."}
//   <- {"event":"parameter","name":"p","value":V}   (to the other clients)
//
// V is a number (scalar) or an array of 2-4 numbers (vecN), like scene.json
// literals (§4). Browser pages may connect only from localhost origins: a
// remote page attempting ws://127.0.0.1 is refused at the upgrade (403).
//
// Single-threaded: the app loop polls fd() for POLLIN and calls drive().

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/Scene.h"

namespace drift::platform {

class ControlServer {
public:
    struct SceneInfo {
        std::string name;
        bool animated = false;
        std::vector<core::SceneParam> parameters;
        bool loaded = false;
    };
    struct Callbacks {
        std::function<SceneInfo()> describe;
        // Applies to every scene instance (§17.6: parameters are shared).
        // applied returns the value after clamping to the declared range —
        // that is what the parameter event broadcasts.
        std::function<bool(const std::string& name, const core::Value& value,
                           std::string& error, core::Value& applied)>
            setParameter;
    };

    ~ControlServer();

    bool start(uint16_t port, Callbacks callbacks, std::string& error);
    int fd() const { return mEpoll; }
    void drive();

private:
    struct Client {
        std::string in;      // unparsed bytes (HTTP headers, then frames)
        std::string out;     // pending unwritten bytes
        std::string message; // accumulated fragmented text message
        bool upgraded = false;
        bool closing = false; // close frame sent; drop after flush
    };

    void acceptClients();
    void readClient(int fd);
    bool handshake(int fd, Client& client);
    bool handleFrames(int fd, Client& client);
    void handleRequest(int fd, Client& client, const std::string& text);
    void send(int fd, Client& client, std::string bytes);
    void flush(int fd, Client& client);
    void closeClient(int fd);
    void updateEpoll(int fd, const Client& client);

    Callbacks mCallbacks;
    int mListen = -1;
    int mEpoll = -1;
    std::map<int, Client> mClients;
};

} // namespace drift::platform
