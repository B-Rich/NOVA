/*
 * Semaphore
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012 Udo Steinberg, Intel Corporation.
 *
 * This file is part of the NOVA microhypervisor.
 *
 * NOVA is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * NOVA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License version 2 for more details.
 */

#pragma once

#include "ec.hpp"

class Sm : public Kobject, public Queue<Ec>
{
    private:
        mword counter;

        static Slab_cache cache;

        static void free (Rcu_elem * a) {
            Sm * sm = static_cast <Sm *>(a);

            while(!sm->counter)
                sm->up(true);

            delete sm;
        }

    public:
        Sm (Pd *, mword, mword = 0);
        ~Sm ()
        {
            while(!counter) up();
        }

        ALWAYS_INLINE
        inline void dn (bool zero)
        {
            Ec *e = Ec::current;

            {   Lock_guard <Spinlock> guard (lock);

                if (counter) {
                    counter = zero ? 0 : counter - 1;
                    return;
                }

                e->add_ref();

                enqueue (e);
            }

            e->block_sc();
        }

        ALWAYS_INLINE
        inline void up(bool abort = false)
        {
            Ec *e;

            {   Lock_guard <Spinlock> guard (lock);

                loop:

                if (!(e = dequeue())) {
                    counter++;
                    return;
                }

                if (EXPECT_FALSE(e->del_ref())) {
                    delete e;
                    goto loop;
                }
            }

            e->release(abort);
        }

        ALWAYS_INLINE
        static inline void *operator new (size_t) { return cache.alloc(); }

        ALWAYS_INLINE
        static inline void operator delete (void *ptr) { cache.free (ptr); }
};
