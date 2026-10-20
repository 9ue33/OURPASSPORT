idf_component_register(
    SRCS "main.c"
         "couple_app.c"
         "couple_audio.c"
         "couple_net.c"
         "couple_proto.c"
         "couple_prov.c"
         "couple_radio.c"
         "couple_sha256.c"
         "couple_state.c"
         "couple_store.c"
         "couple_store_blob.c"
         "couple_tone.c"
         "couple_ui.c"
         "couple_wifi.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_http_client esp_http_server esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
)

# 中文字体：assets/fonts/ 下是 lv_font_conv 生成物（见 assets/README 与 tools/），
# 它们定义的是 lv_font_t 常量而不是头文件，所以只能当源文件编进来，不能 #include。
# couple_fonts.h 靠下面的宏在「子集中文字体」和「退回 Montserrat」之间切换。
target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_16.c")
target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_20.c")
target_compile_definitions(${COMPONENT_LIB} PRIVATE COUPLE_USE_APP_FONT=1)