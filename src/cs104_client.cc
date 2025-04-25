
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h> // Для mkdir на POSIX
#endif

#include <vector>
#include <tuple>
#include <map>
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fstream>
#include <sstream>
#include <inttypes.h> // Добавляем для PRIu64
#include "cs104_client.h"

using namespace Napi;
using namespace std;

Napi::FunctionReference IEC104Client::constructor;

Napi::Object IEC104Client::Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "IEC104Client", {
        InstanceMethod("connect", &IEC104Client::Connect),
        InstanceMethod("disconnect", &IEC104Client::Disconnect),
        InstanceMethod("sendStartDT", &IEC104Client::SendStartDT),
        InstanceMethod("sendStopDT", &IEC104Client::SendStopDT),
        InstanceMethod("sendCommands", &IEC104Client::SendCommands),
        InstanceMethod("getStatus", &IEC104Client::GetStatus),
        InstanceMethod("requestFileList", &IEC104Client::RequestFileList),
        InstanceMethod("selectFile", &IEC104Client::SelectFile),
        InstanceMethod("openFile", &IEC104Client::OpenFile),
        InstanceMethod("requestFileSegment", &IEC104Client::RequestFileSegment),
        InstanceMethod("confirmFileTransfer", &IEC104Client::ConfirmFileTransfer)
    });

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();
    exports.Set("IEC104Client", func);
    return exports;
}

IEC104Client::IEC104Client(const Napi::CallbackInfo& info) : Napi::ObjectWrap<IEC104Client>(info) {
    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(info.Env(), "Expected a callback function").ThrowAsJavaScriptException();
        return;
    }

    Napi::Function emit = info[0].As<Napi::Function>();
    running = false;
    connected = false;
    activated = false;
    originatorAddress = 1;
    asduAddress = 0;
    usingPrimaryIp = true;
    currentNOF = 0; // Инициализация
    try {
        tsfn = Napi::ThreadSafeFunction::New(
            info.Env(),
            emit,
            "IEC104ClientTSFN",
            0,
            1,
            [](Napi::Env) {}
        );
    } catch (const std::exception& e) {
        printf("Failed to create ThreadSafeFunction: %s\n", e.what());
        Napi::Error::New(info.Env(), string("TSFN creation failed: ") + e.what()).ThrowAsJavaScriptException();
    }
}

IEC104Client::~IEC104Client() {
    std::lock_guard<std::mutex> lock(this->connMutex);
    if (running) {
        running = false;
        if (connected) {
            printf("Destructor closing connection, clientID: %s\n", clientID.c_str());
            CS104_Connection_destroy(connection);
            connected = false;
            activated = false;
        }
        if (_thread.joinable()) {
            _thread.join();
        }
        tsfn.Release();
    }
}

Napi::Value IEC104Client::Connect(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsObject()) {
        Napi::TypeError::New(env, "Expected an object with connection parameters").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Object params = info[0].As<Napi::Object>();

    if (!params.Has("ip") || !params.Get("ip").IsString() ||
        !params.Has("port") || !params.Get("port").IsNumber() ||
        !params.Has("clientID") || !params.Get("clientID").IsString()) {
        Napi::TypeError::New(env, "Object must contain 'ip' (string), 'port' (number), and 'clientID' (string)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string ip = params.Get("ip").As<Napi::String>().Utf8Value();
    int port = params.Get("port").As<Napi::Number>().Int32Value();
    clientID = params.Get("clientID").As<Napi::String>().Utf8Value();

    std::string ipReserve = "";
    if (params.Has("ipReserve") && params.Get("ipReserve").IsString()) {
        ipReserve = params.Get("ipReserve").As<Napi::String>().Utf8Value();
    }

    if (ip.empty() || port <= 0 || clientID.empty()) {
        Napi::Error::New(env, "Invalid 'ip', 'port', or 'clientID'").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    {
        std::lock_guard<std::mutex> lock(this->connMutex);
        if (running) {
            Napi::Error::New(env, "Client already running").ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    int k = 12, w = 8, t0 = 30, t1 = 15, t2 = 10, t3 = 20, reconnectDelay = 5;
    if (params.Has("originatorAddress")) originatorAddress = params.Get("originatorAddress").As<Napi::Number>().Int32Value();
    if (params.Has("asduAddress")) asduAddress = params.Get("asduAddress").As<Napi::Number>().Int32Value();
    if (params.Has("k")) k = params.Get("k").As<Napi::Number>().Int32Value();
    if (params.Has("w")) w = params.Get("w").As<Napi::Number>().Int32Value();
    if (params.Has("t0")) t0 = params.Get("t0").As<Napi::Number>().Int32Value();
    if (params.Has("t1")) t1 = params.Get("t1").As<Napi::Number>().Int32Value();
    if (params.Has("t2")) t2 = params.Get("t2").As<Napi::Number>().Int32Value();
    if (params.Has("t3")) t3 = params.Get("t3").As<Napi::Number>().Int32Value();
    if (params.Has("reconnectDelay")) reconnectDelay = params.Get("reconnectDelay").As<Napi::Number>().Int32Value();

    if (originatorAddress < 0 || originatorAddress > 255 || asduAddress < 0 || asduAddress > 65535 ||
        k <= 0 || w <= 0 || t0 <= 0 || t1 <= 0 || t2 <= 0 || t3 <= 0 || reconnectDelay < 1) {
        Napi::Error::New(env, "Invalid connection parameters").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    try {
        printf("Creating connection to %s:%d, clientID: %s\n", ip.c_str(), port, clientID.c_str());
        fflush(stdout);
        connection = CS104_Connection_create(ip.c_str(), port);
        if (!connection) {
            throw runtime_error("Failed to create connection object");
        }

        CS101_AppLayerParameters alParams = CS104_Connection_getAppLayerParameters(connection);
        alParams->originatorAddress = originatorAddress;
        alParams->sizeOfCA = 2;

        CS104_APCIParameters apciParams = CS104_Connection_getAPCIParameters(connection);
        apciParams->k = k;
        apciParams->w = w;
        apciParams->t0 = t0;
        apciParams->t1 = t1;
        apciParams->t2 = t2;
        apciParams->t3 = t3;

        CS104_Connection_setConnectionHandler(connection, ConnectionHandler, this);
        CS104_Connection_setASDUReceivedHandler(connection, RawMessageHandler, this);

        printf("Connecting with params: originatorAddress=%d, asduAddress=%d, k=%d, w=%d, t0=%d, t1=%d, t2=%d, t3=%d, reconnectDelay=%d, clientID: %s\n",
               originatorAddress, asduAddress, k, w, t0, t1, t2, t3, reconnectDelay, clientID.c_str());
        fflush(stdout);

        running = true;
        usingPrimaryIp = true;
        _thread = std::thread([this, ip, ipReserve, port, k, w, t0, t1, t2, t3, reconnectDelay]() {
            try {
                int retryCount = 0;
                const int failoverDelay = 3; // 3 попытки переподключения
                std::string currentIp = ip;

                while (running) {
                    printf("Attempting to connect to %s:%d (attempt %d), clientID: %s\n", currentIp.c_str(), port, retryCount + 1, clientID.c_str());
                    fflush(stdout);
                    bool connectSuccess = CS104_Connection_connect(connection);
                    {
                        std::lock_guard<std::mutex> lock(this->connMutex);
                        connected = connectSuccess;
                        activated = false;
                    }

                    if (connectSuccess) {
                        printf("Connected successfully to %s:%d, clientID: %s\n", currentIp.c_str(), port, clientID.c_str());
                        fflush(stdout);
                        //CS104_Connection_sendStartDT(connection);
                        retryCount = 0;

                        while (running) {
                            {
                                std::lock_guard<std::mutex> lock(this->connMutex);
                                if (!connected) break;
                            }
                            Thread_sleep(100);

                            if (!usingPrimaryIp && !ipReserve.empty()) {
                                printf("Checking primary IP %s:%d availability, clientID: %s\n", ip.c_str(), port, clientID.c_str());
                                fflush(stdout);
                                CS104_Connection testConn = CS104_Connection_create(ip.c_str(), port);
                                if (testConn && CS104_Connection_connect(testConn)) {
                                    printf("Primary IP %s restored, switching back, clientID: %s\n", ip.c_str(), clientID.c_str());
                                    fflush(stdout);
                                    CS104_Connection_destroy(testConn);

                                    // Переключаемся на основной IP
                                    printf("Closing current connection to %s:%d, clientID: %s\n", currentIp.c_str(), port, clientID.c_str());
                                    fflush(stdout);
                                    CS104_Connection_destroy(connection);
                                    connection = nullptr;

                                    {
                                        std::lock_guard<std::mutex> lock(this->connMutex);
                                        connected = false;
                                        activated = false;
                                    }

                                    printf("Creating new connection to primary IP %s:%d, clientID: %s\n", ip.c_str(), port, clientID.c_str());
                                    fflush(stdout);
                                    connection = CS104_Connection_create(ip.c_str(), port);
                                    if (!connection) {
                                        printf("Failed to create connection object for primary IP %s:%d, clientID: %s\n", ip.c_str(), port, clientID.c_str());
                                        fflush(stdout);
                                        throw runtime_error("Failed to recreate connection object after switching back");
                                    }

                                    CS101_AppLayerParameters alParams = CS104_Connection_getAppLayerParameters(connection);
                                    alParams->originatorAddress = this->originatorAddress;
                                    alParams->sizeOfCA = 2;

                                    CS104_APCIParameters apciParams = CS104_Connection_getAPCIParameters(connection);
                                    apciParams->k = k;
                                    apciParams->w = w;
                                    apciParams->t0 = t0;
                                    apciParams->t1 = t1;
                                    apciParams->t2 = t2;
                                    apciParams->t3 = t3;

                                    CS104_Connection_setConnectionHandler(connection, ConnectionHandler, this);
                                    CS104_Connection_setASDUReceivedHandler(connection, RawMessageHandler, this);

                                    currentIp = ip;
                                    usingPrimaryIp = true;

                                    printf("Attempting to reconnect to primary IP %s:%d, clientID: %s\n", ip.c_str(), port, clientID.c_str());
                                    fflush(stdout);
                                    connectSuccess = CS104_Connection_connect(connection);
                                    {
                                        std::lock_guard<std::mutex> lock(this->connMutex);
                                        connected = connectSuccess;
                                        activated = false;
                                    }
                                    if (connectSuccess) {
                                        printf("Successfully reconnected to primary IP %s:%d, clientID: %s\n", ip.c_str(), port, clientID.c_str());
                                        fflush(stdout);
                                        //CS104_Connection_sendStartDT(connection);
                                    } else {
                                        printf("Failed to reconnect to primary IP %s:%d, reverting to reserve IP, clientID: %s\n", ip.c_str(), port, clientID.c_str());
                                        fflush(stdout);
                                        CS104_Connection_destroy(connection);
                                        connection = CS104_Connection_create(ipReserve.c_str(), port);
                                        if (!connection) {
                                            printf("Failed to create connection object for reserve IP %s:%d, clientID: %s\n", ipReserve.c_str(), port, clientID.c_str());
                                            fflush(stdout);
                                            throw runtime_error("Failed to recreate connection object for reserve IP");
                                        }

                                        alParams = CS104_Connection_getAppLayerParameters(connection);
                                        alParams->originatorAddress = this->originatorAddress;
                                        alParams->sizeOfCA = 2;

                                        apciParams = CS104_Connection_getAPCIParameters(connection);
                                        apciParams->k = k;
                                        apciParams->w = w;
                                        apciParams->t0 = t0;
                                        apciParams->t1 = t1;
                                        apciParams->t2 = t2;
                                        apciParams->t3 = t3;

                                        CS104_Connection_setConnectionHandler(connection, ConnectionHandler, this);
                                        CS104_Connection_setASDUReceivedHandler(connection, RawMessageHandler, this);

                                        currentIp = ipReserve;
                                        usingPrimaryIp = false;
                                        printf("Attempting to reconnect to reserve IP %s:%d, clientID: %s\n", ipReserve.c_str(), port, clientID.c_str());
                                        fflush(stdout);
                                        connectSuccess = CS104_Connection_connect(connection);
                                        if (connectSuccess) {
                                            printf("Reconnected to reserve IP %s:%d, clientID: %s\n", ipReserve.c_str(), port, clientID.c_str());
                                            fflush(stdout);
                                            //CS104_Connection_sendStartDT(connection);
                                            connected = true;
                                        } else {
                                            printf("Failed to reconnect to reserve IP %s:%d, clientID: %s\n", ipReserve.c_str(), port, clientID.c_str());
                                            fflush(stdout);
                                        }
                                    }
                                    break; // Выходим из внутреннего цикла для работы с новым соединением
                                } else {
                                    printf("Primary IP %s still unavailable, clientID: %s\n", ip.c_str(), clientID.c_str());
                                    fflush(stdout);
                                    CS104_Connection_destroy(testConn);
                                    Thread_sleep(5000);
                                }
                            }
                        }

                        std::lock_guard<std::mutex> lock(this->connMutex);
                        if (running && !connected) {
                            printf("Connection lost to %s:%d, clientID: %s\n", currentIp.c_str(), port, clientID.c_str());
                            fflush(stdout);
                            CS104_Connection_destroy(connection);
                            connection = nullptr;

                            if (usingPrimaryIp && !ipReserve.empty() && retryCount >= failoverDelay) {
                                printf("Switching to reserve IP %s, clientID: %s\n", ipReserve.c_str(), clientID.c_str());
                                fflush(stdout);
                                currentIp = ipReserve;
                                usingPrimaryIp = false;
                                retryCount = 0;
                            } else {
                                currentIp = usingPrimaryIp ? ip : ipReserve;
                            }

                            connection = CS104_Connection_create(currentIp.c_str(), port);
                            if (!connection) {
                                throw runtime_error("Failed to recreate connection object");
                            }
                            CS101_AppLayerParameters alParams = CS104_Connection_getAppLayerParameters(connection);
                            alParams->originatorAddress = this->originatorAddress;
                            alParams->sizeOfCA = 2;
                            CS104_APCIParameters apciParams = CS104_Connection_getAPCIParameters(connection);
                            apciParams->k = k;
                            apciParams->w = w;
                            apciParams->t0 = t0;
                            apciParams->t1 = t1;
                            apciParams->t2 = t2;
                            apciParams->t3 = t3;

                            CS104_Connection_setConnectionHandler(connection, ConnectionHandler, this);
                            CS104_Connection_setASDUReceivedHandler(connection, RawMessageHandler, this);
                        } else if (!running && connected) {
                            printf("Thread stopped by client, closing connection, clientID: %s\n", clientID.c_str());
                            fflush(stdout);
                            CS104_Connection_destroy(connection);
                            connected = false;
                            activated = false;
                            return;
                        }
                    } else {
                        printf("Connection failed to %s:%d, clientID: %s\n", currentIp.c_str(), port, clientID.c_str());
                        fflush(stdout);
                        tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
                            Napi::Object eventObj = Napi::Object::New(env);
                            eventObj.Set("clientID", Napi::String::New(env, clientID.c_str()));
                            eventObj.Set("type", Napi::String::New(env, "control"));
                            eventObj.Set("event", Napi::String::New(env, "reconnecting"));
                            eventObj.Set("reason", Napi::String::New(env, string("attempt ") + to_string(retryCount + 1) + " to " + currentIp));
                            std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                            jsCallback.Call(args);
                        });
                    }

                    if (running && !connected) {
                        retryCount++;
                        printf("Reconnection attempt %d failed, retrying in %d seconds, clientID: %s\n", retryCount, reconnectDelay, clientID.c_str());
                        fflush(stdout);
                        Thread_sleep(reconnectDelay * 1000);

                        if (usingPrimaryIp && !ipReserve.empty() && retryCount >= failoverDelay) {
                            printf("Primary IP %s unresponsive, switching to reserve IP %s, clientID: %s\n", ip.c_str(), ipReserve.c_str(), clientID.c_str());
                            fflush(stdout);
                            CS104_Connection_destroy(connection);
                            currentIp = ipReserve;
                            usingPrimaryIp = false;
                            connection = CS104_Connection_create(currentIp.c_str(), port);
                            if (!connection) {
                                throw runtime_error("Failed to recreate connection object for reserve IP");
                            }
                            CS101_AppLayerParameters alParams = CS104_Connection_getAppLayerParameters(connection);
                            alParams->originatorAddress = this->originatorAddress;
                            alParams->sizeOfCA = 2;
                            CS104_APCIParameters apciParams = CS104_Connection_getAPCIParameters(connection);
                            apciParams->k = k;
                            apciParams->w = w;
                            apciParams->t0 = t0;
                            apciParams->t1 = t1;
                            apciParams->t2 = t2;
                            apciParams->t3 = t3;

                            CS104_Connection_setConnectionHandler(connection, ConnectionHandler, this);
                            CS104_Connection_setASDUReceivedHandler(connection, RawMessageHandler, this);
                            retryCount = 0;
                        }
                    }
                }
            } catch (const std::exception& e) {
                printf("Exception in connection thread: %s, clientID: %s\n", e.what(), clientID.c_str());
                fflush(stdout);
                std::lock_guard<std::mutex> lock(this->connMutex);
                running = false;
                if (connected) {
                    CS104_Connection_destroy(connection);
                    connected = false;
                    activated = false;
                }
                tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, clientID.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "control"));
                    eventObj.Set("event", Napi::String::New(env, "error"));
                    eventObj.Set("reason", Napi::String::New(env, string("Thread exception: ") + e.what()));
                    std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                    jsCallback.Call(args);
                });
            }
        });

        return env.Undefined();
    } catch (const std::exception& e) {
        printf("Exception in Connect: %s, clientID: %s\n", e.what(), clientID.c_str());
        Napi::Error::New(env, string("Connect failed: ") + e.what()).ThrowAsJavaScriptException();
        return env.Undefined();
    }
}

Napi::Value IEC104Client::Disconnect(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    {
        std::lock_guard<std::mutex> lock(this->connMutex);
        if (running) {
            running = false;
            if (connected) {
                printf("Disconnect called by client, clientID: %s\n", clientID.c_str());
                CS104_Connection_destroy(connection);
                connected = false;
                activated = false;
            }
        }
    }

    if (_thread.joinable()) {
        _thread.join();
    }

    std::lock_guard<std::mutex> lock(this->connMutex);
    tsfn.Release();

    return env.Undefined();
}

Napi::Value IEC104Client::SendStartDT(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(this->connMutex);
    if (!connected) {
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    try {
        CS104_Connection_sendStartDT(connection);
        return Napi::Boolean::New(env, true);
    } catch (const std::exception& e) {
        printf("Exception in SendStartDT: %s, clientID: %s\n", e.what(), clientID.c_str());
        Napi::Error::New(env, string("SendStartDT failed: ") + e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}

Napi::Value IEC104Client::SendStopDT(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(this->connMutex);
    if (!connected || !activated) {
        Napi::Error::New(env, "Not connected or not activated").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    try {
        CS104_Connection_sendStopDT(connection);
        return Napi::Boolean::New(env, true);
    } catch (const std::exception& e) {
        printf("Exception in SendStopDT: %s, clientID: %s\n", e.what(), clientID.c_str());
        Napi::Error::New(env, string("SendStopDT failed: ") + e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}

Napi::Value IEC104Client::SendCommands(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsArray()) {
        Napi::TypeError::New(env, "Expected commands (array of objects with 'typeId', 'ioa', 'value', and optional fields)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Array commands = info[0].As<Napi::Array>();

    std::lock_guard<std::mutex> lock(this->connMutex);
    if (!connected || !activated) {
        Napi::Error::New(env, "Not connected or not activated").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    try {
        bool allSuccess = true;
        for (uint32_t i = 0; i < commands.Length(); i++) {
            Napi::Value item = commands[i];
            if (!item.IsObject()) {
                Napi::TypeError::New(env, "Each command must be an object").ThrowAsJavaScriptException();
                return env.Undefined();
            }

            Napi::Object cmdObj = item.As<Napi::Object>();
            if (!cmdObj.Has("typeId") || !cmdObj.Has("ioa") || !cmdObj.Has("asdu") || !cmdObj.Has("value")) {
                Napi::TypeError::New(env, "Each command must have 'typeId' (number), 'ioa' (number), 'asdu' (number), and 'value'").ThrowAsJavaScriptException();
                return env.Undefined();
            }

            int typeId = cmdObj.Get("typeId").As<Napi::Number>().Int32Value();
            int ioa = cmdObj.Get("ioa").As<Napi::Number>().Int32Value();
            int asduAddress = cmdObj.Get("asdu").As<Napi::Number>().Int32Value();

            bool bselCmd = cmdObj.Has("bselCmd") && cmdObj.Get("bselCmd").IsBoolean() ? cmdObj.Get("bselCmd").As<Napi::Boolean>() : false;
            int ql = cmdObj.Has("ql") && cmdObj.Get("ql").IsNumber() ? cmdObj.Get("ql").As<Napi::Number>().Int32Value() : 0;
            if (ql < 0 || ql > 31) {
                Napi::RangeError::New(env, "ql must be between 0 and 31").ThrowAsJavaScriptException();
                return env.Undefined();
            }

            CS101_ASDU asdu = CS101_ASDU_create(CS104_Connection_getAppLayerParameters(connection), false, CS101_COT_ACTIVATION, originatorAddress, asduAddress, false, false);

            bool success = false;
            switch (typeId) {
                case C_SC_NA_1: {
                    if (!cmdObj.Get("value").IsBoolean()) {
                        Napi::TypeError::New(env, "C_SC_NA_1 requires 'value' as boolean").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    SingleCommand sc = SingleCommand_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sc);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    SingleCommand_destroy(sc);
                    break;
                }

                case C_DC_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_DC_NA_1 requires 'value' as number (0-3)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "C_DC_NA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    DoubleCommand dc = DoubleCommand_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dc);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    DoubleCommand_destroy(dc);
                    break;
                }

                case C_RC_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_RC_NA_1 requires 'value' as number (0-3)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "C_RC_NA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    StepCommand rc = StepCommand_create(NULL, ioa, (StepCommandValue)value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)rc);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    StepCommand_destroy(rc);
                    break;
                }

                case C_SE_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_NA_1 requires 'value' as number (-1.0 to 1.0)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    if (value < -1.0f || value > 1.0f) {
                        Napi::RangeError::New(env, "C_SE_NA_1 'value' must be between -1.0 and 1.0").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    SetpointCommandNormalized scn = SetpointCommandNormalized_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scn);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    SetpointCommandNormalized_destroy(scn);
                    break;
                }

                case C_SE_NB_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_NB_1 requires 'value' as number (-32768 to 32767)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < -32768 || value > 32767) {
                        Napi::RangeError::New(env, "C_SE_NB_1 'value' must be between -32768 and 32767").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    SetpointCommandScaled scs = SetpointCommandScaled_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scs);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    SetpointCommandScaled_destroy(scs);
                    break;
                }

                case C_SE_NC_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_NC_1 requires 'value' as number").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    SetpointCommandShort scsf = SetpointCommandShort_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scsf);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    SetpointCommandShort_destroy(scsf);
                    break;
                }

                case C_BO_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_BO_NA_1 requires 'value' as number (32-bit unsigned integer)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    uint32_t value = cmdObj.Get("value").As<Napi::Number>().Uint32Value();
                    Bitstring32Command bc = Bitstring32Command_create(NULL, ioa, value);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bc);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    Bitstring32Command_destroy(bc);
                    break;
                }

                case C_SC_TA_1: {
                    if (!cmdObj.Get("value").IsBoolean() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SC_TA_1 requires 'value' (boolean) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    SingleCommandWithCP56Time2a sc = SingleCommandWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sc);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    SingleCommandWithCP56Time2a_destroy(sc);
                    break;
                }

                case C_DC_TA_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_DC_TA_1 requires 'value' (number 0-3) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "C_DC_TA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    DoubleCommandWithCP56Time2a dc = DoubleCommandWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dc);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    DoubleCommandWithCP56Time2a_destroy(dc);
                    break;
                }

                case C_RC_TA_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_RC_TA_1 requires 'value' (number 0-3) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "C_RC_TA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    StepCommandWithCP56Time2a rc = StepCommandWithCP56Time2a_create(NULL, ioa, (StepCommandValue)value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)rc);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    StepCommandWithCP56Time2a_destroy(rc);
                    break;
                }

                case C_SE_TA_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TA_1 requires 'value' (number -1.0 to 1.0) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -1.0f || value > 1.0f) {
                        Napi::RangeError::New(env, "C_SE_TA_1 'value' must be between -1.0 and 1.0").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    SetpointCommandNormalizedWithCP56Time2a scn = SetpointCommandNormalizedWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scn);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    SetpointCommandNormalizedWithCP56Time2a_destroy(scn);
                    break;
                }

                case C_SE_TB_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TB_1 requires 'value' (number -32768 to 32767) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -32768 || value > 32767) {
                        Napi::RangeError::New(env, "C_SE_TB_1 'value' must be between -32768 and 32767").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    SetpointCommandScaledWithCP56Time2a scs = SetpointCommandScaledWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scs);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    SetpointCommandScaledWithCP56Time2a_destroy(scs);
                    break;
                }

                case C_SE_TC_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TC_1 requires 'value' (number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    SetpointCommandShortWithCP56Time2a scsf = SetpointCommandShortWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scsf);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    SetpointCommandShortWithCP56Time2a_destroy(scsf);
                    break;
                }

                case C_BO_TA_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_BO_TA_1 requires 'value' (32-bit number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    uint32_t value = cmdObj.Get("value").As<Napi::Number>().Uint32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    Bitstring32CommandWithCP56Time2a bc = Bitstring32CommandWithCP56Time2a_create(NULL, ioa, value, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bc);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    Bitstring32CommandWithCP56Time2a_destroy(bc);
                    break;
                }

                case C_IC_NA_1: {
                    CS101_ASDU_setTypeID(asdu, C_IC_NA_1);
                    CS101_ASDU_setCOT(asdu, CS101_COT_ACTIVATION);
                    InformationObject io = (InformationObject)InterrogationCommand_create(NULL, ioa, cmdObj.Get("value").As<Napi::Number>().Uint32Value());
                    CS101_ASDU_addInformationObject(asdu, io);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    InformationObject_destroy(io);
                    break;
                }

                case C_CI_NA_1: {
                    CS101_ASDU_setTypeID(asdu, C_CI_NA_1);
                    CS101_ASDU_setCOT(asdu, CS101_COT_REQUEST);
                    InformationObject io = (InformationObject)CounterInterrogationCommand_create(NULL, ioa, cmdObj.Get("value").As<Napi::Number>().Uint32Value());
                    CS101_ASDU_addInformationObject(asdu, io);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    InformationObject_destroy(io);
                    break;
                }

                case C_RD_NA_1: {
                    CS101_ASDU_setTypeID(asdu, C_RD_NA_1);
                    CS101_ASDU_setCOT(asdu, CS101_COT_REQUEST);
                    InformationObject io = (InformationObject)ReadCommand_create(NULL, ioa);
                    CS101_ASDU_addInformationObject(asdu, io);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    InformationObject_destroy(io);
                    break;
                }

                case C_CS_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_CS_NA_1 requires 'value' as number (timestamp)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    uint64_t value = cmdObj.Get("value").As<Napi::Number>().Int64Value();
                    CS101_ASDU_setTypeID(asdu, C_CS_NA_1);
                    CS101_ASDU_setCOT(asdu, CS101_COT_ACTIVATION);
                    InformationObject io = (InformationObject)ClockSynchronizationCommand_create(NULL, ioa, CP56Time2a_createFromMsTimestamp(NULL, value));
                    CS101_ASDU_addInformationObject(asdu, io);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    InformationObject_destroy(io);
                    break;
                }

                case 122: { // F_SC_NA_1
                    CS101_CauseOfTransmission cot = cmdObj.Has("cot") ? (CS101_CauseOfTransmission)cmdObj.Get("cot").As<Napi::Number>().Int32Value() : CS101_COT_ACTIVATION;
                    int scq = cmdObj.Has("scq") ? cmdObj.Get("scq").As<Napi::Number>().Int32Value() : 0;
                    CS101_ASDU_setTypeID(asdu, F_SC_NA_1);
                    
                        CS101_ASDU_setCOT(asdu, cot);
               

                    if (scq == 0) { 
                        uint16_t value = cmdObj.Has("value") ? cmdObj.Get("value").As<Napi::Number>().Uint32Value() : 4;
                        uint8_t payload[] = {
                            (uint8_t)(ioa & 0xff), (uint8_t)((ioa >> 8) & 0xff), (uint8_t)((ioa >> 16) & 0xff), // IOA
                            (uint8_t)scq, // SCQ
                            (uint8_t)(value & 0xff), (uint8_t)((value >> 8) & 0xff) // NOF
                        };
                        CS101_ASDU_setNumberOfElements(asdu, 1); // Устанавливаем NumIx=1
                        CS101_ASDU_addPayload(asdu, payload, sizeof(payload));
                        printf("Sending F_SC_NA_1: IOA=%d, SCQ=%d, NOF=%u, COT=%d, clientID: %s\n",
                            ioa, scq, value, cot, clientID.c_str());
                        success = CS104_Connection_sendASDU(connection, asdu);
                    } 
                    else if (scq == 1 || scq == 2 || scq == 6) {
                        std::string fileName = cmdObj.Get("value").As<Napi::String>().Utf8Value();
                        uint16_t nof;
                        if (sscanf(fileName.c_str(), "%hu", &nof) != 1) {
                            CS101_ASDU_destroy(asdu);
                            Napi::TypeError::New(env, "Invalid file name format, expected decimal (e.g., '1710')").ThrowAsJavaScriptException();
                            return env.Undefined();
                        }
                        uint8_t lof = (scq == 1 ? 0 : 1);
                        uint8_t foq = scq;
                        uint8_t payload[] = {
                            (uint8_t)(ioa & 0xff), (uint8_t)((ioa >> 8) & 0xff), (uint8_t)((ioa >> 16) & 0xff), // IOA
                            (uint8_t)(nof & 0xff), (uint8_t)((nof >> 8) & 0xff), // NOF
                            lof, // LOF
                            foq // FOQ
                        };
                        CS101_ASDU_setNumberOfElements(asdu, 1); // Устанавливаем NumIx=1
                        CS101_ASDU_addPayload(asdu, payload, sizeof(payload));
                        printf("F_SC_NA_1 ASDU: TypeID=%d, COT=%d, OA=%d, ASDUAddr=%d, NumIx=%d, Payload: ", 
                            CS101_ASDU_getTypeID(asdu), cot, 0, asduAddress, CS101_ASDU_getNumberOfElements(asdu));
                        for (int i = 0; i < sizeof(payload); i++) {
                            printf("%02x ", payload[i]);
                        }
                        printf("\n");
                        success = CS104_Connection_sendASDU(connection, asdu);
                    }
                    else {
                        CS101_ASDU_destroy(asdu);
                        Napi::TypeError::New(env, "Unsupported SCQ for F_SC_NA_1").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    break;
                }
                case 124: { // F_AF_NA_1
                    CS101_CauseOfTransmission cot = cmdObj.Has("cot") ? (CS101_CauseOfTransmission)cmdObj.Get("cot").As<Napi::Number>().Int32Value() : CS101_COT_ACTIVATION;
                    std::string valueStr = cmdObj.Get("value").As<Napi::String>().Utf8Value();
                    uint16_t nof;
                    if (sscanf(valueStr.c_str(), "%hu", &nof) != 1) {
                        CS101_ASDU_destroy(asdu);
                        Napi::TypeError::New(env, "Invalid NOF format for F_AF_NA_1, expected decimal (e.g., '1710')").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    CS101_ASDU_setTypeID(asdu, F_AF_NA_1);
                    CS101_ASDU_setCOT(asdu, cot);
                    uint8_t payload[] = {
                        (uint8_t)(ioa & 0xff), (uint8_t)((ioa >> 8) & 0xff), (uint8_t)((ioa >> 16) & 0xff), // IOA
                        (uint8_t)(nof & 0xff), (uint8_t)((nof >> 8) & 0xff), // NOF
                        0x01, // LOS = 1
                        0x01  // CHKS = 1
                    };
                    CS101_ASDU_setNumberOfElements(asdu, 1);
                    CS101_ASDU_addPayload(asdu, payload, sizeof(payload));
                    printf("Sending F_AF_NA_1: IOA=%d, NOF=%u, LOS=1, CHKS=1, COT=%d, clientID: %s\n",
                        ioa, nof, cot, clientID.c_str());
                    success = CS104_Connection_sendASDU(connection, asdu);
                    break;
                }
            }

            CS101_ASDU_destroy(asdu);

            if (!success) {
                allSuccess = false;
                printf("Failed to send command: typeId=%d, ioa=%d, clientID: %s, isPrimaryIP=%d\n", typeId, ioa, clientID.c_str(), usingPrimaryIp);
            } else {
                printf("Sent command: typeId=%d, ioa=%d, bselCmd=%d, ql=%d, clientID: %s, isPrimaryIP=%d\n", typeId, ioa, bselCmd, ql, clientID.c_str(), usingPrimaryIp);
            }
        } // Закрытие for
        return Napi::Boolean::New(env, allSuccess);
    } catch (const std::exception& e) {
        printf("Exception in SendCommands: %s, clientID: %s, isPrimaryIP=%d\n", e.what(), clientID.c_str(), usingPrimaryIp);
        Napi::Error::New(env, string("SendCommands failed: ") + e.what()).ThrowAsJavaScriptException();

        // Добавляем isPrimaryIP в объект события
        tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object eventObj = Napi::Object::New(env);
            eventObj.Set("clientID", Napi::String::New(env, clientID.c_str()));
            eventObj.Set("type", Napi::String::New(env, "error"));
            eventObj.Set("reason", Napi::String::New(env, string("SendCommands failed: ") + e.what()));
            eventObj.Set("isPrimaryIP", Napi::Boolean::New(env, usingPrimaryIp)); // Добавляем isPrimaryIP
            std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
            jsCallback.Call(args);
        });

        return Napi::Boolean::New(env, false);
    }
}

Napi::Value IEC104Client::RequestFileList(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    std::lock_guard<std::mutex> lock(this->connMutex);
    if (!connected || !activated) {
        Napi::Error::New(env, "Not connected or not activated").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    try {
        fileList.clear();

        // Запрос директории с COT=5, ожидаем ответ F_DR (TypeID=126, COT=5)
        CS101_ASDU asdu = CS101_ASDU_create(CS104_Connection_getAppLayerParameters(connection), false, CS101_COT_REQUEST, asduAddress, originatorAddress, false, false);
        CS101_ASDU_setTypeID(asdu, F_SC_NA_1); // Используем F_SC_NA_1 для запроса
        uint8_t payload[] = {0x00, 0x00, 0x00,  // IOA = 0
                    0x00,  // SCQ = 0
                    0x04, 0x00};  // NOF = 4 (little-endian)
        CS101_ASDU_addPayload(asdu, payload, sizeof(payload));
        printf("Sending F_SC_NA_1 for file list: payload=");
        for (int i = 0; i < sizeof(payload); i++) printf("%02x ", payload[i]);
        printf("\n");
        bool success = CS104_Connection_sendASDU(connection, asdu);
        CS101_ASDU_destroy(asdu);

        if (!success) {
            printf("Failed to send file list request, clientID: %s\n", clientID.c_str());
            Napi::Error::New(env, "Failed to send file list request").ThrowAsJavaScriptException();
            return Napi::Boolean::New(env, false);
        }

        printf("File list request sent (F_SC_NA_1, COT=5, NOF=4), expecting F_DR (126, COT=5) response, clientID: %s\n", clientID.c_str());
        return Napi::Boolean::New(env, true);
    } catch (const std::exception& e) {
        printf("Exception in RequestFileList: %s, clientID: %s\n", e.what(), clientID.c_str());
        Napi::Error::New(env, string("RequestFileList failed: ") + e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}

Napi::Value IEC104Client::SelectFile(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected file name (string, e.g., '0169')").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string fileName = info[0].As<Napi::String>().Utf8Value();
    std::lock_guard<std::mutex> lock(this->connMutex);
    if (!connected || !activated) {
        Napi::Error::New(env, "Not connected or not activated").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    try {
        // Конвертируем имя файла (например, "0169") в oscnum
        uint16_t oscnum;
        if (sscanf(fileName.c_str(), "%hx", &oscnum) != 1) {
            Napi::TypeError::New(env, "Invalid file name format, expected hexadecimal (e.g., '0169')").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        // Формируем NOF: oscnum * 10 + filetype (filetype=0 для осциллограмм)
        uint16_t nof = oscnum * 10;

        // F_SC_NA_1 с COT=13, SCQ=1 (выбор файла)
        CS101_ASDU asdu = CS101_ASDU_create(CS104_Connection_getAppLayerParameters(connection), false, CS101_COT_ACTIVATION, asduAddress, originatorAddress, false, false);
        CS101_ASDU_setTypeID(asdu, F_SC_NA_1);
        uint8_t payload[] = {
            0x00, 0x00, 0x00, // IOA = 0
            0x01, // SCQ = 1 (выбор файла)
            (uint8_t)(nof & 0xff), (uint8_t)((nof >> 8) & 0xff) // NOF (2 байта, little-endian)
        };
        CS101_ASDU_addPayload(asdu, payload, sizeof(payload));

        bool success = CS104_Connection_sendASDU(connection, asdu);
        CS101_ASDU_destroy(asdu);

        if (!success) {
            printf("Failed to select file %s (NOF=%u, oscnum=%u), clientID: %s\n", fileName.c_str(), nof, oscnum, clientID.c_str());
            Napi::Error::New(env, "Failed to select file").ThrowAsJavaScriptException();
            return Napi::Boolean::New(env, false);
        }

        printf("File selected (F_SC_NA_1, COT=13, SCQ=1, NOF=%u, oscnum=%u), clientID: %s\n", nof, oscnum, clientID.c_str());
        return Napi::Boolean::New(env, true);
    } catch (const std::exception& e) {
        printf("Exception in SelectFile: %s, clientID: %s\n", e.what(), clientID.c_str());
        Napi::Error::New(env, string("SelectFile failed: ") + e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}

Napi::Value IEC104Client::OpenFile(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    std::lock_guard<std::mutex> lock(this->connMutex);
    if (!connected || !activated) {
        Napi::Error::New(env, "Not connected or not activated").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    try {
        // F_SC_NA_1 с COT=13, SCQ=2 (открытие файла)
        CS101_ASDU asdu = CS101_ASDU_create(CS104_Connection_getAppLayerParameters(connection), false, CS101_COT_ACTIVATION, asduAddress, originatorAddress, false, false);
        CS101_ASDU_setTypeID(asdu, F_SC_NA_1);
        uint8_t payload[] = {0x00, 0x00, 0x00,  // IOA = 0
                            0x02};             // SCQ = 2 (открытие файла)
        CS101_ASDU_addPayload(asdu, payload, sizeof(payload));

        bool success = CS104_Connection_sendASDU(connection, asdu);
        CS101_ASDU_destroy(asdu);

        if (!success) {
            printf("Failed to open file, clientID: %s\n", clientID.c_str());
            Napi::Error::New(env, "Failed to open file").ThrowAsJavaScriptException();
            return Napi::Boolean::New(env, false);
        }

        printf("File opened (F_SC_NA_1, COT=13, SCQ=2), clientID: %s\n", clientID.c_str());
        return Napi::Boolean::New(env, true);
    } catch (const std::exception& e) {
        printf("Exception in OpenFile: %s, clientID: %s\n", e.what(), clientID.c_str());
        Napi::Error::New(env, string("OpenFile failed: ") + e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}

Napi::Value IEC104Client::RequestFileSegment(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    std::lock_guard<std::mutex> lock(this->connMutex);
    if (!connected || !activated) {
        Napi::Error::New(env, "Not connected or not activated").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    try {
        // F_SC_NA_1 с COT=13, SCQ=6 (запрос секции 1)
        CS101_ASDU asdu = CS101_ASDU_create(CS104_Connection_getAppLayerParameters(connection), false, CS101_COT_ACTIVATION, asduAddress, originatorAddress, false, false);
        CS101_ASDU_setTypeID(asdu, F_SC_NA_1);
        uint8_t payload[] = {0x00, 0x00, 0x00,  // IOA = 0
                            0x06};             // SCQ = 6 (запрос передачи секции)
        CS101_ASDU_addPayload(asdu, payload, sizeof(payload));

        bool success = CS104_Connection_sendASDU(connection, asdu);
        CS101_ASDU_destroy(asdu);

        if (!success) {
            printf("Failed to request file segment, clientID: %s\n", clientID.c_str());
            Napi::Error::New(env, "Failed to request file segment").ThrowAsJavaScriptException();
            return Napi::Boolean::New(env, false);
        }

        printf("File segment requested (F_SC_NA_1, COT=13, SCQ=6), clientID: %s\n", clientID.c_str());
        return Napi::Boolean::New(env, true);
    } catch (const std::exception& e) {
        printf("Exception in RequestFileSegment: %s, clientID: %s\n", e.what(), clientID.c_str());
        Napi::Error::New(env, string("RequestFileSegment failed: ") + e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}

Napi::Value IEC104Client::ConfirmFileTransfer(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsNumber()) {
        Napi::TypeError::New(env, "Expected IOA (number)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    int ioa = info[0].As<Napi::Number>().Int32Value();
    std::lock_guard<std::mutex> lock(this->connMutex);
    if (!connected || !activated) {
        Napi::Error::New(env, "Not connected or not activated").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    try {
        // F_AF_NA_1 для подтверждения передачи
        CS101_ASDU asdu = CS101_ASDU_create(CS104_Connection_getAppLayerParameters(connection), false, CS101_COT_ACTIVATION, asduAddress, originatorAddress, false, false);
        CS101_ASDU_setTypeID(asdu, F_AF_NA_1);
        InformationObject io = (InformationObject)FileACK_create(NULL, ioa, 1, 0, 0); // NOS=1, подтверждение
        CS101_ASDU_addInformationObject(asdu, io);
        InformationObject_destroy(io);

        bool success = CS104_Connection_sendASDU(connection, asdu);
        CS101_ASDU_destroy(asdu);

        if (!success) {
            printf("Failed to confirm file transfer for IOA=%d, clientID: %s\n", ioa, clientID.c_str());
            Napi::Error::New(env, "Failed to confirm file transfer").ThrowAsJavaScriptException();
            return Napi::Boolean::New(env, false);
        }

        printf("File transfer confirmed (F_AF_NA_1, IOA=%d), clientID: %s\n", ioa, clientID.c_str());
        return Napi::Boolean::New(env, true);
    } catch (const std::exception& e) {
        printf("Exception in ConfirmFileTransfer: %s, clientID: %s\n", e.what(), clientID.c_str());
        Napi::Error::New(env, string("ConfirmFileTransfer failed: ") + e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}


void IEC104Client::ConnectionHandler(void* parameter, CS104_Connection con, CS104_ConnectionEvent event) {
    IEC104Client* client = static_cast<IEC104Client*>(parameter);
    std::string eventStr;
    std::string reason;

    {
        std::lock_guard<std::mutex> lock(client->connMutex);
        switch (event) {
            case CS104_CONNECTION_FAILED:
                eventStr = "failed";
                reason = "connection attempt failed";
                client->connected = false;
                client->activated = false;
                break;
            case CS104_CONNECTION_OPENED:
                eventStr = "opened";
                reason = "connection established";
                client->connected = true;
                break;
            case CS104_CONNECTION_CLOSED:
                eventStr = "closed";
                if (client->running) {
                    reason = "server closed connection or timeout";
                } else {
                    reason = "client closed connection";
                }
                client->connected = false;
                client->activated = false;
                break;
            case CS104_CONNECTION_STARTDT_CON_RECEIVED:
                eventStr = "activated";
                reason = "STARTDT confirmed";
                client->activated = true;
                break;
            case CS104_CONNECTION_STOPDT_CON_RECEIVED:
                eventStr = "deactivated";
                reason = "STOPDT confirmed";
                client->activated = false;
                break;
        }
    }

    printf("Connection event: %s, reason: %s, clientID: %s\n", eventStr.c_str(), reason.c_str(), client->clientID.c_str());

    client->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
        Napi::Object eventObj = Napi::Object::New(env);
        eventObj.Set("clientID", Napi::String::New(env, client->clientID.c_str()));
        eventObj.Set("type", Napi::String::New(env, "control"));
        eventObj.Set("event", Napi::String::New(env, eventStr));
        eventObj.Set("reason", Napi::String::New(env, reason));
        eventObj.Set("isPrimaryIP", Napi::Boolean::New(env, client->usingPrimaryIp));
        std::vector<napi_value> args = {Napi::String::New(env, "conn"), eventObj};
        jsCallback.Call(args);
    });
}
       
    bool IEC104Client::RawMessageHandler(void* parameter, int address, CS101_ASDU asdu) {
        IEC104Client* client = static_cast<IEC104Client*>(parameter);
        IEC60870_5_TypeID typeID = CS101_ASDU_getTypeID(asdu);
        CS101_CauseOfTransmission cot = CS101_ASDU_getCOT(asdu);
        int numberOfElements = CS101_ASDU_getNumberOfElements(asdu);
        int receivedAsduAddress = CS101_ASDU_getCA(asdu);
        bool isPrimaryIP = client->usingPrimaryIp;

        printf("Received ASDU: TypeID=%d, COT=%d, ASDUAddr=%d, Elements=%d, clientID: %s\n", 
            typeID, cot, receivedAsduAddress, numberOfElements, client->clientID.c_str());

        try {
            // Логика для данных мониторинга (M_ types)
            std::vector<std::tuple<int, double, uint8_t, uint64_t>> elements;

            switch (typeID) {
                // Обработка данных мониторинга
                case M_SP_NA_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        SinglePointInformation io = (SinglePointInformation)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = SinglePointInformation_getValue(io) ? 1.0 : 0.0;
                            uint8_t quality = SinglePointInformation_getQuality(io);
                            uint64_t timestamp = 0;
                            elements.emplace_back(ioa, val, quality, timestamp);
                            SinglePointInformation_destroy(io);
                        }
                    }
                    break;

                case M_DP_NA_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        DoublePointInformation io = (DoublePointInformation)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = static_cast<double>(DoublePointInformation_getValue(io));
                            uint8_t quality = DoublePointInformation_getQuality(io);
                            uint64_t timestamp = 0;
                            elements.emplace_back(ioa, val, quality, timestamp);
                            DoublePointInformation_destroy(io);
                        }
                    }
                    break;

                case M_ST_NA_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        StepPositionInformation io = (StepPositionInformation)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = static_cast<double>(StepPositionInformation_getValue(io));
                            uint8_t quality = StepPositionInformation_getQuality(io);
                            uint64_t timestamp = 0;
                            elements.emplace_back(ioa, val, quality, timestamp);
                            StepPositionInformation_destroy(io);
                        }
                    }
                    break;

                case M_BO_NA_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        BitString32 io = (BitString32)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = static_cast<double>(BitString32_getValue(io));
                            uint8_t quality = BitString32_getQuality(io);
                            uint64_t timestamp = 0;
                            elements.emplace_back(ioa, val, quality, timestamp);
                            BitString32_destroy(io);
                        }
                    }
                    break;

                case M_ME_NA_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        MeasuredValueNormalized io = (MeasuredValueNormalized)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = MeasuredValueNormalized_getValue(io);
                            uint8_t quality = MeasuredValueNormalized_getQuality(io);
                            uint64_t timestamp = 0;
                            elements.emplace_back(ioa, val, quality, timestamp);
                            MeasuredValueNormalized_destroy(io);
                        }
                    }
                    break;

                case M_ME_NB_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        MeasuredValueScaled io = (MeasuredValueScaled)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = MeasuredValueScaled_getValue(io);
                            uint8_t quality = MeasuredValueScaled_getQuality(io);
                            uint64_t timestamp = 0;
                            elements.emplace_back(ioa, val, quality, timestamp);
                            MeasuredValueScaled_destroy(io);
                        }
                    }
                    break;

                case M_ME_NC_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        MeasuredValueShort io = (MeasuredValueShort)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = MeasuredValueShort_getValue(io);
                            uint8_t quality = MeasuredValueShort_getQuality(io);
                            uint64_t timestamp = 0;
                            elements.emplace_back(ioa, val, quality, timestamp);
                            MeasuredValueShort_destroy(io);
                        }
                    }
                    break;

                case M_IT_NA_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        IntegratedTotals io = (IntegratedTotals)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = BinaryCounterReading_getValue(IntegratedTotals_getBCR(io));
                            uint8_t quality = IEC60870_QUALITY_GOOD;
                            uint64_t timestamp = 0;
                            elements.emplace_back(ioa, val, quality, timestamp);
                            IntegratedTotals_destroy(io);
                        }
                    }
                    break;

                case M_SP_TB_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        SinglePointWithCP56Time2a io = (SinglePointWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = SinglePointInformation_getValue((SinglePointInformation)io) ? 1.0 : 0.0;
                            uint8_t quality = SinglePointInformation_getQuality((SinglePointInformation)io);
                            uint64_t timestamp = CP56Time2a_toMsTimestamp(SinglePointWithCP56Time2a_getTimestamp(io));
                            elements.emplace_back(ioa, val, quality, timestamp);
                            SinglePointWithCP56Time2a_destroy(io);
                        }
                    }
                    break;

                case M_DP_TB_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        DoublePointWithCP56Time2a io = (DoublePointWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = static_cast<double>(DoublePointInformation_getValue((DoublePointInformation)io));
                            uint8_t quality = DoublePointInformation_getQuality((DoublePointInformation)io);
                            uint64_t timestamp = CP56Time2a_toMsTimestamp(DoublePointWithCP56Time2a_getTimestamp(io));
                            elements.emplace_back(ioa, val, quality, timestamp);
                            DoublePointWithCP56Time2a_destroy(io);
                        }
                    }
                    break;

                case M_ST_TB_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        StepPositionWithCP56Time2a io = (StepPositionWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = static_cast<double>(StepPositionInformation_getValue((StepPositionInformation)io));
                            uint8_t quality = StepPositionInformation_getQuality((StepPositionInformation)io);
                            uint64_t timestamp = CP56Time2a_toMsTimestamp(StepPositionWithCP56Time2a_getTimestamp(io));
                            elements.emplace_back(ioa, val, quality, timestamp);
                            StepPositionWithCP56Time2a_destroy(io);
                        }
                    }
                    break;

                case M_BO_TB_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        Bitstring32WithCP56Time2a io = (Bitstring32WithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = static_cast<double>(BitString32_getValue((BitString32)io));
                            uint8_t quality = BitString32_getQuality((BitString32)io);
                            uint64_t timestamp = CP56Time2a_toMsTimestamp(Bitstring32WithCP56Time2a_getTimestamp(io));
                            elements.emplace_back(ioa, val, quality, timestamp);
                            Bitstring32WithCP56Time2a_destroy(io);
                        }
                    }
                    break;

                case M_ME_TD_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        MeasuredValueNormalizedWithCP56Time2a io = (MeasuredValueNormalizedWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = MeasuredValueNormalized_getValue((MeasuredValueNormalized)io);
                            uint8_t quality = MeasuredValueNormalized_getQuality((MeasuredValueNormalized)io);
                            uint64_t timestamp = CP56Time2a_toMsTimestamp(MeasuredValueNormalizedWithCP56Time2a_getTimestamp(io));
                            elements.emplace_back(ioa, val, quality, timestamp);
                            MeasuredValueNormalizedWithCP56Time2a_destroy(io);
                        }
                    }
                    break;

                case M_ME_TE_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        MeasuredValueScaledWithCP56Time2a io = (MeasuredValueScaledWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = MeasuredValueScaled_getValue((MeasuredValueScaled)io);
                            uint8_t quality = MeasuredValueScaled_getQuality((MeasuredValueScaled)io);
                            uint64_t timestamp = CP56Time2a_toMsTimestamp(MeasuredValueScaledWithCP56Time2a_getTimestamp(io));
                            elements.emplace_back(ioa, val, quality, timestamp);
                            MeasuredValueScaledWithCP56Time2a_destroy(io);
                        }
                    }
                    break;

                case M_ME_TF_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        MeasuredValueShortWithCP56Time2a io = (MeasuredValueShortWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = MeasuredValueShort_getValue((MeasuredValueShort)io);
                            uint8_t quality = MeasuredValueShort_getQuality((MeasuredValueShort)io);
                            uint64_t timestamp = CP56Time2a_toMsTimestamp(MeasuredValueShortWithCP56Time2a_getTimestamp(io));
                            elements.emplace_back(ioa, val, quality, timestamp);
                            MeasuredValueShortWithCP56Time2a_destroy(io);
                        }
                    }
                    break;

                case M_IT_TB_1:
                    for (int i = 0; i < numberOfElements; i++) {
                        IntegratedTotalsWithCP56Time2a io = (IntegratedTotalsWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            double val = BinaryCounterReading_getValue(IntegratedTotals_getBCR((IntegratedTotals)io));
                            uint8_t quality = IEC60870_QUALITY_GOOD;
                            uint64_t timestamp = CP56Time2a_toMsTimestamp(IntegratedTotalsWithCP56Time2a_getTimestamp(io));
                            elements.emplace_back(ioa, val, quality, timestamp);
                            IntegratedTotalsWithCP56Time2a_destroy(io);
                        }
                    }
                    break;

            
    
    case F_DR_TA_1: {
    uint8_t* rawData = CS101_ASDU_getPayload(asdu);
    int payloadSize = CS101_ASDU_getPayloadSize(asdu);
    int numberOfElements = CS101_ASDU_getNumberOfElements(asdu);
    int offset = 0;

    // Пропускаем первые 3 байта (IOA)
    offset += 3;

    printf("Parsing F_DR_TA_1: NumIx=%d, PayloadSize=%d, RawPayload=", 
           numberOfElements, payloadSize);
    for (int i = 0; i < payloadSize; i++) {
        printf("%02x ", rawData[i]);
    }
    printf("\n");

    for (int elem = 0; elem < numberOfElements && offset + 13 <= payloadSize; elem++) {
        // Извлекаем NOF (2 байта, little-endian)
        uint16_t nof = rawData[offset] | (rawData[offset + 1] << 8);
        offset += 2;

        // Извлекаем oscnum из NOF (filetype=0)
        uint16_t oscnum = nof / 10;
        char fileName[16];
        snprintf(fileName, sizeof(fileName), "%u", oscnum * 10);

        // Извлекаем размер файла (3 байта, little-endian)
        uint32_t fileSize = rawData[offset] | (rawData[offset + 1] << 8) | (rawData[offset + 2] << 16);
        offset += 3;

        // Извлекаем метку времени (8 байт, используем только 7 как CP56Time2a)
        uint64_t timestampRaw = 0;
        for (int i = 0; i < 8; i++) {
            timestampRaw |= ((uint64_t)rawData[offset + i]) << (i * 8);
        }
        uint64_t ms = rawData[offset] | ((rawData[offset + 1] & 0x3f) << 8); // Миллисекунды (2 байта, игнорируем старшие 2 бита)
        uint8_t minute = rawData[offset + 2] & 0x3f; // Минуты (6 бит)
        uint8_t hour = rawData[offset + 3] & 0x1f; // Часы (5 бит)
        uint8_t day = rawData[offset + 4] & 0x1f; // День (5 бит)
        uint8_t month = rawData[offset + 5] & 0x0f; // Месяц (4 бита)
        uint8_t year = rawData[offset + 6] & 0x7f; // Год (7 бит, 0-99, добавляем 2000)
        offset += 8; // Пропускаем 8 байт

        // Преобразуем в Unix timestamp
        struct tm timeinfo = {0};
        timeinfo.tm_year = year + 2000 - 1900; // Год с 1900
        timeinfo.tm_mon = month - 1; // Месяц 0-11
        timeinfo.tm_mday = day;
        timeinfo.tm_hour = hour;
        timeinfo.tm_min = minute;
        timeinfo.tm_sec = ms / 1000;
        timeinfo.tm_isdst = -1; // Автоопределение DST
        time_t seconds = timegm(&timeinfo);
        uint64_t msTimestamp = (uint64_t)seconds * 1000 + (ms % 1000);

        char timeStr[32];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", gmtime(&seconds));
        snprintf(timeStr + strlen(timeStr), sizeof(timeStr) - strlen(timeStr), ".%03uZ", (unsigned)(ms % 1000));

        // Сохраняем в fileList
        client->fileList.emplace(nof, FileInfo{std::string(fileName), fileSize, msTimestamp});

        printf("Parsed F_DR_TA_1 Element %d/%d: fileName=%s, NOF=%u, oscnum=%u, size=%u, timestampRaw=0x%016" PRIx64 ", timestamp=%s, ms=%u, min=%u, hour=%u, day=%u, mon=%u, year=%u, clientID: %s\n",
               elem + 1, numberOfElements, fileName, nof, oscnum, fileSize, timestampRaw, timeStr, ms, minute, hour, day, month, year, client->clientID.c_str());

        // Отправляем данные в JavaScript
        client->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
            try {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, client->clientID.c_str()));
                eventObj.Set("type", Napi::String::New(env, "fileList"));
                eventObj.Set("fileName", Napi::String::New(env, fileName));
                eventObj.Set("fileSize", Napi::Number::New(env, fileSize));
                eventObj.Set("timestamp", Napi::Number::New(env, static_cast<double>(msTimestamp)));
                eventObj.Set("oscnum", Napi::Number::New(env, oscnum));
                eventObj.Set("isPrimaryIP", Napi::Boolean::New(env, client->usingPrimaryIp));
                std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                jsCallback.Call(args);
            } catch (const Napi::Error& e) {
                printf("N-API callback error in F_DR_TA_1: %s, clientID: %s\n", e.what(), client->clientID.c_str());
            }
        });
    }

    if (offset < payloadSize) {
        printf("Warning: %d bytes remaining in F_DR_TA_1 payload after parsing %d elements, clientID: %s\n",
               payloadSize - offset, numberOfElements, client->clientID.c_str());
    }
    break;
}
            
            case F_FR_NA_1: { // File Ready (TypeID = 120)
                if (!asdu || numberOfElements < 1) {
                    printf("Invalid ASDU or no elements for F_FR_NA_1, COT=%d, clientID: %s\n", cot, client->clientID.c_str());
                    return false;
                }

                for (int i = 0; i < numberOfElements; i++) {
                    FileReady io = (FileReady)CS101_ASDU_getElement(asdu, i);
                    if (!io) {
                        printf("Failed to get element %d for F_FR_NA_1, clientID: %s\n", i, client->clientID.c_str());
                        continue;
                    }

                    int ioa = InformationObject_getObjectAddress((InformationObject)io);
                    uint16_t nof = FileReady_getNOF(io);
                    uint8_t frq = FileReady_getFRQ(io);
                    std::string fileName = client->getFileNameByNOF(nof);

                    // Проверка COT
                    if (cot != 7 && cot != 13) { // Ожидаем COT=7 или COT=13
                        printf("F_FR_NA_1 with unexpected COT=%d (expected 7 or 13), IOA=%d, NOF=%u, FRQ=%u, File=%s, clientID: %s\n", 
                            cot, ioa, nof, frq, fileName.c_str(), client->clientID.c_str());
                        if (cot == CS101_COT_UNKNOWN_COT) { // COT=47
                            client->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
                                Napi::Object errorObj = Napi::Object::New(env);
                                errorObj.Set("clientID", Napi::String::New(env, client->clientID.c_str()));
                                errorObj.Set("type", Napi::String::New(env, "error"));
                                errorObj.Set("message", Napi::String::New(env, "File selection failed (COT=47)"));
                                errorObj.Set("ioa", Napi::Number::New(env, ioa));
                                errorObj.Set("fileName", Napi::String::New(env, fileName));
                                std::vector<napi_value> args = {Napi::String::New(env, "data"), errorObj};
                                jsCallback.Call(args);
                            });
                        }
                        FileReady_destroy(io);
                        continue;
                    }

                    // Обработка FRQ
                    if (frq != 0 && frq != 5 && frq != 92) {
                        printf("F_FR_NA_1 with unexpected FRQ=%u (expected 0, 5, or 92), IOA=%d, NOF=%u, File=%s, clientID: %s\n", 
                            frq, ioa, nof, fileName.c_str(), client->clientID.c_str());
                        client->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
                            Napi::Object errorObj = Napi::Object::New(env);
                            errorObj.Set("clientID", Napi::String::New(env, client->clientID.c_str()));
                            errorObj.Set("type", Napi::String::New(env, "error"));
                            errorObj.Set("message", Napi::String::New(env, "Unexpected FRQ value"));
                            errorObj.Set("ioa", Napi::Number::New(env, ioa));
                            errorObj.Set("fileName", Napi::String::New(env, fileName));
                            errorObj.Set("frq", Napi::Number::New(env, frq)); // Include FRQ value
                            std::vector<napi_value> args = {Napi::String::New(env, "data"), errorObj};
                            jsCallback.Call(args);
                        });
                        FileReady_destroy(io);
                        continue; // Continue to allow processing of other elements
                    }

                    printf("File ready (F_FR_NA_1, COT=%d) for IOA=%d, NOF=%u, FRQ=%u, File=%s, clientID: %s\n", 
                        cot, ioa, nof, frq, fileName.c_str(), client->clientID.c_str());

                    client->currentNOF = nof;

                    client->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
                        Napi::Object eventObj = Napi::Object::New(env);
                        eventObj.Set("clientID", Napi::String::New(env, client->clientID.c_str()));
                        eventObj.Set("type", Napi::String::New(env, "fileReady"));
                        eventObj.Set("ioa", Napi::Number::New(env, ioa));
                        eventObj.Set("fileName", Napi::String::New(env, fileName));
                        eventObj.Set("nof", Napi::Number::New(env, nof));
                        eventObj.Set("frq", Napi::Number::New(env, frq));
                        std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                        jsCallback.Call(args);
                    });

                    FileReady_destroy(io);
                }
                return true;
            }

            case F_SR_NA_1: { // Section Ready (TypeID = 121)
        for (int i = 0; i < numberOfElements; i++) {
            FileSegment io = (FileSegment)CS101_ASDU_getElement(asdu, i);
            if (io) {
                int ioa = InformationObject_getObjectAddress((InformationObject)io);
                uint16_t nos = FileSegment_getNOF(io);
                std::string fileName = client->getFileNameByNOF(nos); // Используем NOS как NOF
                printf("Section ready (F_SR_NA_1) for IOA=%d, NOS=%u, File=%s, clientID: %s\n", 
                    ioa, nos, fileName.c_str(), client->clientID.c_str());

                client->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, client->clientID.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "sectionReady"));
                    eventObj.Set("ioa", Napi::Number::New(env, ioa));
                    eventObj.Set("nos", Napi::Number::New(env, nos));
                    eventObj.Set("fileName", Napi::String::New(env, fileName));
                    std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                    jsCallback.Call(args);
                });
                FileSegment_destroy(io);
            }
        }
        return true;
    }

            case F_SG_NA_1: { // File Segment (TypeID = 125)
        for (int i = 0; i < numberOfElements; i++) {
            FileSegment io = (FileSegment)CS101_ASDU_getElement(asdu, i);
            if (io) {
                int ioa = InformationObject_getObjectAddress((InformationObject)io);
                uint8_t* segment = FileSegment_getSegmentData(io);
                uint8_t length = FileSegment_getLengthOfSegment(io);
                uint16_t nos = FileSegment_getNOF(io); // Используем NOS как NOF
                std::string fileName = client->getFileNameByNOF(nos);
                printf("Received file segment (F_SG_NA_1) for IOA=%d, Length=%u, NOS=%u, File=%s, clientID: %s\n", 
                    ioa, length, nos, fileName.c_str(), client->clientID.c_str());

                client->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, client->clientID.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "fileData"));
                    eventObj.Set("ioa", Napi::Number::New(env, ioa));
                    eventObj.Set("fileName", Napi::String::New(env, fileName));
                    eventObj.Set("nos", Napi::Number::New(env, nos)); // Добавляем NOS
                    eventObj.Set("data", Napi::Buffer<uint8_t>::Copy(env, segment, length));
                    std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                    jsCallback.Call(args);
                });
                FileSegment_destroy(io);
            }
        }
        return true;
    }
            case F_LS_NA_1: { // Last Section (TypeID = 123)
        for (int i = 0; i < numberOfElements; i++) {
            InformationObject io = CS101_ASDU_getElement(asdu, i);
            if (io) {
                int ioa = InformationObject_getObjectAddress(io);
                uint8_t* rawData = CS101_ASDU_getPayload(asdu);
                int payloadSize = CS101_ASDU_getPayloadSize(asdu);
                uint16_t nof = 0;
                if (payloadSize >= 5) { // IOA (3 байта) + NOF (2 байта)
                    nof = rawData[3] | (rawData[4] << 8); // NOF в little-endian
                }
                std::string fileName = client->getFileNameByNOF(nof);
                printf("End of section (F_LS_NA_1) for IOA=%d, NOF=%u, File=%s, clientID: %s\n", 
                    ioa, nof, fileName.c_str(), client->clientID.c_str());

                client->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, client->clientID.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "fileEnd"));
                    eventObj.Set("ioa", Napi::Number::New(env, ioa));
                    eventObj.Set("fileName", Napi::String::New(env, fileName));
                    eventObj.Set("nof", Napi::Number::New(env, nof)); // Используем извлеченный NOF
                    std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                    jsCallback.Call(args);
                });
                InformationObject_destroy(io);
            }
        }
        client->currentNOF = 0; // Сбрасываем после завершения передачи
        return true;
    }
                case F_AF_NA_1: { // File Acknowledgment (TypeID = 124)
                    for (int i = 0; i < numberOfElements; i++) {
                        FileACK io = (FileACK)CS101_ASDU_getElement(asdu, i);
                        if (io) {
                            int ioa = InformationObject_getObjectAddress((InformationObject)io);
                            uint16_t nof = FileACK_getNOF(io);
                            printf("File transfer acknowledged (F_AF_NA_1) for IOA=%d, NOF=%u, clientID: %s\n", 
                                ioa, nof, client->clientID.c_str());
                            FileACK_destroy(io);
                        }
                    }
                    return true;
                }

                default:
                    printf("Received unsupported ASDU type: %s (%i), clientID: %s\n", TypeID_toString(typeID), typeID, client->clientID.c_str());
                    int payloadSize = CS101_ASDU_getPayloadSize(asdu);
                    uint8_t* payload = CS101_ASDU_getPayload(asdu);
                    printf("Raw payload (size=%d): ", payloadSize);
                    for (int i = 0; i < payloadSize; i++) {
                        printf("%02x ", payload[i]);
                    }
                    printf("\n");
                    return true;
            }

            // Обработка данных мониторинга (M_ types), если есть элементы
            if (!elements.empty()) {
                for (const auto& [ioa, val, quality, timestamp] : elements) {
                    printf("ASDU type: %s, clientID: %s, asduAddress: %d, ioa: %i, value: %f, quality: %u, timestamp: %" PRIu64 ", cnt: %i\n", 
                        TypeID_toString(typeID), client->clientID.c_str(), receivedAsduAddress, ioa, val, quality, timestamp, client->cnt);
                }

                client->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
                    Napi::Array jsArray = Napi::Array::New(env, elements.size());
                    for (size_t i = 0; i < elements.size(); i++) {
                        const auto& [ioa, val, quality, timestamp] = elements[i];
                        Napi::Object msg = Napi::Object::New(env);
                        msg.Set("clientID", Napi::String::New(env, client->clientID.c_str()));
                        msg.Set("typeId", Napi::Number::New(env, typeID));
                        msg.Set("asdu", Napi::Number::New(env, receivedAsduAddress));
                        msg.Set("ioa", Napi::Number::New(env, ioa));
                        msg.Set("val", Napi::Number::New(env, val));
                        msg.Set("quality", Napi::Number::New(env, quality));
                        if (timestamp > 0) {
                            msg.Set("timestamp", Napi::Number::New(env, static_cast<double>(timestamp)));
                        }
                        jsArray[i] = msg;
                    }
                    std::vector<napi_value> args = {Napi::String::New(env, "data"), jsArray};
                    jsCallback.Call(args);
                    client->cnt++;
                });
            }

            return true;

        } catch (const std::exception& e) {
            printf("Exception in RawMessageHandler: %s, clientID: %s\n", e.what(), client->clientID.c_str());
            client->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
                Napi::Object eventObj = Napi::Object::New(env);
                eventObj.Set("clientID", Napi::String::New(env, client->clientID.c_str()));
                eventObj.Set("type", Napi::String::New(env, "error"));
                eventObj.Set("reason", Napi::String::New(env, std::string("ASDU handling failed: ") + e.what()));
                eventObj.Set("isPrimaryIP", Napi::Boolean::New(env, isPrimaryIP));
                std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                jsCallback.Call(args);
            });
            return false;
        }
}

std::string IEC104Client::getFileNameByNOF(uint16_t nof) {
    auto it = fileList.find(nof);
    if (it != fileList.end()) {
        return it->second.name;
    }
    return "Unknown_" + std::to_string(nof);
}

/*std::string IEC104Client::getFileNameByIOA(int ioa) {
    for (const auto& [fileIOA, fileName] : fileList) {
        if (fileIOA == ioa) return fileName;
    }
    return "Unknown_" + std::to_string(ioa);
}*/

Napi::Value IEC104Client::GetStatus(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(this->connMutex);
    Napi::Object status = Napi::Object::New(env);
    status.Set("connected", Napi::Boolean::New(env, connected));
    status.Set("activated", Napi::Boolean::New(env, activated));
    status.Set("clientID", Napi::String::New(env, clientID.c_str()));
    status.Set("usingPrimaryIp", Napi::Boolean::New(env, usingPrimaryIp));
    return status;
}