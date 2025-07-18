// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "pal_compiler.h"
#include "pal_config.h"
#include "pal_errno.h"
#include "pal_io.h"
#include "pal_utilities.h"
#include "pal_safecrt.h"
#include "pal_types.h"

#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <fnmatch.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#if !HAVE_MAKEDEV_FILEH && HAVE_MAKEDEV_SYSMACROSH
#include <sys/sysmacros.h>
#endif
#include <sys/uio.h>
#if HAVE_SYSLOG_H
#include <syslog.h>
#endif
#if HAVE_TERMIOS_H
#include <termios.h>
#endif
#include <unistd.h>
#include <limits.h>
#if HAVE_FCOPYFILE
#include <copyfile.h>
#elif HAVE_SENDFILE_4
#include <sys/sendfile.h>
#endif
#if HAVE_INOTIFY
#include <sys/inotify.h>
#endif
#if HAVE_STATFS_VFS // Linux
#include <sys/vfs.h>
#elif HAVE_STATFS_MOUNT // BSD
#include <sys/mount.h>
#elif HAVE_SYS_STATVFS_H && !HAVE_NON_LEGACY_STATFS // SunOS
#include <sys/types.h>
#include <sys/statvfs.h>
#if HAVE_STATFS_VFS
#include <sys/vfs.h>
#endif
#endif

#ifdef TARGET_SUNOS
#include <sys/param.h>
#endif

#ifdef _AIX
#include <alloca.h>
// Somehow, AIX mangles the definition for this behind a C++ def
// Redeclare it here
extern int     getpeereid(int, uid_t *__restrict__, gid_t *__restrict__);
#endif

#if defined(TARGET_SUNOS)
#include <procfs.h>
#endif

#ifdef __linux__
#include <sys/utsname.h>

// Ensure FICLONE is defined for all Linux builds.
#ifndef FICLONE
#define FICLONE _IOW(0x94, 9, int)
#endif /* __linux__ */

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreserved-id-macro"
// Ensure __NR_copy_file_range is defined for portable builds.
#include <sys/syscall.h> // __NR_copy_file_range
# if !defined(__NR_copy_file_range)
#  if defined(__amd64__)
#   define __NR_copy_file_range  326
#  elif defined(__i386__)
#   define __NR_copy_file_range  377
#  elif defined(__arm__)
#   define __NR_copy_file_range  391
#  elif defined(__aarch64__)
#   define __NR_copy_file_range  285
#  else
#   error Unknown architecture
#  endif
# endif
#pragma clang diagnostic pop

#endif

#if HAVE_STAT64
#define stat_ stat64
#define fstat_ fstat64
#define lstat_ lstat64
#else /* HAVE_STAT64 */
#define stat_ stat
#define fstat_ fstat
#define lstat_ lstat
#endif  /* HAVE_STAT64 */

// These numeric values are specified by POSIX.
// Validate that our definitions match.
c_static_assert(PAL_S_IRWXU == S_IRWXU);
c_static_assert(PAL_S_IRUSR == S_IRUSR);
c_static_assert(PAL_S_IWUSR == S_IWUSR);
c_static_assert(PAL_S_IXUSR == S_IXUSR);
c_static_assert(PAL_S_IRWXG == S_IRWXG);
c_static_assert(PAL_S_IRGRP == S_IRGRP);
c_static_assert(PAL_S_IWGRP == S_IWGRP);
c_static_assert(PAL_S_IXGRP == S_IXGRP);
c_static_assert(PAL_S_IRWXO == S_IRWXO);
c_static_assert(PAL_S_IROTH == S_IROTH);
c_static_assert(PAL_S_IWOTH == S_IWOTH);
c_static_assert(PAL_S_IXOTH == S_IXOTH);
c_static_assert(PAL_S_ISUID == S_ISUID);
c_static_assert(PAL_S_ISGID == S_ISGID);

// These numeric values are not specified by POSIX, but the values
// are common to our current targets.  If these static asserts fail,
// ConvertFileStatus needs to be updated to twiddle mode bits
// accordingly.
#if !defined(TARGET_WASI)
c_static_assert(PAL_S_IFMT == S_IFMT);
c_static_assert(PAL_S_IFIFO == S_IFIFO);
#endif /* TARGET_WASI */
c_static_assert(PAL_S_IFBLK == S_IFBLK);
c_static_assert(PAL_S_IFCHR == S_IFCHR);
c_static_assert(PAL_S_IFDIR == S_IFDIR);
c_static_assert(PAL_S_IFREG == S_IFREG);
c_static_assert(PAL_S_IFLNK == S_IFLNK);
c_static_assert(PAL_S_IFSOCK == S_IFSOCK);

// Validate that our enum for inode types is the same as what is
// declared by the dirent.h header on the local system.
// (AIX doesn't have dirent d_type, so none of this there)
// WebAssembly (BROWSER) has dirent d_type but is not correct
// by returning UNKNOWN the managed code properly stats the file
// to detect if entry is directory or not.
#if (defined(DT_UNKNOWN) || defined(TARGET_WASM)) && !defined(TARGET_WASI)
c_static_assert((int)PAL_DT_UNKNOWN == (int)DT_UNKNOWN);
c_static_assert((int)PAL_DT_FIFO == (int)DT_FIFO);
c_static_assert((int)PAL_DT_CHR == (int)DT_CHR);
c_static_assert((int)PAL_DT_DIR == (int)DT_DIR);
c_static_assert((int)PAL_DT_BLK == (int)DT_BLK);
c_static_assert((int)PAL_DT_REG == (int)DT_REG);
c_static_assert((int)PAL_DT_LNK == (int)DT_LNK);
c_static_assert((int)PAL_DT_SOCK == (int)DT_SOCK);
c_static_assert((int)PAL_DT_WHT == (int)DT_WHT);
#endif

// Validate that our Lock enum value are correct for the platform
c_static_assert(PAL_LOCK_SH == LOCK_SH);
c_static_assert(PAL_LOCK_EX == LOCK_EX);
c_static_assert(PAL_LOCK_NB == LOCK_NB);
c_static_assert(PAL_LOCK_UN == LOCK_UN);

// Validate our AccessMode enum values are correct for the platform
c_static_assert(PAL_F_OK == F_OK);
c_static_assert(PAL_X_OK == X_OK);
c_static_assert(PAL_W_OK == W_OK);
c_static_assert(PAL_R_OK == R_OK);

// Validate our SeekWhence enum values are correct for the platform
c_static_assert(PAL_SEEK_SET == SEEK_SET);
c_static_assert(PAL_SEEK_CUR == SEEK_CUR);
c_static_assert(PAL_SEEK_END == SEEK_END);

// Validate our NotifyEvents enum values are correct for the platform
#if HAVE_INOTIFY
c_static_assert(PAL_IN_ACCESS == IN_ACCESS);
c_static_assert(PAL_IN_MODIFY == IN_MODIFY);
c_static_assert(PAL_IN_ATTRIB == IN_ATTRIB);
c_static_assert(PAL_IN_MOVED_FROM == IN_MOVED_FROM);
c_static_assert(PAL_IN_MOVED_TO == IN_MOVED_TO);
c_static_assert(PAL_IN_CREATE == IN_CREATE);
c_static_assert(PAL_IN_DELETE == IN_DELETE);
c_static_assert(PAL_IN_Q_OVERFLOW == IN_Q_OVERFLOW);
c_static_assert(PAL_IN_IGNORED == IN_IGNORED);
c_static_assert(PAL_IN_ONLYDIR == IN_ONLYDIR);
c_static_assert(PAL_IN_DONT_FOLLOW == IN_DONT_FOLLOW);
#if HAVE_IN_EXCL_UNLINK
c_static_assert(PAL_IN_EXCL_UNLINK == IN_EXCL_UNLINK);
#endif // HAVE_IN_EXCL_UNLINK
c_static_assert(PAL_IN_ISDIR == IN_ISDIR);
#endif // HAVE_INOTIFY

static void ConvertFileStatus(const struct stat_* src, FileStatus* dst)
{
    dst->Dev = (int64_t)src->st_dev;
    dst->RDev = (int64_t)src->st_rdev;
    dst->Ino = (int64_t)src->st_ino;
    dst->Flags = FILESTATUS_FLAGS_NONE;
    dst->Mode = (int32_t)src->st_mode;
    dst->Uid = src->st_uid;
    dst->Gid = src->st_gid;
    dst->Size = src->st_size;

    dst->ATime = src->st_atime;
    dst->MTime = src->st_mtime;
    dst->CTime = src->st_ctime;

    dst->ATimeNsec = ST_ATIME_NSEC(src);
    dst->MTimeNsec = ST_MTIME_NSEC(src);
    dst->CTimeNsec = ST_CTIME_NSEC(src);

#if HAVE_STAT_BIRTHTIME
    dst->BirthTime = src->st_birthtimespec.tv_sec;
    dst->BirthTimeNsec = src->st_birthtimespec.tv_nsec;
    dst->Flags |= FILESTATUS_FLAGS_HAS_BIRTHTIME;
#else
    // Linux path: until we use statx() instead
    dst->BirthTime = 0;
    dst->BirthTimeNsec = 0;
#endif

#if HAVE_STAT_FLAGS && defined(UF_HIDDEN)
    dst->UserFlags = ((src->st_flags & UF_HIDDEN) == UF_HIDDEN) ? PAL_UF_HIDDEN : 0;
#else
    dst->UserFlags = 0;
#endif
}

int32_t SystemNative_Stat(const char* path, FileStatus* output)
{
    struct stat_ result;
    int ret;
    while ((ret = stat_(path, &result)) < 0 && errno == EINTR);

    if (ret == 0)
    {
        ConvertFileStatus(&result, output);
    }

    return ret;
}

int32_t SystemNative_FStat(intptr_t fd, FileStatus* output)
{
    struct stat_ result;
    int ret;
    while ((ret = fstat_(ToFileDescriptor(fd), &result)) < 0 && errno == EINTR);

    if (ret == 0)
    {
        ConvertFileStatus(&result, output);
    }

    return ret;
}

int32_t SystemNative_LStat(const char* path, FileStatus* output)
{
    struct stat_ result;
    int ret = lstat_(path, &result);

    if (ret == 0)
    {
        ConvertFileStatus(&result, output);
    }

    return ret;
}

static int32_t ConvertOpenFlags(int32_t flags)
{
    int32_t ret;
    switch (flags & PAL_O_ACCESS_MODE_MASK)
    {
        case PAL_O_RDONLY:
            ret = O_RDONLY;
            break;
        case PAL_O_RDWR:
            ret = O_RDWR;
            break;
        case PAL_O_WRONLY:
            ret = O_WRONLY;
            break;
        default:
            assert_msg(false, "Unknown Open access mode", (int)flags);
            return -1;
    }

    if (flags & ~(PAL_O_ACCESS_MODE_MASK | PAL_O_CLOEXEC | PAL_O_CREAT | PAL_O_EXCL | PAL_O_TRUNC | PAL_O_SYNC | PAL_O_NOFOLLOW))
    {
        assert_msg(false, "Unknown Open flag", (int)flags);
        return -1;
    }

#if HAVE_O_CLOEXEC
    if (flags & PAL_O_CLOEXEC)
        ret |= O_CLOEXEC;
#endif
    if (flags & PAL_O_CREAT)
        ret |= O_CREAT;
    if (flags & PAL_O_EXCL)
        ret |= O_EXCL;
    if (flags & PAL_O_TRUNC)
        ret |= O_TRUNC;
    if (flags & PAL_O_SYNC)
        ret |= O_SYNC;
    if (flags & PAL_O_NOFOLLOW)
        ret |= O_NOFOLLOW;

    assert(ret != -1);
    return ret;
}

intptr_t SystemNative_Open(const char* path, int32_t flags, int32_t mode)
{
// these two ifdefs are for platforms where we dont have the open version of CLOEXEC and thus
// must simulate it by doing a fcntl with the SETFFD version after the open instead
#if !HAVE_O_CLOEXEC
    int32_t old_flags = flags;
#endif
    flags = ConvertOpenFlags(flags);
    if (flags == -1)
    {
        errno = EINVAL;
        return -1;
    }

    int result;
    while ((result = open(path, flags, (mode_t)mode)) < 0 && errno == EINTR);
#if !HAVE_O_CLOEXEC
    if (old_flags & PAL_O_CLOEXEC)
    {
        fcntl(result, F_SETFD, FD_CLOEXEC);
    }
#endif
    return result;
}

int32_t SystemNative_Close(intptr_t fd)
{
    int result = close(ToFileDescriptor(fd));
    if (result < 0 && errno == EINTR) result = 0; // on all supported platforms, close(2) returning EINTR still means it was released
    return result;
}

intptr_t SystemNative_Dup(intptr_t oldfd)
{
    int result;
#if HAVE_F_DUPFD_CLOEXEC
    while ((result = fcntl(ToFileDescriptor(oldfd), F_DUPFD_CLOEXEC, 0)) < 0 && errno == EINTR);
#elif HAVE_F_DUPFD
    while ((result = fcntl(ToFileDescriptor(oldfd), F_DUPFD, 0)) < 0 && errno == EINTR);
    // do CLOEXEC here too
    fcntl(result, F_SETFD, FD_CLOEXEC);
#else
    // The main use cases for dup are setting up the classic Unix dance of setting up file descriptors in advance of performing a fork. Since WASI has no fork, these don't apply.
    // https://github.com/bytecodealliance/wasmtime/blob/b2fefe77148582a9b8013e34fe5808ada82b6efc/docs/WASI-rationale.md#why-no-dup
    result = oldfd;
#endif
    return result;
}

int32_t SystemNative_Unlink(const char* path)
{
    int32_t result;
    while ((result = unlink(path)) < 0 && errno == EINTR);
    return result;
}

#ifdef __NR_memfd_create
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS (1024 + 9)
#endif
#ifndef F_SEAL_WRITE
#define F_SEAL_WRITE 0x0008
#endif
#endif

int32_t SystemNative_IsMemfdSupported(void)
{
#ifdef __NR_memfd_create
#ifdef TARGET_LINUX
    struct utsname uts;
    int32_t major, minor;

    // memfd_create is known to only work properly on kernel version > 3.17.
    // On earlier versions, it may raise SIGSEGV instead of returning ENOTSUP.
    if (uname(&uts) == 0 && sscanf(uts.release, "%d.%d", &major, &minor) == 2 && (major < 3 || (major == 3 && minor < 17)))
    {
        return 0;
    }
#endif

    // Note that the name has no affect on file descriptor behavior. From linux manpage: 
    //   Names do not affect the behavior of the file descriptor, and as such multiple files can have the same name without any side effects.
    int32_t fd = (int32_t)syscall(__NR_memfd_create, "test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return 0;

    close(fd);
    return 1;
#else
    errno = ENOTSUP;
    return 0;
#endif
}

intptr_t SystemNative_MemfdCreate(const char* name, int32_t isReadonly)
{
#ifdef __NR_memfd_create
#if defined(SHM_NAME_MAX) // macOS
    assert(strlen(name) <= SHM_NAME_MAX);
#elif defined(PATH_MAX) // other Unixes
    assert(strlen(name) <= PATH_MAX);
#endif

    int32_t fd = (int32_t)syscall(__NR_memfd_create, name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (!isReadonly || fd < 0) return fd;

    // Add a write seal when readonly protection requested
    while (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) < 0 && errno == EINTR);
    return fd;
#else
    (void)name;
    (void)isReadonly;
    errno = ENOTSUP;
    return -1;
#endif
}

intptr_t SystemNative_ShmOpen(const char* name, int32_t flags, int32_t mode)
{
#if defined(SHM_NAME_MAX) // macOS
    assert(strlen(name) <= SHM_NAME_MAX);
#elif defined(PATH_MAX) // other Unixes
    assert(strlen(name) <= PATH_MAX);
#endif

#if HAVE_SHM_OPEN_THAT_WORKS_WELL_ENOUGH_WITH_MMAP
    flags = ConvertOpenFlags(flags);
    if (flags == -1)
    {
        errno = EINVAL;
        return -1;
    }

    return shm_open(name, flags, (mode_t)mode);
#else
    (void)name, (void)flags, (void)mode;
    errno = ENOTSUP;
    return -1;
#endif
}

int32_t SystemNative_ShmUnlink(const char* name)
{
#if HAVE_SHM_OPEN_THAT_WORKS_WELL_ENOUGH_WITH_MMAP
    int32_t result;
    while ((result = shm_unlink(name)) < 0 && errno == EINTR);
    return result;
#else
    // Not supported on e.g. Android. Also, prevent a compiler error because name is unused
    (void)name;
    errno = ENOTSUP;
    return -1;
#endif
}

static void ConvertDirent(const struct dirent* entry, DirectoryEntry* outputEntry)
{
    // We use Marshal.PtrToStringAnsi on the managed side, which takes a pointer to
    // the start of the unmanaged string. Give the caller back a pointer to the
    // location of the start of the string that exists in their own byte buffer.
    outputEntry->Name = entry->d_name;
#if !defined(DT_UNKNOWN) || defined(TARGET_WASM)
    // AIX has no d_type, and since we can't get the directory that goes with
    // the filename from ReadDir, we can't stat the file. Return unknown and
    // hope that managed code can properly stat the file.
    // WebAssembly (BROWSER) has dirent d_type but is not correct
    // by returning UNKNOWN the managed code properly stats the file
    // to detect if entry is directory or not.
    outputEntry->InodeType = PAL_DT_UNKNOWN;
#else
    outputEntry->InodeType = (int32_t)entry->d_type;
#endif

#if HAVE_DIRENT_NAME_LEN
    outputEntry->NameLength = entry->d_namlen;
#else
    outputEntry->NameLength = -1; // sentinel value to mean we have to walk to find the first \0
#endif
}

// The caller must ensure no calls are made to readdir/closedir since those will invalidate
// the current dirent. We assume the platform supports concurrent readdir calls to different DIRs.
int32_t SystemNative_ReadDir(DIR* dir, DirectoryEntry* outputEntry)
{
    assert(dir != NULL);
    assert(outputEntry != NULL);

    errno = 0;
    struct dirent* entry = readdir(dir);

    // 0 returned with null result -> end-of-stream
    if (entry == NULL)
    {
        memset(outputEntry, 0, sizeof(*outputEntry)); // managed out param must be initialized

        //  kernel set errno -> failure
        if (errno != 0)
        {
            assert_err(errno == EBADF, "Invalid directory stream descriptor dir", errno);
            return errno;
        }
        return -1;
    }

    ConvertDirent(entry, outputEntry);
    return 0;
}

DIR* SystemNative_OpenDir(const char* path)
{
    DIR *result;

    // EINTR isn't documented, happens in practice on macOS.
    while ((result = opendir(path)) == NULL && errno == EINTR);

    return result;
}

int32_t SystemNative_CloseDir(DIR* dir)
{
    int32_t result;

    result = closedir(dir);

    // EINTR isn't documented, happens in practice on macOS.
    if (result < 0 && errno == EINTR)
    {
        result = 0;
    }

    return result;
}

int32_t SystemNative_Pipe(int32_t pipeFds[2], int32_t flags)
{
    switch (flags)
    {
        case 0:
            break;
        case PAL_O_CLOEXEC:
#if HAVE_O_CLOEXEC
            flags = O_CLOEXEC;
#endif
            break;
        default:
            assert_msg(false, "Unknown pipe flag", (int)flags);
            errno = EINVAL;
            return -1;
    }

    int32_t result;
#if HAVE_PIPE2
    // If pipe2 is available, use it.  This will handle O_CLOEXEC if it was set.
    while ((result = pipe2(pipeFds, flags)) < 0 && errno == EINTR);
#elif HAVE_PIPE
    // Otherwise, use pipe.
    while ((result = pipe(pipeFds)) < 0 && errno == EINTR);

    // Then, if O_CLOEXEC was specified, use fcntl to configure the file descriptors appropriately.
#if HAVE_O_CLOEXEC
    if ((flags & O_CLOEXEC) != 0 && result == 0)
#else
    if ((flags & PAL_O_CLOEXEC) != 0 && result == 0)
#endif
    {
        while ((result = fcntl(pipeFds[0], F_SETFD, FD_CLOEXEC)) < 0 && errno == EINTR);
        if (result == 0)
        {
            while ((result = fcntl(pipeFds[1], F_SETFD, FD_CLOEXEC)) < 0 && errno == EINTR);
        }

        if (result != 0)
        {
            int tmpErrno = errno;
            close(pipeFds[0]);
            close(pipeFds[1]);
            errno = tmpErrno;
        }
    }
#else /* HAVE_PIPE */
    result = -1;
#endif /* HAVE_PIPE */
    return result;
}

int32_t SystemNative_FcntlSetFD(intptr_t fd, int32_t flags)
{
    int result;
    while ((result = fcntl(ToFileDescriptor(fd), F_SETFD, ConvertOpenFlags(flags))) < 0 && errno == EINTR);
    return result;
}

int32_t SystemNative_FcntlGetFD(intptr_t fd)
{
    return fcntl(ToFileDescriptor(fd), F_GETFD);
}

int32_t SystemNative_FcntlCanGetSetPipeSz(void)
{
#if defined(F_GETPIPE_SZ) && defined(F_SETPIPE_SZ)
    return true;
#else
    return false;
#endif
}

int32_t SystemNative_FcntlGetPipeSz(intptr_t fd)
{
#ifdef F_GETPIPE_SZ
    int32_t result;
    while ((result = fcntl(ToFileDescriptor(fd), F_GETPIPE_SZ)) < 0 && errno == EINTR);
    return result;
#else
    (void)fd;
    errno = ENOTSUP;
    return -1;
#endif
}

int32_t SystemNative_FcntlSetPipeSz(intptr_t fd, int32_t size)
{
#ifdef F_SETPIPE_SZ
    int32_t result;
    while ((result = fcntl(ToFileDescriptor(fd), F_SETPIPE_SZ, size)) < 0 && errno == EINTR);
    return result;
#else
    (void)fd, (void)size;
    errno = ENOTSUP;
    return -1;
#endif
}

int32_t SystemNative_FcntlSetIsNonBlocking(intptr_t fd, int32_t isNonBlocking)
{
    int fileDescriptor = ToFileDescriptor(fd);

    int flags = fcntl(fileDescriptor, F_GETFL);
    if (flags == -1)
    {
        return -1;
    }

    if (isNonBlocking == 0)
    {
        flags &= ~O_NONBLOCK;
    }
    else
    {
        flags |= O_NONBLOCK;
    }

    return fcntl(fileDescriptor, F_SETFL, flags);
}

int32_t SystemNative_FcntlGetIsNonBlocking(intptr_t fd, int32_t* isNonBlocking)
{
    if (isNonBlocking == NULL)
    {
        return Error_EFAULT;
    }

    int flags = fcntl(ToFileDescriptor(fd), F_GETFL);
    if (flags == -1)
    {
        *isNonBlocking = 0;
        return -1;
    }

    *isNonBlocking = ((flags & O_NONBLOCK) == O_NONBLOCK) ? 1 : 0;
    return 0;
}

int32_t SystemNative_MkDir(const char* path, int32_t mode)
{
    int32_t result;
    while ((result = mkdir(path, (mode_t)mode)) < 0 && errno == EINTR);
    return result;
}

int32_t SystemNative_ChMod(const char* path, int32_t mode)
{
#if HAVE_CHMOD
    int32_t result;
    while ((result = chmod(path, (mode_t)mode)) < 0 && errno == EINTR);
    return result;
#else /* HAVE_CHMOD */
    (void)path; // unused
    (void)mode; // unused
    return EINTR;
#endif /* HAVE_CHMOD */
}

int32_t SystemNative_FChMod(intptr_t fd, int32_t mode)
{
#if HAVE_FCHMOD
    int32_t result;
    while ((result = fchmod(ToFileDescriptor(fd), (mode_t)mode)) < 0 && errno == EINTR);
    return result;
#else /* HAVE_FCHMOD */
    (void)fd; // unused
    (void)mode; // unused
    return EINTR;
#endif /* HAVE_FCHMOD */
}

int32_t SystemNative_FSync(intptr_t fd)
{
    int fileDescriptor = ToFileDescriptor(fd);

    int32_t result;
    while ((result =
#if defined(TARGET_OSX) && HAVE_F_FULLFSYNC
    fcntl(fileDescriptor, F_FULLFSYNC)
#else
    fsync(fileDescriptor)
#endif
    < 0) && errno == EINTR);
    return result;
}

int32_t SystemNative_FLock(intptr_t fd, int32_t operation)
{
    int32_t result;
#if !defined(TARGET_WASI)
    while ((result = flock(ToFileDescriptor(fd), operation)) < 0 && errno == EINTR);
#else /* TARGET_WASI */
    result = EINTR;
#endif /* TARGET_WASI */
    return result;
}

int32_t SystemNative_ChDir(const char* path)
{
    int32_t result;
    while ((result = chdir(path)) < 0 && errno == EINTR);
    return result;
}

int32_t SystemNative_Access(const char* path, int32_t mode)
{
    return access(path, mode);
}

int64_t SystemNative_LSeek(intptr_t fd, int64_t offset, int32_t whence)
{
    int64_t result;
    while ((
        result =
#if HAVE_LSEEK64
            lseek64(
                 ToFileDescriptor(fd),
                 (off_t)offset,
                 whence)) < 0 && errno == EINTR);
#else
            lseek(
                 ToFileDescriptor(fd),
                 (off_t)offset,
                 whence)) < 0 && errno == EINTR);
#endif
    return result;
}

int32_t SystemNative_Link(const char* source, const char* linkTarget)
{
    int32_t result;
    while ((result = link(source, linkTarget)) < 0 && errno == EINTR);
    return result;
}

int32_t SystemNative_SymLink(const char* target, const char* linkPath)
{
    int32_t result;
    while ((result = symlink(target, linkPath)) < 0 && errno == EINTR);
    return result;
}

void SystemNative_GetDeviceIdentifiers(uint64_t dev, uint32_t* majorNumber, uint32_t* minorNumber)
{
#if !defined(TARGET_WASI)
    dev_t castedDev = (dev_t)dev;
    *majorNumber = (uint32_t)major(castedDev);
    *minorNumber = (uint32_t)minor(castedDev);
#else /* TARGET_WASI */
    dev_t castedDev = (dev_t)dev;
    *majorNumber = 0;
    *minorNumber = 0;
#endif /* TARGET_WASI */
}

int32_t SystemNative_MkNod(const char* pathName, uint32_t mode, uint32_t major, uint32_t minor)
{
#if !defined(TARGET_WASI)
    dev_t dev = (dev_t)makedev(major, minor);

    int32_t result;
    while ((result = mknod(pathName, (mode_t)mode, dev)) < 0 && errno == EINTR);
    return result;
#else /* TARGET_WASI */
    return EINTR;
#endif /* TARGET_WASI */
}

int32_t SystemNative_MkFifo(const char* pathName, uint32_t mode)
{
#if !defined(TARGET_WASI)
    int32_t result;
    while ((result = mkfifo(pathName, (mode_t)mode)) < 0 && errno == EINTR);
    return result;
#else /* TARGET_WASI */
    return EINTR;
#endif /* TARGET_WASI */
}

char* SystemNative_MkdTemp(char* pathTemplate)
{
#if !defined(TARGET_WASI)
    char* result = NULL;
    while ((result = mkdtemp(pathTemplate)) == NULL && errno == EINTR);
    return result;
#else /* TARGET_WASI */
    return NULL;
#endif /* TARGET_WASI */
}

intptr_t SystemNative_MksTemps(char* pathTemplate, int32_t suffixLength)
{
    intptr_t result;
#if HAVE_MKSTEMPS
    while ((result = mkstemps(pathTemplate, suffixLength)) < 0 && errno == EINTR);
#elif HAVE_MKSTEMP
    // mkstemps is not available bionic/Android, but mkstemp is
    // mkstemp doesn't allow the suffix that msktemps does allow, so we'll need to
    // remove that before passisng pathTemplate to mkstemp

    int32_t pathTemplateLength = (int32_t)strlen(pathTemplate);

    // pathTemplate must include at least XXXXXX (6 characters) which are not part of
    // the suffix
    if (suffixLength < 0 || suffixLength > pathTemplateLength - 6)
    {
        errno = EINVAL;
        return -1;
    }

    // Make mkstemp ignore the suffix by setting the first char of the suffix to \0,
    // if there is a suffix
    int32_t firstSuffixIndex = 0;
    char firstSuffixChar = 0;

    if (suffixLength > 0)
    {
        firstSuffixIndex = pathTemplateLength - suffixLength;
        firstSuffixChar = pathTemplate[firstSuffixIndex];
        pathTemplate[firstSuffixIndex] = 0;
    }

    while ((result = mkstemp(pathTemplate)) < 0 && errno == EINTR);

    // Reset the first char of the suffix back to its original value, if there is a suffix
    if (suffixLength > 0)
    {
        pathTemplate[firstSuffixIndex] = firstSuffixChar;
    }
#elif TARGET_WASI
    assert_msg(false, "Not supported on WASI", 0);
    result = -1;
#else
#error "Cannot find mkstemps nor mkstemp on this platform"
#endif
    return  result;
}

static int32_t ConvertMMapProtection(int32_t protection)
{
    if (protection == PAL_PROT_NONE)
        return PROT_NONE;

    if (protection & ~(PAL_PROT_READ | PAL_PROT_WRITE | PAL_PROT_EXEC))
    {
        assert_msg(false, "Unknown protection", (int)protection);
        return -1;
    }

    int32_t ret = 0;
    if (protection & PAL_PROT_READ)
        ret |= PROT_READ;
    if (protection & PAL_PROT_WRITE)
        ret |= PROT_WRITE;
    if (protection & PAL_PROT_EXEC)
        ret |= PROT_EXEC;

    assert(ret != -1);
    return ret;
}

static int32_t ConvertMMapFlags(int32_t flags)
{
    if (flags & ~(PAL_MAP_SHARED | PAL_MAP_PRIVATE | PAL_MAP_ANONYMOUS))
    {
        assert_msg(false, "Unknown MMap flag", (int)flags);
        return -1;
    }

    int32_t ret = 0;
    if (flags & PAL_MAP_PRIVATE)
        ret |= MAP_PRIVATE;
    if (flags & PAL_MAP_SHARED)
        ret |= MAP_SHARED;
    if (flags & PAL_MAP_ANONYMOUS)
        ret |= MAP_ANON;

    assert(ret != -1);
    return ret;
}

static int32_t ConvertMSyncFlags(int32_t flags)
{
    if (flags & ~(PAL_MS_SYNC | PAL_MS_ASYNC | PAL_MS_INVALIDATE))
    {
        assert_msg(false, "Unknown MSync flag", (int)flags);
        return -1;
    }

    int32_t ret = 0;
    if (flags & PAL_MS_SYNC)
        ret |= MS_SYNC;
    if (flags & PAL_MS_ASYNC)
        ret |= MS_ASYNC;
    if (flags & PAL_MS_INVALIDATE)
        ret |= MS_INVALIDATE;

    assert(ret != -1);
    return ret;
}

void* SystemNative_MMap(void* address,
                      uint64_t length,
                      int32_t protection, // bitwise OR of PAL_PROT_*
                      int32_t flags,      // bitwise OR of PAL_MAP_*, but PRIVATE and SHARED are mutually exclusive.
                      intptr_t fd,
                      int64_t offset)
{
    if (length > SIZE_MAX)
    {
        errno = ERANGE;
        return NULL;
    }

    protection = ConvertMMapProtection(protection);
    flags = ConvertMMapFlags(flags);

    if (flags == -1 || protection == -1)
    {
        errno = EINVAL;
        return NULL;
    }

    // Use ToFileDescriptorUnchecked to allow -1 to be passed for the file descriptor, since managed code explicitly uses -1
    void* ret =
#if HAVE_MMAP64
        mmap64(
#else
        mmap(
#endif
            address,
            (size_t)length,
            protection,
            flags,
            ToFileDescriptorUnchecked(fd),
            (off_t)offset);

    if (ret == MAP_FAILED)
    {
        return NULL;
    }

    assert(ret != NULL);
    return ret;
}

int32_t SystemNative_MUnmap(void* address, uint64_t length)
{
    if (length > SIZE_MAX)
    {
        errno = ERANGE;
        return -1;
    }

    return munmap(address, (size_t)length);
}

int32_t SystemNative_MProtect(void* address, uint64_t length, int32_t protection)
{
    if (length > SIZE_MAX)
    {
        errno =  ERANGE;
        return -1;
    }

    protection = ConvertMMapProtection(protection);

    return mprotect(address, (size_t)length, protection);
}

int32_t SystemNative_MAdvise(void* address, uint64_t length, int32_t advice)
{
    if (length > SIZE_MAX)
    {
        errno = ERANGE;
        return -1;
    }

    switch (advice)
    {
        case PAL_MADV_DONTFORK:
#if defined(MADV_DONTFORK) && !defined(TARGET_WASI)
            return madvise(address, (size_t)length, MADV_DONTFORK);
#else
            (void)address, (void)length, (void)advice;
            errno = ENOTSUP;
            return -1;
#endif
        default:
            break; // fall through to error
    }

    assert_msg(false, "Unknown MemoryAdvice", (int)advice);
    errno = EINVAL;
    return -1;
}

int32_t SystemNative_MSync(void* address, uint64_t length, int32_t flags)
{
    if (length > SIZE_MAX)
    {
        errno = ERANGE;
        return -1;
    }

    flags = ConvertMSyncFlags(flags);
    if (flags == -1)
    {
        errno = EINVAL;
        return -1;
    }

#if !defined(TARGET_WASI)
    return msync(address, (size_t)length, flags);
#else
    return -1;
#endif
}

int64_t SystemNative_SysConf(int32_t name)
{
    switch (name)
    {
        case PAL_SC_CLK_TCK:
            return sysconf(_SC_CLK_TCK);
        case PAL_SC_PAGESIZE:
            return sysconf(_SC_PAGESIZE);
        default:
            break; // fall through to error
    }

    assert_msg(false, "Unknown SysConf name", (int)name);
    errno = EINVAL;
    return -1;
}

int32_t SystemNative_FTruncate(intptr_t fd, int64_t length)
{
    int32_t result;
    while ((
        result =
#if HAVE_FTRUNCATE64
        ftruncate64(
#else
        ftruncate(
#endif
            ToFileDescriptor(fd),
            (off_t)length)) < 0 && errno == EINTR);
    return result;
}

int32_t SystemNative_Poll(PollEvent* pollEvents, uint32_t eventCount, int32_t milliseconds, uint32_t* triggered)
{
    return Common_Poll(pollEvents, eventCount, milliseconds, triggered);
}

int32_t SystemNative_PosixFAdvise(intptr_t fd, int64_t offset, int64_t length, int32_t advice)
{
#if HAVE_POSIX_ADVISE
    // POSIX_FADV_* may be different on each platform. Convert the values from PAL to the system's.
    int32_t actualAdvice;
    switch (advice)
    {
        case PAL_POSIX_FADV_NORMAL:     actualAdvice = POSIX_FADV_NORMAL;     break;
        case PAL_POSIX_FADV_RANDOM:     actualAdvice = POSIX_FADV_RANDOM;     break;
        case PAL_POSIX_FADV_SEQUENTIAL: actualAdvice = POSIX_FADV_SEQUENTIAL; break;
        case PAL_POSIX_FADV_WILLNEED:   actualAdvice = POSIX_FADV_WILLNEED;   break;
        case PAL_POSIX_FADV_DONTNEED:   actualAdvice = POSIX_FADV_DONTNEED;   break;
        case PAL_POSIX_FADV_NOREUSE:    actualAdvice = POSIX_FADV_NOREUSE;    break;
        default: return EINVAL; // According to the man page
    }
    int32_t result;
    while ((
        result =
#if HAVE_POSIX_FADVISE64
            posix_fadvise64(
#else
            posix_fadvise(
#endif
                ToFileDescriptor(fd),
                (off_t)offset,
                (off_t)length,
                actualAdvice)) < 0 && errno == EINTR);
    return result;
#else
    // Not supported on this platform. Caller can ignore this failure since it's just a hint.
    (void)fd, (void)offset, (void)length, (void)advice;
    return ENOTSUP;
#endif
}

int32_t SystemNative_FAllocate(intptr_t fd, int64_t offset, int64_t length)
{
    assert_msg(offset == 0, "Invalid offset value", (int)offset);

    int fileDescriptor = ToFileDescriptor(fd);
    int32_t result;
#if HAVE_FALLOCATE // Linux
    while ((result = fallocate(fileDescriptor, FALLOC_FL_KEEP_SIZE, (off_t)offset, (off_t)length)) == -1 && errno == EINTR);
#elif defined(F_PREALLOCATE) // macOS
    fstore_t fstore;
    fstore.fst_flags = F_ALLOCATEALL; // Allocate all requested space or no space at all.
    fstore.fst_posmode = F_PEOFPOSMODE; // Allocate from the physical end of file.
    fstore.fst_offset = (off_t)offset;
    fstore.fst_length = (off_t)length;
    fstore.fst_bytesalloc = 0; // output size, can be > length

    while ((result = fcntl(fileDescriptor, F_PREALLOCATE, &fstore)) == -1 && errno == EINTR);
#else
    (void)offset; // unused
    (void)length; // unused
    result = -1;
    errno = EOPNOTSUPP;
#endif

    assert(result == 0 || errno != EINVAL);

    return result;
}

int32_t SystemNative_Read(intptr_t fd, void* buffer, int32_t bufferSize)
{
    return Common_Read(fd, buffer, bufferSize);
}

int32_t SystemNative_ReadLink(const char* path, char* buffer, int32_t bufferSize)
{
    assert(buffer != NULL || bufferSize == 0);
    assert(bufferSize >= 0);

    if (bufferSize <= 0)
    {
        errno = EINVAL;
        return -1;
    }

    ssize_t count = readlink(path, buffer, (size_t)bufferSize);
    assert(count >= -1 && count <= bufferSize);

    return (int32_t)count;
}

int32_t SystemNative_Rename(const char* oldPath, const char* newPath)
{
    int32_t result;
    while ((result = rename(oldPath, newPath)) < 0 && errno == EINTR);
    return result;
}

int32_t SystemNative_RmDir(const char* path)
{
    int32_t result;
    while ((result = rmdir(path)) < 0 && errno == EINTR);
    return result;
}

void SystemNative_Sync(void)
{
#if !defined(TARGET_WASI)
    sync();
#endif /* TARGET_WASI */
}

int32_t SystemNative_Write(intptr_t fd, const void* buffer, int32_t bufferSize)
{
    return Common_Write(fd, buffer, bufferSize);
}

#if !HAVE_FCOPYFILE
// Read all data from inFd and write it to outFd
static int32_t CopyFile_ReadWrite(int inFd, int outFd)
{
    // Allocate a buffer
    const int BufferLength = 80 * 1024 * sizeof(char);
    char* buffer = (char*)malloc(BufferLength);
    if (buffer == NULL)
    {
        return -1;
    }

    // Repeatedly read from the source and write to the destination
    while (true)
    {
        // Read up to what will fit in our buffer.  We're done if we get back 0 bytes.
        ssize_t bytesRead;
        while ((bytesRead = read(inFd, buffer, BufferLength)) < 0 && errno == EINTR);
        if (bytesRead == -1)
        {
            int tmp = errno;
            free(buffer);
            errno = tmp;
            return -1;
        }
        if (bytesRead == 0)
        {
            break;
        }
        assert(bytesRead > 0);

        // Write what was read.
        ssize_t offset = 0;
        while (bytesRead > 0)
        {
            ssize_t bytesWritten;
            while ((bytesWritten = write(outFd, buffer + offset, (size_t)bytesRead)) < 0 && errno == EINTR);
            if (bytesWritten == -1)
            {
                int tmp = errno;
                free(buffer);
                errno = tmp;
                return -1;
            }
            assert(bytesWritten >= 0);
            bytesRead -= bytesWritten;
            offset += bytesWritten;
        }
    }

    free(buffer);
    return 0;
}
#endif // !HAVE_FCOPYFILE


#ifdef __linux__
static ssize_t CopyFileRange(int inFd, int outFd, size_t len)
{
    return syscall(__NR_copy_file_range, inFd, NULL, outFd, NULL, len, 0);
}

static bool SupportsCopyFileRange(void)
{
    static volatile int s_isSupported = 0;

    int isSupported = s_isSupported;
    if (isSupported == 0)
    {
        isSupported = -1;

        // Avoid known issues with copy_file_range that are fixed in Linux 5.3+ (https://lwn.net/Articles/789527/).
        struct utsname name;
        if (uname(&name) == 0)
        {
            unsigned int major = 0, minor = 0;
            sscanf(name.release, "%u.%u", &major, &minor);
            if (major > 5 || (major == 5 && minor >=3))
            {
                isSupported = CopyFileRange(-1, -1, 0) == -1 && errno != ENOSYS ? 1 : -1;
            }
        }

        s_isSupported = isSupported;
    }
    return isSupported == 1;
}
#endif

int32_t SystemNative_CopyFile(intptr_t sourceFd, intptr_t destinationFd, int64_t sourceLength)
{
    // unused on some platforms.
    (void)sourceLength;

    int inFd = ToFileDescriptor(sourceFd);
    int outFd = ToFileDescriptor(destinationFd);

#if HAVE_FCOPYFILE
    // If fcopyfile is available (OS X), try to use it, as the whole copy
    // can be performed in the kernel, without lots of unnecessary copying.
    // Copy data and metadata.
    return fcopyfile(inFd, outFd, NULL, COPYFILE_ALL) == 0 ? 0 : -1;
#else
    // Get the stats on the source file.
    int ret;
    bool copied = false;
    bool trySendFile = true;

    // Certain files (e.g. procfs) may return a size of 0 even though reading them will
    // produce data. We use plain read/write for those.
#ifdef FICLONE
    // Try copying data using a copy-on-write clone. This shares storage between the files.
    if (sourceLength != 0)
    {
#if HAVE_IOCTL_WITH_INT_REQUEST
        while ((ret = ioctl(outFd, (int)FICLONE, inFd)) < 0 && errno == EINTR);
#else
        while ((ret = ioctl(outFd, FICLONE, inFd)) < 0 && errno == EINTR);
#endif
        copied = ret == 0;
    }
#endif
#ifdef __linux__
    if (SupportsCopyFileRange() && !copied && sourceLength != 0)
    {
        do
        {
            size_t copyLength = (sourceLength >= SSIZE_MAX ? SSIZE_MAX : (size_t)sourceLength);
            ssize_t sent = CopyFileRange(inFd, outFd, copyLength);
            if (sent <= 0)
            {
                // sendfile will likely encounter the same error, don't try it.
                trySendFile = false;
                break; // Fall through.
            }
            else
            {
                assert(sent <= sourceLength);
                sourceLength -= sent;
            }
        } while (sourceLength > 0);

        copied = sourceLength == 0;
    }
#endif
#if HAVE_SENDFILE_4
    // Try copying the data using sendfile.
    if (trySendFile && !copied && sourceLength != 0)
    {
        // Note that per man page for large files, you have to iterate until the
        // whole file is copied (Linux has a limit of 0x7ffff000 bytes copied).
        do
        {
            size_t copyLength = (sourceLength >= SSIZE_MAX ? SSIZE_MAX : (size_t)sourceLength);
            ssize_t sent = sendfile(outFd, inFd, NULL, copyLength);
            if (sent < 0)
            {
                if (errno != EINVAL && errno != ENOSYS)
                {
                    return -1;
                }
                else
                {
                    break;
                }
            }
            else if (sent == 0)
            {
                // The file was truncated (or maybe some other condition occurred).
                // Perform the remaining copying using read/write.
                break;
            }
            else
            {
                assert(sent <= sourceLength);
                sourceLength -= sent;
            }
        } while (sourceLength > 0);

        copied = sourceLength == 0;
    }
#endif // HAVE_SENDFILE_4

    // Perform a manual copy.
    if (!copied && CopyFile_ReadWrite(inFd, outFd) != 0)
    {
        return -1;
    }

    // Copy file times.
    struct stat_ sourceStat;
    while ((ret = fstat_(inFd, &sourceStat)) < 0 && errno == EINTR);
    if (ret == 0)
    {
#if HAVE_FUTIMENS
        // futimens is preferred because it has a higher resolution.
        struct timespec origTimes[2];
        origTimes[0].tv_sec = (time_t)sourceStat.st_atime;
        origTimes[0].tv_nsec = ST_ATIME_NSEC(&sourceStat);
        origTimes[1].tv_sec = (time_t)sourceStat.st_mtime;
        origTimes[1].tv_nsec = ST_MTIME_NSEC(&sourceStat);
        while ((ret = futimens(outFd, origTimes)) < 0 && errno == EINTR);
#elif HAVE_FUTIMES
        struct timeval origTimes[2];
        origTimes[0].tv_sec = sourceStat.st_atime;
        origTimes[0].tv_usec = (int32_t)(ST_ATIME_NSEC(&sourceStat) / 1000);
        origTimes[1].tv_sec = sourceStat.st_mtime;
        origTimes[1].tv_usec = (int32_t)(ST_MTIME_NSEC(&sourceStat) / 1000);
        while ((ret = futimes(outFd, origTimes)) < 0 && errno == EINTR);
#endif
    }
    // If we copied to a filesystem (eg EXFAT) that does not preserve POSIX ownership, all files appear
    // to be owned by root. If we aren't running as root, then we won't be an owner of our new file, and
    // attempting to copy metadata to it will fail with EPERM. We have copied successfully, we just can't
    // copy metadata. The best thing we can do is skip copying the metadata.
    if (ret != 0 && errno != EPERM)
    {
        return -1;
    }

#if HAVE_FCHMOD
    // Copy permissions.
    // Even though managed code created the file with permissions matching those of the source file,
    // we need to copy permissions because the open permissions may be filtered by 'umask'.
    while ((ret = fchmod(outFd, sourceStat.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO))) < 0 && errno == EINTR);
    if (ret != 0 && errno != EPERM) // See EPERM comment above
    {
        return -1;
    }
#endif /* HAVE_FCHMOD */

    return 0;
#endif // HAVE_FCOPYFILE
}

intptr_t SystemNative_INotifyInit(void)
{
#if HAVE_INOTIFY
    return inotify_init1(IN_CLOEXEC);
#else
    errno = ENOTSUP;
    return -1;
#endif
}

int32_t SystemNative_INotifyAddWatch(intptr_t fd, const char* pathName, uint32_t mask)
{
    assert(fd >= 0);
    assert(pathName != NULL);

#if HAVE_INOTIFY
#if !HAVE_IN_EXCL_UNLINK
    mask &= ~((uint32_t)PAL_IN_EXCL_UNLINK);
#endif
    return inotify_add_watch(ToFileDescriptor(fd), pathName, mask);
#else
    (void)fd, (void)pathName, (void)mask;
    errno = ENOTSUP;
    return -1;
#endif
}

int32_t SystemNative_INotifyRemoveWatch(intptr_t fd, int32_t wd)
{
    assert(fd >= 0);
    assert(wd >= 0);

#if HAVE_INOTIFY
    return inotify_rm_watch(
        ToFileDescriptor(fd),
#if INOTIFY_RM_WATCH_WD_UNSIGNED
        (uint32_t)wd);
#else
        wd);
#endif
#else
    (void)fd, (void)wd;
    errno = ENOTSUP;
    return -1;
#endif
}

int32_t SystemNative_GetPeerID(intptr_t socket, uid_t* euid)
{
    int fd = ToFileDescriptor(socket);

    // ucred causes Emscripten to fail even though it's defined,
    // but getting peer credentials won't work for WebAssembly anyway
#if defined(SO_PEERCRED) && !defined(TARGET_WASM)
    struct ucred creds;
    socklen_t len = sizeof(creds);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &creds, &len) == 0)
    {
        *euid = creds.uid;
        return 0;
    }
    return -1;
#elif HAVE_GETPEEREID
    uid_t egid;
    return getpeereid(fd, euid, &egid);
#else
    (void)fd;
    (void)*euid;
    errno = ENOTSUP;
    return -1;
#endif
}

char* SystemNative_RealPath(const char* path)
{
    assert(path != NULL);
#if !defined(TARGET_WASI)
    return realpath(path, NULL);
#else /* TARGET_WASI */
    return NULL;
#endif /* TARGET_WASI */
}

#if !defined(TARGET_WASI)
static int16_t ConvertLockType(int16_t managedLockType)
{
    // the managed enum Interop.Sys.LockType has no 1:1 mapping with corresponding Unix values
    // which can be different per distro:
    // https://github.com/torvalds/linux/blob/fcadab740480e0e0e9fa9bd272acd409884d431a/arch/alpha/include/uapi/asm/fcntl.h#L48-L50
    // https://github.com/freebsd/freebsd-src/blob/fb8c2f743ab695f6004650b58bf96972e2535b20/sys/sys/fcntl.h#L277-L279
    switch (managedLockType)
    {
        case 0:
            return F_RDLCK;
        case 1:
            return F_WRLCK;
        default:
            assert_msg(managedLockType == 2, "Unknown Lock Type", (int)managedLockType);
            return F_UNLCK;
    }
}

#if !HAVE_NON_LEGACY_STATFS || defined(TARGET_APPLE) || defined(TARGET_FREEBSD)
static uint32_t MapFileSystemNameToEnum(const char* fileSystemName)
{
    uint32_t result = 0;

    if (strcmp(fileSystemName, "adfs") == 0) result = 0xADF5;
    else if (strcmp(fileSystemName, "affs") == 0) result = 0xADFF;
    else if (strcmp(fileSystemName, "afs") == 0) result = 0x5346414F;
    else if (strcmp(fileSystemName, "anoninode") == 0) result = 0x09041934;
    else if (strcmp(fileSystemName, "apfs") == 0) result = 0x1A;
    else if (strcmp(fileSystemName, "aufs") == 0) result = 0x61756673;
    else if (strcmp(fileSystemName, "autofs") == 0) result = 0x0187;
    else if (strcmp(fileSystemName, "autofs4") == 0) result = 0x6D4A556D;
    else if (strcmp(fileSystemName, "befs") == 0) result = 0x42465331;
    else if (strcmp(fileSystemName, "bdevfs") == 0) result = 0x62646576;
    else if (strcmp(fileSystemName, "bfs") == 0) result = 0x1BADFACE;
    else if (strcmp(fileSystemName, "bpf_fs") == 0) result = 0xCAFE4A11;
    else if (strcmp(fileSystemName, "binfmt_misc") == 0) result = 0x42494E4D;
    else if (strcmp(fileSystemName, "bootfs") == 0) result = 0xA56D3FF9;
    else if (strcmp(fileSystemName, "btrfs") == 0) result = 0x9123683E;
    else if (strcmp(fileSystemName, "ceph") == 0) result = 0x00C36400;
    else if (strcmp(fileSystemName, "cgroupfs") == 0) result = 0x0027E0EB;
    else if (strcmp(fileSystemName, "cgroup2fs") == 0) result = 0x63677270;
    else if (strcmp(fileSystemName, "cifs") == 0) result = 0xFF534D42;
    else if (strcmp(fileSystemName, "coda") == 0) result = 0x73757245;
    else if (strcmp(fileSystemName, "coherent") == 0) result = 0x012FF7B7;
    else if (strcmp(fileSystemName, "configfs") == 0) result = 0x62656570;
    else if (strcmp(fileSystemName, "cpuset") == 0) result = 0x01021994;
    else if (strcmp(fileSystemName, "cramfs") == 0) result = 0x28CD3D45;
    else if (strcmp(fileSystemName, "ctfs") == 0) result = 0x01021994;
    else if (strcmp(fileSystemName, "debugfs") == 0) result = 0x64626720;
    else if (strcmp(fileSystemName, "dev") == 0) result = 0x1373;
    else if (strcmp(fileSystemName, "devfs") == 0) result = 0x1373;
    else if (strcmp(fileSystemName, "devpts") == 0) result = 0x1CD1;
    else if (strcmp(fileSystemName, "ecryptfs") == 0) result = 0xF15F;
    else if (strcmp(fileSystemName, "efs") == 0) result = 0x00414A53;
    else if (strcmp(fileSystemName, "exofs") == 0) result = 0x5DF5;
    else if (strcmp(fileSystemName, "ext") == 0) result = 0x137D;
    else if (strcmp(fileSystemName, "ext2_old") == 0) result = 0xEF51;
    else if (strcmp(fileSystemName, "ext2") == 0) result = 0xEF53;
    else if (strcmp(fileSystemName, "ext3") == 0) result = 0xEF53;
    else if (strcmp(fileSystemName, "ext4") == 0) result = 0xEF53;
    else if (strcmp(fileSystemName, "f2fs") == 0) result = 0xF2F52010;
    else if (strcmp(fileSystemName, "fat") == 0) result = 0x4006;
    else if (strcmp(fileSystemName, "fd") == 0) result = 0xF00D1E;
    else if (strcmp(fileSystemName, "fhgfs") == 0) result = 0x19830326;
    else if (strcmp(fileSystemName, "fuse") == 0) result = 0x65735546;
    else if (strcmp(fileSystemName, "fuseblk") == 0) result = 0x65735546;
    else if (strcmp(fileSystemName, "fusectl") == 0) result = 0x65735543;
    else if (strcmp(fileSystemName, "futexfs") == 0) result = 0x0BAD1DEA;
    else if (strcmp(fileSystemName, "gfsgfs2") == 0) result = 0x1161970;
    else if (strcmp(fileSystemName, "gfs2") == 0) result = 0x01161970;
    else if (strcmp(fileSystemName, "gpfs") == 0) result = 0x47504653;
    else if (strcmp(fileSystemName, "hfs") == 0) result = 0x4244;
    else if (strcmp(fileSystemName, "hfsplus") == 0) result = 0x482B;
    else if (strcmp(fileSystemName, "hpfs") == 0) result = 0xF995E849;
    else if (strcmp(fileSystemName, "hugetlbfs") == 0) result = 0x958458F6;
    else if (strcmp(fileSystemName, "inodefs") == 0) result = 0x11307854;
    else if (strcmp(fileSystemName, "inotifyfs") == 0) result = 0x2BAD1DEA;
    else if (strcmp(fileSystemName, "isofs") == 0) result = 0x9660;
    else if (strcmp(fileSystemName, "jffs") == 0) result = 0x07C0;
    else if (strcmp(fileSystemName, "jffs2") == 0) result = 0x72B6;
    else if (strcmp(fileSystemName, "jfs") == 0) result = 0x3153464A;
    else if (strcmp(fileSystemName, "kafs") == 0) result = 0x6B414653;
    else if (strcmp(fileSystemName, "lofs") == 0) result = 0xEF53;
    else if (strcmp(fileSystemName, "logfs") == 0) result = 0xC97E8168;
    else if (strcmp(fileSystemName, "lustre") == 0) result = 0x0BD00BD0;
    else if (strcmp(fileSystemName, "minix_old") == 0) result = 0x137F;
    else if (strcmp(fileSystemName, "minix") == 0) result = 0x138F;
    else if (strcmp(fileSystemName, "minix2") == 0) result = 0x2468;
    else if (strcmp(fileSystemName, "minix2v2") == 0) result = 0x2478;
    else if (strcmp(fileSystemName, "minix3") == 0) result = 0x4D5A;
    else if (strcmp(fileSystemName, "mntfs") == 0) result = 0x01021994;
    else if (strcmp(fileSystemName, "mqueue") == 0) result = 0x19800202;
    else if (strcmp(fileSystemName, "msdos") == 0) result = 0x4D44;
    else if (strcmp(fileSystemName, "nfs") == 0) result = 0x6969;
    else if (strcmp(fileSystemName, "nfsd") == 0) result = 0x6E667364;
    else if (strcmp(fileSystemName, "nilfs") == 0) result = 0x3434;
    else if (strcmp(fileSystemName, "novell") == 0) result = 0x564C;
    else if (strcmp(fileSystemName, "ntfs") == 0) result = 0x5346544E;
    else if (strcmp(fileSystemName, "objfs") == 0) result = 0x01021994;
    else if (strcmp(fileSystemName, "ocfs2") == 0) result = 0x7461636F;
    else if (strcmp(fileSystemName, "openprom") == 0) result = 0x9FA1;
    else if (strcmp(fileSystemName, "omfs") == 0) result = 0xC2993D87;
    else if (strcmp(fileSystemName, "overlay") == 0) result = 0x794C7630;
    else if (strcmp(fileSystemName, "overlayfs") == 0) result = 0x794C764F;
    else if (strcmp(fileSystemName, "panfs") == 0) result = 0xAAD7AAEA;
    else if (strcmp(fileSystemName, "pipefs") == 0) result = 0x50495045;
    else if (strcmp(fileSystemName, "proc") == 0) result = 0x9FA0;
    else if (strcmp(fileSystemName, "pstorefs") == 0) result = 0x6165676C;
    else if (strcmp(fileSystemName, "qnx4") == 0) result = 0x002F;
    else if (strcmp(fileSystemName, "qnx6") == 0) result = 0x68191122;
    else if (strcmp(fileSystemName, "ramfs") == 0) result = 0x858458F6;
    else if (strcmp(fileSystemName, "reiserfs") == 0) result = 0x52654973;
    else if (strcmp(fileSystemName, "romfs") == 0) result = 0x7275;
    else if (strcmp(fileSystemName, "rootfs") == 0) result = 0x53464846;
    else if (strcmp(fileSystemName, "rpc_pipefs") == 0) result = 0x67596969;
    else if (strcmp(fileSystemName, "samba") == 0) result = 0x517B;
    else if (strcmp(fileSystemName, "sdcardfs") == 0) result = 0x5DCA2DF5;
    else if (strcmp(fileSystemName, "securityfs") == 0) result = 0x73636673;
    else if (strcmp(fileSystemName, "selinux") == 0) result = 0xF97CFF8C;
    else if (strcmp(fileSystemName, "sffs") == 0) result = 0x786F4256;
    else if (strcmp(fileSystemName, "sharefs") == 0) result = 0x01021994;
    else if (strcmp(fileSystemName, "smb") == 0) result = 0x517B;
    else if (strcmp(fileSystemName, "smb2") == 0) result = 0xFE534D42;
    else if (strcmp(fileSystemName, "sockfs") == 0) result = 0x534F434B;
    else if (strcmp(fileSystemName, "squashfs") == 0) result = 0x73717368;
    else if (strcmp(fileSystemName, "sysfs") == 0) result = 0x62656572;
    else if (strcmp(fileSystemName, "sysv2") == 0) result = 0x012FF7B6;
    else if (strcmp(fileSystemName, "sysv4") == 0) result = 0x012FF7B5;
    else if (strcmp(fileSystemName, "tmpfs") == 0) result = 0x01021994;
    else if (strcmp(fileSystemName, "tracefs") == 0) result = 0x74726163;
    else if (strcmp(fileSystemName, "ubifs") == 0) result = 0x24051905;
    else if (strcmp(fileSystemName, "udf") == 0) result = 0x15013346;
    else if (strcmp(fileSystemName, "ufs") == 0) result = 0x00011954;
    else if (strcmp(fileSystemName, "ufscigam") == 0) result = 0x54190100;
    else if (strcmp(fileSystemName, "ufs2") == 0) result = 0x19540119;
    else if (strcmp(fileSystemName, "usbdevice") == 0) result = 0x9FA2;
    else if (strcmp(fileSystemName, "v9fs") == 0) result = 0x01021997;
    else if (strcmp(fileSystemName, "vagrant") == 0) result = 0x786F4256;
    else if (strcmp(fileSystemName, "vboxfs") == 0) result = 0x786F4256;
    else if (strcmp(fileSystemName, "vmhgfs") == 0) result = 0xBACBACBC;
    else if (strcmp(fileSystemName, "vxfs") == 0) result = 0xA501FCF5;
    else if (strcmp(fileSystemName, "vzfs") == 0) result = 0x565A4653;
    else if (strcmp(fileSystemName, "xenfs") == 0) result = 0xABBA1974;
    else if (strcmp(fileSystemName, "xenix") == 0) result = 0x012FF7B4;
    else if (strcmp(fileSystemName, "xfs") == 0) result = 0x58465342;
    else if (strcmp(fileSystemName, "xia") == 0) result = 0x012FD16D;
    else if (strcmp(fileSystemName, "udev") == 0) result = 0x01021994;
    else if (strcmp(fileSystemName, "zfs") == 0) result = 0x2FC12FC1;

    assert(result != 0);
    return result;
}
#endif
#endif /* TARGET_WASI */

uint32_t SystemNative_GetFileSystemType(intptr_t fd)
{
#if HAVE_STATFS_VFS || HAVE_STATFS_MOUNT
    int statfsRes;
    struct statfs statfsArgs;
    // for our needs (get file system type) statfs is always enough and there is no need to use statfs64
    // which got deprecated in macOS 10.6, in favor of statfs
    while ((statfsRes = fstatfs(ToFileDescriptor(fd), &statfsArgs)) == -1 && errno == EINTR) ;
    if (statfsRes == -1) return 0;

#if defined(TARGET_APPLE) || defined(TARGET_FREEBSD)
    // * On OSX-like systems, f_type is version-specific. Don't use it, just map the name.
    // * Specifically, on FreeBSD with ZFS, f_type may return a value like 0xDE when emulating
    //   FreeBSD on macOS (e.g., FreeBSD-x64 on macOS ARM64). Therefore, we use f_fstypename to
    //   get the correct filesystem type.
    return MapFileSystemNameToEnum(statfsArgs.f_fstypename);
#else
    // On Linux, f_type is signed. This causes some filesystem types to be represented as
    // negative numbers on 32-bit platforms. We cast to uint32_t to make them positive.
    uint32_t result = (uint32_t)statfsArgs.f_type;
    return result;
#endif
#elif defined(TARGET_WASI)
    return EINTR;
#elif !HAVE_NON_LEGACY_STATFS
    int statfsRes;
    struct statvfs statfsArgs;
    while ((statfsRes = fstatvfs(ToFileDescriptor(fd), &statfsArgs)) == -1 && errno == EINTR) ;
    if (statfsRes == -1) return 0;

    return MapFileSystemNameToEnum(statfsArgs.f_basetype);
#else
    #error "Platform doesn't support fstatfs or fstatvfs"
#endif
}

int32_t SystemNative_LockFileRegion(intptr_t fd, int64_t offset, int64_t length, int16_t lockType)
{
#if !defined(TARGET_WASI)
    int16_t unixLockType = ConvertLockType(lockType);
    if (offset < 0 || length < 0)
    {
        errno = EINVAL;
        return -1;
    }

#if HAVE_FLOCK64
    struct flock64 lockArgs;
#else
    struct flock lockArgs;
#endif

#if defined(TARGET_ANDROID) && HAVE_FLOCK64
    // On Android, fcntl is always implemented by fcntl64 but before https://github.com/aosp-mirror/platform_bionic/commit/09e77f35ab8d291bf88302bb9673aaa518c6bcb0
    // there was no remapping of F_SETLK to F_SETLK64 when _FILE_OFFSET_BITS=64 (which we set in eng/native/configurecompiler.cmake) so we need to always pass F_SETLK64
    int command = F_SETLK64;
#else
    int command = F_SETLK;
#endif

    lockArgs.l_type = unixLockType;
    lockArgs.l_whence = SEEK_SET;
    lockArgs.l_start = (off_t)offset;
    lockArgs.l_len = (off_t)length;

    int32_t ret;
    while ((ret = fcntl (ToFileDescriptor(fd), command, &lockArgs)) < 0 && errno == EINTR);
    return ret;
#else /* TARGET_WASI */
    return EINTR;
#endif /* TARGET_WASI */
}

int32_t SystemNative_LChflags(const char* path, uint32_t flags)
{
#if HAVE_LCHFLAGS
    int32_t result;
    while ((result = lchflags(path, flags)) < 0 && errno == EINTR);
    return result;
#else
    (void)path, (void)flags;
    errno = ENOTSUP;
    return -1;
#endif
}

int32_t SystemNative_FChflags(intptr_t fd, uint32_t flags)
{
#if HAVE_LCHFLAGS
    int32_t result;
    while ((result = fchflags(ToFileDescriptor(fd), flags)) < 0 && errno == EINTR);
    return result;
#else
    (void)fd, (void)flags;
    errno = ENOTSUP;
    return -1;
#endif
}

int32_t SystemNative_LChflagsCanSetHiddenFlag(void)
{
#if HAVE_LCHFLAGS
    return SystemNative_CanGetHiddenFlag();
#else
    return false;
#endif
}

int32_t SystemNative_CanGetHiddenFlag(void)
{
#if HAVE_STAT_FLAGS && defined(UF_HIDDEN)
    return true;
#else
    return false;
#endif
}

int32_t SystemNative_ReadProcessStatusInfo(pid_t pid, ProcessStatus* processStatus)
{
#ifdef __sun
    char statusFilename[64];
    snprintf(statusFilename, sizeof(statusFilename), "/proc/%d/psinfo", pid);

    intptr_t fd;
    while ((fd = open(statusFilename, O_RDONLY)) < 0 && errno == EINTR);
    if (fd < 0)
    {
        return 0;
    }

    psinfo_t status;
    int result = Common_Read(fd, &status, sizeof(psinfo_t));
    close(fd);
    if (result >= 0)
    {
        processStatus->ResidentSetSize = status.pr_rssize * 1024; // pr_rssize is in Kbytes
        return 1;
    }

    return 0;
#else
    (void)pid, (void)processStatus;
    errno = ENOTSUP;
    return -1;
#endif // __sun
}

int32_t SystemNative_PRead(intptr_t fd, void* buffer, int32_t bufferSize, int64_t fileOffset)
{
    assert(buffer != NULL);
    assert(bufferSize >= 0);

    ssize_t count;
    while ((count = pread(ToFileDescriptor(fd), buffer, (uint32_t)bufferSize, (off_t)fileOffset)) < 0 && errno == EINTR);

    assert(count >= -1 && count <= bufferSize);
    return (int32_t)count;
}

int32_t SystemNative_PWrite(intptr_t fd, void* buffer, int32_t bufferSize, int64_t fileOffset)
{
    assert(buffer != NULL);
    assert(bufferSize >= 0);

    ssize_t count;
    while ((count = pwrite(ToFileDescriptor(fd), buffer, (uint32_t)bufferSize, (off_t)fileOffset)) < 0 && errno == EINTR);

    assert(count >= -1 && count <= bufferSize);
    return (int32_t)count;
}

#if (HAVE_PREADV || HAVE_PWRITEV) && !defined(TARGET_WASM)
static int GetAllowedVectorCount(IOVector* vectors, int32_t vectorCount)
{
#if defined(IOV_MAX)
    const int IovMax = IOV_MAX;
#else
    // In theory all the platforms that we support define IOV_MAX,
    // but we want to be extra safe and provde a fallback
    // in case it turns out to not be true.
    // 16 is low, but supported on every platform.
    const int IovMax = 16;
#endif

    int allowedCount = (int)vectorCount;

    // We need to respect the limit of items that can be passed in iov.
    // In case of writes, the managed code is responsible for handling incomplete writes.
    // In case of reads, we simply returns the number of bytes read and it's up to the users.
    if (IovMax < allowedCount)
    {
        allowedCount = IovMax;
    }

#if defined(TARGET_APPLE)
    // For macOS preadv and pwritev can fail with EINVAL when the total length
    // of all vectors overflows a 32-bit integer.
    size_t totalLength = 0;
    for (int i = 0; i < allowedCount; i++) 
    {
        assert(INT_MAX >= vectors[i].Count);

        totalLength += vectors[i].Count;

        if (totalLength > INT_MAX)
        {
            allowedCount = i;
            break;
        }
    }
#else
    (void)vectors;
#endif

    return allowedCount;
}
#endif // (HAVE_PREADV || HAVE_PWRITEV) && !defined(TARGET_WASM)

int64_t SystemNative_PReadV(intptr_t fd, IOVector* vectors, int32_t vectorCount, int64_t fileOffset)
{
    assert(vectors != NULL);
    assert(vectorCount >= 0);

    int64_t count = 0;
    int fileDescriptor = ToFileDescriptor(fd);
#if HAVE_PREADV && !defined(TARGET_WASM) // preadv is buggy on WASM
    int allowedVectorCount = GetAllowedVectorCount(vectors, vectorCount);
    while ((count = preadv(fileDescriptor, (struct iovec*)vectors, allowedVectorCount, (off_t)fileOffset)) < 0 && errno == EINTR);
#else
    int64_t current;
    for (int i = 0; i < vectorCount; i++)
    {
        IOVector vector = vectors[i];
        while ((current = pread(fileDescriptor, vector.Base, vector.Count, (off_t)(fileOffset + count))) < 0 && errno == EINTR);

        if (current < 0)
        {
            // if previous calls were successful, we return what we got so far
            // otherwise, we return the error code
            return count > 0 ? count : current;
        }

        count += current;

        // Incomplete pread operation may happen for two reasons:
        // a) We have reached EOF.
        // b) The operation was interrupted by a signal handler.
        // To mimic preadv, we stop on the first incomplete operation.
        if (current != (int64_t)vector.Count)
        {
            return count;
        }
    }
#endif

    assert(count >= -1);
    return count;
}

int64_t SystemNative_PWriteV(intptr_t fd, IOVector* vectors, int32_t vectorCount, int64_t fileOffset)
{
    assert(vectors != NULL);
    assert(vectorCount >= 0);

    int64_t count = 0;
    int fileDescriptor = ToFileDescriptor(fd);
#if HAVE_PWRITEV && !defined(TARGET_WASM) // pwritev is buggy on WASM
    int allowedVectorCount = GetAllowedVectorCount(vectors, vectorCount);
    while ((count = pwritev(fileDescriptor, (struct iovec*)vectors, allowedVectorCount, (off_t)fileOffset)) < 0 && errno == EINTR);
#else
    int64_t current;
    for (int i = 0; i < vectorCount; i++)
    {
        IOVector vector = vectors[i];
        while ((current = pwrite(fileDescriptor, vector.Base, vector.Count, (off_t)(fileOffset + count))) < 0 && errno == EINTR);

        if (current < 0)
        {
            // if previous calls were successful, we return what we got so far
            // otherwise, we return the error code
            return count > 0 ? count : current;
        }

        count += current;

        // Incomplete pwrite operation may happen for few reasons:
        // a) There was not enough space available or the file is too large for given file system.
        // b) The operation was interrupted by a signal handler.
        // To mimic pwritev, we stop on the first incomplete operation.
        if (current != (int64_t)vector.Count)
        {
            return count;
        }
    }
#endif

    assert(count >= -1);
    return count;
}
