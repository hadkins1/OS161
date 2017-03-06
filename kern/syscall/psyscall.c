#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <filetable.h>
#include <limits.h>
#include <kern/errno.h>
#include <machine/trapframe.h>
#include <cpu.h>

#include <kern/fcntl.h>
#include <lib.h>
#include <vm.h>
#include <vfs.h>
#include <syscall.h>
#include <test.h>
#include <kern/wait.h>
#include <copyinout.h>
#include <proc.h>
#include <psyscall.h>

/*
 * Gets the PID of the current process.
 */
int
sys_getpid(int32_t *retval0)
{
	lock_acquire(pidtable->pid_lock);

	*retval0 = curproc->pid;

	lock_release(pidtable->pid_lock);
	return 0;
}


/*
 * Function called by a parent process to wait until a child process exits.
 */
int
sys_waitpid(pid_t pid, int32_t *retval0, int32_t options)
{
	int status; // The status of the process which is being waited upon
	int waitcode; // The reason for process exit as defined in wait.h

	if (options != 0){
		return EINVAL;
	}

	if (pid < PID_MIN || pid > PID_MAX || pidtable->pid_status[pid] == READY){
		return ESRCH;
	}

	/* Check that the pid being called is a child of the current process */
	int ischild = 0;
	struct proc *child = pidtable->pid_procs[pid];
	int parentnum = array_num(curproc->children);
	for (int i = 0; i < parentnum; i++){
		if (child == array_get(curproc->children, i)){
			ischild = 1;
			break;
		}
	}
	if(ischild == 0){
		return ECHILD;
	}

	lock_acquire(pidtable->pid_lock);

	status = pidtable->pid_status[pid];
	while(status != ZOMBIE){
		cv_wait(pidtable->pid_cv, pidtable->pid_lock);
		status = pidtable->pid_status[pid];
	}
	waitcode = pidtable->pid_waitcode[pid];

	lock_release(pidtable->pid_lock);

	/* A NULL retval0 indicates that nothing is to be returned. */
	if(retval0 != NULL){
		int ret = copyout(&waitcode, (userptr_t) retval0, sizeof(int32_t));
		if (ret){
			return ret;
		}
	}

	return 0;
}

/*
 * Exits the current process and stores the waitcode as defined in <kern/wait.h>.
 * The supplied waitcode to this funtion is assumed to be already encoded properly.
 */
void
sys__exit(int32_t waitcode)
{
	pidtable_exit(curproc, waitcode);
	panic("Exit syscall should never get to this point.");
}





/*
Make crashed programs go back to kernel menu.
*/

int
sys_execv(const char *prog, char **args)
{
	int ret;

	if (prog == NULL || args == NULL) {
		return EFAULT;
	}

	char *progname;
	ret = string_in(prog, &progname, PATH_MAX);
	if (ret) {
		return ret;
	}

	int argc;
	ret = get_argc(args, &argc);
	if (ret) {
		return ret;
	}

	char **args_in = kmalloc(argc*sizeof(char *));
	int *size = kmalloc(argc*sizeof(int));
	ret = copy_in_args(argc, args, args_in, size);
	if (ret) {
		return ret;
	}

	struct vnode *v;
	vaddr_t entrypoint, stackptr;

	ret = vfs_open(progname, O_RDONLY, 0, &v);
	if (ret) {
		return ret;
	}

	struct addrspace *as;
	as = proc_setas(NULL);
	as_deactivate();
	as_destroy(as);

	KASSERT(proc_getas() == NULL);

	as = as_create();
	if (as == NULL) {
		return ENOMEM;
	}

	proc_setas(as);
	as_activate();

	ret = load_elf(v, &entrypoint);
	if (ret) {
		vfs_close(v);
		return ret;
	}

	vfs_close(v);

	ret = as_define_stack(as, &stackptr);
	if (ret) {
		return ret;
	}

	userptr_t args_out_addr;
	copy_out_args(argc, args_in, size, &stackptr, &args_out_addr);

	enter_new_process(argc, args_out_addr, NULL, stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

/*
Checks to make sure the user string is less than max_len. If it is, then
actual_length holds true length (not including null terminator).
*/
int
strlen_check(const char *string, int max_len, size_t *actual_length)
{
	int i = 0;
	while (string[i] != 0 && i < max_len) i++;

	if (string[i] != 0) {
		return E2BIG;
	}

	*actual_length = i;
	return 0;
}

int
get_argc(char **args, int *argc)
{
	int i = 0;
	while(args[i] != NULL && i < ARG_MAX) i++;

	if (args[i] != NULL) {
		return E2BIG;
	}

	*argc = i;
	return 0;
}

int
string_in(const char *user_src, char **kern_dest, size_t copy_size)
{
	int ret;

	copy_size++;
	*kern_dest = kmalloc(copy_size*sizeof(char));
    size_t *path_len = kmalloc(sizeof(int));
	ret = copyinstr((const_userptr_t) user_src, *kern_dest, copy_size, path_len);
	if (ret) {
		return ret;
	}

	kfree(path_len);
	return 0;
}

int
string_out(const char *kernel_src, userptr_t user_dest, size_t copy_size)
{
	int ret;

	size_t *path_len = kmalloc(sizeof(int));
	ret = copyoutstr(kernel_src, user_dest, copy_size, path_len);
	if (ret) {
		return ret;
	}

	kfree(path_len);
	return 0;
}

/*
Copies the user strings into ther kernel, populating size[] with their respective lengths.
Returns an error if bytes surpasses ARG_MAX.
*/
int
copy_in_args(int argc, char **args, char **args_in, int *size)
{
	int arg_size_left = ARG_MAX;
	size_t cur_size;
	int ret;

	for (int i = 0; i < argc; i++) {
		ret = strlen_check((const char *) args[i], arg_size_left - 1, &cur_size);
		if (ret) {
			return ret;
		}

		arg_size_left -= (cur_size + 1);
		size[i] = (int) cur_size + 1;
		string_in((const char *) args[i], &args_in[i], cur_size);
	}

	return 0;
}

/*
Copies the kernel strings out to the user stack, and cleans up the kernel strings.
*/
void
copy_out_args(int argc, char **args, int *size, vaddr_t *stackptr, userptr_t *args_out_addr)
{
	userptr_t arg_addr = (userptr_t) (*stackptr - argc*sizeof(userptr_t *) - sizeof(NULL));
	userptr_t *args_out = (userptr_t *) (*stackptr - argc*sizeof(userptr_t *) - sizeof(NULL));
	for (int i = 0; i < argc; i++) {
		arg_addr -= size[i];
		*args_out = arg_addr;
		string_out((const char *) args[i], arg_addr, (size_t) size[i]);
		args_out++;
		kfree(args[i]);
	}

	*args_out = NULL;
	*args_out_addr = (userptr_t) (*stackptr - argc*sizeof(int) - sizeof(NULL));
	arg_addr -= (int) arg_addr % sizeof(void *);
	*stackptr = (vaddr_t) arg_addr;
}



int
sys_fork(struct trapframe *tf, int32_t *retval0)
{
	struct proc *new_proc;
	int ret;

	ret = proc_create_fork("new_proc", &new_proc);
	if(ret) {
		return ret;
	}

	ret = pidtable_add(new_proc, &new_proc->pid);
	if (ret){
		proc_destroy(new_proc);
		return ret;
	}

	struct trapframe *new_tf;
	setup_forked_trapframe(tf, &new_tf);

	*retval0 = new_proc->pid;
	ret = thread_fork("new_thread", new_proc, enter_usermode, new_tf, 1);
	if (ret) {
		proc_destroy(new_proc);
		kfree(new_tf);
		return ret;
	}

	return 0;
}
/*
 * Sets up the memory structures for a newly forked process.
 */
int
proc_create_fork(const char *name, struct proc **new_proc)
{
	int ret;

	*new_proc = proc_create(name);
	if (*new_proc == NULL) {
		return ENOMEM;
	}

	ret = as_copy(curproc->p_addrspace, &(*new_proc)->p_addrspace);
	if (ret) {
		proc_destroy(*new_proc);
		return ret;
	}

	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		(*new_proc)->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	struct ft *ft = curproc->proc_ft;
	lock_acquire(ft->ft_lock);
	ft_copy(ft, (*new_proc)->proc_ft);
	lock_release(ft->ft_lock);

	return 0;
}

int
setup_forked_trapframe(struct trapframe *old_tf, struct trapframe **new_tf)
{
	*new_tf = kmalloc(sizeof(struct trapframe));
	if (*new_tf == NULL) {
		return ENOMEM;
	}

	memcpy((void *) *new_tf, (const void *) old_tf, sizeof(struct trapframe));
	(*new_tf)->tf_v0 = 0;
	(*new_tf)->tf_v1 = 0;
	(*new_tf)->tf_a3 = 0;      /* signal no error */
	(*new_tf)->tf_epc += 4;

	return 0;
}

void
enter_usermode(void *data1, unsigned long data2)
{
	(void) data2;
	void *tf = (void *) curthread->t_stack + 16;

	memcpy(tf, (const void *) data1, sizeof(struct trapframe));
	kfree((struct trapframe *) data1);

	as_activate();
	mips_usermode(tf);
}