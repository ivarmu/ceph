
#ifndef __CDIR_H
#define __CDIR_H

#include "include/types.h"
#include "include/config.h"
#include "include/DecayCounter.h"

#include <iostream>
#include <cassert>

#include <ext/rope>
#include <list>
#include <set>
#include <map>
#include <ext/hash_map>
#include <string>
using namespace std;

class CInode;
class CDentry;
class MDS;
class MDCluster;
class Context;


// directory authority types
//  >= 0 is the auth mds
#define CDIR_AUTH_PARENT   -1   // default



// state bits
#define CDIR_STATE_COMPLETE      1   // the complete contents are in cache
#define CDIR_STATE_DIRTY         4   // has been modified since last commit

#define CDIR_STATE_FROZENTREE    8   // root of tree (bounded by exports)
#define CDIR_STATE_FREEZINGTREE 16   // in process of freezing 
#define CDIR_STATE_FROZENDIR    32
#define CDIR_STATE_FREEZINGDIR  64

#define CDIR_STATE_COMMITTING  128   // mid-commit
#define CDIR_STATE_FETCHING    256   // currenting fetching

#define CDIR_STATE_IMPORT     1024   // flag set if this is an import.
#define CDIR_STATE_EXPORT     2-48
#define CDIR_STATE_AUTH       4096   // auth for this dir (hashing doesn't count)
#define CDIR_STATE_PROXY      8192

#define CDIR_STATE_HASHED    16384   // if hashed.  only hashed+auth on auth node.
#define CDIR_STATE_HASHING   32768
#define CDIR_STATE_UNHASHING 65536

#define CDIR_STATE_SYNCBYME      131072
#define CDIR_STATE_PRESYNC       262144
#define CDIR_STATE_SYNCBYAUTH    524288
#define CDIR_STATE_WAITONUNSYNC 1048576



// these state bits are preserved by an import/export
// ...except if the directory is hashed, in which case none of them are!
#define CDIR_MASK_STATE_EXPORTED    (CDIR_STATE_COMPLETE\
                                    |CDIR_STATE_DIRTY)  
#define CDIR_MASK_STATE_IMPORT_KEPT (CDIR_STATE_IMPORT) // see import_dir_block()
#define CDIR_MASK_STATE_EXPORT_KEPT (CDIR_STATE_HASHED\
                                    |CDIR_STATE_FROZENTREE\
                                    |CDIR_STATE_FROZENDIR)

// common states
#define CDIR_STATE_CLEAN   0
#define CDIR_STATE_INITIAL 0  

// directory replication
#define CDIR_REP_ALL       1
#define CDIR_REP_NONE      0
#define CDIR_REP_LIST      2



// pins

#define CDIR_PIN_CHILD     20000
#define CDIR_PIN_OPENED    20001  // open by another node
#define CDIR_PIN_HASHED    20002  // hashed
#define CDIR_PIN_WAITER    20003  // waiter(s)

#define CDIR_PIN_IMPORT    20010  
#define CDIR_PIN_EXPORT    20011
#define CDIR_PIN_FREEZE    20012
#define CDIR_PIN_PROXY     20013  // auth just changed.

#define CDIR_PIN_AUTHPIN   30000



// wait reasons
#define CDIR_WAIT_DENTRY         1  // wait for item to be in cache
     // waiters: path_traverse
     // trigger: handle_discover, fetch_dir_2
#define CDIR_WAIT_COMPLETE       2  // wait for complete dir contents
     // waiters: fetch_dir, commit_dir
     // trigger: fetch_dir_2
#define CDIR_WAIT_FREEZEABLE     4  // hard_pins removed
     // waiters: freeze, freeze_finish
     // trigger: auth_unpin, adjust_nested_auth_pins
#define CDIR_WAIT_UNFREEZE       8  // unfreeze
     // waiters: path_traverse, handle_discover, handle_inode_update,
     //           export_dir_frozen                                   (mdcache)
     //          handle_client_readdir                                (mds)
     // trigger: unfreeze
#define CDIR_WAIT_AUTHPINNABLE  CDIR_WAIT_UNFREEZE
    // waiters: commit_dir                                           (mdstore)
    // trigger: (see CDIR_WAIT_UNFREEZE)
#define CDIR_WAIT_COMMITTED     32  // did commit (who uses this?**)
    // waiters: commit_dir (if already committing)
    // trigger: commit_dir_2
#define CDIR_WAIT_IMPORTED      64  // import finish
    // waiters: import_dir_block
    // triggers: handle_export_dir_finish

#define CDIR_WAIT_EXPORTWARNING 8192    // on bystander.
    // watiers: handle_export_dir_notify
    // triggers: handle_export_dir_warning


#define CDIR_WAIT_SYNC          128
#define CDIR_WAIT_UNSYNC        256

#define CDIR_WAIT_ANY   (0xffff)

#define CDIR_WAIT_ATFREEZEROOT  (CDIR_WAIT_AUTHPINNABLE|\
                                 CDIR_WAIT_UNFREEZE)      // hmm, same same


ostream& operator<<(ostream& out, class CDir& dir);


// CDir
typedef map<string, CDentry*> CDir_map_t;

class CDir {
 public:
  CInode          *inode;

 protected:
  // contents
  CDir_map_t       items;              // use map; ordered list
  size_t           nitems;
  size_t           nauthitems;
  //size_t           namesize;

  // state
  unsigned         state;
  __uint64_t       version;
  __uint64_t       committing_version;

  // authority, replicas
  set<int>         open_by;        // nodes that have me open
  map<int,int>     open_by_nonce;
  int              replica_nonce;
  int              dir_auth;       

  // reference countin/pins
  int              ref;       // reference count
  set<int>         ref_set;

  // lock nesting, freeze
  int        auth_pins;
  int        nested_auth_pins;

  // context
  MDS              *mds;


  // waiters
  multimap<int, Context*> waiting;  // tag -> context
  hash_map< string, multimap<int, Context*> >
	                      waiting_on_dentry;

  // cache control  (defined for authority; hints for replicas)
  int              dir_rep;
  set<int>         dir_rep_by;      // if dir_rep == CDIR_REP_LIST


  // sync (for hashed dirs)
  set<int>   sync_waiting_for_ack;

  DecayCounter popularity[MDS_NPOP];

  friend class CInode;
  friend class MDCache;
  friend class MDiscover;
  friend class MDBalancer;

 public:
  CDir(CInode *in, MDS *mds);



  // -- accessors --
  CInode *get_inode() { return inode; }
  CDir *get_parent_dir();
  inodeno_t ino();

  CDir_map_t::iterator begin() { return items.begin(); }
  CDir_map_t::iterator end() { return items.end(); }
  size_t get_size() { 
#if DEBUG_LEVEL>10
	if (nitems != items.size()) {
	  for (CDir_map_t::iterator it = items.begin();
		   it != items.end();
		   it++)
		cout << "item " << (*it).first << endl;
	  cout << "nitems " << nitems << endl;
	  assert(nitems == items.size());
	}
#endif
	if ( is_auth() && !is_hashed()) assert(nauthitems == nitems);
	if (!is_auth() && !is_hashed()) assert(nauthitems == 0);
	return nitems; 
  }
  size_t get_auth_size() { 
	assert(nauthitems <= nitems);
	return nauthitems; 
  }

  float get_popularity() {
	return popularity[0].get();
  }
  

  // -- manipulation --
  void add_child(CDentry *d);
  void remove_child(CDentry *d);
  CDentry* lookup(const string& n);



  // -- authority --
  int authority();
  int dentry_authority(const string& d);

  bool is_open_by_anyone() { return !open_by.empty(); }
  bool is_open_by(int mds) { return open_by.count(mds); }
  int get_open_by_nonce(int mds) {
	map<int,int>::iterator it = open_by_nonce.find(mds);
	return it->second;
  }
  set<int>::iterator open_by_begin() { return open_by.begin(); }
  set<int>::iterator open_by_end() { return open_by.end(); }
  set<int>& get_open_by() { return open_by; }

  int get_replica_nonce() { assert(!is_auth()); return replica_nonce; }
  
  
  


  // -- state --
  unsigned get_state() { return state; }
  void reset_state(unsigned s) { 
	state = s; 
	dout(10) << *this << " state reset" << endl;
  }
  void state_clear(unsigned mask) {	
	state &= ~mask; 
	dout(10) << *this << " state -" << mask << " = " << state << endl;
  }
  void state_set(unsigned mask) { 
	state |= mask; 
	dout(10) << *this << " state +" << mask << " = " << state << endl;
  }
  unsigned state_test(unsigned mask) { return state & mask; }

  bool is_complete() { return state & CDIR_STATE_COMPLETE; }
  bool is_dirty() { return state_test(CDIR_STATE_DIRTY); }

  bool is_auth() { return state & CDIR_STATE_AUTH; }
  bool is_proxy() { return state & CDIR_STATE_PROXY; }
  bool is_import() { return state & CDIR_STATE_IMPORT; }
  bool is_export() { return state & CDIR_STATE_EXPORT; }

  bool is_hashed() { return state & CDIR_STATE_HASHED; }
  bool is_hashing() { return state & CDIR_STATE_HASHING; }
  bool is_unhashing() { return state & CDIR_STATE_UNHASHING; }

  bool is_rep() { 
	if (dir_rep == CDIR_REP_NONE) return false;
	return true;
  }
  int get_rep_count(MDCluster *mdc);
  
  void update_auth(int whoami);


  // -- dirtyness --
  __uint64_t get_version() { return version; }
  void float_version(__uint64_t ge) {
	if (version < ge)
	  version = ge;
  }
  __uint64_t get_committing_version() { 
	return committing_version;
  }
  // as in, we're committing the current version.
  void set_committing_version() { committing_version = version; }
  void mark_dirty();
  void mark_clean();
  void mark_complete() { state_set(CDIR_STATE_COMPLETE); }
  bool is_clean() { return !state_test(CDIR_STATE_DIRTY); }


  // -- popularity --
  void hit(int dir);


  // -- encoded state --
  crope encode_basic_state();
  int decode_basic_state(crope r, int off=0);



  // -- reference counting --
  void put(int by);
  void get(int by);
  bool is_pinned_by(int by) {
	return ref_set.count(by);
  }


  
  // -- sync --
  bool is_sync() { return is_syncbyme() || is_syncbyauth(); }
  bool is_syncbyme() { return state & CDIR_STATE_SYNCBYME; }
  bool is_syncbyauth() { return state & CDIR_STATE_SYNCBYAUTH; }
  bool is_presync() { return state & CDIR_STATE_PRESYNC; }
  bool is_waitonnsync() { return state & CDIR_STATE_WAITONUNSYNC; }

  
  // -- waiters --
  void add_waiter(int tag, Context *c);
  void add_waiter(int tag,
				  const string& dentry,
				  Context *c);
  void take_waiting(int mask, list<Context*>& ls);  // includes dentry waiters
  void take_waiting(int mask, 
					const string& dentry, 
					list<Context*>& ls);  
  void finish_waiting(int mask, int result = 0);    // ditto


  // -- auth pins --
  bool can_auth_pin() { return !(is_frozen() || is_freezing()); }
  int is_auth_pinned() { return auth_pins; }
  void auth_pin();
  void auth_unpin();
  void adjust_nested_auth_pins(int inc);
  void on_freezeable();

  // -- freezing --
  void freeze_tree(Context *c);
  void freeze_tree_finish(Context *c);
  void unfreeze_tree();

  void freeze_dir(Context *c);
  void freeze_dir_finish(Context *c);
  void unfreeze_dir();

  bool is_freezing() { return is_freezing_tree() || is_freezing_dir(); }
  bool is_freezing_tree();
  bool is_freezing_tree_root() { return state & CDIR_STATE_FREEZINGTREE; }
  bool is_freezing_dir() { return state & CDIR_STATE_FREEZINGDIR; }

  bool is_frozen() { return is_frozen_dir() || is_frozen_tree(); }
  bool is_frozen_tree();
  bool is_frozen_tree_root() { return state & CDIR_STATE_FROZENTREE; }
  bool is_frozen_dir() { return state & CDIR_STATE_FROZENDIR; }
  
  bool is_freezeable() {
	if (auth_pins == 0 && nested_auth_pins == 0) return true;
	return false;
  }
  bool is_freezeable_dir() {
	if (auth_pins == 0) return true;
	return false;
  }



  // debuggin bs
  void dump(int d = 0);
  void dump_to_disk(MDS *m);
};



#endif
