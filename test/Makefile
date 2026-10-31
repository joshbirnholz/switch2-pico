# Host unit tests for the hardware independent modules.
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wno-unused-parameter -O1 -g
SRC = ../core/src
OBJS = test_main.c $(SRC)/hd_rumble.c $(SRC)/s2_proto.c $(SRC)/mapping.c $(SRC)/ds5.c $(SRC)/x360.c $(SRC)/gc_adapter.c $(SRC)/sinput.c $(SRC)/mode_select.c $(SRC)/battery.c $(SRC)/bonds.c $(SRC)/profiles.c $(SRC)/joycon.c

run: test_runner test_usb_lifecycle test_usb_audio2 test_usb_audio2_speaker test_headset_jack test_mic_test test_mic_api
	./test_runner
	./test_usb_lifecycle
	./test_usb_audio2
	./test_usb_audio2_speaker
	./test_headset_jack
	./test_mic_test
	./test_mic_api
	python3 test_audio_preflight.py
	python3 test_decky_update.py
	python3 test_decky_mic.py
	python3 test_decky_udev.py

test_runner: $(OBJS) $(wildcard $(SRC)/*.h)
	$(CC) $(CFLAGS) -I$(SRC) -o $@ $(OBJS) -lm

test_usb_lifecycle: test_usb_lifecycle.c stubs/tusb.h $(SRC)/app_core.c $(SRC)/web_api.c $(wildcard $(SRC)/*.h)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -ffunction-sections -fdata-sections -Istubs -I$(SRC) -Wl,--gc-sections -o $@ $< -lm

test_usb_audio2: test_usb_audio2.c $(SRC)/usb_audio2.c $(SRC)/usb_audio2.h
	$(CC) $(CFLAGS) -DS2_HEADSET_MIC=1 -I$(SRC) -o $@ test_usb_audio2.c $(SRC)/usb_audio2.c

test_usb_audio2_speaker: test_usb_audio2.c $(SRC)/usb_audio2.c $(SRC)/usb_audio2.h
	$(CC) $(CFLAGS) -DS2_HEADSET_MIC=0 -I$(SRC) -o $@ test_usb_audio2.c $(SRC)/usb_audio2.c

test_headset_jack: test_headset_jack.c $(SRC)/s2_link.c $(SRC)/s2_proto.c $(wildcard $(SRC)/*.h)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -ffunction-sections -fdata-sections -I$(SRC) -Wl,--gc-sections -o $@ test_headset_jack.c $(SRC)/s2_proto.c -lm

test_mic_test: test_mic_test.c $(SRC)/mic_test.c $(SRC)/mic_test.h
	$(CC) $(CFLAGS) -DS2_HEADSET_MIC=1 -I$(SRC) -o $@ test_mic_test.c $(SRC)/mic_test.c

test_mic_api: test_mic_api.c $(SRC)/web_api.c $(SRC)/mic_test.c $(wildcard $(SRC)/*.h)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -DS2_HEADSET_MIC=1 -ffunction-sections -fdata-sections -I$(SRC) -Wl,--gc-sections -o $@ test_mic_api.c $(SRC)/mic_test.c

clean:
	rm -f test_runner test_usb_lifecycle test_usb_audio2 test_usb_audio2_speaker test_headset_jack test_mic_test test_mic_api

.PHONY: run clean
