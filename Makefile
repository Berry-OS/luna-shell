# Copyright © 2026 Yuichiro Nakada / Project Vespera
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

PROFILE  ?= release
TARGET   := target/$(PROFILE)
PORT     ?= 8081
PREFIX   ?= /usr/local
LUNA_LIB := $(PREFIX)/lib/luna
# System font dir (fc-cache / GTK / other apps); skins/fonts links here.
FONTDIR  ?= /usr/share/fonts/luna
# e.g. make webgl APP=/usr/bin/gtk4-demo
APP      ?= $(TARGET)/hello-gtk

GLFW_CFLAGS := $(shell pkg-config --cflags glfw3 2>/dev/null)
GLFW_LIBS   := $(shell pkg-config --libs glfw3 2>/dev/null)

.PHONY: all build build-dri build-webgl build-desktop build-desktop-system build-shell \
        symlinks symlinks-system run server demo webgl run-gtk desktop desktop-system \
        luna-session install install-system stop clean opengl_gui luna-shell luna-ui \
        luna-clipboard luna-tray-notify sample

all: build symlinks

build:
	cargo build $(if $(filter release,$(PROFILE)),--release,)

build-dri:
	cargo build -p wayland-server-rs --features dri,gpu $(if $(filter release,$(PROFILE)),--release,)

build-webgl:
	cargo build --features webgl $(if $(filter release,$(PROFILE)),--release,)

build-desktop: build build-dri build-shell symlinks

# Compositor + shell only; GTK/GLFW use system libwayland-client
build-desktop-system: build-dri build-shell symlinks-system

build-shell: luna-shell luna-clipboard luna-tray-notify opengl_gui luna-ui

# Symlink libwayland-client.so.0 (SONAME expected by GTK4)
symlinks:
	@echo "→ Creating symlinks in $(TARGET)/"
	ln -sf libwayland_client.so $(TARGET)/libwayland-client.so.0
	ln -sf libwayland_client.so $(TARGET)/libwayland-client.so
	ln -sf ../../luna-shell $(TARGET)/luna-shell
	ln -sf ../../luna-clipboard $(TARGET)/luna-clipboard
	@echo "✓ Done"

# Symlinks without libwayland-client (use system Wayland)
symlinks-system:
	@echo "→ Creating symlinks in $(TARGET)/ (system Wayland)"
	ln -sf ../../luna-shell $(TARGET)/luna-shell
	ln -sf ../../luna-clipboard $(TARGET)/luna-clipboard
	@echo "✓ Done"

UI_DIR = ui

$(UI_DIR)/demo.css.h $(UI_DIR)/demo.html.h: $(UI_DIR)/demo.css $(UI_DIR)/demo.html $(UI_DIR)/gen_include.sh
	cd $(UI_DIR) && ./gen_include.sh demo.css demo.html

$(UI_DIR)/luna-ui.css.h $(UI_DIR)/luna-ui.html.h: $(UI_DIR)/luna-ui.css $(UI_DIR)/luna-ui.html $(UI_DIR)/gen_include.sh
	cd $(UI_DIR) && ./gen_include.sh luna-ui.css luna-ui.html

$(UI_DIR)/luna-shell.css.h $(UI_DIR)/luna-shell.html.h: $(UI_DIR)/luna-shell.css $(UI_DIR)/luna-shell.html $(UI_DIR)/gen_include.sh
	cd $(UI_DIR) && ./gen_include.sh luna-shell.css luna-shell.html

SHELL_CFLAGS := -pthread \
                $(shell pkg-config --cflags libdrm 2>/dev/null) \
                $(shell pkg-config --cflags wayland-client 2>/dev/null) \
                $(shell pkg-config --cflags xkbcommon 2>/dev/null)
SHELL_LIBS   := -pthread -lm -lEGL -lgbm -ldrm -linput -ludev -lxkbcommon \
                -lwayland-client -lwayland-egl -lGL

LAYER_SHELL_XML  := $(UI_DIR)/protocols/wlr-layer-shell-unstable-v1.xml
LAYER_SHELL_HDR  := $(UI_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h
LAYER_SHELL_SRC  := $(UI_DIR)/wlr-layer-shell-unstable-v1-protocol.c

$(LAYER_SHELL_HDR): $(LAYER_SHELL_XML)
	wayland-scanner client-header $< $@

$(LAYER_SHELL_SRC): $(LAYER_SHELL_XML)
	wayland-scanner private-code $< $@

luna-shell: $(UI_DIR)/luna-shell.c $(UI_DIR)/xdg-shell-protocol.c \
            $(LAYER_SHELL_HDR) $(LAYER_SHELL_SRC) \
            $(UI_DIR)/luna-ui.h $(UI_DIR)/luna-wifi.h $(UI_DIR)/luna-weather.h $(UI_DIR)/luna-monitor.h \
            $(UI_DIR)/stb_truetype.h $(UI_DIR)/stb_image_write.h \
            $(UI_DIR)/luna-shell.css.h $(UI_DIR)/luna-shell.html.h
	@echo "→ Building luna-shell (Luna Desktop shell)"
	# luna-shell's layout, animation and draw-list walks are hot on every KMS \
	# frame.  Prefer runtime optimization over the size-oriented global default.
	# Wi-Fi backend (luna-wifi.h) and the status poller both use pthreads.
	gcc -O2 -Wall -Wextra -DLUNA_BACKEND_X11 $(SHELL_CFLAGS) -I$(UI_DIR) \
	    $(UI_DIR)/luna-shell.c $(UI_DIR)/xdg-shell-protocol.c $(LAYER_SHELL_SRC) \
	    -o luna-shell $(SHELL_LIBS) -lX11

luna-clipboard: clipboard/luna-clipboard.c
	@echo "→ Building luna-clipboard (Wayland clipboard manager)"
	gcc -Os -Wall -Wextra -o luna-clipboard clipboard/luna-clipboard.c \
	    $$(pkg-config --cflags --libs wayland-client)

luna-tray-notify: tray/luna-tray-notify.c
	@echo "→ Building luna-tray-notify (XEmbed tray notification service)"
	gcc -Os -Wall -Wextra $$(pkg-config --cflags dbus-1) -o luna-tray-notify tray/luna-tray-notify.c -lX11 $$(pkg-config --libs dbus-1)

opengl_gui: $(UI_DIR)/opengl_gui.c $(UI_DIR)/luna-ui.h $(UI_DIR)/stb_truetype.h $(UI_DIR)/stb_image_write.h $(UI_DIR)/demo.css.h $(UI_DIR)/demo.html.h
	@echo "→ Building Luna UI demo host (opengl_gui)"
	gcc -Os -Wall -Wextra $(GLFW_CFLAGS) -I$(UI_DIR) $(UI_DIR)/opengl_gui.c -o opengl_gui -lm -lGL $(GLFW_LIBS)

luna-ui: $(UI_DIR)/luna-ui.c $(UI_DIR)/luna-ui.h $(UI_DIR)/stb_truetype.h $(UI_DIR)/stb_image_write.h $(UI_DIR)/luna-ui.css.h $(UI_DIR)/luna-ui.html.h
	@echo "→ Building Aurora Noir demo (luna-ui)"
	gcc -Os -Wall -Wextra $(GLFW_CFLAGS) -I$(UI_DIR) $(UI_DIR)/luna-ui.c -o luna-ui -lm -lGL $(GLFW_LIBS)

sample: $(UI_DIR)/sample_02.c $(UI_DIR)/luna-ui.h $(UI_DIR)/stb_truetype.h $(UI_DIR)/stb_image_write.h
	@echo "→ Building Luna UI sample"
	gcc -Os -Wall -Wextra $(GLFW_CFLAGS) -I$(UI_DIR) $(UI_DIR)/sample_02.c -o $(UI_DIR)/sample -lm -lGL $(GLFW_LIBS)

# Run app with Rust libwayland-client preloaded
run: build symlinks
	LD_LIBRARY_PATH=$(PWD)/$(TARGET):$$LD_LIBRARY_PATH \
	LD_PRELOAD=$(PWD)/$(TARGET)/libwayland-client.so.0 \
	$(APP)

# Start pure Rust compositor (no libwayland-server)
server: build
	$(TARGET)/luna-compositor --socket wayland-1 --screenshot /tmp/luna-compositor.ppm

# Start compositor and connect a GTK app via Rust libwayland-client
demo: build symlinks
	@echo "→ Starting compositor (wayland-1)"
	export XDG_RUNTIME_DIR=$${XDG_RUNTIME_DIR:-/tmp}; \
	  $(TARGET)/luna-compositor --socket wayland-1 --screenshot /tmp/luna-compositor.ppm & \
	sleep 0.5; \
	echo "→ Connecting app: $(APP)"; \
	XDG_RUNTIME_DIR=$${XDG_RUNTIME_DIR:-/tmp} \
	WAYLAND_DISPLAY=wayland-1 \
	LD_LIBRARY_PATH=$(PWD)/$(TARGET):$$LD_LIBRARY_PATH \
	LD_PRELOAD=$(PWD)/$(TARGET)/libwayland-client.so.0 \
	  $(APP)

# WebGL browser display
#
#   make webgl                    # hello-gtk in browser
#   make webgl APP=gtk4-demo      # any GTK4 app
#   make webgl APP=/usr/bin/foo PORT=9090
#
# Open http://localhost:$(PORT)/ for live streaming
webgl: build-webgl symlinks
	@echo "→ Starting WebGL compositor (port=$(PORT))"
	export XDG_RUNTIME_DIR=$${XDG_RUNTIME_DIR:-/tmp}; \
	  $(TARGET)/luna-compositor --socket wayland-webgl --backend webgl --port $(PORT) & \
	echo $$! > /tmp/luna-compositor.pid; \
	sleep 0.5; \
	echo ""; \
	echo "  Open in browser → http://localhost:$(PORT)/"; \
	echo ""; \
	echo "→ Launching app: $(APP)"; \
	XDG_RUNTIME_DIR=$${XDG_RUNTIME_DIR:-/tmp} \
	WAYLAND_DISPLAY=wayland-webgl \
	LD_LIBRARY_PATH=$(PWD)/$(TARGET):$$LD_LIBRARY_PATH \
	LD_PRELOAD=$(PWD)/$(TARGET)/libwayland-client.so.0 \
	  $(APP); \
	$(MAKE) stop

# Invoke run-gtk directly (pass app via APP=...)
#   make run-gtk APP="gtk4-demo --some-flag"
run-gtk: build-webgl symlinks
	PROFILE=$(PROFILE) PORT=$(PORT) ./run-gtk $(APP)

# Full Luna Desktop session (compositor + shell + vendored libwayland-client)
desktop: build-desktop
	chmod +x luna-session
	PROFILE=$(PROFILE) BACKEND=dri ./luna-session

# Luna Desktop with system libwayland-client (recommended for production)
desktop-system: build-desktop-system
	chmod +x luna-session
	PROFILE=$(PROFILE) BACKEND=dri LUNA_USE_SYSTEM_WAYLAND=1 ./luna-session

# Software backend desktop (VM / no GPU)
desktop-soft: build-desktop
	chmod +x luna-session
	PROFILE=$(PROFILE) BACKEND=software ./luna-session

# Software backend + system libwayland-client
desktop-soft-system: build-desktop-system
	chmod +x luna-session
	PROFILE=$(PROFILE) BACKEND=software LUNA_USE_SYSTEM_WAYLAND=1 ./luna-session

luna-session: build-desktop
	chmod +x luna-session

# Rewrite systemd unit paths for the chosen PREFIX.
define install_systemd
	@sed -e 's|/usr/local|$(PREFIX)|g' systemd/luna-desktop.service > /tmp/luna-desktop.service.$$$$; \
	  if install -m 644 /tmp/luna-desktop.service.$$$$ /etc/systemd/system/luna-desktop.service 2>/dev/null; then \
	    echo "→ Installed systemd unit → /etc/systemd/system/luna-desktop.service"; \
	  else \
	    install -d $(PREFIX)/share/luna-desktop; \
	    install -m 644 /tmp/luna-desktop.service.$$$$ $(PREFIX)/share/luna-desktop/luna-desktop.service; \
	    echo "→ Unit saved to $(PREFIX)/share/luna-desktop/luna-desktop.service (no root for /etc)"; \
	  fi; \
	  rm -f /tmp/luna-desktop.service.$$$$
endef

install: build-desktop
	install -d $(PREFIX)/bin $(LUNA_LIB) $(PREFIX)/share/luna-desktop/shell \
	            $(PREFIX)/share/luna-desktop/cursors $(PREFIX)/share/luna-desktop/skins \
	            $(PREFIX)/share/doc/luna-desktop
	install -d $(FONTDIR)
	install -m 755 luna-session $(PREFIX)/bin/luna-session
	install -m 755 $(TARGET)/luna-compositor $(PREFIX)/bin/luna-compositor
	install -m 755 luna-shell $(PREFIX)/bin/luna-shell
	install -m 755 luna-clipboard $(PREFIX)/bin/luna-clipboard
	install -m 755 luna-tray-notify $(PREFIX)/bin/luna-tray-notify
	install -D -m 644 tray/luna-tray-notify.desktop $(PREFIX)/share/applications/luna-tray-notify.desktop
	install -m 755 $(TARGET)/libwayland_client.so $(LUNA_LIB)/
	ln -sf libwayland_client.so $(LUNA_LIB)/libwayland-client.so.0
	ln -sf libwayland_client.so $(LUNA_LIB)/libwayland-client.so
	install -m 644 ui/luna-shell.html ui/luna-shell.css $(PREFIX)/share/luna-desktop/shell/
	install -m 644 ui/luna-ui.h ui/cssparser.h $(PREFIX)/share/luna-desktop/shell/
	install -m 644 skins/fonts/LunaSymbols-Solid.otf skins/fonts/LunaSymbols-Regular.otf \
	               skins/fonts/LunaSymbols-Brands.otf $(FONTDIR)/
	-fc-cache -f $(FONTDIR) 2>/dev/null || true
	# Cursor themes (.cur / .ani) — default is miku
	rm -rf $(PREFIX)/share/luna-desktop/cursors/miku
	cp -a cursors/miku $(PREFIX)/share/luna-desktop/cursors/
	# Drop prior skins/fonts symlink (→ FONTDIR) so cp can refresh themes;
	# fonts are re-linked below (avoids duplicating OTFs for fontconfig).
	rm -rf $(PREFIX)/share/luna-desktop/skins/fonts
	cp -a skins/. $(PREFIX)/share/luna-desktop/skins/
	# Browser-previewable skins link ../_base/luna-shell.css; replace the
	# repo symlink with a real copy next to the installed themes.
	install -d $(PREFIX)/share/luna-desktop/skins/_base
	install -m 644 ui/luna-shell.css $(PREFIX)/share/luna-desktop/skins/_base/luna-shell.css
	rm -rf $(PREFIX)/share/luna-desktop/skins/fonts
	ln -sfn $(FONTDIR) $(PREFIX)/share/luna-desktop/skins/fonts
	#install -m 644 README.md $(PREFIX)/share/doc/luna-desktop/README.md 2>/dev/null || true
	$(install_systemd)
	@echo "✓ Installed to $(PREFIX)"
	@echo "  Fonts: $(FONTDIR)/LunaSymbols-*.otf"
	@echo "  Enable boot: systemctl enable luna-desktop.service"

# Install using system libwayland (skip building/installing libwayland*.so)
install-system: build-desktop-system
	install -d $(PREFIX)/bin $(LUNA_LIB) $(PREFIX)/share/luna-desktop/shell \
	            $(PREFIX)/share/luna-desktop/cursors $(PREFIX)/share/luna-desktop/skins \
	            $(PREFIX)/share/doc/luna-desktop
	install -d $(FONTDIR)
	install -m 755 luna-session $(PREFIX)/bin/luna-session
	install -m 755 $(TARGET)/luna-compositor $(PREFIX)/bin/luna-compositor
	install -m 755 luna-shell $(PREFIX)/bin/luna-shell
	install -m 755 luna-clipboard $(PREFIX)/bin/luna-clipboard
	install -m 755 luna-tray-notify $(PREFIX)/bin/luna-tray-notify
	install -D -m 644 tray/luna-tray-notify.desktop $(PREFIX)/share/applications/luna-tray-notify.desktop
	install -m 644 ui/luna-shell.html ui/luna-shell.css $(PREFIX)/share/luna-desktop/shell/
	install -m 644 ui/luna-ui.h ui/cssparser.h $(PREFIX)/share/luna-desktop/shell/
	install -m 644 skins/fonts/LunaSymbols-Solid.otf skins/fonts/LunaSymbols-Regular.otf \
	               skins/fonts/LunaSymbols-Brands.otf $(FONTDIR)/
	-fc-cache -f $(FONTDIR) 2>/dev/null || true
	rm -rf $(PREFIX)/share/luna-desktop/cursors/miku
	cp -a cursors/miku $(PREFIX)/share/luna-desktop/cursors/
	# Drop prior skins/fonts symlink (→ FONTDIR) so cp can refresh themes;
	# fonts are re-linked below (avoids duplicating OTFs for fontconfig).
	rm -rf $(PREFIX)/share/luna-desktop/skins/fonts
	cp -a skins/. $(PREFIX)/share/luna-desktop/skins/
	# Browser-previewable skins link ../_base/luna-shell.css; replace the
	# repo symlink with a real copy next to the installed themes.
	install -d $(PREFIX)/share/luna-desktop/skins/_base
	install -m 644 ui/luna-shell.css $(PREFIX)/share/luna-desktop/skins/_base/luna-shell.css
	rm -rf $(PREFIX)/share/luna-desktop/skins/fonts
	ln -sfn $(FONTDIR) $(PREFIX)/share/luna-desktop/skins/fonts
	#install -m 644 README.md $(PREFIX)/share/doc/luna-desktop/README.md 2>/dev/null || true
	$(install_systemd)
	@echo "✓ Installed to $(PREFIX) (using system libwayland)"
	@echo "  Fonts: $(FONTDIR)/LunaSymbols-*.otf"
	@echo "  Enable boot: systemctl enable luna-desktop.service"

# Stop background compositor
stop:
	@if [ -f /tmp/luna-compositor.pid ]; then \
	  PID=$$(cat /tmp/luna-compositor.pid); \
	  kill "$$PID" 2>/dev/null && echo "→ Compositor stopped (PID=$$PID)" || true; \
	  rm -f /tmp/luna-compositor.pid; \
	fi

clean:
	cargo clean
	rm -f opengl_gui luna-shell luna-clipboard luna-tray-notify luna-ui $(UI_DIR)/sample \
	  $(UI_DIR)/demo.css.h $(UI_DIR)/demo.html.h \
	  $(UI_DIR)/luna-ui.css.h $(UI_DIR)/luna-ui.html.h \
	  $(UI_DIR)/luna-shell.css.h $(UI_DIR)/luna-shell.html.h
