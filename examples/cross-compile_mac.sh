#!/bin/bash

# Check for node-gyp
if ! command -v node-gyp &> /dev/null; then
    echo "node-gyp не установлен. Установите его с помощью 'npm install -g node-gyp'"
    exit 1
fi

# Specify Node.js version
NODE_VERSION=${1:-"23.9.0"}
NODE_DIR="$HOME/.cache/node-gyp/$NODE_VERSION"

# Check NODE_DIR existence and attempt to create it
if [ ! -d "$NODE_DIR" ]; then
    echo "NODE_DIR ($NODE_DIR) не существует. Запускаем node-gyp configure для загрузки..."
    node-gyp configure --target="$NODE_VERSION" --verbose
    if [ ! -d "$NODE_DIR/include/node" ]; then
        echo "Ошибка: не удалось создать $NODE_DIR или скачать заголовки."
        echo "Попробуем скачать вручную..."
        mkdir -p "$NODE_DIR"
        curl -o headers.tar.gz "https://nodejs.org/dist/v$NODE_VERSION/node-v$NODE_VERSION-headers.tar.gz" || {
            echo "Не удалось скачать заголовки. Проверьте сеть."
            exit 1
        }
        tar -xzf headers.tar.gz -C "$NODE_DIR" --strip-components=1 || {
            echo "Не удалось распаковать заголовки."
            exit 1
        }
        rm headers.tar.gz
        if [ ! -d "$NODE_DIR/include/node" ]; then
            echo "Ошибка: заголовки не установлены в $NODE_DIR"
            exit 1
        fi
    fi
fi

# Directories for build outputs and libraries
OUTPUT_DIR="$(pwd)/builds"
LIB_DIR="$(pwd)/lib/build"
mkdir -p "$OUTPUT_DIR"

# Clean previous build
clean_build() {
    echo "Очистка предыдущей сборки..."
    #node-gyp clean
    #rm -rf ./build
}

# Compile function (macOS ARM64 only)
compile_target() {
    local target_os="mac"
    local target_arch="arm64"
    local output_name="macos_arm64"
    local cc="/usr/bin/clang"
    local cxx="/usr/bin/clang++"
    local link="/usr/bin/clang++"
    local lib_name="lib60870_darwin_arm64.a"

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

# Build for macOS ARM64
clean_build
compile_target

echo "Компиляция завершена! Результаты находятся в $OUTPUT_DIR"