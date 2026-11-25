# SPDX-License-Identifier: AGPL-3.0-or-later
APP = demo
SRC = demo.c fe.c util.c r3d.c stars.c solid.c fx.c
EXTRA =
# speed over size: the inner loops are the whole point
CFLAGS = -O2 -fno-stack-protector
include $(T48_SDK)/share/t48/app.mk

# how long each part takes to draw, for running on the phone by hand (see bench.c)
PARTS = util.c r3d.c stars.c solid.c fx.c
bench: $(O)/arm/demo-bench
$(O)/arm/demo-bench: bench.c $(PARTS) $(wildcard *.h)
	@mkdir -p $(@D)
	$(ARM_CC) -O2 -fno-stack-protector $(T48_FLAGS) -static -s -o $@ bench.c $(PARTS) -lt48 -lm
$(O)/host/demo-bench: bench.c $(PARTS) $(wildcard *.h) $(HOST_LIB)
	@mkdir -p $(@D)
	$(HOST_CC) -O2 -g $(T48_FLAGS) -I$(T48_SHARE)/libt48/include -o $@ bench.c $(PARTS) $(HOST_LIB) -lm
.PHONY: bench
