#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"
#include "fs.h"
#define MAX_SYSCALL_NUM 500

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
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

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}
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

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_spawn(uint64 va)
{
	struct proc *p = curr_proc();
	char name[200];
	copyinstr(p->pagetable, name, va, 200);

	struct inode *ip = namei(name);
	if (ip == NULL)
		return -1;

	struct proc *np = allocproc();
	if (np == NULL) {
		iput(ip);
		return -1;
	}

	if (bin_loader(ip, np) < 0) {
		iput(ip);
		freeproc(np);
		return -1;
	}
	iput(ip);

	np->parent = p;
	for (int i = 0; i < FD_BUFFER_SIZE; i++) {
		if (p->files[i] != NULL) {
			p->files[i]->ref++;
			np->files[i] = p->files[i];
		}
	}
	np->task_info.status = Ready;
	np->task_info.time = 0;
	for (int i = 0; i < MAX_SYSCALL_NUM; i++)
		np->task_info.syscall_times[i] = 0;

	add_task(np);
	return np->pid;
}

uint64 sys_set_priority(long long prio)
{
	// TODO: your job is to complete the sys call
	if (prio < 2)
		return -1;
	struct proc *p = curr_proc();
	p->priority = (int)prio;
	return prio;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

int sys_fstat(int fd,uint64 stat){
	//TODO: your job is to complete the syscall
	//return -1;
	struct proc *p = curr_proc();
	if (fd < 0 || fd >= FD_BUFFER_SIZE)
		return -1;
	struct file *f = p->files[fd];
	if (f == 0 || f->type != FD_INODE)
		return -1;
	Stat *kst = (Stat *)useraddr(p->pagetable, stat);
	if (kst == 0)
		return -1;
	struct inode *ip = f->ip;
	ivalid(ip);
	kst->dev = 0;
	kst->ino = ip->inum;
	kst->mode = (ip->type == T_DIR) ? DIR : FILE;
	kst->nlink = ip->nlink;
	memset(kst->pad, 0, sizeof(kst->pad));
	return 0;
}

int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint64 flags){
	//TODO: your job is to complete the syscall
	struct proc *p = curr_proc();
	char old[200], newp[200];
	copyinstr(p->pagetable, old, oldpath, 200);
	copyinstr(p->pagetable, newp, newpath, 200);

	// Look up the existing file
	struct inode *ip = namei(old);
	if (ip == 0)
		return -1;
	ivalid(ip);

	// Cannot hard-link a directory
	if (ip->type == T_DIR) {
		iput(ip);
		return -1;
	}

	// Linking a file with the same name is an error
	if (strncmp(old, newp, DIRSIZ) == 0) {
		iput(ip);
		return -1;
	}

	// Get root directory and create new directory entry
	struct inode *dp = root_dir();
	if (dirlink(dp, newp, ip->inum) < 0) {
		iput(dp);
		iput(ip);
		return -1;
	}

	// Increment link count and persist
	ip->nlink++;
	iupdate(ip);

	iput(dp);
	iput(ip);
	return 0;
}

int sys_unlinkat(int dirfd, uint64 name, uint64 flags){
	//TODO: your job is to complete the syscall
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, name, 200);

	struct inode *ip = namei(path);
	if (ip == 0)
		return -1;
	ivalid(ip);

	struct inode *dp = root_dir();
	if (dirunlink(dp, path) < 0) {
		iput(dp);
		iput(ip);
		return -1;
	}
	iput(dp);

	ip->nlink--;
	iupdate(ip);
	iput(ip);
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

	struct proc *p = curr_proc();
	if (id >= 0 && id < MAX_SYSCALL_NUM) {
		p->task_info.syscall_times[id]++;
	}

	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	case SYS_taskinfo:
		ret = sys_task_info((TaskInfo *)args[0]);
		break;
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], (int)args[2], (int)args[3], (int)args[4]);
		break;
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
	case SYS_setpriority:
		ret = sys_set_priority((long long)args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
