GBDK_HOME ?= .experiment/gbdk
SNAP_DIRECTORY ?= GBCSYS/SNAP
LCC := $(GBDK_HOME)/bin/lcc
CFLAGS := -Isrc -Ivendor/petitfatfs -Wf--max-allocs-per-node -Wf50000
CPPFLAGS := -DSNAP_DIRECTORY=$(SNAP_DIRECTORY)
SOURCES := src/main.c src/x7_disk.c src/checksum.c vendor/petitfatfs/pff.c

.PHONY: all build/x7-read-diagnostic.gb build/x7-visual.gb build/visual-demo.gb
all: build/x7-visual.gb build/visual-demo.gb

build/x7-read-diagnostic.gb: $(SOURCES) src/x7_io.h src/checksum.h vendor/petitfatfs/pff.h vendor/petitfatfs/diskio.h vendor/petitfatfs/pffconf.h
	mkdir -p build
	$(LCC) $(CPPFLAGS) $(CFLAGS) -Wl-yt0x19 -Wl-yo4 -Wm-ynX7READ -Wm-yc -Wl-m -Wl-j -o $@ $(SOURCES)

build/x7-visual.gb: src/visual_sender.c src/visual_grid.c src/x7_disk.c src/checksum.c vendor/petitfatfs/pff.c src/visual_grid.h src/x7_io.h src/checksum.h vendor/petitfatfs/pff.h vendor/petitfatfs/diskio.h vendor/petitfatfs/pffconf.h
	mkdir -p build
	$(LCC) $(CFLAGS) -Wl-yt0x19 -Wl-yo4 -Wm-ynX7VISUAL -Wm-yc -Wl-m -Wl-j -o $@ src/visual_sender.c src/visual_grid.c src/x7_disk.c src/checksum.c vendor/petitfatfs/pff.c

build/visual-demo.gb: src/visual_demo.c src/visual_grid.c src/checksum.c src/visual_grid.h src/checksum.h
	mkdir -p build
	$(LCC) $(CFLAGS) -Wl-yt0x19 -Wl-yo2 -Wm-ynVISUALDEMO -Wm-yc -Wl-m -Wl-j -o $@ src/visual_demo.c src/visual_grid.c src/checksum.c
