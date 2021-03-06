
#include "JournalingObjectStore.h"

#include "common/debug.h"

#define dout_subsys ceph_subsys_journal
#undef dout_prefix
#define dout_prefix *_dout << "journal "



void JournalingObjectStore::journal_start()
{
  dout(10) << "journal_start" << dendl;
  finisher.start();
}
 
void JournalingObjectStore::journal_stop() 
{
  dout(10) << "journal_stop" << dendl;
  finisher.stop();
  if (journal) {
    journal->close();
    delete journal;
    journal = 0;
  }
}

int JournalingObjectStore::journal_replay(uint64_t fs_op_seq)
{
  dout(10) << "journal_replay fs op_seq " << fs_op_seq << dendl;

  if (g_conf->journal_replay_from) {
    dout(0) << "journal_replay forcing replay from " << g_conf->journal_replay_from
	    << " instead of " << fs_op_seq << dendl;
    // the previous op is the last one committed
    fs_op_seq = g_conf->journal_replay_from - 1;
  }

  journal_lock.Lock();
  op_seq = fs_op_seq;
  committed_seq = fs_op_seq;
  committing_seq = fs_op_seq;
  applied_seq = fs_op_seq;
  journal_lock.Unlock();

  if (!journal)
    return 0;

  int err = journal->open(op_seq);
  if (err < 0) {
    char buf[80];
    dout(3) << "journal_replay open failed with " 
	    << strerror_r(-err, buf, sizeof(buf)) << dendl;
    delete journal;
    journal = 0;
    return err;
  }

  journal_lock.Lock();

  replaying = true;

  int count = 0;
  while (1) {
    bufferlist bl;
    uint64_t seq = op_seq + 1;
    if (!journal->read_entry(bl, seq)) {
      dout(3) << "journal_replay: end of journal, done." << dendl;
      break;
    }

    if (seq <= op_seq) {
      dout(3) << "journal_replay: skipping old op seq " << seq << " <= " << op_seq << dendl;
      continue;
    }
    assert(op_seq == seq-1);
    
    dout(3) << "journal_replay: applying op seq " << seq << dendl;
    bufferlist::iterator p = bl.begin();
    list<Transaction*> tls;
    while (!p.end()) {
      Transaction *t = new Transaction(p);
      tls.push_back(t);
    }

    open_ops++;
    journal_lock.Unlock();
    int r = do_transactions(tls, seq);
    journal_lock.Lock();
    open_ops--;
    cond.Signal();

    op_seq = applied_seq = seq;

    while (!tls.empty()) {
      delete tls.front(); 
      tls.pop_front();
    }

    dout(3) << "journal_replay: r = " << r << ", op_seq now " << op_seq << dendl;
    assert(op_seq == seq);
    seq++;  // we expect the next op
  }

  replaying = false;

  journal_lock.Unlock();

  // done reading, make writeable.
  journal->make_writeable();

  return count;
}


// ------------------------------------

uint64_t JournalingObjectStore::op_apply_start(uint64_t op) 
{
  Mutex::Locker l(journal_lock);
  return _op_apply_start(op);
}

uint64_t JournalingObjectStore::_op_apply_start(uint64_t op) 
{
  assert(journal_lock.is_locked());

  // if we ops are blocked, or there are already people (left) in
  // line, get in line.
  if (blocked || !ops_apply_blocked.empty()) {
    Cond cond;
    ops_apply_blocked.push_back(&cond);
    dout(10) << "op_apply_start " << op << " blocked (getting in back of line)" << dendl;
    // sleep until we are not blocked AND we are at the front of line
    while (blocked || ops_apply_blocked.front() != &cond)
      cond.Wait(journal_lock);
    dout(10) << "op_apply_start " << op << " woke (at front of line)" << dendl;
    ops_apply_blocked.pop_front();
    if (!ops_apply_blocked.empty()) {
      dout(10) << "op_apply_start " << op << "  ...and kicking next in line" << dendl;
      ops_apply_blocked.front()->Signal();
    }
  }
  dout(10) << "op_apply_start " << op << " open_ops " << open_ops << " -> " << (open_ops+1) << dendl;
  assert(!blocked);
  open_ops++;
  return op;
}

void JournalingObjectStore::op_apply_finish(uint64_t op) 
{
  journal_lock.Lock();
  dout(10) << "op_apply_finish " << op << " open_ops " << open_ops << " -> " << (open_ops-1) << dendl;
  if (--open_ops == 0)
    cond.Signal();

  // there can be multiple applies in flight; track the max value we
  // note.  note that we can't _read_ this value and learn anything
  // meaningful unless/until we've quiesced all in-flight applies.
  if (op > applied_seq)
    applied_seq = op;

  journal_lock.Unlock();
}

uint64_t JournalingObjectStore::op_submit_start()
{
  journal_lock.Lock();
  uint64_t op = ++op_seq;
  dout(10) << "op_submit_start " << op << dendl;
  ops_submitting.push_back(op);
  return op;
}

void JournalingObjectStore::op_submit_finish(uint64_t op)
{
  dout(10) << "op_submit_finish " << op << dendl;
  if (op != ops_submitting.front()) {
    dout(0) << "op_submit_finish " << op << " expected " << ops_submitting.front()
	    << ", OUT OF ORDER" << dendl;
    assert(0 == "out of order op_submit_finish");
  }
  ops_submitting.pop_front();
  journal_lock.Unlock();
}


// ------------------------------------------

/*
 * this may (will generally) get called by an op_queue thread holding
 * an open_ops reference.  it should block only long enough for the
 * commit to _start_ waiting for open_ops, but not longer or else we
 * will deadlock.
 *
 * caller must hold journal_lock.
 */
void JournalingObjectStore::_trigger_commit(uint64_t seq)
{
  assert(journal_lock.is_locked());
  dout(10) << "trigger_commit " << seq << dendl;
  force_commit = true;
  while (!blocked && committing_seq < seq) {
    dout(20) << "trigger_commit not blocked and seq " << seq << " > committing " << committing_seq << dendl;
    cond.Wait(journal_lock);
  }
  dout(10) << "trigger_commit triggered, will commit something >= " << seq << dendl;
}

bool JournalingObjectStore::commit_start() 
{
  bool ret = false;

  journal_lock.Lock();
  dout(10) << "commit_start op_seq " << op_seq
	   << ", applied_seq " << applied_seq
	   << ", committed_seq " << committed_seq << dendl;
  blocked = true;
  cond.Signal();  // for trigger_commit() caller
  while (open_ops > 0) {
    dout(10) << "commit_start blocked, waiting for " << open_ops << " open ops" << dendl;
    cond.Wait(journal_lock);
  }
  dout(10) << "commit_start blocked, all open_ops have completed" << dendl;
  assert(open_ops == 0);

  if (applied_seq == committed_seq && !force_commit) {
    dout(10) << "commit_start nothing to do" << dendl;
    blocked = false;
    if (!ops_apply_blocked.empty())
      ops_apply_blocked.front()->Signal();
    assert(commit_waiters.empty());
    goto out;
  }

  force_commit = false;

  com_lock.Lock();
  // we can _only_ read applied_seq here because open_ops == 0 (we've
  // quiesced all in-flight applies).
  committing_seq = applied_seq;
  com_lock.Unlock();

  dout(10) << "commit_start committing " << committing_seq << ", still blocked" << dendl;
  ret = true;

 out:
  if (journal)
    journal->commit_start();  // tell the journal too
  journal_lock.Unlock();
  return ret;
}

void JournalingObjectStore::commit_started() 
{
  Mutex::Locker l(journal_lock);
  // allow new ops. (underlying fs should now be committing all prior ops)
  dout(10) << "commit_started committing " << committing_seq << ", unblocking" << dendl;
  blocked = false;
  if (!ops_apply_blocked.empty())
    ops_apply_blocked.front()->Signal();
}

void JournalingObjectStore::commit_finish()
{
  Mutex::Locker l(journal_lock);
  dout(10) << "commit_finish thru " << committing_seq << dendl;
  
  if (journal)
    journal->committed_thru(committing_seq);

  com_lock.Lock();
  committed_seq = committing_seq;
  com_lock.Unlock();
  
  map<version_t, vector<Context*> >::iterator p = commit_waiters.begin();
  while (p != commit_waiters.end() &&
	 p->first <= committing_seq) {
    finisher.queue(p->second);
    commit_waiters.erase(p++);
  }
}

void JournalingObjectStore::_op_journal_transactions(list<ObjectStore::Transaction*>& tls, uint64_t op,
						     Context *onjournal, TrackedOpRef osd_op)
{
  assert(journal_lock.is_locked());
  dout(10) << "op_journal_transactions " << op << " " << tls << dendl;
    
  if (journal && journal->is_writeable()) {
    bufferlist tbl;
    unsigned data_len = 0, data_align = 0;
    for (list<ObjectStore::Transaction*>::iterator p = tls.begin(); p != tls.end(); p++) {
      ObjectStore::Transaction *t = *p;
      if (t->get_data_length() > data_len &&
	  (int)t->get_data_length() >= g_conf->journal_align_min_size) {
	data_len = t->get_data_length();
	data_align = (t->get_data_alignment() - tbl.length()) & ~CEPH_PAGE_MASK;
      }
      ::encode(*t, tbl);
    }
    journal->submit_entry(op, tbl, data_align, onjournal, osd_op);
  } else if (onjournal)
    commit_waiters[op].push_back(onjournal);
}
