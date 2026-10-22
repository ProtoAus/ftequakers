/* Filesystem sink only. Native source/model/namespace validation is mandatory
 * both before staging and immediately before the single no-replace commit. */
static unsigned int held_store_fault;
static void Held_StoreWipe(void *memory, size_t size)
{
	volatile unsigned char *p = memory;
	while (size--) *p++ = 0;
}
qboolean SV_HeldStoreHealthy(void)
{
#if defined(_WIN64) || (defined(__linux__) && defined(O_TMPFILE) && defined(AT_SYMLINK_FOLLOW))
	return !held_store_fault;
#else
	return false;
#endif
}
typedef struct
{
	const unsigned char *bytes;
	held_reader_t input;
	void *input_context;
	size_t size;
	unsigned char digest[32];
	char generation[33], destination[MAX_OSPATH];
	unsigned int committed, commit_attempted;
#ifdef _WIN32
	HANDLE file, reader, dirs[128];
	unsigned int numdirs;
	wchar_t canonical[MAX_OSPATH], stage[MAX_OSPATH], target[MAX_OSPATH];
	BY_HANDLE_FILE_INFORMATION identity;
	BOOL (WINAPI *setinfo)(HANDLE, int, LPVOID, DWORD);
	DWORD (WINAPI *finalpath)(HANDLE, LPWSTR, DWORD, DWORD);
#else
	int file, reader, stage_dir, target_dir;
	struct stat identity;
	char canonical[MAX_OSPATH], leaf[MAX_QPATH];
#endif
} held_store_t;
static int Held_StoreArguments(const char *destination, const char *generation)
{
	const char *p, *start;
	size_t size;
	if (!destination || !generation || strlen(generation) != 32) return 0;
	for (p = generation; *p; ++p)
		if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return 0;
	if (strncmp(destination,"data/runs/",10) && strncmp(destination,"data/evidence/",14)) return 0;
	size = strlen(destination);
	if (size >= MAX_OSPATH || size < 15 || strcmp(destination+size-4,".rec")) return 0;
	start = destination;
	for (p = start; ; ++p)
		if (!*p || *p == '/')
		{
			if (!Held_FileComponent(start,p-start)) return 0;
			if (!*p) return 1;
			start = p+1;
		}
}
#ifdef _WIN32
static int Held_StoreWinDirs(held_store_t *s, const char *native, size_t create_below)
{
	wchar_t path[MAX_OSPATH], root[4];
	size_t i, size, start = 3;
	if (!native || strlen(native) >= MAX_OSPATH) return 0;
	widen(path,sizeof(path),native); size = wcslen(path);
	if (size < 3 || path[1] != ':' || (path[2] != '/' && path[2] != '\\') ||
		!((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))) return 0;
	for (i = 0; i < size; ++i) if (path[i] == '/') path[i] = '\\';
	while (size > 3 && path[size-1] == '\\') path[--size] = 0;
	root[0] = path[0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
	if (s->numdirs == countof(s->dirs)) return 0;
	s->dirs[s->numdirs] = Held_FileWinDir(root);
	if (s->dirs[s->numdirs] == INVALID_HANDLE_VALUE) return 0;
	++s->numdirs;
	if (size == 3) return 1;
	for (i = 3; i <= size; ++i)
		if (!path[i] || path[i] == '\\')
		{
			wchar_t saved = path[i];
			if (i == start || (i-start == 1 && path[start] == '.') ||
				(i-start == 2 && path[start] == '.' && path[start+1] == '.') || s->numdirs == countof(s->dirs)) return 0;
			path[i] = 0;
			if (create_below && i > create_below && !CreateDirectoryW(path,NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
			s->dirs[s->numdirs] = Held_FileWinDir(path);
			path[i] = saved;
			if (s->dirs[s->numdirs] == INVALID_HANDLE_VALUE) return 0;
			++s->numdirs; start = i+1;
		}
	return 1;
}
static int Held_StoreWinCanonical(held_store_t *s, HANDLE file, wchar_t *path)
{
	DWORD size = s->finalpath(file,path,MAX_OSPATH,1); /* Actual volume-GUID path, not a drive alias. */
	return size && size < MAX_OSPATH;
}
static qboolean Held_StoreOutside(const char *root, void *opaque)
{
	held_store_t *s = opaque;
	wchar_t native[MAX_OSPATH], path[MAX_OSPATH];
	BY_HANDLE_FILE_INFORMATION info;
	HANDLE h;
	size_t a, b;
	int ok;
	if (!root || !*root || strlen(root) >= MAX_OSPATH) return false;
	widen(native,sizeof(native),root);
	/* Existing mounted roots may themselves be junctions: resolve their TARGET,
	 * while staging traversal separately rejects every reparse component. */
	h = CreateFileW(native,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
		NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
	if (h == INVALID_HANDLE_VALUE) return false;
	ok = GetFileInformationByHandle(h,&info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
		Held_StoreWinCanonical(s,h,path);
	if (!CloseHandle(h)) ok = 0;
	if (!ok) return false;
	a = wcslen(path); b = wcslen(s->canonical);
	while (a && path[a-1] == '\\') --a;
	while (b && s->canonical[b-1] == '\\') --b;
	if (a <= b && !_wcsnicmp(path,s->canonical,a) && (!s->canonical[a] || s->canonical[a] == '\\')) return false;
	if (b <= a && !_wcsnicmp(path,s->canonical,b) && (!path[b] || path[b] == '\\')) return false;
	return true;
}
static int Held_StoreWinRegular(HANDLE file, BY_HANDLE_FILE_INFORMATION *info)
{
	return GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file,info) &&
		!(info->dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) && info->nNumberOfLinks == 1;
}
static int Held_StoreWinSame(held_store_t *s, HANDLE file)
{
	BY_HANDLE_FILE_INFORMATION info;
	return Held_StoreWinRegular(file,&info) && info.dwVolumeSerialNumber == s->identity.dwVolumeSerialNumber &&
		info.nFileIndexHigh == s->identity.nFileIndexHigh && info.nFileIndexLow == s->identity.nFileIndexLow &&
		!info.nFileSizeHigh && info.nFileSizeLow == s->size;
}
static int Held_StoreWinDelete(held_store_t *s, HANDLE file)
{
	struct {BYTE delete_file;} disposition = {1};
	return s->setinfo(file,4,&disposition,sizeof(disposition));
}
static int Held_StoreOpen(held_store_t *s, const char *stage_root, const char *write_root, const char *parent)
{
	typedef BOOL (WINAPI *convert_t)(LPCWSTR,DWORD,PSECURITY_DESCRIPTOR*,PULONG);
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll"), security;
	convert_t convert;
	PSECURITY_DESCRIPTOR descriptor = NULL;
	SECURITY_ATTRIBUTES attributes;
	BY_HANDLE_FILE_INFORMATION directory;
	wchar_t root[MAX_OSPATH], probe[MAX_OSPATH];
	char joined[MAX_OSPATH];
	HANDLE empty = INVALID_HANDLE_VALUE;
	DWORD clear = 8;
	size_t size;
	int ok = 0;
	if (!kernel || sizeof(void*) != 8) return 0;
	s->setinfo = (void*)GetProcAddress(kernel,"SetFileInformationByHandle");
	s->finalpath = (void*)GetProcAddress(kernel,"GetFinalPathNameByHandleW");
	if (!s->setinfo || !s->finalpath || !Held_StoreWinDirs(s,stage_root,0) ||
		!Held_StoreWinCanonical(s,s->dirs[s->numdirs-1],s->canonical) ||
		!FS_ValidateExternalDirectory(Held_StoreOutside,s)) return 0;
	widen(root,sizeof(root),write_root); size = wcslen(root);
	while (size > 3 && (root[size-1] == '/' || root[size-1] == '\\')) --size;
	if (!Held_StoreWinDirs(s,parent,size) || !GetFileInformationByHandle(s->dirs[s->numdirs-1],&directory)) return 0;
	security = LoadLibraryExW(L"advapi32.dll",NULL,0x00000800); /* System32 only. */
	if (!security) return 0;
	convert = (void*)GetProcAddress(security,"ConvertStringSecurityDescriptorToSecurityDescriptorW");
	if (convert) ok = convert(L"D:P(A;;FA;;;OW)(A;;FA;;;SY)",1,&descriptor,NULL);
	FreeLibrary(security);
	if (!ok) return 0;
	ok = 0;
	memset(&attributes,0,sizeof(attributes)); attributes.nLength = sizeof(attributes); attributes.lpSecurityDescriptor = descriptor;
	size = strlen(stage_root);
	if (size+60 >= sizeof(joined)) goto done;
	Q_snprintfz(joined,sizeof(joined),"%s%sheld-%s.probe",stage_root,
		(stage_root[size-1] == '/' || stage_root[size-1] == '\\') ? "":"/",s->generation);
	widen(probe,sizeof(probe),joined);
	empty = CreateFileW(probe,GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,&attributes,CREATE_NEW,
		FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
	if (empty == INVALID_HANDLE_VALUE) goto done;
	ok = s->setinfo(empty,21,&clear,sizeof(clear)) && Held_StoreWinDelete(s,empty);
	if (!CloseHandle(empty)) ok = 0;
	empty = INVALID_HANDLE_VALUE;
	if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND)
	{
		held_store_fault = 1;
		Con_Printf(CON_ERROR "held staging cleanup ERROR (owned held-%s.probe); collection disabled\n",s->generation);
		ok = 0;
	}
	if (!ok) goto done;
	Q_snprintfz(joined,sizeof(joined),"%s%sheld-%s.tmp",stage_root,
		(stage_root[size-1] == '/' || stage_root[size-1] == '\\') ? "":"/",s->generation);
	widen(s->stage,sizeof(s->stage),joined);
	s->file = CreateFileW(s->stage,GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,&attributes,CREATE_NEW,
		FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
	ok = s->file != INVALID_HANDLE_VALUE && Held_StoreWinRegular(s->file,&s->identity) &&
		s->identity.dwVolumeSerialNumber == directory.dwVolumeSerialNumber;
	if (!FS_SystemPath(s->destination,FS_GAMEONLY,joined,sizeof(joined))) ok = 0;
	if (ok) widen(s->target,sizeof(s->target),joined);
 done:
	if (empty != INVALID_HANDLE_VALUE) CloseHandle(empty);
	LocalFree(descriptor);
	return ok;
}
#else
static int Held_StoreResolved(const char *root, char path[MAX_OSPATH], struct stat *st)
{
	char *resolved = realpath(root,NULL);
	int valid = resolved && strlen(resolved) < MAX_OSPATH && !stat(resolved,st) && S_ISDIR(st->st_mode);
	if (valid) Q_strncpyz(path,resolved,MAX_OSPATH);
	free(resolved);
	return valid;
}
static qboolean Held_StoreOutside(const char *root, void *opaque)
{
	held_store_t *s = opaque;
	char path[MAX_OSPATH];
	struct stat st;
	size_t a, b;
	if (!Held_StoreResolved(root,path,&st)) return false;
	a = strlen(path); b = strlen(s->canonical);
	while (a > 1 && path[a-1] == '/') --a;
	while (b > 1 && s->canonical[b-1] == '/') --b;
	if (a == 1 || b == 1) return false;
	if (a <= b && !memcmp(path,s->canonical,a) && (!s->canonical[a] || s->canonical[a] == '/')) return false;
	if (b <= a && !memcmp(path,s->canonical,b) && (!path[b] || path[b] == '/')) return false;
	return true;
}
static int Held_StorePosixRoot(const char *root)
{
	char path[MAX_OSPATH];
	size_t size;
	int fd;
	if (!root || *root != '/' || strlen(root) >= sizeof(path)) return -1;
	Q_strncpyz(path,root+1,sizeof(path)); size = strlen(path);
	while (size && path[size-1] == '/') path[--size] = 0;
	fd = open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
	if (fd >= 0 && *path && !Held_FileWalk(&fd,path,false)) {close(fd); return -1;}
	return fd;
}
static int Held_StoreOpen(held_store_t *s, const char *stage_root, const char *write_root, const char *parent)
{
#if defined(__linux__) && defined(O_TMPFILE)
	struct stat dir, resolved, target;
	char relative[MAX_OSPATH];
	size_t size = strlen(write_root);
	s->stage_dir = Held_StorePosixRoot(stage_root);
	if (s->stage_dir < 0 || fstat(s->stage_dir,&dir) || dir.st_uid != geteuid() || (dir.st_mode & 0077) ||
		!Held_StoreResolved(stage_root,s->canonical,&resolved) ||
		dir.st_dev != resolved.st_dev || dir.st_ino != resolved.st_ino ||
		!FS_ValidateExternalDirectory(Held_StoreOutside,s)) return 0;
	s->target_dir = Held_StorePosixRoot(write_root);
	if (s->target_dir < 0 || strncmp(parent,write_root,size)) return 0;
	while (parent[size] == '/') ++size;
	Q_strncpyz(relative,parent+size,sizeof(relative));
	if (*relative && !Held_FileWalk(&s->target_dir,relative,true)) return 0;
	if (fstat(s->target_dir,&target) || dir.st_dev != target.st_dev) return 0;
	s->file = openat(s->stage_dir,".",O_TMPFILE|O_RDWR|O_CLOEXEC,0600);
	return s->file >= 0 && !fstat(s->file,&s->identity) && S_ISREG(s->identity.st_mode) &&
		s->identity.st_uid == geteuid() && s->identity.st_nlink == 0 && s->identity.st_dev == target.st_dev;
#else
	return 0;
#endif
}
#endif
static int Held_StoreFetch(held_store_t *s, size_t offset, void *buffer, size_t count)
{
	if (offset > s->size || count > s->size-offset || count > 4096) return 0;
	if (s->input) return s->input(s->input_context,offset,buffer,count) == count;
	if (!s->bytes) return 0;
	memcpy(buffer,s->bytes+offset,count);
	return 1;
}
static int Held_StoreWrite(held_store_t *s)
{
	unsigned char buffer[4096];
	size_t offset = 0, count;
	int valid = 0;
#ifdef _WIN32
	DWORD written;
	HANDLE lease;
#else
	ssize_t written;
	char proc[64];
#endif
	while (offset < s->size)
	{
		count = min(sizeof(buffer),s->size-offset);
		if (!Held_StoreFetch(s,offset,buffer,count)) goto done;
#ifdef _WIN32
		if (!WriteFile(s->file,buffer,(DWORD)count,&written,NULL) || !written) goto done;
#else
		do {written = write(s->file,buffer,count);} while (written < 0 && errno == EINTR);
		if (written <= 0) goto done;
#endif
		offset += written;
	}
#ifdef _WIN32
	if (!FlushFileBuffers(s->file) || !DuplicateHandle(GetCurrentProcess(),s->file,GetCurrentProcess(),&lease,0,FALSE,DUPLICATE_SAME_ACCESS)) goto done;
	if (!CloseHandle(s->file)) {CloseHandle(lease); goto done;}
	s->file = lease; /* Same object pins delete-on-close across writer-handle close. */
	s->reader = CreateFileW(s->stage,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,
		OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
	valid = s->reader != INVALID_HANDLE_VALUE && Held_StoreWinSame(s,s->reader);
#else
	if (fsync(s->file)) goto done;
	Q_snprintfz(proc,sizeof(proc),"/proc/self/fd/%d",s->file);
	s->reader = open(proc,O_RDONLY|O_CLOEXEC);
	if (s->reader < 0) goto done;
	if (close(s->file)) {s->file = -1; goto done;}
	s->file = s->reader; s->reader = -1;
	valid = 1;
#endif
done:
	Held_StoreWipe(buffer,sizeof(buffer));
	return valid;
}
static int Held_StoreReadback(held_store_t *s)
{
	union {uint64_t align; unsigned char bytes[512];} hashcontext;
	unsigned char buffer[4096], expected[4096], digest[32];
	size_t offset = 0, count;
	int valid = 0;
#ifdef _WIN32
	DWORD got;
	LARGE_INTEGER zero;
	zero.QuadPart = 0;
	if (!Held_StoreWinSame(s,s->reader) || !SetFilePointerEx(s->reader,zero,NULL,FILE_BEGIN)) goto done;
#else
	ssize_t got;
	struct stat st;
	if (fstat(s->file,&st) || st.st_dev != s->identity.st_dev || st.st_ino != s->identity.st_ino ||
		!S_ISREG(st.st_mode) || st.st_nlink || st.st_size != s->size || lseek(s->file,0,SEEK_SET)) goto done;
#endif
	if (hash_sha2_256.contextsize > sizeof(hashcontext.bytes) || hash_sha2_256.digestsize != sizeof(digest)) goto done;
	hash_sha2_256.init(hashcontext.bytes);
	while (offset < s->size)
	{
		count = min(sizeof(buffer),s->size-offset);
#ifdef _WIN32
		if (!ReadFile(s->reader,buffer,(DWORD)count,&got,NULL) || got != count) goto done;
#else
		do {got = read(s->file,buffer,count);} while (got < 0 && errno == EINTR);
		if (got != count) goto done;
#endif
		if (!Held_StoreFetch(s,offset,expected,count) || memcmp(buffer,expected,count)) goto done;
		hash_sha2_256.process(hashcontext.bytes,buffer,count);
		offset += count;
	}
	hash_sha2_256.terminate(digest,hashcontext.bytes);
	valid = !memcmp(digest,s->digest,sizeof(digest));
 done:
	Held_StoreWipe(&hashcontext,sizeof(hashcontext)); Held_StoreWipe(buffer,sizeof(buffer));
	Held_StoreWipe(expected,sizeof(expected)); Held_StoreWipe(digest,sizeof(digest));
	return valid;
}
static int Held_StoreCommit(held_store_t *s)
{
#ifdef _WIN32
	typedef struct {BOOL replace; HANDLE root; DWORD bytes; WCHAR name[MAX_OSPATH];} rename_t;
	rename_t rename;
	DWORD clear = 8;
	size_t length = wcslen(s->target)*sizeof(WCHAR);
	if (!Held_StoreWinSame(s,s->file) || length >= sizeof(rename.name)) return 0;
	memset(&rename,0,sizeof(rename)); rename.bytes = (DWORD)length;
	memcpy(rename.name,s->target,length);
	if (!CloseHandle(s->reader)) return 0;
	s->reader = INVALID_HANDLE_VALUE;
	if (!s->setinfo(s->file,21,&clear,sizeof(clear))) return 0;
	s->commit_attempted = 1;
	if (!s->setinfo(s->file,3,&rename,(DWORD)(offsetof(rename_t,name)+length))) return 0;
#else
#if defined(__linux__) && defined(AT_SYMLINK_FOLLOW)
	char proc[64];
	if (fchmod(s->file,0644)) return 0; /* Still anonymous/nonserved until link. */
	Q_snprintfz(proc,sizeof(proc),"/proc/self/fd/%d",s->file);
	s->commit_attempted = 1;
	if (linkat(AT_FDCWD,proc,s->target_dir,s->leaf,AT_SYMLINK_FOLLOW)) return 0;
#else
	return 0;
#endif
#endif
	s->committed = 1;
	FS_FlushFSHashWritten(s->destination);
	return 1;
}
static int Held_StoreClose(held_store_t *s)
{
	int valid = 1;
#ifdef _WIN32
	if (s->reader != INVALID_HANDLE_VALUE && !CloseHandle(s->reader)) valid = 0;
	if (s->file != INVALID_HANDLE_VALUE)
	{
		if (!s->committed && !Held_StoreWinDelete(s,s->file))
		{
			LARGE_INTEGER zero;
			zero.QuadPart = 0;
			/* Retire/truncate ONLY the creator's exact object, never a name. */
			if (!SetFilePointerEx(s->file,zero,NULL,FILE_BEGIN) || !SetEndOfFile(s->file) || !FlushFileBuffers(s->file)) valid = 0;
			valid = 0;
		}
		if (!CloseHandle(s->file)) valid = 0;
	}
	while (s->numdirs) if (!CloseHandle(s->dirs[--s->numdirs])) valid = 0;
#else
	if (s->reader >= 0 && close(s->reader)) valid = 0;
	if (s->file >= 0 && close(s->file)) valid = 0; /* Unlinked O_TMPFILE erased on failure. */
	if (s->target_dir >= 0 && close(s->target_dir)) valid = 0;
	if (s->stage_dir >= 0 && close(s->stage_dir)) valid = 0;
#endif
	if (!valid)
	{
		held_store_fault = 1;
		Con_Printf(CON_ERROR "held staging cleanup ERROR (owned held-%s.tmp); collection disabled\n",s->generation);
	}
	return valid;
}
static int Held_StorePublish(const char *stage_root, const char *destination, const char *generation,
	const void *bytes, held_reader_t input, size_t size, const unsigned char digest[32],
	held_store_validator_t validate, void *context, held_store_t **retained)
{
	held_store_t *s;
	unsigned char actual[32];
	char root[MAX_OSPATH], parent[MAX_OSPATH], *leaf;
	int result = 0, clean;
	if (retained) *retained = NULL;
	if (held_store_fault || !stage_root || !*stage_root || strlen(stage_root) >= MAX_OSPATH || !size ||
		size > (size_t)INT64_MAX || (!bytes && !input) || (bytes && input) || !digest || !validate ||
		!Held_StoreArguments(destination,generation)) return 0;
	if (input) result = SV_HeldReadHash(input,context,size,actual);
	else result = size <= INT_MAX && CalcHash(&hash_sha2_256,actual,sizeof(actual),bytes,size) == 32;
	if (!result || memcmp(actual,digest,32) || !validate(context,bytes,size) ||
		!FS_SystemPath("",FS_GAMEONLY,root,sizeof(root)) || strlen(root)+strlen(destination)+2 >= sizeof(parent))
		{Held_StoreWipe(actual,sizeof(actual)); return 0;}
	result = 0;
	Q_snprintfz(parent,sizeof(parent),"%s%s%s",root,(*root && root[strlen(root)-1] != '/' && root[strlen(root)-1] != '\\') ? "/":"",destination);
	leaf = strrchr(parent,'/');
	if (!leaf || !Held_FileComponent(leaf+1,strlen(leaf+1))) return 0;
	s = Z_Malloc(sizeof(*s)); memset(s,0,sizeof(*s));
	s->bytes = bytes; s->input = input; s->input_context = context; s->size = size; memcpy(s->digest,digest,32);
	Q_strncpyz(s->generation,generation,sizeof(s->generation)); Q_strncpyz(s->destination,destination,sizeof(s->destination));
#ifdef _WIN32
	s->file = s->reader = INVALID_HANDLE_VALUE;
#else
	s->file = s->reader = s->stage_dir = s->target_dir = -1;
	Q_strncpyz(s->leaf,leaf+1,sizeof(s->leaf));
#endif
	*leaf = 0;
	if (Held_StoreOpen(s,stage_root,root,parent) && Held_StoreWrite(s) && Held_StoreReadback(s) &&
		validate(context,bytes,size) && FS_ValidateExternalDirectory(Held_StoreOutside,s) && Held_StoreReadback(s))
		result = Held_StoreCommit(s);
	Held_StoreWipe(actual,sizeof(actual));
	if (retained) {*retained = s; return result;} /* Native unit owns this exact object until cleanup. */
	clean = Held_StoreClose(s);
	Held_StoreWipe(s,sizeof(*s)); Z_Free(s);
	return result ? 1:(clean ? 0:-1); /* A real commit is never relabeled a failed swap. */
}
int SV_HeldStorePublish(const char *stage_root, const char *destination, const char *generation,
	const void *bytes, size_t size, const unsigned char digest[32], held_store_validator_t validate, void *context)
{
	return Held_StorePublish(stage_root,destination,generation,bytes,NULL,size,digest,validate,context,NULL);
}
int SV_HeldStorePublishReader(const char *stage_root, const char *destination, const char *generation,
	held_reader_t reader, size_t size, const unsigned char digest[32], held_store_validator_t validate, void *context)
{
	return Held_StorePublish(stage_root,destination,generation,NULL,reader,size,digest,validate,context,NULL);
}
static const char held_store_test_bytes[] = "native-store-dummy\n";
typedef struct {unsigned int calls, reject_at;} held_store_test_t;
static qboolean Held_StoreTestValidate(void *opaque, const void *bytes, size_t size)
{
	held_store_test_t *test = opaque;
	++test->calls;
	return test->calls != test->reject_at && size == sizeof(held_store_test_bytes)-1 && !memcmp(bytes,held_store_test_bytes,size);
}
typedef struct
{
	size_t size, reads, maximum;
	unsigned int starts, calls, fault, acted;
} held_store_read_test_t;
static int Held_StoreTestRead(void *opaque, size_t offset, void *buffer, size_t size)
{
	held_store_read_test_t *test = opaque;
	unsigned char *out = buffer;
	size_t i;
	++test->reads; if (size > test->maximum) test->maximum = size;
	if (!offset) ++test->starts;
	if (offset > test->size || size > test->size-offset) return 0;
	if (test->fault == 1 && offset >= HELD_IO_BUFFER) {++test->acted; return (int)size-1;}
	for (i = 0; i < size; ++i) out[i] = (unsigned char)((offset+i)%251);
	if (test->fault == 2 && test->starts == 2 && !offset && size) {out[0] ^= 1; ++test->acted;}
	return (int)size;
}
static qboolean Held_StoreTestReadValidate(void *opaque, const void *bytes, size_t size)
{
	held_store_read_test_t *test = opaque;
	++test->calls;
	return !bytes && size == test->size;
}
static int Held_StoreReadTest(const char *stage_root, unsigned int *checks)
{
	static const char generation[] = "2122232425262728292a2b2c2d2e2f30";
	static const char destination[] = "data/evidence/p615-held-store/reader.rec";
	held_store_read_test_t test;
	held_store_t *owned = NULL;
	vfsfile_t *file = NULL;
	unsigned char digest[32], actual[4096], expected[4096];
	size_t offset, count;
	int failures = 0, result, equal = 1;
#define STREAM_CHECK(x) do {++*checks; if (!(x)) {++failures; Con_Printf("held stream selftest failed line %u\n",(unsigned int)__LINE__);}} while (0)
	memset(&test,0,sizeof(test)); test.size = HELD_MAX_DETAIL+HELD_IO_BUFFER+17;
	STREAM_CHECK(SV_HeldReadHash(Held_StoreTestRead,&test,test.size,digest));
	test.starts = test.reads = test.calls = 0; test.fault = 1;
	STREAM_CHECK(!SV_HeldStorePublishReader(stage_root,destination,generation,Held_StoreTestRead,test.size,digest,Held_StoreTestReadValidate,&test) && test.acted && !test.calls);
	test.starts = test.reads = test.calls = test.acted = 0; test.fault = 2;
	STREAM_CHECK(!SV_HeldStorePublishReader(stage_root,destination,generation,Held_StoreTestRead,test.size,digest,Held_StoreTestReadValidate,&test) && test.acted == 1 && test.calls == 1 && test.starts >= 3 && SV_HeldStoreHealthy());
	file = FS_OpenVFS(destination,"rb",FS_GAMEONLY);
	STREAM_CHECK(!file); if (file) {VFS_CLOSE(file); file = NULL;}
	test.starts = test.reads = test.calls = test.acted = 0; test.fault = 0;
	result = Held_StorePublish(stage_root,destination,generation,NULL,Held_StoreTestRead,test.size,digest,Held_StoreTestReadValidate,&test,&owned);
	STREAM_CHECK(result == 1 && owned && owned->committed && owned->commit_attempted && test.calls == 2 && test.maximum <= 4096);
	if (!owned || !owned->committed) goto done;
	file = FS_OpenVFS(destination,"rb",FS_GAMEONLY);
	STREAM_CHECK(file != NULL);
	if (file)
	{
		equal = VFS_GETLEN(file) == test.size;
		for (offset = 0; equal && offset < test.size; offset += count)
		{
			count = min(sizeof(actual),test.size-offset);
			equal = VFS_READ(file,actual,count) == count && Held_StoreTestRead(&test,offset,expected,count) == count && !memcmp(actual,expected,count);
		}
		STREAM_CHECK(equal && VFS_READ(file,actual,1) == 0);
		STREAM_CHECK(VFS_CLOSE(file)); file = NULL;
	}
done:
	if (file) VFS_CLOSE(file);
	if (owned)
	{
		if (owned->committed)
		{
#ifdef _WIN32
			STREAM_CHECK(Held_StoreWinSame(owned,owned->file) && Held_StoreWinDelete(owned,owned->file));
#else
			struct stat current;
			STREAM_CHECK(!fstatat(owned->target_dir,owned->leaf,&current,AT_SYMLINK_NOFOLLOW) &&
				current.st_dev == owned->identity.st_dev && current.st_ino == owned->identity.st_ino &&
				!unlinkat(owned->target_dir,owned->leaf,0));
#endif
			FS_FlushFSHashRemoved(destination);
		}
		STREAM_CHECK(Held_StoreClose(owned)); Held_StoreWipe(owned,sizeof(*owned)); Z_Free(owned);
	}
	STREAM_CHECK(SV_HeldStoreHealthy());
	Held_StoreWipe(&test,sizeof(test)); Held_StoreWipe(digest,sizeof(digest));
	Held_StoreWipe(actual,sizeof(actual)); Held_StoreWipe(expected,sizeof(expected));
#undef STREAM_CHECK
	return failures;
}
int SV_HeldStoreTest(const char *stage_root, unsigned int *checks)
{
	static const char destination[] = "data/runs/p615_held_store/test.rec";
	static const char rejected[] = "data/runs/p615_held_store/rejected.rec";
	static const char generation[] = "0102030405060708090a0b0c0d0e0f10";
	static const char collision[] = "0102030405060708090a0b0c0d0e0f11";
	held_store_t *owned = NULL, *blocked = NULL;
	held_store_test_t test;
	unsigned char digest[32], bad[32];
	char game[MAX_OSPATH], buffer[64];
	vfsfile_t *f;
	int failures = 0, result;
	*checks = 0;
#define STORE_CHECK(x) do {++*checks; if (!(x)) {++failures; Con_Printf("held sink selftest failed line %u\n",(unsigned int)__LINE__);}} while (0)
	STORE_CHECK(CalcHash(&hash_sha2_256,digest,32,(const unsigned char*)held_store_test_bytes,sizeof(held_store_test_bytes)-1) == 32);
	memcpy(bad,digest,32); bad[0] ^= 1;
	memset(&test,0,sizeof(test));
	STORE_CHECK(!SV_HeldStorePublish(stage_root,destination,generation,held_store_test_bytes,sizeof(held_store_test_bytes)-1,bad,Held_StoreTestValidate,&test) && !test.calls);
	STORE_CHECK(!SV_HeldStorePublish(stage_root,"data/runs/../outside.rec",generation,held_store_test_bytes,sizeof(held_store_test_bytes)-1,digest,Held_StoreTestValidate,&test) && !test.calls);
	test.reject_at = 1;
	STORE_CHECK(!SV_HeldStorePublish(stage_root,destination,generation,held_store_test_bytes,sizeof(held_store_test_bytes)-1,digest,Held_StoreTestValidate,&test) && test.calls == 1);
	memset(&test,0,sizeof(test));
	STORE_CHECK(FS_SystemPath("",FS_GAMEONLY,game,sizeof(game)) &&
		!SV_HeldStorePublish(game,destination,generation,held_store_test_bytes,sizeof(held_store_test_bytes)-1,digest,Held_StoreTestValidate,&test) && test.calls == 1);
	memset(&test,0,sizeof(test)); test.reject_at = 2;
	STORE_CHECK(!SV_HeldStorePublish(stage_root,rejected,generation,held_store_test_bytes,sizeof(held_store_test_bytes)-1,digest,Held_StoreTestValidate,&test) && test.calls == 2 && SV_HeldStoreHealthy());
	f = FS_OpenVFS(rejected,"rb",FS_GAMEONLY);
	STORE_CHECK(!f); if (f) VFS_CLOSE(f);
	memset(&test,0,sizeof(test));
	result = Held_StorePublish(stage_root,destination,generation,held_store_test_bytes,NULL,sizeof(held_store_test_bytes)-1,digest,Held_StoreTestValidate,&test,&owned);
	STORE_CHECK(result == 1 && owned && owned->committed && owned->commit_attempted && test.calls == 2);
	if (!owned || !owned->committed) goto done; /* Never remove a preexisting destination. */
	f = FS_OpenVFS(destination,"rb",FS_GAMEONLY);
	STORE_CHECK(f != NULL);
	if (f)
	{
		STORE_CHECK(VFS_READ(f,buffer,sizeof(buffer)) == sizeof(held_store_test_bytes)-1 && !memcmp(buffer,held_store_test_bytes,sizeof(held_store_test_bytes)-1));
		STORE_CHECK(VFS_CLOSE(f));
	}
	f = FS_OpenVFS("held-0102030405060708090a0b0c0d0e0f10.tmp","rb",FS_GAME);
	STORE_CHECK(!f); if (f) VFS_CLOSE(f);
	memset(&test,0,sizeof(test));
	result = Held_StorePublish(stage_root,destination,collision,held_store_test_bytes,NULL,sizeof(held_store_test_bytes)-1,digest,Held_StoreTestValidate,&test,&blocked);
	STORE_CHECK(!result && blocked && blocked->commit_attempted && !blocked->committed && test.calls == 2);
	f = FS_OpenVFS(destination,"rb",FS_GAMEONLY);
	STORE_CHECK(f != NULL);
	if (f)
	{
		STORE_CHECK(VFS_READ(f,buffer,sizeof(buffer)) == sizeof(held_store_test_bytes)-1 && !memcmp(buffer,held_store_test_bytes,sizeof(held_store_test_bytes)-1));
		STORE_CHECK(VFS_CLOSE(f));
	}
 done:
	if (blocked)
	{
		STORE_CHECK(Held_StoreClose(blocked)); Held_StoreWipe(blocked,sizeof(*blocked)); Z_Free(blocked);
	}
	if (owned)
	{
		if (owned->committed)
		{
#ifdef _WIN32
			/* Delete the SAME committed dummy object, not a pathname/header claim. */
			STORE_CHECK(Held_StoreWinSame(owned,owned->file) && Held_StoreWinDelete(owned,owned->file));
#else
			struct stat current;
			STORE_CHECK(!fstatat(owned->target_dir,owned->leaf,&current,AT_SYMLINK_NOFOLLOW) &&
				current.st_dev == owned->identity.st_dev && current.st_ino == owned->identity.st_ino &&
				!unlinkat(owned->target_dir,owned->leaf,0));
#endif
			FS_FlushFSHashRemoved(destination);
		}
		STORE_CHECK(Held_StoreClose(owned)); Held_StoreWipe(owned,sizeof(*owned)); Z_Free(owned);
	}
	STORE_CHECK(SV_HeldStoreHealthy());
	Held_StoreWipe(digest,sizeof(digest)); Held_StoreWipe(bad,sizeof(bad)); Held_StoreWipe(buffer,sizeof(buffer));
	if (SV_HeldStoreHealthy()) failures += Held_StoreReadTest(stage_root,checks);
	if (SV_HeldStoreHealthy()) failures += Held_FileRecoveryTest(stage_root,checks);
	if (SV_HeldStoreHealthy()) failures += Held_FileRetentionTest(stage_root,checks);
#undef STORE_CHECK
	return failures;
}
