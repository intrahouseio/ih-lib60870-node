#include <napi.h>
#include "cs104_server.h"          // Assuming this defines IEC104Server
#include "cs101_master_unbalanced.h" // Assuming this defines IEC101MasterUnbalanced
#include "cs101_master_balanced.h"  // Assuming this defines IEC101MasterBalanced
#include "cs101_slave1.h"           // Assuming this defines IEC101Slave
#include "cs104_client.h"          // Assuming this defines IEC104Client

// Define the Example class
class Example : public Napi::ObjectWrap<Example> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "Example", {
      InstanceMethod("getPlatform", &Example::GetPlatform),
      InstanceMethod("add", &Example::Add)
    });
    exports.Set("Example", func);
    return exports;
  }

  Example(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Example>(info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsNumber()) {
      Napi::TypeError::New(env, "Number expected").ThrowAsJavaScriptException();
      return;
    }
    value_ = info[0].As<Napi::Number>().Int32Value();
  }

private:
  Napi::Value GetPlatform(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    #ifdef _WIN32
      return Napi::String::New(env, "win32");
    #elif __APPLE__
      return Napi::String::New(env, "darwin");
    #elif __linux__
      return Napi::String::New(env, "linux");
    #else
      return Napi::String::New(env, "unknown");
    #endif
  }

  Napi::Value Add(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsNumber()) {
      Napi::TypeError::New(env, "Number expected").ThrowAsJavaScriptException();
      return env.Null();
    }
    int arg = info[0].As<Napi::Number>().Int32Value();
    return Napi::Number::New(env, value_ + arg);
  }

  int value_;
};

// Main initialization function
Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
  // Export the Example class
  Example::Init(env, exports);

  // Export existing IEC classes
  IEC104Server::Init(env, exports);
  IEC101MasterUnbalanced::Init(env, exports);
  IEC101MasterBalanced::Init(env, exports);
  IEC101Slave::Init(env, exports);
  IEC104Client::Init(env, exports);

  return exports;
}

NODE_API_MODULE(addon_iec60870, InitAll)