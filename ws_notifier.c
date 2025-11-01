#include "ws_notifier.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

#ifdef __linux__
#include <sys/epoll.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#endif

struct ws_notifier {
#ifdef __linux__
    int epoll_fd;
#elif defined(__APPLE__)
    int kqueue_fd;
#endif
};

ws_notifier_t* ws_notifier_init(void) {
    ws_notifier_t *notifier = (ws_notifier_t*)malloc(sizeof(ws_notifier_t));
    if (!notifier) {
        return NULL;
    }

    memset(notifier, 0, sizeof(ws_notifier_t));

#ifdef __linux__
    // Create epoll instance
    notifier->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (notifier->epoll_fd < 0) {
        perror("epoll_create1");
        free(notifier);
        return NULL;
    }
#elif defined(__APPLE__)
    // Create kqueue instance
    notifier->kqueue_fd = kqueue();
    if (notifier->kqueue_fd < 0) {
        perror("kqueue");
        free(notifier);
        return NULL;
    }
#else
    fprintf(stderr, "Event notifier not supported on this platform\n");
    free(notifier);
    return NULL;
#endif

    return notifier;
}

void ws_notifier_free(ws_notifier_t *notifier) {
    if (!notifier) return;

#ifdef __linux__
    if (notifier->epoll_fd >= 0) {
        close(notifier->epoll_fd);
    }
#elif defined(__APPLE__)
    if (notifier->kqueue_fd >= 0) {
        close(notifier->kqueue_fd);
    }
#endif

    free(notifier);
}

int ws_notifier_add(ws_notifier_t *notifier, int fd, int events) {
    if (!notifier || fd < 0) {
        return -1;
    }

#ifdef __linux__
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    ev.data.fd = fd;
    ev.events = EPOLLET;  // Edge-triggered mode

    if (events & WS_EVENT_READ) {
        ev.events |= EPOLLIN;
    }
    if (events & WS_EVENT_WRITE) {
        ev.events |= EPOLLOUT;
    }

    if (epoll_ctl(notifier->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl ADD");
        return -1;
    }

    return 0;

#elif defined(__APPLE__)
    struct kevent kev[2];
    int n_changes = 0;

    // Add read filter if requested
    if (events & WS_EVENT_READ) {
        EV_SET(&kev[n_changes], fd, EVFILT_READ,
               EV_ADD | EV_CLEAR,  // EV_CLEAR for edge-triggered behavior
               0, 0, NULL);
        n_changes++;
    }

    // Add write filter if requested
    if (events & WS_EVENT_WRITE) {
        EV_SET(&kev[n_changes], fd, EVFILT_WRITE,
               EV_ADD | EV_CLEAR,  // EV_CLEAR for edge-triggered behavior
               0, 0, NULL);
        n_changes++;
    }

    if (n_changes > 0) {
        if (kevent(notifier->kqueue_fd, kev, n_changes, NULL, 0, NULL) < 0) {
            perror("kevent ADD");
            return -1;
        }
    }

    return 0;
#else
    (void)events;
    return -1;
#endif
}

int ws_notifier_mod(ws_notifier_t *notifier, int fd, int events) {
    if (!notifier || fd < 0) {
        return -1;
    }

#ifdef __linux__
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    ev.data.fd = fd;
    ev.events = EPOLLET;  // Edge-triggered mode

    if (events & WS_EVENT_READ) {
        ev.events |= EPOLLIN;
    }
    if (events & WS_EVENT_WRITE) {
        ev.events |= EPOLLOUT;
    }

    if (epoll_ctl(notifier->epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        perror("epoll_ctl MOD");
        return -1;
    }

    return 0;

#elif defined(__APPLE__)
    struct kevent kev[2];
    int n_changes = 0;

    // Modify read filter
    if (events & WS_EVENT_READ) {
        EV_SET(&kev[n_changes], fd, EVFILT_READ,
               EV_ADD | EV_CLEAR,  // Modify (or add) with edge-triggered
               0, 0, NULL);
        n_changes++;
    } else {
        // Remove read filter if not requested
        EV_SET(&kev[n_changes], fd, EVFILT_READ,
               EV_DELETE,
               0, 0, NULL);
        n_changes++;
    }

    // Modify write filter
    if (events & WS_EVENT_WRITE) {
        EV_SET(&kev[n_changes], fd, EVFILT_WRITE,
               EV_ADD | EV_CLEAR,
               0, 0, NULL);
        n_changes++;
    } else {
        // Remove write filter if not requested
        EV_SET(&kev[n_changes], fd, EVFILT_WRITE,
               EV_DELETE,
               0, 0, NULL);
        n_changes++;
    }

    if (n_changes > 0) {
        if (kevent(notifier->kqueue_fd, kev, n_changes, NULL, 0, NULL) < 0) {
            perror("kevent MOD");
            return -1;
        }
    }

    return 0;
#else
    (void)events;
    return -1;
#endif
}

int ws_notifier_del(ws_notifier_t *notifier, int fd) {
    if (!notifier || fd < 0) {
        return -1;
    }

#ifdef __linux__
    if (epoll_ctl(notifier->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        perror("epoll_ctl DEL");
        return -1;
    }

    return 0;

#elif defined(__APPLE__)
    struct kevent kev[2];

    // Delete both read and write filters
    EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

    // It's okay if deletion fails (filter might not exist)
    kevent(notifier->kqueue_fd, kev, 2, NULL, 0, NULL);

    return 0;
#else
    return -1;
#endif
}

void ws_notifier_wait(ws_notifier_t *notifier) {
    if (!notifier) {
        return;
    }

#ifdef __linux__
    struct epoll_event events[1];
    static const int TIMEOUT_MS = 100;  // Fixed 100ms timeout for HFT
    epoll_wait(notifier->epoll_fd, events, 1, TIMEOUT_MS);

#elif defined(__APPLE__)
    struct kevent events[1];
    static const struct timespec TIMEOUT = {0, 100000000};  // Fixed 100ms timeout for HFT
    kevent(notifier->kqueue_fd, NULL, 0, events, 1, &TIMEOUT);

#endif
}
