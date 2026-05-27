/** Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */

#include "platform/platform-stackguard.h"

#include "xylem/xylem-logger.h"

#include <signal.h>
#include <stddef.h>
#include <stdlib.h>

#define STACKGUARD_ALT_STACK_SIZE (64 * 1024)

static platform_stackguard_cb_t _callback;
static struct sigaction         _old_segv;
static struct sigaction         _old_bus;
static _Thread_local void*      _alt_stack_mem;

static void _stackguard_handler(int sig, siginfo_t* info, void* uctx) {
    (void)uctx;

    if (_callback && _callback((uintptr_t)info->si_addr)) {
        return;
    }

    struct sigaction* old = (sig == SIGBUS) ? &_old_bus : &_old_segv;
    sigaction(sig, old, NULL);
    raise(sig);
}

void platform_stackguard_install(platform_stackguard_cb_t cb) {
    _callback = cb;

    struct sigaction sa = {0};
    sa.sa_sigaction     = _stackguard_handler;
    sa.sa_flags         = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;

    sigaction(SIGSEGV, &sa, &_old_segv);
    sigaction(SIGBUS, &sa, &_old_bus);
}

void platform_stackguard_uninstall(void) {
    sigaction(SIGSEGV, &_old_segv, NULL);
    sigaction(SIGBUS, &_old_bus, NULL);
    _callback = NULL;
}

void platform_stackguard_thread_init(void) {
    void* mem = malloc(STACKGUARD_ALT_STACK_SIZE);
    if (!mem) {
        xylem_loge("stackguard: failed to allocate alt stack");
        return;
    }
    _alt_stack_mem = mem;

    stack_t ss  = {0};
    ss.ss_sp    = mem;
    ss.ss_size  = STACKGUARD_ALT_STACK_SIZE;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
}

void platform_stackguard_thread_deinit(void) {
    if (_alt_stack_mem) {
        stack_t ss = {0};
        ss.ss_flags = SS_DISABLE;
        sigaltstack(&ss, NULL);
        free(_alt_stack_mem);
        _alt_stack_mem = NULL;
    }
}
