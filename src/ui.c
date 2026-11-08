#include "ui.h"
#include "scanner.h"
#include "util.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdlib.h>
#include <wchar.h>

enum {
    ID_UP = 100,
    ID_BROWSE,
    ID_REFRESH,
    ID_ADDRESS,
    ID_LIST,
    ID_STATUS
};

enum {
    COLUMN_NAME,
    COLUMN_SIZE,
    COLUMN_TYPE,
    COLUMN_MODIFIED,
    COLUMN_COUNT
};

typedef struct Entry {
    wchar_t *name;
    wchar_t type[96];
    ULONGLONG size;
    FILETIME modified;
    DWORD attributes;
    DWORD scan_errors;
    BOOL scan_complete;
} Entry;

typedef struct AppState {
    HWND window;
    HWND up_button;
    HWND browse_button;
    HWND refresh_button;
    HWND address;
    HWND list;
    HWND status;
    HFONT font;
    wchar_t *current_path;
    Entry *entries;
    size_t entry_count;
    size_t entry_capacity;
    UINT generation;
    size_t folders_total;
    size_t folders_complete;
    size_t partial_count;
    int sort_column;
    BOOL sort_ascending;
    FolderScanner scanner;
} AppState;

static AppState *state_from_window(HWND window) {
    return (AppState *)GetWindowLongPtrW(window, GWLP_USERDATA);
}

static int scaled(HWND window, int value) {
    UINT dpi = GetDpiForWindow(window);
    return MulDiv(value, dpi ? (int)dpi : 96, 96);
}

static BOOL is_directory(const Entry *entry) {
    return (entry->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static void free_entries(AppState *state) {
    size_t index;
    for (index = 0; index < state->entry_count; ++index) free(state->entries[index].name);
    free(state->entries);
    state->entries = NULL;
    state->entry_count = 0;
    state->entry_capacity = 0;
}

static Entry *add_entry(AppState *state, const WIN32_FIND_DATAW *data) {
    Entry *grown;
    Entry *entry;
    size_t capacity;

    if (state->entry_count == state->entry_capacity) {
        capacity = state->entry_capacity ? state->entry_capacity * 2 : 64;
        grown = (Entry *)realloc(state->entries, capacity * sizeof(*grown));
        if (!grown) return NULL;
        state->entries = grown;
        state->entry_capacity = capacity;
    }

    entry = &state->entries[state->entry_count++];
    ZeroMemory(entry, sizeof(*entry));
    entry->name = wide_dup(data->cFileName);
    if (!entry->name) {
        --state->entry_count;
        return NULL;
    }
    entry->attributes = data->dwFileAttributes;
    entry->modified = data->ftLastWriteTime;
    entry->size = ((ULONGLONG)data->nFileSizeHigh << 32) | data->nFileSizeLow;
    entry->scan_complete = !(data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                           (data->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    return entry;
}

static void set_entry_type_and_icon(Entry *entry, int *icon) {
    SHFILEINFOW info = { 0 };
    DWORD_PTR result = SHGetFileInfoW(entry->name, entry->attributes, &info, sizeof(info),
                                      SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME | SHGFI_SYSICONINDEX |
                                      SHGFI_SMALLICON);
    if (result) {
        wcsncpy_s(entry->type, ARRAYSIZE(entry->type), info.szTypeName, _TRUNCATE);
        *icon = info.iIcon;
    } else {
        wcscpy_s(entry->type, ARRAYSIZE(entry->type), is_directory(entry) ? L"Folder" : L"File");
        *icon = 0;
    }
    if (is_directory(entry) && (entry->attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        wcscpy_s(entry->type, ARRAYSIZE(entry->type), L"Linked folder");
    }
}

static int compare_filetime(const FILETIME *left, const FILETIME *right) {
    LONG value = CompareFileTime(left, right);
    return value < 0 ? -1 : value > 0 ? 1 : 0;
}

static int CALLBACK compare_list_items(LPARAM left_param, LPARAM right_param, LPARAM context) {
    AppState *state = (AppState *)context;
    const Entry *left = &state->entries[(size_t)left_param - 1];
    const Entry *right = &state->entries[(size_t)right_param - 1];
    int result;

    if (is_directory(left) != is_directory(right)) return is_directory(left) ? -1 : 1;

    switch (state->sort_column) {
        case COLUMN_SIZE:
            if (left->scan_complete != right->scan_complete) result = left->scan_complete ? -1 : 1;
            else result = left->size < right->size ? -1 : left->size > right->size ? 1 : 0;
            break;
        case COLUMN_TYPE:
            result = compare_text_natural(left->type, right->type);
            break;
        case COLUMN_MODIFIED:
            result = compare_filetime(&left->modified, &right->modified);
            break;
        case COLUMN_NAME:
        default:
            result = compare_text_natural(left->name, right->name);
            break;
    }
    if (!result && state->sort_column != COLUMN_NAME) result = compare_text_natural(left->name, right->name);
    return state->sort_ascending ? result : -result;
}

static void update_sort_header(AppState *state) {
    HWND header = ListView_GetHeader(state->list);
    int column;
    for (column = 0; column < COLUMN_COUNT; ++column) {
        HDITEMW item = { 0 };
        item.mask = HDI_FORMAT;
        Header_GetItem(header, column, &item);
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (column == state->sort_column) item.fmt |= state->sort_ascending ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(header, column, &item);
    }
}

static void sort_list(AppState *state) {
    ListView_SortItems(state->list, compare_list_items, (LPARAM)state);
    update_sort_header(state);
}

static void update_status(AppState *state, BOOL done) {
    wchar_t text[256];
    if (!state->folders_total) {
        _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, L"%zu items", state->entry_count);
    } else if (!done && state->folders_complete < state->folders_total) {
        _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE,
                     L"%zu items    Calculating folder sizes: %zu of %zu",
                     state->entry_count, state->folders_complete, state->folders_total);
    } else if (state->partial_count) {
        _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE,
                     L"%zu items    Complete — %zu folder%s had unreadable content (sizes marked *)",
                     state->entry_count, state->partial_count, state->partial_count == 1 ? L"" : L"s");
    } else {
        _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, L"%zu items    Folder sizes complete", state->entry_count);
    }
    SetWindowTextW(state->status, text);
}

static void insert_list_entry(AppState *state, size_t index) {
    Entry *entry = &state->entries[index];
    LVITEMW item = { 0 };
    wchar_t size_text[64];
    wchar_t modified[128];
    int icon;

    set_entry_type_and_icon(entry, &icon);
    item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
    item.iItem = (int)index;
    item.pszText = entry->name;
    item.iImage = icon;
    item.lParam = (LPARAM)(index + 1);
    item.iItem = ListView_InsertItem(state->list, &item);

    if (is_directory(entry)) {
        if (entry->attributes & FILE_ATTRIBUTE_REPARSE_POINT) wcscpy_s(size_text, ARRAYSIZE(size_text), L"—");
        else wcscpy_s(size_text, ARRAYSIZE(size_text), L"Calculating...");
    } else {
        format_bytes(entry->size, size_text, ARRAYSIZE(size_text));
    }
    format_file_time(&entry->modified, modified, ARRAYSIZE(modified));
    ListView_SetItemText(state->list, item.iItem, COLUMN_SIZE, size_text);
    ListView_SetItemText(state->list, item.iItem, COLUMN_TYPE, entry->type);
    ListView_SetItemText(state->list, item.iItem, COLUMN_MODIFIED, modified);
}

static BOOL path_is_directory(const wchar_t *path) {
    wchar_t *extended = path_extended(path);
    DWORD attributes = extended ? GetFileAttributesW(extended) : INVALID_FILE_ATTRIBUTES;
    free(extended);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void show_path_error(HWND owner, const wchar_t *path, DWORD error) {
    wchar_t *system_message = NULL;
    wchar_t message[768];
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0, (wchar_t *)&system_message, 0, NULL);
    _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"Could not open:\n%s\n\n%s",
                 path, system_message ? system_message : L"The folder is unavailable.");
    MessageBoxW(owner, message, APP_NAME, MB_OK | MB_ICONERROR);
    LocalFree(system_message);
}

static BOOL navigate_to(AppState *state, const wchar_t *requested_path) {
    wchar_t *full = path_full(requested_path);
    wchar_t *extended = NULL;
    wchar_t *pattern = NULL;
    WIN32_FIND_DATAW data;
    HANDLE find = INVALID_HANDLE_VALUE;
    ScanRequest *requests = NULL;
    wchar_t **request_paths = NULL;
    size_t request_count = 0;
    size_t index;
    DWORD error = ERROR_PATH_NOT_FOUND;
    BOOL ok = FALSE;

    if (!full || !path_is_directory(full)) {
        error = GetLastError();
        if (error == ERROR_SUCCESS) error = ERROR_PATH_NOT_FOUND;
        show_path_error(state->window, requested_path, error);
        free(full);
        return FALSE;
    }

    extended = path_extended(full);
    pattern = extended ? path_join(extended, L"*") : NULL;
    if (!pattern) {
        show_path_error(state->window, full, ERROR_NOT_ENOUGH_MEMORY);
        goto cleanup;
    }
    find = FindFirstFileExW(pattern, FindExInfoBasic, &data, FindExSearchNameMatch, NULL,
                            FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            show_path_error(state->window, full, error);
            goto cleanup;
        }
    }

    folder_scanner_cancel(&state->scanner);
    ++state->generation;
    ListView_DeleteAllItems(state->list);
    free_entries(state);
    state->folders_total = 0;
    state->folders_complete = 0;
    state->partial_count = 0;

    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
            if (!add_entry(state, &data)) {
                MessageBoxW(state->window, L"There was not enough memory to list this folder.", APP_NAME,
                            MB_OK | MB_ICONERROR);
                FindClose(find);
                find = INVALID_HANDLE_VALUE;
                goto cleanup;
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
        find = INVALID_HANDLE_VALUE;
    }

    free(state->current_path);
    state->current_path = full;
    full = NULL;
    SetWindowTextW(state->address, state->current_path);
    {
        wchar_t title[512];
        const wchar_t *name = PathFindFileNameW(state->current_path);
        if (!name || !*name) name = state->current_path;
        _snwprintf_s(title, ARRAYSIZE(title), _TRUNCATE, L"%s — %s", name, APP_NAME);
        SetWindowTextW(state->window, title);
    }

    for (index = 0; index < state->entry_count; ++index) insert_list_entry(state, index);
    sort_list(state);

    requests = (ScanRequest *)calloc(state->entry_count, sizeof(*requests));
    request_paths = (wchar_t **)calloc(state->entry_count, sizeof(*request_paths));
    if (state->entry_count && (!requests || !request_paths)) {
        MessageBoxW(state->window, L"Folder size scanning could not start because memory is low.",
                    APP_NAME, MB_OK | MB_ICONWARNING);
        update_status(state, TRUE);
        ok = TRUE;
        goto cleanup;
    }

    for (index = 0; index < state->entry_count; ++index) {
        Entry *entry = &state->entries[index];
        if (!is_directory(entry) || (entry->attributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        request_paths[request_count] = path_join(state->current_path, entry->name);
        if (!request_paths[request_count]) continue;
        requests[request_count].entry_index = index;
        requests[request_count].path = request_paths[request_count];
        ++request_count;
    }
    state->folders_total = request_count;
    update_status(state, request_count == 0);
    if (!folder_scanner_start(&state->scanner, state->window, state->generation, requests, request_count)) {
        SetWindowTextW(state->status, L"Folder size scanning could not start.");
    }
    ok = TRUE;

cleanup:
    if (find != INVALID_HANDLE_VALUE) FindClose(find);
    if (request_paths) {
        for (index = 0; index < request_count; ++index) free(request_paths[index]);
    }
    free(request_paths);
    free(requests);
    free(pattern);
    free(extended);
    free(full);
    return ok;
}

static void navigate_from_address(AppState *state) {
    int length = GetWindowTextLengthW(state->address);
    wchar_t *path = (wchar_t *)malloc(((size_t)length + 1) * sizeof(*path));
    if (!path) return;
    GetWindowTextW(state->address, path, length + 1);
    if (length >= 2 && path[0] == L'"' && path[length - 1] == L'"') {
        path[length - 1] = L'\0';
        memmove(path, path + 1, (size_t)length * sizeof(*path));
    }
    if (*path && !navigate_to(state, path) && state->current_path) {
        SetWindowTextW(state->address, state->current_path);
    }
    free(path);
}

static int CALLBACK browse_callback(HWND dialog, UINT message, LPARAM data, LPARAM context) {
    (void)data;
    if (message == BFFM_INITIALIZED && context) {
        SendMessageW(dialog, BFFM_SETSELECTIONW, TRUE, context);
    }
    return 0;
}

static void browse_for_folder(AppState *state) {
    BROWSEINFOW browse = { 0 };
    PIDLIST_ABSOLUTE item;
    wchar_t path[MAX_PATH];

    browse.hwndOwner = state->window;
    browse.lpszTitle = L"Choose a folder to inspect";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    browse.lpfn = browse_callback;
    browse.lParam = (LPARAM)state->current_path;
    item = SHBrowseForFolderW(&browse);
    if (item) {
        if (SHGetPathFromIDListW(item, path)) navigate_to(state, path);
        CoTaskMemFree(item);
    }
}

static void navigate_up(AppState *state) {
    wchar_t *joined;
    wchar_t *parent;
    if (!state->current_path) return;
    joined = path_join(state->current_path, L"..");
    parent = joined ? path_full(joined) : NULL;
    free(joined);
    if (parent) {
        if (_wcsicmp(parent, state->current_path) != 0) navigate_to(state, parent);
        free(parent);
    }
}

static LRESULT CALLBACK address_subclass(HWND control, UINT message, WPARAM wparam, LPARAM lparam,
                                         UINT_PTR id, DWORD_PTR reference) {
    (void)id;
    if (message == WM_KEYDOWN && wparam == VK_RETURN) {
        navigate_from_address((AppState *)reference);
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(control, address_subclass, 1);
    return DefSubclassProc(control, message, wparam, lparam);
}

static void open_selected_folder(AppState *state, int row) {
    LVITEMW item = { 0 };
    size_t index;
    wchar_t *path;
    if (row < 0) return;
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (!ListView_GetItem(state->list, &item) || !item.lParam) return;
    index = (size_t)item.lParam - 1;
    if (index >= state->entry_count || !is_directory(&state->entries[index])) return;
    path = path_join(state->current_path, state->entries[index].name);
    if (path) {
        navigate_to(state, path);
        free(path);
    }
}

static void apply_scan_result(AppState *state, ScanResult *result) {
    LVFINDINFOW find = { 0 };
    int row;
    wchar_t text[72];
    Entry *entry;

    if (result->generation != state->generation || result->entry_index >= state->entry_count) return;
    entry = &state->entries[result->entry_index];
    entry->size = result->bytes;
    entry->scan_errors = result->error_count;
    entry->scan_complete = TRUE;
    ++state->folders_complete;
    if (result->error_count) ++state->partial_count;

    format_bytes(result->bytes, text, ARRAYSIZE(text));
    if (result->error_count) wcscat_s(text, ARRAYSIZE(text), L" *");
    find.flags = LVFI_PARAM;
    find.lParam = (LPARAM)(result->entry_index + 1);
    row = ListView_FindItem(state->list, -1, &find);
    if (row >= 0) ListView_SetItemText(state->list, row, COLUMN_SIZE, text);
    if (state->sort_column == COLUMN_SIZE) sort_list(state);
    update_status(state, FALSE);
}

static void layout_controls(AppState *state, int width, int height) {
    int margin = scaled(state->window, 10);
    int gap = scaled(state->window, 6);
    int button_width = scaled(state->window, 76);
    int toolbar_height = scaled(state->window, 30);
    int status_height = scaled(state->window, 24);
    int x = margin;
    int list_top = margin + toolbar_height + gap;
    int address_width = width - (margin * 2) - (button_width * 3) - (gap * 3);

    if (address_width < scaled(state->window, 120)) address_width = scaled(state->window, 120);
    MoveWindow(state->up_button, x, margin, button_width, toolbar_height, TRUE);
    x += button_width + gap;
    MoveWindow(state->address, x, margin, address_width, toolbar_height, TRUE);
    x += address_width + gap;
    MoveWindow(state->browse_button, x, margin, button_width, toolbar_height, TRUE);
    x += button_width + gap;
    MoveWindow(state->refresh_button, x, margin, button_width, toolbar_height, TRUE);
    MoveWindow(state->list, margin, list_top, width - margin * 2,
               height - list_top - status_height - margin, TRUE);
    MoveWindow(state->status, margin, height - status_height, width - margin * 2, status_height, TRUE);
}

static void create_columns(HWND list) {
    static const wchar_t *names[] = { L"Name", L"Size", L"Type", L"Date modified" };
    static const int widths[] = { 320, 130, 170, 180 };
    int index;
    for (index = 0; index < COLUMN_COUNT; ++index) {
        LVCOLUMNW column = { 0 };
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        column.pszText = (wchar_t *)names[index];
        column.cx = widths[index];
        column.fmt = index == COLUMN_SIZE ? LVCFMT_RIGHT : LVCFMT_LEFT;
        ListView_InsertColumn(list, index, &column);
    }
}

static BOOL create_child_controls(AppState *state) {
    HIMAGELIST images;
    SHFILEINFOW shell_info = { 0 };
    state->up_button = CreateWindowExW(0, L"BUTTON", L"Up", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                       0, 0, 0, 0, state->window, (HMENU)ID_UP, NULL, NULL);
    state->address = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE |
                                     WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, state->window,
                                     (HMENU)ID_ADDRESS, NULL, NULL);
    state->browse_button = CreateWindowExW(0, L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE |
                                           WS_TABSTOP, 0, 0, 0, 0, state->window,
                                           (HMENU)ID_BROWSE, NULL, NULL);
    state->refresh_button = CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE |
                                            WS_TABSTOP, 0, 0, 0, 0, state->window,
                                            (HMENU)ID_REFRESH, NULL, NULL);
    state->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE |
                                  WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
                                  0, 0, 0, 0, state->window, (HMENU)ID_LIST, NULL, NULL);
    state->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    0, 0, 0, 0, state->window, (HMENU)ID_STATUS, NULL, NULL);
    if (!state->up_button || !state->address || !state->browse_button || !state->refresh_button ||
        !state->list || !state->status) return FALSE;

    SendMessageW(state->up_button, WM_SETFONT, (WPARAM)state->font, TRUE);
    SendMessageW(state->address, WM_SETFONT, (WPARAM)state->font, TRUE);
    SendMessageW(state->browse_button, WM_SETFONT, (WPARAM)state->font, TRUE);
    SendMessageW(state->refresh_button, WM_SETFONT, (WPARAM)state->font, TRUE);
    SendMessageW(state->list, WM_SETFONT, (WPARAM)state->font, TRUE);
    SendMessageW(state->status, WM_SETFONT, (WPARAM)state->font, TRUE);
    SetWindowSubclass(state->address, address_subclass, 1, (DWORD_PTR)state);

    ListView_SetExtendedListViewStyle(state->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                   LVS_EX_LABELTIP);
    create_columns(state->list);
    images = (HIMAGELIST)SHGetFileInfoW(L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &shell_info,
                                        sizeof(shell_info), SHGFI_USEFILEATTRIBUTES |
                                        SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    if (images) ListView_SetImageList(state->list, images, LVSIL_SMALL);
    return TRUE;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    AppState *state = state_from_window(window);
    switch (message) {
        case WM_CREATE: {
            NONCLIENTMETRICSW metrics = { 0 };
            state = (AppState *)calloc(1, sizeof(*state));
            if (!state) return -1;
            state->window = window;
            state->sort_column = COLUMN_NAME;
            state->sort_ascending = TRUE;
            SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)state);
            metrics.cbSize = sizeof(metrics);
            if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
                state->font = CreateFontIndirectW(&metrics.lfMessageFont);
            }
            if (!state->font) state->font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            if (!create_child_controls(state)) return -1;
            SetWindowTextW(state->status, L"Choose a folder with Browse, or type a path and press Enter.");
            SetFocus(state->address);
            return 0;
        }
        case WM_SIZE:
            if (state) layout_controls(state, LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_COMMAND:
            if (!state) break;
            switch (LOWORD(wparam)) {
                case ID_UP: navigate_up(state); return 0;
                case ID_BROWSE: browse_for_folder(state); return 0;
                case ID_REFRESH:
                    if (state->current_path) navigate_to(state, state->current_path);
                    return 0;
            }
            break;
        case WM_NOTIFY:
            if (state && ((NMHDR *)lparam)->idFrom == ID_LIST) {
                NMHDR *header = (NMHDR *)lparam;
                if (header->code == NM_DBLCLK) {
                    open_selected_folder(state, ((NMITEMACTIVATE *)lparam)->iItem);
                    return 0;
                }
                if (header->code == LVN_COLUMNCLICK) {
                    int column = ((NMLISTVIEW *)lparam)->iSubItem;
                    if (state->sort_column == column) state->sort_ascending = !state->sort_ascending;
                    else {
                        state->sort_column = column;
                        state->sort_ascending = TRUE;
                    }
                    sort_list(state);
                    return 0;
                }
            }
            break;
        case WM_APP_SCAN_RESULT:
            if (state) apply_scan_result(state, (ScanResult *)lparam);
            free((void *)lparam);
            return 0;
        case WM_APP_SCAN_DONE:
            if (state && (UINT)wparam == state->generation) update_status(state, TRUE);
            return 0;
        case WM_DESTROY:
            if (state) {
                folder_scanner_cancel(&state->scanner);
                free_entries(state);
                free(state->current_path);
                if (state->font && state->font != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(state->font);
                free(state);
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int ui_run(HINSTANCE instance, int show_command) {
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    WNDCLASSEXW window_class = { 0 };
    HWND window;
    MSG message;

    InitCommonControlsEx(&controls);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = L"FolderSizeExplorerWindow";
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    window_class.hIconSm = window_class.hIcon;
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&window_class)) return 1;

    window = CreateWindowExW(0, window_class.lpszClassName, APP_NAME,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 980, 640,
                             NULL, NULL, instance, NULL);
    if (!window) return 1;
    ShowWindow(window, show_command);
    UpdateWindow(window);
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return (int)message.wParam;
}
