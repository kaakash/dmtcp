/****************************************************************************
 *   Copyright (C) 2006-2008 by Jason Ansel, Kapil Arya, and Gene Cooperman *
 *   jansel@csail.mit.edu, kapil@ccs.neu.edu, gene@ccs.neu.edu              *
 *                                                                          *
 *   This file is part of the dmtcp/src module of DMTCP (DMTCP:dmtcp/src).  *
 *                                                                          *
 *  DMTCP:dmtcp/src is free software: you can redistribute it and/or        *
 *  modify it under the terms of the GNU Lesser General Public License as   *
 *  published by the Free Software Foundation, either version 3 of the      *
 *  License, or (at your option) any later version.                         *
 *                                                                          *
 *  DMTCP:dmtcp/src is distributed in the hope that it will be useful,      *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 *  GNU Lesser General Public License for more details.                     *
 *                                                                          *
 *  You should have received a copy of the GNU Lesser General Public        *
 *  License along with DMTCP:dmtcp/src.  If not, see                        *
 *  <http://www.gnu.org/licenses/>.                                         *
 ****************************************************************************/


#define _GNU_SOURCE
#define _XOPEN_SOURCE 500
// These next two are defined in features.h based on the user macros above.
// #define GNU_SRC
// #define __USE_UNIX98

#include <pthread.h>
#include "syscallwrappers.h"
// We should not need dlopen/dlsym
// #include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "constants.h"
#include "sockettable.h"
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <ctype.h>
#include <syslog.h>


// See syscallsreal.c for original model.  In dmtcphijack.so, system calls
//   for XXX() in jalib call a wrapper which modifies it and calls
//   syscallsreal.c:_real_XXX(), to directly calls kernel.
// For other functions (dmtcp_checkpoint, dmtcp_restart, etc.),
//   we want their invocations of jalib to directly call glibc with no wrappers.
// Jalib has some calls to real_XXX() to avoid going through the wrapper.
//   Those are defined in syscallsreal.cpp, but this is a simpler interface
//   that avoids calling on dlsym() and syscallsreal.cpp (by calling this
//   smaller image, jnosyscallsreal.cpp), in order to keep those
//   binaries smaller, and to keep the code simpler and more maintainable.
//   Can add wrapper code for libhijack.so without fear of contaminating
//   the other DMTCP executables with that wrapper.

// NOTE:  An alternative to this strategy would be to put this in a file,
//   nosyscallwrappers.h and #define away the real_XXX() calls.
//   But some files like uniquepid.cpp and connection.cpp could be
//   linked either to dmtcphijack.so or to dmtcp_restart.

/// FIXME:  dmtcpworker.cpp is linked into some ordinary executables.
///         It should be modified to avoid this, so we don't need gratuitous
///         extra reall_syscalls here like dmtcp_unsetenv(), dmtcp_lock()

//////////////////////////
//// DEFINE REAL VERSIONS OF NEEDED FUNCTIONS (based on syscallsreal.cpp)
//// (Define only functions needed for dmtcp_checkpoint, dmtcp_restart, etc.

static pthread_mutex_t theMutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

void _dmtcp_lock() { pthread_mutex_lock ( &theMutex ); }

void _dmtcp_unlock() { pthread_mutex_unlock ( &theMutex ); }

#define REAL_FUNC_PASSTHROUGH(name) return name

#define REAL_FUNC_PASSTHROUGH_TYPED(type,name) REAL_FUNC_PASSTHROUGH(name)

#define REAL_FUNC_PASSTHROUGH_PID_T(name) REAL_FUNC_PASSTHROUGH(name)

// No return statement for functions returning void:
#define REAL_FUNC_PASSTHROUGH_VOID(name) name

/// call the libc version of this function via dlopen/dlsym
int _real_socket ( int domain, int type, int protocol )
{
  REAL_FUNC_PASSTHROUGH ( socket ) ( domain,type,protocol );
}

/// call the libc version of this function via dlopen/dlsym
int _real_connect ( int sockfd,  const  struct sockaddr *serv_addr, socklen_t addrlen )
{
  REAL_FUNC_PASSTHROUGH ( connect ) ( sockfd,serv_addr,addrlen );
}

/// call the libc version of this function via dlopen/dlsym
int _real_bind ( int sockfd,  const struct  sockaddr  *my_addr,  socklen_t addrlen )
{
  REAL_FUNC_PASSTHROUGH ( bind ) ( sockfd,my_addr,addrlen );
}

/// call the libc version of this function via dlopen/dlsym
int _real_listen ( int sockfd, int backlog )
{
  REAL_FUNC_PASSTHROUGH ( listen ) ( sockfd,backlog );
}

/// call the libc version of this function via dlopen/dlsym
int _real_accept ( int sockfd, struct sockaddr *addr, socklen_t *addrlen )
{
  REAL_FUNC_PASSTHROUGH ( accept ) ( sockfd,addr,addrlen );
}

/// call the libc version of this function via dlopen/dlsym
int _real_setsockopt ( int s, int  level,  int  optname,  const  void  *optval,
                       socklen_t optlen )
{
  REAL_FUNC_PASSTHROUGH ( setsockopt ) ( s,level,optname,optval,optlen );
}

int _real_execve ( const char *filename, char *const argv[],
                   char *const envp[] )
{
  REAL_FUNC_PASSTHROUGH ( execve ) ( filename,argv,envp );
}

int _real_execv ( const char *path, char *const argv[] )
{
  REAL_FUNC_PASSTHROUGH ( execv ) ( path,argv );
}

int _real_execvp ( const char *file, char *const argv[] )
{
  REAL_FUNC_PASSTHROUGH ( execvp ) ( file,argv );
}

int _real_system ( const char *cmd )
{
  REAL_FUNC_PASSTHROUGH ( system ) ( cmd );
}

pid_t _real_fork( void )
{
  REAL_FUNC_PASSTHROUGH_PID_T ( fork ) ();
}

int _real_close ( int fd )
{
  REAL_FUNC_PASSTHROUGH ( close ) ( fd );
}

void _real_exit ( int status )
{
  REAL_FUNC_PASSTHROUGH_VOID ( exit ) ( status );
}

int _real_ptsname_r ( int fd, char * buf, size_t buflen )
{
  REAL_FUNC_PASSTHROUGH ( ptsname_r ) ( fd, buf, buflen );
}

int _real_socketpair ( int d, int type, int protocol, int sv[2] )
{
  REAL_FUNC_PASSTHROUGH ( socketpair ) ( d,type,protocol,sv );
}

void _real_openlog ( const char *ident, int option, int facility )
{
  REAL_FUNC_PASSTHROUGH_VOID ( openlog ) ( ident,option,facility );
}

void _real_closelog ( void )
{
  REAL_FUNC_PASSTHROUGH_VOID ( closelog ) ();
}

int _dmtcp_unsetenv( const char *name ) {
  REAL_FUNC_PASSTHROUGH ( unsetenv ) ( name );
}

#ifdef PID_VIRTUALIZATION
pid_t _real_getpid(void){
  REAL_FUNC_PASSTHROUGH_PID_T ( getpid ) ( );
}

pid_t _real_getppid(void){
  REAL_FUNC_PASSTHROUGH_PID_T ( getppid ) ( );
}

int _real_tcsetpgrp(int fd, pid_t pgrp){
  REAL_FUNC_PASSTHROUGH ( tcsetpgrp ) ( fd, pgrp );
}

int _real_tcgetpgrp(int fd) {
  REAL_FUNC_PASSTHROUGH ( tcgetpgrp ) ( fd );
}

pid_t _real_getpgrp(void) {
  REAL_FUNC_PASSTHROUGH_PID_T ( getpgrp ) ( );
}

pid_t _real_setpgrp(void) {
  REAL_FUNC_PASSTHROUGH_PID_T ( setpgrp ) ( );
}

pid_t _real_getpgid(pid_t pid) {
  REAL_FUNC_PASSTHROUGH_PID_T ( getpgid ) ( pid );
}

int   _real_setpgid(pid_t pid, pid_t pgid) {
  REAL_FUNC_PASSTHROUGH ( setpgid ) ( pid, pgid );
}

pid_t _real_getsid(pid_t pid) {
  REAL_FUNC_PASSTHROUGH_PID_T ( getsid ) ( pid );
}

pid_t _real_setsid(void) {
  REAL_FUNC_PASSTHROUGH_PID_T ( setsid ) ( );
}

int   _real_kill(pid_t pid, int sig) {
  REAL_FUNC_PASSTHROUGH ( kill ) ( pid, sig );
}

pid_t _real_wait(__WAIT_STATUS stat_loc) {
  REAL_FUNC_PASSTHROUGH_PID_T ( wait ) ( stat_loc );
}

pid_t _real_waitpid(pid_t pid, int *stat_loc, int options) {
  REAL_FUNC_PASSTHROUGH_PID_T ( waitpid ) ( pid, stat_loc, options );
}

int   _real_waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options) {
  REAL_FUNC_PASSTHROUGH ( waitid ) ( idtype, id, infop, options );
}

pid_t _real_wait3(__WAIT_STATUS status, int options, struct rusage *rusage) {
  REAL_FUNC_PASSTHROUGH_PID_T ( wait3 ) ( status, options, rusage );
}

pid_t _real_wait4(pid_t pid, __WAIT_STATUS status, int options, struct rusage *rusage) {
  REAL_FUNC_PASSTHROUGH_PID_T ( wait4 ) ( pid, status, options, rusage );
}

#endif

// Needed for _real_gettid, etc.
long int _real_syscall(long int sys_num, ... ) {
  int i;
  void * arg[7];
  va_list ap;

  va_start(ap, sys_num);
  for (i = 0; i < 7; i++)
    arg[i] = va_arg(ap, void *);
  va_end(ap);

  // /usr/include/unistd.h says syscall returns long int (contrary to man page)
  REAL_FUNC_PASSTHROUGH_TYPED ( long int, syscall ) ( sys_num, arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6] );
}

// gettid / tkill / tgkill are not defined in libc.
// So, this is needed even if there is no PID_VIRTUALIZATION.
pid_t _real_gettid(void){
#ifdef PID_VIRTUALIZATION
// IS THIS LIKE ORIGINAL?
  REAL_FUNC_PASSTHROUGH_PID_T ( _real_syscall(SYS_gettid) );
#else
  REAL_FUNC_PASSTHROUGH_PID_T ( syscall(SYS_gettid) );
#endif
}

int _real_open ( const char *pathname, int flags, mode_t mode ) {
  REAL_FUNC_PASSTHROUGH ( open ) ( pathname, flags, mode );
}

int _real_shmget (key_t key, size_t size, int shmflg) {
  REAL_FUNC_PASSTHROUGH ( shmget ) (key, size, shmflg);
}

void* _real_shmat (int shmid, const void *shmaddr, int shmflg) {
  REAL_FUNC_PASSTHROUGH_TYPED ( void*, shmat ) (shmid, shmaddr, shmflg);
}

int _real_shmdt (const void *shmaddr) {
  REAL_FUNC_PASSTHROUGH ( shmdt ) (shmaddr);
}

int _real_shmctl (int shmid, int cmd, struct shmid_ds *buf) {
  REAL_FUNC_PASSTHROUGH ( shmctl ) (shmid, cmd, buf);
}

