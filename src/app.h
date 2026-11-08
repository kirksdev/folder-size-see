#ifndef FOLDERVIEW_APP_H
#define FOLDERVIEW_APP_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#define APP_NAME L"Folder Size Explorer"
#define WM_APP_SCAN_RESULT (WM_APP + 1)
#define WM_APP_SCAN_DONE   (WM_APP + 2)

int ui_run(HINSTANCE instance, int show_command);

#endif
