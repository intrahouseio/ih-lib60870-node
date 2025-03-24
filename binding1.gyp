{
  "targets": [
    {
      "target_name": "addon_iec60870",
      "sources": [
        "src/cs104_client.cc",
        "src/cs101_master_balanced.cc",
        "src/cs101_master_unbalanced.cc",
        "src/cs104_server.cc",
        "src/cs101_slave1.cc",
        "src/iec60870.cc"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "lib/src/inc/api",
        "lib/src/inc/internal",
        "lib/src/hal/inc",
        "lib/src/tls",
        "src"
      ],
      "targets": [{
        "cflags": ["-std=c++17"],
        "cflags_cc": ["-std=c++17"],
        "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS"]
    }],
    "cflags": ["-std=c++17", "-fexceptions"],
      "cflags_cc": ["-std=c++17", "-fexceptions"],
      "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS"],
      "conditions": [
        ["OS=='win'", {
          "libraries": ["-lws2_32"],
          "msvs_settings": {
            "VCCLCompilerTool": {"ExceptionHandling": 1}
          }
        }]
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "defines": [ "NAPI_DISABLE_CPP_EXCEPTIONS" ],
      "conditions": [        
        ["OS=='linux'", {
          "cflags": [ "-Wall", "-Wno-unused-parameter" ],
          "cflags_cc": [ "-Wall", "-Wno-unused-parameter", "-std=c++17", "-fexceptions" ],
          "libraries": [ "-lpthread" ]
        }],
        ["OS=='win'", {
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": 1,
              "AdditionalOptions": [ "/std:c++17" ]
            }
          },
          "libraries": [ "-l$(NODE_DIR)/x64/node.lib" ]
        }]
      ]
    }
  ]
}