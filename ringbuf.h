/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in
 *  the LICENSE file in the root directory of this source tree. An
 *  additional grant of patent rights can be found in the PATENTS file
 *  in the same directory.
 *
 */
#include <stdint.h>
#include <unistd.h>
#include <sys/uio.h>
#include "util.h"

struct ringbuf* ringbuf_new(size_t capacity);
size_t ringbuf_capacity(const struct ringbuf* rb);
size_t ringbuf_size(const struct ringbuf* rb);
size_t ringbuf_room(const struct ringbuf* rb);
size_t ringbuf_read_in(struct ringbuf* rb, int fd, size_t sz);
size_t ringbuf_write_out(const struct ringbuf* rb, int fd, size_t sz);
void ringbuf_copy_in(struct ringbuf* rb, const void* buf, size_t sz);
void ringbuf_copy_out(const struct ringbuf* rb, void* buf, size_t sz);

void ringbuf_readable_iov(const struct ringbuf* rb,
                          struct iovec iov[2],
                          size_t sz);

void ringbuf_writable_iov(const struct ringbuf* rb,
                          struct iovec iov[2],
                          size_t sz);

size_t ringbuf_note_removed(struct ringbuf* rb, size_t nr);
size_t ringbuf_note_added(struct ringbuf* rb, size_t nr);
