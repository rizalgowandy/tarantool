#pragma once
/*
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <small/small.h>
#include <trivia/util.h>

#include "memtx_engine.h"
#include "system_allocator.h"
#include "tuple.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

#define noop_one_arg(a)
#define noop_two_arg(a, b)	

struct allocator_stats {
	size_t used;
	size_t total;
};

#define MEM_CHECK_FUNC(prefix, func, param)					\
static inline void								\
prefix##_mem_check(MAYBE_UNUSED struct prefix##_alloc *alloc)			\
{										\
	func(alloc->param);							\
}
MEM_CHECK_FUNC(small, slab_cache_check, cache)
MEM_CHECK_FUNC(system, noop_one_arg, noop)

/**
 * Global abstract method to memory alloc
 */
typedef void *(*global_alloc)(void *alloc, unsigned bytes);		
static global_alloc memtx_global_alloc;

/**
 * Global abstract method to memory free
 */
typedef void (*global_free)(void *alloc, void *ptr, unsigned bytes);		
static global_free memtx_global_free;

/**
 * Global abstract method to delayed memory free
 */
typedef void (*global_free_delayed)(void *alloc, void *ptr, unsigned bytes);		
static global_free_delayed memtx_global_free_delayed;

#define DECLARE_MEMTX_ALLOCATOR_DESTROY(prefix)					\
static inline void								\
prefix##_allocator_destroy(struct memtx_engine *memtx)  			\
{										\
	prefix##_alloc_destroy(memtx->alloc);					\
}
DECLARE_MEMTX_ALLOCATOR_DESTROY(small)
DECLARE_MEMTX_ALLOCATOR_DESTROY(system)
#if 0
#define DECLARE_MEMTX_ALLOCATOR_CREATE(prefix, SLAB_CACHE_CREATE, param)	\
static inline void								\
prefix##_allocator_create(struct memtx_engine *memtx, uint32_t objsize_min,	\
			float alloc_factor)					\
{										\	
	prefix##_alloc_create(memtx->alloc, &memtx->param, objsize_min,		\
		alloc_factor);							\
}
DECLARE_MEMTX_ALLOCATOR_CREATE(small)
DECLARE_MEMTX_ALLOCATOR_CREATE(system)
#endif
#define DECLARE_MEMTX_ALLOCATOR_ENTER_DELAYED_FREE_MODE(prefix, PREFIX)		\
static inline void								\
prefix##_allocator_enter_delayed_free_mode(struct memtx_engine *memtx)	  	\
{										\
	return prefix##_##alloc_setopt(memtx->alloc,				\
		PREFIX##_##DELAYED_FREE_MODE, true);				\
}
DECLARE_MEMTX_ALLOCATOR_ENTER_DELAYED_FREE_MODE(small, SMALL)
DECLARE_MEMTX_ALLOCATOR_ENTER_DELAYED_FREE_MODE(system, SYSTEM)

#define DECLARE_MEMTX_ALLOCATOR_LEAVE_DELAYED_FREE_MODE(prefix, PREFIX)		\
static inline void								\
prefix##_allocator_leave_delayed_free_mode(struct memtx_engine *memtx) 	 	\
{										\
	return prefix##_##alloc_setopt(memtx->alloc,				\
		PREFIX##_##DELAYED_FREE_MODE, false);				\
}
DECLARE_MEMTX_ALLOCATOR_LEAVE_DELAYED_FREE_MODE(small, SMALL)
DECLARE_MEMTX_ALLOCATOR_LEAVE_DELAYED_FREE_MODE(system, SYSTEM)
#if 0
#define DECLARE_MEMTX_ALLOCATOR_STATS(prefix)					\
static inline __attribute__((always_inline)) void				\
prefix##_allocator_stats(struct memtx_engine *memtx,				\
		struct allocator_stats *stats,					\
		void *stats_cb, void *cb_ctx)					\
{										\
	struct prefix##_stats data_stats;					\
	prefix##_stats(memtx->alloc, &data_stats, stats_cb, cb_ctx);		\
	stats->used = data_stats.used;						\
	stats->total = data_stats.total;					\
}
DECLARE_MEMTX_ALLOCATOR_STATS(small)
DECLARE_MEMTX_ALLOCATOR_STATS(system)
#endif
#define DECLARE_MEMTX_MEM_CHECK(prefix)	 					\
static inline void								\
prefix##_##allocator_mem_check(struct memtx_engine *memtx)			\
{										\
	prefix##_mem_check((struct prefix##_alloc *)(memtx->alloc));		\
}
DECLARE_MEMTX_MEM_CHECK(small)
DECLARE_MEMTX_MEM_CHECK(system)

#define DECLARE_MEMTX_ALLOCATOR_CHOICE(prefix, alloc_func, free_func, 		\
			free_dealyed_func)					\
static inline void								\
prefix##_memtx_allocator_choice(struct memtx_engine *memtx)			\
{										\
	memtx_global_alloc = (void *)alloc_func;				\
	memtx_global_free = (void *)free_func;					\
	memtx_global_free_delayed = (void *)free_dealyed_func;  		\
	memtx->alloc = &memtx->prefix##_alloc;  				\
	memtx->memtx_allocator_create = prefix##_allocator_create;		\
	memtx->memtx_allocator_destroy = prefix##_allocator_destroy;		\
	memtx->memtx_enter_delayed_free_mode =					\
		prefix##_allocator_enter_delayed_free_mode;			\
	memtx->memtx_leave_delayed_free_mode =					\
		prefix##_allocator_leave_delayed_free_mode;			\
	memtx->memtx_allocator_stats = prefix##_allocator_stats;		\
	memtx->memtx_mem_check = prefix##_allocator_mem_check;			\
}
DECLARE_MEMTX_ALLOCATOR_CHOICE(small, smalloc, smfree, smfree_delayed)
DECLARE_MEMTX_ALLOCATOR_CHOICE(system, sysalloc, sysfree, sysfree_delayed)

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */
