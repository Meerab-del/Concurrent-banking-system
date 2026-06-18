#include <stdio.h>

void scheduler_demo(void);
void synchronization_demo(void);
void ipc_demo(void);
void memory_demo(void);

static void print_module_banner(const char *title) {
    printf("\n=== ");
    printf("%s", title);
    printf(" ===\n\n");
}

int main(void) {
    print_module_banner("Running Scheduler Module");
    scheduler_demo();
    print_module_banner("Running Synchronization Module");
    synchronization_demo();
    print_module_banner("Running IPC Module");
    ipc_demo();
    print_module_banner("Running Memory Module");
    memory_demo();
    return 0;
}