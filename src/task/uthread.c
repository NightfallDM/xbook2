#include <xbook/task.h>
#include <xbook/schedule.h>
#include <xbook/process.h>
#include <arch/interrupt.h>

#define DEBUG_LOCAL 1

/* 用户线程 */

/*
typedef sturct __xthread_attr {


} xthread_attr_t;


void *thread_routine(void *arg)
{


    return (void *) 0;
}

xthraed_t tid = xthread_start(thread_routine, arg, xthread_attr);

retval = xthread_exit((void *) 123);

retval = xthread_join(tig, (void *) status);

tid = sys_uthread_start(routine, arg);
sys_uthread_exit(tid);
*/

/**
 * uthread_entry - 用户线程内核的入口
 * @arg: 参数
 * 
 * 通过这个入口，可以跳转到用户态运行。
 * 
 */
void uthread_entry(void *arg) 
{
    printk(KERN_DEBUG "uthread_entry: ready into user thread.\n");
    trap_frame_t *frame = GET_TASK_TRAP_FRAME(current_task);
    switch_to_user(frame);
}

#if 0
/**
 * uthread_entry - 用户线程内核的入口
 * @arg: 参数
 * 
 * 构建c语言函数栈。
 * 
 * void *thread_start(* arg) {
 *  ...
 * }
 * 堆栈看起来是这样的：
 * 
 * esp + 8: arg 参数
 * esp + 4: caller addr 调用者返回地址
 * esp    : free stack top 可用栈顶
 */
void *uthread_entry(void *arg) 
{
    printk(KERN_DEBUG "uthread_entry: ready into user thread.\n");
    
    /* 获取当前任务的中断栈框 */
    trap_frame_t *frame = GET_TASK_TRAP_FRAME(current_task);   
    frame->esp -= sizeof(unsigned int); /* 预留参数（4字节） */
    unsigned int *stack_top = (unsigned int *)frame->esp;
    *stack_top = (unsigned int *)arg;   /* 往堆栈写入参数 */
    frame->esp -= sizeof(unsigned int); /* 预留调用者返回地址 */
    
    printk(KERN_DEBUG "uthread_entry: esp=%x eip=%x arg=%x stack=%x\n", frame->esp, frame->eip, arg, *stack_top);
    switch_to_user(frame);
}
#endif

/**
 * uthread_start - 开始一个用户线程
 * @func: 线程入口
 * @arg: 线程参数
 * 
 * 1.进程需要分配线程的堆栈
 * 2.需要传入线程入口
 * 3.需要传入线程例程和参数
 */
task_t *uthread_start(task_func_t *func, void *arg, 
    uthread_attr_t *attr, void *thread_entry)
{
    /* 创建线程的父进程 */
    task_t *parent = current_task;
#if DEBUG_LOCAL == 1
    printk(KERN_DEBUG "uthread_start: routine=%x arg=%x stackaddr=%x stacksize=%x detach=%d\n",
        func, arg, attr->stackaddr, attr->stacksize, attr->detachstate);
#endif
    // 创建一个新的线程结构体
    task_t *task = (task_t *) kmalloc(TASK_KSTACK_SIZE);
    
    if (!task)
        return NULL;
    
    // 初始化线程
    task_init(task, "uthread", TASK_PRIO_USER);

    if (parent->tgid == parent->pid) {  /* 父进程是主线程 */
        task->tgid = parent->pid;   /* 线程组id指向父进程的pid */
        task->parent_pid = parent->pid; /* 父进程是创建者进程 */
    } else {    /* 父进程不是主线程，是子线程 */
        task->tgid = parent->tgid;   /* 线程组指向父进程的tpid */
        task->parent_pid = parent->tgid; /* 父进程是主线程 */
    }

#if DEBUG_LOCAL == 1
    printk(KERN_DEBUG "uthread_start: pid=%x tgid=%x parent pid=%d\n",
        task->pid, task->tgid, task->parent_pid);
#endif

    task->vmm = parent->vmm;    /*共享内存 */
    task->res = parent->res;    /* 共享资源 */
    task->triggers = parent->triggers;/* 共享触发器 */

    /* 中断栈框 */
    proc_make_trap_frame(task);

    // 创建一个线程
    make_task_stack(task, uthread_entry, arg);

#if 0
    /* 写入关键信息 */
    trap_frame_t *frame = GET_TASK_TRAP_FRAME(task);
    frame->eip = func;
    frame->esp = stack_top;
#endif
    /* 构建用户线程栈框 */
    trap_frame_t *frame = GET_TASK_TRAP_FRAME(task);
    build_user_thread_frame(frame, arg, (void *)func, thread_entry, 
        (unsigned char *)attr->stackaddr + attr->stacksize);

    if (attr->detachstate) {    /* 设置detach分离 */
        task->flags |= TASK_FLAG_DETACH;
    }
    
    /* 操作链表时关闭中断，结束后恢复之前状态 */
    unsigned long flags;
    save_intr(flags);

    task_global_list_add(task);
    task_priority_queue_add_tail(task);
    
    restore_intr(flags);
    return task;
}

uthread_t sys_thread_create(
    uthread_attr_t *attr,
    task_func_t *func,
    void *arg,
    void *thread_entry
){
    /* 传进来的属性为空就返回 */
    if (attr == NULL)
        return -1;

    task_t *task = uthread_start(func, arg, attr, thread_entry);
    if (task == NULL)
        return -1;  /* failed */
    return task->pid;       /* 返回线程的id */
}


void uthread_exit(void *status)
{
    unsigned long flags;
    save_intr(flags);

    task_t *cur = current_task;  /* 当前进程是父进程 */
    
    /* 检测是用户进程还是线程退出 */
    if (TASK_IS_MAIN_THREAD(cur)) { /* 主线程退出 */
        sys_exit((int) status);
    }

    cur->exit_status = (int)status;
    
    /* 释放内核资源 */
    thread_release(cur);

    /* 子线程退出 */
    if (cur->flags & TASK_FLAG_DETACH) {    /* 不需要同步等待，"自己释放资源"(让init来释放) */
        printk(KERN_DEBUG "uthread_exit: detached.\n");
        /* 有可能启动时是joinable的，但是执行过程中变成detach状态，
        因此，可能存在父进程join等待，所以，这里就需要检测任务状态 */
        if (cur->flags &  THREAD_FLAG_JOINED) {    /* 处于join状态 */
            /* 父进程指向join中的进程 */
            task_t *parent = find_task_by_pid(cur->parent_pid);
            if (parent != NULL && parent->state == TASK_WAITING) {  /* 如果父进程在等待中 */
                if (parent->tgid == cur->tgid) {    /* 父进程和自己属于同一个线程组 */
                    printk(KERN_DEBUG "uthread_exit: parent %s pid=%d joining, wakeup it.\n",
                        parent->name, parent->pid);
                    parent->flags &= ~THREAD_FLAG_JOINING;  /* 去掉等待中标志 */
                    /* 唤醒父进程 */
                    task_unblock(parent);
                }
            }
        }
        /* 过继给init进程，实现线程"自己释放资源"(init隐藏释放) */
        cur->parent_pid = INIT_PROC_PID;   
    } else {    /* 需要同步释放 */
        printk(KERN_DEBUG "uthread_exit: joinable.\n");
    }
    printk(KERN_DEBUG "uthread_exit: pid=%d tgid=%d ppid=%d.\n", cur->pid, cur->tgid, cur->parent_pid);

    task_t *parent = find_task_by_pid(cur->parent_pid); 
    if (parent) {
        /* 查看父进程状态 */
        if (parent->state == TASK_WAITING) {
            restore_intr(flags);
#if DEBUG_LOCAL == 1
            printk(KERN_DEBUG "sys_exit: parent waiting...\n");
#endif    

            //printk("parent waiting...\n");
            task_unblock(parent); /* 唤醒父进程 */
            task_block(TASK_HANGING);   /* 把自己挂起 */
        } else { /* 父进程没有 */
            restore_intr(flags);
#if DEBUG_LOCAL == 1
            printk(KERN_DEBUG "sys_exit: parent not waiting, zombie!\n");
#endif    
            //printk("parent not waiting, zombie!\n");
            task_block(TASK_ZOMBIE);   /* 变成僵尸进程 */
        }
    } else {
        /* 没有父进程，变成不可被收养的孤儿+僵尸进程 */
#if DEBUG_LOCAL == 1
            printk(KERN_DEBUG "sys_exit: no parent! zombie!\n");
#endif    
        //printk("no parent!\n");
        restore_intr(flags);
        task_block(TASK_ZOMBIE); 
    }
}

void sys_thread_exit(void *retval)
{
    printk(KERN_DEBUG "sys_thread_exit: exit with %x\n", retval);
    uthread_exit(retval);
}

/**
 * sys_thread_detach - 设置线程为分离状态
 * @thread: 线程
 * 
 */
int sys_thread_detach(uthread_t thread) 
{
    task_t *task = find_task_by_pid(thread);
    if (task == NULL)   /* not found */
        return -1;
    /* 线程才可以分离 */
    //if (!TASK_IS_MAIN_THREAD(task)) {    /* 不是主线程才能进行分离 */
    task->flags |= TASK_FLAG_DETACH; /* 分离标志 */
    return 0;
    //}
    return -1;
}


int uthread_join(uthread_t thread, void **thread_return)
{
    task_t *waiter = current_task;  /* 当前进程是父进程 */
    unsigned long flags;
    save_intr(flags);
    /* 先查看线程，是否存在，并且要是线程才行 */
    task_t *task, *find = NULL;
    list_for_each_owner (task, &task_global_list, global_list) {
        /* find the thread and not zombie */
        if (task->pid == thread && task->state != TASK_ZOMBIE) {
            find = task;    /* find thread */
            break;
        }
    }
    
    if (find == NULL) { /* 线程不存在 */
        restore_intr(flags);
        return -1;  /* 没找到线程 */
    }
#if DEBUG_LOCAL == 1
    printk(KERN_DEBUG "uthread_join: join thread %d\n", thread);
#endif
    /* 线程存在，查看其是否为分离状态 */
    if (find->flags & TASK_FLAG_DETACH) {
#if DEBUG_LOCAL == 1
        printk(KERN_DEBUG "uthread_join: thread was detached, just return.\n");
#endif  
        restore_intr(flags);
        return -1;
    }
 
    /* 么有线程等待中才能等待 */
    if (find->flags & THREAD_FLAG_JOINED) {
#if DEBUG_LOCAL == 1
        printk(KERN_DEBUG "uthread_join: the thread %d is joining by thread %d.\n",
            find->pid, find->parent_pid);
#endif 
        restore_intr(flags);
        return -1;  /* 已经有一个线程在等待，不能等待 */
    }

    find->flags |= THREAD_FLAG_JOINED; /* 被线程等待 */
    find->parent_pid = waiter->pid;     /* 等待者变成父进程，等待子线程，可以通过thread_exit来唤醒父进程 */

    waiter->flags |= THREAD_FLAG_JOINING;   /* 处于等待中 */

    int status = 0;
    pid_t pid;
    /* 当线程没有退出的时候就一直等待 */
    do {
        
        pid = wait_one_hangging_child(waiter, thread, &status);
        restore_intr(flags);
#if DEBUG_LOCAL == 1
        printk(KERN_DEBUG "uthread_join: wait pid=%d status=%x\n", pid, status);
#endif
        if (pid == thread) {
            break; /* 处理了指定的任务，就返回 */
        }
        /* 如果等待者joining状态取消了，就说明等待的线程在执行过程中变成DETACH状态，
        并且退出时来取消该标志 */
        if (!(waiter->flags & THREAD_FLAG_JOINING)) {
            break;
        }

        printk(KERN_DEBUG "uthread_join: waiting...\n");
        
        /* WATING for thread to exit */
        task_block(TASK_WAITING);
        
        save_intr(flags);
    } while (pid == -1);
    /* 回写状态 */
    if (thread_return != NULL) {
        *thread_return = (void *)status;
#if DEBUG_LOCAL == 1
        printk(KERN_DEBUG "uthread_join: thread exit, will return status=%x\n", *thread_return);
#endif
    }

    
    restore_intr(flags);
    return 0;
}

/**
 * sys_thread_join - 等待线程退出
 * @thread: 线程
 * @thread_return: 返回值 
 */
int sys_thread_join(uthread_t thread, void **thread_return)
{
    return uthread_join(thread, thread_return);
}

