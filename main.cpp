#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"
#include <windows.h>
#include <commdlg.h>
#include <tchar.h>
#include <GL/gl.h>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>

// ============================================================
//  GAP BUFFER (with line offset cache for O(1) line lookups)
// ============================================================
class GapBuffer {
    std::vector<char> data;
    size_t gap_start = 0;
    size_t gap_end = 0;
    mutable std::vector<size_t> line_offsets;
    mutable bool lines_dirty = true;

    void grow() {
        size_t old_size = data.size();
        size_t old_gap_len = gap_end - gap_start;
        data.resize(old_size * 2);
        size_t tail_len = old_size - gap_end;
        if (tail_len > 0)
            memmove(&data[data.size() - tail_len], &data[gap_end], tail_len);
        gap_start = old_size - old_gap_len;
        gap_end = data.size() - tail_len;
    }

    void rebuild_lines() const {
        line_offsets.clear();
        line_offsets.push_back(0);
        size_t sz = size();
        for (size_t i = 0; i < sz; ++i) {
            if (get(i) == '\n') {
                line_offsets.push_back(i + 1);
            }
        }
        lines_dirty = false;
    }

    void ensure_lines() const {
        if (lines_dirty) rebuild_lines();
    }

public:
    GapBuffer() { data.resize(4096, '\0'); gap_end = data.size(); }

    size_t size() const { return data.size() - (gap_end - gap_start); }

    void move_gap(size_t pos) {
        if (pos == gap_start) return;
        if (pos < gap_start) {
            size_t count = gap_start - pos;
            memmove(&data[gap_end - count], &data[pos], count);
            gap_start -= count; gap_end -= count;
        } else {
            size_t count = pos - gap_start;
            memmove(&data[gap_start], &data[gap_end], count);
            gap_start += count; gap_end += count;
        }
    }

    void insert(char c) {
        if (gap_start == gap_end) grow();
        data[gap_start++] = c;
        lines_dirty = true;
    }

    void insert(const char* s, size_t len) {
        for (size_t i = 0; i < len; ++i) insert(s[i]);
    }

    void erase(size_t pos, size_t len = 1) {
        move_gap(pos);
        gap_end = std::min(gap_end + len, data.size());
        lines_dirty = true;
    }

    char get(size_t pos) const {
        return (pos < gap_start) ? data[pos] : data[pos + (gap_end - gap_start)];
    }

    void clear() {
        data.resize(4096);
        gap_start = 0;
        gap_end = data.size();
        line_offsets.clear();
        line_offsets.push_back(0);
        lines_dirty = false;
    }

    size_t line_count() const {
        ensure_lines();
        return line_offsets.size();
    }

    size_t line_start(size_t pos) const {
        ensure_lines();
        if (pos == 0) return 0;
        size_t line = line_of(pos);
        if (line < line_offsets.size()) return line_offsets[line];
        return size();
    }

    size_t line_end(size_t pos) const {
        ensure_lines();
        size_t line = line_of(pos);
        if (line + 1 < line_offsets.size()) {
            // line_offsets[line+1] is the start of next line, so -1 is the \n position
            return line_offsets[line + 1] - 1;
        }
        return size();
    }

    size_t prev_line_start(size_t pos) const {
        ensure_lines();
        size_t line = line_of(pos);
        if (line == 0) return 0;
        return line_offsets[line - 1];
    }

    size_t next_line_start(size_t pos) const {
        ensure_lines();
        size_t line = line_of(pos);
        if (line + 1 < line_offsets.size()) return line_offsets[line + 1];
        return size();
    }

    size_t col(size_t pos) const { return pos - line_start(pos); }

    size_t line_of(size_t pos) const {
        ensure_lines();
        if (line_offsets.empty()) return 0;
        if (pos >= size()) return line_offsets.size() - 1;
        // Binary search: largest i where line_offsets[i] <= pos
        size_t lo = 0, hi = line_offsets.size();
        while (lo + 1 < hi) {
            size_t mid = (lo + hi) / 2;
            if (line_offsets[mid] <= pos) lo = mid;
            else hi = mid;
        }
        return lo;
    }

    size_t pos_from_line_col(size_t line, size_t target_col) const {
        ensure_lines();
        if (line >= line_offsets.size()) return size();
        size_t start = line_offsets[line];
        size_t end = (line + 1 < line_offsets.size()) ? line_offsets[line + 1] - 1 : size();
        size_t len = end - start;
        if (target_col > len) target_col = len;
        return start + target_col;
    }
};

// ============================================================
//  CLIPBOARD WINDOWS
// ============================================================
static void SetClipboardText(const std::string& text) {
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (hMem) {
        memcpy(GlobalLock(hMem), text.c_str(), text.size() + 1);
        GlobalUnlock(hMem);
        SetClipboardData(CF_TEXT, hMem);
    }
    CloseClipboard();
}

static std::string GetClipboardText() {
    if (!OpenClipboard(NULL)) return "";
    HANDLE hData = GetClipboardData(CF_TEXT);
    if (!hData) { CloseClipboard(); return ""; }
    char* pszText = static_cast<char*>(GlobalLock(hData));
    std::string text = pszText ? pszText : "";
    GlobalUnlock(hData);
    CloseClipboard();
    return text;
}

// ============================================================
//  DIALOGOS DE ARCHIVO
// ============================================================
static std::string OpenFileDialog(HWND owner) {
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "All files\0*.*\0Text files\0*.txt\0C++ files\0*.cpp;*.h;*.hpp\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) return filename;
    return "";
}

static std::string SaveFileDialog(HWND owner, const char* default_name = "") {
    char filename[MAX_PATH] = "";
    if (default_name) strncpy(filename, default_name, MAX_PATH - 1);
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "All files\0*.*\0Text files\0*.txt\0C++ files\0*.cpp;*.h;*.hpp\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameA(&ofn)) return filename;
    return "";
}

// ============================================================
//  FILE IO
// ============================================================
static bool LoadFile(const char* filename, GapBuffer& buf) {
    std::ifstream f(filename, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    size_t sz = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    buf.clear();
    if (sz > 0) {
        std::vector<char> tmp(sz);
        f.read(tmp.data(), sz);
        buf.insert(tmp.data(), sz);
    }
    return true;
}

static bool SaveFile(const char* filename, const GapBuffer& buf) {
    std::ofstream f(filename, std::ios::binary);
    if (!f) return false;
    for (size_t i = 0; i < buf.size(); ++i) f.put(buf.get(i));
    return true;
}

static std::string GetFilenameFromPath(const std::string& path) {
    size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

// ============================================================
//  SYNTAX HIGHLIGHTING BASICO
// ============================================================
static bool IsKeywordChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

static bool IsKeyword(const std::string& s) {
    static const char* keywords[] = {
        "alignas","alignof","and","and_eq","asm","auto","bitand","bitor","bool","break",
        "case","catch","char","char8_t","char16_t","char32_t","class","compl","concept",
        "const","consteval","constexpr","constinit","const_cast","continue","co_await",
        "co_return","co_yield","decltype","default","delete","do","double","dynamic_cast",
        "else","enum","explicit","export","extern","false","float","for","friend","goto",
        "if","inline","int","long","mutable","namespace","new","noexcept","not","not_eq",
        "nullptr","operator","or","or_eq","private","protected","public","register",
        "reinterpret_cast","requires","return","short","signed","sizeof","static",
        "static_assert","static_cast","struct","switch","template","this","thread_local",
        "throw","true","try","typedef","typeid","typename","union","unsigned","using",
        "virtual","void","volatile","wchar_t","while","xor","xor_eq",
        "include","define","ifdef","ifndef","endif","pragma","undef","elif","error","warning"
    };
    for (const char* k : keywords) if (s == k) return true;
    return false;
}

static ImU32 GetTokenColor(int token_type, bool dark) {
    if (dark) {
        if (token_type == 1) return IM_COL32(86, 156, 214, 255);
        if (token_type == 2) return IM_COL32(181, 206, 168, 255);
        if (token_type == 3) return IM_COL32(214, 157, 133, 255);
        if (token_type == 4) return IM_COL32(106, 153, 85, 255);
        if (token_type == 5) return IM_COL32(197, 134, 192, 255);
        return IM_COL32(212, 212, 212, 255);
    } else {
        if (token_type == 1) return IM_COL32(0, 0, 255, 255);
        if (token_type == 2) return IM_COL32(9, 134, 88, 255);
        if (token_type == 3) return IM_COL32(163, 21, 21, 255);
        if (token_type == 4) return IM_COL32(0, 128, 0, 255);
        if (token_type == 5) return IM_COL32(128, 0, 128, 255);
        return IM_COL32(0, 0, 0, 255);
    }
}

static void TokenizeLine(const GapBuffer& buf, size_t start, size_t end,
                         std::vector<std::pair<std::string, int>>& out) {
    out.clear();
    size_t i = start;
    while (i < end) {
        char c = buf.get(i);
        if (c == ' ' || c == '\t') {
            size_t j = i;
            while (j < end && (buf.get(j) == ' ' || buf.get(j) == '\t')) ++j;
            std::string ws; ws.reserve(j - i);
            for (size_t k = i; k < j; ++k) ws.push_back(buf.get(k));
            out.push_back({ws, 0});
            i = j; continue;
        }
        if (c == '/' && i + 1 < end && buf.get(i + 1) == '/') {
            std::string comment;
            for (size_t k = i; k < end; ++k) comment.push_back(buf.get(k));
            out.push_back({comment, 4}); break;
        }
        if (c == '#') {
            std::string prep;
            for (size_t k = i; k < end; ++k) prep.push_back(buf.get(k));
            out.push_back({prep, 5}); break;
        }
        if (c == '"' || c == '\'') {
            char quote = c;
            size_t j = i + 1;
            while (j < end && buf.get(j) != quote) { if (buf.get(j) == '\\') ++j; ++j; }
            if (j < end) ++j;
            std::string str;
            for (size_t k = i; k < j; ++k) str.push_back(buf.get(k));
            out.push_back({str, 3}); i = j; continue;
        }
        if (std::isdigit((unsigned char)c)) {
            size_t j = i;
            while (j < end && (std::isdigit((unsigned char)buf.get(j)) || buf.get(j) == '.' || buf.get(j) == 'x' || buf.get(j) == 'X')) ++j;
            std::string num;
            for (size_t k = i; k < j; ++k) num.push_back(buf.get(k));
            out.push_back({num, 2}); i = j; continue;
        }
        if (IsKeywordChar(c)) {
            size_t j = i;
            while (j < end && IsKeywordChar(buf.get(j))) ++j;
            std::string word;
            for (size_t k = i; k < j; ++k) word.push_back(buf.get(k));
            out.push_back({word, IsKeyword(word) ? 1 : 0}); i = j; continue;
        }
        out.push_back({std::string(1, c), 0}); ++i;
    }
}

// ============================================================
//  DOCUMENT (TAB)
// ============================================================
struct Document {
    GapBuffer buffer;
    size_t cursor = 0;
    size_t select_start = 0;
    size_t select_end = 0;
    bool has_selection = false;
    size_t preferred_col = 0;
    float scroll_y = 0.0f;
    std::string filename;
    std::string display_name;
    bool dirty = false;
    bool show_search = false;
    char search_query[256] = {};
    size_t search_result = (size_t)-1;
    bool cursor_moved = false;
    bool selecting_words = false;
    size_t word_select_anchor = 0;
    // Scrollbar state
    bool scrollbar_dragging = false;
    float scrollbar_drag_start_y = 0.0f;
    float scrollbar_drag_start_scroll = 0.0f;

    // Undo/Redo
    struct EditAction {
        enum Type { ACT_INSERT, ACT_REMOVE } type;
        size_t pos;
        std::string text;
    };
    std::vector<EditAction> undo_stack;
    std::vector<EditAction> redo_stack;
    bool recording = true;
    enum { MAX_UNDO = 500 };

    // Goto line
    bool show_goto = false;
    char goto_line_str[16] = {};

    // Replace
    char replace_query[256] = {};

    Document() {}
    Document(const std::string& fname) {
        filename = fname;
        display_name = GetFilenameFromPath(fname);
        LoadFile(fname.c_str(), buffer);
    }

    void set_cursor(size_t pos, bool keep_selection = false) {
        if (pos > buffer.size()) pos = buffer.size();
        if (!keep_selection) {
            select_start = select_end = pos;
            has_selection = false;
        } else {
            select_end = pos;
            has_selection = (select_start != select_end);
        }
        cursor = pos;
        preferred_col = buffer.col(pos);
    }

    void record_insert(size_t pos, const std::string& text) {
        if (!recording || text.empty()) return;
        if (!undo_stack.empty() && undo_stack.back().type == EditAction::ACT_INSERT &&
            undo_stack.back().pos + undo_stack.back().text.size() == pos) {
            undo_stack.back().text += text;
        } else {
            if (undo_stack.size() >= MAX_UNDO) undo_stack.erase(undo_stack.begin());
            undo_stack.push_back({EditAction::ACT_INSERT, pos, text});
        }
        redo_stack.clear();
    }

    void record_delete(size_t pos, const std::string& text) {
        if (!recording || text.empty()) return;
        if (!undo_stack.empty() && undo_stack.back().type == EditAction::ACT_REMOVE &&
            undo_stack.back().pos == pos) {
            undo_stack.back().text += text;
        } else {
            if (undo_stack.size() >= MAX_UNDO) undo_stack.erase(undo_stack.begin());
            undo_stack.push_back({EditAction::ACT_REMOVE, pos, text});
        }
        redo_stack.clear();
    }

    void undo() {
        if (undo_stack.empty()) return;
        recording = false;
        EditAction act = undo_stack.back();
        undo_stack.pop_back();
        if (act.type == EditAction::ACT_INSERT) {
            buffer.erase(act.pos, act.text.size());
            cursor = act.pos;
        } else {
            buffer.move_gap(act.pos);
            buffer.insert(act.text.c_str(), act.text.size());
            cursor = act.pos + act.text.size();
        }
        set_cursor(cursor);
        redo_stack.push_back(act);
        recording = true;
        dirty = true;
        cursor_moved = true;
    }

    void redo() {
        if (redo_stack.empty()) return;
        recording = false;
        EditAction act = redo_stack.back();
        redo_stack.pop_back();
        if (act.type == EditAction::ACT_INSERT) {
            buffer.move_gap(act.pos);
            buffer.insert(act.text.c_str(), act.text.size());
            cursor = act.pos + act.text.size();
        } else {
            buffer.erase(act.pos, act.text.size());
            cursor = act.pos;
        }
        set_cursor(cursor);
        undo_stack.push_back(act);
        recording = true;
        dirty = true;
        cursor_moved = true;
    }

    void delete_selection() {
        if (!has_selection) return;
        size_t start = std::min(select_start, select_end);
        size_t end = std::max(select_start, select_end);
        std::string deleted; deleted.reserve(end - start);
        for (size_t i = start; i < end; ++i) deleted.push_back(buffer.get(i));
        record_delete(start, deleted);
        buffer.erase(start, end - start);
        cursor = start;
        set_cursor(start);
        dirty = true;
        cursor_moved = true;
    }

    void insert_char(char c) {
        if (has_selection) delete_selection();
        buffer.move_gap(cursor);
        buffer.insert(c);
        record_insert(cursor, std::string(1, c));
        cursor++;
        set_cursor(cursor);
        dirty = true;
        cursor_moved = true;
    }

    void insert_string(const std::string& s) {
        if (has_selection) delete_selection();
        buffer.insert(s.c_str(), s.size());
        record_insert(cursor, s);
        cursor += s.size();
        set_cursor(cursor);
        dirty = true;
        cursor_moved = true;
    }

    void backspace() {
        if (has_selection) { delete_selection(); return; }
        if (cursor > 0) {
            char c = buffer.get(cursor - 1);
            buffer.erase(cursor - 1, 1);
            cursor--;
            record_delete(cursor, std::string(1, c));
            set_cursor(cursor);
            dirty = true;
            cursor_moved = true;
        }
    }

    void del() {
        if (has_selection) { delete_selection(); return; }
        if (cursor < buffer.size()) {
            char c = buffer.get(cursor);
            buffer.erase(cursor, 1);
            record_delete(cursor, std::string(1, c));
            dirty = true;
            cursor_moved = true;
        }
    }

    void delete_word_left() {
        if (has_selection) { delete_selection(); return; }
        if (cursor == 0) return;
        size_t start = cursor;
        while (start > 0 && (buffer.get(start - 1) == ' ' || buffer.get(start - 1) == '\t')) --start;
        if (start > 0) {
            bool is_id = IsKeywordChar(buffer.get(start - 1));
            while (start > 0 && IsKeywordChar(buffer.get(start - 1)) == is_id) --start;
        }
        if (start < cursor) {
            std::string deleted; deleted.reserve(cursor - start);
            for (size_t i = start; i < cursor; ++i) deleted.push_back(buffer.get(i));
            record_delete(start, deleted);
            buffer.erase(start, cursor - start);
            cursor = start;
            set_cursor(cursor);
            dirty = true;
            cursor_moved = true;
        }
    }

    void move_left(bool sel = false)  { if (cursor > 0) { set_cursor(cursor - 1, sel); cursor_moved = true; } }
    void move_right(bool sel = false) { if (cursor < buffer.size()) { set_cursor(cursor + 1, sel); cursor_moved = true; } }

    void move_up(bool sel = false) {
        size_t ls = buffer.line_start(cursor);
        if (ls == 0) { set_cursor(0, sel); cursor_moved = true; return; }
        size_t pls = buffer.prev_line_start(cursor);
        size_t ple = buffer.line_end(pls);
        size_t target = pls + preferred_col;
        if (target > ple) target = ple;
        set_cursor(target, sel);
        cursor_moved = true;
    }

    void move_down(bool sel = false) {
        size_t le = buffer.line_end(cursor);
        if (le >= buffer.size()) { set_cursor(buffer.size(), sel); cursor_moved = true; return; }
        size_t nls = le + 1;
        size_t nle = buffer.line_end(nls);
        size_t target = nls + preferred_col;
        if (target > nle) target = nle;
        set_cursor(target, sel);
        cursor_moved = true;
    }

    void move_home(bool sel = false) { set_cursor(buffer.line_start(cursor), sel); cursor_moved = true; }
    void move_end(bool sel = false)  { set_cursor(buffer.line_end(cursor), sel); cursor_moved = true; }

    void goto_line(size_t line) {
        if (line < 1) line = 1;
        size_t max_line = buffer.line_count();
        if (line > max_line) line = max_line;
        size_t pos = buffer.pos_from_line_col(line - 1, 0);
        set_cursor(pos);
        cursor_moved = true;
    }

    void select_all() {
        select_start = 0;
        select_end = buffer.size();
        cursor = buffer.size();
        has_selection = (buffer.size() > 0);
    }

    void select_word_at(size_t pos) {
        if (pos >= buffer.size()) pos = buffer.size();
        if (buffer.size() == 0) { select_start = select_end = cursor = 0; has_selection = false; return; }
        if (pos < buffer.size() && !IsKeywordChar(buffer.get(pos))) {
            size_t start = pos;
            size_t end = pos;
            while (start > 0 && !IsKeywordChar(buffer.get(start - 1))) --start;
            while (end < buffer.size() && !IsKeywordChar(buffer.get(end))) ++end;
            select_start = start; select_end = end; cursor = end;
            has_selection = (start != end);
            return;
        }
        size_t start = pos;
        while (start > 0 && IsKeywordChar(buffer.get(start - 1))) --start;
        size_t end = pos;
        while (end < buffer.size() && IsKeywordChar(buffer.get(end))) ++end;
        select_start = start; select_end = end; cursor = end;
        has_selection = (start != end);
    }

    std::string get_selection_text() const {
        if (!has_selection) return "";
        size_t start = std::min(select_start, select_end);
        size_t end = std::max(select_start, select_end);
        std::string result; result.reserve(end - start);
        for (size_t i = start; i < end; ++i) result.push_back(buffer.get(i));
        return result;
    }

    void copy() { if (has_selection) SetClipboardText(get_selection_text()); }
    void cut()  { if (has_selection) { SetClipboardText(get_selection_text()); delete_selection(); } }
    void paste() { std::string text = GetClipboardText(); if (!text.empty()) insert_string(text); }

    void find_next() {
        size_t qLen = strlen(search_query);
        if (qLen == 0) return;
        size_t start = cursor + 1;
        for (size_t i = start; i + qLen <= buffer.size(); ++i) {
            bool match = true;
            for (size_t j = 0; j < qLen; ++j)
                if (buffer.get(i + j) != search_query[j]) { match = false; break; }
            if (match) { search_result = i; set_cursor(i + qLen); cursor_moved = true; return; }
        }
        for (size_t i = 0; i + qLen <= buffer.size() && i < start; ++i) {
            bool match = true;
            for (size_t j = 0; j < qLen; ++j)
                if (buffer.get(i + j) != search_query[j]) { match = false; break; }
            if (match) { search_result = i; set_cursor(i + qLen); cursor_moved = true; return; }
        }
    }

    void replace_next() {
        size_t qLen = strlen(search_query);
        size_t rLen = strlen(replace_query);
        if (qLen == 0) return;
        if (search_result != (size_t)-1 && search_result + qLen <= buffer.size()) {
            bool match = true;
            for (size_t j = 0; j < qLen; ++j)
                if (buffer.get(search_result + j) != search_query[j]) { match = false; break; }
            if (match) {
                std::string del; del.reserve(qLen);
                for (size_t k = 0; k < qLen; ++k) del.push_back(buffer.get(search_result + k));
                record_delete(search_result, del);
                buffer.erase(search_result, qLen);
                if (rLen > 0) {
                    buffer.move_gap(search_result);
                    buffer.insert(replace_query, rLen);
                    record_insert(search_result, std::string(replace_query, rLen));
                }
                cursor = search_result + rLen;
                dirty = true; cursor_moved = true;
                find_next(); return;
            }
        }
        find_next();
    }

    void replace_all() {
        size_t qLen = strlen(search_query);
        size_t rLen = strlen(replace_query);
        if (qLen == 0) return;
        recording = false;
        size_t i = 0;
        while (i + qLen <= buffer.size()) {
            bool match = true;
            for (size_t j = 0; j < qLen; ++j)
                if (buffer.get(i + j) != search_query[j]) { match = false; break; }
            if (match) {
                buffer.erase(i, qLen);
                if (rLen > 0) {
                    buffer.move_gap(i);
                    buffer.insert(replace_query, rLen);
                }
                i += rLen;
            } else ++i;
        }
        recording = true;
        undo_stack.clear(); redo_stack.clear(); // Clear undo for bulk replace
        dirty = true; cursor_moved = true;
        search_result = (size_t)-1;
    }

    void save() {
        if (!filename.empty()) {
            SaveFile(filename.c_str(), buffer);
            dirty = false;
        }
    }
};

// ============================================================
//  APP STATE
// ============================================================
struct App {
    std::vector<Document> docs;
    int active_doc = -1;
    bool dark_theme = true;
    HWND hwnd;
    bool block_shortcuts = false;
    std::vector<std::string> recent_files;

    Document& doc() { return docs[active_doc]; }
    bool has_doc() const { return active_doc >= 0 && active_doc < (int)docs.size(); }

    void new_doc() {
        docs.emplace_back();
        docs.back().display_name = "untitled";
        active_doc = (int)docs.size() - 1;
    }

    void add_recent(const std::string& fname) {
        if (fname.empty()) return;
        // Eliminar si ya existe
        auto it = std::find(recent_files.begin(), recent_files.end(), fname);
        if (it != recent_files.end()) recent_files.erase(it);
        // Agregar al principio
        recent_files.insert(recent_files.begin(), fname);
        // Mantener maximo 10
        if (recent_files.size() > 10) recent_files.resize(10);
    }

    void open_doc(const std::string& fname) {
        for (int i = 0; i < (int)docs.size(); ++i) {
            if (docs[i].filename == fname) { active_doc = i; return; }
        }
        docs.emplace_back(fname);
        active_doc = (int)docs.size() - 1;
        add_recent(fname);
    }

    void close_doc(int idx) {
        if (idx < 0 || idx >= (int)docs.size()) return;
        docs.erase(docs.begin() + idx);
        if (docs.empty()) active_doc = -1;
        else if (active_doc >= (int)docs.size()) active_doc = (int)docs.size() - 1;
    }
};

// ============================================================
//  WIN32 / OPENGL / IMGUI
// ============================================================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            glViewport(0, 0, LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProc(hWnd, msg, wParam, lParam);
}

// ============================================================
//  RENDER EDITOR
// ============================================================
static bool RenderScrollbar(Document& ed, ImVec2 canvas_p0, ImVec2 canvas_sz, bool dark) {
    float line_height = ImGui::GetTextLineHeight();
    float canvas_h = canvas_sz.y;
    float content_height = ed.buffer.line_count() * line_height;

    if (content_height <= canvas_h) return false; // No scrollbar needed

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();

    const float scrollbar_width = 12.0f;
    const float scrollbar_x = canvas_p0.x + canvas_sz.x - scrollbar_width - 2.0f;
    const float scrollbar_y = canvas_p0.y + 2.0f;
    const float scrollbar_h = canvas_h - 4.0f;
    const float thumb_min_height = 30.0f;

    float max_scroll = content_height - canvas_h;
    float scroll_ratio = ed.scroll_y / max_scroll;

    // Calculate thumb size proportional to visible content
    float thumb_h = (canvas_h / content_height) * scrollbar_h;
    if (thumb_h < thumb_min_height) thumb_h = thumb_min_height;

    float track_h = scrollbar_h - thumb_h;
    float thumb_y = scrollbar_y + scroll_ratio * track_h;

    // Track (background)
    ImVec2 track_p0(scrollbar_x, scrollbar_y);
    ImVec2 track_p1(scrollbar_x + scrollbar_width, scrollbar_y + scrollbar_h);

    // Determine hover/active states
    ImVec2 mouse = io.MousePos;
    bool hovering_track = (mouse.x >= track_p0.x && mouse.x <= track_p1.x &&
                           mouse.y >= track_p0.y && mouse.y <= track_p1.y);
    bool hovering_thumb = (mouse.x >= track_p0.x && mouse.x <= track_p1.x &&
                           mouse.y >= thumb_y && mouse.y <= thumb_y + thumb_h);

    // Draw track (subtle, only visible on hover or when dragging)
    if (ed.scrollbar_dragging || hovering_track) {
        ImU32 track_col = dark ? IM_COL32(60, 60, 65, 120) : IM_COL32(200, 200, 200, 120);
        draw_list->AddRectFilled(track_p0, track_p1, track_col, 6.0f);
    }

    // Draw thumb
    ImU32 thumb_col;
    if (ed.scrollbar_dragging) {
        thumb_col = dark ? IM_COL32(140, 140, 150, 255) : IM_COL32(100, 100, 110, 255);
    } else if (hovering_thumb) {
        thumb_col = dark ? IM_COL32(120, 120, 130, 255) : IM_COL32(120, 120, 130, 255);
    } else {
        thumb_col = dark ? IM_COL32(80, 80, 85, 200) : IM_COL32(160, 160, 160, 200);
    }

    ImVec2 thumb_p0(scrollbar_x + 2.0f, thumb_y);
    ImVec2 thumb_p1(scrollbar_x + scrollbar_width - 2.0f, thumb_y + thumb_h);
    draw_list->AddRectFilled(thumb_p0, thumb_p1, thumb_col, 5.0f);

    // Handle interactions
    if (ed.scrollbar_dragging) {
        if (ImGui::IsMouseDown(0)) {
            float delta_y = mouse.y - ed.scrollbar_drag_start_y;
            float new_scroll_ratio = (ed.scrollbar_drag_start_scroll + delta_y) / track_h;
            if (new_scroll_ratio < 0.0f) new_scroll_ratio = 0.0f;
            if (new_scroll_ratio > 1.0f) new_scroll_ratio = 1.0f;
            ed.scroll_y = new_scroll_ratio * max_scroll;
        } else {
            ed.scrollbar_dragging = false;
        }
    } else {
        // Click on thumb to start dragging
        if (hovering_thumb && ImGui::IsMouseClicked(0)) {
            ed.scrollbar_dragging = true;
            ed.scrollbar_drag_start_y = mouse.y;
            ed.scrollbar_drag_start_scroll = (thumb_y - scrollbar_y);
        }
        // Click on track to page jump
        else if (hovering_track && ImGui::IsMouseClicked(0) && !hovering_thumb) {
            if (mouse.y < thumb_y) {
                ed.scroll_y -= canvas_h * 0.8f;
            } else {
                ed.scroll_y += canvas_h * 0.8f;
            }
        }
    }

    return hovering_track || hovering_thumb || ed.scrollbar_dragging;
}

static bool RenderEditor(App& app, ImVec2 canvas_p0, ImVec2 canvas_sz, bool canvas_hovered) {
    if (!app.has_doc()) return false;
    Document& ed = app.doc();
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    bool dark = app.dark_theme;

    float line_height = ImGui::GetTextLineHeight();
    float char_width = ImGui::CalcTextSize("M").x;
    const float line_num_width = 50.0f;
    const float text_x = canvas_p0.x + line_num_width;
    float canvas_h = canvas_sz.y;

    // Scroll con rueda del mouse (capturado desde el main loop)
    if (canvas_hovered && !ed.scrollbar_dragging) {
        float wheel = io.MouseWheel;
        if (wheel != 0.0f) {
            ed.scroll_y -= wheel * line_height * 3.0f;
        }
    }

    size_t total_lines = ed.buffer.line_count();
    float content_height = total_lines * line_height;
    float max_scroll = content_height - canvas_h;
    if (max_scroll < 0.0f) max_scroll = 0.0f;
    if (ed.scroll_y < 0.0f) ed.scroll_y = 0.0f;
    if (ed.scroll_y > max_scroll) ed.scroll_y = max_scroll;

    // Auto-scroll to cursor (SOLO si el cursor se movio con teclado en este frame)
    if (ed.cursor_moved) {
        float cursor_line_y = (float)ed.buffer.line_of(ed.cursor) * line_height;
        if (cursor_line_y < ed.scroll_y) ed.scroll_y = cursor_line_y - line_height;
        else if (cursor_line_y + line_height * 2 > ed.scroll_y + canvas_h) ed.scroll_y = cursor_line_y + line_height * 2 - canvas_h;
        ed.cursor_moved = false;
    }

    size_t first_line = (size_t)(ed.scroll_y / line_height);

    draw_list->PushClipRect(canvas_p0, ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y), true);

    // Background line numbers
    draw_list->AddRectFilled(
        ImVec2(canvas_p0.x, canvas_p0.y),
        ImVec2(canvas_p0.x + line_num_width, canvas_p0.y + canvas_sz.y),
        dark ? IM_COL32(30, 30, 30, 255) : IM_COL32(240, 240, 240, 255)
    );

    size_t current_line = first_line;
    size_t pos = ed.buffer.pos_from_line_col(first_line, 0);

    float y = canvas_p0.y - fmodf(ed.scroll_y, line_height);
    std::vector<std::pair<std::string, int>> tokens;

    while (pos < ed.buffer.size() && y < canvas_p0.y + canvas_h) {
        size_t line_start = pos;
        while (pos < ed.buffer.size() && ed.buffer.get(pos) != '\n') ++pos;
        size_t line_end = pos;

        // Line number
        char line_num[16];
        snprintf(line_num, sizeof(line_num), "%4zu", current_line + 1);
        ImU32 ln_col = dark ? IM_COL32(100, 100, 100, 255) : IM_COL32(120, 120, 120, 255);
        if (ed.buffer.line_of(ed.cursor) == current_line)
            ln_col = dark ? IM_COL32(200, 200, 200, 255) : IM_COL32(50, 50, 50, 255);
        draw_list->AddText(ImVec2(canvas_p0.x + 4, y), ln_col, line_num);

        // Highlight current line
        if (current_line == ed.buffer.line_of(ed.cursor)) {
            draw_list->AddRectFilled(
                ImVec2(canvas_p0.x + line_num_width, y), ImVec2(canvas_p0.x + canvas_sz.x, y + line_height),
                dark ? IM_COL32(40, 44, 52, 255) : IM_COL32(235, 235, 240, 255)
            );
        }

        // Selection background for this line
        if (ed.has_selection) {
            size_t sel_start = std::min(ed.select_start, ed.select_end);
            size_t sel_end = std::max(ed.select_start, ed.select_end);
            if (sel_start < line_end && sel_end > line_start) {
                float sx = text_x;
                size_t sc = (sel_start > line_start) ? (sel_start - line_start) : 0;
                size_t ec = (sel_end < line_end) ? (sel_end - line_start) : (line_end - line_start);
                std::string prefix;
                for (size_t k = line_start; k < line_start + sc && k < line_end; ++k) prefix.push_back(ed.buffer.get(k));
                sx += ImGui::CalcTextSize(prefix.c_str()).x;
                float ex = sx;
                std::string sel_str;
                for (size_t k = line_start + sc; k < line_start + ec && k < line_end; ++k) sel_str.push_back(ed.buffer.get(k));
                ex += ImGui::CalcTextSize(sel_str.c_str()).x;
                draw_list->AddRectFilled(
                    ImVec2(sx, y), ImVec2(ex, y + line_height),
                    dark ? IM_COL32(55, 75, 105, 180) : IM_COL32(180, 210, 240, 180)
                );
            }
        }

        // Syntax highlighting
        TokenizeLine(ed.buffer, line_start, line_end, tokens);
        float x = text_x;
        for (auto& tk : tokens) {
            ImU32 col = GetTokenColor(tk.second, dark);
            draw_list->AddText(ImVec2(x, y), col, tk.first.c_str());
            x += ImGui::CalcTextSize(tk.first.c_str()).x;
        }

        // Cursor
        size_t cursor_line = ed.buffer.line_of(ed.cursor);
        if (current_line == cursor_line) {
            size_t col_in_line = ed.buffer.col(ed.cursor);
            float cursor_x = text_x;
            for (auto& tk : tokens) {
                if (col_in_line >= tk.first.size()) {
                    cursor_x += ImGui::CalcTextSize(tk.first.c_str()).x;
                    col_in_line -= tk.first.size();
                } else {
                    std::string prefix = tk.first.substr(0, col_in_line);
                    cursor_x += ImGui::CalcTextSize(prefix.c_str()).x;
                    break;
                }
            }
            double t = ImGui::GetTime();
            if (fmod(t, 1.0) < 0.5) {
                draw_list->AddLine(
                    ImVec2(cursor_x, y + 1), ImVec2(cursor_x, y + line_height - 1),
                    dark ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255), 2.0f
                );
            }
        }

        // Search highlight
        if (ed.search_result != (size_t)-1 && ed.search_query[0] != '\0') {
            size_t sr = ed.search_result;
            size_t sr_end = sr + strlen(ed.search_query);
            if (sr < line_end && sr_end > line_start) {
                float hx = text_x;
                size_t hc = (sr > line_start) ? (sr - line_start) : 0;
                size_t he = (sr_end < line_end) ? (sr_end - line_start) : (line_end - line_start);
                std::string hprefix;
                for (size_t k = line_start; k < line_start + hc && k < line_end; ++k) hprefix.push_back(ed.buffer.get(k));
                hx += ImGui::CalcTextSize(hprefix.c_str()).x;
                float he_x = hx;
                std::string hstr;
                for (size_t k = line_start + hc; k < line_start + he && k < line_end; ++k) hstr.push_back(ed.buffer.get(k));
                he_x += ImGui::CalcTextSize(hstr.c_str()).x;
                draw_list->AddRectFilled(
                    ImVec2(hx, y), ImVec2(he_x, y + line_height),
                    IM_COL32(255, 255, 0, 80)
                );
            }
        }

        y += line_height;
        if (pos < ed.buffer.size() && ed.buffer.get(pos) == '\n') ++pos;
        ++current_line;
    }

    draw_list->PopClipRect();

    // RENDER SCROLLBAR (outside clip rect so it's always visible)
    // Retorna true si el mouse esta interactuando con el scrollbar
    return RenderScrollbar(ed, canvas_p0, canvas_sz, dark);
}

// ============================================================
//  HANDLE INPUT
// ============================================================
static void HandleInput(App& app) {
    if (!app.has_doc()) return;
    Document& ed = app.doc();
    ImGuiIO& io = ImGui::GetIO();
    bool ctrl = io.KeyCtrl;
    bool shift = io.KeyShift;

    // Si acabamos de cerrar un dialogo nativo, bloqueamos shortcuts hasta que suelten las teclas
    if (app.block_shortcuts) {
        if (!ctrl && !shift && !io.KeyAlt) {
            app.block_shortcuts = false;
        } else {
            // Solo permitimos tipear caracteres normales, no shortcuts
            for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
                ImWchar c = io.InputQueueCharacters[i];
                if (c != 0 && c < 0x10000 && ((c >= 32 && c < 127) || c == '\n' || c == '\t'))
                    ed.insert_char((char)c);
            }
            return;
        }
    }

    for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
        ImWchar c = io.InputQueueCharacters[i];
        if (c != 0 && c < 0x10000) {
            if (c == '\r') c = '\n';
            // Solo insertar caracteres imprimibles o control valido (enter, tab)
            // Filtrar caracteres de control como DEL (127) que manda Ctrl+Backspace
            if ((c >= 32 && c < 127) || c == '\n' || c == '\t')
                ed.insert_char((char)c);
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { ed.show_search = false; ed.show_goto = false; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_F)) { ed.show_search = !ed.show_search; ed.show_goto = false; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_H)) { ed.show_search = !ed.show_search; ed.show_goto = false; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_G)) { ed.show_goto = !ed.show_goto; ed.show_search = false; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N)) app.new_doc();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
        app.block_shortcuts = true;
        std::string f = OpenFileDialog(app.hwnd);
        if (!f.empty()) app.open_doc(f);
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        if (!ed.filename.empty()) { ed.save(); }
        else {
            app.block_shortcuts = true;
            std::string f = SaveFileDialog(app.hwnd, "untitled.txt");
            if (!f.empty()) { ed.filename = f; ed.display_name = GetFilenameFromPath(f); ed.save(); app.add_recent(f); }
        }
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A)) ed.select_all();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C)) ed.copy();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X)) ed.cut();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V)) ed.paste();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) ed.undo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) ed.redo();
    if (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_Z)) ed.redo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Backspace)) ed.delete_word_left();
    else if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) ed.backspace();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) ed.del();

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        if (ctrl) {
            if (ed.cursor > 0) {
                do { ed.move_left(shift); } while (ed.cursor > 0 && ed.buffer.get(ed.cursor - 1) == ' ');
                while (ed.cursor > 0 && IsKeywordChar(ed.buffer.get(ed.cursor - 1))) ed.move_left(shift);
            }
        } else ed.move_left(shift);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        if (ctrl) {
            if (ed.cursor < ed.buffer.size()) {
                do { ed.move_right(shift); } while (ed.cursor < ed.buffer.size() && ed.buffer.get(ed.cursor) == ' ');
                while (ed.cursor < ed.buffer.size() && IsKeywordChar(ed.buffer.get(ed.cursor))) ed.move_right(shift);
            }
        } else ed.move_right(shift);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    ed.move_up(shift);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  ed.move_down(shift);
    if (ImGui::IsKeyPressed(ImGuiKey_Home))       ed.move_home(shift);
    if (ImGui::IsKeyPressed(ImGuiKey_End))        ed.move_end(shift);
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        // Auto-indent: copy leading whitespace from current line
        size_t line_start = ed.buffer.line_start(ed.cursor);
        std::string indent;
        for (size_t i = line_start; i < ed.buffer.size() && (ed.buffer.get(i) == ' ' || ed.buffer.get(i) == '\t'); ++i)
            indent.push_back(ed.buffer.get(i));
        ed.insert_char('\n');
        if (!indent.empty()) ed.insert_string(indent);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        // Insert 4 spaces instead of raw tab character
        ed.insert_string("    ");
    }
}

// ============================================================
//  MOUSE CLICK -> CURSOR POSITION
// ============================================================
static size_t PosFromMouse(Document& ed, ImVec2 canvas_p0, ImVec2 mouse_pos) {
    float line_height = ImGui::GetTextLineHeight();
    float char_width = ImGui::CalcTextSize("M").x;
    const float line_num_width = 50.0f;
    const float text_x = canvas_p0.x + line_num_width;

    float rel_y = mouse_pos.y - canvas_p0.y + ed.scroll_y;
    float rel_x = mouse_pos.x - text_x;

    size_t line = (size_t)std::max(0.0f, rel_y / line_height);
    size_t col = (size_t)std::max(0.0f, rel_x / char_width + 0.5f);
    return ed.buffer.pos_from_line_col(line, col);
}

static void HandleMouseClick(App& app, ImVec2 canvas_p0, bool clicked, bool dragging, bool double_clicked) {
    if (!app.has_doc()) return;
    Document& ed = app.doc();
    ImGuiIO& io = ImGui::GetIO();

    size_t pos = PosFromMouse(ed, canvas_p0, io.MousePos);

    if (double_clicked) {
        ed.select_word_at(pos);
        ed.selecting_words = true;
        ed.word_select_anchor = ed.select_start;
        return;
    }

    if (clicked) {
        ed.selecting_words = false;
        ed.set_cursor(pos);
    } else if (dragging) {
        if (ed.selecting_words) {
            // Seleccion por palabras (doble click + drag)
            size_t anchor = ed.word_select_anchor;
            if (pos < anchor) {
                // Extender hacia atras
                size_t word_start = pos;
                while (word_start > 0 && IsKeywordChar(ed.buffer.get(word_start - 1))) --word_start;
                ed.select_start = anchor;
                // Encontrar fin de palabra desde anchor
                size_t word_end = anchor;
                while (word_end < ed.buffer.size() && IsKeywordChar(ed.buffer.get(word_end))) ++word_end;
                ed.select_end = word_start;
                ed.cursor = word_start;
                ed.has_selection = true;
            } else if (pos > anchor) {
                // Extender hacia adelante
                size_t word_start = pos;
                while (word_start > 0 && IsKeywordChar(ed.buffer.get(word_start - 1))) --word_start;
                size_t word_end = pos;
                while (word_end < ed.buffer.size() && IsKeywordChar(ed.buffer.get(word_end))) ++word_end;
                ed.select_start = anchor;
                ed.select_end = word_end;
                ed.cursor = word_end;
                ed.has_selection = true;
            } else {
                ed.select_start = anchor;
                ed.select_end = anchor;
                ed.cursor = anchor;
                ed.has_selection = false;
            }
        } else {
            ed.set_cursor(pos, true);
        }
    }
}

// ============================================================
//  STYLE SETUP
// ============================================================
static void SetupStyle(bool dark) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    if (dark) {
        // VS Code Dark+ colors
        colors[ImGuiCol_Text]                   = ImVec4(0.85f, 0.87f, 0.91f, 1.00f);
        colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
        colors[ImGuiCol_WindowBg]               = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
        colors[ImGuiCol_ChildBg]                = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        colors[ImGuiCol_PopupBg]                = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
        colors[ImGuiCol_Border]                 = ImVec4(0.15f, 0.15f, 0.17f, 1.00f);
        colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_FrameBg]                = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
        colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
        colors[ImGuiCol_FrameBgActive]          = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
        colors[ImGuiCol_TitleBg]                = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        colors[ImGuiCol_TitleBgActive]          = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        colors[ImGuiCol_MenuBarBg]              = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.35f, 0.35f, 0.38f, 1.00f);
        colors[ImGuiCol_CheckMark]              = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        colors[ImGuiCol_SliderGrab]             = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.46f, 0.77f, 1.00f, 1.00f);
        colors[ImGuiCol_Button]                 = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
        colors[ImGuiCol_ButtonHovered]          = ImVec4(0.30f, 0.30f, 0.35f, 1.00f);
        colors[ImGuiCol_ButtonActive]           = ImVec4(0.35f, 0.35f, 0.40f, 1.00f);
        colors[ImGuiCol_Header]                 = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
        colors[ImGuiCol_HeaderHovered]          = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
        colors[ImGuiCol_HeaderActive]           = ImVec4(0.35f, 0.35f, 0.38f, 1.00f);
        colors[ImGuiCol_Separator]              = ImVec4(0.15f, 0.15f, 0.17f, 1.00f);
        colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
        colors[ImGuiCol_SeparatorActive]        = ImVec4(0.35f, 0.35f, 0.38f, 1.00f);
        colors[ImGuiCol_ResizeGrip]             = ImVec4(0.26f, 0.59f, 0.98f, 0.20f);
        colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
        colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
        colors[ImGuiCol_Tab]                    = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
        colors[ImGuiCol_TabHovered]             = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
        colors[ImGuiCol_TabActive]              = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
        colors[ImGuiCol_TabUnfocused]           = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
        colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
        colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
    } else {
        // Light theme
        ImGui::StyleColorsLight();
    }

    // Frame padding and rounding for modern look
    style.FramePadding     = ImVec2(6, 3);
    style.ItemSpacing      = ImVec2(6, 4);
    style.ItemInnerSpacing = ImVec2(4, 4);
    style.WindowRounding   = 0.0f;
    style.ChildRounding    = 0.0f;
    style.FrameRounding    = 3.0f;
    style.PopupRounding    = 4.0f;
    style.ScrollbarRounding= 4.0f;
    style.TabRounding      = 3.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize  = 0.0f;
    style.PopupBorderSize  = 1.0f;
    style.ChildBorderSize  = 0.0f;
    style.TabBorderSize    = 0.0f;
}

// ============================================================
//  MAIN
// ============================================================
int main(int, char**)
{
    HINSTANCE hInstance = GetModuleHandle(NULL);
    WNDCLASSEX wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, NULL, NULL, NULL, NULL, _T("FastEditor"), NULL };
    ::RegisterClassEx(&wc);
    HWND hwnd = ::CreateWindow(wc.lpszClassName, _T("FastEditor - VS Code Style"), WS_OVERLAPPEDWINDOW,
                               100, 100, 1280, 800, NULL, NULL, hInstance, NULL);
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);
    HDC hdc = ::GetDC(hwnd);

    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1,
        PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    int pf = ::ChoosePixelFormat(hdc, &pfd);
    ::SetPixelFormat(hdc, pf, &pfd);
    HGLRC rc = ::wglCreateContext(hdc);
    ::wglMakeCurrent(hdc, rc);

    typedef BOOL (WINAPI * PFNWGLSWAPINTERVALEXTPROC)(int interval);
    PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
    if (wglSwapIntervalEXT) wglSwapIntervalEXT(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    // Desactivamos NavEnableKeyboard porque en un editor de texto las flechas van al cursor, no a los botones
    // io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    SetupStyle(true);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplOpenGL3_Init("#version 130");

    App app;
    app.hwnd = hwnd;
    app.new_doc(); // Start with one empty doc

    bool done = false;

    while (!done) {
        MSG msg;
        bool has_msg = false;
        while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            has_msg = true;
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (!has_msg && !ImGui::IsAnyItemActive()) {
            ::Sleep(8);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("FastEditor", NULL,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_MenuBar);

        // ---- MENU BAR ----
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New File", "Ctrl+N")) app.new_doc();
                if (ImGui::MenuItem("Open File...", "Ctrl+O")) {
                    app.block_shortcuts = true;
                    std::string f = OpenFileDialog(hwnd);
                    if (!f.empty()) app.open_doc(f);
                }
                if (ImGui::BeginMenu("Open Recent", !app.recent_files.empty())) {
                    for (const std::string& rf : app.recent_files) {
                        std::string name = GetFilenameFromPath(rf);
                        if (ImGui::MenuItem(name.c_str())) {
                            app.open_doc(rf);
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Clear Recent")) {
                        app.recent_files.clear();
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (app.has_doc()) {
                    Document& ed = app.doc();
                    if (ImGui::MenuItem("Save", "Ctrl+S", false, !ed.filename.empty())) ed.save();
                    if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) {
                        app.block_shortcuts = true;
                        std::string f = SaveFileDialog(hwnd, ed.filename.empty() ? "untitled.txt" : ed.filename.c_str());
                        if (!f.empty()) { ed.filename = f; ed.display_name = GetFilenameFromPath(f); ed.save(); app.add_recent(f); }
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                if (app.has_doc()) {
                    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !app.doc().undo_stack.empty())) app.doc().undo();
                    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !app.doc().redo_stack.empty())) app.doc().redo();
                    ImGui::Separator();
                    if (ImGui::MenuItem("Cut", "Ctrl+X")) app.doc().cut();
                    if (ImGui::MenuItem("Copy", "Ctrl+C")) app.doc().copy();
                    if (ImGui::MenuItem("Paste", "Ctrl+V")) app.doc().paste();
                    ImGui::Separator();
                    if (ImGui::MenuItem("Find", "Ctrl+F")) { app.doc().show_search = !app.doc().show_search; app.doc().show_goto = false; }
                    if (ImGui::MenuItem("Find & Replace", "Ctrl+H")) { app.doc().show_search = !app.doc().show_search; app.doc().show_goto = false; }
                    if (ImGui::MenuItem("Go to Line", "Ctrl+G")) { app.doc().show_goto = !app.doc().show_goto; app.doc().show_search = false; }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Select All", "Ctrl+A")) app.doc().select_all();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem(app.dark_theme ? "Switch to Light Theme" : "Switch to Dark Theme")) {
                    app.dark_theme = !app.dark_theme;
                    SetupStyle(app.dark_theme);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        ImGui::PopStyleVar();
        ImGui::Separator();

        // ---- TABS (VS Code style) ----
        if (!app.docs.empty()) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            float tab_height = ImGui::GetTextLineHeight() + 10;
            ImVec2 win_pos = ImGui::GetCursorScreenPos();
            float avail_width = ImGui::GetContentRegionAvail().x;
            float x = win_pos.x;

            for (int i = 0; i < (int)app.docs.size(); ++i) {
                bool active = (i == app.active_doc);
                Document& doc = app.docs[i];
                std::string label = doc.display_name;
                if (doc.dirty) label += "  ";

                ImVec2 text_size = ImGui::CalcTextSize(label.c_str());
                float tab_width = text_size.x + 30; // extra space for close button and padding
                if (x + tab_width > win_pos.x + avail_width) break; // don't overflow

                // Tab background
                ImVec2 tab_p0(x, win_pos.y);
                ImVec2 tab_p1(x + tab_width, win_pos.y + tab_height);
                ImDrawList* dl = ImGui::GetWindowDrawList();

                ImU32 tab_bg = active
                    ? (app.dark_theme ? IM_COL32(30, 30, 35, 255) : IM_COL32(240, 240, 240, 255))
                    : (app.dark_theme ? IM_COL32(18, 18, 20, 255) : IM_COL32(220, 220, 220, 255));
                ImU32 tab_border = app.dark_theme ? IM_COL32(60, 60, 70, 255) : IM_COL32(180, 180, 180, 255);

                dl->AddRectFilled(tab_p0, tab_p1, tab_bg);
                if (active) dl->AddRectFilled(tab_p0, ImVec2(tab_p1.x, tab_p0.y + 2), IM_COL32(0, 120, 212, 255)); // blue top border for active
                dl->AddRect(tab_p0, tab_p1, tab_border, 0.0f, 0, 0.5f);

                // Tab text
                ImU32 text_col = active
                    ? (app.dark_theme ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255))
                    : (app.dark_theme ? IM_COL32(150, 150, 150, 255) : IM_COL32(100, 100, 100, 255));
                dl->AddText(ImVec2(x + 10, win_pos.y + (tab_height - text_size.y) * 0.5f), text_col, label.c_str());

                // Dirty indicator (circle)
                if (doc.dirty) {
                    ImVec2 dot_center(x + text_size.x + 14, win_pos.y + tab_height * 0.5f);
                    dl->AddCircleFilled(dot_center, 3.0f, IM_COL32(200, 200, 200, 255));
                }

                // Close button (invisible button for interaction)
                ImGui::SetCursorScreenPos(ImVec2(x + tab_width - 20, win_pos.y + (tab_height - 14) * 0.5f));
                ImGui::PushID(i);
                if (ImGui::Button("x", ImVec2(14, 14))) {
                    app.close_doc(i);
                }
                ImGui::PopID();

                // Tab click area
                ImGui::SetCursorScreenPos(tab_p0);
                ImGui::InvisibleButton(("tab" + std::to_string(i)).c_str(), ImVec2(tab_width - 22, tab_height));
                if (ImGui::IsItemClicked()) app.active_doc = i;

                x += tab_width;
            }
            ImGui::PopStyleVar();
            ImGui::Dummy(ImVec2(0, tab_height));
        }
        ImGui::Separator();

        // ---- SEARCH / REPLACE BAR ----
        if (app.has_doc() && app.doc().show_search) {
            Document& ed = app.doc();
            ImGui::PushStyleColor(ImGuiCol_FrameBg, app.dark_theme ? ImVec4(0.18f, 0.18f, 0.20f, 1.0f) : ImVec4(0.90f, 0.90f, 0.90f, 1.0f));
            ImGui::PushItemWidth(250);
            if (ImGui::InputText("Find", ed.search_query, sizeof(ed.search_query), ImGuiInputTextFlags_EnterReturnsTrue)) {
                ed.find_next();
            }
            ImGui::SameLine();
            if (ImGui::InputText("Replace", ed.replace_query, sizeof(ed.replace_query), ImGuiInputTextFlags_EnterReturnsTrue)) {
                ed.replace_next();
            }
            ImGui::PopItemWidth();
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton("Find Next")) ed.find_next();
            ImGui::SameLine();
            if (ImGui::SmallButton("Replace")) ed.replace_next();
            ImGui::SameLine();
            if (ImGui::SmallButton("Replace All")) ed.replace_all();
            ImGui::SameLine();
            if (ImGui::SmallButton("Close")) ed.show_search = false;
            ImGui::Separator();
        }

        // ---- GOTO LINE BAR ----
        if (app.has_doc() && app.doc().show_goto) {
            Document& ed = app.doc();
            ImGui::PushStyleColor(ImGuiCol_FrameBg, app.dark_theme ? ImVec4(0.18f, 0.18f, 0.20f, 1.0f) : ImVec4(0.90f, 0.90f, 0.90f, 1.0f));
            ImGui::PushItemWidth(100);
            if (ImGui::InputText("Go to line", ed.goto_line_str, sizeof(ed.goto_line_str), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal)) {
                int line = atoi(ed.goto_line_str);
                if (line > 0) ed.goto_line((size_t)line);
                ed.show_goto = false;
                memset(ed.goto_line_str, 0, sizeof(ed.goto_line_str));
            }
            ImGui::PopItemWidth();
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton("Go")) {
                int line = atoi(ed.goto_line_str);
                if (line > 0) ed.goto_line((size_t)line);
                ed.show_goto = false;
                memset(ed.goto_line_str, 0, sizeof(ed.goto_line_str));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Close")) ed.show_goto = false;
            ImGui::Separator();
        }

        // ---- EDITOR CANVAS ----
        float status_bar_height = 22.0f;
        ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
        ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
        canvas_sz.y -= status_bar_height;

        ImGui::InvisibleButton("canvas", canvas_sz);
        bool is_hovered = ImGui::IsItemHovered();
        bool is_active = ImGui::IsItemActive();

        // Zoom with Ctrl + mouse wheel
        if (is_hovered && io.KeyCtrl && io.MouseWheel != 0.0f) {
            float new_scale = io.FontGlobalScale + io.MouseWheel * 0.1f;
            if (new_scale < 0.5f) new_scale = 0.5f;
            if (new_scale > 3.0f) new_scale = 3.0f;
            io.FontGlobalScale = new_scale;
        }

        if (app.has_doc() && !ImGui::IsMouseDown(0)) {
            app.doc().selecting_words = false;
        }

        // Render text FIRST to detect scrollbar interaction
        bool scrollbar_active = RenderEditor(app, canvas_p0, canvas_sz, is_hovered);

        // Only process editor mouse clicks if NOT interacting with scrollbar
        if (!scrollbar_active) {
            if (is_hovered && ImGui::IsMouseDoubleClicked(0)) {
                HandleMouseClick(app, canvas_p0, false, false, true);
            } else if (ImGui::IsItemClicked()) {
                HandleMouseClick(app, canvas_p0, true, false, false);
            }
            if (is_active && ImGui::IsMouseDragging(0)) {
                HandleMouseClick(app, canvas_p0, false, true, false);
            }
        }

        if (is_hovered && ImGui::IsMouseClicked(1)) {
            ImGui::OpenPopup("editor_context");
        }

        if (ImGui::BeginPopup("editor_context")) {
            if (ImGui::MenuItem("Cut", "Ctrl+X")) app.doc().cut();
            if (ImGui::MenuItem("Copy", "Ctrl+C")) app.doc().copy();
            if (ImGui::MenuItem("Paste", "Ctrl+V")) app.doc().paste();
            ImGui::Separator();
            if (ImGui::MenuItem("Select All", "Ctrl+A")) app.doc().select_all();
            ImGui::Separator();
            if (ImGui::MenuItem("Find", "Ctrl+F")) app.doc().show_search = true;
            ImGui::EndPopup();
        }

        // Handle keyboard
        if (is_hovered || is_active) {
            HandleInput(app);
        }

        // ---- STATUS BAR ----
        ImVec2 status_p0 = ImVec2(canvas_p0.x, canvas_p0.y + canvas_sz.y);
        ImVec2 status_p1 = ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y + status_bar_height);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(status_p0, status_p1,
            app.dark_theme ? IM_COL32(0, 122, 204, 255) : IM_COL32(0, 122, 204, 255));

        if (app.has_doc()) {
            Document& ed = app.doc();
            char status[256];
            snprintf(status, sizeof(status),
                "Ln %zu, Col %zu   |   %zu lines   |   %s   |   %s",
                ed.buffer.line_of(ed.cursor) + 1,
                ed.buffer.col(ed.cursor) + 1,
                ed.buffer.line_count(),
                ed.dirty ? "Modified" : "Saved",
                ed.filename.empty() ? "untitled" : ed.filename.c_str());
            dl->AddText(ImVec2(status_p0.x + 10, status_p0.y + 3), IM_COL32(255, 255, 255, 255), status);
        } else {
            dl->AddText(ImVec2(status_p0.x + 10, status_p0.y + 3), IM_COL32(255, 255, 255, 255), "Ready");
        }

        ImGui::End();

        // Render
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        if (app.dark_theme)
            glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        else
            glClearColor(0.95f, 0.95f, 0.95f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        ::SwapBuffers(hdc);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ::wglMakeCurrent(NULL, NULL);
    ::wglDeleteContext(rc);
    ::ReleaseDC(hwnd, hdc);
    ::DestroyWindow(hwnd);
    ::UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 0;
}
