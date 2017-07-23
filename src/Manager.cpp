#include <fjs/Manager.h>
#include <fjs/Thread.h>
#include <fjs/Fiber.h>
#include <thread>

fjs::Manager::Manager(const ManagerOptions& options) :
	m_numThreads(options.NumThreads),
	m_numFibers(options.NumFibers),
	m_highPriorityQueue(options.HighPriorityQueueSize),
	m_normalPriorityQueue(options.NormalPriorityQueueSize),
	m_lowPriorityQueue(options.LowPriorityQueueSize)
{}

fjs::Manager::~Manager()
{
	// TODO
}

fjs::Manager::ReturnCode fjs::Manager::Run(Main_t main)
{
	if (m_threads || m_fibers)
		return ReturnCode::AlreadyInitialized;

	// Threads
	m_threads = new Thread[m_numThreads];

	// Current (Main) Thread
	m_threads[0].FromCurrentThread();

	auto mainThread = &m_threads[0];
	auto mainThreadTLS = mainThread->GetTLS();
	mainThreadTLS->ThreadFiber.FromCurrentThread();

	// Create Fibers
	// This has to be done after Thread is converted to Fiber!
	if (m_numFibers == 0)
		return ReturnCode::InvalidNumFibers;

	m_fibers = new Fiber[m_numFibers];
	m_idleFibers = new std::atomic_bool[m_numFibers];

	for (uint16_t i = 0; i < m_numFibers; i++)
	{
		m_fibers[i].Reset(FiberCallback_Worker);
		m_idleFibers[i].store(true, std::memory_order_relaxed);
	}

	// Spawn Threads
	for (uint8_t i = 1; i < m_numThreads; i++) // offset 1 because 0 is current thread
	{
		if (!m_threads[i].Spawn(ThreadCallback_Worker, this))
			return ReturnCode::OSError;
	}

	// Main
	if (main == nullptr)
		return ReturnCode::NullCallback;

	m_mainCallback = main;
	
	// Setup main Fiber
	mainThreadTLS->CurrentFiberIndex = FindFreeFiber();
	auto mainFiber = &m_fibers[mainThreadTLS->CurrentFiberIndex];
	mainFiber->Reset(FiberCallback_Main);

	mainThreadTLS->ThreadFiber.SwitchTo(mainFiber, this);

	// Wait for all Threads to shut down
	for (uint8_t i = 1; i < m_numThreads; i++)
		m_threads[i].Join();
	
	// Done
	return ReturnCode::Succes;
}

void fjs::Manager::Shutdown(bool blocking)
{
	m_shuttingDown.store(true, std::memory_order_release);

	if (blocking)
	{
		for (uint8_t i = 1; i < m_numThreads; i++)
			m_threads[i].Join();
	}
}

uint16_t fjs::Manager::FindFreeFiber()
{
	while (true)
	{
		for (uint16_t i = 0; i < m_numFibers; i++)
		{
			if (!m_idleFibers[i].load(std::memory_order_relaxed) ||
				!m_idleFibers[i].load(std::memory_order_acquire))
				continue;

			bool expected = true;
			if (std::atomic_compare_exchange_weak_explicit(&m_idleFibers[i], &expected, false, std::memory_order_release, std::memory_order_relaxed)) {
				return i;
			}
		}

		// TODO: Add Debug Counter and error message
	}
}

void fjs::Manager::CleanupPreviousFiber(TLS* tls)
{
	if (tls == nullptr)
		tls = GetCurrentTLS();

	switch (tls->PreviousFiberDestination)
	{
	case FiberDestination::None:
		return;

	case FiberDestination::Pool:
		m_idleFibers[tls->PreviousFiberIndex].store(true, std::memory_order_release);
		break;

	case FiberDestination::Waiting:
		tls->PreviousFiberStored->store(true, std::memory_order_relaxed);
		break;

	default:
		break;
	}

	// Cleanup TLS
	tls->PreviousFiberIndex = UINT16_MAX;
	tls->PreviousFiberDestination = FiberDestination::None;
	tls->PreviousFiberStored = nullptr;
}