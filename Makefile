CC ?= cc
BUILD_DIR ?= build
PKGS = wayland-client wayland-egl egl glesv2 libpng glib-2.0
CFLAGS += -O2 -Wall -Wextra -Wpedantic $(shell pkg-config --cflags $(PKGS)) -I$(BUILD_DIR) -Isrc
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lm
PROTOCOLS = $(BUILD_DIR)/wlr-layer-shell-protocol.c $(BUILD_DIR)/xdg-shell-protocol.c $(BUILD_DIR)/viewporter-protocol.c

all: $(BUILD_DIR)/cities-earth

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/wlr-layer-shell-client-protocol.h: src/wlr-layer-shell-unstable-v1.xml | $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(BUILD_DIR)/wlr-layer-shell-protocol.c: src/wlr-layer-shell-unstable-v1.xml | $(BUILD_DIR)
	wayland-scanner private-code $< $@

$(BUILD_DIR)/xdg-shell-client-protocol.h: /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml | $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(BUILD_DIR)/xdg-shell-protocol.c: /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml | $(BUILD_DIR)
	wayland-scanner private-code $< $@

$(BUILD_DIR)/viewporter-client-protocol.h: /usr/share/wayland-protocols/stable/viewporter/viewporter.xml | $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(BUILD_DIR)/viewporter-protocol.c: /usr/share/wayland-protocols/stable/viewporter/viewporter.xml | $(BUILD_DIR)
	wayland-scanner private-code $< $@

$(BUILD_DIR)/cities-earth: src/main.c src/solar.c src/solar.h src/fonts.h $(BUILD_DIR)/wlr-layer-shell-client-protocol.h $(BUILD_DIR)/xdg-shell-client-protocol.h $(BUILD_DIR)/viewporter-client-protocol.h $(PROTOCOLS)
	$(CC) $(CFLAGS) src/main.c src/solar.c $(PROTOCOLS) -o $@ $(LDLIBS)

clean:
	rm -rf -- "$(BUILD_DIR)"

.PHONY: all clean
