#!/usr/bin/env bash
# Helper: activate ESP-IDF 5.5.2 toolchain for this project (Git Bash)
export IDF_PATH='E:/ESP-IDF/Espressif/frameworks/esp-idf-v5.5.2'
export IDF_TOOLS_PATH='E:/ESP-IDF/Espressif'
export IDF_PYTHON_ENV_PATH='E:/ESP-IDF/Espressif/tools/python_env/idf5.5_py3.11_env'
export ESP_ROM_ELF_DIR='E:/ESP-IDF/Espressif/tools/esp-rom-elfs/20241011'

PYENV_S="/e/ESP-IDF/Espressif/tools/python_env/idf5.5_py3.11_env/Scripts"
CMAKE_B="/e/ESP-IDF/Espressif/tools/cmake/3.30.2/bin"
NINJA_D="/e/ESP-IDF/Espressif/tools/ninja/1.12.1"
XTENSA_B="/e/ESP-IDF/Espressif/tools/tools/xtensa-esp-elf/esp-14.2.0_20251107/xtensa-esp-elf/bin"

export PATH="$PYENV_S:$CMAKE_B:$NINJA_D:$XTENSA_B:$PATH"
export IDF_PY="$PYENV_S/python.exe"

# Git Bash sets MSYSTEM, which causes idf.py to print a warning and skip main().
# Strip it at exec time (the harness re-injects it into the environment).
export IDF_PY_CLEAN="env -u MSYSTEM -u MSYS $IDF_PY"
