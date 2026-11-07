# alpha-reap-churn

A small stress tool that wedges an AlphaServer ES47/GS1280 (EV7, two CPUs) within
seconds, and a request: if you have an Alpha, please run it and tell me what
happens.

On the machine this was written for, running one pinned process per CPU that
creates and reaps child tasks in a tight loop hangs the whole system, usually in
3 to 15 seconds. There is no panic and no console output. The cause is unknown.
It still happens after replacing the RAM, VRMs, CPU modules, DUO board and
management board, and with each power supply enabled alone, so it does not look
like a simple faulty part on that one machine. The
interesting question is whether other Alphas, with other kernels, firmware and
hardware, show it too.

## What the trigger is

The failing case needs the parent-side `wait4()` reap (`wait_task_zombie()` then
`release_task()`) running on **both CPUs at the same time**. Child creation and
reaping on a single CPU does not do it, and neither do the self-reaping paths
(`SIGCHLD` ignored, thread lifecycle). Each mode is a few lines in `churn.c`.

Results on the original machine (kernel 7.x, two EV7 CPUs, one pinned copy per
CPU):

| Mode (same on both CPUs) | Result |
|---|---|
| `clonesh`, `clonenosig`, `clonevm` | wedge in about 3 to 15 s |
| `vfork`, `fork` | wedge in about 10 s to 10 min |
| `forkign`, `forkign2`, `clonethr`, `thread`, `ctxsw`, `fault`, `syscall`, `times`, `rusage`, `waitn`, `pgid` | survive 45 s or longer |
| one CPU only, any mode | survives |

## Build and run

```
make
./churnseq.sh 300 clonenosig
```

`clonenosig` is the fastest reliable mode. Needs two or more CPUs, a C compiler
and a Linux kernel. No root is required. Use `CPUS="2 3"` to pin to other CPUs.

Every 5 seconds the script appends a line to `churn-heartbeat.log` and syncs it.
After a hang and reboot, `tail churn-heartbeat.log` shows the last mode and time
that was reached. Other modes:

```
./churnseq.sh 60 clonenosig clonesh fork forkign
./churnpair.sh 60 clonenosig:syscall clonenosig:fault   # different mode per CPU
./churn 0 clonenosig                                    # one copy, by hand
```

## Please read before running

This can hang a machine hard enough that only a power cycle recovers it. Run it
on a machine you can reset, not on anything in production. It does not write
to disk besides the heartbeat file, but an unclean reset can still cost you a
filesystem check. Run `sync` first.

If your Alpha runs it for several minutes with no problem, that is a useful
result too.

## What to send back

- Hang or no hang, and the last heartbeat line if it hung (or how long it ran).
- Machine model, number of CPUs and CPU type (`/proc/cpuinfo`).
- Kernel version and config (`/proc/config.gz`) and distribution.
- Console firmware version (SRM `show version`) and PALcode if you know it.
- Mode(s) you ran and whether you changed `CPUS`.

Runs on other architectures are also welcome as a control. On x86 the expected
result is that it simply runs.

## Files

- `churn.c`: the workload. One process per CPU, pinned, running one mode.
- `churnseq.sh`: runs modes back to back on all chosen CPUs with the heartbeat.
- `churnpair.sh`: same, with a different mode on each of two CPUs.
- `Makefile`: `make` builds `churn`.

## License

BSD 2-Clause, see `LICENSE`.
