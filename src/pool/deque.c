#include "abti.h"
#include <stdatomic.h>

// Deque pool implementation based on the .NET Framework's WorkStealingQueue
// (https://referencesource.microsoft.com/#mscorlib/system/threading/threadpool.cs,c6809900d25746e6).

typedef struct data {
    // Pointer to array itself, array length and mask do not need to be _Atomic
    // because they are modified only in between mutex_spinlock/unlock.
    ABTI_unit *_Atomic *unit_array;
    size_t array_length;
    size_t mask;
    size_t _Atomic head_idx;
    size_t _Atomic tail_idx;
    ABTI_mutex foreign_lock;
} data_t;

static size_t const INITIAL_LENGTH = 256;

/* Pool functions */

static int deque_init(ABT_pool pool, ABT_pool_config config)
{
    ABTI_UNUSED(config);
    int abt_errno = ABT_SUCCESS;

    data_t *p_data = (data_t *)ABTU_malloc(sizeof(data_t));

    p_data->unit_array = ABTU_malloc(INITIAL_LENGTH * sizeof(ABTI_unit *));
    p_data->array_length = INITIAL_LENGTH;
    p_data->mask = INITIAL_LENGTH - 1;
    p_data->head_idx = 0;
    p_data->tail_idx = 0;
    ABTI_mutex_init(&p_data->foreign_lock);

    ABT_pool_set_data(pool, p_data);

    return abt_errno;
}

static int deque_free(ABT_pool pool)
{
    int abt_errno = ABT_SUCCESS;
    data_t *p_data;
    ABT_pool_get_data(pool, &p_data);

    ABTU_free(p_data);

    return abt_errno;
}

static size_t deque_get_size(ABTI_pool *self)
{
    data_t *m = self->data;
    // cheap but inaccurate calculation
    return m->tail_idx - m->head_idx;
}

static void deque_push(ABTI_pool *self, ABTI_unit *unit)
{
    data_t *m = self->data;

    size_t tail = m->tail_idx;

    // We're going to increment the tail; if we'll overflow, then we need to reset our counts.
    if (tail == SIZE_MAX) {
        ABTI_mutex_spinlock(&m->foreign_lock);
        if (m->tail_idx == SIZE_MAX) {
            m->head_idx = m->head_idx & m->mask;
            m->tail_idx = tail = m->tail_idx & m->mask;
        }
        ABTI_mutex_unlock(&m->foreign_lock);
    }

    // When there are at least 2 elements' worth of space, we can take the fast path.
    if (tail < m->head_idx + m->mask) {
        m->unit_array[tail & m->mask] = unit;
        m->tail_idx = tail + 1;
    } else {
        // We need to contend with foreign pops, so we lock.
        ABTI_mutex_spinlock(&m->foreign_lock);

        size_t head = m->head_idx;
        size_t count = m->tail_idx - m->head_idx;

        // If there is still space (one left), just add the element.
        if (count >= m->mask) {
            // We're full; expand the queue by doubling its size.
            ABTI_unit *_Atomic *new_array = ABTU_malloc((m->array_length << 1) * sizeof(ABTI_unit *));
            for (int i = 0; i < m->array_length; i++) {
                new_array[i] = m->unit_array[(i + head) & m->mask];
            }

            // Reset the field values, incl. the mask.
            m->unit_array = new_array;
            m->head_idx = 0;
            m->tail_idx = tail = count;
            m->mask = (m->mask << 1) | 1;
        }

        m->unit_array[tail & m->mask] = unit;
        m->tail_idx + tail + 1;

        ABTI_mutex_unlock(&m->foreign_lock);
    }
}

static ABT_unit deque_pop_local(ABTI_pool *self)
{
    data_t *m = self->data;

    while (1) {
        // Decrement the tail using a fence to ensure subsequent read doesn't come before.
        size_t tail = m->tail_idx;
        if (m->head_idx >= tail) {
            return ABT_UNIT_NULL;
        }

        tail -= 1;
        atomic_exchange(&m->tail_idx, tail);

        // If there is no interaction with a take, we can head down the fast path.
        if (m->head_idx <= tail) {
            size_t idx = tail & m->mask;
            ABTI_unit *unit = m->unit_array[idx];

            // Check for nulls in the array.
            if (unit == ABT_UNIT_NULL) {
                continue;
            }

            m->unit_array[idx] = ABT_UNIT_NULL;
            return unit;
        } else {
            // Interaction with takes: 0 or 1 elements left.
            ABTI_mutex_spinlock(&m->foreign_lock);

            if (m->head_idx <= tail) {
                // Element still available. Take it.
                size_t idx = tail & m->mask;
                ABTI_unit *unit = m->unit_array[idx];

                // Check for nulls in the array.
                if (unit == ABT_UNIT_NULL) {
                    continue;
                }

                m->unit_array[idx] = ABT_UNIT_NULL;
                ABTI_mutex_unlock(&m->foreign_lock);
                return unit;
            } else {
                // Element was stolen, restore the tail.
                m->tail_idx = tail + 1;
                ABTI_mutex_unlock(&m->foreign_lock);
                return ABT_UNIT_NULL;
            }
        }
    }
}

// called from sched_randws directly
ABT_unit deque_pop_steal(ABTI_pool *self)
{
    data_t *m = self->data;

    while (1) {
        if (m->head_idx >= m->tail_idx) {
            return ABT_UNIT_NULL;
        }

        ABTI_mutex_spinlock(&m->foreign_lock);

        // Increment head, and ensure read of tail doesn't move before it (fence).
        size_t head = m->head_idx;
        atomic_exchange(&m->head_idx, head + 1);

        if (head < m->tail_idx) {
            size_t idx = head & m->mask;
            ABTI_unit *unit = m->unit_array[idx];

            // Check for nulls in the array.
            if (unit == ABT_UNIT_NULL) {
                continue;
            }

            m->unit_array[idx] = ABT_UNIT_NULL;
            ABTI_mutex_unlock(&m->foreign_lock);
            return unit;
        } else {
            // Failed, restore head.
            m->head_idx = head;
            ABTI_mutex_unlock(&m->foreign_lock);
            return ABT_UNIT_NULL;
        }
    }
}

static int deque_remove(ABTI_pool *self, ABTI_unit *unit)
{
    data_t *m = self->data;

    // Fast path: check the tail. If equal, we can skip the lock.
    if (m->unit_array[(m->tail_idx - 1) & m->mask] == unit) {
        return deque_pop_local(self) != ABT_UNIT_NULL ? ABT_SUCCESS : ABT_ERR_POOL;
    }

    // Else, do an O(N) search for the work item. The theory of work stealing and our
    // inlining logic is that most waits will happen on recently queued work.  And
    // since recently queued work will be close to the tail end (which is where we
    // begin our search), we will likely find it quickly.
    for (size_t i = m->tail_idx - 2; i >= m->head_idx; i--) {
        if (m->unit_array[i & m->mask] == unit) {
            // If we found the element, block out steals to avoid interference.
            ABTI_mutex_spinlock(&m->foreign_lock);

            if (m->unit_array[i & m->mask] == ABT_UNIT_NULL) {
                ABTI_mutex_unlock(&m->foreign_lock);
                return ABT_ERR_POOL;
            }

            // Null out the element.
            m->unit_array[i & m->mask] = ABT_UNIT_NULL;

            // And then check to see if we can fix up the indexes (if we're at
            // the edge).  If we can't, we just leave nulls in the array and they'll
            // get filtered out eventually (but may lead to superflous resizing).
            if (i == m->tail_idx) {
                m->tail_idx -= 1;
            } else if (i == m->head_idx) {
                m->head_idx += 1;
            }

            ABTI_mutex_unlock(&m->foreign_lock);
            return ABT_SUCCESS;
        }
    }

    return ABT_ERR_POOL;
}

/* Unit functions */

typedef ABTI_unit unit_t;

static ABT_unit_type unit_get_type(ABT_unit unit)
{
   unit_t *p_unit = (unit_t *)unit;
   return p_unit->type;
}

static ABT_thread unit_get_thread(ABT_unit unit)
{
    ABT_thread h_thread;
    unit_t *p_unit = (unit_t *)unit;
    if (p_unit->type == ABT_UNIT_TYPE_THREAD) {
        h_thread = p_unit->thread;
    } else {
        h_thread = ABT_THREAD_NULL;
    }
    return h_thread;
}

static ABT_task unit_get_task(ABT_unit unit)
{
    ABT_task h_task;
    unit_t *p_unit = (unit_t *)unit;
    if (p_unit->type == ABT_UNIT_TYPE_TASK) {
        h_task = p_unit->task;
    } else {
        h_task = ABT_TASK_NULL;
    }
    return h_task;
}

static ABT_bool unit_is_in_pool(ABT_unit unit)
{
    unit_t *p_unit = (unit_t *)unit;
    return (p_unit->pool != ABT_POOL_NULL) ? ABT_TRUE : ABT_FALSE;
}

static ABT_unit unit_create_from_thread(ABT_thread thread)
{
    ABTI_thread *p_thread = ABTI_thread_get_ptr(thread);
    unit_t *p_unit = &p_thread->unit_def;
    p_unit->p_prev = NULL;
    p_unit->p_next = NULL;
    p_unit->pool   = ABT_POOL_NULL;
    p_unit->thread = thread;
    p_unit->type   = ABT_UNIT_TYPE_THREAD;

    return (ABT_unit)p_unit;
}

static ABT_unit unit_create_from_task(ABT_task task)
{
    ABTI_task *p_task = ABTI_task_get_ptr(task);
    unit_t *p_unit = &p_task->unit_def;
    p_unit->p_prev = NULL;
    p_unit->p_next = NULL;
    p_unit->pool   = ABT_POOL_NULL;
    p_unit->task   = task;
    p_unit->type   = ABT_UNIT_TYPE_TASK;

    return (ABT_unit)p_unit;
}

static void unit_free(ABT_unit *unit)
{
    *unit = ABT_UNIT_NULL;
}

/* Deque pool definition */
ABT_pool_def ABTI_pool_deque = {
    .access               = ABT_POOL_ACCESS_SPMC,
    .p_init               = deque_init,
    .p_free               = deque_free,
    .p_get_size           = deque_get_size,
    .p_push               = deque_push,
    .p_pop                = deque_pop_local,
    .p_remove             = deque_remove,
    .u_get_type           = unit_get_type,
    .u_get_thread         = unit_get_thread,
    .u_get_task           = unit_get_task,
    .u_is_in_pool         = unit_is_in_pool,
    .u_create_from_thread = unit_create_from_thread,
    .u_create_from_task   = unit_create_from_task,
    .u_free               = unit_free,
};
