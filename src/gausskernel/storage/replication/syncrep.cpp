/* -------------------------------------------------------------------------
 *
 * syncrep.cpp
 *
 * Synchronous replication is new as of PostgreSQL 9.1.
 *
 * If requested, transaction commits wait until their commit LSN is
 * acknowledged by the sync standby.
 *
 * This module contains the code for waiting and release of backends.
 * All code in this module executes on the primary. The core streaming
 * replication transport remains within WALreceiver/WALsender modules.
 *
 * The essence of this design is that it isolates all logic about
 * waiting/releasing onto the primary. The primary defines which standbys
 * it wishes to wait for. The standby is completely unaware of the
 * durability requirements of transactions on the primary, reducing the
 * complexity of the code and streamlining both standby operations and
 * network bandwidth because there is no requirement to ship
 * per-transaction state information.
 *
 * Replication is either synchronous or not synchronous (async). If it is
 * async, we just fastpath out of here. If it is sync, then we wait for
 * the write or flush location on the standby before releasing the waiting backend.
 * Further complexity in that interaction is expected in later releases.
 *
 * The best performing way to manage the waiting backends is to have a
 * single ordered queue of waiting backends, so that we can avoid
 * searching the through all waiters each time we receive a reply.
 *
 * In 9.1 we support only a single synchronous standby, chosen from a
 * priority list of synchronous_standby_names. Before it can become the
 * synchronous standby it must have caught up with the primary; that may
 * take some time. Once caught up, the current highest priority standby
 * will release waiters from the queue.
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 2010-2012, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *      src/gausskernel/storage/replication/syncrep.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include <unistd.h>

#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/syncrep_gramparse.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "replication/shared_storage_walreceiver.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/ps_status.h"
#include "utils/distribute_test.h"
#include "replication/dcf_replication.h"

/*
 * To control whether a master configured with synchronous commit is
 * allowed to stop waiting for standby WAL sync when there is synchronous
 * standby WAL senders are disconnected.
 */
volatile bool most_available_sync = false;

const int MAX_SYNC_REP_RETRY_COUNT = 1000;
const int SYNC_REP_SLEEP_DELAY = 1000;

typedef enum SyncStandbyNumState {
    STANDBIES_EMPTY = 0,
    STANDBIES_NOT_ENOUGH,
    STANDBIES_ENOUGH
} SyncStandbyNumState;
static SyncStandbyNumState check_sync_standbys_num(const List* sync_standbys);
static bool judge_sync_standbys_num(const List* sync_standbys, SyncStandbyNumState* state);

static void SyncRepQueueInsert(int mode);
static bool SyncRepCancelWait(void);
static void SyncRepWaitCompletionQueue();
static void SyncRepNotifyComplete();

static void SyncRepGetStandbyGroupAndPriority(int* gid, int* prio);
#ifndef ENABLE_MULTIPLE_NODES
static bool SyncRepGetSyncLeftTime(XLogRecPtr XactCommitLSN, TimestampTz* leftTime);
#endif
static void SyncRepGetOldestSyncRecPtr(XLogRecPtr* receivePtr, XLogRecPtr* writePtr, XLogRecPtr* flushPtr,
                                       XLogRecPtr* replayPtr, List* sync_standbys);
static void SyncRepGetNthLatestSyncRecPtr(XLogRecPtr* receivePtr, XLogRecPtr* writePtr, XLogRecPtr* flushPtr,
                                          XLogRecPtr* replayPtr, List* sync_standbys, uint8 nth);

static void SyncPaxosQueueInsert(void);
static void SyncPaxosCancelWait(void);
static int SyncPaxosWakeQueue(void);

#ifdef USE_ASSERT_CHECKING
static bool SyncRepQueueIsOrderedByLSN(int mode);
static bool SyncPaxosQueueIsOrderedByLSN(void);
#endif

static List *SyncRepGetSyncStandbysPriority(bool *am_sync, int groupid, List** catchup_standbys = NULL);
static List *SyncRepGetSyncStandbysQuorum(bool *am_sync, int groupid, List** catchup_standbys = NULL);
static inline void free_sync_standbys_list(List* sync_standbys);
static bool judge_sync_standbys_num(const List* sync_standbys, SyncStandbyNumState* state);
static int cmp_lsn(const void *a, const void *b);
static bool DelayIntoMostAvaSync(bool checkSyncNum, SyncStandbyNumState state = STANDBIES_EMPTY);

typedef struct TransContext {
    /* for global */
    List* source;
    bool has_star;
    List *existers;
    int SyncRepAllCount;
    int SyncRepMinPossib;

    /* for each group*/
    SyncRepConfigData* conf;
    bool is_star;

    /* results */
    bool success;
    List *SyncRepConfig;
    int SyncRepConfigGroups;
    int SyncRepMaxPossib;
} TransContext;
static TransContext* create_transform_context();
static void bind_transform_context(TransContext *tcxt, SyncRepConfigData *conf);
static void advance_transform_result(TransContext *tcxt);
static void finalize_transform_result(TransContext *tcxt);
static void clear_transform_context(TransContext *tcxt);
static void destroy_transform_context(TransContext *tcxt);
static bool analyze_star_and_num(TransContext *tcxt);
static bool analyze_duplicate_names(TransContext *tcxt);
static void transform_synchronous_standby_names(TransContext *tcxt);

#define CATCHUP_XLOG_DIFF(ptr1, ptr2, amount) \
    XLogRecPtrIsInvalid(ptr1) ? false : (XLByteDifference(ptr2, ptr1) < amount)

/*
 * sync_standbys_list is a two-dimensional array means sync standby nodes in groups
 * The structure is as follows: List[IntList[int, int,...], IntList[int, int, ...], ...]
 */
static inline void free_sync_standbys_list(List* sync_standbys)
{
    ListCell *lc = NULL;
    foreach(lc, sync_standbys) {
        list_free((List*)lfirst(lc));
    }
    list_free(sync_standbys);
}

/*
 * Determine whether to wait for standby catching up, if requested by user.
 * 
 * Return true if it is need to wait for catching up(synchronous replication),
 * return false if don't wait for catching up.
 */
bool SynRepWaitCatchup(XLogRecPtr XactCommitLSN)
{
#ifndef ENABLE_MULTIPLE_NODES
    /* 
     * When most_available_sync is off or catchup2normal_wait_time is not set,
     * return true.
     */
    if (!t_thrd.walsender_cxt.WalSndCtl->most_available_sync ||
        u_sess->attr.attr_storage.catchup2normal_wait_time < 0) {
        return true;
    }

    static const TimestampTz catchup2normalWaitTime = u_sess->attr.attr_storage.catchup2normal_wait_time * 1000;
    TimestampTz syncLeftTime = 0;

    /* 
     * SyncRepGetSyncLeftTime() return false means that there is at lease one
     * sync standby, or no standby in catchup, so it is need to wait for
     * synchronous replication.
     */
    if (!SyncRepGetSyncLeftTime(XactCommitLSN, &syncLeftTime)) {
        return true;
    }
    if (syncLeftTime == 0 || syncLeftTime > catchup2normalWaitTime) {
        return false;
    }
#endif
    return true;
}

/*
 * Wait for synchronous replication, if requested by user.
 *
 * Initially backends start in state SYNC_REP_NOT_WAITING and then
 * change that state to SYNC_REP_WAITING before adding ourselves
 * to the wait queue. During SyncRepWakeQueue() a WALSender changes
 * the state to SYNC_REP_WAIT_COMPLETE once replication is confirmed.
 * This backend then resets its state to SYNC_REP_NOT_WAITING.
 */
void SyncRepWaitForLSN(XLogRecPtr XactCommitLSN, bool enableHandleCancel)
{
    char *new_status = NULL;
    const char *old_status = NULL;
    int mode = u_sess->attr.attr_storage.sync_rep_wait_mode;

    /*
     * Fast exit if user has not requested sync replication, or there are no
     * sync replication standby names defined. Note that those standbys don't
     * need to be connected.
     */
    if (!u_sess->attr.attr_storage.enable_stream_replication || !SyncRepRequested() ||
        !SyncStandbysDefined() || (t_thrd.postmaster_cxt.HaShmData->current_mode == NORMAL_MODE))
        return;

    Assert(SHMQueueIsDetached(&(t_thrd.proc->syncRepLinks)));
    Assert(t_thrd.walsender_cxt.WalSndCtl != NULL);

    /* Prevent the queue cleanups to be influenced by external interruptions */
    HOLD_INTERRUPTS();

    (void)LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
    Assert(t_thrd.proc->syncRepState == SYNC_REP_NOT_WAITING);

    /*
     * We don't wait for sync rep if WalSndCtl->sync_standbys_defined is not
     * set.  See SyncRepUpdateSyncStandbysDefined.
     *
     * Also check that the standby hasn't already replied. Unlikely race
     * condition but we'll be fetching that cache line anyway so its likely to
     * be a low cost check. We don't wait for sync rep if no sync standbys alive
     */
    if (!t_thrd.walsender_cxt.WalSndCtl->sync_standbys_defined ||
        XLByteLE(XactCommitLSN, t_thrd.walsender_cxt.WalSndCtl->lsn[mode]) ||
        (t_thrd.walsender_cxt.WalSndCtl->sync_master_standalone && !IS_SHARED_STORAGE_MODE &&
         !DelayIntoMostAvaSync(false)) ||
        !SynRepWaitCatchup(XactCommitLSN)) {
        LWLockRelease(SyncRepLock);
        RESUME_INTERRUPTS();
        return;
    }

    /*
     * Set our waitLSN so WALSender will know when to wake us, and add
     * ourselves to the queue.
     */
    t_thrd.proc->waitLSN = XactCommitLSN;
    t_thrd.proc->syncRepState = SYNC_REP_WAITING;
    SyncRepQueueInsert(mode);
    Assert(SyncRepQueueIsOrderedByLSN(mode));
    LWLockRelease(SyncRepLock);

    /* Alter ps display to show waiting for sync rep. */
    if (u_sess->attr.attr_common.update_process_title) {
        int len;
        errno_t ret = EOK;
        int rc = 0;
#define NEW_STATUS_LEN 33
        old_status = get_ps_display(&len);
        new_status = (char *)palloc(len + NEW_STATUS_LEN);
        if (len > 0) {
            ret = memcpy_s(new_status, len + NEW_STATUS_LEN, old_status, len);
            securec_check(ret, "\0", "\0");
        }

        rc = snprintf_s(new_status + len, NEW_STATUS_LEN, NEW_STATUS_LEN - 1, " waiting for %X/%X",
                        (uint32)(XactCommitLSN >> 32), (uint32)XactCommitLSN);
        securec_check_ss(rc, "", "");

        set_ps_display(new_status, false);
        new_status[len] = '\0'; /* truncate off " waiting ..." */
    }

    WaitState oldStatus = pgstat_report_waitstatus(STATE_WAIT_WALSYNC);

    /*
     * Wait for specified LSN to be confirmed.
     *
     * Each proc has its own wait latch, so we perform a normal latch
     * check/wait loop here.
     */
    for (;;) {
        /* Must reset the latch before testing state. */
        ResetLatch(&t_thrd.proc->procLatch);

#ifdef ENABLE_DISTRIBUTE_TEST
        if (TEST_STUB(DN_STANDBY_SLEEPIN_SYNCCOMMIT, stub_sleep_emit)) {
            ereport(get_distribute_test_param()->elevel,
                    (errmsg("sleep_emit happen during SyncRepWaitForLSN  time:%ds, stub_name:%s",
                            get_distribute_test_param()->sleep_time, get_distribute_test_param()->test_stub_name)));
        }
#endif

        /*
         * Acquiring the lock is not needed, the latch ensures proper barriers.
         * If it looks like we're done, we must really be done, because once
         * walsender changes the state to SYNC_REP_WAIT_COMPLETE, it will never
         * update it again, so we can't be seeing a stale value in that case.
         */
        if (t_thrd.proc->syncRepState == SYNC_REP_WAIT_COMPLETE && !DelayIntoMostAvaSync(true))
            break;

        /*
         * If a wait for synchronous replication is pending, we can neither
         * acknowledge the commit nor raise ERROR or FATAL.  The latter would
         * lead the client to believe that the transaction aborted, which
         * is not true: it's already committed locally. The former is no good
         * either: the client has requested synchronous replication, and is
         * entitled to assume that an acknowledged commit is also replicated,
         * which might not be true. So in this case we issue a WARNING (which
         * some clients may be able to interpret) and shut off further output.
         * We do NOT reset t_thrd.int_cxt.ProcDiePending, so that the process will die after
         * the commit is cleaned up.
         */
        if (t_thrd.int_cxt.ProcDiePending || t_thrd.proc_cxt.proc_exit_inprogress) {
            ereport(WARNING,
                    (errcode(ERRCODE_ADMIN_SHUTDOWN),
                     errmsg("canceling the wait for synchronous replication and terminating connection due to "
                            "administrator command"),
                     errdetail("The transaction has already committed locally, but might not have been replicated to "
                               "the standby.")));
            t_thrd.postgres_cxt.whereToSendOutput = DestNone;
            if (SyncRepCancelWait()) {
                break;
            }
        }

        /*
         * It's unclear what to do if a query cancel interrupt arrives.  We
         * can't actually abort at this point, but ignoring the interrupt
         * altogether is not helpful, so we just terminate the wait with a
         * suitable warning.
         */
        if (enableHandleCancel && t_thrd.int_cxt.QueryCancelPending) {
            /* reset query cancel signal after vacuum. */
            if (!t_thrd.vacuum_cxt.in_vacuum) {
                t_thrd.int_cxt.QueryCancelPending = false;
            }
            ereport(WARNING,
                    (errmsg("canceling wait for synchronous replication due to user request"),
                     errdetail("The transaction has already committed locally, but might not have been replicated to "
                               "the standby.")));
            if (SyncRepCancelWait()) {
                break;
            }
        }

        /*
         * If the postmaster dies, we'll probably never get an
         * acknowledgement, because all the wal sender processes will exit. So
         * just bail out.
         */
        if (!PostmasterIsAlive()) {
            t_thrd.int_cxt.ProcDiePending = true;
            t_thrd.postgres_cxt.whereToSendOutput = DestNone;
            if (SyncRepCancelWait()) {
                break;
            }
        }

        /*
         * If we modify the syncmode dynamically, we'll stop wait.
         * Reload config file here to update most_available_sync if it's modified
         * dynamically.
         */
        reload_configfile();
        if ((t_thrd.walsender_cxt.WalSndCtl->sync_master_standalone && !IS_SHARED_STORAGE_MODE &&
             !DelayIntoMostAvaSync(false)) ||
            u_sess->attr.attr_storage.guc_synchronous_commit <= SYNCHRONOUS_COMMIT_LOCAL_FLUSH) {
            ereport(WARNING,
                    (errmsg("canceling wait for synchronous replication due to syncmaster standalone."),
                     errdetail("The transaction has already committed locally, but might not have been replicated to "
                               "the standby.")));
            if (SyncRepCancelWait()) {
                break;
            }
        }

        /*
         * For gs_rewind, if standby or secondary is not connected, we'll stop wait
         */
        if (strcmp(u_sess->attr.attr_common.application_name, "gs_rewind") == 0) {
            if (IS_DN_MULTI_STANDYS_MODE() ||
                (IS_DN_DUMMY_STANDYS_MODE() &&
                 !(WalSndInProgress(SNDROLE_PRIMARY_STANDBY | SNDROLE_PRIMARY_DUMMYSTANDBY)))) {
                ereport(WARNING,
                        (errmsg("canceling wait for synchronous replication due to client is gs_rewind and "
                                "secondary is not connected."),
                         errdetail("The transaction has already committed locally, but might not have been replicated "
                                   "to the standby.")));
                if (SyncRepCancelWait()) {
                    break;
                }
            }
        }

        /*
         * For case that query cancel pending or proc die pending signal not reached, if current
         * session is set closed, we'll stop wait
         */
        if (u_sess->status == KNL_SESS_CLOSE) {
            ereport(WARNING,
                    (errmsg("canceling wait for synchronous replication due to session close."),
                     errdetail("The transaction has already committed locally, but might not have been replicated to "
                               "the standby.")));
            if (SyncRepCancelWait()) {
                break;
            }
        }

        /*
         * Wait on latch.  Any condition that should wake us up will set the
         * latch, so no need for timeout.
         */
        WaitLatch(&t_thrd.proc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 3000L);
    }

    /* Make sure that syncRepLinks is read after syncRepState */
    pg_read_barrier();

    /* Leader informs following procs */
    if (t_thrd.proc->syncRepLinks.next != NULL) {
        SyncRepNotifyComplete();
    }

    (void)pgstat_report_waitstatus(oldStatus);

    /*
     * WalSender has checked our LSN and has removed us from queue. Clean up
     * state and leave.  It's OK to reset these shared memory fields without
     * holding SyncRepLock, because any walsenders will ignore us anyway when
     * we're not on the queue. pg_read_barrier() has been invoked after for
     * loop to make sure the changes to the queue link is visible.
     */
    Assert(SHMQueueIsDetached(&(t_thrd.proc->syncRepLinks)));
    t_thrd.proc->syncRepState = SYNC_REP_NOT_WAITING;
    t_thrd.proc->syncRepInCompleteQueue = false;
    t_thrd.proc->waitLSN = 0;

    if (new_status != NULL) {
        /* Reset ps display */
        set_ps_display(new_status, false);
        pfree(new_status);
        new_status = NULL;
    }

    RESUME_INTERRUPTS();
}

/*
 * Insert t_thrd.proc into the specified SyncRepQueue, maintaining sorted invariant.
 *
 * Usually we will go at tail of queue, though it's possible that we arrive
 * here out of order, so start at tail and work back to insertion point.
 */
static void SyncRepQueueInsert(int mode)
{
    PGPROC *proc = NULL;

    Assert(mode >= 0 && mode < NUM_SYNC_REP_WAIT_MODE);
    proc = (PGPROC *)SHMQueuePrev(&(t_thrd.walsender_cxt.WalSndCtl->SyncRepQueue[mode]),
                                  &(t_thrd.walsender_cxt.WalSndCtl->SyncRepQueue[mode]),
                                  offsetof(PGPROC, syncRepLinks));

    while (proc != NULL) {
        /*
         * Stop at the queue element that we should after to ensure the queue
         * is ordered by LSN. The same lsn is allowed in sync queue.
         */
        if (XLByteLE(proc->waitLSN, t_thrd.proc->waitLSN))
            break;

        proc = (PGPROC *)SHMQueuePrev(&(t_thrd.walsender_cxt.WalSndCtl->SyncRepQueue[mode]), &(proc->syncRepLinks),
                                      offsetof(PGPROC, syncRepLinks));
    }

    if (proc != NULL)
        SHMQueueInsertAfter(&(proc->syncRepLinks), &(t_thrd.proc->syncRepLinks));
    else
        SHMQueueInsertAfter(&(t_thrd.walsender_cxt.WalSndCtl->SyncRepQueue[mode]), &(t_thrd.proc->syncRepLinks));
}

/*
 * Acquire SyncRepLock and cancel any wait currently not in completion queue.
 */
static bool SyncRepCancelWait(void)
{
    bool success = false;

    LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
    if (!t_thrd.proc->syncRepInCompleteQueue) {
        if (!SHMQueueIsDetached(&(t_thrd.proc->syncRepLinks))) {
            SHMQueueDelete(&(t_thrd.proc->syncRepLinks));
        }
        t_thrd.proc->syncRepState = SYNC_REP_NOT_WAITING;
        success = true;
    }
    LWLockRelease(SyncRepLock);

    return success;
}

void SyncRepCleanupAtProcExit(void)
{
    if (t_thrd.proc->syncRepLinks.prev || t_thrd.proc->syncRepLinks.next ||
        t_thrd.proc->syncRepState != SYNC_REP_NOT_WAITING) {
        LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
        if (!t_thrd.proc->syncRepInCompleteQueue) {
            if (!SHMQueueIsDetached(&(t_thrd.proc->syncRepLinks))) {
                SHMQueueDelete(&(t_thrd.proc->syncRepLinks));
            }
            LWLockRelease(SyncRepLock);
            return;
        }
        LWLockRelease(SyncRepLock);
        SyncRepWaitCompletionQueue();
    }
}

/*
 * ===========================================================
 * Synchronous Replication functions for wal sender processes
 * ===========================================================
 *
 *
 * Take any action required to initialise sync rep state from config
 * data. Called at WALSender startup and after each SIGHUP.
 */
void SyncRepInitConfig(void)
{
    int group;
    int priority;

    /*
     * Determine if we are a potential sync standby and remember the result
     * for handling replies from standby.
     */
    SyncRepGetStandbyGroupAndPriority(&group, &priority);
    if (t_thrd.walsender_cxt.MyWalSnd->sync_standby_group != group ||
        t_thrd.walsender_cxt.MyWalSnd->sync_standby_priority != priority) {
        LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

        t_thrd.walsender_cxt.MyWalSnd->sync_standby_group = group;
        t_thrd.walsender_cxt.MyWalSnd->sync_standby_priority = priority;

        /*
         * Synchronous standby is starting, so we should change the standalone
         * sync_master_standalone, if required.
         */
        SyncRepCheckSyncStandbyAlive();

        LWLockRelease(SyncRepLock);
        ereport(DEBUG1, (errmsg("standby \"%s\" now has synchronous standby group and priority: %d %d",
                                u_sess->attr.attr_common.application_name, group, priority)));
    }
}

/*
 * Update the LSNs on each queue based upon our latest state. This
 * implements a simple policy of first-valid-standby-releases-waiter.
 *
 * Other policies are possible, which would change what we do here and what
 * perhaps also which information we store as well.
 */
void SyncRepReleaseWaiters(void)
{
    volatile WalSndCtlData *walsndctl = t_thrd.walsender_cxt.WalSndCtl;
    XLogRecPtr receivePtr;
    XLogRecPtr writePtr;
    XLogRecPtr flushPtr;
    XLogRecPtr replayPtr;
    int numreceive = 0;
    int numwrite = 0;
    int numflush = 0;
    int numapply = 0;
    bool got_recptr = false;
    bool am_sync = false;

    /*
     * If this WALSender is serving a standby that is not on the list of
     * potential standbys then we have nothing to do. If we are still starting
     * up, still running base backup or the current flush position is still
     * invalid, then leave quickly also.
     */
    if (t_thrd.walsender_cxt.MyWalSnd->sync_standby_priority == 0 ||
        t_thrd.walsender_cxt.MyWalSnd->state < WALSNDSTATE_STREAMING ||
        XLByteEQ(t_thrd.walsender_cxt.MyWalSnd->flush, InvalidXLogRecPtr)) {
        t_thrd.syncrep_cxt.announce_next_takeover = true;
        return;
    }

    /*
     * We're a potential sync standby. Release waiters if we are the highest
     * priority standby. If there are multiple standbys with same priorities
     * then we use the first mentioned standby. If you change this, also
     * change pg_stat_get_wal_senders().
     */
    (void)LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

    /*
     * Check whether we are a sync standby or not, and calculate the synced
     * positions among all sync standbys.
     */
    got_recptr = SyncRepGetSyncRecPtr(&receivePtr, &writePtr, &flushPtr, &replayPtr, &am_sync);

    /*
     * If we are managing a sync standby, though we weren't prior to this,
     * then announce we are now a sync standby.
     */
    if (t_thrd.syncrep_cxt.announce_next_takeover && am_sync) {
        t_thrd.syncrep_cxt.announce_next_takeover = false;
        if (GetWalsndSyncRepConfig(t_thrd.walsender_cxt.MyWalSnd)->syncrep_method == SYNC_REP_PRIORITY) {
            ereport(LOG, (errmsg("standby \"%s\" is now a synchronous standby with group and priority: %d %d",
                                 u_sess->attr.attr_common.application_name,
                                 t_thrd.walsender_cxt.MyWalSnd->sync_standby_group,
                                 t_thrd.walsender_cxt.MyWalSnd->sync_standby_priority)));
        } else {
            ereport(LOG, (errmsg("standby \"%s\" is now a candidate for quorum synchronous standby in group: %d",
                                 u_sess->attr.attr_common.application_name,
                                 t_thrd.walsender_cxt.MyWalSnd->sync_standby_group)));
        }
    }

    /*
     * If the number of sync standbys is less than requested or we aren't
     * managing a sync standby then just leave.
     */
    if (!got_recptr || !am_sync) {
        LWLockRelease(SyncRepLock);
        t_thrd.syncrep_cxt.announce_next_takeover = !am_sync;
        return;
    }

    /*
     * Set the lsn first so that when we wake backends they will release up to
     * this location.
     */
    if (XLByteLT(walsndctl->lsn[SYNC_REP_WAIT_RECEIVE], receivePtr)) {
        walsndctl->lsn[SYNC_REP_WAIT_RECEIVE] = t_thrd.walsender_cxt.MyWalSnd->receive;
        numreceive = SyncRepWakeQueue(false, SYNC_REP_WAIT_RECEIVE);
    }
    if (XLByteLT(walsndctl->lsn[SYNC_REP_WAIT_WRITE], writePtr)) {
        walsndctl->lsn[SYNC_REP_WAIT_WRITE] = t_thrd.walsender_cxt.MyWalSnd->write;
        numwrite = SyncRepWakeQueue(false, SYNC_REP_WAIT_WRITE);
    }
    if (XLByteLT(walsndctl->lsn[SYNC_REP_WAIT_FLUSH], flushPtr)) {
        walsndctl->lsn[SYNC_REP_WAIT_FLUSH] = t_thrd.walsender_cxt.MyWalSnd->flush;
        numflush = SyncRepWakeQueue(false, SYNC_REP_WAIT_FLUSH);
    }
    if (XLByteLT(walsndctl->lsn[SYNC_REP_WAIT_APPLY], replayPtr)) {
        walsndctl->lsn[SYNC_REP_WAIT_APPLY] = t_thrd.walsender_cxt.MyWalSnd->apply;
        numapply = SyncRepWakeQueue(false, SYNC_REP_WAIT_APPLY);
    }

    LWLockRelease(SyncRepLock);

    ereport(DEBUG3,
            (errmsg("released %d procs up to receive %X/%X, %d procs up to write %X/%X, "
                    "%d procs up to flush %X/%X, %d procs up to apply %X/%X",
                    numreceive, (uint32)(receivePtr >> 32), (uint32)receivePtr, numwrite, (uint32)(writePtr >> 32),
                    (uint32)writePtr, numflush, (uint32)(flushPtr >> 32), (uint32)flushPtr,
                    numapply, (uint32)(replayPtr >> 32), (uint32)replayPtr)));
}

static SyncStandbyNumState check_sync_standbys_num(const List* sync_standbys)
{
    if (t_thrd.syncrep_cxt.SyncRepConfig == NULL)
        return STANDBIES_ENOUGH;

    ListCell* lc = NULL;
    List* per_group = NIL;
    int gid = 0;
    int alive = 0;
    SyncStandbyNumState res = STANDBIES_ENOUGH;

    foreach(lc, sync_standbys) {
        per_group = (List*)lfirst(lc);
        if (list_length(per_group) < t_thrd.syncrep_cxt.SyncRepConfig[gid]->num_sync)
            res = STANDBIES_NOT_ENOUGH;

        alive += list_length(per_group);
        gid++;
    }

    if (alive == 0) res = STANDBIES_EMPTY;
    return res;
}

/*
 * Check whether to delay the time of entering most_available_sync mode.
 *
 * Return false if keep_sync_window is not set, or most_available_sync
 * is not set, or current time is not in the time range since the time
 * when the number of sync standby is not qualified.
 * Otherwise it's set to true.
 */
static bool DelayIntoMostAvaSync(bool checkSyncNum, SyncStandbyNumState state)
{
    bool result = false;

    if (!t_thrd.walsender_cxt.WalSndCtl->most_available_sync || !u_sess->attr.attr_storage.keep_sync_window) {
        return result;
    }

    if (checkSyncNum) {
        List *sync_standbys = NIL;
        bool am_sync = false;
        sync_standbys = SyncRepGetSyncStandbys(&am_sync);
        state = check_sync_standbys_num(sync_standbys);
        free_sync_standbys_list(sync_standbys);
    }

    if (state == STANDBIES_ENOUGH) {
        if (t_thrd.walsender_cxt.WalSndCtl->keep_sync_window_start != 0) {
            t_thrd.walsender_cxt.WalSndCtl->keep_sync_window_start = 0;
            t_thrd.walsender_cxt.WalSndCtl->out_keep_sync_window = false;
        }
    } else if (t_thrd.walsender_cxt.WalSndCtl->keep_sync_window_start == 0) {
        t_thrd.walsender_cxt.WalSndCtl->keep_sync_window_start = GetCurrentTimestamp();
        result = true;
    } else if (!t_thrd.walsender_cxt.WalSndCtl->out_keep_sync_window) {
        TimestampTz now_time = 0;
        long diff_sec = 0;
        int diff_microsec = 0;
        now_time = GetCurrentTimestamp();
        TimestampDifference(t_thrd.walsender_cxt.WalSndCtl->keep_sync_window_start, now_time, &diff_sec,
                            &diff_microsec);
        if (diff_sec < (long)u_sess->attr.attr_storage.keep_sync_window) {
            result = true;
            ereport(DEBUG1, (errmsg("Delay to entering most_available_sync mode, %ld seconds passed.", diff_sec)));
        } else {
            t_thrd.walsender_cxt.WalSndCtl->out_keep_sync_window = true;
        }
    }

    return result;
}

/*
 * In a particular scenario, when most_available_sync is true, primary only wait 
 * the alive sync standbys, even if the quantity does not meet the configuration 
 * requirements.
 */
static bool judge_sync_standbys_num(const List* sync_standbys, SyncStandbyNumState* state)
{
    *state = check_sync_standbys_num(sync_standbys);

    if (*state == STANDBIES_ENOUGH) {
        DelayIntoMostAvaSync(false, STANDBIES_ENOUGH);  // just for refresh keep_sync_window if needed
        return true;
    }
    if (t_thrd.walsender_cxt.WalSndCtl->most_available_sync && !DelayIntoMostAvaSync(false, *state)) {
        return true;
    }
    return false;
}

/*
 * Calculate the synced Receive, Write, Flush and Apply positions among sync standbys.
 *
 * Return false if the number of sync standbys is less than
 * synchronous_standby_names specifies. Otherwise return true and
 * store the positions into *receivePtr *writePtr, *flushPtr and *applyPtr.
 *
 * On return, *am_sync is set to true if this walsender is connecting to
 * sync standby. Otherwise it's set to false.
 */
bool SyncRepGetSyncRecPtr(XLogRecPtr *receivePtr, XLogRecPtr *writePtr, XLogRecPtr *flushPtr, XLogRecPtr* replayPtr, bool *am_sync, bool check_am_sync)
{
    List *sync_standbys = NIL;

    *receivePtr = InvalidXLogRecPtr;
    *writePtr = InvalidXLogRecPtr;
    *flushPtr = InvalidXLogRecPtr;
    *replayPtr = InvalidXLogRecPtr;
    *am_sync = false;

    /* Get standbys that are considered as synchronous at this moment */
    sync_standbys = SyncRepGetSyncStandbys(am_sync);
    /*
     * Quick exit if we are not managing a sync standby (or not check for check_am_sync is false)
     * or there are not enough synchronous standbys.
     * or most_available_sync is working and allow some standby nodes to be missing.
     */
    SyncStandbyNumState state;
    if ((!(*am_sync) && check_am_sync) ||
        t_thrd.syncrep_cxt.SyncRepConfig == NULL ||
        !judge_sync_standbys_num(sync_standbys, &state)) {
        free_sync_standbys_list(sync_standbys);
        return false;
    }

    /*
     * In a priority-based sync replication, the synced positions are the
     * oldest ones among sync standbys. In a quorum-based, they are the Nth
     * latest ones.
     *
     * SyncRepGetNthLatestSyncRecPtr() also can calculate the oldest positions.
     * But we use SyncRepGetOldestSyncRecPtr() for that calculation because
     * it's a bit more efficient.
     *
     * XXX If the numbers of current and requested sync standbys are the same,
     * we can use SyncRepGetOldestSyncRecPtr() to calculate the synced
     * positions even in a quorum-based sync replication.
     */
    if (state == STANDBIES_EMPTY) {
        /* deal with sync standbys list is empty when most available sync mode is on */
        *writePtr = GetXLogWriteRecPtr();
        *flushPtr = GetFlushRecPtr();
        *receivePtr = *writePtr;
        *replayPtr = *flushPtr;
    } else {
        int i = 0;
        ListCell *lc = NULL;
        foreach(lc, sync_standbys) {
            List *per_group = (List*)lfirst(lc);
            if (per_group == NIL) {
                /* do nothing, this group of sync standbys are all offline or no sync standbys. */
            } else if (t_thrd.syncrep_cxt.SyncRepConfig[i]->syncrep_method == SYNC_REP_PRIORITY) {
                SyncRepGetOldestSyncRecPtr(receivePtr, writePtr, flushPtr, replayPtr, per_group);
            } else {
                SyncRepGetNthLatestSyncRecPtr(receivePtr, writePtr, flushPtr, replayPtr,
                    per_group, t_thrd.syncrep_cxt.SyncRepConfig[i]->num_sync);
            }
            i++;
        }
    }
    free_sync_standbys_list(sync_standbys);
    return true;
}

#ifndef ENABLE_MULTIPLE_NODES
/*
 * Obtains the remaining time for synchronizing to the sync standby.
 *
 * If there is at lease one sync standby, or no standby in catchup, no need
 * to consider standby in catchup, return false. Otherwise return true.
 */
static bool SyncRepGetSyncLeftTime(XLogRecPtr XactCommitLSN, TimestampTz* leftTime)
{
    List* sync_standbys = NIL;
    List* catchup_standbys = NIL;
    bool am_sync = false;
    ListCell* cell = NULL;
    *leftTime = 0;

    /* Get standbys that are considered as synchronous at this moment. */
    sync_standbys = SyncRepGetSyncStandbys(&am_sync, &catchup_standbys);
    /* Skip here if there is at lease one sync standby, or no standby in catchup. */
    if (check_sync_standbys_num(sync_standbys) != STANDBIES_EMPTY || list_length(catchup_standbys) == 0) {
        free_sync_standbys_list(sync_standbys);
        list_free(catchup_standbys);
        return false;
    }

    /*
     * Scan through all sync standbys and calculate the left time
     * for sync.
     */
    foreach (cell, catchup_standbys) {
        WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[lfirst_int(cell)];
        TimestampTz syncNeededTime;
        SpinLockAcquire(&walsnd->mutex);
        XLogRecPtr xrp = walsnd->receive;
        double rate = walsnd->catchupRate;
        SpinLockRelease(&walsnd->mutex);

        syncNeededTime = (TimestampTz)(rate * XLByteDifference(XactCommitLSN, xrp));
        if (*leftTime == 0 || *leftTime > syncNeededTime) {
            *leftTime = syncNeededTime;
        }
    }

    free_sync_standbys_list(sync_standbys);
    list_free(catchup_standbys);
    return true;
}
#endif

/*
 * Calculate the oldest Write, Flush and Apply positions among sync standbys.
 */
static void SyncRepGetOldestSyncRecPtr(XLogRecPtr* receivePtr, XLogRecPtr* writePtr, XLogRecPtr* flushPtr,
                                       XLogRecPtr* replayPtr, List* sync_standbys)
{
    ListCell *cell = NULL;

    /*
     * Scan through all sync standbys and calculate the oldest
     * Write, Flush and Apply positions.
     */
    foreach (cell, sync_standbys) {
        WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[lfirst_int(cell)];
        XLogRecPtr receive;
        XLogRecPtr write;
        XLogRecPtr flush;
        XLogRecPtr apply;

        SpinLockAcquire(&walsnd->mutex);
        receive = walsnd->receive;
        write = walsnd->write;
        flush = walsnd->flush;
        apply = walsnd->apply;
        SpinLockRelease(&walsnd->mutex);

        if (XLogRecPtrIsInvalid(*writePtr) || !XLByteLE(*writePtr, write))
            *writePtr = write;
        if (XLogRecPtrIsInvalid(*flushPtr) || !XLByteLE(*flushPtr, flush))
            *flushPtr = flush;
        if (XLogRecPtrIsInvalid(*receivePtr) || !XLByteLE(*receivePtr, receive))
            *receivePtr = receive;
        if (XLogRecPtrIsInvalid(*replayPtr) || !XLByteLE(*replayPtr, apply))
            *replayPtr = apply;
    }
}

/*
 * Calculate the Nth latest Write, Flush and Apply positions among sync
 * standbys.
 */
static void SyncRepGetNthLatestSyncRecPtr(XLogRecPtr* receivePtr, XLogRecPtr* writePtr, XLogRecPtr* flushPtr,
                                          XLogRecPtr* replayPtr, List* sync_standbys, uint8 nth)
{
    ListCell *cell = NULL;
    XLogRecPtr *receive_array = NULL;
    XLogRecPtr *write_array = NULL;
    XLogRecPtr *flush_array = NULL;
    XLogRecPtr* apply_array = NULL;
    int len;
    int i = 0;

    len = list_length(sync_standbys);
    receive_array = (XLogRecPtr*)palloc(sizeof(XLogRecPtr) * len);
    write_array = (XLogRecPtr*)palloc(sizeof(XLogRecPtr) * len);
    flush_array = (XLogRecPtr*)palloc(sizeof(XLogRecPtr) * len);
    apply_array = (XLogRecPtr*)palloc(sizeof(XLogRecPtr) * len);

    foreach (cell, sync_standbys) {
        WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[lfirst_int(cell)];

        if (walsnd->is_cross_cluster) {
            continue;
        }
        SpinLockAcquire(&walsnd->mutex);
        receive_array[i] = walsnd->receive;
        write_array[i] = walsnd->write;
        flush_array[i] = walsnd->flush;
        apply_array[i] = walsnd->apply;
        SpinLockRelease(&walsnd->mutex);

        i++;
    }

    qsort(receive_array, len, sizeof(XLogRecPtr), cmp_lsn);
    qsort(write_array, len, sizeof(XLogRecPtr), cmp_lsn);
    qsort(flush_array, len, sizeof(XLogRecPtr), cmp_lsn);
    qsort(apply_array, len, sizeof(XLogRecPtr), cmp_lsn);

    /*
    * rewrite nth if current sync standby num < nth, when most_available_sync is true,
    * primary only wait the alive sync standbys if list_length(sync_standbys) doesn't satisfy num_sync in quroum.
    */
    if (t_thrd.walsender_cxt.WalSndCtl->most_available_sync && list_length(sync_standbys) < nth) {
        nth = (uint8)list_length(sync_standbys);
    }

    /* Get Nth latest Write, Flush, Apply positions */
    if (XLogRecPtrIsInvalid(*writePtr) || XLByteLE(write_array[nth - 1], *writePtr))
        *writePtr = write_array[nth - 1];
    if (XLogRecPtrIsInvalid(*flushPtr) || XLByteLE(flush_array[nth - 1], *flushPtr))
        *flushPtr = flush_array[nth - 1];
    if (XLogRecPtrIsInvalid(*receivePtr) || XLByteLE(receive_array[nth - 1], *receivePtr))
        *receivePtr = receive_array[nth - 1];
    if (XLogRecPtrIsInvalid(*replayPtr) || XLByteLE(apply_array[nth - 1], *replayPtr))
        *replayPtr = apply_array[nth - 1];

    pfree(receive_array);
    receive_array = NULL;
    pfree(write_array);
    write_array = NULL;
    pfree(flush_array);
    flush_array = NULL;
    pfree(apply_array);
    apply_array = NULL;
}

/*
 * Compare lsn in order to sort array in descending order.
 */
static int cmp_lsn(const void *a, const void *b)
{
    XLogRecPtr lsn1 = *((const XLogRecPtr *)a);
    XLogRecPtr lsn2 = *((const XLogRecPtr *)b);

    if (!XLByteLE(lsn1, lsn2))
        return -1;
    else if (XLByteEQ(lsn1, lsn2))
        return 0;
    else
        return 1;
}

/*
 * Check if we are in the list of sync standbys, and if so, determine
 * priority sequence. Return groupid and priority if set, or zero to indicate that
 * we are not a potential sync standby.
 *
 * Compare the parameter SyncRepStandbyNames against the application_name
 * for this WALSender, or allow any name if we find a wildcard "*".
 */
static void SyncRepGetStandbyGroupAndPriority(int* gid, int* prio)
{
    const char *standby_name = NULL;
    int group;
    int priority;
    bool found = false;

    *gid = 0;
    *prio = 0;

    /*
     * Since synchronous cascade replication is not allowed, we always set the
     * priority of cascading walsender to zero.
     */
    if (AM_WAL_STANDBY_SENDER || AM_WAL_SHARE_STORE_SENDER || AM_WAL_HADR_DNCN_SENDER || AM_WAL_DB_SENDER)
        return;

    if (!SyncStandbysDefined() || t_thrd.syncrep_cxt.SyncRepConfig == NULL || !SyncRepRequested())
        return;

    for (group = 0; group < t_thrd.syncrep_cxt.SyncRepConfigGroups && !found; group++) {
        standby_name = t_thrd.syncrep_cxt.SyncRepConfig[group]->member_names;
        for (priority = 1; priority <= t_thrd.syncrep_cxt.SyncRepConfig[group]->nmembers; priority++) {
            if (pg_strcasecmp(standby_name, u_sess->attr.attr_common.application_name) == 0 ||
                strcmp(standby_name, "*") == 0) {
                Assert(!(group > 1 && strcmp(standby_name, "*") == 0));
                found = true;
                break;
            }
            standby_name += strlen(standby_name) + 1;
        }
    }

    if (!found) {
        return;
    }

    /*
     * In quorum-based sync replication, all the standbys in the list
     * have the same priority, one.
     */
    *gid = group - 1;
    *prio = (t_thrd.syncrep_cxt.SyncRepConfig[group - 1]->syncrep_method == SYNC_REP_PRIORITY) ? priority : 1;
    return;
}

/*
 * Wake the specified queue from head.	Set the state of any backends that
 * need to be woken, remove them from the queue, and then wake them.
 * Pass all = true to wake whole queue; otherwise, just wake up to
 * the walsender's LSN.
 *
 * Must hold SyncRepLock.
 */
int SyncRepWakeQueue(bool all, int mode)
{
    volatile WalSndCtlData *walsndctl = t_thrd.walsender_cxt.WalSndCtl;
    PGPROC *proc = NULL;
    PGPROC *thisproc = NULL;
    int numprocs = 0;

    Assert(mode >= 0 && mode < NUM_SYNC_REP_WAIT_MODE);
    Assert(SyncRepQueueIsOrderedByLSN(mode));

    proc = (PGPROC *)SHMQueueNext(&(t_thrd.walsender_cxt.WalSndCtl->SyncRepQueue[mode]),
                                  &(t_thrd.walsender_cxt.WalSndCtl->SyncRepQueue[mode]),
                                  offsetof(PGPROC, syncRepLinks));
    SHM_QUEUE* pHead = &(t_thrd.walsender_cxt.WalSndCtl->SyncRepQueue[mode]);
    SHM_QUEUE* pTail = pHead;

    while (proc != NULL) {
        /*
         * Assume the queue is ordered by LSN
         */
        if (!all && XLByteLT(walsndctl->lsn[mode], proc->waitLSN))
            break;

        /*
         * Move to next proc, so we can delete thisproc from the queue.
         * thisproc is valid, proc may be NULL after this.
         */
        thisproc = proc;
        thisproc->syncRepInCompleteQueue = true;
        proc = (PGPROC *)SHMQueueNext(&(t_thrd.walsender_cxt.WalSndCtl->SyncRepQueue[mode]), &(proc->syncRepLinks),
                                      offsetof(PGPROC, syncRepLinks));

        /* Refers to the last removable node */
        pTail = &(thisproc->syncRepLinks);
        numprocs++;
    }

    /* Delete the finished segment from the list, and only notifies leader proc */
    if (pTail != pHead) {
        PGPROC* leaderProc = (PGPROC *) (((char *) pHead->next) - offsetof(PGPROC, syncRepLinks));
        pHead->next->prev = NULL;
        pHead->next = pTail->next;
        pTail->next->prev = pHead;
        pTail->next = NULL;

        /*
         * SyncRepWaitForLSN() reads syncRepState without holding the lock, so
         * make sure that it sees the queue link being removed before the
         * syncRepState change.
         */
        pg_write_barrier();

        /*
         * Set state to complete; see SyncRepWaitForLSN() for discussion of
         * the various states.
         */
        leaderProc->syncRepState = SYNC_REP_WAIT_COMPLETE;

        /*
         * Wake only when we have set state and removed from queue.
         */
        SetLatch(&(leaderProc->procLatch));
    }

    return numprocs;
}

/*
 * Wait for notification from completion queue. It should be finished
 * soon, and is irrelevant to the network. So we just wait to avoid using lock.
 */
static void SyncRepWaitCompletionQueue()
{
    /* Waiting for complete */
    int i = MAX_SYNC_REP_RETRY_COUNT;
    while (t_thrd.proc->syncRepState == SYNC_REP_WAITING) {
        if (i-- > 0) {
            pg_usleep(SYNC_REP_SLEEP_DELAY);
        } else {
            ereport(WARNING, (errmsg("Waiting for syncrep completion queue timeout.")));
            i = MAX_SYNC_REP_RETRY_COUNT;
        }
    }

    /* Make sure that syncRepLinks is read after syncRepState */
    pg_read_barrier();

    /* Leader informs following procs */
    if (t_thrd.proc->syncRepState == SYNC_REP_WAIT_COMPLETE && t_thrd.proc->syncRepLinks.next) {
        SyncRepNotifyComplete();
    }
}

/*
 * Leader informs following procs
 */
static void SyncRepNotifyComplete()
{
    SHM_QUEUE *nextElement = t_thrd.proc->syncRepLinks.next;
    if (nextElement != NULL) {
        t_thrd.proc->syncRepLinks.next = NULL;
        while (nextElement != NULL) {
            PGPROC* curProc = (PGPROC*)(((char*)nextElement) - offsetof(PGPROC, syncRepLinks));
            /*
             * Move to next proc, so we can delete thisproc from the queue.
             * curProc is valid, proc may be NULL after this.
             */
            nextElement = curProc->syncRepLinks.next;

            /*
             * Remove curProc from queue.
             */
            curProc->syncRepLinks.next = NULL;
            curProc->syncRepLinks.prev = NULL;

            /*
             * SyncRepWaitForLSN() reads syncRepState without holding the lock, so
             * make sure that it sees the queue link being removed before the
             * syncRepState change.
             */
            pg_write_barrier();

            curProc->syncRepState = SYNC_REP_WAIT_COMPLETE;

            /*
             * Wake only when we have set state and removed from queue.
             */
            SetLatch(&(curProc->procLatch));
        }
    }
}

/*
 * The checkpointer calls this as needed to update the shared
 * sync_standbys_defined flag, so that backends don't remain permanently wedged
 * if synchronous_standby_names is unset.  It's safe to check the current value
 * without the lock, because it's only ever updated by one process.  But we
 * must take the lock to change it.
 */
void SyncRepUpdateSyncStandbysDefined(void)
{
    bool sync_standbys_defined = SyncStandbysDefined();
    if (sync_standbys_defined != t_thrd.walsender_cxt.WalSndCtl->sync_standbys_defined) {
        (void)LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

        /*
         * If synchronous_standby_names has been reset to empty, it's futile
         * for backends to continue to waiting.  Since the user no longer
         * wants synchronous replication, we'd better wake them up.
         */
        if (!sync_standbys_defined) {
            int i;

            for (i = 0; i < NUM_SYNC_REP_WAIT_MODE; i++)
                (void)SyncRepWakeQueue(true, i);
        }

        /*
         * Only allow people to join the queue when there are synchronous
         * standbys defined.  Without this interlock, there's a race
         * condition: we might wake up all the current waiters; then, some
         * backend that hasn't yet reloaded its config might go to sleep on
         * the queue (and never wake up).  This prevents that.
         */
        t_thrd.walsender_cxt.WalSndCtl->sync_standbys_defined = sync_standbys_defined;

        if (sync_standbys_defined && t_thrd.walsender_cxt.WalSndCtl->most_available_sync &&
            !t_thrd.walsender_cxt.WalSndCtl->sync_master_standalone)
            SyncRepCheckSyncStandbyAlive();

        LWLockRelease(SyncRepLock);
    }

    /*
     * Check if new value of parameter is same as earlier,
     * if not then change in shared memory. Since here were enabling this
     * parameter now only, so it may happen that there were no synchronous
     * standby but master has not gone in stand-alone mode because it was
     * not configured to do so.
     */
    if (most_available_sync != t_thrd.walsender_cxt.WalSndCtl->most_available_sync) {
        LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

        t_thrd.walsender_cxt.WalSndCtl->most_available_sync = most_available_sync;
        (void)SyncRepCheckSyncStandbyAlive();

        LWLockRelease(SyncRepLock);
    }
}

/*
 * Return the list of sync standbys, or NIL if no sync standby is connected.
 *
 * If there are multiple standbys with the same priority,
 * the first one found is selected preferentially.
 * The caller must hold SyncRepLock.
 *
 * On return, *am_sync is set to true if this walsender is connecting to
 * sync standby. Otherwise it's set to false.
 */
List *SyncRepGetSyncStandbys(bool *am_sync, List** catchup_standbys)
{
    /* Set default result */
    if (am_sync != NULL)
        *am_sync = false;

    /* Quick exit if sync replication is not requested */
    if (t_thrd.syncrep_cxt.SyncRepConfig == NULL)
        return NIL;

    List* results = NIL;
    for(int i = 0; i < t_thrd.syncrep_cxt.SyncRepConfigGroups; i++) {
        if (t_thrd.syncrep_cxt.SyncRepConfig[i]->syncrep_method == SYNC_REP_PRIORITY) {
            results = lappend(results, SyncRepGetSyncStandbysPriority(am_sync, i,catchup_standbys));
        } else {
            results = lappend(results, SyncRepGetSyncStandbysQuorum(am_sync, i, catchup_standbys));
        }
    }

    return results;
}

/*
 * Return the list of all the candidates for quorum sync standbys,
 * or NIL if no such standby is connected.
 *
 * The caller must hold SyncRepLock. This function must be called only in
 * a quorum-based sync replication.
 *
 * On return, *am_sync is set to true if this walsender is connecting to
 * sync standby. Otherwise it's set to false.
 */
static List *SyncRepGetSyncStandbysQuorum(bool *am_sync, int groupid, List** catchup_standbys)
{
    List *result = NIL;
    int i;
    volatile WalSnd *walsnd = NULL; /* Use volatile pointer to prevent code rearrangement */

    Assert(t_thrd.syncrep_cxt.SyncRepConfig[groupid]->syncrep_method == SYNC_REP_QUORUM);

    for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];

        /* Must be active */
        if (walsnd->pid == 0)
            continue;

        /* Must be synchronous */
        if (walsnd->sync_standby_priority == 0 || walsnd->sync_standby_group != groupid)
            continue;

        if ((walsnd->state == WALSNDSTATE_CATCHUP || walsnd->peer_state == CATCHUP_STATE) &&
            catchup_standbys != NULL) {
            *catchup_standbys = lappend_int(*catchup_standbys, i);
        }

        /* Must have a valid flush position */
        if (XLogRecPtrIsInvalid(walsnd->flush))
            continue;

        /* Must be streaming */
        if (walsnd->state != WALSNDSTATE_STREAMING)
            continue;

        if (walsnd->peer_role == STANDBY_CLUSTER_MODE) {
            continue;
        }

        /*
         * Consider this standby as a candidate for quorum sync standbys
         * and append it to the result.
         */
        result = lappend_int(result, i);
        if (am_sync != NULL && walsnd == t_thrd.walsender_cxt.MyWalSnd)
            *am_sync = true;
    }

    return result;
}

/*
 * Return the list of sync standbys chosen based on their priorities,
 * or NIL if no sync standby is connected.
 *
 * If there are multiple standbys with the same priority,
 * the first one found is selected preferentially.
 *
 * The caller must hold SyncRepLock. This function must be called only in
 * a priority-based sync replication.
 *
 * On return, *am_sync is set to true if this walsender is connecting to
 * sync standby. Otherwise it's set to false.
 */
static List *SyncRepGetSyncStandbysPriority(bool *am_sync, int groupid, List** catchup_standbys)
{
    List *result = NIL;
    List *pending = NIL;
    int lowest_priority;
    int next_highest_priority;
    int this_priority;
    int priority;
    int i;
    bool am_in_pending = false;
    volatile WalSnd *walsnd = NULL; /* Use volatile pointer to prevent code rearrangement */

    Assert(t_thrd.syncrep_cxt.SyncRepConfig[groupid]->syncrep_method == SYNC_REP_PRIORITY);

    lowest_priority = t_thrd.syncrep_cxt.SyncRepConfig[groupid]->nmembers;
    next_highest_priority = lowest_priority + 1;

    /*
     * Find the sync standbys which have the highest priority (i.e, 1). Also
     * store all the other potential sync standbys into the pending list, in
     * order to scan it later and find other sync standbys from it quickly.
     */
    for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];

        /* Must be active */
        if (walsnd->pid == 0)
            continue;

        /* Must be synchronous */
        this_priority = walsnd->sync_standby_priority;
        if (this_priority == 0 || walsnd->sync_standby_group != groupid)
            continue;

        if ((walsnd->state == WALSNDSTATE_CATCHUP || walsnd->peer_state == CATCHUP_STATE) && 
            catchup_standbys != NULL) {
            *catchup_standbys = lappend_int(*catchup_standbys, i);
        }

        /* Must have a valid flush position */
        if (XLogRecPtrIsInvalid(walsnd->flush))
            continue;

        /* Must be streaming */
        if (walsnd->state != WALSNDSTATE_STREAMING)
            continue;

        /*
         * If the priority is equal to 1, consider this standby as sync and
         * append it to the result. Otherwise append this standby to the
         * pending list to check if it's actually sync or not later.
         */
        if (this_priority == 1) {
            result = lappend_int(result, i);
            if (am_sync != NULL && walsnd == t_thrd.walsender_cxt.MyWalSnd)
                *am_sync = true;
            if (list_length(result) == t_thrd.syncrep_cxt.SyncRepConfig[groupid]->num_sync) {
                list_free(pending);
                return result; /* Exit if got enough sync standbys */
            }
        } else {
            pending = lappend_int(pending, i);
            if (am_sync != NULL && walsnd == t_thrd.walsender_cxt.MyWalSnd)
                am_in_pending = true;

            /*
             * Track the highest priority among the standbys in the pending
             * list, in order to use it as the starting priority for later
             * scan of the list. This is useful to find quickly the sync
             * standbys from the pending list later because we can skip
             * unnecessary scans for the unused priorities.
             */
            if (this_priority < next_highest_priority)
                next_highest_priority = this_priority;
        }
    }

    /*
     * Consider all pending standbys as sync if the number of them plus
     * already-found sync ones is lower than the configuration requests.
     */
    if (list_length(result) + list_length(pending) <= t_thrd.syncrep_cxt.SyncRepConfig[groupid]->num_sync) {
        bool needfree = (result != NIL && pending != NIL);

        /*
         * Set *am_sync to true if this walsender is in the pending list
         * because all pending standbys are considered as sync.
         */
        if (am_sync != NULL && !(*am_sync))
            *am_sync = am_in_pending;

        result = list_concat(result, pending);
        if (needfree) {
            pfree(pending);
            pending = NULL;
        }
        return result;
    }

    /*
     * Find the sync standbys from the pending list.
     */
    priority = next_highest_priority;
    while (priority <= lowest_priority) {
        ListCell *cell = NULL;
        ListCell *prev = NULL;
        ListCell *next = NULL;

        next_highest_priority = lowest_priority + 1;

        for (cell = list_head(pending); cell != NULL; cell = next) {
            i = lfirst_int(cell);
            walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];

            next = lnext(cell);

            this_priority = walsnd->sync_standby_priority;
            if (this_priority == priority) {
                result = lappend_int(result, i);
                if (am_sync != NULL && walsnd == t_thrd.walsender_cxt.MyWalSnd)
                    *am_sync = true;

                /*
                 * We should always exit here after the scan of pending list
                 * starts because we know that the list has enough elements to
                 * reach SyncRepConfig[groupid]->num_sync.
                 */
                if (list_length(result) == t_thrd.syncrep_cxt.SyncRepConfig[groupid]->num_sync) {
                    list_free(pending);
                    return result; /* Exit if got enough sync standbys */
                }

                /*
                 * Remove the entry for this sync standby from the list to
                 * prevent us from looking at the same entry again.
                 */
                pending = list_delete_cell(pending, cell, prev);

                continue;
            }

            if (this_priority < next_highest_priority)
                next_highest_priority = this_priority;

            prev = cell;
        }

        priority = next_highest_priority;
    }

    /* never reached, but keep compiler quiet */
    Assert(false);
    return result;
}

/*
 * check to see whether synchronous standby is alive.
 * Loop through all sender task and check if there is any
 * synchronous standby is alive. If alive then master needs
 * to continue to wait for synchronous standby otherwise,
 * it does not have to and it can switch to standalone mode.
 * Whenever mode is changing from one to another then
 * log the appropriate log message, which will be used by DBA.
 */
void SyncRepCheckSyncStandbyAlive(void)
{
    bool sync_standby_alive = false;
    int i = 0;

    if (!t_thrd.walsender_cxt.WalSndCtl->sync_standbys_defined ||
        !t_thrd.walsender_cxt.WalSndCtl->most_available_sync) {
        t_thrd.walsender_cxt.WalSndCtl->sync_master_standalone = false;
        return;
    }

    for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        volatile WalSnd *walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];

        SpinLockAcquire(&walsnd->mutex);

        /*
         * Check if this synchronous standby and its pid is not zero i.e. synchronous
         * standby is alive.
         */
        if (walsnd->pid != 0 && walsnd->sync_standby_priority > 0 &&
            (walsnd->sendRole == SNDROLE_PRIMARY_DUMMYSTANDBY || walsnd->sendRole == SNDROLE_PRIMARY_STANDBY)) {
            SpinLockRelease(&walsnd->mutex);
            sync_standby_alive = true;
            break;
        }

        SpinLockRelease(&walsnd->mutex);
    }

    if (sync_standby_alive && t_thrd.walsender_cxt.WalSndCtl->sync_master_standalone) {
        ereport(LOG, (errmsg("standalone synchronous master now have synchronous standby")));

        t_thrd.walsender_cxt.WalSndCtl->sync_master_standalone = false;
        return;
    }

    if (!sync_standby_alive && !t_thrd.walsender_cxt.WalSndCtl->sync_master_standalone &&
        t_thrd.walsender_cxt.WalSndCtl->most_available_sync) {
        ereport(LOG, (errmsg("synchronous master is now standalone")));

        t_thrd.walsender_cxt.WalSndCtl->sync_master_standalone = true;

        /*
         * If there is any waiting sender, then wake-up them as
         * master has switched to standalone mode
         */
        for (i = 0; i < NUM_SYNC_REP_WAIT_MODE; i++)
            (void)SyncRepWakeQueue(true, i);
    }
}

/*
 * Wait for paxos, if requested by user.
 *
 * Initially backends start in state SYNC_REP_NOT_WAITING and then
 * change that state to SYNC_REP_WAITING before adding ourselves
 * to the wait queue. During SyncRepWakeQueue() ConsensusLogCallback changes
 * the state to SYNC_REP_WAIT_COMPLETE once paxos is consensus.
 * This backend then resets its state to SYNC_REP_NOT_WAITING.
 * 
 * Returns true if it is really awaken by paxos callback, or false if it
 * is terminated by proc die or demotion.
 */
bool SyncPaxosWaitForLSN(XLogRecPtr PaxosConsensusLSN)
{
    bool syncSuccessed = true;
    char* new_status = NULL;
    const char* old_status = NULL;

    Assert(SHMQueueIsDetached(&(t_thrd.proc->syncPaxosLinks)));
    Assert(t_thrd.walsender_cxt.WalSndCtl != NULL);

    LWLockAcquire(SyncPaxosLock, LW_EXCLUSIVE);
    Assert(t_thrd.proc->syncPaxosState == SYNC_REP_NOT_WAITING);

    if (XLByteLE(PaxosConsensusLSN, t_thrd.walsender_cxt.WalSndCtl->paxosLsn)) {
        LWLockRelease(SyncPaxosLock);
        return true;
    }

    /*
     * Set our waitPaxosLSN so ConsensusLogCb will know when to wake us, and add
     * ourselves to the queue.
     */
    t_thrd.proc->waitPaxosLSN = PaxosConsensusLSN;
    t_thrd.proc->syncPaxosState = SYNC_REP_WAITING;
    SyncPaxosQueueInsert();
    Assert(SyncPaxosQueueIsOrderedByLSN());
    LWLockRelease(SyncPaxosLock);

    /* Alter ps display to show waiting for sync rep. */
    if (u_sess->attr.attr_common.update_process_title) {
        int len;
        errno_t ret = EOK;
        int rc = 0;
#define NEW_STATUS_LEN 33
        old_status = get_ps_display(&len);
        new_status = (char *) palloc(len + NEW_STATUS_LEN);
        ret = memcpy_s(new_status, len + NEW_STATUS_LEN, old_status, len);
        securec_check(ret, "\0", "\0");

        rc = snprintf_s(new_status + len, NEW_STATUS_LEN, NEW_STATUS_LEN - 1, " waiting for %X/%X",
                        (uint32)(PaxosConsensusLSN >> 32), (uint32)PaxosConsensusLSN);
        securec_check_ss(rc, "", "");

        set_ps_display(new_status, false);
        new_status[len] = '\0'; /* truncate off " waiting ..." */
    }

    WaitState oldStatus = pgstat_report_waitstatus(STATE_WAIT_WALSYNC);

    /*
     * Wait for specified LSN to be confirmed.
     *
     * Each proc has its own wait latch, so we perform a normal latch
     * check/wait loop here.
     */
    for (;;) {
        /* Must reset the latch before testing state. */
        ResetLatch(&t_thrd.proc->procLatch);

        if (t_thrd.proc->syncPaxosState == SYNC_REP_WAIT_COMPLETE) 
            break;

        /*
         * If a wait for synchronous replication is pending, we can neither
         * acknowledge the commit nor raise ERROR or FATAL.  The latter would
         * lead the client to believe that the transaction aborted, which
         * is not true: it's already committed locally. The former is no good
         * either: the client has requested synchronous replication, and is
         * entitled to assume that an acknowledged commit is also replicated,
         * which might not be true. So in this case we issue a WARNING (which
         * some clients may be able to interpret) and shut off further output.
         * We do NOT reset ProcDiePending, so that the process will die after
         * the commit is cleaned up.
         */
        if (t_thrd.int_cxt.ProcDiePending || t_thrd.proc_cxt.proc_exit_inprogress) {
            ereport(WARNING,
                    (errcode(ERRCODE_ADMIN_SHUTDOWN),
                     errmsg("canceling the wait for paxos consensus and terminating connection due to administrator command"),
                     errdetail("The transaction will not be committed locally, because it is not consensus by paxos yet.")));
            t_thrd.postgres_cxt.whereToSendOutput = DestNone;
            SyncPaxosCancelWait();
            syncSuccessed = false;
            break;
        }

        /*
         * It's unclear what to do if a query cancel interrupt arrives.  We
         * can't actually abort at this point, but ignoring the interrupt
         * altogether is not helpful, so we just terminate the wait with a
         * suitable warning.
         */
        if (t_thrd.int_cxt.QueryCancelPending) {
            /* reset query cancel signal after vacuum. */
            if (!t_thrd.vacuum_cxt.in_vacuum) {
                t_thrd.int_cxt.QueryCancelPending = false;
            }
            ereport(WARNING,
                    (errmsg("canceling wait for paxos consensus due to user request"),
                     errdetail("The transaction will not be committed locally, because it is not consensus by paxos yet.")));
            SyncPaxosCancelWait();
            syncSuccessed = false;
            break;
        }

        /*
         * If the postmaster dies, we'll probably never get an
         * acknowledgement, because all the wal sender processes will exit. So
         * just bail out.
         */
        if (!PostmasterIsAlive()) {
            t_thrd.int_cxt.ProcDiePending = true;
            t_thrd.postgres_cxt.whereToSendOutput = DestNone;
            SyncPaxosCancelWait();
            syncSuccessed = false;
            break;
        }

        /*
         * For case that query cancel pending or proc die pending signal not reached, if current
         * session is set closed, we'll stop wait
         */
        if (u_sess->status == KNL_SESS_CLOSE) {
            ereport(WARNING,
                    (errmsg("canceling wait for paxos consensus due to session close."),
                     errdetail("The transaction has already committed locally, but might not have been consensus by paxos yet.")));
            SyncPaxosCancelWait();
            syncSuccessed = false;
            break;
        }

        /*
         * Wait on latch.  Any condition that should wake us up will set the
         * latch, so no need for timeout.
         */
        WaitLatch(&t_thrd.proc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 3000L);
    }

    pgstat_report_waitstatus(oldStatus);

    /*
     * ConsensusLogCb has checked our LSN and has removed us from queue. Clean up
     * state and leave.  It's OK to reset these shared memory fields without
     * holding SyncPaxosLock, because any walsenders will ignore us anyway when
     * we're not on the queue.
     */
    pg_read_barrier();
    Assert(SHMQueueIsDetached(&(t_thrd.proc->syncPaxosLinks)));
    t_thrd.proc->syncPaxosState = SYNC_REP_NOT_WAITING;
    t_thrd.proc->waitPaxosLSN = 0;

    if (new_status != NULL) {
        /* Reset ps display */
        set_ps_display(new_status, false);
        pfree(new_status);
        new_status = NULL;
    }
    return syncSuccessed;
}

/*
 * Acquire SyncPaxosLock and cancel any wait currently in progress.
 */
static void SyncPaxosCancelWait(void)
{
    LWLockAcquire(SyncPaxosLock, LW_EXCLUSIVE);
    if (!SHMQueueIsDetached(&(t_thrd.proc->syncPaxosLinks)))
        SHMQueueDelete(&(t_thrd.proc->syncPaxosLinks));
    t_thrd.proc->syncPaxosState = SYNC_REP_NOT_WAITING;
    LWLockRelease(SyncPaxosLock);
}

/*
 * Insert t_thrd.proc into the specified SyncPaxosQueue, maintaining sorted invariant.
 *
 * Usually we will go at tail of queue, though it's possible that we arrive
 * here out of order, so start at tail and work back to insertion point.
 */
static void SyncPaxosQueueInsert(void)
{
    PGPROC* proc = NULL;

    proc = (PGPROC* ) SHMQueuePrev(&(t_thrd.walsender_cxt.WalSndCtl->SyncPaxosQueue),
                                   &(t_thrd.walsender_cxt.WalSndCtl->SyncPaxosQueue),
                                   offsetof(PGPROC, syncPaxosLinks));

    while (proc != NULL) {
        /*
         * Stop at the queue element that we should after to ensure the queue
         * is ordered by LSN.
         */
        if (XLByteLE(proc->waitPaxosLSN, t_thrd.proc->waitPaxosLSN))
            break;

        proc = (PGPROC *) SHMQueuePrev(&(t_thrd.walsender_cxt.WalSndCtl->SyncPaxosQueue),
                                       &(proc->syncPaxosLinks),
                                       offsetof(PGPROC, syncPaxosLinks));
    }

    if (proc != NULL)
        SHMQueueInsertAfter(&(proc->syncPaxosLinks), &(t_thrd.proc->syncPaxosLinks));
    else
        SHMQueueInsertAfter(&(t_thrd.walsender_cxt.WalSndCtl->SyncPaxosQueue), &(t_thrd.proc->syncPaxosLinks));
}

/*
 * Update the LSNs on paxos queue based upon our latest state.
 * 
 * NOTE: we pass paxosConsensus lsn here, in order not to get xlogctl->info_lck.
 */
void SyncPaxosReleaseWaiters(XLogRecPtr PaxosConsensusLSN)
{
    volatile WalSndCtlData *walsndctl = t_thrd.walsender_cxt.WalSndCtl;
    int numProc = 0;

    LWLockAcquire(SyncPaxosLock, LW_EXCLUSIVE);

    /*
     * Set the lsn first so that when we wake backends they will release up to
     * this location.
     */
    if (XLByteLE(walsndctl->paxosLsn, PaxosConsensusLSN)) {
        walsndctl->paxosLsn = PaxosConsensusLSN;
        numProc = SyncPaxosWakeQueue();
    }

    LWLockRelease(SyncPaxosLock);

    ereport(DEBUG3, (errmsg("released %d procs up to paxos consensus %X/%X", 
            numProc, (uint32) (PaxosConsensusLSN >> 32), (uint32) PaxosConsensusLSN)));
}

/*
 * Walk the specified queue from head.    Set the state of any backends that
 * need to be woken, remove them from the queue, and then wake them.
 *
 * Must hold SyncPaxosLock.
 */
static int SyncPaxosWakeQueue(void)
{
    volatile WalSndCtlData *walsndctl = t_thrd.walsender_cxt.WalSndCtl;
    PGPROC *proc = NULL;
    PGPROC *thisproc = NULL;
    int numprocs = 0;

    Assert(SyncPaxosQueueIsOrderedByLSN());

    proc = (PGPROC *) SHMQueueNext(&(t_thrd.walsender_cxt.WalSndCtl->SyncPaxosQueue),
                                   &(t_thrd.walsender_cxt.WalSndCtl->SyncPaxosQueue),
                                   offsetof(PGPROC, syncPaxosLinks));

    while (proc != NULL) {
        /*
         * Assume the queue is ordered by LSN
         */
        if (XLByteLT(walsndctl->paxosLsn, proc->waitPaxosLSN))
            return numprocs;

        /*
         * Move to next proc, so we can delete thisproc from the queue.
         * thisproc is valid, proc may be NULL after this.
         */
        thisproc = proc;
        proc = (PGPROC *) SHMQueueNext(&(t_thrd.walsender_cxt.WalSndCtl->SyncPaxosQueue),
                                       &(proc->syncPaxosLinks),
                                       offsetof(PGPROC, syncPaxosLinks));

        /*
         * Remove thisproc from queue.
         */
        SHMQueueDelete(&(thisproc->syncPaxosLinks));

        /*
         * SyncRepWaitForLSN() reads syncRepState without holding the lock, so
         * make sure that it sees the queue link being removed before the
         * syncRepState change.
         */
        pg_write_barrier();

        /*
         * Set state to complete; see SyncRepWaitForLSN() for discussion of
         * the various states.
         */
        thisproc->syncPaxosState = SYNC_REP_WAIT_COMPLETE;

        /*
         * Wake only when we have set state and removed from queue.
         */
        SetLatch(&(thisproc->procLatch));

        numprocs++;
    }

    return numprocs;
}

#ifdef USE_ASSERT_CHECKING
static bool SyncRepQueueIsOrderedByLSN(int mode)
{
    PGPROC *proc = NULL;
    XLogRecPtr lastLSN;

    Assert(mode >= 0 && mode < NUM_SYNC_REP_WAIT_MODE);

    lastLSN = 0;

    proc = (PGPROC *)SHMQueueNext(&(t_thrd.walsender_cxt.WalSndCtl->SyncRepQueue[mode]),
                                  &(t_thrd.walsender_cxt.WalSndCtl->SyncRepQueue[mode]),
                                  offsetof(PGPROC, syncRepLinks));

    while (proc != NULL) {
        /*
         * Check the queue is ordered by LSN
         */
        if (XLByteLT(proc->waitLSN, lastLSN))
            return false;

        lastLSN = proc->waitLSN;

        proc = (PGPROC *)SHMQueueNext(&(t_thrd.walsender_cxt.WalSndCtl->SyncRepQueue[mode]), &(proc->syncRepLinks),
                                      offsetof(PGPROC, syncRepLinks));
    }

    return true;
}
#endif

#ifdef USE_ASSERT_CHECKING
static bool SyncPaxosQueueIsOrderedByLSN(void)
{
    PGPROC *proc = NULL;
    XLogRecPtr lastPaxosLSN = 0;

    proc = (PGPROC *) SHMQueueNext(&(t_thrd.walsender_cxt.WalSndCtl->SyncPaxosQueue),
                                   &(t_thrd.walsender_cxt.WalSndCtl->SyncPaxosQueue),
                                   offsetof(PGPROC, syncPaxosLinks));

    while (proc != NULL) {
        /*
         * Check the queue is ordered by LSN
         */
        if (XLByteLT(proc->waitPaxosLSN, lastPaxosLSN))
            return false;

        lastPaxosLSN = proc->waitPaxosLSN;

        proc = (PGPROC *) SHMQueueNext(&(t_thrd.walsender_cxt.WalSndCtl->SyncPaxosQueue),
                                       &(proc->syncPaxosLinks),
                                       offsetof(PGPROC, syncPaxosLinks));
    }

    return true;
}
#endif

/*
 * =================================================================
 * Analyze and transform GUC param synchronous_standby_names content
 * Create result in session MEMORY_CONTEXT_STORAGE session.
 * =================================================================
 */
static TransContext* create_transform_context()
{
    MemoryContext old_context = MemoryContextSwitchTo(SESS_GET_MEM_CXT_GROUP(MEMORY_CONTEXT_STORAGE));
    TransContext* tcxt = (TransContext*)palloc(sizeof(TransContext));
    (void)MemoryContextSwitchTo(old_context);

    tcxt->source = t_thrd.syncrepgram_cxt.syncrep_parse_result;
    tcxt->SyncRepConfigGroups = list_length(tcxt->source);
    Assert(tcxt->SyncRepConfigGroups > 0);
    tcxt->success = true;
    tcxt->has_star = false;
    tcxt->existers = NIL;
    tcxt->conf = NULL;
    tcxt->is_star = false;
    
    tcxt->SyncRepConfig = NIL;
    tcxt->SyncRepAllCount = 0;
    tcxt->SyncRepMaxPossib = MAX_INT32;
    tcxt->SyncRepMinPossib = 0;

    return tcxt;
}

static void bind_transform_context(TransContext *tcxt, SyncRepConfigData *conf)
{
    tcxt->conf = conf;
    tcxt->is_star = false;
}

static void advance_transform_result(TransContext *tcxt)
{
    tcxt->SyncRepMinPossib += tcxt->conf->num_sync;
    if (tcxt->has_star) {
        tcxt->SyncRepAllCount = -1;
        tcxt->SyncRepMaxPossib = tcxt->SyncRepMinPossib;
    } else {
        tcxt->SyncRepAllCount += tcxt->conf->nmembers;
        tcxt->SyncRepMaxPossib = Min(tcxt->SyncRepMaxPossib, tcxt->conf->nmembers - tcxt->conf->num_sync);
    }
}

static void finalize_transform_result(TransContext *tcxt)
{
    if (!tcxt->success) {
        clear_transform_context(tcxt);
        return;
    }

    MemoryContext old_context = MemoryContextSwitchTo(SESS_GET_MEM_CXT_GROUP(MEMORY_CONTEXT_STORAGE));
    SyncRepConfigData* conf = NULL;
    ListCell* lc = NULL;
    SyncRepConfigData* result = NULL;
    errno_t rc;
    foreach(lc, tcxt->source) {
        conf = (SyncRepConfigData*)lfirst(lc);
        result = (SyncRepConfigData*)palloc(conf->config_size);
        rc = memcpy_s(result, conf->config_size, conf, conf->config_size);
        securec_check(rc, "", "");
        tcxt->SyncRepConfig = lappend(tcxt->SyncRepConfig, result);
    }
    (void)MemoryContextSwitchTo(old_context);

    if (!tcxt->has_star) {
        tcxt->SyncRepMaxPossib = tcxt->SyncRepAllCount - tcxt->SyncRepMaxPossib;
    }

    clear_transform_context(tcxt);
}

/*
 * clear the transform context, except results.
 */
static void clear_transform_context(TransContext *tcxt)
{
    tcxt->source = NIL;
    tcxt->conf = NULL;
    list_free(tcxt->existers);
    tcxt->existers = NIL;
}

static void destroy_transform_context(TransContext *tcxt)
{
    list_free(tcxt->existers);
    list_free_deep(tcxt->SyncRepConfig);
    pfree(tcxt);
}

static bool analyze_star_and_num(TransContext *tcxt)
{
    SyncRepConfigData *conf = tcxt->conf;

    char *p = conf->member_names;
    for (int i = 0; i < conf->nmembers; i++) {
        if (strcmp(p, "*") != 0) {
            p += strlen(p) + 1;
            continue;
        }

        if (tcxt->SyncRepConfigGroups > 1) {
            GUC_check_errdetail("'*' is not support when sync standby strategy is combinated.");
            return false;
        }
        if (conf->nmembers > 1) {
            GUC_check_errdetail("Please use the '*' separately, otherwise, it is semantic ambiguity.");
            return false;
        }
        tcxt->is_star = true;
        tcxt->has_star = true;
    }

    if (!tcxt->is_star && conf->num_sync > conf->nmembers) {
        GUC_check_errdetail("The sync number must less or equals to the number of standby node names.");
        return false;
    }

    return true;
}

static bool analyze_duplicate_names(TransContext *tcxt)
{
    if (tcxt->is_star)
        return true;

    SyncRepConfigData *conf = tcxt->conf;
    char *p = conf->member_names;
    char *exister = NULL;

    ListCell *lc = NULL;
    for (int i = 0; i < conf->nmembers; i++) {
        foreach(lc, tcxt->existers) {
            exister = (char*)lfirst(lc);
            if (pg_strcasecmp(p, exister) == 0) {
                GUC_check_errdetail("Duplicate standby node name: %s", p);
                return false;
            }
        }
        tcxt->existers = lappend(tcxt->existers, p);
        p += strlen(p) + 1;
    }
    return true;
}

static void transform_synchronous_standby_names(TransContext* tcxt)
{
    if (tcxt->SyncRepConfigGroups > SYNC_REP_MAX_GROUPS) {
        GUC_check_errdetail("Too much groups. Please no more than %d.", SYNC_REP_MAX_GROUPS);
        tcxt->success = false;
        clear_transform_context(tcxt);
        return;
    }

    ListCell* lc = NULL;
    foreach(lc, t_thrd.syncrepgram_cxt.syncrep_parse_result) {
        bind_transform_context(tcxt, (SyncRepConfigData*)lfirst(lc));

        if (tcxt->conf->syncrep_method == SYNC_REP_PRIORITY && tcxt->SyncRepConfigGroups > 1) {
            GUC_check_errdetail("FIRST rule is not support when sync standby strategy is combinated.");
            tcxt->success = false;
            break;
        }
        if (!analyze_star_and_num(tcxt) ||
            !analyze_duplicate_names(tcxt)) {
            tcxt->success = false;
            break;
        }

        advance_transform_result(tcxt);
    }

    finalize_transform_result(tcxt);
}

/*
 * ===========================================================
 * Synchronous Replication functions executed by any process
 * ===========================================================
 */
bool check_synchronous_standby_names(char **newval, void **extra, GucSource source)
{
    if (*newval != NULL && (*newval)[0] != '\0') {
        int parse_rc;
        syncrep_scanner_yyscan_t yyscanner;

        /* Reset communication variables to ensure a fresh start */
        t_thrd.syncrepgram_cxt.syncrep_parse_result = NIL;

        /* Parse the synchronous_standby_names string */
        yyscanner = syncrep_scanner_init(*newval);
        parse_rc = syncrep_yyparse(yyscanner);
        syncrep_scanner_finish(yyscanner);

        if (parse_rc != 0 || t_thrd.syncrepgram_cxt.syncrep_parse_result == NIL) {
            GUC_check_errcode(ERRCODE_SYNTAX_ERROR);
            GUC_check_errdetail("synchronous_standby_names parser failed");
            return false;
        }

        /*
         * analyze and transform synchronous_standby_names content,
         * make result in session MEMORY_CONTEXT_STORAGE.
         */
        TransContext* tcxt = create_transform_context();
        transform_synchronous_standby_names(tcxt);
        if (!tcxt->success) {
            destroy_transform_context(tcxt);
            return false;
        }

        *extra = (void *)tcxt;
        if (t_thrd.syncrepgram_cxt.syncrep_parse_result) {
            list_free_deep(t_thrd.syncrepgram_cxt.syncrep_parse_result);
            t_thrd.syncrepgram_cxt.syncrep_parse_result = NIL;
        }

        /*
         * We need not explicitly clean up syncrep_parse_result.  It, and any
         * other cruft generated during parsing, will be freed when the
         * current memory context is deleted.  (This code is generally run in
         * a short-lived context used for config file processing, so that will
         * not be very long.)
         */
    } else
        *extra = NULL;

    return true;
}

void assign_synchronous_standby_names(const char *newval, void *extra)
{
    /*
     * At present, SyncRepConfig is kept at thread level, on the assumption that
     * it should be safe to know the latest rep config ASAP for all sessions.
     * If this assumption no longer holds, please move it to session level.
     */
    for (int i = 0; i < t_thrd.syncrep_cxt.SyncRepConfigGroups; i++) {
        pfree_ext(t_thrd.syncrep_cxt.SyncRepConfig[i]);
    }
    pfree_ext(t_thrd.syncrep_cxt.SyncRepConfig);
    t_thrd.syncrep_cxt.SyncRepConfigGroups = 0;
    t_thrd.syncrep_cxt.SyncRepMaxPossib = 0;

    if (extra == NULL)
        return;

    TransContext* tcxt = (TransContext*)extra;
    ListCell* lc = NULL;
    int i = 0;
    errno_t rc = EOK;
    MemoryContext old_context = MemoryContextSwitchTo(THREAD_GET_MEM_CXT_GROUP(MEMORY_CONTEXT_STORAGE));
    t_thrd.syncrep_cxt.SyncRepConfigGroups = tcxt->SyncRepConfigGroups;
    t_thrd.syncrep_cxt.SyncRepMaxPossib = tcxt->SyncRepMaxPossib;
    t_thrd.syncrep_cxt.SyncRepConfig = (SyncRepConfigData **)palloc(
        tcxt->SyncRepConfigGroups * sizeof(SyncRepConfigData*));

    foreach(lc, tcxt->SyncRepConfig) {
        SyncRepConfigData* pconf = (SyncRepConfigData*)lfirst(lc);
        t_thrd.syncrep_cxt.SyncRepConfig[i] = (SyncRepConfigData*)palloc(pconf->config_size);
        rc = memcpy_s(t_thrd.syncrep_cxt.SyncRepConfig[i], pconf->config_size, pconf, pconf->config_size);
        securec_check(rc, "", "");

        i++;
    }

    (void)MemoryContextSwitchTo(old_context);
}

void assign_synchronous_commit(int newval, void *extra)
{
    switch (newval) {
        case SYNCHRONOUS_COMMIT_REMOTE_RECEIVE:
            u_sess->attr.attr_storage.sync_rep_wait_mode = SYNC_REP_WAIT_RECEIVE;
            break;
        case SYNCHRONOUS_COMMIT_REMOTE_WRITE:
            u_sess->attr.attr_storage.sync_rep_wait_mode = SYNC_REP_WAIT_WRITE;
            break;
        case SYNCHRONOUS_COMMIT_REMOTE_FLUSH:
            u_sess->attr.attr_storage.sync_rep_wait_mode = SYNC_REP_WAIT_FLUSH;
            break;
        case SYNCHRONOUS_COMMIT_REMOTE_APPLY:
            u_sess->attr.attr_storage.sync_rep_wait_mode = SYNC_REP_WAIT_APPLY;
            break;
        default:
            u_sess->attr.attr_storage.sync_rep_wait_mode = SYNC_REP_NO_WAIT;
            break;
    }
}

int syncrep_yylex(YYSTYPE *lvalp, YYLTYPE *llocp, syncrep_scanner_yyscan_t yyscanner)
{
    return syncrep_scanner_yylex(&(lvalp->yy_core), llocp, yyscanner);
}
