#ifndef __THREADQUEUE_H__
#define __THREADQUEUE_H__

#ifdef USE_LIBUV
# include <uv.h>
# define thread_t uv_thread_t
# define thread_create(t, f, a) uv_thread_create(&(t), f, a)
# define thread_join(t) uv_thread_join(&(t))
# define thread_cb_t uv_thread_cb
# define mutex_t uv_mutex_t
# define mutex_init(m) uv_mutex_init(&(m))
# define mutex_destroy(m) uv_mutex_destroy(&(m))
# define mutex_lock(m) uv_mutex_lock(&(m))
# define mutex_unlock(m) uv_mutex_unlock(&(m))
# define condvar_t uv_cond_t
# define condvar_init(c) uv_cond_init(&(c))
# define condvar_destroy(c) uv_cond_destroy(&(c))
# define condvar_signal(c) uv_cond_signal(&(c))
#else
# include <thread>
# define thread_t std::thread
# define thread_create(t, f, a) t = std::thread(f, a)
# define thread_join(t) t.join()
# define thread_cb_t std::function<void(void*)>
# include <mutex>
# include <memory>
# define mutex_t std::unique_ptr<std::mutex>
# define mutex_init(m) m = std::unique_ptr<std::mutex>(new std::mutex())
# define mutex_destroy(m)
# define mutex_lock(m) m->lock()
# define mutex_unlock(m) m->unlock()
# include <condition_variable>
# define condvar_t std::unique_ptr<std::condition_variable>
# define condvar_init(c) c = std::unique_ptr<std::condition_variable>(new std::condition_variable())
# define condvar_destroy(c)
# define condvar_signal(c) c->notify_one()
#endif
#include <queue>

template<typename T>
class ThreadMessageQueue {
	std::queue<T> q;
	mutex_t mutex;
	condvar_t cond;
	bool skipDestructor;
	
	// disable copy constructor
	ThreadMessageQueue(const ThreadMessageQueue&);
	ThreadMessageQueue& operator=(const ThreadMessageQueue&);
	// ...but allow moves
	void move(ThreadMessageQueue& other) {
		q = std::move(other.q);
#ifdef USE_LIBUV
		mutex = other.mutex;
		cond = other.cond;
#else
		mutex = std::move(other.mutex);
		cond = std::move(other.cond);
#endif
		skipDestructor = false;
		
		other.skipDestructor = true;
	}
	
	
public:
	ThreadMessageQueue() {
		mutex_init(mutex);
		condvar_init(cond);
		skipDestructor = false;
	}
	~ThreadMessageQueue() {
		if(skipDestructor) return;
		mutex_destroy(mutex);
		condvar_destroy(cond);
	}
	
	ThreadMessageQueue(ThreadMessageQueue&& other) noexcept {
		move(other);
	}
	ThreadMessageQueue& operator=(ThreadMessageQueue&& other) noexcept {
		move(other);
		return *this;
	}
	void push(T item) {
		mutex_lock(mutex);
		q.push(item);
		condvar_signal(cond);
		mutex_unlock(mutex);
	}
	template<class Iterable>
	void push_multi(const Iterable& list) {
		mutex_lock(mutex);
		for(auto it = list.cbegin(); it != list.cend(); ++it) {
			q.push(*it);
		}
		condvar_signal(cond);
		mutex_unlock(mutex);
	}
	T pop() {
#ifdef USE_LIBUV
		mutex_lock(mutex);
		while(q.empty()) {
			uv_cond_wait(&cond, &mutex);
		}
		T item = q.front();
		q.pop();
		mutex_unlock(mutex);
#else
		std::unique_lock<std::mutex> lk(*mutex);
		cond->wait(lk, [this]{ return !q.empty(); });
		T item = q.front();
		q.pop();
#endif
		return item;
	}
	
	bool trypop(T* item) {
		bool notEmpty;
		mutex_lock(mutex);
		notEmpty = !q.empty();
		if(notEmpty) {
			*item = q.front();
			q.pop();
		}
		mutex_unlock(mutex);
		return notEmpty;
	}
	
	size_t size() {
		mutex_lock(mutex);
		size_t s = q.size();
		mutex_unlock(mutex);
		return s;
	}
	bool empty() {
		mutex_lock(mutex);
		bool e = q.empty();
		mutex_unlock(mutex);
		return e;
	}
};


#ifdef USE_LIBUV
struct tnqCloseWrap {
	void(*cb)(void*);
	void* data;
};

template<class P>
class ThreadNotifyQueue {
	ThreadMessageQueue<void*> q;
	uv_async_t a;
	P* o;
	void (P::*cb)(void*);
	
	static void notified(uv_async_t *handle) {
		auto self = static_cast<ThreadNotifyQueue*>(handle->data);
		void* notification;
		while(self->q.trypop(&notification))
			(self->o->*(self->cb))(notification);
	}
public:
	explicit ThreadNotifyQueue(uv_loop_t* loop, P* object, void (P::*callback)(void*)) {
		uv_async_init(loop, &a, notified);
		a.data = static_cast<void*>(this);
		cb = callback;
		o = object;
	}
	
	void notify(void* item) {
		q.push(item);
		uv_async_send(&a);
	}
	
	void close(void* data, void(*closeCb)(void*)) {
		auto* d = new tnqCloseWrap;
		d->cb = closeCb;
		d->data = data;
		a.data = d;
		uv_close(reinterpret_cast<uv_handle_t*>(&a), [](uv_handle_t* handle) {
			auto* d = static_cast<tnqCloseWrap*>(handle->data);
			d->cb(d->data);
			delete d;
		});
	}
	void close() {
		uv_close(reinterpret_cast<uv_handle_t*>(&a), nullptr);
	}
};
#endif

#if defined(_WINDOWS) || defined(__WINDOWS__) || defined(_WIN32) || defined(_WIN64)
# define NOMINMAX
# define WIN32_LEAN_AND_MEAN
# include <Windows.h>
#else
# include <pthread.h>
#endif
#if defined(__linux) || defined(__linux__)
# include <unistd.h>
#endif
class MessageThread {
	ThreadMessageQueue<void*> q;
	thread_t thread;
	bool threadActive;
	bool threadCreated;
	thread_cb_t cb;
	
	static void thread_func(void* parent) {
		MessageThread* self = static_cast<MessageThread*>(parent);
		ThreadMessageQueue<void*>& q = self->q;
		auto cb = self->cb;
		
		void* item;
		while((item = q.pop()) != NULL) {
			cb(item);
		}
	}
	
	static void thread_func_low_prio(void* parent) {
		#if defined(_WINDOWS) || defined(__WINDOWS__) || defined(_WIN32) || defined(_WIN64)
		HANDLE hThread = GetCurrentThread();
		switch(GetThreadPriority(hThread)) {
			case THREAD_PRIORITY_TIME_CRITICAL:
				SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);
				break;
			case THREAD_PRIORITY_HIGHEST:
				SetThreadPriority(hThread, THREAD_PRIORITY_ABOVE_NORMAL);
				break;
			case THREAD_PRIORITY_ABOVE_NORMAL:
				SetThreadPriority(hThread, THREAD_PRIORITY_NORMAL);
				break;
			case THREAD_PRIORITY_NORMAL:
				SetThreadPriority(hThread, THREAD_PRIORITY_BELOW_NORMAL);
				break;
			case THREAD_PRIORITY_BELOW_NORMAL:
				SetThreadPriority(hThread, THREAD_PRIORITY_LOWEST);
				break;
			case THREAD_PRIORITY_LOWEST:
				SetThreadPriority(hThread, THREAD_PRIORITY_IDLE);
				break;
			case THREAD_PRIORITY_IDLE: // can't go lower
			default: // do nothing
				break;
		}
		#else
		// it seems that threads cannot have lower priority on POSIX, unless it's scheduled realtime, however we can declare it to be CPU intensive
		int policy;
		struct sched_param param;
		pthread_t self = pthread_self();
		if(!pthread_getschedparam(self, &policy, &param)) {
			if(policy == SCHED_OTHER) {
				#ifdef __MACH__
				// MacOS doesn't support SCHED_BATCH, but does seem to permit priorities on SCHED_OTHER
				int min = sched_get_priority_min(policy);
				if(min < param.sched_priority) {
					param.sched_priority -= 1;
					if(param.sched_priority < min) param.sched_priority = min;
					pthread_setschedparam(self, policy, &param);
				}
				#else
				pthread_setschedparam(self, SCHED_BATCH, &param);
				#endif
			}
		}
		
		# if defined(__linux) || defined(__linux__)
		// ...but Linux allows per-thread priority
		nice(1);
		# endif
		#endif
		thread_func(parent);
	}
	
	// disable copy constructor
	MessageThread(const MessageThread&);
	MessageThread& operator=(const MessageThread&);
	// ...but allow moves
	void move(MessageThread& other) {
		q = std::move(other.q);
#ifdef USE_LIBUV
		thread = other.thread;
#else
		thread = std::move(other.thread);
#endif
		threadActive = other.threadActive;
		threadCreated = other.threadCreated;
		cb = other.cb;
		
		other.threadActive = false;
		other.threadCreated = false;
	}
	
public:
	bool lowPrio;
	MessageThread() {
		cb = NULL;
		threadActive = false;
		threadCreated = false;
		lowPrio = false;
	}
	MessageThread(thread_cb_t callback) {
		cb = callback;
		threadActive = false;
		threadCreated = false;
		lowPrio = false;
	}
	void setCallback(thread_cb_t callback) {
		cb = callback;
	}
	~MessageThread() {
		if(threadActive)
			q.push(NULL);
		if(threadCreated)
			thread_join(thread);
	}
	
	MessageThread(MessageThread&& other) noexcept {
		move(other);
	}
	MessageThread& operator=(MessageThread&& other) noexcept {
		move(other);
		return *this;
	}
	
	void start() {
		if(threadActive) return;
		threadActive = true;
		if(threadCreated) // previously created, but end fired, so need to wait for this thread to close before starting another
			thread_join(thread);
		threadCreated = true;
		thread_create(thread, lowPrio ? thread_func_low_prio : thread_func, this);
	}
	// item cannot be NULL
	void send(void* item) {
		start();
		q.push(item);
	}
	template<class Iterable>
	void send_multi(Iterable items) {
		start();
		q.push_multi(items);
	}
	void end() {
		if(threadActive) {
			q.push(NULL);
			threadActive = false;
		}
	}
	
	size_t size() {
		return q.size();
	}
	bool empty() {
		return q.empty();
	}
};

static inline int hardware_concurrency() {
#ifdef USE_LIBUV
	int threads;
#if UV_VERSION_HEX >= 0x12c00  // 1.44.0
	threads = uv_available_parallelism();
#else
	uv_cpu_info_t *info;
	uv_cpu_info(&info, &threads);
	uv_free_cpu_info(info, threads);
#endif
	return threads;
#else
	return (int)std::thread::hardware_concurrency();
#endif
}

#endif // defined(__THREADQUEUE_H__)