// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "core.h"
#include "ds/dllist.h"
#include "ds/hashmap.h"
#include "ds/mpscq.h"
#include "mpmcq.h"
#include "object/object.h"
#include "schedulerlist.h"
#include "schedulerstats.h"
#include "threadpool.h"

#include <snmalloc/snmalloc.h>

namespace verona::rt
{
  /**
   * There is typically one scheduler thread pinned to each physical CPU core.
   * Each scheduler thread is responsible for running cowns in its queue and
   * periodically stealing cowns from the queues of other scheduler threads.
   * This periodic work stealing is done to fairly distribute work across the
   * available scheduler threads. The period of work stealing for fairness is
   * determined by a single token cown that will be dequeued once all cowns
   * before it have been run. The removal of the token cown from the queue
   * occurs at a rate inversely proportional to the amount of cowns pending work
   * on that thread. A scheduler thread will enqueue a new token, if its
   * previous one has been dequeued or stolen, once more work is scheduled on
   * the scheduler thread.
   */
  template<class T>
  class SchedulerThread
  {
  public:
    /// Friendly thread identifier for logging information.
    size_t systematic_id = 0;

  private:
    using Scheduler = ThreadPool<SchedulerThread<T>, T>;
    friend Scheduler;
    friend T;
    friend DLList<SchedulerThread<T>>;
    friend SchedulerList<SchedulerThread<T>>;

    template<typename Owner>
    friend class Noticeboard;

    static constexpr uint64_t TSC_QUIESCENCE_TIMEOUT = 1'000'000;

    Core<T>* core = nullptr;
#ifdef USE_SYSTEMATIC_TESTING
    friend class ThreadSyncSystematic<SchedulerThread>;
    Systematic::Local* local_systematic{nullptr};
#else
    friend class ThreadSync<SchedulerThread>;
    LocalSync local_sync{};
#endif

    Alloc* alloc = nullptr;
    Core<T>* victim = nullptr;

    bool running = true;

    // `n_ld_tokens` indicates the times of token cown a scheduler has to
    // process before reaching its LD checkpoint (`n_ld_tokens == 0`).
    uint8_t n_ld_tokens = 0;

    bool should_steal_for_fairness = false;

    std::atomic<bool> scheduled_unscanned_cown = false;

    EpochMark send_epoch = EpochMark::EPOCH_A;
    EpochMark prev_epoch = EpochMark::EPOCH_B;

    ThreadState::State state = ThreadState::State::NotInLD;

    /// The MessageBody of a running behaviour.
    typename T::MessageBody* message_body = nullptr;

    /// SchedulerList pointers.
    SchedulerThread<T>* prev = nullptr;
    SchedulerThread<T>* next = nullptr;

    T* get_token_cown()
    {
      assert(core != nullptr);
      assert(core->token_cown);
      return core->token_cown;
    }

    SchedulerThread()
    {
      Logging::cout() << "Scheduler Thread created" << Logging::endl;
    }

    ~SchedulerThread() {}

    void set_core(Core<T>* core)
    {
      this->core = core;
    }

    inline void stop()
    {
      running = false;
    }

    inline void schedule_fifo(T* a)
    {
      Logging::cout() << "Enqueue cown " << a << " (" << a->get_epoch_mark()
                      << ")" << Logging::endl;

      // Scheduling on this thread, from this thread.
      if (!a->scanned(send_epoch))
      {
        Logging::cout() << "Enqueue unscanned cown " << a << Logging::endl;
        scheduled_unscanned_cown = true;
      }
      assert(!a->queue.is_sleeping());
      core->q.enqueue(*alloc, a);

      if (Scheduler::get().unpause())
        core->stats.unpause();
    }

    static inline void schedule_lifo(Core<T>* c, T* a)
    {
      // A lifo scheduled cown is coming from an external source, such as
      // asynchronous I/O.
      Logging::cout() << "LIFO scheduling cown " << a << " onto " << c->affinity
                      << Logging::endl;
      c->q.enqueue_front(ThreadAlloc::get(), a);
      Logging::cout() << "LIFO scheduled cown " << a << " onto " << c->affinity
                      << Logging::endl;

      c->stats.lifo();

      if (Scheduler::get().unpause())
        c->stats.unpause();
    }

    template<typename... Args>
    static void run(SchedulerThread* t, void (*startup)(Args...), Args... args)
    {
      t->run_inner(startup, args...);
    }

    /**
     * Startup is supplied to initialise thread local state before the runtime
     * starts.
     *
     * This is used for initialising the interpreters per-thread data-structures
     **/
    template<typename... Args>
    void run_inner(void (*startup)(Args...), Args... args)
    {
      startup(args...);

      Scheduler::local() = this;
      alloc = &ThreadAlloc::get();
      assert(core != nullptr);
      victim = core->next;
      T* cown = nullptr;
      core->servicing_threads++;

#ifdef USE_SYSTEMATIC_TESTING
      Systematic::attach_systematic_thread(local_systematic);
#endif

      while (true)
      {
        if (
          (core->total_cowns < (core->free_cowns << 1))
#ifdef USE_SYSTEMATIC_TESTING
          || Systematic::coin()
#endif
        )
          collect_cown_stubs();

        if (should_steal_for_fairness)
        {
          if (cown == nullptr)
          {
            should_steal_for_fairness = false;
            fast_steal(cown);
          }
        }

        if (cown == nullptr)
        {
          cown = core->q.dequeue(*alloc);
          if (cown != nullptr)
            Logging::cout()
              << "Pop cown " << clear_thread_bit(cown) << Logging::endl;
        }

        if (cown == nullptr)
        {
          cown = steal();

          // If we can't steal, we are done.
          if (cown == nullptr)
            break;
        }

        // Administrative work before handling messages.
        if (!prerun(cown))
        {
          cown = nullptr;
          continue;
        }

        Logging::cout() << "Schedule cown " << cown << " ("
                        << cown->get_epoch_mark() << ")" << Logging::endl;

        // This prevents the LD protocol advancing if this cown has not been
        // scanned. This catches various cases where we have stolen, or
        // reschedule with the empty queue. We are effectively rescheduling, so
        // check if unscanned. This seems a little agressive, but prevents the
        // protocol advancing too quickly.
        // TODO refactor this could be made more optimal if we only do this for
        // stealing, and running on same cown as previous loop.
        if (Scheduler::should_scan() && (cown->get_epoch_mark() != send_epoch))
        {
          Logging::cout() << "Unscanned cown next" << Logging::endl;
          scheduled_unscanned_cown = true;
        }

        ld_protocol();

        Logging::cout() << "Running cown " << cown << Logging::endl;

        // Update progress counter on that core.
        Core<T>* cown_core = cown->owning_core();
        assert(core != nullptr);

        // If the cown comes from another core, both core counts are
        // incremented. This reflects both CPU utilization and queue progress.
        if (cown_core != nullptr)
          cown_core->progress_counter++;
        if (cown_core != core)
          core->progress_counter++;
        core->last_worker = systematic_id;

        bool reschedule = cown->run(*alloc, state);

        if (reschedule)
        {
          if (should_steal_for_fairness)
          {
            schedule_fifo(cown);
            cown = nullptr;
          }
          else
          {
            assert(!cown->queue.is_sleeping());
            // Push to the back of the queue if the queue is not empty,
            // otherwise run this cown again. Don't push to the queue
            // immediately to avoid another thread stealing our only cown.

            T* n = core->q.dequeue(*alloc);

            if (n != nullptr)
            {
              schedule_fifo(cown);
              cown = n;
            }
            else
            {
              if (core->q.nothing_old())
              {
                Logging::cout() << "Queue empty" << Logging::endl;
                // We have effectively reached token cown.
                n_ld_tokens = 0;

                T* stolen;
                if (Scheduler::get().fair && fast_steal(stolen))
                {
                  schedule_fifo(cown);
                  cown = stolen;
                }
              }

              if (!has_thread_bit(cown))
              {
                Logging::cout()
                  << "Reschedule cown " << cown << " ("
                  << cown->get_epoch_mark() << ")" << Logging::endl;
              }
            }
          }
        }
        else
        {
          // Don't reschedule.
          cown = nullptr;
        }

        yield();
      }

      Logging::cout() << "Begin teardown (phase 1)" << Logging::endl;

      if (core != nullptr)
      {
        core->collect(*alloc);
      }

      Logging::cout() << "End teardown (phase 1)" << Logging::endl;

      Epoch(ThreadAlloc::get()).flush_local();
      Scheduler::get().enter_barrier();

      Logging::cout() << "Begin teardown (phase 2)" << Logging::endl;

      GlobalEpoch::advance();

      collect_cown_stubs<true>();

      Logging::cout() << "End teardown (phase 2)" << Logging::endl;

      if (core != nullptr)
      {
        auto val = core->servicing_threads.fetch_sub(1);
        if (val == 1)
        {
          Logging::cout() << "Destroying core " << core->affinity
                          << Logging::endl;
          core->q.destroy(*alloc);
        }
      }
      Systematic::finished_thread();

      // Reset the local thread pointer as this physical thread could be reused
      // for a different SchedulerThread later.
      Scheduler::local() = nullptr;
    }

    bool fast_steal(T*& result)
    {
      // auto cur_victim = victim;
      T* cown;

      // Try to steal from the victim thread.
      if (victim != core)
      {
        cown = victim->q.dequeue(*alloc);

        if (cown != nullptr)
        {
          // stats.steal();
          Logging::cout() << "Fast-steal cown " << clear_thread_bit(cown)
                          << " from " << victim->affinity << Logging::endl;
          result = cown;
          return true;
        }
      }

      // We were unable to steal, move to the next victim thread.
      victim = victim->next;

      return false;
    }

    void dec_n_ld_tokens()
    {
      assert(n_ld_tokens == 1 || n_ld_tokens == 2);
      Logging::cout() << "Reached LD token" << Logging::endl;
      n_ld_tokens--;
    }

    T* steal()
    {
      uint64_t tsc = Aal::tick();
      T* cown;

      while (running)
      {
        yield();

        if (core->q.nothing_old())
        {
          n_ld_tokens = 0;
        }

        // Participate in the cown LD protocol.
        ld_protocol();

        // Check if some other thread has pushed work on our queue.
        cown = core->q.dequeue(*alloc);

        if (cown != nullptr)
          return cown;

        // Try to steal from the victim thread.
        if (victim != core)
        {
          cown = victim->q.dequeue(*alloc);

          if (cown != nullptr)
          {
            core->stats.steal();
            Logging::cout() << "Stole cown " << clear_thread_bit(cown)
                            << " from " << victim->affinity << Logging::endl;
            return cown;
          }
        }

        // We were unable to steal, move to the next victim thread.
        victim = victim->next;

#ifdef USE_SYSTEMATIC_TESTING
        // Only try to pause with 1/(2^5) probability
        UNUSED(tsc);
        if (!Systematic::coin(5))
        {
          yield();
          continue;
        }
#else
        // Wait until a minimum timeout has passed.
        uint64_t tsc2 = Aal::tick();
        if ((tsc2 - tsc) < TSC_QUIESCENCE_TIMEOUT)
        {
          Aal::pause();
          continue;
        }
#endif

        // Enter sleep only if we aren't executing the leak detector currently.
        if (state == ThreadState::NotInLD)
        {
          // We've been spinning looking for work for some time. While paused,
          // our running flag may be set to false, in which case we terminate.
          if (Scheduler::get().pause())
            core->stats.pause();
        }
      }

      return nullptr;
    }

    bool has_thread_bit(T* cown)
    {
      return (uintptr_t)cown & 1;
    }

    T* clear_thread_bit(T* cown)
    {
      return (T*)((uintptr_t)cown & ~(uintptr_t)1);
    }

    /**
     * Some preliminaries required before we start processing messages
     *
     * - Check if this is the token, rather than a cown.
     * - Register cown to scheduler thread if not already on one.
     *
     * This returns false, if this is a token, and true if it is real cown.
     **/
    bool prerun(T* cown)
    {
      // See if this is a SchedulerThread enqueued as a cown LD marker.
      // It may not be this one.
      if (has_thread_bit(cown))
      {
        auto unmasked = clear_thread_bit(cown);
        Core<T>* owning_core = unmasked->owning_core();

        if (owning_core == core)
        {
          if (Scheduler::get().fair)
          {
            Logging::cout() << "Should steal for fairness!" << Logging::endl;
            should_steal_for_fairness = true;
          }

          if (n_ld_tokens > 0)
          {
            dec_n_ld_tokens();
          }

          Logging::cout() << "Reached token" << Logging::endl;
        }
        else
        {
          Logging::cout() << "Reached token: stolen from "
                          << owning_core->affinity << Logging::endl;
        }

        // Put back the token
        owning_core->q.enqueue(*alloc, cown);
        return false;
      }

      // Register this cown with the scheduler thread if it is not currently
      // registered with a scheduler thread.
      if (cown->owning_core() == nullptr)
      {
        Logging::cout() << "Bind cown to core: " << core << Logging::endl;
        assert(core != nullptr);
        cown->set_owning_core(core);
        core->add_cown(cown);
        core->total_cowns++;
      }

      return true;
    }

    void want_ld()
    {
      if (state == ThreadState::NotInLD)
      {
        Logging::cout() << "==============================================="
                        << Logging::endl;
        Logging::cout() << "==============================================="
                        << Logging::endl;
        Logging::cout() << "==============================================="
                        << Logging::endl;
        Logging::cout() << "==============================================="
                        << Logging::endl;

        ld_state_change(ThreadState::WantLD);
      }
    }

    bool ld_checkpoint_reached()
    {
      return n_ld_tokens == 0;
    }

    /**
     * This function updates the current thread state in the cown collection
     * protocol. This basically plays catch up with the global state, and can
     * vote for new states.
     **/
    void ld_protocol()
    {
      // Set state to BelieveDone_Vote when we think we've finished scanning.
      if ((state == ThreadState::AllInScan) && ld_checkpoint_reached())
      {
        Logging::cout() << "Scheduler unscanned flag: "
                        << scheduled_unscanned_cown << Logging::endl;

        if (!scheduled_unscanned_cown && Scheduler::no_inflight_messages())
        {
          ld_state_change(ThreadState::BelieveDone_Vote);
        }
        else
        {
          enter_scan();
        }
      }

      bool first = true;

      while (true)
      {
        ThreadState::State sprev = state;
        // Next state can affect global thread pool state, so add to testing for
        // systematic testing.
        yield();
        ThreadState::State snext = Scheduler::get().next_state(sprev);

        // If we have a lost wake-up, then all threads can get stuck
        // trying to perform a LD.
        if (
          sprev == ThreadState::PreScan && snext == ThreadState::PreScan &&
          Scheduler::get().unpause())
        {
          core->stats.unpause();
        }

        if (snext == sprev)
          return;
        yield();

        if (first)
        {
          first = false;
          Logging::cout() << "LD protocol loop" << Logging::endl;
        }

        ld_state_change(snext);

        // Actions taken when a state transition occurs.
        switch (state)
        {
          case ThreadState::PreScan:
          {
            if (Scheduler::get().unpause())
              core->stats.unpause();

            enter_prescan();
            return;
          }

          case ThreadState::Scan:
          {
            if (sprev != ThreadState::PreScan)
              enter_prescan();
            enter_scan();
            return;
          }

          case ThreadState::AllInScan:
          {
            if (sprev == ThreadState::PreScan)
              enter_scan();
            return;
          }

          case ThreadState::BelieveDone:
          {
            if (scheduled_unscanned_cown)
              ld_state_change(ThreadState::BelieveDone_Retract);
            else
              ld_state_change(ThreadState::BelieveDone_Confirm);
            continue;
          }

          case ThreadState::ReallyDone_Confirm:
          {
            continue;
          }

          case ThreadState::Sweep:
          {
            collect_cowns();
            continue;
          }

          default:
          {
            continue;
          }
        }
      }
    }

    bool in_sweep_state()
    {
      return state == ThreadState::Sweep;
    }

    void ld_state_change(ThreadState::State snext)
    {
      Logging::cout() << "Scheduler state change: " << state << " -> " << snext
                      << Logging::endl;
      state = snext;
    }

    void enter_prescan()
    {
      // Save epoch for when we start scanning
      prev_epoch = send_epoch;

      // Set sending Epoch to EpochNone. As these new messages need to be
      // counted to ensure all inflight work is processed before we complete
      // scanning.
      send_epoch = EpochMark::EPOCH_NONE;

      Logging::cout() << "send_epoch (1): " << send_epoch << Logging::endl;
    }

    void enter_scan()
    {
      send_epoch = (prev_epoch == EpochMark::EPOCH_B) ? EpochMark::EPOCH_A :
                                                        EpochMark::EPOCH_B;
      Logging::cout() << "send_epoch (2): " << send_epoch << Logging::endl;

      // Send empty messages to all cowns that can be LIFO scheduled.

      assert(core != nullptr);
      core->scan();
      n_ld_tokens = 2;
      scheduled_unscanned_cown = false;
      Logging::cout() << "Enqueued LD check point" << Logging::endl;
    }

    void collect_cowns()
    {
      assert(core != nullptr);
      core->try_collect(*alloc, send_epoch);
    }

    template<bool during_teardown = false>
    void collect_cown_stubs()
    {
      // Cannot collect the cown state while another thread could be
      // sweeping.  The other thread could be checking to see if it should
      // issue a decref to the object that is part of the same collection,
      // and thus cause a use-after-free.
      switch (state)
      {
        case ThreadState::ReallyDone_Confirm:
        case ThreadState::Finished:
          return;

        default:;
      }

      assert(core != nullptr);
      T* _list = core->drain();
      T** list = &_list;
      T** p = &_list;
      T* tail = nullptr;
      assert(p != nullptr);
      size_t removed_count = 0;
      size_t count = 0;

      while (*p != nullptr)
      {
        count++;
        T* c = *p;
        // Collect cown stubs when the weak count is zero.
        if (c->weak_count == 0 || during_teardown)
        {
          if (c->weak_count != 0)
          {
            Logging::cout() << "Leaking cown " << c << Logging::endl;
            if (Scheduler::get_detect_leaks())
            {
              *p = c->next;
              continue;
            }
          }
          Logging::cout() << "Stub collect cown " << c << Logging::endl;
          // TODO: Investigate systematic testing coverage here.
          auto epoch = c->epoch_when_popped;
          auto outdated =
            epoch == T::NO_EPOCH_SET || GlobalEpoch::is_outdated(epoch);
          if (outdated)
          {
            removed_count++;
            *p = c->next;
            Logging::cout() << "Stub collected cown " << c << Logging::endl;
            c->dealloc(*alloc);
            continue;
          }
          else
          {
            if (!outdated)
              Logging::cout()
                << "Cown " << c << " not outdated." << Logging::endl;
          }
        }
        tail = c;
        p = &(c->next);
      }

      // Put the list back
      if (*list != nullptr)
      {
        assert(tail != nullptr);
        core->add_cowns(*list, tail);
      }

      assert(this->core != nullptr);
      // TODO This will become false once we have multiple scheduler threads per
      // core.
      assert(this->core->total_cowns == count);
      this->core->free_cowns -= removed_count;
      this->core->total_cowns -= removed_count;

      Logging::cout() << "Stub collected " << removed_count << " cowns"
                      << " Free cowns " << this->core->free_cowns
                      << " Total cowns " << this->core->total_cowns
                      << Logging::endl;
    }
  };
} // namespace verona::rt
