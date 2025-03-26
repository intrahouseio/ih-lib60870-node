const { execSync } = require('child_process');
const path = require('path');

// Список поддерживаемых платформ и архитектур
const builds = [
  { platform: 'win32', arch: 'x64' },
  { platform: 'darwin', arch: 'x64' },
  { platform: 'darwin', arch: 'arm64' },
  { platform: 'linux', arch: 'x64' },
  { platform: 'linux', arch: 'arm64' },
  { platform: 'linux', arch: 'arm' }
];

// Путь к prebuild-install
const prebuildInstall = path.join(__dirname, 'node_modules', '.bin', 'prebuild-install');

// Функция для выполнения prebuild-install с заданными параметрами
function downloadBuild(platform, arch) {
  try {
    console.log(`Downloading build for ${platform}/${arch}...`);
    execSync(`${prebuildInstall} --platform ${platform} --arch ${arch} --verbose`, { stdio: 'inherit' });
  } catch (error) {
    console.error(`Failed to download ${platform}/${arch}: ${error.message}`);
  }
}

// Загрузка всех билдов
for (const { platform, arch } of builds) {
  downloadBuild(platform, arch);
}

console.log('All builds downloaded successfully.');