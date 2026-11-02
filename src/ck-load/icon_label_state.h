#ifndef CK_LOAD_ICON_LABEL_STATE_H
#define CK_LOAD_ICON_LABEL_STATE_H

typedef struct {
    int icon_width;
    int icon_height;
    int omit_cpu_prefix;
} CkLoadIconLabelState;

#define CK_LOAD_ICON_LABEL_STATE_INIT { -1, -1, 0 }

int ck_load_icon_label_use_cpu_prefix(CkLoadIconLabelState *state,
                                      int icon_width,
                                      int icon_height,
                                      int full_label_width,
                                      int available_width);

#endif
