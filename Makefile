PROJECT_NAME = snd_analizer
BUILD_DIR = build
PICO_SDK_PATH = ../../../pico/pico-sdk
UF2_FILE = $(BUILD_DIR)/$(PROJECT_NAME).uf2
TTY_DEVICE = /dev/ttyACM0
BAUD_RATE = 115200

compile:
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake .. && make -j4

upload: compile
	picotool load -F $(UF2_FILE) && picotool reboot -f

reboot:
	@picotool reboot -f

clean:
	@cd $(BUILD_DIR) && make clean

clean-all:
	@rm -rf $(BUILD_DIR)

monitor:
	@minicom -b $(BAUD_RATE) -o -D $(TTY_DEVICE)

init:
	@mkdir -p $(BUILD_DIR)
	@export PICO_SDK_PATH=$(PICO_SDK_PATH) && cd $(BUILD_DIR) && cmake ..
	@echo "Project initialized. Read 'Getting Started with Pico' at /home/pi/Bookshelf/getting-started-with-pico.pdf"

.PHONY: compile upload reboot clean clean-all monitor init
