#include "app.h"
#include "ui.h"

static void enable_best_dpi_mode(void) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show_command) {
    (void)previous;
    (void)command_line;
    enable_best_dpi_mode();
    return ui_run(instance, show_command);
}
