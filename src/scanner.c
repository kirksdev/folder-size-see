#include "scanner.h"
#include "util.h"

#include <limits.h>
#include <stdlib.h>
#include <wchar.h>

typedef struct OwnedRequest {
    size_t entry_index;
    wchar_t *path;
} OwnedRequest;

typedef struct ScanJob {
    HWND target;
    UINT generation;
    HANDLE cancel_event;
    OwnedRequest *requests;
    size_t count;
} ScanJob;

typedef struct PathStack {
    wchar_t **items;
    size_t count;
    size_t capacity;
} PathStack;

static BOOL is_canceled(HANDLE event) {
    return WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}

static BOOL stack_push(PathStack *stack, wchar_t *path) {
    wchar_t **grown;
    size_t capacity;

    if (stack->count < stack->capacity) {
        stack->items[stack->count++] = path;
        return TRUE;
    }
    capacity = stack->capacity ? stack->capacity * 2 : 64;
    grown = (wchar_t **)realloc(stack->items, capacity * sizeof(*grown));
    if (!grown) return FALSE;
    stack->items = grown;
    stack->capacity = capacity;
    stack->items[stack->count++] = path;
    return TRUE;
}

static void stack_clear(PathStack *stack) {
    size_t index;
    for (index = 0; index < stack->count; ++index) free(stack->items[index]);
    free(stack->items);
}

static BOOL scan_tree(const wchar_t *root, HANDLE cancel_event, ULONGLONG *bytes,
                      ULONGLONG *file_count, DWORD *error_count) {
    PathStack stack = { 0 };
    wchar_t *extended = path_extended(root);

    if (!extended || !stack_push(&stack, extended)) {
        free(extended);
        ++*error_count;
        return TRUE;
    }

    while (stack.count) {
        wchar_t *directory = stack.items[--stack.count];
        wchar_t *pattern = path_join(directory, L"*");
        WIN32_FIND_DATAW data;
        HANDLE find = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &data,
                                                  FindExSearchNameMatch, NULL,
                                                  FIND_FIRST_EX_LARGE_FETCH) : INVALID_HANDLE_VALUE;
        free(pattern);

        if (find == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) ++*error_count;
            free(directory);
            if (is_canceled(cancel_event)) {
                stack_clear(&stack);
                return FALSE;
            }
            continue;
        }

        do {
            wchar_t *child;
            if (is_canceled(cancel_event)) {
                FindClose(find);
                free(directory);
                stack_clear(&stack);
                return FALSE;
            }
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;

            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                child = path_join(directory, data.cFileName);
                if (!child || !stack_push(&stack, child)) {
                    free(child);
                    ++*error_count;
                }
            } else {
                ULONGLONG size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
                if (ULLONG_MAX - *bytes < size) *bytes = ULLONG_MAX;
                else *bytes += size;
                ++*file_count;
            }
        } while (FindNextFileW(find, &data));

        if (GetLastError() != ERROR_NO_MORE_FILES) ++*error_count;
        FindClose(find);
        free(directory);
    }

    free(stack.items);
    return TRUE;
}

static DWORD WINAPI scan_thread(void *context) {
    ScanJob *job = (ScanJob *)context;
    size_t index;

    for (index = 0; index < job->count && !is_canceled(job->cancel_event); ++index) {
        ScanResult *result = (ScanResult *)calloc(1, sizeof(*result));
        if (!result) break;
        result->generation = job->generation;
        result->entry_index = job->requests[index].entry_index;

        if (!scan_tree(job->requests[index].path, job->cancel_event, &result->bytes,
                       &result->file_count, &result->error_count)) {
            free(result);
            break;
        }
        if (!PostMessageW(job->target, WM_APP_SCAN_RESULT, 0, (LPARAM)result)) free(result);
    }

    if (!is_canceled(job->cancel_event)) {
        PostMessageW(job->target, WM_APP_SCAN_DONE, (WPARAM)job->generation, 0);
    }
    for (index = 0; index < job->count; ++index) free(job->requests[index].path);
    free(job->requests);
    free(job);
    return 0;
}

BOOL folder_scanner_start(FolderScanner *scanner, HWND target, UINT generation,
                          const ScanRequest *requests, size_t count) {
    ScanJob *job;
    size_t index;

    folder_scanner_cancel(scanner);
    if (!count) return TRUE;

    job = (ScanJob *)calloc(1, sizeof(*job));
    scanner->cancel_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!job || !scanner->cancel_event) {
        free(job);
        if (scanner->cancel_event) CloseHandle(scanner->cancel_event);
        scanner->cancel_event = NULL;
        return FALSE;
    }

    job->requests = (OwnedRequest *)calloc(count, sizeof(*job->requests));
    if (!job->requests) {
        CloseHandle(scanner->cancel_event);
        scanner->cancel_event = NULL;
        free(job);
        return FALSE;
    }
    job->target = target;
    job->generation = generation;
    job->cancel_event = scanner->cancel_event;
    job->count = count;

    for (index = 0; index < count; ++index) {
        job->requests[index].entry_index = requests[index].entry_index;
        job->requests[index].path = wide_dup(requests[index].path);
        if (!job->requests[index].path) {
            size_t cleanup;
            for (cleanup = 0; cleanup < index; ++cleanup) free(job->requests[cleanup].path);
            free(job->requests);
            free(job);
            CloseHandle(scanner->cancel_event);
            scanner->cancel_event = NULL;
            return FALSE;
        }
    }

    scanner->thread = CreateThread(NULL, 0, scan_thread, job, 0, NULL);
    if (!scanner->thread) {
        for (index = 0; index < count; ++index) free(job->requests[index].path);
        free(job->requests);
        free(job);
        CloseHandle(scanner->cancel_event);
        scanner->cancel_event = NULL;
        return FALSE;
    }
    return TRUE;
}

void folder_scanner_cancel(FolderScanner *scanner) {
    if (scanner->cancel_event) SetEvent(scanner->cancel_event);
    if (scanner->thread) {
        WaitForSingleObject(scanner->thread, INFINITE);
        CloseHandle(scanner->thread);
    }
    if (scanner->cancel_event) CloseHandle(scanner->cancel_event);
    scanner->thread = NULL;
    scanner->cancel_event = NULL;
}
