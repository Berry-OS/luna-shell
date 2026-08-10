# Copyright © 2026 Yuichiro Nakada / Project Vespera
#
# Include from app Makefiles to compile/link against vendored
# wayland-client headers + wayland-client-rs (no system libwayland-dev).
#
#   LUNA_SHELL ?= ../luna-shell
#   include $(LUNA_SHELL)/wayland-client-rs/luna-wayland-client.mk
#   CFLAGS += $(LUNA_WAYLAND_CFLAGS)
#   LIBS   += $(LUNA_WAYLAND_LIBS)

LUNA_WL_MK_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
LUNA_SHELL_ROOT ?= $(abspath $(LUNA_WL_MK_DIR)/..)
LUNA_WL_PROFILE ?= release
LUNA_WL_TARGET  ?= $(LUNA_SHELL_ROOT)/target/$(LUNA_WL_PROFILE)

LUNA_WAYLAND_CFLAGS := -I$(LUNA_WL_MK_DIR)/include
# Prefer the SONAME symlink (libwayland-client.so) so -lwayland-client
# resolves here; rpath keeps runtime resolution on the vendored .so
# (GLFW also dlopens libwayland-client.so.0).
LUNA_WAYLAND_LIBS := -L$(LUNA_WL_TARGET) -lwayland-client \
	-Wl,-rpath,$(LUNA_WL_TARGET)

# Ensure cdylib + SONAME symlinks exist (no-op if already built).
.PHONY: luna-wayland-client-lib
luna-wayland-client-lib:
	@if [ ! -f "$(LUNA_WL_TARGET)/libwayland_client.so" ]; then \
	  $(MAKE) -C "$(LUNA_SHELL_ROOT)" build PROFILE=$(LUNA_WL_PROFILE); \
	fi
	@ln -sf libwayland_client.so "$(LUNA_WL_TARGET)/libwayland-client.so"
	@ln -sf libwayland_client.so "$(LUNA_WL_TARGET)/libwayland-client.so.0"
