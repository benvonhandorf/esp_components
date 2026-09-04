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

CFLAGS  := -std=gnu11 -O1 -g -Wall -Wextra -Werror \
           -I$(COMP)/include -I$(GEN) -include js2c_error_capture.h

# Sourcing ESP-IDF's export.sh (or an idf profile line in ~/.bashrc) puts the xtensa,
# riscv32 and esp32ulp toolchains ahead of /usr/bin on PATH, and their `ld` cannot
# link a host binary -- "unrecognised emulation mode: elf_x86_64". -B names the
# directory to take `ld` from, so this builds correctly whatever PATH looks like.
LDFLAGS := -B/usr/bin
SRCS    := test_js2c.c $(COMP)/src/js2c_error_capture.c \
           $(GEN)/net_config.c $(GEN)/app_config.c

.PHONY: all clean check-runtime-identical
all: check-runtime-identical run

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

# The generator and its runtime headers are a matched pair with no version check,
# so the component's copies must be the generator's own. A predecessor project let
# these drift to a different upstream jsmn and only got away with it by luck.
check-runtime-identical:
	@cmp $(COMP)/include/js2c_builtins.h $(COMP)/tool/js2c/codegen/js2c_builtins.h
	@cmp $(COMP)/include/jsmn.h          $(COMP)/tool/js2c/codegen/jsmn.h
	@echo "runtime headers identical to the vendored generator's"

clean:
	rm -rf $(GEN) test_js2c
