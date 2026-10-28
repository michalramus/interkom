#pragma once

// Raises the calling thread's scheduling priority, for threads that must run
// on time every chunk (clock, mixer, RTP sender). macOS: QoS user-interactive.
// Linux: SCHED_FIFO; without permission it prints one warning and carries on
// at normal priority.
void raiseThreadPriority();
