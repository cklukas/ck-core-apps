#include "icon_label_state.h"

int
ck_load_icon_label_use_cpu_prefix(CkLoadIconLabelState *state,
                                  int icon_width,
                                  int icon_height,
                                  int full_label_width,
                                  int available_width)
{
    if (!state) return 0;

    if (state->icon_width != icon_width || state->icon_height != icon_height) {
        state->icon_width = icon_width;
        state->icon_height = icon_height;
        state->omit_cpu_prefix = 0;
    }

    if (full_label_width > available_width) {
        state->omit_cpu_prefix = 1;
    }

    return !state->omit_cpu_prefix;
}
