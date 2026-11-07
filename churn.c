/*
 * Split the fork/exec storm into its ingredients, one per run.
 * Run one copy per CPU (pinned) to test the "both CPUs at once" trigger:
 *
 *   churn <cpu> fault    mmap 8 MB, touch every page, munmap (PTE alloc/free, TLB, page faults)
 *   churn <cpu> fork     fork + _exit + wait (process create/teardown, no exec)
 *   churn <cpu> vfork    vfork + _exit + wait (new task, no page-table copy, no new mm)
 *   churn <cpu> clonevm  clone(CLONE_VM|SIGCHLD) + wait (concurrent child sharing mm, SIGCHLD reaping)
 *   churn <cpu> clonesh  clone(CLONE_VM|SIGHAND|FILES|FS|SIGCHLD) + wait (shares sighand/files/fs)
 *   churn <cpu> clonenosig  clone(no SIGCHLD) reaped with __WCLONE
 *   churn <cpu> forkign  SIGCHLD ignored, fork/_exit, never wait4 (kernel auto-reaps)
 *   churn <cpu> forkpoll fork, then poll waitpid(WNOHANG)+sched_yield (parent reaps, never sleeps)
 *   churn <cpu> forkign2 forkign without sched_yield (max rate, self-reaping)
 *   churn <cpu> clonethr raw CLONE_THREAD create, futex wait on CLEARTID (max rate thread lifecycle)
 *   churn <cpu> times    times(): thread_group_cputime_adjusted() -> cputime_adjust() 64x64/128 mul+div
 *   churn <cpu> rusage   getrusage(RUSAGE_SELF): same cputime_adjust path
 *   churn <cpu> waitn    waitpid(-1, WNOHANG) with no children: read_lock(&tasklist_lock)
 *   churn <cpu> pgid     setpgid(0,0): write_lock_irq(&tasklist_lock)
 *   churn <cpu> thread   pthread_create/join (CLONE_VM task, no new mm)
 *   churn <cpu> ctxsw    two processes ping-pong over pipes on this CPU (swpctx, no fork loop)
 *   churn <cpu> syscall  getppid in a loop (kernel entry/exit only)
 *   churn <cpu> reapwork clonenosig base + child spins before _exit (heavier
 *                        __exit_signal cstime/cutime merge write into parent's
 *                        signal_struct at reap time, same create/reap rate)
 *   churn <cpu> reappgid clonenosig base + child setpgid(0,0) before _exit
 *                        (extra PIDTYPE_PGID tasklist hash detach at reap,
 *                        same create/reap rate)
 *
 * reapwork/reappgid isolate release_task() sub-steps: can't literally split a
 * single kernel function from userspace, so instead vary the WEIGHT of one
 * sub-step (signal_struct stat-merge magnitude, or PGID hash detach) while
 * holding the clonenosig baseline's create/reap rate fixed, to see if wedge
 * onset speeds up/slows down relative to plain clonenosig.
 *
 * build: cc -O2 -pthread -o churn churn.c
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <linux/futex.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAP (8UL << 20)

static int cl(void *a)
{
	return 0;
}

static int cl_work(void *a)
{
	volatile unsigned long i, s = 0;

	for (i = 0; i < 200000; i++)
		s += i;
	return (int)s & 0;
}

static int cl_pgid(void *a)
{
	setpgid(0, 0);
	return 0;
}

static void *thr(void *a)
{
	return a;
}

static void pin(int cpu)
{
	cpu_set_t s;

	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	if (sched_setaffinity(0, sizeof(s), &s))
		perror("sched_setaffinity");
}

int main(int argc, char **argv)
{
	long pg = sysconf(_SC_PAGESIZE), i;
	unsigned long n = 0;
	int cpu, a[2], b[2];
	char c = 'x';

	if (argc < 3) {
		fprintf(stderr, "usage: %s cpu fault|fork|vfork|clonevm|clonesh|clonenosig|forkign|times|rusage|forkign2|clonethr|forkpoll|waitn|pgid|thread|ctxsw|syscall|reapwork|reappgid\n", argv[0]);
		return 1;
	}
	cpu = atoi(argv[1]);
	pin(cpu);

	if (!strcmp(argv[2], "fault")) {
		for (;; n++) {
			char *p = mmap(NULL, MAP, PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			for (i = 0; i < (long)MAP; i += pg)
				p[i] = 1;
			munmap(p, MAP);
		}
	} else if (!strcmp(argv[2], "fork")) {
		for (;; n++) {
			pid_t p = fork();

			if (!p)
				_exit(0);
			waitpid(p, NULL, 0);
		}
	} else if (!strcmp(argv[2], "vfork")) {
		for (;; n++) {
			pid_t p = vfork();

			if (!p)
				_exit(0);
			waitpid(p, NULL, 0);
		}
	} else if (!strcmp(argv[2], "clonevm")) {
		static char stk[65536];

		for (;; n++) {
			pid_t p = clone(cl, stk + sizeof(stk),
					CLONE_VM | SIGCHLD, NULL);

			if (p > 0)
				waitpid(p, NULL, 0);
		}
	} else if (!strcmp(argv[2], "clonesh")) {
		static char stk2[65536];

		for (;; n++) {
			pid_t p = clone(cl, stk2 + sizeof(stk2),
					CLONE_VM | CLONE_SIGHAND | CLONE_FILES |
					CLONE_FS | SIGCHLD, NULL);

			if (p > 0)
				waitpid(p, NULL, 0);
		}
	} else if (!strcmp(argv[2], "clonenosig")) {
		static char stk3[65536];

		for (;; n++) {
			pid_t p = clone(cl, stk3 + sizeof(stk3),
					CLONE_VM | CLONE_SIGHAND | CLONE_FILES |
					CLONE_FS, NULL);

			if (p > 0)
				waitpid(p, NULL, __WCLONE);
		}
	} else if (!strcmp(argv[2], "forkign")) {
		signal(SIGCHLD, SIG_IGN);
		for (;; n++) {
			pid_t p = fork();

			if (!p)
				_exit(0);
			sched_yield();
		}
	} else if (!strcmp(argv[2], "forkpoll")) {
		for (;; n++) {
			pid_t p = fork();

			if (!p)
				_exit(0);
			while (waitpid(p, NULL, WNOHANG) == 0)
				sched_yield();
		}
	} else if (!strcmp(argv[2], "forkign2")) {
		signal(SIGCHLD, SIG_IGN);
		for (;; n++) {
			pid_t p = fork();

			if (!p)
				_exit(0);
		}
	} else if (!strcmp(argv[2], "clonethr")) {
		static char stk4[65536];
		static volatile pid_t tid;

		for (;; n++) {
			pid_t p = clone(cl, stk4 + sizeof(stk4),
					CLONE_VM | CLONE_SIGHAND | CLONE_THREAD |
					CLONE_FILES | CLONE_FS | CLONE_SYSVSEM |
					CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID,
					NULL, &tid, NULL, &tid);

			if (p > 0)
				while (tid)
					syscall(SYS_futex, &tid, FUTEX_WAIT,
						tid, NULL, NULL, 0);
		}
	} else if (!strcmp(argv[2], "times")) {
		struct tms t;

		for (;; n++)
			times(&t);
	} else if (!strcmp(argv[2], "rusage")) {
		struct rusage ru;

		for (;; n++)
			getrusage(RUSAGE_SELF, &ru);
	} else if (!strcmp(argv[2], "waitn")) {
		for (;; n++)
			waitpid(-1, NULL, WNOHANG);
	} else if (!strcmp(argv[2], "pgid")) {
		for (;; n++)
			setpgid(0, 0);
	} else if (!strcmp(argv[2], "thread")) {
		for (;; n++) {
			pthread_t t;

			if (pthread_create(&t, NULL, thr, NULL))
				continue;
			pthread_join(t, NULL);
		}
	} else if (!strcmp(argv[2], "reapwork")) {
		static char stk5[65536];

		for (;; n++) {
			pid_t p = clone(cl_work, stk5 + sizeof(stk5),
					CLONE_VM | CLONE_SIGHAND | CLONE_FILES |
					CLONE_FS, NULL);

			if (p > 0)
				waitpid(p, NULL, __WCLONE);
		}
	} else if (!strcmp(argv[2], "reappgid")) {
		static char stk6[65536];

		for (;; n++) {
			pid_t p = clone(cl_pgid, stk6 + sizeof(stk6),
					CLONE_VM | CLONE_SIGHAND | CLONE_FILES |
					CLONE_FS, NULL);

			if (p > 0)
				waitpid(p, NULL, __WCLONE);
		}
	} else if (!strcmp(argv[2], "ctxsw")) {
		if (pipe(a) || pipe(b))
			return 1;
		if (!fork()) {
			for (;;) {
				if (read(a[0], &c, 1) != 1)
					_exit(0);
				write(b[1], &c, 1);
			}
		}
		for (;; n++) {
			write(a[1], &c, 1);
			read(b[0], &c, 1);
		}
	} else {
		for (;; n++)
			getppid();
	}
	return 0;
}
