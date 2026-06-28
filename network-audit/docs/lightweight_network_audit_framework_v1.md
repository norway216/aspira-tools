
# Lightweight Network Audit Framework V1 (C / Linux)

## 1. Design Goals
- Ultra-low memory usage (<20MB runtime)
- High concurrency (10k+ non-blocking connections)
- Event-driven architecture (epoll-based)
- Stability-first design (no blocking paths, no leaks)
- Minimal dependencies (libc + pthread + optional sqlite)

---

## 2. System Architecture

```
CLI Controller
      |
      v
Core Reactor (epoll event loop)
      |
      +----------------------+
      |                      |
      v                      v
Scan Engine          Worker Pool (optional)
      |                      |
      +----------+-----------+
                 |
                 v
        Non-blocking Socket Layer
                 |
                 v
           Linux TCP/IP Stack
```

---

## 3. Core Components

### 3.1 Reactor (Event Loop)
- epoll-based event dispatcher
- manages all socket lifecycle
- integrates timer wheel

```c
typedef struct {
    int epoll_fd;
    int running;
} reactor_t;
```

---

### 3.2 FD Manager
- controls max open sockets
- prevents FD leakage
- reuse buffer strategy

```c
typedef struct {
    int max_fd;
    int current_fd;
} fd_manager_t;
```

---

### 3.3 Scan Engine
- non-blocking TCP connect scanning
- event-driven state machine

States:
- INIT
- CONNECTING
- OPEN
- CLOSED
- TIMEOUT

---

### 3.4 Timer Wheel
- replaces sleep/timeout blocking
- O(1) timer management

```c
#define WHEEL_SIZE 256

typedef struct timer_node {
    int expire_tick;
    void (*callback)(void *);
    void *arg;
} timer_node_t;
```

---

### 3.5 Worker Pool
- optional multithread processing
- used for CPU-heavy tasks only

Tasks:
- fingerprint parsing
- JSON serialization
- risk scoring

---

### 3.6 Fingerprint Engine
- lightweight string matching
- no heavy parsing

Example:
- SSH banner detection
- HTTP server header parsing

---

### 3.7 Result Pipeline
- lock-free ring buffer (or mutex fallback)

```c
typedef struct {
    int head;
    int tail;
    void *items[1024];
} ring_queue_t;
```

---

### 3.8 Storage Layer (Optional)
SQLite schema:

```sql
CREATE TABLE asset (
    ip TEXT,
    port INTEGER,
    service TEXT,
    state INTEGER,
    last_seen INTEGER
);
```

---

## 4. Network Model

Non-blocking socket flow:

1. socket()
2. set O_NONBLOCK
3. connect()
4. epoll monitoring
5. result callback

---

## 5. Concurrency Model

Single reactor thread + optional worker threads:

- 1 event loop thread
- N worker threads (CPU tasks only)

Advantages:
- minimal locking
- low context switching
- scalable to multi-core

---

## 6. Memory Optimization

- avoid malloc in hot path
- reuse buffers
- batch epoll events
- FD limit control

---

## 7. Security Constraints

Allowed scope:
- internal lab networks only

Restrictions:
- no brute force
- no exploitation
- no credential attacks
- no lateral movement

---

## 8. Performance Targets

| Metric | Target |
|------|--------|
| Memory | < 20MB |
| Concurrency | 10k–50k sockets |
| CPU idle | > 90% |
| Crash rate | 0 |

---

## 9. Project Structure

```
net-audit-light/
├── core/
│   ├── reactor.c
│   ├── timer_wheel.c
├── scan/
│   ├── scanner.c
├── net/
│   ├── socket.c
├── fp/
│   ├── fingerprint.c
├── worker/
│   ├── pool.c
├── result/
│   ├── queue.c
├── db/
│   ├── sqlite.c
├── cli/
│   ├── main.c
├── include/
├── Makefile
```

---

## 10. Future Evolution (V2)

- io_uring integration
- lock-free MPMC queue
- distributed agent mode
- topology visualization
- embedded RK3588 deployment daemon
