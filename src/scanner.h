#ifndef FOLDERVIEW_SCANNER_H
#define FOLDERVIEW_SCANNER_H

#include "app.h"
#include <stddef.h>

typedef struct ScanRequest {
    size_t entry_index;
    const wchar_t *path;
} ScanRequest;

typedef struct ScanResult {
    UINT generation;
    size_t entry_index;
    ULONGLONG bytes;
    ULONGLONG file_count;
    DWORD error_count;
} ScanResult;

typedef struct FolderScanner {
    HANDLE thread;
    HANDLE cancel_event;
} FolderScanner;

BOOL folder_scanner_start(FolderScanner *scanner, HWND target, UINT generation,
                          const ScanRequest *requests, size_t count);
void folder_scanner_cancel(FolderScanner *scanner);

#endif
