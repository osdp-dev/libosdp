#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

ifeq ($(wildcard config.make),)
$(error run ./configure.sh first. See ./configure.sh -h)
endif

include config.make

O ?= $(BUILD_DIR)
OBJ_LIBOSDP := $(SRC_LIBOSDP:%.c=$(O)/%.o)
OBJ_TEST := $(SRC_TEST:%.c=$(O)/check/%.o)
OBJ_CP_APP := $(O)/examples/c/cp_app.o
OBJ_PD_APP := $(O)/examples/c/pd_app.o
DEP_LIBOSDP := $(OBJ_LIBOSDP:.o=.d)
DEP_TEST := $(OBJ_TEST:.o=.d)
DEP_APP := $(OBJ_CP_APP:.o=.d) $(OBJ_PD_APP:.o=.d)
CCFLAGS += -Wall -Wextra -O3

ifeq ($(V),)
MAKE := make -s
Q := @
else
Q :=
MAKE := make
endif

.PHONY: all
all: libosdp $(TARGETS)

.PHONY: libosdp
libosdp: $(O)/libosdp.a $(O)/libosdp.pc

.PHONY: pd_app
pd_app: $(O)/pd_app.elf

.PHONY: cp_app
cp_app: $(O)/cp_app.elf

$(O)/%.o: %.c
	@echo "  CC $<"
	@mkdir -p $(@D)
	$(Q)$(CC) -c $< $(CCFLAGS) $(CCFLAGS_EXTRA) -MMD -MP -o $@

$(O)/check/%.o: %.c
	@echo "  CC $<"
	@mkdir -p $(@D)
	$(Q)$(CC) -c $< $(CCFLAGS) $(CCFLAGS_EXTRA) -MMD -MP -o $@

$(O)/libosdp.a: CCFLAGS_EXTRA=-Iutils/include -Iinclude -Isrc -I$(O)
$(O)/libosdp.a: $(OBJ_LIBOSDP)
	@echo "  AR $(@F)"
	$(Q)$(AR) qc $@ $^

## Samples

$(O)/examples/c/%.o: CCFLAGS_EXTRA=-Iinclude

$(O)/cp_app.elf: $(O)/libosdp.a $(OBJ_CP_APP)
	@echo "LINK $(@F)"
	$(Q)$(CC) $(CCFLAGS) $(OBJ_CP_APP) -o $@ -L$(O) -losdp $(LDFLAGS)

$(O)/pd_app.elf: $(O)/libosdp.a $(OBJ_PD_APP)
	@echo "LINK $(@F)"
	$(Q)$(CC) $(CCFLAGS) $(OBJ_PD_APP) -o $@ -L$(O) -losdp $(LDFLAGS)

## Tests

.PHONY: unit-test
# OSDP_PD_ONLINE_TOUT_MS override mirrors tests/unit-tests/CMakeLists.txt:
# link-loss tests budget a 4000ms wait window, so the 8s production timeout
# would guarantee failure.
unit-test: CCFLAGS_EXTRA=-DUNIT_TESTING -DOSDP_PD_ONLINE_TOUT_MS=1500 -Iutils/include -Iinclude -Isrc -I$(O)
unit-test: $(O)/unit-test

$(O)/unit-test: $(OBJ_TEST)
	@echo "LINK $(@F)"
	$(Q)$(CC) $(CCFLAGS) $(OBJ_TEST) -o $@ $(LDFLAGS)

.PHONY: check
check: unit-test
	$(Q)$(O)/unit-test

## Clean

.PHONY: clean
clean:
	$(Q)rm -f $(O)/src/*.o $(O)/src/crypto/*.o $(OBJ_TEST) $(OBJ_CP_APP) $(OBJ_PD_APP)
	$(Q)rm -rf $(O)/check
	$(Q)rm -f $(O)/*.a $(O)/*.elf

.PHONY: distclean
distclean: clean
	$(Q)rm config.make
	$(Q)rm -rf $(O)

-include $(DEP_LIBOSDP) $(DEP_TEST) $(DEP_APP)

## Install

.PHONY: install
install: libosdp
	install -d $(DESTDIR)$(PREFIX)/lib/
	install -m 644 $(O)/libosdp.a $(DESTDIR)$(PREFIX)/lib/
	install -d $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m 644 $(O)/libosdp.pc $(DESTDIR)$(PREFIX)/lib/pkgconfig/
	install -d $(DESTDIR)$(PREFIX)/include/libosdp/
	install -m 644 include/osdp.h $(DESTDIR)$(PREFIX)/include/libosdp/
	install -m 644 include/osdp.hpp $(DESTDIR)$(PREFIX)/include/libosdp/
	install -m 644 include/osdp_export.h $(DESTDIR)$(PREFIX)/include/libosdp/
