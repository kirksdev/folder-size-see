#include "util.h"

#include <shlwapi.h>
#include <stdlib.h>
#include <wchar.h>

wchar_t *wide_dup(const wchar_t *text) {
    size_t length;
    wchar_t *copy;

    if (!text) return NULL;
    length = wcslen(text) + 1;
    copy = (wchar_t *)malloc(length * sizeof(*copy));
    if (copy) memcpy(copy, text, length * sizeof(*copy));
    return copy;
}

wchar_t *path_join(const wchar_t *left, const wchar_t *right) {
    size_t left_length = wcslen(left);
    size_t right_length = wcslen(right);
    BOOL needs_separator = left_length > 0 && left[left_length - 1] != L'\\' && left[left_length - 1] != L'/';
    wchar_t *joined = (wchar_t *)malloc((left_length + right_length + (needs_separator ? 2 : 1)) * sizeof(*joined));

    if (!joined) return NULL;
    memcpy(joined, left, left_length * sizeof(*joined));
    if (needs_separator) joined[left_length++] = L'\\';
    memcpy(joined + left_length, right, (right_length + 1) * sizeof(*joined));
    return joined;
}

wchar_t *path_extended(const wchar_t *path) {
    size_t length;
    wchar_t *result;

    if (wcsncmp(path, L"\\\\?\\", 4) == 0) return wide_dup(path);
    length = wcslen(path);

    if (wcsncmp(path, L"\\\\", 2) == 0) {
        result = (wchar_t *)malloc((length + 7) * sizeof(*result));
        if (!result) return NULL;
        wcscpy_s(result, length + 7, L"\\\\?\\UNC\\");
        wcscat_s(result, length + 7, path + 2);
        return result;
    }

    result = (wchar_t *)malloc((length + 5) * sizeof(*result));
    if (!result) return NULL;
    wcscpy_s(result, length + 5, L"\\\\?\\");
    wcscat_s(result, length + 5, path);
    return result;
}

wchar_t *path_full(const wchar_t *path) {
    DWORD needed = GetFullPathNameW(path, 0, NULL, NULL);
    wchar_t *full;

    if (!needed) return NULL;
    full = (wchar_t *)malloc((size_t)needed * sizeof(*full));
    if (!full) return NULL;
    if (!GetFullPathNameW(path, needed, full, NULL)) {
        free(full);
        return NULL;
    }
    return full;
}

void format_bytes(ULONGLONG bytes, wchar_t *buffer, size_t count) {
    static const wchar_t *units[] = { L"bytes", L"KB", L"MB", L"GB", L"TB", L"PB" };
    double value = (double)bytes;
    size_t unit = 0;

    while (value >= 1024.0 && unit + 1 < ARRAYSIZE(units)) {
        value /= 1024.0;
        ++unit;
    }

    if (unit == 0) {
        _snwprintf_s(buffer, count, _TRUNCATE, L"%llu bytes", bytes);
    } else if (value >= 100.0) {
        _snwprintf_s(buffer, count, _TRUNCATE, L"%.0f %s", value, units[unit]);
    } else if (value >= 10.0) {
        _snwprintf_s(buffer, count, _TRUNCATE, L"%.1f %s", value, units[unit]);
    } else {
        _snwprintf_s(buffer, count, _TRUNCATE, L"%.2f %s", value, units[unit]);
    }
}

void format_file_time(const FILETIME *time, wchar_t *buffer, size_t count) {
    FILETIME local_time;
    SYSTEMTIME system_time;
    wchar_t date[64];
    wchar_t clock[64];

    if (!FileTimeToLocalFileTime(time, &local_time) || !FileTimeToSystemTime(&local_time, &system_time)) {
        buffer[0] = L'\0';
        return;
    }
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &system_time, NULL, date, ARRAYSIZE(date));
    GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &system_time, NULL, clock, ARRAYSIZE(clock));
    _snwprintf_s(buffer, count, _TRUNCATE, L"%s %s", date, clock);
}

int compare_text_natural(const wchar_t *left, const wchar_t *right) {
    int value = StrCmpLogicalW(left, right);
    return value < 0 ? -1 : value > 0 ? 1 : 0;
}
