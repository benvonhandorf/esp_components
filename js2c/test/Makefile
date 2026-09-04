# Host test for js2c. Plain gcc: the generated parsers depend only on jsmn and
# js2c_builtins.h, so this needs no ESP-IDF, no toolchain and no hardware -- and it
# compiles the real generated sources rather than a copy of them.
#
#   make -C js2c/test
#
COMP    := $(abspath $(CURDIR)/..)
TOOL    := $(COMP)/tool/json_schema_to_c.py
GEN     := $(CURDIR)/generated
FIX     := $(CURDIR)/fixtures
PYTHON  ?= python3

# Each test gets only its own generated directory on the include path: both
# pipelines produce an app_config.h, and sharing -I would let one shadow the other.
BASE_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE \
               -I$(COMP)/include -include js2c_error_capture.h
CFLAGS  := $(BASE_CFLAGS) -I$(GEN)

# Sourcing ESP-IDF's export.sh (or an idf profile line in ~/.bashrc) puts the xtensa,
# riscv32 and esp32ulp toolchains ahead of /usr/bin on PATH, and their `ld` cannot
# link a host binary -- "unrecognised emulation mode: elf_x86_64". -B names the
# directory to take `ld` from, so this builds correctly whatever PATH looks like.
LDFLAGS := -B/usr/bin
SRCS    := test_js2c.c $(COMP)/src/js2c_error_capture.c \
           $(GEN)/net_config.c $(GEN)/app_config.c

.PHONY: all clean check-runtime-identical run run-sections
all: check-runtime-identical run run-sections

$(GEN)/net_config.c $(GEN)/net_config.h: $(FIX)/net_config_schema.json $(TOOL)
	@mkdir -p $(GEN)
	$(PYTHON) $(TOOL) --include-external-builtins-file "js2c_builtins.h" \
	    $(FIX)/net_config_schema.json $(GEN)/net_config.c $(GEN)/net_config.h \
	    --authorized-paths $(FIX)

# --allow-additional-properties leaves token space for keys the schema does not
# name. It is a global switch, but this schema has exactly one object (every
# section is a "raw" scalar), so it cannot loosen anything nested.
$(GEN)/app_config.c $(GEN)/app_config.h: $(FIX)/app_config_schema.json $(FIX)/shared_defs.json $(TOOL)
	@mkdir -p $(GEN)
	$(PYTHON) $(TOOL) --include-external-builtins-file "js2c_builtins.h" \
	    --allow-additional-properties 32 \
	    $(FIX)/app_config_schema.json $(GEN)/app_config.c $(GEN)/app_config.h \
	    --authorized-paths $(FIX)

test_js2c: $(GEN)/net_config.h $(GEN)/app_config.h $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@

run: test_js2c
	./test_js2c

# --- the sections pipeline -------------------------------------------------
# js2c_sections.py derives a top-level walker from the authored config schema and
# an X-macro over its sections; each component's fragment generates its own parser.
SECT     := $(CURDIR)/fixtures/sections
SECT_GEN := $(GEN)/sections
SECTIONS := $(COMP)/tool/js2c_sections.py

$(SECT_GEN)/app_config_sections.json $(SECT_GEN)/app_config_sections.h: \
		$(SECT)/app_config_schema.json \
		$(SECT)/wifi_manager/wifi_config_schema.json \
		$(SECT)/mqtt_manager/mqtt_config_schema.json $(SECTIONS)
	@mkdir -p $(SECT_GEN)
	$(PYTHON) $(SECTIONS) $(SECT)/app_config_schema.json \
	    $(SECT_GEN)/app_config_sections.json $(SECT_GEN)/app_config_sections.h \
	    --authorized-paths $(SECT)

# The token budget rides in the derived schema's js2cSettings, so no flag here.
$(SECT_GEN)/app_config.c $(SECT_GEN)/app_config.h: $(SECT_GEN)/app_config_sections.json $(TOOL)
	$(PYTHON) $(TOOL) --include-external-builtins-file "js2c_builtins.h" \
	    $< $(SECT_GEN)/app_config.c $(SECT_GEN)/app_config.h --authorized-paths $(SECT)

# A component generates its own parser from its own fragment, exactly as it would
# inside its own CMakeLists -- the aggregate is not involved.
define component_parser
$(SECT_GEN)/$(1)_config.c $(SECT_GEN)/$(1)_config.h: $(SECT)/$(1)_manager/$(1)_config_schema.json $(TOOL)
	@mkdir -p $(SECT_GEN)
	$(PYTHON) $(TOOL) --include-external-builtins-file "js2c_builtins.h" \
	    $(SECT)/$(1)_manager/$(1)_config_schema.json \
	    $(SECT_GEN)/$(1)_config.c $(SECT_GEN)/$(1)_config.h --authorized-paths $(SECT)
endef
$(eval $(call component_parser,wifi))
$(eval $(call component_parser,mqtt))

SECT_SRCS := test_sections.c $(COMP)/src/js2c_error_capture.c \
             $(SECT_GEN)/app_config.c $(SECT_GEN)/wifi_config.c $(SECT_GEN)/mqtt_config.c

SECT_CFLAGS := $(BASE_CFLAGS) -I$(SECT_GEN)

test_sections: $(SECT_GEN)/app_config.h $(SECT_GEN)/app_config_sections.h \
               $(SECT_GEN)/wifi_config.h $(SECT_GEN)/mqtt_config.h $(SECT_SRCS)
	$(CC) $(SECT_CFLAGS) $(LDFLAGS) $(SECT_SRCS) -o $@

run-sections: test_sections
	./test_sections

# The generator and its runtime headers are a matched pair with no version check,
# so the component's copies must be the generator's own. A predecessor project let
# these drift to a different upstream jsmn and only got away with it by luck.
check-runtime-identical:
	@cmp $(COMP)/include/js2c_builtins.h $(COMP)/tool/js2c/codegen/js2c_builtins.h
	@cmp $(COMP)/include/jsmn.h          $(COMP)/tool/js2c/codegen/jsmn.h
	@echo "runtime headers identical to the vendored generator's"

clean:
	rm -rf $(GEN) test_js2c test_sections
