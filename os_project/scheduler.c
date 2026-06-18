#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    int customer_type;
    int id;
    int arrival_time;
    int burst_time;
    int priority;
} ProcessSpec;

typedef struct {
    int process_id;
    int start_time;
    int end_time;
} ScheduleSlice;

typedef struct {
    ScheduleSlice *slices;
    size_t slice_count;
} ScheduleResult;

typedef struct {
    int process_id;
    int waiting_time;
    int turnaround_time;
    int completion_time;
} ProcessMetrics;

typedef struct {
    ProcessSpec spec;
    int remaining_time;
}
ProcessState;

typedef enum {
    CUSTOMER_REGULAR = 0,
    CUSTOMER_PREMIUM,
    CUSTOMER_LOAN_APPLICANT,
    CUSTOMER_CORPORATE,
    CUSTOMER_VIP
} CustomerType;

static const char *customer_type_name(CustomerType type) {
    switch (type) {
        case CUSTOMER_REGULAR:
            return "REGULAR";
        case CUSTOMER_PREMIUM:
            return "PREMIUM";
        case CUSTOMER_LOAN_APPLICANT:
            return "LOAN_APPLICANT";
        case CUSTOMER_CORPORATE:
            return "CORPORATE";
        case CUSTOMER_VIP:
            return "VIP";
        default:
            return "UNKNOWN";
    }
}

static const char *customer_type_short_name(CustomerType type) {
    switch (type) {
        case CUSTOMER_REGULAR:
            return "REG";
        case CUSTOMER_PREMIUM:
            return "PREM";
        case CUSTOMER_LOAN_APPLICANT:
            return "LOAN";
        case CUSTOMER_CORPORATE:
            return "CORP";
        case CUSTOMER_VIP:
            return "VIP";
        default:
            return "UNK";
    }
}

typedef struct {
    int *items;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t size;
} ReadyQueue;

#define GANTT_CELL_WIDTH 10
#define GANTT_TIME_WIDTH 13
#define PROCESS_COLUMN_WIDTH 20
#define WAIT_COLUMN_WIDTH 9
#define TURNAROUND_COLUMN_WIDTH 10

static int compare_fcfs(const void *left, const void *right) {
    const ProcessSpec *a = left;
    const ProcessSpec *b = right;
    if (a->arrival_time != b->arrival_time) {
        return a->arrival_time - b->arrival_time;
    }
    return a->id - b->id;
}

static int compare_priority(const void *left, const void *right) {
    const ProcessSpec *a = left;
    const ProcessSpec *b = right;
    if (a->priority != b->priority) {
        return b->priority - a->priority;
    }
    if (a->arrival_time != b->arrival_time) {
        return a->arrival_time - b->arrival_time;
    }
    return a->id - b->id;
}

static int compare_priority_state(const ProcessState *a, const ProcessState *b) {
    if (a->spec.priority != b->spec.priority) {
        return b->spec.priority - a->spec.priority;
    }
    if (a->spec.arrival_time != b->spec.arrival_time) {
        return a->spec.arrival_time - b->spec.arrival_time;
    }
    return a->spec.id - b->spec.id;
}

static bool append_schedule_slice(ScheduleResult *result, size_t *slice_capacity, size_t *slice_count, int process_id, int start_time, int end_time) {
    if (result == NULL || slice_capacity == NULL || slice_count == NULL) {
        return false;
    }

    if (*slice_count == *slice_capacity) {
        size_t new_capacity = (*slice_capacity == 0) ? 8 : (*slice_capacity * 2);
        ScheduleSlice *expanded = realloc(result->slices, new_capacity * sizeof(*expanded));
        if (expanded == NULL) {
            return false;
        }
        result->slices = expanded;
        *slice_capacity = new_capacity;
    }

    result->slices[*slice_count].process_id = process_id;
    result->slices[*slice_count].start_time = start_time;
    result->slices[*slice_count].end_time = end_time;
    ++(*slice_count);
    return true;
}

static void ready_queue_init(ReadyQueue *queue, size_t capacity) {
    queue->items = malloc(capacity * sizeof(*queue->items));
    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->size = 0;
}

static void ready_queue_destroy(ReadyQueue *queue) {
    if (queue == NULL) {
        return;
    }
    free(queue->items);
    queue->items = NULL;
    queue->capacity = 0;
    queue->head = 0;
    queue->tail = 0;
    queue->size = 0;
}

static bool ready_queue_push(ReadyQueue *queue, int process_index) {
    if (queue == NULL || queue->items == NULL || queue->size == queue->capacity) {
        return false;
    }

    queue->items[queue->tail] = process_index;
    queue->tail = (queue->tail + 1) % queue->capacity;
    ++queue->size;
    return true;
}

static bool ready_queue_pop(ReadyQueue *queue, int *process_index) {
    if (queue == NULL || queue->items == NULL || process_index == NULL || queue->size == 0) {
        return false;
    }

    *process_index = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    --queue->size;
    return true;
}

static bool ready_queue_is_empty(const ReadyQueue *queue) {
    return queue == NULL || queue->size == 0;
}

static void print_centered_text(const char *text, size_t width) {
    size_t text_length = strlen(text);
    if (text_length >= width) {
        printf("%s", text);
        return;
    }

    size_t left_padding = (width - text_length) / 2;
    size_t right_padding = width - text_length - left_padding;
    printf("%*s%s%*s", (int)left_padding, "", text, (int)right_padding, "");
}

static void print_centered_int(int value, size_t width) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d", value);
    print_centered_text(buffer, width);
}

static ScheduleResult build_linear_schedule(const ProcessSpec *processes, size_t process_count, int (*comparator)(const void *, const void *)) {
    ScheduleResult result = {0};
    if (process_count == 0) {
        return result;
    }

    ProcessSpec *ordered = malloc(process_count * sizeof(*ordered));
    if (ordered == NULL) {
        return result;
    }

    for (size_t index = 0; index < process_count; ++index) {
        ordered[index] = processes[index];
    }

    qsort(ordered, process_count, sizeof(*ordered), comparator);

    result.slices = malloc(process_count * sizeof(*result.slices));
    if (result.slices == NULL) {
        free(ordered);
        return result;
    }

    int current_time = 0;
    for (size_t index = 0; index < process_count; ++index) {
        if (current_time < ordered[index].arrival_time) {
            current_time = ordered[index].arrival_time;
        }
        result.slices[index].process_id = ordered[index].id;
        result.slices[index].start_time = current_time;
        current_time += ordered[index].burst_time;
        result.slices[index].end_time = current_time;
    }

    result.slice_count = process_count;
    free(ordered);
    return result;
}

static const ProcessSpec *find_process_spec(const ProcessSpec *processes, size_t process_count, int process_id) {
    for (size_t index = 0; index < process_count; ++index) {
        if (processes[index].id == process_id) {
            return &processes[index];
        }
    }
    return NULL;
}

static int get_completion_time(const ScheduleResult *result, int process_id) {
    int completion_time = -1;
    if (result == NULL) {
        return completion_time;
    }

    for (size_t index = 0; index < result->slice_count; ++index) {
        if (result->slices[index].process_id == process_id) {
            completion_time = result->slices[index].end_time;
        }
    }

    return completion_time;
}

static void print_gantt_chart(const ProcessSpec *processes, size_t process_count, const ScheduleResult *result) {
    if (result == NULL || result->slice_count == 0) {
        printf("No Gantt chart available\n");
        return;
    }

    printf("Gantt Chart\n");
    for (size_t index = 0; index < result->slice_count; ++index) {
        const ProcessSpec *process = find_process_spec(processes, process_count, result->slices[index].process_id);
        if (process != NULL) {
            printf("| %-*s ", GANTT_CELL_WIDTH, customer_type_short_name((CustomerType)process->customer_type));
        } else {
            printf("| %-*s ", GANTT_CELL_WIDTH, "P?");
        }
    }
    printf("|\n");

    print_centered_int(result->slices[0].start_time, GANTT_TIME_WIDTH);
    for (size_t index = 0; index < result->slice_count; ++index) {
        print_centered_int(result->slices[index].end_time, GANTT_TIME_WIDTH);
    }
    printf("\n");
}

static void print_schedule_metrics(const ProcessSpec *processes, size_t process_count, const ScheduleResult *result) {
    if (processes == NULL || result == NULL || process_count == 0 || result->slice_count == 0) {
        printf("No scheduling metrics available\n");
        return;
    }

    printf("%-*s | %*s | %*s\n", PROCESS_COLUMN_WIDTH, "Process", WAIT_COLUMN_WIDTH, "Wait Time", TURNAROUND_COLUMN_WIDTH, "Turnaround");
    printf("---------------------------------------------------\n");

    double total_waiting_time = 0.0;
    double total_turnaround_time = 0.0;

    for (size_t index = 0; index < process_count; ++index) {
        const ProcessSpec *process = find_process_spec(processes, process_count, processes[index].id);
        if (process == NULL) {
            continue;
        }

        int completion_time = get_completion_time(result, process->id);
        if (completion_time < 0) {
            continue;
        }

        int turnaround_time = completion_time - process->arrival_time;
        int waiting_time = turnaround_time - process->burst_time;
        total_waiting_time += (double)waiting_time;
        total_turnaround_time += (double)turnaround_time;

        char process_label[64];
        snprintf(process_label, sizeof(process_label), "[%s] P%d", customer_type_name((CustomerType)process->customer_type), process->id);
        printf("%-*s | %*d | %*d\n", PROCESS_COLUMN_WIDTH, process_label, WAIT_COLUMN_WIDTH, waiting_time, TURNAROUND_COLUMN_WIDTH, turnaround_time);
    }

    printf("Average Waiting Time: %.2f\n", total_waiting_time / (double)process_count);
    printf("Average Turnaround Time: %.2f\n", total_turnaround_time / (double)process_count);
}

static void scheduler_print_report(const char *title, const ProcessSpec *processes, size_t process_count, const ScheduleResult *result) {
    printf("--- %s ---\n", title);
    print_gantt_chart(processes, process_count, result);
    print_schedule_metrics(processes, process_count, result);
}

ScheduleResult scheduler_run_fcfs(const ProcessSpec *processes, size_t process_count) {
    return build_linear_schedule(processes, process_count, compare_fcfs);
}

ScheduleResult scheduler_run_priority(const ProcessSpec *processes, size_t process_count) {
    ScheduleResult result = {0};
    if (processes == NULL || process_count == 0) {
        return result;
    }

    ProcessState *states = malloc(process_count * sizeof(*states));
    if (states == NULL) {
        return result;
    }

    for (size_t index = 0; index < process_count; ++index) {
        states[index].spec = processes[index];
        states[index].remaining_time = processes[index].burst_time;
    }

    size_t slice_capacity = 0;
    size_t slice_count = 0;
    size_t completed = 0;
    int current_time = 0;

    while (completed < process_count) {
        int selected_index = -1;
        for (size_t index = 0; index < process_count; ++index) {
            if (states[index].remaining_time <= 0 || states[index].spec.arrival_time > current_time) {
                continue;
            }
            if (selected_index < 0 || compare_priority_state(&states[index], &states[selected_index]) < 0) {
                selected_index = (int)index;
            }
        }

        if (selected_index < 0) {
            int next_arrival = -1;
            for (size_t index = 0; index < process_count; ++index) {
                if (states[index].remaining_time <= 0 || states[index].spec.arrival_time <= current_time) {
                    continue;
                }
                if (next_arrival < 0 || states[index].spec.arrival_time < next_arrival) {
                    next_arrival = states[index].spec.arrival_time;
                }
            }
            if (next_arrival < 0) {
                break;
            }
            current_time = next_arrival;
            continue;
        }

        int next_preempt_time = -1;
        for (size_t index = 0; index < process_count; ++index) {
            if (states[index].remaining_time <= 0 || states[index].spec.arrival_time <= current_time) {
                continue;
            }
            if (states[index].spec.priority <= states[selected_index].spec.priority) {
                continue;
            }
            if (next_preempt_time < 0 || states[index].spec.arrival_time < next_preempt_time) {
                next_preempt_time = states[index].spec.arrival_time;
            }
        }

        int run_until = current_time + states[selected_index].remaining_time;
        if (next_preempt_time >= 0 && next_preempt_time < run_until) {
            run_until = next_preempt_time;
        }

        if (!append_schedule_slice(&result, &slice_capacity, &slice_count, states[selected_index].spec.id, current_time, run_until)) {
            break;
        }

        states[selected_index].remaining_time -= run_until - current_time;
        current_time = run_until;
        if (states[selected_index].remaining_time == 0) {
            ++completed;
        }
    }

    free(states);
    result.slice_count = slice_count;
    return result;
}

ScheduleResult scheduler_run_round_robin(const ProcessSpec *processes, size_t process_count, int quantum) {
    ScheduleResult result = {0};
    if (process_count == 0 || quantum <= 0) {
        return result;
    }

    ProcessState *states = malloc(process_count * sizeof(*states));
    ReadyQueue queue = {0};
    bool *queued = NULL;
    if (states == NULL) {
        return result;
    }

    ready_queue_init(&queue, process_count + 1);
    queued = calloc(process_count, sizeof(*queued));
    if (queue.items == NULL || queued == NULL) {
        free(states);
        ready_queue_destroy(&queue);
        free(queued);
        return result;
    }

    for (size_t index = 0; index < process_count; ++index) {
        states[index].spec = processes[index];
        states[index].remaining_time = processes[index].burst_time;
    }

    size_t slice_capacity = 0;
    size_t slice_count = 0;
    size_t completed = 0;
    int current_time = 0;

    for (size_t index = 0; index < process_count; ++index) {
        if (states[index].spec.arrival_time <= current_time) {
            ready_queue_push(&queue, (int)index);
            queued[index] = true;
        }
    }

    while (completed < process_count) {
        if (ready_queue_is_empty(&queue)) {
            int next_arrival = -1;
            for (size_t index = 0; index < process_count; ++index) {
                if (states[index].remaining_time <= 0 || queued[index]) {
                    continue;
                }
                if (next_arrival < 0 || states[index].spec.arrival_time < next_arrival) {
                    next_arrival = states[index].spec.arrival_time;
                }
            }
            if (next_arrival < 0) {
                break;
            }
            current_time = next_arrival;
            for (size_t index = 0; index < process_count; ++index) {
                if (states[index].remaining_time > 0 && !queued[index] && states[index].spec.arrival_time <= current_time) {
                    ready_queue_push(&queue, (int)index);
                    queued[index] = true;
                }
            }
        }

        int index = -1;
        if (!ready_queue_pop(&queue, &index)) {
            continue;
        }
        queued[index] = false;
        if (states[index].remaining_time <= 0) {
            continue;
        }

        if (current_time < states[index].spec.arrival_time) {
            current_time = states[index].spec.arrival_time;
        }

        int run_time = states[index].remaining_time < quantum ? states[index].remaining_time : quantum;
        if (!append_schedule_slice(&result, &slice_capacity, &slice_count, states[index].spec.id, current_time, current_time + run_time)) {
            break;
        }

        current_time += run_time;

        states[index].remaining_time -= run_time;
        if (states[index].remaining_time == 0) {
            ++completed;
        }

        if (states[index].remaining_time > 0) {
            queued[index] = true;
        }

        for (size_t arrival_index = 0; arrival_index < process_count; ++arrival_index) {
            if (states[arrival_index].remaining_time > 0 && !queued[arrival_index] && states[arrival_index].spec.arrival_time <= current_time) {
                ready_queue_push(&queue, (int)arrival_index);
                queued[arrival_index] = true;
            }
        }

        if (states[index].remaining_time > 0) {
            ready_queue_push(&queue, index);
            queued[index] = true;
        }
    }

    free(states);
    free(queued);
    ready_queue_destroy(&queue);
    result.slice_count = slice_count;
    return result;
}

void scheduler_result_destroy(ScheduleResult *result) {
    if (result == NULL) {
        return;
    }
    free(result->slices);
    result->slices = NULL;
    result->slice_count = 0;
}

void scheduler_print_result(const char *title, const ScheduleResult *result) {
    printf("%s\n", title);
    if (result == NULL || result->slice_count == 0) {
        printf("No scheduled processes\n");
        return;
    }

    for (size_t index = 0; index < result->slice_count; ++index) {
        printf("P%d [%d -> %d]\n", result->slices[index].process_id, result->slices[index].start_time, result->slices[index].end_time);
    }
}

void scheduler_demo(void) {
    const ProcessSpec processes[] = {
        {CUSTOMER_REGULAR, 1, 0, 6, 1},
        {CUSTOMER_PREMIUM, 2, 1, 4, 3},
        {CUSTOMER_LOAN_APPLICANT, 3, 2, 3, 2},
        {CUSTOMER_CORPORATE, 4, 3, 2, 4},
        {CUSTOMER_VIP, 5, 4, 2, 5}
    };

    ScheduleResult fcfs = scheduler_run_fcfs(processes, sizeof(processes) / sizeof(processes[0]));
    ScheduleResult priority = scheduler_run_priority(processes, sizeof(processes) / sizeof(processes[0]));
    ScheduleResult round_robin = scheduler_run_round_robin(processes, sizeof(processes) / sizeof(processes[0]), 2);

    printf("\n[SCHEDULER MODULE]\n\n");
    scheduler_print_report("\n\nFCFS", processes, sizeof(processes) / sizeof(processes[0]), &fcfs);
    scheduler_print_report("\n\nPriority (Preemptive)", processes, sizeof(processes) / sizeof(processes[0]), &priority);
    scheduler_print_report("\n\nRound Robin", processes, sizeof(processes) / sizeof(processes[0]), &round_robin);

    scheduler_result_destroy(&fcfs);
    scheduler_result_destroy(&priority);
    scheduler_result_destroy(&round_robin);
}