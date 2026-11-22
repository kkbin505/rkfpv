# Cross-build rkfpv against the Luckfox Pico SDK's prebuilt media libs.
#   make                 # target binary build/rkfpv
#   make test            # host unit test of the RTP packetizer
#   make SDK=/path/to/luckfox-pico

SDK      ?= $(abspath $(CURDIR)/../luckfox-pico)
TC       := $(SDK)/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin
CROSS    ?= $(TC)/arm-rockchip830-linux-uclibcgnueabihf-
CC       := $(CROSS)gcc

TRIPLE   := arm-rockchip830-linux-uclibcgnueabihf
ROCKIT   := $(SDK)/media/rockit/rockit
AIQ      := $(SDK)/media/isp/release_camera_engine_rkaiq_rv1106_$(TRIPLE)
MPP      := $(SDK)/media/mpp/release_mpp_rv1106_$(TRIPLE)
RGA      := $(SDK)/media/rga/release_rga_rv1106_$(TRIPLE)
# librkmuxer (DVR): the SDK recorder; on the board it is /oem/usr/lib/librkmuxer.so
MUXER    := $(SDK)/media/common_algorithm/common_algorithm/misc

AIQ_INC  := $(AIQ)/include/rkaiq
INCS     := -I$(ROCKIT)/mpi/sdk/include \
	    -I$(AIQ_INC) -I$(AIQ_INC)/uAPI2 -I$(AIQ_INC)/common -I$(AIQ_INC)/xcore \
	    -I$(AIQ_INC)/algos -I$(AIQ_INC)/iq_parser -I$(AIQ_INC)/iq_parser_v2 \
	    -I$(AIQ_INC)/smartIr -I$(MUXER)/include

CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu99 -Wall -Wextra -Wno-unused-parameter \
	    -DRV1106_RV1103 -DUAPI2 -DISP_HW_V30 -DRKPLATFORM=ON \
	    -march=armv7-a -mfpu=neon -mfloat-abi=hard $(INCS)
LDFLAGS  += -L$(ROCKIT)/lib/lib32 -L$(AIQ)/lib -L$(MPP)/lib -L$(RGA)/lib \
	    -L$(MUXER)/lib/$(TRIPLE) \
	    -Wl,-rpath-link,$(AIQ)/lib:$(MPP)/lib:$(RGA)/lib:$(ROCKIT)/lib/lib32 \
	    -Wl,-rpath,/oem/usr/lib
# rpath: on the board the Rockchip libs live in /oem/usr/lib, which the
# uClibc loader does not search (only RkEnv.sh's LD_LIBRARY_PATH adds it)
LDLIBS   := -lrockit -lrkaiq -lrockchip_mpp -lrga -lrkmuxer -lpthread -lm -lstdc++

SRCS     := src/main.c src/config.c src/isp.c src/rtp_h265.c src/dvr.c src/dvr_rkmuxer.c src/osd.c src/osd_layout.c src/telem.c src/preview.c
OBJS     := $(SRCS:src/%.c=build/%.o)

all: build/rkfpv build/mavbridge build/fpvd build/dvrhttpd

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/rkfpv: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# no SDK libs needed
build/mavbridge: src/mavbridge.c | build
	$(CC) -O2 -std=gnu99 -Wall -Wextra -march=armv7-a -mfpu=neon -mfloat-abi=hard -o $@ $<

build/dvrhttpd: src/dvrhttpd.c | build
	$(CC) -O2 -std=gnu99 -Wall -Wextra -march=armv7-a -mfpu=neon -mfloat-abi=hard -o $@ $<

build/fpvd: src/fpvd.c | build
	$(CC) -O2 -std=gnu99 -Wall -Wextra -march=armv7-a -mfpu=neon -mfloat-abi=hard -o $@ $<

build:
	mkdir -p build

# ---- host tests ----
build-host:
	mkdir -p build-host

build-host/test_rtp: test/test_rtp_h265.c src/rtp_h265.c | build-host
	gcc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o $@ $^

build-host/send_file: test/send_file.c src/rtp_h265.c | build-host
	gcc -Wall -Wextra -O2 -o $@ $^

build-host/mavbridge: src/mavbridge.c | build-host
	gcc -Wall -Wextra -O1 -g -fsanitize=address,undefined -o $@ $<

build-host/test_dvr: test/test_dvr.c src/dvr.c src/dvr.h | build-host
	gcc -std=gnu99 -Wall -Wextra -Wno-format-truncation -O1 -g -fsanitize=address,undefined -pthread -o $@ test/test_dvr.c src/dvr.c

build-host/test_config: test/test_config.c src/config.c src/config.h | build-host
	gcc -std=gnu99 -Wall -Wextra -O1 -g -fsanitize=address,undefined -Isrc -o $@ test/test_config.c src/config.c

build-host/dvrhttpd: src/dvrhttpd.c | build-host
	gcc -std=gnu99 -Wall -Wextra -O1 -g -fsanitize=address,undefined -o $@ $<

# OSD drawing with the RGN calls stubbed (SDK headers only, no SDK libs)
build-host/test_osd: test/test_osd.c src/osd.c src/osd.h src/osd_font.h src/osd_layout.c src/telem.c src/config.c src/config.h | build-host
	gcc -std=gnu99 -Wall -Wextra -O1 -g -fsanitize=address,undefined -I$(ROCKIT)/mpi/sdk/include -o $@ test/test_osd.c src/osd.c src/osd_layout.c src/telem.c src/config.c

build-host/test_telem: test/test_telem.c src/telem.c src/telem.h | build-host
	gcc -std=gnu99 -Wall -Wextra -O1 -g -fsanitize=address,undefined -o $@ test/test_telem.c src/telem.c

build-host/test_preview: test/test_preview.c src/preview.c src/preview.h | build-host
	gcc -std=gnu99 -Wall -Wextra -O1 -g -fsanitize=address,undefined -o $@ test/test_preview.c src/preview.c

build-host/fpvd: src/fpvd.c | build-host
	gcc -std=gnu99 -Wall -Wextra -O1 -g -fsanitize=address,undefined -o $@ $<

test: build-host/test_rtp build-host/send_file build-host/mavbridge build-host/fpvd build-host/test_dvr build-host/test_config build-host/dvrhttpd build-host/test_osd build-host/test_telem build-host/test_preview
	./build-host/test_rtp
	./build-host/test_dvr 2>/dev/null
	./build-host/test_preview 2>/dev/null | tail -1
	./build-host/test_config files/rkfpv.conf
	./build-host/test_osd build-host/osd.pgm > /dev/null && echo "osd drawing ok"
	test/check_osd_default.sh build-host/test_osd files/fpv
	python3 -I test/test_telem.py build-host/test_telem | tail -1
	python3 -I test/test_mavbridge.py build-host/mavbridge
	test/test_fpv.sh
	python3 -I test/test_fpvd.py build-host/fpvd files/fpv
	python3 -I test/test_switch.py build-host/fpvd | tail -1
	python3 -I test/test_dvrhttpd.py build-host/dvrhttpd | tail -1
	python3 -I test/test_preview_relay.py | tail -1

clean:
	rm -rf build build-host

.PHONY: all test clean
