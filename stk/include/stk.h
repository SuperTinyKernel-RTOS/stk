/*
 * SuperTinyKernel(TM) (STK): Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_H_
#define STK_H_

#include "stk_helper.h"
#include "strategy/stk_strategy_rrobin.h"
#include "strategy/stk_strategy_swrrobin.h"
#include "strategy/stk_strategy_monotonic.h"
#include "strategy/stk_strategy_edf.h"
#include "strategy/stk_strategy_fpriority.h"

/*! \file  stk.h
    \brief Contains core implementation (Kernel) of the task scheduler.
*/

namespace stk {

/*! \class Kernel
    \brief Concrete implementation of IKernel (thread scheduling kernel).
    \note  Kernel expects at least 1 task, e.g. Kernel<N> where N != 0.

    Usage example:
    \code
    static Kernel<KERNEL_STATIC, 3> kernel;
    static PlatformArmCortexM platform;
    static SwitchStrategyRoundRobin tsstrategy;

    static Task<ACCESS_PRIVILEGED> task1;
    static Task<ACCESS_USER> task2;
    static Task<ACCESS_USER> task3;

    kernel.Initialize(&platform, &tsstrategy);

    kernel.AddTask(&task1);
    kernel.AddTask(&task2);
    kernel.AddTask(&task3);

    kernel.Start(PERIODICITY_DEFAULT);
    \endcode
*/
template <int32_t _Mode, uint32_t _Size, class _TyStrategy, class _TyPlatform>
class Kernel : public IKernel, private IPlatform::IEventHandler
{
    /*! \typedef SleepTrapStackMemory
        \brief   Stack memory wrapper type for a Sleep trap.
    */
    typedef StackMemoryWrapper<STK_SLEEP_TRAP_STACK_SIZE> SleepTrapStackMemory;

    /*! \typedef ExitTrapStackMemory
        \brief   Stack memory wrapper type for an Exit trap.
    */
    typedef StackMemoryWrapper<STACK_SIZE_MIN> ExitTrapStackMemory;

    /*! \enum  ERequest
        \brief Request flags.
    */
    enum ERequest : uint8_t
    {
        REQUEST_NONE     = 0,       //!< none
        REQUEST_ADD_TASK = (1 << 0) //!< request for Kernel::AddTask is pending from some of the tasks
    };

    /*! \class KernelTask
        \brief Concrete implementation of the IKernelTask interface.
    */
    class KernelTask : public IKernelTask
    {
        friend class Kernel;

        /*! \enum  EStateFlags
            \brief Task state flags.
        */
        enum EStateFlags
        {
            STATE_NONE           = 0,        //!< none
            STATE_REMOVE_PENDING = (1 << 0), //!< task signaled that it exited
            STATE_SLEEP_PENDING  = (1 << 1)  //!< task signaled that it wants to sleep
        };

    public:
        /*! \class AddTaskRequest
            \brief Serialized add task request.
            \note  Related to stk::KERNEL_STATIC or stk::KERNEL_DYNAMIC modes only.
        */
        struct AddTaskRequest
        {
            ITask *user_task; //!< user task to add
        };

        /*! \brief Default initializer.
        */
        explicit KernelTask() : m_user(nullptr), m_stack(), m_state(STATE_NONE), m_time_sleep(0),
            m_srt(), m_hrt(), m_rt_weight()
        {
            // bind to wait object
            if (_Mode & KERNEL_SYNC)
                m_wait_obj->m_task = this;
        }

        ITask *GetUserTask() { return m_user; }

        Stack *GetUserStack() { return &m_stack; }

        bool IsBusy() const { return (m_user != nullptr); }

        bool IsSleeping() const { return (m_time_sleep < 0); }

        TId GetTid() const { return reinterpret_cast<TId>(m_user); }

        void Wake()
        {
            STK_ASSERT(IsSleeping());

            // wakeup on a next cycle
            m_time_sleep = -1;
        }

        void SetCurrentWeight(int32_t weight)
        {
            if (_TyStrategy::WEIGHT_API)
                m_rt_weight[0] = weight;
        }

        int32_t GetWeight() const { return (_TyStrategy::WEIGHT_API ? m_user->GetWeight() : 1); }
        int32_t GetCurrentWeight() const { return (_TyStrategy::WEIGHT_API ? m_rt_weight[0] : 1); }

        Timeout GetHrtPeriodicity() const
        {
            STK_ASSERT(_Mode & KERNEL_HRT);

            return (_Mode & KERNEL_HRT ? m_hrt[0].periodicity : 0);
        }

        Timeout GetHrtDeadline() const
        {
            STK_ASSERT(_Mode & KERNEL_HRT);

            return (_Mode & KERNEL_HRT ? m_hrt[0].deadline : 0);
        }

        Timeout GetHrtRelativeDeadline() const
        {
            STK_ASSERT(_Mode & KERNEL_HRT);
            STK_ASSERT(!IsSleeping());

            return (_Mode & KERNEL_HRT ? (m_hrt[0].deadline - m_hrt[0].duration) : 0);
        }

    private:
        /*! \class SrtInfo
            \brief Soft Real-Time info of the bound task.
            \note  Related to non stk::KERNEL_HRT mode only.
        */
        struct SrtInfo
        {
            SrtInfo() : add_task_req(nullptr)
            {}

            /*! \brief     Clear values.
            */
            void Clear()
            {
                add_task_req = nullptr;
            }

            AddTaskRequest *add_task_req; //!< add task request made from another active task (see Kernel::AddTask)
        };

        /*! \class HrtInfo
            \brief Hard Real-Time info of the bound task.
            \note  Related to stk::KERNEL_HRT mode only.
        */
        struct HrtInfo
        {
            HrtInfo() : periodicity(0), deadline(0), duration(0), done(false)
            {}

            /*! \brief     Clear values.
            */
            void Clear()
            {
                periodicity = 0;
                deadline    = 0;
                duration    = 0;
                done        = false;
            }

            Timeout periodicity; //!< scheduling periodicity (ticks)
            Timeout deadline;    //!< work deadline (ticks)
            Timeout duration;    //!< current duration of the active state when work is being carried out by the task (ticks)
            volatile bool done;  //!< true if task completed its work and called Yield()
        };

        /*! \class WaitObject
            \brief Concrete implementation of the IWaitObject interface.
            \note  Related to stk::KERNEL_SYNC mode only.
        */
        struct WaitObject : public IWaitObject
        {
            explicit WaitObject() : m_task(nullptr), m_sync_obj(nullptr), m_timeout(false), m_time_wait(0)
            {}

            /*! \class WaitRequest
                \brief Serialized request for registration of ISyncObject.
                \note  Related to stk::KERNEL_SYNC mode only.
            */
            struct WaitRequest
            {
                ISyncObject *sync_obj; //!< sync object to register
            };

            TId GetTid() const { return m_task->GetTid(); }

            bool IsTimeout() const { return m_timeout; }

            void Wake(bool timeout)
            {
                STK_ASSERT(m_sync_obj != nullptr);

                m_timeout   = timeout;
                m_time_wait = 0;

                m_sync_obj->RemoveWaitObject(this);
                m_sync_obj = nullptr;

                return m_task->Wake();
            }

            bool Tick()
            {
                if ((m_time_wait != WAIT_INFINITE) && !m_timeout)
                {
                    --m_time_wait;

                    if (m_time_wait <= 0)
                        m_timeout = true;
                }

                return !m_timeout;
            }

            void SetupWait(ISyncObject *sync_obj, Timeout timeout)
            {
                STK_ASSERT(m_sync_obj == nullptr);

                m_sync_obj  = sync_obj;
                m_time_wait = timeout;
                m_timeout   = false;

                sync_obj->AddWaitObject(this);
            }

            KernelTask   *m_task;      //!< waiting task
            ISyncObject  *m_sync_obj;  //!< synchronization object which caused waiting
            volatile bool m_timeout;   //!< true if timeout happened due to waiting
            Timeout       m_time_wait; //!< number of ticks left till timeout
        };

        /*! \brief     Release variables from info about previous task.
        */
        void Bind(_TyPlatform *platform, ITask *user_task)
        {
            // set access mode for this stack
            m_stack.mode = user_task->GetAccessMode();

            // set task id for tracking purpose
        #if STK_NEED_TASK_ID
            m_stack.tid = user_task->GetId();
        #endif

            // init stack of the user task
            if (!platform->InitStack(STACK_USER_TASK, &m_stack, user_task, user_task))
            {
                STK_ASSERT(false);
            }

            // bind user task
            m_user = user_task;
        }

        /*! \brief     Release variables from info about previous task.
        */
        void Unbind()
        {
            m_user       = nullptr;
            m_stack      = {};
            m_state      = STATE_NONE;
            m_time_sleep = 0;

            if (_Mode & KERNEL_HRT)
                m_hrt[0].Clear();
            else
                m_srt->Clear();
        }

        /*! \brief     Schedule the removal of the task from the kernel on next tick.
        */
        void ScheduleRemoval()
        {
            // make this task sleeping to switch it out from scheduling process
            ScheduleSleep(INT32_MAX);

            // mark it as done HRT task
            if (_Mode & KERNEL_HRT)
                HrtOnWorkCompleted();

            // mark it as pending for removal
            m_state |= STATE_REMOVE_PENDING;
        }

        /*! \brief     Check if task is pending removal.
        */
        bool IsPendingRemoval() const { return (m_state & STATE_REMOVE_PENDING) != 0; }

        /*! \brief     Check if Stack Pointer (SP) belongs to this task.
            \param[in] SP: Stack Pointer.
        */
        bool IsMemoryOfSP(size_t SP) const
        {
            size_t *start = m_user->GetStack();
            size_t *end   = start + m_user->GetStackSize();

            return (SP >= (size_t)start) && (SP <= (size_t)end);
        }

        /*! \brief     Initialize task with HRT info.
            \note      Related to stk::KERNEL_HRT mode only.
            \param[in] periodicity_tc: Periodicity time at which task is scheduled (ticks).
            \param[in] deadline_tc: Deadline time within which a task must complete its work (ticks).
            \param[in] start_delay_tc: Initial start delay for the task (ticks).
        */
        void HrtInit(Timeout periodicity_tc, Timeout deadline_tc, Timeout start_delay_tc)
        {
            STK_ASSERT(periodicity_tc > 0);
            STK_ASSERT(deadline_tc > 0);
            STK_ASSERT(start_delay_tc >= 0);
            STK_ASSERT(periodicity_tc < INT32_MAX);
            STK_ASSERT(deadline_tc < INT32_MAX);

            m_hrt[0].periodicity = periodicity_tc;
            m_hrt[0].deadline    = deadline_tc;

            if (start_delay_tc > 0)
                ScheduleSleep(start_delay_tc);
        }

        /*! \brief     Called when task is switched into the scheduling process.
            \note      Related to stk::KERNEL_HRT mode only.
            \param[in] ticks: Current ticks of the Kernel.
        */
        void HrtOnSwitchedIn() {}

        /*! \brief     Called when task is switched out from the scheduling process.
            \note      Related to stk::KERNEL_HRT mode only.
            \param[in] platform: Platform driver instance.
            \param[in] ticks: Current ticks of the Kernel.
        */
        void HrtOnSwitchedOut(IPlatform */*platform*/)
        {
            const Timeout duration = m_hrt[0].duration;

            STK_ASSERT(duration >= 0);

            Timeout sleep = m_hrt[0].periodicity - duration;
            if (sleep > 0)
                ScheduleSleep(sleep);

            m_hrt[0].duration = 0;
            m_hrt[0].done     = false;
        }

        /*! \brief     Hard-fail HRT task when it missed its deadline.
            \note      Related to stk::KERNEL_HRT mode only.
            \param[in] platform: Platform driver instance.
        */
        void HrtHardFailDeadline(IPlatform *platform)
        {
            const Timeout duration = m_hrt[0].duration;

            STK_ASSERT(duration >= 0);
            STK_ASSERT(HrtIsDeadlineMissed(duration));

            m_user->OnDeadlineMissed(duration);
            platform->ProcessHardFault();
        }

        /*! \brief     Called when task process called IKernelService::SwitchToNext to inform Kernel that work is completed.
            \note      Related to stk::KERNEL_HRT mode only.
        */
        void HrtOnWorkCompleted()
        {
            m_hrt[0].done = true;
            __stk_full_memfence();
        }

        /*! \brief     Check if deadline missed.
            \note      Related to stk::KERNEL_HRT mode only.
        */
        bool HrtIsDeadlineMissed(Timeout duration) const { return (duration > m_hrt[0].deadline); }

        /*! \brief     Schedule task to sleep.
            \param[in] ticks: Number of ticks to sleep.
        */
        void ScheduleSleep(Timeout ticks)
        {
            STK_ASSERT(ticks > 0);

            // set state first as kernel checks it when task IsSleeping
            if (_TyStrategy::SLEEP_EVENT_API)
            {
                if (m_time_sleep >= 0)
                    m_state |= STATE_SLEEP_PENDING;
            }

            m_time_sleep = -ticks;
            __stk_full_memfence();
        }

        ITask            *m_user;       //!< user task
        Stack             m_stack;      //!< stack descriptor
        volatile uint32_t m_state;      //!< state flags
        volatile Timeout  m_time_sleep; //!< time to sleep (ticks)
        SrtInfo           m_srt[STK_ALLOCATE_COUNT(_Mode, KERNEL_HRT, 0, 1)];       //!< Soft Real-Time info (does not occupy memory if kernel operation mode is stk::KERNEL_HRT)
        HrtInfo           m_hrt[STK_ALLOCATE_COUNT(_Mode, KERNEL_HRT, 1, 0)];       //!< Hard Real-Time info (does not occupy memory if kernel operation mode is not stk::KERNEL_HRT)
        int32_t           m_rt_weight[_TyStrategy::WEIGHT_API ? 1 : 0];             //!< current (run-time) weight, see SwitchStrategySmoothWeightedRoundRobin
        WaitObject        m_wait_obj[STK_ALLOCATE_COUNT(_Mode, KERNEL_SYNC, 1, 0)]; //!< wait object (synchronization support)
    };

    /*! \class KernelService
        \brief Concrete implementation of the IKernelService interface.
    */
    class KernelService : public IKernelService
    {
        friend class Kernel;

    public:
        TId GetTid() const { return m_platform->GetTid(); }

        Ticks GetTicks() const { return hw::ReadVolatileMemory(&m_ticks); }

        int32_t GetTickResolution() const  { return m_platform->GetTickResolution(); }

        __stk_attr_noinline void Delay(Timeout msec)
        {
            Ticks deadline = GetTicks() + GetTicksFromMsec(msec, GetTickResolution());
            while (GetTicks() < deadline)
            {
                __stk_relax_cpu();
            }
        }

        __stk_attr_noinline void Sleep(Timeout msec)
        {
            if ((_Mode & KERNEL_HRT) == 0)
            {
                m_platform->SleepTicks((Timeout)GetTicksFromMsec(msec, GetTickResolution()));
            }
            else
            {
                // sleeping is not supported in HRT mode, task will sleep according to its periodicity and workload
                STK_ASSERT(false);
            }
        }

        void SwitchToNext() { m_platform->SwitchToNext(); }

        IWaitObject *StartWaiting(ISyncObject *sobj, IMutex *mutex, Timeout ticks)
        {
            if (_Mode & KERNEL_SYNC)
            {
                return m_platform->StartWaiting(sobj, mutex, ticks);
            }
            else
            {
                STK_ASSERT(false);
                return nullptr;
            }
        }

    private:
        /*! \brief     Default initializer.
        */
        explicit KernelService() : m_platform(0), m_ticks(0)
        {}

        /*! \brief     Initialize instance.
            \note      When call completes Singleton<IKernelService *> will start referencing this
                       instance (see g_KernelService).
            \param[in] platform: IPlatform instance.
        */
        void Initialize(IPlatform *platform)
        {
            m_platform = static_cast<_TyPlatform *>(platform);
        }

        /*! \brief     Increment tick by 1.
            \note      Modified always from ISR (guaranteed) therefore hw::WriteSafe64() is not required.
        */
        void IncrementTick() { ++m_ticks; }

        _TyPlatform   *m_platform; //!< platform
        volatile Ticks m_ticks;    //!< CPU ticks elapsed (volatile to reload value from the memory by the consumer)
    };

public:
    /*! \brief Constants.
    */
    enum EConsts
    {
        TASKS_MAX = _Size //!< Maximum number of tasks supported by the instance of the Kernel.
    };

    /*! \brief Default initializer.
    */
    explicit Kernel() : m_platform(), m_strategy(), m_task_now(nullptr), m_task_storage(), m_sleep_trap(),
        m_exit_trap(), m_fsm_state(FSM_STATE_NONE), m_request(~0)
    {
        // HRT mode does not support scheduling strategy with task weights
        STK_STATIC_ASSERT((!_TyStrategy::WEIGHT_API && ((_Mode & KERNEL_HRT) != 0)) ||
            ((_Mode & KERNEL_HRT) == 0));

    #ifdef _DEBUG
        // _TyPlatform must inherit IPlatform
        IPlatform *platform = &m_platform;
        (void)platform;

        // _TyStrategy must inherit ITaskSwitchStrategy
        ITaskSwitchStrategy *strategy = &m_strategy;
        (void)strategy;
    #endif
    }

    __stk_attr_noinline void Initialize(uint32_t resolution_us = PERIODICITY_DEFAULT)
    {
        STK_ASSERT(resolution_us != 0);
        STK_ASSERT(resolution_us <= PERIODICITY_MAX);
        STK_ASSERT(!IsInitialized());

        m_task_now  = nullptr;
        m_fsm_state = FSM_STATE_NONE;
        m_request   = REQUEST_NONE;

        m_service.Initialize(&m_platform);
        m_platform.Initialize(this, &m_service, resolution_us, (_Mode & KERNEL_DYNAMIC ? &m_exit_trap[0].stack : nullptr));
    }

    __stk_attr_noinline void AddTask(ITask *user_task)
    {
        if ((_Mode & KERNEL_HRT) == 0)
        {
            STK_ASSERT(user_task != nullptr);
            STK_ASSERT(IsInitialized());

            // when started the operation must be serialized by switching out from processing until
            // kernel processes this request
            if (IsStarted())
            {
                if ((_Mode & KERNEL_DYNAMIC) != 0)
                {
                    RequestAddTask(user_task);
                }
                else
                {
                    STK_ASSERT(false);
                }
            }
            else
            {
                AllocateAndAddNewTask(user_task);
            }
        }
        else
        {
            STK_ASSERT(false);
        }
    }

    __stk_attr_noinline void AddTask(ITask *user_task, Timeout periodicity_tc, Timeout deadline_tc, Timeout start_delay_tc)
    {
        if (_Mode & KERNEL_HRT)
        {
            STK_ASSERT(user_task != nullptr);
            STK_ASSERT(IsInitialized());
            STK_ASSERT(!IsStarted());

            HrtAllocateAndAddNewTask(user_task, periodicity_tc, deadline_tc, start_delay_tc);
        }
        else
        {
            STK_ASSERT(false);
        }
    }

    __stk_attr_noinline void RemoveTask(ITask *user_task)
    {
        if (_Mode & KERNEL_DYNAMIC)
        {
            STK_ASSERT(user_task != nullptr);
            STK_ASSERT(!IsStarted());

            KernelTask *task = FindTask(user_task);
            if (task != nullptr)
                RemoveTask(task);
        }
        else
        {
            // kernel operating mode must be KERNEL_DYNAMIC for tasks to be able to be removed
            STK_ASSERT(false);
        }
    }

    __stk_attr_noinline void Start()
    {
        STK_ASSERT(IsInitialized());

        m_task_now = nullptr;

         // stacks of the traps must be re-initilized on every subsequent Start
        InitTraps();

        // start tracing
    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_Start();
        for (int32_t i = 0; i < TASKS_MAX; ++i)
        {
            KernelTask *task = &m_task_storage[i];
            if (task->IsBusy())
                SendTaskTraceInfo(task);
        }
    #endif

        m_platform.Start();
    }

    bool IsStarted() const { return (m_task_now != nullptr); }

    IPlatform *GetPlatform() { return &m_platform; }

    ITaskSwitchStrategy *GetSwitchStrategy() { return &m_strategy; }

protected:
    /*! \enum  EFsmState
        \brief Finite-state machine (FSM) state.
    */
    enum EFsmState : int8_t
    {
        FSM_STATE_NONE = -1,
        FSM_STATE_SWITCHING,
        FSM_STATE_SLEEPING,
        FSM_STATE_WAKING,
        FSM_STATE_EXITING,
        FSM_STATE_MAX
    };

    /*! \enum  EFsmEvent
        \brief Finite-state machine (FSM) event.
    */
    enum EFsmEvent : int8_t
    {
        FSM_EVENT_SWITCH = 0,
        FSM_EVENT_SLEEP,
        FSM_EVENT_WAKE,
        FSM_EVENT_EXIT,
        FSM_EVENT_MAX
    };

    /*! \brief     Initialize stack of the traps.
    */
    __stk_attr_noinline void InitTraps()
    {
        // init stack for a Sleep trap
        {
            SleepTrapStack &sleep = m_sleep_trap[0];

            SleepTrapStackMemory wrapper(&sleep.memory);
            sleep.stack.mode = ACCESS_PRIVILEGED;
        #if STK_NEED_TASK_ID
            sleep.stack.tid  = SYS_TASK_ID_SLEEP;
        #endif

            m_platform.InitStack(STACK_SLEEP_TRAP, &sleep.stack, &wrapper, nullptr);
        }

        // init stack for an Exit trap
        if (_Mode & KERNEL_DYNAMIC)
        {
            ExitTrapStack &exit = m_exit_trap[0];

            ExitTrapStackMemory wrapper(&exit.memory);
            exit.stack.mode = ACCESS_PRIVILEGED;
        #if STK_NEED_TASK_ID
            exit.stack.tid  = SYS_TASK_ID_EXIT;
        #endif

            m_platform.InitStack(STACK_EXIT_TRAP, &exit.stack, &wrapper, nullptr);
        }
    }

    /*! \brief     Allocate new instance of KernelTask.
        \param[in] user_task: User task for which kernel task object is allocated.
        \return    Kernel task.
    */
    KernelTask *AllocateNewTask(ITask *user_task)
    {
        // look for a free kernel task
        KernelTask *new_task = nullptr;
        for (uint32_t i = 0; i < TASKS_MAX; ++i)
        {
            KernelTask *task = &m_task_storage[i];
            if (task->IsBusy())
            {
                // avoid task collision
                STK_ASSERT(task->m_user != user_task);

                // avoid stack collision
                STK_ASSERT(task->m_user->GetStack() != user_task->GetStack());
            }
            else
            if (new_task == nullptr)
            {
                new_task = task;
            #if defined(NDEBUG) && !defined(_STK_ASSERT_REDIRECT)
                break; // break if assertions are inactive and do not try to validate collision with existing tasks
            #endif
            }
        }

        // if nullptr - exceeded max supported kernel task count, application design failure
        STK_ASSERT(new_task != nullptr);

        new_task->Bind(&m_platform, user_task);

        return new_task;
    }

    /*! \brief     Add kernel task to the scheduling strategy.
        \param[in] task: Pointer to the kernel task.
    */
    void AddKernelTask(KernelTask *task)
    {
    #if STK_SEGGER_SYSVIEW
        // start tracing new task
        SEGGER_SYSVIEW_OnTaskCreate(task->GetUserStack()->tid);
        if (IsStarted())
            SendTaskTraceInfo(task);
    #endif

        m_strategy.AddTask(task);
    }

    /*! \brief     Allocate new instance of KernelTask and add it into the scheduling process.
        \param[in] user_task: User task for which kernel task object is allocated.
        \return    Kernel task.
    */
    void AllocateAndAddNewTask(ITask *user_task)
    {
        KernelTask *task = AllocateNewTask(user_task);
        STK_ASSERT(task != nullptr);

        AddKernelTask(task);
    }

    /*! \brief     Allocate new instance of KernelTask and add it into the HRT scheduling process.
        \note      Related to stk::KERNEL_HRT mode only.
        \param[in] user_task: User task for which kernel task object is allocated.
        \param[in] periodicity_tc: Periodicity time at which task is scheduled (ticks).
        \param[in] deadline_tc: Deadline time within which a task must complete its work (ticks).
        \param[in] start_delay_tc: Initial start delay for the task (ticks).
        \return    Kernel task.
    */
    void HrtAllocateAndAddNewTask(ITask *user_task, Timeout periodicity_tc, Timeout deadline_tc, Timeout start_delay_tc)
    {
        KernelTask *task = AllocateNewTask(user_task);
        STK_ASSERT(task != nullptr);

        task->HrtInit(periodicity_tc, deadline_tc, start_delay_tc);

        AddKernelTask(task);
    }

    /*! \brief     Request to add new task.
        \note      Must be called by the task process only!
        \param[in] user_task: User task to add.
    */
    __stk_attr_noinline void RequestAddTask(ITask *user_task)
    {
        STK_ASSERT(_Mode & KERNEL_DYNAMIC);

        KernelTask *caller = FindTaskBySP(m_platform.GetCallerSP());
        STK_ASSERT(caller != nullptr);

        typename KernelTask::AddTaskRequest req = { .user_task = user_task };
        caller->m_srt[0].add_task_req = &req;

        // notify kernel
        ScheduleAddTask();

        // switch out and wait for completion (due to context switch request could be processed here)
        if (caller->m_srt[0].add_task_req != nullptr)
            m_service.SwitchToNext();

        STK_ASSERT(caller->m_srt[0].add_task_req == nullptr);
    }

    /*! \brief     Find kernel task for the bound ITask instance.
        \param[in] user_task: User task.
        \return    Kernel task.
    */
    __stk_attr_noinline KernelTask *FindTask(const ITask *user_task)
    {
        for (uint32_t i = 0; i < TASKS_MAX; ++i)
        {
            KernelTask *task = &m_task_storage[i];
            if (task->GetUserTask() == user_task)
                return task;
        }

        return nullptr;
    }

    /*! \brief     Find kernel task for the bound Stack instance.
        \param[in] stack: Stack.
        \return    Kernel task.
    */
    KernelTask *FindTask(const Stack *stack) const
    {
        for (uint32_t i = 0; i < TASKS_MAX; ++i)
        {
            KernelTask *task = const_cast<KernelTask *>(&m_task_storage[i]);
            if (task->GetUserStack() == stack)
                return task;
        }

        return nullptr;
    }

    /*! \brief     Find kernel task for a Stack Pointer (SP).
        \param[in] SP: Stack pointer.
        \return    Kernel task.
    */
    __stk_attr_noinline KernelTask *FindTaskBySP(size_t SP) const
    {
        STK_ASSERT(m_task_now != nullptr);

        if (m_task_now->IsMemoryOfSP(SP))
            return m_task_now;

        for (uint32_t i = 0; i < TASKS_MAX; ++i)
        {
            KernelTask *task = const_cast<KernelTask *>(&m_task_storage[i]);

            // skip finished tasks (applicable only for KERNEL_DYNAMIC mode)
            if ((_Mode & KERNEL_DYNAMIC) && !task->IsBusy())
                continue;

            if (task->IsMemoryOfSP(SP))
                return task;
        }

        return nullptr;
    }

    /*! \brief     Remove kernel task.
        \note      Removal of the kernel task means releasing it from the user task details.
        \param[in] task: Kernel task.
    */
    void RemoveTask(KernelTask *task)
    {
        STK_ASSERT(task != nullptr);

    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_OnTaskTerminate(task->GetUserStack()->tid);
    #endif

        m_strategy.RemoveTask(task);
        task->Unbind();
    }

    __stk_attr_noinline void OnStart(Stack **active)
    {
        STK_ASSERT(m_strategy.GetSize() != 0);

        // iterate tasks and generate OnTaskSleep for a strategy for all initially sleeping tasks
        if (_TyStrategy::SLEEP_EVENT_API)
        {
            for (uint32_t i = 0; i < TASKS_MAX; ++i)
            {
                KernelTask *task = &m_task_storage[i];

                if (task->IsSleeping())
                {
                    if (task->m_state & KernelTask::STATE_SLEEP_PENDING)
                    {
                        task->m_state &= ~KernelTask::STATE_SLEEP_PENDING;

                        // notify strategy that task is sleeping
                        m_strategy.OnTaskSleep(task);
                    }
                }
            }
        }

        // get initial state and first task
        {
            m_fsm_state = FSM_STATE_SWITCHING;

            KernelTask *next = nullptr;
            m_fsm_state = GetNewFsmState(&next);

            // expecting only SLEEPING or SWITCHING states
            STK_ASSERT((m_fsm_state == FSM_STATE_SLEEPING) || (m_fsm_state == FSM_STATE_SWITCHING));

            if (m_fsm_state == FSM_STATE_SWITCHING)
            {
                m_task_now = next;

                (*active) = next->GetUserStack();

                if (_Mode & KERNEL_HRT)
                    next->HrtOnSwitchedIn();
            }
            else
            if (m_fsm_state == FSM_STATE_SLEEPING)
            {
                m_task_now = static_cast<KernelTask *>(m_strategy.GetFirst());

                (*active) = &m_sleep_trap[0].stack;
            }
        }

    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_OnTaskStartExec(m_task_now->tid);
    #endif
    }

    bool OnTick(Stack **idle, Stack **active)
    {
        m_service.IncrementTick();
        UpdateTasks();
        return UpdateFsmState(idle, active);
    }

    void OnTaskSwitch(size_t caller_SP)
    {
        // yield with 2 ticks: 1 will be incremented on the next OnTick call by UpdateTasks
        // and remaining 1 will cause a context switch by UpdateFsmState when strategy detects
        // it as a sleeping test
        OnTaskSleep(caller_SP, 2);
    }

    void OnTaskSleep(size_t caller_SP, Timeout ticks)
    {
        KernelTask *task = FindTaskBySP(caller_SP);
        STK_ASSERT(task != nullptr);

        // make change to HRT state and sleep time atomic
        {
            hw::CriticalSection::ScopedLock __cs;

            if (_Mode & KERNEL_HRT)
            {
                task->HrtOnWorkCompleted();
            }

            task->ScheduleSleep(ticks);
        }

        // note: we do not spin long here, kernel will switch this task out from scheduling on the next tick
        while (task->IsSleeping())
        {
            __stk_relax_cpu();
        }
    }

    void OnTaskExit(Stack *stack)
    {
        if (_Mode & KERNEL_DYNAMIC)
        {
            KernelTask *task = FindTask(stack);
            STK_ASSERT(task != nullptr);

            task->ScheduleRemoval();
        }
        else
        {
            // kernel operating mode must be KERNEL_DYNAMIC for tasks to be able to exit
            STK_ASSERT(false);
        }
    }

    IWaitObject *OnTaskWait(size_t caller_SP, ISyncObject *sync_obj, IMutex *mutex, Timeout timeout)
    {
        if (_Mode & KERNEL_SYNC)
        {
            STK_ASSERT(timeout != 0);
            STK_ASSERT(sync_obj != nullptr);
            STK_ASSERT(mutex != nullptr);
            STK_ASSERT((sync_obj->GetHead() == nullptr) || (sync_obj->GetHead() == &m_sync_list[0]));

            KernelTask *task = FindTaskBySP(caller_SP);
            STK_ASSERT(task != nullptr);

            // configure waiting
            task->m_wait_obj->SetupWait(sync_obj, timeout);

            // register ISyncObject if not yet
            if (sync_obj->GetHead() == nullptr)
                m_sync_list->LinkBack(sync_obj);

            // start sleeping infinitely, we rely on a Wake call via WaitObject
            task->ScheduleSleep(WAIT_INFINITE);

            // unlock mutex locked externally, so that we could wait in a busy-waiting loop
            mutex->Unlock();

            // note: we do not spin long here, kernel will switch this task out from scheduling on the next tick
            while (task->IsSleeping())
            {
                __stk_relax_cpu();
            }

            // re-lock mutex when returning to the task's execution space
            mutex->Lock();

            return task->m_wait_obj;
        }
        else
        {
            STK_ASSERT(false);
            return nullptr;
        }
    }

    TId OnGetTid(size_t caller_SP) const
    {
        KernelTask *task = FindTaskBySP(caller_SP);
        STK_ASSERT(task != nullptr);

        return task->GetTid();
    }

    /*! \brief     Update tasks (sleep, requests).
    */
    void UpdateTasks()
    {
        // sync objects are updated before UpdateTaskRequest which may add a new object (newly added object must become 1 tick older)
        if (_Mode & KERNEL_SYNC)
            UpdateSyncObjects();

        UpdateTaskRequest();
        UpdateTaskState();
    }

    /*! \brief     Update task state (removal, sleep, duration of HRT task).
    */
    void UpdateTaskState()
    {
        for (uint32_t i = 0; i < TASKS_MAX; ++i)
        {
            KernelTask *task = &m_task_storage[i];

            if (task->IsSleeping())
            {
                if (_Mode & KERNEL_DYNAMIC)
                {
                    // task is pending removal, wait until it is switched out
                    if (task->IsPendingRemoval())
                    {
                        if ((task != m_task_now) ||
                            ((m_strategy.GetSize() == 1) && (m_fsm_state == FSM_STATE_SLEEPING)))
                        {
                            RemoveTask(task);
                            continue;
                        }
                    }
                }

                // deliver sleep event to strategy
                // note: only currently scheduled task can be pending to sleep
                if (_TyStrategy::SLEEP_EVENT_API)
                {
                    if (task->m_state & KernelTask::STATE_SLEEP_PENDING)
                    {
                        task->m_state &= ~KernelTask::STATE_SLEEP_PENDING;

                        // notify strategy that task is sleeping
                        m_strategy.OnTaskSleep(task);
                    }
                }

                // advance sleep time by a tick
                ++task->m_time_sleep;

                // deliver sleep event to strategy
                if (_TyStrategy::SLEEP_EVENT_API)
                {
                    // notify strategy that task woke up
                    if (task->m_time_sleep >= 0)
                        m_strategy.OnTaskWake(task);
                }
            }
            else
            if (_Mode & KERNEL_HRT)
            {
                // in HRT mode we trace how long task spent in active state (doing some work)
                if (task->IsBusy())
                {
                    ++task->m_hrt[0].duration;

                    // check if deadline is missed (HRT failure)
                    if (task->HrtIsDeadlineMissed(task->m_hrt[0].duration))
                        task->HrtHardFailDeadline(&m_platform);
                }
            }
        }
    }

    /*! \brief     Update synchronization objects.
    */
    void UpdateSyncObjects()
    {
        STK_ASSERT(_Mode & KERNEL_SYNC);

        ISyncObject::ListEntryType *itr = m_sync_list->GetFirst();

        while (itr != nullptr)
        {
            ISyncObject::ListEntryType *next = itr->GetNext();

            if (!static_cast<ISyncObject *>(itr)->Tick())
                m_sync_list->Unlink(itr);

            itr = next;
        }
    }

    /*! \brief     Update pending task requests.
    */
    void UpdateTaskRequest()
    {
        if (m_request == REQUEST_NONE)
            return;

        // process AddTask requests coming from tasks (KERNEL_DYNAMIC mode only)
        if (!(_Mode & KERNEL_HRT) && (_Mode & KERNEL_DYNAMIC))
        {
            // process serialized AddTask request made from another active task, requesting process
            // is currently waiting due to SwitchToNext()
            if (m_request & REQUEST_ADD_TASK)
            {
                m_request &= ~REQUEST_ADD_TASK;

                for (uint32_t i = 0; i < TASKS_MAX; ++i)
                {
                    KernelTask *task = &m_task_storage[i];

                    if (task->m_srt[0].add_task_req != nullptr)
                    {
                        AllocateAndAddNewTask(task->m_srt[0].add_task_req->user_task);

                        task->m_srt[0].add_task_req = nullptr;
                        __stk_full_memfence();
                    }
                }
            }
        }
    }

    /*! \brief     Fetch next event for the FSM.
        \param[in] next: Next kernel task to which Kernel can switch.
        \return    FSM event.
    */
    EFsmEvent FetchNextEvent(KernelTask **next)
    {
        EFsmEvent type;
        KernelTask *itr;

        // check if no tasks left in Dynamic mode and exit
        if ((_Mode & KERNEL_DYNAMIC) && (m_strategy.GetSize() == 0))
        {
            itr  = nullptr;
            type = FSM_EVENT_EXIT;
        }
        else
        {
            itr = static_cast<KernelTask *>(m_strategy.GetNext());

            // sleep-aware strategy returns nullptr if no active tasks available, start sleeping
            if (itr == nullptr)
            {
                type = FSM_EVENT_SLEEP;
            }
            else
            {
                // strategy must provide active-only task
                STK_ASSERT(!itr->IsSleeping());

                // if was sleeping, process wake event first
                type = (m_fsm_state == FSM_STATE_SLEEPING ? FSM_EVENT_WAKE : FSM_EVENT_SWITCH);
            }
        }

        (*next) = itr;
        return type;
    }

    /*! \brief     Get new FSM state.
        \param[in] next: Next kernel task to which Kernel can switch.
        \return    FSM state.
    */
    EFsmState GetNewFsmState(KernelTask **next)
    {
        STK_ASSERT(m_fsm_state != FSM_STATE_NONE);
        return m_fsm[m_fsm_state][FetchNextEvent(next)];
    }

    /*! \brief      Update FSM state.
        \param[out] idle: Stack of the task which must enter Idle state.
        \param[out] active: Stack of the task which must enter Active state (to which context will switch).
        \return     FSM state.
    */
    bool UpdateFsmState(Stack **idle, Stack **active)
    {
        KernelTask *now = m_task_now, *next;
        EFsmState new_state = GetNewFsmState(&next);
        bool switch_context;

        switch (new_state)
        {
        case FSM_STATE_SWITCHING: {
            switch_context = StateSwitch(now, next, idle, active);
            break; }

        case FSM_STATE_WAKING: {
            switch_context = StateWake(now, next, idle, active);
            break; }

        case FSM_STATE_SLEEPING: {
            switch_context = StateSleep(now, next, idle, active);
            break; }

        case FSM_STATE_EXITING: {
            switch_context = StateExit(now, next, idle, active);
            break; }

        default:
            return false;
        }

        m_fsm_state = new_state;
        return switch_context;
    }

    /*! \brief      Switches contexts.
        \note       FSM state: stk::FSM_STATE_SWITCHING.
        \param[in]  now: Currently active kernel task.
        \param[in]  next: Next kernel task.
        \param[out] idle: Stack of the task which must enter Idle state.
        \param[out] active: Stack of the task which must enter Active state (to which context will switch).
    */
    bool StateSwitch(KernelTask *now, KernelTask *next, Stack **idle, Stack **active)
    {
        STK_ASSERT(now != nullptr);
        STK_ASSERT(next != nullptr);

        // do not switch context because task did not change
        if (next == now)
            return false;

        (*idle)   = now->GetUserStack();
        (*active) = next->GetUserStack();

        // if stack memory is exceeded these assertions will be hit
        if (now->IsBusy())
        {
            // current task could exit, thus we check it with IsBusy to avoid referencing nullptr returned by GetUserTask()
            STK_ASSERT(now->GetUserTask()->GetStack()[0] == STK_STACK_MEMORY_FILLER);
        }
        STK_ASSERT(next->GetUserTask()->GetStack()[0] == STK_STACK_MEMORY_FILLER);

        m_task_now = next;

        if (_Mode & KERNEL_HRT)
        {
            if (now->m_hrt[0].done)
            {
                now->HrtOnSwitchedOut(&m_platform);
                next->HrtOnSwitchedIn();
            }
        }

    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_OnTaskStopReady(now->GetUserStack()->tid, TRACE_EVENT_SWITCH);
        SEGGER_SYSVIEW_OnTaskStartReady(next->GetUserStack()->tid);
    #endif

        return true; // switch context
    }

    /*! \brief      Wakes up after sleeping.
        \note       FSM state: stk::FSM_STATE_AWAKENING.
        \param[in]  now: Currently active kernel task (ignored).
        \param[in]  next: Next kernel task.
        \param[out] idle: Stack of the task which must enter Idle state.
        \param[out] active: Stack of the task which must enter Active state (to which context will switch).
    */
    bool StateWake(KernelTask *now, KernelTask *next, Stack **idle, Stack **active)
    {
        (void)now;

        STK_ASSERT(next != nullptr);

        (*idle)   = &m_sleep_trap[0].stack;
        (*active) = next->GetUserStack();

        // if stack memory is exceeded these assertions will be hit
        STK_ASSERT(m_sleep_trap[0].memory[0] == STK_STACK_MEMORY_FILLER);
        STK_ASSERT(next->GetUserTask()->GetStack()[0] == STK_STACK_MEMORY_FILLER);

        m_task_now = next;

    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_OnTaskStartReady(next->GetUserStack()->tid);
    #endif

        if (_Mode & KERNEL_HRT)
        {
            next->HrtOnSwitchedIn();
        }

        return true; // switch context
    }

    /*! \brief      Enters into a sleeping mode.
        \note       FSM state: stk::FSM_STATE_SLEEPING.
        \param[in]  now: Currently active kernel task.
        \param[in]  next: Next kernel task (ignored).
        \param[out] idle: Stack of the task which must enter Idle state.
        \param[out] active: Stack of the task which must enter Active state (to which context will switch).
    */
    bool StateSleep(KernelTask *now, KernelTask *next, Stack **idle, Stack **active)
    {
        (void)next;

        STK_ASSERT(now != nullptr);
        STK_ASSERT(m_sleep_trap[0].stack.SP != 0);

        (*idle)   = now->GetUserStack();
        (*active) = &m_sleep_trap[0].stack;

        m_task_now = static_cast<KernelTask *>(m_strategy.GetFirst());

    #if STK_SEGGER_SYSVIEW
        SEGGER_SYSVIEW_OnTaskStopReady(now->GetUserStack()->tid, TRACE_EVENT_SLEEP);
    #endif

        if (_Mode & KERNEL_HRT)
        {
            if (!now->IsPendingRemoval())
                now->HrtOnSwitchedOut(&m_platform);
        }

        return true; // switch context
    }

    /*! \brief      Exits from scheduling.
        \note       FSM state: stk::FSM_STATE_EXITING.
        \note       Exits only if stk::KERNEL_DYNAMIC mode is specified, otherwise ignored.
        \param[in]  now: Currently active kernel task (ignored).
        \param[in]  next: Next kernel task (ignored).
        \param[out] idle: Stack of the task which must enter Idle state.
        \param[out] active: Stack of the task which must enter Active state (to which context will switch).
    */
    bool StateExit(KernelTask *now, KernelTask *next, Stack **idle, Stack **active)
    {
        (void)now;
        (void)next;

        if (_Mode & KERNEL_DYNAMIC)
        {
            // dynamic tasks are not supported if main processes's stack memory is not provided in Start()
            STK_ASSERT(m_exit_trap[0].stack.SP != 0);

            (*idle)   = nullptr;
            (*active) = &m_exit_trap[0].stack;

            m_task_now = nullptr;

            m_platform.Stop();
        }
        else
        {
            (void)idle;
            (void)active;
        }

        return false;
    }

    /*! \brief     Check if kernel was initialized with IKernel::Initialize().
        \return    True if initialized, otherwise false.
    */
    bool IsInitialized() const { return (m_request == REQUEST_NONE); }

    /*! \brief     Schedule processing of the add task request.
    */
    void ScheduleAddTask()
    {
        m_request |= REQUEST_ADD_TASK;
        __stk_full_memfence();
    }

#if STK_SEGGER_SYSVIEW
    /*! \brief      Send task trace info.
        \param[in]  task: Pointer to the kernel task.
    */
    void SendTaskTraceInfo(KernelTask *task)
    {
        STK_ASSERT(task->IsBusy());

        SEGGER_SYSVIEW_TASKINFO info =
        {
            .TaskID    = task->GetUserStack()->tid,
            .sName     = task->GetUserTask()->GetTraceName(),
            .Prio      = 0,
            .StackBase = reinterpret_cast<U32>(task->GetUserTask()->GetStack()),
            .StackSize = task->GetUserTask()->GetStackSizeBytes(),
        };
        SEGGER_SYSVIEW_SendTaskInfo(&info);
    }
#endif

    // If hit here: Kernel<N> expects at least 1 task, e.g. N > 0
    STK_STATIC_ASSERT_N(TASKS_MAX, TASKS_MAX > 0);

    // If hit here: Kernel mode must be assigned.
    STK_STATIC_ASSERT_N(KENREL_MODE_MUST_BE_SET, (_Mode != 0));

    // If hit here: KERNEL_STATIC and KERNEL_DYNAMIC can not be mixed, either one of these is possible.
    STK_STATIC_ASSERT_N(KENREL_MODE_MIX_NOT_ALLOWED,
        (((_Mode & KERNEL_STATIC) & (_Mode & KERNEL_DYNAMIC)) == 0));

    // If hit here: KERNEL_HRT must accompany KERNEL_STATIC or KERNEL_DYNAMIC.
    STK_STATIC_ASSERT_N(KENREL_MODE_HRT_ALONE, ((_Mode & KERNEL_HRT) == 0) ||
        ((_Mode & KERNEL_HRT) && ((_Mode & KERNEL_STATIC) || (_Mode & KERNEL_DYNAMIC))));

    /*! \typedef TaskStorageType
        \brief   KernelTask array type used as a storage for the KernelTask instances.
    */
    typedef KernelTask TaskStorageType[TASKS_MAX];

    /*! \class SleepTrapStack
        \brief Trap stack is used to execute the sleep trap when required.

        \note  Sleep trap - used to execute a sleep procedure of the driver when all user tasks are currently
               in a sleep state.
    */
    struct SleepTrapStack
    {
        typedef SleepTrapStackMemory::MemoryType Memory;

        Stack  stack;  //!< stack information
        Memory memory; //!< stack memory
    };

    /*! \class ExitTrapStack
        \brief Trap stack is used to execute the exit trap when required.

        \note  Exit trap - used for an exit into the main process from which IKernel::Start() was called
               when all tasks completed their processing and exited by returning from their Run function.
    */
    struct ExitTrapStack
    {
        typedef ExitTrapStackMemory::MemoryType Memory;

        Stack  stack;  //!< stack information
        Memory memory; //!< stack memory
    };

    /*! \typedef WaitObjectList
        \brief   List of wait objects.
    */
    typedef ISyncObject::ListHeadType SyncObjectList;

    KernelService     m_service;         //!< run-time kernel service
    _TyPlatform       m_platform;        //!< platform driver
    _TyStrategy       m_strategy;        //!< task switching strategy
    KernelTask       *m_task_now;        //!< current task task
    TaskStorageType   m_task_storage;    //!< task storage
    SleepTrapStack    m_sleep_trap[1];   //!< sleep trap
    ExitTrapStack     m_exit_trap[STK_ALLOCATE_COUNT(_Mode, KERNEL_DYNAMIC, 1, 0)]; //!< exit trap (does not occupy memory if kernel operation mode is not KERNEL_DYNAMIC)
    EFsmState         m_fsm_state;       //!< FSM state
    volatile uint32_t m_request;         //!< pending requests from the tasks
    SyncObjectList    m_sync_list[STK_ALLOCATE_COUNT(_Mode, KERNEL_SYNC, 1, 0)]; //!< list of sync objects

    const EFsmState m_fsm[FSM_STATE_MAX][FSM_EVENT_MAX] = {
    //    FSM_EVENT_SWITCH     FSM_EVENT_SLEEP     FSM_EVENT_WAKE    FSM_EVENT_EXIT
        { FSM_STATE_SWITCHING, FSM_STATE_SLEEPING, FSM_STATE_NONE,   FSM_STATE_EXITING }, // FSM_STATE_SWITCHING
        { FSM_STATE_NONE,      FSM_STATE_NONE,     FSM_STATE_WAKING, FSM_STATE_EXITING }, // FSM_STATE_SLEEPING
        { FSM_STATE_SWITCHING, FSM_STATE_SLEEPING, FSM_STATE_NONE,   FSM_STATE_EXITING }, // FSM_STATE_WAKING
        { FSM_STATE_NONE,      FSM_STATE_NONE,     FSM_STATE_NONE,   FSM_STATE_NONE }     // FSM_STATE_EXITING
    }; //!< FSM state table (Kernel implements table-based FSM)
};

} // namespace stk

#endif /* STK_H_ */
