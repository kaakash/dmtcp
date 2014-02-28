#ifndef TLSINFO_H
#define TLSINFO_H

#include <ucontext.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <linux/version.h>
#include "syscallwrappers.h"  /* for _real_syscall */
#include "protectedfds.h"
#include "mtcp/mtcp_header.h"

// For i386 and x86_64, SETJMP currently has bugs.  Don't turn this
//   on for them until they are debugged.
// Default is to use  setcontext/getcontext.
#if defined(__arm__)
# define SETJMP /* setcontext/getcontext not defined for ARM glibc */
#endif

#ifdef SETJMP
# include <setjmp.h>
#else
# include <ucontext.h>
#endif

#ifdef __x86_64__
# define ELF_AUXV_T Elf64_auxv_t
# define UINT_T uint64_t
#else
  // else __i386__ and __arm__
# define ELF_AUXV_T Elf32_auxv_t
# define UINT_T uint32_t
#endif

#ifdef __x86_64__
# define eax rax
# define ebx rbx
# define ecx rcx
# define edx rax
# define ebp rbp
# define esi rsi
# define edi rdi
# define esp rsp
# define CLEAN_FOR_64_BIT(args...) CLEAN_FOR_64_BIT_HELPER(args)
# define CLEAN_FOR_64_BIT_HELPER(args...) #args
#elif __i386__
# define CLEAN_FOR_64_BIT(args...) #args
#else
# define CLEAN_FOR_64_BIT(args...) "CLEAN_FOR_64_BIT_undefined"
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#define GETTID() _real_syscall(SYS_gettid)
#define TGKILL(pid,tid,sig) _real_syscall(SYS_tgkill, pid, tid, sig)

pid_t dmtcp_get_real_tid() __attribute((weak));
pid_t dmtcp_get_real_pid() __attribute((weak));
int dmtcp_real_tgkill(pid_t pid, pid_t tid, int sig) __attribute((weak));

#define THREAD_REAL_PID() \
  (dmtcp_get_real_pid != NULL ? dmtcp_get_real_pid() : getpid())

#define THREAD_REAL_TID() \
  (dmtcp_get_real_tid != NULL ? dmtcp_get_real_tid() : GETTID())

#define THREAD_TGKILL(pid, tid, sig) \
  (dmtcp_real_tgkill != NULL ? dmtcp_real_tgkill(pid,tid,sig) \
                                 : TGKILL(pid, tid, sig))

typedef int (*fptr)(void*);

typedef enum ThreadState {
  ST_RUNNING,
  ST_SIGNALED,
  ST_SUSPINPROG,
  ST_SUSPENDED,
  ST_ZOMBIE,
  ST_CKPNTHREAD
} ThreadState;

typedef struct Thread Thread;

struct Thread {
  pid_t tid;
  Thread *next;
  Thread *prev;
  int state;

  int (*fn)(void *);
  void *arg;
  int flags;
  pid_t *ptid;
  pid_t *ctid;

  pid_t virtual_tid;
  sigset_t sigblockmask; // blocked signals
  sigset_t sigpending;   // pending signals

  void *saved_sp; // at restart, we use a temporary stack just
                  //   beyond original stack (red zone)

  ThreadTLSInfo tlsInfo;

  ///JA: new code ported from v54b
#ifdef SETJMP
  sigjmp_buf jmpbuf;     // sigjmp_buf saved by sigsetjmp on ckpt
#else
  ucontext_t savctx;     // context saved on suspend
#endif

};

void TLSInfo_VerifyPidTid(pid_t pid, pid_t tid);
void TLSInfo_UpdatePid();
void TLSInfo_SaveTLSState (ThreadTLSInfo *tlsInfo);
void TLSInfo_RestoreTLSState(ThreadTLSInfo *tlsInfo);
void TLSInfo_SetThreadSysinfo(void *sysinfo);
void *TLSInfo_GetThreadSysinfo();
int  TLSInfo_HaveThreadSysinfoOffset();
int  TLSInfo_GetTidOffset();
int  TLSInfo_GetPidOffset();

#ifdef __cplusplus
}
#endif

#endif