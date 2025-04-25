## ih-lib60870-node

A cross-platform Node.js native addon providing support for the IEC 60870-5 protocol suite, enabling communication with industrial control systems, SCADA, and RTUs. This addon is built using node-gyp and prebuild to ensure compatibility across multiple operating systems and architectures.

## Features

- IEC 60870-5 Protocol Support: Implements key functionalities of the IEC 60870-5 standards (e.g., IEC 60870-5-101/104) for robust communication in industrial automation.

- Cross-Platform Compatibility: Supports Windows, Linux, and macOS with prebuilt binaries for x64, arm, and arm64 architectures.

- High Performance: Native C++ implementation optimized for low-latency and reliable data exchange.

- File Transfer: Supports file transfer operations in both monitoring and control directions.

- Flexible Integration: Easy-to-use APIs for integrating with existing Node.js applications, SCADA systems, or custom control solutions.

- Prebuilt Binaries: Includes precompiled binaries for Node.js v20, reducing setup complexity and ensuring quick deployment.

# Supported Platforms

Operating System: Windows, Linux, macOS

Architectures: x64, arm, arm64

## Installation

1. Ensure you have Node.js v20 installed.

2. Install the package via npm:

npm install ih-lib60870-node

3. The prebuilt binaries will be automatically downloaded for your platform and architecture. If a prebuilt binary is unavailable, the addon will be compiled using node-gyp, requiring:

  - Python 3.11+

  - A compatible C++ compiler (e.g., gcc on Linux, MSVC on Windows, clang on macOS)

## Usage

Below is a basic example of using ih-lib60870-node to establish a connection and handle IEC 60870-5-104 data:

const { IEC104Client } = require('ih-lib60870-node');
const util = require('util');
const fs = require('fs');
// Initialize an IEC 60870-5-104 client
const client = new IEC104Client((event, data) => {
    if (data.event === 'opened') client.sendStartDT();
    console.log(`Server 1 Event: ${event}, Data: ${util.inspect(data)}`);
    if (data.event === 'activated') client.sendCommands([
        { typeId: 100, ioa: 0, asdu: 1, value: 20 },    // interrogation command
        { typeId: 45, ioa: 145, value: true, asdu: 1, bselCmd: true, ql: 1 },    // C_SC_NA_1: On  
        { typeId: 46, ioa: 4000, value: 1, asdu: 1, bselCmd: 1, ql: 2 },      // C_DC_NA_1: Off
        { typeId: 47, ioa: 147, value: 1, asdu: 1, bselCmd: 1, ql: 0 },      // C_RC_NA_1: Increase
        { typeId: 48, ioa: 148, value: 0.001, asdu: 1, selCmd: 1, ql: 0 },  // C_SE_NA_1: Normalized setpoint
        { typeId: 49, ioa: 149, value: 5000, asdu: 1, bselCmd: 1, ql: 0 },   // C_SE_NB_1: Scaled setpoint
        { typeId: 50, ioa: 150, value: 123.45, asdu: 1 }, // C_SE_NC_1: Floating point setpoint
    ]);
});

const client2 = new IEC104Client((event, data) => {
    if (data.event === 'opened') client2.sendStartDT();
    console.log(`Server 2 Event: ${event}, Data: ${util.inspect(data)}`);
});

async function main() {
    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

    client.connect({
        ip: "192.168.0.1",
        port: 2404,
        clientID: "client1",
        ipReserve: "192.168.0.2",
        reconnectDelay: 2,           // reconnection delay in seconds
        originatorAddress: 0, 
        asduAddress: 1,       
        k: 12,
        w: 8,
        t0: 30,
        t1: 15,
        t2: 10,
        t3: 20,
        reconnectDelay: 2,
        maxRetries: 5
    });

   client2.connect({
        ip: "192.168.0.10",
        port: 2404,
        clientID: "client2",
        originatorAddress: 1,
        k: 12,
        w: 8,
        t0: 30,
        t1: 15,
        t2: 10,
        t3: 20,
        reconnectDelay: 2,
        maxRetries: 5
    });

    // Wait few seconds (optional to the synchronize actions)
    await sleep(1000);
}

main();

Examples for each supported protocol (e.g., IEC 60870-5-101, IEC 60870-5-104) can be found in the examples/ directory. These examples demonstrate how to configure and use the addon for various use cases in industrial automation.

## Building from Source

If you need to build the addon from source:

Clone the repository:

git clone https://github.com/intrahouseio/ih-lib60870-node.git
cd ih-lib60870-node

Install dependencies:

npm install

Configure and build:

npm run configure
npm run build

Optionally, generate prebuilt binaries:

npm run prebuild


## Contributing

Contributions are welcome! Please follow these steps:

1. Fork the repository.

2. Create a new branch for your feature or bug fix.

3. Submit a pull request with a clear description of your changes.


## License

This project is licensed under the MIT License. See the LICENSE file for details.


## Support

For issues, questions, or feature requests, please open an issue on the GitHub repository.

