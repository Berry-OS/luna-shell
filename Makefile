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

SPECFILE     ?= luna-shell.spec
# Spec Version is %{build_date} (see %global build_date).  Plain awk leaves the
# macro literal, so `make dist` would ship luna-shell-%{build_date}.tar while
# rpmbuild looks for luna-shell-YYYYMMDD.tar.  Expand with rpmspec when
# available; otherwise mirror the Tokyo-date fallback used in the spec.
RPM_NAME     := $(shell rpmspec -q --srpm --qf '%{name}\n' $(SPECFILE) 2>/dev/null | head -n1)
RPM_VERSION  := $(shell rpmspec -q --srpm --qf '%{version}\n' $(SPECFILE) 2>/dev/null | head -n1)
RPM_RELEASE  := $(shell rpmspec -q --srpm --qf '%{release}\n' $(SPECFILE) 2>/dev/null | head -n1)
ifeq ($(strip $(RPM_NAME)),)
RPM_NAME := $(shell awk '/^Name:/{print $$2; exit}' $(SPECFILE))
endif
ifeq ($(strip $(RPM_VERSION)),)
RPM_VERSION := $(shell TZ=Asia/Tokyo date +%Y%m%d)
endif
ifeq ($(strip $(RPM_RELEASE)),)
RPM_RELEASE := $(shell awk '/^Release:/{print $$2; exit}' $(SPECFILE))
endif
DIST_NAME    := $(RPM_NAME)-$(RPM_VERSION)
TARBALL      := $(DIST_NAME).tar
RPMBUILD_DIR := $(CURDIR)/rpmbuild
RPM_DIST     := $(CURDIR)/.rpm-dist
# Local rustup toolchains are not the distro rust/cargo RPMs listed in the spec.
RPMBUILD_FLAGS ?= $(shell rpm -q rust cargo >/dev/null 2>&1 || echo --nodeps)

.PHONY: all build build-dri build-webgl build-desktop build-desktop-system build-shell \
        symlinks symlinks-system run server demo webgl run-gtk desktop desktop-system \
        luna-session install install-system stop clean dist rpm srpm luna-shell \
        luna-clipboard luna-tray-notify

all: build symlinks

build:
	cargo build $(if $(filter release,$(PROFILE)),--release,)

build-dri:
	cargo build -p wayland-server-rs --features dri,gpu $(if $(filter release,$(PROFILE)),--release,)

build-webgl:
	cargo build --features webgl $(if $(filter release,$(PROFILE)),--release,)

# Compositor + shell; default shell links system libwayland-client.
# Use LUNA_WAYLAND_CLIENT=vendored for the replacement client.
build-desktop: build build-dri build-shell symlinks

# Force system libwayland-client for shell/clipboard (ignores LUNA_WAYLAND_CLIENT).
build-desktop-system:
	$(MAKE) build-dri build-shell symlinks-system LUNA_WAYLAND_CLIENT=system

build-shell: luna-shell luna-clipboard luna-tray-notify

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
DEFAULT_SKIN_DIR = skins/default

# Wayland client for luna-shell / luna-clipboard:
#   LUNA_WAYLAND_CLIENT=system   (default) — distro libwayland-client
#   LUNA_WAYLAND_CLIENT=vendored — wayland-client-rs (replacement work)
LUNA_WAYLAND_CLIENT ?= system

include wayland-client-rs/luna-wayland-client.mk

ifeq ($(LUNA_WAYLAND_CLIENT),vendored)
SHELL_WL_CFLAGS := $(LUNA_WAYLAND_CFLAGS)
SHELL_WL_LIBS   := $(LUNA_WAYLAND_LIBS)
SHELL_WL_DEPS   := luna-wayland-client-lib
SHELL_WL_LABEL  := vendored wayland-client
else
SHELL_WL_CFLAGS := $(shell pkg-config --cflags wayland-client 2>/dev/null)
SHELL_WL_LIBS   := -lwayland-client
SHELL_WL_DEPS   :=
SHELL_WL_LABEL  := system wayland-client
endif

$(UI_DIR)/luna-shell.css.h: $(DEFAULT_SKIN_DIR)/style.css $(UI_DIR)/gen_include.sh
	cd $(UI_DIR) && ./gen_include.sh -o luna-shell.css.h ../$(DEFAULT_SKIN_DIR)/style.css

$(UI_DIR)/luna-shell-widgets.css.h: skins/widgets.css $(UI_DIR)/gen_include.sh
	cd $(UI_DIR) && ./gen_include.sh -o luna-shell-widgets.css.h ../skins/widgets.css

$(UI_DIR)/luna-shell-widgets.html.h: skins/widgets.html $(UI_DIR)/gen_include.sh
	cd $(UI_DIR) && ./gen_include.sh -o luna-shell-widgets.html.h ../skins/widgets.html

$(UI_DIR)/luna-shell.html.h: $(DEFAULT_SKIN_DIR)/layout.html $(UI_DIR)/gen_include.sh
	cd $(UI_DIR) && ./gen_include.sh -o luna-shell.html.h ../$(DEFAULT_SKIN_DIR)/layout.html

SHELL_CFLAGS := -pthread \
                $(shell pkg-config --cflags libdrm 2>/dev/null) \
                $(SHELL_WL_CFLAGS) \
                $(shell pkg-config --cflags xkbcommon 2>/dev/null) \
                $(shell pkg-config --cflags dbus-1 2>/dev/null) \
                $(shell pkg-config --cflags openssl 2>/dev/null)
SHELL_LIBS   := -pthread -lm -lEGL -lgbm -ldrm -linput -ludev -lxkbcommon \
                $(SHELL_WL_LIBS) -lwayland-egl -lGL \
                $(shell pkg-config --libs dbus-1 2>/dev/null) \
                $(shell pkg-config --libs openssl 2>/dev/null || echo -lssl -lcrypto)

LAYER_SHELL_XML  := $(UI_DIR)/protocols/wlr-layer-shell-unstable-v1.xml
LAYER_SHELL_HDR  := $(UI_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h
LAYER_SHELL_SRC  := $(UI_DIR)/wlr-layer-shell-unstable-v1-protocol.c

$(LAYER_SHELL_HDR): $(LAYER_SHELL_XML)
	wayland-scanner client-header $< $@

$(LAYER_SHELL_SRC): $(LAYER_SHELL_XML)
	wayland-scanner private-code $< $@

luna-shell: $(SHELL_WL_DEPS) $(UI_DIR)/luna-shell.c $(UI_DIR)/xdg-shell-protocol.c \
            $(LAYER_SHELL_HDR) $(LAYER_SHELL_SRC) \
            $(UI_DIR)/luna-ui/luna-ui.h $(UI_DIR)/luna-wifi.h $(UI_DIR)/luna-ethernet.h \
            $(UI_DIR)/luna-bluetooth.h $(UI_DIR)/luna-sni.h $(UI_DIR)/luna-weather.h $(UI_DIR)/luna-monitor.h \
            $(UI_DIR)/luna-ui/stb_truetype.h $(UI_DIR)/luna-ui/stb_image_write.h \
            $(UI_DIR)/luna-shell.css.h $(UI_DIR)/luna-shell.html.h \
            $(UI_DIR)/luna-shell-widgets.css.h $(UI_DIR)/luna-shell-widgets.html.h
	@echo "→ Building luna-shell (Luna Desktop shell, $(SHELL_WL_LABEL))"
	# luna-shell's layout, animation and draw-list walks are hot on every KMS \
	# frame.  Prefer runtime optimization over the size-oriented global default.
	# These MUST come AFTER SHELL_CFLAGS: Berry rpm %{optflags} is
	# `-Os -ffast-math`, gcc keeps the last -O* / -f* of each kind, and
	# -ffast-math left luna_render as a no-op (black desktop, live cursor).
	# Wi-Fi backend (luna-wifi.h) and the status poller both use pthreads.
	gcc -Wall -Wextra -DLUNA_BACKEND_X11 $(SHELL_CFLAGS) \
	    -O2 -fno-fast-math -fno-finite-math-only -I$(UI_DIR) \
	    $(UI_DIR)/luna-shell.c $(UI_DIR)/xdg-shell-protocol.c $(LAYER_SHELL_SRC) \
	    -o luna-shell $(SHELL_LIBS) -lX11

luna-clipboard: $(SHELL_WL_DEPS) clipboard/luna-clipboard.c
	@echo "→ Building luna-clipboard (Wayland clipboard manager, $(SHELL_WL_LABEL))"
	gcc -Os -Wall -Wextra $(SHELL_WL_CFLAGS) -o luna-clipboard clipboard/luna-clipboard.c \
	    $(SHELL_WL_LIBS)

luna-tray-notify: tray/luna-tray-notify.c
	@echo "→ Building luna-tray-notify (XEmbed tray notification service)"
	gcc -Os -Wall -Wextra $$(pkg-config --cflags dbus-1) -o luna-tray-notify tray/luna-tray-notify.c -lX11 $$(pkg-config --libs dbus-1)

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

# Full Luna Desktop session (compositor + shell)
# Vendored client: make desktop LUNA_WAYLAND_CLIENT=vendored
desktop: build-desktop
	chmod +x luna-session
	PROFILE=$(PROFILE) BACKEND=dri LUNA_WAYLAND_CLIENT=$(LUNA_WAYLAND_CLIENT) ./luna-session

# Luna Desktop with system libwayland-client (recommended default)
desktop-system: build-desktop-system
	chmod +x luna-session
	PROFILE=$(PROFILE) BACKEND=dri LUNA_USE_SYSTEM_WAYLAND=1 LUNA_WAYLAND_CLIENT=system ./luna-session

# Software backend desktop (VM / no GPU)
desktop-soft: build-desktop
	chmod +x luna-session
	PROFILE=$(PROFILE) BACKEND=software LUNA_WAYLAND_CLIENT=$(LUNA_WAYLAND_CLIENT) ./luna-session

# Software backend + system libwayland-client
desktop-soft-system: build-desktop-system
	chmod +x luna-session
	PROFILE=$(PROFILE) BACKEND=software LUNA_USE_SYSTEM_WAYLAND=1 LUNA_WAYLAND_CLIENT=system ./luna-session

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
	install -d $(FONTDIR)/web
	install -m 755 luna-session $(PREFIX)/bin/luna-session
	install -m 755 $(TARGET)/luna-compositor $(PREFIX)/bin/luna-compositor
	install -m 755 luna-shell $(PREFIX)/bin/luna-shell
	install -m 755 luna-clipboard $(PREFIX)/bin/luna-clipboard
	install -m 755 luna-tray-notify $(PREFIX)/bin/luna-tray-notify
	install -D -m 644 tray/luna-tray-notify.desktop $(PREFIX)/share/applications/luna-tray-notify.desktop
	install -m 755 $(TARGET)/libwayland_client.so $(LUNA_LIB)/
	ln -sf libwayland_client.so $(LUNA_LIB)/libwayland-client.so.0
	ln -sf libwayland_client.so $(LUNA_LIB)/libwayland-client.so
	install -d $(PREFIX)/include/luna-wayland $(PREFIX)/lib/pkgconfig \
	            $(PREFIX)/share/luna-desktop/wayland-client
	install -m 644 wayland-client-rs/include/*.h $(PREFIX)/include/luna-wayland/
	install -m 644 wayland-client-rs/include/README $(PREFIX)/include/luna-wayland/
	install -m 644 wayland-client-rs/luna-wayland-client.mk \
	               $(PREFIX)/share/luna-desktop/wayland-client/
	# Installed .pc points at PREFIX (not the build tree).
	printf '%s\n' \
	  'prefix=$(PREFIX)' \
	  'libdir=$${prefix}/lib/luna' \
	  'includedir=$${prefix}/include/luna-wayland' \
	  '' \
	  'Name: wayland-client' \
	  'Description: Luna Wayland client API (vendored headers + wayland-client-rs)' \
	  'Version: 1.25.0' \
	  'Libs: -L$${libdir} -lwayland-client -Wl,-rpath,$${libdir}' \
	  'Cflags: -I$${includedir}' \
	  > $(PREFIX)/lib/pkgconfig/luna-wayland-client.pc
	install -m 644 $(DEFAULT_SKIN_DIR)/layout.html $(PREFIX)/share/luna-desktop/shell/luna-shell.html
	install -m 644 $(DEFAULT_SKIN_DIR)/style.css $(PREFIX)/share/luna-desktop/shell/luna-shell.css
	install -m 644 ui/luna-ui/luna-ui.h ui/luna-ui/cssparser.h $(PREFIX)/share/luna-desktop/shell/
	install -m 644 skins/fonts/LunaSymbols-Solid.otf skins/fonts/LunaSymbols-Regular.otf \
	               skins/fonts/LunaSymbols-Brands.otf $(FONTDIR)/
	install -m 644 skins/fonts/web/Inter-Regular.ttf skins/fonts/web/Manrope-Regular.ttf $(FONTDIR)/web/
	-fc-cache -f $(FONTDIR) 2>/dev/null || true
	# Cursor themes (.cur / .ani) — default is miku
	rm -rf $(PREFIX)/share/luna-desktop/cursors/miku
	cp -a cursors/miku $(PREFIX)/share/luna-desktop/cursors/
	# Drop prior skins/fonts symlink (→ FONTDIR) so cp can refresh themes;
	# fonts are re-linked below (avoids duplicating OTFs for fontconfig).
	rm -rf $(PREFIX)/share/luna-desktop/skins/fonts
	cp -a skins/. $(PREFIX)/share/luna-desktop/skins/
	rm -rf $(PREFIX)/share/luna-desktop/skins/fonts
	ln -sfn $(FONTDIR) $(PREFIX)/share/luna-desktop/skins/fonts
	ln -sfn $(FONTDIR) $(PREFIX)/share/luna-desktop/fonts
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
	install -d $(FONTDIR)/web
	install -m 755 luna-session $(PREFIX)/bin/luna-session
	install -m 755 $(TARGET)/luna-compositor $(PREFIX)/bin/luna-compositor
	install -m 755 luna-shell $(PREFIX)/bin/luna-shell
	install -m 755 luna-clipboard $(PREFIX)/bin/luna-clipboard
	install -m 755 luna-tray-notify $(PREFIX)/bin/luna-tray-notify
	install -D -m 644 tray/luna-tray-notify.desktop $(PREFIX)/share/applications/luna-tray-notify.desktop
	install -m 644 $(DEFAULT_SKIN_DIR)/layout.html $(PREFIX)/share/luna-desktop/shell/luna-shell.html
	install -m 644 $(DEFAULT_SKIN_DIR)/style.css $(PREFIX)/share/luna-desktop/shell/luna-shell.css
	install -m 644 ui/luna-ui/luna-ui.h ui/luna-ui/cssparser.h $(PREFIX)/share/luna-desktop/shell/
	install -m 644 skins/fonts/LunaSymbols-Solid.otf skins/fonts/LunaSymbols-Regular.otf \
	               skins/fonts/LunaSymbols-Brands.otf $(FONTDIR)/
	install -m 644 skins/fonts/web/Inter-Regular.ttf skins/fonts/web/Manrope-Regular.ttf $(FONTDIR)/web/
	-fc-cache -f $(FONTDIR) 2>/dev/null || true
	rm -rf $(PREFIX)/share/luna-desktop/cursors/miku
	cp -a cursors/miku $(PREFIX)/share/luna-desktop/cursors/
	# Drop prior skins/fonts symlink (→ FONTDIR) so cp can refresh themes;
	# fonts are re-linked below (avoids duplicating OTFs for fontconfig).
	rm -rf $(PREFIX)/share/luna-desktop/skins/fonts
	cp -a skins/. $(PREFIX)/share/luna-desktop/skins/
	rm -rf $(PREFIX)/share/luna-desktop/skins/fonts
	ln -sfn $(FONTDIR) $(PREFIX)/share/luna-desktop/skins/fonts
	ln -sfn $(FONTDIR) $(PREFIX)/share/luna-desktop/fonts
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
	rm -f luna-shell luna-clipboard luna-tray-notify \
	  $(UI_DIR)/luna-shell.css.h $(UI_DIR)/luna-shell.html.h \
	  $(UI_DIR)/luna-shell-widgets.css.h $(UI_DIR)/luna-shell-widgets.html.h
	rm -rf $(RPMBUILD_DIR) $(RPM_DIST)
	rm -f $(TARBALL) $(RPM_NAME)-$(RPM_VERSION)-$(RPM_RELEASE).*.rpm

# Source tarball consumed by luna-shell.spec (Source0).  Cargo crates are
# vendored so rpmbuild can run with CARGO_NET_OFFLINE=true.
dist:
	@command -v cargo >/dev/null || { echo 'cargo is required for make dist'; exit 1; }
	rm -rf $(RPM_DIST) $(TARBALL)
	mkdir -p $(RPM_DIST)/$(DIST_NAME)
	tar -C . \
	  --exclude-vcs \
	  --exclude='.rpm-dist' \
	  --exclude='patches' \
	  --exclude='*.patch' \
	  --exclude='target' \
	  --exclude='vendor' \
	  --exclude='.cargo' \
	  --exclude='$(DIST_NAME)' \
	  --exclude='$(TARBALL)' \
	  --exclude='luna-shell' \
	  --exclude='luna-clipboard' \
	  --exclude='luna-tray-notify' \
	  --exclude='hello-gtk' \
	  --exclude='opengl-webui-ultralight' \
	  --exclude='*.tar' \
	  --exclude='*.rpm' \
	  -cf - . | tar -C $(RPM_DIST)/$(DIST_NAME) -xf -
	@if [ -L $(RPM_DIST)/$(DIST_NAME)/ui/luna-ui ] || \
	    [ ! -f $(RPM_DIST)/$(DIST_NAME)/ui/luna-ui/luna-ui.h ]; then \
	  if [ ! -f ../luna-ui/luna-ui.h ]; then \
	    echo 'error: ui/luna-ui is a symlink; ../luna-ui is required to build the RPM tarball'; \
	    exit 1; \
	  fi; \
	  rm -rf $(RPM_DIST)/$(DIST_NAME)/ui/luna-ui; \
	  cp -a ../luna-ui $(RPM_DIST)/$(DIST_NAME)/ui/luna-ui; \
	fi
	rm -f $(RPM_DIST)/$(DIST_NAME)/ui/luna-editor.c \
	      $(RPM_DIST)/$(DIST_NAME)/ui/luna-view.c \
	      $(RPM_DIST)/$(DIST_NAME)/ui/luna-fm.c
	rm -f $(RPM_DIST)/$(DIST_NAME)/rust-toolchain.toml \
	      $(RPM_DIST)/$(DIST_NAME)/rust-toolchain
	sed -i 's/, "hello-gtk"//' $(RPM_DIST)/$(DIST_NAME)/Cargo.toml
	cd $(RPM_DIST)/$(DIST_NAME) && cargo generate-lockfile
	cd $(RPM_DIST)/$(DIST_NAME) && cargo vendor --locked vendor
	mkdir -p $(RPM_DIST)/$(DIST_NAME)/.cargo
	printf '%s\n' \
	  '[source.crates-io]' \
	  'replace-with = "vendored-sources"' \
	  '' \
	  '[source.vendored-sources]' \
	  'directory = "vendor"' \
	  > $(RPM_DIST)/$(DIST_NAME)/.cargo/config.toml
	tar -C $(RPM_DIST) -cf $(TARBALL) $(DIST_NAME)
	rm -rf $(RPM_DIST)
	@echo "✓ Created $(TARBALL)"

rpm: dist
	@command -v rpmbuild >/dev/null || { echo 'rpmbuild is required (dnf install rpm-build)'; exit 1; }
	rm -rf $(RPMBUILD_DIR)
	mkdir -p $(RPMBUILD_DIR)/BUILD $(RPMBUILD_DIR)/BUILDROOT \
	         $(RPMBUILD_DIR)/RPMS $(RPMBUILD_DIR)/SOURCES \
	         $(RPMBUILD_DIR)/SPECS $(RPMBUILD_DIR)/SRPMS
	cp -f $(TARBALL) $(RPMBUILD_DIR)/SOURCES/
	cp -f $(SPECFILE) $(RPMBUILD_DIR)/SPECS/
	rpmbuild -bb $(RPMBUILD_FLAGS) \
	  --define "_topdir $(RPMBUILD_DIR)" \
	  $(RPMBUILD_DIR)/SPECS/$(notdir $(SPECFILE))
	@find $(RPMBUILD_DIR)/RPMS -name '*.rpm' -exec cp -f {} . \;
	@echo "✓ RPM:"
	@find . -maxdepth 1 -name '$(RPM_NAME)-$(RPM_VERSION)-$(RPM_RELEASE).*.rpm' -print

srpm: dist
	@command -v rpmbuild >/dev/null || { echo 'rpmbuild is required (dnf install rpm-build)'; exit 1; }
	mkdir -p $(RPMBUILD_DIR)/BUILD $(RPMBUILD_DIR)/BUILDROOT \
	         $(RPMBUILD_DIR)/RPMS $(RPMBUILD_DIR)/SOURCES \
	         $(RPMBUILD_DIR)/SPECS $(RPMBUILD_DIR)/SRPMS
	cp -f $(TARBALL) $(RPMBUILD_DIR)/SOURCES/
	cp -f $(SPECFILE) $(RPMBUILD_DIR)/SPECS/
	rpmbuild -bs $(RPMBUILD_FLAGS) \
	  --define "_topdir $(RPMBUILD_DIR)" \
	  $(RPMBUILD_DIR)/SPECS/$(notdir $(SPECFILE))
	@find $(RPMBUILD_DIR)/SRPMS -name '*.src.rpm' -exec cp -f {} . \;
	@echo "✓ SRPM:"
	@find . -maxdepth 1 -name '$(RPM_NAME)-$(RPM_VERSION)-$(RPM_RELEASE).*.src.rpm' -print
