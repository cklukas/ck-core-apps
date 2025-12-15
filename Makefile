CC ?= cc
CFLAGS ?= -O2 -Wall -Isrc

# Where to place build artifacts
BUILD_DIR ?= build
BIN_DIR := $(BUILD_DIR)/bin

# CDE include/lib paths (override on CLI if different on your system)
CDE_PREFIX ?= /usr/local/CDE
CDE_INC ?= $(CDE_PREFIX)/include
CDE_LIB ?= $(CDE_PREFIX)/lib

CDE_CFLAGS := -I$(CDE_INC)
CDE_LDFLAGS := -L$(CDE_LIB)
CDE_LIBS := -lDtSvc -lDtXinerama -lDtWidget -ltt -lXm -lXt -lSM -lICE -lXinerama -lX11

PROGRAMS := $(BIN_DIR)/ck-about \
            $(BIN_DIR)/ck-load \
            $(BIN_DIR)/ck-mixer \
            $(BIN_DIR)/ck-clock \
            $(BIN_DIR)/ck-calc \
            $(BIN_DIR)/ck-nibbles \
            $(BIN_DIR)/ck-mines

.PHONY: all clean

all: $(PROGRAMS)

$(BIN_DIR):
	@mkdir -p $@

# ck-about
$(BIN_DIR)/ck-about: src/ck-about/ck-about.c src/shared/session_utils.c src/shared/session_utils.h src/shared/about_dialog.c src/shared/about_dialog.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/ck-about/ck-about.c src/shared/session_utils.c src/shared/about_dialog.c -o $@ $(CDE_LDFLAGS) $(CDE_LIBS)

# ck-load
$(BIN_DIR)/ck-load: src/ck-load/ck-load.c src/ck-load/vertical_meter.c src/ck-load/vertical_meter.h src/shared/session_utils.c src/shared/session_utils.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/ck-load/ck-load.c src/ck-load/vertical_meter.c src/shared/session_utils.c -o $@ $(CDE_LDFLAGS) $(CDE_LIBS)

# ck-mixer
$(BIN_DIR)/ck-mixer: src/ck-mixer/ck-mixer.c src/shared/session_utils.c src/shared/session_utils.h src/shared/config_utils.c src/shared/config_utils.h src/shared/about_dialog.c src/shared/about_dialog.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/ck-mixer/ck-mixer.c src/shared/session_utils.c src/shared/config_utils.c src/shared/about_dialog.c -o $@ $(CDE_LDFLAGS) $(CDE_LIBS) -lasound

# ck-clock (does not depend on CDE, only Motif/X11 + cairo)
$(BIN_DIR)/ck-clock: src/ck-clock/ck-clock.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) $< -o $@ $(CDE_LDFLAGS) -lX11 -lcairo -lXm -lXt -lm

# ck-calc
$(BIN_DIR)/ck-calc: src/ck-calc/ck-calc.c src/ck-calc/app_state_utils.c src/ck-calc/logic/display_api.c src/ck-calc/logic/formula_eval.c src/ck-calc/logic/calc_state.c src/ck-calc/logic/input_handler.c src/ck-calc/ui/keypad_layout.c src/ck-calc/ui/sci_visuals.c src/ck-calc/ui/window_metrics.c src/ck-calc/clipboard.c src/ck-calc/ui/menu_handlers.c src/shared/session_utils.c src/shared/session_utils.h src/shared/about_dialog.c src/shared/about_dialog.h src/shared/config_utils.c src/shared/config_utils.h src/shared/cde_palette.c src/shared/cde_palette.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/ck-calc/ck-calc.c src/ck-calc/app_state_utils.c src/ck-calc/logic/display_api.c src/ck-calc/logic/formula_eval.c src/ck-calc/logic/calc_state.c src/ck-calc/logic/input_handler.c src/ck-calc/ui/keypad_layout.c src/ck-calc/ui/sci_visuals.c src/ck-calc/ui/window_metrics.c src/ck-calc/clipboard.c src/ck-calc/ui/menu_handlers.c src/shared/session_utils.c src/shared/about_dialog.c src/shared/config_utils.c src/shared/cde_palette.c -o $@ $(CDE_LDFLAGS) $(CDE_LIBS) -lm

# ck-nibbles (Motif/X11 game, with CDE session dependency)
$(BIN_DIR)/ck-nibbles: src/games/ck-nibbles/ck-nibbles.c src/shared/about_dialog.c src/shared/about_dialog.h src/shared/session_utils.c src/shared/session_utils.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/games/ck-nibbles/ck-nibbles.c src/shared/about_dialog.c src/shared/session_utils.c -o $@ $(CDE_LDFLAGS) $(CDE_LIBS)

# ck-mines (Motif/X11 game, with CDE session dependency)
$(BIN_DIR)/ck-mines: src/games/ck-mines/ck-mines.c src/shared/session_utils.c src/shared/session_utils.h src/shared/about_dialog.c src/shared/about_dialog.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CDE_CFLAGS) src/games/ck-mines/ck-mines.c src/shared/session_utils.c src/shared/about_dialog.c -o $@ $(CDE_LDFLAGS) $(CDE_LIBS)

clean:
	rm -rf $(BUILD_DIR)
