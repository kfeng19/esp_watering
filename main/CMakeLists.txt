idf_component_register(SRCS "my_rest.c" "main.c" "../../moisture/main/moisture.c" "../../led_blink/main/my_led.c" "my_rest.c" "../../server/main/my_wifi.c"
                    INCLUDE_DIRS "." "../../moisture/main" "../../led_blink/main/" "../../server/main/")

if(CONFIG_EXAMPLE_WEB_DEPLOY_SF)
    set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../front")
    message(WEB_SRC_DIR="${WEB_SRC_DIR}")
    if(EXISTS ${WEB_SRC_DIR}/index.html)
        message("Found index.html")
        spiffs_create_partition_image(www ${WEB_SRC_DIR} FLASH_IN_PROJECT)
    else()
        message(FATAL_ERROR "Can't find html")
    endif()
endif()