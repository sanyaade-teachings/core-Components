/*
* This file is part of Wakanda software, licensed by 4D under
*  (i) the GNU General Public License version 3 (GNU GPL v3), or
*  (ii) the Affero General Public License version 3 (AGPL v3) or
*  (iii) a commercial license.
* This file remains the exclusive property of 4D and/or its licensors
* and is protected by national and international legislations.
* In any event, Licensee's compliance with the terms and conditions
* of the applicable license constitutes a prerequisite to any use of this file.
* Except as otherwise expressly stated in the applicable license,
* such license does not include any other license or rights on this file,
* 4D's and/or its licensors' trademarks and/or other proprietary rights.
* Consequently, no title, copyright or other proprietary rights
* other than those specified in the applicable license is granted.
*/
#include "LanguageSyntaxHeaders.h"
#include "JavaScriptParser.h"

struct ENTITY_MODEL_ATTR_TYPE
{
	VString	type;
	VString	jsType;
	VString	jsFile;
};

struct ENTITY_MODEL_ATTR_TYPE ENTITY_MODEL_ATTR_TYPES [] =
{
	{ CVSTR ( "blob" ),		CVSTR ( "" ),			CVSTR ( "" ) },
	{ CVSTR ( "bool" ),		CVSTR ( "Boolean" ),	CVSTR ( "JSCore.js" ) },
	{ CVSTR ( "byte" ),		CVSTR ( "Number" ),		CVSTR ( "JSCore.js" ) },
	{ CVSTR ( "date" ),		CVSTR ( "Date" ),		CVSTR ( "JSCore.js" ) },
	{ CVSTR ( "duration" ),	CVSTR ( "Number" ),		CVSTR ( "JSCore.js" ) },
	{ CVSTR ( "image" ),	CVSTR ( "" ),			CVSTR ( "" ) },
	{ CVSTR ( "long" ),		CVSTR ( "Number" ),		CVSTR ( "JSCore.js" ) },
	{ CVSTR ( "long64" ),	CVSTR ( "Number" ),		CVSTR ( "JSCore.js" ) },
	{ CVSTR ( "number" ),	CVSTR ( "Number" ),		CVSTR ( "JSCore.js" ) },
	{ CVSTR ( "string" ),	CVSTR ( "String" ),		CVSTR ( "JSCore.js" ) },
	{ CVSTR ( "uuid" ),		CVSTR ( "" ),			CVSTR ( "" ) },
	{ CVSTR ( "word" ),		CVSTR ( "Number" ),		CVSTR ( "JSCore.js" ) },
};

#define	ENTITY_MODEL_ATTR_TYPE_COUNT	(sizeof( ENTITY_MODEL_ATTR_TYPES ) / sizeof( ENTITY_MODEL_ATTR_TYPE ))


class VDocumentParserManager : public IDocumentParserManager, public XBOX::VObject {
friend class WorkerThread;

private:
	typedef struct WorkItem {
		// The path to the file that we want to parse
		VFilePath fFilePath;
		// file size (in bytes)
		uLONG fFileBytes;
		// The unique caller identifier
		const void *fHandler;
		// File base folder (Studio/Server/Project/Solution)
		ESymbolFileBaseFolder fBaseFolder;
		// File execution context (client side, server side or both)
		ESymbolFileExecContext fExecutionScope;
		// The priority of the work item
		Priority fPriority;
		// If this is non-empty, this is the contents that we want to parse, regardless of
		// whether the file exists on disk or not
		VString fFileContents;
		// (OPTIONAL) The symbol table we will add symbols to
		ISymbolTable *fSymTable;
		// (OPTIONAL) If this item is a part of a job, then the job pointer lives here
		IJob *fJob;
		// (OPTIONAL) If the work item is actually an entity model catalog, we need to use
		// the special interface to handle it.  If this is non-NULL, then we will ignore
		// the fFileContents field.
		IEntityModelCatalog *fCatalog;
		// This field is used to indicate if the file content has been set (even if the file
		// content is empty
		bool fFileContentHasBeenSet;
		// This field is used to indicate if we have to process parsing regardless file
		// modification time stamps (usefull case to discard file modification saving)
		bool fParsingIsMandatory;

		void CleanUp( void ) {
			if (fSymTable)	fSymTable->Release();
			if (fJob)		fJob->Release();
			if (fCatalog)	fCatalog->Release();
		}
	} WorkItem;

	// This is our less-than comparator, used for determining relative placement
	// within the priority queue
	static bool WorkItemComparator( const WorkItem *a, const WorkItem *b ) {
		return a->fPriority < b->fPriority;
	}

	typedef std::list< WorkItem * >	WorkList;

	VCriticalSection *fTaskLock;
	
	/////////////////	---- Start shared resource section
	// The items in this section are all shared resources with the worker thread, and need to be
	// protected by a critical section
	
	// This vector is kept in sorted order, with the higher priority items at the end of the
	// vector.  So the worker thread should always be using pop_back to get the next item to work on
	WorkList fWorkQueue;
	std::vector< class WorkerThread * > fWorkers;
	std::map< VFilePath, VCriticalSection* > fParseOutlineMutexes;
	////////////////   ---- End shared resource section

	ParsingStartSignal fParsingStartSignal;
	ParsingCompleteSignal fParsingCompleteSignal;
	CompileErrorSignal fCompileErrorSignal;
	PendingParsingRequestSignal fPendingParsingRequestSignal;

	enum { kDefault = 0 };
	void CreateWorkerThreads( sLONG inNumThreads );		// Pass kDefault to create the default number of threads
	void ShutdownWorkerThread( WorkerThread *inThread );
	void EmptyWorkQueue( const void *inCaller );

	bool AnyThreadWorkingOn( const VFilePath &inPath, const void *inCaller );
	WorkerThread *GetThreadWorkingFor( const void *inCaller );

	bool ScheduleTaskHelper( const void *inCaller, const VSymbolFileInfos &inFileInfos, IEntityModelCatalog *inCatalog, const VString &inContents, const bool& inFileContentIsSet, ISymbolTable *inSymTable, Priority inPriority, IJob *inJob, const bool& inParsingMandatory );

	// Used for user feedback, should be incremented each time we schedule a parsing task and decremented each time a parsing task is finished
	sLONG fFilesToParseCount;
	bool fParsingStatusStartNotified;
	void ResetFilesToParseCounter()
	{
		fFilesToParseCount=0;
		fParsingStatusStartNotified=false;
		fPendingParsingRequestSignal.Trigger(fFilesToParseCount);
	}
	void IncrementFilesToParseCounter()
	{
		fFilesToParseCount++;
		if( fFilesToParseCount>=1 && fParsingStatusStartNotified==false )
		{
			fParsingStatusStartNotified = true;
			fPendingParsingRequestSignal.Trigger(fFilesToParseCount);
		}
#if VERSIONDEBUG
		VString msg("[Parsing] Pending(");
		msg.AppendLong(fFilesToParseCount);
		msg.AppendString(")\n");
		DebugMsg(msg);
#endif
	}
	void DecrementFilesToParseCounter()
	{
		fFilesToParseCount--;
		if( fFilesToParseCount<=0 && fParsingStatusStartNotified==true)
		{
			fParsingStatusStartNotified = false;
			fPendingParsingRequestSignal.Trigger(fFilesToParseCount);
		}
#if VERSIONDEBUG
		VString msg("[Parsing] Pending(");
		msg.AppendLong(fFilesToParseCount);
		msg.AppendString(")\n");
		DebugMsg(msg);
#endif
	}

public:
	VDocumentParserManager();
	virtual ~VDocumentParserManager();

	virtual IJob *CreateJob();
	
	virtual XBOX::VCriticalSection* GetLockForParseOrOutline(const XBOX::VFilePath&);
	
	virtual ParsingStartSignal &GetParsingStartSignal() { return fParsingStartSignal; }
	virtual ParsingCompleteSignal &GetParsingCompleteSignal() { return fParsingCompleteSignal; }
	virtual CompileErrorSignal &GetCompileErrorSignal() { return fCompileErrorSignal; }
	virtual PendingParsingRequestSignal &GetPendingParsingRequestSignal() { return fPendingParsingRequestSignal; }

	virtual bool ScheduleTask( const void *inCaller, const VSymbolFileInfos &inFileInfos, const VString &inContents, class ISymbolTable *inTable, Priority inPriority = kPriorityNormal, const bool& inParsingMandatory = false );
	virtual bool ScheduleTask( const void *inCaller, const VSymbolFileInfos &inFileInfos, class ISymbolTable *inTable, Priority inPriority = kPriorityNormal, const bool& inParsingMandatory = false );
	virtual bool ScheduleTask( const void *inCaller, IEntityModelCatalog *inCatalog, class ISymbolTable *inTable, Priority inPriority = kPriorityNormal, const bool& inParsingMandatory = false );
	virtual bool ScheduleTask( const void *inCaller, IJob *inJob, class ISymbolTable *inTable, Priority inPriority = kPriorityNormal, const bool& inParsingMandatory = false );
	virtual void UnscheduleTasksForHandler( const void *inCaller );
	
	virtual sLONG GetFilesToParseCount() const { return fFilesToParseCount; }
};

class WorkerThread : public VTask, public JavaScriptParserDelegate {
private:
	VDocumentParserManager::WorkItem *fCurrentWorkItem;
	VDocumentParserManager *fManager;
	VSyncEvent *fQueueSync;
	bool fWaitingForWork;
	
	static sLONG WorkerRunner( VTask *inTask ) {
		static sLONG sCounter = 1;
		char buffer[ 1024 ] = { 0 };
		sprintf( buffer, "Declaration Parsing Thread %d", sCounter++ );
		inTask->SetName( buffer );
		WorkerThread *instance = (WorkerThread *)inTask->GetKindData();
		instance->ExecuteTask();
		return 0;
	}
	void ExecuteTask();
	bool ParseDocument( VDocumentParserManager::WorkItem *inWorkItem, bool& outHasParsingError, bool& outContentOverwritenFromDisk, bool& outTimeStampUpdated );
	//Symbols::IFile *GetFileForDeclarationParsingUpdate( ISymbolTable *inTable, const VString &inFilePath );
	Symbols::IFile *GetFileForDeclarationParsingUpdate( ISymbolTable *inTable, const VFilePath &inFilePath, const ESymbolFileBaseFolder inBaseFolder, const ESymbolFileExecContext inExecContext);


	// Members for the JavaScriptParserDelegate interface
	virtual void Error( XBOX::VFilePath, sLONG inLine, sLONG inOffset, const XBOX::VString& inMessage );
	virtual Symbols::ISymbol *GetDeclarationContext( ) { return NULL; }

	void ParseEntityModel( ISymbolTable *inTable, Symbols::IFile *inOwnerFile, const VString &inEntityName, std::vector< IEntityModelCatalogAttribute* >& inAttributes, std::vector< IEntityModelCatalogMethod* >& inMethods );
	Symbols::ISymbol *GetSymbol( ISymbolTable *inTable, const XBOX::VString &inSymbolName, const XBOX::VString &inFileName );
	void CreateNewInstance( ISymbolTable *inTable, Symbols::ISymbol *inSym, const XBOX::VString &inSymbolName, const XBOX::VString &inFileName );
	
	bool _IsJavaScriptContent(const VDocumentParserManager::WorkItem* inWorkItem) const
	{
		bool ret = false;

		VString extension;
		inWorkItem->fFilePath.GetExtension( extension );
		if( extension == "js" || _IsJSONContent(inWorkItem) )
			ret = true;
		
		return ret;
	}
	
	bool _IsJSONContent(const VDocumentParserManager::WorkItem* inWorkItem) const
	{
		bool ret = false;
		
		VString extension;
		inWorkItem->fFilePath.GetExtension( extension );

		if( extension == "waModel" && inWorkItem->fCatalog == NULL && inWorkItem->fFileContents.BeginsWith("{") )
			ret = true;
		
		return ret;
	}
	
	void _PrepareJSONContentForParsing(VDocumentParserManager::WorkItem* inWorkItem, VString& outContent) const
	{
		outContent.AppendString("var waModel=");
		outContent.AppendString(inWorkItem->fFileContents);
	}
	
	bool _IsDataModelContent(const VDocumentParserManager::WorkItem* inWorkItem) const
	{
		bool ret = false;
		
		if( inWorkItem->fCatalog )
			ret = true;
		
		return ret;
	}

public:
	WorkerThread( VDocumentParserManager *inCreator ) : VTask( inCreator, 0, eTaskStylePreemptive, WorkerRunner ), fCurrentWorkItem( NULL ), fManager( inCreator ), fWaitingForWork( true ) {
		SetKindData( (sLONG_PTR)this );
		fQueueSync = new VSyncEvent();
	}
	virtual ~WorkerThread() {
		if (fCurrentWorkItem) {
			fCurrentWorkItem->CleanUp();
			delete fCurrentWorkItem;
		}
		
		delete fQueueSync;
	}

	void WakeMeUpPlease() {
		fQueueSync->Unlock();
	}
	
	bool IsWaitingForWork() const {
		return fWaitingForWork;
	}
	
	bool IsWorkingOn( const VFilePath &inPath, const void *inHandler ) {
		bool bRet = false;
		fManager->fTaskLock->Lock();
		if (fCurrentWorkItem) {
			/*if (fCurrentWorkItem->fHandler == inHandler &&
				fCurrentWorkItem->fFilePath.EqualTo( inPath, true )) {
				bRet = true;
			}*/
			if (fCurrentWorkItem->fFilePath == inPath)
			{
				bRet = true;
			}
		}
		fManager->fTaskLock->Unlock();
		return bRet;
	}

	bool IsWorkingFor( const void *inHandler ) {
		bool bRet = false;
		fManager->fTaskLock->Lock();
		if (fCurrentWorkItem) {
			if (fCurrentWorkItem->fHandler == inHandler) {
				bRet = true;
			}
		}
		fManager->fTaskLock->Unlock();
		return bRet;
	}
};

class VJobObject : public IDocumentParserManager::IJob
{
friend class VDocumentParserManager;
friend class WorkerThread;

private:
	std::vector< VFilePath > fPaths;
	std::vector< VSymbolFileInfos > fFiles;
	sLONG fRemainingCount;

	inline bool TaskComplete() {
		// Decrease our job counter and return whether that was the last item
		// in the job
		return (0 == VInterlocked::Decrement( &fRemainingCount ));
	}

public:
	VJobObject() : fRemainingCount( 0 ) {}

	virtual void ScheduleTask( const VSymbolFileInfos inFileInfos )
	{
		VInterlocked::Increment( &fRemainingCount );
		fFiles.push_back( inFileInfos );
	}


	virtual sLONG GetTaskCount() { return fRemainingCount; }
};

VDocumentParserManager::VDocumentParserManager() 
{
	ResetFilesToParseCounter();
	fTaskLock = new VCriticalSection();
	CreateWorkerThreads( kDefault );
}

VDocumentParserManager::~VDocumentParserManager()
{
	EmptyWorkQueue( NULL );
	xbox_assert( fWorkQueue.empty() );

	// We have to ensure our worker thread stops processing whatever work item it
	// is currently working on.
	ShutdownWorkerThread( NULL );

	for( std::map< VFilePath, VCriticalSection* >::iterator it=fParseOutlineMutexes.begin(); it!=fParseOutlineMutexes.end(); ++it)
	{
		if( it->second )
		{
			delete it->second;
			it->second = NULL;
		}
	}

	delete fTaskLock;
}

IDocumentParserManager::IJob *VDocumentParserManager::CreateJob() {
	return new VJobObject();
}

void VDocumentParserManager::EmptyWorkQueue( const void *inCaller )
{
	fTaskLock->Lock();

	WorkList new_work_queue;
	for (WorkList::iterator iter = fWorkQueue.begin(); iter != fWorkQueue.end(); ++iter) {
		// Check to see if this work item has a matching handler.  If we've not been handed
		// a handler, then all work items match.  If the item "matches" what we're looking for,
		// then we don't move it over to the new work queue.
		if (!inCaller || (*iter)->fHandler == inCaller) {
			// This is a matching item, which we want to delete
			WorkItem *item = *iter;
			item->CleanUp();
			delete item;
		} else {
			new_work_queue.push_back( *iter );
		}
	}

	fWorkQueue.assign( new_work_queue.begin(), new_work_queue.end() );

	fTaskLock->Unlock();
}

void VDocumentParserManager::ShutdownWorkerThread( WorkerThread *inThread )
{
	if (!inThread) {
		// The caller wants us to shutdown all of the worker threads
		for (std::vector< WorkerThread * >::reverse_iterator iter = fWorkers.rbegin(); iter != fWorkers.rend(); ++iter) {
			ShutdownWorkerThread( *iter );
		}
		return;
	}

	fTaskLock->Lock();
	// Find the worker thread in our list and remove it
	for (std::vector< WorkerThread * >::iterator iter = fWorkers.begin(); iter != fWorkers.end(); ++iter) {
		if ((*iter) == inThread) {
			fWorkers.erase( iter );
			break;
		}
	}
	
	// First, put the worker thread into the dying state
	inThread->StopMessaging();
	inThread->Kill();

	// If the thread was currently asleep, we need to ensure it wakes up in order for
	// it to receive the kill message
	inThread->WakeMeUpPlease();
	fTaskLock->Unlock();

	// Now, we have to wait for the worker to finish dying, which should hopefully happen
	// relatively quickly
	while (!inThread->WaitForDeath( 250 )) {
		// We just hang out, waiting for it finish dying off
	}

	// Now we know the thread is dead, so we can release it
	inThread->Release();
}

void VDocumentParserManager::CreateWorkerThreads( sLONG inNumThreads )
{
	if (inNumThreads == kDefault) {
		// We always want to use at least one processor, but if the machine is a multicore
		// or multiprocessor machine, then we want to make as many threads as we can.  We
		// reduce the number by one, however, so that the GUI thread can continue to update
		// as it needs to.
		inNumThreads = std::max< sLONG >( 1, VSystem::GetNumberOfProcessors() - 1 );
	}

	inNumThreads = 1;

	for (sLONG i = 0; i < inNumThreads; i++) {
		WorkerThread *worker = new WorkerThread( this );
		fWorkers.push_back( worker );

		// And we want to start the thread up now
		worker->Run();
	}
}

bool VDocumentParserManager::AnyThreadWorkingOn( const VFilePath &inPath, const void *inCaller )
{
	bool bRet = false;
	fTaskLock->Lock();
	for (std::vector< WorkerThread * >::iterator iter = fWorkers.begin(); !bRet && iter != fWorkers.end(); ++iter) {
		bRet = (*iter)->IsWorkingOn( inPath, inCaller );
	}
	fTaskLock->Unlock();
	return bRet;
}


bool VDocumentParserManager::ScheduleTaskHelper( const void *inCaller, const VSymbolFileInfos &inFileInfos, IEntityModelCatalog *inCatalog, const VString &inContents, const bool& inFileContentIsSet, ISymbolTable *inSymTable, Priority inPriority, IJob *inJob, const bool& inParsingMandatory )
{
	// We need to iterate over the list of work items already scheduled and see if there is already a
	// work item for this file and handler.  If there is, we just want to update the priority.  Also, if
	// this file and handler are currently being processed by the worker thread, we just want to bail out
	// entirely.
	bool bAddToQueue = true;
	fTaskLock->Lock();
	if (AnyThreadWorkingOn( inFileInfos.GetFilePath(), inCaller )) {
		// The worker thread is currently processing this item, so we don't want to add it to the queue
		bAddToQueue = false;
	} else {
		// Check to see if this item is already in the queue, and if it is, remove it -- we will be re-adding
		// it to the queue anyways
		for (WorkList::iterator iter = fWorkQueue.begin(); iter != fWorkQueue.end(); ++iter) {
			WorkItem *item = *iter;
			if (item->fHandler == inCaller && (item->fFilePath == inFileInfos.GetFilePath()) )
			{
				if (item->fPriority == inPriority) {
					// This exact item is already in the queue, so we don't want to do anything
					bAddToQueue = false;
				} else if (item->fJob == inJob) {
					// We are going to remove the current item from the queue and add a new item in to represent it
					item->CleanUp();
					delete item;
					fWorkQueue.erase( iter );
					break;	// Done looking!
				}
			}
		}
	}
	fTaskLock->Unlock();

	if (!bAddToQueue)
	{
		// If we are not scheduling this task, but the task is part of a batch of jobs, we need to let the job
		// know not to expect this document
		if (inJob)
			static_cast< VJobObject * >( inJob )->TaskComplete();
	}
	else
	{
		// Now we want to create a new work item and add it to the queue.  Once we've added it to the queue, we want to
		// sort the queue to ensure that all of our work items are in priority order, from lowest to highest.
		WorkItem *item = new WorkItem;
		item->fFilePath					=	inFileInfos.GetFilePath();
		item->fBaseFolder				=	inFileInfos.GetBaseFolder();
		item->fExecutionScope			=	inFileInfos.GetExecutionContext();
		item->fHandler					=	inCaller;
		item->fPriority					=	inPriority;
		item->fFileContents				=	inContents;
		item->fSymTable					=	inSymTable;
		item->fJob						=	inJob;
		item->fCatalog					=	inCatalog;
		item->fFileContentHasBeenSet	=	inFileContentIsSet;
		item->fParsingIsMandatory		=	inParsingMandatory;
		
		if (item->fCatalog)		item->fCatalog->Retain();
		if (item->fJob)			item->fJob->Retain();
		if (item->fSymTable)	item->fSymTable->Retain();
		
		fTaskLock->Lock();
		fWorkQueue.insert( fWorkQueue.begin(), item );
		fWorkQueue.sort( WorkItemComparator );
		
		// We want to make sure our worker is awake, since there's now some work to do.  However, we have to pick a worker
		// thread to wake up because each thread is responsible for its own synchronization event.	Lucky for us, our worker
		// threads are smart enough to track whether they're waiting for work.  So we just loop through and find the first one
		// that is waiting for work.  If none of them are waiting for work, then we don't have to do anything anyways.
		for (std::vector< WorkerThread * >::iterator iter = fWorkers.begin(); iter != fWorkers.end(); ++iter) {
			if ((*iter)->IsWaitingForWork()) {
				(*iter)->WakeMeUpPlease();
				break;
			}
		}
		
		// Update the count of files to be parsed
		IncrementFilesToParseCounter();
		
		fTaskLock->Unlock();
	}
	
#if VERSIONDEBUG
	if( bAddToQueue )
	{
		VString msg("[Scheduling Done] File(");
		msg.AppendString(inFileInfos.GetFilePath().GetPath());
		msg.AppendString(") ContentSet(");
		msg.AppendLong(inFileContentIsSet);
		msg.AppendString(") ParsingMandatory(");
		msg.AppendLong(inParsingMandatory);
		msg.AppendString(")\r\n");
		DebugMsg(msg);		
	}
	else
	{
		VString msg("[Scheduling Discarded] File(");
		msg.AppendString(inFileInfos.GetFilePath().GetPath());
		msg.AppendString(")\r\n");
		DebugMsg(msg);
	}
#endif
	
	return bAddToQueue;
}

bool VDocumentParserManager::ScheduleTask( const void *inCaller, const VSymbolFileInfos& inFileInfos, const VString &inContents, ISymbolTable *inTable, Priority inPriority, const bool& inParsingMandatory )
{
	bool res = ScheduleTaskHelper( inCaller, inFileInfos, NULL, inContents, true, inTable, inPriority, NULL, inParsingMandatory );
	return res;
}

bool VDocumentParserManager::ScheduleTask( const void *inCaller, const VSymbolFileInfos& inFileInfos, ISymbolTable *inTable, Priority inPriority, const bool& inParsingMandatory )
{
	bool res = ScheduleTaskHelper( inCaller, inFileInfos, NULL, CVSTR( "" ), false, inTable, inPriority, NULL, inParsingMandatory );
	return res;
}

bool VDocumentParserManager::ScheduleTask( const void *inCaller, IEntityModelCatalog *inCatalog, ISymbolTable *inTable, Priority inPriority, const bool& inParsingMandatory )
{
	VFilePath path;
	inCatalog->GetCatalogPath( path );

	VSymbolFileInfos fileInfos(path, eSymbolFileBaseFolderProject, eSymbolFileExecContextServer);

	bool res = ScheduleTaskHelper( inCaller, fileInfos, inCatalog, CVSTR( "" ), false, inTable, inPriority, NULL, inParsingMandatory );
	return res;
}

bool VDocumentParserManager::ScheduleTask( const void *inCaller, IJob *inJob, ISymbolTable *inTable, Priority inPriority, const bool& inParsingMandatory )
{
	bool res = true;
	
	// Jobs are faily easy to handle -- we just add their items as separate tasks in the queue, but pass in the
	// job the item is associated with so we can tell when the job is complete.
	for (int i = 0; i < static_cast< VJobObject * >( inJob )->fFiles.size(); i++) {
		res &= ScheduleTaskHelper( inCaller, static_cast< VJobObject * >( inJob )->fFiles[ i ], NULL, CVSTR( "" ), false, inTable, inPriority, inJob, inParsingMandatory );
	}
	
	return res;
}

VCriticalSection* VDocumentParserManager::GetLockForParseOrOutline(const XBOX::VFilePath& path)
{
	VCriticalSection* mutex = NULL;

	fTaskLock->Lock();
	std::map< VFilePath, VCriticalSection* >::iterator it = fParseOutlineMutexes.find(path);
	if( it != fParseOutlineMutexes.end() )
	{
		mutex = it->second;
	}
	else
	{
		mutex = new VCriticalSection();
		fParseOutlineMutexes[path] = mutex;
	}
	fTaskLock->Unlock();
	
	return mutex;
}

WorkerThread *VDocumentParserManager::GetThreadWorkingFor( const void *inCaller )
{
	WorkerThread *ret = NULL;
	fTaskLock->Lock();

	for (std::vector< WorkerThread * >::iterator iter = fWorkers.begin(); !ret && iter != fWorkers.end(); ++iter) {
		if ((*iter)->IsWorkingFor( inCaller )) {
			ret = *iter;
		}
	}
	fTaskLock->Unlock();
	return ret;
}

void VDocumentParserManager::UnscheduleTasksForHandler( const void *inHandler )
{
	// First, we want to remove any items this handler has scheduled for us to work on
	EmptyWorkQueue( inHandler );

	// But we also have to see whether the worker thread is currently working on something that
	// this handler cares about.  If it does, we have to ask the worker thread to please stop
	// working on that item.  Unfortunately, the logic for doing this is rather obtuse because we
	// can't subclass VTask (since IsDying isn't virtual, and the parser calls VTask::GetCurrent, which
	// returns a VTask -- so IsDying would never get called on our subclass).  So, what we will do is
	// kill off the worker thread entirely, and just make a new thread to replace the one we terminated.
	// But we only need to do this if the thread is currently processing for the given handler.
	WorkerThread *worker = NULL;
	do {
		worker = GetThreadWorkingFor( inHandler );
		if (worker) {
			ShutdownWorkerThread( worker );
			CreateWorkerThreads( 1 );
		}
	} while (worker);
}

Symbols::IFile *WorkerThread::GetFileForDeclarationParsingUpdate( ISymbolTable *inTable, const VFilePath &inFilePath, const ESymbolFileBaseFolder inBaseFolder, const ESymbolFileExecContext inExecContext)
{
	Symbols::IFile	*ret = NULL;

	std::vector< Symbols::IFile * > files = inTable->GetFilesByPathAndBaseFolder( inFilePath, inBaseFolder);
	if (!files.empty()) {
		ret = files.front();
		ret->Retain();
	} else {
		// The file doesn't exist in the database, so we want to make a new one.  We won't
		// be setting the file time however, because we want to ensure that a declaration
		// parsing pass happens
		Symbols::IFile *file = new VSymbolFile();
		VString			pathStr;

		inTable->GetRelativePath( inFilePath, inBaseFolder, pathStr );

		file->SetPath( pathStr );
		file->SetBaseFolder( inBaseFolder );
		file->SetExecutionContext( inExecContext );

		std::vector< Symbols::IFile * > files;
		files.push_back( file );
		inTable->AddFiles( files );
		ret = file;
	}

	// Now we want to release all of the files in the vector so that we don't leak them
	for( std::vector< Symbols::IFile * >::iterator iter = files.begin(); iter != files.end(); ++iter) {
		(*iter)->Release();
	}

	return ret;
}


void WorkerThread::Error( XBOX::VFilePath inFilePath, sLONG inLine, sLONG inOffset, const XBOX::VString& inMessage )
{
	fManager->fCompileErrorSignal.Trigger( inFilePath, inLine, inMessage );

#if VERSIONDEBUG
	VString msg("[Parsing Error] File(");
	msg.AppendString(inFilePath.GetPath());
	msg.AppendString(") Line(");
	msg.AppendLong(inLine+1);
	msg.AppendString(") Message(");
	msg.AppendString(inMessage);
	msg.AppendString(")\r\n");
	DebugMsg(msg);
#endif
}

bool WorkerThread::ParseDocument( VDocumentParserManager::WorkItem *inWorkItem, bool& outHasParsingError, bool& outContentOverwritenFromDisk, bool& outTimeStampUpdated )
{
	xbox_assert( inWorkItem );

	bool parsed = false;
	
	if( !IsDying() )
	{
		if( !inWorkItem->fFilePath.IsFolder() )
		{
			VFile file( inWorkItem->fFilePath );
			if( file.Exists() )
			{
				// Prepare time stamp
				VTime modTime;
				if( inWorkItem->fFileContents.IsEmpty() && !inWorkItem->fFileContentHasBeenSet )
					file.GetTimeAttributes( &modTime );
				else
				{
					VTime::Now( modTime );
					outTimeStampUpdated = true;
				}
				
				
				// Prepare file content to parse
				if(inWorkItem->fFileContents.IsEmpty() && !inWorkItem->fFileContentHasBeenSet)
				{
					fManager->fTaskLock->Lock();
					if ( file.Exists() )
					{
						VFileStream stream( &file );
						if (VE_OK == stream.OpenReading())
						{
							VString contents;
							stream.GuessCharSetFromLeadingBytes( VTC_DefaultTextExport );
							stream.GetText( contents );
							stream.CloseReading();
							inWorkItem->fFileContents = contents;
							outContentOverwritenFromDisk = true;
						}
					}
					fManager->fTaskLock->Unlock();
				}
				
				
				// Prepare parsing
				if( inWorkItem->fSymTable && !IsDying() )
				{
					// The first thing we should do is clone the symbol table for use with threads -- we need to do
					// this to ensure proper thread safety.
					ISymbolTable* symTable = inWorkItem->fSymTable->CloneForNewThread();
					
					Symbols::IFile *ownerFile = GetFileForDeclarationParsingUpdate( symTable, inWorkItem->fFilePath, inWorkItem->fBaseFolder, inWorkItem->fExecutionScope );
					if (ownerFile)
					{
						// Check to see whether this file has been modified since the last time we did a parsing pass
						// on it.  We just need to test the modification dates.  If the file has been modified, we want
						// to remove its old symbols from the database.  If we're working with the file contents being passed
						// in to us instead of gathered from disk, the modification time is "right now" and should always be
						// different from the modification time in the database.  This actually serves two purposes -- first is
						// that it means we will always parse declarations when handed file contents directly.  Second, if the
						// user does NOT save their changes out to disk and then terminates the application, we will reparse the file's
						// old contents because the modification stamps are different.  That is why we only compare equality instead of
						// relativity of the stamps!
						if ( (modTime.GetStamp() > ownerFile->GetModificationTimestamp()) ||	// The file has been modified and saved
							 (inWorkItem->fParsingIsMandatory) )								// The file has been modified, not saved yet and we discard modifications
						{
							// Now, figure out what parser to use and perform the actual parsing
							if( _IsJavaScriptContent(inWorkItem) )
							{
								// We need to parse the file either for saving or discarding latest modifications
								symTable->DeleteSymbolsForFile( ownerFile );

								// Notify parsing start
								fManager->GetParsingStartSignal().Trigger(inWorkItem->fFilePath);

								JavaScriptAST::Visitor* declParser = JavaScriptAST::Visitor::GetDeclarationParser( symTable, ownerFile, this );
								if( !IsDying() && declParser )
								{
									JavaScriptParser *parser = new JavaScriptParser();
									if( parser )
									{
										// We will parse by gathering an AST.  If we manage to get a valid AST back,
										// then we can see if the user wants it declaration parsed.
										parser->AssignDelegate( this );
										
										if( _IsJSONContent(inWorkItem) )
										{
											VString msg;
											
											VString content;
											_PrepareJSONContentForParsing(inWorkItem, content);
											
											inWorkItem->fFileContents = content;
										}
										
										JavaScriptAST::Node *program = parser->GetAST( inWorkItem->fFilePath, &inWorkItem->fFileContents, outHasParsingError );
										if( program )
										{
											program->Accept( declParser );

											parsed = true;
											
											delete program;
										}
										delete parser;
									}
									delete declParser;
								}
							}
							else if( _IsDataModelContent(inWorkItem) )
							{
								// We need to parse the file either for saving or discarding latest modifications
								symTable->DeleteSymbolsForFile( ownerFile );

								// Notify parsing start
								fManager->GetParsingStartSignal().Trigger(inWorkItem->fFilePath);

								VectorOfVString entityNames;
								inWorkItem->fCatalog->GetEntityModelNames( entityNames );
								
								for (VectorOfVString::iterator iter = entityNames.begin(); iter != entityNames.end(); ++iter)
								{
									std::vector<IEntityModelCatalogAttribute* >	attributes;
									std::vector<IEntityModelCatalogMethod* >	methods;
									
									inWorkItem->fCatalog->GetEntityModelAttributes( *iter, attributes );
									inWorkItem->fCatalog->GetEntityModelMethods( *iter, methods );
									
									ParseEntityModel( inWorkItem->fSymTable, ownerFile, *iter, attributes, methods );

									parsed = true;
									
									for (std::vector<IEntityModelCatalogAttribute* >::iterator it = attributes.begin(); it != attributes.end(); ++it)
										ReleaseRefCountable(&(*it));
									
									for (std::vector<IEntityModelCatalogMethod* >::iterator it = methods.begin(); it != methods.end(); ++it)
										ReleaseRefCountable(&(*it));
								}
							}
							
							if( parsed )
							{
								ownerFile->SetModificationTimestamp( modTime.GetStamp() );
								
								symTable->UpdateFile( ownerFile );
							}
						}
					}
					
					ReleaseRefCountable( &ownerFile );
					ReleaseRefCountable( &symTable );
				}				
			}
		}
	}
	
	return parsed;

}

Symbols::ISymbol *WorkerThread::GetSymbol( ISymbolTable *inTable, const VString &inSymbolName, const VString &inFileName )
{
	// Get the file we expect the symbol to reside in
	std::vector< Symbols::IFile * > files = inTable->GetFilesByName( inFileName );
	if (files.empty())	return NULL;
	Symbols::ISymbol *ret = NULL;

	// Now see if we can find the symbol
	std::vector< Symbols::ISymbol * > syms = inTable->GetSymbolsByName( NULL, inSymbolName, files.front() );
	if (!syms.empty()) {
		ret = syms.front();
		ret->Retain();
	}

	for (std::vector< Symbols::ISymbol * >::iterator iter = syms.begin(); iter != syms.end(); ++iter)	(*iter)->Release();
	for (std::vector< Symbols::IFile * >::iterator iter = files.begin(); iter != files.end(); ++iter)	(*iter)->Release();

	return ret;
}

void WorkerThread::CreateNewInstance( ISymbolTable *inTable, Symbols::ISymbol *inSym, const VString &inSymbolName, const VString &inFileName )
{
	// Get the file we expect the symbol to reside in
	std::vector< Symbols::IFile * > files = inTable->GetFilesByName( inFileName );
	if (files.empty())	return;

	// Now see if we can find the symbol
	std::vector< Symbols::ISymbol * > syms = inTable->GetSymbolsByName( NULL, inSymbolName, files.front() );
	if (!syms.empty()) {
		// Now that we have the symbol, we'll see if we can find its prototype property
		Symbols::ISymbol *temp = const_cast< Symbols::ISymbol * >( syms.front()->RetainReferencedSymbol() );
		std::vector< Symbols::ISymbol * > prototypes = inTable->GetSymbolsByName( temp, "prototype" );

		if (prototypes.empty())
			prototypes.push_back( GetSymbol( inTable, "Object", "JSCore.js" ) );

		temp->Release();
		inSym->AddPrototypes( prototypes );
		inSym->AddAuxillaryKindInformation( Symbols::ISymbol::kInstanceValue );

		for (std::vector< Symbols::ISymbol * >::iterator iter = prototypes.begin(); iter != prototypes.end(); ++iter)
			ReleaseRefCountable( &(*iter) );
	}

	for (std::vector< Symbols::ISymbol * >::iterator iter = syms.begin(); iter != syms.end(); ++iter)
		ReleaseRefCountable( &(*iter) );

	for (std::vector< Symbols::IFile * >::iterator iter = files.begin(); iter != files.end(); ++iter)
		ReleaseRefCountable( &(*iter) );
}

void WorkerThread::ParseEntityModel( ISymbolTable *inTable, Symbols::IFile *inOwnerFile, const VString &inEntityName, std::vector<IEntityModelCatalogAttribute* > &inAttributes, std::vector< IEntityModelCatalogMethod* > &inMethods )
{
	std::vector< Symbols::ISymbol * > symsToAdd;

	// First loop is intented to create an EM with no context to make 
	// the "MyEntityModel." completion work.
	for (int i = 0; i <= 1; i++)
	{
		// Now we can actually add the entity model symbol 
		// information to the table. Gin up a symbol for 
		// this entity
		VSymbol *entitySym = new VSymbol();
		VSymbol *constructor = NULL; 

		entitySym->SetID( inTable->GetNextUniqueID() );
		entitySym->SetName( inEntityName );
		entitySym->SetFile( inOwnerFile );
		entitySym->SetLineNumber( -1 );
		entitySym->SetLineCompletionNumber( -1 );

		if ( !i )
		{
			entitySym->SetKind( Symbols::ISymbol::kKindClass | Symbols::ISymbol::kKindEntityModel );

			constructor = new VSymbol();
			constructor->SetID( inTable->GetNextUniqueID() );
			constructor->SetName( entitySym->GetName() );
			constructor->SetOwner( entitySym );
			constructor->SetFile( inOwnerFile );
			constructor->SetLineNumber( -1 );
			constructor->SetLineCompletionNumber( -1 );
			constructor->SetKind( Symbols::ISymbol::kKindClassConstructor );
		} 
		else
		{
			// Entity models are public properties of the "ds" object, so let's load that symbol up
			Symbols::ISymbol *dsSym = GetSymbol( inTable, CVSTR( "ds" ), CVSTR( "application.js" ) );
			
			if (dsSym)
			{
				Symbols::ISymbol *refDsSym = const_cast< Symbols::ISymbol * >( dsSym->RetainReferencedSymbol() );
				entitySym->SetOwner( refDsSym );
				entitySym->SetKind( Symbols::ISymbol::kKindPublicProperty | Symbols::ISymbol::kKindEntityModel );
				dsSym->Release();
				refDsSym->Release();
			}
		}

		CreateNewInstance( inTable, entitySym, CVSTR( "dataClass" ), CVSTR( "datastore.js" ) );

		symsToAdd.push_back( entitySym );
		if ( NULL != constructor )
			symsToAdd.push_back( constructor );

		// prototype creation
		VSymbol *entityPrototype = new VSymbol();
		entityPrototype->SetID( inTable->GetNextUniqueID() );
		entityPrototype->SetName( "prototype" );
		entityPrototype->SetOwner( entitySym );
		entityPrototype->SetFile( inOwnerFile );
		entityPrototype->SetLineNumber( entitySym->GetLineNumber() );
		entityPrototype->SetKind( Symbols::ISymbol::kKindPublicProperty );

		symsToAdd.push_back( entityPrototype );

		// If there are attributes to handle, we can parse those next
		for (std::vector<IEntityModelCatalogAttribute* >::iterator iter = inAttributes.begin(); iter != inAttributes.end(); ++iter)
		{
			VString name, type;

			(*iter)->GetName(name);
			(*iter)->GetType(type);

			VSymbol *attrib = new VSymbol();
			attrib->SetID( inTable->GetNextUniqueID() );
			attrib->SetName( name );
			attrib->SetKind( Symbols::ISymbol::kKindPublicProperty | Symbols::ISymbol::kKindEntityModelAttribute );
			attrib->SetFile( inOwnerFile );
			attrib->SetLineNumber( -1 );
			attrib->SetLineCompletionNumber( -1 );
			attrib->SetOwner( entityPrototype );

			// Affect the right type to the symbol
			if ( type.GetLength() )
			{
				for (int count = 0; count < ENTITY_MODEL_ATTR_TYPE_COUNT; count++)
				{
					if ( ENTITY_MODEL_ATTR_TYPES[count].type == type)
					{
						VString jsType = ENTITY_MODEL_ATTR_TYPES[count].jsType;
						VString jsFile = ENTITY_MODEL_ATTR_TYPES[count].jsFile;

						if ( jsType.GetLength() && jsFile.GetLength() )
							CreateNewInstance( inTable, attrib, jsType, jsFile);

						break;
					}
				}
			}
			
			symsToAdd.push_back( attrib );

			// Create a static attribute of EntityAttribute type. Usefull for static completion
			VSymbol *staticAttrib = new VSymbol();
			staticAttrib->SetID( inTable->GetNextUniqueID() );
			staticAttrib->SetName( name );
			staticAttrib->SetKind( Symbols::ISymbol::kKindStaticProperty | Symbols::ISymbol::kKindEntityModelAttribute );
			staticAttrib->SetFile( inOwnerFile );
			staticAttrib->SetLineNumber( -1 );
			staticAttrib->SetLineCompletionNumber( -1 );
			staticAttrib->SetOwner( entitySym );

			// EM static attributes were Object (fix WAK0074165). Now They are EntityAttribute.
			CreateNewInstance( inTable, staticAttrib, CVSTR( "EntityAttribute" ), CVSTR( "datastore.js" ) );
			symsToAdd.push_back( staticAttrib );
		}

		// If there are methods to handle, we can parse those next
		for (std::vector< IEntityModelCatalogMethod* >::iterator iter = inMethods.begin(); iter != inMethods.end(); ++iter)
		{
			// Functions are a bit special in that they're actually two symbols.  One symbol is the anonymous
			// function itself.  The other symbol is the named property which is assigned the anonymous function.
			// So this looks very convoluted because it is.

			// Create the function symbol
			VSymbol *sym = new VSymbol();
			sym->SetID( inTable->GetNextUniqueID() );
			sym->SetFile( inOwnerFile );
			sym->SetLineNumber( -1 );
			sym->SetLineCompletionNumber( -1 );
			sym->SetKind( Symbols::ISymbol::kKindFunctionDeclaration );
			symsToAdd.push_back( sym );

			// Functions get an external prototype property as well as an internal one.
			VSymbol *prototype = new VSymbol();
			prototype->SetID( inTable->GetNextUniqueID() );
			prototype->SetName( "prototype" );
			prototype->SetOwner( sym );
			prototype->SetFile( inOwnerFile );
			prototype->SetLineNumber( -1 );
			prototype->SetKind( Symbols::ISymbol::kKindPublicProperty );
			symsToAdd.push_back( prototype );

			// The external prototype property is always of type Object by default, as though it
			// were set via a call to new Object();
			CreateNewInstance( inTable, prototype, CVSTR( "Object" ), CVSTR( "JSCore.js" ) );

			// Functions also have to set up their internal prototype, which points to Function
			// as though it were a call to new Function();
			CreateNewInstance( inTable, sym, CVSTR( "Function" ), CVSTR( "JSCore.js" ) );

			VString name, applyTo;
			sLONG	methodType = 0;

			(*iter)->GetName(name);
			(*iter)->GetApplyTo(applyTo);

			if ( applyTo == CVSTR("entity") )
				methodType = Symbols::ISymbol::kKindClassPublicMethod | Symbols::ISymbol::kKindEntityModelMethodEntity;
			else if ( applyTo == CVSTR("entityCollection") )
				methodType = Symbols::ISymbol::kKindClassPublicMethod | Symbols::ISymbol::kKindEntityModelMethodEntityCollection;
			else if ( applyTo == CVSTR("dataClass") )
				methodType = Symbols::ISymbol::kKindClassStaticMethod | Symbols::ISymbol::kKindEntityModelMethodDataClass;

			VSymbol *method = new VSymbol();

			method->SetID( inTable->GetNextUniqueID() );
			method->SetName( name );
			method->SetKind( methodType );
			method->SetFile( inOwnerFile );
			method->SetLineNumber( -1 );
			method->SetLineCompletionNumber( -1 );
			method->SetOwner( applyTo == CVSTR("dataClass") ? entitySym : entityPrototype );
			method->SetReferenceSymbol( sym );	// This property now references the function symbol

			symsToAdd.push_back( method );
		}
	}

	if (!symsToAdd.empty())
		inTable->AddSymbols( symsToAdd );
}

void WorkerThread::ExecuteTask()
{
	// This code is executing within the context of the worker thread, and is responsible for dequeing items
	// from the work queue and parsing them.
	do {
		// If we don't have any items available on the worker queue, then we want to freeze the worker thread until
		// some other work comes in
		bool bQueueEmpty = false;
		fManager->fTaskLock->Lock();

		// If the reason we just woke up is that the thread is now dying, we want to bail out immediately.  This
		// can happen when the thread is put to sleep because there are no more documents to parse, but is woken up
		// because the application is quitting.
		if (IsDying()) {
			fManager->fTaskLock->Unlock();
			break;
		}

		bQueueEmpty = fManager->fWorkQueue.empty();
		if (bQueueEmpty) {
			fQueueSync->Reset();	// This makes it available to lock again
		} else {
			// Get an item from the work queue.  We pull from the end of the queue because that's where the highest 
			// priority items live.
			fCurrentWorkItem = fManager->fWorkQueue.back();
			fManager->fWorkQueue.pop_back();
			fWaitingForWork = false;	// We have some work to do
		}
		fManager->fTaskLock->Unlock();

		if (bQueueEmpty) {
			// We're waiting for something to do now!
			fWaitingForWork = true;
			fQueueSync->Lock();		// This causes us to block until the sync mechanism is signaled
			// Try to grab another item
			continue;
		} else if (IsDying()) {
			break;
		}


		if (fCurrentWorkItem != NULL)
		{
			// Now that we have a work item, it's time to start processing it
			{
				// VTaskLock is a scoped lock
				// Enclosing it in a local scope ensure us to unlock it in any case
				VTaskLock lock(fManager->GetLockForParseOrOutline(fCurrentWorkItem->fFilePath));
				bool hasParsingErrors = false;
				bool hasContentBeenOverwritenFromDisk = false;
				bool hasTimeStampBeenUpdated = false;
#if VERSIONDEBUG
				uLONG tStart = XBOX::VSystem::GetCurrentTime();
#endif
				if( !ParseDocument(fCurrentWorkItem, hasParsingErrors, hasContentBeenOverwritenFromDisk, hasTimeStampBeenUpdated) )
				{
#if VERSIONDEBUG
					uLONG tStop = XBOX::VSystem::GetCurrentTime();
					VString msg("[Parsing Discarded] File(");
					msg.AppendString(fCurrentWorkItem->fFilePath.GetPath());
					msg.AppendString(") Length(");
					msg.AppendLong(fCurrentWorkItem->fFileContents.GetLength());
					msg.AppendString(") Duration(");
					msg.AppendLong(tStop-tStart);
					msg.AppendString("ms)\r\n");
					DebugMsg(msg);
#endif
				}
				else
				{
#if VERSIONDEBUG
					uLONG tStop = XBOX::VSystem::GetCurrentTime();
					VString msg("[Parsing Done] File(");
					msg.AppendString(fCurrentWorkItem->fFilePath.GetPath());
					msg.AppendString(") ContentSet(");
					msg.AppendLong(fCurrentWorkItem->fFileContentHasBeenSet);
					msg.AppendString(") Length(");
					msg.AppendLong(fCurrentWorkItem->fFileContents.GetLength());
					msg.AppendString(") ContentOverwritenFromDisk(");
					msg.AppendLong(hasContentBeenOverwritenFromDisk);
					msg.AppendString(") ParsingMandatory(");
					msg.AppendLong(fCurrentWorkItem->fParsingIsMandatory);
					msg.AppendString(") TimeStampUpdated(");
					msg.AppendLong(hasTimeStampBeenUpdated);
					msg.AppendString(") Duration(");
					msg.AppendLong(tStop-tStart);
					msg.AppendString("ms)\r\n");
					DebugMsg(msg);
#endif
					// Trigger the event only if the item has been parsed
					fManager->fParsingCompleteSignal.Trigger( fCurrentWorkItem->fFilePath, hasParsingErrors );
				}
			}
			
			// Now that parsing is over, we can fire the completed event
			fManager->DecrementFilesToParseCounter();

			// If we have a job object, update its internal counter and possibly fire the job complete notification
			if (fCurrentWorkItem->fJob)
				static_cast< VJobObject * >( fCurrentWorkItem->fJob )->TaskComplete();

			// Now that we've finished, we're able to destroy the work item
			fManager->fTaskLock->Lock();
			fCurrentWorkItem->CleanUp();
			delete fCurrentWorkItem;
			fCurrentWorkItem = NULL;
			fManager->fTaskLock->Unlock();
		}
	} while (!IsDying());
}


VLanguageSyntaxComponent* gLanguageSyntax = NULL;

VLanguageSyntaxComponent::VLanguageSyntaxComponent() : fJavaScriptSyntax( NULL ),
	fXMLSyntax( NULL ), fHTMLSyntax( NULL ), fCSSSyntax( NULL ), fSQLSyntax( NULL ), fPHPSyntax( NULL )
{
	gLanguageSyntax = this;
	InitLocalizationManager();

	fJavaScriptSyntax = new VJavaScriptSyntax();
	fXMLSyntax = new VXMLSyntax();
	fHTMLSyntax = new VHTMLSyntax();
	fCSSSyntax = new VCSSSyntax();
	fSQLSyntax =  new VSQLSyntax();
	fPHPSyntax = new VPHPSyntax();
}

VLanguageSyntaxComponent::~VLanguageSyntaxComponent()
{
	delete fJavaScriptSyntax;
	delete fXMLSyntax;
	delete fHTMLSyntax;
	delete fCSSSyntax;
	delete fSQLSyntax;
	delete fPHPSyntax;

	ClearLocalizationManager();
}

VJSObject VLanguageSyntaxComponent::CreateJavaScriptTestObject( XBOX::VJSContext inJSContext )
{
	VJSObject ret = VJSLanguageSyntaxTester::CreateInstance( inJSContext, NULL );

	return ret;
}

void VLanguageSyntaxComponent::SetBreakPointManager( ISyntaxInterface* inSyntax, IBreakPointManager* inBreakPointManager )
{
	VJavaScriptSyntax* syntax = dynamic_cast<VJavaScriptSyntax*>( inSyntax );
	if ( NULL != syntax )
	{
		syntax->SetBreakPointManager( inBreakPointManager );
	}
}

void VLanguageSyntaxComponent::SetSQLTokenizer ( SQLTokenizeFuncPtr inPtr, const std::vector< XBOX::VString * >& vctrSQLKeywords, const std::vector< XBOX::VString * >& vctrSQLFunctions )
{
	if ( testAssert ( fSQLSyntax != NULL ) )
	{
		fSQLSyntax-> SetSQLTokenizer ( inPtr, vctrSQLKeywords, vctrSQLFunctions );
	}
}

IDocumentParserManager *VLanguageSyntaxComponent::CreateDocumentParserManager()
{
	return new VDocumentParserManager();
}

ISymbolTable *VLanguageSyntaxComponent::CreateSymbolTable()
{
	return NewSymbolTable();
}


bool VLanguageSyntaxComponent::ParseScriptDocComment( const VString &inComment, std::vector< IScriptDocCommentField * > &outFields )
{
	ScriptDocComment *sdoc = ScriptDocComment::Create( inComment );
	if (sdoc)
	{
		for (sLONG i = 0; i < sdoc->ElementCount(); i++)
		{
			ScriptDocLexer::Element *element = sdoc->GetElement( i );
			element->Retain();
			outFields.push_back( element );
		}
		sdoc->Release();
		return true;
	}

	return false;
}

//initialize localization manager
void VLanguageSyntaxComponent::InitLocalizationManager()
{
	fComponentFolder = gLanguageSyntaxCompLib->GetLibrary()->RetainFolder(kBF_BUNDLE_FOLDER);
	if (fComponentFolder != NULL)
	{
		fDefaultLocalization = new VLocalizationManager(VComponentManager::GetLocalizationLanguage(fComponentFolder,true));
		fDefaultLocalization->LoadDefaultLocalizationFoldersForComponentOrPlugin(fComponentFolder);
		VErrorBase::RegisterLocalizer(CLanguageSyntaxComponent::Component_Type, fDefaultLocalization);
	}
}

//clear localization manager
void VLanguageSyntaxComponent::ClearLocalizationManager()
{
	if (fDefaultLocalization != NULL)
		fDefaultLocalization->Release();
	ReleaseRefCountable(&fComponentFolder);
}


//localization manager accessor
VLocalizationManager* VLanguageSyntaxComponent::GetLocalizationMgr()
{
	return fDefaultLocalization;
}

ISyntaxInterface* VLanguageSyntaxComponent::GetSyntaxByExtension( const VString& inExtension )
{	
	if (inExtension == JAVASCRIPT_EXTENSION )
		 {
			return fJavaScriptSyntax;
		 }
	else if (inExtension == PHP_EXTENSION )
	     {
		    return fPHPSyntax;
	     }
	else if  ( inExtension == SQL_EXTENSION )
		 {
			return fSQLSyntax;
         }
	else if  ( inExtension == XML_EXTENSION )
	     {
			return fXMLSyntax;                       //for debug
		 }
	else if ( inExtension == HTML_EXTENSION )
		 {
			return fHTMLSyntax;
		 }
	else if ( inExtension == HTM_EXTENSION )
	{
			return fHTMLSyntax;
	}
	else if ( inExtension == CSS_EXTENSION )	
		{
			return fCSSSyntax;
		}
	else
			return NULL;
	
}

void VLanguageSyntaxComponent::Stop()
{
	// We are going to stop the JS syntax engine by deleting it because it holds on to a
	// document parser manager.  Anything which does so should be deleted in the call to Stop
	// so the parser manager has a chance to clean up its threads properly before the application
	// terminates.
	if (fJavaScriptSyntax)	delete fJavaScriptSyntax;
	fJavaScriptSyntax = NULL;
}
