#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "cs104_server.h"
#include <inttypes.h>
#include <stdexcept>
#include <vector>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    asduAddress = 0; // Инициализация по умолчанию

    try {
        tsfn = Napi::ThreadSafeFunction::New(
            info.Env(),
            emit,
            "IEC104ServerTSFN",
            0,
            1,
            [](Napi::Env) {}
        );
    } catch (const std::exception& e) {
        printf("Failed to create ThreadSafeFunction: %s\n", e.what());
        Napi::Error::New(info.Env(), string("TSFN creation failed: ") + e.what()).ThrowAsJavaScriptException();
    }
}

IEC104Server::~IEC104Server() {
    std::lock_guard<std::mutex> lock(connMutex);
    if (running) {
        running = false;
        if (started) {
            printf("Destructor stopping server, serverID: %s, serverId: %i\n", serverID.c_str(), serverId);
            CS104_Slave_stop(server);
            CS104_Slave_destroy(server);
            started = false;
        }
        if (_thread.joinable()) {
            _thread.join();
        }
        tsfn.Release();
    }
}

Napi::Value IEC104Server::Start(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsString()) {
        Napi::TypeError::New(env, "Expected port (number), serverId (number), serverID (string), [params (object)]").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    int port = info[0].As<Napi::Number>().Int32Value();
    serverId = info[1].As<Napi::Number>().Int32Value();
    serverID = info[2].As<Napi::String>();

    if (port <= 0 || serverID.empty()) {
        Napi::Error::New(env, "Invalid port or serverID").ThrowAsJavaScriptException();
        return env.Undefined();
    }

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

    if (info.Length() > 3 && info[3].IsObject()) {
        Napi::Object params = info[3].As<Napi::Object>();
        if (params.Has("originatorAddress")) originatorAddress = params.Get("originatorAddress").As<Napi::Number>().Int32Value();
        if (params.Has("asduAddress")) asduAddress = params.Get("asduAddress").As<Napi::Number>().Int32Value(); // Извлечение asduAddress
        if (params.Has("k")) k = params.Get("k").As<Napi::Number>().Int32Value();
        if (params.Has("w")) w = params.Get("w").As<Napi::Number>().Int32Value();
        if (params.Has("t0")) t0 = params.Get("t0").As<Napi::Number>().Int32Value();
        if (params.Has("t1")) t1 = params.Get("t1").As<Napi::Number>().Int32Value();
        if (params.Has("t2")) t2 = params.Get("t2").As<Napi::Number>().Int32Value();
        if (params.Has("t3")) t3 = params.Get("t3").As<Napi::Number>().Int32Value();
        if (params.Has("maxClients")) maxClients = params.Get("maxClients").As<Napi::Number>().Int32Value();

        if (originatorAddress < 0 || originatorAddress > 255) {
            Napi::Error::New(env, "originatorAddress must be 0-255").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (asduAddress < 0 || asduAddress > 65535) { // Ограничение для ASDU адреса (2 байта)
            Napi::Error::New(env, "asduAddress must be 0-65535").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (k <= 0 || w <= 0 || t0 <= 0 || t1 <= 0 || t2 <= 0 || t3 <= 0) {
            Napi::Error::New(env, "k, w, t0, t1, t2, t3 must be positive").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (maxClients <= 0) {
            Napi::Error::New(env, "maxClients must be positive").ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    try {
        printf("Creating server on port %d, serverID: %s, serverId: %i\n", port, serverID.c_str(), serverId);
        server = CS104_Slave_create(maxClients, maxClients); // Max clients and queue size
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

        CS104_Slave_setServerMode(server, CS104_MODE_MULTIPLE_REDUNDANCY_GROUPS);

        printf("Starting server with params: originatorAddress=%d, asduAddress=%d, k=%d, w=%d, t0=%d, t1=%d, t2=%d, t3=%d, maxClients=%d, serverID: %s, serverId: %i\n",
               originatorAddress, asduAddress, k, w, t0, t1, t2, t3, maxClients, serverID.c_str(), serverId);

        running = true;
        _thread = std::thread([this] {
            CS104_Slave_start(server);
            {
                std::lock_guard<std::mutex> lock(connMutex);
                started = true;
            }
            printf("Server started, serverID: %s, serverId: %i\n", serverID.c_str(), serverId);

            while (running) {
                Thread_sleep(100);
            }

            std::lock_guard<std::mutex> lock(connMutex);
            CS104_Slave_stop(server);
            CS104_Slave_destroy(server);
            started = false;
            printf("Server stopped, serverID: %s, serverId: %i\n", serverID.c_str(), serverId);
        });

        return env.Undefined();
    } catch (const std::exception& e) {
        printf("Exception in Start: %s, serverID: %s, serverId: %i\n", e.what(), serverID.c_str(), serverId);
        Napi::Error::New(env, string("Start failed: ") + e.what()).ThrowAsJavaScriptException();
        return env.Undefined();
    }
}

Napi::Value IEC104Server::Stop(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    {
        std::lock_guard<std::mutex> lock(connMutex);
        if (running) {
            running = false;
            if (started) {
                printf("Stop called, stopping server, serverID: %s, serverId: %i\n", serverID.c_str(), serverId);
                CS104_Slave_stop(server);
                CS104_Slave_destroy(server);
                started = false;
            }
        }
    }

    if (_thread.joinable()) {
        _thread.join();
    }

    std::lock_guard<std::mutex> lock(connMutex);
    tsfn.Release();

    return env.Undefined();
}

Napi::Value IEC104Server::SendCommands(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsArray()) {
        Napi::TypeError::New(env, "Expected clientId (number), commands (array of objects with 'typeId', 'ioa', 'value', and optional fields)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    int targetClientId = info[0].As<Napi::Number>().Int32Value();
    Napi::Array commands = info[1].As<Napi::Array>();

    std::lock_guard<std::mutex> lock(connMutex);
    if (!started) {
        Napi::Error::New(env, "Server not started").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    IMasterConnection targetConnection = nullptr;
    for (const auto& [conn, id] : clientConnections) {
        if (id == targetClientId) {
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
        for (uint32_t i = 0; i < commands.Length(); i++) {
            Napi::Value item = commands[i];
            if (!item.IsObject()) {
                Napi::TypeError::New(env, "Each command must be an object").ThrowAsJavaScriptException();
                return env.Undefined();
            }

            Napi::Object cmdObj = item.As<Napi::Object>();
            if (!cmdObj.Has("typeId") || !cmdObj.Has("ioa") || !cmdObj.Has("value")) {
                Napi::TypeError::New(env, "Each command must have 'typeId' (number), 'ioa' (number), and 'value'").ThrowAsJavaScriptException();
                return env.Undefined();
            }

            int typeId = cmdObj.Get("typeId").As<Napi::Number>().Int32Value();
            int ioa = cmdObj.Get("ioa").As<Napi::Number>().Int32Value();

            // Извлечение дополнительных параметров bselCmd и ql
            bool bselCmd = false; // По умолчанию: исполнение (false), а не выбор (true)
            int ql = 0;           // По умолчанию: без импульса
            if (cmdObj.Has("bselCmd") && cmdObj.Get("bselCmd").IsBoolean()) {
                bselCmd = cmdObj.Get("bselCmd").As<Napi::Boolean>();
            }
            if (cmdObj.Has("ql") && cmdObj.Get("ql").IsNumber()) {
                ql = cmdObj.Get("ql").As<Napi::Number>().Int32Value();
                if (ql < 0 || ql > 31) { // QOC обычно 0-31 согласно IEC 60870-5-101/104
                    Napi::RangeError::New(env, "ql must be between 0 and 31").ThrowAsJavaScriptException();
                    return env.Undefined();
                }
            }

            CS101_ASDU asdu = CS101_ASDU_create(alParams, false, CS101_COT_SPONTANEOUS, 0, this->asduAddress, false, false); // Используем asduAddress

            bool success = false;
            switch (typeId) {
                // Существующие типы мониторинга
                case M_SP_NA_1: {
                    if (!cmdObj.Get("value").IsBoolean()) {
                        Napi::TypeError::New(env, "M_SP_NA_1 requires 'value' as boolean").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    SinglePointInformation sp = SinglePointInformation_create(NULL, ioa, value, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sp);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
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
                    DoublePointInformation dp = DoublePointInformation_create(NULL, ioa, (DoublePointValue)value, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dp);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
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
                    StepPositionInformation st = StepPositionInformation_create(NULL, ioa, value, false, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)st);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
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
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
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
                    MeasuredValueNormalized mn = MeasuredValueNormalized_create(NULL, ioa, value, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)mn);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    MeasuredValueNormalized_destroy(mn);
                    break;
                }
                case M_ME_NB_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_ME_NB_1 requires 'value' as number (-32768 to 32767)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < -32768 || value > 32767) {
                        Napi::RangeError::New(env, "M_ME_NB_1 'value' must be between -32768 and 32767").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    MeasuredValueScaled ms = MeasuredValueScaled_create(NULL, ioa, value, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)ms);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    MeasuredValueScaled_destroy(ms);
                    break;
                }
                case M_ME_NC_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_ME_NC_1 requires 'value' as number").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    MeasuredValueShort mc = MeasuredValueShort_create(NULL, ioa, value, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)mc);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
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
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
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
                    SinglePointWithCP56Time2a sp = SinglePointWithCP56Time2a_create(NULL, ioa, value, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
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
                    DoublePointWithCP56Time2a dp = DoublePointWithCP56Time2a_create(NULL, ioa, (DoublePointValue)value, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dp);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
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
                    StepPositionWithCP56Time2a st = StepPositionWithCP56Time2a_create(NULL, ioa, value, false, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)st);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
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
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -1.0f || value > 1.0f) {
                        Napi::RangeError::New(env, "M_ME_TD_1 'value' must be between -1.0 and 1.0").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    MeasuredValueNormalizedWithCP56Time2a mn = MeasuredValueNormalizedWithCP56Time2a_create(NULL, ioa, value, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)mn);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
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
                    MeasuredValueScaledWithCP56Time2a ms = MeasuredValueScaledWithCP56Time2a_create(NULL, ioa, value, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)ms);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    MeasuredValueScaledWithCP56Time2a_destroy(ms);
                    break;
                }
                case M_ME_TF_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_ME_TF_1 requires 'value' (number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    MeasuredValueShortWithCP56Time2a mc = MeasuredValueShortWithCP56Time2a_create(NULL, ioa, value, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)mc);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
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
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    IntegratedTotalsWithCP56Time2a_destroy(it);
                    BinaryCounterReading_destroy(bcr);
                    break;
                }

                // Новые типы команд управления
                case C_SC_NA_1: { // Одиночная команда
                    if (!cmdObj.Get("value").IsBoolean()) {
                        Napi::TypeError::New(env, "C_SC_NA_1 requires 'value' as boolean").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    SingleCommand sc = SingleCommand_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sc);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SingleCommand_destroy(sc);
                    break;
                }
                case C_DC_NA_1: { // Двойная команда
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
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    DoubleCommand_destroy(dc);
                    break;
                }
                case C_RC_TA_1: { // Команда регулирования шага с меткой времени
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
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    StepCommandWithCP56Time2a_destroy(rc);
                    break;
                }
                case C_SE_TA_1: { // Установка нормализованного значения с меткой времени
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TA_1 requires 'value' (number -1.0 to 1.0) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -1.0f || value > 1.0f) {
                        Napi::RangeError::New(env, "C_SE_TA_1 'value' must be between -1.0 and 1.0").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    SetpointCommandNormalizedWithCP56Time2a se = SetpointCommandNormalizedWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)se);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SetpointCommandNormalizedWithCP56Time2a_destroy(se);
                    break;
                }
                case C_SE_NB_1: { // Установка масштабированного значения
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
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SetpointCommandScaled_destroy(se);
                    break;
                }
                case C_SE_NC_1: { // Установка короткого значения с плавающей точкой
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_NC_1 requires 'value' as number").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    SetpointCommandShort se = SetpointCommandShort_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)se);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SetpointCommandShort_destroy(se);
                    break;
                }
                case C_BO_NA_1: { // Битовая строка 32 бита
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_BO_NA_1 requires 'value' as number (32-bit unsigned integer)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint32_t value = cmdObj.Get("value").As<Napi::Number>().Uint32Value();
                    Bitstring32Command bo = Bitstring32Command_create(NULL, ioa, value);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bo);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    Bitstring32Command_destroy(bo);
                    break;
                }
                case C_SC_TA_1: { // Одиночная команда с меткой времени
                    if (!cmdObj.Get("value").IsBoolean() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SC_TA_1 requires 'value' (boolean) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    SingleCommandWithCP56Time2a sc = SingleCommandWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sc);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SingleCommandWithCP56Time2a_destroy(sc);
                    break;
                }
                case C_DC_TA_1: { // Двойная команда с меткой времени
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
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    DoubleCommandWithCP56Time2a_destroy(dc);
                    break;
                }
                case C_SE_TB_1: { // Установка масштабированного значения с меткой времени
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
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SetpointCommandScaledWithCP56Time2a_destroy(se);
                    break;
                }
                case C_SE_TC_1: { // Установка короткого значения с плавающей точкой с меткой времени
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TC_1 requires 'value' (number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    SetpointCommandShortWithCP56Time2a se = SetpointCommandShortWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)se);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    SetpointCommandShortWithCP56Time2a_destroy(se);
                    break;
                }
                case C_BO_TA_1: { // Битовая строка 32 бита с меткой времени
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_BO_TA_1 requires 'value' (32-bit number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint32_t value = cmdObj.Get("value").As<Napi::Number>().Uint32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    Bitstring32CommandWithCP56Time2a bo = Bitstring32CommandWithCP56Time2a_create(NULL, ioa, value, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bo);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    Bitstring32CommandWithCP56Time2a_destroy(bo);
                    break;
                }
                case C_IC_NA_1: { // Команда опроса (интеррогейшн)
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
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    InterrogationCommand_destroy(ic);
                    break;
                }
                case C_CI_NA_1: { // Команда опроса счетчиков
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
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    CounterInterrogationCommand_destroy(ci);
                    break;
                }
                case C_RD_NA_1: { // Команда чтения
                    // Для C_RD_NA_1 значение не требуется, только IOA
                    ReadCommand rd = ReadCommand_create(NULL, ioa);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)rd);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    ReadCommand_destroy(rd);
                    break;
                }
                case C_CS_NA_1: { // Команда синхронизации времени
                    if (!cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_CS_NA_1 requires 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    ClockSynchronizationCommand cs = ClockSynchronizationCommand_create(NULL, ioa, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)cs);
                    success = IMasterConnection_sendASDU(targetConnection, asdu);
                    ClockSynchronizationCommand_destroy(cs);
                    break;
                }

                default:
                    printf("Unsupported command type: %d, serverID: %s, serverId: %i, clientId: %i\n", typeId, serverID.c_str(), serverId, targetClientId);
                    CS101_ASDU_destroy(asdu);
                    allSuccess = false;
                    continue;
            }

            CS101_ASDU_destroy(asdu);

            if (!success) {
                allSuccess = false;
                printf("Failed to send command: typeId=%d, ioa=%d, bselCmd=%d, ql=%d, serverID: %s, serverId: %i, clientId: %i\n", 
                       typeId, ioa, bselCmd, ql, serverID.c_str(), serverId, targetClientId);
            } else {
                printf("Sent command: typeId=%d, ioa=%d, bselCmd=%d, ql=%d, serverID: %s, serverId: %i, clientId: %i\n", 
                       typeId, ioa, bselCmd, ql, serverID.c_str(), serverId, targetClientId);
            }
        }
        return Napi::Boolean::New(env, allSuccess);
    } catch (const std::exception& e) {
        printf("Exception in SendCommands: %s, serverID: %s, serverId: %i\n", e.what(), serverID.c_str(), serverId);
        Napi::Error::New(env, string("SendCommands failed: ") + e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}

Napi::Value IEC104Server::GetStatus(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(connMutex);
    Napi::Object status = Napi::Object::New(env);
    status.Set("started", Napi::Boolean::New(env, started));
    status.Set("serverId", Napi::Number::New(env, serverId));
    status.Set("serverID", Napi::String::New(env, serverID));
    status.Set("asduAddress", Napi::Number::New(env, asduAddress)); // Добавляем asduAddress в статус

    Napi::Array clients = Napi::Array::New(env, clientConnections.size());
    int index = 0;
    for (const auto& [conn, id] : clientConnections) {
        clients[index++] = Napi::Number::New(env, id);
    }
    status.Set("connectedClients", clients);

    return status;
}

bool IEC104Server::ConnectionRequestHandler(void* parameter, const char* ipAddress) {
    IEC104Server* server = static_cast<IEC104Server*>(parameter);
    printf("Connection request from %s, serverID: %s, serverId: %i\n", ipAddress, server->serverID.c_str(), server->serverId);
    return true; // Accept all connections
}

void IEC104Server::ConnectionEventHandler(void* parameter, IMasterConnection connection, CS104_PeerConnectionEvent event) {
    IEC104Server* server = static_cast<IEC104Server*>(parameter);
    std::string eventStr;
    std::string reason;
    int clientId = -1;

    {
        std::lock_guard<std::mutex> lock(server->connMutex);
        switch (event) {
            case CS104_CON_EVENT_CONNECTION_OPENED:
                eventStr = "connected";
                reason = "client connected";
                clientId = server->clientConnections.size() + 1;
                server->clientConnections[connection] = clientId;
                break;
            case CS104_CON_EVENT_CONNECTION_CLOSED:
                eventStr = "disconnected";
                reason = "client disconnected";
                if (server->clientConnections.find(connection) != server->clientConnections.end()) {
                    clientId = server->clientConnections[connection];
                    server->clientConnections.erase(connection);
                }
                break;
            case CS104_CON_EVENT_ACTIVATED:
                eventStr = "activated";
                reason = "STARTDT confirmed";
                if (server->clientConnections.find(connection) != server->clientConnections.end()) {
                    clientId = server->clientConnections[connection];
                }
                break;
            case CS104_CON_EVENT_DEACTIVATED:
                eventStr = "deactivated";
                reason = "STOPDT confirmed";
                if (server->clientConnections.find(connection) != server->clientConnections.end()) {
                    clientId = server->clientConnections[connection];
                }
                break;
        }
    }

    printf("Connection event: %s, reason: %s, serverID: %s, serverId: %i, clientId: %i\n", eventStr.c_str(), reason.c_str(), server->serverID.c_str(), server->serverId, clientId);

    server->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
        Napi::Object eventObj = Napi::Object::New(env);
        eventObj.Set("serverID", Napi::String::New(env, server->serverID));
        eventObj.Set("type", Napi::String::New(env, "control"));
        eventObj.Set("event", Napi::String::New(env, eventStr));
        eventObj.Set("reason", Napi::String::New(env, reason));
        eventObj.Set("clientId", Napi::Number::New(env, clientId));
        jsCallback.Call({Napi::String::New(env, "data"), eventObj});
    });
}

bool IEC104Server::RawMessageHandler(void* parameter, IMasterConnection connection, CS101_ASDU asdu) {
    IEC104Server* server = static_cast<IEC104Server*>(parameter);
    IEC60870_5_TypeID typeID = CS101_ASDU_getTypeID(asdu);
    int numberOfElements = CS101_ASDU_getNumberOfElements(asdu);
    int receivedAsduAddress = CS101_ASDU_getCA(asdu); // Получаем адрес ASDU из полученного сообщения
    int clientId = -1;

    {
        std::lock_guard<std::mutex> lock(server->connMutex);
        if (server->clientConnections.find(connection) != server->clientConnections.end()) {
            clientId = server->clientConnections[connection];
        } else {
            printf("Received message from unknown client, serverID: %s, serverId: %i\n", server->serverID.c_str(), server->serverId);
            return false;
        }
    }

    try {
        vector<tuple<int, double, uint8_t, uint64_t>> elements;

        switch (typeID) {
            case C_SC_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SingleCommand io = (SingleCommand)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SingleCommand_getState(io) ? 1.0 : 0.0;
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
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
                        elements.emplace_back(ioa, val, quality, timestamp);
                        ClockSynchronizationCommand_destroy(io);
                    }
                }
                break;
            }

            default:
                printf("Received unsupported ASDU type: %s (%i), serverID: %s, serverId: %i, clientId: %i, asduAddress: %d\n", TypeID_toString(typeID), typeID, server->serverID.c_str(), server->serverId, clientId, receivedAsduAddress);
                return false;
        }

        for (const auto& [ioa, val, quality, timestamp] : elements) {
            printf("ASDU type: %s, serverID: %s, serverId: %i, clientId: %i, asduAddress: %d, ioa: %i, value: %f, quality: %u, timestamp: %" PRIu64 ", cnt: %i\n",
                   TypeID_toString(typeID), server->serverID.c_str(), server->serverId, clientId, receivedAsduAddress, ioa, val, quality, timestamp, server->cnt);
        }

        server->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
            Napi::Array jsArray = Napi::Array::New(env, elements.size());
            for (size_t i = 0; i < elements.size(); i++) {
                const auto& [ioa, val, quality, timestamp] = elements[i];
                Napi::Object msg = Napi::Object::New(env);
                msg.Set("serverID", Napi::String::New(env, server->serverID));
                msg.Set("clientId", Napi::Number::New(env, clientId));
                msg.Set("typeId", Napi::Number::New(env, typeID));
                msg.Set("asduAddress", Napi::Number::New(env, receivedAsduAddress)); // Добавляем asduAddress в сообщение
                msg.Set("ioa", Napi::Number::New(env, ioa));
                msg.Set("val", Napi::Number::New(env, val));
                msg.Set("quality", Napi::Number::New(env, quality));
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
        printf("Exception in RawMessageHandler: %s, serverID: %s, serverId: %i, clientId: %i, asduAddress: %d\n", e.what(), server->serverID.c_str(), server->serverId, clientId, receivedAsduAddress); 
        server->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) { 
            Napi::Object eventObj = Napi::Object::New(env); 
            eventObj.Set("serverID", Napi::String::New(env, server->serverID)); 
            eventObj.Set("clientId", Napi::Number::New(env, clientId)); 
            eventObj.Set("type", Napi::String::New(env, "error")); 
            eventObj.Set("reason", Napi::String::New(env, string("Обработка ASDU не удалась: ") + e.what())); 
            jsCallback.Call({Napi::String::New(env, "data"), eventObj}); 
        }); 
        return false; 
    } 
}