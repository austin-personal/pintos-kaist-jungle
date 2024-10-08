#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"


// TIMER_FREQ 유효성 검사 주파수는 최저 19 최고 1000미만이어야 한다. /* See [8254] for hardware details of the 8254 timer chip. */
#if TIMER_FREQ < 19 // TIMER_FREQ = time/frequency to generate interrupts
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

// 시스템이 부팅된 이후 경과한 타이머 틱(tick)의 수를 저장
static int64_t ticks; 

// Number of loops per timer tick. Initialized by timer_calibrate().
static unsigned loops_per_tick;

// 함수 선언
static intr_handler_func timer_interrupt; // 인터럽트때마다 틱을 하나씩 올려주는 함수 based on 8254 Timer
static bool too_many_loops (unsigned loops); // 루프의 횟수가 너무 많은지를 판단하는 함수. 틱당 루프의 수를 조정하기 위한
static void busy_wait (int64_t loops); // 지정된 횟수만큼 바쁘게 루프를도는 함수 for 정확한 딜레이 of stopped thread
static void real_time_sleep (int64_t num, int32_t denom); // 주어진 시간 동안 대기(슬립)하는 함수

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
// 하드웨어 타이머를 설정하여 일정 주기마다 인터럽트를 발생시키는 것
void timer_init (void) {
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ; //타이머 칩에 입력되는 기본 클럭 주파수(

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "");
}

// 정확한 시간 지연을 구현하기 위해 loops_per_tick 값을 보정하는 역할 : Calibrating = 눈금으로 재는 / 보정하는
void timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	
	loops_per_tick = 1u << 10; // 2의 10승(1024)으로 타이머 틱 동안 실행할 수 있는 루프값 초기화
	while (!too_many_loops (loops_per_tick << 1)) { // too_many_loops = 총 루프가 한틱이 이상 걸렸는지 체크/
		loops_per_tick <<= 1; // 1틱을 넘지 않았다면 루프수를 2배로 증가
		ASSERT (loops_per_tick != 0);
	}
	// 위에서는 위의 8개의 비트들을 수정. 비유하자면, 큰자리 수를 구했다. 이젠 작은수를 구하자: 세밀조정
	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t timer_ticks (void) {
	enum intr_level old_level = intr_disable (); // 인터럽트를 비활성화,  이전 인터럽트 상태를 old_level 변수에 저장
	int64_t t = ticks; // 전역 변수 ticks의 현재 값을 t라는 지역 변수에 복사
	intr_set_level (old_level);//이전의 인터럽트 상태(old_level)를 복원
	barrier ();
	return t;
}

/* Returns the number of timer ticks elapsed(얼마나 시간이 지났는지) since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

/* Suspends execution for approximately TICKS timer ticks. */
// 현재 쓰레드를 특정 기간동안 재우기:인자 값 ticks는 목표 수면시간
void timer_sleep (int64_t ticks) {
	int64_t start = timer_ticks ();// 현재 틱 을 start로

	ASSERT (intr_get_level () == INTR_ON);

	if (ticks >0)
	{
		thread_sleep(ticks);
	}

}

/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;
	thread_tick ();
	if (thread_mlfqs) {
		mlfqs_increment_recent_cpu();  // Increment recent_cpu for the current thread

		if (ticks % TIMER_FREQ == 0) {  // Every second
			mlfqs_calculate_load_avg();  // Recalculate system load average
			mlfqs_recalculate_recent_cpu();  // Recalculate recent_cpu for all threads
		}

		if (ticks % 4 == 0) {  // Every 4 ticks, recalculate priorities
			mlfqs_recalculate_priority();  // Recalculate priorities of all threads
		}
	}

	thread_awake(ticks);  // Wake up sleeping threads
}

// 루프의 반복 횟수(loops)가 타이머 틱(tick) 하나를 초과하여 기다리게 하는지 여부를 확인
// busy_wait의 동작/기간을 평가: 
static bool too_many_loops (unsigned loops) {

	/* Wait for a timer tick. */
	int64_t start = ticks;

	// While: 현재 틱동안은 가만히 있어라
	while (ticks == start) 
		barrier (); // 컴파일러가 재정렬 방지 

	
	start = ticks; //다음 틱일때 다시 할당
	
	busy_wait (loops); // 자고 있는 쓰레드가 룹만큼, 계속 깨워야 되는지 확인하는 함수 (CPU 소모 with no working)

	/* If the tick count changed, we iterated too long. */
	barrier ();
	// 만약 틱 값이 바뀌었다면, 루프가 타이머 틱 하나 이상 지속되었음을 의미하므로 true를 반환, meaning that it was too long
	return start != ticks;
	// Else, return false which means it has not been too long
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
// 바쁜대기란: 프로그램이 특정 조건이 만족될 때까지 아무 일도 하지 않고 계속해서 반복문을 돌며 CPU를 점유하는 것
// 자고 있는 쓰레드가 일어나야 하는지 매 loop마다 확인 함
static void NO_INLINE busy_wait (int64_t loops) {
	while (loops-- > 0) // loops를 하나씩 줄임 0이 될때까지
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
// 주어진 시간 동안 스레드를 일시 정지
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
