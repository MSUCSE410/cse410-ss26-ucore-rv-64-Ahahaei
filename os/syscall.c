#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"
#define MAX_SYSCALL_NUM 500

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(TimeVal *val, int _tz) // TODO: implement sys_gettimeofday in pagetable. (VA to PA)
{
	// YOUR CODE
	// val->sec = 0;
	// val->usec = 0;

	/* The code in `ch3` will leads to memory bugs*/

	// uint64 cycle = get_cycle();
	// val->sec = cycle / CPU_FREQ;
	// val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	struct proc *p = curr_proc();
	TimeVal *kval = (TimeVal *)useraddr(p->pagetable, (uint64)val);
	if (kval == 0)
		return -1;
	uint64 cycle = get_cycle();
	kval->sec = cycle / CPU_FREQ;
	kval->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	return 0;
}

// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)
uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd)
{
	if (len == 0)
		return 0;
	if (len > (1UL << 30))
		return -1;
	if (port & ~0x7)
		return -1;
	if ((port & 0x7) == 0)
		return -1;
	if (start % PGSIZE != 0)
		return -1;

	uint64 npages = (len + PGSIZE - 1) / PGSIZE;
	struct proc *p = curr_proc();

	

	int perm = PTE_U;
	if (port & 1)
		perm |= PTE_R;
	if (port & 2)
		perm |= PTE_W;
	if (port & 4)
		perm |= PTE_X;

	// Allocate and map physical pages one at a time
	for (uint64 i = 0; i < npages; i++, start += PGSIZE) {
		void *pa = kalloc();
		memset(pa, 0, PGSIZE);
		if (mappages(p->pagetable, start, PGSIZE, (uint64)pa, perm) != 0) {
			kfree(pa);
			
			return -1;
		}
	}
	return 0;
}

uint64 sys_munmap(uint64 start, uint64 len)
{
	if (len == 0)
		return 0;
	if (len > (1UL << 30))
		return -1;
	if (start % PGSIZE != 0)
		return -1;

	uint64 npages = (len + PGSIZE - 1) / PGSIZE;
	struct proc *p = curr_proc();

	// must already be mapped
	for (uint64 va = start; va < start + npages * PGSIZE; va += PGSIZE) {
		if (walkaddr(p->pagetable, va) == 0)
			return -1;
	}

	uvmunmap(p->pagetable, start, npages, 1);
	return 0;
}

/*
* LAB1: you may need to define sys_task_info here
*/

int sys_task_info(TaskInfo *ti)
{
	struct proc *p = curr_proc();
	if (ti == 0)
		return -1;
	// translate to physical before writing
	TaskInfo *kti = (TaskInfo *)useraddr(p->pagetable, (uint64)ti);
	if (kti == 0)
		return -1;
	p->task_info.time = (get_cycle() - p->start_time) / (CPU_FREQ / 1000);
	switch (p->state) {
	case RUNNING:
		p->task_info.status = Running;
		break;
	case RUNNABLE:
		p->task_info.status = Ready;
		break;
	case SLEEPING:
		p->task_info.status = Ready;
		break;
	case USED:
		p->task_info.status = UnInit;
		break;
	case ZOMBIE:
		p->task_info.status = Exited;
		break;
	default:
		p->task_info.status = Ready;
	}
	// *ti = p->task_info;
	*kti = p->task_info;
	return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	/*
	* LAB1: you may need to update syscall counter for task info here
	*/
	struct proc *p = curr_proc();
	if (id >= 0 && id < MAX_SYSCALL_NUM) {
		p->task_info.syscall_times[id]++;
	}

	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday((TimeVal *)args[0], args[1]);
		break;
	/*
	* LAB1: you may need to add SYS_taskinfo case here
	*/
	case SYS_taskinfo:
		ret = sys_task_info((TaskInfo *)args[0]);
		break;
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], (int)args[2], (int)args[3], (int)args[4]);
		break;
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
