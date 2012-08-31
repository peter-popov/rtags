#include "Indexer.h"

#include "ValidateDBJob.h"
#include "Database.h"
#include "FileInformation.h"
#include "IndexerJob.h"
#include "Log.h"
#include "MemoryMonitor.h"
#include "Path.h"
#include "RTags.h"
#include "ReadLocker.h"
#include "RegExp.h"
#include "Server.h"
#include "WriteLocker.h"
#include <math.h>

Indexer::Indexer()
    : mJobCounter(0), mModifiedFilesTimerId(-1), mTimerRunning(false), mValidate(false)
{
}

void Indexer::init(const shared_ptr<Project> &proj, bool validate)
{
    mValidate = validate;
    mProject = proj;
    mWatcher.modified().connect(this, &Indexer::onFileModified);
    {
        // watcher
        ScopedDB db = proj->db(Project::Dependency, ReadWriteLock::Read);
        RTags::Ptr<Iterator> it(db->createIterator());
        it->seekToFirst();
        DependencyMap dependencies;
        while (it->isValid()) {
            const Slice key = it->key();
            const uint32_t fileId = *reinterpret_cast<const uint32_t*>(key.data());
            const Set<uint32_t> deps = it->value<Set<uint32_t> >();
            dependencies[fileId] = deps;
            it->next();
        }
        MutexLocker lock(&mMutex);
        commitDependencies(dependencies, false);
    }

    initDB(validate ? Normal : NoValidate);
}

Indexer::~Indexer()
{
}

static inline bool isFile(uint32_t fileId)
{
    return Location::path(fileId).isFile();
}

void Indexer::initDB(InitMode mode, const ByteArray &pattern)
{
    shared_ptr<Project> proj = project();
    assert(proj);
    Set<uint32_t> dirtyFiles;
    Map<Path, List<ByteArray> > toIndex;
    {
        MutexLocker lock(&mMutex);
        assert(mode == ForceDirty || pattern.isEmpty());
        Timer timer;
        Map<uint32_t, Set<uint32_t> > deps, depsReversed;
        RTags::Ptr<Iterator> it;
        {
            ScopedDB dependencyDB = proj->db(Project::Dependency, ReadWriteLock::Write);
            it.reset(dependencyDB->createIterator());
            it->seekToFirst();
            {
                while (it->isValid()) {
                    const Slice key = it->key();
                    const uint32_t file = *reinterpret_cast<const uint32_t*>(key.data());
                    if (isFile(file)) {
                        const Set<uint32_t> v = it->value<Set<uint32_t> >();
                        depsReversed[file] = v;
                        for (Set<uint32_t>::const_iterator vit = v.begin(); vit != v.end(); ++vit) {
                            deps[*vit].insert(file);
                        }
                    } else {
                        dependencyDB->remove(key);
                    }
                    it->next();
                }
            }
        }

        int checked = 0;
        {
            ScopedDB fileInformationDB = proj->db(Project::FileInformation, ReadWriteLock::Write);
            Batch batch(fileInformationDB);
            it.reset(fileInformationDB->createIterator());
            it->seekToFirst();
            RegExp rx(pattern);
            while (it->isValid()) {
                const Slice key = it->key();
                const uint32_t fileId = *reinterpret_cast<const uint32_t*>(key.data());
                assert(key.size() == 4);
                const Path path = Location::path(fileId);
                if (path.isFile()) {
                    const FileInformation fi = it->value<FileInformation>();
                    if (!fi.compileArgs.isEmpty()) {
                        ++checked;
                        bool dirty = false;
                        Set<uint32_t> dependencies = deps.value(fileId);
                        if (!dependencies.contains(fileId)) {
                            error() << Location::path(fileId) << " doesn't depend on itself ";
                            dependencies.insert(fileId);
                        }
                        if (mode != NoValidate) {
                            for (Set<uint32_t>::const_iterator it = dependencies.begin(); it != dependencies.end(); ++it) {
                                const uint32_t id = *it;
                                if (dirtyFiles.contains(id)) {
                                    dirty = true;
                                } else {
                                    const Path p = Location::path(id);
                                    bool pdirty = false;
                                    switch (mode) {
                                    case NoValidate:
                                        assert(0);
                                        break;
                                    case ForceDirty:
                                        if (pattern.isEmpty() || rx.indexIn(p) != -1) {
                                            pdirty = true;
                                            break;
                                        }
                                        // fall through
                                    case Normal:
                                        pdirty = (p.lastModified() > fi.lastTouched);
                                        break;
                                    }
                                    if (pdirty) {
                                        dirty = true;
                                        dirtyFiles.insert(id);
                                        dirtyFiles += depsReversed.value(id);
                                    }
                                }
                            }
                        }
                        if (dirty) {
                            toIndex[path] = fi.compileArgs;
                        } else {
                            mVisitedFiles += dependencies;
                        }
                        warning() << "checking if " << path << " is dirty => " << dirty;
                    }
                } else {
                    batch.remove(key);
                }
                it->next();
            }
        }

        if (checked)
            error() << proj->srcRoot << ": Checked " << checked << " files. Found " << dirtyFiles.size() << " dirty files and "
                    << toIndex.size() << " sources to reindex in " << timer.elapsed() << "ms";

        assert(dirtyFiles.isEmpty() == toIndex.isEmpty());

    }
    if (!dirtyFiles.isEmpty()) {
        dirty(dirtyFiles, toIndex);
    }
}

void Indexer::commitDependencies(const DependencyMap &deps, bool sync) // always called with mMutex held
{
    DependencyMap newDependencies;

    if (mDependencies.isEmpty()) {
        mDependencies = deps;
        newDependencies = deps;
    } else {
        const DependencyMap::const_iterator end = deps.end();
        for (DependencyMap::const_iterator it = deps.begin(); it != end; ++it) {
            newDependencies[it->first].unite(it->second - mDependencies[it->first]);
            DependencyMap::iterator i = newDependencies.find(it->first);
            if (i->second.isEmpty())
                newDependencies.erase(i);
            mDependencies[it->first].unite(it->second);
        }
    }
    shared_ptr<Project> proj = project();
    if (sync && !newDependencies.isEmpty()) {
        ScopedDB db = proj->db(Project::Dependency, ReadWriteLock::Write);
        Batch batch(db);
        DependencyMap::const_iterator it = newDependencies.begin();
        const DependencyMap::const_iterator end = newDependencies.end();
        char buf[4];
        const Slice key(buf, 4);
        while (it != end) {
            memcpy(buf, &it->first, sizeof(buf));
            Set<uint32_t> added = it->second;
            Set<uint32_t> current = db->value<Set<uint32_t> >(key);
            const int oldSize = current.size();
            if (current.unite(added).size() > oldSize) {
                batch.add(key, current);
            }
            ++it;
        }
    }

    Path parentPath;
    Set<ByteArray> watchPaths;
    const DependencyMap::const_iterator end = newDependencies.end();
    for (DependencyMap::const_iterator it = newDependencies.begin(); it != end; ++it) {
        const Path path = Location::path(it->first);
        parentPath = path.parentDir();
        if (mWatchedPaths.insert(parentPath)) {
            mWatcher.watch(parentPath);
        }
    }
}

void Indexer::onJobFinished(IndexerJob *job)
{
    MutexLocker lock(&mMutex);
    const uint32_t fileId = job->fileId();
    mJobs.remove(fileId);
    ByteArray message = job->message();
    if (job->isAborted()) {
        const Set<uint32_t> visited = job->visitedFiles();
        mVisitedFiles -= visited;

        shared_ptr<Project> proj = project();

        message += job->path() + " Aborted";

        // IndexerJob* waiting;
        // if (mWaiting.remove(fileId, &waiting)) {
        //     startJob(waiting);
        // }
        lock.unlock();

        ScopedDB symbols = proj->db(Project::Symbol, ReadWriteLock::Write);
        ScopedDB symbolNames = proj->db(Project::SymbolName, ReadWriteLock::Write);
        RTags::dirtySymbols(symbols, visited);
        RTags::dirtySymbolNames(symbolNames, visited);
    }

    const int idx = mJobCounter - mJobs.size();

    error("[%3d%%] %d/%d %s %s. Pending jobs %d. %d mb mem.",
          static_cast<int>(round((double(idx) / double(mJobCounter)) * 100.0)), idx, mJobCounter,
          RTags::timeToString(time(0), RTags::Time).constData(),
          message.constData(), mJobs.size(), int((MemoryMonitor::usage() / (1024 * 1024))));

    if (mJobs.isEmpty()) {
        assert(mTimerRunning);
        mTimerRunning = false;
        error() << "jobs took " << ((double)(mTimer.elapsed()) / 1000.0) << " secs, using "
                << MemoryMonitor::usage() / (1024.0 * 1024.0) << " mb of memory";
        mJobCounter = 0;
        jobsComplete()(this);
        if (mValidate) {
            ValidateDBJob *validateJob = new ValidateDBJob(project(), mPreviousErrors);
            validateJob->errors().connect(this, &Indexer::onValidateDBJobErrors);
            Server::instance()->startJob(validateJob);
        }
    }
    mWaitCondition.wakeAll();
}

void Indexer::index(const Path &input, const List<ByteArray> &arguments, unsigned indexerJobFlags,
                    const Set<uint32_t> &dirtyFiles, const Map<Path, List<ByteArray> > &pending)
{
    MutexLocker locker(&mMutex);

    const uint32_t fileId = Location::insertFile(input);
    IndexerJob *&job = mJobs[fileId];
    if (job && job->restart(time(0), dirtyFiles, pending))
        return;

    job = new IndexerJob(this, indexerJobFlags, input, arguments, dirtyFiles, pending);
    if (!dirtyFiles.isEmpty() || !pending.isEmpty())
        mVisitedFiles -= dirtyFiles;

    job->finished().connect(this, &Indexer::onJobFinished);

    ++mJobCounter;
    if (!mTimerRunning) {
        mTimerRunning = true;
        mTimer.start();
    }

    Server::instance()->threadPool()->start(job, job->priority());
}

void Indexer::onFileModified(const Path &file)
{
    // error() << file << "was modified";
    const uint32_t fileId = Location::fileId(file);
    if (!fileId)
        return;
    mModifiedFiles.insert(fileId);
    if (mModifiedFilesTimerId != -1) {
        EventLoop::instance()->removeTimer(mModifiedFilesTimerId);
        mModifiedFilesTimerId = -1;
    }
    enum { Timeout = 100 };
    mModifiedFilesTimerId = EventLoop::instance()->addTimer(Timeout, &Indexer::onFilesModifiedTimeout, this);
}

void Indexer::addDependencies(const DependencyMap &deps)
{
    MutexLocker lock(&mMutex);
    commitDependencies(deps, true);
}

Set<uint32_t> Indexer::dependencies(uint32_t fileId) const
{
    MutexLocker lock(&mMutex);
    return mDependencies.value(fileId);
}

void Indexer::abort()
{
    MutexLocker lock(&mMutex);

    if (!mJobs.isEmpty()) {
        for (Map<uint32_t, IndexerJob*>::const_iterator it = mJobs.begin(); it != mJobs.end(); ++it) {
            it->second->abort();
        }
        while (!mJobs.isEmpty()) {
            mWaitCondition.wait(&mMutex);
        }
    }
}

ByteArray Indexer::fixIts(const Path &path) const
{
    uint32_t fileId = Location::fileId(path);
    if (!fileId)
        return ByteArray();
    MutexLocker lock(&mMutex);
    Map<Location, std::pair<int, ByteArray> >::const_iterator it = mFixIts.lower_bound(Location(fileId, 0));
    ByteArray ret;
    char buf[1024];
    while (it != mFixIts.end() && it->first.fileId() == fileId) {
        int w;
        if ((*it).second.first) {
            w = snprintf(buf, sizeof(buf), "%d-%d %s%s", it->first.offset(), (*it).second.first,
                         (*it).second.second.constData(), ret.isEmpty() ? "" : "\n");
        } else {
            w = snprintf(buf, sizeof(buf), "%d %s%s", it->first.offset(),
                         (*it).second.second.constData(), ret.isEmpty() ? "" : "\n");
        }
        ret.prepend(ByteArray(buf, w)); // we want the last ones front()
        ++it;
    }
    return ret;
}

ByteArray Indexer::errors(const Path &path) const
{
    uint32_t fileId = Location::fileId(path);
    if (!fileId)
        return ByteArray();
    MutexLocker lock(&mMutex);
    return mErrors.value(fileId);
}


void Indexer::setDiagnostics(const Map<uint32_t, List<ByteArray> > &diagnostics,
                             const Map<Location, std::pair<int, ByteArray> > &fixIts)
{
    MutexLocker lock(&mMutex);

    for (Map<uint32_t, List<ByteArray> >::const_iterator it = diagnostics.begin(); it != diagnostics.end(); ++it) {
        const uint32_t fileId = it->first;
        Map<Location, std::pair<int, ByteArray> >::iterator i = mFixIts.lower_bound(Location(fileId, 0));
        while (i != mFixIts.end() && i->first.fileId() == fileId) {
            mFixIts.erase(i++);
        }
        if (it->second.isEmpty()) {
            mErrors.remove(it->first);
        } else {
            mErrors[it->first] = ByteArray::join(it->second, "\n");
        }
    }
    for (Map<Location, std::pair<int, ByteArray> >::const_iterator it = fixIts.begin(); it != fixIts.end(); ++it) {
        mFixIts[it->first] = (*it).second;
    }
}

void Indexer::reindex(const ByteArray &pattern)
{
    initDB(ForceDirty, pattern);
}
void Indexer::onValidateDBJobErrors(const Set<Location> &errors)
{
    MutexLocker lock(&mMutex);
    mPreviousErrors = errors;
}

void Indexer::dirty(const Set<uint32_t> &dirtyFileIds,
                    const Map<Path, List<ByteArray> > &dirty)
{
    {
        MutexLocker lock(&mMutex);
        mVisitedFiles -= dirtyFileIds;
    }
    shared_ptr<Project> proj = project();
    ScopedDB symbols = proj->db(Project::Symbol, ReadWriteLock::Write);
    ScopedDB symbolNames = proj->db(Project::SymbolName, ReadWriteLock::Write);
    RTags::dirtySymbols(symbols, dirtyFileIds);
    RTags::dirtySymbolNames(symbolNames, dirtyFileIds);

    for (Map<Path, List<ByteArray> >::const_iterator it = dirty.begin(); it != dirty.end(); ++it) {
        index(it->first, it->second, IndexerJob::Dirty);
    }
}
void Indexer::onFilesModifiedTimeout()
{
    Set<uint32_t> dirtyFiles;
    Map<Path, List<ByteArray> > toIndex;
    {
        MutexLocker lock(&mMutex);
        for (Set<uint32_t>::const_iterator it = mModifiedFiles.begin(); it != mModifiedFiles.end(); ++it) {
            dirtyFiles.insert(*it);
            dirtyFiles.unite(mDependencies.at(*it));
        }
        error() << mModifiedFiles << dirtyFiles;
        mModifiedFiles.clear();
        ScopedDB db = project()->db(Project::FileInformation, ReadWriteLock::Read);
        bool ok;
        char buf[4];
        for (Set<uint32_t>::const_iterator it = dirtyFiles.begin(); it != dirtyFiles.end(); ++it) {
            const uint32_t id = *it;
            memcpy(buf, &id, sizeof(buf));
            const FileInformation fi = db->value<FileInformation>(Slice(buf, sizeof(buf)), &ok);
            if (ok) {
                const Path path = Location::path(id);
                toIndex[path] = fi.compileArgs;
            }
        }
    }
    if (!toIndex.isEmpty()) {
        const Path src = toIndex.begin()->first;
        const List<ByteArray> args = toIndex.begin()->second;
        toIndex.erase(toIndex.begin());
        index(src, args, IndexerJob::Dirty, dirtyFiles, toIndex);
    }
}
