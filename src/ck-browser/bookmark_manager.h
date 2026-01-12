#ifndef CK_BROWSER_BOOKMARK_MANAGER_H
#define CK_BROWSER_BOOKMARK_MANAGER_H

#include <X11/Xlib.h>
#include <memory>
#include <string>
#include <vector>

struct BookmarkEntry {
    std::string name;
    std::string url;
    bool show_in_menu = false;
    std::vector<unsigned char> icon_raw;
    std::vector<unsigned char> icon_png;
    int icon_width = 0;
    int icon_height = 0;
};

struct BookmarkGroup {
    std::string name;
    std::vector<std::unique_ptr<BookmarkEntry>> entries;
    std::vector<std::unique_ptr<BookmarkGroup>> children;
};

std::string escape_html(const std::string &input);

BookmarkGroup *ensure_bookmark_groups();
std::unique_ptr<BookmarkGroup> create_default_bookmark_tree();
BookmarkGroup *get_selected_bookmark_group();
void set_selected_bookmark_group(BookmarkGroup *group);
BookmarkGroup *get_bookmark_root();
void replace_bookmark_root(std::unique_ptr<BookmarkGroup> root);
BookmarkGroup *add_bookmark_group(BookmarkGroup *parent, const char *name);
void collect_bookmark_menu_entries(BookmarkGroup *group, std::vector<BookmarkEntry *> &entries);
BookmarkEntry *find_bookmark_by_url(BookmarkGroup *group, const std::string &url, BookmarkGroup **out_group);
BookmarkGroup *find_bookmark_parent_group(BookmarkGroup *group, BookmarkEntry *entry);
std::unique_ptr<BookmarkEntry> detach_bookmark_entry_from_group(BookmarkGroup *group, BookmarkEntry *entry);
BookmarkGroup *find_parent_group(BookmarkGroup *root, BookmarkGroup *target);
bool is_group_descendant_or_same(BookmarkGroup *candidate, BookmarkGroup *ancestor);
void move_all_bookmarks_from_subtree(BookmarkGroup *source, BookmarkGroup *target);
void remove_bookmark_group_from_parent(BookmarkGroup *parent, BookmarkGroup *group);
const char *get_bookmarks_file_path();
void save_bookmarks_to_file();
std::unique_ptr<BookmarkGroup> load_bookmarks_from_file();

#endif // CK_BROWSER_BOOKMARK_MANAGER_H
