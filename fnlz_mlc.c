/*
 * Copyright (c) 2011 by Hewlett-Packard Company.  All rights reserved.
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program
 * for any purpose, provided the above notices are retained on all copies.
 * Permission to modify the code and to distribute modified code is granted,
 * provided the above notices are retained, and a notice that the code was
 * modified is included with the above copyright notice.
 *
 */

#include "private/gc_priv.h"

#ifdef ENABLE_DISCLAIM

#include "gc/gc_disclaim.h"
#include "private/dbg_mlc.h" /* for oh type */

#if defined(KEEP_BACK_PTRS) || defined(MAKE_BACK_GRAPH)
  /* The first bit is already used for a debug purpose. */
# define FINALIZER_CLOSURE_FLAG 0x2
# define HAS_BEEN_FINALIZED_FLAG 0x3
#else
# define FINALIZER_CLOSURE_FLAG 0x1
# define HAS_BEEN_FINALIZED_FLAG 0x2
#endif

STATIC int GC_CALLBACK GC_finalized_disclaim(void *obj)
{
#   ifdef AO_HAVE_load
        word fc_word = (word)AO_load((volatile AO_t *)obj);
#   else
        word fc_word = *(word *)obj;
#   endif

    if ((fc_word & FINALIZER_CLOSURE_FLAG) != 0) {
       /* The disclaim function may be passed fragments from the        */
       /* free-list, on which it should not run finalization.           */
       /* To recognize this case, we use the fact that the first word   */
       /* on such fragments is always multiple of 4 (a link to the next */
       /* fragment, or NULL).  If it is desirable to have a finalizer   */
       /* which does not use the first word for storing finalization    */
       /* info, GC_disclaim_and_reclaim() must be extended to clear     */
       /* fragments so that the assumption holds for the selected word. */
        const struct GC_finalizer_closure *fc
                        = (struct GC_finalizer_closure *)(fc_word
                                        & ~(word)FINALIZER_CLOSURE_FLAG);
        GC_ASSERT(!GC_find_leak);
        (*fc->proc)((word *)obj + 1, fc->cd);
    }
#ifdef DISCLAIM_MARK_CHILDREN
    /* Prevent the objects reachable from the disclaimed object from    */
    /* being reclaimed immediately by marking them as live. Useful if   */
    /* the disclaim function queues objects to be finalised later.      */
    return 1;
#else
    return 0;
#endif
}

STATIC void GC_register_disclaim_proc_inner(unsigned kind,
                                            GC_disclaim_proc proc,
                                            GC_bool mark_unconditionally)
{
    GC_ASSERT(kind < MAXOBJKINDS);
    if (EXPECT(GC_find_leak, FALSE)) return;

    GC_obj_kinds[kind].ok_disclaim_proc = proc;
    GC_obj_kinds[kind].ok_mark_unconditionally = mark_unconditionally;
}

GC_API void GC_CALL GC_init_finalized_malloc(void)
{
    GC_init();  /* In case it's not already done.       */
    LOCK();
    if (GC_finalized_kind != 0) {
        UNLOCK();
        return;
    }

    /* The finalizer closure is placed in the first word in order to    */
    /* use the lower bits to distinguish live objects from objects on   */
    /* the free list.  The downside of this is that we need one-word    */
    /* offset interior pointers, and that GC_base does not return the   */
    /* start of the user region.                                        */
    GC_register_displacement_inner(sizeof(word));

    /* And, the pointer to the finalizer closure object itself is       */
    /* displaced due to baking in this indicator.                       */
    GC_register_displacement_inner(FINALIZER_CLOSURE_FLAG);
    GC_register_displacement_inner(sizeof(oh) + FINALIZER_CLOSURE_FLAG);

    GC_finalized_kind = GC_new_kind_inner(GC_new_free_list_inner(),
                                          GC_DS_LENGTH, TRUE, TRUE);
    GC_ASSERT(GC_finalized_kind != 0);
    GC_register_disclaim_proc_inner(GC_finalized_kind, GC_finalized_disclaim,
                                    TRUE);
    UNLOCK();
}

GC_API void GC_CALL GC_register_disclaim_proc(int kind, GC_disclaim_proc proc,
                                              int mark_unconditionally)
{
    LOCK();
    GC_register_disclaim_proc_inner((unsigned)kind, proc,
                                    (GC_bool)mark_unconditionally);
    UNLOCK();
}

GC_API GC_ATTR_MALLOC void * GC_CALL GC_finalized_malloc(size_t lb,
                                const struct GC_finalizer_closure *fclos)
{
    void *op;

    GC_ASSERT(GC_finalized_kind != 0);
    GC_ASSERT(NONNULL_ARG_NOT_NULL(fclos));
    GC_ASSERT(((word)fclos & FINALIZER_CLOSURE_FLAG) == 0);
    op = GC_malloc_kind(SIZET_SAT_ADD(lb, sizeof(word)),
                        (int)GC_finalized_kind);
    if (EXPECT(NULL == op, FALSE))
        return NULL;
#   ifdef AO_HAVE_store
        AO_store((volatile AO_t *)op, (AO_t)fclos | FINALIZER_CLOSURE_FLAG);
#   else
        *(word *)op = (word)fclos | FINALIZER_CLOSURE_FLAG;
#   endif
    GC_dirty(op);
    REACHABLE_AFTER_DIRTY(fclos);
    return (word *)op + 1;
}

# ifdef BUFFERED_FINALIZATION

static void* init_finalize_thread(void *arg)
{
    while (1) {
        GC_finalize_objects();
    }
    return arg;
}

GC_finalization_buffer_hdr* GC_new_buffer() {
    GC_ASSERT(I_HOLD_LOCK());
    void* ptr = GC_os_get_mem(GC_page_size);
    if (NULL == ptr)
      ABORT("Insufficient memory for finalization buffer.");
    GC_add_roots_inner(ptr, ptr + GC_page_size, FALSE);
    return ptr;
}

void GC_delete_buffer(GC_finalization_buffer_hdr* buffer) {
    GC_remove_roots((void*) buffer, (void*) buffer + GC_page_size);
    GC_unmap((void*)buffer, GC_page_size);

}

STATIC int GC_CALLBACK GC_push_object_to_fin_buffer(void *obj)
{
    GC_ASSERT(I_HOLD_LOCK());

    word finalizer_word = *(word *)obj;
    if ((finalizer_word & FINALIZER_CLOSURE_FLAG) == 0) {
        return 0;
    }

    if (finalizer_word & HAS_BEEN_FINALIZED_FLAG) {
        /* The object has already been finalized. Return 0 ensures it is
         * immediately reclaimed.
         */
        return 0;
    }

    if (GC_finalizer_buffer_head == NULL) {
        /* This can happen for two reasons:
         *   1) This is first time a finalizable object is unreachable and no
         *   finalization buffers have been created yet.
         *
         *   2) The buffer(s) have already passed to a finalization thread
         *   which is processing them. We must start again. */
        GC_finalizer_buffer_head = GC_new_buffer();
        GC_finalizer_buffer_current.hdr = GC_finalizer_buffer_head;
        GC_finalizer_buffer_current.cursor = (void**) (GC_finalizer_buffer_head + 1);
    }

    void** last = (void**) ((void *)GC_finalizer_buffer_current.hdr + GC_page_size);
    if (GC_finalizer_buffer_current.cursor == last) {
        GC_finalization_buffer_hdr* next = GC_new_buffer();
        GC_finalizer_buffer_current.hdr->next = next;
        GC_finalizer_buffer_current.hdr = next;
        GC_finalizer_buffer_current.cursor = (void**) (next + 1);
    }

    *GC_finalizer_buffer_current.cursor = obj;
    GC_finalizer_buffer_current.cursor++;

    return 1;
}


GC_API GC_ATTR_MALLOC void * GC_CALL GC_buffered_finalize_malloc(size_t lb)
{
    void *op;

    GC_ASSERT(GC_fin_q_kind != 0);
    op = GC_malloc_kind(lb, (int)GC_fin_q_kind);
    if (EXPECT(NULL == op, FALSE))
        return NULL;
    return (word *)op;
}

GC_API GC_ATTR_MALLOC void * GC_CALL GC_buffered_finalize_memalign(size_t align, size_t lb)
{
    size_t offset;
    ptr_t result;
    size_t align_m1 = align - 1;

    /* Check the alignment argument.    */
    if (EXPECT(0 == align || (align & align_m1) != 0, FALSE)) return NULL;
    if (align <= GC_GRANULE_BYTES) return GC_buffered_finalize_malloc(lb);

    if (align >= HBLKSIZE/2 || lb >= HBLKSIZE/2) {
      return GC_clear_stack(GC_generic_malloc_aligned(lb, GC_fin_q_kind,
                                        0 /* flags */, align_m1));
    }

    result = (ptr_t)GC_buffered_finalize_malloc(SIZET_SAT_ADD(lb, align_m1));
    offset = (size_t)(word)result & align_m1;
    if (offset != 0) {
        offset = align - offset;
        if (!GC_all_interior_pointers) {
            GC_STATIC_ASSERT(VALID_OFFSET_SZ <= HBLKSIZE);
            GC_ASSERT(offset < VALID_OFFSET_SZ);
            GC_register_displacement(offset);
        }
        result += offset;
    }
    GC_ASSERT(((word)result & align_m1) == 0);
    return result;
}

GC_API int GC_CALL GC_buffered_finalize_posix_memalign(void **memptr, size_t align, size_t lb)
{
  size_t align_minus_one = align - 1; /* to workaround a cppcheck warning */

  /* Check alignment properly.  */
  if (EXPECT(align < sizeof(void *)
             || (align_minus_one & align) != 0, FALSE)) {
      return EINVAL;
  }

  if ((*memptr = GC_buffered_finalize_memalign(align, lb)) == NULL) {
      return ENOMEM;
  }
  return 0;
}


GC_API void GC_CALL GC_init_buffered_finalization(void)
{
    LOCK();
    GC_new_buffer();
    GC_fin_q_kind = GC_new_kind_inner(GC_new_free_list_inner(),
                                          GC_DS_LENGTH, TRUE, TRUE);
    GC_ASSERT(GC_fin_q_kind != 0);

    GC_register_disclaim_proc_inner(GC_fin_q_kind, GC_push_object_to_fin_buffer, TRUE);
    UNLOCK();
}

void GC_finalize_buffer(GC_finalization_buffer_hdr* buffer) {
    void** cursor = (void**) (buffer + 1);
    void** last = (void**) ((void *)buffer + GC_page_size);
    while (cursor != last)
    {
        if (*cursor == NULL) {
            break;
        }
        void* obj = *cursor;
        word finalizer_word = (*(word *)obj) & ~(word)FINALIZER_CLOSURE_FLAG;
        GC_disclaim_proc finalizer = (GC_disclaim_proc) finalizer_word;
        (finalizer)(obj);
        /* Prevent the object from being re-added to the finalization queue */
        *(word *)obj = finalizer_word | HAS_BEEN_FINALIZED_FLAG;
        cursor++;
    }
}

GC_API void GC_CALL GC_finalize_objects(void) {
    /* This is safe to do without locking because this global is only ever
     * mutated from within a collection where all mutator threads (including
     * this finalisation thread) are paused. It is not possible for them to race.
     *
     * In addition, a memory barrier synchronises threads at the end of a
     * collection, so the finalisation thread will always load the up-to-date
     * version of this global. */
    GC_disable();
    GC_finalization_buffer_hdr* buffer = GC_finalizer_buffer_head;
    GC_finalizer_buffer_head = NULL;
    GC_enable();

    while(buffer != NULL)
    {
        GC_finalize_buffer(buffer);
        GC_finalization_buffer_hdr* next = buffer->next;

        GC_delete_buffer(buffer);
        buffer = next;
    }
}

GC_INNER void GC_maybe_spawn_finalize_thread()
{
    if (GC_finalizer_thread_exists || !GC_finalizer_buffer_head)
        return;

    pthread_t t;
    pthread_create(&t, NULL, init_finalize_thread, NULL /* arg */);
    GC_finalizer_thread_exists = 1;
}

# endif

#endif /* ENABLE_DISCLAIM */
