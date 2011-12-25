/*
 * Copyright (c) 2011 Tony Arcieri. Distributed under the MIT License. See
 * LICENSE.txt for further details.
 */

#include "nio4r.h"
#include <assert.h>

static VALUE mNIO = Qnil;
static VALUE cNIO_Monitor = Qnil;

/* Allocator/deallocator */
static VALUE NIO_Monitor_allocate(VALUE klass);
static void NIO_Monitor_mark(struct NIO_Monitor *monitor);
static void NIO_Monitor_free(struct NIO_Monitor *monitor);

/* Methods */
static VALUE NIO_Monitor_initialize(VALUE self, VALUE selector, VALUE io, VALUE interests);
static VALUE NIO_Monitor_deactivate(VALUE self);
static VALUE NIO_Monitor_io(VALUE self);
static VALUE NIO_Monitor_interests(VALUE self);
static VALUE NIO_Monitor_value(VALUE self);
static VALUE NIO_Monitor_set_value(VALUE self, VALUE obj);

/* Internal functions */
static void NIO_Monitor_callback(struct ev_loop *ev_loop, struct ev_io *io, int revents);

#if HAVE_RB_IO_T
  rb_io_t *fptr;
#else
  OpenFile *fptr;
#endif

/* Monitor control how a channel is being waited for by a monitor */
void Init_NIO_Monitor()
{
    mNIO = rb_define_module("NIO");
    cNIO_Monitor = rb_define_class_under(mNIO, "Monitor", rb_cObject);
    rb_define_alloc_func(cNIO_Monitor, NIO_Monitor_allocate);

    rb_define_method(cNIO_Monitor, "initialize", NIO_Monitor_initialize, 3);
    rb_define_method(cNIO_Monitor, "deactivate", NIO_Monitor_io, 0);
    rb_define_method(cNIO_Monitor, "io", NIO_Monitor_io, 0);
    rb_define_method(cNIO_Monitor, "interests", NIO_Monitor_interests, 0);
    rb_define_method(cNIO_Monitor, "value", NIO_Monitor_value, 0);
    rb_define_method(cNIO_Monitor, "value=", NIO_Monitor_set_value, 1);
}

static VALUE NIO_Monitor_allocate(VALUE klass)
{
    struct NIO_Monitor *monitor = (struct NIO_Monitor *)xmalloc(sizeof(struct NIO_Monitor));

    return Data_Wrap_Struct(klass, NIO_Monitor_mark, NIO_Monitor_free, monitor);
}

static void NIO_Monitor_mark(struct NIO_Monitor *monitor)
{
}

static void NIO_Monitor_free(struct NIO_Monitor *monitor)
{
    xfree(monitor);
}

static VALUE NIO_Monitor_initialize(VALUE self, VALUE selector_obj, VALUE io, VALUE interests)
{
    struct NIO_Monitor *monitor;
    struct NIO_Selector *selector;
    int events;
    ID interests_id;

    #if HAVE_RB_IO_T
        rb_io_t *fptr;
    #else
        OpenFile *fptr;
    #endif

    interests_id = SYM2ID(interests);

    if(interests_id == rb_intern("r")) {
        events = EV_READ;
    } else if(interests_id == rb_intern("w")) {
        events = EV_WRITE;
    } else if(interests_id == rb_intern("rw")) {
        events = EV_READ | EV_WRITE;
    } else {
        rb_raise(rb_eArgError, "invalid event type %s (must be :r, :w, or :rw)",
            RSTRING_PTR(rb_funcall(interests, rb_intern("inspect"), 0, 0)));
    }

    Data_Get_Struct(self, struct NIO_Monitor, monitor);

    GetOpenFile(rb_convert_type(io, T_FILE, "IO", "to_io"), fptr);
    ev_io_init(&monitor->ev_io, NIO_Monitor_callback, FPTR_TO_FD(fptr), events);

    rb_ivar_set(self, rb_intern("selector"), selector_obj);
    rb_ivar_set(self, rb_intern("io"), io);
    rb_ivar_set(self, rb_intern("interests"), interests);

    Data_Get_Struct(selector_obj, struct NIO_Selector, selector);

    monitor->self = self;
    monitor->ev_io.data = (void *)monitor;

    /* We can safely hang onto this as we also hang onto a reference to the
       object where it originally came from */
    monitor->selector = selector;

    ev_io_start(selector->ev_loop, &monitor->ev_io);

    return Qnil;
}

static VALUE NIO_Monitor_deactivate(VALUE self)
{
    struct NIO_Monitor *monitor;

    Data_Get_Struct(self, struct NIO_Monitor, monitor);

    if(monitor->selector) {
        ev_io_stop(monitor->selector->ev_loop, &monitor->ev_io);
        monitor->selector = 0;
    }

    return Qnil;
}

static VALUE NIO_Monitor_io(VALUE self)
{
    return rb_ivar_get(self, rb_intern("io"));
}

static VALUE NIO_Monitor_interests(VALUE self)
{
    return rb_ivar_get(self, rb_intern("interests"));
}

static VALUE NIO_Monitor_value(VALUE self)
{
    return rb_ivar_get(self, rb_intern("value"));
}

static VALUE NIO_Monitor_set_value(VALUE self, VALUE obj)
{
    return rb_ivar_set(self, rb_intern("value"), obj);
}

/* libev callback fired whenever this monitor gets events */
static void NIO_Monitor_callback(struct ev_loop *ev_loop, struct ev_io *io, int revents)
{
    struct NIO_Monitor *monitor = (struct NIO_Monitor *)io->data;

    assert(monitor->selector != 0);
    NIO_Selector_handle_event(monitor->selector, monitor->self, revents);
}