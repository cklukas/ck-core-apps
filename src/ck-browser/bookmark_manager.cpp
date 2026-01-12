#include "bookmark_manager.h"

#include <Xm/Xm.h>
#include <algorithm>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif
#include "../shared/config_utils.h"
#ifdef __cplusplus
}
#endif
#include "browser_ui_bridge.h"

namespace {
constexpr const char *kBookmarksFileName = "bookmarks.html";

std::unique_ptr<BookmarkGroup> g_bookmark_root;
BookmarkGroup *g_selected_bookmark_group = NULL;
char g_bookmarks_file_path[PATH_MAX] = "";
bool g_bookmarks_path_ready = false;
time_t g_bookmarks_file_mtime = 0;
}

static void ensure_path_directory(const char *path);
static time_t get_file_mtime(const char *path);
static std::string base64_encode(const std::vector<unsigned char> &data);
static bool base64_decode(const std::string &input, std::vector<unsigned char> &output);
static bool extract_base64_payload(const std::string &value, std::string &out_payload);
static BookmarkGroup *find_or_create_child_group(BookmarkGroup *parent, const std::string &name);
static void write_group_contents(FILE *f, BookmarkGroup *group, int indent);
static void write_indent(FILE *f, int indent);

BookmarkGroup *add_bookmark_group(BookmarkGroup *parent, const char *name)
{
    if (!parent) return NULL;
    parent->children.emplace_back(std::make_unique<BookmarkGroup>());
    BookmarkGroup *child = parent->children.back().get();
    child->name = name ? name : "";
    return child;
}

std::unique_ptr<BookmarkGroup> create_default_bookmark_tree()
{
    auto root = std::make_unique<BookmarkGroup>();
    root->name = "Bookmarks Menu";
    BookmarkGroup *favorites = add_bookmark_group(root.get(), "Favorites");
    add_bookmark_group(favorites, "Work");
    add_bookmark_group(favorites, "Personal");
    BookmarkGroup *other = add_bookmark_group(root.get(), "Other Bookmarks");
    add_bookmark_group(other, "Research");
    add_bookmark_group(other, "Snippets");
    return root;
}

BookmarkGroup *ensure_bookmark_groups()
{
    if (g_bookmark_root) return g_bookmark_root.get();
    std::unique_ptr<BookmarkGroup> loaded = load_bookmarks_from_file();
    if (loaded) {
        g_bookmark_root = std::move(loaded);
    } else {
        g_bookmark_root = create_default_bookmark_tree();
    }
    BookmarkGroup *selected = g_bookmark_root.get();
    if (!g_bookmark_root->children.empty()) {
        selected = g_bookmark_root->children[0].get();
    }
    g_selected_bookmark_group = selected ? selected : g_bookmark_root.get();
    return g_bookmark_root.get();
}

BookmarkGroup *get_selected_bookmark_group()
{
    ensure_bookmark_groups();
    return g_selected_bookmark_group ? g_selected_bookmark_group : g_bookmark_root.get();
}

void set_selected_bookmark_group(BookmarkGroup *group)
{
    g_selected_bookmark_group = group ? group : (g_bookmark_root ? g_bookmark_root.get() : nullptr);
}

BookmarkGroup *get_bookmark_root()
{
    return g_bookmark_root ? g_bookmark_root.get() : ensure_bookmark_groups();
}

void replace_bookmark_root(std::unique_ptr<BookmarkGroup> root)
{
    g_bookmark_root = std::move(root);
    if (!g_bookmark_root) {
        g_selected_bookmark_group = nullptr;
        return;
    }
    BookmarkGroup *selected = g_bookmark_root.get();
    if (!g_bookmark_root->children.empty()) {
        selected = g_bookmark_root->children[0].get();
    }
    g_selected_bookmark_group = selected ? selected : g_bookmark_root.get();
}

void collect_bookmark_menu_entries(BookmarkGroup *group, std::vector<BookmarkEntry *> &entries)
{
    if (!group) return;
    for (const auto &entry : group->entries) {
        if (entry && entry->show_in_menu) {
            entries.push_back(entry.get());
        }
    }
    for (const auto &child : group->children) {
        collect_bookmark_menu_entries(child.get(), entries);
    }
}

BookmarkEntry *find_bookmark_by_url(BookmarkGroup *group, const std::string &url, BookmarkGroup **out_group)
{
    if (!group || url.empty()) return NULL;
    for (const auto &entry : group->entries) {
        if (entry && entry->url == url) {
            if (out_group) *out_group = group;
            return entry.get();
        }
    }
    for (const auto &child : group->children) {
        BookmarkEntry *found = find_bookmark_by_url(child.get(), url, out_group);
        if (found) return found;
    }
    return NULL;
}

BookmarkGroup *find_bookmark_parent_group(BookmarkGroup *group, BookmarkEntry *entry)
{
    if (!group || !entry) return NULL;
    for (const auto &child_entry : group->entries) {
        if (child_entry.get() == entry) {
            return group;
        }
    }
    for (const auto &child : group->children) {
        BookmarkGroup *found = find_bookmark_parent_group(child.get(), entry);
        if (found) return found;
    }
    return NULL;
}

std::unique_ptr<BookmarkEntry> detach_bookmark_entry_from_group(BookmarkGroup *group, BookmarkEntry *entry)
{
    if (!group || !entry) return nullptr;
    auto &entries = group->entries;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->get() == entry) {
            std::unique_ptr<BookmarkEntry> detached = std::move(*it);
            entries.erase(it);
            return detached;
        }
    }
    return nullptr;
}

BookmarkGroup *find_parent_group(BookmarkGroup *root, BookmarkGroup *target)
{
    if (!root || !target) return NULL;
    for (const auto &child : root->children) {
        if (child.get() == target) {
            return root;
        }
        BookmarkGroup *found = find_parent_group(child.get(), target);
        if (found) return found;
    }
    return NULL;
}

bool is_group_descendant_or_same(BookmarkGroup *candidate, BookmarkGroup *ancestor)
{
    if (!candidate || !ancestor) return false;
    if (candidate == ancestor) return true;
    for (const auto &child : ancestor->children) {
        if (is_group_descendant_or_same(candidate, child.get())) return true;
    }
    return false;
}

void move_all_bookmarks_from_subtree(BookmarkGroup *source, BookmarkGroup *target)
{
    if (!source || !target || source == target) return;
    for (auto &entry : source->entries) {
        target->entries.emplace_back(std::move(entry));
    }
    source->entries.clear();
    for (const auto &child : source->children) {
        move_all_bookmarks_from_subtree(child.get(), target);
    }
}

void remove_bookmark_group_from_parent(BookmarkGroup *parent, BookmarkGroup *group)
{
    if (!parent || !group) return;
    auto &children = parent->children;
    for (auto it = children.begin(); it != children.end(); ++it) {
        if (it->get() == group) {
            children.erase(it);
            return;
        }
    }
}

static void write_indent(FILE *f, int indent)
{
    for (int i = 0; i < indent; ++i) {
        fputc(' ', f);
    }
}

std::string escape_html(const std::string &input)
{
    std::string output;
    output.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '&': output += "&amp;"; break;
            case '<': output += "&lt;"; break;
            case '>': output += "&gt;"; break;
            case '\"': output += "&quot;"; break;
            default: output += c; break;
        }
    }
    return output;
}

static void write_bookmark_entry(FILE *f, BookmarkEntry *entry, int indent)
{
    if (!f || !entry) return;
    time_t now = time(NULL);
    write_indent(f, indent);
    std::string icon_attr;
    if (!entry->icon_png.empty()) {
        std::string encoded = base64_encode(entry->icon_png);
        icon_attr = " ICON=\"data:image/png;base64,";
        icon_attr += encoded;
        icon_attr += "\"";
    }
    fprintf(f,
            "<DT><A HREF=\"%s\" ADD_DATE=\"%lld\" LAST_MODIFIED=\"%lld\" LAST_VISIT=\"%lld\" SHOW_IN_MENU=\"%d\"%s>%s</A>\n",
            escape_html(entry->url).c_str(),
            (long long)now,
            (long long)now,
            (long long)now,
            entry->show_in_menu ? 1 : 0,
            icon_attr.c_str(),
            escape_html(entry->name).c_str());
}

static void write_group_contents(FILE *f, BookmarkGroup *group, int indent)
{
    if (!f || !group) return;
    time_t now = time(NULL);
    for (const auto &child : group->children) {
        if (!child) continue;
        write_indent(f, indent);
        fprintf(f,
                "<DT><H3 ADD_DATE=\"%lld\" LAST_MODIFIED=\"%lld\">%s</H3>\n",
                (long long)now,
                (long long)now,
                escape_html(child->name).c_str());
        write_indent(f, indent);
        fprintf(f, "<DL><p>\n");
        write_group_contents(f, child.get(), indent + 4);
        write_indent(f, indent);
        fprintf(f, "</DL><p>\n");
    }
    for (const auto &entry : group->entries) {
        if (!entry) continue;
        write_bookmark_entry(f, entry.get(), indent);
    }
}

static void write_netscape_bookmarks(FILE *f, BookmarkGroup *root)
{
    if (!f || !root) return;
    fprintf(f,
            "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
            "<!-- This is an automatically generated file.\n"
            "     It will be read and overwritten.\n"
            "     DO NOT EDIT! -->\n"
            "<HTML>\n"
            "<HEAD>\n"
            "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
            "<TITLE>Bookmarks</TITLE>\n"
            "</HEAD>\n"
            "<BODY>\n"
            "<H1>Bookmarks</H1>\n\n"
            "<DL><p>\n");
    write_group_contents(f, root, 4);
    fprintf(f, "</DL><p>\n</BODY>\n</HTML>\n");
}

static const char kBase64Chars[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
"abcdefghijklmnopqrstuvwxyz"
"0123456789+/";

static std::string base64_encode(const std::vector<unsigned char> &data)
{
    std::string output;
    size_t len = data.size();
    size_t i = 0;
    output.reserve(((len + 2) / 3) * 4);
    while (i + 2 < len) {
        unsigned char a = data[i++];
        unsigned char b = data[i++];
        unsigned char c = data[i++];
        output.push_back(kBase64Chars[a >> 2]);
        output.push_back(kBase64Chars[((a & 0x3) << 4) | (b >> 4)]);
        output.push_back(kBase64Chars[((b & 0xf) << 2) | (c >> 6)]);
        output.push_back(kBase64Chars[c & 0x3f]);
    }
    size_t remaining = len - i;
    if (remaining == 1) {
        unsigned char a = data[i];
        output.push_back(kBase64Chars[a >> 2]);
        output.push_back(kBase64Chars[(a & 0x3) << 4]);
        output.push_back('=');
        output.push_back('=');
    } else if (remaining == 2) {
        unsigned char a = data[i++];
        unsigned char b = data[i];
        output.push_back(kBase64Chars[a >> 2]);
        output.push_back(kBase64Chars[((a & 0x3) << 4) | (b >> 4)]);
        output.push_back(kBase64Chars[(b & 0xf) << 2]);
        output.push_back('=');
    }
    return output;
}

static bool base64_decode(const std::string &input, std::vector<unsigned char> &output)
{
    auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    size_t len = input.size();
    if (len % 4 != 0) return false;
    size_t padding = 0;
    if (len >= 1 && input[len - 1] == '=') padding++;
    if (len >= 2 && input[len - 2] == '=') padding++;
    size_t out_len = (len / 4) * 3 - padding;
    output.clear();
    output.reserve(out_len);
    for (size_t i = 0; i < len; i += 4) {
        int v0 = value_of(input[i]);
        int v1 = value_of(input[i + 1]);
        int v2 = input[i + 2] == '=' ? 0 : value_of(input[i + 2]);
        int v3 = input[i + 3] == '=' ? 0 : value_of(input[i + 3]);
        if (v0 < 0 || v1 < 0 || (input[i + 2] != '=' && v2 < 0) || (input[i + 3] != '=' && v3 < 0)) {
            return false;
        }
        unsigned char a = (unsigned char)((v0 << 2) | ((v1 & 0x30) >> 4));
        unsigned char b = (unsigned char)(((v1 & 0xf) << 4) | ((v2 & 0x3c) >> 2));
        unsigned char c = (unsigned char)(((v2 & 0x3) << 6) | v3);
        output.push_back(a);
        if (input[i + 2] != '=') {
            output.push_back(b);
        }
        if (input[i + 3] != '=') {
            output.push_back(c);
        }
    }
    return true;
}

static bool extract_base64_payload(const std::string &value, std::string &out_payload)
{
    out_payload.clear();
    size_t comma = value.find(',');
    if (comma == std::string::npos) {
        out_payload = value;
    } else {
        out_payload = value.substr(comma + 1);
    }
    while (!out_payload.empty() && isspace((unsigned char)out_payload.front())) {
        out_payload.erase(out_payload.begin());
    }
    while (!out_payload.empty() && isspace((unsigned char)out_payload.back())) {
        out_payload.pop_back();
    }
    return !out_payload.empty();
}

static bool parse_attribute_value(const std::string &tag, const char *name, std::string &out_value)
{
    std::string lower_name;
    for (const char *c = name; *c; ++c) {
        lower_name.push_back(std::tolower((unsigned char)*c));
    }
    size_t pos = 0;
    while (pos < tag.size()) {
        while (pos < tag.size() && isspace((unsigned char)tag[pos])) ++pos;
        size_t name_start = pos;
        while (pos < tag.size() && tag[pos] != '=' && !isspace((unsigned char)tag[pos])) ++pos;
        if (pos >= tag.size()) break;
        std::string attr_name = tag.substr(name_start, pos - name_start);
        std::string attr_name_lower;
        for (char ch : attr_name) {
            attr_name_lower.push_back(std::tolower((unsigned char)ch));
        }
        while (pos < tag.size() && isspace((unsigned char)tag[pos])) ++pos;
        if (pos >= tag.size() || tag[pos] != '=') continue;
        ++pos;
        while (pos < tag.size() && isspace((unsigned char)tag[pos])) ++pos;
        if (pos >= tag.size()) break;
        char quote = tag[pos];
        if (quote == '\"' || quote == '\'') {
            ++pos;
            size_t value_start = pos;
            while (pos < tag.size() && tag[pos] != quote) ++pos;
            std::string attr_value = tag.substr(value_start, pos - value_start);
            if (attr_name_lower == lower_name) {
                out_value = attr_value;
                return true;
            }
            if (pos < tag.size()) ++pos;
        } else {
            size_t value_start = pos;
            while (pos < tag.size() && !isspace((unsigned char)tag[pos])) ++pos;
            std::string attr_value = tag.substr(value_start, pos - value_start);
            if (attr_name_lower == lower_name) {
                out_value = attr_value;
                return true;
            }
        }
    }
    return false;
}

static bool extract_tag_text(const std::string &content, size_t start, const char *closing_tag, size_t &out_end, std::string &out_text)
{
    size_t closing = content.find(closing_tag, start);
    if (closing == std::string::npos) return false;
    out_text = content.substr(start, closing - start);
    out_end = closing + strlen(closing_tag);
    while (!out_text.empty() && isspace((unsigned char)out_text.front())) {
        out_text.erase(out_text.begin());
    }
    while (!out_text.empty() && isspace((unsigned char)out_text.back())) {
        out_text.pop_back();
    }
    return true;
}

static std::string trim_whitespace(std::string value)
{
    size_t start = 0;
    while (start < value.size() && isspace((unsigned char)value[start])) start++;
    size_t end = value.size();
    while (end > start && isspace((unsigned char)value[end - 1])) end--;
    return value.substr(start, end - start);
}

static BookmarkGroup *find_or_create_child_group(BookmarkGroup *parent, const std::string &name)
{
    if (!parent) return NULL;
    for (const auto &child : parent->children) {
        if (child && child->name == name) {
            return child.get();
        }
    }
    return add_bookmark_group(parent, name.c_str());
}

static void parse_netscape_bookmarks(const std::string &content, BookmarkGroup *root)
{
    if (!root) {
        fprintf(stderr, "[bookmark_manager] parse_netscape_bookmarks called with null root\n");
        return;
    }
    std::vector<BookmarkGroup *> stack;
    stack.push_back(root);
    size_t pos = 0;
    size_t iterations = 0;
    static const size_t kBookmarkParseMaxStack = 128;
    static const size_t kBookmarkParseMaxIterations = 200000;
    while (pos < content.size()) {
        if (++iterations > kBookmarkParseMaxIterations) {
            fprintf(stderr, "[bookmark_manager] parse aborted: too many iterations (%zu)\n", iterations);
            break;
        }
        size_t lt = content.find('<', pos);
        if (lt == std::string::npos) break;
        size_t gt = content.find('>', lt);
        if (gt == std::string::npos) break;
        std::string tag = content.substr(lt + 1, gt - lt - 1);
        size_t name_start = 0;
        while (name_start < tag.size() && isspace((unsigned char)tag[name_start])) {
            name_start++;
        }
        if (name_start >= tag.size()) {
            pos = gt + 1;
            continue;
        }
        bool closing = false;
        if (tag[name_start] == '/') {
            closing = true;
            name_start++;
        }
        size_t name_end = name_start;
        while (name_end < tag.size() && !isspace((unsigned char)tag[name_end])) {
            name_end++;
        }
        std::string name = tag.substr(name_start, name_end - name_start);
        for (char &c : name) {
            c = std::toupper((unsigned char)c);
        }
        if (closing) {
            if (name == "DL") {
                if (stack.size() > 1) {
                    stack.pop_back();
                }
            }
            pos = gt + 1;
            continue;
        }
        if (name == "DL" || name == "P" || name == "DT") {
            pos = gt + 1;
            continue;
        }
        if (name == "H3") {
            std::string folder_name;
            size_t end_pos;
            if (extract_tag_text(content, gt + 1, "</H3>", end_pos, folder_name)) {
                BookmarkGroup *parent = stack.back();
                std::string trimmed = trim_whitespace(folder_name);
                BookmarkGroup *child = find_or_create_child_group(parent, trimmed);
                if (child) {
                    if (stack.size() < kBookmarkParseMaxStack) {
                        stack.push_back(child);
                    } else {
                        stack.clear();
                        stack.push_back(root);
                    }
                }
                pos = end_pos;
                continue;
            }
        } else if (name == "A") {
            std::string href;
            parse_attribute_value(tag, "HREF", href);
            std::string show_in_menu;
            parse_attribute_value(tag, "SHOW_IN_MENU", show_in_menu);
            std::string icon_value;
            parse_attribute_value(tag, "ICON", icon_value);
            std::string link_text;
            size_t end_pos;
            if (extract_tag_text(content, gt + 1, "</A>", end_pos, link_text) && !href.empty()) {
                BookmarkGroup *current = stack.back();
                if (!current) current = root;
                auto entry = std::make_unique<BookmarkEntry>();
                entry->name = trim_whitespace(link_text);
                entry->url = href;
                entry->show_in_menu = (show_in_menu == "1" || show_in_menu == "true" || show_in_menu == "yes");
                if (!icon_value.empty()) {
                    std::string payload;
                    if (extract_base64_payload(icon_value, payload)) {
                        std::vector<unsigned char> icon_bytes;
                        if (base64_decode(payload, icon_bytes)) {
                            entry->icon_png = icon_bytes;
                            entry->icon_width = 0;
                            entry->icon_height = 0;
                        }
                    }
                }
                current->entries.emplace_back(std::move(entry));
                pos = end_pos;
                continue;
            }
        }
        pos = gt + 1;
    }
}

const char *get_bookmarks_file_path()
{
    if (!g_bookmarks_path_ready) {
        config_build_path(g_bookmarks_file_path, sizeof(g_bookmarks_file_path), kBookmarksFileName);
        g_bookmarks_path_ready = true;
    }
    return g_bookmarks_file_path;
}

static time_t get_file_mtime(const char *path)
{
    if (!path || path[0] == '\0') return 0;
    struct stat st;
    if (stat(path, &st) == 0) {
        return st.st_mtime;
    }
    return 0;
}

static void ensure_path_directory(const char *path)
{
    if (!path || path[0] == '\0') return;
    char *sep = const_cast<char *>(strrchr(path, '/'));
    if (!sep) return;
    *sep = '\0';
    mkdir(path, 0755);
    *sep = '/';
}

void save_bookmarks_to_file()
{
    if (!g_bookmark_root) return;
    const char *path = get_bookmarks_file_path();
    if (!path || path[0] == '\0') return;
    ensure_path_directory(path);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    write_netscape_bookmarks(f, g_bookmark_root.get());
    fclose(f);
    g_bookmarks_file_mtime = get_file_mtime(path);
}

std::unique_ptr<BookmarkGroup> load_bookmarks_from_file()
{
    const char *path = get_bookmarks_file_path();
    if (!path || path[0] == '\0') return nullptr;
    time_t new_mtime = get_file_mtime(path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        g_bookmarks_file_mtime = new_mtime;
        return nullptr;
    }
    std::string content;
    char buffer[4096];
    size_t read = 0;
    while ((read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        content.append(buffer, read);
    }
    fclose(f);
    auto root = std::make_unique<BookmarkGroup>();
    root->name = "Bookmarks Menu";
    parse_netscape_bookmarks(content, root.get());
    g_bookmarks_file_mtime = new_mtime;
    if (root->children.empty() && root->entries.empty()) {
        return create_default_bookmark_tree();
    }
    return root;
}
