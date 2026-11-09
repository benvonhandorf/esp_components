# The arithmetic and the descriptor builder (usb_mic_calc.c, usb_mic_desc.c) are plain
# C and build everywhere; the device (usb_mic.c) is compiled to stubs on a chip with
# no USB OTG controller, so a project can keep the command and report the chip.
#
# On a chip with OTG this component installs its own tusb_config.h into the TinyUSB
# component, the way espressif/usb_device_uac does -- see tusb/tusb_config.h.

idf_build_get_property(target IDF_TARGET)

set(priv_requires esp_hw_support freertos)

idf_component_register(
    SRCS         "src/usb_mic_calc.c" "src/usb_mic_desc.c" "src/usb_mic.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${priv_requires})

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

set(otg_targets esp32s2 esp32s3 esp32p4 esp32s31)
if(target IN_LIST otg_targets)
    # Local component, or the managed one: same lookup esp_tinyusb does.
    idf_build_get_property(build_components BUILD_COMPONENTS)
    if(tinyusb IN_LIST build_components)
        set(tinyusb_name tinyusb)
    else()
        set(tinyusb_name espressif__tinyusb)
    endif()
    idf_component_get_property(tusb_lib ${tinyusb_name} COMPONENT_LIB)
    target_include_directories(${tusb_lib} PUBLIC "${COMPONENT_DIR}/tusb")
endif()
