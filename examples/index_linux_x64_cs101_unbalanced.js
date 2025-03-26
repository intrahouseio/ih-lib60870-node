// Load the addon compiled for macOS ARM64
const { IEC101MasterUnbalanced } = require('./builds/linux_x64/addon_iec60870');
const util = require('util');

// CS101 Master: Connects to a device via serial port in unbalanced mode
const master = new IEC101MasterUnbalanced((event, data) => {
    try {
        // Handle link layer state changes and data reception
        if (event === 'data' && Array.isArray(data)) {
            console.log('Master: Data received from device:');
            data.forEach(item => {
                console.log(`  Slave: ${item.slaveAddress}, IOA: ${item.ioa}, Value: ${item.val}, Quality: ${item.quality}${item.timestamp ? `, Timestamp: ${item.timestamp}` : ''}`);
            });
            console.log('Master: Sending control commands to slave 1...');
            master.sendCommands([
                { typeId: 45, ioa: 145, value: true },    // C_SC_NA_1: Single command (On)
                { typeId: 46, ioa: 146, value: 1 },      // C_DC_NA_1: Double command (On)
                { typeId: 47, ioa: 147, value: 1 },      // C_RC_NA_1: Regulating step (Raise)
                { typeId: 48, ioa: 148, value: 0.001 },  // C_SE_NA_1: Setpoint normalized
                { typeId: 49, ioa: 149, value: 5000 },   // C_SE_NB_1: Setpoint scaled
                { typeId: 50, ioa: 150, value: 123.45 }, // C_SE_NC_1: Setpoint floating point
            ]);
            // Poll the slave again after sending commands
            master.pollSlave(1);
        } else if (event === 'data' && data.event === 'opened') {
            console.log('Master: Serial connection opened.');
            // In unbalanced mode, activation is automatic, but we can call sendStartDT if desired
            master.sendStartDT();
            // Add a slave to poll
            console.log('Master: Adding slave with address 1...');
            master.addSlave(1);
            // Start polling the slave
            console.log('Master: Polling slave 1...');
            master.pollSlave(1);
        } else if (event === 'data' && data.event === 'failed') {
            console.error(`Master: Connection failed - ${data.reason}`);
        } else if (event === 'data' && data.event === 'reconnecting') {
            console.log(`Master: Reconnecting - ${data.reason}`);
        }
        console.log(`CS101 Event: ${event}, Data: ${util.inspect(data)}`);
    } catch (error) {
        console.error(`CS101 Master Error: ${error.message}`);
    }
});

async function main() {
    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

    try {
        console.log('Starting IEC 60870-5-101 master in unbalanced mode...');

        // Connect to the device via the specific serial port with baud rate 19200
        master.connect('/dev/tty.usbserial-A505KXKT', 19200, 1, 'cs101_master_1', {
            linkAddress: 1,           // Link layer address (0-255)
            originatorAddress: 1,     // Originator address (0-255)
            asduAddress: 1,           // ASDU address (0-65535)
            t0: 30,                   // Link state timeout (seconds)
            t1: 15,                   // ACK timeout (seconds)
            t2: 10,                   // Repeat timeout (seconds)
            reconnectDelay: 5,        // Delay between reconnects (seconds)
            maxRetries: 3,            // Max reconnect attempts
            queueSize: 100            // Queue size for ASDUs
        });

        // Wait for connection to stabilize and initial polling to start
        await sleep(1000);

        // Check initial status
        const status = master.getStatus();
        console.log(`Initial Status: ${util.inspect(status)}`);

        console.log('Master initialized. Monitoring events...');
    } catch (error) {
        console.error(`Main Error: ${error.message}`);
        process.exit(1);
    }
}

// Run and keep the script alive
main().catch(err => console.error(`Startup Error: ${err.message}`));

// Handle graceful shutdown
process.on('SIGINT', () => {
    console.log('Shutting down CS101 master...');
    master.disconnect();
    process.exit(0);
});