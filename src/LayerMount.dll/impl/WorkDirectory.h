#pragma once

#include "LayerMount.h"
#include "ScopedHandle.h"

#include <memory>
#include <string>

namespace LayerMount {

// The directory in the work directory where the engine builds an entry
// before one rename moves it into the upper: <workDirPath>\work.
std::wstring StagingAreaPath(const std::wstring& workDirPath);

// The work directory of one live overlay. The object holds two lock files,
// each opened with no sharing and deleted on close: one in the work
// directory, and one in <upperPath>\.overlay. While this overlay lives, a
// second overlay can use neither its work directory nor its upper. The
// kernel closes the locks when the process ends.
class WorkDirectory {
public:
    // Refuses a layout in which the work directory and the upper are the
    // same directory or one contains the other, unless the work directory
    // is <upperPath>\.overlay. Refuses the same between the work directory
    // and each lower, with no exception. Compares the paths as
    // ComparablePath gives them, so no directory has to exist. Returns
    // E_INVALIDARG with error naming the work directory, or the lower, and
    // S_OK otherwise.
    static HRESULT CheckLayout(const LayerConfig& config, std::wstring& error);

    // Checks the layout as CheckLayout does, on the final paths of the
    // directories, so a junction, a substituted drive or a short name
    // cannot hide an overlap. A pair with a path that cannot be opened is
    // compared as CheckLayout compares it. Then takes the lock of
    // config.workDirPath and the lock of config.upperPath, creating
    // <upperPath>\.overlay when it is missing. Then deletes the staging
    // area with everything in it and creates it empty. The delete uses
    // backup semantics, clears the read-only attribute, and deletes a link
    // or a mounted folder itself without entering it. It enters a directory
    // reparse point that is not a link, such as a cloud placeholder. It
    // deletes an entry whose DACL refuses the delete or the listing when
    // the entry's owner is in the thread's token. It removes the sidecar
    // records of each entry it deletes. The other entries of the work
    // directory stay. The work directory must exist.
    //
    // Fails with E_INVALIDARG when the layout is refused. Fails with
    // HRESULT_FROM_WIN32(ERROR_BUSY) when another overlay holds either
    // lock. Fails with the Win32 error as an HRESULT when a lock cannot be
    // taken, when an entry in the staging area cannot be deleted or when
    // the staging area cannot be created. error then names the directory
    // or the entry, and *workDirectory stays empty.
    static HRESULT Open(const LayerConfig& config,
                        std::unique_ptr<WorkDirectory>* workDirectory,
                        std::wstring& error);

    WorkDirectory(const WorkDirectory&) = delete;
    WorkDirectory& operator=(const WorkDirectory&) = delete;

private:
    WorkDirectory(ScopedHandle workDirectoryLock, ScopedHandle upperLock);

    ScopedHandle workDirectoryLock_;
    ScopedHandle upperLock_;
};

}
