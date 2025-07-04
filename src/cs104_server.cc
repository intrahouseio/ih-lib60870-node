#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <tuple>
#include <napi.h>
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h> // Для PRIu64
#include "cs104_server.h"

using namespace Napi;
using namespace std;

Napi::FunctionReference IEC104Server::constructor;

Napi::Object IEC104Server::Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "IEC104Server", {
        InstanceMethod("start", &IEC104Server::Start),
        InstanceMethod("stop", &IEC104Server::Stop),
        InstanceMethod("sendCommands", &IEC104Server::SendCommands),
        InstanceMethod("getStatus", &IEC104Server::GetStatus)
    });

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();
    exports.Set("IEC104Server", func);
    return exports;
}

IEC104Server::IEC104Server(const Napi::CallbackInfo& info) : Napi::ObjectWrap<IEC104Server>(info) {
    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(info.Env(), "Expected a callback function").ThrowAsJavaScriptException();
        return;
    }

    Napi::Function emit = info[0].As<Napi::Function>();
    running = false;
    started = false;
    server = nullptr; // Explicitly initialize
    cnt = 0;
    restrictIPs = false;
    serverMode = CS104_MODE_CONNECTION_IS_REDUNDANCY_GROUP;

    try {
        tsfn = Napi::ThreadSafeFunction::New(
            info.Env(),
            emit,
            "IEC104ServerTSFN",
            0,
            1,
            [this](Napi::Env) {
                printf("ThreadSafeFunction finalized, serverID: %s\n", serverID.c_str());
                fflush(stdout);
            }
        );
    } catch (const std::exception& e) {
        printf("Failed to create ThreadSafeFunction: %s\n", e.what());
        fflush(stdout);
        Napi::Error::New(info.Env(), string("TSFN creation failed: ") + e.what()).ThrowAsJavaScriptException();
    }
}

IEC104Server::~IEC104Server() {
    // Ensure server is stopped and thread is joined
    {
        std::lock_guard<std::mutex> lock(connMutex);
        if (running) {
            running = false;
            if (started && server) {
                printf("Destructor stopping server, serverID: %s\n", serverID.c_str());
                fflush(stdout);
                CS104_Slave_stop(server);
                CS104_Slave_destroy(server);
                server = nullptr;
                started = false;
            }
        }
    }

    if (_thread.joinable()) {
        _thread.join();
    }

    // Clean up redundancy groups
    for (auto& [name, group] : redundancyGroups) {
        if (group) {
            CS104_RedundancyGroup_destroy(group);
            group = nullptr;
        }
    }
    redundancyGroups.clear();

    // Release thread-safe function
    tsfn.Release();
}

Napi::Value IEC104Server::Start(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsObject()) {
        Napi::TypeError::New(env, "Expected an object with { port (number), serverID (string), mode (string), [params (object), clients (array)] }").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Object config = info[0].As<Napi::Object>();

    if (!config.Has("port") || !config.Get("port").IsNumber() ||
        !config.Has("serverID") || !config.Get("serverID").IsString() ||
        !config.Has("mode") || !config.Get("mode").IsString()) {
        Napi::TypeError::New(env, "Object must contain 'port' (number), 'serverID' (string), and 'mode' (string)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    int port = config.Get("port").As<Napi::Number>().Int32Value();
    serverID = config.Get("serverID").As<Napi::String>().Utf8Value();
    std::string mode = config.Get("mode").As<Napi::String>().Utf8Value();

    if (port <= 0 || serverID.empty()) {
        Napi::Error::New(env, "Invalid 'port' or 'serverID'").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (mode != "redundant" && mode != "multi") {
        Napi::Error::New(env, "Mode must be 'redundant' or 'multi'").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    serverMode = (mode == "redundant") ? CS104_MODE_MULTIPLE_REDUNDANCY_GROUPS : CS104_MODE_CONNECTION_IS_REDUNDANCY_GROUP; // Исправлено

    {
        std::lock_guard<std::mutex> lock(connMutex);
        if (running) {
            Napi::Error::New(env, "Server already running").ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    int originatorAddress = 1;
    int k = 12;
    int w = 8;
    int t0 = 30;
    int t1 = 15;
    int t2 = 10;
    int t3 = 20;
    int maxClients = 10;

    if (config.Has("params") && config.Get("params").IsObject()) {
        Napi::Object params = config.Get("params").As<Napi::Object>();
        if (params.Has("originatorAddress")) originatorAddress = params.Get("originatorAddress").As<Napi::Number>().Int32Value();
        if (params.Has("k")) k = params.Get("k").As<Napi::Number>().Int32Value();
        if (params.Has("w")) w = params.Get("w").As<Napi::Number>().Int32Value();
        if (params.Has("t0")) t0 = params.Get("t0").As<Napi::Number>().Int32Value();
        if (params.Has("t1")) t1 = params.Get("t1").As<Napi::Number>().Int32Value();
        if (params.Has("t2")) t2 = params.Get("t2").As<Napi::Number>().Int32Value();
        if (params.Has("t3")) t3 = params.Get("t3").As<Napi::Number>().Int32Value();
        if (params.Has("maxClients")) maxClients = params.Get("maxClients").As<Napi::Number>().Int32Value();

        if (originatorAddress < 0 || originatorAddress > 255 ||
            k <= 0 || w <= 0 || t0 <= 0 || t1 <= 0 || t2 <= 0 || t3 <= 0 || maxClients <= 0) {
            Napi::Error::New(env, "Invalid parameters").ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    try {
        printf("Creating server on port %d, serverID: %s, mode: %s\n", port, serverID.c_str(), mode.c_str());
        fflush(stdout);
        server = CS104_Slave_create(maxClients, maxClients);
        if (!server) {
            throw runtime_error("Failed to create server object");
        }

        CS104_Slave_setLocalPort(server, port);
        CS104_Slave_setConnectionRequestHandler(server, ConnectionRequestHandler, this);
        CS104_Slave_setConnectionEventHandler(server, ConnectionEventHandler, this);
        CS104_Slave_setASDUHandler(server, RawMessageHandler, this);

        CS101_AppLayerParameters alParams = CS104_Slave_getAppLayerParameters(server);
        alParams->originatorAddress = originatorAddress;

        CS104_APCIParameters apciParams = CS104_Slave_getConnectionParameters(server);
        if (!apciParams) {
            CS104_Slave_destroy(server);
            throw runtime_error("Failed to get APCI parameters");
        }
        apciParams->k = k;
        apciParams->w = w;
        apciParams->t0 = t0;
        apciParams->t1 = t1;
        apciParams->t2 = t2;
        apciParams->t3 = t3;

        CS104_Slave_setServerMode(server, serverMode);

        // Настройка групп резервирования для режима redundant
        if (serverMode == CS104_MODE_MULTIPLE_REDUNDANCY_GROUPS) { // Исправлено
            if (config.Has("clients") && config.Get("clients").IsArray()) {
                restrictIPs = true;
                Napi::Array clients = config.Get("clients").As<Napi::Array>();
                for (uint32_t i = 0; i < clients.Length(); i++) {
                    Napi::Value clientVal = clients[i];
                    if (!clientVal.IsObject()) continue;
                    Napi::Object client = clientVal.As<Napi::Object>(); // Исправлено
                    if (client.Has("ip") && client.Has("group")) {
                        std::string ip = client.Get("ip").As<Napi::String>().Utf8Value();
                        std::string group = client.Get("group").As<Napi::String>().Utf8Value();
                        if (redundancyGroups.find(group) == redundancyGroups.end()) {
                            redundancyGroups[group] = CS104_RedundancyGroup_create(group.c_str());
                            CS104_Slave_addRedundancyGroup(server, redundancyGroups[group]);
                        }
                        CS104_RedundancyGroup_addAllowedClient(redundancyGroups[group], ip.c_str());
                        printf("Added client %s to redundancy group %s\n", ip.c_str(), group.c_str());
                        fflush(stdout);
                    }
                }
            } else {
                // Создаем группу по умолчанию без ограничений IP
                restrictIPs = false;
                std::string defaultGroup = "DefaultGroup";
                redundancyGroups[defaultGroup] = CS104_RedundancyGroup_create(defaultGroup.c_str());
                CS104_Slave_addRedundancyGroup(server, redundancyGroups[defaultGroup]);
                printf("Created default redundancy group: %s (no IP restrictions)\n", defaultGroup.c_str());
                fflush(stdout);
            }
        }

        printf("Starting server with params: originatorAddress=%d, k=%d, w=%d, t0=%d, t1=%d, t2=%d, t3=%d, maxClients=%d, serverID: %s, mode: %s\n",
               originatorAddress, k, w, t0, t1, t2, t3, maxClients, serverID.c_str(), mode.c_str());
        fflush(stdout);

        running = true;
        _thread = std::thread([this] {
            CS104_Slave_start(server);
            {
                std::lock_guard<std::mutex> lock(connMutex);
                started = true;
            }
            printf("Server started, serverID: %s\n", serverID.c_str());
            fflush(stdout);

         while (running) {
                Thread_sleep(100);
            }

            // Thread cleanup: only stop server, do not destroy
            {
                std::lock_guard<std::mutex> lock(connMutex);
                if (started && server) {
                    CS104_Slave_stop(server);
                    started = false;
                    printf("Server stopped by thread, serverID: %s\n", serverID.c_str());
                    fflush(stdout);
                }
            }
        });

        return env.Undefined();
    } catch (const std::exception& e) {
        if (server) {
            CS104_Slave_destroy(server);
            server = nullptr;
        }
        printf("Exception in Start: %s, serverID: %s\n", e.what(), serverID.c_str());
        fflush(stdout);
        Napi::Error::New(env, string("Start failed: ") + e.what()).ThrowAsJavaScriptException();
        return env.Undefined();
    }
}

bool IEC104Server::ConnectionRequestHandler(void* parameter, const char* ipAddress) {
    IEC104Server* server = static_cast<IEC104Server*>(parameter);
    printf("Connection request from %s, serverID: %s\n", ipAddress, server->serverID.c_str());
    fflush(stdout);

    if (server->serverMode == CS104_MODE_CONNECTION_IS_REDUNDANCY_GROUP) { // Исправлено
        // В режиме multi разрешаем все подключения
       printf("Connection allowed (multi mode), IP: %s, serverID: %s\n", ipAddress, server->serverID.c_str());
        fflush(stdout);
        return true;
    }

    // В режиме redundant проверяем IP, если ограничения заданы
    if (server->restrictIPs) {
        printf("Connection request for IP %s, validated by redundancy groups, serverID: %s\n", ipAddress, server->serverID.c_str());
            fflush(stdout);
            return true; // lib60870 will reject invalid IPs
        } else {
            printf("IP %s added to DefaultGroup (no restrictions), serverID: %s\n", ipAddress, server->serverID.c_str());
            fflush(stdout);
            if (server->redundancyGroups.find("DefaultGroup") != server->redundancyGroups.end()) {
                CS104_RedundancyGroup_addAllowedClient(server->redundancyGroups["DefaultGroup"], ipAddress);
            }
            return true;
        }
}

static CS101_ASDU createStopDTASDU(CS101_AppLayerParameters alParams) {
    // Create an ASDU with Cause of Transmission = ACTIVATION_TERMINATION (STOPDT)
    CS101_ASDU asdu = CS101_ASDU_create(alParams, false, CS101_COT_ACTIVATION_TERMINATION, 0, 0, false, false);
    // No additional information objects are typically needed for STOPDT
    return asdu;
}

void IEC104Server::ConnectionEventHandler(void* parameter, IMasterConnection connection, CS104_PeerConnectionEvent event) {
    IEC104Server* server = static_cast<IEC104Server*>(parameter);
    std::string eventStr;
    std::string reason;
    std::string clientIdStr;

    {
        std::lock_guard<std::mutex> lock(server->connMutex);
        switch (event) {
            case CS104_CON_EVENT_CONNECTION_OPENED: {
                char ipBuffer[256] = {0};
                IMasterConnection_getPeerAddress(connection, ipBuffer, sizeof(ipBuffer)-1);
                std::string ipPort = ipBuffer;
                
                server->ipConnectionCounts[ipPort]++;
                clientIdStr = ipPort;

                server->clientConnections[connection] = clientIdStr;

                eventStr = "connected";
                reason = "client connected";
                break;
            }
            case CS104_CON_EVENT_CONNECTION_CLOSED: {
                eventStr = "disconnected";
                reason = "client disconnected";
                if (server->clientConnections.find(connection) != server->clientConnections.end()) {
                    clientIdStr = server->clientConnections[connection];
                    server->clientConnections.erase(connection);
                    server->ipConnectionCounts[clientIdStr]--;
                    if (server->ipConnectionCounts[clientIdStr] <= 0) {
                        server->ipConnectionCounts.erase(clientIdStr);
                    }
                }
                break;
            }
            case CS104_CON_EVENT_ACTIVATED: {
                eventStr = "activated";
                reason = "STARTDT confirmed";
                if (server->clientConnections.find(connection) != server->clientConnections.end()) {
                    clientIdStr = server->clientConnections[connection];
                }
                break;
            }
            case CS104_CON_EVENT_DEACTIVATED: {
                eventStr = "deactivated";
                reason = "STOPDT confirmed";
                if (server->clientConnections.find(connection) != server->clientConnections.end()) {
                    clientIdStr = server->clientConnections[connection];
                    // Send STOPDT ASDU in redundant mode
                    if (server->serverMode == CS104_MODE_MULTIPLE_REDUNDANCY_GROUPS) {
                        CS101_AppLayerParameters alParams = CS104_Slave_getAppLayerParameters(server->server); // Fix: Use server->server
                        CS101_ASDU stopdtAsdu = createStopDTASDU(alParams);
                        IMasterConnection_sendASDU(connection, stopdtAsdu);
                        CS101_ASDU_destroy(stopdtAsdu);
                    }
                }
                break;
            }
        }
    }

    printf("Connection event: %s, clientId: %s, reason: %s, serverID: %s\n",
           eventStr.c_str(), clientIdStr.c_str(), reason.c_str(), server->serverID.c_str());
    fflush(stdout);

    server->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
        Napi::Object eventObj = Napi::Object::New(env);
        eventObj.Set("serverID", Napi::String::New(env, server->serverID));
        eventObj.Set("type", Napi::String::New(env, "control"));
        eventObj.Set("event", Napi::String::New(env, eventStr));
        eventObj.Set("reason", Napi::String::New(env, reason));
        eventObj.Set("clientId", Napi::String::New(env, clientIdStr));        
        jsCallback.Call({Napi::String::New(env, "data"), eventObj});
    });
}

Napi::Value IEC104Server::Stop(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    {
        std::lock_guard<std::mutex> lock(connMutex);
        if (running) {
            running = false;
            if (started && server) {
                printf("Stop called, stopping server, serverID: %s\n", serverID.c_str());
                fflush(stdout);
                CS104_Slave_stop(server);
                CS104_Slave_destroy(server);
                server = nullptr;
                started = false;
            }
        }
    }

    if (_thread.joinable()) {
        _thread.join();
    }

    // Clear redundancy groups
    for (auto& [name, group] : redundancyGroups) {
        if (group) {
            CS104_RedundancyGroup_destroy(group);
            group = nullptr;
        }
    }
    redundancyGroups.clear();

    // Clear client connections
    {
        std::lock_guard<std::mutex> lock(connMutex);
        clientConnections.clear();
        ipConnectionCounts.clear();
    }

    return env.Undefined();
}

Napi::Value IEC104Server::SendCommands(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsArray()) {
        Napi::TypeError::New(env, "Expected clientId (string), commands (array of objects with 'typeId', 'ioa', 'value', 'asduAddress' and optional fields)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string clientIdStr = info[0].As<Napi::String>().Utf8Value();
    Napi::Array commands = info[1].As<Napi::Array>();

    std::lock_guard<std::mutex> lock(connMutex);
    if (!started) {
        Napi::Error::New(env, "Server not started").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    IMasterConnection targetConnection = nullptr;
    for (const auto& [conn, id] : clientConnections) {
        if (id == clientIdStr) {
            targetConnection = conn;
            break;
        }
    }

    if (!targetConnection) {
        Napi::Error::New(env, "Client with specified ID not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    CS101_AppLayerParameters alParams = CS104_Slave_getAppLayerParameters(server);

    try {
        bool allSuccess = true;

        // Группировка команд по typeId и asduAddress
        std::map<std::pair<int, int>, std::vector<Napi::Object>> groupedCommands;
        for (uint32_t i = 0; i < commands.Length(); i++) {
            Napi::Value item = commands[i];
            if (!item.IsObject()) {
                Napi::TypeError::New(env, "Each command must be an object").ThrowAsJavaScriptException();
                return env.Undefined();
            }

            Napi::Object cmdObj = item.As<Napi::Object>();
            if (!cmdObj.Has("typeId") || !cmdObj.Has("ioa") || !cmdObj.Has("value") || !cmdObj.Has("asduAddress")) {
                Napi::TypeError::New(env, "Each command must have 'typeId' (number), 'ioa' (number), 'value', and 'asduAddress' (number)").ThrowAsJavaScriptException();
                return env.Undefined();
            }

            int typeId = cmdObj.Get("typeId").As<Napi::Number>().Int32Value();
            int asduAddress = cmdObj.Get("asduAddress").As<Napi::Number>().Int32Value();
            groupedCommands[{typeId, asduAddress}].push_back(cmdObj);
        }

        // Обработка каждой группы
        for (const auto& [key, cmdList] : groupedCommands) {
            int typeId = key.first;
            int asduAddress = key.second;

            // Получение параметров для ASDU (cot, quality и т.д. берем из первой команды в группе)
            Napi::Object firstCmd = cmdList[0];
            bool bselCmd = false;
            int ql = 0;
            int cot = CS101_COT_SPONTANEOUS;
            uint8_t quality = IEC60870_QUALITY_GOOD;

            if (firstCmd.Has("bselCmd") && firstCmd.Get("bselCmd").IsBoolean()) {
                bselCmd = firstCmd.Get("bselCmd").As<Napi::Boolean>();
            }
            if (firstCmd.Has("ql") && firstCmd.Get("ql").IsNumber()) {
                ql = firstCmd.Get("ql").As<Napi::Number>().Int32Value();
                if (ql < 0 || ql > 31) {
                    Napi::RangeError::New(env, "ql must be between 0 and 31").ThrowAsJavaScriptException();
                    return env.Undefined();
                }
            }
            if (firstCmd.Has("cot") && firstCmd.Get("cot").IsNumber()) {
                cot = firstCmd.Get("cot").As<Napi::Number>().Int32Value();
                if (cot < 0 || cot > 63) {
                    Napi::RangeError::New(env, "cot must be between 0 and 63").ThrowAsJavaScriptException();
                    return env.Undefined();
                }
            }
            if (firstCmd.Has("quality") && firstCmd.Get("quality").IsNumber()) {
                quality = firstCmd.Get("quality").As<Napi::Number>().Uint32Value();
            }

            // Создание ASDU для группы
            CS101_ASDU asdu = CS101_ASDU_create(alParams, false, (CS101_CauseOfTransmission)cot, 0, asduAddress, false, false);
            bool success = true;

        // Обработка всех команд в группе
        for (const auto& cmdObj : cmdList) {
            int ioa = cmdObj.Get("ioa").As<Napi::Number>().Int32Value();
            switch (typeId) {
                case M_SP_NA_1: {
                    if (!cmdObj.Get("value").IsBoolean()) {
                        Napi::TypeError::New(env, "M_SP_NA_1 requires 'value' as boolean").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    SinglePointInformation sp = SinglePointInformation_create(NULL, ioa, value, quality);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sp);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SinglePointInformation_destroy(sp);
                    break;
                }
                case M_DP_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_DP_NA_1 requires 'value' as number (0-3)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "M_DP_NA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    DoublePointInformation dp = DoublePointInformation_create(NULL, ioa, (DoublePointValue)value, quality);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dp);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    DoublePointInformation_destroy(dp);
                    break;
                }
                case M_ST_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_ST_NA_1 requires 'value' as number (-64 to 63)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < -64 || value > 63) {
                        Napi::RangeError::New(env, "M_ST_NA_1 'value' must be between -64 and 63").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    StepPositionInformation st = StepPositionInformation_create(NULL, ioa, value, false, quality);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)st);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    StepPositionInformation_destroy(st);
                    break;
                }
                case M_BO_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_BO_NA_1 requires 'value' as number (32-bit unsigned integer)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint32_t value = cmdObj.Get("value").As<Napi::Number>().Uint32Value();
                    BitString32 bo = BitString32_create(NULL, ioa, value);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bo);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    BitString32_destroy(bo);
                    break;
                }
                case M_ME_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_ME_NA_1 requires 'value' as number (-1.0 to 1.0)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    if (value < -1.0f || value > 1.0f) {
                        Napi::RangeError::New(env, "M_ME_NA_1 'value' must be between -1.0 and 1.0").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    MeasuredValueNormalized mn = MeasuredValueNormalized_create(NULL, ioa, value, quality);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)mn);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    MeasuredValueNormalized_destroy(mn);
                    break;
                }
                case M_ME_NB_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_ME_NB_1 requires 'value' as number (-32768 to 32767)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    double doubleValue = cmdObj.Get("value").As<Napi::Number>().DoubleValue();
                    int value = static_cast<int>(doubleValue);
                    if (value < -32768 || value > 32767) {
                        Napi::RangeError::New(env, "M_ME_NB_1 'value' must be between -32768 and 32767").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    MeasuredValueScaled ms = MeasuredValueScaled_create(NULL, ioa, value, quality);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)ms);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    MeasuredValueScaled_destroy(ms);
                    break;
                }
                case M_ME_NC_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_ME_NC_1 requires 'value' as number").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    double doubleValue = cmdObj.Get("value").As<Napi::Number>().DoubleValue();                  
                    float value = static_cast<float>(doubleValue);
                    MeasuredValueShort mc = MeasuredValueShort_create(NULL, ioa, value, quality);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)mc);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    MeasuredValueShort_destroy(mc);
                    break;
                }
                case M_IT_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_IT_NA_1 requires 'value' as number").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    BinaryCounterReading bcr = BinaryCounterReading_create(NULL, value, 0, false, false, false);
                    IntegratedTotals it = IntegratedTotals_create(NULL, ioa, bcr);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)it);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    IntegratedTotals_destroy(it);
                    BinaryCounterReading_destroy(bcr);
                    break;
                }
                case M_SP_TB_1: {
                    if (!cmdObj.Get("value").IsBoolean() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_SP_TB_1 requires 'value' (boolean) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    SinglePointWithCP56Time2a sp = SinglePointWithCP56Time2a_create(NULL, ioa, value, quality, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sp);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SinglePointWithCP56Time2a_destroy(sp);
                    break;
                }
                case M_DP_TB_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_DP_TB_1 requires 'value' (number 0-3) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "M_DP_TB_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    DoublePointWithCP56Time2a dp = DoublePointWithCP56Time2a_create(NULL, ioa, (DoublePointValue)value, quality, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dp);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    DoublePointWithCP56Time2a_destroy(dp);
                    break;
                }
                case M_ST_TB_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_ST_TB_1 requires 'value' (number -64 to 63) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -64 || value > 63) {
                        Napi::RangeError::New(env, "M_ST_TB_1 'value' must be between -64 and 63").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    StepPositionWithCP56Time2a st = StepPositionWithCP56Time2a_create(NULL, ioa, value, false, quality, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)st);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    StepPositionWithCP56Time2a_destroy(st);
                    break;
                }
                case M_BO_TB_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_BO_TB_1 requires 'value' (32-bit number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint32_t value = cmdObj.Get("value").As<Napi::Number>().Uint32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    Bitstring32WithCP56Time2a bo = Bitstring32WithCP56Time2a_create(NULL, ioa, value, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bo);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    Bitstring32WithCP56Time2a_destroy(bo);
                    break;
                }
                case M_ME_TD_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_ME_TD_1 requires 'value' (number -1.0 to 1.0) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    double doubleValue = cmdObj.Get("value").As<Napi::Number>().DoubleValue();                  
                    float value = static_cast<float>(doubleValue);
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -1.0f || value > 1.0f) {
                        Napi::RangeError::New(env, "M_ME_TD_1 'value' must be between -1.0 and 1.0").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    MeasuredValueNormalizedWithCP56Time2a mn = MeasuredValueNormalizedWithCP56Time2a_create(NULL, ioa, value, quality, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)mn);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    MeasuredValueNormalizedWithCP56Time2a_destroy(mn);
                    break;
                }
                case M_ME_TE_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_ME_TE_1 requires 'value' (number -32768 to 32767) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -32768 || value > 32767) {
                        Napi::RangeError::New(env, "M_ME_TE_1 'value' must be between -32768 and 32767").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    MeasuredValueScaledWithCP56Time2a ms = MeasuredValueScaledWithCP56Time2a_create(NULL, ioa, value, quality, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)ms);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    MeasuredValueScaledWithCP56Time2a_destroy(ms);
                    break;
                }
                case M_ME_TF_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_ME_TF_1 requires 'value' (number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    double doubleValue = cmdObj.Get("value").As<Napi::Number>().DoubleValue();                  
                    float value = static_cast<float>(doubleValue);
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    MeasuredValueShortWithCP56Time2a mc = MeasuredValueShortWithCP56Time2a_create(NULL, ioa, value, quality, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)mc);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    MeasuredValueShortWithCP56Time2a_destroy(mc);
                    break;
                }
                case M_IT_TB_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_IT_TB_1 requires 'value' (number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    BinaryCounterReading bcr = BinaryCounterReading_create(NULL, value, 0, false, false, false);
                    IntegratedTotalsWithCP56Time2a it = IntegratedTotalsWithCP56Time2a_create(NULL, ioa, bcr, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)it);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    IntegratedTotalsWithCP56Time2a_destroy(it);
                    BinaryCounterReading_destroy(bcr);
                    break;
                }
                case C_SC_NA_1: {
                    if (!cmdObj.Get("value").IsBoolean()) {
                        Napi::TypeError::New(env, "C_SC_NA_1 requires 'value' as boolean").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    SingleCommand sc = SingleCommand_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sc);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SingleCommand_destroy(sc);
                    break;
                }
                case C_DC_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_DC_NA_1 requires 'value' as number (0-3)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "C_DC_NA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    DoubleCommand dc = DoubleCommand_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dc);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    DoubleCommand_destroy(dc);
                    break;
                }
                case C_RC_TA_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_RC_TA_1 requires 'value' (number 0-3) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "C_RC_TA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    StepCommandWithCP56Time2a rc = StepCommandWithCP56Time2a_create(NULL, ioa, (StepCommandValue)value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)rc);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    StepCommandWithCP56Time2a_destroy(rc);
                    break;
                }
                case C_SE_TA_1: {
                    if (!cmdObj.Get("value").IsString() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TA_1 requires 'value' (string representing float -1.0 to 1.0) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    std::string valueStr = cmdObj.Get("value").As<Napi::String>().Utf8Value();
                    float value;
                    try {
                        value = std::stof(valueStr);
                    } catch (...) {
                        Napi::TypeError::New(env, "C_SE_TA_1 'value' must be a valid float string").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -1.0f || value > 1.0f) {
                        Napi::RangeError::New(env, "C_SE_TA_1 'value' must be between -1.0 and 1.0").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    SetpointCommandNormalizedWithCP56Time2a se = SetpointCommandNormalizedWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)se);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SetpointCommandNormalizedWithCP56Time2a_destroy(se);
                    break;
                }
                case C_SE_NB_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_NB_1 requires 'value' as number (-32768 to 32767)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < -32768 || value > 32767) {
                        Napi::RangeError::New(env, "C_SE_NB_1 'value' must be between -32768 and 32767").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    SetpointCommandScaled se = SetpointCommandScaled_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)se);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SetpointCommandScaled_destroy(se);
                    break;
                }
                case C_SE_NC_1: {
                    if (!cmdObj.Get("value").IsString()) {
                        Napi::TypeError::New(env, "C_SE_NC_1 requires 'value' as string representing a float").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    std::string valueStr = cmdObj.Get("value").As<Napi::String>().Utf8Value();
                    float value;
                    try {
                        value = std::stof(valueStr);
                    } catch (...) {
                        Napi::TypeError::New(env, "C_SE_NC_1 'value' must be a valid float string").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    SetpointCommandShort se = SetpointCommandShort_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)se);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SetpointCommandShort_destroy(se);
                    break;
                }
                case C_BO_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_BO_NA_1 requires 'value' as number (32-bit unsigned integer)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint32_t value = cmdObj.Get("value").As<Napi::Number>().Uint32Value();
                    Bitstring32Command bo = Bitstring32Command_create(NULL, ioa, value);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bo);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    Bitstring32Command_destroy(bo);
                    break;
                }
                case C_SC_TA_1: {
                    if (!cmdObj.Get("value").IsBoolean() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SC_TA_1 requires 'value' (boolean) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    SingleCommandWithCP56Time2a sc = SingleCommandWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sc);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SingleCommandWithCP56Time2a_destroy(sc);
                    break;
                }
                case C_DC_TA_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_DC_TA_1 requires 'value' (number 0-3) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "C_DC_TA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    DoubleCommandWithCP56Time2a dc = DoubleCommandWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dc);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    DoubleCommandWithCP56Time2a_destroy(dc);
                    break;
                }
                case C_SE_TB_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TB_1 requires 'value' (number -32768 to 32767) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -32768 || value > 32767) {
                        Napi::RangeError::New(env, "C_SE_TB_1 'value' must be between -32768 and 32767").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    SetpointCommandScaledWithCP56Time2a se = SetpointCommandScaledWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)se);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SetpointCommandScaledWithCP56Time2a_destroy(se);
                    break;
                }
                case C_SE_TC_1: {
                    if (!cmdObj.Get("value").IsString() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TC_1 requires 'value' (string representing float) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    std::string valueStr = cmdObj.Get("value").As<Napi::String>().Utf8Value();
                    float value;
                    try {
                        value = std::stof(valueStr);
                    } catch (...) {
                        Napi::TypeError::New(env, "C_SE_TC_1 'value' must be a valid float string").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    SetpointCommandShortWithCP56Time2a se = SetpointCommandShortWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)se);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SetpointCommandShortWithCP56Time2a_destroy(se);
                    break;
                }
                case C_BO_TA_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_BO_TA_1 requires 'value' (32-bit number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint32_t value = cmdObj.Get("value").As<Napi::Number>().Uint32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    Bitstring32CommandWithCP56Time2a bo = Bitstring32CommandWithCP56Time2a_create(NULL, ioa, value, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bo);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    Bitstring32CommandWithCP56Time2a_destroy(bo);
                    break;
                }
                case C_IC_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_IC_NA_1 requires 'value' as number (QOI, 0-255)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < 0 || value > 255) {
                        Napi::RangeError::New(env, "C_IC_NA_1 'value' (QOI) must be 0-255").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    InterrogationCommand ic = InterrogationCommand_create(NULL, ioa, value);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)ic);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    InterrogationCommand_destroy(ic);
                    break;
                }
                case C_CI_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_CI_NA_1 requires 'value' as number (QCC, 0-255)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < 0 || value > 255) {
                        Napi::RangeError::New(env, "C_CI_NA_1 'value' (QCC) must be 0-255").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    CounterInterrogationCommand ci = CounterInterrogationCommand_create(NULL, ioa, value);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)ci);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    CounterInterrogationCommand_destroy(ci);
                    break;
                }
                case C_RD_NA_1: {
                    ReadCommand rd = ReadCommand_create(NULL, ioa);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)rd);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    ReadCommand_destroy(rd);
                    break;
                }
                case C_CS_NA_1: {
                    if (!cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_CS_NA_1 requires 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    ClockSynchronizationCommand cs = ClockSynchronizationCommand_create(NULL, ioa, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)cs);
                    //success = IMasterConnection_sendASDU(targetConnection, asdu);
                    ClockSynchronizationCommand_destroy(cs);
                    break;
                }
                default:
                        printf("Unsupported command type: %d, serverID: %s, clientId: %s\n", typeId, serverID.c_str(), clientIdStr.c_str());
                        fflush(stdout);
                        success = false;
                        break;
                }
            }

            // Отправка ASDU после добавления всех объектов
            if (success) {
                success = IMasterConnection_sendASDU(targetConnection, asdu);
                if (!success) {
                    allSuccess = false;
                    printf("Failed to send ASDU: typeId=%d, asduAddress=%d, serverID: %s, clientId: %s\n",
                           typeId, asduAddress, serverID.c_str(), clientIdStr.c_str());
                    fflush(stdout);
                } else {
                    printf("Sent ASDU: typeId=%d, asduAddress=%d, serverID: %s, clientId: %s\n",
                           typeId, asduAddress, serverID.c_str(), clientIdStr.c_str());
                    fflush(stdout);
                }
            }

            CS101_ASDU_destroy(asdu);
        }

        return Napi::Boolean::New(env, allSuccess);
    } catch (const std::exception& e) {
        printf("Exception in SendCommands: %s, serverID: %s\n", e.what(), serverID.c_str());
        fflush(stdout);
        Napi::Error::New(env, string("SendCommands failed: ") + e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}

            

Napi::Value IEC104Server::GetStatus(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(connMutex);
    Napi::Object status = Napi::Object::New(env);
    status.Set("started", Napi::Boolean::New(env, started));
    status.Set("serverID", Napi::String::New(env, serverID));
    status.Set("mode", Napi::String::New(env, serverMode == CS104_MODE_MULTIPLE_REDUNDANCY_GROUPS ? "redundant" : "multi"));
    status.Set("restrictIPs", Napi::Boolean::New(env, restrictIPs));

    Napi::Array clients = Napi::Array::New(env, clientConnections.size());
    int index = 0;
    for (const auto& [conn, id] : clientConnections) {
        clients[index++] = Napi::String::New(env, id);
    }
    status.Set("connectedClients", clients);

    return status;
}

bool IEC104Server::RawMessageHandler(void* parameter, IMasterConnection connection, CS101_ASDU asdu) {
    IEC104Server* server = static_cast<IEC104Server*>(parameter);
    IEC60870_5_TypeID typeID = CS101_ASDU_getTypeID(asdu);
    int numberOfElements = CS101_ASDU_getNumberOfElements(asdu);
    int receivedAsduAddress = CS101_ASDU_getCA(asdu);
    std::string clientIdStr;

    {
        std::lock_guard<std::mutex> lock(server->connMutex);
        if (server->clientConnections.find(connection) != server->clientConnections.end()) {
            clientIdStr = server->clientConnections[connection];
        } else {
            printf("Received message from unknown client, serverID: %s\n", server->serverID.c_str());
            fflush(stdout);
            return false;
        }
    }

    try {
        vector<tuple<int, double, uint8_t, uint64_t, bool, int>> elements;

        switch (typeID) {
            case C_SC_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SingleCommand io = (SingleCommand)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SingleCommand_getState(io) ? 1.0 : 0.0;
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = SingleCommand_isSelect(io);
                        int ql = SingleCommand_getQU(io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SingleCommand_destroy(io);
                    }
                }
                break;
            }
            case C_DC_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    DoubleCommand io = (DoubleCommand)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(DoubleCommand_getState(io));
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = DoubleCommand_isSelect(io);
                        int ql = DoubleCommand_getQU(io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        DoubleCommand_destroy(io);
                    }
                }
                break;
            }
            case C_RC_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    StepCommand io = (StepCommand)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(StepCommand_getState(io));
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = StepCommand_isSelect(io);
                        int ql = StepCommand_getQU(io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        StepCommand_destroy(io);
                    }
                }
                break;
            }
            case C_SE_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SetpointCommandNormalized io = (SetpointCommandNormalized)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SetpointCommandNormalized_getValue(io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = SetpointCommandNormalized_isSelect(io);
                        int ql = SetpointCommandNormalized_getQL(io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SetpointCommandNormalized_destroy(io);
                    }
                }
                break;
            }
            case C_SE_NB_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SetpointCommandScaled io = (SetpointCommandScaled)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SetpointCommandScaled_getValue(io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = SetpointCommandScaled_isSelect(io);
                        int ql = SetpointCommandScaled_getQL(io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SetpointCommandScaled_destroy(io);
                    }
                }
                break;
            }
            case C_SE_NC_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SetpointCommandShort io = (SetpointCommandShort)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SetpointCommandShort_getValue(io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = SetpointCommandShort_isSelect(io);
                        int ql = SetpointCommandShort_getQL(io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SetpointCommandShort_destroy(io);
                    }
                }
                break;
            }
            case C_BO_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    Bitstring32Command io = (Bitstring32Command)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(Bitstring32Command_getValue(io));
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = false;
                        int ql = 0;
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        Bitstring32Command_destroy(io);
                    }
                }
                break;
            }
            case C_SC_TA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SingleCommandWithCP56Time2a io = (SingleCommandWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SingleCommand_getState((SingleCommand)io) ? 1.0 : 0.0;
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(SingleCommandWithCP56Time2a_getTimestamp(io));
                        bool bselCmd = SingleCommand_isSelect((SingleCommand)io);
                        int ql = SingleCommand_getQU((SingleCommand)io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SingleCommandWithCP56Time2a_destroy(io);
                    }
                }
                break;
            }
            case C_DC_TA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    DoubleCommandWithCP56Time2a io = (DoubleCommandWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(DoubleCommand_getState((DoubleCommand)io));
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(DoubleCommandWithCP56Time2a_getTimestamp(io));
                        bool bselCmd = DoubleCommand_isSelect((DoubleCommand)io);
                        int ql = DoubleCommand_getQU((DoubleCommand)io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        DoubleCommandWithCP56Time2a_destroy(io);
                    }
                }
                break;
            }
            case C_RC_TA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    StepCommandWithCP56Time2a io = (StepCommandWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(StepCommand_getState((StepCommand)io));
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(StepCommandWithCP56Time2a_getTimestamp(io));
                        bool bselCmd = StepCommand_isSelect((StepCommand)io);
                        int ql = StepCommand_getQU((StepCommand)io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        StepCommandWithCP56Time2a_destroy(io);
                    }
                }
                break;
            }
            case C_SE_TA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SetpointCommandNormalizedWithCP56Time2a io = (SetpointCommandNormalizedWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SetpointCommandNormalized_getValue((SetpointCommandNormalized)io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(SetpointCommandNormalizedWithCP56Time2a_getTimestamp(io));
                        bool bselCmd = SetpointCommandNormalized_isSelect((SetpointCommandNormalized)io);
                        int ql = SetpointCommandNormalized_getQL((SetpointCommandNormalized)io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SetpointCommandNormalizedWithCP56Time2a_destroy(io);
                    }
                }
                break;
            }
            case C_SE_TB_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SetpointCommandScaledWithCP56Time2a io = (SetpointCommandScaledWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SetpointCommandScaled_getValue((SetpointCommandScaled)io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(SetpointCommandScaledWithCP56Time2a_getTimestamp(io));
                        bool bselCmd = SetpointCommandScaled_isSelect((SetpointCommandScaled)io);
                        int ql = SetpointCommandScaled_getQL((SetpointCommandScaled)io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SetpointCommandScaledWithCP56Time2a_destroy(io);
                    }
                }
                break;
            }
            case C_SE_TC_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SetpointCommandShortWithCP56Time2a io = (SetpointCommandShortWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SetpointCommandShort_getValue((SetpointCommandShort)io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(SetpointCommandShortWithCP56Time2a_getTimestamp(io));
                        bool bselCmd = SetpointCommandShort_isSelect((SetpointCommandShort)io);
                        int ql = SetpointCommandShort_getQL((SetpointCommandShort)io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SetpointCommandShortWithCP56Time2a_destroy(io);
                    }
                }
                break;
            }
            case C_BO_TA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    Bitstring32CommandWithCP56Time2a io = (Bitstring32CommandWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(Bitstring32Command_getValue((Bitstring32Command)io));
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(Bitstring32CommandWithCP56Time2a_getTimestamp(io));
                        bool bselCmd = false;
                        int ql = 0;
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        Bitstring32CommandWithCP56Time2a_destroy(io);
                    }
                }
                break;
            }
            case C_IC_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    InterrogationCommand io = (InterrogationCommand)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = InterrogationCommand_getQOI(io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = false;
                        int ql = 0;
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        InterrogationCommand_destroy(io);
                    }
                }
                break;
            }
            case C_CI_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    CounterInterrogationCommand io = (CounterInterrogationCommand)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = CounterInterrogationCommand_getQCC(io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = false;
                        int ql = 0;
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        CounterInterrogationCommand_destroy(io);
                    }
                }
                break;
            }
            case C_RD_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    ReadCommand io = (ReadCommand)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = 0;
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = false;
                        int ql = 0;
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        ReadCommand_destroy(io);
                    }
                }
                break;
            }
            case C_CS_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    ClockSynchronizationCommand io = (ClockSynchronizationCommand)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = 0;
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(ClockSynchronizationCommand_getTime(io));
                        bool bselCmd = false;
                        int ql = 0;
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        ClockSynchronizationCommand_destroy(io);
                    }
                }
                break;
            }
           default:
                printf("Received unsupported ASDU type: %s (%i), serverID: %s, clientId: %s, asduAddress: %d\n",
                       TypeID_toString(typeID), typeID, server->serverID.c_str(), clientIdStr.c_str(), receivedAsduAddress);
                fflush(stdout);
                return false;
        }
      
     for (const auto& [ioa, val, quality, timestamp, bselCmd, ql] : elements) {
            printf("ASDU type: %s, serverID: %s, clientId: %s, asduAddress: %d, ioa: %i, value: %f, quality: %u, timestamp: %" PRIu64 ", bselCmd: %d, ql: %d, cnt: %i\n",
                   TypeID_toString(typeID), server->serverID.c_str(), clientIdStr.c_str(), receivedAsduAddress, ioa, val, quality, timestamp, bselCmd, ql, server->cnt);
            fflush(stdout);
        }

        server->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
            Napi::Array jsArray = Napi::Array::New(env, elements.size());
            for (size_t i = 0; i < elements.size(); i++) {
                const auto& [ioa, val, quality, timestamp, bselCmd, ql] = elements[i];
                Napi::Object msg = Napi::Object::New(env);
                msg.Set("serverID", Napi::String::New(env, server->serverID));               
                msg.Set("clientId", Napi::String::New(env, clientIdStr));
                msg.Set("typeId", Napi::Number::New(env, typeID));
                msg.Set("asduAddress", Napi::Number::New(env, receivedAsduAddress));
                msg.Set("ioa", Napi::Number::New(env, ioa));
                msg.Set("val", Napi::Number::New(env, val));
                msg.Set("quality", Napi::Number::New(env, quality));
                msg.Set("bselCmd", Napi::Boolean::New(env, bselCmd));
                msg.Set("ql", Napi::Number::New(env, ql));
                if (timestamp > 0) {
                    msg.Set("timestamp", Napi::Number::New(env, static_cast<double>(timestamp)));
                }
                jsArray[i] = msg;
            }
            jsCallback.Call({Napi::String::New(env, "data"), jsArray});
            server->cnt++;
        });

        return true;
    } catch (const std::exception& e) {
        printf("Exception in RawMessageHandler: %s, serverID: %s, clientId: %s, asduAddress: %d\n",
               e.what(), server->serverID.c_str(), clientIdStr.c_str(), receivedAsduAddress);
        fflush(stdout);
        server->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object eventObj = Napi::Object::New(env);
            eventObj.Set("serverID", Napi::String::New(env, server->serverID));
            eventObj.Set("clientId", Napi::String::New(env, clientIdStr));
            eventObj.Set("type", Napi::String::New(env, "error"));
            eventObj.Set("reason", Napi::String::New(env, string("Обработка ASDU не удалась: ") + e.what()));
            jsCallback.Call({Napi::String::New(env, "data"), eventObj});
        });
        return false;
    }
}

