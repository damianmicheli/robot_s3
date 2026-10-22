file(GLOB_RECURSE srcs "esphome/*.cpp")

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "." "include"
                 "include/esphome/core"
                 "include/esphome/components/micro_wake_word"
                 "include/esphome/components/microphone"
    REQUIRES "esp_common"
             "esp_driver_i2s"
             "esp_system"
             "esp_timer"
             "esp_rom"
             "freertos"
             "log"
             "micro_speech_features"
             "espressif__esp-tflite-micro"
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC USE_ESP32 USE_ESP_IDF)
