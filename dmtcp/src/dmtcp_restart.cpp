/****************************************************************************
 *   Copyright (C) 2006-2010 by Jason Ansel, Kapil Arya, and Gene Cooperman *
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

#include <unistd.h>

#include <stdlib.h>
#include <string>
#include <stdio.h>
#include <ctype.h>
#include  "../jalib/jassert.h"
#include  "../jalib/jfilesystem.h"
#include "constants.h"
#include "connectionmanager.h"
#include "dmtcpworker.h"
#include "dmtcpmessagetypes.h"
#include "connectionstate.h"
#include "mtcpinterface.h"
#include "syscallwrappers.h"
#include "protectedfds.h"
#include "util.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <vector>

// Some global definitions
static dmtcp::UniquePid compGroup;
static int numPeers;
static int coordTstamp = 0;

dmtcp::string dmtcpTmpDir = "/DMTCP/Uninitialized/Tmp/Dir";

using namespace dmtcp;

#ifdef PID_VIRTUALIZATION
static void openOriginalToCurrentMappingFiles();
void unlockPidMapFile();
#endif
static void runMtcpRestore ( const char* path, int offset,
                             size_t argvSize, size_t envSize);

namespace dmtcp
{

#ifdef PID_VIRTUALIZATION

  class OriginalPidTable {
    public:
      OriginalPidTable(){}

      void insertFromVirtualPidTable ( dmtcp::VirtualPidTable& vt )
      {
        dmtcp::vector< pid_t > tmpVector;

        _insert(vt.pid());

        tmpVector = vt.getChildPidVector();
        for ( size_t i = 0; i < tmpVector.size(); ++i )
          _insert(tmpVector[i]);

        tmpVector = vt.getTidVector();
        for ( size_t i = 0; i < tmpVector.size(); ++i )
          _insert(tmpVector[i]);
      }

      void _insert( pid_t pid )
      {
        if (!isConflictingChildPid (pid) /* && newVector[i] != getpid()*/) {
          _vector.push_back ( pid );
          JTRACE("New Pid Pushed to PidVector") (pid);
        }
      }

      bool isConflictingChildPid ( pid_t pid )
      {
        //iterator i = _vector.find ( pid );
        //if ( i == _vector.end() )
        //  return false;
        for ( size_t i = 0; i < _vector.size(); ++i )
          if ( _vector[i] == pid )
            return true;

        return false;
      }

      size_t numPids () { return _vector.size(); }

    private:
      typedef dmtcp::vector< pid_t >::iterator iterator;
      dmtcp::vector< pid_t > _vector;
  };

  OriginalPidTable originalPidTable;

#endif


  class RestoreTarget
  {
  public:
    RestoreTarget ( const dmtcp::string& path )
      : _path ( path )
    {
      JASSERT ( jalib::Filesystem::FileExists ( _path ) ) ( _path )
		.Text ( "checkpoint file missing" );

      dmtcp::SerializedWorkerInfo workerInfo;
      _offset = _conToFd.loadFromFile(_path, &workerInfo);

      _compGroup = workerInfo.compGroup;
      _numPeers  = workerInfo.numPeers;
      _argvSize  = workerInfo.argvSize;
      _envSize   = workerInfo.envSize;

#ifdef PID_VIRTUALIZATION
      _virtualPidTable = workerInfo.virtualPidTable;
      _virtualPidTable.erase(getpid());
      _roots.clear();
      _children.clear();
      _smap.clear();
      _used = 0;
#endif

      JTRACE ( "restore target" ) ( _path ) (_numPeers ) (_compGroup)
	                          ( _conToFd.size() ) (_offset);
    }

    void dupAllSockets ( SlidingFdTable& slidingFd )
    {
      int lastfd = -1;
      dmtcp::vector<int> fdlist;
      for ( ConnectionToFds::const_iterator i = _conToFd.begin();
	    i!=_conToFd.end(); ++i )
        {
          Connection& con = ConnectionList::instance() [i->first];
          if ( con.conType() == Connection::INVALID ){
            JWARNING(false)(i->first).Text("Can't restore invalid Connection");
            continue;
          }

          const dmtcp::vector<int>& fds = i->second;
          for ( size_t x=0; x<fds.size(); ++x )
	    {
	      int fd = fds[x];
	      fdlist.push_back ( fd );
	      slidingFd.freeUpFd ( fd );
	      int oldFd = slidingFd.getFdFor ( i->first );
	      JTRACE ( "restoring fd" ) ( i->first ) ( oldFd ) ( fd );
	      //let connection do custom dup2 handling
	      con.restartDup2( oldFd, fd );

	      if ( fd > lastfd )
		{
		  lastfd = fd;
		}
	    }
        }

      size_t j;
      for ( int i = 0 ; i < slidingFd.startFd() ; i++ )
        {
          for ( j = 0 ; j < fdlist.size() ; j++ )
	    {
	      if ( fdlist.at ( j ) == i )
		break;
	    }
          if ( j == fdlist.size() )
	    {
	      _real_close ( i );
	    }
        }

      slidingFd.closeAll();
    }

    int find_stdin( SlidingFdTable& slidingFd )
    {
      for ( ConnectionToFds::const_iterator i = _conToFd.begin();
          i!=_conToFd.end(); ++i )
      {
        const dmtcp::vector<int>& fds = i->second;
        for ( size_t x=0; x<fds.size(); ++x )
        {
          if (fds[x] == STDIN_FILENO){
            JTRACE("Found stdin: fds[x] <---> slidingFd.getFdFor()")
              (x) (fds[x]) (slidingFd.getFdFor ( i->first ));
            return slidingFd.getFdFor ( i->first );
          }
        }
      }
      return -1;
    }

    void mtcpRestart()
    {
      runMtcpRestore ( _path.c_str(), _offset, _argvSize, _envSize );
    }

    const UniquePid& upid() const { return _conToFd.upid(); }
    const dmtcp::string& procname() const { return _conToFd.procname(); }

#ifdef PID_VIRTUALIZATION
    typedef map<pid_t,bool> sidMapping;
    typedef sidMapping::iterator s_iterator;
    typedef vector<RestoreTarget *>::iterator t_iterator;

    VirtualPidTable& getVirtualPidTable() { return _virtualPidTable; }
    void addChild(RestoreTarget *t){ _children.push_back(t); }

    bool isSessionLeader(){
      JTRACE("")(_virtualPidTable.sid()) (upid().pid());
      if( _virtualPidTable.sid() == upid().pid() )
	return true;
      else
	return false;
    }

    bool isGroupLeader(){
      JTRACE("")(_virtualPidTable.sid()) (upid().pid());
      if( _virtualPidTable.gid() == upid().pid() )
	return true;
      else
	return false;
    }

    bool isForegroundProcess() {
      JTRACE("")(_virtualPidTable.sid()) (upid().pid());
      if( _virtualPidTable.fgid() == _virtualPidTable.gid() )
	return true;
      else
	return false;
    }

    bool isInitChild(){
      JTRACE("")(_virtualPidTable.ppid());
      if( _virtualPidTable.ppid() == 1 )
	return true;
      else
	return false;
    }

    int addRoot(RestoreTarget *t, pid_t sid){
      if( isSessionLeader() && _virtualPidTable.sid() == sid ){
	_roots.push_back(t);
	return 1;
      }else{
	t_iterator it = _children.begin();
	for(; it != _children.end(); it++){
	  if( (*it)->addRoot(t, sid) )
	    return 1;
	}
      }
      return 0;
    }

    // Traverse this process subtree and set up information about sessions
    //   and their leaders for all children.
    sidMapping &setupSessions() {
      pid_t sid = _virtualPidTable.sid();
      if( !_children.size() ) {
	_smap[sid] = isSessionLeader();
	return _smap;
      }
      // We have at least one child
      t_iterator it = _children.begin();
      _smap = (*it)->setupSessions();
      for(it++; it != _children.end();it++) {
	sidMapping tmp = (*it)->setupSessions();
	s_iterator it1 = tmp.begin();
	for(;it1 != tmp.end(); it1++) {
	  s_iterator it2 = _smap.find(it1->first);
	  if( it2 != _smap.end() ) {
	    // mapping already exist
	    if( it2->second != it1->second ) {
	      // Session was created after child creation.  So child from one
	      // thread cannot be member of session of child from other thread.
	      JASSERT(false). Text("One child contains session leader"
				   " and other contains session member!\n");
	      exit(0);
	    }
	  } else {
	    // add new mapping
	    _smap[it1->first] = it1->second;
	  }
	}
      }

      s_iterator sit = _smap.find(sid);
      if( sit != _smap.end() ) {
	if( sit->second && !isSessionLeader() ) {
	  // child is leader and parent is slave - impossible
	  JASSERT(false)
	         .Text("child is leader and parent is slave - impossible\n");
	  exit(0);
	}
      }
      _smap[sid] = isSessionLeader();
      return _smap;
}

    void printMapping(){
      t_iterator it = _children.begin();
      for(; it != _children.end(); it++){
	(*it)->printMapping();
      }
      JTRACE("")(upid());
      s_iterator sit = _smap.begin();
      for(; sit != _smap.end(); sit++){
	JTRACE("") (sit->first) (sit->second);
      }
    }

    sidMapping &getSmap(){ return _smap; }

    pid_t checkDependence(RestoreTarget *t){
      sidMapping smap = t->getSmap();
      s_iterator ext = smap.begin();
      // Run through sessions --> has leader mapping
      for(; ext != smap.end(); ext++){
	if( ext->second == false ){
	  // Session pointed by ext has no leader in target t process tree
	  s_iterator intern = _smap.find(ext->first);
	  if( intern != _smap.end() && intern->second == true ){
	    // internal target has session leader in its tree
	    // TODO: can process trees be connected through several sessions?
	    return ext->first;
	  }
	}
      }
      return -1;
    }

    void bringToForeground(SlidingFdTable& slidingFd)
    {
      char controllingTerm[L_ctermid];
      pid_t pid;

      int sin = find_stdin(slidingFd);

      if( isSessionLeader() ){
        // XXX: Where is the controlling terminal being set?
	char *ptr =  ttyname(sin);
	int fd = open(ptr,O_RDWR);
	if( ctermid(controllingTerm) ){
	  int tfd = open(ptr,O_RDONLY);
	  if( tfd >= 0 ){
	    JTRACE("Setting current controlling terminal") (controllingTerm);
	    close(tfd);
	  }else if (ptr == NULL){
            JTRACE("Cannot restore controlling terminal") (ttyname(sin));
          } else {
	    JWARNING(false) (ttyname(sin))
                    .Text("Cannot restore controlling terminal");
	  }
	}
	if (fd >= 0) close(fd);
      }

      pid_t gid = getpgid(0);
      pid_t fgid = tcgetpgrp(sin);

      if( !isForegroundProcess() )
	return;
      if( !isGroupLeader()  ){
	return;
      }

      if( gid != fgid ){
	if( !(pid = fork()) ){ // fork subversive process
	  // This process moves itself to current foreground Group
	  // and then changes foreground Group to what we need
	  // so it works as a spy, saboteur or wrecker :)
	  // -- Artem
	  JTRACE("Change current GID to foreground GID.");

	if( setpgid(0, fgid) ){
          if (fgid == -1) {
            JTRACE("CANNOT Change current GID to foreground GID")
                  (getpid()) (fgid) (_virtualPidTable.fgid()) (gid) (JASSERT_ERRNO);
          } else {
            JWARNING(false)
                     (getpid()) (fgid) (_virtualPidTable.fgid()) (gid) (JASSERT_ERRNO)
                    .Text("CANNOT Change current GID to foreground GID");
          }
 	  fflush(stdout);
 	  exit(0);
	}

        if( tcsetpgrp(sin, gid) ){
	  printf("CANNOT Move parent GID to foreground: %s\n",
		 strerror(errno));
 	  printf("PID=%d, FGID=%d, GID=%d\n",getpid(),fgid,gid);
 	  printf("PID=%d, FGID=%d, _FGID=%d, GID=%d\n",
		 getpid(),fgid,_virtualPidTable.fgid(), gid);
 	  fflush(stdout);
 	  exit(0);
 	  }

	  JTRACE("Finish foregrounding.")(getpid())(getpgid(0))(tcgetpgrp(0));
	  exit(0);
	}else{
	  int status;
	  wait(&status);
	}
      }
    }

    void restoreGroup( SlidingFdTable& slidingFd )
    {
      if( isGroupLeader() ){
	// create new Group where this process becomes a leader
	JTRACE("Create new Group.");
	setpgid(0, 0);
	bringToForeground(slidingFd);
      }
    }

    void CreateProcess(DmtcpCoordinatorAPI& coordinatorAPI,
                       SlidingFdTable& slidingFd)
    {
      //change UniquePid
      UniquePid::resetOnFork(upid());
      //UniquePid::ThisProcess(true) = _conToFd.upid();

      Util::initializeLogFile(procname());
      JTRACE("Creating process during restart") (upid()) (procname());


      VirtualPidTable &vt = _virtualPidTable;

      JTRACE("")(_real_getpid())(_real_getppid())(_real_getsid(0));

      vt.updateMapping(upid().pid(), _real_getpid());
      pid_t psid = vt.sid();

      if( !isSessionLeader() ){

	// Restore Group information
	restoreGroup(slidingFd);

	// If process is not session leader, restore it and all children.
	t_iterator it = _children.begin();
	for(; it != _children.end(); it++){
	  JTRACE ( "Forking Child Process" ) ( (*it)->upid() );
	  pid_t cid = forkChild();

	  if ( cid == 0 )
            {
              (*it)->CreateProcess (coordinatorAPI, slidingFd);
              JASSERT ( false ) . Text ( "Unreachable" );
            }
	  JASSERT ( cid > 0 );
	  VirtualPidTable::iterator vit = vt.begin();
	  for(; vit != vt.end(); vit++){
	    if( (*it)->upid() == vit->second ){
	      vt.updateMapping ( vit->first, cid );
	      break;
	    }
	  }

	}
      }else{
	// Process is session leader.
	// There may be not setsid-ed children.
	for(t_iterator it = _children.begin(); it != _children.end(); it++){
	  s_iterator sit = (*it)->getSmap().find(psid);
	  JTRACE("Restore processes that was created before their parent called setsid()");
	  if( sit == (*it)->getSmap().end() ){
	    JTRACE ( "Forking Child Process" ) ( (*it)->upid() );
	    pid_t cid = forkChild();
	    if ( cid == 0 )
	      {
		(*it)->CreateProcess (coordinatorAPI, slidingFd);
		JASSERT ( false ) . Text ( "Unreachable" );
	      }
	    JASSERT ( cid > 0 );
	    VirtualPidTable::iterator vit = _virtualPidTable.begin();
	    for(; vit != _virtualPidTable.end(); vit++){
	      if( (*it)->upid() == vit->second ){
		_virtualPidTable.updateMapping ( vit->first, cid );
	      }
	    }
	  }
	}

	pid_t nsid = setsid();
	JTRACE("change SID")(nsid);

	// Restore Group information
	restoreGroup(slidingFd);

	for(t_iterator it = _children.begin(); it != _children.end(); it++) {
	  JTRACE("Restore processes that was created after their parent called setsid()");
	  s_iterator sit = (*it)->getSmap().find(psid);
	  if( sit != (*it)->getSmap().end() ) {
	    JTRACE ( "Forking Child Process" ) ( (*it)->upid() );
	    pid_t cid = forkChild();
	    if ( cid == 0 ){
	      (*it)->CreateProcess (coordinatorAPI, slidingFd );
	      JASSERT ( false ) . Text ( "Unreachable" );
	    }
	    JASSERT ( cid> 0 );
	    VirtualPidTable::iterator vit = _virtualPidTable.begin();
	    for(; vit != _virtualPidTable.end(); vit++) {
	      if( (*it)->upid() == vit->second ) {
		_virtualPidTable.updateMapping ( vit->first, cid );
	      }
	    }
	  }
	}

	for(t_iterator it = _roots.begin() ; it != _roots.end(); it++) {
	  JTRACE ( "Forking Dependent Root Process" ) ( (*it)->upid() );
	  pid_t cid;
	  if( (cid = fork()) ){
	    waitpid(cid, NULL, 0);
	  }else{
	    if( fork() )
	      exit(0);
	    (*it)->CreateProcess(coordinatorAPI, slidingFd );
	    JASSERT (false) . Text( "Unreachable" );
	  }
	}
      }

      JTRACE("Child and dependent root processes forked, restoring process")
	    (upid())(getpid())(isGroupLeader());
      // Save PID mapping information
      pid_t orig = upid().pid();
      pid_t curr = _real_getpid();
      dmtcp::VirtualPidTable::InsertIntoPidMapFile(orig, curr);

      //Reconnect to dmtcp_coordinator
      WorkerState::setCurrentState ( WorkerState::RESTARTING );

      int tmpCoordFd = dup(PROTECTED_COORD_FD);
      JASSERT(tmpCoordFd != -1);
      coordinatorAPI.connectToCoordinator();
      coordinatorAPI.sendCoordinatorHandshake(procname(), _compGroup);
      coordinatorAPI.recvCoordinatorHandshake();
      close(tmpCoordFd);

      dmtcp::string serialFile = dmtcp::UniquePid::pidTableFilename();

      JTRACE ( "PidTableFile: ") ( serialFile ) ( dmtcp::UniquePid::ThisProcess() );
      jalib::JBinarySerializeWriter tblwr ( serialFile );
      _virtualPidTable.serialize ( tblwr );
      tblwr.~JBinarySerializeWriter();

      int stmpfd =  open( serialFile.c_str(), O_RDONLY);
      JASSERT ( stmpfd >= 0 ) ( serialFile ) ( errno );

      JASSERT ( dup2 ( stmpfd, PROTECTED_PIDTBL_FD) == PROTECTED_PIDTBL_FD )
	      ( serialFile ) ( stmpfd );

      close (stmpfd);

      //restart targets[i]
      dupAllSockets ( slidingFd );

      mtcpRestart();

      JASSERT ( false ).Text ( "unreachable" );
    }


    static pid_t forkChild()
    {
      while ( 1 ) {

	pid_t childPid = fork();

	JASSERT ( childPid != -1 ) .Text ( "fork() failed" );

	if ( childPid == 0 ) { /* child process */
	  if ( originalPidTable.isConflictingChildPid ( getpid() ) )
	    _exit(DMTCP_FAIL_RC);
	  else
	    return 0;
	}
	else { /* Parent Process */
	  if ( originalPidTable.isConflictingChildPid ( childPid ) ) {
	    JTRACE( "PID Conflict, creating new child" ) (childPid);
	    waitpid ( childPid, NULL, 0 );
	  }
	  else
	    return childPid;
	}
      }

      return -1;
    }
#endif

    dmtcp::string _path;
    int _offset;
    ConnectionToFds _conToFd;
    UniquePid _compGroup;
    int _numPeers;
    size_t _argvSize;
    size_t _envSize;
#ifdef PID_VIRTUALIZATION
    VirtualPidTable _virtualPidTable;
    // Links to children of this process
    vector<RestoreTarget *> _children;
    // Links to roots that depend on this target
    // i.e. have SID of this target in its tree.
    vector<RestoreTarget *> _roots;
    sidMapping _smap;
    bool _used;
#endif
  };

} // end namespace

// gcc-4.3.4 -Wformat=2 issues false positives for warnings unless the format
// string has at least one format specifier with corresponding format argument.
// Ubuntu 9.01 uses -Wformat=2 by default.
static const char* theUsage =
  "USAGE:\n dmtcp_restart [OPTIONS] <ckpt1.dmtcp> [ckpt2.dmtcp...]\n\n"
  "OPTIONS:\n"
  "  --host, -h, (environment variable DMTCP_HOST):\n"
  "      Hostname where dmtcp_coordinator is run (default: localhost)\n"
  "  --port, -p, (environment variable DMTCP_PORT):\n"
  "      Port where dmtcp_coordinator is run (default: 7779)\n"
  "  --tmpdir, -t, (environment variable DMTCP_TMPDIR):\n"
  "      Directory to store temporary files \n"
  "        (default: $TMDPIR/dmtcp-$USER@$HOST or /tmp/dmtcp-$USER@$HOST)\n"
  "  --join, -j:\n"
  "      Join an existing coordinator, raise error if one doesn't already exist\n"
  "  --new, -n:\n"
  "      Create a new coordinator, raise error if one already exists\n"
  "  --new-coordinator:\n"
  "      Create a new coordinator even if one already exists\n"
  "  --batch, -b:\n"
  "      Enable batch mode i.e. start the coordinator on the same node on\n"
  "        a randomly assigned port (if no port is specified by --port)\n"
  "  --interval, -i, (environment variable DMTCP_CHECKPOINT_INTERVAL):\n"
  "      Time in seconds between automatic checkpoints.\n"
  "      Not allowed if --join is specified\n"
  "      --batch implies -i 3600, unless otherwise specified.\n"
  "  --no-check:\n"
  "      Skip check for valid coordinator and never start one automatically\n"
  "  --quiet, -q, (or set environment variable DMTCP_QUIET = 0, 1, or 2):\n"
  "      Skip banner and NOTE messages; if given twice, also skip WARNINGs\n\n"
  "See http://dmtcp.sf.net/ for more information.\n"
;

static const char* theBanner =
  "DMTCP-" PACKAGE_VERSION " (+ MTCP), Copyright (C) 2006-2011  Jason Ansel,"
  " Michael Rieker,\n"
  "                                       Kapil Arya, and Gene Cooperman\n"
  "This program comes with ABSOLUTELY NO WARRANTY.\n"
  "This is free software, and you are welcome to redistribute it\n"
  "under certain conditions; see COPYING file for details.\n"
  "(Use flag \"-q\" to hide this message.)\n\n"
;

//shift args
#define shift argc--,argv++

dmtcp::vector<RestoreTarget> targets;

#ifdef PID_VIRTUALIZATION
typedef struct {
  RestoreTarget *t;
  bool indep;
} RootTarget;
dmtcp::vector<RootTarget> roots;
void BuildProcessTree();
void ProcessGroupInfo();
void SetupSessions();

#endif

static void restoreSockets(dmtcp::DmtcpCoordinatorAPI& coordinatorAPI,
                           dmtcp::ConnectionState& ckptCoord)
{
  JTRACE ("restoreSockets begin");
  jalib::JSocket& restoreSocket = coordinatorAPI.openRestoreSocket();

  //reconnect to our coordinator
  coordinatorAPI.connectToCoordinator();
  coordinatorAPI.sendCoordinatorHandshake(jalib::Filesystem::GetProgramName(),
                                          compGroup, numPeers,
                                          DMT_RESTART_PROCESS);
  coordinatorAPI.recvCoordinatorHandshake(&coordTstamp);
  JTRACE("Connected to coordinator") (coordTstamp);

  jalib::JSocket& coordinatorSocket = coordinatorAPI.coordinatorSocket();
  // finish sockets restoration
  ckptCoord.doReconnect(coordinatorSocket, restoreSocket);

  JTRACE ("sockets restored!");
}

int main ( int argc, char** argv )
{
  bool autoStartCoordinator=true;
  bool isRestart = true;
  int allowedModes = dmtcp::DmtcpCoordinatorAPI::COORD_ANY;

  initializeJalib();

  if (! getenv(ENV_VAR_QUIET))
    setenv(ENV_VAR_QUIET, "0", 0);

  if (argc == 1) {
    JASSERT_STDERR << theBanner;
    JASSERT_STDERR << "(For help:  " << argv[0] << " --help)\n\n";
    return DMTCP_FAIL_RC;
  }

  //process args
  shift;
  while(true){
    dmtcp::string s = argc>0 ? argv[0] : "--help";
    if(s=="--help" || (s=="-h" && argc==1)){
      JASSERT_STDERR << theUsage;
      //fprintf(stderr, theUsage, "");
      return DMTCP_FAIL_RC;
    }else if(s == "--no-check"){
      autoStartCoordinator = false;
      shift;
    }else if(s == "-j" || s == "--join"){
      allowedModes = dmtcp::DmtcpCoordinatorAPI::COORD_JOIN;
      shift;
    }else if(s == "-n" || s == "--new"){
      allowedModes = dmtcp::DmtcpCoordinatorAPI::COORD_NEW;
      shift;
    }else if(s == "--new-coordinator"){
      allowedModes = dmtcp::DmtcpCoordinatorAPI::COORD_FORCE_NEW;
      shift;
    }else if(s == "-b" || s == "--batch"){
      allowedModes = dmtcp::DmtcpCoordinatorAPI::COORD_BATCH;
      shift;
    }else if(s == "-i" || s == "--interval" ||
             (s.c_str()[0] == '-' && s.c_str()[1] == 'i' &&
              isdigit(s.c_str()[2]) ) ){
      if (isdigit(s.c_str()[2])) { // if -i5, for example
        setenv(ENV_VAR_CKPT_INTR, s.c_str()+2, 1);
        shift;
      } else { // else -i 5
        setenv(ENV_VAR_CKPT_INTR, argv[1], 1);
        shift; shift;
      }
    }else if(argc>1 && (s == "-h" || s == "--host")){
      setenv(ENV_VAR_NAME_HOST, argv[1], 1);
      shift; shift;
    }else if(argc>1 && (s == "-p" || s == "--port")){
      setenv(ENV_VAR_NAME_PORT, argv[1], 1);
      shift; shift;
    }else if(argc>1 && (s == "-t" || s == "--tmpdir")){
      setenv(ENV_VAR_TMPDIR, argv[1], 1);
      shift; shift;
    }else if(s == "-q" || s == "--quiet"){
      *getenv(ENV_VAR_QUIET) = *getenv(ENV_VAR_QUIET) + 1;
      // Just in case a non-standard version of setenv is being used:
      setenv(ENV_VAR_QUIET, getenv(ENV_VAR_QUIET), 1);
      shift;
    }else if( (s.length()>2 && s.substr(0, 2)=="--") ||
              (s.length()>1 && s.substr(0, 1)=="-" ) ) {
      JASSERT_STDERR << "Invalid Argument\n";
      JASSERT_STDERR << theUsage;
      return DMTCP_FAIL_RC;
    }else if(argc>1 && s=="--"){
      shift;
      break;
    }else{
      break;
    }
  }

  dmtcp::UniquePid::setTmpDir(getenv(ENV_VAR_TMPDIR));
  dmtcpTmpDir = dmtcp::UniquePid::getTmpDir();

  jassert_quiet = *getenv(ENV_VAR_QUIET) - '0';

  //make sure JASSERT initializes now, rather than during restart
  Util::initializeLogFile();

  if (jassert_quiet == 0)
    JASSERT_STDERR << theBanner;

  if (autoStartCoordinator)
    dmtcp::DmtcpCoordinatorAPI::startCoordinatorIfNeeded(allowedModes,
                                                         isRestart);

  JTRACE("New dmtcp_restart process; _argc_ ckpt images") (argc);

  bool doAbort = false;
  for(; argc>0; shift){
    dmtcp::string restorename(argv[0]);
    struct stat buf;
    int rc = stat(restorename.c_str(), &buf);
    if (Util::strStartsWith(restorename, "ckpt_") &&
        Util::strEndsWith(restorename, "_files")) {
      continue;
    } else if (!Util::strEndsWith(restorename, ".dmtcp")) {
      JNOTE("File doesn't have .dmtcp extension. Check Usage.")
        (restorename);
      JASSERT_STDERR << theUsage;
      doAbort = true;
    } else if (rc == -1) {
      char error_msg[1024];
      sprintf(error_msg, "\ndmtcp_restart: ckpt image %s", restorename.c_str());
      perror(error_msg);
      doAbort = true;
    } else if (buf.st_uid != getuid()) { /*Could also run if geteuid() matches*/
      printf("\nProcess uid (%d) doesn't match uid (%d) of\n" \
             "checkpoint image (%s).\n" \
	     "This is dangerous.  Aborting for security reasons.\n" \
           "If you still want to do this (at your own risk),\n" \
           "  then modify dmtcp/src/%s:%d and re-compile.\n",
           getuid(), buf.st_uid, restorename.c_str(), __FILE__, __LINE__ - 6);
      doAbort = true;
    }
    if (doAbort) {
      exit(DMTCP_FAIL_RC);
    }

    JTRACE("Will restart ckpt image _argv[0]_") (argv[0]);
    targets.push_back ( RestoreTarget ( argv[0] ) );
  }

  if (targets.size() <= 0) {
    JNOTE("ERROR: No DMTCP checkpoint image(s) found. Check Usage.");
    JASSERT_STDERR << theUsage;
    exit(DMTCP_FAIL_RC);
  }

  // Check that all targets belongs to one computation Group
  // If not - abort
  compGroup = targets[0]._compGroup;
  numPeers = targets[0]._numPeers;
  for(size_t i=0; i<targets.size(); i++){
    JTRACE ( "Check targets: " )
      ( targets[i]._path ) ( targets[i]._compGroup ) ( targets[i]._numPeers );
    if( compGroup != targets[i]._compGroup){
      JASSERT(false)(compGroup)(targets[i]._compGroup)
	.Text("ERROR: Restored programs belongs to different computation IDs");
    }else if( numPeers != targets[i]._numPeers ){
      JASSERT(false)(numPeers)(targets[i]._numPeers)
	.Text("ERROR: Different number of processes saved in checkpoint images");
    }
  }

  SlidingFdTable slidingFd;
  ConnectionToFds conToFd;

  ostringstream out;
  out << "will restore:\n";
  out << "\tfd  -> connection-id\n";
  ConnectionList& connections = ConnectionList::instance();
  ConnectionList::iterator it;
  for (it = connections.begin(); it != connections.end(); ++it ) {
    int fd = slidingFd.getFdFor(it->first);
    conToFd[it->first].push_back (fd);
    out << "\t" << fd << " -> " << (it->first)
        << " -> " << (it->second)->str() << "\n";
  }
  JTRACE ( "Allocating fds for Connections" ) (out.str());

  //------------------------
  WorkerState::setCurrentState ( WorkerState::RESTARTING );
  ConnectionState ckptCoord ( conToFd );
  DmtcpCoordinatorAPI coordinatorAPI;
  restoreSockets(coordinatorAPI, ckptCoord);

#ifndef PID_VIRTUALIZATION
  int i = (int)targets.size();

  //fork into targs.size() processes
  while(--i > 0){
    int cid = fork();
    if(cid==0) break;
    else JASSERT(cid>0);
  }
  RestoreTarget& targ = targets[i];

  JTRACE("forked, restoring process")(i)(targets.size())(targ.upid())(getpid());

  //change UniquePid
  UniquePid::resetOnFork(targ.upid());

  //Reconnect to dmtcp_coordinator
  WorkerState::setCurrentState ( WorkerState::RESTARTING );

  int tmpCoordFd = dup(PROTECTED_COORD_FD);
  JASSERT(tmpCoordFd != -1);
  coordinatorAPI.connectToCoordinator();
  coordinatorAPI.sendCoordinatorHandshake(targ.procname(), targ._compGroup);
  coordinatorAPI.recvCoordinatorHandshake();
  close(tmpCoordFd);

  //restart targets[i]
  targets[i].dupAllSockets ( slidingFd );
  targets[i].mtcpRestart();

  JASSERT ( false ).Text ( "unreachable" );
  return -1;
#else
  //size_t i = targets.size();

  // Create roots vector, assign children to their parents.
  // Delete children that don't exist.
  BuildProcessTree();

  // Process all checkpoints to find one of them that can switch
  // needed Group to foreground.
  ProcessGroupInfo();
  // Create session meta-information in each node of the process tree.
  // Node contains info about all sessions which exists at lower levels.
  // Also node is aware of session leader existence at lower levels.
  SetupSessions();

  /* Create the file to hold the pid/tid maps. */
  openOriginalToCurrentMappingFiles();

  int pgrp_index=-1;
  JTRACE ( "Creating ROOT Processes" )(roots.size());
  for ( size_t j = 0 ; j < roots.size(); ++j )
  {
    if( roots[j].indep == false ){
      // We will restore this process from one of the independent roots.
      continue;
    }
    if (pgrp_index == -1 && !roots[j].t->isInitChild() ){
      pgrp_index = j;
      continue;
    }

    pid_t cid = fork();
    if ( cid == 0 ){
      JTRACE ( "Root of process tree" ) ( _real_getpid() ) ( _real_getppid() );
      if( roots[j].t->isInitChild() ){
        JTRACE ( "Create init-child process" ) ( _real_getpid() )
					       ( _real_getppid() );
        if( fork() )
          _exit(0);
      }
      roots[j].t->CreateProcess(coordinatorAPI, slidingFd);
      JASSERT (false) . Text( "Unreachable" );
    }
    JASSERT ( cid > 0 );
    if( roots[j].t->isInitChild() ){
      waitpid(cid, NULL, 0);
    }
  }

  JTRACE("Restore processes without corresponding Root Target");
  int flat_index = -1;
  size_t j = 0;
  if( pgrp_index < 0 ){ // No root processes at all
    // Find first flat process that can replace currently running
    //   dmtcp_restart context.
    for (j = 0; j < targets.size(); ++j){
      if( !targets[j]._used ){
            // Save first flat-like process to be restored after all others
            flat_index = j;
            j++;
            break;
      }
    }
  }
  // Use j set to 0 (if at least one root non-init-child process exists),
  // or else j set to some value if no such process found.
  for(; j < targets.size(); ++j)
  {
    if( !targets[j]._used ){
      if( pgrp_index < 0 ){
          // Save first flat-like process to be restored after all others
          pgrp_index = j;
          continue;
      }else{
        targets[j].CreateProcess(coordinatorAPI, slidingFd);
        JTRACE("Need in flat-like restore for process")(targets[j].upid());
      }
    }
  }

  if( pgrp_index >=0 ){
    JTRACE("Restore first Root Target")(roots[pgrp_index].t->upid());
    roots[pgrp_index].t->CreateProcess(coordinatorAPI, slidingFd);
  }else if (flat_index >= 0){
    JTRACE("Restore first Flat Target")(targets[flat_index].upid());
    targets[flat_index].CreateProcess(coordinatorAPI, slidingFd );
  }else{
    // FIXME: Under what conditions will this path be exercised?
    JNOTE ("unknown type of target?") (targets[flat_index]._path);
  }
#endif
}

#ifdef PID_VIRTUALIZATION
void BuildProcessTree()
{
  for (size_t j = 0; j < targets.size(); ++j)
  {
    VirtualPidTable& virtualPidTable = targets[j].getVirtualPidTable();
    originalPidTable.insertFromVirtualPidTable ( virtualPidTable );
    if( virtualPidTable.isRootOfProcessTree() == true ){
      // If this process is independent (root of process tree
      RootTarget rt;
      rt.t = &targets[j];
      rt.indep = true;
      roots.push_back(rt);
      targets[j]._used = true;
    }else if( !targets[j]._used ){
      // We set used flag if we use target as somebody's child.
      // If it is used, then there is no need to check if it is root.
      // Iterate through all targets and try to find the one who has
      // this process as their child process.
      JTRACE("Process is not root of process tree: try to find if it has parent");
      bool is_root = true;
      for (size_t i = 0; i < targets.size(); i++) {
        VirtualPidTable & virtualPidTable = targets[i].getVirtualPidTable();
        VirtualPidTable::iterator it;
        // Search inside the child list of target[j], make sure that i != j
        for (it = virtualPidTable.begin(); (i != j) && (it != virtualPidTable.end()) ; it++) {
          UniquePid& childUniquePid = it->second;
          JTRACE("Check child")(childUniquePid)(" parent ")(targets[i].upid())("checked ")(targets[j].upid());
          if (childUniquePid == targets[j].upid()){
            is_root = false;
            break;
          }
        }
      }
      JTRACE("Root detection:")(is_root)(targets[j].upid());
      if( is_root ){
        RootTarget rt;
        rt.t = &targets[j];
        rt.indep = true;
        roots.push_back(rt);
        targets[j]._used = true;
      }
    }

    // Add all children
    VirtualPidTable::iterator it;
    for(it = virtualPidTable.begin(); it != virtualPidTable.end(); it++ ){
      // find target
      bool found = false;
      pid_t childOriginalPid = it->first;
      UniquePid& childUniquePid = it->second;

      for ( size_t i = 0; i < targets.size(); i++ )
      {
        if ( childUniquePid == targets[i].upid() )
        {
          found = 1;
          JTRACE ( "Add child to current target" ) ( targets[j].upid() ) ( childUniquePid );
          targets[i]._used = true;
          targets[j].addChild(&targets[i]);
        }
      }
      if ( !found ){
        JTRACE("Child not found")(childOriginalPid);
        virtualPidTable.erase( childOriginalPid );
      }
    }
  }
}

/*
 * Group processing
 * 1. Divide all processes into sessions
 * 2. Divide processes in each session into groups
 * 3. In each group check that stored foreground values are equal.
 *    If not, something's wrong:  ABORT
 * 4. In each session choose the process that can bring appropriate group
 *    to foreground
 * 5. Serialize information about chosen UniquePIDs in following
 *    format: "COUNT:unique-pid1:unique-pid2:..."
 * 6. Deserialize information from step 5 in forked and restored processes.
 *
 */

class ProcessGroup {
public:
  ProcessGroup(){
    gid = -2;
  }
  pid_t gid;
  vector<RestoreTarget*> targets;
};

class session{
public:
  session(){
    sid = -2;
    fgid = -2;
  }
  pid_t sid;
  pid_t fgid;
  map<pid_t,ProcessGroup> groups;
  typedef map<pid_t,ProcessGroup>::iterator group_it;
  UniquePid upid;
};

void ProcessGroupInfo()
{
  map<pid_t,session> smap;
  map<pid_t,session>::iterator it;

  // 1. divide processes into sessions and groups
  for (size_t j = 0; j < targets.size(); j++)
  {
    VirtualPidTable& virtualPidTable = targets[j].getVirtualPidTable();
    JTRACE("Process ")
      (virtualPidTable.pid())(virtualPidTable.ppid())(virtualPidTable.sid())
      (virtualPidTable.gid())(virtualPidTable.fgid())
      (virtualPidTable.isRootOfProcessTree());

    pid_t sid = virtualPidTable.sid();
    pid_t gid = virtualPidTable.gid();
    //pid_t fgid = virtualPidTable.fgid();

    /*
    // If Group ID doesn't belong to known PIDs, indicate that fact
    //   using -1 value.
    if( !virtualPidTable.pidExists(gid) ){
    JTRACE("DROP gid")(gid);
    virtualPidTable.setgid(-1);
    gid = -1;
    }
    // If foreground Group ID not belongs to known PIDs,
    //   indicate that fact using -1 value.
    if( !virtualPidTable.pidExists(fgid) ){
    JTRACE("DROP fgid")(fgid);
    virtualPidTable.setfgid(-1);
    fgid = -1;
    }
    */

    session &s = smap[sid];
    // if this is first element of this session
    if( s.sid == -2 ){
      s.sid = sid;
    }
    ProcessGroup &g = smap[sid].groups[gid];
    // if this is first element of Group gid
    if( g.gid == -2 ){
      g.gid = gid;
    }
    g.targets.push_back(&targets[j]);
  }

  // 2. Check if foreground setting is correct
  it = smap.begin();
  for(;it != smap.end();it++){
    session &s = it->second;
    session::group_it g_it = s.groups.begin();
    pid_t fgid = -2;
    if( s.sid == -1) // skip default bash session all processes will join
      continue;
    for(; g_it!=s.groups.end();g_it++){
      ProcessGroup &g = g_it->second;
      for(size_t k=0; k<g.targets.size(); k++){
        VirtualPidTable& virtualPidTable = g.targets[k]->getVirtualPidTable();
        pid_t cfgid = virtualPidTable.fgid();
        if( fgid == -2 ){
          fgid = cfgid;
        }else if( fgid != -1 && cfgid != -1 && fgid != cfgid ){
          printf("Error: process from same session stores different"
              " foreground Group ID: %d, %d\n", fgid, cfgid);
          // DEBUG PRINTOUT:
          {
            session::group_it g_it1 = s.groups.begin();
            for(; g_it1!=s.groups.end();g_it1++){
              ProcessGroup &g1 = g_it1->second;
              for(size_t m=0; m<g1.targets.size() ;m++){
                VirtualPidTable& virtualPidTable = g1.targets[m]->getVirtualPidTable();
                pid_t pid = virtualPidTable.pid();
                pid_t ppid = virtualPidTable.ppid();
                pid_t sid = virtualPidTable.sid();
                pid_t cfgid = virtualPidTable.fgid();
                printf("PID=%d, PPID=%d, SID=%d <--> FGID = %d\n",pid,ppid,sid,cfgid);
              }
            }
          }
          abort();
        }
      }
      JTRACE("Checked ")(fgid);
    }
    s.fgid = fgid;
    if( s.groups.find(s.fgid) == s.groups.end() ){
      // foreground Group is missing, don't need to change foreground Group
      s.fgid = -1;
    }

    {
      session::group_it g_it1 = s.groups.begin();
      for(; g_it1!=s.groups.end();g_it1++){
        ProcessGroup &g1 = g_it1->second;
        for(size_t m=0; m<g1.targets.size(); m++){
          VirtualPidTable& virtualPidTable = g1.targets[m]->getVirtualPidTable();
          pid_t pid = virtualPidTable.pid();
          pid_t cfgid = virtualPidTable.fgid();
          JTRACE("PID=%d <--> FGID = %d")(pid)(cfgid);
        }
      }
    }
  }

  // Print out session mapping.
  JTRACE("Session number:")(smap.size());
  it = smap.begin();
  for( ; it != smap.end(); it++ ){
    session &s = it->second;
    JTRACE("Session printout:")(s.sid)(s.fgid)(s.upid.toString().c_str());
    session::group_it g_it = s.groups.begin();
    for(; g_it!=s.groups.end();g_it++){
      ProcessGroup &g = g_it->second;
      JTRACE("\tGroup ID: ")(g.gid);
      /*
         for(k=0; k<g.targets.size() ;k++){
         printf("%d ", g.targets[k]->pid().pid());
         }
         printf("\n");
         */
    }
  }
}

void SetupSessions()
{
  for(size_t j=0; j < roots.size(); j++){
    roots[j].t->setupSessions();
  }

  for(size_t i = 0; i < roots.size(); i++){
    for(size_t j = 0; j < roots.size(); j++){
      if( i == j )
        continue;
      pid_t sid;
      if( (sid = (roots[i].t)->checkDependence(roots[j].t)) >= 0 ){
        // it2 depends on it1
        JTRACE("Root target j depends on Root target i")(i)(roots[i].t->upid())(j)(roots[j].t->upid());
        (roots[i].t)->addRoot(roots[j].t, sid);
        roots[j].indep = false;
      }
    }
  }
}

int openSharedFile(dmtcp::string name, int flags)
{
  int fd;
  // try to create, truncate & open file
  if( (fd = open(name.c_str(), O_EXCL|O_CREAT|O_TRUNC | flags, 0600)) >= 0) {
    return fd;
  }
  if (fd < 0 && errno == EEXIST) {
    if ((fd = open(name.c_str(), flags, 0600)) > 0) {
      return fd;
    }
  }
  // unable to create & open OR open
  JASSERT( false )(name)(strerror(errno)).Text("Cannot open file");
  return -1;
}

static void openOriginalToCurrentMappingFiles()
{
  dmtcp::ostringstream pidMapFile, pidMapCountFile;
  dmtcp::ostringstream shmidListFile, shmidMapFile;
  int fd;

  shmidMapFile << dmtcpTmpDir << "/dmtcpShmidMap."
     << compGroup << "." << std::hex << coordTstamp;
  shmidListFile << dmtcpTmpDir << "/dmtcpShmidList."
     << compGroup << "." << std::hex << coordTstamp;

  pidMapFile << dmtcpTmpDir << "/dmtcpPidMap."
     << compGroup << "." << std::hex << coordTstamp;
  pidMapCountFile << dmtcpTmpDir << "/dmtcpPidMapCount."
     << compGroup << "." << std::hex << coordTstamp;

  // Open and create shmidListFile if it doesn't exist.
  JTRACE("Open dmtcpShmidListFile")(shmidListFile.str());
  fd = openSharedFile(shmidListFile.str(), (O_WRONLY|O_APPEND));
  JASSERT ( fd != -1 );
  JASSERT ( dup2 ( fd, PROTECTED_SHMIDLIST_FD ) == PROTECTED_SHMIDLIST_FD )
	  ( shmidListFile.str() );
  close (fd);

  // Open and create shmidMapFile if it doesn't exist.
  JTRACE("Open dmtcpShmidMapFile")(shmidMapFile.str());
  fd = openSharedFile(shmidMapFile.str(), (O_WRONLY|O_APPEND));
  JASSERT ( fd != -1 );
  JASSERT ( dup2 ( fd, PROTECTED_SHMIDMAP_FD ) == PROTECTED_SHMIDMAP_FD )
	  ( shmidMapFile.str() );
  close (fd);

  // Open and create pidMapFile if it doesn't exist.
  JTRACE("Open dmtcpPidMapFile")(pidMapFile.str());
  fd = openSharedFile(pidMapFile.str(), (O_WRONLY|O_APPEND));
  JASSERT ( fd != -1 );
  JASSERT ( dup2 ( fd, PROTECTED_PIDMAP_FD ) == PROTECTED_PIDMAP_FD )
	  ( pidMapFile.str() );
  close (fd);

  // Open and create pidMapCountFile if it doesn't exist.
  JTRACE("Open dmtcpPidMapCount files for writing")(pidMapCountFile.str());
  fd = openSharedFile(pidMapCountFile.str(), O_RDWR);
  JASSERT ( fd != -1 );
  JASSERT ( dup2 ( fd, PROTECTED_PIDMAPCNT_FD ) == PROTECTED_PIDMAPCNT_FD )
	  ( pidMapCountFile.str() );
  close(fd);

  Util::lockFile(PROTECTED_PIDMAPCNT_FD);

  // Initialize pidMapCountFile with zero value.
  static jalib::JBinarySerializeWriterRaw countwr(pidMapCountFile.str(),
						  PROTECTED_PIDMAPCNT_FD);
  if( countwr.isempty() ){
    JTRACE("pidMapCountFile is empty.  Initialize it with count = 0")
      (pidMapCountFile.str());
    size_t numMaps = 0;
    dmtcp::VirtualPidTable::serializeEntryCount (countwr, numMaps);
    fsync(PROTECTED_PIDMAPCNT_FD);
  }else{
    JTRACE("pidMapCountFile is not empty - do nothing");
  }

  Util::unlockFile(PROTECTED_PIDMAPCNT_FD);
}
#endif

static void runMtcpRestore ( const char* path, int offset,
                             size_t argvSize, size_t envSize)
{
  static dmtcp::string mtcprestart =
    jalib::Filesystem::FindHelperUtility ( "mtcp_restart" );

  // Tell mtcp_restart process to write its debugging information to
  // PROTECTED_STDERR_FD. This way we prevent it from spitting out garbage onto
  // FD_STDERR if it is being used by the user process in a special way.
  char protected_stderr_fd_str[16];
  sprintf(protected_stderr_fd_str, "%d", PROTECTED_STDERR_FD);

#ifdef USE_MTCP_FD_CALLING
  int fd = ConnectionToFds::openMtcpCheckpointFile(path);
  char buf[64];
  char buf2[64];

  sprintf(buf, "%d", fd);
  // gzip_child_pid set by openMtcpCheckpointFile() above.
  sprintf(buf2, "%d", dmtcp::ConnectionToFds::gzip_child_pid);
  char* newArgs[] = {
    ( char* ) mtcprestart.c_str(),
    ( char* ) "--stderr-fd",
    protected_stderr_fd_str,
    ( char* ) "--fd",
    buf,
    ( char* ) "--gzip-child-pid",
    buf2,
    NULL
  };
  if (dmtcp::ConnectionToFds::gzip_child_pid == -1) // If no gzip compression
    newArgs[3] = NULL;
  JTRACE ( "launching mtcp_restart --fd" )(fd)(path);
#else
  char buf[64];

  sprintf(buf, "%d", offset);
  char* newArgs[] = {
    ( char* ) mtcprestart.c_str(),
    ( char* ) "--stderr-fd",
    protected_stderr_fd_str,
    ( char* ) "--offset",
    buf,
    (char*) path,
    NULL
  };
  JTRACE ( "launching mtcp_restart --offset" )(path)(offset);
#endif

  // Create the placeholder for "MTCP_OLDPERS" environment.
  // setenv("MTCP_OLDPERS_DUMMY", "XXXXXXXXXXXXXXXX", 1);
  // FIXME: Put an explanation of the logic below.   -- Kapil
#define ENV_PTR(x) ((char*)(getenv(x) - strlen(x) - 1))
  char* dummyEnviron = NULL;
  const int dummyEnvironIndex = 0; // index in newEnv[]
  const int pathIndex = 1; // index in newEnv[]
  // Eventually, newEnv = {ENV_PTR("MTCP_OLDPERS"), ENV_PTR("PATH"), NULL}
  char* newEnv[3] = {NULL, NULL, NULL};
  // Will put ENV_PTR("MTCP_OLDPERS") here.
  newEnv[dummyEnvironIndex] = (char*) dummyEnviron;
  newEnv[pathIndex] = (getenv("PATH") ? ENV_PTR("PATH") : NULL);

  size_t newArgsSize = 0;
  for (int i = 0; newArgs[i] != 0; i++) {
    newArgsSize += strlen(newArgs[i]) + 1;
  }
  size_t newEnvSize = 0;
  for (int i = 0; newEnv[i] != 0; i++) {
    newEnvSize += strlen(newEnv[i]) + 1;
  }
  size_t originalArgvEnvSize = argvSize + envSize;
  size_t newArgvEnvSize = newArgsSize + newEnvSize + strlen(newArgs[0]);
  size_t argvSizeDiff = originalArgvEnvSize - newArgvEnvSize;
  dummyEnviron = (char*) malloc(argvSizeDiff);
  memset(dummyEnviron, '0', (argvSizeDiff >= 1 ? argvSizeDiff - 1 : 0));
  strncpy(dummyEnviron, ENV_VAR_DMTCP_DUMMY "=0", strlen(ENV_VAR_DMTCP_DUMMY "="));
  dummyEnviron[argvSizeDiff - 1] = '\0';

  newEnv[dummyEnvironIndex] = dummyEnviron;
  JTRACE("Args/Env Sizes") (newArgsSize) (newEnvSize) (argvSize) (envSize) (argvSizeDiff);

  execve ( newArgs[0], newArgs, newEnv );
  JASSERT ( false ) ( newArgs[0] ) ( newArgs[1] ) ( JASSERT_ERRNO )
          .Text ( "exec() failed" );
}
