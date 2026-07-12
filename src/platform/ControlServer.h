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
//   -> {"id":3,"method":"time"}
//   <- {"id":3,"result":{"seconds":T,"paused":B}}   (scene clock; editors
//                                            sync their preview clocks to it)
//   -> {"id":4,"method":"pause","params":{"paused":B}}
//   -> {"id":5,"method":"seek","params":{"time":T}}   (backward re-creates
//                                            the scene instances, §9.7)
//   -> {"id":6,"method":"reload"}          (re-read scene from disk, keeping
//                                            clock and parameter values)
//   -> {"id":7,"method":"source"}
//   <- {"id":7,"result":{"scene":"<scene.json text>"}}
//   -> {"id":8,"method":"load","params":{"scene":"<scene.json text>"}}
//                                          (swap to the pushed document —
//                                           editors own the document; the
//                                           old scene keeps running if it
//                                           fails validation)
//   -> {"id":9,"method":"fire","params":{"node":"seq","port":"cue"}}
//                                          (manually fire an event output —
//                                           the editor's "fire now" verb)
//   -> {"id":10,"method":"write-asset","params":{"path":"graphs/x.json",
//                                                "contents":"..."}}
//                                          (write a project file — §19
//                                           subgraph edits and the shader
//                                           pane; project-root confined,
//                                           text only; follow with load/
//                                           reload to apply)
//   -> {"id":11,"method":"read-asset","params":{"path":"graphs/x.json"}}
//   <- {"id":11,"result":{"contents":"..."}}
//   <- {"event":"transport","paused":B,"seconds":T}   (after pause/seek)
//   <- {"event":"reload"}                  (after reload/load; clients
//                                           should re-describe)
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
#include "platform/ParamJson.h"

namespace drift::platform {

class ControlServer {
public:
    struct SceneInfo {
        std::string name;
        bool animated = false;
        std::vector<core::SceneParam> parameters;
        std::vector<SequenceDesc> sequences;
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
        std::function<double()> time;         // current scene clock, seconds
        std::function<bool()> paused;
        std::function<void(bool paused)> setPaused;
        std::function<bool(double seconds, std::string& error)> seek;
        std::function<bool(std::string& error)> reload;
        std::function<std::string()> source; // current scene.json text
        std::function<bool(const std::string& sceneJson, std::string& error)>
            load;
        std::function<bool(const std::string& node, const std::string& port,
                           std::string& error)> fire;
        // Project files (write-asset/read-asset): paths are project-root
        // relative and confined there; the callback owner enforces it.
        std::function<bool(const std::string& path,
                           const std::string& contents, std::string& error)>
            writeAsset;
        std::function<bool(const std::string& path, std::string& out,
                           std::string& error)> readAsset;
    };

    ~ControlServer();

    bool start(uint16_t port, Callbacks callbacks, std::string& error);
    // The poll set (epoll on Linux, kqueue elsewhere): one fd covering the
    // listener and every connection; poll it for readability, then drive().
    int fd() const { return mPoll; }
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
    void broadcast(const std::string& message, int exceptFd);
    std::string transportJson() const;
    void readClient(int fd);
    bool handshake(int fd, Client& client);
    bool handleFrames(int fd, Client& client);
    void handleRequest(int fd, Client& client, const std::string& text);
    void send(int fd, Client& client, std::string bytes);
    void flush(int fd, Client& client);
    void closeClient(int fd);
    void updateWatch(int fd, const Client& client);

    Callbacks mCallbacks;
    int mListen = -1;
    int mPoll = -1;
    std::map<int, Client> mClients;
};

} // namespace drift::platform
