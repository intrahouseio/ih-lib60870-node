{
  "variables": {
    "openssl_fips%": ""
  },
  
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
      "actions": [
        {
          "action_name": "print_variable",
          "inputs": [],
          "outputs": ["print_output"],
          "action": ["echo", "openssl_fips: $(openssl_fips)"]
        }
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "lib/src/inc/api",
        "lib/src/inc/internal",
        "lib/src/hal/inc",
        "lib/src/tls",
        "src"
      ],
      "defines": ["NAPI_CPP_EXCEPTIONS"],
      "cflags": ["-Wall", "-Wno-unused-parameter"],
      "cflags_cc": ["-Wall", "-Wno-unused-parameter", "-std=c++17", "-fexceptions"],
      "conditions": [
        ["OS=='mac' and target_arch=='arm64'", {
          "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "MACOSX_DEPLOYMENT_TARGET": "11.0",
            "ARCHS": ["arm64"],
            "OTHER_CFLAGS": ["-Wall", "-Wno-unused-parameter"],
            "OTHER_CPLUSPLUSFLAGS": ["-Wall", "-Wno-unused-parameter", "-std=c++17", "-fexceptions"]
          },
          "libraries": [
            "<(module_root_dir)/lib/build/lib60870_darwin_arm64.a"
          ]
        }],
        ["OS=='mac' and target_arch=='x64'", {
          "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "MACOSX_DEPLOYMENT_TARGET": "11.0",
            "ARCHS": ["x64"],
            "OTHER_CFLAGS": ["-Wall", "-Wno-unused-parameter"],
            "OTHER_CPLUSPLUSFLAGS": ["-Wall", "-Wno-unused-parameter", "-std=c++17", "-fexceptions"]
          },
          "libraries": [
            "<(module_root_dir)/lib/build/lib60870_darwin_x64.a"
          ]
        }],
        ["OS=='linux' and target_arch=='x64'", {
          "cflags": ["-fPIC"],
          "cflags_cc": ["-fPIC"],
          "libraries": [
            "<(module_root_dir)/lib/build/lib60870_linux_x64.a",
            "-lpthread"
          ]
        }],
        ["OS=='linux' and target_arch=='arm64'", {
          "cflags": ["-fPIC", "-march=armv8-a"],
          "cflags_cc": ["-fPIC", "-march=armv8-a"],
          "libraries": [
            "<(module_root_dir)/lib/build/lib60870_linux_arm64.a",
            "-lpthread"
          ]
        }],
        ["OS=='linux' and target_arch=='arm'", {
          "cflags": ["-fPIC", "-march=armv7-a", "-mfpu=vfp", "-mfloat-abi=hard"],
          "cflags_cc": ["-fPIC", "-march=armv7-a", "-mfpu=vfp", "-mfloat-abi=hard"],
          "libraries": [
            "<(module_root_dir)/lib/build/lib60870_linux_arm.a",
            "-lpthread"
          ]
        }],
        ["OS=='win' and target_arch=='x64'", {
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": 1,
              "AdditionalOptions": ["/std:c++17"]
            }
          },
          "libraries": [
            "<(module_root_dir)/lib/build/lib60870_win_x64.lib",
            "-lws2_32.lib",
            "-liphlpapi.lib",
            "-lbcrypt.lib",
            "-lmsvcrt.lib"
          ]
        }]
      ]
    }
  ]
}