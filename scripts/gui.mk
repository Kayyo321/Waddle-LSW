# Native GUI uses system libraries; explicit CLI commands stay display-independent.
GuiPackages = gtk4 >= 4.8 libadwaita-1 >= 1.2 json-glib-1.0 vte-2.91-gtk4 >= 0.70
GuiPkgNames = gtk4 libadwaita-1 json-glib-1.0 vte-2.91-gtk4
GuiSources = src/gui/gui_main.c src/gui/gui_backend.c src/gui/gui_store.c src/gui/gui_window.c src/gui/gui_forms.c src/gui/gui_terminal.c src/gui/gui_library.c
GuiHeaders = src/gui/gui_private.h src/gui/gui_backend.h src/gui/gui_store.h
GuiFlags = -DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_8 -DADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_2 $(shell pkg-config --cflags $(GuiPkgNames) 2>/dev/null)
GuiLibraries = $(shell pkg-config --libs $(GuiPkgNames) 2>/dev/null)
PREFIX ?= /usr/local
DESTDIR ?=

.PHONY: gui gui-deps gui-test gui-install
gui: build/waddle build/waddle-gui

gui-deps:
	@pkg-config --exists 'gtk4 >= 4.8' 'libadwaita-1 >= 1.2' json-glib-1.0 'vte-2.91-gtk4 >= 0.70' || { \
		echo 'GUI requires GTK4 >= 4.8, libadwaita >= 1.2, JSON-GLib and GTK4 VTE >= 0.70 development packages.' >&2; exit 1; }

build/waddle-gui: $(GuiSources) $(GuiHeaders) | build gui-deps
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GuiFlags) $(GuiSources) $(LDFLAGS) $(GuiLibraries) -o $@

gui-install: gui build/waddled
	install -Dm755 build/waddle $(DESTDIR)$(PREFIX)/bin/waddle
	install -Dm755 build/waddle-gui $(DESTDIR)$(PREFIX)/bin/waddle-gui
	install -Dm755 build/waddled $(DESTDIR)$(PREFIX)/bin/waddled
	install -Dm644 data/io.github.Kayyo321.Waddle.desktop $(DESTDIR)$(PREFIX)/share/applications/io.github.Kayyo321.Waddle.desktop
	install -Dm644 imgs/logo.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/io.github.Kayyo321.Waddle.png

build/gui_backend_test: tests/gui/backend.c src/gui/gui_backend.c src/gui/gui_store.c $(GuiHeaders) | build gui-deps
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GuiFlags) -Isrc/gui tests/gui/backend.c src/gui/gui_backend.c src/gui/gui_store.c $(LDFLAGS) $(GuiLibraries) -o $@

build/gui_window_test: tests/gui/window.c $(filter-out src/gui/gui_main.c,$(GuiSources)) $(GuiHeaders) | build gui-deps
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GuiFlags) -Isrc/gui tests/gui/window.c $(filter-out src/gui/gui_main.c,$(GuiSources)) $(LDFLAGS) $(GuiLibraries) -o $@

gui-test: gui build/gui_backend_test build/gui_window_test
	./build/gui_backend_test
	GTK_A11Y=none GSK_RENDERER=cairo dbus-run-session -- xvfb-run -a ./build/gui_window_test
	python3 tests/gui/dispatch.py
	GTK_A11Y=none GSK_RENDERER=cairo dbus-run-session -- xvfb-run -a python3 tests/gui/instance.py

.PHONY: gui-sanitizers
GuiSanitizerFlags = -O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,leak,undefined -fno-omit-frame-pointer

build/gui_backend_sanitized: tests/gui/backend.c src/gui/gui_backend.c src/gui/gui_store.c $(GuiHeaders) | build gui-deps
	$(CC) $(CPPFLAGS) $(GuiSanitizerFlags) $(GuiFlags) -Isrc/gui tests/gui/backend.c src/gui/gui_backend.c src/gui/gui_store.c $(GuiLibraries) -o $@

build/gui_window_sanitized: tests/gui/window.c $(filter-out src/gui/gui_main.c,$(GuiSources)) $(GuiHeaders) | build gui-deps
	$(CC) $(CPPFLAGS) $(GuiSanitizerFlags) $(GuiFlags) -Isrc/gui tests/gui/window.c $(filter-out src/gui/gui_main.c,$(GuiSources)) $(GuiLibraries) -o $@

gui-sanitizers: build/gui_backend_sanitized build/gui_window_sanitized
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./build/gui_backend_sanitized
	GTK_A11Y=none GSK_RENDERER=cairo ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:fast_unwind_on_malloc=0 LSAN_OPTIONS=suppressions=$(CURDIR)/tests/gui/lsan.supp dbus-run-session -- xvfb-run -a ./build/gui_window_sanitized
