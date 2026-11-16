#!/usr/bin/env python3
"""P1593: compile qkupdate.c's identity selection and signed-manifest policy; drive two identities.

The real slices (QKU_ValidPath .. QKU_FSValidateManifest, QKU_FSNoLinks, QKU_FSRegular ..
QKU_FSIdentity) are compiled with the real json.c and qclib/hash.c under a stub quakedef.h,
once per platform; QKU_InstallPath is stubbed onto a fixture root. Signatures are not involved: the
policy runs on the parsed payload, exactly as after QKU_FSParseSigned. Mutants are plausible
wrong cuts of the change; each must let through exactly the cases that guard against it.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

ENGINE = Path(__file__).resolve().parents[1] / 'engine'
HASH = '0' * 64
EXES = {'ftesurf': ('ftesurf64.exe', 'ftesurf64'), 'worldofsurf': ('WorldofSurf.exe', 'worldofsurf64')}

STUB = r'''
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <errno.h>
#include <sys/stat.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
typedef int qboolean;
#define true 1
#define false 0
typedef unsigned char qbyte;
typedef uint64_t quint64_t;
typedef intptr_t qintptr_t;
typedef uintptr_t quintptr_t;
#define MAX_OSPATH 1024
#define MAX_QPATH 64
#define max(a, b) ((a) > (b) ? (a) : (b))
unsigned int utf8_encode(void *out, unsigned int unicode, int maxlen);
#define countof(array) (sizeof(array)/sizeof((array)[0]))
#define Q_strncasecmp strncasecmp
#define Q_strcasecmp strcasecmp
#define Q_snprintfz snprintf
#define Z_Malloc(n) calloc(1, (n))
#define Z_Free free
#define BZ_Free free
#define QKU_HEXLEN 64
enum { GAMEDIR_BASEGAME=1u<<0, GAMEDIR_PRIVATE=1u<<1, GAMEDIR_USEBASEDIR=1u<<3, GAMEDIR_STEAMGAME=1u<<4,
	GAMEDIR_SPECIAL=GAMEDIR_USEBASEDIR|GAMEDIR_STEAMGAME };
typedef struct { struct { unsigned flags; char *path; } gamepath[8]; } ftemanifest_t;
'''

PRE = r'''
static char harness_root[1024];
static void QKU_InstallPath(char *out, size_t outsize, const char *rel)
{
	snprintf(out, outsize, "%s/%s", harness_root, rel);
}
static char *QKU_ReadTextFile(const char *abspath, size_t maxsize)	//the engine's, over stdio
{
	FILE *f = fopen(abspath, "rb");
	char *buf = calloc(1, maxsize + 1);
	size_t n = f ? fread(buf, 1, maxsize, f) : 0;
	if (f) fclose(f);
	if (!f || n >= maxsize || strlen(buf) != n) { free(buf); return NULL; }
	return buf;
}
'''

DRIVER = r'''
static char *readall(const char *name)
{
	FILE *f = fopen(name, "rb");
	long n;
	char *text;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	text = calloc(1, n + 1);
	if (fread(text, 1, n, f) != (size_t)n) { fclose(f); free(text); return NULL; }
	fclose(f);
	return text;
}

static const qku_identity_t *named(const char *name)
{
	size_t i;
	for (i = 0; i < countof(qku_identities); i++) if (!strcmp(name, qku_identities[i].game)) return &qku_identities[i];
	return NULL;
}

//select [*]basegame...: '*' marks a GAMEDIR_SPECIAL entry. identity <root> <basegame>: the boot
//rule, BASEGAME plus its gamedir on disk. validate <identity|none> <payload.json>.
//managed <identity> <path>. feed <identity>. Prints the verdict; exit 2 on harness misuse.
int main(int argc, char **argv)
{
	if (argc >= 2 && (!strcmp(argv[1], "select") || !strcmp(argv[1], "identity")))
	{
		ftemanifest_t man;
		const qku_identity_t *id;
		int i, first = 2;
		memset(&man, 0, sizeof(man));
		if (!strcmp(argv[1], "identity"))
		{
			if (argc < 3) return 2;
			snprintf(harness_root, sizeof(harness_root), "%s", argv[2]);
			first = 3;
		}
		for (i = first; i < argc && i - first < 8; i++)
		{
			man.gamepath[i-first].flags = GAMEDIR_BASEGAME | (argv[i][0] == '*' ? GAMEDIR_STEAMGAME : 0);
			man.gamepath[i-first].path = argv[i] + (argv[i][0] == '*');
		}
		id = first == 3 ? QKU_FSIdentity(&man) : QKU_FSSelectIdentity(&man);
		printf("%s\n", id ? id->game : "none");
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "feed"))
	{
		const qku_identity_t *id = named(argv[2]);
		printf("%s\n", id ? id->feed : "none");
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "validate"))
	{
		char *text = readall(argv[3]);
		json_t *root = text ? JSON_Parse(text) : NULL;
		if (!root) return 2;
		qku_id = named(argv[2]);
		printf("%s\n", QKU_FSValidateManifest(root) ? "accept" : "refuse");
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "managed"))
	{
		qku_id = named(argv[2]);
		printf("%s\n", QKU_FSManaged(argv[3]) ? "managed" : "refused");
		return 0;
	}
	return 2;
}
'''

# (needle, replacement, the case names it must let through)
MUTANTS = {
    'union-paths': ('\t\tif (!strcmp(p, old->exe[0]) || !strcmp(p, old->exe[1]) || !strncmp(p, old->dir, olddir)) return false;\n', '',
                    {'legacy gamedir in a new feed', 'legacy exe in a new feed', 'legacy path under new'}),
    'game-unchecked': ('if (!qku_id || strcmp(buf, qku_id->game)) return false;', 'if (!qku_id) return false;',
                       {'ftesurf contents labelled worldofsurf', 'worldofsurf contents labelled ftesurf'}),
    'legacy-exec-accepted': ('JSON_GetString(exec, "cmd", buf, sizeof(buf), ""); if (strcmp(buf, exe)) return false;',
                             'JSON_GetString(exec, "cmd", buf, sizeof(buf), ""); if (strcmp(buf, exe) && strcmp(buf, qku_identities[0].exe[!!strcmp(QKU_PLATFORM, "win64")])) return false;',
                             {'legacy exec in a new feed'}),
    'length-on-respelling': ('\treturn strlen(p) < 240 && QKU_FSLegacySpelling', '\treturn QKU_FSLegacySpelling',
                             {'new path of 240'}),
    'first-basegame': ('\t\t\tlast = man->gamepath[i].path;\n', '\t\t\tif (!last) last = man->gamepath[i].path;\n',
                       {'last basegame wins'}),
    'special-basegame-counted': (' && !(man->gamepath[i].flags & GAMEDIR_SPECIAL) && man->gamepath[i].path', ' && man->gamepath[i].path',
                                 {'special basegame skipped'}),
    'casefold-identity': ('\t\tif (!strcmp(last, qku_identities[i].game))\n', '\t\tif (!Q_strcasecmp(last, qku_identities[i].game))\n',
                          {'basegame matched exactly'}),
    'fallback-without-gamedir': ('\t\tif (QKU_FSGamedirCounts(&qku_identities[i]) != (&qku_identities[i] == id)) return NULL;\n', '\t\t;\n',
                                 {'no gamedir', 'stray folder', 'one required file', 'linked gamedir', 'required file is a directory',
                                  'basegame names a different gamedir', 'half-moved without marker'}),
    'half-moved-allowed': ('\t\tif (QKU_FSGamedirCounts(&qku_identities[i]) != (&qku_identities[i] == id)) return NULL;\n',
                           '\t\tif (&qku_identities[i] == id && !QKU_FSGamedirCounts(id)) return NULL;\n', {'half-moved without marker'}),
    'marker-ignored': ('\tm = QKU_FSMarker(&marked);\n\tif (m) return m > 0 && marked == id ? id : NULL;\n', '\tm = 0; (void)marked; (void)m;\n',
                       {'marker disagrees', 'marker over files', 'marker over half-moved', 'marker in other case',
                        'marker with CRLF', 'marker with two newlines', 'marker is a directory', 'linked marker'}),
    'marker-disagreement-allowed': ('\tif (m) return m > 0 && marked == id ? id : NULL;', '\tif (m) return m > 0 ? id : NULL;',
                                    {'marker disagrees'}),
    'marker-damage-falls-back': ('\tif (m) return m > 0 && marked == id ? id : NULL;', '\tif (m > 0) return marked == id ? id : NULL;',
                                 {'marker in other case', 'marker with CRLF', 'marker with two newlines', 'marker is a directory',
                                  'linked marker'}),
    'marker-loose': ("\tif (n && text[n-1] == '\\n') text[--n] = 0;", "\twhile (n && (text[n-1] == '\\n' || text[n-1] == '\\r')) text[--n] = 0;",
                     {'marker with CRLF', 'marker with two newlines'}),
    'any-file-counts': ('\treturn have >= 2;', '\treturn have >= 1;', {'one required file', 'required file is a directory'}),
    'all-three-required': ('\treturn have >= 2;', '\treturn have >= 3;', {'mid-apply, two files'}),
    'links-count': ('if (QKU_FSNoLinks(rel) && QKU_FSRegular(abs) == 1)', 'if (QKU_FSRegular(abs) == 1)', {'linked gamedir'}),
    'keys-unchecked': ('\tif (QKU_JSONBadKeys(root)) return false;\n', '',
                       {'repeated top-level key', 'repeated nested key', 'escaped duplicate key', 'dotted key',
                        'object over the key cap'}),
    'escapes-allowed': (" || strchr(names[i], '\\\\')", '', {'escaped duplicate key'}),
    'dots-allowed': ("strchr(names[i], '.') || ", '', {'dotted key'}),
    'no-key-cap': ('if (++n > 8192) return true;', '++n;', {'object over the key cap'}),
}


def feed_for(game):
    win, linux = EXES[game]
    gamedir = game + '/'
    files = [{'path': win, 'hash': HASH, 'size': 1, 'platform': 'win64', 'exec': False},
             {'path': linux, 'hash': HASH, 'size': 1, 'platform': 'linux64', 'exec': True},
             {'path': 'default.fmf', 'hash': HASH, 'size': 1, 'platform': 'all', 'exec': False}]
    files += [{'path': gamedir + leaf, 'hash': HASH, 'size': 1, 'platform': 'all', 'exec': False}
              for leaf in ('menu.dat', 'csprogs.dat', 'qwprogs.dat')]
    run = ['-manifest', 'default.fmf']
    return {'schema': 2, 'hash_algo': 'blake2b-256', 'game': game, 'channel': 'stable', 'version': '0.1.25',
            'object_layout': 'objects/{h0h1}/{hash}', 'mirrors': ['https://dl.proto.bar/ftesurf/update'],
            'files': files, 'removed': [], 'exec': {'cmd': win, 'args': run},
            'exec_by_platform': {'win64': {'cmd': win, 'args': run}, 'linux64': {'cmd': linux, 'args': run}}}


def plus(payload, path, platform='all'):
    return dict(payload, files=payload['files'] + [{'path': path, 'hash': HASH, 'size': 1, 'platform': platform, 'exec': False}])


def fixtures(work):
    """Gamedir layouts on disk for the boot identity rule: name -> install root."""
    roots = {}
    layouts = {'full': ('ftesurf', ['menu.dat', 'csprogs.dat', 'qwprogs.dat']), 'empty': (None, []),
               'stray': ('worldofsurf', ['readme.txt']), 'one': ('worldofsurf', ['menu.dat']),
               'two': ('worldofsurf', ['csprogs.dat', 'qwprogs.dat']), 'dirfile': ('worldofsurf', ['csprogs.dat'])}
    for name, (gamedir, leaves) in layouts.items():
        root = work / 'roots' / name
        root.mkdir(parents=True)
        for leaf in leaves:
            (root / gamedir).mkdir(exist_ok=True)
            (root / gamedir / leaf).write_bytes(leaf.encode())
        roots[name] = root
    (roots['dirfile'] / 'worldofsurf/menu.dat').mkdir()
    target = work / 'roots/elsewhere/worldofsurf'
    target.mkdir(parents=True)
    for leaf in ('menu.dat', 'csprogs.dat', 'qwprogs.dat'):
        (target / leaf).write_bytes(leaf.encode())
    roots['linked'] = work / 'roots/linked'
    roots['linked'].mkdir()
    link(target, roots['linked'] / 'worldofsurf')
    # marker layouts: every gamedir qualifies where a fallback would otherwise give an identity
    full = ['menu.dat', 'csprogs.dat', 'qwprogs.dat']
    markers = {'agrees': (['ftesurf'], b'ftesurf\n'), 'disagrees': (['ftesurf'], b'worldofsurf\n'),
               'overfiles': (['ftesurf'], b'worldofsurf\n'), 'halfmoved': (['ftesurf', 'worldofsurf'], b'worldofsurf\n'),
               'bare': (['worldofsurf'], b'worldofsurf'), 'case': (['worldofsurf'], b'WorldofSurf\n'),
               'crlf': (['worldofsurf'], b'worldofsurf\r\n'), 'twonl': (['worldofsurf'], b'worldofsurf\n\n'),
               'dirmarker': (['worldofsurf'], None), 'linkmarker': (['worldofsurf'], None),
               'halfnomarker': (['ftesurf', 'worldofsurf'], False)}
    for name, (gamedirs, marker) in markers.items():
        root = work / 'roots' / name
        for gamedir in gamedirs:
            (root / gamedir).mkdir(parents=True)
            for leaf in full:
                (root / gamedir / leaf).write_bytes(leaf.encode())
        (root / '.ftesurf-updater').mkdir()
        if marker:
            (root / '.ftesurf-updater/identity').write_bytes(marker)
        roots[name] = root
    (roots['dirmarker'] / '.ftesurf-updater/identity').mkdir()
    (work / 'roots/elsewhere/marker').mkdir()
    link(work / 'roots/elsewhere/marker', roots['linkmarker'] / '.ftesurf-updater' / 'identity')
    return roots


def link(target, at):
    if os.name == 'nt':
        subprocess.run(['cmd', '/C', 'mklink', '/J', str(at), str(target)], check=True, capture_output=True)
    else:
        os.symlink(target, at)


def cases(platform, roots):
    """name -> (argv after the binary, expected stdout)."""
    here = 0 if platform == 'win64' else 1
    text = json.dumps(dict(feed_for('ftesurf'), game='worldofsurf'), separators=(',', ':'))
    first_game = '{"game":"ftesurf",' + text[1:]  # JSON_FindChild takes the first: ftesurf
    legacy_text = json.dumps(feed_for('ftesurf'), separators=(',', ':'))
    nested = legacy_text.replace('"hash":"', f'"hash":"{HASH}","hash":"', 1)
    escaped = '{"g\\u0061me":"worldofsurf",' + legacy_text[1:]
    dotted = '{"game.":"ftesurf",' + text[1:]  # JSON_FindChild("game") matches "game.": ftesurf
    capped = legacy_text[:-1] + ',"big":{' + ','.join(f'"k{i}":0' for i in range(8193)) + '}}'
    relabelled = dict(feed_for('worldofsurf'), game='ftesurf')
    legacy_as_new = dict(feed_for('ftesurf'), game='worldofsurf')
    legacy_exec = json.loads(json.dumps(feed_for('worldofsurf')))
    legacy_exec['exec_by_platform'][platform]['cmd'] = EXES['ftesurf'][here]
    feeds = {
        'legacy feed, legacy install': (feed_for('ftesurf'), 'ftesurf', 'accept'),
        'new feed, new install': (feed_for('worldofsurf'), 'worldofsurf', 'accept'),
        'new feed, legacy install': (feed_for('worldofsurf'), 'ftesurf', 'refuse'),
        'legacy feed, new install': (feed_for('ftesurf'), 'worldofsurf', 'refuse'),
        'any feed, no identity': (feed_for('ftesurf'), 'none', 'refuse'),
        'worldofsurf contents labelled ftesurf': (relabelled, 'worldofsurf', 'refuse'),
        'ftesurf contents labelled worldofsurf': (legacy_as_new, 'ftesurf', 'refuse'),
        'legacy gamedir in a new feed': (plus(feed_for('worldofsurf'), 'ftesurf/gfx/x.png'), 'worldofsurf', 'refuse'),
        'legacy exe in a new feed': (plus(feed_for('worldofsurf'), EXES['ftesurf'][here], platform), 'worldofsurf', 'refuse'),
        'legacy exec in a new feed': (legacy_exec, 'worldofsurf', 'refuse'),
        'new gamedir in a legacy feed': (plus(feed_for('ftesurf'), 'worldofsurf/gfx/x.png'), 'ftesurf', 'refuse'),
        'outside the gamedir': (plus(feed_for('worldofsurf'), 'worldofsurf/data/consent.txt'), 'worldofsurf', 'refuse'),
        'traversal out of the gamedir': (plus(feed_for('worldofsurf'), 'worldofsurf/../ftesurf/menu.dat'), 'worldofsurf', 'refuse'),
        'repeated top-level key': (first_game, 'ftesurf', 'refuse'),
        'repeated nested key': (nested, 'ftesurf', 'refuse'),
        'escaped duplicate key': (escaped, 'ftesurf', 'refuse'),
        'dotted key': (dotted, 'ftesurf', 'refuse'),
        'object over the key cap': (capped, 'ftesurf', 'refuse'),
    }
    out = {name: (('validate', ident, payload), want) for name, (payload, ident, want) in feeds.items()}
    out.update({
        'legacy path under new': (('managed', 'worldofsurf', 'ftesurf/menu.dat'), 'refused'),
        'new path under legacy': (('managed', 'ftesurf', 'worldofsurf/menu.dat'), 'refused'),
        'new path of 239': (('managed', 'worldofsurf', 'worldofsurf/gfx/' + 'a' * 219 + '.png'), 'managed'),
        'new path of 240': (('managed', 'worldofsurf', 'worldofsurf/gfx/' + 'a' * 220 + '.png'), 'refused'),
        'legacy path of 236 stays managed': (('managed', 'ftesurf', 'ftesurf/gfx/' + 'a' * 220 + '.png'), 'managed'),
        'new zone file': (('managed', 'worldofsurf', 'worldofsurf/maps/zones/local/a.json'), 'managed'),
        'new player cfg refused': (('managed', 'worldofsurf', 'worldofsurf/cfg/settings.cfg'), 'refused'),
        'shared root file': (('managed', 'worldofsurf', 'default.fmf'), 'managed'),
        'file at the gamedir name refused': (('managed', 'worldofsurf', 'worldofsurf'), 'refused'),
        'select ftesurf': (('select', 'ftesurf'), 'ftesurf'),
        'select worldofsurf': (('select', 'worldofsurf'), 'worldofsurf'),
        'select other': (('select', 'quakers'), 'none'),
        'select nothing': (('select',), 'none'),
        'basegame matched exactly': (('select', 'WorldofSurf'), 'none'),
        'last basegame wins': (('select', 'ftesurf', 'worldofsurf'), 'worldofsurf'),
        'special basegame skipped': (('select', 'worldofsurf', '*ftesurf'), 'worldofsurf'),
        'full gamedir': (('identity', str(roots['full']), 'ftesurf'), 'ftesurf'),
        'no gamedir': (('identity', str(roots['empty']), 'worldofsurf'), 'none'),
        'stray folder': (('identity', str(roots['stray']), 'worldofsurf'), 'none'),
        'one required file': (('identity', str(roots['one']), 'worldofsurf'), 'none'),
        'mid-apply, two files': (('identity', str(roots['two']), 'worldofsurf'), 'worldofsurf'),
        'linked gamedir': (('identity', str(roots['linked']), 'worldofsurf'), 'none'),
        'required file is a directory': (('identity', str(roots['dirfile']), 'worldofsurf'), 'none'),
        'basegame names a different gamedir': (('identity', str(roots['full']), 'worldofsurf'), 'none'),
        'marker agrees': (('identity', str(roots['agrees']), 'ftesurf'), 'ftesurf'),
        'marker disagrees': (('identity', str(roots['disagrees']), 'ftesurf'), 'none'),
        'marker over files': (('identity', str(roots['overfiles']), 'worldofsurf'), 'worldofsurf'),
        'marker over half-moved': (('identity', str(roots['halfmoved']), 'worldofsurf'), 'worldofsurf'),
        'half-moved without marker': (('identity', str(roots['halfnomarker']), 'worldofsurf'), 'none'),
        'marker without newline': (('identity', str(roots['bare']), 'worldofsurf'), 'worldofsurf'),
        'marker in other case': (('identity', str(roots['case']), 'worldofsurf'), 'none'),
        'marker with CRLF': (('identity', str(roots['crlf']), 'worldofsurf'), 'none'),
        'marker with two newlines': (('identity', str(roots['twonl']), 'worldofsurf'), 'none'),
        'marker is a directory': (('identity', str(roots['dirmarker']), 'worldofsurf'), 'none'),
        'linked marker': (('identity', str(roots['linkmarker']), 'worldofsurf'), 'none'),
        'legacy feed url': (('feed', 'ftesurf'), 'https://proto.bar/ftesurf/update.json'),
        'worldofsurf feed url': (('feed', 'worldofsurf'), 'https://proto.bar/ftesurf/worldofsurf.json'),
    })
    return out


def build(work, name, code, platform, cc, env):
    """The subject slice is held to -Werror; json.c and hash.c are engine code, built as they are."""
    c = work / f'{name}-{platform}.c'
    binary = work / f'{name}-{platform}.exe'
    c.write_text('#include "quakedef.h"\n#include "hash.h"\n' + PRE + code + DRIVER)
    include = ['-I', str(work), '-I', str(ENGINE / 'qclib')]
    for obj, src in (('json.o', work / 'json.c'), ('hash.o', ENGINE / 'qclib/hash.c')):
        if not (work / obj).exists():
            subprocess.run([cc, '-std=gnu99', '-w', '-DFTEPLUGIN', *include, '-c', str(src), '-o', str(work / obj)],
                           check=True, env=env)  # FTEPLUGIN: json.c's own utf8_encode
    subprocess.run([cc, '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', f'-DQKU_PLATFORM="{platform}"',
                    *include, str(c), str(work / 'json.o'), str(work / 'hash.o'), '-o', str(binary)], check=True, env=env)
    return binary


def run(binary, table, work, env):
    got = {}
    for name, (argv, _) in table.items():
        if argv[0] == 'validate':
            payload = work / 'payload.json'
            payload.write_text(argv[2] if isinstance(argv[2], str) else json.dumps(argv[2]))
            argv = (argv[0], argv[1], str(payload))
        r = subprocess.run([str(binary), *argv], capture_output=True, text=True, env=env)
        assert r.returncode == 0, (name, r.returncode, r.stderr)
        got[name] = r.stdout.strip()
    return got


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work-root', type=Path, required=True)
    ap.add_argument('--cc', default='gcc')
    a = ap.parse_args()
    a.work_root.mkdir(parents=True, exist_ok=False)
    (a.work_root / 'OWNER.md').write_text('Owned P1593 identity-policy compile/mutation fixtures. No game processes or player data.\n')
    env = dict(os.environ)
    if Path(a.cc).is_absolute():
        env['PATH'] = str(Path(a.cc).parent) + os.pathsep + env.get('PATH', '')
    source = (ENGINE / 'common/qkupdate.c').read_text().replace('\r\n', '\n')
    policy = source[source.index('static qboolean QKU_ValidHash('):source.index('//`path` comes straight out')] + \
        source[source.index('static qboolean QKU_ValidPath('):source.index('static qboolean QKU_FSNoLinks(')] + \
        source[source.index('static qboolean QKU_FSNoLinks('):source.index('//FTESurf Patch 1209: the launcher is WOS-Launcher.')] + \
        source[source.index('static int QKU_FSRegular('):source.index('// Missing content can be repaired;')]
    common = (ENGINE / 'common/common.h').read_text().replace('\r\n', '\n')
    json_block = common[common.index('//json.c\n'):common.index("//there's no write logic")]
    (a.work_root / 'quakedef.h').write_text(STUB + json_block)
    (a.work_root / 'json.c').write_text((ENGINE / 'common/json.c').read_text())
    roots = fixtures(a.work_root)
    failures = 0
    for platform in ('win64', 'linux64'):
        table = cases(platform, roots)
        got = run(build(a.work_root, 'subject', policy, platform, a.cc, env), table, a.work_root, env)
        wrong = {n: got[n] for n, (_, want) in table.items() if got[n] != want}
        print(f'subject {platform}: {len(table) - len(wrong)}/{len(table)} as expected', wrong or '')
        failures += bool(wrong)
        for name, (needle, replacement, expected) in MUTANTS.items():
            assert policy.count(needle) == 1, name
            mutant = run(build(a.work_root, name, policy.replace(needle, replacement), platform, a.cc, env),
                         table, a.work_root, env)
            flipped = {n for n, (_, want) in table.items() if mutant[n] != want}
            ok = flipped == expected
            print(f'  mutant {name}: lets through {sorted(flipped)}', '' if ok else f'EXPECTED {sorted(expected)}')
            failures += not ok
    print('PASS' if not failures else f'FAIL ({failures})')
    raise SystemExit(1 if failures else 0)


if __name__ == '__main__':
    main()
