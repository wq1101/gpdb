/*-------------------------------------------------------------------------
 *
 * resgroup.c
 *	  GPDB resource group management code.
 *
 *
 * Portions Copyright (c) 2006-2010, Greenplum inc.
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 *
 *
 * IDENTIFICATION
 *	    src/backend/utils/resgroup/resgroup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_resgroup.h"
#include "cdb/cdbgang.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "cdb/memquota.h"
#include "commands/resgroupcmds.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/pg_shmem.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/resgroup-ops.h"
#include "utils/resgroup.h"
#include "utils/resource_manager.h"
#include "utils/resowner.h"
#include "utils/session_state.h"
#include "utils/vmem_tracker.h"

#define InvalidSlotId	(-1)
#define RESGROUP_MAX_SLOTS	300

/*
 * GUC variables.
 */
char                		*gp_resgroup_memory_policy_str = NULL;
ResManagerMemoryPolicy     	gp_resgroup_memory_policy = RESMANAGER_MEMORY_POLICY_NONE;
bool						gp_log_resgroup_memory = false;
int							gp_resgroup_memory_policy_auto_fixed_mem;
bool						gp_resgroup_print_operator_memory_limits = false;
int							memory_spill_ratio=20;

/*
 * Data structures
 */

typedef struct ResGroupHashEntry		ResGroupHashEntry;
typedef struct ResGroupProcData			ResGroupProcData;
typedef struct ResGroupSlotData			ResGroupSlotData;
typedef struct ResGroupData				ResGroupData;
typedef struct ResGroupControl			ResGroupControl;

struct ResGroupHashEntry
{
	Oid		groupId;
	int		index;
};

/*
 * Per proc resource group information.
 *
 * Config snapshot and runtime accounting information in current proc.
 */
struct ResGroupProcData
{
	Oid		groupId;
	int		slotId;

	ResGroupData		*group;

	ResGroupCaps	caps;

	uint32	memUsage;			/* memory usage of current proc */
	bool	doMemCheck;			/* whether to do memory limit check */
};

/*
 * Per slot resource group information.
 *
 * Resource group have 'concurrency' number of slots.
 * Each transaction acquires a slot on master before running.
 * The information shared by QE processes on each segments are stored
 * in this structure.
 */
struct ResGroupSlotData
{
	int				sessionId;

	ResGroupCaps	caps;

	int32			memQuota;	/* memory quota of current slot */
	int32			memUsage;	/* total memory usage of procs belongs to this slot */
	int				nProcs;		/* number of procs in this slot */
	bool			inUse;
};

/*
 * Resource group information.
 */
struct ResGroupData
{
	Oid			groupId;		/* Id for this group */
	ResGroupCaps	caps;
	int			nRunning;		/* number of running trans */
	PROC_QUEUE	waitProcs;
	int			totalExecuted;	/* total number of executed trans */
	int			totalQueued;	/* total number of queued trans	*/
	Interval	totalQueuedTime;/* total queue time */

	bool		lockedForDrop;  /* true if resource group is dropped but not committed yet */

	int32		memExpected;		/* expected memory chunks according to current caps */
	int32		memQuotaGranted;	/* memory chunks for quota part */
	int32		memSharedGranted;	/* memory chunks for shared part */

	int32		memQuotaUsed;		/* memory chunks assigned to all the running slots */

	/*
	 * memory usage of this group, should always equal to the
	 * sum of session memory(session_state->sessionVmem) that
	 * belongs to this group
	 */
	int32		memUsage;
	int32		memSharedUsage;

	ResGroupSlotData slots[RESGROUP_MAX_SLOTS];
};

struct ResGroupControl
{
	HTAB			*htbl;
	int 			segmentsOnMaster;

	/*
	 * The hash table for resource groups in shared memory should only be populated
	 * once, so we add a flag here to implement this requirement.
	 */
	bool			loaded;

	int32			totalChunks;	/* total memory chunks on this segment */
	int32			freeChunks;		/* memory chunks not allocated to any group */

	int				nGroups;
	ResGroupData	groups[1];
};

/* GUC */
int		MaxResourceGroups;

/* static variables */

static ResGroupControl *pResGroupControl = NULL;

static ResGroupProcData __self =
{
	InvalidOid, InvalidSlotId,
};
static ResGroupProcData *self = &__self;

static bool localResWaiting = false;

/* static functions */

static bool groupApplyMemCaps(ResGroupData *group, const ResGroupCaps *caps);
static int32 getChunksFromPool(Oid groupId, int32 chunks);
static void returnChunksToPool(Oid groupId, int32 chunks);
static void groupAssginChunks(ResGroupData *group,
							  int32 chunks,
							  const ResGroupCaps *caps);
static int32 getSegmentChunks(void);
static int32 groupGetMemExpected(const ResGroupCaps *caps);
static int32 groupGetMemQuotaExpected(const ResGroupCaps *caps);
static int32 groupGetMemSharedExpected(const ResGroupCaps *caps);
static int32 groupGetMemSpillTotal(const ResGroupCaps *caps);
static int32 slotGetMemQuotaExpected(const ResGroupCaps *caps);
static int32 slotGetMemSpill(const ResGroupCaps *caps);
static void wakeupSlots(ResGroupData *group);
static void wakeupGroups(Oid skipGroupId);
static bool groupReleaseMemQuota(ResGroupData *group,
								ResGroupSlotData *slot);
static void groupAcquireMemQuota(ResGroupData *group, const ResGroupCaps *caps);
static ResGroupData *ResGroupHashNew(Oid groupId);
static ResGroupData *ResGroupHashFind(Oid groupId);
static bool ResGroupHashRemove(Oid groupId);
static void ResGroupWait(ResGroupData *group);
static ResGroupData *ResGroupCreate(Oid groupId, const ResGroupCaps *caps);
static void AtProcExit_ResGroup(int code, Datum arg);
static void ResGroupWaitCancel(void);
static void groupAssginChunks(ResGroupData *group,
							  int32 chunks,
							  const ResGroupCaps *caps);
static void attachToSlot(ResGroupData *group, ResGroupSlotData *slot);
static void detachFromSlot(ResGroupData *group, ResGroupSlotData *slot);
static int getFreeSlot(ResGroupData *group);
static int getSlot(ResGroupData *group);
static void putSlot(ResGroupData *group, int slotId);
static int ResGroupSlotAcquire(void);
static void addTotalQueueDuration(ResGroupData *group);
static void ResGroupSlotRelease(void);
static void ResGroupSetMemorySpillRatio(const ResGroupCaps *caps);
static char* DumpResGroupMemUsage(ResGroupData *group);

/*
 * Estimate size the resource group structures will need in
 * shared memory.
 */
Size
ResGroupShmemSize(void)
{
	Size		size = 0;

	/* The hash of groups. */
	size = hash_estimate_size(MaxResourceGroups, sizeof(ResGroupHashEntry));

	/* The control structure. */
	size = add_size(size, sizeof(ResGroupControl) - sizeof(ResGroupData));

	/* The control structure. */
	size = add_size(size, mul_size(MaxResourceGroups, sizeof(ResGroupData)));

	/* Add a safety margin */
	size = add_size(size, size / 10);

	return size;
}

/*
 * Initialize the global ResGroupControl struct of resource groups.
 */
void
ResGroupControlInit(void)
{
	int			i;
    bool        found;
    HASHCTL     info;
    int         hash_flags;
	int			size;

	size = sizeof(*pResGroupControl) - sizeof(ResGroupData);
	size += mul_size(MaxResourceGroups, sizeof(ResGroupData));

    pResGroupControl = ShmemInitStruct("global resource group control",
                                       size, &found);
    if (found)
        return;
    if (pResGroupControl == NULL)
        goto error_out;

    /* Set key and entry sizes of hash table */
    MemSet(&info, 0, sizeof(info));
    info.keysize = sizeof(Oid);
    info.entrysize = sizeof(ResGroupHashEntry);
    info.hash = tag_hash;

    hash_flags = (HASH_ELEM | HASH_FUNCTION);

    LOG_RESGROUP_DEBUG(LOG, "Creating hash table for %d resource groups", MaxResourceGroups);

    pResGroupControl->htbl = ShmemInitHash("Resource Group Hash Table",
                                           MaxResourceGroups,
                                           MaxResourceGroups,
                                           &info, hash_flags);

    if (!pResGroupControl->htbl)
        goto error_out;

    /*
     * No need to acquire LWLock here, since this is expected to be called by
     * postmaster only
     */
    pResGroupControl->loaded = false;
    pResGroupControl->nGroups = MaxResourceGroups;
	pResGroupControl->totalChunks = 0;
	pResGroupControl->freeChunks = 0;

	for (i = 0; i < MaxResourceGroups; i++)
		pResGroupControl->groups[i].groupId = InvalidOid;

    return;

error_out:
	ereport(FATAL,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("not enough shared memory for resource group control")));
}

/*
 * Allocate a resource group entry from a hash table
 */
void
AllocResGroupEntry(Oid groupId, const ResGroupOpts *opts)
{
	ResGroupData	*group;
	ResGroupCaps	caps;

	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	ResGroupOptsToCaps(opts, &caps);
	group = ResGroupCreate(groupId, &caps);
	if (!group)
	{
		LWLockRelease(ResGroupLock);

		ereport(PANIC,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				errmsg("not enough shared memory for resource groups")));
	}

	LWLockRelease(ResGroupLock);
}

/*
 * Remove a resource group entry from the hash table
 */
void
FreeResGroupEntry(Oid groupId)
{
	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

#ifdef USE_ASSERT_CHECKING
	bool groupOK = 
#endif
		ResGroupHashRemove(groupId);
	Assert(groupOK);

	LWLockRelease(ResGroupLock);
}

/*
 * Load the resource groups in shared memory. Note this
 * can only be done after enough setup has been done. This uses
 * heap_open etc which in turn requires shared memory to be set up.
 */
void
InitResGroups(void)
{
	HeapTuple	tuple;
	SysScanDesc	sscan;
	int			numGroups;
	CdbComponentDatabases *cdbComponentDBs;
	CdbComponentDatabaseInfo *qdinfo;
	ResGroupCaps		caps;
	Relation			relResGroup;
	Relation			relResGroupCapability;

	/*
	 * On master, the postmaster does the initialization
	 * On segments, the first QE does the initialization
	 */
	if (Gp_role == GP_ROLE_DISPATCH && GpIdentity.segindex != MASTER_CONTENT_ID)
		return;

	on_shmem_exit(AtProcExit_ResGroup, 0);
	if (pResGroupControl->loaded)
		return;
	/*
	 * Need a resource owner to keep the heapam code happy.
	 */
	Assert(CurrentResourceOwner == NULL);
	ResourceOwner owner = ResourceOwnerCreate(NULL, "InitResGroups");
	CurrentResourceOwner = owner;

	if (Gp_role == GP_ROLE_DISPATCH && pResGroupControl->segmentsOnMaster == 0)
	{
		Assert(GpIdentity.segindex == MASTER_CONTENT_ID);
		cdbComponentDBs = getCdbComponentDatabases();
		qdinfo = &cdbComponentDBs->entry_db_info[0];
		pResGroupControl->segmentsOnMaster = qdinfo->hostSegs;
		Assert(pResGroupControl->segmentsOnMaster > 0);
	}

	/*
	 * The resgroup shared mem initialization must be serialized. Only the first session
	 * should do the init.
	 * Serialization is done by LW_EXCLUSIVE ResGroupLock. However, we must obtain all DB
	 * locks before obtaining LWlock to prevent deadlock.
	 */
	relResGroup = heap_open(ResGroupRelationId, AccessShareLock);
	relResGroupCapability = heap_open(ResGroupCapabilityRelationId, AccessShareLock);
	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	if (pResGroupControl->loaded)
		goto exit;

	/* These initialization must be done before ResGroupCreate() */
	pResGroupControl->totalChunks = getSegmentChunks();
	pResGroupControl->freeChunks = pResGroupControl->totalChunks;
	if (pResGroupControl->totalChunks == 0)
		ereport(PANIC,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("insufficient memory available"),
				 errhint("Increase gp_resource_group_memory_limit")));

	ResGroupOps_Init();

	numGroups = 0;
	sscan = systable_beginscan(relResGroup, InvalidOid, false, SnapshotNow, 0, NULL);
	while (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		ResGroupData	*group;
		int cpuRateLimit;
		Oid groupId = HeapTupleGetOid(tuple);

		GetResGroupCapabilities(groupId, &caps);
		cpuRateLimit = caps.cpuRateLimit.value;

		group = ResGroupCreate(groupId, &caps);
		if (!group)
			ereport(PANIC,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("not enough shared memory for resource groups")));

		ResGroupOps_CreateGroup(groupId);
		ResGroupOps_SetCpuRateLimit(groupId, cpuRateLimit);

		numGroups++;
		Assert(numGroups <= MaxResourceGroups);
	}
	systable_endscan(sscan);

	pResGroupControl->loaded = true;
	LOG_RESGROUP_DEBUG(LOG, "initialized %d resource groups", numGroups);

exit:
	LWLockRelease(ResGroupLock);
	heap_close(relResGroup, AccessShareLock);
	heap_close(relResGroupCapability, AccessShareLock);
	CurrentResourceOwner = NULL;
	ResourceOwnerDelete(owner);
}

/*
 * Check resource group status when DROP RESOURCE GROUP
 *
 * Errors out if there're running transactions, otherwise lock the resource group.
 * New transactions will be queued if the resource group is locked.
 */
void
ResGroupCheckForDrop(Oid groupId, char *name)
{
	ResGroupData	*group;

	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	group = ResGroupHashFind(groupId);
	if (group == NULL)
	{
		LWLockRelease(ResGroupLock);

		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Cannot find resource group with Oid %d in shared memory", groupId)));
	}

	if (group->nRunning > 0)
	{
		int nQuery = group->nRunning + group->waitProcs.size;
		LWLockRelease(ResGroupLock);

		Assert(name != NULL);
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("Cannot drop resource group \"%s\"", name),
				 errhint(" The resource group is currently managing %d query(ies) and cannot be dropped.\n"
						 "\tTerminate the queries first or try dropping the group later.\n"
						 "\tThe view pg_stat_activity tracks the queries managed by resource groups.", nQuery)));
	}
	group->lockedForDrop = true;

	LWLockRelease(ResGroupLock);
}

/*
 * Wake up the backends in the wait queue when DROP RESOURCE GROUP finishes.
 * Unlock the resource group if the transaction is aborted.
 * Remove the resource group entry in shared memory if the transaction is committed.
 *
 * This function is called in the callback function of DROP RESOURCE GROUP.
 */
void
ResGroupDropCheckForWakeup(Oid groupId, bool isCommit)
{
	int wakeNum;
	PROC_QUEUE	*waitQueue;
	ResGroupData	*group;

	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	group = ResGroupHashFind(groupId);
	if (group == NULL)
	{
		LWLockRelease(ResGroupLock);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("Cannot find resource group %d in shared memory", groupId)));
	}

	Assert(group->lockedForDrop);

	waitQueue = &(group->waitProcs);
	wakeNum = waitQueue->size;

	while (wakeNum > 0)
	{
		PGPROC *waitProc;

		/* wake up one process in the wait queue */
		waitProc = (PGPROC *) MAKE_PTR(waitQueue->links.next);
		SHMQueueDelete(&(waitProc->links));
		waitQueue->size--;

		waitProc->resWaiting = false;
		waitProc->resSlotId = InvalidSlotId;
		SetLatch(&waitProc->procLatch);
		wakeNum--;
	}

	if (isCommit)
	{
#ifdef USE_ASSERT_CHECKING
		bool groupOK = 
#endif
			ResGroupHashRemove(groupId);
		Assert(groupOK);
	}

	group->lockedForDrop = false;

	LWLockRelease(ResGroupLock);
}

/*
 * Apply the new resgroup caps.
 */
void
ResGroupAlterOnCommit(Oid groupId,
					  ResGroupLimitType limittype,
					  const ResGroupCaps *caps)
{
	ResGroupData	*group;
	bool			shouldWakeUp;

	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	group = ResGroupHashFind(groupId);
	if (group == NULL)
	{
		LWLockRelease(ResGroupLock);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("Cannot find resource group %d in shared memory", groupId)));
	}

	group->caps = *caps;

	if (limittype == RESGROUP_LIMIT_TYPE_CPU)
	{
		volatile int savedInterruptHoldoffCount;

		PG_TRY();
		{
			savedInterruptHoldoffCount = InterruptHoldoffCount;
			ResGroupOps_SetCpuRateLimit(groupId, caps->cpuRateLimit.proposed);
		}
		PG_CATCH();
		{
			InterruptHoldoffCount = savedInterruptHoldoffCount;
			elog(LOG, "Fail to set cpu_rate_limit for resource group %d", groupId);
		}
		PG_END_TRY();

		LWLockRelease(ResGroupLock);
		return;
	}

	shouldWakeUp = groupApplyMemCaps(group, caps);

	wakeupSlots(group);
	if (shouldWakeUp)
		wakeupGroups(groupId);

	LWLockRelease(ResGroupLock);
}

/*
 *  Retrieve statistic information of type from resource group
 */
Datum
ResGroupGetStat(Oid groupId, ResGroupStatType type)
{
	ResGroupData	*group;
	Datum result;

	Assert(IsResGroupActivated());

	LWLockAcquire(ResGroupLock, LW_SHARED);

	group = ResGroupHashFind(groupId);
	if (group == NULL)
	{
		LWLockRelease(ResGroupLock);

		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Cannot find resource group with Oid %d in shared memory", groupId)));
	}

	switch (type)
	{
		case RES_GROUP_STAT_NRUNNING:
			result = Int32GetDatum(group->nRunning);
			break;
		case RES_GROUP_STAT_NQUEUEING:
			result = Int32GetDatum(group->waitProcs.size);
			break;
		case RES_GROUP_STAT_TOTAL_EXECUTED:
			result = Int32GetDatum(group->totalExecuted);
			break;
		case RES_GROUP_STAT_TOTAL_QUEUED:
			result = Int32GetDatum(group->totalQueued);
			break;
		case RES_GROUP_STAT_TOTAL_QUEUE_TIME:
			result = IntervalPGetDatum(&group->totalQueuedTime);
			break;
		case RES_GROUP_STAT_MEM_USAGE:
			result = CStringGetDatum(DumpResGroupMemUsage(group));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Invalid stat type %d", type)));
	}

	LWLockRelease(ResGroupLock);

	return result;
}

static char*
DumpResGroupMemUsage(ResGroupData *group)
{
	int32 slotUsage;
	StringInfoData memUsage;

	if (Gp_role == GP_ROLE_DISPATCH)
		slotUsage = group->memQuotaUsed;
	else
		/* slotUsage has no meaning in QE */
		slotUsage = -1;	

	initStringInfo(&memUsage);

	appendStringInfo(&memUsage, "{");
	appendStringInfo(&memUsage, "\"used\":%d, ",
					 group->memUsage);
	appendStringInfo(&memUsage, "\"available\":%d, ",
					 group->memQuotaGranted + group->memSharedGranted - group->memUsage);
	appendStringInfo(&memUsage, "\"quota_used\":%d, ",
					 slotUsage);
	appendStringInfo(&memUsage, "\"quota_available\":%d, ",
					 group->memQuotaGranted - group->memQuotaUsed);
	appendStringInfo(&memUsage, "\"quota_granted\":%d, ",
					 group->memQuotaGranted);
	appendStringInfo(&memUsage, "\"quota_proposed\":%d, ",
					 groupGetMemQuotaExpected(&group->caps));
	appendStringInfo(&memUsage, "\"shared_used\":%d, ",
					 group->memSharedUsage);
	appendStringInfo(&memUsage, "\"shared_available\":%d, ",
					 group->memSharedGranted - group->memSharedUsage);
	appendStringInfo(&memUsage, "\"shared_granted\":%d, ",
					 group->memSharedGranted);
	appendStringInfo(&memUsage, "\"shared_proposed\":%d",
					 groupGetMemSharedExpected(&group->caps));
	appendStringInfo(&memUsage, "}");

	return memUsage.data;
}

/*
 * Dump memory information for current resource group.
 */
void
ResGroupDumpMemoryInfo(void)
{
	ResGroupSlotData	*slot;
	ResGroupData		*group = self->group;

	if (group)
	{
		Assert(self->slotId != InvalidSlotId);

		slot = &group->slots[self->slotId];

		write_log("Resource group memory information: "
				  "group memory limit is %d MB, "
				  "shared quota in current resource group is %d MB, "
				  "memory usage in current resource group is %d MB, "
				  "memory quota in current slot is %d MB, "
				  "memory usage in current slot is %d MB, "
				  "memory usage in current proc is %d MB",
				  VmemTracker_ConvertVmemChunksToMB(group->memExpected),
				  VmemTracker_ConvertVmemChunksToMB(group->memSharedGranted),
				  VmemTracker_ConvertVmemChunksToMB(group->memUsage),
				  VmemTracker_ConvertVmemChunksToMB(slot->memQuota),
				  VmemTracker_ConvertVmemChunksToMB(slot->memUsage),
				  VmemTracker_ConvertVmemChunksToMB(self->memUsage));
	}
	else
	{
		write_log("Resource group memory information: "
				  "memory usage in current proc is %d MB",
				  VmemTracker_ConvertVmemChunksToMB(self->memUsage));
	}
}

/*
 * Reserve 'memoryChunks' number of chunks for current resource group.
 * It will first try to reserve memory from the resource group slot; if the slot
 * quota exceeded, it will reserve memory from the shared zone. It fails if the
 * shared quota is also exceeded, and no memory is reserved.
 *
 * 'overuseChunks' number of chunks can be overused for error handling,
 * in such a case waiverUsed is marked as true.
 */
bool
ResGroupReserveMemory(int32 memoryChunks, int32 overuseChunks, bool *waiverUsed)
{
	int32				slotMemUsage;
	int32				slotMemSharedNeeded;
	ResGroupSlotData	*slot;
	ResGroupData		*group = self->group;

	/*
	 * Memories may be allocated before resource group is initialized,
	 * however,we need to track those memories once resource group is
	 * enabled, so we use IsResGroupEnabled() instead of
	 * IsResGroupActivated() here.
	 */
	if (!IsResGroupEnabled())
		return true;

	Assert(memoryChunks >= 0);

	/*
	 * Bypass the limit check when we are not in a valid resource group.
	 * But will update the memory usage of this proc, and it will be added up
	 * when this proc is assigned to a valid resource group.
	 */
	self->memUsage += memoryChunks;
	if (!self->doMemCheck)
		return true;

	if (group->groupId != self->groupId)
	{
		if (Debug_resource_group)
			write_log("Resource group is concurrently dropped while reserving memory: "
					  "dropped group=%d, my group=%d",
					  group->groupId, self->groupId);
		self->group = NULL;
		self->doMemCheck = false;
		return true;
	}

	Assert(group != NULL);
	Assert(group->groupId != InvalidOid);
	Assert(self->slotId != InvalidSlotId);
	Assert(group->memUsage >= 0);
	Assert(self->memUsage >= 0);

	slot = &group->slots[self->slotId];

	/* reserve from slot memory */
	slotMemUsage = pg_atomic_add_fetch_u32((pg_atomic_uint32 *)&slot->memUsage, memoryChunks);
	slotMemSharedNeeded = slotMemUsage - slot->memQuota;

	if (slotMemSharedNeeded > 0)
	{
		int32 total;

		slotMemSharedNeeded = Min(slotMemSharedNeeded, memoryChunks);

		/* reserve from shared zone */
		total = pg_atomic_add_fetch_u32((pg_atomic_uint32 *)&group->memSharedUsage,
										slotMemSharedNeeded);

		if (CritSectionCount == 0 &&
			total > group->memSharedGranted + overuseChunks)
		{
			int32		oldUsage;

			oldUsage = pg_atomic_fetch_sub_u32((pg_atomic_uint32 *)&group->memSharedUsage,
										slotMemSharedNeeded);
			Assert(oldUsage >= slotMemSharedNeeded);

			oldUsage = pg_atomic_fetch_sub_u32((pg_atomic_uint32 *)&slot->memUsage,
										memoryChunks);
			Assert(oldUsage >= memoryChunks);

			Assert(self->memUsage >= memoryChunks);
			self->memUsage -= memoryChunks;

			if (overuseChunks == 0)
				ResGroupDumpMemoryInfo();

			return false;
		}
		else if (CritSectionCount == 0 && total > group->memSharedGranted)
		{
			*waiverUsed = true;
		}
	}

	/* update memory usage of current resource group */
	pg_atomic_add_fetch_u32((pg_atomic_uint32 *)&group->memUsage,
							memoryChunks);

	return true;
}

/*
 * Release the memory of resource group
 */
void
ResGroupReleaseMemory(int32 memoryChunks)
{
	int32				sharedMemoryUsage;
	ResGroupSlotData	*slot;
	ResGroupData		*group = self->group;
	int32				oldUsage;

	if (!IsResGroupActivated())
		return;

	Assert(memoryChunks >= 0);
	Assert(memoryChunks <= self->memUsage);

	self->memUsage -= memoryChunks;
	if (!self->doMemCheck)
		return;

	if (group->groupId != self->groupId)
	{
		if (Debug_resource_group)
			write_log("Resource group is concurrently dropped while releasing memory: "
					  "dropped group=%d, my group=%d",
					  group->groupId, self->groupId);
		self->group = NULL;
		self->doMemCheck = false;
		return;
	}

	Assert(group != NULL);
	Assert(group->groupId != InvalidOid);
	Assert(self->slotId != InvalidSlotId);

	slot = &group->slots[self->slotId];
	sharedMemoryUsage = slot->memUsage - slot->memQuota;
	if (sharedMemoryUsage > 0)
	{
		int32 returnSize = Min(memoryChunks, sharedMemoryUsage);

		oldUsage = pg_atomic_fetch_sub_u32((pg_atomic_uint32 *)&group->memSharedUsage,
										   returnSize);
		Assert(oldUsage >= returnSize);
	}

	oldUsage = pg_atomic_fetch_sub_u32((pg_atomic_uint32 *)&slot->memUsage, memoryChunks);
	Assert(oldUsage >= memoryChunks);

	oldUsage = pg_atomic_fetch_sub_u32((pg_atomic_uint32 *)&group->memUsage,
								memoryChunks);
	Assert(oldUsage >= memoryChunks);
}

/*
 * Decide the new resource group concurrency capabilities
 * of pg_resgroupcapability.
 *
 * The decision is based on current runtime information:
 * - 'proposed' will always be set to the latest setting;
 * - 'value' will be set to the most recent version of concurrency
 *   with which current nRunning doesn't exceed the limit;
 */
void
ResGroupDecideConcurrencyCaps(Oid groupId,
							  ResGroupCaps *caps,
							  const ResGroupOpts *opts)
{
	ResGroupData	*group;

	/* If resource group is not in use we can always pick the new settings. */
	if (!IsResGroupActivated())
	{
		caps->concurrency.value = opts->concurrency;
		caps->concurrency.proposed = opts->concurrency;
		return;
	}

	LWLockAcquire(ResGroupLock, LW_SHARED);

	group = ResGroupHashFind(groupId);
	if (group == NULL)
	{
		LWLockRelease(ResGroupLock);

		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Cannot find resource group with Oid %d in shared memory", groupId)));
	}

	/*
	 * If the runtime usage information doesn't exceed the new setting
	 * then we can pick this setting as the new 'value'.
	 */
	if (group->nRunning <= opts->concurrency)
		caps->concurrency.value = opts->concurrency;

	/* 'proposed' is always set with latest setting */
	caps->concurrency.proposed = opts->concurrency;

	LWLockRelease(ResGroupLock);
}

/*
 * Decide the new resource group memory capabilities
 * of pg_resgroupcapability.
 *
 * The decision is based on current runtime information:
 * - 'proposed' will always be set to the latest setting;
 * - 'value' will be set to the most recent version of memory settings
 *   with which current memory quota usage and memory shared usage
 *   doesn't exceed the limit;
 */
void
ResGroupDecideMemoryCaps(int groupId,
						 ResGroupCaps *caps,
						 const ResGroupOpts *opts)
{
	ResGroupData	*group;
	ResGroupCaps	capsNew;

	/* If resource group is not in use we can always pick the new settings. */
	if (!IsResGroupActivated())
	{
		caps->memLimit.value = opts->memLimit;
		caps->memLimit.proposed = opts->memLimit;

		caps->memSharedQuota.value = opts->memSharedQuota;
		caps->memSharedQuota.proposed = opts->memSharedQuota;

		return;
	}

	LWLockAcquire(ResGroupLock, LW_SHARED);

	group = ResGroupHashFind(groupId);
	if (group == NULL)
	{
		LWLockRelease(ResGroupLock);

		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Cannot find resource group with Oid %d in shared memory",
						groupId)));
	}

	ResGroupOptsToCaps(opts, &capsNew);
	/*
	 * If the runtime usage information doesn't exceed the new settings
	 * then we can pick these settings as the new 'value's.
	 */
	if (opts->memLimit <= caps->memLimit.proposed &&
		group->memQuotaUsed <= groupGetMemQuotaExpected(&capsNew) &&
		group->memSharedUsage <= groupGetMemSharedExpected(&capsNew))
	{
		caps->memLimit.value = opts->memLimit;
		caps->memSharedQuota.value = opts->memSharedQuota;
	}

	/* 'proposed' is always set with latest setting */
	caps->memSharedQuota.proposed = opts->memSharedQuota;
	caps->memLimit.proposed = opts->memLimit;

	LWLockRelease(ResGroupLock);
}

int64
ResourceGroupGetQueryMemoryLimit(void)
{
	ResGroupSlotData	*slot;
	int64				memSpill;

	Assert(self->group != NULL);
	Assert(self->slotId != InvalidSlotId);

	slot = &self->group->slots[self->slotId];

	if (IsResManagerMemoryPolicyNone())
		return 0;

	memSpill = slotGetMemSpill(&slot->caps);

	return memSpill << VmemTracker_GetChunkSizeInBits();
}

/*
 * ResGroupCreate -- initialize the elements for a resource group.
 *
 * Notes:
 *	It is expected that the appropriate lightweight lock is held before
 *	calling this - unless we are the startup process.
 */
static ResGroupData *
ResGroupCreate(Oid groupId, const ResGroupCaps *caps)
{
	ResGroupData	*group;
	int32			chunks;

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));
	Assert(OidIsValid(groupId));

	group = ResGroupHashNew(groupId);
	if (group == NULL)
		return NULL;

	group->groupId = groupId;
	group->caps = *caps;
	group->nRunning = 0;
	ProcQueueInit(&group->waitProcs);
	group->totalExecuted = 0;
	group->totalQueued = 0;
	group->memUsage = 0;
	group->memSharedUsage = 0;
	group->memQuotaUsed = 0;
	memset(&group->totalQueuedTime, 0, sizeof(group->totalQueuedTime));
	group->lockedForDrop = false;
	memset(group->slots, 0, sizeof(group->slots));

	group->memQuotaGranted = 0;
	group->memSharedGranted = 0;
	group->memExpected = groupGetMemExpected(caps);

	chunks = getChunksFromPool(groupId, group->memExpected);
	groupAssginChunks(group, chunks, caps);

	return group;
}

/*
 * Attach current proc to a resource group & slot.
 *
 * Current proc's memory usage will be added to the group & slot.
 */
static void
attachToSlot(ResGroupData *group, ResGroupSlotData *slot)
{
	int32			slotMemUsage;
	int32			sharedMemUsage;

	pg_atomic_add_fetch_u32((pg_atomic_uint32 *) &slot->nProcs, 1);

	/* Add proc memory accounting info to memUsage in slot */
	slotMemUsage = pg_atomic_add_fetch_u32((pg_atomic_uint32 *) &slot->memUsage,
										   self->memUsage);

	/* Add proc memory accounting info to memSharedUsage in group */
	sharedMemUsage = slotMemUsage - slot->memQuota;
	if (sharedMemUsage > 0)
	{
		/* Decide how many shared memory is in use by proc */
		sharedMemUsage = Min(sharedMemUsage, self->memUsage);
		pg_atomic_add_fetch_u32((pg_atomic_uint32 *) &group->memSharedUsage,
								sharedMemUsage);
	}

	/* Add proc memory accounting info to memUsage in group */
	pg_atomic_add_fetch_u32((pg_atomic_uint32 *) &group->memUsage,
							self->memUsage);
}

/*
 * Detach current proc from a resource group & slot.
 *
 * Current proc's memory usage will be subtracted from the group & slot.
 */
static void
detachFromSlot(ResGroupData *group, ResGroupSlotData *slot)
{
	int32			value;
	int32			slotMemUsage;
	int32			sharedMemUsage;

	/* Sub proc memory accounting info from memUsage in group */
	value = pg_atomic_sub_fetch_u32((pg_atomic_uint32 *) &group->memUsage,
									self->memUsage);
	Assert(value >= 0);

	/* Sub proc memory accounting info from memUsage in slot */
	slotMemUsage = pg_atomic_fetch_sub_u32((pg_atomic_uint32 *) &slot->memUsage,
										   self->memUsage);
	Assert(slotMemUsage >= self->memUsage);

	/* Sub proc memory accounting info from memSharedUsage in group */
	sharedMemUsage = slotMemUsage - slot->memQuota;
	if (sharedMemUsage > 0)
	{
		/* Decide how many shared memory is in use by proc */
		int32 returnSize = Min(self->memUsage, sharedMemUsage);

		value = pg_atomic_sub_fetch_u32((pg_atomic_uint32 *) &group->memSharedUsage,
										returnSize);
		Assert(value >= 0);
	}

	value = pg_atomic_sub_fetch_u32((pg_atomic_uint32*)&slot->nProcs, 1);
	Assert(value >= 0);
}

/*
 * Get a free resource group slot.
 *
 * A free resource group slot has inUse == false, no other information is checked.
 */
static int
getFreeSlot(ResGroupData *group)
{
	int i;

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));

	for (i = 0; i < RESGROUP_MAX_SLOTS; i++)
	{
		if (group->slots[i].inUse)
			continue;

		group->slots[i].inUse = true;
		return i;
	}

	Assert(false && "No free slot available");
	return InvalidSlotId;
}

/*
 * Get a slot with memory quota granted.
 *
 * A slot can be got with this function if there is enough memory quota
 * available and the concurrency limit is not reached.
 *
 * On success the memory quota is marked as granted, nRunning is increased
 * and the slot's inUse flag is also set, the slot id is returned.
 *
 * On failure nothing is changed and InvalidSlotId is returned.
 */
static int
getSlot(ResGroupData *group)
{
	ResGroupSlotData	*slot;
	int32				slotMemQuota;
	int32				memQuotaUsed;
	int					slotId;
	ResGroupCaps		*caps;

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));
	Assert(Gp_role == GP_ROLE_DISPATCH);

	Assert(group != NULL);
	Assert(group->groupId != InvalidOid);

	caps = &group->caps;

	/* First check if the concurrency limit is reached */
	if (group->nRunning >= caps->concurrency.proposed)
		return InvalidSlotId;

	groupAcquireMemQuota(group, caps);

	/* Then check for memory stocks */
	Assert(pResGroupControl->segmentsOnMaster > 0);

	/* Calculate the expected per slot quota */
	slotMemQuota = slotGetMemQuotaExpected(caps);

	Assert(slotMemQuota > 0);
	Assert(group->memQuotaUsed >= 0);
	Assert(group->memQuotaUsed <= group->memQuotaGranted);

	memQuotaUsed = pg_atomic_add_fetch_u32((pg_atomic_uint32*) &group->memQuotaUsed,
										   slotMemQuota);

	if (memQuotaUsed > group->memQuotaGranted)
	{
		/* No enough memory quota available, give up */
		memQuotaUsed = pg_atomic_sub_fetch_u32((pg_atomic_uint32*)&group->memQuotaUsed,
											   slotMemQuota);
		Assert(memQuotaUsed >= 0);
		return InvalidSlotId;
	}

	/* Now actually get a free slot */
	slotId = getFreeSlot(group);
	Assert(slotId != InvalidSlotId);

	slot = &group->slots[slotId];
	Assert(slot->inUse);

	/* Grant the memory quota to it */
	slot->memQuota = slotMemQuota;

	/* Store the config snapshot to it */
	slot->caps = *caps;

	/* And finally increase nRunning */
	pg_atomic_add_fetch_u32((pg_atomic_uint32*)&group->nRunning, 1);

	return slotId;
}

/*
 * Put back a slot.
 *
 * This will release a slot, its memory quota will be freed and
 * nRunning will be decreased.
 */
static void
putSlot(ResGroupData *group, int slotId)
{
	ResGroupSlotData	*slot;
	bool				shouldWakeUp;
#ifdef USE_ASSERT_CHECKING
	int32				memQuotaUsed;
#endif

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));
	Assert(Gp_role == GP_ROLE_DISPATCH);
	Assert(group != NULL);
	Assert(group->memQuotaUsed >= 0);
	Assert(group->nRunning > 0);
	Assert(slotId != InvalidSlotId);

	slot = &group->slots[slotId];

	Assert(slot->inUse);
	Assert(slot->memQuota > 0);

	/* Return the memory quota granted to this slot */
#ifdef USE_ASSERT_CHECKING
	memQuotaUsed =
#endif
		pg_atomic_sub_fetch_u32((pg_atomic_uint32*)&group->memQuotaUsed,
								slot->memQuota);
	Assert(memQuotaUsed >= 0);

	shouldWakeUp = groupReleaseMemQuota(group, slot);
	if (shouldWakeUp)
		wakeupGroups(group->groupId);

	/* Mark the slot as free */
	slot->inUse = false;

	/* And finally decrease nRunning */
	pg_atomic_sub_fetch_u32((pg_atomic_uint32*)&group->nRunning, 1);
}

/*
 * Acquire a resource group slot
 *
 * Call this function at the start of the transaction.
 * This function set current resource group in MyResGroupSharedInfo,
 * and current slot id in MyProc->resSlotId.
 */
static int
ResGroupSlotAcquire(void)
{
	ResGroupData	*group;
	Oid				 groupId;
	PROC_QUEUE		*waitQueue;
	PGPROC			*headProc;
	int				slotId;

	Assert(self->groupId == InvalidOid);
	Assert(self->group == NULL);

retry:
	self->group = NULL;
	/* always find out the up-to-date resgroup id */
	groupId = GetResGroupIdForRole(GetUserId());
	if (groupId == InvalidOid)
		groupId = superuser() ? ADMINRESGROUP_OID : DEFAULTRESGROUP_OID;

	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);
	group = ResGroupHashFind(groupId);
	if (group == NULL)
	{
		LWLockRelease(ResGroupLock);
		self->groupId = InvalidOid;
		self->group = NULL;

		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Cannot find resource group %d in shared memory", groupId)));
	}
	/*
	 * it's neccessary to set MyResGroupSharedInfo before we
	 * got signal and goes to ResGroupWaitCancel
	 */
	self->group = group;

	/* acquire a slot */
	if (!group->lockedForDrop)
	{
		slotId = getSlot(group);
		if (slotId != InvalidSlotId)
		{
			group->totalExecuted++;
			pgstat_report_resgroup(0, group->groupId);
			MyProc->resSlotId = slotId;
			LWLockRelease(ResGroupLock);
			return slotId;
		}
	}

	/* add into group wait queue */
	waitQueue = &(group->waitProcs);
	headProc = (PGPROC *) &(waitQueue->links);
	SHMQueueInsertBefore(&(headProc->links), &(MyProc->links));
	waitQueue->size++;

	MyProc->resWaiting = true;
	if (!group->lockedForDrop)
		group->totalQueued++;
	LWLockRelease(ResGroupLock);

	/*
	 * wait on the queue
	 * slot will be assigned by the proc wakes me up
	 * if i am waken up by DROP RESOURCE GROUP statement, the
	 * resSlotId will be InvalidSlotId.
	 */
	ResGroupWait(group);

	if (MyProc->resSlotId == InvalidSlotId)
		goto retry;

	/*
	 * The waking process has granted us a valid slot.
	 * Update the statistic information of the resource group.
	 */
	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);
	addTotalQueueDuration(group);
	group->totalExecuted++;
	LWLockRelease(ResGroupLock);

	pgstat_report_resgroup(0, group->groupId);
	return MyProc->resSlotId;
}

/* Update the total queued time of this group */
/*
 * Wake up the backends in the wait queue when 'concurrency' is increased.
 * This function is called in the callback function of ALTER RESOURCE GROUP.
 */
/*
 * XXX
 *
 * Suppose concurrency is 10, running is 4,
 * memory limit is 0.5, memory shared is 0.4
 *
 * assume currentSharedUsage is 0
 *
 * currentSharedStocks is 0.5*0.4 = 0.2
 * memQuotaGranted is 0.5*0.6 = 0.3
 * memStocksInuse is 0.5*0.4/10*6 = 0.12
 * memStocksFree is 0.3 - 0.12 = 0.18
 *
 * * memLimit: 0.5 -> 0.4
 *   for memQuotaGranted we could free 0.18 - 0.4*0.6/10*6 = 0.18-0.144 = 0.036
 *       new memQuotaGranted is 0.3-0.036 = 0.264
 *       new memStocksFree is 0.18-0.036 = 0.144
 *   for memShared we could free currentSharedStocks - Max(currentSharedUsage, 0.4*0.4)=0.04
 *       new currentSharedStocks is 0.2-0.04 = 0.16
 *
 * * concurrency: 10 -> 20
 *   for memQuotaGranted we could free 0.144 - 0.4*0.6/20*16 = 0.144 - 0.24*0.8 = -0.048
 *   for memShared we could free currentSharedStocks - Max(currentSharedUsage, 0.4*0.4)=0.00
 *
 * * memShared: 0.4 -> 0.2
 *   for memQuotaGranted we could free 0.144 - 0.4*0.8/20*16 = 0.144 - 0.256 = -0.122
 *   for memShared we could free currentSharedUsage - Max(currentSharedUsage, 0.4*0.2)=0.08
 *       new currentSharedStocks is 0.16-0.08 = 0.08
 *
 * * memShared: 0.2 -> 0.6
 *   for memQuotaGranted we could free 0.144 - 0.4*0.4/20*16 = 0.144 - 0.128 = 0.016
 *       new memQuotaGranted is 0.264 - 0.016 = 0.248
 *       new memStocksFree is 0.144 - 0.016 = 0.128
 *   for memShared we could free currentSharedUsage - Max(currentSharedUsage, 0.4*0.6) = -0.18
 *
 * * memLimit: 0.4 -> 0.2
 *   for memQuotaGranted we could free 0.128 - 0.2*0.4/20*16 = 0.128 - 0.064 = 0.064
 *       new memQuotaGranted is 0.248-0.064 = 0.184
 *       new memStocksFree is 0.128 - 0.064 = 0.064
 *   for memShared we could free currentSharedStocks - Max(currentSharedUsage, 0.2*0.6) = -0.04
 */
static bool
groupApplyMemCaps(ResGroupData *group, const ResGroupCaps *caps)
{
	int32 memStocksAvailable;
	int32 memStocksNeeded;
	int32 memStocksToFree;
	int32 memSharedNeeded;
	int32 memSharedToFree;

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));

	group->memExpected = groupGetMemExpected(caps);

	/* memStocksAvailable is the total free non-shared quota */
	memStocksAvailable = group->memQuotaGranted - group->memQuotaUsed;

	if (caps->concurrency.proposed > group->nRunning)
	{
		/*
		 * memStocksNeeded is the total non-shared quota needed
		 * by all the free slots
		 */
		memStocksNeeded = slotGetMemQuotaExpected(caps) *
			(caps->concurrency.proposed - group->nRunning);

		/*
		 * if memStocksToFree > 0 then we can safely release these
		 * non-shared quota and still have enough quota to run
		 * all the free slots.
		 */
		memStocksToFree = memStocksAvailable - memStocksNeeded;
	}
	else
	{
		memStocksToFree = Min(memStocksAvailable,
							  group->memQuotaGranted - groupGetMemQuotaExpected(caps));
	}

	/* TODO: optimize the free logic */
	if (memStocksToFree > 0)
	{
		returnChunksToPool(group->groupId, memStocksToFree);
		group->memQuotaGranted -= memStocksToFree;
	}

	memSharedNeeded = Max(group->memSharedUsage,
						  groupGetMemSharedExpected(caps));
	memSharedToFree = group->memSharedGranted - memSharedNeeded;

	if (memSharedToFree > 0)
	{
		returnChunksToPool(group->groupId, memSharedToFree);
		group->memSharedGranted -= memSharedToFree;
	}
#if 1
	/*
	 * FIXME: why we need this?
	 *
	 * suppose rg1 has memory_limit=10, memory_shared_quota=40,
	 * and session1 is running in rg1.
	 *
	 * now we alter rg1 memory_limit to 40 in another session,
	 * apparently both memory quota and shared quota are expected to increase,
	 * our as our design is to let them increase on new queries,
	 * then for session1 it won't see memory shared quota being increased
	 * until new queries being executed in rg1.
	 */
	groupAcquireMemQuota(group, caps);
#endif
	return (memStocksToFree > 0 || memSharedToFree > 0);
}

/*
 * Get quota from sys pool.
 *
 * chunks is the expected amount to get.
 *
 * return the actual got chunks, might be smaller than expectation.
 */
static int32
getChunksFromPool(Oid groupId, int32 chunks)
{
	Assert(LWLockHeldExclusiveByMe(ResGroupLock));

	LOG_RESGROUP_DEBUG(LOG, "Allocate %u out of %u chunks to group %d",
					   chunks, pResGroupControl->freeChunks, groupId);

	chunks = Min(pResGroupControl->freeChunks, chunks);
	pResGroupControl->freeChunks -= chunks;

	Assert(pResGroupControl->freeChunks >= 0);
	Assert(pResGroupControl->freeChunks <= pResGroupControl->totalChunks);

	return chunks;
}

/*
 * Return chunks to sys pool.
 */
static void
returnChunksToPool(Oid groupId, int32 chunks)
{
	LOG_RESGROUP_DEBUG(LOG, "Free %u to pool(%u) chunks from group %d",
					   chunks, pResGroupControl->freeChunks, groupId);

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));
	Assert(chunks > 0);

	pResGroupControl->freeChunks += chunks;

	Assert(pResGroupControl->freeChunks >= 0);
	Assert(pResGroupControl->freeChunks <= pResGroupControl->totalChunks);
}

/*
 * Assign the chunks we get from the sys pool to the 'quota' and 'shared'
 * part of the group, the amount is calculated from caps.
 */
static void
groupAssginChunks(ResGroupData *group, int32 chunks, const ResGroupCaps *caps)
{
	int32 delta;
	int32 memQuotaGranted = groupGetMemQuotaExpected(caps);

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));

	delta = memQuotaGranted - group->memQuotaGranted;
	if (delta >= 0)
	{
		delta = Min(chunks, delta);

		group->memQuotaGranted += delta;
		chunks -= delta;
	}

	group->memSharedGranted += chunks;
}

/*
 * Calculate the total memory chunks of the segment
 */
static int32
getSegmentChunks(void)
{
	int nsegments = Gp_role == GP_ROLE_EXECUTE ? host_segments : pResGroupControl->segmentsOnMaster;

	Assert(nsegments > 0);

	return ResGroupOps_GetTotalMemory() * gp_resource_group_memory_limit / nsegments;
}

/*
 * Get total expected memory quota of a group in chunks
 */
static int32
groupGetMemExpected(const ResGroupCaps *caps)
{
	Assert(pResGroupControl->totalChunks > 0);
	return pResGroupControl->totalChunks * caps->memLimit.proposed / 100;
}

/*
 * Get per-group expected memory quota in chunks
 */
static int32
groupGetMemQuotaExpected(const ResGroupCaps *caps)
{
	return groupGetMemExpected(caps) *
		(100 - caps->memSharedQuota.proposed) / 100;
}

/*
 * Get per-group expected memory shared quota in chunks
 */
static int32
groupGetMemSharedExpected(const ResGroupCaps *caps)
{
	return groupGetMemExpected(caps) - groupGetMemQuotaExpected(caps);
}

/*
 * Get per-group expected memory spill in chunks
 */
static int32
groupGetMemSpillTotal(const ResGroupCaps *caps)
{
	return groupGetMemExpected(caps) * memory_spill_ratio / 100;
}

/*
 * Get per-slot expected memory quota in chunks
 */
static int32
slotGetMemQuotaExpected(const ResGroupCaps *caps)
{
	Assert(caps->concurrency.proposed != 0);
	return Max(1, groupGetMemQuotaExpected(caps) / caps->concurrency.proposed);
}

/*
 * Get per-slot expected memory spill in chunks
 */
static int32
slotGetMemSpill(const ResGroupCaps *caps)
{
	Assert(caps->concurrency.proposed != 0);
	return groupGetMemSpillTotal(caps) / caps->concurrency.proposed;
}

/*
 * Attempt to wake up pending slots in the group as long as there are free
 * slots and enough memory quota.
 */
static void
wakeupSlots(ResGroupData *group)
{
	PROC_QUEUE		*waitQueue;

	waitQueue = &group->waitProcs;

	while (waitQueue->size > 0)
	{
		PGPROC		*waitProc;
		int			slotId;

		slotId = getSlot(group);
		if (slotId == InvalidSlotId)
			break;

		/* wake up one process in the wait queue */
		waitProc = (PGPROC *) MAKE_PTR(waitQueue->links.next);
		SHMQueueDelete(&waitProc->links);
		waitQueue->size--;

		waitProc->resWaiting = false;
		waitProc->resSlotId = slotId;
		SetLatch(&waitProc->procLatch);
	}
}

/*
 * When a group returns chunks to sys pool, we need to wake up
 * the transactions waiting on other groups for memory quota.
 */
static void
wakeupGroups(Oid skipGroupId)
{
	int				i;

	if (Gp_role != GP_ROLE_DISPATCH)
		return;

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));

	for (i = 0; i < MaxResourceGroups; i++)
	{
		ResGroupData	*group = &pResGroupControl->groups[i];
		int32			delta;

		if (group->groupId == InvalidOid)
			continue;

		if (group->groupId == skipGroupId)
			continue;

		if (group->lockedForDrop)
			continue;

		if (group->waitProcs.size == 0)
			continue;

		delta = group->memExpected - group->memQuotaGranted - group->memSharedGranted;
		if (delta <= 0)
			continue;

		wakeupSlots(group);

		if (!pResGroupControl->freeChunks)
			break;
	}
}

/*
 * Release memory quota when a slot gets freed and the caps has been changed,
 * the released memory quota includes:
 * * the slot over-used quota
 * * the group over-used shared quota
 */
static bool 
groupReleaseMemQuota(ResGroupData *group, ResGroupSlotData *slot)
{
	int32		memQuotaNeedFree;
	int32		memSharedNeeded;
	int32		memQuotaToFree;
	int32		memSharedToFree;
	int32       memQuotaExpected;
	ResGroupCaps *caps = &group->caps;

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));

	/* Return the over used memory quota to sys */
	memQuotaNeedFree = group->memQuotaGranted - groupGetMemQuotaExpected(caps);
	memQuotaToFree = memQuotaNeedFree > 0 ? Min(memQuotaNeedFree, slot->memQuota) : 0;

	if (caps->concurrency.proposed > 0)
	{
		/*
		 * Under this situation, when this slot is released,
		 * others will not be blocked by concurrency limit if
		 * they come to acquire this slot. So we could decide
		 * not to give all the memory to syspool even if we could.
		 */
		memQuotaExpected = slotGetMemQuotaExpected(caps);
		if (memQuotaToFree > memQuotaExpected)
			memQuotaToFree -= memQuotaExpected;
	}

	if (memQuotaToFree > 0)
	{
		returnChunksToPool(group->groupId, memQuotaToFree); 
		group->memQuotaGranted -= memQuotaToFree; 
	}

	/* Return the over used shared quota to sys */
	memSharedNeeded = Max(group->memSharedUsage,
						  groupGetMemSharedExpected(caps));
	memSharedToFree = group->memSharedGranted - memSharedNeeded;
	if (memSharedToFree > 0)
	{
		returnChunksToPool(group->groupId, memSharedToFree);
		pg_atomic_sub_fetch_u32((pg_atomic_uint32 *) &group->memSharedGranted,
								memSharedToFree);
	}
	return (memQuotaToFree > 0 || memSharedToFree > 0);
}

/*
 * Try to acquire enough quota & shared quota for current group,
 * the actual acquired quota depends on system loads.
 */
static void
groupAcquireMemQuota(ResGroupData *group, const ResGroupCaps *caps)
{
	int32 currentMemStocks = group->memSharedGranted + group->memQuotaGranted;
	int32 neededMemStocks = group->memExpected - currentMemStocks;

	if (neededMemStocks > 0)
	{
		int32 chunks = getChunksFromPool(group->groupId, neededMemStocks);
		groupAssginChunks(group, chunks, caps);
	}
}

static void
addTotalQueueDuration(ResGroupData *group)
{
	Assert(LWLockHeldExclusiveByMe(ResGroupLock));
	if (group == NULL)
		return;

	TimestampTz start = pgstat_fetch_resgroup_queue_timestamp();
	TimestampTz now = GetCurrentTimestamp();
	Datum durationDatum = DirectFunctionCall2(timestamptz_age, TimestampTzGetDatum(now), TimestampTzGetDatum(start));
	Datum sumDatum = DirectFunctionCall2(interval_pl, IntervalPGetDatum(&group->totalQueuedTime), durationDatum);
	memcpy(&group->totalQueuedTime, DatumGetIntervalP(sumDatum), sizeof(Interval));
}

/*
 * Release the resource group slot
 *
 * Call this function at the end of the transaction.
 */
static void
ResGroupSlotRelease(void)
{
	ResGroupData	*group;
	PROC_QUEUE	*waitQueue;
	PGPROC		*waitProc;

	group = self->group;
	Assert(group != NULL);

	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	waitQueue = &group->waitProcs;

	Assert(MyProc->resSlotId != InvalidSlotId);
	putSlot(group, MyProc->resSlotId);
	MyProc->resSlotId = InvalidSlotId;

	/*
	 * My slot is put back, then how many queuing queries should I wake up?
	 * Maybe zero, maybe one, maybe more, depends on how the resgroup's
	 * configuration were changed during our execution.
	 */
	while (waitQueue->size > 0)
	{
		int			slotId;

		slotId = getSlot(group);
		if (slotId == InvalidSlotId)
			break;

		/* wake up one process in the wait queue */
		waitProc = (PGPROC *) MAKE_PTR(waitQueue->links.next);
		SHMQueueDelete(&waitProc->links);
		waitQueue->size--;
		waitProc->resSlotId = slotId;	/* pass the slot to new query */
		waitProc->resWaiting = false;
		SetLatch(&waitProc->procLatch);
	}

	AssertImply(waitQueue->size == 0,
				waitQueue->links.next == MAKE_OFFSET(&waitQueue->links) &&
				waitQueue->links.prev == MAKE_OFFSET(&waitQueue->links));

	LWLockRelease(ResGroupLock);
}

/*
 * Serialize the resource group information that need to dispatch to segment.
 */
void
SerializeResGroupInfo(StringInfo str)
{
	int i;
	int tmp;
	ResGroupCaps caps0;
	ResGroupCap *caps;

	if (self->groupId != InvalidOid)
		caps = (ResGroupCap *) &self->caps;
	else
	{
		MemSet(&caps0, 0, sizeof(caps0));
		caps = (ResGroupCap *) &caps0;
	}

	tmp = htonl(self->groupId);
	appendBinaryStringInfo(str, (char *) &tmp, sizeof(self->groupId));

	tmp = htonl(self->slotId);
	appendBinaryStringInfo(str, (char *) &tmp, sizeof(self->slotId));

	for (i = 0; i < RESGROUP_LIMIT_TYPE_COUNT; i++)
	{
		tmp = htonl(caps[i].value);
		appendBinaryStringInfo(str, (char *) &tmp, sizeof(caps[i].value));

		tmp = htonl(caps[i].proposed);
		appendBinaryStringInfo(str, (char *) &tmp, sizeof(caps[i].proposed));
	}
}

/*
 * Deserialize the resource group information dispatched by QD.
 */
void
DeserializeResGroupInfo(struct ResGroupCaps *capsOut,
						const char *buf, int len)
{
	int			i;
	int			tmp;
	const char	*ptr = buf;
	ResGroupCap *caps = (ResGroupCap *) capsOut;

	Assert(len > 0);

	memcpy(&tmp, ptr, sizeof(self->groupId));
	self->groupId = ntohl(tmp);
	ptr += sizeof(self->groupId);

	memcpy(&tmp, ptr, sizeof(self->slotId));
	self->slotId = ntohl(tmp);
	ptr += sizeof(self->slotId);

	for (i = 0; i < RESGROUP_LIMIT_TYPE_COUNT; i++)
	{
		memcpy(&tmp, ptr, sizeof(caps[i].value));
		caps[i].value = ntohl(tmp);
		ptr += sizeof(caps[i].value);

		memcpy(&tmp, ptr, sizeof(caps[i].proposed));
		caps[i].proposed = ntohl(tmp);
		ptr += sizeof(caps[i].proposed);
	}

	Assert(len == ptr - buf);
}

/*
 * Check whether should assign resource group on master.
 */
bool
ShouldAssignResGroupOnMaster(void)
{
	return IsResGroupActivated() &&
		IsNormalProcessingMode() &&
		Gp_role == GP_ROLE_DISPATCH &&
		!AmIInSIGUSR1Handler();
}

/*
 * On master, QD is assigned to a resource group at the beginning of a transaction.
 * It will first acquire a slot from the resource group, and then, it will get the
 * current capability snapshot, update the memory usage information, and add to
 * the corresponding cgroup.
 */
void
AssignResGroupOnMaster(void)
{
	ResGroupData		*group;
	ResGroupSlotData	*slot;
	int		slotId;
	Oid		groupId;

	Assert(Gp_role == GP_ROLE_DISPATCH);

	PG_TRY();
	{
		/* Acquire slot */
		slotId = ResGroupSlotAcquire();
		Assert(slotId != InvalidSlotId);
		Assert(self->group != NULL);
		group = self->group;
		groupId = group->groupId;
		Assert(groupId != InvalidOid);
		Assert(!self->doMemCheck);

		/* Init slot */
		slot = &group->slots[slotId];
		Assert(slot->memQuota > 0);
		slot->sessionId = gp_session_id;
		pg_atomic_add_fetch_u32((pg_atomic_uint32*)&slot->nProcs, 1);

		/* Init self */
		self->groupId = groupId;
		self->slotId = slotId;
		self->caps = slot->caps;
		Assert(pResGroupControl != NULL);
		Assert(pResGroupControl->segmentsOnMaster > 0);

		attachToSlot(group, slot);

		/* Start memory limit checking */
		Assert(self->groupId != InvalidOid);
		Assert(self->slotId != InvalidSlotId);
		self->doMemCheck = true;

		/* Don't error out before this line in this function */
		SIMPLE_FAULT_INJECTOR(ResGroupAssignedOnMaster);

		/* Add into cgroup */
		ResGroupOps_AssignGroup(group->groupId, MyProcPid);

		/* Set spill guc */
		ResGroupSetMemorySpillRatio(&slot->caps);
	}
	PG_CATCH();
	{
		UnassignResGroupOnMaster();
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Detach from a resource group at the end of the transaction.
 */
void
UnassignResGroupOnMaster(void)
{
	ResGroupData		*group;
	ResGroupSlotData	*slot;

	if (self->group == NULL)
	{
		Assert(self->doMemCheck == false);
		return;
	}

	group = self->group;

	Assert(group->groupId != InvalidOid);
	Assert(self->groupId == group->groupId);
	Assert(self->slotId != InvalidSlotId);

	/* Stop memory limit checking */
	self->doMemCheck = false;

	slot = &group->slots[self->slotId];

	detachFromSlot(group, slot);

	/* Cleanup self */
	if (self->memUsage > 10)
		LOG_RESGROUP_DEBUG(LOG, "Idle proc memory usage: %d", self->memUsage);
	self->groupId = InvalidOid;
	self->slotId = InvalidSlotId;

	/* Cleanup slotInfo */
	pg_atomic_sub_fetch_u32((pg_atomic_uint32*)&slot->nProcs, 1);

	/* Release the slot */
	ResGroupSlotRelease();

	/* Cleanup group */
	self->group = NULL;
}

/*
 * QEs are not assigned/unassigned to a resource group on segments for each
 * transaction, instead, they switch resource group when a new resource group
 * id or slot id is dispatched.
 */
void
SwitchResGroupOnSegment(const char *buf, int len)
{
	Oid		prevGroupId;
	int		prevSlotId;
	ResGroupCaps		caps;
	ResGroupData		*group;
	ResGroupSlotData	*slot;
	ResGroupData		*prevSharedInfo = NULL;
	ResGroupSlotData	*prevSlot = NULL;

	prevGroupId = self->groupId;
	prevSlotId = self->slotId;

	/* Stop memory limit checking */
	self->doMemCheck = false;

	DeserializeResGroupInfo(&caps, buf, len);

	AssertImply(self->groupId != InvalidOid,
				self->slotId != InvalidSlotId);
	AssertImply(prevGroupId != InvalidOid,
				prevSlotId != InvalidSlotId);

	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	if (self->groupId == InvalidOid)
	{
		prevSharedInfo = self->group;
		self->group = NULL;

		if (prevSharedInfo)
		{
			Assert(prevSharedInfo->groupId == prevGroupId);
			prevSlot = &prevSharedInfo->slots[prevSlotId];
			detachFromSlot(prevSharedInfo, prevSlot);

			groupReleaseMemQuota(prevSharedInfo, prevSlot);
		}
		else
		{
			Assert(prevGroupId == InvalidOid);
			Assert(prevSlotId == InvalidSlotId);
			Assert(self->group == NULL);
		}

		LWLockRelease(ResGroupLock);
		return;
	}

	if (self->group != NULL && self->group->groupId != InvalidOid)
	{
		prevSharedInfo = self->group;
		if (prevSharedInfo->groupId == prevGroupId)
		{
			/* previous resource group is valid and not dropped yet */
			prevSlot = &prevSharedInfo->slots[prevSlotId];
		}
		else
		{
			/* previous resource group is already dropped */
			prevSharedInfo = NULL;
			prevSlot = NULL;
			prevGroupId = InvalidOid;
			prevSlotId = InvalidSlotId;
		}
	}

	group = ResGroupHashFind(self->groupId);
	Assert(group != NULL);

	LWLockRelease(ResGroupLock);

	/* Init self */
	Assert(host_segments > 0);
	Assert(caps.concurrency.proposed > 0);
	Assert(self->slotId != InvalidSlotId);
	self->caps = caps;

	/* Init slot */
	slot = &group->slots[self->slotId];
	slot->sessionId = gp_session_id;
	slot->caps = caps;
	slot->memQuota = slotGetMemQuotaExpected(&caps);
	ResGroupSetMemorySpillRatio(&caps);
	Assert(slot->memQuota > 0);

	if (prevSharedInfo != group || prevSlot != slot)
	{
		LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);
		if (prevSharedInfo)
		{
			Assert(prevSlot != NULL);
			detachFromSlot(prevSharedInfo, prevSlot);
			groupReleaseMemQuota(prevSharedInfo, prevSlot);
		}

		groupAcquireMemQuota(group, &slot->caps);
		LWLockRelease(ResGroupLock);

		attachToSlot(group, slot);
	}

	self->group = group;

	/* Start memory limit checking */
	Assert(self->groupId != InvalidOid);
	Assert(self->slotId != InvalidSlotId);
	self->doMemCheck = true;

	/* Add into cgroup */
	ResGroupOps_AssignGroup(self->groupId, MyProcPid);
}

/*
 * Wait on the queue of resource group
 */
static void
ResGroupWait(ResGroupData *group)
{
	PGPROC *proc = MyProc;

	pgstat_report_resgroup(GetCurrentTimestamp(), group->groupId);

	/* similar to lockAwaited in ProcSleep for interrupt cleanup */
	localResWaiting = true;

	/*
	 * Make sure we have released all locks before going to sleep, to eliminate
	 * deadlock situations
	 */
	PG_TRY();
	{
		for (;;)
		{
			ResetLatch(&proc->procLatch);

			CHECK_FOR_INTERRUPTS();

			if (!proc->resWaiting)
				break;
			WaitLatch(&proc->procLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, -1);
		}
	}
	PG_CATCH();
	{
		ResGroupWaitCancel();
		PG_RE_THROW();
	}
	PG_END_TRY();

	localResWaiting = false;

	pgstat_report_waiting(PGBE_WAITING_NONE);
}

/*
 * ResGroupHashNew -- return a new (empty) group object to initialize.
 *
 * Notes
 *	The resource group lightweight lock (ResGroupLock) *must* be held for
 *	this operation.
 */
static ResGroupData *
ResGroupHashNew(Oid groupId)
{
	int			i;
	bool		found;
	ResGroupHashEntry *entry;

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));

	if (groupId == InvalidOid)
		return NULL;

	for (i = 0; i < pResGroupControl->nGroups; i++)
	{
		if (pResGroupControl->groups[i].groupId == InvalidOid)
			break;
	}
	Assert(i < pResGroupControl->nGroups);

	entry = (ResGroupHashEntry *)
		hash_search(pResGroupControl->htbl, (void *) &groupId, HASH_ENTER_NULL, &found);
	/* caller should test that the group does not exist already */
	Assert(!found);
	entry->index = i;

	return &pResGroupControl->groups[i];
}

/*
 * ResGroupHashFind -- return the group for a given oid.
 *
 * Notes
 *	The resource group lightweight lock (ResGroupLock) *must* be held for
 *	this operation.
 */
static ResGroupData *
ResGroupHashFind(Oid groupId)
{
	bool				found;
	ResGroupHashEntry	*entry;

	Assert(LWLockHeldByMe(ResGroupLock));

	entry = (ResGroupHashEntry *)
		hash_search(pResGroupControl->htbl, (void *) &groupId, HASH_FIND, &found);
	if (!found)
		return NULL;

	Assert(entry->index < pResGroupControl->nGroups);
	return &pResGroupControl->groups[entry->index];
}


/*
 * ResGroupHashRemove -- remove the group for a given oid.
 *
 * Notes
 *	The resource group lightweight lock (ResGroupLock) *must* be held for
 *	this operation.
 */
static bool
ResGroupHashRemove(Oid groupId)
{
	bool		found;
	ResGroupHashEntry	*entry;
	ResGroupData		*group;

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));

	entry = (ResGroupHashEntry*)hash_search(pResGroupControl->htbl, (void *) &groupId, HASH_FIND, &found);
	if (!found)
		return false;

	group = &pResGroupControl->groups[entry->index];
	returnChunksToPool(groupId, group->memQuotaGranted + group->memSharedGranted);
	group->memQuotaGranted = 0;
	group->memSharedGranted = 0;
	group->groupId = InvalidOid;

	hash_search(pResGroupControl->htbl, (void *) &groupId, HASH_REMOVE, &found);

	wakeupGroups(groupId);
	return true;
}

/* Process exit without waiting for slot or received SIGTERM */
static void
AtProcExit_ResGroup(int code, Datum arg)
{
	ResGroupWaitCancel();
}

/*
 * Handle the interrupt cases when waiting on the queue
 *
 * The proc may wait on the queue for a slot, or wait for the
 * DROP transaction to finish. In the first case, at the same time
 * we get interrupted (SIGINT or SIGTERM), we could have been
 * granted a slot or not. In the second case, there's no running
 * transaction in the group. If the DROP transaction is finished
 * (commit or abort) at the same time as we get interrupted,
 * MyProc should have been removed from the wait queue, and the
 * ResGroupData entry may have been removed if the DROP is committed.
 */
static void
ResGroupWaitCancel(void)
{
	ResGroupData	*group;
	PROC_QUEUE		*waitQueue;

	/* Process exit without waiting for slot */
	group = self->group;
	if (group == NULL || !localResWaiting)
		return;

	/* We are sure to be interrupted in the for loop of ResGroupWait now */
	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	waitQueue = &group->waitProcs;

	if (MyProc->links.next != INVALID_OFFSET)
	{
		/* Still waiting on the queue when get interrupted, remove myself from the queue */

		Assert(waitQueue->size > 0);
		Assert(MyProc->resWaiting);
		Assert(MyProc->resSlotId == InvalidSlotId);

		addTotalQueueDuration(group);

		SHMQueueDelete(&(MyProc->links));
		waitQueue->size--;
	}
	else if (MyProc->links.next == INVALID_OFFSET && MyProc->resSlotId != InvalidSlotId)
	{
		/* Woken up by a slot holder */

		Assert(MyProc->resSlotId != InvalidSlotId);
		putSlot(group, MyProc->resSlotId);
		MyProc->resSlotId = InvalidSlotId;

		group->totalExecuted++;
		addTotalQueueDuration(group);

		/*
		 * Similar as ResGroupSlotRelease(), how many pending queries to
		 * wake up depends on how many slots we can get.
		 */
		wakeupSlots(group);

		AssertImply(waitQueue->size == 0,
					waitQueue->links.next == MAKE_OFFSET(&waitQueue->links) &&
					waitQueue->links.prev == MAKE_OFFSET(&waitQueue->links));
	}
	else
	{
		/*
		 * The transaction of DROP RESOURCE GROUP is finished,
		 * ResGroupSlotAcquire will do the retry.
		 */
	}

	LWLockRelease(ResGroupLock);
	localResWaiting = false;
	pgstat_report_waiting(PGBE_WAITING_NONE);
	self->group = NULL;
}

static void
ResGroupSetMemorySpillRatio(const ResGroupCaps *caps)
{
	char value[64];

	snprintf(value, sizeof(value), "%d", caps->memSpillRatio.proposed);
	set_config_option("memory_spill_ratio", value, PGC_USERSET, PGC_S_RESGROUP, GUC_ACTION_SET, true);
}

void
ResGroupGetMemInfo(int *memLimit, int *slotQuota, int *sharedQuota)
{
	const ResGroupCaps *caps = &self->caps;

	*memLimit = groupGetMemExpected(caps);
	*slotQuota = caps->concurrency.proposed ? slotGetMemQuotaExpected(caps) : -1;
	*sharedQuota = groupGetMemSharedExpected(caps);
}

/*
 * Convert ResGroupOpts to ResGroupCaps
 */
void
ResGroupOptsToCaps(const ResGroupOpts *optsIn, ResGroupCaps *capsOut)
{
	int i;
	ResGroupCap		*caps = (ResGroupCap *) capsOut;
	const int32		*opts = (int32 *) optsIn;

	for (i = 0; i < RESGROUP_LIMIT_TYPE_COUNT; i++)
	{
		caps[i].value = opts[i];
		caps[i].proposed = opts[i];
	}
}

/*
 * Convert ResGroupCaps to ResGroupOpts
 */
void
ResGroupCapsToOpts(const ResGroupCaps *capsIn, ResGroupOpts *optsOut)
{
	int i;
	const ResGroupCap	*caps = (ResGroupCap *) capsIn;
	int32				*opts = (int32 *) optsOut;

	for (i = 0; i < RESGROUP_LIMIT_TYPE_COUNT; i++)
		opts[i] = caps[i].proposed;
}
