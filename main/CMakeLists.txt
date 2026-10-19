file(GLOB_RECURSE SOURCES
    "*.cc"
    "*.cpp"
)

# 音效资源：符号名由文件基名决定（_binary_<name>_ogg_start/end），
# 与 main/assets/lang_config.h 中的声明保持一致。
set(SOUND_FILES
    "assets/sounds/0.ogg"
    "assets/sounds/1.ogg"
    "assets/sounds/2.ogg"
    "assets/sounds/3.ogg"
    "assets/sounds/4.ogg"
    "assets/sounds/5.ogg"
    "assets/sounds/6.ogg"
    "assets/sounds/7.ogg"
    "assets/sounds/8.ogg"
    "assets/sounds/9.ogg"
    "assets/sounds/activation.ogg"
    "assets/sounds/err_pin.ogg"
    "assets/sounds/err_reg.ogg"
    "assets/sounds/upgrade.ogg"
    "assets/sounds/welcome.ogg"
    "assets/sounds/wificonfig.ogg"
    "assets/sounds/exclamation.ogg"
    "assets/sounds/low_battery.ogg"
    "assets/sounds/popup.ogg"
    "assets/sounds/success.ogg"
    "assets/sounds/vibration.ogg"
)

set(INCLUDE_DIRS
    "."
    "assets"
    "audio"
    "audio/codes"
    "audio/processors"
    "audio/wake_words"
    "boards"
    "boards/common"
    "boards/bread-compact-wifi-lcd"
    "display"
    "drivers"
    "protocols"
    "protocols/include"
    "wifi"
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    EMBED_FILES
        "wifi/wifi_configuration.html"
        "wifi/wifi_configuration_done.html"
        ${SOUND_FILES}
)

# Board identity, referenced by WifiBoard::GetBoardJson()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BOARD_TYPE="bread-compact-wifi-lcd"
    BOARD_NAME="bread-compact-wifi-lcd"
)
