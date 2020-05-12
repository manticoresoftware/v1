//
// Copyright (c) 2017-2020, Manticore Software LTD (http://manticoresearch.com)
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "coroutine.h"
#include "sphinxstd.h"
#include <atomic>

#ifndef NDEBUG
#define BOOST_USE_VALGRIND 1
#endif

#ifdef BOOST_USE_VALGRIND
#ifndef HAVE_VALGRIND
#undef BOOST_USE_VALGRIND
#endif
#endif

#include <boost/context/detail/fcontext.hpp>

#if BOOST_USE_VALGRIND
#include <valgrind/valgrind.h>
#endif

#define LOG_LEVEL_DIAG false
#define LOG_LEVEL_DEBUGV false

namespace Threads {

static const size_t g_iStackSize = 1024 * 128; // stack size - 128K
#define LOG_COMPONENT_CORO "Stack: " << m_dStack.GetLength() << " (" << m_eState << ") "

//////////////////////////////////////////////////////////////
/// Coroutine - uses boost::context to switch between jobs
using namespace boost::context::detail;
class CoRoutine_c
{
	enum class State_e
	{
		Paused, Running, Finished,
	};

	State_e m_eState = State_e::Paused;
	Handler m_fnHandler;
	CSphFixedVector<BYTE> m_dStack;

#if BOOST_USE_VALGRIND
	unsigned m_uValgrindStackID = 0;
#endif

	using Ctx_t = fcontext_t;
	Ctx_t m_tCoroutineContext;
	Ctx_t m_tExternalContext;
	friend StringBuilder_c & operator<< ( StringBuilder_c &, CoRoutine_c::State_e);

private:
	void WorkerLowest ( Ctx_t tCtx )
	{
		m_tExternalContext = tCtx;
		m_eState = State_e::Running;
		m_fnHandler ();
		m_eState = State_e::Finished;
		YieldLowest ();
	}

	// goto back to external context
	void YieldLowest ()
	{
		m_tExternalContext = jump_fcontext ( m_tExternalContext, nullptr ).fctx;
	}

public:
	explicit CoRoutine_c ( Handler fnHandler, size_t iStack=0 ) : m_fnHandler ( std::move ( fnHandler )), m_dStack ( iStack? (int) iStack: g_iStackSize )
	{
#if BOOST_USE_VALGRIND
		m_uValgrindStackID = VALGRIND_STACK_REGISTER( m_dStack.begin(), &m_dStack.Last () );
#endif
		m_tCoroutineContext = make_fcontext ( &m_dStack.Last (), m_dStack.GetLength (), [] ( transfer_t pT )
		{
			static_cast<CoRoutine_c *>(pT.data)->WorkerLowest ( pT.fctx );
		} );
	}

#if BOOST_USE_VALGRIND
	~CoRoutine_c()
	{
		VALGRIND_STACK_DEREGISTER( m_uValgrindStackID );
	}
#endif

	void Run ()
	{
		LOG( DEBUGV, CORO ) << "Run";
		assert ( m_eState==State_e::Paused );
		m_tCoroutineContext = jump_fcontext ( m_tCoroutineContext, static_cast<void*>(this) ).fctx;
	}

	bool IsFinished () const
	{
		return m_eState==State_e::Finished;
	}

	// yield to external context
	void Yield_ ()
	{
		LOG( DEBUGV, CORO ) << "Yield_";
		assert ( m_eState==State_e::Running );
		m_eState = State_e::Paused;
		YieldLowest ();
		m_eState = State_e::Running;
	}

	BYTE* GetTopOfStack() const
	{
		return m_dStack.End();
	}

	int GetStackSize() const
	{
		return m_dStack.GetLength();
	}
};

StringBuilder_c & operator<< ( StringBuilder_c & dOut, CoRoutine_c::State_e eData )
{
	switch (eData) {
		case CoRoutine_c::State_e::Paused: return dOut << "Paused";
		case CoRoutine_c::State_e::Running: return dOut << "Running";
		case CoRoutine_c::State_e::Finished: return dOut << "Finished";
	}
	return dOut;
}


struct CoroState_t
{
	enum ESTATE : DWORD
	{
		Entered_e = 1, Done_e = 2,
	};

	std::atomic<DWORD> m_uState {Entered_e};

	bool IsDone ()
	{
		auto uState = m_uState.load ( std::memory_order_relaxed );
		return ( uState & Done_e )!=0;
	}

	// reset some flags and return previous state
	DWORD ResetFlags( DWORD uFlags )
	{
		return m_uState.fetch_and ( ~uFlags, std::memory_order_relaxed );
	}

	// set some flags and return previous state
	DWORD SetFlags ( DWORD uFlags )
	{
		return m_uState.fetch_or ( uFlags, std::memory_order_relaxed );
	}
};

#define LOG_COMPONENT_COROW "(" << m_tState.m_uState << ") "


class CoroWorker_c
{
	// our executor (thread pool, etc which provides Schedule(handler) method)
	Scheduler_i * m_pScheduler = nullptr;
	Keeper_t m_tKeepSchedulerAlive;
	Waiter_t m_tTracer; // may be set to trace this worker lifetime

	// our main execution coroutine
	CoRoutine_c m_tCoroutine;
	CoroState_t	m_tState;
	Handler 	m_fnProceeder = nullptr;

	// chain nested workers via TLS
	static thread_local CoroWorker_c * m_pTlsThis;
	CoroWorker_c * m_pPreviousWorker = nullptr;

	// RAII worker's keeper
	struct CoroGuard_t
	{
		CoroGuard_t ( CoroWorker_c * pWorker )
		{
			pWorker->m_pPreviousWorker = CoroWorker_c::m_pTlsThis;
			CoroWorker_c::m_pTlsThis = pWorker;
		}

		~CoroGuard_t ()
		{
			CoroWorker_c::m_pTlsThis = CoroWorker_c::m_pTlsThis->m_pPreviousWorker;
		}
	};

private:
	CoroWorker_c ( Handler fnHandler, Scheduler_i* pScheduler )
	: m_pScheduler ( pScheduler )
	, m_tKeepSchedulerAlive { pScheduler->KeepWorking () }
	, m_tCoroutine {std::move (fnHandler), 0 }
	{
		assert ( m_pScheduler );
	}

	CoroWorker_c ( Handler fnHandler, Scheduler_i* pScheduler, Waiter_t tTracer, size_t iStack=0 )
	: m_pScheduler ( pScheduler )
	, m_tKeepSchedulerAlive { pScheduler->KeepWorking () }
	, m_tTracer ( std::move(tTracer))
	, m_tCoroutine {std::move (fnHandler), iStack}
	{
		assert ( m_pScheduler );
	}

	void Run ()
	{
		{
			CoroGuard_t pThis (this);
			m_tCoroutine.Run();
		}
		if ( m_tCoroutine.IsFinished () )
		{
			delete this;
			return;
		}
		ResetEnteredAndReschedule ();
	}

	void ResetEnteredAndReschedule ()
	{
		auto uPrevState = m_tState.m_uState.load ( std::memory_order_relaxed );
		do
			if (( uPrevState & CoroState_t::Done_e )!=0 )
			{
				LOG ( DIAG, COROW ) << "ResetEnteredAndReschedule schedule because done";
				Schedule();
				return;
			}
		while (!m_tState.m_uState.compare_exchange_weak (
				uPrevState, uPrevState & ~CoroState_t::Entered_e, std::memory_order_relaxed ));
		///^ lock-free primitive. If m_tState.m_uState==uPrevState here, write new value with unset Entered_e.
		/// if not (another thread changed value before) - exec uPrevState=m_tState.m_uState and loop.
		if ( m_fnProceeder )
		{
			Handler fnProceeder = nullptr;
			Swap ( fnProceeder, m_fnProceeder );
			fnProceeder();
		}
	}

	void Schedule()
	{
		LOG ( DEBUGV, COROW ) << "CoroWorker_c::Schedule (" << m_pScheduler << ")";
		assert ( m_pScheduler );
		m_pScheduler->Schedule ( [this] { Run (); }, false ); // false means 'new task'
	}

	void ScheduleContinuation ()
	{
		LOG ( DIAG, COROW ) << "CoroWorker_c::ScheduleContinuation (" << m_pScheduler << ")";
		assert ( m_pScheduler );
		m_pScheduler->Schedule ( [this] { Run (); }, true ); // true means 'continuation'
	}

public:
	static void Start ( Handler fnHandler, Scheduler_i* pScheduler )
	{
		(new CoroWorker_c ( std::move ( fnHandler ), pScheduler ))->Schedule ();
	}

	static void Start ( Handler fnHandler, Scheduler_i* pScheduler, Waiter_t tWait )
	{
		(new CoroWorker_c ( std::move ( fnHandler ), pScheduler, std::move ( tWait )))->Schedule ();
	}

	static void StartContinuation ( Handler fnHandler, Scheduler_i * pScheduler, size_t iStack, Waiter_t tWait )
	{
		(new CoroWorker_c ( std::move(fnHandler), pScheduler, std::move(tWait), iStack))->ScheduleContinuation ();
	}

	void Restart ()
	{
		if (( m_tState.SetFlags ( CoroState_t::Entered_e ) & CoroState_t::Entered_e )==0 )
			Schedule ();
	}

	void RestartAsContinuation ()
	{
		if (( m_tState.SetFlags ( CoroState_t::Entered_e ) & CoroState_t::Entered_e )==0 )
			ScheduleContinuation ();
	}

	void Done()
	{
		if (( m_tState.SetFlags ( CoroState_t::Entered_e | CoroState_t::Done_e ) & CoroState_t::Entered_e )==0 )
			Schedule ();
	}

	void Reschedule()
	{
		Done();
		Yield_();
		m_tState.ResetFlags ( CoroState_t::Done_e );
	}

	void Yield_ ()
	{
		m_tCoroutine.Yield_ ();
	}

	void YieldWith ( Handler fnHandler )
	{
		m_fnProceeder = std::move (fnHandler);
		m_tCoroutine.Yield_ ();
	}

	void MoveTo ( Scheduler_i * pScheduler )
	{
		if ( m_pScheduler == pScheduler )
			return; // nothing to do

		m_pScheduler = pScheduler;
		Reschedule();
	}

	Handler Continuation()
	{
		return [this] { RestartAsContinuation (); };
	}

	Handler Restarter ()
	{
		return [this] { Restart (); };
	}

	BYTE * GetTopOfStack () const
	{
		return m_tCoroutine.GetTopOfStack();
	}

	int GetStackSize () const
	{
		return m_tCoroutine.GetStackSize ();
	}

	Scheduler_i * CurrentScheduler() const
	{
		return m_pScheduler;
	}

	static CoroWorker_c* CurrentWorker()
	{
		return m_pTlsThis;
	}
};
thread_local CoroWorker_c * CoroWorker_c::m_pTlsThis = nullptr;

CoroWorker_c * CoWorker ()
{
	auto pWorker = CoroWorker_c::CurrentWorker();
	assert ( pWorker && "function must be called from inside coroutine");
	return pWorker;
}


void CoGo ( Handler fnHandler, Scheduler_i * pScheduler )
{
	if ( !pScheduler )
		return;

	assert ( pScheduler );
	CoroWorker_c::Start ( std::move(fnHandler), pScheduler );
}

void CoGo ( Handler fnHandler, Waiter_t tSignaller )
{
	auto pScheduler = CoCurrentScheduler ();
	if ( !pScheduler )
		pScheduler = GetGlobalScheduler ();

	assert ( pScheduler );
	CoroWorker_c::Start ( std::move ( fnHandler ), pScheduler, std::move ( tSignaller ));
}

void CoContinueHandler ( Handler fnHandler, int iStack )
{
	auto pScheduler = CoCurrentScheduler();
	if ( !pScheduler )
		pScheduler = GetGlobalScheduler ();

	assert ( pScheduler );

	auto dWaiter = DefferedRestarter ();
	CoroWorker_c::StartContinuation ( std::move ( fnHandler ), pScheduler, iStack, dWaiter );
	WaitForDeffered ( std::move ( dWaiter ));
}

void CallCoroutine ( Handler fnHandler )
{
	auto pScheduler = GetGlobalScheduler ();
	CSphAutoEvent tEvent;
	auto dWaiter = Waiter_t ( nullptr, [&tEvent] ( void * ) { tEvent.SetEvent (); } );
	CoroWorker_c::Start ( std::move ( fnHandler ), pScheduler, std::move(dWaiter) );
	tEvent.WaitEvent ();
}


void CoYieldWith ( Handler fnHandler )
{
	CoWorker ()->YieldWith ( std::move(fnHandler) );
}

// Move current task to another scheduler. Say, from netloop to worker, or from plain worker to vip, etc.
void CoMoveTo ( Scheduler_i * pScheduler )
{
	CoWorker ()->MoveTo ( pScheduler );
}

// Async schedule self continuation.
// Invoking handler will schedule continuation of yielded coroutine and return immediately.
// Scheduled task ('goto continue...') will be pefromed by scheduler's worker (threadpool, etc.)
Handler CurrentContinuation ()
{
	return CoWorker()->Continuation();
}

// Async schedule continuation.
// Invoking handler will schedule continuation of yielded coroutine and return immediately.
// Scheduled task ('goto continue...') will be pefromed by scheduler's worker (threadpool, etc.)
Handler CurrentRestarter ()
{
	return CoWorker()->Restarter();
}

Waiter_t DefferedRestarter ()
{
	return Waiter_t ( nullptr, [fnProceed=CurrentContinuation ()] ( void * )
	{
		fnProceed ();
	} );
}

void WaitForDeffered ( Waiter_t&& dWaiter, Handler fnCleanup )
{
	// do nothing. Moved dWaiter will be released outside the coro after yield.
	CoYieldWith ( [capturedWaiter = std::move ( dWaiter ), &fnCleanup ] {
		if ( fnCleanup )
			fnCleanup();
	} );
}
}

void * sphMyStack ()
{
	auto pWorker = Threads::CoroWorker_c::CurrentWorker ();
	if (!pWorker)
		return sphThreadStack();
	return pWorker->GetTopOfStack ();
}

int sphMyStackSize ()
{
	auto pWorker = Threads::CoroWorker_c::CurrentWorker ();
	if ( !pWorker )
		return g_iThreadStackSize;
	return pWorker->GetStackSize ();
}

Threads::Scheduler_i * Threads::CoCurrentScheduler ()
{
	auto pWorker = Threads::CoroWorker_c::CurrentWorker ();
	if ( !pWorker )
		return nullptr;
	return pWorker->CurrentScheduler ();
}

Threads::CoThrottle_c::CoThrottle_c ( int64_t tmPeriodUs )
	: m_tmPeriodUs ( tmPeriodUs )
{
	m_tmTimestamp = sphMicroTimer() +m_tmPeriodUs;
}

bool Threads::CoThrottle_c::MaybeThrottle()
{
	auto tmNow = sphMicroTimer ();
	if ( tmNow < m_tmTimestamp )
		return false;

	m_tmTimestamp = tmNow +m_tmPeriodUs;
	CoWorker ()->Reschedule ();
	return true;
}