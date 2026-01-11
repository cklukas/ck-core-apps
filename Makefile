CC ?= cc
CXX ?= c++
AR ?= ar
CFLAGS ?= -O2 -Wall -Isrc -Ilib
CXXFLAGS ?= -O2 -Wall -Isrc -Ilib -std=c++17

.DEFAULT_GOAL := all

# Where to place build artifacts
BUILD_DIR ?= build
BIN_DIR := $(BUILD_DIR)/bin

CEF_WRAPPER_SRC_DIR := third_party/cef/libcef_dll
CEF_WRAPPER_SRCS := $(shell find $(CEF_WRAPPER_SRC_DIR) -name '*.cc' | sort)
CEF_WRAPPER_OBJS := $(patsubst $(CEF_WRAPPER_SRC_DIR)/%.cc,$(BUILD_DIR)/cef-wrapper/%.o,$(CEF_WRAPPER_SRCS))
CEF_WRAPPER_LIB := $(BUILD_DIR)/libcef_dll_wrapper.a
CEF_WRAPPER_CFLAGS := -I$(CEF_WRAPPER_SRC_DIR) -Ithird_party/cef

# CDE include/lib paths (override on CLI if different on your system)
CDE_PREFIX ?= /usr/local/CDE
CDE_INC ?= $(CDE_PREFIX)/include
CDE_LIB ?= $(CDE_PREFIX)/lib

CDE_CFLAGS := -I$(CDE_INC)
CDE_LDFLAGS := -L$(CDE_LIB)
CDE_LIBS := -lDtSvc -lDtXinerama -lDtWidget -ltt -lXm -lXt -lSM -lICE -lXinerama -lX11 -lXpm
CEF_CFLAGS := -Ithird_party/cef/include -Ithird_party/cef/include/include
CEF_LDFLAGS := -Lthird_party/cef/lib
CEF_LIBS := -lcef -ldl -lpthread
CEF_RPATH := -Wl,-rpath,$(abspath third_party/cef/lib)

$(CEF_WRAPPER_LIB): $(CEF_WRAPPER_OBJS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/cef-wrapper/%.o: $(CEF_WRAPPER_SRC_DIR)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CEF_CFLAGS) $(CEF_WRAPPER_CFLAGS) -DWRAPPING_CEF_SHARED -c $< -o $@

PROGRAMS := $(BIN_DIR)/ck-about \
            $(BIN_DIR)/ck-load \
            $(BIN_DIR)/ck-tasks \
            $(BIN_DIR)/ck-mixer \
            $(BIN_DIR)/ck-clock \
            $(BIN_DIR)/ck-calc \
            $(BIN_DIR)/ck-character-map \
            $(BIN_DIR)/ck-browser \
            $(BIN_DIR)/ck-nibbles \
            $(BIN_DIR)/ck-mines \
            $(BIN_DIR)/ck-plasma-1

.PHONY: all clean ck-about ck-load ck-tasks ck-mixer ck-clock ck-calc ck-character-map ck-browser ck-nibbles ck-mines ck-plasma-1

all: $(PROGRAMS)

ck-about: $(BIN_DIR)/ck-about
ck-load: $(BIN_DIR)/ck-load
ck-mixer: $(BIN_DIR)/ck-mixer
ck-clock: $(BIN_DIR)/ck-clock
ck-calc: $(BIN_DIR)/ck-calc
ck-character-map: $(BIN_DIR)/ck-character-map
ck-browser: $(BIN_DIR)/ck-browser
ck-nibbles: $(BIN_DIR)/ck-nibbles
ck-mines: $(BIN_DIR)/ck-mines
ck-plasma-1: $(BIN_DIR)/ck-plasma-1

$(BIN_DIR):
	@mkdir -p $@

# ck-about
$(BIN_DIR)/ck-about: src/ck-about/ck-about.c src/shared/session_utils.c src/shared/session_utils.h src/shared/about_dialog.c src/shared/about_dialog.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/ck-about/ck-about.c src/shared/session_utils.c src/shared/about_dialog.c -o $@ $(CDE_LDFLAGS) $(CDE_LIBS)

# ck-load
$(BIN_DIR)/ck-load: src/ck-load/ck-load.c src/ck-load/vertical_meter.c src/ck-load/vertical_meter.h src/shared/session_utils.c src/shared/session_utils.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/ck-load/ck-load.c src/ck-load/vertical_meter.c src/shared/session_utils.c -o $@ $(CDE_LDFLAGS) $(CDE_LIBS)

# ck-tasks
$(BIN_DIR)/ck-tasks: src/ck-tasks/ck-tasks.c src/ck-tasks/ck-tasks-ctrl.c src/ck-tasks/ck-tasks-model.c src/ck-tasks/ck-tasks-ui.c src/ck-tasks/ck-tasks-tab-processes.c src/ck-tasks/ck-tasks-tab-applications.c src/ck-tasks/ck-tasks-tab-performance.c src/ck-tasks/ck-tasks-tab-networking.c src/ck-tasks/ck-tasks-tab-services.c src/ck-tasks/ck-tasks-tab-users.c src/ck-tasks/ck-tasks-tab-simple.c src/ck-tasks/ck-tasks-ui-helpers.c src/ck-load/vertical_meter.c src/shared/session_utils.c src/shared/session_utils.h src/shared/about_dialog.c src/shared/about_dialog.h src/shared/ck-table/ck_table.c src/shared/table/table_widget.c src/shared/gridlayout/gridlayout.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/ck-tasks/ck-tasks.c src/ck-tasks/ck-tasks-ctrl.c src/ck-tasks/ck-tasks-model.c src/ck-tasks/ck-tasks-ui.c src/ck-tasks/ck-tasks-tab-processes.c src/ck-tasks/ck-tasks-tab-applications.c src/ck-tasks/ck-tasks-tab-performance.c src/ck-tasks/ck-tasks-tab-networking.c src/ck-tasks/ck-tasks-tab-services.c src/ck-tasks/ck-tasks-tab-users.c src/ck-tasks/ck-tasks-tab-simple.c src/ck-tasks/ck-tasks-ui-helpers.c src/ck-load/vertical_meter.c src/shared/session_utils.c src/shared/about_dialog.c src/shared/ck-table/ck_table.c src/shared/table/table_widget.c src/shared/gridlayout/gridlayout.c -o $@ $(CDE_LDFLAGS) $(CDE_LIBS)

# ck-mixer
$(BIN_DIR)/ck-mixer: src/ck-mixer/ck-mixer.c src/shared/session_utils.c src/shared/session_utils.h src/shared/config_utils.c src/shared/config_utils.h src/shared/about_dialog.c src/shared/about_dialog.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/ck-mixer/ck-mixer.c src/shared/session_utils.c src/shared/config_utils.c src/shared/about_dialog.c -o $@ $(CDE_LDFLAGS) $(CDE_LIBS) -lasound

# ck-clock (does not depend on CDE, only Motif/X11 + cairo)
$(BIN_DIR)/ck-clock: src/ck-clock/ck-clock.c src/ck-clock/ck-clock-time.c src/ck-clock/ck-clock-calendar.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/ck-clock/ck-clock.c src/ck-clock/ck-clock-time.c src/ck-clock/ck-clock-calendar.c -o $@ $(CDE_LDFLAGS) -lX11 -lcairo -lXm -lXt -lm

# ck-calc
$(BIN_DIR)/ck-calc: src/ck-calc/ck-calc.c src/ck-calc/app_state_utils.c src/ck-calc/logic/display_api.c src/ck-calc/logic/formula_eval.c src/ck-calc/logic/calc_state.c src/ck-calc/logic/input_handler.c src/ck-calc/ui/keypad_layout.c src/ck-calc/ui/sci_visuals.c src/ck-calc/ui/window_metrics.c src/ck-calc/clipboard.c src/ck-calc/ui/menu_handlers.c src/shared/session_utils.c src/shared/session_utils.h src/shared/about_dialog.c src/shared/about_dialog.h src/shared/config_utils.c src/shared/config_utils.h src/shared/cde_palette.c src/shared/cde_palette.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/ck-calc/ck-calc.c src/ck-calc/app_state_utils.c src/ck-calc/logic/display_api.c src/ck-calc/logic/formula_eval.c src/ck-calc/logic/calc_state.c src/ck-calc/logic/input_handler.c src/ck-calc/ui/keypad_layout.c src/ck-calc/ui/sci_visuals.c src/ck-calc/ui/window_metrics.c src/ck-calc/clipboard.c src/ck-calc/ui/menu_handlers.c src/shared/session_utils.c src/shared/about_dialog.c src/shared/config_utils.c src/shared/cde_palette.c -o $@ $(CDE_LDFLAGS) $(CDE_LIBS) -lm

# ck-character-map
$(BIN_DIR)/ck-character-map: src/ck-character-map/ck-character-map.c src/shared/session_utils.c src/shared/session_utils.h src/shared/about_dialog.c src/shared/about_dialog.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/ck-character-map/ck-character-map.c src/shared/session_utils.c src/shared/about_dialog.c -o $@ $(CDE_LDFLAGS) $(CDE_LIBS)

# ck-browser
$(BIN_DIR)/ck-browser: src/ck-browser/ck-browser.cpp \
    src/shared/about_dialog.c src/shared/about_dialog.h \
    src/shared/session_utils.c src/shared/session_utils.h \
    src/shared/config_utils.c src/shared/config_utils.h \
    $(CEF_WRAPPER_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) -c src/shared/about_dialog.c -o $(BUILD_DIR)/ck-browser-about_dialog.o
	$(CC) $(CFLAGS) $(CDE_CFLAGS) -c src/shared/session_utils.c -o $(BUILD_DIR)/ck-browser-session_utils.o
	$(CC) $(CFLAGS) $(CDE_CFLAGS) -c src/shared/config_utils.c -o $(BUILD_DIR)/ck-browser-config_utils.o
	$(CXX) $(CXXFLAGS) $(CDE_CFLAGS) $(CEF_CFLAGS) $(CEF_WRAPPER_CFLAGS) -c src/ck-browser/browser_app.cpp -o $(BUILD_DIR)/ck-browser-browser_app.o
	$(CXX) $(CXXFLAGS) $(CDE_CFLAGS) $(CEF_CFLAGS) $(CEF_WRAPPER_CFLAGS) src/ck-browser/ck-browser.cpp $(BUILD_DIR)/ck-browser-about_dialog.o $(BUILD_DIR)/ck-browser-session_utils.o $(BUILD_DIR)/ck-browser-config_utils.o $(BUILD_DIR)/ck-browser-browser_app.o $(CEF_WRAPPER_LIB) -o $@ $(CDE_LDFLAGS) $(CEF_LDFLAGS) $(CEF_RPATH) $(CDE_LIBS) $(CEF_LIBS)

# ck-nibbles (Motif/X11 game, with CDE session dependency)
$(BIN_DIR)/ck-nibbles: src/games/ck-nibbles/ck-nibbles.c src/shared/about_dialog.c src/shared/about_dialog.h src/shared/session_utils.c src/shared/session_utils.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/games/ck-nibbles/ck-nibbles.c src/shared/about_dialog.c src/shared/session_utils.c -o $@ $(CDE_LDFLAGS) $(CDE_LIBS)

# ck-mines (Motif/X11 game, with CDE session dependency)
$(BIN_DIR)/ck-mines: src/games/ck-mines/ck-mines.c src/shared/session_utils.c src/shared/session_utils.h src/shared/about_dialog.c src/shared/about_dialog.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/games/ck-mines/ck-mines.c src/shared/session_utils.c src/shared/about_dialog.c -o $@ $(CDE_LDFLAGS) $(CDE_LIBS)

# ck-plasma-1 (Motif/X11 demo, multi-process animation)
$(BIN_DIR)/ck-plasma-1: src/demos/plasma/ck-plasma-1.c src/demos/plasma/plasma_renderer.c src/shared/session_utils.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/demos/plasma/ck-plasma-1.c src/demos/plasma/plasma_renderer.c src/shared/session_utils.c -o $@ $(CDE_LDFLAGS) $(CDE_LIBS) -lXm -lXt -lX11 -lm

clean:
	rm -rf $(BUILD_DIR)
