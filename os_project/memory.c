#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    size_t page_faults;
    size_t page_hits;
} MemoryStats;

typedef struct {
    int page;
    int last_used;
} Frame;

static void print_frames(const int *frames, size_t frame_count) {
    putchar('[');
    for (size_t index = 0; index < frame_count; ++index) {
        if (index > 0) {
            printf(", ");
        }
        if (frames[index] < 0) {
            printf("-");
        } else {
            printf("%d", frames[index]);
        }
    }
    putchar(']');
}

static void print_trace_line(int page, const int *frames, size_t frame_count, bool hit) {
    printf("Ref: %d | Frames: ", page);
    print_frames(frames, frame_count);
    printf(" | %s\n", hit ? "HIT" : "FAULT");
}

static void print_comparison_table_row(const char *algorithm, MemoryStats stats, size_t page_count) {
    double hit_ratio = page_count == 0 ? 0.0 : (double)stats.page_hits / (double)page_count;
    printf("%-9s | %11zu | %9zu | %9.2f\n", algorithm, stats.page_faults, stats.page_hits, hit_ratio);
}

static MemoryStats memory_trace_fifo(const int *pages, size_t page_count, size_t frame_count) {
    MemoryStats stats = {0, 0};
    if (pages == NULL || page_count == 0 || frame_count == 0) {
        return stats;
    }

    int *frames = malloc(frame_count * sizeof(*frames));
    if (frames == NULL) {
        return stats;
    }

    for (size_t index = 0; index < frame_count; ++index) {
        frames[index] = -1;
    }

    size_t next_replace = 0;
    for (size_t index = 0; index < page_count; ++index) {
        bool hit = false;
        for (size_t frame = 0; frame < frame_count; ++frame) {
            if (frames[frame] == pages[index]) {
                hit = true;
                break;
            }
        }

        if (hit) {
            ++stats.page_hits;
        } else {
            ++stats.page_faults;
            frames[next_replace] = pages[index];
            next_replace = (next_replace + 1) % frame_count;
        }

        print_trace_line(pages[index], frames, frame_count, hit);
    }

    free(frames);
    return stats;
}

static MemoryStats memory_trace_lru(const int *pages, size_t page_count, size_t frame_count) {
    MemoryStats stats = {0, 0};
    if (pages == NULL || page_count == 0 || frame_count == 0) {
        return stats;
    }

    Frame *frames = malloc(frame_count * sizeof(*frames));
    if (frames == NULL) {
        return stats;
    }

    for (size_t index = 0; index < frame_count; ++index) {
        frames[index].page = -1;
        frames[index].last_used = -1;
    }

    for (size_t step = 0; step < page_count; ++step) {
        int page = pages[step];
        bool hit = false;
        for (size_t frame = 0; frame < frame_count; ++frame) {
            if (frames[frame].page == page) {
                hit = true;
                frames[frame].last_used = (int)step;
                break;
            }
        }

        if (hit) {
            ++stats.page_hits;
        } else {
            ++stats.page_faults;
            size_t victim = 0;
            int oldest_use = frames[0].last_used;
            for (size_t frame = 1; frame < frame_count; ++frame) {
                if (frames[frame].last_used < oldest_use) {
                    oldest_use = frames[frame].last_used;
                    victim = frame;
                }
            }
            frames[victim].page = page;
            frames[victim].last_used = (int)step;
        }

        int frame_snapshot[frame_count];
        for (size_t frame = 0; frame < frame_count; ++frame) {
            frame_snapshot[frame] = frames[frame].page;
        }
        print_trace_line(page, frame_snapshot, frame_count, hit);
    }

    free(frames);
    return stats;
}

static void print_memory_comparison_table(MemoryStats fifo, MemoryStats lru, size_t page_count) {
    printf("%-9s | %11s | %9s | %9s\n", "Algorithm", "Page Faults", "Page Hits", "Hit Ratio");
    printf("-----------------------------------------------\n");
    print_comparison_table_row("FIFO", fifo, page_count);
    print_comparison_table_row("LRU", lru, page_count);
}

void memory_demo(void) {
    const int pages[] = {7, 0, 1, 2, 0, 3, 0, 4, 2, 3, 0, 3, 2};
    size_t page_count = sizeof(pages) / sizeof(pages[0]);

    printf("[MEMORY] Reference string derived from transaction history of customers 101-105 (customer_id mod 8 used as page number)\n");
    printf("FIFO Trace\n");
    MemoryStats fifo = memory_trace_fifo(pages, page_count, 3);
    printf("LRU Trace\n");
    MemoryStats lru = memory_trace_lru(pages, page_count, 3);

    print_memory_comparison_table(fifo, lru, page_count);
}