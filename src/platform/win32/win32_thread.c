struct Platform_Thread {
	HANDLE handle;
	Platform_Thread_Function *function;
	void *data;
};

struct Platform_Mutex {
	SRWLOCK lock;
};

struct Platform_Condition {
	CONDITION_VARIABLE condition;
};

static DWORD WINAPI platform_thread_entry(void *data)
{
	Platform_Thread *thread = data;
	return thread->function(thread->data);
}

Platform_Thread *platform_thread_create(Platform_Thread_Function *function, void *data)
{
	Platform_Thread *thread;
	if (!function) return NULL;
	thread = calloc(1, sizeof(*thread));
	if (!thread) return NULL;
	thread->function = function;
	thread->data = data;
	thread->handle = CreateThread(NULL, 0, platform_thread_entry, thread, 0, NULL);
	if (!thread->handle) {
		free(thread);
		return NULL;
	}
	return thread;
}

b32 platform_thread_join(Platform_Thread *thread)
{
	return thread && WaitForSingleObject(thread->handle, INFINITE) == WAIT_OBJECT_0;
}

void platform_thread_destroy(Platform_Thread *thread)
{
	if (!thread) return;
	CloseHandle(thread->handle);
	free(thread);
}

u64 platform_current_thread_id(void)
{
	return GetCurrentThreadId();
}

Platform_Mutex *platform_mutex_create(void)
{
	Platform_Mutex *mutex = calloc(1, sizeof(*mutex));
	if (mutex) InitializeSRWLock(&mutex->lock);
	return mutex;
}

void platform_mutex_destroy(Platform_Mutex *mutex)
{
	free(mutex);
}

void platform_mutex_lock(Platform_Mutex *mutex)
{
	AcquireSRWLockExclusive(&mutex->lock);
}

void platform_mutex_unlock(Platform_Mutex *mutex)
{
	ReleaseSRWLockExclusive(&mutex->lock);
}

Platform_Condition *platform_condition_create(void)
{
	Platform_Condition *condition = calloc(1, sizeof(*condition));
	if (condition) InitializeConditionVariable(&condition->condition);
	return condition;
}

void platform_condition_destroy(Platform_Condition *condition)
{
	free(condition);
}

b32 platform_condition_wait(Platform_Condition *condition, Platform_Mutex *mutex)
{
	return SleepConditionVariableSRW(&condition->condition, &mutex->lock, INFINITE, 0) != 0;
}

void platform_condition_signal(Platform_Condition *condition)
{
	WakeConditionVariable(&condition->condition);
}

void platform_condition_broadcast(Platform_Condition *condition)
{
	WakeAllConditionVariable(&condition->condition);
}
