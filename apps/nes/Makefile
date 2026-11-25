# SPDX-License-Identifier: AGPL-3.0-or-later
APP = nes
CORE = cpu.c ppu.c apu.c cart.c nes.c
SRC = main.c video.c sound.c $(CORE)
# the games that come with it, and their licences (roms/README.md)
EXTRA = $(wildcard roms/*.nes) $(wildcard roms/*.txt)
CFLAGS = -O2
# a whole path: the simulator hands the data folder to the app as it was given,
# and the app runs from its own folder
O = $(CURDIR)/build
# tests/sources.script makes a pretend USB stick and lists it here
export NES_MOUNTS = $(O)/mounts
include $(T48_SDK)/share/t48/app.mk

# nesrun: the core without the phone, for measuring and for test ROMs
#   make nesrun     build/nesrun for this PC and build/nesrun-arm for the phone
nesrun: $(O)/nesrun $(O)/nesrun-arm

$(O)/nesrun: nesrun.c $(CORE) $(wildcard *.h)
	@mkdir -p $(@D)
	$(HOST_CC) -O2 -g $(T48_FLAGS) -o $@ nesrun.c $(CORE) -lm

$(O)/nesrun-arm: nesrun.c $(CORE) $(wildcard *.h)
	@mkdir -p $(@D)
	$(ARM_CC) -O2 $(T48_FLAGS) -static -s -o $@ nesrun.c $(CORE) -lm

.PHONY: nesrun

# the same on a T46S's screen: 480x272, no touch, soft keys
check: check-small
check-small: $(O)/host/$(APP)/$(APP)
	@for t in tests/small/*.script; do echo "== $$t"; rm -rf $(O)/sim; mkdir -p $(O)/sim/apps; \
		$(SIM) --model t46s --app $(APP) --script $$t || exit 1; done

.PHONY: check-small
