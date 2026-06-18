#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    CUSTOMER_REGULAR = 0,
    CUSTOMER_PREMIUM,
    CUSTOMER_LOAN_APPLICANT,
    CUSTOMER_CORPORATE,
    CUSTOMER_VIP,
    CUSTOMER_TYPE_COUNT
} CustomerType;

const char *customer_type_name(CustomerType type);

void ipc_demo(void);

typedef struct {
    int customer_id;
    CustomerType customer_type;
    char action[16];
    int amount;
} BankRequest;

typedef struct {
    int customer_id;
    CustomerType customer_type;
    char action[16];
    int amount;
} PipeRequest;

typedef struct {
    char status[32];
    int balance;
} PipeReply;

typedef struct {
    BankRequest *items;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t size;
    bool closed;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} RequestQueue;

typedef struct {
    RequestQueue *queue;
    const BankRequest *requests;
    size_t request_count;
} ProducerContext;

typedef struct {
    RequestQueue *queue;
    int *processed_balance;
} ConsumerContext;

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

const char *customer_type_name(CustomerType type) {
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

static void log_line(const char *format, ...) {
    va_list args;
    pthread_mutex_lock(&log_mutex);
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
    fputc('\n', stdout);
    fflush(stdout);
    pthread_mutex_unlock(&log_mutex);
}

static void queue_init(RequestQueue *queue, size_t capacity) {
    queue->items = calloc(capacity, sizeof(*queue->items));
    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->size = 0;
    queue->closed = false;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    pthread_cond_init(&queue->not_full, NULL);
}

static void queue_destroy(RequestQueue *queue) {
    if (queue == NULL) {
        return;
    }
    free(queue->items);
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
}

static bool queue_push(RequestQueue *queue, const BankRequest *request) {
    pthread_mutex_lock(&queue->mutex);
    while (!queue->closed && queue->size == queue->capacity) {
        pthread_cond_wait(&queue->not_full, &queue->mutex);
    }
    if (queue->closed) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }

    queue->items[queue->tail] = *request;
    queue->tail = (queue->tail + 1) % queue->capacity;
    ++queue->size;

    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
    return true;
}

static bool queue_pop(RequestQueue *queue, BankRequest *request) {
    pthread_mutex_lock(&queue->mutex);
    while (!queue->closed && queue->size == 0) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    if (queue->size == 0 && queue->closed) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }

    *request = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    --queue->size;

    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
    return true;
}

static void queue_close(RequestQueue *queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->closed = true;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
}

static void *producer_main(void *arg) {
    ProducerContext *context = (ProducerContext *)arg;
    for (size_t index = 0; index < context->request_count; ++index) {
        BankRequest request = context->requests[index];
        if (!queue_push(context->queue, &request)) {
            break;
        }
        log_line("[MSGQUEUE-PRODUCER] [%s] Customer %d initiated request: action=%s amount=%d", customer_type_name(request.customer_type), request.customer_id, request.action, request.amount);
    }
    queue_close(context->queue);
    return NULL;
}

static void *consumer_main(void *arg) {
    ConsumerContext *context = (ConsumerContext *)arg;
    BankRequest request;
    while (queue_pop(context->queue, &request)) {
        if (request.action[0] == 'd') {
            *context->processed_balance += request.amount;
        } else if (request.action[0] == 'w') {
            *context->processed_balance -= request.amount;
        }
        log_line("[MSGQUEUE-CONSUMER] [%s] Customer %d completed request: action=%s amount=%d balance=%d", customer_type_name(request.customer_type), request.customer_id, request.action, request.amount, *context->processed_balance);
    }
    return NULL;
}

#define CORPORATE_EMPLOYEE_COUNT 50

typedef struct {
    RequestQueue    *queue;
    int              corporate_id;
    int              employee_index;
    int              salary;
} PayrollThreadContext;

static void *payroll_worker(void *arg) {
    PayrollThreadContext *ctx = (PayrollThreadContext *)arg;
    BankRequest req;
    req.customer_id   = ctx->corporate_id;
    req.customer_type = CUSTOMER_CORPORATE;
    snprintf(req.action, sizeof(req.action), "deposit");
    req.amount        = ctx->salary;

    log_line("[CORPORATE-PAYROLL] [CORPORATE] Customer %d — employee %d payroll deposit: amount=%d",
             ctx->corporate_id, ctx->employee_index, ctx->salary);
    queue_push(ctx->queue, &req);
    return NULL;
}

static void run_corporate_payroll_demo(RequestQueue *queue) {
    printf("[CORPORATE PAYROLL DEMO] Customer 301 spawning %d payroll threads\n",
           CORPORATE_EMPLOYEE_COUNT);
    fflush(stdout);

    PayrollThreadContext contexts[CORPORATE_EMPLOYEE_COUNT];
    pthread_t            threads[CORPORATE_EMPLOYEE_COUNT];

    for (int i = 0; i < CORPORATE_EMPLOYEE_COUNT; ++i) {
        contexts[i].queue          = queue;
        contexts[i].corporate_id   = 301;
        contexts[i].employee_index = i + 1;
        contexts[i].salary         = 2800 + (i * 75);
        pthread_create(&threads[i], NULL, payroll_worker, &contexts[i]);
        /* stagger so producer log appears before consumer log for each employee */
        struct timespec delay = {0, 50 * 1000000L};
        nanosleep(&delay, NULL);
    }

    for (int i = 0; i < CORPORATE_EMPLOYEE_COUNT; ++i) {
        pthread_join(threads[i], NULL);
    }

    printf("[CORPORATE PAYROLL DEMO] All %d payroll threads completed\n",
           CORPORATE_EMPLOYEE_COUNT);
    fflush(stdout);
}

static void log_pipe_line(const char *label, const char *message) {
    printf("%s %s\n", label, message);
    fflush(stdout);
}

static void run_pipe_demo(void) {
    int pipe_fds[2];
    int reply_fds[2];
    if (pipe(pipe_fds) != 0) {
        perror("pipe");
        return;
    }
    if (pipe(reply_fds) != 0) {
        perror("pipe");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        close(reply_fds[0]);
        close(reply_fds[1]);
        return;
    }

    if (pid == 0) {
        close(pipe_fds[1]);
        close(reply_fds[0]);
        PipeRequest request;
        ssize_t bytes_read = read(pipe_fds[0], &request, sizeof(request));
        if (bytes_read == (ssize_t)sizeof(request)) {
            char message[256];
            int processed_balance = 500;
            if (request.action[0] == 'd') {
                processed_balance += request.amount;
            } else if (request.action[0] == 'w') {
                processed_balance -= request.amount;
            }
            snprintf(message, sizeof(message), "[%s] Received request: customer=%d action=%s amount=%d -> balance=%d", customer_type_name(request.customer_type), request.customer_id, request.action, request.amount, processed_balance);
            log_pipe_line("[PIPE-CONSUMER]", message);

            PipeReply reply;
            snprintf(reply.status, sizeof(reply.status), "Transaction Successful");
            reply.balance = processed_balance;
            write(reply_fds[1], &reply, sizeof(reply));
        }
        close(pipe_fds[0]);
        close(reply_fds[1]);
        _exit(0);
    }

    close(pipe_fds[0]);
    close(reply_fds[1]);
    PipeRequest request = {201, CUSTOMER_VIP, "deposit", 125};
    char message[256];
    snprintf(message, sizeof(message), "[%s] Customer %d writing request: action=%s amount=%d", customer_type_name(request.customer_type), request.customer_id, request.action, request.amount);
    log_pipe_line("[PIPE-PRODUCER]", message);
    write(pipe_fds[1], &request, sizeof(request));
    close(pipe_fds[1]);

    PipeReply reply;
    ssize_t reply_bytes = read(reply_fds[0], &reply, sizeof(reply));
    if (reply_bytes == (ssize_t)sizeof(reply)) {
        snprintf(message, sizeof(message), "[%s] Customer %d received reply: %s, balance=%d", customer_type_name(request.customer_type), request.customer_id, reply.status, reply.balance);
        log_pipe_line("[PIPE-PRODUCER]", message);
    }
    close(reply_fds[0]);
    waitpid(pid, NULL, 0);
}

void ipc_demo(void) {
    RequestQueue queue;
    queue_init(&queue, 2);

    const struct {
        int customer_id;
        const char *action;
        int amount;
    } request_specs[] = {
        {101, "deposit", 300},
        {102, "withdraw", 200},
        {103, "deposit", 150},
        {104, "withdraw", 50},
        {105, "deposit", 500}
    };

    BankRequest requests[sizeof(request_specs) / sizeof(request_specs[0])];

    const CustomerType type_cycle[] = {
        CUSTOMER_REGULAR,
        CUSTOMER_PREMIUM,
        CUSTOMER_LOAN_APPLICANT,
        CUSTOMER_CORPORATE,
        CUSTOMER_VIP
    };

    for (size_t index = 0; index < sizeof(requests) / sizeof(requests[0]); ++index) {
        requests[index].customer_id = request_specs[index].customer_id;
        requests[index].customer_type = type_cycle[index % CUSTOMER_TYPE_COUNT];
        strcpy(requests[index].action, request_specs[index].action);
        requests[index].amount = request_specs[index].amount;
    }

    int processed_balance = 5000;
    ProducerContext producer_context = {&queue, requests, sizeof(requests) / sizeof(requests[0])};
    ConsumerContext consumer_context = {&queue, &processed_balance};

    pthread_t producer_thread;
    pthread_t consumer_thread;
    pthread_create(&producer_thread, NULL, producer_main, &producer_context);
    pthread_create(&consumer_thread, NULL, consumer_main, &consumer_context);

    pthread_join(producer_thread, NULL);
    pthread_join(consumer_thread, NULL);

    queue_destroy(&queue);

    printf("\n\n[CORPORATE PAYROLL DEMO]\n");
    fflush(stdout);

    RequestQueue payroll_queue;
    queue_init(&payroll_queue, CORPORATE_EMPLOYEE_COUNT);

    int payroll_balance = 0;
    ConsumerContext payroll_consumer_ctx = {&payroll_queue, &payroll_balance};
    pthread_t payroll_consumer_thread;
    pthread_create(&payroll_consumer_thread, NULL, consumer_main, &payroll_consumer_ctx);

    run_corporate_payroll_demo(&payroll_queue);

    queue_close(&payroll_queue);
    pthread_join(payroll_consumer_thread, NULL);

    printf("[CORPORATE PAYROLL DEMO] Total payroll disbursed: %d\n", payroll_balance);
    fflush(stdout);

    queue_destroy(&payroll_queue);

    printf("\n[PIPE IPC DEMO]\n");
    fflush(stdout);
    run_pipe_demo();
}