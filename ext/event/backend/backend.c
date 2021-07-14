// Copyright, 2021, by Samuel G. D. Williams. <http://www.codeotaku.com>
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "backend.h"
#include <fcntl.h>

static ID id_transfer, id_alive_p;

#ifndef HAVE__RB_FIBER_TRANSFER
VALUE Event_Backend_fiber_transfer(VALUE fiber, int argc, VALUE *argv) {
	return rb_funcallv(fiber, id_transfer, argc, argv);
}
#endif

#ifndef HAVE__RB_FIBER_RAISE
static ID id_raise;

VALUE Event_Backend_fiber_raise(VALUE fiber, int argc, VALUE *argv) {
	return rb_funcallv(fiber, id_raise, argc, argv);
}
#endif

#ifndef HAVE_RB_IO_DESCRIPTOR
static ID id_fileno;

int Event_Backend_io_descriptor(VALUE io) {
	return RB_NUM2INT(rb_funcall(io, id_fileno, 0));
}
#endif

#ifndef HAVE_RB_PROCESS_STATUS_WAIT
static ID id_wait;
static VALUE rb_Process_Status = Qnil;

VALUE Event_Backend_process_status_wait(rb_pid_t pid)
{
	return rb_funcall(rb_Process_Status, id_wait, 2, PIDT2NUM(pid), INT2NUM(WNOHANG));
}
#endif

int Event_Backend_nonblock_set(int file_descriptor)
{
	int flags = fcntl(file_descriptor, F_GETFL, 0);
	
	if (!(flags & O_NONBLOCK)) {
		fcntl(file_descriptor, F_SETFL, flags | O_NONBLOCK);
	}
	
	return flags;
}

void Event_Backend_nonblock_restore(int file_descriptor, int flags)
{
	if (!(flags & O_NONBLOCK)) {
		fcntl(file_descriptor, F_SETFL, flags & ~flags);
	}
}

void Init_Event_Backend(VALUE Event_Backend) {
	id_transfer = rb_intern("transfer");
	id_alive_p = rb_intern("alive?");
	
#ifndef HAVE__RB_FIBER_RAISE
	id_raise = rb_intern("raise");
#endif
	
#ifndef HAVE_RB_IO_DESCRIPTOR
	id_fileno = rb_intern("fileno");
#endif
	
#ifndef HAVE_RB_PROCESS_STATUS_WAIT
	id_wait = rb_intern("wait");
	rb_Process_Status = rb_const_get_at(rb_mProcess, rb_intern("Status"));
#endif
}

struct wait_and_transfer_arguments {
	int argc;
	VALUE *argv;
	
	struct Event_Backend *backend;
	struct Event_Backend_Queue *waiting;
};

static void queue_pop(struct Event_Backend *backend, struct Event_Backend_Queue *waiting) {
	if (waiting->behind) {
		waiting->behind->infront = waiting->infront;
	} else {
		backend->waiting = waiting->infront;
	}
	
	if (waiting->infront) {
		waiting->infront->behind = waiting->behind;
	} else {
		backend->ready = waiting->behind;
	}
}

static void queue_push(struct Event_Backend *backend, struct Event_Backend_Queue *waiting) {
	if (backend->waiting) {
		backend->waiting->behind = waiting;
		waiting->infront = backend->waiting;
	} else {
		backend->ready = waiting;
	}
	
	backend->waiting = waiting;
}

static VALUE wait_and_transfer(VALUE _arguments) {
	struct wait_and_transfer_arguments *arguments = (struct wait_and_transfer_arguments *)_arguments;
	
	VALUE fiber = arguments->argv[0];
	int argc = arguments->argc - 1;
	VALUE *argv = arguments->argv + 1;
	
	return Event_Backend_fiber_transfer(fiber, argc, argv);
}

static VALUE wait_and_transfer_ensure(VALUE _arguments) {
	struct wait_and_transfer_arguments *arguments = (struct wait_and_transfer_arguments *)_arguments;
	
	queue_pop(arguments->backend, arguments->waiting);
	
	return Qnil;
}

VALUE Event_Backend_wait_and_transfer(struct Event_Backend *backend, int argc, VALUE *argv)
{
	rb_check_arity(1, UNLIMITED_ARGUMENTS);
	
	struct Event_Backend_Queue waiting = {
		.behind = NULL,
		.infront = NULL,
		.flags = EVENT_BACKEND_QUEUE_FIBER,
		.fiber = rb_fiber_current()
	};
	
	queue_push(backend, &waiting);
	
	struct wait_and_transfer_arguments arguments = {
		.argc = argc,
		.argv = argv,
		.backend = backend,
		.waiting = &waiting,
	};
	
	return rb_ensure(wait_and_transfer, (VALUE)&arguments, wait_and_transfer_ensure, (VALUE)&arguments);
}

static VALUE wait_and_raise(VALUE _arguments) {
	struct wait_and_transfer_arguments *arguments = (struct wait_and_transfer_arguments *)_arguments;
	
	VALUE fiber = arguments->argv[0];
	int argc = arguments->argc - 1;
	VALUE *argv = arguments->argv + 1;
	
	return Event_Backend_fiber_raise(fiber, argc, argv);
}

VALUE Event_Backend_wait_and_raise(struct Event_Backend *backend, int argc, VALUE *argv)
{
	rb_check_arity(2, UNLIMITED_ARGUMENTS);
	
	struct Event_Backend_Queue waiting = {
		.behind = NULL,
		.infront = NULL,
		.flags = EVENT_BACKEND_QUEUE_FIBER,
		.fiber = rb_fiber_current()
	};
	
	queue_push(backend, &waiting);
	
	struct wait_and_transfer_arguments arguments = {
		.argc = argc,
		.argv = argv,
		.backend = backend,
		.waiting = &waiting,
	};
	
	return rb_ensure(wait_and_raise, (VALUE)&arguments, wait_and_transfer_ensure, (VALUE)&arguments);
}

void Event_Backend_queue_push(struct Event_Backend *backend, VALUE fiber)
{
	struct Event_Backend_Queue *waiting = malloc(sizeof(struct Event_Backend_Queue));
	
	waiting->behind = NULL;
	waiting->infront = NULL;
	waiting->flags = EVENT_BACKEND_QUEUE_INTERNAL;
	waiting->fiber = fiber;
	
	queue_push(backend, waiting);
}

static inline
void Event_Backend_queue_pop(struct Event_Backend *backend, struct Event_Backend_Queue *ready)
{
	if (ready->flags & EVENT_BACKEND_QUEUE_FIBER) {
		Event_Backend_fiber_transfer(ready->fiber, 0, NULL);
	} else {
		VALUE fiber = ready->fiber;
		queue_pop(backend, ready);
		free(ready);
		
		if (RTEST(rb_funcall(fiber, id_alive_p, 0))) {
			rb_funcall(fiber, id_transfer, 0);
		}
	}
}

int Event_Backend_queue_flush(struct Event_Backend *backend)
{
	int count = 0;
	
	// Get the current tail and head of the queue:
	struct Event_Backend_Queue *waiting = backend->waiting;
	
	// Process from head to tail in order:
	// During this, more items may be appended to tail.
	while (backend->ready) {
		struct Event_Backend_Queue *ready = backend->ready;
		
		count += 1;
		Event_Backend_queue_pop(backend, ready);
		
		if (ready == waiting) break;
	}
	
	return count;
}

void Event_Backend_elapsed_time(struct timespec* start, struct timespec* stop, struct timespec *duration)
{
	if ((stop->tv_nsec - start->tv_nsec) < 0) {
		duration->tv_sec = stop->tv_sec - start->tv_sec - 1;
		duration->tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
	} else {
		duration->tv_sec = stop->tv_sec - start->tv_sec;
		duration->tv_nsec = stop->tv_nsec - start->tv_nsec;
	}
}

void Event_Backend_current_time(struct timespec *time) {
	clock_gettime(CLOCK_MONOTONIC, time);
}
