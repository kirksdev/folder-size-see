#ifndef FOLDERVIEW_UTIL_H
#define FOLDERVIEW_UTIL_H

#include "app.h"
#include <stddef.h>

wchar_t *wide_dup(const wchar_t *text);
wchar_t *path_join(const wchar_t *left, const wchar_t *right);
wchar_t *path_extended(const wchar_t *path);
wchar_t *path_full(const wchar_t *path);
void format_bytes(ULONGLONG bytes, wchar_t *buffer, size_t count);
void format_file_time(const FILETIME *time, wchar_t *buffer, size_t count);
int compare_text_natural(const wchar_t *left, const wchar_t *right);

#endif
