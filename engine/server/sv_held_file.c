#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* O_TMPFILE and native no-replace publication. */
#endif
#include "quakedef.h"
#include "sv_held_file.h"
#include "fs.h"
#include <stdint.h>
#include <limits.h>
#include <time.h>
#include "sv_held_ledger.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <dirent.h>
#include <sys/syscall.h>
#endif
#endif

/* The VM stream and its native ticket own separate references. Closing the VM
 * stream does not release the source object before final validation. This is
 * ordinary recorder data, never a place to serialize held witness detail. */
struct held_file_ticket_s
{
	unsigned int refs, closed, failed, resized, wrote, owner_set, recovering, recovered;
	char recovery_root[MAX_OSPATH], recovery_generation[33];
	held_source_owner_t owner;
	char relative[MAX_QPATH];
#ifdef _WIN32
	HANDLE file;
	BY_HANDLE_FILE_INFORMATION identity;
#else
	int file, parent;
	struct stat identity;
	char leaf[MAX_QPATH];
#endif
};
typedef struct
{
	vfsfile_t funcs;
	held_file_ticket_t *ticket;
} held_source_stream_t;

static qboolean Held_FileComponent(const char *s, size_t n)
{
	char stem[5];
	size_t i, j;
	if (!n || n >= MAX_QPATH || s[0] == '.' || s[n-1] == '.')
		return false;
	for (i = 0; i < n; ++i)
		if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
		      (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-' || s[i] == '.'))
			return false;
	for (j = 0; j < n && j < sizeof(stem)-1 && s[j] != '.'; ++j)
		stem[j] = (s[j] >= 'a' && s[j] <= 'z') ? s[j]-'a'+'A' : s[j];
	stem[j] = 0;
	if (!strcmp(stem, "CON") || !strcmp(stem, "PRN") || !strcmp(stem, "AUX") || !strcmp(stem, "NUL"))
		return false;
	if (j == 4 && (!strncmp(stem, "COM", 3) || !strncmp(stem, "LPT", 3)) && stem[3] >= '1' && stem[3] <= '9')
		return false;
	return true;
}
static qboolean Held_FileRelative(const char *s)
{
	const char *p, *start;
	if (!s || strncmp(s, "data/parts/", 11) || strlen(s) >= MAX_QPATH)
		return false;
	start = s;
	for (p = s; ; ++p)
		if (!*p || *p == '/')
		{
			if (!Held_FileComponent(start, p-start))
				return false;
			if (!*p)
				return true;
			start = p+1;
		}
}

#ifdef _WIN32
static HANDLE Held_FileWinDir(const wchar_t *path)
{
	BY_HANDLE_FILE_INFORMATION info;
	HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_WRITE,
	                      NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return h;
	if (!GetFileInformationByHandle(h, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
	    (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
	{
		CloseHandle(h);
		return INVALID_HANDLE_VALUE;
	}
	return h;
}
static HANDLE Held_FileWinPrivateCreate(const wchar_t *path)
{
	typedef BOOL (WINAPI *convert_t)(LPCWSTR,DWORD,PSECURITY_DESCRIPTOR*,PULONG);
	HMODULE security = LoadLibraryExW(L"advapi32.dll",NULL,0x00000800);
	PSECURITY_DESCRIPTOR descriptor = NULL;
	SECURITY_ATTRIBUTES attributes;
	convert_t convert;
	HANDLE file = INVALID_HANDLE_VALUE;
	if (!security) return file;
	convert = (void*)GetProcAddress(security,"ConvertStringSecurityDescriptorToSecurityDescriptorW");
	if (convert && convert(L"D:P(A;;FA;;;OW)(A;;FA;;;SY)",1,&descriptor,NULL))
	{
		memset(&attributes,0,sizeof(attributes));
		attributes.nLength = sizeof(attributes); attributes.lpSecurityDescriptor = descriptor;
		file = CreateFileW(path,GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,&attributes,
			CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
	}
	if (descriptor) LocalFree(descriptor);
	FreeLibrary(security);
	return file;
}
static qboolean Held_FileOpen(held_file_ticket_t *t, const char *root, const char *relative)
{
	wchar_t path[MAX_OSPATH], drive[4];
	char joined[MAX_OSPATH];
	HANDLE locks[64];
	size_t i, n, rootlen, count = 0, component;
	qboolean ok = false;
	rootlen = strlen(root);
	if (rootlen < 3 || root[1] != ':' || (root[2] != '/' && root[2] != '\\') ||
	    !((root[0] >= 'A' && root[0] <= 'Z') || (root[0] >= 'a' && root[0] <= 'z')) ||
	    rootlen + strlen(relative) + 2 > sizeof(joined))
		return false;
	Q_snprintfz(joined, sizeof(joined), "%s%s%s", root,
	            (root[rootlen-1] == '/' || root[rootlen-1] == '\\') ? "" : "/", relative);
	widen(path, sizeof(path), joined);
	n = wcslen(path);
	for (i = 0; i < n; ++i)
		if (path[i] == '/') path[i] = '\\';
	/* Convert the root separately: byte offsets are not wchar offsets. */
	widen(drive, sizeof(drive), "C:/");
	drive[0] = path[0]; drive[2] = '\\';
	locks[count] = Held_FileWinDir(drive);
	if (locks[count] == INVALID_HANDLE_VALUE)
		return false;
	++count;
	{
		wchar_t wide_root[MAX_OSPATH];
		widen(wide_root, sizeof(wide_root), root);
		rootlen = wcslen(wide_root);
	}
	component = 3;
	for (i = 3; i < n; ++i)
		if (path[i] == '\\')
		{
			if (i == component || (i-component == 1 && path[component] == '.') ||
			    (i-component == 2 && path[component] == '.' && path[component+1] == '.') || count == countof(locks))
				goto done;
			path[i] = 0;
			/* Only descendants of the already-existing write root may be made. */
			if (i >= rootlen && !CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
				goto done;
			locks[count] = Held_FileWinDir(path);
			path[i] = '\\';
			if (locks[count] == INVALID_HANDLE_VALUE)
				goto done;
			++count;
			component = i+1;
		}
	/* CREATE_NEW, not OPEN_ALWAYS/CREATE_ALWAYS: occupied and unreadable are
	 * both refusal. Parent handles deny rename while the path is traversed. */
	t->file = Held_FileWinPrivateCreate(path);
	if (t->file == INVALID_HANDLE_VALUE)
		goto done;
	if (!GetFileInformationByHandle(t->file, &t->identity) ||
	    (t->identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
	    t->identity.nNumberOfLinks != 1 || GetFileType(t->file) != FILE_TYPE_DISK)
	{
		CloseHandle(t->file);
		t->file = INVALID_HANDLE_VALUE;
		goto done;
	}
	ok = true;
done:
	while (count) CloseHandle(locks[--count]);
	return ok;
}
#else
static int Held_FilePosixDir(int parent, const char *name, qboolean create)
{
	int fd;
	struct stat st;
	if (create && mkdirat(parent, name, 0700) && errno != EEXIST)
		return -1;
	fd = openat(parent, name, O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || !S_ISDIR(st.st_mode))
	{
		close(fd);
		return -1;
	}
	return fd;
}
static qboolean Held_FileWalk(int *fd, char *path, qboolean create)
{
	char *p, *start = path;
	int next;
	for (p = path; ; ++p)
		if (!*p || *p == '/')
		{
			int end = !*p;
			*p = 0;
			if (!*start || !strcmp(start, ".") || !strcmp(start, ".."))
				return false;
			next = Held_FilePosixDir(*fd, start, create);
			if (next < 0)
				return false;
			close(*fd); *fd = next;
			if (end) return true;
			start = p+1;
		}
}
static qboolean Held_FileOpen(held_file_ticket_t *t, const char *root, const char *relative)
{
	char path[MAX_OSPATH], rel[MAX_QPATH], *leaf;
	size_t n = strlen(root);
	if (!n || root[0] != '/' || n >= sizeof(path))
		return false;
	Q_strncpyz(path, root+1, sizeof(path));
	n = strlen(path);
	while (n && path[n-1] == '/') path[--n] = 0;
	t->parent = open("/", O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
	if (t->parent < 0)
		return false;
	if (*path && !Held_FileWalk(&t->parent, path, false))
		goto fail;
	Q_strncpyz(rel, relative, sizeof(rel));
	leaf = strrchr(rel, '/');
	*leaf++ = 0;
	if (!Held_FileWalk(&t->parent, rel, true))
		goto fail;
	Q_strncpyz(t->leaf, leaf, sizeof(t->leaf));
	t->file = openat(t->parent, leaf, O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, 0600);
	if (t->file < 0)
		goto fail;
	if (fstat(t->file, &t->identity) || !S_ISREG(t->identity.st_mode) ||
	    t->identity.st_nlink != 1 || t->identity.st_uid != geteuid())
	{
		close(t->file);
		goto fail;
	}
	return true;
fail:
	close(t->parent);
	return false;
}
#endif

static void Held_FileDrop(held_file_ticket_t *t)
{
	if (--t->refs) return;
#ifdef _WIN32
	CloseHandle(t->file);
#else
	close(t->file); close(t->parent);
#endif
	memset(t, 0, sizeof(*t));
	Z_Free(t);
}
static int Held_FileTransfer(held_file_ticket_t *t, void *data, int bytes, qboolean writing)
{
	int result;
	if (bytes < 0 || t->failed) return -1;
	if (writing && bytes) t->wrote = 1;
#ifdef _WIN32
	{
		DWORD done;
		qboolean ok = writing ? WriteFile(t->file, data, bytes, &done, NULL) : ReadFile(t->file, data, bytes, &done, NULL);
		result = ok ? (int)done : -1;
	}
#else
	do { result = writing ? write(t->file, data, bytes) : read(t->file, data, bytes); } while (result < 0 && errno == EINTR);
#endif
	if (result < 0 || (writing && result != bytes)) t->failed = 1;
	return result;
}
static qboolean Held_FileSeekTo(held_file_ticket_t *t, qofs_t offset)
{
#ifdef _WIN32
	LARGE_INTEGER pos;
	pos.QuadPart = (int64_t)offset;
	if (pos.QuadPart < 0 || (qofs_t)pos.QuadPart != offset || !SetFilePointerEx(t->file, pos, NULL, FILE_BEGIN))
#else
	off_t pos = (off_t)offset;
	if (pos < 0 || (qofs_t)pos != offset || lseek(t->file, pos, SEEK_SET) != pos)
#endif
	{
		t->failed = 1;
		return false;
	}
	return true;
}
static int QDECL Held_SourceRead(vfsfile_t *f, void *data, int bytes)
{
	return Held_FileTransfer(((held_source_stream_t*)f)->ticket, data, bytes, false);
}
static int QDECL Held_SourceWrite(vfsfile_t *f, const void *data, int bytes)
{
	return Held_FileTransfer(((held_source_stream_t*)f)->ticket, (void*)data, bytes, true);
}
static qboolean QDECL Held_SourceSeek(vfsfile_t *f, qofs_t offset)
{
	return Held_FileSeekTo(((held_source_stream_t*)f)->ticket, offset);
}
static qofs_t QDECL Held_SourceTell(vfsfile_t *f)
{
	held_file_ticket_t *t = ((held_source_stream_t*)f)->ticket;
#ifdef _WIN32
	LARGE_INTEGER zero, pos;
	zero.QuadPart = 0;
	if (SetFilePointerEx(t->file, zero, &pos, FILE_CURRENT)) return (qofs_t)pos.QuadPart;
#else
	off_t pos = lseek(t->file, 0, SEEK_CUR);
	if (pos >= 0) return (qofs_t)pos;
#endif
	t->failed = 1;
	return (qofs_t)-1;
}
static qofs_t QDECL Held_SourceLength(vfsfile_t *f)
{
	held_file_ticket_t *t = ((held_source_stream_t*)f)->ticket;
#ifdef _WIN32
	LARGE_INTEGER size;
	if (GetFileSizeEx(t->file, &size) && size.QuadPart >= 0) return (qofs_t)size.QuadPart;
#else
	struct stat st;
	if (!fstat(t->file, &st) && st.st_size >= 0) return (qofs_t)st.st_size;
#endif
	t->failed = 1;
	return (qofs_t)-1;
}
static void QDECL Held_SourceFlush(vfsfile_t *f)
{
	held_file_ticket_t *t = ((held_source_stream_t*)f)->ticket;
#ifdef _WIN32
	if (!FlushFileBuffers(t->file)) t->failed = 1;
#else
	int result;
	do { result = fsync(t->file); } while (result && errno == EINTR);
	if (result) t->failed = 1;
#endif
}
static qboolean QDECL Held_SourceClose(vfsfile_t *f)
{
	held_file_ticket_t *t = ((held_source_stream_t*)f)->ticket;
	qboolean ok;
	Held_SourceFlush(f);
	t->closed = 1;
	ok = !t->failed;
	if (t->recovering && !SV_HeldFileRecover(t,t->recovery_root,t->recovery_generation)) ok = false;
	Held_FileDrop(t);
	Z_Free(f);
	return ok;
}
vfsfile_t *SV_HeldFileCreate(const char *relative, held_file_ticket_t **ticket)
{
	char root[MAX_OSPATH];
	held_file_ticket_t *t;
	held_source_stream_t *f;
	if (!ticket) return NULL;
	*ticket = NULL;
	if (fs_readonly || !Held_FileRelative(relative) || !FS_SystemPath("", FS_GAMEONLY, root, sizeof(root)))
		return NULL;
	t = Z_Malloc(sizeof(*t));
	memset(t, 0, sizeof(*t));
	if (!Held_FileOpen(t, root, relative))
	{
		Z_Free(t);
		return NULL;
	}
	t->refs = 2;
	Q_strncpyz(t->relative, relative, sizeof(t->relative));
	FS_FlushFSHashWritten(relative);
	f = Z_Malloc(sizeof(*f));
	memset(f, 0, sizeof(*f));
	f->ticket = t;
	f->funcs.ReadBytes = Held_SourceRead;
	f->funcs.WriteBytes = Held_SourceWrite;
	f->funcs.Seek = Held_SourceSeek;
	f->funcs.Tell = Held_SourceTell;
	f->funcs.GetLen = Held_SourceLength;
	f->funcs.Flush = Held_SourceFlush;
	f->funcs.Close = Held_SourceClose;
	f->funcs.seekstyle = SS_SEEKABLE;
	*ticket = t;
	return &f->funcs;
}
qboolean SV_HeldFileOwnsStream(vfsfile_t *f, held_file_ticket_t *t)
{
	return f && t && f->Close == Held_SourceClose && ((held_source_stream_t*)f)->ticket == t && !t->closed && !t->failed;
}
static qboolean Held_FileOwnerValid(const held_source_owner_t *owner)
{
	const char *p;
	if (!owner || !owner->vm || !owner->client_uid || !owner->entity_index ||
	    owner->record_slot >= 2048 || !owner->serial || owner->serial > 16777215 ||
	    !memchr(owner->record, 0, sizeof(owner->record)) || !*owner->record) return false;
	for (p = owner->record; *p; ++p)
		if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
		      (*p >= 'A' && *p <= 'Z') || *p == '-')) return false;
	return true;
}
qboolean SV_HeldFileSetOwner(held_file_ticket_t *t, const held_source_owner_t *owner)
{
	if (!t || t->closed || t->failed || t->resized || t->wrote || t->owner_set || !Held_FileOwnerValid(owner))
		return false;
	t->owner = *owner;
	t->owner_set = 1;
	return true;
}
held_file_ticket_t *SV_HeldFileAcquire(vfsfile_t *f, const held_source_owner_t *owner)
{
	held_file_ticket_t *t;
	if (!f || f->Close != Held_SourceClose || !Held_FileOwnerValid(owner)) return NULL;
	t = ((held_source_stream_t*)f)->ticket;
	if (t->closed || t->failed || t->resized || !t->owner_set || t->refs == ~0u ||
	    t->owner.vm != owner->vm || t->owner.client_uid != owner->client_uid ||
	    t->owner.entity_index != owner->entity_index || t->owner.record_slot != owner->record_slot ||
	    t->owner.serial != owner->serial || strcmp(t->owner.record, owner->record)) return NULL;
	++t->refs;
	return t;
}
int SV_HeldFileResize(vfsfile_t *f, qofs_t length)
{
	held_file_ticket_t *t;
	qofs_t oldpos;
	qboolean ok;
	if (!f || f->Close != Held_SourceClose) return -1;
	t = ((held_source_stream_t*)f)->ticket;
	if (t->closed || t->failed) return 0;
	oldpos = Held_SourceTell(f);
	if (!Held_FileSeekTo(t, length)) return 0;
#ifdef _WIN32
	ok = SetEndOfFile(t->file) != 0;
#else
	ok = !ftruncate(t->file, (off_t)length);
#endif
	if (!ok)
	{
		t->failed = 1;
		return 0;
	}
	t->resized = 1;                 /* Never a fresh initial-only candidate again. */
	if (!Held_FileSeekTo(t, min(oldpos, length))) return 0;
	return 1;
}
qboolean SV_HeldFilePristine(held_file_ticket_t *t)
{
	return t && !t->failed && !t->resized;
}
qboolean SV_HeldFileClosed(held_file_ticket_t *t)
{
	return t && t->closed && !t->failed;
}
qofs_t SV_HeldFileLength(held_file_ticket_t *t)
{
	if (!SV_HeldFileClosed(t)) return -1;
#ifdef _WIN32
	{
		BY_HANDLE_FILE_INFORMATION now;
		LARGE_INTEGER length;
		if (!GetFileInformationByHandle(t->file,&now) || now.nNumberOfLinks != 1 ||
			now.dwVolumeSerialNumber != t->identity.dwVolumeSerialNumber ||
			now.nFileIndexHigh != t->identity.nFileIndexHigh || now.nFileIndexLow != t->identity.nFileIndexLow ||
			!GetFileSizeEx(t->file,&length) || length.QuadPart < 0) return -1;
		return length.QuadPart;
	}
#else
	{
		struct stat now, named;
		if (fstat(t->file,&now) || !S_ISREG(now.st_mode) || now.st_nlink != 1 || now.st_size < 0 ||
			now.st_dev != t->identity.st_dev || now.st_ino != t->identity.st_ino ||
			fstatat(t->parent,t->leaf,&named,AT_SYMLINK_NOFOLLOW) || !S_ISREG(named.st_mode) ||
			named.st_dev != now.st_dev || named.st_ino != now.st_ino) return -1;
		return now.st_size;
	}
#endif
}
int SV_HeldFileReadAt(held_file_ticket_t *t, qofs_t offset, void *data, int bytes)
{
	if (!SV_HeldFileClosed(t) || !Held_FileSeekTo(t, offset)) return -1;
	return Held_FileTransfer(t, data, bytes, false);
}
qboolean SV_HeldFileRetire(held_file_ticket_t *t, qboolean consume)
{
	qboolean ok = true;
	if (!t) return false;
	if (consume && t->recovered) {Held_FileDrop(t); return false;}
	if (consume)
	{
		ok = SV_HeldFileClosed(t);
#ifdef _WIN32
		if (ok)
		{
			FILE_DISPOSITION_INFO info;
			BY_HANDLE_FILE_INFORMATION now;
			BOOL (WINAPI *setinfo)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
			/* Do not add a mandatory post-XP import to an otherwise inactive feature. */
			setinfo = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetFileInformationByHandle");
			ok = setinfo && GetFileInformationByHandle(t->file, &now) && now.nNumberOfLinks == 1 &&
			     now.dwVolumeSerialNumber == t->identity.dwVolumeSerialNumber &&
			     now.nFileIndexHigh == t->identity.nFileIndexHigh && now.nFileIndexLow == t->identity.nFileIndexLow;
			info.DeleteFile = TRUE;
			if (ok) ok = setinfo(t->file, FileDispositionInfo, &info, sizeof(info)) != 0;
		}
#else
		if (ok)
		{
			struct stat st;
			ok = !fstatat(t->parent, t->leaf, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) &&
			     st.st_dev == t->identity.st_dev && st.st_ino == t->identity.st_ino && st.st_nlink == 1;
			if (ok) ok = !unlinkat(t->parent, t->leaf, 0);
		}
#endif
	}
	if (consume && ok) FS_FlushFSHashRemoved(t->relative);
	Held_FileDrop(t);
	return ok;
}

int SV_HeldFileProbe(const char *relative)
{
	const char bytes[] = "ordinary-source-probe";
	held_file_ticket_t *ticket = NULL;
	vfsfile_t *f;
	qboolean ok, retired;
	if (!relative || strncmp(relative, "data/parts/held-file-test-", 26))
		return -2;
	f = SV_HeldFileCreate(relative, &ticket);
	if (!f) return 0;
	ok = VFS_WRITE(f, bytes, sizeof(bytes)) == (int)sizeof(bytes);
	if (!VFS_CLOSE(f)) ok = false;
	retired = SV_HeldFileRetire(ticket, ok);
	return (ok && retired) ? 1 : -1;
}

// Native filesystem sink shares only the checked OS path/object helpers.
static int Held_FileRecoveryTest(const char *stage_root, unsigned int *checks);
static int Held_FileRetentionTest(const char *stage_root, unsigned int *checks);
#include "sv_held_store.inc"
#include "sv_held_recovery.inc"
#include "sv_held_retention.inc"

int SV_HeldFileTest(unsigned int *checks)
{
	static unsigned int serial;
	char path[MAX_QPATH], data[32];
	const char source[] = "ordinary-source";
	held_file_ticket_t *t = NULL, *other = NULL, *borrow;
	held_source_owner_t owner, wrong;
	vfsfile_t *f, *occupied, *prefix;
	unsigned int n = 0;
	int failed = 0;
#define CHECK(x) do { ++n; if (!(x)) ++failed; } while (0)
	Q_snprintfz(path, sizeof(path), "data/parts/held-file-test-%u.rec", ++serial);
	CHECK(!Held_FileRelative("data/parts/../outside.rec"));
	CHECK(!Held_FileRelative("data/parts//slot.rec"));
	CHECK(!Held_FileRelative("data/parts/CON.rec"));
	CHECK(!Held_FileRelative("data/parts/slot.rec."));
	CHECK(!Held_FileRelative("data/runs/slot.rec"));
	f = SV_HeldFileCreate(path, &t);
	CHECK(f && t);
	if (!f) { *checks = n; return failed; }
	CHECK(SV_HeldFileOwnsStream(f, t) && SV_HeldFilePristine(t));
	CHECK(!SV_HeldFileClosed(t));
	memset(&owner, 0, sizeof(owner));
	owner.vm = &serial; owner.client_uid = 17; owner.entity_index = 1;
	owner.serial = 1; Q_strncpyz(owner.record, "unit-1", sizeof(owner.record));
	CHECK(!SV_HeldFileAcquire(f, &owner));
	CHECK(SV_HeldFileSetOwner(t, &owner));
	CHECK(!SV_HeldFileSetOwner(t, &owner));
	borrow = SV_HeldFileAcquire(f, &owner);
	CHECK(borrow == t);
	CHECK(borrow && SV_HeldFileRetire(borrow, false));
	wrong = owner; wrong.vm = &n; CHECK(!SV_HeldFileAcquire(f, &wrong));
	wrong = owner; ++wrong.client_uid; CHECK(!SV_HeldFileAcquire(f, &wrong));
	wrong = owner; ++wrong.entity_index; CHECK(!SV_HeldFileAcquire(f, &wrong));
	wrong = owner; ++wrong.record_slot; CHECK(!SV_HeldFileAcquire(f, &wrong));
	wrong = owner; ++wrong.serial; CHECK(!SV_HeldFileAcquire(f, &wrong));
	wrong = owner; wrong.record[0] = 'x'; CHECK(!SV_HeldFileAcquire(f, &wrong));
	CHECK(VFS_WRITE(f, source, sizeof(source)) == (int)sizeof(source));
	CHECK(VFS_TELL(f) == sizeof(source) && VFS_GETLEN(f) == sizeof(source));
	occupied = SV_HeldFileCreate(path, &other);
	CHECK(!occupied && !other);
	if (occupied) { VFS_CLOSE(occupied); SV_HeldFileRetire(other, false); }
	CHECK(VFS_SEEK(f, 0));
	CHECK(VFS_READ(f, data, sizeof(source)) == (int)sizeof(source) && !memcmp(data, source, sizeof(source)));
	prefix = FS_OpenVFS(path, "rb", FS_GAMEONLY);
	CHECK(prefix != NULL);
	if (prefix)
	{
		CHECK(VFS_READ(prefix, data, sizeof(source)) == (int)sizeof(source) && !memcmp(data, source, sizeof(source)));
		CHECK(VFS_CLOSE(prefix));
	}
	CHECK(SV_HeldFileResize(f, 4) == 1);
	CHECK(VFS_GETLEN(f) == 4 && VFS_TELL(f) == 4);
	CHECK(!SV_HeldFilePristine(t));
	CHECK(!SV_HeldFileAcquire(f, &owner));
	CHECK(VFS_SEEK(f, 0) && VFS_WRITE(f, source, sizeof(source)) == (int)sizeof(source));
	CHECK(VFS_GETLEN(f) == sizeof(source));
	CHECK(VFS_CLOSE(f));
	CHECK(SV_HeldFileClosed(t));
	CHECK(SV_HeldFileReadAt(t, 0, data, sizeof(source)) == (int)sizeof(source) && !memcmp(data, source, sizeof(source)));
	CHECK(SV_HeldFileRetire(t, true));
	f = SV_HeldFileCreate(path, &t);
	CHECK(f && t);
	if (f)
	{
		CHECK(VFS_WRITE(f, source, sizeof(source)) == (int)sizeof(source));
		CHECK(!SV_HeldFileSetOwner(t, &owner)); /* Not already stamped: wrote is the gate. */
		CHECK(VFS_CLOSE(f));
		CHECK(SV_HeldFileRetire(t, true));
	}
#undef CHECK
	*checks = n;
	return failed;
}
