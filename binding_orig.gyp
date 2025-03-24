{
  "targets": [
    {
      "target_name": "addon_iec60870",
      "cflags!": ["-fexceptions"],
      "cflags_cc!": ["-fexceptions"],
      "sources": [
        "./src/cs104_client.cc",
        "./src/cs101_master_balanced.cc",
        "./src/cs101_master_unbalanced.cc",        
        "./src/cs104_server.cc",
        "./src/cs101_slave1.cc",
        "./src/iec60870.cc"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "./lib/src/inc/api", 
        "./lib/src/inc/internal",
        "./lib/src/hal/inc",  
        "./lib/src/tls",      
        "./src"               
      ],
      "conditions": [
        ["OS=='mac'", {
          "cflags_cc": ["-arch arm64"],
          "libraries": ["<(module_root_dir)/lib/build/lib60870_darwin_arm64.a", "-lpthread"],
          "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "MACOSX_DEPLOYMENT_TARGET": "13.0"
          }
        }],
        ["OS=='linux'", {
          "cflags_cc": ["-std=c++11"],
          "libraries": ["-lpthread"],
          "conditions": [
            ["target_arch=='arm'", {
              "libraries": ["<(module_root_dir)/lib/build/lib60870_linux_arm.a"]
            }],
            ["target_arch=='arm64'", {
              "libraries": ["<(module_root_dir)/lib/build/lib60870_linux_arm64.a"]
            }],
            ["target_arch=='x64'", {
              "libraries": ["<(module_root_dir)/lib/build/lib60870_linux_x64.a"]
            }]
          ]
        }],
        ["OS=='win'", {
          "cflags_cc": ["-std=c++11"],
          "libraries": ["<(module_root_dir)/lib/build/lib60870_win32.a", "ws2_32.lib"],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": 1,
              "AdditionalOptions": ["/std:c++11"]
            }
          }
        }]
      ]
    }
  ]
}
