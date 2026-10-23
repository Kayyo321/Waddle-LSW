# Native GUI uses system libraries; explicit CLI commands stay display-independent.
GuiPackages = gtk4 >= 4.8 libadwaita-1 >= 1.2 json-glib-1.0 vte-2.91-gtk4 >= 0.70
GuiPkgNames = gtk4 libadwaita-1 json-glib-1.0 vte-2.91-gtk4
GuiSources = src/gui/gui_main.c src/gui/gui_backend.c src/gui/gui_store.c
GuiHeaders = src/gui/gui_backend.h src/gui/gui_store.h
GuiFlags = $(shell pkg-config --cflags $(GuiPkgNames) 2>/dev/null)
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

gui-install: gui
	install -Dm755 build/waddle $(DESTDIR)$(PREFIX)/bin/waddle
	install -Dm755 build/waddle-gui $(DESTDIR)$(PREFIX)/bin/waddle-gui
	install -Dm644 data/io.github.Kayyo321.Waddle.desktop $(DESTDIR)$(PREFIX)/share/applications/io.github.Kayyo321.Waddle.desktop
	install -Dm644 imgs/logo.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/io.github.Kayyo321.Waddle.png
