#ifndef TICKET_LOCK_H_INCLUDED
#define TICKET_LOCK_H_INCLUDED

struct ABTI_ticket_lock {
    uint32_t next_ticket;
    uint32_t now_serving;
};

static inline void ABTI_ticket_lock_create(ABTI_ticket_lock *p_lock)
{
    p_lock->next_ticket = 0;
    p_lock->now_serving = 0;
}

static inline void ABTI_ticket_lock_free(ABTI_ticket_lock *p_lock)
{
    ABTI_UNUSED(p_lock);
}

static inline void ABTI_ticket_lock_acquire(ABTI_ticket_lock *p_lock)
{
    uint32_t my_ticket = __atomic_fetch_add(&p_lock->next_ticket, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&p_lock->now_serving, __ATOMIC_ACQUIRE) != my_ticket) ;
}

static inline void ABTI_ticket_lock_release(ABTI_ticket_lock *p_lock)
{
    __atomic_fetch_add(&p_lock->now_serving, 1, __ATOMIC_RELEASE);
}

#endif /* TICKET_LOCK_H_INCLUDED */