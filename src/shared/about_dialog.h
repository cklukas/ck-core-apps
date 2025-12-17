#ifndef ABOUT_DIALOG_H
#define ABOUT_DIALOG_H

#include <Xm/Xm.h>

typedef struct {
    const char *label;
    char value[256];
} AboutField;

/* Helpers to gather standard fields */
int about_get_os_fields(AboutField fields[], int max_fields);
int about_get_cde_fields(AboutField fields[], int max_fields);

/* Create a simple title page with a bold title and a subtitle */
Widget about_add_title_page(Widget notebook, int page_number,
                           const char *page_name,
                           const char *tab_label,
                           const char *title_text,
                           const char *subtitle_text);

/* Create a two-column table page */
Widget about_add_table_page(Widget notebook, int page_number,
                            const char *page_name,
                            const char *tab_label,
                            AboutField fields[], int field_count);

/* Build a standard about dialog shell + notebook. The notebook is returned and
 * the caller may optionally keep a reference to the dialog shell. */
Widget about_dialog_build(Widget parent,
                          const char *shell_name,
                          const char *title,
                          Widget *out_shell);

/* Add the standard CK-Core page (with icon) */
Widget about_add_ck_core_page(Widget notebook, int page_number,
                              const char *page_name,
                              const char *tab_label);

/* Set the window manager icon pixmap (iconified window). Returns nonzero on success. */
int about_set_window_icon_from_xpm(Widget toplevel, char **xpm_data);
int about_set_window_icon_ck_core(Widget toplevel);

/* Convenience: add app tab + optional CK-Core tab + CDE + OS.
 * Returns next page number after appended pages.
 */
int about_add_standard_pages(Widget notebook, int start_page,
                             const char *app_tab_label,
                             const char *app_title,
                             const char *app_subtitle,
                             Boolean include_ck_core_tab);

#endif /* ABOUT_DIALOG_H */
