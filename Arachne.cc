#include <stdio.h>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include "string.h"
#include "Arachne.h"
#include "SpinLock.h"

namespace Arachne {

enum InitializationState {
    NOT_INITIALIZED,
    INITIALIZING,
    INITIALIZED
};

struct WorkUnit {
    std::function<void()> workFunction;
    void* sp;
    bool finished; // The top of the stack of the current workunit, used for both indicating
    // whether this is new or old work, and also for making it easier to
    // recycle the stacks.
    void* stack;
};
void threadWrapper(WorkUnit* work);

const int stackSize = 1024 * 1024;
const int stackPoolSize = 100;

InitializationState initializationState = NOT_INITIALIZED;
unsigned numCores = 1;

/**
 * Work to do on each thread. 
 * TODO(hq6): If vector is too slow, we may switch to a linked list.
 */
static std::vector<std::deque<WorkUnit*> > workQueues;

/**
 * Protect each work queue.
 */
static SpinLock *workQueueLocks;

thread_local int kernelThreadId;
thread_local std::deque<void*> stackPool;

/**
 * These two values store the place in the kernel thread to return to when a
 * user task either yields or completes.
 */
thread_local void* libraryStackPointer;
thread_local WorkUnit *running;

/**
 * This function will allocate stacks and create kernel threads pinned to particular cores.
 *
 * Note that the setting of initializationState should probably use memory
 * barriers, but we are currently assuming that the application will only call
 * it from a single thread of execution.
 *
 * Calling the function twice is a no-op.
 */
void threadInit() {
    if (initializationState != NOT_INITIALIZED) {
        return;
    }
    initializationState = INITIALIZING;

    // Allocate stacks. Note that number of cores is actually number of
    // hyperthreaded cores, rather than necessarily real CPU cores.
    numCores = std::thread::hardware_concurrency(); 
    workQueueLocks = new SpinLock[numCores];
    for (unsigned int i = 0; i < numCores; i++) {
        workQueues.push_back(std::deque<WorkUnit*>());

        // Leave one thread for the main thread
        if (i != numCores - 1) {
            std::thread(threadMainFunction, i).detach();
        }
    }

    // Set the kernelThreadId for the main thread
    kernelThreadId = numCores - 1;

    initializationState = INITIALIZED;
}

/**
 * A utility function that runs the running user thread, and saves the current
 * stack pointer into saveStackPointer.
 *
 * If the running thread is a new thread and we have run out of stacks, we
 * simply put the running context back onto the queue, and return.
 */
bool runThread(void** saveStackPointer) {
    // No stack implies this thread has never run before
    if (!running->stack) {
        if (!stackPool.empty()) {
            running->stack = stackPool.front();
            stackPool.pop_front();
        } else {
            // Yield to someone else who does have a stack to run on
            std::lock_guard<SpinLock> guard(workQueueLocks[kernelThreadId]);
            workQueues[kernelThreadId].push_back(running);
            return false;
        }
        // Wrap the function to restore control when the user thread
        // terminates instead of yielding.
        running->sp = (char*) running->stack + stackSize - 64; 


        // set up the stack to pass the single argument in this case.
        *(void**) running->sp = (void*) running;
        running->sp = (char*) running->sp - sizeof (void*);
        *(void**) running->sp = (void*) threadWrapper;

        // Set up the initial stack with the registers. Currently we assume a reasonable initial value for the registers is 0.
        memset(((void**) running->sp - 6), 0, 6*sizeof(void*));
        running->sp = (char*) running->sp - 6*sizeof (void*);

        swapcontext(&running->sp, saveStackPointer);

        // Resume right after here when user task finishes or yields
        // Check if the currently running user thread is finished and recycle
        // its stack if it is.
        if (running->finished) {
            stackPool.push_front(running->stack);
        }
    } else {
        // Resume where we left off
        swapcontext(&running->sp, saveStackPointer);
    }
    return true;
}

/**
 * Main function for a kernel-level thread participating in the thread pool. 
 * It first allocates a pool of stacks, and then polls its own work queue for
 * work to do.
 */
void threadMainFunction(int id) {
    // Initialize stack pool for this thread
    for (int i = 0; i < stackPoolSize; i++) {
        stackPool.push_back(malloc(stackSize));
    }

    kernelThreadId = id;

    // Poll for work on my queue
    while (true) {
        // Need to lock the queue with a SpinLock
        { // Make the guard go out of scope as soon as it is no longer needed.
            std::lock_guard<SpinLock> guard(workQueueLocks[kernelThreadId]);
            if (workQueues[kernelThreadId].empty()) continue;

            running = workQueues[kernelThreadId].front();
            workQueues[kernelThreadId].pop_front();
        }
        runThread(&libraryStackPointer);
    }
}

/**
 * Load a new context without saving anything.
 */
void  __attribute__ ((noinline))  setcontext(void **context) {
//   asm("movq  0(%rdi), %rsp");
//   asm("movq  8(%rdi), %rax"); // TODO: Switch to using push / pop instructions
//   asm("movq  %rax, 0(%rsp)");

    // Load the stack pointer and restore the registers
    asm("movq (%rdi), %rsp");

    asm("popq %rbp\n\t"
        "popq %rbx\n\t"
        "popq %r15\n\t"
        "popq %r14\n\t"
        "popq %r13\n\t"
        "popq %r12");
}

/**
 * Save one set of registers and load another set.
 * %rdi, %rsi are the two addresses of where stack pointers are stored.
 *
 * Load from saved and store into target.
 */
void  __attribute__ ((noinline))  swapcontext(void **saved, void **target) {

    // Save the registers and store the stack pointer
    asm("pushq %r12\n\t"
        "pushq %r13\n\t"
        "pushq %r14\n\t"
        "pushq %r15\n\t"
        "pushq %rbx\n\t"
        "pushq %rbp\n\t"
        "movq  %rsp, (%rsi)");

    // Load the stack pointer and restore the registers
    asm("movq (%rdi), %rsp\n\t"
        "popq %rbp\n\t"
        "popq %rbx\n\t"
        "popq %r15\n\t"
        "popq %r14\n\t"
        "popq %r13\n\t"
        "popq %r12");
}

/**
 * TODO(hq6): Figure out whether we can omit the argument altogether and just
 * use the 'running' global variable instead.
 *
 * Note that this function will be jumped to directly using setcontext, so
 * there might be some weirdness with local variables.
 */
void threadWrapper(WorkUnit* work) {
   asm("movq 8(%%rsp), %0": "=r" (work));
   work->workFunction();
   work->finished = true;
   __asm__ __volatile__("lfence" ::: "memory");
   setcontext(&libraryStackPointer);
}

/**
 * Restore control back to the thread library.
 * Assume we are the running process in the current kernel thread if we are calling
 * yield.
 */
void yield() {
    workQueueLocks[kernelThreadId].lock();
    if (workQueues[kernelThreadId].empty()) {
        workQueueLocks[kernelThreadId].unlock();
        return; // Yield is noop if there is no longer work to be done.
    }

    void** saved = &running->sp;

    workQueues[kernelThreadId].push_back(running);
    running = workQueues[kernelThreadId].front();
    workQueues[kernelThreadId].pop_front();
    workQueueLocks[kernelThreadId].unlock();

    // Loop until we find a thread to run, in case there are new threads that
    // do not have a thread.
    while (!runThread(saved));
}

/**
 * Create a WorkUnit for the given task, on the same queue as the current
 * function.
 */
void createTask(std::function<void()> task) {
    WorkUnit *work = new WorkUnit;
    work->finished = false;
    work->workFunction = task;
    work->stack = NULL;
    std::lock_guard<SpinLock> guard(workQueueLocks[kernelThreadId]);
    workQueues[kernelThreadId].push_back(work);
}

/**
 * This is a special function to allow the main thread to join the thread pool
 * after seeding initial tasks for itself and possibly other threads.
 *
 * The other way of implementing this is to merge this function with the
 * threadInit, and ask the user to provide an initial task to run in the main
 * thread, which will presumably spawn other tasks.
 */
void mainThreadJoinPool() {
    threadMainFunction(numCores - 1);
}

}
