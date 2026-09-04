# No filesystem dependency on purpose: this component takes paths and uses stdio,
# so the application chooses and mounts the filesystem. That keeps a LittleFS (or
# FAT, or SD) dependency out of every project that stores a config file, and lets
# the file handling be tested on the host against real files.
idf_component_register(
    SRCS         "src/config_store.c"
                 "src/config_store_restart.c"
    INCLUDE_DIRS "include"
    REQUIRES     esp_common          # esp_err_t in the public header
    PRIV_REQUIRES esp_timer          # the deferred restart
                  esp_system)        # esp_restart

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
