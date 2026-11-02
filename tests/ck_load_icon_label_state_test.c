#include <assert.h>

#include "../src/ck-load/icon_label_state.h"

int
main(void)
{
    CkLoadIconLabelState state = CK_LOAD_ICON_LABEL_STATE_INIT;

    assert(ck_load_icon_label_use_cpu_prefix(&state, 32, 32, 36, 38));
    assert(!ck_load_icon_label_use_cpu_prefix(&state, 32, 32, 48, 38));
    assert(!ck_load_icon_label_use_cpu_prefix(&state, 32, 32, 36, 38));

    assert(ck_load_icon_label_use_cpu_prefix(&state, 48, 48, 48, 54));
    assert(!ck_load_icon_label_use_cpu_prefix(&state, 48, 48, 60, 54));
    assert(!ck_load_icon_label_use_cpu_prefix(&state, 48, 48, 36, 54));

    assert(ck_load_icon_label_use_cpu_prefix(&state, 64, 64, 48, 70));
    return 0;
}
