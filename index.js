const path = require('path');
const os = require('os');
const fs = require('fs');

const platform = os.platform();
const arch = os.arch();

// Карта соответствий платформ и архитектур с именами папок
const bindingsMap = {
  'win32_x64': 'windows_x64',  
  'darwin_x64': 'macos_x64',
  'darwin_arm64': 'macos_arm64',
  'linux_x64': 'linux_x64',
  'linux_arm64': 'linux_arm64',
  'linux_arm': 'linux_arm'
};


const key = platform+"_"+arch;
const bindingFolder = bindingsMap[key];

if (!bindingFolder) {
  throw new Error(`Unsupported platform/architecture combination: ${platform}/${arch}`);
}

const bindingPath = path.join(__dirname, 'builds', bindingFolder, 'addon_iec60870.node');

if (!fs.existsSync(bindingPath)) {
  throw new Error(`Native binding not found at: ${bindingPath}`);
}

const binding = require(bindingPath);
module.exports = binding;