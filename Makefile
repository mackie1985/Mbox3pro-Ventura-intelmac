# SPDX-FileCopyrightText: 2026 Mackie
# SPDX-License-Identifier: LGPL-3.0-or-later
# Pure-logic layer builds and tests on any host (macOS / Linux).
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g

.PHONY: test clean
test: build/test_params build/test_dice
	./build/test_params
	./build/test_dice

build/test_dice: tests/test_dice.c src/dice.c src/dice.h src/mbox3_params.c src/mbox3_params.h src/fw_transport.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_dice.c src/dice.c src/mbox3_params.c

build/test_params: tests/test_params.c src/mbox3_params.c src/mbox3_params.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_params.c src/mbox3_params.c

# macOS only: read-only hardware probe.
.PHONY: probe
probe: build/dice_probe

build/dice_probe: tools/dice_probe.c src/mbox3_params.c src/mbox3_params.h
	@mkdir -p build
	$(CC) -std=gnu11 -Wall -Wextra -O1 -g -o $@ tools/dice_probe.c src/mbox3_params.c \
	  -framework IOKit -framework CoreFoundation

# macOS only: control tool (app parameters + mixer coefficients).
.PHONY: ctl
ctl: build/dice_ctl

build/dice_ctl: tools/dice_ctl.c lib/dice_io.c lib/dice_io.h tools/dice_iso.c tools/dice_iso.h src/mbox3_params.c src/mbox3_params.h
	@mkdir -p build
	$(CC) -std=gnu11 -Wall -Wextra -O1 -g -Ilib -o $@ tools/dice_ctl.c lib/dice_io.c tools/dice_iso.c src/mbox3_params.c \
	  -framework IOKit -framework CoreFoundation

# macOS only: libmbox3 (static library) and its demo.
.PHONY: lib demo
lib: build/libmbox3.a
demo: build/mbox3_demo

build/mbox3.o: lib/mbox3.c lib/mbox3.h lib/dice_io.h src/mbox3_params.h
	@mkdir -p build
	$(CC) -std=gnu11 -Wall -Wextra -O2 -g -Ilib -c -o $@ lib/mbox3.c

build/dice_io.o: lib/dice_io.c lib/dice_io.h
	@mkdir -p build
	$(CC) -std=gnu11 -Wall -Wextra -O2 -g -Ilib -c -o $@ lib/dice_io.c

build/mbox3_params.o: src/mbox3_params.c src/mbox3_params.h
	@mkdir -p build
	$(CC) -std=gnu11 -Wall -Wextra -O2 -g -c -o $@ src/mbox3_params.c

build/libmbox3.a: build/mbox3.o build/dice_io.o build/mbox3_params.o
	ar rcs $@ build/mbox3.o build/dice_io.o build/mbox3_params.o

build/mbox3_demo: tools/mbox3_demo.c build/libmbox3.a lib/mbox3.h
	$(CC) -std=gnu11 -Wall -Wextra -O2 -g -Ilib -o $@ tools/mbox3_demo.c build/libmbox3.a \
	  -framework IOKit -framework CoreFoundation -lm

# macOS only: the daemon, the Audio Server plug-in, and the IPC check tool.
.PHONY: daemon driver shmcheck pkg
daemon: build/mbox3d
driver: build/Mbox3.driver
shmcheck: build/m3_shm_check

build/mbox3d: daemon/mbox3d.c daemon/ctlserver.c daemon/ctlserver.h driver/proto.h build/libmbox3.a lib/mbox3.h
	$(CC) -std=gnu11 -Wall -Wextra -O2 -g -Ilib -Idaemon -o $@ daemon/mbox3d.c daemon/ctlserver.c build/libmbox3.a \
	  -framework IOKit -framework CoreFoundation

build/m3_shm_check: tools/m3_shm_check.c driver/proto.h
	@mkdir -p build
	$(CC) -std=gnu11 -Wall -Wextra -O2 -g -o $@ tools/m3_shm_check.c -lm

build/Mbox3.driver: driver/Mbox3Driver.c driver/proto.h driver/Info.plist
	rm -rf $@
	mkdir -p $@/Contents/MacOS
	$(CC) -std=gnu11 -Wall -Wextra -O2 -g -bundle -o $@/Contents/MacOS/Mbox3Driver driver/Mbox3Driver.c \
	  -framework CoreAudio -framework CoreFoundation
	cp driver/Info.plist $@/Contents/Info.plist
	codesign --force --sign - $@

# macOS only: an installer package (Intel binaries), build/Mbox3pro-Ventura-intelmac-<version>.pkg
pkg:
	scripts/make_pkg.sh

clean:
	rm -rf build
