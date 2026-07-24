/*
 * Copyright 2011-2015, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Oliver Tappe <zooey@hirschkaefer.de>
 *		Rene Gollent <rene@gollent.com>
 */


#include <Job.h>

#include <Errors.h>


namespace BSupportKit {


BJobStateListener::~BJobStateListener()
{
}


void
BJobStateListener::JobStarted(BJob* job)
{
}


void
BJobStateListener::JobProgress(BJob* job)
{
}


void
BJobStateListener::JobSucceeded(BJob* job)
{
}


void
BJobStateListener::JobFailed(BJob* job)
{
}


void
BJobStateListener::JobAborted(BJob* job)
{
}


// #pragma mark -


BJob::BJob(const BString& title)
	:
	fTitle(title),
	fState(B_JOB_STATE_WAITING_TO_RUN),
	fTicketNumber(0xFFFFFFFFUL)
{
	if (fTitle.Length() == 0)
		fInitStatus = B_BAD_VALUE;
	else
		fInitStatus = B_OK;
}


BJob::~BJob()
{
	// Ownership safety: the JobQueue deletes dependant jobs when a job fails
	// (JobQueue::_RemoveDependantJobsOf -> "delete dependantJob") without any
	// global bookkeeping of the dependency graph - see the "we need some sort
	// of ownership management" TODO there. With an empty destructor, a job that
	// other jobs still depended on stayed listed in their fDependantJobs even
	// after being freed; a later JobQueue::JobSucceeded() ->
	// _RequeueDependantJobsOf() then dereferenced the dangling pointer (via
	// DependantJobAt()->RemoveDependency()) and crashed in BList::IndexOf,
	// reading the freed job's storage back as the 0xFFFFFFFF ticket sentinel.
	//
	// Sever the cross-links here: for every job this one depends on, drop this
	// from that job's dependant list. The dependency invariant is symmetric
	// (A.fDependencies has B  <=>  B.fDependantJobs has A), so this covers
	// every list that could still be holding a soon-to-dangle pointer. Only
	// fDependantJobs items are ever dereferenced, so the mirror direction needs
	// no cleanup - and skipping it means we never touch a queued job's
	// dependency count, keeping the JobQueue's ordered set invariant intact.
	for (int32 i = fDependencies.CountItems() - 1; i >= 0; i--) {
		BJob* dependency = fDependencies.ItemAt(i);
		if (dependency != NULL)
			dependency->fDependantJobs.RemoveItem(this);
	}
}


status_t
BJob::InitCheck() const
{
	return fInitStatus;
}


const BString&
BJob::Title() const
{
	return fTitle;
}


BJobState
BJob::State() const
{
	return fState;
}


status_t
BJob::Result() const
{
	return fResult;
}


const BString&
BJob::ErrorString() const
{
	return fErrorString;
}


uint32
BJob::TicketNumber() const
{
	return fTicketNumber;
}


void
BJob::_SetTicketNumber(uint32 ticketNumber)
{
	fTicketNumber = ticketNumber;
}


void
BJob::_ClearTicketNumber()
{
	fTicketNumber = 0xFFFFFFFFUL;
}


void
BJob::SetErrorString(const BString& error)
{
	fErrorString = error;
}


status_t
BJob::Run()
{
	if (fState != B_JOB_STATE_WAITING_TO_RUN)
		return B_NOT_ALLOWED;

	fState = B_JOB_STATE_STARTED;
	NotifyStateListeners();

	fState = B_JOB_STATE_IN_PROGRESS;
	fResult = Execute();
	Cleanup(fResult);

	fState = fResult == B_OK
		? B_JOB_STATE_SUCCEEDED
		: fResult == B_CANCELED
			? B_JOB_STATE_ABORTED
			: B_JOB_STATE_FAILED;
	NotifyStateListeners();

	return fResult;
}


void
BJob::Cleanup(status_t /*jobResult*/)
{
}


status_t
BJob::AddStateListener(BJobStateListener* listener)
{
	return fStateListeners.AddItem(listener) ? B_OK : B_ERROR;
}


status_t
BJob::RemoveStateListener(BJobStateListener* listener)
{
	return fStateListeners.RemoveItem(listener) ? B_OK : B_ERROR;
}


status_t
BJob::AddDependency(BJob* job)
{
	if (fDependencies.HasItem(job))
		return B_ERROR;

	if (fDependencies.AddItem(job) && job->fDependantJobs.AddItem(this))
		return B_OK;

	return B_ERROR;
}


status_t
BJob::RemoveDependency(BJob* job)
{
	if (!fDependencies.HasItem(job))
		return B_ERROR;

	if (fDependencies.RemoveItem(job) && job->fDependantJobs.RemoveItem(this))
		return B_OK;

	return B_ERROR;
}


bool
BJob::IsRunnable() const
{
	return fDependencies.IsEmpty();
}


int32
BJob::CountDependencies() const
{
	return fDependencies.CountItems();
}


BJob*
BJob::DependantJobAt(int32 index) const
{
	return fDependantJobs.ItemAt(index);
}


void
BJob::SetState(BJobState state)
{
	fState = state;
}


void
BJob::NotifyStateListeners()
{
	int32 count = fStateListeners.CountItems();
	for (int i = 0; i < count; ++i) {
		BJobStateListener* listener = fStateListeners.ItemAt(i);
		if (listener == NULL)
			continue;
		switch (fState) {
			case B_JOB_STATE_STARTED:
				listener->JobStarted(this);
				break;
			case B_JOB_STATE_IN_PROGRESS:
				listener->JobProgress(this);
				break;
			case B_JOB_STATE_SUCCEEDED:
				listener->JobSucceeded(this);
				break;
			case B_JOB_STATE_FAILED:
				listener->JobFailed(this);
				break;
			case B_JOB_STATE_ABORTED:
				listener->JobAborted(this);
				break;
			default:
				break;
		}
	}
}


}	// namespace BPackageKit
