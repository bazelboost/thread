#ifndef BOOST_READ_WRITE_MUTEX_WIN32_HPP
#define BOOST_READ_WRITE_MUTEX_WIN32_HPP

//  read_write_mutex_win32.hpp
//
//  (C) Copyright 2005 Anthony Williams 
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#include <boost/thread/once.hpp>
#include <boost/detail/interlocked.hpp>
#include <boost/thread/detail/win32_thread_primitives.hpp>
#include <boost/thread/detail/lightweight_mutex_win32.hpp>
#include <boost/thread/detail/read_write_scheduling_policy.hpp>
#include <boost/thread/detail/read_write_lock_state.hpp>
#include <boost/thread/detail/interlocked_read_win32.hpp>

namespace boost
{
#define BOOST_RW_MUTEX_INIT {BOOST_ONCE_INIT}

    class read_write_mutex_static
    {
        enum states{
            unlocked_state=0,
            reading_state,
            writing_state,
            upgrading_state,
            reading_with_upgradable_state,
            acquiring_reading_state,
            releasing_reading_state,
            releasing_reading_with_upgradable_state
        };
        
    public:
        ::boost::once_flag flag;
        typedef ::boost::detail::lightweight_mutex gate_type;
        gate_type active_pending;
        gate_type state_change_gate;
        typedef gate_type::scoped_lock gate_scoped_lock;
        long mutex_state_flag;
        void* mutex_state_sem;
        long reader_count;
        long waiting_count;

    private:
        
        struct initializer
        {
            read_write_mutex_static* self;
            
            initializer(read_write_mutex_static* self_):
                self(self_)
            {}
            void operator()()
            {
                self->active_pending.initialize();
                self->state_change_gate.initialize();
                self->mutex_state_sem=BOOST_CREATE_SEMAPHORE(NULL,0,LONG_MAX,NULL);
                self->reader_count=0;
                self->mutex_state_flag=unlocked_state;
                self->waiting_count=0;
            }
        };
        
        void initialize()
        {
            boost::call_once(initializer(this),flag);
        }

        void release_mutex_state_sem()
        {
            state_change_gate.lock();
            long const count_to_unlock=::boost::detail::interlocked_read(&waiting_count);
            if(count_to_unlock)
            {
                BOOST_RELEASE_SEMAPHORE(mutex_state_sem,count_to_unlock,NULL);
            }
            else
            {
                state_change_gate.unlock();
            }
        }

        template<unsigned array_size>
        long try_enter_new_state(long const (&old_states)[array_size],long new_state)
        {
            for(unsigned i=0;i<array_size;++i)
            {
                if(BOOST_INTERLOCKED_COMPARE_EXCHANGE(&mutex_state_flag,new_state,old_states[i])==old_states[i])
                {
                    return i;
                }
            }
            return array_size;
        }
        

        template<unsigned array_size>
        long enter_new_state(long const (&old_states)[array_size],long new_state)
        {
            unsigned const old_state_index=try_enter_new_state(old_states,new_state);
            if(old_state_index<array_size)
            {
                return old_states[old_state_index];
            }
            
            while(true)
            {
                {
                    gate_scoped_lock lock(state_change_gate);
                    unsigned const old_state_index=try_enter_new_state(old_states,new_state);
                    if(old_state_index<array_size)
                    {
                        return old_states[old_state_index];
                    }
                    BOOST_INTERLOCKED_INCREMENT(&waiting_count);
                }
            
                BOOST_WAIT_FOR_SINGLE_OBJECT(mutex_state_sem,BOOST_INFINITE);
                long const remaining_waiters=BOOST_INTERLOCKED_DECREMENT(&waiting_count);
                if(!remaining_waiters)
                {
                    state_change_gate.unlock();
                }
            }
        }

        void enter_new_state(long old_state,long new_state)
        {
            long const old_states[]={
                old_state
            };
            enter_new_state(old_states,new_state);
        }

        void acquire_read_lock()
        {
            initialize();
            gate_scoped_lock lock(active_pending);

            long const old_states[]={
                unlocked_state,reading_state,reading_with_upgradable_state
            };
            
            long const previous_state=enter_new_state(old_states,acquiring_reading_state);
            BOOST_INTERLOCKED_INCREMENT(&reader_count);
            long const new_state=(previous_state==reading_with_upgradable_state)?
                reading_with_upgradable_state:reading_state;
            BOOST_INTERLOCKED_EXCHANGE(&mutex_state_flag,new_state);
            release_mutex_state_sem();
        }

        void release_read_lock()
        {
            long const old_states[]={
                reading_state,reading_with_upgradable_state
            };
            long const previous_state=enter_new_state(old_states,releasing_reading_state);
            bool const last_reader=!BOOST_INTERLOCKED_DECREMENT(&reader_count);
            long const new_state=(previous_state==reading_with_upgradable_state)?
                reading_with_upgradable_state:
                (last_reader?unlocked_state:reading_state);
            BOOST_INTERLOCKED_EXCHANGE(&mutex_state_flag,new_state);
            release_mutex_state_sem();
        }

        void acquire_write_lock()
        {
            initialize();
            gate_scoped_lock lock(active_pending);
            enter_new_state(unlocked_state,writing_state);
        }
        
        void release_write_lock()
        {
            enter_new_state(writing_state,unlocked_state);
            release_mutex_state_sem();
        }

        void demote_write_lock()
        {
            enter_new_state(writing_state,reading_with_upgradable_state);
            release_mutex_state_sem();
        }

        void acquire_upgradable_lock()
        {
            initialize();
            gate_scoped_lock lock(active_pending);
            long const old_states[]={
                unlocked_state,reading_state
            };
            enter_new_state(old_states,reading_with_upgradable_state);
            release_mutex_state_sem();
        }
        
        void release_upgradable_lock()
        {
            enter_new_state(reading_with_upgradable_state,releasing_reading_with_upgradable_state);
            long const new_state=::boost::detail::interlocked_read(&reader_count)?reading_state:unlocked_state;
            BOOST_INTERLOCKED_EXCHANGE(&mutex_state_flag,new_state);
            release_mutex_state_sem();
        }

        void upgrade_upgradable_lock()
        {
            bool acquired=false;
            while(!acquired)
            {
                enter_new_state(reading_with_upgradable_state,upgrading_state);
                acquired=!::boost::detail::interlocked_read(&reader_count);
                long const new_state=acquired?writing_state:reading_with_upgradable_state;
                BOOST_INTERLOCKED_EXCHANGE(&mutex_state_flag,new_state);
                if(!acquired)
                {
                    release_mutex_state_sem();
                }
            }
        }

    public:
        static read_write_mutex_static const dynamic_initializer()
        {
            read_write_mutex_static const res=BOOST_RW_MUTEX_INIT;
            return res;
        }

        ~read_write_mutex_static()
        {
            BOOST_CLOSE_HANDLE(mutex_state_sem);
        }

        class scoped_read_lock
        {
            read_write_mutex_static& mutex;
        public:
            scoped_read_lock(read_write_mutex_static& mutex_):
                mutex(mutex_)
            {
                mutex.acquire_read_lock();
            }
            ~scoped_read_lock()
            {
                mutex.release_read_lock();
            }
        };

        class scoped_write_lock;

        class scoped_upgradable_lock
        {
            friend class ::boost::read_write_mutex_static::scoped_write_lock;
            read_write_mutex_static& mutex;
            bool upgraded;
        public:
            scoped_upgradable_lock(read_write_mutex_static& mutex_):
                mutex(mutex_),upgraded(false)
            {
                mutex.acquire_upgradable_lock();
            }
            ~scoped_upgradable_lock()
            {
                if(!upgraded)
                {
                    mutex.release_upgradable_lock();
                }
            }
        };
    
        class scoped_write_lock
        {
            read_write_mutex_static& mutex;
        public:
            scoped_write_lock(read_write_mutex_static& mutex_):
                mutex(mutex_)
            {
                mutex.acquire_write_lock();
            }
            scoped_write_lock(scoped_upgradable_lock& upgradable):
                mutex(upgradable.mutex)
            {
                mutex.upgrade_upgradable_lock();
                upgradable.upgraded=true;
            }
            ~scoped_write_lock()
            {
                mutex.release_write_lock();
            }
        };

        class scoped_read_write_lock
        {
            read_write_mutex_static& mutex;
            ::boost::read_write_lock_state::read_write_lock_state_enum current_state;
        public:
            scoped_read_write_lock(read_write_mutex_static& mutex_,
                                   ::boost::read_write_lock_state::read_write_lock_state_enum initial_state):
                mutex(mutex_),current_state(initial_state)
            {
                if(read_locked())
                {
                    mutex.acquire_upgradable_lock();
                }
                else if(write_locked())
                {
                    mutex.acquire_write_lock();
                }
            }
            ~scoped_read_write_lock()
            {
                unlock();
            }
            ::boost::read_write_lock_state::read_write_lock_state_enum state() const
            {
                return current_state;
            }
            bool read_locked() const
            {
                return current_state==::boost::read_write_lock_state::read_locked;
            }
            bool write_locked() const
            {
                return current_state==::boost::read_write_lock_state::write_locked;
            }
            bool locked() const
            {
                return read_locked() || write_locked();
            }
            operator void const*() const
            {
                return locked()?static_cast<void const*>(this):0;
            }
            void read_lock()
            {
                mutex.acquire_upgradable_lock();
                current_state=::boost::read_write_lock_state::read_locked;
            }

            void write_lock()
            {
                mutex.acquire_write_lock();
                current_state=::boost::read_write_lock_state::write_locked;
            }
            
            void promote()
            {
                mutex.upgrade_upgradable_lock();
                current_state=::boost::read_write_lock_state::write_locked;
            }
            void demote()
            {
                mutex.demote_write_lock();
                current_state=::boost::read_write_lock_state::read_locked;
            }
            void unlock()
            {
                if(read_locked())
                {
                    mutex.release_upgradable_lock();
                }
                else if(write_locked())
                {
                    mutex.release_write_lock();
                }
                current_state=::boost::read_write_lock_state::unlocked;
            }
        };
        
    };


    class read_write_mutex:
        public read_write_mutex_static
    {
    public:
        read_write_mutex():
            read_write_mutex_static(read_write_mutex_static::dynamic_initializer())
        {}
        read_write_mutex(::boost::read_write_scheduling_policy::read_write_scheduling_policy_enum):
            read_write_mutex_static(read_write_mutex_static::dynamic_initializer())
        {}
    };
}

#endif