#include <napi.h>
#include "cs104_server.h"          // Assuming this defines IEC104Server
#include "cs101_master_unbalanced.h" // Assuming this defines IEC101MasterUnbalanced
#include "cs101_master_balanced.h"  // Assuming this defines IEC101MasterBalanced
#include "cs101_slave1.h"           // Assuming this defines IEC101Slave
#include "cs104_client.h"          // Assuming this defines IEC104Client

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    IEC104Server::Init(env, exports);          // Export IEC104Server class
    IEC101MasterUnbalanced::Init(env, exports); // Export IEC101MasterUnbalanced class
    IEC101MasterBalanced::Init(env, exports);   // Export IEC101MasterBalanced class
    IEC101Slave::Init(env, exports);           // Export IEC101Slave class
    IEC104Client::Init(env, exports);          // Export IEC104Client class
    return exports;
}

NODE_API_MODULE(addon_iec60870, InitAll)