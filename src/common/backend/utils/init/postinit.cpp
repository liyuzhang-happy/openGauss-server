/* -------------------------------------------------------------------------
 *
 * postinit.c
 *	  postgres initialization utilities
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/postinit.c
 *
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

#include "access/heapam.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_hashbucket_fn.h"
#include "executor/executor.h"
#include "executor/execStream.h"
#include "executor/nodeModifyTable.h"
#include "job/job_scheduler.h"
#include "job/job_worker.h"
#include "libpq/auth.h"
#include "libpq/ip.h"
#include "libpq/libpq-be.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "pgxc/execRemote.h"
#include "pgxc/poolmgr.h"
#include "pgxc/groupmgr.h"
#include "pgxc/pgxc.h"
#include "pgxc/pgxcnode.h"
#include "postmaster/autovacuum.h"
#include "postmaster/postmaster.h"
#include "replication/catchup.h"
#include "replication/walsender.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/sinvaladt.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "threadpool/threadpool.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/pg_locale.h"
#include "utils/portal.h"
#include "utils/postinit.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tqual.h"
#include "auditfuncs.h"
#include "gssignal/gs_signal.h"
#include "storage/cucache_mgr.h"
#include "alarm/alarm.h"
#include "commands/user.h"
#include "instruments/snapshot.h"
#include "instruments/instr_user.h"
#include "instruments/percentile.h"
#include "instruments/instr_workload.h"

#ifdef PGXC
#include "catalog/pgxc_node.h"
#include "utils/rel.h"
#include "utils/rel_gs.h"
#include "utils/lsyscache.h"
static void AlterPgxcNodePort(void);
#endif

bool ConnAuthMethodCorrect = true;
Alarm alarmItemTooManyDatabaseConn[1] = {ALM_AI_Unknown, ALM_AS_Normal, 0, 0, 0, 0, {0}, {0}, NULL};

static HeapTuple GetDatabaseTuple(const char* dbname);
static HeapTuple GetDatabaseTupleByOid(Oid dboid);
static void PerformAuthentication(Port* port);
static void CheckMyDatabase(const char* name, bool am_superuser);
static void InitCommunication(void);
static bool ThereIsAtLeastOneRole(void);
static void process_startup_options(Port* port, bool am_superuser);
static void process_pgoptions(Port* port, bool am_superuser);
static void process_settings(Oid databaseid, Oid roleid);
extern bool StreamThreadAmI();
extern bool StreamTopConsumerAmI();
void ShutdownPostgres(int code, Datum arg);

#ifdef ENABLE_MULTIPLE_NODES
/* 
 * init cache api for pgxc nodes
 */
extern void PGXC_Cache_Init();
#endif

AlarmCheckResult ConnAuthMethodChecker(Alarm* alarm, AlarmAdditionalParam* additionalParam)
{
    if (true == ConnAuthMethodCorrect) {
        // fill the resume message
        WriteAlarmAdditionalInfo(
            additionalParam, g_instance.attr.attr_common.PGXCNodeName, "", "", alarm, ALM_AT_Resume);
        return ALM_ACR_Normal;
    } else {
        // fill the alarm message
        WriteAlarmAdditionalInfo(additionalParam,
            g_instance.attr.attr_common.PGXCNodeName,
            "",
            "",
            alarm,
            ALM_AT_Fault,
            g_instance.attr.attr_common.PGXCNodeName);
        return ALM_ACR_Abnormal;
    }
}

void ReportAlarmTooManyDatabaseConn(const char* dbName)
{
    AlarmAdditionalParam temp_additional_param;

    // Initialize the alarm item
    AlarmItemInitialize(alarmItemTooManyDatabaseConn,
        ALM_AI_TooManyDatabaseConn,
        alarmItemTooManyDatabaseConn->stat,
        NULL,
        alarmItemTooManyDatabaseConn->lastReportTime,
        alarmItemTooManyDatabaseConn->reportCount);
    // fill the alarm message
    WriteAlarmAdditionalInfo(&temp_additional_param,
        g_instance.attr.attr_common.PGXCNodeName,
        const_cast<char *>(dbName),
        "",
        alarmItemTooManyDatabaseConn,
        ALM_AT_Fault,
        const_cast<char *>(dbName));
    // report the alarm
    AlarmReporter(alarmItemTooManyDatabaseConn, ALM_AT_Fault, &temp_additional_param);
}

void ReportResumeTooManyDatabaseConn(const char* dbName)
{
    AlarmAdditionalParam temp_additional_param;

    // Initialize the alarm item
    AlarmItemInitialize(alarmItemTooManyDatabaseConn,
        ALM_AI_TooManyDatabaseConn,
        alarmItemTooManyDatabaseConn->stat,
        NULL,
        alarmItemTooManyDatabaseConn->lastReportTime,
        alarmItemTooManyDatabaseConn->reportCount);
    // fill the alarm message
    WriteAlarmAdditionalInfo(&temp_additional_param,
        g_instance.attr.attr_common.PGXCNodeName,
        const_cast<char *>(dbName),
        "",
        alarmItemTooManyDatabaseConn,
        ALM_AT_Resume);
    // report the alarm
    AlarmReporter(alarmItemTooManyDatabaseConn, ALM_AT_Resume, &temp_additional_param);
}

/*
 * GetDatabaseTuple -- fetch the pg_database row for a database
 *
 * This is used during backend startup when we don't yet have any access to
 * system catalogs in general.	In the worst case, we can seqscan pg_database
 * using nothing but the hard-wired descriptor that relcache.c creates for
 * pg_database.  In more typical cases, relcache.c was able to load
 * descriptors for both pg_database and its indexes from the shared relcache
 * cache file, and so we can do an indexscan.  u_sess->relcache_cxt.criticalSharedRelcachesBuilt
 * tells whether we got the cached descriptors.
 */
static HeapTuple GetDatabaseTuple(const char* dbname)
{
    HeapTuple tuple;
    Relation relation;
    SysScanDesc scan;
    ScanKeyData key[1];

    /*
     * form a scan key
     */
    ScanKeyInit(&key[0], Anum_pg_database_datname, BTEqualStrategyNumber, F_NAMEEQ, CStringGetDatum(dbname));

    /*
     * Open pg_database and fetch a tuple.	Force heap scan if we haven't yet
     * built the critical shared relcache entries (i.e., we're starting up
     * without a shared relcache cache file).
     */
    relation = heap_open(DatabaseRelationId, AccessShareLock);
    scan = systable_beginscan(
        relation, DatabaseNameIndexId, u_sess->relcache_cxt.criticalSharedRelcachesBuilt, SnapshotNow, 1, key);

    tuple = systable_getnext(scan);

    /* Must copy tuple before releasing buffer */
    if (HeapTupleIsValid(tuple)) {
        tuple = heap_copytuple(tuple);
    }
    /* all done */
    systable_endscan(scan);
    heap_close(relation, AccessShareLock);

    return tuple;
}

/*
 * GetDatabaseTupleByOid -- as above, but search by database OID
 */
static HeapTuple GetDatabaseTupleByOid(Oid dboid)
{
    HeapTuple tuple;
    Relation relation;
    SysScanDesc scan;
    ScanKeyData key[1];

    /*
     * form a scan key
     */
    ScanKeyInit(&key[0], ObjectIdAttributeNumber, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(dboid));

    /*
     * Open pg_database and fetch a tuple.	Force heap scan if we haven't yet
     * built the critical shared relcache entries (i.e., we're starting up
     * without a shared relcache cache file).
     */
    relation = heap_open(DatabaseRelationId, AccessShareLock);
    scan = systable_beginscan(
        relation, DatabaseOidIndexId, u_sess->relcache_cxt.criticalSharedRelcachesBuilt, SnapshotNow, 1, key);

    tuple = systable_getnext(scan);

    /* Must copy tuple before releasing buffer */
    if (HeapTupleIsValid(tuple)) {
        tuple = heap_copytuple(tuple);
    }
    /* all done */
    systable_endscan(scan);
    heap_close(relation, AccessShareLock);

    return tuple;
}

/*
 * PerformAuthentication -- authenticate a remote client
 *
 * returns: nothing.  Will not return at all if there's any failure.
 */
static void PerformAuthentication(Port* port)
{
    sigset_t old_sigset;

    /* This should be set already, but let's make sure */
    u_sess->ClientAuthInProgress = true; /* limit visibility of log messages */

    /*
     * In EXEC_BACKEND case, we didn't inherit the contents of pg_hba.conf
     * etcetera from the postmaster, and have to load them ourselves.  Note we
     * are loading them into the startup transaction's memory context, not
     * t_thrd.mem_cxt.postmaster_mem_cxt, but that shouldn't matter.
     */
#ifdef EXEC_BACKEND

    int loadhbaCount = 0;
    while (!load_hba()) {
        loadhbaCount++;
        pg_usleep(200000L);  // slepp 200ms for reload
        if (loadhbaCount >= 3) {
            /*
             * It makes no sense to continue if we fail to load the HBA file,
             * since there is no way to connect to the database in this case.
             */
            ereport(FATAL, (errmsg("could not load pg_hba.conf")));
        }
    }

    /*
     * It is ok to continue if we fail to load the IDENT file, although it
     * means that we do not exist any authentication mapping between sys_user
     * and database user. load_ident() already logged the details of error
     * to the log.
     */
    (void)load_ident();

#endif

    /*
     * Set up a timeout in case a buggy or malicious client fails to respond
     * during authentication.  Since we're inside a transaction and might do
     * database access, we have to use the statement_timeout infrastructure.
     */
    if (!enable_sig_alarm(u_sess->attr.attr_security.AuthenticationTimeout * 1000, true)) {
        ereport(FATAL, (errmsg("could not set timer for authorization timeout")));
    }
    /*
     * Unblock SIGUSR2 so that SIGALRM can be triggered when perform authentication timeout.
     */
    old_sigset = gs_signal_unblock_sigusr2();

    /*
     * Now perform authentication exchange.
     */
    ClientAuthentication(port); /* might not return, if failure */

    /*
     * recover the signal mask before call ClientAuthentication.
     */
    gs_signal_recover_mask(old_sigset);

    /*
     * Done with authentication.  Disable the timeout, and log if needed.
     */
    if (!disable_sig_alarm(true)) {
        ereport(FATAL, (errmsg("could not disable timer for authorization timeout")));
    }

    if (u_sess->attr.attr_storage.Log_connections) {
        if (AM_WAL_SENDER) {
            ereport(LOG, (errmsg("replication connection authorized: user=%s", port->user_name)));
        } else {
            ereport(LOG, (errmsg("connection authorized: user=%s database=%s", port->user_name, port->database_name)));
        }
    }

    /* INSTR: update user login counter */
    if (IsUnderPostmaster && !IsBootstrapProcessingMode() && !dummyStandbyMode) {
        InstrUpdateUserLogCounter(true);
    }

    set_ps_display("startup", false);

    u_sess->ClientAuthInProgress = false; /* client_min_messages is active now */
    u_sess->misc_cxt.authentication_finished = true;
}

// Check if the connection is local
//
static bool CheckLocalConnection()
{
    Assert(u_sess->proc_cxt.MyProcPort != NULL);
    if (IS_AF_UNIX(u_sess->proc_cxt.MyProcPort->raddr.addr.ss_family) ||
        strcmp(u_sess->proc_cxt.MyProcPort->remote_host, "127.0.0.1") == 0 ||
        strcmp(u_sess->proc_cxt.MyProcPort->remote_host, "::1") == 0) {
        return true;
    } else {
        return false;
    }
}

static void SaveSessionEncodingInfo(Form_pg_database dbform)
{
    errno_t rc;
    rc = strncpy_s(NameStr(u_sess->mb_cxt.datctype), NAMEDATALEN, NameStr(dbform->datctype), NAMEDATALEN - 1);
    securec_check(rc, "\0", "\0");
    rc = strncpy_s(NameStr(u_sess->mb_cxt.datcollate), NAMEDATALEN, NameStr(dbform->datcollate), NAMEDATALEN - 1);
    securec_check(rc, "\0", "\0");
    rc = strncpy_s(NameStr(t_thrd.port_cxt.cur_datctype), NAMEDATALEN, NameStr(dbform->datctype), NAMEDATALEN - 1);
    securec_check(rc, "\0", "\0");
    rc = strncpy_s(NameStr(t_thrd.port_cxt.cur_datcollate), NAMEDATALEN, NameStr(dbform->datcollate), NAMEDATALEN - 1);
    securec_check(rc, "\0", "\0");
}

/*
 * CheckMyDatabase -- fetch information from the pg_database entry for our DB
 */
static void CheckMyDatabase(const char* name, bool am_superuser)
{
    HeapTuple tup;
    Form_pg_database db_form;
    char* collate = NULL;
    char* ctype = NULL;

    /* Fetch our pg_database row normally, via syscache */
    tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(u_sess->proc_cxt.MyDatabaseId));

    if (!HeapTupleIsValid(tup)) {
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("cache lookup failed for database %u", u_sess->proc_cxt.MyDatabaseId)));
    }

    db_form = (Form_pg_database)GETSTRUCT(tup);

    /* This recheck is strictly paranoia */
    if (strcmp(name, NameStr(db_form->datname)) != 0) {
        ereport(FATAL,
            (errcode(ERRCODE_UNDEFINED_DATABASE),
                errmsg("database \"%s\" has disappeared from pg_database", name),
                errdetail("Database OID %u now seems to belong to \"%s\".",
                    u_sess->proc_cxt.MyDatabaseId,
                    NameStr(db_form->datname))));
    }
    /*
     * Check permissions to connect to the database.
     *
     * These checks are not enforced when in standalone mode, so that there is
     * a way to recover from disabling all access to all databases, for
     * example "UPDATE pg_database SET datallowconn = false;".
     *
     * We do not enforce them for autovacuum worker processes either.
     */
    if (IsUnderPostmaster && !IsAutoVacuumWorkerProcess()) {
        /*
         * Check that the database is currently allowing connections.
         */
        if (!db_form->datallowconn && (u_sess->attr.attr_common.upgrade_mode == 0 || !am_superuser)) {
            ereport(FATAL,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                    errmsg("database \"%s\" is not currently accepting connections", name)));
        }
        /*
         * Check privilege to connect to the database.	(The am_superuser test
         * is redundant, but since we have the flag, might as well check it
         * and save a few cycles.)
         */
        if (!am_superuser &&
            pg_database_aclcheck(u_sess->proc_cxt.MyDatabaseId, GetUserId(), ACL_CONNECT) != ACLCHECK_OK) {
            ereport(FATAL,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    errmsg("permission denied for database \"%s\"", name),
                    errdetail("User does not have CONNECT privilege.")));
        }
        /*
         * Check connection limit for this database.
         *
         * There is a race condition here --- we create our PGPROC before
         * checking for other PGPROCs.	If two backends did this at about the
         * same time, they might both think they were over the limit, while
         * ideally one should succeed and one fail.  Getting that to work
         * exactly seems more trouble than it is worth, however; instead we
         * just document that the connection limit is approximate.
         */
        if (db_form->datconnlimit >= 0 && !am_superuser &&
            CountDBBackends(u_sess->proc_cxt.MyDatabaseId) > db_form->datconnlimit) {
            ReportAlarmTooManyDatabaseConn(name);

            ereport(FATAL,
                (errcode(ERRCODE_TOO_MANY_CONNECTIONS), errmsg("too many connections for database \"%s\"", name)));
        } else if (!am_superuser) {
            ReportResumeTooManyDatabaseConn(name);
        }
    }

    /*
     * OK, we're golden.  Next to-do item is to save the encoding info out of
     * the pg_database tuple.
     */
    SetDatabaseEncoding(db_form->encoding);
    /* Record it as a GUC internal option, too */
    SetConfigOption("server_encoding", GetDatabaseEncodingName(), PGC_INTERNAL, PGC_S_OVERRIDE);
    /* If we have no other source of client_encoding, use server encoding */
    SetConfigOption("client_encoding", GetDatabaseEncodingName(), PGC_BACKEND, PGC_S_DYNAMIC_DEFAULT);

    // if we are identical no bother to set that in thread pool settings.
    if (!IS_THREAD_POOL_WORKER || strcmp(NameStr(db_form->datcollate), NameStr(t_thrd.port_cxt.cur_datcollate)) != 0 ||
        strcmp(NameStr(db_form->datctype), NameStr(t_thrd.port_cxt.cur_datctype)) != 0) {
        /* assign locale variables */
        collate = NameStr(db_form->datcollate);
        ctype = NameStr(db_form->datctype);

        if (pg_perm_setlocale(LC_COLLATE, collate) == NULL) {
            ereport(FATAL,
                (errmsg("database locale is incompatible with operating system"),
                    errdetail("The database was initialized with LC_COLLATE \"%s\", "
                              " which is not recognized by setlocale().",
                        collate),
                    errhint("Recreate the database with another locale or install the missing locale.")));
        }

        if (pg_perm_setlocale(LC_CTYPE, ctype) == NULL) {
            ereport(FATAL,
                (errmsg("database locale is incompatible with operating system"),
                    errdetail("The database was initialized with LC_CTYPE \"%s\", "
                              " which is not recognized by setlocale().",
                        ctype),
                    errhint("Recreate the database with another locale or install the missing locale.")));
        }

        /* Make the locale settings visible as GUC variables, too */
        SetConfigOption("lc_collate", collate, PGC_INTERNAL, PGC_S_OVERRIDE);
        SetConfigOption("lc_ctype", ctype, PGC_INTERNAL, PGC_S_OVERRIDE);

        /* Use the right encoding in translated messages */
#ifdef ENABLE_NLS
        pg_bind_textdomain_codeset(textdomain(NULL));
#endif
    }

    if (IS_THREAD_POOL_WORKER) {
        // save for next session time restore.
        SaveSessionEncodingInfo(db_form);
    }

    SetConfigOption("sql_compatibility", NameStr(db_form->datcompatibility), PGC_INTERNAL, PGC_S_OVERRIDE);

    ReleaseSysCache(tup);
}

static void CheckConnAuthority(const char* name, bool am_superuser)
{
    // Database Security: Check privilege to connect to the database.
    // Only superuser on the local machine can connect to "template1".
    if (IsUnderPostmaster && !IsAutoVacuumWorkerProcess() && !IsJobSchedulerProcess() && !IsJobWorkerProcess()) {
        if ((IS_PGXC_COORDINATOR || IS_SINGLE_NODE) && IsConnFromApp() &&
            (!am_superuser || !IsLocalAddr(u_sess->proc_cxt.MyProcPort)) &&
            strcmp(name, "template1") == 0) {
            ereport(FATAL,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    errmsg("permission denied for database \"%s\"", name),
                    errdetail("User does not have CONNECT privilege.")));
        }
    }
}

/* --------------------------------
 *		InitCommunication
 *
 *		This routine initializes stuff needed for ipc, locking, etc.
 *		it should be called something more informative.
 * --------------------------------
 */
static void InitCommunication(void)
{

    // initialize shared memory and semaphores appropriately.
    if (!IsUnderPostmaster) { // postmaster already did this
        /*
         * We're running a postgres bootstrap process or a standalone backend.
         * Create private "shmem" and semaphores.
         */
        CreateSharedMemoryAndSemaphores(true, 0);
    }
}

/*
 * pg_split_opts -- split a string of options and append it to an argv array
 *
 * NB: the input string is destructively modified!	Also, caller is responsible
 * for ensuring the argv array is large enough.  The maximum possible number
 * of arguments added by this routine is (strlen(optstr) + 1) / 2.
 *
 * Since no current POSTGRES arguments require any quoting characters,
 * we can use the simple-minded tactic of assuming each set of space-
 * delimited characters is a separate argv element.
 *
 * If you don't like that, well, we *used* to pass the whole option string
 * as ONE argument to execl(), which was even less intelligent...
 */
void pg_split_opts(char** argv, int* argcp, char* optstr)
{
    while (*optstr) {
        while (isspace((unsigned char)*optstr)) {
            optstr++;
        }
        if (*optstr == '\0') {
            break;
        }

        argv[(*argcp)++] = optstr;

        while (*optstr && !isspace((unsigned char)*optstr)) {
            optstr++;
        }

        if (*optstr) {
            *optstr++ = '\0';
        }
    }
}

/*
 * Early initialization of a backend (either standalone or under postmaster).
 * This happens even before InitPostgres.
 *
 * This is separate from InitPostgres because it is also called by auxiliary
 * processes, such as the background writer process, which may not call
 * InitPostgres at all.
 */
void BaseInit(void)
{
    /*
     * Attach to shared memory and semaphores, and initialize our
     * input/output/debugging file descriptors.
     */
    InitCommunication();
    DebugFileOpen();

    /* Do local initialization of file, storage and buffer managers */
    InitFileAccess();
    smgrinit();
    InitBufferPoolAccess();
}

/* -------------------------------------
 * Postgres reset username and pgoption.
 * -------------------------------------
 */
void PostgresResetUsernamePgoption(const char* username)
{
    ereport(DEBUG3, (errmsg("PostgresResetUsernamePgoption()")));

    bool boot_strap = IsBootstrapProcessingMode();
    bool am_superuser = false;

    /*
     * Start a new transaction here before first access to db, and get a
     * snapshot.  We don't have a use for the snapshot itself, but we're
     * interested in the secondary effect that it sets RecentGlobalXmin. (This
     * is critical for anything that reads heap pages, because HOT may decide
     * to prune them even if the process doesn't attempt to modify any
     * tuples.)
     */
    if (!boot_strap && !dummyStandbyMode) {
        /* statement_timestamp must be set for timeouts to work correctly */
        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();

        /*
         * transaction_isolation will have been set to the default by the
         * above.  If the default is "serializable", and we are in hot
         * standby, we will fail if we don't change it to something lower.
         * Fortunately, "read committed" is plenty good enough.
         */
        u_sess->utils_cxt.XactIsoLevel = XACT_READ_COMMITTED;

        (void)GetTransactionSnapshot();
    }

    /*
     * Perform client authentication if necessary, then figure out our
     * postgres user ID, and see if we are a superuser.
     *
     * In standalone mode and in autovacuum worker processes, we use a fixed
     * ID, otherwise we figure it out from the authenticated user name.
     */
    if (boot_strap) {
        InitializeSessionUserIdStandalone();
        am_superuser = true;
    } else if (!IsUnderPostmaster) {
        InitializeSessionUserIdStandalone();
        am_superuser = true;

        if (!ThereIsAtLeastOneRole()) {
            ereport(WARNING,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("no roles are defined in this database system"),
                    errhint("You should immediately run CREATE USER \"%s\" sysadmin;.", username)));
        }
    } else {
        /* normal multiuser case */
        Assert(u_sess->proc_cxt.MyProcPort != NULL);

        if (AM_WAL_SENDER) {
            InitializeSessionUserIdStandalone();
            am_superuser = true;
        } else {
            /*
             * In the wlm worker thread, we set the user is super user
             * and the database is default database, we will send the query
             * to data nodes with the user and the database.
             */
            if (AmWLMWorkerProcess() || AmWLMMonitorProcess() || AmWLMArbiterProcess()) {
                u_sess->proc_cxt.MyProcPort->user_name = (char*)GetSuperUserName((char*)username);
            }

            InitializeSessionUserId(username);
            am_superuser = superuser();
            u_sess->misc_cxt.CurrentUserName = u_sess->proc_cxt.MyProcPort->user_name;
        }
    }

    /*
     * Now process any command-line switches and any additional GUC variable
     * settings passed in the startup packet.	We couldn't do this before
     * because we didn't know if client is a superuser.
     */
    if (u_sess->proc_cxt.MyProcPort != NULL) {
        process_pgoptions(u_sess->proc_cxt.MyProcPort, am_superuser);
    }

    /* close the transaction we started above */
    if (!boot_strap) {
        CommitTransactionCommand();
    }
}

/*
 * Process any command-line switches and any additional GUC variable
 * settings passed in the startup packet.
 */
static void process_startup_options(Port* port, bool am_superuser)
{
    GucContext gucctx;
    ListCell* gucopts = NULL;
    const int SEARCH_PATH_LEN = 64;
    char sql[NAMEDATALEN + SEARCH_PATH_LEN] = {0};
    int rc = -1;
    char* name = NULL;
    char* value = NULL;

    gucctx = am_superuser ? PGC_SUSET : PGC_BACKEND;

    /*
     * First process any command-line switches that were included in the
     * startup packet, if we are in a regular backend.
     */
    if (port->cmdline_options != NULL) {
        /*
         * The maximum possible number of commandline arguments that could
         * come from port->cmdline_options is (strlen + 1) / 2; see
         * pg_split_opts().
         */
        char** av;
        int ac;

        const int maxac = 2 + (strlen(port->cmdline_options) + 1) / 2;

        av = (char**)palloc(maxac * sizeof(char*));
        ac = 0;

        av[ac++] = "postgres";

        /* Note this mangles port->cmdline_options */
        pg_split_opts(av, &ac, port->cmdline_options);

        av[ac] = NULL;

        Assert(ac < maxac);

        (void)process_postgres_switches(ac, av, gucctx, NULL);
    }

    /*
     * At this stage in session initialization, all system catalogs are accessable and
     * we can try to load pgxc node information into shared memory if necessary.
     */
    if (IS_PGXC_COORDINATOR && *t_thrd.pgxc_cxt.shmemNumCoordsInCluster == 0) {
        PgxcNodeListAndCount();
    }

    /* sanity check for ha maintenance port -- only super users are allowed to connect with client applications. */
    if (IsConnFromApp() && IsHAPort(port) && !am_superuser) {
        ConnAuthMethodCorrect = false;
        ereport(FATAL,
            (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
                errmsg("Normal user is not allowed to use HA channel!")));
    }

    /* sanity check for peer address -- outer-cluster connections are only allowed from client applications */
    if (IS_PGXC_COORDINATOR && !is_cluster_internal_connection(port) && !IsConnFromApp()) {
        ereport(FATAL,
            (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION), errmsg("Only applications can connect remotely.")));
    }

    /* sanity check for inner maintenance tools */
    if (u_sess->proc_cxt.IsInnerMaintenanceTools) {
        /* check 1 -- forbid outer-cluster connections, except for resizing and replace with gs_ctl build */
        if (((IS_PGXC_COORDINATOR && !is_cluster_internal_connection(port)) || 
            (IS_SINGLE_NODE && !is_node_internal_connection(port))) &&
            !(u_sess->proc_cxt.clientIsGsCtl && AM_WAL_SENDER)) {
            ereport(FATAL,
                (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
                    errmsg("Forbid remote connection via internal maintenance tools.")));
        }

        /* check 2 -- forbid non-initial users, except during cluster resizing with gs_redis */
        if (!dummyStandbyMode && GetRoleOid(port->user_name) != INITIAL_USER_ID &&
            !(ClusterResizingInProgress() && u_sess->proc_cxt.clientIsGsredis)) {
            ereport(FATAL,
                (errcode(ERRCODE_INVALID_OPERATION), errmsg("Inner maintenance tools only for the initial user.")));
        }
    }

    /* finally, inform alarm reporter about authentification result */
    if (port->hba != NULL && !port->hba->remoteTrust && IsConnFromApp() && !CheckLocalConnection()) {
        ConnAuthMethodCorrect = true;
    }

    /*
     * Process any additional GUC variable settings passed in startup packet.
     * These are handled exactly like command-line variables.
     */
    gucopts = list_head(port->guc_options);

    while (gucopts != NULL) {
        name = (char*)lfirst(gucopts);
        gucopts = lnext(gucopts);

        value = (char*)lfirst(gucopts);
        gucopts = lnext(gucopts);

        SetConfigOption(name, value, gucctx, PGC_S_CLIENT);
        /*
         * JDBC can set schema with connect option,
         * save it in pooler that need remote synchronous.
         * WARNING: schema value do not case sensitive.
         */
        if (IS_PGXC_COORDINATOR && !IsConnFromCoord() &&
            ((pg_strcasecmp(name, "search_path") == 0 &&
                 pg_strcasecmp(u_sess->attr.attr_common.namespace_search_path, value) == 0) ||
                (pg_strcasecmp(name, "current_schema") == 0 &&
                    pg_strcasecmp(u_sess->attr.attr_common.namespace_current_schema, value) == 0))) {
            rc = sprintf_s(sql, sizeof(sql), "SET %s = %s;", name, value);
            securec_check_ss(rc, "\0", "\0");
            (void)register_pooler_session_param(name, sql);
            ereport(DEBUG1, (errmsg("Save pooler session param: %s in startup", sql)));
        }
    }
    return;
}

/*
 * Process pgoptions in pooler stateless reuse mode.
 */
static void process_pgoptions(Port* port, bool am_superuser)
{
    GucContext gucctx;

    gucctx = am_superuser ? PGC_SUSET : PGC_BACKEND;

    /*
     * Process any command-line if we are in a regular backend.
     */
    if (port->cmdline_options != NULL) {
        /*
         * The maximum possible number of commandline arguments that could
         * come from port->cmdline_options is (strlen + 1) / 2; see
         * pg_split_opts().
         */
        char** av;
        int ac;

        const int maxac = 2 + (strlen(port->cmdline_options) + 1) / 2;

        av = (char**)palloc(maxac * sizeof(char*));
        ac = 0;

        av[ac++] = "postgres";

        /* Note this mangles port->cmdline_options */
        pg_split_opts(av, &ac, port->cmdline_options);

        av[ac] = NULL;

        Assert(ac < maxac);

        (void)process_postgres_switches(ac, av, gucctx, NULL);
    }
}

/*
 * Load GUC settings from pg_db_role_setting.
 *
 * We try specific settings for the database/role combination, as well as
 * general for this database and for this user.
 */
static void process_settings(Oid databaseid, Oid roleid)
{
    Relation rel_setting;

    if (!IsUnderPostmaster) {
        return;
    }
    rel_setting = heap_open(DbRoleSettingRelationId, AccessShareLock);

    /* Later settings are ignored if set earlier. */
    ApplySetting(databaseid, roleid, rel_setting, PGC_S_DATABASE_USER);
    ApplySetting(InvalidOid, roleid, rel_setting, PGC_S_USER);
    ApplySetting(databaseid, InvalidOid, rel_setting, PGC_S_DATABASE);

    heap_close(rel_setting, AccessShareLock);
}

/*
 * Backend-shutdown callback.  Do cleanup that we want to be sure happens
 * before all the supporting modules begin to nail their doors shut via
 * their own callbacks.
 *
 * User-level cleanup, such as temp-relation removal and UNLISTEN, happens
 * via separate callbacks that execute before this one.  We don't combine the
 * callbacks because we still want this one to happen if the user-level
 * cleanup fails.
 */
void ShutdownPostgres(int code, Datum arg)
{
    SetInstrNull();
    /* Mark recursive vfd is invalid before aborting transaction. */
#ifdef ENABLE_MULTIPLE_NODES
    StreamNodeGroup::MarkRecursiveVfdInvalid();
#endif
    /* Make sure we've killed any active transaction */
    AbortOutOfAnyTransaction();

    /*
     * If stream Top consumer or stream thread end up as elog FATAL, we must wait until we
     * get a sync point
     */
#ifdef ENABLE_MULTIPLE_NODES
    StreamNodeGroup::syncQuit(STREAM_ERROR);
    StreamNodeGroup::destroy(STREAM_ERROR);
    ForgetRegisterStreamSnapshots();
#endif
    /* Free remote xact state */
    free_RemoteXactState();

    /* If waiting, get off wait queue (should only be needed after error) */
    LockErrorCleanup();
    /* Release standard locks, including session-level if aborting */
    LockReleaseAll(DEFAULT_LOCKMETHOD, true);

    /*
     * User locks are not released by transaction end, so be sure to release
     * them explicitly.
     */
    LockReleaseAll(USER_LOCKMETHOD, true);
}

/*
 * Returns true if at least one role is defined in this database cluster.
 */
static bool ThereIsAtLeastOneRole(void)
{
    Relation pg_authid_rel;
    HeapScanDesc scan;
    bool result = false;

    pg_authid_rel = heap_open(AuthIdRelationId, AccessShareLock);

    scan = heap_beginscan(pg_authid_rel, SnapshotNow, 0, NULL);
    result = (heap_getnext(scan, ForwardScanDirection) != NULL);

    heap_endscan(scan);
    heap_close(pg_authid_rel, AccessShareLock);

    return result;
}

/*
 * when initializing a Postgres-XC cluster node, it executes "CREATE
 * NODE nodename WITH (type = 'coordinator');"  to create node for the
 * current node. The port is not given in this statement, so use the
 * default value 5432. That is why we see the current node's port is 5432,
 * no matter we modify the port or not before start up.
 *
 * This function is used to repair the port of current node in pgxc_node
 * catalog. It is called at the initializing process of postgress, in order
 * to repair the port only once, we use a mutex variable and a static variable.
 */
#ifdef PGXC
static void AlterPgxcNodePort(void)
{
    const char* node_name = NULL;
    const char* node_port_str = NULL;
    int node_port;
    HeapTuple oldtup, newtup;
    Oid node_oid;
    Relation rel;
    Datum new_record[Natts_pgxc_node];
    bool new_record_nulls[Natts_pgxc_node];
    bool new_record_repl[Natts_pgxc_node];
    static bool need_repair = TRUE;
    volatile HaShmemData* hashmdata = t_thrd.postmaster_cxt.HaShmData;
    char node_type = PGXC_NODE_NONE;
    Form_pgxc_node pgxc_node_form;

    SpinLockAcquire(&hashmdata->mutex);
    if (hashmdata->current_mode == STANDBY_MODE || hashmdata->current_mode == PENDING_MODE) {
        SpinLockRelease(&hashmdata->mutex);
        return;
    }
    SpinLockRelease(&hashmdata->mutex);

    if (!IsPostmasterEnvironment || !need_repair || isRestoreMode) {
        return;
    }
    node_name = GetConfigOption("pgxc_node_name", false, false);
    node_port_str = GetConfigOption("port", false, false);
    node_oid = get_pgxc_nodeoid(node_name);

    if (IS_PGXC_DATANODE) {
        node_type = PGXC_NODE_DATANODE;
    }
    /* Only a DB administrator can alter cluster nodes */
    if (!superuser()) {
        return;
    }
    /* Look at the node tuple, and take exclusive lock on it */
    rel = heap_open(PgxcNodeRelationId, RowExclusiveLock);

    /* Check that node exists */
    if (!OidIsValid(node_oid)) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("PGXC Node %s: object not defined", node_name)));
    }
    /* Open new tuple, checks are performed on it and new values */
    oldtup = SearchSysCacheCopy1(PGXCNODEOID, ObjectIdGetDatum(node_oid));
    if (!HeapTupleIsValid(oldtup)) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("cache lookup failed for object %u", node_oid)));
    }
    /* Update values for catalog entry */
    node_port = atoi(node_port_str);

    pgxc_node_form = (Form_pgxc_node)GETSTRUCT(oldtup);

    if (pgxc_node_form->node_port != node_port || pgxc_node_form->node_port1 != node_port) {
        int ss_rc = memset_s(new_record, sizeof(new_record), 0, sizeof(new_record));
        securec_check(ss_rc, "\0", "\0");
        ss_rc = memset_s(new_record_nulls, sizeof(new_record_nulls), false, sizeof(new_record_nulls));
        securec_check(ss_rc, "\0", "\0");
        ss_rc = memset_s(new_record_repl, sizeof(new_record_repl), false, sizeof(new_record_repl));
        securec_check(ss_rc, "\0", "\0");
        new_record[Anum_pgxc_node_port - 1] = Int32GetDatum(node_port);
        new_record_repl[Anum_pgxc_node_port - 1] = true;
        new_record[Anum_pgxc_node_port1 - 1] = Int32GetDatum(node_port);
        new_record_repl[Anum_pgxc_node_port1 - 1] = true;

        if (IS_PGXC_DATANODE) {
            new_record[Anum_pgxc_node_type - 1] = CharGetDatum(node_type);
            new_record_repl[Anum_pgxc_node_type - 1] = true;
        }

        /* Update relation */
        newtup = heap_modify_tuple(oldtup, RelationGetDescr(rel), new_record, new_record_nulls, new_record_repl);
        simple_heap_update(rel, &oldtup->t_self, newtup);

        /* Update indexes */
        CatalogUpdateIndexes(rel, newtup);
    }

    need_repair = FALSE;

    heap_freetuple(oldtup);
    /* Release lock at Commit */
    heap_close(rel, NoLock);
}
#endif

PostgresInitializer::PostgresInitializer()
{
    m_indbname = NULL;
    m_dboid = InvalidOid;
    m_username = NULL;
    m_isSuperUser = false;
    m_fullpath = NULL;
    memset_s(m_dbname, NAMEDATALEN, 0, NAMEDATALEN);
    memset_s(m_details, PGAUDIT_MAXLENGTH, 0, PGAUDIT_MAXLENGTH);
}

PostgresInitializer::~PostgresInitializer()
{
    if (m_fullpath != NULL) {
        pfree_ext(m_fullpath);
    }
    m_indbname = NULL;
    m_username = NULL;
}

void PostgresInitializer::SetDatabaseAndUser(const char* in_dbname, Oid dboid, const char* username)
{
    m_indbname = in_dbname;
    m_dboid = dboid;
    m_username = username;
}

void PostgresInitializer::InitBootstrap()
{
    InitThread();

    InitSysCache();

    SetProcessExitCallback();

    SetSuperUserStandalone();

    SetDefaultDatabase();

    LoadSysCache();

    ProcessStartupOpt();

    InitPGXCPort();

    InitSettings();

    AuditUserLogin();
}

void PostgresInitializer::InitJobScheduler()
{
    InitThread();

    InitSysCache();

    /* Initialize stats collection --- must happen before first xact */
    pgstat_initialize();

    SetProcessExitCallback();

    StartXact();

    SetSuperUserAndDatabase();

    CheckConnPermission();

    SetDatabase();

    LoadSysCache();

    CheckDatabaseAuth();

    InitPGXCPort();

    InitSettings();

    FinishInit();

    AuditUserLogin();
}

void PostgresInitializer::InitJobExecuteWorker()
{
    InitThread();

    InitSysCache();

    /* Initialize stats collection --- must happen before first xact */
    pgstat_initialize();

    SetProcessExitCallback();

    StartXact();

    InitUser();

    CheckConnPermission();

    SetDatabase();

    LoadSysCache();

    CheckDatabaseAuth();

    InitPGXCPort();

    InitSettings();

    FinishInit();

    AuditUserLogin();
}

void PostgresInitializer::InitSnapshotWorker()
{
    InitThread();

    InitSysCache();

    /* Initialize stats collection --- must happen before first xact */
    pgstat_initialize();

    SetProcessExitCallback();

    StartXact();

    SetSuperUserAndDatabase();

    CheckConnPermission();

    SetDatabase();

    LoadSysCache();

    CheckDatabaseAuth();

    InitPGXCPort();

    InitSettings();

    FinishInit();

    AuditUserLogin();
}

void PostgresInitializer::InitPercentileWorker()
{
    InitThread();

    InitSysCache();

    /* Initialize stats collection --- must happen before first xact */
    pgstat_initialize();

    SetProcessExitCallback();

    StartXact();

    SetSuperUserAndDatabase();

    CheckConnPermission();

    SetDatabase();

    LoadSysCache();

    CheckDatabaseAuth();

    InitPGXCPort();

    InitSettings();

    FinishInit();

    AuditUserLogin();
}

void PostgresInitializer::InitAutoVacLauncher()
{
    InitThread();

    InitSysCache();

    /* Initialize stats collection --- must happen before first xact */
    pgstat_initialize();

    SetProcessExitCallback();

    return;
}

void PostgresInitializer::InitAutoVacWorker()
{
    InitThread();

    InitSysCache();

    /* Initialize stats collection --- must happen before first xact */
    pgstat_initialize();

    SetProcessExitCallback();

    StartXact();

    SetSuperUserStandalone();

    CheckConnPermission();

    SetDatabase();

    LoadSysCache();

    CheckDatabaseAuth();

    InitPGXCPort();

    InitSettings();

    FinishInit();

    AuditUserLogin();
}

void PostgresInitializer::InitCatchupWorker()
{
    InitThread();

    InitSysCache();

    /* Initialize stats collection --- must happen before first xact */
    pgstat_initialize();

    SetProcessExitCallback();

    return;
}

void PostgresInitializer::InitBackendWorker()
{
    InitThread();

    pgstat_initialize();

    SetProcessExitCallback();

    if (!IS_THREAD_POOL_WORKER) {
        InitSession();
    } else {
        pgstat_bestart();
        pgstat_report_appname("ThreadPoolWorker");
        pgstat_report_activity(STATE_IDLE, NULL);
    }
}

void PostgresInitializer::InitStreamWorker()
{
    InitThread();

    InitSysCache();

    /* Initialize stats collection --- must happen before first xact */
    pgstat_initialize();

    SetProcessExitCallback();

    StartXact();

    InitUser();

    CheckConnPermission();

    SetDatabase();

    LoadSysCache();

    CheckDatabaseAuth();

    InitPGXCPort();

    InitSettings();

    FinishInit();

    AuditUserLogin();
}

void PostgresInitializer::InitWLM()
{
    InitThread();

    InitSysCache();

    /* Initialize stats collection --- must happen before first xact */
    pgstat_initialize();

    SetProcessExitCallback();

    StartXact();

    SetSuperUserAndDatabase();

    CheckConnPermission();

    SetDatabase();

    LoadSysCache();

    CheckDatabaseAuth();

    InitPGXCPort();

    InitSettings();

    FinishInit();

    AuditUserLogin();
}

void PostgresInitializer::InitWAL()
{
    /* Check replication permissions needed for walsender processes. */
    Assert(!IsBootstrapProcessingMode());

    InitThread();

    InitSysCache();

    /* Initialize stats collection --- must happen before first xact */
    pgstat_initialize();

    SetProcessExitCallback();

    StartXact();

    CheckAuthentication();

    /* Don't set superuser when connection is from gs_basebackup */
    if (u_sess->proc_cxt.clientIsGsBasebackup) {
        InitUser();
    } else {
        SetSuperUserStandalone();
    }

    CheckConnPermission();

    if (!AM_WAL_DB_SENDER) {
        InitPlainWalSender();
        return;
    }

    SetDatabase();

    LoadSysCache();

    CheckDatabaseAuth();

    InitPGXCPort();

    InitSettings();

    FinishInit();

    AuditUserLogin();
}

void PostgresInitializer::GetDatabaseName(char* out_dbname)
{
    /* pass the database name back to the caller */
    if (out_dbname != NULL) {
        errno_t rc = strcpy_s(out_dbname, NAMEDATALEN, m_dbname);
        securec_check_c(rc, "\0", "\0");
    }
}

void PostgresInitializer::InitThread()
{
    ereport(DEBUG3, (errmsg("InitPostgres")));
    /*
     * Add my PGPROC struct to the ProcArray.
     *
     * Once I have done this, I am visible to other backends!
     */
    InitProcessPhase2();

    /*
     * Initialize my entry in the shared-invalidation manager's array of
     * per-backend data.
     *
     * Sets up t_thrd.proc_cxt.MyBackendId, a unique backend identifier.
     */
    t_thrd.proc_cxt.MyBackendId = InvalidBackendId;

    SharedInvalBackendInit(IS_THREAD_POOL_WORKER, false);

    if (t_thrd.proc_cxt.MyBackendId > g_instance.shmem_cxt.MaxBackends || t_thrd.proc_cxt.MyBackendId <= 0) {
        ereport(FATAL, (errmsg("bad backend ID: %d", t_thrd.proc_cxt.MyBackendId)));
    }
    /* Now that we have a BackendId, we can participate in ProcSignal */
    ProcSignalInit(t_thrd.proc_cxt.MyBackendId);

    /*
     * bufmgr needs another initialization call too
     */
    InitBufferPoolBackend();

    /*
     * Initialize local process's access to XLOG.
     */
    if (IsUnderPostmaster) {
        /*
         * The postmaster already started the XLOG machinery, but we need to
         * call InitXLOGAccess(), if the system isn't in hot-standby mode.
         * This is handled by calling RecoveryInProgress and ignoring the
         * result.
         */
        (void)RecoveryInProgress();
    } else {
        /*
         * We are either a bootstrap process or a standalone backend. Either
         * way, start up the XLOG machinery, and register to have it closed
         * down at exit.
         */
        StartupXLOG();
        on_shmem_exit(ShutdownXLOG, 0);
    }
}

void PostgresInitializer::InitSession()
{
    /* Init rel cache for new session. */
    InitSysCache();

    StartXact();

    if (IsUnderPostmaster) {
        CheckAuthentication();
        InitUser();
    } else {
        CheckAtLeastOneRoles();
        SetSuperUserStandalone();
    }

    CheckConnPermission();

    SetDatabase();

    LoadSysCache();

    CheckDatabaseAuth();

    InitPGXCPort();

    InitSettings();

    FinishInit();

    AuditUserLogin();
}

void PostgresInitializer::InitSysCache()
{
    /*
     * Initialize the relation cache and the system catalog caches.  Note that
     * no catalog access happens here; we only set up the hashtable structure.
     * We must do this before starting a transaction because transaction abort
     * would try to touch these hashtables.
     */
    RelationCacheInitialize();
    /*
     * Load relcache entries for the shared system catalogs.  This must create
     * at least entries for pg_database and catalogs used for authentication.
     */
    RelationCacheInitializePhase2();

    PartitionCacheInitialize();

    BucketCacheInitialize();

    InitCatalogCache();
    InitPlanCache();

#ifdef ENABLE_MULTIPLE_NODES
    /* init pgxc caches (local and global) */
    PGXC_Cache_Init();
#endif

    /* Initialize portal manager */
    EnablePortalManager();
}

void PostgresInitializer::SetProcessExitCallback()
{
    /*
     * Set up process-exit callback to do pre-shutdown cleanup.  This has to
     * be after we've initialized all the low-level modules like the buffer
     * manager, because during shutdown this has to run before the low-level
     * modules start to close down.  On the other hand, we want it in place
     * before we begin our first transaction --- if we fail during the
     * initialization transaction, as is entirely possible, we need the
     * AbortTransaction call to clean up.
     */
    on_shmem_exit(ShutdownPostgres, 0);
}

void PostgresInitializer::StartXact()
{
    /*
     * Start a new transaction here before first access to db, and get a
     * snapshot.  We don't have a use for the snapshot itself, but we're
     * interested in the secondary effect that it sets RecentGlobalXmin. (This
     * is critical for anything that reads heap pages, because HOT may decide
     * to prune them even if the process doesn't attempt to modify any
     * tuples.)
     */
    if (!dummyStandbyMode) {
        /* statement_timestamp must be set for timeouts to work correctly */
        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();

        /*
         * transaction_isolation will have been set to the default by the
         * above.  If the default is "serializable", and we are in hot
         * standby, we will fail if we don't change it to something lower.
         * Fortunately, "read committed" is plenty good enough.
         */
        u_sess->utils_cxt.XactIsoLevel = XACT_READ_COMMITTED;

        (void)GetTransactionSnapshot();
    }
}

void PostgresInitializer::CheckAuthentication()
{
    /* for logic conn, we do auth in libcomm, so no auth process anymore */
    if (u_sess->proc_cxt.MyProcPort->is_logic_conn) {
        u_sess->ClientAuthInProgress = false;
    } else {
        PerformAuthentication(u_sess->proc_cxt.MyProcPort);
    }
}

void PostgresInitializer::SetSuperUserStandalone()
{
    InitializeSessionUserIdStandalone();
    m_isSuperUser = true;
}

void PostgresInitializer::CheckAtLeastOneRoles()
{
    if (!ThereIsAtLeastOneRole()) {
        ereport(WARNING,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("no roles are defined in this database system"),
                errhint("You should immediately run CREATE USER \"%s\" sysadmin;.", m_username)));
    }
}

void PostgresInitializer::SetSuperUserAndDatabase()
{
    /*
     * In the wlm worker thread, we set the user is super user
     * and the database is default database, we will send the query
     * to data nodes with the user and the database.
     */
    u_sess->proc_cxt.MyProcPort->database_name = (char*)m_indbname;
    u_sess->proc_cxt.MyProcPort->user_name = (char*)GetSuperUserName((char*)m_username);
    InitUser();
}

void PostgresInitializer::InitUser()
{
    InitializeSessionUserId(m_username);
    m_isSuperUser = superuser();
    u_sess->misc_cxt.CurrentUserName = u_sess->proc_cxt.MyProcPort->user_name;
}

void PostgresInitializer::CheckConnPermission()
{
    CheckConnPermissionInShutDown();
    CheckConnPermissionInBinaryUpgrade();
    CheckConnLimitation();
}

void PostgresInitializer::CheckConnPermissionInShutDown()
{
    /*
     * If we're trying to shut down, only superusers can connect, and new
     * replication connections are not allowed.
     */
    if ((!m_isSuperUser || AM_WAL_SENDER) && u_sess->proc_cxt.MyProcPort != NULL &&
        u_sess->proc_cxt.MyProcPort->canAcceptConnections == CAC_WAITBACKUP) {
        if (AM_WAL_SENDER) {
            ereport(FATAL,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    errmsg("new replication connections are not allowed during database shutdown")));
        } else {
            ereport(FATAL,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    errmsg("must be system admin to connect during database shutdown")));
        }
    }
}

void PostgresInitializer::CheckConnPermissionInBinaryUpgrade()
{
    /*
     * Binary upgrades only allowed super-user connections
     */
    if (u_sess->proc_cxt.IsBinaryUpgrade && !m_isSuperUser) {
        ereport(FATAL,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                errmsg("must be system admin to connect in binary upgrade mode")));
    }
}

void PostgresInitializer::CheckConnLimitation()
{
    /*
     * The last few connections slots are reserved for superusers and CM agent.
     * Although replication connections currently require superuser privileges,
     * we don't allow them to consume the reserved slots, which are intended for
     * interactive use.
     * Inner tools use independent counter.
     */
    if (!u_sess->proc_cxt.IsInnerMaintenanceTools) {
        if (GetUsedConnectionCount() > g_instance.attr.attr_network.MaxConnections ||
            !HaveNFreeProcs(g_instance.attr.attr_network.ReservedBackends)) {
            /* if postgres for am_superuser, allowed to pass */
            if (!m_isSuperUser || AM_WAL_SENDER) {
                int active_count = pgstat_get_current_active_numbackends();
                ereport(FATAL,
                    (errcode(ERRCODE_TOO_MANY_CONNECTIONS),
                        errmsg("Already too many clients, "
                               "active/non-active/reserved: %d/%d/%d.",
                            active_count,
                            GetUsedConnectionCount() - active_count,
                            g_instance.attr.attr_network.ReservedBackends)));
            }
        }
    } else if (GetUsedInnerToolConnCount() > g_instance.attr.attr_network.maxInnerToolConnections) {
        ereport(FATAL, (errcode(ERRCODE_TOO_MANY_CONNECTIONS), errmsg("Already too many tools connected, max num: %d",
            g_instance.attr.attr_network.maxInnerToolConnections)));
    }
}

void PostgresInitializer::InitPlainWalSender()
{
    /*
     * If this is a plain walsender only supporting physical replication, we
     * don't want to connect to any particular database. Just finish the
     * backend startup by processing any options from the startup packet, and
     * we're done.
     */
    /* process any options passed in the startup packet */
    if (u_sess->proc_cxt.MyProcPort != NULL) {
        process_startup_options(u_sess->proc_cxt.MyProcPort, m_isSuperUser);
    }
    /* Apply PostAuthDelay as soon as we've read all options */
    if (u_sess->attr.attr_security.PostAuthDelay > 0) {
        pg_usleep(u_sess->attr.attr_security.PostAuthDelay * 1000000L);
    }
    /* initialize client encoding */
    InitializeClientEncoding();

    /* report this backend in the PgBackendStatus array */
    pgstat_bestart();

    /* close the transaction we started above */
    if (!dummyStandbyMode) {
        CommitTransactionCommand();
    }
}

void PostgresInitializer::SetDefaultDatabase()
{
    u_sess->proc_cxt.MyDatabaseId = TemplateDbOid;
    u_sess->proc_cxt.MyDatabaseTableSpace = DEFAULTTABLESPACE_OID;
    t_thrd.proc->databaseId = u_sess->proc_cxt.MyDatabaseId;

    m_fullpath = GetDatabasePath(u_sess->proc_cxt.MyDatabaseId, u_sess->proc_cxt.MyDatabaseTableSpace);
    /* This should happen only once per process */
    Assert(!u_sess->proc_cxt.DatabasePath);
    u_sess->proc_cxt.DatabasePath = MemoryContextStrdup(u_sess->top_mem_cxt, m_fullpath);
}

void PostgresInitializer::SetDatabase()
{
    /*
     * Set up the global variables holding database id and default tablespace.
     * But note we won't actually try to touch the database just yet.
     *
     * We take a shortcut in the bootstrap case, otherwise we have to look up
     * the db's entry in pg_database.
     */
    if (m_indbname != NULL) {
        SetDatabaseByName();
    } else {
        SetDatabaseByOid();
    }
    LockDatabase();

    RecheckDatabaseExists();

    SetDatabasePath();
}

void PostgresInitializer::SetDatabaseByName()
{
    HeapTuple tuple;
    Form_pg_database db_form;

    tuple = GetDatabaseTuple(m_indbname);

    if (!HeapTupleIsValid(tuple)) {
        /* Database Security: Support database audit */
        errno_t rc = snprintf_s(m_details,
            sizeof(m_details),
            sizeof(m_details) - 1,
            "login db failed,database(%s)does not exist",
            m_indbname);
        securec_check_ss(rc, "\0", "\0");
        pgaudit_user_login(FALSE, (char*)m_indbname, m_details);
        ereport(FATAL, (errcode(ERRCODE_UNDEFINED_DATABASE), errmsg("database \"%s\" does not exist", m_indbname)));
    }
    db_form = (Form_pg_database)GETSTRUCT(tuple);
    u_sess->proc_cxt.MyDatabaseId = HeapTupleGetOid(tuple);
    u_sess->proc_cxt.MyDatabaseTableSpace = db_form->dattablespace;
    /* take database name from the caller, just for paranoia */
    strlcpy(m_dbname, m_indbname, sizeof(m_dbname));
}

void PostgresInitializer::SetDatabaseByOid()
{
    /* caller specified database by OID */
    HeapTuple tuple;
    Form_pg_database db_form;

    tuple = GetDatabaseTupleByOid(m_dboid);

    if (!HeapTupleIsValid(tuple)) {
    // Database Security: Support database audit
    // Audit user login
        snprintf_s(
            m_details, sizeof(m_details), sizeof(m_details) - 1, "login db failed,database(%u)does not exist", m_dboid);
        pgaudit_user_login(FALSE, "unkown", m_details);

        ereport(FATAL, (errcode(ERRCODE_UNDEFINED_DATABASE), errmsg("database %u does not exist", m_dboid)));
    }
    db_form = (Form_pg_database)GETSTRUCT(tuple);
    u_sess->proc_cxt.MyDatabaseId = HeapTupleGetOid(tuple);
    u_sess->proc_cxt.MyDatabaseTableSpace = db_form->dattablespace;
    Assert(u_sess->proc_cxt.MyDatabaseId == m_dboid);
    strlcpy(m_dbname, NameStr(db_form->datname), sizeof(m_dbname));
}

void PostgresInitializer::LockDatabase()
{
    /*
     * Now, take a writer's lock on the database we are trying to connect to.
     * If there is a concurrently running DROP DATABASE on that database, this
     * will block us until it finishes (and has committed its update of
     * pg_database).
     *
     * Note that the lock is not held long, only until the end of this startup
     * transaction.  This is OK since we will advertise our use of the
     * database in the ProcArray before dropping the lock (in fact, that's the
     * next thing to do).  Anyone trying a DROP DATABASE after this point will
     * see us in the array once they have the lock.  Ordering is important for
     * this because we don't want to advertise ourselves as being in this
     * database until we have the lock; otherwise we create what amounts to a
     * deadlock with CountOtherDBBackends().
     *
     * Note: use of RowExclusiveLock here is reasonable because we envision
     * our session as being a concurrent writer of the database.  If we had a
     * way of declaring a session as being guaranteed-read-only, we could use
     * AccessShareLock for such sessions and thereby not conflict against
     * CREATE DATABASE.
     */
    LockSharedObject(DatabaseRelationId, u_sess->proc_cxt.MyDatabaseId, 0, RowExclusiveLock);
    /*
     * Now we can mark our PGPROC entry with the database ID.
     *
     * We assume this is an atomic store so no lock is needed; though actually
     * things would work fine even if it weren't atomic.  Anyone searching the
     * ProcArray for this database's ID should hold the database lock, so they
     * would not be executing concurrently with this store.  A process looking
     * for another database's ID could in theory see a chance match if it read
     * a partially-updated databaseId value; but as long as all such searches
     * wait and retry, as in CountOtherDBBackends(), they will certainly see
     * the correct value on their next try.
     */
    t_thrd.proc->databaseId = u_sess->proc_cxt.MyDatabaseId;
}

void PostgresInitializer::RecheckDatabaseExists()
{
    /*
     * Recheck pg_database to make sure the target database hasn't gone away.
     * If there was a concurrent DROP DATABASE, this ensures we will die
     * cleanly without creating a mess.
     */
    HeapTuple tuple;

    tuple = GetDatabaseTuple(m_dbname);

    if (!HeapTupleIsValid(tuple) || u_sess->proc_cxt.MyDatabaseId != HeapTupleGetOid(tuple) ||
        u_sess->proc_cxt.MyDatabaseTableSpace != ((Form_pg_database)GETSTRUCT(tuple))->dattablespace) {
        // Database Security: Support database audit
        // Audit user login
        errno_t rc = snprintf_s(m_details,
            sizeof(m_details),
            sizeof(m_details) - 1,
            "database \"%s\" does not exist,It seems to have just been dropped or renamed",
            m_dbname);
        securec_check_ss(rc, "\0", "\0");
        pgaudit_user_login(FALSE, m_dbname, m_details);
        ereport(FATAL,
            (errcode(ERRCODE_UNDEFINED_DATABASE),
                errmsg("database \"%s\" does not exist", m_dbname),
                errdetail("It seems to have just been dropped or renamed.")));
    }
}

void PostgresInitializer::SetDatabasePath()
{
    /*
     * Now we should be able to access the database directory safely. Verify
     * it's there and looks reasonable.
     */
    m_fullpath = GetDatabasePath(u_sess->proc_cxt.MyDatabaseId, u_sess->proc_cxt.MyDatabaseTableSpace);

    if (access(m_fullpath, F_OK) == -1) {
        // Database Security: Support database audit
        // Audit login db
        int rcs = snprintf_truncated_s(
            m_details, sizeof(m_details), "Audit messge:login db(%s) failed, database not exists", m_dbname);
        securec_check_ss(rcs, "\0", "\0");

        pgaudit_user_login(FALSE, (char*)m_username, m_details);
        if (errno == ENOENT) {
            ereport(FATAL,
                (errcode(ERRCODE_UNDEFINED_DATABASE),
                    errmsg("database \"%s\" does not exist", m_dbname),
                    errdetail("The database subdirectory \"%s\" is missing.", m_fullpath)));
        } else {
            ereport(FATAL, (errcode_for_file_access(), errmsg("could not access directory \"%s\": %m", m_fullpath)));
        }
    }

    ValidatePgVersion(m_fullpath);
    // This should happen only once per process
    Assert(!u_sess->proc_cxt.DatabasePath);
    u_sess->proc_cxt.DatabasePath = MemoryContextStrdup(u_sess->top_mem_cxt, m_fullpath);
}

void PostgresInitializer::LoadSysCache()
{
    /*
     * It's now possible to do real access to the system catalogs.
     *
     * Load relcache entries for the system catalogs.  This must create at
     * least the minimum set of "nailed-in" cache entries.
     */
    RelationCacheInitializePhase3();

    /* set up ACL framework (so CheckMyDatabase can check permissions) */
    initialize_acl();
}

void PostgresInitializer::ProcessStartupOpt()
{
    /*
     * Now process any command-line switches and any additional GUC variable
     * settings passed in the startup packet.	We couldn't do this before
     * because we didn't know if client is a superuser.
     */
    if (u_sess->proc_cxt.MyProcPort != NULL) {
        process_startup_options(u_sess->proc_cxt.MyProcPort, m_isSuperUser);
    }
}

void PostgresInitializer::CheckDatabaseAuth()
{
    /*
     * Re-read the pg_database row for our database, check permissions and set
     * up database-specific GUC settings.  We can't do this until all the
     * database-access infrastructure is up.  (Also, it wants to know if the
     * user is a superuser, so the above stuff has to happen first.)
     */
    CheckMyDatabase(m_dbname, m_isSuperUser);

    ProcessStartupOpt();

    CheckConnAuthority(m_dbname, m_isSuperUser);
}

void PostgresInitializer::InitPGXCPort()
{
#ifndef ENABLE_MULTIPLE_NODES
    /* don't need to init pgxc port for single node mode */
    return;
#endif

#ifdef PGXC
    /* update pgxc_node info from configfile */
    LWLockAcquire(AlterPortLock, LW_EXCLUSIVE);

    if (!u_sess->attr.attr_common.xc_maintenance_mode && !g_instance.attr.attr_storage.IsRoachStandbyCluster) {
        AlterPgxcNodePort();
    }
    LWLockRelease(AlterPortLock);
#endif
}

void PostgresInitializer::InitSettings()
{
    /* Process pg_db_role_setting options */
    process_settings(u_sess->proc_cxt.MyDatabaseId, GetSessionUserId());

    /* Apply PostAuthDelay as soon as we've read all options */
    if (u_sess->attr.attr_security.PostAuthDelay > 0) {
        pg_usleep(u_sess->attr.attr_security.PostAuthDelay * 1000000L);
    }
    /* set default namespace search path */
    InitializeSearchPath();

    /* initialize client encoding */
    InitializeClientEncoding();
}

void PostgresInitializer::FinishInit()
{
    /* report this backend in the PgBackendStatus array */
    pgstat_bestart();

    /*
     * Create a global hashtable and list used for cluster sql count
     * on processMemoryContext which could be shared among threads.
     * And load all users into the hashtable and list at the same time.
     */
    initSqlCount();

    InitInstrWorkloadTransaction();

    /* close the transaction we started above */
    CommitTransactionCommand();
}

void PostgresInitializer::AuditUserLogin()
{
    if (NULL != m_username) {
        int rc = snprintf_s(m_details,
            sizeof(m_details),
            sizeof(m_details) - 1,
            "login db(%s) success,the current user is:%s",
            m_dbname,
            m_username);
        securec_check_ss(rc, "\0", "\0");

        pgaudit_user_login(TRUE, m_dbname, m_details);
    }
}