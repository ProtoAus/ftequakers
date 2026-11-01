#ifdef __linux__
#define FS_TAS_EXPORT_RECORDS 262144
#define FS_TAS_EXPORT_NAMES (64u*1024u*1024u)
#define FS_TAS_EXPORT_FILES 8000
#define FS_TAS_EXPORT_FILE ((qofs_t)512*1024*1024)
#define FS_TAS_EXPORT_TOTAL ((qofs_t)2*1024*1024*1024)
// Once-per-profile materialisation, never ordinary replay-job limits.
#define FS_TAS_PREP_FILES 131072
#define FS_TAS_PREP_TOTAL ((qofs_t)8*1024*1024*1024)
typedef struct {
	const char *name;
	qofs_t size;
	unsigned int source, directory;
} fs_tas_export_name_t;
typedef struct {
	fs_tas_export_name_t *names;
	char *arena;
	unsigned int count, used, source, depth, pathcount, files, missing, epoch;
	searchpath_t *paths[FS_TAS_LOOKUP_PATHS], *current;
	char queries[96][MAX_OSPATH], output[MAX_OSPATH];
	unsigned int querycount;
	vfsfile_t *manifest;
	qbyte manifesthash[512];
	qofs_t bytes, largest;
	qboolean measure, preparation;
	const char *error;
} fs_tas_export_t;
static qboolean FS_TASExportRow(fs_tas_export_t *e, const char *fmt, ...) LIKEPRINTF(2);
static qboolean FS_TASExportRow(fs_tas_export_t *e, const char *fmt, ...)
{
	char row[16384];
	va_list args;
	int len;
	if (e->error) return false;
	va_start(args,fmt);
	len = vsnprintf(row,sizeof(row),fmt,args);
	va_end(args);
	if (len < 0 || len >= sizeof(row)) { e->error = "row_bound"; return false; }
	if (VFS_WRITE(e->manifest,row,len) != len) { e->error = "manifest_write"; return false; }
	hash_sha2_256.process(e->manifesthash,row,len);
	return true;
}
static qboolean FS_TASExportAdd(fs_tas_export_t *e, const char *name, qofs_t size, qboolean directory)
{
	size_t len = strlen(name);
	char check[MAX_OSPATH];
	unsigned int i;
	if (!len || len >= sizeof(check)) { e->error = "name_bound"; return false; }
	Q_strncpyz(check,name,sizeof(check));
	if (directory) check[len-1] = 0;
	if (!FS_TASLookupName(check)) { e->error = "name_unsupported"; return false; }
	for (i = 0; i < len; i++)
		if ((unsigned char)name[i] >= 128) { e->error = "name_encoding_unsupported"; return false; }
	if (e->count == FS_TAS_EXPORT_RECORDS || len+1 > FS_TAS_EXPORT_NAMES-e->used)
	{
		e->error = "namespace_bound";
		return false;
	}
	memcpy(e->arena+e->used,name,len+1);
	e->names[e->count].name = e->arena+e->used;
	e->names[e->count].source = e->source;
	e->names[e->count].directory = directory;
	e->names[e->count++].size = size;
	e->used += len+1;
	return true;
}
static qboolean FS_TASExportQuery(fs_tas_export_t *e, const char *name)
{
	if (e->querycount == countof(e->queries) || strlen(name) >= sizeof(e->queries[0]))
	{
		e->error = "query_bound";
		return false;
	}
	Q_strncpyz(e->queries[e->querycount++],name,sizeof(e->queries[0]));
	return true;
}
static qboolean FS_TASExportDomain(fs_tas_export_t *e, const char *name, qboolean directory)
{
	unsigned int i;
	size_t len = strlen(name), alen;
	for (i = 0; i < countof(fs_tas_asset_prefixes); i++)
	{
		alen = strlen(fs_tas_asset_prefixes[i]);
		if (!Q_strncasecmp(name,fs_tas_asset_prefixes[i],alen)) return true;
		if (directory && len <= alen && !Q_strncasecmp(name,fs_tas_asset_prefixes[i],len)) return true;
	}
	for (i = 0; i < e->querycount; i++)
		if (directory ? !Q_strncasecmp(name,e->queries[i],len) : !Q_strcasecmp(name,e->queries[i])) return true;
	return false;
}
static int QDECL FS_TASExportEnumerated(const char *name, qofs_t size, time_t mtime, void *opaque, searchpathfuncs_t *spath)
{
	fs_tas_export_t *e = opaque;
	size_t len = strlen(name);
	qboolean directory = len && name[len-1] == '/';
	char physical[MAX_OSPATH], pattern[MAX_OSPATH];
	struct stat info;
	int okay;
	if (e->error) return false;
	if (!FS_TASExportDomain(e,name,directory)) return true;
	if (size == qofs_ErrorValue()) { e->error = "enum_unmodelled"; return false; }
	if (spath && spath != e->current->handle) { e->error = "enum_provider"; return false; }
	if (!FS_TASExportAdd(e,name,size,directory)) return false;
	if (e->current->origin_kind != FSORIGIN_DIRECTORY) return true;
	if (!try_snprintf(physical,sizeof(physical),"%s/%s",e->current->origin_root,name))
	{
		e->error = "directory_bound";
		return false;
	}
	if (directory) physical[strlen(physical)-1] = 0;
	if (lstat(physical,&info))
	{
		e->error = "directory_stat";
		return false;
	}
	if (S_ISLNK(info.st_mode) || (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode)))
	{
		e->error = "directory_type";
		return false;
	}
	if (!directory) return true;
	if (e->depth == 64 || !try_snprintf(pattern,sizeof(pattern),"%s*",name))
	{
		e->error = "directory_depth";
		return false;
	}
	e->depth++;
	okay = e->current->handle->EnumerateFiles(e->current->handle,pattern,FS_TASExportEnumerated,e);
	e->depth--;
	if (!okay && !e->error) e->error = "directory_enum";
	return !e->error;
}
static int FS_TASExportCompare(const void *av, const void *bv)
{
	const fs_tas_export_name_t *a = av, *b = bv;
	int value = Q_strcasecmp(a->name,b->name);
	if (!value) value = strcmp(a->name,b->name);
	if (!value) value = (int)a->source-(int)b->source;
	return value;
}
static const char *FS_TASExportAliases(const fs_tas_export_name_t *a, const fs_tas_export_name_t *b)
{
	size_t i, lastslash = 0, len = strlen(a->name);
	qboolean different = false, parentdifferent = false;
	for (i = 0; a->name[i] && b->name[i] && tolower((unsigned char)a->name[i]) == tolower((unsigned char)b->name[i]); i++)
	{
		if (a->name[i] != b->name[i]) different = true;
		if (a->name[i] == '/') { lastslash = i+1; parentdifferent = different; }
	}
	if (lastslash && parentdifferent) return "parent_case_alias";
	if (!a->name[i] && !b->name[i] && different) return "case_alias";
	if (!a->directory && !Q_strncasecmp(a->name,b->name,len) && b->name[len] == '/') return "file_directory_alias";
	if (!strcmp(a->name,b->name) && a->source == b->source) return "duplicate_member";
	return NULL;
}
static qboolean FS_TASExportHasDescendant(fs_tas_export_t *e, const char *name)
{
	char prefix[MAX_OSPATH+1];
	size_t len;
	unsigned int lo = 0, hi = e->count, mid;
	Q_snprintfz(prefix,sizeof(prefix),"%s/",name);
	len = strlen(prefix);
	// Prefix lower-bound also finds descendants separated by punctuation siblings.
	while (lo < hi)
	{
		mid = lo+(hi-lo)/2;
		if (Q_strncasecmp(e->names[mid].name,prefix,len) < 0) lo = mid+1;
		else hi = mid;
	}
	return lo < e->count && !Q_strncasecmp(e->names[lo].name,prefix,len);
}
static qboolean FS_TASExportParents(fs_tas_export_t *e, const char *full)
{
	char path[MAX_OSPATH], *p;
	struct stat info;
	Q_strncpyz(path,full,sizeof(path));
	for (p = path+1; *p; p++)
		if (*p == '/')
		{
			*p = 0;
			if (lstat(path,&info))
			{
				if (errno != ENOENT || !FS_TASCheckOSOpen(path,"wb") || mkdir(path,0700))
				{
					e->error = "output_parent";
					return false;
				}
			}
			else if (!S_ISDIR(info.st_mode)) { e->error = "output_parent_type"; return false; }
			*p = '/';
		}
	return true;
}
static qboolean FS_TASExportFile(fs_tas_export_t *e, const char *name, qboolean enumerated)
{
	flocation_t loc;
	vfsfile_t *input = NULL, *output = NULL;
	hashfunc_t *hash = &hash_sha2_256;
	qbyte state[512], buffer[8192], digest[32];
	char target[MAX_OSPATH], hex[MAX_OSPATH*2+1], sha[65];
	qofs_t size = 0, total = 0;
	unsigned int id;
	int got, want;
	struct stat info;
	qboolean okay = false;
	if (!FS_TASLookupHex(name,hex,sizeof(hex))) { e->error = "name_encoding"; return false; }
	memset(&loc,0,sizeof(loc));
	if (!FS_FLocateFilePurpose(name,FSLF_IFFOUND|FSLF_DONTREFERENCE|FSLF_UNCACHED,&loc,true))
	{
		if (FS_TASPolicyError()) { e->error = FS_TASPolicyError(); return false; }
		if (enumerated) { e->error = "unresolved_entry"; return false; }
		e->missing++;
		return FS_TASExportRow(e,"MISSING %s\n",hex);
	}
	id = FS_TASLookupPathId(loc.search,e->paths,e->pathcount);
	if (!id) { e->error = "external_provider"; return false; }
	input = FS_OpenReadLocationInternal(name,&loc,true,true);
	if (!input) { e->error = "open"; goto done; }
	size = VFS_GETLEN(input);
	if (size == qofs_ErrorValue() || size > FS_TAS_EXPORT_FILE)
	{
		e->error = "file_bound";
		goto done;
	}
	if (e->measure)
	{
		// Reported native handle lengths only: no body/hash/export evidence.
		if (!VFS_CLOSE(input)) { input = NULL; e->error = "close"; goto done; }
		input = NULL;
		e->bytes += size; // Finite namespace records * per-file ceiling <= 2^47.
		if (size > e->largest) e->largest = size;
		e->files++;
		okay = FS_TASExportRow(e,"MEASURED %u %"PRIuQOFS" %s\n",id,size,hex);
		goto done;
	}
	if (e->bytes > (e->preparation?FS_TAS_PREP_TOTAL:FS_TAS_EXPORT_TOTAL)-size ||
		e->files == (e->preparation?FS_TAS_PREP_FILES:FS_TAS_EXPORT_FILES))
	{
		e->error = "file_bound";
		goto done;
	}
	if (!try_snprintf(target,sizeof(target),"%s/%s",e->output,name)) { e->error = "output_bound"; goto done; }
	if (!FS_TASExportParents(e,target)) goto done;
	if (!lstat(target,&info) || errno != ENOENT) { e->error = "output_collision"; goto done; }
	output = VFSOS_Open(target,"wb");
	if (!output) { e->error = "output_open"; goto done; }
	hash->init(state);
	while (total < size)
	{
		want = size-total > sizeof(buffer) ? sizeof(buffer) : (int)(size-total);
		got = VFS_READ(input,buffer,want);
		if (got <= 0 || got > want) { e->error = "read"; goto done; }
		if (VFS_WRITE(output,buffer,got) != got) { e->error = "output_write"; goto done; }
		hash->process(state,buffer,got);
		total += got;
	}
	if (VFS_READ(input,buffer,1) != 0 || VFS_GETLEN(input) != size) { e->error = "read_changed"; goto done; }
	if (!VFS_CLOSE(input)) { input = NULL; e->error = "close"; goto done; }
	input = NULL;
	if (!VFS_CLOSE(output)) { output = NULL; e->error = "output_close"; goto done; }
	output = NULL;
	hash->terminate(digest,state);
	Base16_EncodeBlock(digest,sizeof(digest),sha,sizeof(sha));
	sha[64] = 0;
	e->bytes += size;
	e->files++;
	okay = FS_TASExportRow(e,"FILE %u %"PRIuQOFS" %s %s\n",id,size,sha,hex);
 done:
	if (input) VFS_CLOSE(input);
	if (output) VFS_CLOSE(output);
	return okay;
}
static void FS_TASExportNative(fs_tas_export_t *e)
{
	static const char *sidecars[] = {".bsp",".phy",".phs",".ent",".edo",".entities",".lit",".vis",".rtlights",".bspx",".lux",".loc"};
	static const char *zones[] = {"data/maps/zones","data/maps/zones/curated","data/maps/zones/local","maps/zones","maps/zones/curated","maps/zones/local","data/maps/zones/online","maps/zones/online","data/maps/zones/dl","maps/zones/dl"};
	searchpath_t *path;
	fs_tas_input_t *inputs = NULL;
	qboolean pureseen[FS_TAS_LOOKUP_PATHS] = {false};
	unsigned int i, j, id, purecount = 0, inputcount, pureorder[FS_TAS_LOOKUP_PATHS];
	qboolean enumerated;
	int root, result;
	char name[MAX_OSPATH], hex[MAX_OSPATH*2+1], roothex[MAX_OSPATH*2+1], sha[65], manifest[MAX_OSPATH];
	qbyte digest[32];
	struct stat info;
	size_t stem = strlen(fs_tas_worldfile)-4, privateprefix;
	const char *previous = NULL, *world = fs_tas_worldfile+5;

	if (!fs_tas_policy_enabled || !fs_tas_policy_began) { e->error = "policy_not_begun"; return; }
	if (*fs_preferhint) { e->error = "prefer_unsupported"; return; }
	if (hash_sha2_256.contextsize > sizeof(e->manifesthash) || hash_sha2_256.digestsize != sizeof(digest)) { e->error = "hash_bound"; return; }
	root = FS_TASPhysicalRoot(Cmd_Argv(1),e->output,sizeof(e->output));
	if (root < 0 || fs_tas_roots[root].kind != 2) { e->error = "output_scope"; return; }
	privateprefix = strlen(fs_tas_roots[root].name)+1;
	if (strlen(e->output) <= privateprefix || strncmp(e->output+privateprefix,"data/tasnative/",sizeof("data/tasnative/")-1)) { e->error = "output_namespace"; return; }
	if (!lstat(e->output,&info) || errno != ENOENT) { e->error = "output_exists"; return; }
	if (!FS_TASCheckOSOpen(e->output,"wb") || !FS_TASExportParents(e,e->output) || mkdir(e->output,0700)) { e->error = "output_create"; return; }
	if (!try_snprintf(manifest,sizeof(manifest),"%s/index.txt",e->output)) { e->error = "output_bound"; return; }
	e->manifest = VFSOS_Open(manifest,"wb");
	if (!e->manifest) { e->error = "manifest_open"; return; }
	hash_sha2_256.init(e->manifesthash);
	e->names = BZ_Malloc(FS_TAS_EXPORT_RECORDS*sizeof(*e->names));
	e->arena = BZ_Malloc(FS_TAS_EXPORT_NAMES);
	for (i = 0; i < countof(fs_tas_queries); i++) if (!FS_TASExportQuery(e,fs_tas_queries[i])) return;
	for (i = 0; i < countof(fs_tas_data_queries); i++) if (!FS_TASExportQuery(e,fs_tas_data_queries[i])) return;
	for (i = 0; i < countof(sidecars); i++)
	{
		if (!try_snprintf(name,sizeof(name),"%.*s%s",(int)stem,fs_tas_worldfile,sidecars[i]) || !FS_TASExportQuery(e,name)) { e->error = "query_bound"; return; }
	}
	for (i = 0; i < countof(zones); i++)
	{
		if (!try_snprintf(name,sizeof(name),"%s/%.*s.json",zones[i],(int)stem-5,world) || !FS_TASExportQuery(e,name)) { e->error = "query_bound"; return; }
	}
	if (!try_snprintf(name,sizeof(name),"cfg/maps/map_%.*s.cfg",(int)stem-5,world) || !FS_TASExportQuery(e,name)) { e->error = "query_bound"; return; }
	if (!try_snprintf(name,sizeof(name),"cfg/maps/entfix_%.*s.cfg",(int)stem-5,world) || !FS_TASExportQuery(e,name)) { e->error = "query_bound"; return; }
	if (!try_snprintf(name,sizeof(name),"data/maps/settings/local/%.*s.cfg",(int)stem-5,world) || !FS_TASExportQuery(e,name)) { e->error = "query_bound"; return; }
	if (!try_snprintf(name,sizeof(name),"maps/settings/local/%.*s.cfg",(int)stem-5,world) || !FS_TASExportQuery(e,name)) { e->error = "query_bound"; return; }
	for (i = 0; i < countof(fs_tas_map_probes); i++)
		if (!FS_TASMapProbeName(i,name,sizeof(name)) || !FS_TASExportQuery(e,name)) { e->error = "query_bound"; return; }
	e->epoch = fs_restarts;
	for (path = com_searchpaths; path; path = path->next)
	{
		if (e->pathcount == countof(e->paths) || FS_TASLookupPathId(path,e->paths,e->pathcount)) { e->error = "path_bound"; return; }
		if (!path->origin_id || !path->origin_policy_bound) { e->error = "unbound_provider"; return; }
		if (*path->prefix) { e->error = "prefix_unsupported"; return; }
		if (path->origin_kind == FSORIGIN_DIRECTORY)
		{
			if (!*path->origin_root) { e->error = "directory_unbound"; return; }
		}
		else
		{
			const char *ext = COM_GetFileExtension(path->origin_name,NULL);
			if (path->origin_kind == FSORIGIN_OPAQUE ||
				(Q_strcasecmp(ext,".zip") && Q_strcasecmp(ext,".pk3") && Q_strcasecmp(ext,".pak") && Q_strcasecmp(ext,".vpk") && path->origin_kind != FSORIGIN_MAP))
			{
				e->error = "driver_unsupported";
				return;
			}
		}
		e->paths[e->pathcount++] = path;
	}
	if (!e->pathcount) { e->error = "no_paths"; return; }
	FS_TASLookupHex(fs_tas_worldfile,hex,sizeof(hex));
	if (!FS_TASExportRow(e,e->measure ? "TASVFSMEASURE 1 diagnostic-length view10 %u %u %i %i %i %s\n" :
		e->preparation ? "TASVFSEXPORT 10 profileprep %u %u %i %i %i %s\n" : "TASVFSEXPORT 10 component %u %u %i %i %i %s\n",e->epoch,e->pathcount,fs_puremode,com_fs_cache.ival,fs_mapmaterials.ival,hex)) return;
	for (i = 0; i < fs_tas_rootcount; i++)
	{
		if (!FS_TASLookupHex(fs_tas_roots[i].name,hex,sizeof(hex)) || !FS_TASExportRow(e,"ROOT %u %u %s\n",i+1,fs_tas_roots[i].kind,hex)) return;
	}
	for (i = 0; i < e->pathcount; i++)
	{
		path = e->paths[i];
		if (!FS_TASLookupHex(path->origin_name,hex,sizeof(hex)) || !FS_TASLookupHex(path->origin_root,roothex,sizeof(roothex))) { e->error = "origin_encoding"; return; }
		if (!FS_TASExportRow(e,"SOURCE %u %u %u %u %u %"PRIu64" %"PRIu64" %s %s\n",i+1,path->flags,(unsigned int)path->origin_kind,path->origin_policy_bound,path->origin_policy_private,path->origin_id,path->origin_parent_id,hex,roothex)) return;
		if (!FS_TASLookupHex(path->logicalpath,hex,sizeof(hex)) || !FS_TASExportRow(e,"PATH %u %s\n",i+1,hex)) return;
		if (!FS_TASLookupHex(path->purepath,hex,sizeof(hex)) || !FS_TASExportRow(e,"PUREPATH %u %s\n",i+1,hex)) return;
	}
	for (path = com_purepaths; path; path = path->nextpure)
	{
		id = FS_TASLookupPathId(path,e->paths,e->pathcount);
		if (!id || pureseen[id-1] || purecount == e->pathcount) { e->error = "pure_bound"; return; }
		pureseen[id-1] = true;
		pureorder[purecount] = id;
		if (!FS_TASExportRow(e,"PUREORDER %u %u\n",++purecount,id)) return;
	}
	for (i = 0; i < e->querycount; i++) if (!FS_TASExportAdd(e,e->queries[i],0,false)) return;
	for (i = 0; i < e->pathcount; i++)
	{
		e->source = i+1;
		e->current = e->paths[i];
		if (!e->current->handle->EnumerateFiles) { e->error = "enum_unsupported"; return; }
		if (e->current->origin_kind == FSORIGIN_DIRECTORY)
		{
			if (lstat(e->current->origin_root,&info))
			{
				if (errno != ENOENT) { e->error = "root_stat"; return; }
				if (!FS_TASExportRow(e,"DIRECTORY_ABSENT %u\n",i+1)) return;
				continue;
			}
			if (!S_ISDIR(info.st_mode)) { e->error = "root_type"; return; }
		}
		// Known archive drivers expose their complete table for NULL, not per-dir '*'.
		result = e->current->handle->EnumerateFiles(e->current->handle,e->current->origin_kind == FSORIGIN_DIRECTORY ? "*" : NULL,FS_TASExportEnumerated,e);
		if (!result || e->error) { if (!e->error) e->error = "enum_failed"; return; }
	}
	qsort(e->names,e->count,sizeof(*e->names),FS_TASExportCompare);
	for (i = 1; i < e->count; i++)
		if ((e->error = FS_TASExportAliases(&e->names[i-1],&e->names[i]))) return;
	for (i = 0; i < e->count; i++)
		if (!e->names[i].directory && FS_TASExportHasDescendant(e,e->names[i].name))
		{
			e->error = "file_directory_alias";
			return;
		}
	for (i = 0; i < e->count; i++)
	{
		if (!FS_TASLookupHex(e->names[i].name,hex,sizeof(hex)) || !FS_TASExportRow(e,"ENTRY %u %u %"PRIuQOFS" %s\n",e->names[i].source,e->names[i].directory,e->names[i].size,hex)) return;
		if (e->names[i].directory) continue;
		if (previous && !strcmp(previous,e->names[i].name)) continue;
		previous = e->names[i].name;
		enumerated = false;
		for (j = i; j < e->count && !strcmp(e->names[j].name,previous); j++)
			if (e->names[j].source && !e->names[j].directory) enumerated = true;
		if (!FS_TASExportFile(e,previous,enumerated)) return;
	}
	for (path = com_searchpaths, i = 0; path && i < e->pathcount; path = path->next, i++)
		if (path != e->paths[i]) { e->error = "graph_changed"; return; }
	if (path || i != e->pathcount || fs_restarts != e->epoch) { e->error = "graph_changed"; return; }
	for (path = com_purepaths, i = 0; path; path = path->nextpure, i++)
	{
		id = FS_TASLookupPathId(path,e->paths,e->pathcount);
		if (!id || i >= purecount || pureorder[i] != id) { e->error = "pure_changed"; return; }
	}
	if (i != purecount) { e->error = "pure_changed"; return; }
	inputs = BZ_Malloc(sizeof(fs_tas_inputs));
	Sys_LockMutex(fs_tas_policy_mutex);
	inputcount = fs_tas_inputcount;
	memcpy(inputs,fs_tas_inputs,inputcount*sizeof(*inputs));
	Sys_UnlockMutex(fs_tas_policy_mutex);
	for (i = 0; i < inputcount; i++)
	{
		if (!FS_TASLookupHex(inputs[i].physical,hex,sizeof(hex))) { e->error = "input_encoding"; break; }
		if (!FS_TASExportRow(e,"INPUT %u %u %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRId64" %u %"PRId64" %u %s\n",i+1,inputs[i].kind,inputs[i].device,inputs[i].inode,inputs[i].links,inputs[i].size,inputs[i].mtime,inputs[i].mtime_ns,inputs[i].ctime,inputs[i].ctime_ns,hex)) break;
	}
	BZ_Free(inputs);
	if (e->error) return;
	Sys_LockMutex(fs_tas_policy_mutex);
	result = inputcount == fs_tas_inputcount;
	Sys_UnlockMutex(fs_tas_policy_mutex);
	if (!result) { e->error = "inputs_changed"; return; }
	if ((e->error = FS_TASPolicyError())) return;
	if (e->measure)
	{
		if (!FS_TASExportRow(e,"END diagnostic-length %u %u %u %"PRIuQOFS" %"PRIuQOFS"\n",e->files,e->missing,e->count,e->bytes,e->largest)) return;
	}
	else if (!FS_TASExportRow(e,"END %s %u %u %u %"PRIuQOFS"\n",e->preparation?"profileprep":"component",e->files,e->missing,e->count,e->bytes)) return;
	if (!VFS_CLOSE(e->manifest)) { e->manifest = NULL; e->error = "manifest_close"; return; }
	e->manifest = NULL;
	hash_sha2_256.terminate(digest,e->manifesthash);
	Base16_EncodeBlock(digest,sizeof(digest),sha,sizeof(sha));
	sha[64] = 0;
	if (!FS_TASLookupHex(e->output,hex,sizeof(hex))) { e->error = "output_encoding"; return; }
	if (e->measure)
		Con_Printf("TASVFSMEASURE 1 end diagnostic-length %u %"PRIuQOFS" %"PRIuQOFS" %s %s\n",e->files,e->bytes,e->largest,sha,hex);
	else
		Con_Printf("TASVFSEXPORT 1 end %s %u %"PRIuQOFS" %s %s\n",e->preparation?"profileprep":"component",e->files,e->bytes,sha,hex);
}
#endif
static void FS_TASExport_f(void)
{
	const char *tag = !strcmp(Cmd_Argv(0),"fs_tas_measure") ? "TASVFSMEASURE" : "TASVFSEXPORT";
#ifdef __linux__
	fs_tas_export_t *e;
	const char *error;
	qboolean preparation = !strcmp(Cmd_Argv(0),"fs_tas_profile_export");
	if (Cmd_IsInsecure() || Cmd_FromGamecode()) { Con_Printf("%s 1 error untrusted\n",tag); return; }
	if (!COM_CheckParm("-tas-vfs-probe")) { Con_Printf("%s 1 error disabled\n",tag); return; }
	if (preparation && !COM_CheckParm("-tas-vfs-profile-prepare")) { Con_Printf("%s 1 error preparation_context\n",tag); return; }
	if (Cmd_Argc() != 2) { Con_Printf("%s 1 error arguments\n",tag); return; }
	error = FS_TASPolicyError();
	if (error) { Con_Printf("%s 1 error %s\n",tag,error); FS_TASPolicyDetail(); return; }
	e = BZ_Malloc(sizeof(*e));
	memset(e,0,sizeof(*e));
	e->measure = !strcmp(tag,"TASVFSMEASURE");
	e->preparation = preparation;
	COM_WorkerLock();
	FS_TASExportNative(e);
	if (FS_TASPolicyError()) e->error = FS_TASPolicyError();
	if (e->manifest) VFS_CLOSE(e->manifest);
	if (e->error) { Con_Printf("%s 1 error %s\n",tag,e->error); FS_TASPolicyDetail(); }
	COM_WorkerUnlock();
	if (e->arena) BZ_Free(e->arena);
	if (e->names) BZ_Free(e->names);
	BZ_Free(e);
#else
	Con_Printf("%s 1 error platform_unsupported\n",tag);
#endif
}
