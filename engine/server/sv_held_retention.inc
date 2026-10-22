/* Existing evidence age/scope only. Recovered ORIGINALS keep their original
 * runid date; recovery/mtime must never restart the retention period. */
static int Held_RecoveryDay(int y, int m, int d)
{
	int era, yoe, mp, doy;
	static const int monthdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
	if (y < 1970 || y > 9999 || m < 1 || m > 12 || d < 1 ||
		d > monthdays[m-1] + (m == 2 && !(y%4) && ((y%100) || !(y%400)))) return -1;
	if (m <= 2) --y;
	era = y/400; yoe = y-era*400; mp = m > 2 ? m-3 : m+9;
	doy = (153*mp+2)/5+d-1;
	return era*146097+yoe*365+yoe/4-yoe/100+doy-719468;
}
static int Held_RecoveryLeaf(const char *name)
{
	unsigned int i;
	if (strlen(name) != 48 || strncmp(name,"held-",5) || strcmp(name+37,".failed.rec")) return 0;
	for (i = 5; i < 37; ++i)
		if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return 0;
	return 1;
}
static int Held_RecoveryExpired(char *header, const char *map, int today, float days)
{
	char *line = header, *next, recorded_map[MAX_QPATH] = "";
	int day = -1, seenmap = 0, seenid = 0, i, y, m, d;
	next = strchr(line,'\n');
	if (!next) return 0;
	*next++ = 0;
	/* A source, not a native publication or arbitrary private file. */
	if (strcmp(line,"FTESURF-REC 12") && strcmp(line,"FTESURF-REC 14")) return 0;
	for (line = next; (next = strchr(line,'\n')) != NULL; line = next)
	{
		*next++ = 0;
		if (!strcmp(line,"begin"))
			return seenmap == 1 && seenid == 1 && day >= 0 &&
				(!*map || !strcmp(map,recorded_map)) && today-day > days;
		if (!strncmp(line,"map ",4))
		{
			if (++seenmap != 1 || !line[4] || strlen(line+4) >= sizeof(recorded_map)) return 0;
			Q_strncpyz(recorded_map,line+4,sizeof(recorded_map));
		}
		else if (!strncmp(line,"runid ",6))
		{
			if (++seenid != 1 || strlen(line+6) < 8) return 0;
			for (i = 6; i < 14; ++i) if (line[i] < '0' || line[i] > '9') return 0;
			y = (line[6]-'0')*1000+(line[7]-'0')*100+(line[8]-'0')*10+line[9]-'0';
			m = (line[10]-'0')*10+line[11]-'0'; d = (line[12]-'0')*10+line[13]-'0';
			day = Held_RecoveryDay(y,m,d);
		}
	}
	return 0; /* Unknown/truncated header: preserve, never infer age from mtime. */
}
static int Held_RecoveryToday(time_t now)
{
	struct tm calendar, *local;
	if (now < 0 || !(local = localtime(&now))) return -1;
	calendar = *local;
	/* Match PF_strftime(TRUE), runids and the existing QC evidence policy. */
	return Held_RecoveryDay(calendar.tm_year+1900,calendar.tm_mon+1,calendar.tm_mday);
}
#ifdef _WIN32
typedef HANDLE held_recovery_handle_t;
#else
typedef int held_recovery_handle_t;
#endif
typedef int (*held_recovery_read_t)(held_recovery_handle_t, char *, int);
static int Held_RecoveryRead(held_recovery_handle_t file, char *header, int bytes)
{
#ifdef _WIN32
	DWORD got;
	return ReadFile(file,header,bytes,&got,NULL) ? (int)got : -1;
#else
	return pread(file,header,bytes,0);
#endif
}
#ifdef _WIN32
static int Held_RecoveryPrivate(HANDLE file)
{
	typedef DWORD (WINAPI *security_t)(HANDLE,int,DWORD,PSID*,PSID*,PACL*,PACL*,PSECURITY_DESCRIPTOR*);
	typedef BOOL (WINAPI *control_t)(PSECURITY_DESCRIPTOR,PSECURITY_DESCRIPTOR_CONTROL,LPDWORD);
	typedef BOOL (WINAPI *sid_t)(LPCWSTR,PSID*);
	typedef BOOL (WINAPI *equal_t)(PSID,PSID);
	typedef BOOL (WINAPI *token_t)(HANDLE,DWORD,PHANDLE);
	typedef BOOL (WINAPI *info_t)(HANDLE,TOKEN_INFORMATION_CLASS,LPVOID,DWORD,PDWORD);
	typedef BOOL (WINAPI *ace_t)(PACL,DWORD,LPVOID*);
	HMODULE library = LoadLibraryExW(L"advapi32.dll",NULL,0x00000800);
	security_t security; control_t control; sid_t sid; equal_t equal;
	token_t token; info_t info; ace_t ace;
	PSECURITY_DESCRIPTOR descriptor = NULL;
	PSID owner = NULL, ow = NULL, system = NULL;
	PACL acl = NULL;
	SECURITY_DESCRIPTOR_CONTROL flags;
	HANDLE process_token = NULL;
	union {TOKEN_USER user; unsigned char bytes[sizeof(TOKEN_USER)+SECURITY_MAX_SID_SIZE];} user;
	DWORD bytes, revision, i;
	int result = 0, sawowner = 0, sawsystem = 0;
	if (!library) return 0;
	security = (void*)GetProcAddress(library,"GetSecurityInfo");
	control = (void*)GetProcAddress(library,"GetSecurityDescriptorControl");
	sid = (void*)GetProcAddress(library,"ConvertStringSidToSidW");
	equal = (void*)GetProcAddress(library,"EqualSid");
	token = (void*)GetProcAddress(library,"OpenProcessToken");
	info = (void*)GetProcAddress(library,"GetTokenInformation");
	ace = (void*)GetProcAddress(library,"GetAce");
	if (!security || !control || !sid || !equal || !token || !info || !ace ||
		security(file,1,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,NULL,&acl,NULL,&descriptor) ||
		!descriptor || !owner || !acl || acl->AceCount != 2 ||
		!control(descriptor,&flags,&revision) || !(flags & SE_DACL_PROTECTED) ||
		!token(GetCurrentProcess(),TOKEN_QUERY,&process_token) ||
		!info(process_token,TokenUser,&user,sizeof(user),&bytes) || !equal(owner,user.user.User.Sid) ||
		!sid(L"S-1-3-4",&ow) || !sid(L"S-1-5-18",&system)) goto done;
	for (i = 0; i < acl->AceCount; ++i)
	{
		ACCESS_ALLOWED_ACE *entry;
		if (!ace(acl,i,(void**)&entry) || entry->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
			entry->Header.AceFlags || entry->Mask != FILE_ALL_ACCESS) goto done;
		if (equal(&entry->SidStart,ow)) ++sawowner;
		else if (equal(&entry->SidStart,system)) ++sawsystem;
		else goto done;
	}
	result = sawowner == 1 && sawsystem == 1;
done:
	if (process_token) CloseHandle(process_token);
	if (ow) LocalFree(ow);
	if (system) LocalFree(system);
	if (descriptor) LocalFree(descriptor);
	FreeLibrary(library);
	return result;
}
#endif
static int Held_RecoverySweepAt(const char *root, const char *map, int today, float days,
	held_recovery_read_t readheader)
{
	held_store_t s;
	char header[8192]; /* Bounded ordinary header only, never whole recordings. */
	int removed = 0;
	unsigned int daybits;
	memcpy(&daybits,&days,sizeof(daybits));
	if ((daybits & 0x7f800000u) == 0x7f800000u || days <= 0) return 0;
	if (!root || !*root || !map || today < 0) return -1;
	memset(&s,0,sizeof(s));
#ifdef _WIN32
	{
		HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
		WIN32_FIND_DATAW entry;
		HANDLE search = INVALID_HANDLE_VALUE;
		wchar_t path[MAX_OSPATH];
		s.setinfo = kernel ? (void*)GetProcAddress(kernel,"SetFileInformationByHandle") : NULL;
		s.finalpath = kernel ? (void*)GetProcAddress(kernel,"GetFinalPathNameByHandleW") : NULL;
		if (!s.setinfo || !s.finalpath || !Held_StoreWinDirs(&s,root,0) ||
			!Held_StoreWinCanonical(&s,s.dirs[s.numdirs-1],s.canonical) ||
			!FS_ValidateExternalDirectory(Held_StoreOutside,&s) || wcslen(s.canonical)+64 >= MAX_OSPATH)
		{removed = -1; goto win_done;}
		wcscpy(path,s.canonical); wcscat(path,L"\\held-*.failed.rec");
		search = FindFirstFileW(path,&entry);
		if (search == INVALID_HANDLE_VALUE)
		{
			if (GetLastError() != ERROR_FILE_NOT_FOUND) removed = -1;
			goto win_done;
		}
		do
		{
			HANDLE file;
			BY_HANDLE_FILE_INFORMATION before, after;
			char leaf[64];
			int got;
			size_t i, length = wcslen(entry.cFileName);
			if (length >= sizeof(leaf)) continue;
			for (i = 0; i < length; ++i) leaf[i] = entry.cFileName[i] < 128 ? entry.cFileName[i] : '?';
			leaf[length] = 0;
			if (!Held_RecoveryLeaf(leaf)) continue;
			wcscpy(path,s.canonical); wcscat(path,L"\\"); wcscat(path,entry.cFileName);
			file = CreateFileW(path,GENERIC_READ|DELETE|READ_CONTROL,FILE_SHARE_READ,NULL,
				OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
			if (file == INVALID_HANDLE_VALUE) continue; /* Busy/foreign stays intact. */
			if (Held_StoreWinRegular(file,&before) && Held_RecoveryPrivate(file))
			{
				got = readheader(file,header,sizeof(header)-1);
				if (got < 0) removed = -1;
				else
				{
					header[got] = 0;
					if (Held_RecoveryExpired(header,map,today,days) && Held_StoreWinRegular(file,&after) &&
						before.dwVolumeSerialNumber == after.dwVolumeSerialNumber &&
						before.nFileIndexHigh == after.nFileIndexHigh && before.nFileIndexLow == after.nFileIndexLow &&
						before.nFileSizeHigh == after.nFileSizeHigh && before.nFileSizeLow == after.nFileSizeLow &&
						!CompareFileTime(&before.ftLastWriteTime,&after.ftLastWriteTime) &&
						FS_ValidateExternalDirectory(Held_StoreOutside,&s))
					{
						if (Held_StoreWinDelete(&s,file)) ++removed;
						else removed = -1;
					}
				}
			}
			if (!CloseHandle(file)) removed = -1;
			if (removed < 0) break;
		} while (FindNextFileW(search,&entry));
		if (removed >= 0 && GetLastError() != ERROR_NO_MORE_FILES) removed = -1;
win_done:
		if (search != INVALID_HANDLE_VALUE && !FindClose(search)) removed = -1;
		while (s.numdirs) if (!CloseHandle(s.dirs[--s.numdirs])) removed = -1;
	}
#elif defined(__linux__)
	{
		struct stat directory, resolved;
		DIR *entries = NULL;
		struct dirent *entry;
		s.stage_dir = Held_StorePosixRoot(root);
		if (s.stage_dir < 0 || fstat(s.stage_dir,&directory) || directory.st_uid != geteuid() ||
			(directory.st_mode & 0077) || !Held_StoreResolved(root,s.canonical,&resolved) ||
			directory.st_dev != resolved.st_dev || directory.st_ino != resolved.st_ino ||
			!FS_ValidateExternalDirectory(Held_StoreOutside,&s)) {removed = -1; goto posix_done;}
		{
			int copy = dup(s.stage_dir);
			if (copy < 0) {removed = -1; goto posix_done;}
			entries = fdopendir(copy);
			if (!entries) {close(copy); removed = -1; goto posix_done;}
		}
		errno = 0;
		while ((entry = readdir(entries)) != NULL)
		{
			int file;
			int got;
			struct stat before, after, named;
			if (!Held_RecoveryLeaf(entry->d_name)) {errno = 0; continue;}
			file = openat(s.stage_dir,entry->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
			if (file < 0) {errno = 0; continue;}
			if (!fstat(file,&before) && S_ISREG(before.st_mode) && before.st_nlink == 1 &&
				before.st_uid == geteuid() && !(before.st_mode & 0077))
			{
				got = readheader(file,header,sizeof(header)-1);
				if (got < 0) removed = -1;
				else
				{
					header[got] = 0;
					if (Held_RecoveryExpired(header,map,today,days) && !fstat(file,&after) &&
						!fstatat(s.stage_dir,entry->d_name,&named,AT_SYMLINK_NOFOLLOW) &&
						S_ISREG(named.st_mode) && after.st_nlink == 1 && named.st_nlink == 1 &&
						before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
						after.st_dev == named.st_dev && after.st_ino == named.st_ino &&
						before.st_size == after.st_size && after.st_size == named.st_size &&
						before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
						FS_ValidateExternalDirectory(Held_StoreOutside,&s))
					{
						if (!unlinkat(s.stage_dir,entry->d_name,0)) ++removed;
						else removed = -1;
					}
				}
			}
			if (close(file)) removed = -1;
			if (removed < 0) break;
			errno = 0;
		}
		if (errno) removed = -1;
posix_done:
		if (entries && closedir(entries)) removed = -1;
		if (s.stage_dir >= 0 && close(s.stage_dir)) removed = -1;
	}
#else
	removed = -1;
#endif
	Held_StoreWipe(header,sizeof(header));
	Held_StoreWipe(&s,sizeof(s));
	return removed;
}
static int Held_RecoverySweep(const char *root, const char *map, held_recovery_read_t readheader)
{
	cvar_t *days = Cvar_FindVar("run_evidence_days"), *all = Cvar_FindVar("run_evidence_sweepall");
	int result;
	if (!days || days->value <= 0 || !root || !*root) return 0;
	if (!map || (!*map && (!all || !all->value))) return 0;
	result = Held_RecoverySweepAt(root,map,Held_RecoveryToday(time(NULL)),days->value,readheader);
	if (result < 0) held_store_fault = 1; /* No new collection without expiry access. */
	return result;
}
int SV_HeldFileSweep(const char *root, const char *map)
{
	return Held_RecoverySweep(root,map,Held_RecoveryRead);
}

/* Isolated create-new dummy root. Never expire any preexisting test/player tree. */
static int Held_RetentionRead(const char *path, char *data, int bytes)
{
#ifdef _WIN32
	wchar_t wide[MAX_OSPATH];
	HANDLE file;
	DWORD got = 0;
	int valid;
	widen(wide,sizeof(wide),path);
	file = CreateFileW(wide,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,
		OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
	if (file == INVALID_HANDLE_VALUE) return -1;
	valid = ReadFile(file,data,bytes,&got,NULL);
	if (!CloseHandle(file)) valid = 0;
	return valid ? (int)got : -1;
#else
	int file = open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC), got;
	if (file < 0) return -1;
	got = pread(file,data,bytes,0);
	if (close(file)) got = -1;
	return got;
#endif
}
static int Held_RetentionForeign(const char *path)
{
#ifdef _WIN32
	typedef BOOL (WINAPI *convert_t)(LPCWSTR,DWORD,PSECURITY_DESCRIPTOR*,PULONG);
	typedef BOOL (WINAPI *set_t)(LPCWSTR,SECURITY_INFORMATION,PSECURITY_DESCRIPTOR);
	HMODULE library = LoadLibraryExW(L"advapi32.dll",NULL,0x00000800);
	PSECURITY_DESCRIPTOR descriptor = NULL;
	convert_t convert;
	set_t set;
	wchar_t wide[MAX_OSPATH];
	int valid = 0;
	if (!library) return 0;
	convert = (void*)GetProcAddress(library,"ConvertStringSecurityDescriptorToSecurityDescriptorW");
	set = (void*)GetProcAddress(library,"SetFileSecurityW");
	widen(wide,sizeof(wide),path);
	if (convert && set && convert(L"D:P(A;;FA;;;OW)(A;;FA;;;SY)(A;;GR;;;WD)",1,&descriptor,NULL))
		valid = set(wide,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,descriptor);
	if (descriptor) LocalFree(descriptor);
	FreeLibrary(library);
	return valid;
#else
	return !chmod(path,0644);
#endif
}
static unsigned int held_retention_failed_reads;
static int Held_RetentionReadFail(held_recovery_handle_t file, char *header, int bytes)
{
	(void)file; (void)header; (void)bytes;
	++held_retention_failed_reads;
	return -1;
}
static int Held_RetentionZone(const char *zone)
{
#ifdef _WIN32
	if (_putenv_s("TZ",zone ? zone : "")) return 0;
	_tzset();
#else
	if (zone ? setenv("TZ",zone,1) : unsetenv("TZ")) return 0;
	tzset();
#endif
	return 1;
}
static int Held_FileRetentionTest(const char *stage_root, unsigned int *checks)
{
	char root[MAX_OSPATH], paths[6][MAX_OSPATH], headers[6][256], alias[MAX_OSPATH];
	char generation[33], relative[MAX_QPATH], observed[256];
	unsigned char random[16];
	cvar_t *days = Cvar_Get("run_evidence_days","0",0,"FTESurf");
	cvar_t *all = Cvar_Get("run_evidence_sweepall","0",0,"FTESurf");
	char *prior_days = Z_StrDup(days->string), *prior_all = Z_StrDup(all->string);
	unsigned int i, j, created = 0, failures = 0;
	unsigned int prior_fault = held_store_fault;
	int lengths[6];
#define RETAIN_CHECK(x) do {++*checks; if (!(x)) ++failures;} while (0)
	RETAIN_CHECK(Held_RecoveryDay(1970,1,1) == 0 && Held_RecoveryDay(2000,2,29) == 11016);
	RETAIN_CHECK(Held_RecoveryDay(2100,2,29) == -1 && Held_RecoveryDay(2026,13,1) == -1);
	{
		const char *zone = getenv("TZ");
		char *prior_zone = zone ? Z_StrDup(zone) : NULL;
		int today;
		RETAIN_CHECK(Held_RetentionZone("EST5"));
		today = Held_RecoveryToday((time_t)1792720800); /* 2026-10-23 02:00 UTC. */
		RETAIN_CHECK(today == Held_RecoveryDay(2026,10,22));
		strcpy(observed,"FTESURF-REC 14\nmap retention_a\nrunid 20261021-dummy\nbegin\n");
		RETAIN_CHECK(!Held_RecoveryExpired(observed,"retention_a",today,1));
		RETAIN_CHECK(Held_RetentionZone("JST-9"));
		today = Held_RecoveryToday((time_t)1792692000); /* 2026-10-22 18:00 UTC. */
		RETAIN_CHECK(today == Held_RecoveryDay(2026,10,23));
		strcpy(observed,"FTESURF-REC 14\nmap retention_a\nrunid 20261021-dummy\nbegin\n");
		RETAIN_CHECK(Held_RecoveryExpired(observed,"retention_a",today,1));
		RETAIN_CHECK(Held_RetentionZone(prior_zone));
		if (prior_zone) Z_Free(prior_zone);
	}
	if (!Sys_RandomBytes(random,sizeof(random))) {RETAIN_CHECK(0); goto done;}
	for (j = 0; j < sizeof(random); ++j) Q_snprintfz(generation+j*2,3,"%02x",random[j]);
	if (strlen(stage_root)+strlen(generation)+12 >= sizeof(root)) {RETAIN_CHECK(0); goto done;}
	Q_snprintfz(root,sizeof(root),"%s/retention-%s",stage_root,generation);
#ifdef _WIN32
	{
		wchar_t wide[MAX_OSPATH]; widen(wide,sizeof(wide),root);
		if (!CreateDirectoryW(wide,NULL)) {RETAIN_CHECK(0); goto done;}
	}
#else
	if (mkdir(root,0700)) {RETAIN_CHECK(0); goto done;}
#endif
	for (i = 0; i < 6; ++i)
	{
		held_file_ticket_t *ticket = NULL;
		held_source_owner_t owner;
		vfsfile_t *stream;
		if (!Sys_RandomBytes(random,sizeof(random))) {RETAIN_CHECK(0); break;}
		for (j = 0; j < sizeof(random); ++j) Q_snprintfz(generation+j*2,3,"%02x",random[j]);
		Q_snprintfz(relative,sizeof(relative),"data/parts/hr-%s.rec",generation);
		Q_snprintfz(paths[i],sizeof(paths[i]),"%s/held-%s.failed.rec",root,generation);
		Q_snprintfz(headers[i],sizeof(headers[i]),"FTESURF-REC 14\nmap retention_%s\nrunid %s-dummy\nbegin\n",
			i == 2 ? "b":"a",i == 1 ? "99991231":(i == 3 ? "unknown!":"20000101"));
		lengths[i] = strlen(headers[i]);
		stream = SV_HeldFileCreate(relative,&ticket);
		RETAIN_CHECK(stream && ticket);
		if (!stream) break;
		memset(&owner,0,sizeof(owner)); owner.vm = &failures; owner.client_uid = 17;
		owner.entity_index = 1; owner.serial = i+1; Q_strncpyz(owner.record,"retention-dummy",sizeof(owner.record));
		RETAIN_CHECK(SV_HeldFileSetOwner(ticket,&owner));
		RETAIN_CHECK(VFS_WRITE(stream,headers[i],lengths[i]) == lengths[i]);
		RETAIN_CHECK(VFS_CLOSE(stream));
		RETAIN_CHECK(SV_HeldFileRecover(ticket,root,generation));
		RETAIN_CHECK(SV_HeldFileRetire(ticket,false));
		++created;
	}
	if (created != 6) goto done;
	RETAIN_CHECK(Held_RetentionForeign(paths[4]));
	Q_snprintfz(alias,sizeof(alias),"%s/foreign-link.txt",root);
#ifdef _WIN32
	{
		wchar_t target[MAX_OSPATH], linkname[MAX_OSPATH];
		widen(target,sizeof(target),paths[5]); widen(linkname,sizeof(linkname),alias);
		RETAIN_CHECK(CreateHardLinkW(linkname,target,NULL));
	}
#else
	RETAIN_CHECK(!link(paths[5],alias));
#endif
	Cvar_ForceSet(days,"0"); Cvar_ForceSet(all,"1");
	RETAIN_CHECK(SV_HeldFileSweep(root,"") == 0);
	for (i = 0; i < 6; ++i)
		RETAIN_CHECK(Held_RetentionRead(paths[i],observed,sizeof(observed)) == lengths[i] && !memcmp(observed,headers[i],lengths[i]));
	Cvar_ForceSet(days,"1"); Cvar_ForceSet(all,"0");
	/* Old ORIGINAL date, fresh recovery/mtime; current-map only. */
	RETAIN_CHECK(SV_HeldFileSweep(root,"retention_a") == 1);
	RETAIN_CHECK(Held_RetentionRead(paths[0],observed,sizeof(observed)) == -1);
	RETAIN_CHECK(SV_HeldFileSweep(root,"") == 0);
	RETAIN_CHECK(Held_RetentionRead(paths[2],observed,sizeof(observed)) == lengths[2] && !memcmp(observed,headers[2],lengths[2]));
	Cvar_ForceSet(all,"1");
	/* Re-discovery, no ticket/in-memory roster remains: shared opt-in. */
	RETAIN_CHECK(SV_HeldFileSweep(root,"") == 1);
	RETAIN_CHECK(Held_RetentionRead(paths[2],observed,sizeof(observed)) == -1);
	for (i = 1; i < 6; ++i) if (i != 2)
		RETAIN_CHECK(Held_RetentionRead(paths[i],observed,sizeof(observed)) == lengths[i] && !memcmp(observed,headers[i],lengths[i]));
	RETAIN_CHECK(Held_RetentionRead(alias,observed,sizeof(observed)) == lengths[5] && !memcmp(observed,headers[5],lengths[5]));
	Q_snprintfz(alias,sizeof(alias),"%s/absent-policy-root",root);
	Cvar_ForceSet(days,"0");
	RETAIN_CHECK(SV_HeldFileSweep(alias,"retention_a") == 0 && SV_HeldStoreHealthy());
	Cvar_ForceSet(days,"1");
	RETAIN_CHECK(SV_HeldFileSweep(alias,"retention_a") == -1 && !SV_HeldStoreHealthy());
	if (!failures) held_store_fault = prior_fault; /* Verified expected root error only. */
	held_retention_failed_reads = 0;
	RETAIN_CHECK(SV_HeldStoreHealthy());
	RETAIN_CHECK(Held_RecoverySweep(root,"retention_a",Held_RetentionReadFail) == -1 &&
		held_retention_failed_reads == 1 && !SV_HeldStoreHealthy());
	RETAIN_CHECK(SV_HeldFileSweep(root,"retention_a") == 0 && !SV_HeldStoreHealthy());
	for (i = 1; i < 6; ++i) if (i != 2)
		RETAIN_CHECK(Held_RetentionRead(paths[i],observed,sizeof(observed)) == lengths[i] && !memcmp(observed,headers[i],lengths[i]));
	if (!failures) held_store_fault = prior_fault; /* All read-error preservation checks passed. */
	Con_Printf("held recovery retention: local calendar boundaries and owned header-read error acted; synthetic expiry/foreign/link preservation (owned dummy root)\n");
done:
	Cvar_ForceSet(days,prior_days); Cvar_ForceSet(all,prior_all);
	Z_Free(prior_days); Z_Free(prior_all);
#undef RETAIN_CHECK
	return failures;
}
