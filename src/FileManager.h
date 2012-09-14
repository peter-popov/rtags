#ifndef FileManager_h
#define FileManager_h

#include "Path.h"
#include "List.h"
#include "FileSystemWatcher.h"
#include "Mutex.h"
#include "Location.h"

class Project;
class FileManager
{
public:
    FileManager();
    void init(const shared_ptr<Project> &proj);
    void recurseDirs();
    void onFileAdded(const Path &path);
    void onFileRemoved(const Path &path);
    void onRecurseJobFinished(const Set<Path> &mPaths);
private:
    FileSystemWatcher mWatcher;
    weak_ptr<Project> mProject;
};

#endif