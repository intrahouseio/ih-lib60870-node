const { IEC104Server } = require('../build/Release/addon_iec60870');

// Function to create and start a server with specified mode
function startServer(mode, port, serverID, clients = []) {
    console.log(`Starting IEC 104 Server on port ${port} with serverID: ${serverID} in ${mode} mode`);

    // Create server instance with event callback
    const server = new IEC104Server((event, data) => {
        console.log(`Server Event: ${event}, Data: ${JSON.stringify(data, null, 2)}`);

        if (event === 'data') {
            if (Array.isArray(data)) {
                data.forEach(cmd => {
                    const { serverID, clientId, typeId, ioa, val, quality, timestamp, asduAddress } = cmd;

                    // Process incoming commands
                    switch (typeId) {
                        case 45: // C_SC_NA_1 (Single Command)
                            console.log(`Received Single Command: clientId=${clientId}, ioa=${ioa}, value=${val}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 1, ioa: ioa, value: val === 1, asduAddress: asduAddress, quality: quality, timestamp: timestamp }
                            ]);
                            break;

                        case 46: // C_DC_NA_1 (Double Command)
                            console.log(`Received Double Command: clientId=${clientId}, ioa=${ioa}, value=${val}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 3, ioa: ioa, value: Math.round(val), asduAddress: asduAddress }
                            ]);
                            break;

                        case 47: // C_RC_NA_1 (Regulating Step Command)
                            console.log(`Received Step Command: clientId=${clientId}, ioa=${ioa}, value=${val}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 5, ioa: ioa, value: Math.round(val), asduAddress: asduAddress }
                            ]);
                            break;

                        case 48: // C_SE_NA_1 (Setpoint Command Normalized)
                            console.log(`Received Setpoint Normalized Command: clientId=${clientId}, ioa=${ioa}, value=${val}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 9, ioa: ioa, value: val, asduAddress: asduAddress }
                            ]);
                            break;

                        case 49: // C_SE_NB_1 (Setpoint Command Scaled)
                            console.log(`Received Setpoint Scaled Command: clientId=${clientId}, ioa=${ioa}, value=${val}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 11, ioa: ioa, value: Math.round(val), asduAddress: asduAddress }
                            ]);
                            break;

                        case 50: // C_SE_NC_1 (Setpoint Command Short)
                            console.log(`Received Setpoint Short Command: clientId=${clientId}, ioa=${ioa}, value=${val}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 13, ioa: ioa, value: val, asduAddress: asduAddress }
                            ]);
                            break;

                        case 51: // C_BO_NA_1 (Bitstring 32 Command)
                            console.log(`Received Bitstring 32 Command: clientId=${clientId}, ioa=${ioa}, value=${val}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 7, ioa: ioa, value: Math.round(val), asduAddress: asduAddress }
                            ]);
                            break;

                        case 58: // C_SC_TA_1 (Single Command with CP56Time2a)
                            console.log(`Received Single Command with Time: clientId=${clientId}, ioa=${ioa}, value=${val}, timestamp=${timestamp}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 30, ioa: ioa, value: val === 1, timestamp: timestamp, asduAddress: asduAddress }
                            ]);
                            break;

                        case 59: // C_DC_TA_1 (Double Command with CP56Time2a)
                            console.log(`Received Double Command with Time: clientId=${clientId}, ioa=${ioa}, value=${val}, timestamp=${timestamp}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 31, ioa: ioa, value: Math.round(val), timestamp: timestamp, asduAddress: asduAddress }
                            ]);
                            break;

                        case 60: // C_RC_TA_1 (Regulating Step Command with CP56Time2a)
                            console.log(`Received Step Command with Time: clientId=${clientId}, ioa=${ioa}, value=${val}, timestamp=${timestamp}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 32, ioa: ioa, value: Math.round(val), timestamp: timestamp, asduAddress: asduAddress }
                            ]);
                            break;

                        case 61: // C_SE_TA_1 (Setpoint Command Normalized with CP56Time2a)
                            console.log(`Received Setpoint Normalized Command with Time: clientId=${clientId}, ioa=${ioa}, value=${val}, timestamp=${timestamp}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 34, ioa: ioa, value: val, timestamp: timestamp, asduAddress: asduAddress }
                            ]);
                            break;

                        case 62: // C_SE_TB_1 (Setpoint Command Scaled with CP56Time2a)
                            console.log(`Received Setpoint Scaled Command with Time: clientId=${clientId}, ioa=${ioa}, value=${val}, timestamp=${timestamp}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 35, ioa: ioa, value: Math.round(val), timestamp: timestamp, asduAddress: asduAddress }
                            ]);
                            break;

                        case 63: // C_SE_TC_1 (Setpoint Command Short with CP56Time2a)
                            console.log(`Received Setpoint Short Command with Time: clientId=${clientId}, ioa=${ioa}, value=${val}, timestamp=${timestamp}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 36, ioa: ioa, value: val, timestamp: timestamp, asduAddress: asduAddress }
                            ]);
                            break;

                        case 64: // C_BO_TA_1 (Bitstring 32 Command with CP56Time2a)
                            console.log(`Received Bitstring 32 Command with Time: clientId=${clientId}, ioa=${ioa}, value=${val}, timestamp=${timestamp}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 33, ioa: ioa, value: Math.round(val), timestamp: timestamp, asduAddress: asduAddress }
                            ]);
                            break;

                        case 100: // C_IC_NA_1 (Interrogation Command)
                            console.log(`Received Interrogation Command: clientId=${clientId}, ioa=${ioa}, qoi=${val}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 1, ioa: 1, value: true, asduAddress: asduAddress },
                                { typeId: 3, ioa: 2, value: 1, asduAddress: asduAddress },
                                { typeId: 5, ioa: 3, value: 42, asduAddress: asduAddress },
                                { typeId: 13, ioa: 4, value: 23.5, timestamp: Date.now(), asduAddress: asduAddress }
                            ]);
                            break;

                        case 101: // C_CI_NA_1 (Counter Interrogation Command)
                            console.log(`Received Counter Interrogation Command: clientId=${clientId}, ioa=${ioa}, qcc=${val}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 15, ioa: ioa, value: 1234, asduAddress: asduAddress }
                            ]);
                            break;

                        case 102: // C_RD_NA_1 (Read Command)
                            console.log(`Received Read Command: clientId=${clientId}, ioa=${ioa}, asduAddress=${asduAddress}`);
                            server.sendCommands(clientId, [
                                { typeId: 13, ioa: ioa, value: 42.7, asduAddress: asduAddress }
                            ]);
                            break;

                        case 103: // C_CS_NA_1 (Clock Synchronization Command)
                            console.log(`Received Clock Sync Command: clientId=${clientId}, ioa=${ioa}, timestamp=${timestamp}, asduAddress=${asduAddress}`);
                            break;

                        default:
                            console.log(`Unhandled command typeId=${typeId}, clientId=${clientId}, ioa=${ioa}, value=${val}, asduAddress=${asduAddress}`);
                    }
                });
            } else {
                // Handle control events
                console.log(`Control Event: ${data.event}, Client ID: ${data.clientId}, Reason: ${data.reason}`);
            }
        }
    });

    // Server configuration
    const config = {
        port: port,
        serverID: serverID,
        mode: mode, // 'redundant' or 'multi'
        params: {
            originatorAddress: 1,
            k: 12,
            w: 8,
            t0: 30,
            t1: 15,
            t2: 10,
            t3: 20,
            maxClients: 10
        }
    };

    // Add clients for redundant mode
    if (mode === 'redundant') {
        config.clients = clients; // e.g., [{ ip: "127.0.0.1", group: "Group1" }, ...]
    }

    // Start the server
    server.start(config);

    // Check server status after 2 seconds
    setTimeout(() => {
        const status = server.getStatus();
        console.log(`Server Status (${mode} mode):`, JSON.stringify(status, null, 2));
    }, 2000);

    // Send spontaneous data every 5 seconds if clients are connected
    const intervalId = setInterval(() => {
        const status = server.getStatus();
        if (status.connectedClients.length > 0) {
            const clientId = status.connectedClients[0];
            console.log(`Sending spontaneous data to client ${clientId} in ${mode} mode`);
            server.sendCommands(clientId, [
                { typeId: 36, ioa: 11, value: 0.001, timestamp: Date.now(), asduAddress: 1 } // M_ME_TF_1
            ]);
        } else {
            console.log(`No clients connected to send spontaneous data in ${mode} mode`);
        }
    }, 5000);

    // Return server and interval ID for cleanup
    return { server, intervalId };
}

// Test both modes sequentially
async function testServerModes() {
    // Test Multi Mode
    console.log('=== Testing Multi Mode ===');
    const multiConfig = startServer('multi', 2404, 'server_multi');
    await new Promise(resolve => setTimeout(resolve, 60000)); // Run for 60 seconds

    // Stop Multi Mode server
    console.log('Stopping Multi Mode server...');
    multiConfig.server.stop();
    clearInterval(multiConfig.intervalId);

    // Wait briefly to ensure port is free
    await new Promise(resolve => setTimeout(resolve, 2000));

    // Test Redundant Mode
    console.log('=== Testing Redundant Mode ===');
    const redundantConfig = startServer('redundant', 2404, 'server_redundant', [
        { ip: '127.0.0.1', group: 'Group1' },
        { ip: '192.168.1.100', group: 'Group2' }
    ]);
    await new Promise(resolve => setTimeout(resolve, 60000)); // Run for 60 seconds

    // Stop Redundant Mode server
    console.log('Stopping Redundant Mode server...');
    redundantConfig.server.stop();
    clearInterval(redundantConfig.intervalId);
}

// Handle process termination
process.on('SIGINT', () => {
    console.log('Terminating process...');
    process.exit(0);
});

// Run the tests
testServerModes().catch(err => console.error('Test failed:', err));