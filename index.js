const path = require('path');
const os = require('os');
const fs = require('fs');

function getNativeBinding() {
  const platform = os.platform();
  const arch = os.arch();

  const bindingsMap = {
    'win32_x64': 'win64',
   // 'win32_ia32': 'win32',
    'darwin_x64': 'macos_x64',
    'darwin_arm64': 'macos_arm64',
    'linux_x64': 'linux_x64',
    'linux_arm64': 'linux_arm64',
    'linux_arm': 'linux_arm'
  };

  const key = `${platform}_${arch}`;
  const bindingFolder = bindingsMap[key];

  if (!bindingFolder) {
    throw new Error(`Unsupported platform/architecture: ${platform}/${arch}`);
  }

  const bindingPath = path.join(__dirname, '..', 'builds', bindingFolder, 'addon_iec60870.node');

  if (!fs.existsSync(bindingPath)) {
    throw new Error(`Native binding not found at ${bindingPath}`);
  }

  try {
    return require(bindingPath);
  } catch (error) {
    throw new Error(`Failed to load native binding from ${bindingPath}: ${error.message}`);
  }
}

class Example {
  constructor(value) {
    this.binding = getNativeBinding();
    this.instance = new this.binding.Example(value);
  }

  getPlatform() {
    return this.instance.getPlatform();
  }

  add(value) {
    return this.instance.add(value);
  }
}

module.exports = {
  Example
};