const { IEC104Server } = require('../build/Release/addon_iec60870');

// Creating an instance of the server with a callback function to handle events
const server = new IEC104Server((event, data) => {
    console.log(`Server Event: ${event}, Data: ${JSON.stringify(data, null, 2)}`);

    if (event === 'data') {
        if (Array.isArray(data)) {
            data.forEach(cmd => {
                const { serverID, clientId, typeId, ioa, val, quality, timestamp, asduAddress } = cmd;

                // Processing incoming commands
                switch (typeId) {
                    case 45: // C_SC_NA_1 (Single Command)
                        console.log(`Received Single Command: clientId=${clientId}, ioa=${ioa}, value=${val}, asduAddress=${asduAddress}`);
                        // Response: sending the current value as M_SP_NA_1
                        server.sendCommands(clientId, [
                            { typeId: 1, ioa: ioa, value: val === 1, asduAddress: asduAddress, quality: quality, timestamp: timestamp }
                        ]);
                        break;

                    case 46: // C_DC_NA_1 (Double Command)
                        console.log(`Received Double Command: clientId=${clientId}, ioa=${ioa}, value=${val}, asduAddress=${asduAddress}`);
                        // Response: sending the current value as M_DP_NA_1
                        server.sendCommands(clientId, [
                            { typeId: 3, ioa: ioa, value: Math.round(val), asduAddress: asduAddress }
                        ]);
                        break;

                    case 47: // C_RC_NA_1 (Regulating Step Command)
                        console.log(`Received Step Command: clientId=${clientId}, ioa=${ioa}, value=${val}, asduAddress=${asduAddress}`);
                        // Response: sending the current value as M_ST_NA_1
                        server.sendCommands(clientId, [
                            { typeId: 5, ioa: ioa, value: Math.round(val), asduAddress: asduAddress }
                        ]);
                        break;

                    case 48: // C_SE_NA_1 (Setpoint Command Normalized)
                        console.log(`Received Setpoint Normalized Command: clientId=${clientId}, ioa=${ioa}, value=${val}, asduAddress=${asduAddress}`);
                        // Response: sending the current value as M_ME_NA_1
                        server.sendCommands(clientId, [
                            { typeId: 9, ioa: ioa, value: val, asduAddress: asduAddress }
                        ]);
                        break;

                    case 49: // C_SE_NB_1 (Setpoint Command Scaled)
                        console.log(`Received Setpoint Scaled Command: clientId=${clientId}, ioa=${ioa}, value=${val}, asduAddress=${asduAddress}`);
                        // Response: sending the current value as M_ME_NB_1
                        server.sendCommands(clientId, [
                            { typeId: 11, ioa: ioa, value: Math.round(val), asduAddress: asduAddress }
                        ]);
                        break;

                    case 50: // C_SE_NC_1 (Setpoint Command Short)
                        console.log(`Received Setpoint Short Command: clientId=${clientId}, ioa=${ioa}, value=${val}, asduAddress=${asduAddress}`);
                        // Response: sending the current value as M_ME_NC_1
                        server.sendCommands(clientId, [
                            { typeId: 13, ioa: ioa, value: val, asduAddress: asduAddress }
                        ]);
                        break;

                    case 51: // C_BO_NA_1 (Bitstring 32 Command)
                        console.log(`Received Bitstring 32 Command: clientId=${clientId}, ioa=${ioa}, value=${val}, asduAddress=${asduAddress}`);
                        // Response: sending the current value as M_BO_NA_1
                        server.sendCommands(clientId, [
                            { typeId: 7, ioa: ioa, value: Math.round(val), asduAddress: asduAddress }
                        ]);
                        break;

                    case 58: // C_SC_TA_1 (Single Command with CP56Time2a)
                        console.log(`Received Single Command with Time: clientId=${clientId}, ioa=${ioa}, value=${val}, timestamp=${timestamp}, asduAddress=${asduAddress}`);
                        // Response: sending the current value as M_SP_TB_1
                        server.sendCommands(clientId, [
                            { typeId: 30, ioa: ioa, value: val === 1, timestamp: timestamp, asduAddress: asduAddress }
                        ]);
                        break;

                    case 59: // C_DC_TA_1 (Double Command with CP56Time2a)
                        console.log(`Received Double Command with Time: clientId=${clientId}, ioa=${ioa}, value=${val}, timestamp=${timestamp}, asduAddress=${asduAddress}`);
                        // Response: sending the current value as M_DP_TB_1
                        server.sendCommands(clientId, [
                            { typeId: 31, ioa: ioa, value: Math.round(val), timestamp: timestamp, asduAddress: asduAddress }
                        ]);
                        break;

                    case 60: // C_RC_TA_1 (Regulating Step Command with CP56Time2a)
                        console.log(`Received Step Command with Time: clientId=${clientId}, ioa=${ioa}, value=${val}, timestamp=${timestamp}, asduAddress=${asduAddress}`);
                        // Response: sending the current value as M_ST_TB_1
                        server.sendCommands(clientId, [
                            { typeId: 32, ioa: ioa, value: Math.round(val), timestamp: timestamp, asduAddress: asduAddress }
                        ]);
                        break;

                    case 61: // C_SE_TA_1 (Setpoint Command Normalized with CP56Time2a)
                        console.log(`Received Setpoint Normalized Command with Time: clientId=${clientId}, ioa=${ioa}, value=${val}, timestamp=${timestamp}, asduAddress=${asduAddress}`);
                        // Response: sending the current value as M_ME_TD_1
                        server.sendCommands(clientId, [
                            { typeId: 34, ioa: ioa, value: val, timestamp: timestamp, asduAddress: asduAddress }
                        ]);
                        break;

                    case 62: // C_SE_TB_1 (Setpoint Command Scaled with CP56Time2a)
                        console.log(`Received Setpoint Scaled Command with Time: clientId=${clientId}, ioa=${ioa}, value=${val}, timestamp=${timestamp}, asduAddress=${asduAddress}`);
                        // Response: sending the current value as M_ME_TE_1
                        server.sendCommands(clientId, [
                            { typeId: 35, ioa: ioa, value: Math.round(val), timestamp: timestamp, asduAddress: asduAddress }
                        ]);
                        break;

                    case 63: // C_SE_TC_1 (Setpoint Command Short with CP56Time2a)
                        console.log(`Received Setpoint Short Command with Time: clientId=${clientId}, ioa=${ioa}, value=${val}, timestamp=${timestamp}, asduAddress=${asduAddress}`);
                        // Response: sending the current value as M_ME_TF_1
                        server.sendCommands(clientId, [
                            { typeId: 36, ioa: ioa, value: val, timestamp: timestamp, asduAddress: asduAddress }
                        ]);
                        break;

                    case 64: // C_BO_TA_1 (Bitstring 32 Command with CP56Time2a)
                        console.log(`Received Bitstring 32 Command with Time: clientId=${clientId}, ioa=${ioa}, value=${val}, timestamp=${timestamp}, asduAddress=${asduAddress}`);
                        // Response: sending the current value as M_BO_TB_1
                        server.sendCommands(clientId, [
                            { typeId: 33, ioa: ioa, value: Math.round(val), timestamp: timestamp, asduAddress: asduAddress }
                        ]);
                        break;

                    case 100: // C_IC_NA_1 (Interrogation Command)
                        console.log(`Received Interrogation Command: clientId=${clientId}, ioa=${ioa}, qoi=${val}, asduAddress=${asduAddress}`);
                        // Response: sending current values as spontaneous data
                        server.sendCommands(clientId, [
                            { typeId: 1, ioa: 1, value: true, asduAddress: asduAddress },           // M_SP_NA_1
                            { typeId: 3, ioa: 2, value: 1, asduAddress: asduAddress },              // M_DP_NA_1
                            { typeId: 5, ioa: 3, value: 42, asduAddress: asduAddress },             // M_ST_NA_1
                            { typeId: 13, ioa: 4, value: 23.5, timestamp: Date.now(), asduAddress: asduAddress } // M_ME_TF_1 with timestamp
                        ]);
                        break;

                    case 101: // C_CI_NA_1 (Counter Interrogation Command)
                        console.log(`Received Counter Interrogation Command: clientId=${clientId}, ioa=${ioa}, qcc=${val}, asduAddress=${asduAddress}`);
                        // Response: sending a sample counter value as M_IT_NA_1
                        server.sendCommands(clientId, [
                            { typeId: 15, ioa: ioa, value: 1234, asduAddress: asduAddress }
                        ]);
                        break;

                    case 102: // C_RD_NA_1 (Read Command)
                        console.log(`Received Read Command: clientId=${clientId}, ioa=${ioa}, asduAddress=${asduAddress}`);
                        // Response: sending a sample value as M_ME_NC_1 (example)
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
            // Handling control events (connected, disconnected, etc.)
            console.log(`Control Event: ${data.event}, Client ID: ${data.clientId}, Reason: ${data.reason}`);
        }
    }
});

// Server parameters
const port = 2404;
const serverID = "server1";

// Starting the server
console.log(`Starting IEC 104 Server on port ${port} with serverID: ${serverID}`);
server.start({
    port: port,
    serverID: serverID,
    ipReserve: "127.0.0.1",
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
});

// Checking server status after 2 seconds
setTimeout(() => {
    const status = server.getStatus();
    console.log('Server Status:', JSON.stringify(status, null, 2));
}, 2000);

// Example of sending spontaneous data after 5 seconds
setInterval(() => {
    const status = server.getStatus();
    if (status.connectedClients.length > 0) {
        const clientId = status.connectedClients[0];
        console.log(`Sending spontaneous data to client ${clientId}`);
        server.sendCommands(clientId, [
            //{ typeId: 1, ioa: 10, value: true, asduAddress: 1 },                    // M_SP_NA_1
            { typeId: 36, ioa: 11, value: 0.001, timestamp: Date.now(), asduAddress: 1 } // M_ME_TF_1
        ]);
    } else {
        console.log('No clients connected to send spontaneous data');
    }
}, 5000);

// Handling process termination
process.on('SIGINT', () => {
    console.log('Stopping server...');
   // server.stop();
    process.exit(0);
});