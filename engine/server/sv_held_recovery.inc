/* Failed ordinary ORIGINAL, never a reconstructed copy or a publication sink. */
static qboolean Held_FileRecoverClosed(held_file_ticket_t *t)
{
	held_store_t s;
	char leaf[64];
	int valid = 0;
	memset(&s,0,sizeof(s));
	Q_snprintfz(leaf,sizeof(leaf),"held-%s.failed.rec",t->recovery_generation);
#ifdef _WIN32
	{
		typedef struct {BOOL replace; HANDLE root; DWORD bytes; WCHAR name[MAX_OSPATH];} rename_t;
		rename_t rename;
		HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
		BY_HANDLE_FILE_INFORMATION source, directory;
		wchar_t name[64], expected[MAX_OSPATH], actual[MAX_OSPATH];
		DWORD observed;
		size_t length;
		s.setinfo = kernel ? (void*)GetProcAddress(kernel,"SetFileInformationByHandle") : NULL;
		s.finalpath = kernel ? (void*)GetProcAddress(kernel,"GetFinalPathNameByHandleW") : NULL;
		if (!s.setinfo || !s.finalpath || !Held_StoreWinDirs(&s,t->recovery_root,0) ||
			!Held_StoreWinCanonical(&s,s.dirs[s.numdirs-1],s.canonical) ||
			!FS_ValidateExternalDirectory(Held_StoreOutside,&s) ||
			!GetFileInformationByHandle(s.dirs[s.numdirs-1],&directory) ||
			!Held_StoreWinRegular(t->file,&source) ||
			source.dwVolumeSerialNumber != directory.dwVolumeSerialNumber ||
			source.dwVolumeSerialNumber != t->identity.dwVolumeSerialNumber ||
			source.nFileIndexHigh != t->identity.nFileIndexHigh ||
			source.nFileIndexLow != t->identity.nFileIndexLow) goto win_done;
		/* The creator source already has a protected owner/system-only DACL.
		 * Rename the handle itself; no path reopen, replacement or copy/delete. */
		widen(name,sizeof(name),leaf);
		if (wcslen(s.canonical)+1+wcslen(name) >= MAX_OSPATH) goto win_done;
		wcscpy(expected,s.canonical); wcscat(expected,L"\\"); wcscat(expected,name);
		length = wcslen(expected)*sizeof(WCHAR);
		/* Win32 FileRenameInfo uses an absolute name and a NULL root, unlike
		 * NtSetInformationFile's relative-root form. Parent handles stay locked. */
		memset(&rename,0,sizeof(rename)); rename.bytes = (DWORD)length;
		memcpy(rename.name,expected,length);
		if (!s.setinfo(t->file,3,&rename,(DWORD)(offsetof(rename_t,name)+length))) goto win_done;
		observed = s.finalpath(t->file,actual,MAX_OSPATH,FILE_NAME_NORMALIZED|VOLUME_NAME_GUID);
		valid = observed && observed < MAX_OSPATH && !_wcsicmp(expected,actual);
win_done:
		if (!valid) Con_Printf(CON_ERROR "held source recovery Windows refusal %lu\n",(unsigned long)GetLastError());
		while (s.numdirs) if (!CloseHandle(s.dirs[--s.numdirs])) held_store_fault = 1;
	}
#else
#if defined(__linux__) && defined(SYS_renameat2)
	{
		struct stat directory, resolved, source, named;
		s.stage_dir = Held_StorePosixRoot(t->recovery_root);
		if (s.stage_dir < 0 || fstat(s.stage_dir,&directory) ||
			directory.st_uid != geteuid() || (directory.st_mode & 0077) ||
			!Held_StoreResolved(t->recovery_root,s.canonical,&resolved) ||
			directory.st_dev != resolved.st_dev || directory.st_ino != resolved.st_ino ||
			!FS_ValidateExternalDirectory(Held_StoreOutside,&s) ||
			fstat(t->file,&source) || !S_ISREG(source.st_mode) || source.st_nlink != 1 ||
			source.st_uid != geteuid() || (source.st_mode & 0077) ||
			source.st_dev != directory.st_dev || source.st_dev != t->identity.st_dev ||
			source.st_ino != t->identity.st_ino ||
			fstatat(t->parent,t->leaf,&named,AT_SYMLINK_NOFOLLOW) ||
			!S_ISREG(named.st_mode) || named.st_nlink != 1 ||
			named.st_dev != source.st_dev || named.st_ino != source.st_ino) goto posix_done;
		/* Same accepted official-server/OS owner trust as ticket retirement.
		 * RENAME_NOREPLACE=1: unsupported kernels refuse, never emulate by copy. */
		if (syscall(SYS_renameat2,t->parent,t->leaf,s.stage_dir,leaf,1)) goto posix_done;
		if (fstatat(s.stage_dir,leaf,&named,AT_SYMLINK_NOFOLLOW) ||
			named.st_dev != source.st_dev || named.st_ino != source.st_ino ||
			named.st_size != source.st_size || named.st_nlink != 1) goto posix_done;
		if (close(t->parent)) held_store_fault = 1;
		t->parent = s.stage_dir; s.stage_dir = -1;
		Q_strncpyz(t->leaf,leaf,sizeof(t->leaf)); valid = 1;
posix_done:
		if (s.stage_dir >= 0 && close(s.stage_dir)) held_store_fault = 1;
	}
#endif
#endif
	Held_StoreWipe(&s,sizeof(s));
	if (valid)
	{
		t->recovered = 1;
		FS_FlushFSHashRemoved(t->relative);
	}
	return valid;
}
qboolean SV_HeldFileRecover(held_file_ticket_t *t, const char *stage_root, const char *generation)
{
	const char *p;
	if (!t || !t->owner_set || !stage_root || !*stage_root || strlen(stage_root) >= sizeof(t->recovery_root) ||
		!generation || strlen(generation) != 32) return false;
	for (p = generation; *p; ++p)
		if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
	if (t->recovering)
	{
		if (strcmp(t->recovery_root,stage_root) || strcmp(t->recovery_generation,generation)) return false;
	}
	else
	{
		Q_strncpyz(t->recovery_root,stage_root,sizeof(t->recovery_root));
		Q_strncpyz(t->recovery_generation,generation,sizeof(t->recovery_generation));
		t->recovering = 1;
	}
	if (t->recovered || !t->closed) return true; /* Open stream recovers at its exact close. */
	if (Held_FileRecoverClosed(t)) return true;
	held_store_fault = 1;
	Con_Printf(CON_ERROR "held source recovery ERROR; original not consumed; new held collection disabled\n");
	return false;
}

/* Owned dummy-byte control, including deferred teardown and an occupied private
 * destination. Never tests on player source, never consumes a recovered file. */
static int Held_FileRecoveryTest(const char *stage_root, unsigned int *checks)
{
	char generation[33], path[MAX_QPATH], content[48], data[48];
	unsigned char random[16];
	held_file_ticket_t *ticket = NULL, *first = NULL;
	held_source_owner_t owner;
	vfsfile_t *stream, *visible;
	unsigned int i, j, failures = 0, prior_fault = held_store_fault;
	int length;
#define RECOVER_TEST(x) do {++*checks; if (!(x)) ++failures;} while (0)
	for (i = 0; i < 3; ++i)
	{
		if (i != 1)
		{
			if (!Sys_RandomBytes(random,sizeof(random))) {RECOVER_TEST(0); break;}
			for (j = 0; j < sizeof(random); ++j) Q_snprintfz(generation+j*2,3,"%02x",random[j]);
		}
		Q_snprintfz(path,sizeof(path),"data/parts/hr-%s.rec",generation);
		Q_snprintfz(content,sizeof(content),"recovery-original-%u",i); length = strlen(content)+1;
		stream = SV_HeldFileCreate(path,&ticket);
		RECOVER_TEST(stream && ticket);
		if (!stream) break;
		memset(&owner,0,sizeof(owner)); owner.vm = &failures;
		owner.client_uid = 17; owner.entity_index = 1; owner.serial = i+1;
		Q_strncpyz(owner.record,"recovery-unit",sizeof(owner.record));
		RECOVER_TEST(SV_HeldFileSetOwner(ticket,&owner));
		RECOVER_TEST(VFS_WRITE(stream,content,length) == length);
		if (i == 2)
		{
			RECOVER_TEST(SV_HeldFileRecover(ticket,stage_root,generation));
			RECOVER_TEST(ticket->recovering && !ticket->recovered && !ticket->closed);
		}
		RECOVER_TEST(VFS_CLOSE(stream));
		if (i == 1)
		{
			RECOVER_TEST(!SV_HeldFileRecover(ticket,stage_root,generation));
			RECOVER_TEST(!ticket->recovered && held_store_fault);
			RECOVER_TEST(SV_HeldFileReadAt(ticket,0,data,length) == length && !memcmp(data,content,length));
			RECOVER_TEST(first && SV_HeldFileReadAt(first,0,data,sizeof("recovery-original-0")) == sizeof("recovery-original-0") &&
				!memcmp(data,"recovery-original-0",sizeof("recovery-original-0")));
			/* Only our verified disposable dummy still in its original namespace. */
			RECOVER_TEST(SV_HeldFileRetire(ticket,true)); ticket = NULL;
		}
		else
		{
			RECOVER_TEST(SV_HeldFileRecover(ticket,stage_root,generation));
			RECOVER_TEST(ticket->recovered && SV_HeldFileLength(ticket) == length);
			RECOVER_TEST(SV_HeldFileReadAt(ticket,0,data,length) == length && !memcmp(data,content,length));
			visible = FS_OpenVFS(path,"rb",FS_GAMEONLY);
			RECOVER_TEST(!visible); if (visible) VFS_CLOSE(visible);
			if (!i) {first = ticket; ticket = NULL;}
		}
		if (ticket) {RECOVER_TEST(SV_HeldFileRetire(ticket,false)); ticket = NULL;}
	}
	if (first) RECOVER_TEST(SV_HeldFileRetire(first,false));
	/* Restore ONLY a fully verified expected negative control's latch. */
	if (!failures) held_store_fault = prior_fault;
	Held_StoreWipe(random,sizeof(random)); Held_StoreWipe(content,sizeof(content)); Held_StoreWipe(data,sizeof(data));
#undef RECOVER_TEST
	return failures;
}
