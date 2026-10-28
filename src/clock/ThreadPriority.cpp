#include "clock/ThreadPriority.hpp"

#include <pthread.h>

#if defined(__APPLE__)
#include <pthread/qos.h>
#else
#include <atomic>
#include <cstring>
#include <iostream>
#include <sched.h>
#endif

void raiseThreadPriority() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#else
    sched_param param{};
    param.sched_priority = sched_get_priority_min(SCHED_FIFO) + 10;
    if (const int error = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param); error != 0) {
        static std::atomic_bool warned{false};
        if (!warned.exchange(true)) {
            std::cerr << "note: SCHED_FIFO refused (" << std::strerror(error)
                      << "); running at normal priority\n";
        }
    }
#endif
}
