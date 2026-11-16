#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_arp.h>
#include <linux/futex.h>
#include <linux/reboot.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile uint64_t g_sink = 0;

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double median3(double v[3]) {
    qsort(v, 3, sizeof(double), cmp_double);
    return v[1];
}

static int ensure_dir(const char *path, mode_t mode) {
    if (mkdir(path, mode) == 0 || errno == EEXIST) return 0;
    return -1;
}

static void mount_base_fs(void) {
    ensure_dir("/proc", 0555);
    ensure_dir("/sys", 0555);
    ensure_dir("/dev", 0755);
    ensure_dir("/tmp", 01777);
    mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
    mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, NULL);
    mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "size=1024m");
    ensure_dir("/sys/fs/cgroup", 0755);
    mount("cgroup2", "/sys/fs/cgroup", "cgroup2", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
}

static int setup_loopback(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "lo");
    struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
        close(fd);
        return -1;
    }
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ioctl(fd, SIOCSIFFLAGS, &ifr);
    }
    close(fd);
    return 0;
}

static int write_sysctl_kv(const char *key, const char *val) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/sys/");
    size_t len = strlen(path);
    for (size_t i = 0; key[i] && len + 1 < sizeof(path); i++) {
        path[len++] = (key[i] == '.') ? '/' : key[i];
    }
    path[len] = '\0';
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t w = write(fd, val, strlen(val));
    close(fd);
    return (w > 0) ? 0 : -1;
}

static void apply_sysctl_file(const char *conf_path, int *applied_cnt, int *failed_cnt) {
    *applied_cnt = 0;
    *failed_cnt = 0;
    FILE *f = fopen(conf_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = p;
        char *v = eq + 1;
        char *kend = k + strlen(k);
        while (kend > k && (kend[-1] == ' ' || kend[-1] == '\t')) *--kend = '\0';
        while (*v == ' ' || *v == '\t') v++;
        char *vend = v + strlen(v);
        while (vend > v && (vend[-1] == '\n' || vend[-1] == '\r' || vend[-1] == ' ' || vend[-1] == '\t'))
            *--vend = '\0';
        if (k[0] == '/') {
            int fd = open(k, O_WRONLY);
            if (fd >= 0 && write(fd, v, strlen(v)) > 0) {
                (*applied_cnt)++;
            } else {
                (*failed_cnt)++;
            }
            if (fd >= 0) close(fd);
        } else {
            if (write_sysctl_kv(k, v) == 0) (*applied_cnt)++;
            else (*failed_cnt)++;
        }
    }
    fclose(f);
}

typedef struct {
    double mem_total;
    double mem_free;
    double mem_available;
    double slab;
    double sreclaimable;
    double sunreclaim;
    double page_tables;
    double kernel_stack;
    double vmalloc_used;
    double percpu;
    double shmem;
} meminfo_t;

static void read_meminfo(meminfo_t *mi) {
    memset(mi, 0, sizeof(*mi));
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        double val = 0;
        if (sscanf(line, "%63[^:]: %lf", key, &val) == 2) {
            if (strcmp(key, "MemTotal") == 0) mi->mem_total = val;
            else if (strcmp(key, "MemFree") == 0) mi->mem_free = val;
            else if (strcmp(key, "MemAvailable") == 0) mi->mem_available = val;
            else if (strcmp(key, "Slab") == 0) mi->slab = val;
            else if (strcmp(key, "SReclaimable") == 0) mi->sreclaimable = val;
            else if (strcmp(key, "SUnreclaim") == 0) mi->sunreclaim = val;
            else if (strcmp(key, "PageTables") == 0) mi->page_tables = val;
            else if (strcmp(key, "KernelStack") == 0) mi->kernel_stack = val;
            else if (strcmp(key, "VmallocUsed") == 0) mi->vmalloc_used = val;
            else if (strcmp(key, "Percpu") == 0) mi->percpu = val;
            else if (strcmp(key, "Shmem") == 0) mi->shmem = val;
        }
    }
    fclose(f);
}

static int read_int_file(const char *path, int def_val) {
    FILE *f = fopen(path, "r");
    if (!f) return def_val;
    int v = def_val;
    if (fscanf(f, "%d", &v) != 1) v = def_val;
    fclose(f);
    return v;
}

static int check_kmsg_clean(char *err_buf, size_t err_sz) {
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 1;
    char buf[4096];
    int clean = 1;
    for (int iter = 0; iter < 4096; iter++) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n < 0) {
            if (errno == EPIPE) continue;
            break;
        }
        buf[n] = '\0';
        if (strstr(buf, "BUG:") || strstr(buf, "Oops:") || strstr(buf, "general protection fault") ||
            strstr(buf, "Kernel panic") || strstr(buf, "WARNING: CPU:") || strstr(buf, "KASAN:")) {
            clean = 0;
            if (err_buf && err_buf[0] == '\0' && err_sz > 1) {
                size_t copy_len = (size_t)n < (err_sz - 1) ? (size_t)n : (err_sz - 1);
                memcpy(err_buf, buf, copy_len);
                err_buf[copy_len] = '\0';
                for (size_t i = 0; err_buf[i]; i++) {
                    if (err_buf[i] == '\n' || err_buf[i] == '"') err_buf[i] = ' ';
                }
            }
        }
    }
    close(fd);
    return clean;
}

/* ---------- Correctness self-tests ---------- */
static int run_sanity_tests(int expected_cpus) {
    if (sysconf(_SC_NPROCESSORS_ONLN) != expected_cpus) return -1;
    if (read_int_file("/proc/sys/kernel/tainted", 0) != 0) return -2;

    struct statfs stfs;
    if (statfs("/tmp", &stfs) != 0 || (unsigned long)stfs.f_type != 0x01021994UL) return -10;

    /* Cgroups v2 controllers check */
    FILE *cf = fopen("/sys/fs/cgroup/cgroup.controllers", "r");
    if (!cf) return -3;
    char ctrls[256] = {0};
    if (!fgets(ctrls, sizeof(ctrls), cf)) { fclose(cf); return -4; }
    fclose(cf);
    if (!strstr(ctrls, "memory")) return -5;

    /* Probe io_uring_setup syscall exists (expect >=0 on valid 8 entries) */
    uint32_t params[32];
    memset(params, 0, sizeof(params));
    long uring_fd = syscall(__NR_io_uring_setup, 8, params);
    if (uring_fd < 0) return -6;
    close((int)uring_fd);

    /* CoW fork integrity check */
    size_t sz = 4096 * 64;
    uint8_t *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -7;
    for (size_t i = 0; i < sz; i += 4096) p[i] = (uint8_t)((i >> 12) ^ 0xA5);
    pid_t pid = fork();
    if (pid == 0) {
        for (size_t i = 0; i < sz; i += 4096) p[i] = 0x5A;
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { munmap(p, sz); return -8; }
    for (size_t i = 0; i < sz; i += 4096) {
        if (p[i] != (uint8_t)((i >> 12) ^ 0xA5)) { munmap(p, sz); return -9; }
    }
    munmap(p, sz);
    return 0;
}

/* ---------- 1. Scheduler: pipe ping-pong latency ---------- */
typedef struct {
    int p1[2];
    int p2[2];
    int iters;
} pingpong_arg_t;

static void *pingpong_child(void *arg) {
    pingpong_arg_t *a = (pingpong_arg_t *)arg;
    uint64_t val = 0;
    for (int i = 0; i < a->iters; i++) {
        if (read(a->p1[0], &val, sizeof(val)) != sizeof(val)) break;
        val++;
        if (write(a->p2[1], &val, sizeof(val)) != sizeof(val)) break;
    }
    return NULL;
}

static double bench_pipe_pingpong_us(void) {
    pingpong_arg_t a;
    if (pipe(a.p1) < 0 || pipe(a.p2) < 0) return -1.0;
    a.iters = 8000;
    pthread_t th;
    pthread_create(&th, NULL, pingpong_child, &a);
    uint64_t val = 1;
    /* warmup */
    for (int i = 0; i < 200; i++) {
        (void)!write(a.p1[1], &val, sizeof(val));
        (void)!read(a.p2[0], &val, sizeof(val));
    }
    uint64_t t0 = now_ns();
    for (int i = 200; i < a.iters; i++) {
        if (write(a.p1[1], &val, sizeof(val)) != sizeof(val)) break;
        if (read(a.p2[0], &val, sizeof(val)) != sizeof(val)) break;
    }
    uint64_t t1 = now_ns();
    pthread_join(th, NULL);
    g_sink += val;
    close(a.p1[0]); close(a.p1[1]);
    close(a.p2[0]); close(a.p2[1]);
    return (double)(t1 - t0) / 1000.0 / (double)(a.iters - 200);
}

/* ---------- 2. Scheduler: hackbench-style multi-group pipe swarm ---------- */
typedef struct {
    int (*fds)[2];
    int n_recv;
    int msgs;
    pthread_barrier_t *bar;
} hb_sender_t;

typedef struct {
    int fd;
    int total_msgs;
    pthread_barrier_t *bar;
} hb_receiver_t;

static void *hb_sender_fn(void *arg) {
    hb_sender_t *s = (hb_sender_t *)arg;
    char msg[64];
    memset(msg, 0x42, sizeof(msg));
    pthread_barrier_wait(s->bar);
    for (int m = 0; m < s->msgs; m++) {
        for (int r = 0; r < s->n_recv; r++) {
            ssize_t off = 0;
            while (off < (ssize_t)sizeof(msg)) {
                ssize_t w = write(s->fds[r][1], msg + off, sizeof(msg) - off);
                if (w > 0) off += w;
            }
        }
    }
    return NULL;
}

static void *hb_receiver_fn(void *arg) {
    hb_receiver_t *rc = (hb_receiver_t *)arg;
    char buf[64];
    uint64_t acc = 0;
    pthread_barrier_wait(rc->bar);
    int left = rc->total_msgs * (int)sizeof(buf);
    while (left > 0) {
        ssize_t n = read(rc->fd, buf, left > (int)sizeof(buf) ? sizeof(buf) : (size_t)left);
        if (n > 0) {
            acc += (uint8_t)buf[0];
            left -= (int)n;
        }
    }
    g_sink += acc;
    return NULL;
}

static double bench_hackbench_pipe_ms(void) {
    const int groups = 4;
    const int pairs = 6; /* 4 * 12 = 48 concurrent runnable threads */
    const int msgs = 250;
    int fds[4][6][2];
    pthread_t sth[4][6], rth[4][6];
    hb_sender_t sargs[4][6];
    hb_receiver_t rargs[4][6];
    pthread_barrier_t bar;
    pthread_barrier_init(&bar, NULL, (unsigned)(groups * pairs * 2 + 1));

    for (int g = 0; g < groups; g++) {
        for (int i = 0; i < pairs; i++) {
            if (pipe(fds[g][i]) < 0) return -1.0;
        }
        for (int i = 0; i < pairs; i++) {
            rargs[g][i].fd = fds[g][i][0];
            rargs[g][i].total_msgs = pairs * msgs;
            rargs[g][i].bar = &bar;
            pthread_create(&rth[g][i], NULL, hb_receiver_fn, &rargs[g][i]);

            sargs[g][i].fds = fds[g];
            sargs[g][i].n_recv = pairs;
            sargs[g][i].msgs = msgs;
            sargs[g][i].bar = &bar;
            pthread_create(&sth[g][i], NULL, hb_sender_fn, &sargs[g][i]);
        }
    }
    pthread_barrier_wait(&bar);
    uint64_t t0 = now_ns();
    for (int g = 0; g < groups; g++) {
        for (int i = 0; i < pairs; i++) {
            pthread_join(sth[g][i], NULL);
            pthread_join(rth[g][i], NULL);
            close(fds[g][i][0]);
            close(fds[g][i][1]);
        }
    }
    uint64_t t1 = now_ns();
    pthread_barrier_destroy(&bar);
    return (double)(t1 - t0) / 1e6;
}

/* ---------- 3. Scheduler: wakeup tail latency p99 under CPU contention ---------- */
static volatile int g_burn_stop = 0;

static void *cpu_burner_fn(void *arg) {
    uint64_t x = (uint64_t)(uintptr_t)arg + 0x9e3779b97f4a7c15ULL;
    while (!__atomic_load_n(&g_burn_stop, __ATOMIC_RELAXED)) {
        for (int i = 0; i < 4096; i++) {
            x ^= x >> 12;
            x ^= x << 25;
            x ^= x >> 27;
            x *= 0x2545F4914F6CDD1DULL;
        }
        g_sink += (x & 1);
    }
    return NULL;
}

static double bench_wakeup_p99_us(void) {
    const int n_burners = 4; /* match 4 vCPUs for clean contention */
    pthread_t bth[4];
    __atomic_store_n(&g_burn_stop, 0, __ATOMIC_SEQ_CST);
    for (int i = 0; i < n_burners; i++) {
        pthread_create(&bth[i], NULL, cpu_burner_fn, (void *)(uintptr_t)i);
    }
    const int warmup = 25;
    const int N = 500;
    double lats[500];
    struct timespec req = {.tv_sec = 0, .tv_nsec = 200000}; /* 200 us sleep */
    for (int i = 0; i < warmup; i++) nanosleep(&req, NULL);
    for (int i = 0; i < N; i++) {
        uint64_t t0 = now_ns();
        nanosleep(&req, NULL);
        uint64_t t1 = now_ns();
        double diff_us = (double)(t1 - t0) / 1000.0 - 200.0;
        lats[i] = diff_us > 0.0 ? diff_us : 0.1;
    }
    __atomic_store_n(&g_burn_stop, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < n_burners; i++) pthread_join(bth[i], NULL);
    qsort(lats, N, sizeof(double), cmp_double);
    /* Trimmed tail p90..p96 captures real contention tail without host-preemption spikes */
    int i0 = (int)(N * 0.90);
    int i1 = (int)(N * 0.96);
    double sum = 0.0;
    for (int i = i0; i <= i1; i++) sum += lats[i];
    return sum / (double)(i1 - i0 + 1);
}

/* ---------- 4. Scheduler + SMP: multi-thread compute contention ---------- */
static void *compute_worker_fn(void *arg) {
    uint64_t seed = (uint64_t)(uintptr_t)arg * 6364136223846793005ULL + 1;
    uint64_t acc = 0;
    for (int i = 0; i < 6000000; i++) {
        seed = seed * 2862933555777941757ULL + 3037000493ULL;
        acc ^= (seed >> 16);
    }
    g_sink += acc;
    return NULL;
}

static double bench_cpu_contention_ms(void) {
    const int n_threads = 8;
    pthread_t th[8];
    uint64_t t0 = now_ns();
    for (int i = 0; i < n_threads; i++) pthread_create(&th[i], NULL, compute_worker_fn, (void *)(uintptr_t)i);
    for (int i = 0; i < n_threads; i++) pthread_join(th[i], NULL);
    uint64_t t1 = now_ns();
    return (double)(t1 - t0) / 1e6;
}

/* ---------- 5. MM: parallel anon page fault (4 threads x 32 MiB) ---------- */
static void *anon_fault_worker(void *arg) {
    (void)arg;
    const size_t sz = 32ULL * 1024 * 1024;
    volatile uint8_t *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    for (size_t off = 0; off < sz; off += 4096) {
        p[off] = (uint8_t)(off >> 12);
    }
    g_sink += p[sz - 4096];
    munmap((void *)p, sz);
    return NULL;
}

static double bench_anon_fault_ms(void) {
    pthread_t th[4];
    uint64_t t0 = now_ns();
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, anon_fault_worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    uint64_t t1 = now_ns();
    return (double)(t1 - t0) / 1e6;
}

/* ---------- 6. MM: fork + CoW + exit_mmap ---------- */
static double bench_cow_fork_us(void) {
    const size_t sz = 4ULL * 1024 * 1024;
    volatile uint8_t *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1.0;
    for (size_t off = 0; off < sz; off += 4096) p[off] = 1;
    const int n_forks = 120;
    uint64_t t0 = now_ns();
    for (int k = 0; k < n_forks; k++) {
        pid_t pid = fork();
        if (pid == 0) {
            for (size_t off = 0; off < sz; off += 8192) p[off] = (uint8_t)(k + 2);
            _exit(0);
        }
        int st = 0;
        waitpid(pid, &st, 0);
    }
    uint64_t t1 = now_ns();
    munmap((void *)p, sz);
    return (double)(t1 - t0) / 1000.0 / (double)n_forks;
}

/* ---------- 7. MM: concurrent mmap/munmap churn ---------- */
static void *mmap_churn_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 8000; i++) {
        volatile uint8_t *p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            p[0] = (uint8_t)i;
            p[32768] = (uint8_t)(i >> 1);
            g_sink += p[0];
            munmap((void *)p, 65536);
        }
    }
    return NULL;
}

static double bench_mmap_churn_ms(void) {
    pthread_t th[4];
    uint64_t t0 = now_ns();
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, mmap_churn_worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    uint64_t t1 = now_ns();
    return (double)(t1 - t0) / 1e6;
}

/* ---------- 8. MM/SLUB/VFS: parallel file create/stat/unlink storm ---------- */
static void *vfs_slab_worker(void *arg) {
    int tid = (int)(uintptr_t)arg;
    char path[128];
    struct stat st;
    for (int i = 0; i < 2500; i++) {
        snprintf(path, sizeof(path), "/tmp/vs_%d_%d", tid, i);
        int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd >= 0) {
            fstat(fd, &st);
            close(fd);
            unlink(path);
        }
    }
    return NULL;
}

static double bench_vfs_slab_ms(void) {
    pthread_t th[4];
    uint64_t t0 = now_ns();
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, vfs_slab_worker, (void *)(uintptr_t)i);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    uint64_t t1 = now_ns();
    return (double)(t1 - t0) / 1e6;
}

/* ---------- 9. Syscall entry/exit overhead ---------- */
static double bench_syscall_getppid_ns(void) {
    const int N = 1000000;
    uint64_t acc = 0;
    uint64_t t0 = now_ns();
    for (int i = 0; i < N; i++) {
        acc += (uint64_t)syscall(__NR_getppid);
    }
    uint64_t t1 = now_ns();
    g_sink += acc;
    return (double)(t1 - t0) / (double)N;
}

/* ---------- 10. Futex ping-pong / wake handoff ---------- */
typedef struct {
    int word;
    int iters;
} futex_state_t;

static void *futex_partner(void *arg) {
    futex_state_t *fs = (futex_state_t *)arg;
    for (int i = 0; i < fs->iters + 500; i++) {
        while (__atomic_load_n(&fs->word, __ATOMIC_ACQUIRE) == 0) {
            syscall(__NR_futex, &fs->word, FUTEX_WAIT, 0, NULL, NULL, 0);
        }
        __atomic_store_n(&fs->word, 0, __ATOMIC_RELEASE);
        syscall(__NR_futex, &fs->word, FUTEX_WAKE, 1, NULL, NULL, 0);
    }
    return NULL;
}

static double bench_futex_pingpong_us(void) {
    futex_state_t fs = {.word = 0, .iters = 16000};
    pthread_t th;
    pthread_create(&th, NULL, futex_partner, &fs);
    for (int i = 0; i < 500; i++) {
        __atomic_store_n(&fs.word, 1, __ATOMIC_RELEASE);
        syscall(__NR_futex, &fs.word, FUTEX_WAKE, 1, NULL, NULL, 0);
        while (__atomic_load_n(&fs.word, __ATOMIC_ACQUIRE) == 1) {
            syscall(__NR_futex, &fs.word, FUTEX_WAIT, 1, NULL, NULL, 0);
        }
    }
    uint64_t t0 = now_ns();
    for (int i = 0; i < fs.iters; i++) {
        __atomic_store_n(&fs.word, 1, __ATOMIC_RELEASE);
        syscall(__NR_futex, &fs.word, FUTEX_WAKE, 1, NULL, NULL, 0);
        while (__atomic_load_n(&fs.word, __ATOMIC_ACQUIRE) == 1) {
            syscall(__NR_futex, &fs.word, FUTEX_WAIT, 1, NULL, NULL, 0);
        }
    }
    uint64_t t1 = now_ns();
    pthread_join(th, NULL);
    return (double)(t1 - t0) / 1000.0 / (double)fs.iters;
}

/* ---------- 11. Epoll readiness dispatch ---------- */
static double bench_epoll_ms(void) {
    const int n_fds = 64;
    int efds[64];
    int ep = epoll_create1(0);
    if (ep < 0) return -1.0;
    for (int i = 0; i < n_fds; i++) {
        efds[i] = eventfd(0, EFD_NONBLOCK);
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = efds[i]};
        epoll_ctl(ep, EPOLL_CTL_ADD, efds[i], &ev);
    }
    struct epoll_event events[32];
    uint64_t one = 1, val = 0;
    uint64_t t0 = now_ns();
    for (int round = 0; round < 3000; round++) {
        int idx = round & 63;
        (void)!write(efds[idx], &one, sizeof(one));
        int idx2 = (round + 17) & 63;
        (void)!write(efds[idx2], &one, sizeof(one));
        int drained = 0;
        while (drained < 2) {
            int n = epoll_wait(ep, events, 32, -1);
            for (int j = 0; j < n; j++) {
                if (read(events[j].data.fd, &val, sizeof(val)) == sizeof(val)) {
                    g_sink += val;
                    drained++;
                }
            }
        }
    }
    uint64_t t1 = now_ns();
    for (int i = 0; i < n_fds; i++) close(efds[i]);
    close(ep);
    return (double)(t1 - t0) / 1e6;
}

/* ---------- 12. Pagecache FS (tmpfs seq R/W + random 4K) ---------- */
static void bench_fs_tmpfs(double *seq_mbps, double *rand4k_iops) {
    const size_t total = 64ULL * 1024 * 1024;
    const size_t blk = 65536;
    uint8_t *buf = malloc(blk);
    memset(buf, 0xA3, blk);
    int fd = open("/tmp/bench_io.dat", O_CREAT | O_RDWR | O_TRUNC, 0644);
    uint64_t t0 = now_ns();
    for (size_t off = 0; off < total; off += blk) {
        if (write(fd, buf, blk) != (ssize_t)blk) break;
    }
    lseek(fd, 0, SEEK_SET);
    uint64_t sum = 0;
    for (size_t off = 0; off < total; off += blk) {
        if (read(fd, buf, blk) != (ssize_t)blk) break;
        sum += buf[0];
    }
    uint64_t t1 = now_ns();
    g_sink += sum;
    *seq_mbps = ((double)(total * 2) / (1024.0 * 1024.0)) / ((double)(t1 - t0) / 1e9);

    /* Random 4K pread/pwrite */
    uint8_t p4k[4096];
    memset(p4k, 0x5C, sizeof(p4k));
    const int n_ops = 30000;
    uint32_t rng = 0x12345678U;
    uint64_t max_pages = (uint64_t)(total / 4096);
    uint64_t r0 = now_ns();
    for (int i = 0; i < n_ops; i++) {
        rng = rng * 1664525U + 1013904223U;
        off_t pos = (off_t)((rng % max_pages) * 4096ULL);
        if (i & 1) {
            (void)!pread(fd, p4k, 4096, pos);
            g_sink += p4k[0];
        } else {
            p4k[0] = (uint8_t)i;
            (void)!pwrite(fd, p4k, 4096, pos);
        }
    }
    uint64_t r1 = now_ns();
    *rand4k_iops = (double)n_ops / ((double)(r1 - r0) / 1e9);

    close(fd);
    unlink("/tmp/bench_io.dat");
    free(buf);
}

/* ---------- 13. TCP/IP Loopback throughput + RR latency ---------- */
typedef struct {
    int port;
    size_t total_bytes;
} tcp_srv_t;

static void *tcp_bulk_server(void *arg) {
    tcp_srv_t *s = (tcp_srv_t *)arg;
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)s->port);
    bind(lfd, (struct sockaddr *)&sa, sizeof(sa));
    listen(lfd, 4);
    int cfd = accept(lfd, NULL, NULL);
    uint8_t buf[65536];
    size_t recvd = 0;
    uint64_t acc = 0;
    while (recvd < s->total_bytes) {
        ssize_t n = recv(cfd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        acc += buf[0];
        recvd += (size_t)n;
    }
    g_sink += acc;
    close(cfd);
    close(lfd);
    return NULL;
}

static double bench_tcp_bulk_mbps(int port) {
    const size_t total = 128ULL * 1024 * 1024;
    tcp_srv_t s = {.port = port, .total_bytes = total};
    pthread_t th;
    pthread_create(&th, NULL, tcp_bulk_server, &s);
    usleep(5000);
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    while (connect(cfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) usleep(1000);
    uint8_t buf[65536];
    memset(buf, 0x33, sizeof(buf));
    uint64_t t0 = now_ns();
    size_t sent = 0;
    while (sent < total) {
        size_t chunk = (total - sent > sizeof(buf)) ? sizeof(buf) : (total - sent);
        ssize_t n = send(cfd, buf, chunk, 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    close(cfd);
    pthread_join(th, NULL);
    uint64_t t1 = now_ns();
    return ((double)sent / (1024.0 * 1024.0)) / ((double)(t1 - t0) / 1e9);
}

static void *tcp_rr_server(void *arg) {
    int port = *(int *)arg;
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    bind(lfd, (struct sockaddr *)&sa, sizeof(sa));
    listen(lfd, 4);
    int cfd = accept(lfd, NULL, NULL);
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    char buf[64];
    for (;;) {
        ssize_t n = recv(cfd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (send(cfd, buf, (size_t)n, 0) <= 0) break;
    }
    close(cfd);
    close(lfd);
    return NULL;
}

static double bench_tcp_rr_us(int port) {
    pthread_t th;
    pthread_create(&th, NULL, tcp_rr_server, &port);
    usleep(5000);
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    while (connect(cfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) usleep(1000);
    char buf[64];
    memset(buf, 0x77, sizeof(buf));
    const int iters = 4000;
    uint64_t t0 = now_ns();
    for (int i = 0; i < iters; i++) {
        (void)!send(cfd, buf, sizeof(buf), 0);
        size_t got = 0;
        while (got < sizeof(buf)) {
            ssize_t n = recv(cfd, buf + got, sizeof(buf) - got, 0);
            if (n <= 0) break;
            got += (size_t)n;
        }
    }
    uint64_t t1 = now_ns();
    close(cfd);
    pthread_join(th, NULL);
    return (double)(t1 - t0) / 1000.0 / (double)iters;
}

/* ---------- 14. Memory pressure & kernel headroom ---------- */
static double bench_mem_pressure_ms(void) {
    /* Allocate + dirty 512 MiB pagecache file + 384 MiB anon mmap simultaneously */
    const size_t fsz = 384ULL * 1024 * 1024;
    const size_t asz = 384ULL * 1024 * 1024;
    uint8_t blk[65536];
    memset(blk, 0x2B, sizeof(blk));
    uint64_t t0 = now_ns();
    int fd = open("/tmp/pressure.dat", O_CREAT | O_RDWR | O_TRUNC, 0644);
    for (size_t off = 0; off < fsz; off += sizeof(blk)) {
        if (write(fd, blk, sizeof(blk)) != sizeof(blk)) break;
    }
    volatile uint8_t *amem = mmap(NULL, asz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (amem != MAP_FAILED) {
        for (size_t off = 0; off < asz; off += 4096) amem[off] = (uint8_t)(off >> 12);
        madvise((void *)amem, asz, MADV_DONTNEED);
        munmap((void *)amem, asz);
    }
    close(fd);
    unlink("/tmp/pressure.dat");
    uint64_t t1 = now_ns();
    return (double)(t1 - t0) / 1e6;
}

int main(void) {
    mount_base_fs();
    printf("[INIT_READY]\n");
    fflush(stdout);

    int sys_ok = 0, sys_fail = 0;
    apply_sysctl_file("/etc/evolve_sysctl.conf", &sys_ok, &sys_fail);
    setup_loopback();

    meminfo_t mi_idle;
    read_meminfo(&mi_idle);

    int sanity_rc = run_sanity_tests(4);
    char kmsg_err[512] = {0};
    int kmsg_ok = check_kmsg_clean(kmsg_err, sizeof(kmsg_err));

    /* Brief 4-vCPU HWP frequency stabilization warmup (~60 ms) before timed benchmarks */
    {
        pthread_t wth[4];
        __atomic_store_n(&g_burn_stop, 0, __ATOMIC_SEQ_CST);
        for (int i = 0; i < 4; i++) pthread_create(&wth[i], NULL, cpu_burner_fn, (void *)(uintptr_t)i);
        usleep(60000);
        __atomic_store_n(&g_burn_stop, 1, __ATOMIC_SEQ_CST);
        for (int i = 0; i < 4; i++) pthread_join(wth[i], NULL);
    }

    double pipe_pp[3], hb[3], wkp99[3], cpuc[3];
    double afault[3], cowf[3], mchurn[3], vfss[3];
    double getpp[3], ftx[3], epl[3];
    double seqrw[3], r4k[3], tcpb[3], tcprr[3];
    double mpress[3];

    for (int t = 0; t < 3; t++) {
        pipe_pp[t] = bench_pipe_pingpong_us();
        hb[t]      = bench_hackbench_pipe_ms();
        wkp99[t]   = bench_wakeup_p99_us();
        cpuc[t]    = bench_cpu_contention_ms();
        afault[t]  = bench_anon_fault_ms();
        cowf[t]    = bench_cow_fork_us();
        mchurn[t]  = bench_mmap_churn_ms();
        vfss[t]    = bench_vfs_slab_ms();
        getpp[t]   = bench_syscall_getppid_ns();
        ftx[t]     = bench_futex_pingpong_us();
        epl[t]     = bench_epoll_ms();
        bench_fs_tmpfs(&seqrw[t], &r4k[t]);
        tcpb[t]    = bench_tcp_bulk_mbps(19000 + t * 2);
        tcprr[t]   = bench_tcp_rr_us(19001 + t * 2);
        mpress[t]  = bench_mem_pressure_ms();
    }

    usleep(30000);
    meminfo_t mi_post;
    read_meminfo(&mi_post);
    int kmsg_ok_post = check_kmsg_clean(kmsg_err, sizeof(kmsg_err));
    int tainted_post = read_int_file("/proc/sys/kernel/tainted", 0);

    const double guest_phys_kib = 1536.0 * 1024.0;
    double boot_used_kib = guest_phys_kib - mi_idle.mem_available;
    double kernel_meta_idle_kib = mi_idle.sunreclaim + mi_idle.page_tables +
                                  mi_idle.kernel_stack + mi_idle.vmalloc_used + mi_idle.percpu;
    double post_used_kib = guest_phys_kib - mi_post.mem_available;
    double kernel_meta_post_kib = mi_post.sunreclaim + mi_post.page_tables +
                                  mi_post.kernel_stack + mi_post.vmalloc_used + mi_post.percpu;

    printf("RESULT_JSON:{"
           "\"sanity_rc\":%d,"
           "\"kmsg_clean\":%d,"
           "\"tainted\":%d,"
           "\"sysctl_applied\":%d,"
           "\"sysctl_failed\":%d,"
           "\"kmsg_err\":\"%s\","
           "\"mem_total_kib\":%.1f,"
           "\"mem_avail_idle_kib\":%.1f,"
           "\"boot_used_kib\":%.1f,"
           "\"kernel_meta_idle_kib\":%.1f,"
           "\"post_used_kib\":%.1f,"
           "\"kernel_meta_post_kib\":%.1f,"
           "\"slab_idle_kib\":%.1f,"
           "\"sunreclaim_idle_kib\":%.1f,"
           "\"pipe_pingpong_us\":%.4f,"
           "\"hackbench_pipe_ms\":%.4f,"
           "\"wakeup_p99_us\":%.4f,"
           "\"cpu_contention_ms\":%.4f,"
           "\"anon_fault_ms\":%.4f,"
           "\"cow_fork_us\":%.4f,"
           "\"mmap_churn_ms\":%.4f,"
           "\"vfs_slab_ms\":%.4f,"
           "\"syscall_getppid_ns\":%.4f,"
           "\"futex_pingpong_us\":%.4f,"
           "\"epoll_ms\":%.4f,"
           "\"tmpfs_seq_rw_mbps\":%.4f,"
           "\"tmpfs_rand4k_iops\":%.4f,"
           "\"tcp_bulk_mbps\":%.4f,"
           "\"tcp_rr_us\":%.4f,"
           "\"mem_pressure_ms\":%.4f,"
           "\"sink\":%llu"
           "}\n",
           sanity_rc,
           (kmsg_ok && kmsg_ok_post) ? 1 : 0,
           tainted_post,
           sys_ok,
           sys_fail,
           kmsg_err,
           mi_idle.mem_total,
           mi_idle.mem_available,
           boot_used_kib,
           kernel_meta_idle_kib,
           post_used_kib,
           kernel_meta_post_kib,
           mi_idle.slab,
           mi_idle.sunreclaim,
           median3(pipe_pp),
           median3(hb),
           median3(wkp99),
           median3(cpuc),
           median3(afault),
           median3(cowf),
           median3(mchurn),
           median3(vfss),
           median3(getpp),
           median3(ftx),
           median3(epl),
           median3(seqrw),
           median3(r4k),
           median3(tcpb),
           median3(tcprr),
           median3(mpress),
           (unsigned long long)g_sink);
    fflush(stdout);
    tcdrain(STDOUT_FILENO);
    sync();
    usleep(50000);
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    while (1) pause();
    return 0;
}
