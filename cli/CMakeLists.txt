# The shell borrows terminal handling from ESP-IDF's `console` component --
# linenoise for line editing and esp_console_split_argv() as the tokenizer --
# but not its command registry (flat, no grouping) or esp_console_start_repl()
# (owns the reader task and dispatches directly, which would leave a line
# arriving over a WebSocket nowhere to go).
idf_component_register(
    SRCS         "src/cli_dispatch.c"
                 "src/cli_repl.c"
                 "src/cli_io.c"
                 "src/cli_args.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES     esp_common          # esp_err_t in the public header
    PRIV_REQUIRES console            # linenoise + esp_console_split_argv
                 diag
                 freertos
                 esp_driver_uart
                 esp_driver_usb_serial_jtag)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
