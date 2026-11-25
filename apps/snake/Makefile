# SPDX-License-Identifier: AGPL-3.0-or-later
APP = snake
SRC = snake.c sfx.c
LDLIBS = -pthread
# a full path, so the simulator hands the app a data folder it can find from its own folder
O = $(CURDIR)/build
include $(T48_SDK)/share/t48/app.mk

# every sound the game makes as a sound file, from the synth the phone runs: build/snake.wav
wav: $(O)/host/snake-wav
	$< $(O)/snake.wav
$(O)/host/snake-wav: wav.c sfx.c sfx.h
	@mkdir -p $(@D)
	$(HOST_CC) -O2 -g $(T48_FLAGS) -DSFX_ONLY_SYNTH -o $@ wav.c sfx.c -lm
.PHONY: wav

# the same on a T46S's screen: 480x272, no touch, soft keys
check: check-small
check-small: $(O)/host/$(APP)/$(APP)
	@for t in tests/small/*.script; do echo "== $$t"; rm -rf $(O)/sim; mkdir -p $(O)/sim/apps; \
		$(SIM) --model t46s --app $(APP) --script $$t || exit 1; done

.PHONY: check-small
