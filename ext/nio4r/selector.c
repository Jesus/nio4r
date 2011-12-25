/*
 * Copyright (c) 2011 Tony Arcieri. Distributed under the MIT License. See
 * LICENSE.txt for further details.
 */

#include "nio4r.h"
#include "rubysig.h"

static VALUE mNIO = Qnil;
static VALUE cNIO_Channel  = Qnil;
static VALUE cNIO_Monitor  = Qnil;
static VALUE cNIO_Selector = Qnil;

/* Allocator/deallocator */
static VALUE NIO_Selector_allocate(VALUE klass);
static void NIO_Selector_mark(struct NIO_Selector *loop);
static void NIO_Selector_shutdown(struct NIO_Selector *selector);
static void NIO_Selector_free(struct NIO_Selector *loop);

/* Methods */
static VALUE NIO_Selector_initialize(VALUE self);
static VALUE NIO_Selector_register(VALUE self, VALUE selectable, VALUE interest);
static VALUE NIO_Selector_deregister(VALUE self, VALUE io);
static VALUE NIO_Selector_is_registered(VALUE self, VALUE io);
static VALUE NIO_Selector_select(int argc, VALUE *argv, VALUE self);
static VALUE NIO_Selector_wakeup(VALUE self);
static VALUE NIO_Selector_close(VALUE self);
static VALUE NIO_Selector_closed(VALUE self);

/* Internal functions */
static VALUE NIO_Selector_synchronize(VALUE self, VALUE (*func)(VALUE *args), VALUE *args);
static VALUE NIO_Selector_unlock(VALUE lock);
static VALUE NIO_Selector_register_synchronized(VALUE *args);
static VALUE NIO_Selector_deregister_synchronized(VALUE *args);
static VALUE NIO_Selector_select_synchronized(VALUE *args);
static VALUE NIO_Selector_run_evloop(void *ptr);
static void NIO_Selector_timeout_callback(struct ev_loop *ev_loop, struct ev_timer *timer, int revents);
static void NIO_Selector_wakeup_callback(struct ev_loop *ev_loop, struct ev_async *async, int revents);

/* Default number of slots in the buffer for selected monitors */
#define INITIAL_READY_BUFFER 32

/* Ruby 1.8 needs us to busy wait and run the green threads scheduler every 10ms */
#define BUSYWAIT_INTERVAL 0.01

/* Selectors wait for events */
void Init_NIO_Selector()
{
    mNIO = rb_define_module("NIO");
    cNIO_Channel  = rb_define_class_under(mNIO, "Channel",  rb_cObject);
    cNIO_Monitor  = rb_define_class_under(mNIO, "Monitor",  rb_cObject);
    cNIO_Selector = rb_define_class_under(mNIO, "Selector", rb_cObject);
    rb_define_alloc_func(cNIO_Selector, NIO_Selector_allocate);

    rb_define_method(cNIO_Selector, "initialize", NIO_Selector_initialize, 0);
    rb_define_method(cNIO_Selector, "register", NIO_Selector_register, 2);
    rb_define_method(cNIO_Selector, "deregister", NIO_Selector_deregister, 1);
    rb_define_method(cNIO_Selector, "registered?", NIO_Selector_is_registered, 1);
    rb_define_method(cNIO_Selector, "select", NIO_Selector_select, -1);
    rb_define_method(cNIO_Selector, "wakeup", NIO_Selector_wakeup, 0);
    rb_define_method(cNIO_Selector, "close", NIO_Selector_close, 0);
    rb_define_method(cNIO_Selector, "closed?", NIO_Selector_closed, 0);
}

/* Create the libev event loop and incoming event buffer */
static VALUE NIO_Selector_allocate(VALUE klass)
{
    struct NIO_Selector *selector = (struct NIO_Selector *)xmalloc(sizeof(struct NIO_Selector));

    selector->ev_loop = ev_loop_new(0);
    ev_init(&selector->timer, NIO_Selector_timeout_callback);

    ev_async_init(&selector->wakeup, NIO_Selector_wakeup_callback);
    selector->wakeup.data = (void *)selector;

    ev_async_start(selector->ev_loop, &selector->wakeup);

    selector->closed = selector->selecting = selector->ready_count = 0;
    selector->ready_buffer_size = INITIAL_READY_BUFFER;
    selector->ready_buffer = (VALUE *)xmalloc(sizeof(VALUE) * INITIAL_READY_BUFFER);

    return Data_Wrap_Struct(klass, NIO_Selector_mark, NIO_Selector_free, selector);
}

/* NIO selectors store all Ruby objects in instance variables so mark is a stub */
static void NIO_Selector_mark(struct NIO_Selector *selector)
{
}

/* Free a Selector's system resources.
   Called by both NIO::Selector#close and the finalizer below */
static void NIO_Selector_shutdown(struct NIO_Selector *selector)
{
    if(selector->ev_loop) {
        ev_loop_destroy(selector->ev_loop);
        selector->ev_loop = 0;
    }

    if(selector->closed) {
        return;
    }

    selector->closed = 1;
}

/* Ruby finalizer for selector objects */
static void NIO_Selector_free(struct NIO_Selector *selector)
{
    NIO_Selector_shutdown(selector);

    xfree(selector->ready_buffer);
    xfree(selector);
}

/* Create a new selector. This is more or less the pure Ruby version
   translated into an MRI cext */
static VALUE NIO_Selector_initialize(VALUE self)
{
    VALUE lock;

    rb_ivar_set(self, rb_intern("selectables"), rb_hash_new());

    lock = rb_class_new_instance(0, 0, rb_const_get(rb_cObject, rb_intern("Mutex")));
    rb_ivar_set(self, rb_intern("lock"), lock);

    return Qnil;
}

/* Synchronize the given function with the selector mutex */
static VALUE NIO_Selector_synchronize(VALUE self, VALUE (*func)(VALUE *args), VALUE *args)
{
    VALUE lock;

    lock = rb_ivar_get(self, rb_intern("lock"));
    rb_funcall(lock, rb_intern("lock"), 0, 0);
    return rb_ensure(func, (VALUE)args, NIO_Selector_unlock, lock);
}

/* Unlock the selector mutex */
static VALUE NIO_Selector_unlock(VALUE lock)
{
    rb_funcall(lock, rb_intern("unlock"), 0, 0);
}

/* Register an IO object with the selector for the given interests */
static VALUE NIO_Selector_register(VALUE self, VALUE io, VALUE interests)
{
    VALUE args[3] = {self, io, interests};
    return NIO_Selector_synchronize(self, NIO_Selector_register_synchronized, args);
}

/* Internal implementation of register after acquiring mutex */
static VALUE NIO_Selector_register_synchronized(VALUE *args)
{
    VALUE self, io, interests, selectables, monitor;
    VALUE monitor_args[3];

    self = args[0];
    io = args[1];
    interests = args[2];

    selectables = rb_ivar_get(self, rb_intern("selectables"));
    monitor = rb_hash_lookup(selectables, io);

    if(monitor != Qnil)
        rb_raise(rb_eArgError, "this IO is already registered with selector");

    /* Create a new NIO::Monitor */
    monitor_args[0] = self;
    monitor_args[1] = io;
    monitor_args[2] = interests;

    monitor = rb_class_new_instance(3, monitor_args, cNIO_Monitor);
    rb_hash_aset(selectables, io, monitor);

    return monitor;
}

/* Deregister an IO object from the selector */
static VALUE NIO_Selector_deregister(VALUE self, VALUE io)
{
    VALUE args[2] = {self, io};
    return NIO_Selector_synchronize(self, NIO_Selector_deregister_synchronized, args);
}

/* Internal implementation of register after acquiring mutex */
static VALUE NIO_Selector_deregister_synchronized(VALUE *args)
{
    VALUE self, io, interests, selectables, monitor;
    VALUE monitor_args[3];

    self = args[0];
    io = args[1];

    selectables = rb_ivar_get(self, rb_intern("selectables"));
    monitor = rb_hash_delete(selectables, io);

    if(monitor != Qnil) {
        rb_funcall(monitor, rb_intern("deactivate"), 0, 0);
    }

    return Qnil;
}

/* Is the given IO object registered with the selector */
static VALUE NIO_Selector_is_registered(VALUE self, VALUE io)
{
    VALUE selectables = rb_ivar_get(self, rb_intern("selectables"));

    /* Perhaps this should be holding the mutex? */
    return rb_funcall(selectables, rb_intern("has_key?"), 1, io);
}

/* Select from all registered IO objects */
static VALUE NIO_Selector_select(int argc, VALUE *argv, VALUE self)
{
    VALUE timeout, array;
    VALUE args[2];

    rb_scan_args(argc, argv, "01", &timeout);

    args[0] = self;
    args[1] = timeout;

    return NIO_Selector_synchronize(self, NIO_Selector_select_synchronized, args);
}

/* Internal implementation of select with the selector lock held */
static VALUE NIO_Selector_select_synchronized(VALUE *args)
{
    VALUE self, timeout, result;
    struct NIO_Selector *selector;

    self = args[0];
    timeout = args[1];

    Data_Get_Struct(self, struct NIO_Selector, selector);
    selector->selecting = 1;

#if defined(HAVE_RB_THREAD_BLOCKING_REGION) || defined(HAVE_RB_THREAD_ALONE)
    /* Implement the optional timeout (if any) as a ev_timer */
    if(timeout != Qnil) {
        selector->timer.repeat = NUM2DBL(timeout);
        ev_timer_again(selector->ev_loop, &selector->timer);
    } else {
        ev_timer_stop(selector->ev_loop, &selector->timer);
    }
#else
    /* Store when we started the loop so we can calculate the timeout */
    ev_tstamp started_at = ev_now(selector->ev_loop);
#endif

#if defined(HAVE_RB_THREAD_BLOCKING_REGION)
    /* Ruby 1.9 lets us release the GIL and make a blocking I/O call */
    rb_thread_blocking_region(NIO_Selector_run_evloop, selector, RUBY_UBF_IO, 0);
#elif defined(HAVE_RB_THREAD_ALONE)
    /* If we're the only thread we can make a blocking system call */
    if(rb_thread_alone()) {
#else
    /* If we don't have rb_thread_alone() we can't block */
    if(0) {
#endif /* defined(HAVE_RB_THREAD_BLOCKING_REGION) */

#if !defined(HAVE_RB_THREAD_BLOCKING_REGION)
        TRAP_BEG;
        NIO_Selector_run_evloop(selector);
        TRAP_END;
    } else {
        /* We need to busy wait as not to stall the green thread scheduler
           Ruby 1.8: just say no! :( */
        ev_timer_init(&selector->timer, NIO_Selector_timeout_callback, BUSYWAIT_INTERVAL, BUSYWAIT_INTERVAL);
        ev_timer_start(selector->ev_loop, &selector->timer);

        /* Loop until we receive events */
        while(selector->selecting && !selector->ready_count) {
            TRAP_BEG;
            NIO_Selector_run_evloop(selector);
            TRAP_END;

            /* Run the next green thread */
            rb_thread_schedule();

            /* Break if the timeout has elapsed */
            if(timeout != Qnil && ev_now(selector->ev_loop) - started_at >= NUM2DBL(timeout))
                break;
        }

        ev_timer_stop(selector->ev_loop, &selector->timer);
    }
#endif /* defined(HAVE_RB_THREAD_BLOCKING_REGION) */

    result = rb_ary_new4(selector->ready_count, selector->ready_buffer);
    selector->selecting = selector->ready_count = 0;

    return result;
}

/* Run the libev event loop */
static VALUE NIO_Selector_run_evloop(void *ptr)
{
    struct NIO_Selector *selector = (struct NIO_Selector *)ptr;

    ev_loop(selector->ev_loop, EVLOOP_ONESHOT);

    return Qnil;
}

static VALUE NIO_Selector_wakeup(VALUE self)
{
    struct NIO_Selector *selector;
    Data_Get_Struct(self, struct NIO_Selector, selector);

    ev_async_send(selector->ev_loop, &selector->wakeup);

    return Qnil;
}

static VALUE NIO_Selector_close(VALUE self)
{
    struct NIO_Selector *selector;
    Data_Get_Struct(self, struct NIO_Selector, selector);

    NIO_Selector_shutdown(selector);

    return Qnil;
}

static VALUE NIO_Selector_closed(VALUE self)
{
    struct NIO_Selector *selector;
    Data_Get_Struct(self, struct NIO_Selector, selector);

    return selector->closed ? Qtrue : Qfalse;
}

/* Called whenever a timeout fires on the event loop */
static void NIO_Selector_timeout_callback(struct ev_loop *ev_loop, struct ev_timer *timer, int revents)
{
    /* We don't actually need to do anything here, the mere firing of the
       timer is sufficient to interrupt the selector. However, libev still wants a callback */
}

/* Called whenever a wakeup request is sent to a selector */
static void NIO_Selector_wakeup_callback(struct ev_loop *ev_loop, struct ev_async *async, int revents)
{
    struct NIO_Selector *selector = (struct NIO_Selector *)async->data;
    selector->selecting = 0;
}

/* This gets called from individual monitors. We must be careful here because
   the GIL isn't held, so we must rely only on standard C and can't touch
   anything Ruby-related */
void NIO_Selector_handle_event(struct NIO_Selector *selector, VALUE monitor, int revents)
{
    /* Grow the ready buffer if it's too small */
    if(selector->ready_count >= selector->ready_buffer_size) {
      selector->ready_buffer_size *= 2;
      selector->ready_buffer = (VALUE *)xrealloc(selector->ready_buffer, sizeof(VALUE) * selector->ready_buffer_size);
    }

    selector->ready_buffer[selector->ready_count] = monitor;
    selector->ready_count++;
}