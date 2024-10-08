#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
//(P1:convar)
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif
//(P:mlfqs)
#include "threads/fixed_point.h" // 고정 소수점 연산을 위한 헤더

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list; //준비 상태의 쓰레드의 리스트

static struct list sleep_list; //(p1): sleeping thread list

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED); // 시스템에 실행할 다른 스레드가 없을 때 실행. CPU가 항상 뭔가를 실행하고 있도록 보장
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);


// (P1: Alram clock): thread 재우고 깨우고
void thread_sleep(int64_t ticks);
void thread_awake(int64_t current_ticks);




/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
// 현재 실행중인 쓰레드 찾기
#define running_thread() ((struct thread *) (pg_round_down (rrsp ()))) //CPU의 현재 스택 포인터(RSP 레지스터)의 값을 읽는다.


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

//(P:mlfqs)
int load_avg;
struct list all_list;


/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock); //스레드 ID 할당을 위한 락을 초기화
	
	list_init (&destruction_req);//제거될 스레드들의 리스트를 초기화
	
	list_init (&sleep_list); //(P1): sleeping thread들 저장할 리스트
	list_init(&all_list);
	list_init(&ready_list);
	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread (); //시스템 부팅 직후부터 존재했던 초기 실행 흐름: 부트로더가 실행되어 커널을 메모리에 로드 -> 하나의 실행 흐름(즉, 스레드)이 존재
	init_thread (initial_thread, "main", PRI_DEFAULT); //(struct thread *t, const char *name, int priority)
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started); // idle 쓰레드 생성
	load_avg = LOAD_AVG_DEFAULT;

	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create (const char *name, int priority, thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	//(P1:mlfqs)
	//list_push_back(&thread_current()->child_list, &t->child_elem);
	// 부모 스레드의 nice,recent_cpu 값 상속
	struct thread *cur = thread_current ();
	t->nice = cur->nice;
	t->recent_cpu = cur->recent_cpu; 

	// (P2:syscall) fork
	t->parent = thread_current();
	list_push_back(&thread_current()->child_list, &t->child_elem);
	/* Add to run queue. */
	thread_unblock (t);
	preempt();

	return tid;
}
/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
//쓰레드를 재우기
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
// 쓰레드를 깨우기
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	//list_push_back (&ready_list, &t->elem);
	list_insert_ordered(&ready_list, &t->elem, priority_compare, NULL);
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	
	// //ready_list는 현제 쓰레드들이 일자로 쭉 나열되어 있음
	// // list_push_back으로 현재 쓰레드를 맨 뒤로 보냄
	
	// 	list_push_back (&ready_list, &curr->elem);
	if (curr != idle_thread) {
		list_insert_ordered(&ready_list, &curr->elem, priority_compare, NULL); // 우선순위 대로 삽입
	}
	
	
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */

void thread_set_priority(int new_priority)
{
	if (thread_mlfqs) return;
    thread_current()->org_priority = new_priority;
    update_priority();
    preempt();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
	ASSERT(nice >= -20 && nice <= 20);
	enum intr_level old_level = intr_disable();

	thread_current()->nice = nice;
	// 새 값에 기반하여 스레드의 우선순위를 재계산

	mlfqs_calculate_priority(thread_current());

	preempt();

	intr_set_level(old_level);
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	enum intr_level old_level = intr_disable();

	int nice = thread_current()->nice;

	intr_set_level(old_level);
	return nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	enum intr_level old_level = intr_disable();
	// 시스템 부하 평균의 100배를 가장 가까운 정수로 반올림하여 반환
	int load_avg_value = FP_TO_INT_ROUND(MUL_FP_INT(load_avg, 100));
	intr_set_level(old_level);
	return load_avg_value;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	enum intr_level old_level = intr_disable();
	// 현재 스레드의 recent_cpu 값의 100배를 가장 가까운 정수로 반올림하여 반환
	int recent_cpu_value = FP_TO_INT_ROUND((thread_current()->recent_cpu * 100));
	intr_set_level(old_level);
	return recent_cpu_value;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) { //UNUSED는 컴파일러 경고를 방지하기 위한 매크로
	struct semaphore *idle_started = idle_started_;// 세마포어에 대한 포인터

	idle_thread = thread_current (); // 현재 쓰레드를 idle로 저장
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
// 이 함수는 이미 실행 중인 초기 스레드에 대해 proper한 스레드 구조체를 설정합니다. 모든 스레드(초기 스레드 포함)가 일관된 구조를 가져야 하기 때문에
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t); // 초기 쓰레드 구조 초기화
	t->status = THREAD_BLOCKED; // 초기 쓰레드 상태 변경
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC; // 무결성 검사 이숫자를 넣어줌으로써 통과 된 쓰레드다 
	//(P1:mlfqs)
	if (!thread_mlfqs){
		//(P1:P-Donation) 
		t->org_priority = priority; // (P1:P-Donation)
		t->wait_on_lock = NULL; // (P1:P-Donation)
		list_init(&(t->donations));
	}else{
		//(P1:mlfqs)
		//mlfqs_calculate_priority(t);
		t->recent_cpu = RECENT_CPU_DEFAULT;
		t->nice = NICE_DEFAULT;
		list_push_back(&all_list, &t->allelem); // all_list에 initial 스레드 추가
	}

	//(P2:syscall)
	t->is_user = false;
	t->fd_table[0] = STDIN_FILENO;
	t->fd_table[1] = STDOUT_FILENO;
	t->fd_table[2] = STDERR_FILENO;
	for (int i = 3; i < 32; i++) 
	{
		t->fd_table[i] =NULL;
	}
	

	//(P2:syscall) fork
	list_init(&(t->child_list)); //parent child relationship
	sema_init(&t->load_sema, 0); //When child process is loading, parent should wait
	//(P2:syscall) wait
	sema_init(&t->exit_sema, 0); 
    sema_init(&t->wait_sema, 0);
	
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);

	// 파괴해야할 쓰레드 리스트가 빌때까지, 쓰레드(디스트럭트 리스트에 담긴 쓰레드들)를 죽여라
	while (!list_empty (&destruction_req)) {
		// victim은 삭제 할 쓰레드
		// 리스트엔트리를 통해 삭제할 쓰레드를 특정함
		struct thread *victim = list_entry (list_pop_front (&destruction_req), struct thread, elem);
		// 페이지 얼록 프리 페이지를 통해 그 victim을 메모리 반환함.
		palloc_free_page(victim);
	}
	// 
	thread_current ()->status = status;
	schedule ();
}

static void schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}

// (P1): 제울 쓰레드와 현재 sleep_list 의 ticks들 하나하나 비교 하는 함수
bool ticks_compare(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
    struct thread *sa = list_entry(a, struct thread, elem);
    struct thread *sb = list_entry(b, struct thread, elem);
    return sa->wakeup_tick < sb->wakeup_tick;
}
// (P1): 제울 쓰레드와 현재 sleep_list 의 priority들 하나하나 비교 하는 함수
bool
priority_compare(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) 
{
    struct thread *ta = list_entry(a, struct thread, elem);
    struct thread *tb = list_entry(b, struct thread, elem);
    return ta->priority > tb->priority;
}

// (P1) thread를 sleep(동작하지 않도록) 하고 sleep_list에 넣기
void thread_sleep(int64_t ticks) {
    struct thread *cur = thread_current();
    enum intr_level old_level;

    ASSERT(!intr_context());

    old_level = intr_disable();

	cur->wakeup_tick = timer_ticks() + ticks;// 일어날 시간 넣어주기
	
	list_insert_ordered(&sleep_list, &cur->elem, ticks_compare, NULL);// 리스트에 넣어주기
	thread_block(); // 현재 쓰래드 재우기

    intr_set_level(old_level);
}

// (P1): 잠자는 함수 깨우기
void thread_awake(int64_t current_ticks) {
    struct list_elem *e = list_begin(&sleep_list);

    while (e != list_end(&sleep_list)) {
        struct thread *t = list_entry(e, struct thread, elem);
        if (t->wakeup_tick <= current_ticks) {
            e = list_remove(e);
            thread_unblock(t);
        } else {
            // 리스트가 정렬되어 있으므로, 더 이상 깨울 스레드가 없으면 종료
            break;
        }
    }
}

// (P1:P) 현재 쓰레드의 우선순위가 감소 했을때 바로 CPU사용권 넘기기
void preempt(void) {
	struct thread *cur = thread_current(); // 현재 쓰레드

	if (list_empty(&ready_list)){
		return;
	}

	struct thread *next = list_entry(list_begin(&ready_list), struct thread, elem);

    // If the unblocked thread has a higher priority, force a yield immediately
    if (cur != idle_thread && cur->priority < next->priority) {
		if (intr_context()){
			// 인터럽트 컨텍스트에서 호출된 경우: 인터럽트 종료 후 스케줄링
			intr_yield_on_return();
		}
		else{
			// 일반 컨텍스트에서 호출된 경우: 즉시 스레드 양보
			thread_yield();
		}
    }
}


// (P:MLFQS)----------------------------------------------------


void mlfqs_calculate_priority(struct thread *t) {
	if (t == idle_thread)
		return;
	// 우선순위 계산을 위한 중간 값 계산
	int recent_cpu_term = DIV_FP_INT(t->recent_cpu, 4); // recent_cpu / 4
	int nice_term = INT_FP(t->nice * 2);				// nice * 2 (고정 소수점 변환)
	// 우선순위 계산
	int priority_fp = SUB_FP(SUB_FP(INT_FP(PRI_MAX), recent_cpu_term), nice_term);
	t->priority = FP_TO_INT(priority_fp);

	if (t->priority < PRI_MIN)
		t->priority = PRI_MIN;
	else if (t->priority > PRI_MAX)
		t->priority = PRI_MAX;
}



void mlfqs_calculate_recent_cpu(struct thread *t) {
    if (t == idle_thread) return;

    ASSERT(t->nice >= -20 && t->nice <= 20);

    // Coefficient calculation using fixed-point arithmetic
    int coef = DIV_FP(MUL_FP_INT(load_avg, 2), ADD_FP_INT(MUL_FP_INT(load_avg, 2), 1));

    // Update recent_cpu using the fixed-point macros
    t->recent_cpu = ADD_FP(MUL_FP(coef, t->recent_cpu), INT_FP(t->nice));
}


void mlfqs_calculate_load_avg(void){
	int ready_threads = list_size(&ready_list);
  
	if (thread_current () != idle_thread) ready_threads++;
	
	load_avg = ADD_FP(
		MUL_FP(DIV_FP(INT_FP(59), INT_FP(60)), load_avg),
		DIV_FP(INT_FP(ready_threads), INT_FP(60)));
	// load_avg = ADD_FP(MUL_FP(DIV_FP(INT_FP(59), INT_FP(60)), load_avg), MUL_FP(DIV_FP(INT_FP(1), INT_FP(60)), INT_FP(ready_threads)));
}

// ----

void mlfqs_increment_recent_cpu(void){
	if (thread_current() != idle_thread){
		thread_current()->recent_cpu = ADD_FP_INT(thread_current()->recent_cpu, 1);
	}
}

void mlfqs_recalculate_recent_cpu(void){
	struct list_elem *e;
	//c
	ASSERT(intr_get_level() == INTR_OFF);

	for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)){
		struct thread *t = list_entry(e, struct thread, allelem);
		mlfqs_calculate_recent_cpu(t);
	}
}

void mlfqs_recalculate_priority(void){
	struct list_elem *e;
	struct thread *t;


	for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)){
		
		t = list_entry(e, struct thread, allelem);
		mlfqs_calculate_priority(t);

	}
}