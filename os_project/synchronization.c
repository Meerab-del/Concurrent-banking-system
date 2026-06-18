#include <semaphore.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

typedef struct {
    pthread_mutex_t mutex;
    int balance;
} Account;

typedef struct {
    int balance;
    pthread_mutex_t start_mutex;
    pthread_cond_t start_cond;
    int ready_count;
    bool release;
} RaceAccount;

typedef struct {
    size_t process_count;
    size_t resource_count;
    int available[3];
    int maximum[5][3];
    int allocation[5][3];
    int need[5][3];
} BankerState;

typedef struct {
    Account *account;
    int amount;
    int repeat;
    bool deposit;
} TransactionContext;

typedef struct {
    RaceAccount *account;
    int amount;
} RaceTransactionContext;

typedef struct {
    sem_t *semaphore;
    const char *customer_type;
    const char *action;
    int hold_milliseconds;
} SemaphoreContext;

static pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;

static void log_line(const char *format, ...) {
    va_list args;
    va_start(args, format);
    pthread_mutex_lock(&output_mutex);
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    pthread_mutex_unlock(&output_mutex);
    va_end(args);
}

static long long current_time_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (long long)now.tv_sec * 1000LL + (long long)now.tv_nsec / 1000000LL;
}

static void sleep_for_milliseconds(int milliseconds) {
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&delay, NULL);
}

void account_init(Account *account, int initial_balance) {
    if (account == NULL) {
        return;
    }
    pthread_mutex_init(&account->mutex, NULL);
    account->balance = initial_balance;
}

void account_destroy(Account *account) {
    if (account == NULL) {
        return;
    }
    pthread_mutex_destroy(&account->mutex);
}

bool account_deposit(Account *account, int amount) {
    if (account == NULL || amount < 0) {
        return false;
    }
    pthread_mutex_lock(&account->mutex);
    account->balance += amount;
    pthread_mutex_unlock(&account->mutex);
    return true;
}

bool account_withdraw(Account *account, int amount) {
    if (account == NULL || amount < 0) {
        return false;
    }
    pthread_mutex_lock(&account->mutex);
    bool success = false;
    if (account->balance >= amount) {
        account->balance -= amount;
        success = true;
    }
    pthread_mutex_unlock(&account->mutex);
    return success;
}

bool bankers_is_safe(const BankerState *state, int *safe_sequence, size_t *safe_count) {
    if (state == NULL || safe_sequence == NULL || safe_count == NULL) {
        return false;
    }

    int work[3];
    bool finish[5] = {false, false, false, false, false};
    for (size_t resource = 0; resource < state->resource_count; ++resource) {
        work[resource] = state->available[resource];
    }

    size_t sequence_index = 0;
    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t process = 0; process < state->process_count; ++process) {
            if (finish[process]) {
                continue;
            }

            bool can_finish = true;
            for (size_t resource = 0; resource < state->resource_count; ++resource) {
                if (state->need[process][resource] > work[resource]) {
                    can_finish = false;
                    break;
                }
            }

            if (!can_finish) {
                continue;
            }

            for (size_t resource = 0; resource < state->resource_count; ++resource) {
                work[resource] += state->allocation[process][resource];
            }
            finish[process] = true;
            safe_sequence[sequence_index++] = (int)process;
            progress = true;
        }
    }

    for (size_t process = 0; process < state->process_count; ++process) {
        if (!finish[process]) {
            *safe_count = sequence_index;
            return false;
        }
    }

    *safe_count = sequence_index;
    return true;
}

bool bankers_request_resources(BankerState *state, size_t process_index, const int request[3], int *safe_sequence, size_t *safe_count) {
    if (state == NULL || request == NULL || process_index >= state->process_count) {
        return false;
    }

    for (size_t resource = 0; resource < state->resource_count; ++resource) {
        if (request[resource] > state->need[process_index][resource] || request[resource] > state->available[resource]) {
            return false;
        }
    }

    for (size_t resource = 0; resource < state->resource_count; ++resource) {
        state->available[resource] -= request[resource];
        state->allocation[process_index][resource] += request[resource];
        state->need[process_index][resource] -= request[resource];
    }

    bool safe = bankers_is_safe(state, safe_sequence, safe_count);
    if (!safe) {
        for (size_t resource = 0; resource < state->resource_count; ++resource) {
            state->available[resource] += request[resource];
            state->allocation[process_index][resource] -= request[resource];
            state->need[process_index][resource] += request[resource];
        }
    }

    return safe;
}

static void *transaction_worker(void *arg) {
    TransactionContext *context = (TransactionContext *)arg;
    for (int index = 0; index < context->repeat; ++index) {
        if (context->deposit) {
            account_deposit(context->account, context->amount);
        } else {
            account_withdraw(context->account, context->amount);
        }
    }
    return NULL;
}

static void *race_worker(void *arg) {
    RaceTransactionContext *context = (RaceTransactionContext *)arg;
    /* Both threads snapshot the same initial balance — this is the race */
    int snapshot = context->account->balance;

    /* Synchronize: wait until both threads have their snapshot */
    pthread_mutex_lock(&context->account->start_mutex);
    ++context->account->ready_count;
    if (context->account->ready_count == 2) {
        context->account->release = true;
        pthread_cond_broadcast(&context->account->start_cond);
    } else {
        while (!context->account->release) {
            pthread_cond_wait(&context->account->start_cond, &context->account->start_mutex);
        }
    }
    pthread_mutex_unlock(&context->account->start_mutex);

    /* Thread with smaller amount sleeps less so it writes first and gets overwritten */
    sleep_for_milliseconds(context->amount < 200 ? 50 : 100);
    context->account->balance = snapshot + context->amount;
    return NULL;
}

static void *semaphore_worker(void *arg) {
    SemaphoreContext *context = (SemaphoreContext *)arg;
    long long request_time = current_time_ms();
    log_line("[SEMAPHORE] Customer [%s] requesting %s resource at %lld ms", context->customer_type, context->action, request_time);

    sem_wait(context->semaphore);

    long long acquired_time = current_time_ms();
    log_line("[SEMAPHORE] Customer [%s] entered critical section at %lld ms", context->customer_type, acquired_time);
    sleep_for_milliseconds(context->hold_milliseconds);
    long long release_time = current_time_ms();
    log_line("[SEMAPHORE] Customer [%s] releasing resource at %lld ms", context->customer_type, release_time);

    sem_post(context->semaphore);
    return NULL;
}

static bool find_unsafe_request(const BankerState *state, size_t *process_index, int request[3]) {
    for (size_t process = 0; process < state->process_count; ++process) {
        for (int r0 = 0; r0 <= state->available[0] && r0 <= state->need[process][0]; ++r0) {
            for (int r1 = 0; r1 <= state->available[1] && r1 <= state->need[process][1]; ++r1) {
                for (int r2 = 0; r2 <= state->available[2] && r2 <= state->need[process][2]; ++r2) {
                    if (r0 == 0 && r1 == 0 && r2 == 0) {
                        continue;
                    }
                    BankerState candidate = *state;
                    int sequence[5] = {0};
                    size_t count = 0;
                    int trial[3] = {r0, r1, r2};
                    bool granted = bankers_request_resources(&candidate, process, trial, sequence, &count);
                    if (!granted) {
                        *process_index = process;
                        request[0] = r0;
                        request[1] = r1;
                        request[2] = r2;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static void print_safe_sequence(const int *safe_sequence, size_t safe_count) {
    if (safe_sequence == NULL || safe_count == 0) {
        return;
    }

    printf("Safe sequence:");
    for (size_t index = 0; index < safe_count; ++index) {
        printf(" P%d", safe_sequence[index]);
    }
    printf("\n");
}

static void print_balance_summary(int no_mutex_balance, int with_mutex_balance, int expected_balance) {
    printf("%-17s | %13s | %8s | %-8s\n", "Mode", "Final Balance", "Expected", "Correct?");
    printf("-------------------------------------------------------\n");
    printf("%-17s | %13d | %8d | %-8s\n", "No Mutex", no_mutex_balance, expected_balance, no_mutex_balance == expected_balance ? "YES" : "NO");
    printf("%-17s | %13d | %8d | %-8s\n", "With Mutex", with_mutex_balance, expected_balance, with_mutex_balance == expected_balance ? "YES" : "NO");
}

void synchronization_demo(void) {
    log_line("[RACE CONDITION DEMO] Running without mutex");
    RaceAccount race_account = {0};
    race_account.balance = 1000;
    pthread_mutex_init(&race_account.start_mutex, NULL);
    pthread_cond_init(&race_account.start_cond, NULL);
    race_account.ready_count = 0;
    race_account.release = false;

    /* Use amounts 100 and 200: both threads read 1000, then write 1100 or 1200.
       The second write (200) wins the race, giving final balance 1200. */
    RaceTransactionContext race_transactions[2] = {
        {&race_account, 100},   /* writes 1100 — gets overwritten */
        {&race_account, 200}    /* writes 1200 — wins race */
    };
    pthread_t race_threads[2];

    /* Start thread 0 (amount=100) first so it writes first and gets overwritten */
    pthread_create(&race_threads[0], NULL, race_worker, &race_transactions[0]);
    pthread_create(&race_threads[1], NULL, race_worker, &race_transactions[1]);
    for (size_t index = 0; index < 2; ++index) {
        pthread_join(race_threads[index], NULL);
    }

    pthread_mutex_destroy(&race_account.start_mutex);
    pthread_cond_destroy(&race_account.start_cond);
    int no_mutex_final_balance = race_account.balance;

    log_line("[MUTEX DEMO] Re-running with mutex protection");
    Account account;
    account_init(&account, 1000);

    TransactionContext transactions[2] = {
        {&account, 100, 1, true},
        {&account, 200, 1, true}
    };
    pthread_t workers[2];

    for (size_t index = 0; index < 2; ++index) {
        pthread_create(&workers[index], NULL, transaction_worker, &transactions[index]);
    }
    for (size_t index = 0; index < 2; ++index) {
        pthread_join(workers[index], NULL);
    }

    log_line("[MUTEX DEMO] Final balance with mutex: %d", account.balance);
    int with_mutex_final_balance = account.balance;

    print_balance_summary(no_mutex_final_balance, with_mutex_final_balance, 1300);

    BankerState state = {
        .process_count = 5,
        .resource_count = 3,
        .available = {3, 3, 2},
        .maximum = {
            {7, 5, 3},
            {3, 2, 2},
            {9, 0, 2},
            {2, 2, 2},
            {4, 3, 3}
        },
        .allocation = {
            {0, 1, 0},
            {2, 0, 0},
            {3, 0, 2},
            {2, 1, 1},
            {0, 0, 2}
        }
    };

    for (size_t process = 0; process < state.process_count; ++process) {
        for (size_t resource = 0; resource < state.resource_count; ++resource) {
            state.need[process][resource] = state.maximum[process][resource] - state.allocation[process][resource];
        }
    }

    int safe_sequence[5] = {0};
    size_t safe_count = 0;
    bool initial_safe = bankers_is_safe(&state, safe_sequence, &safe_count);
    log_line("[BANKER] Initial state: %s", initial_safe ? "safe" : "unsafe");
    if (initial_safe) {
        print_safe_sequence(safe_sequence, safe_count);
    }

    BankerState unsafe_state = state;
    size_t denied_process = 0;
    int denied_request[3] = {0, 0, 0};
    bool found = find_unsafe_request(&unsafe_state, &denied_process, denied_request);
    if (found) {
        int trial_sequence[5] = {0};
        size_t trial_count = 0;
        bool granted = bankers_request_resources(&unsafe_state, denied_process, denied_request, trial_sequence, &trial_count);
        if (!granted) {
            log_line("[BANKER] Request DENIED — Unsafe State");
            log_line("[BANKER] P%zu requesting [%d %d %d] would leave no safe sequence, so the system cannot guarantee completion.", denied_process, denied_request[0], denied_request[1], denied_request[2]);
        }
    } else {
        log_line("[BANKER] Unsafe request test: no unsafe request found in current state");
    }

    sem_t semaphore;
    sem_init(&semaphore, 0, 2);
    SemaphoreContext semaphore_contexts[3] = {
        {&semaphore, "REGULAR", "deposit", 1200},
        {&semaphore, "PREMIUM", "deposit", 1200},
        {&semaphore, "CORPORATE", "deposit", 1200}
    };
    pthread_t semaphore_threads[3];

    log_line("[SEMAPHORE DEMO] Semaphore value = 2, three threads attempt entry");
    for (size_t index = 0; index < 3; ++index) {
        pthread_create(&semaphore_threads[index], NULL, semaphore_worker, &semaphore_contexts[index]);
        /* Stagger launches so threads request and release in strict order 1->2->3 */
        sleep_for_milliseconds(20);
    }
    for (size_t index = 0; index < 3; ++index) {
        pthread_join(semaphore_threads[index], NULL);
    }
    sem_destroy(&semaphore);

    account_destroy(&account);
}