#!/bin/bash

# Check for node-gyp
if ! command -v node-gyp &> /dev/null; then
    echo "node-gyp не установлен. Установите его с помощью 'npm install -g node-gyp'"
    exit 1
fi

# Specify Node.js version
NODE_VERSION=${1:-"22.14.0"}
NODE_DIR="$HOME/.cache/node-gyp/$NODE_VERSION"

# Check NODE_DIR existence
if [ ! -d "$NODE_DIR" ]; then
    echo "NODE_DIR ($NODE_DIR) не существует. Запускаем node-gyp configure для загрузки..."
    node-gyp configure --target="$NODE_VERSION"
    if [ ! -d "$NODE_DIR" ]; then
        echo "Ошибка: не удалось создать $NODE_DIR"
        exit 1
    fi
fi

# Directories for build outputs and libraries
OUTPUT_DIR="$(pwd)/builds"
LIB_DIR="$(pwd)/lib/build"
mkdir -p "$OUTPUT_DIR"

# Clean previous build
clean_build() {
    echo "Очистка предыдущей сборки..."
    node-gyp clean
    rm -rf ./build
}

# Compile function
compile_target() {
    local target_os=$1
    local target_arch=$2
    local output_name=$3
    local cc=$4
    local cxx=$5
    local link=$6
    local lib_name=$7

    echo "Компиляция для $target_os $target_arch..."
    export CC="$cc"
    export CXX="$cxx"
    export LINK="$link"

    local lib_path="$LIB_DIR/$lib_name"
    if [ ! -f "$lib_path" ]; then
        echo "Ошибка: библиотека $lib_path не найдена!"
        exit 1
    fi

    node-gyp configure --nodedir="$NODE_DIR" --target="$NODE_VERSION" --arch="$target_arch" --dest-os="$target_os" --verbose
    node-gyp build --verbose

    mkdir -p "$OUTPUT_DIR/$output_name"
    if [ -f "./build/Release/addon_iec60870.node" ]; then
        cp "./build/Release/addon_iec60870.node" "$OUTPUT_DIR/$output_name/addon_iec60870.node"
    else
        echo "Предупреждение: файл addon_iec60870.node не найден после сборки"
    fi

    clean_build
}

# 1. Linux x64
clean_build
compile_target "linux" "x64" "linux_x64" \
    "/usr/bin/gcc" \
    "/usr/bin/g++" \
    "/usr/bin/g++" \
    "lib60870_linux_x64.a"

# 2. Linux ARM64
clean_build
compile_target "linux" "arm64" "linux_arm64" \
    "/usr/bin/aarch64-linux-gnu-gcc" \
    "/usr/bin/aarch64-linux-gnu-g++" \
    "/usr/bin/aarch64-linux-gnu-g++" \
    "lib60870_linux_arm64.a"

# 3. Linux ARM (armv7l)
clean_build
compile_target "linux" "arm" "linux_arm" \
    "/usr/bin/arm-linux-gnueabihf-gcc" \
    "/usr/bin/arm-linux-gnueabihf-g++" \
    "/usr/bin/arm-linux-gnueabihf-g++" \
    "lib60870_linux_arm.a"

echo "Кросс-компиляция завершена! Результаты находятся в $OUTPUT_DIR"