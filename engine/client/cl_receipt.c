/*
FTESurf Patch 417 -- THE RUN RECEIPT: a player key, and a signature over the
evidence this client produced.

WHAT IT IS.  At first use the client makes an Ed25519 keypair and keeps the seed
in `fskey`, beside `qkey`, in the install root.  At the end of a run the
gamecode asks this file to sign a short, fixed statement -- the server this
client is connected to, the server's run nonce, the run's tick count, and the
SHA-256 of this client's two evidence files -- and the signature and public key
go to the server, which writes them beside the recording.

WHAT IT BUYS, IN THE WORDS THE REVIEW LEFT STANDING.  The first version of this
paragraph claimed three things and two of them were wrong in the direction that
flattered the patch.  What is true:

  IT DOES NOT PROVE HONESTY.  A patched client signs whatever it likes with its
  own key, and the player holds that key -- so a player who wants to sign a
  false statement about their own run does not even have to patch anything.
  Nothing here reaches the threat classes that patch a client or replay
  hardware, and nothing here may be read as if it did.

  IT FIXES THE DIGESTS AT THE FINISH.  The gamecode signs when the run ends and
  the server refuses a receipt while the run is still recording, so the
  commitment cannot be made before the evidence exists.  What that costs a
  forger is that the fake journal has to be ready by the finish line rather than
  produced at leisure afterwards.  It is a real cost and it is not a wall.

  IT IS AN IDENTITY THAT CANNOT BE HARVESTED BY ASKING, AND A SIGNATURE THAT IS
  NOT PORTABLE.  The first cut claimed simply "an identity a server operator
  cannot wear", and that was FALSE as written: `rec_sign` was reachable from a
  server's stufftext, so any server could mint the key file on a client that had
  never played a ranked run, read back its public key, and -- by relaying a
  nonce it took from a real server -- have a victim's client sign a statement
  the attacker then posted as their own run's receipt.  Two things close that: a
  command from a server is refused outright (see Cmd_FromGamecode below), and
  the statement names THE SERVER THIS CLIENT IS TALKING TO, taken from the
  netchan, so a signature made on one server does not read as a receipt on
  another.

  WHAT IT STILL DOES NOT BUY.  A key is a file: it can be copied, shared,
  deleted and re-minted, so it says "somebody holding this file" and never
  "this person".  A hostile server also owns the client's CSQC, so it can still
  drive this command from gamecode it wrote -- the `server` line is what stops
  that being worth anything off its own server.

WHY THE ENGINE AND NOT THE GAMECODE.  QuakeC has no bignums, no bytes and no
file hashing; and the two things being hashed are an engine buffer (the input
journal, whose digest is taken as it closes -- see IN_Journal_LastDigest) and a
file on disk.  The primitive is common/ed25519.c, checked against RFC 8032's
published vectors in `ed25519_selftest` and cross-checked against
tools/ed25519.py on random vectors and on the three classes a random vector
cannot reach: a small-order key, a non-canonical encoding, and an unreduced s.
*/

#include "quakedef.h"
#ifdef WEBCLIENT
#include "../http/iweb.h"
#endif
#ifdef _WIN32
#include "winquake.h"
#include <shellapi.h>
#else
#include <unistd.h>
#endif

#ifndef SERVERONLY

/*QC_FixFileName lives in common/pr_bgcmd.c and no header declares it; the local
  prototype is in_generic.c's, for the same reason and with the same sandbox.*/
qboolean QC_FixFileName(const char *name, const char **result, const char **fallbackread);
/*Patch 417: the journal's digest, taken as it closed (in_generic.c).*/
qboolean IN_Journal_LastDigest(qbyte digest[32], size_t *len, qboolean *kept);

#define RCPT_KEYFILE	"fskey"
#define RCPT_VERSION	"FTESURF-RCPT 1"
/*Patch 625: an account proof's statement, and the one page the engine will open.*/
#define ACCT_VERSION	"FTESURF-ACCT 1"
#define ACCT_SITE		"https://proto.bar/ftesurf/board/link"
#define ACCT_OPENERLEN	24

static qboolean rcpt_have;
static qbyte rcpt_pk[32], rcpt_sk[64];

/*
  THE KEY FILE, on qkey's precedent (cl_main.c): read it, and make one when
  there is none.  Hex on one line rather than 32 raw bytes, because this is a
  file a player may have to back up, copy to a second machine, or be asked to
  send a fingerprint of -- and none of that is comfortable with a file that
  looks like a corrupt download.

  Sys_RandomBytes IS REQUIRED HERE, unlike qkey, which falls back to rand() when
  the platform has no CSPRNG.  qkey is a pseudonym; this is a signing key, and a
  signing key drawn from a clock-seeded PRNG is one an attacker can search for.
  No entropy, no key, and the receipt is simply not produced -- which is a state
  the whole design already handles, because every client older than this patch
  is in it.
*/
static qboolean CL_Receipt_Key(void)
{
	vfsfile_t *f;
	char line[80];
	qbyte seed[32];
	int len;

	if (rcpt_have)
		return true;

	f = FS_OpenVFS(RCPT_KEYFILE, "rb", FS_ROOT);
	if (f)
	{
		len = VFS_READ(f, line, sizeof(line)-1);
		VFS_CLOSE(f);
		if (len < 64)
			len = 0;
		else
		{
			line[64] = 0;
			if (Base16_DecodeBlock(line, seed, sizeof(seed)) != sizeof(seed))
				len = 0;
		}
		if (len)
		{
			Ed25519_FromSeed(rcpt_pk, rcpt_sk, seed);
			rcpt_have = true;
			return true;
		}
		Con_Printf(CON_WARNING"%s is not a key file; leaving it alone and signing nothing\n", RCPT_KEYFILE);
		return false;
	}

	/*
	  EXISTS BUT WOULD NOT OPEN IS NOT THE SAME AS ABSENT, and treating them
	  alike would MINT A NEW KEY OVER THE OLD ONE -- a player's identity
	  destroyed by a backup tool holding the file open for a second.  There is
	  no errno here to read, so the existence question is asked separately.
	*/
	if (COM_FCheckExists(RCPT_KEYFILE))
	{
		Con_Printf(CON_WARNING"%s exists but cannot be read; signing nothing rather than "
				   "replacing it\n", RCPT_KEYFILE);
		return false;
	}

	if (!Sys_RandomBytes(seed, sizeof(seed)))
	{
		Con_DPrintf("rec_sign: no system entropy, so no signing key\n");
		return false;
	}

	Base16_EncodeBlock((const char*)seed, sizeof(seed), (qbyte*)line, sizeof(line));
	line[64] = '\n';
	f = FS_OpenVFS(RCPT_KEYFILE, "wb", FS_ROOT);
	if (!f)
	{
		Con_Printf(CON_WARNING"rec_sign: cannot write %s\n", RCPT_KEYFILE);
		return false;
	}
	/*
	  THE WRITE IS CHECKED.  A short write -- a full disk -- used to leave a
	  truncated file while this session went on signing under a key whose seed
	  no longer existed anywhere: receipts live on servers, naming a public key
	  the player could never use again.
	*/
	len = VFS_WRITE(f, line, 65);
	VFS_CLOSE(f);
	if (len != 65)
	{
		Con_Printf(CON_WARNING"rec_sign: %s was not written in full (%i of 65 bytes); "
				   "signing nothing\n", RCPT_KEYFILE, len);
		return false;
	}

	Ed25519_FromSeed(rcpt_pk, rcpt_sk, seed);
	rcpt_have = true;
	Con_Printf("A signing key for your runs has been created in %s. "
			   "Back it up: it is your identity on the board, and nothing else can replace it.\n",
			   RCPT_KEYFILE);
	return true;
}

static void CL_Receipt_PubKey_f(void)
{
	char hex[80];
	if (!CL_Receipt_Key())
	{
		Con_Printf("no signing key\n");
		return;
	}
	Base16_EncodeBlock((const char*)rcpt_pk, sizeof(rcpt_pk), (qbyte*)hex, sizeof(hex));
	hex[64] = 0;
	Con_Printf("run signing key: %s\n", hex);
}

/*
  The .view file's digest.  The path comes from the gamecode, so it is put
  through the same sandbox in_journal_end uses AND held to the same `data/`
  prefix -- with one extra condition, because this one returns a HASH OF
  ARBITRARY BYTES to a server if it is allowed to: it must be a `.view`.  A
  server that could name any file would have an oracle for "does this file on
  your disk hash to X", which is a question no server gets to ask.
*/
static qboolean CL_Receipt_ViewDigest(const char *path, qbyte digest[32])
{
	const char *name, *fallback;
	size_t len, l;
	qbyte *data;

	if (!path || !*path || !strcmp(path, "-"))
		return false;
	if (!QC_FixFileName(path, &name, &fallback) || strncmp(name, "data/", 5))
		return false;
	l = strlen(name);
	if (l < 6 || strcmp(name + l - 5, ".view"))
		return false;

	data = FS_LoadMallocFile(name, &len);
	if (!data)
		return false;
	CalcHash(&hash_sha2_256, digest, 32, data, len);
	BZ_Free(data);
	return true;
}

static void CL_Receipt_Hex(char *out, size_t outsize, const qbyte *b, size_t n, qboolean have)
{
	if (!have)
	{
		Q_strncpyz(out, "-", outsize);
		return;
	}
	Base16_EncodeBlock((const char*)b, n, (qbyte*)out, outsize);
	out[n*2] = 0;
}

/*
  rec_sign <nonce> <ticks> <viewpath|-> <journalled 0|1>

  THE SIGNED STATEMENT IS FIXED AND SHORT, and the gamecode chooses none of its
  shape: five lines under a version, each a key and a value, newline-terminated.
  What the caller supplies is the nonce (which the server issued and this client
  echoed), the tick count it saw, where its .view landed, and whether this run
  took a journal at all.  The server address and both digests are measured here.

  THE RUN ID IS DELIBERATELY NOT IN IT.  The client does not know the server's
  runid, and a statement signed over a value the signer cannot see is a statement
  about nothing.  The nonce is the per-run unique the client CAN see, and it is
  the server's own, which is what makes it a binding.
*/
static void CL_Receipt_Sign_f(void)
{
	char msg[512], hidhex[80], viewhex[80], pubhex[80], sighex[160], svname[128];
	qbyte hid[32], view[32], sig[64];
	qboolean havehid, haveview, kept = false;
	size_t hidlen = 0;
	const char *nonce;
	int ticks, i;

	if (Cmd_Argc() < 4)
	{
		Con_Printf("rec_sign <nonce> <ticks> <viewpath|-> <journalled 0|1> -- signs this run's evidence\n");
		return;
	}

	/*
	  NOT FROM A SERVER.  THIS IS THE FIX FOR THE WORST THING THE REVIEW FOUND.

	  Every command CSQC can reach through localcmd is also reachable from a
	  server's stufftext, at a HIGHER restriction level -- which made this a
	  signing oracle any server could drive: one stuffed line and it learns the
	  player's public key, mints the key file on a client that has never played
	  a ranked run, and gets a signature over a statement of its choosing.
	  Worse, it makes a live RELAY work: the attacker takes a nonce from a real
	  server, has a victim's client sign it on the attacker's own server, and
	  posts the victim's key and signature as the receipt for the attacker's
	  run.  Refusing anything at RESTRICT_SERVER closes all of it -- the
	  gamecode's localcmd runs at RESTRICT_INSECURE, one level below, and a
	  console still works.

	  A hostile server can still ship a csprogs that calls this: CSQC comes from
	  the server.  What it cannot do is make the SIGNATURE portable -- see the
	  `server` line below.
	*/
	if (Cmd_FromGamecode())
	{
		/*PRINTED, not dprinted: a server asking for a signature is something the
		  player should be able to see happening, and it is also the only
		  positive evidence a harness has that the refusal fired rather than
		  the command never arriving.  A server can already print to this
		  console at will, so it costs no new abuse.*/
		Con_Printf("rec_sign: a server asked this client to sign a run receipt. Refused.\n");
		return;
	}

	if (cls.state < ca_connected)
	{
		Con_DPrintf("rec_sign: not connected\n");
		return;
	}
	if (!CL_Receipt_Key())
	{
		Con_DPrintf("rec_sign: no key\n");
		return;
	}

	/*32 LOWERCASE HEX, CHECKED HERE TOO.  The gamecode checks it before it
	  stuffs it back (cl_replay.qc), but the gamecode is the server's code and
	  this is the engine's signature.*/
	nonce = Cmd_Argv(1);
	if (strlen(nonce) != 32)
	{
		Con_DPrintf("rec_sign: nonce is %u characters, not 32\n", (unsigned)strlen(nonce));
		return;
	}
	for (i = 0; i < 32; i++)
	{
		char c = nonce[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
		{
			Con_DPrintf("rec_sign: nonce is not lowercase hex\n");
			return;
		}
	}
	ticks = atoi(Cmd_Argv(2));

	/*
	  Patch 625: A RUN, AND NOTHING ELSE.  Below -1 was the account link's
	  (Patches 615-619: connect, ask, confirm, start), signed here because game
	  code had no other way to ask.  It has one now (CL_Receipt_AcctSign), and
	  this is a console command: text can be put in front of it.  Printed, so a
	  harness can see the refusal fire.
	*/
	if (ticks < -1)
	{
		Con_Printf("rec_sign: account proofs are not signed from the console. Refused.\n");
		return;
	}

	/*
	  THE JOURNAL DIGEST IS ONLY THIS RUN'S IF THIS RUN TOOK A JOURNAL, and the
	  gamecode is the only thing that knows.  A run played with `rec_hid 0`, or
	  a Multi-Session resume (which opens no sidecars at all), begins no journal
	  -- and the digest latched by the LAST run that did would otherwise be
	  signed as this one's.  That is the attack the review spelled out: play one
	  clean PB so a real journal is latched, then turn journalling off and sign
	  its digest onto every run after it.
	*/
	havehid = atoi(Cmd_Argv(4)) ? IN_Journal_LastDigest(hid, &hidlen, &kept) : false;
	haveview = CL_Receipt_ViewDigest(Cmd_Argv(3), view);

	CL_Receipt_Hex(hidhex, sizeof(hidhex), hid, 32, havehid);
	CL_Receipt_Hex(viewhex, sizeof(viewhex), view, 32, haveview);

	/*
	  THE SERVER THIS CLIENT IS ACTUALLY TALKING TO, from the netchan and not
	  from anything the gamecode said.  It is what makes a signature
	  NON-PORTABLE: the relay above ends with a receipt on server B carrying a
	  statement that names server A, which a reader compares and rejects.  The
	  client's own view of the address is the right one to sign -- the server's
	  view of itself is what a liar would supply.
	*/
	NET_AdrToString(svname, sizeof(svname), &cls.netchan.remote_address);

	/*SEVEN specifiers and SEVEN arguments.  The first cut had six and five and
	  crashed the client the moment a run ended -- Q_snprintfz carries no format
	  attribute, so nothing warned; the harness found it as a process that
	  stopped writing its log mid-command.*/
	Q_snprintfz(msg, sizeof(msg),
				RCPT_VERSION "\n"
				"server %s\n"
				"nonce %s\n"
				"ticks %i\n"
				"hid %s %u %i\n"
				"view %s\n",
				svname, nonce, ticks, hidhex, (unsigned)hidlen, kept ? 1 : 0, viewhex);

	Ed25519_Sign(sig, (const qbyte*)msg, strlen(msg), rcpt_sk);

	Base16_EncodeBlock((const char*)rcpt_pk, sizeof(rcpt_pk), (qbyte*)pubhex, sizeof(pubhex));
	pubhex[64] = 0;
	Base16_EncodeBlock((const char*)sig, sizeof(sig), (qbyte*)sighex, sizeof(sighex));
	sighex[128] = 0;

	/*
	  THE ADDRESS IS QUOTED AND THE OTHER SEVEN FIELDS ARE NOT, and the reason is
	  measured rather than defensive: the gamecode reads these with QuakeC's
	  tokenize(), which treats a colon as its own token -- so an unquoted
	  "QLoopBack:0" arrived as "QLoopBack", the server wrote that into the
	  receipt, and the signature over the full address then verified against
	  nothing.  Every other field here is hex or digits and cannot contain one.
	*/
	CL_SendClientCommand(true, "rec_rcpt %s %i %s %u %i %s %s \"%s\"",
						 pubhex, ticks, hidhex, (unsigned)hidlen, kept ? 1 : 0,
						 viewhex, sighex, svname);
	Con_DPrintf("rec_sign: %s on %s hid %s view %s\n", nonce, svname, hidhex, viewhex);
}

/*
  FTESurf Patch 625 -- THE ACCOUNT PROOF, as a function game code calls.

  WHY IT LEFT THE CONSOLE.  `rec_sign` refuses a server's stufftext, and game
  code's localcmd runs one level below that, so it obeyed game code -- and
  game code builds console lines out of text in some fifty places.  Patch 617
  found two of them pasting a typed argument in whole; nobody has read the
  rest.  So whoever can write to a player's connection might find a line that
  signs a link with no key pressed.  A builtin has no text in front of it.

  A STATEMENT OF ITS OWN, so that a reader can tell which engine signed.  An
  engine older than this one can only ever sign RCPT_VERSION; this function
  signs ACCT_VERSION and nothing else does, so "this link was made by an
  engine that only signs links under a key press" is something surfd can
  check rather than hope.

      FTESURF-ACCT 1
      server <the address this client is talking to, from the netchan>
      nonce  <32 lowercase hex: the lobby's, or a digest game code made>
      kind   <-2 connect | -3 ask | -4 confirm | -5 start>
      key    <1 if signed inside a device's key press, else 0>

  WHICH KINDS NEED THE KEY.  -3, -4 and -5 are how a link is asked for,
  confirmed and started: refused unless `keyed`, which pr_csqc.c sets only
  while CSQC_InputEvent runs for a key a device pressed.  -2 is the connect's
  "which account am I", asked for by a timer, and says so with `key 0`.

  WHAT GOES BACK is `<pub> <key> <sig> "<server>"`, for game code to send as
  it stands (the address is quoted for the same reason rec_rcpt's is).  Game
  code may see its own public key and signature: a server already does.
*/
/*
  THE SERVER NAMED IS THE ONE THE GAME CODE CAME FROM.  The statement names the
  netchan's address, and a handshake packet can re-point the netchan while the
  previous server's game code is still the code running (the second review:
  `cl_transfer`, then one connectionless `accept` from the new address, which
  that handler takes at ca_active without a disconnect).  So the address is
  noted when the SERVERDATA arrives (the VM is made some frames later, from
  what that serverdata announced), with whether that was a live connection:
  a VM made in demo or QTV playback, or with no connection, never signs, even
  if it is still loaded when a real connection comes up.  A VM takes the note
  with it and is compared when it asks.
*/
static netadr_t acct_vm_adr, acct_sd_adr;
static qboolean acct_vm_live, acct_sd_live;
void CL_Receipt_ServerData(void)
{
	acct_sd_live = cls.state >= ca_connected && !cls.demoplayback;
	acct_sd_adr = cls.netchan.remote_address;
}
/*
  AND THE STATEMENT SAYS WHICH GAME CODE ASKED.  "A key was pressed" is consent
  to whatever drew the box, and the client picks the csprogs it runs out of a
  download cache every server it has visited can write, by a 32-bit checksum
  (CSQC_FindMainProgs; its own comment calls the checksum broken).  So the
  code on a lobby is not known to be the lobby's.  The SHA-256 of the file the
  VM was made from goes into the signed statement, and surfd, which can read
  what its lobbies serve, takes no other.  A VM that loaded any second file of
  code (an addon, addprogs) has no one name and signs nothing.
*/
static char acct_vm_code[65];
static qboolean acct_vm_mixed;
void CL_Receipt_VM(qboolean made)
{
	acct_vm_live = made && acct_sd_live && cls.state >= ca_connected && !cls.demoplayback
		&& NET_CompareAdr(&acct_sd_adr, &cls.netchan.remote_address);
	acct_vm_code[0] = 0;
	acct_vm_mixed = false;
	if (acct_vm_live)
		acct_vm_adr = acct_sd_adr;
}
/*the main file's bytes as the VM was handed them; NULL for any other code*/
void CL_Receipt_VMCode(const void *data, size_t len)
{
	qbyte digest[32];
	char hex[65];

	if (!data)
	{
		acct_vm_mixed = true;
		return;
	}
	CalcHash(&hash_sha2_256, digest, sizeof(digest), data, len);
	CL_Receipt_Hex(hex, sizeof(hex), digest, sizeof(digest), true);
	if (acct_vm_code[0] && strcmp(acct_vm_code, hex))
		acct_vm_mixed = true;	/*a second main file, and not the same one*/
	Q_strncpyz(acct_vm_code, hex, sizeof(acct_vm_code));
}

qboolean CL_Receipt_AcctSign(const char *nonce, int kind, qboolean keyed, char *out, size_t outlen)
{
	char msg[384], pubhex[80], sighex[160], svname[128];
	qbyte sig[64];
	int i;

	if (kind < -6 || kind > -2)
		return false;
	if (kind != -2 && !keyed)
	{
		/*PRINTED: it is the only evidence a harness has that the refusal fired.*/
		Con_Printf("acct_sign: kind %i is signed only under a key press. Refused.\n", kind);
		return false;
	}
	/*
	  NOT WHILE A DEMO OR A QTV STREAM IS PLAYING.  Playback is "connected", runs
	  the game code that was recorded with it, and its netchan's address is
	  whoever last sent this client a packet: the review walked a recorded
	  csprogs to a signature naming a server the client was not talking to.
	*/
	if (cls.state < ca_connected || cls.demoplayback)
		return false;
	if (!acct_vm_live || !NET_CompareAdr(&acct_vm_adr, &cls.netchan.remote_address))
	{
		Con_Printf("acct_sign: this game code is not the connected server's. Refused.\n");
		return false;
	}
	if (strlen(acct_vm_code) != 64 || acct_vm_mixed)
	{
		Con_Printf("acct_sign: this game code is not one file with a name. Refused.\n");
		return false;
	}
	if (strlen(nonce) != 32)
		return false;
	for (i = 0; i < 32; i++)
	{
		char c = nonce[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	}
	if (!CL_Receipt_Key())	/*after the checks: a refused call does not mint the key file*/
		return false;

	// Zone drafts use the existing linked-key proof, with a distinct purpose
	// and fixed HTTPS service instead of the local editor's listen address.
	if (kind == -6)
		Q_strncpyz(svname, "play.proto.bar:443", sizeof(svname));
	else
		NET_AdrToString(svname, sizeof(svname), &cls.netchan.remote_address);
	/*what comes back is spliced into a console line by game code: an address
	  is digits, dots, colons and brackets, and anything else is not sent.*/
	for (i = 0; svname[i]; i++)
	{
		char c = svname[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| c == '.' || c == ':' || c == '[' || c == ']' || c == '-' || c == '%'))
			return false;
	}

	Q_snprintfz(msg, sizeof(msg),
				ACCT_VERSION "\n"
				"server %s\n"
				"nonce %s\n"
				"kind %i\n"
				"key %i\n"
				"code %s\n",
				svname, nonce, kind, keyed ? 1 : 0, acct_vm_code);
	Ed25519_Sign(sig, (const qbyte*)msg, strlen(msg), rcpt_sk);

	Base16_EncodeBlock((const char*)rcpt_pk, sizeof(rcpt_pk), (qbyte*)pubhex, sizeof(pubhex));
	pubhex[64] = 0;
	Base16_EncodeBlock((const char*)sig, sizeof(sig), (qbyte*)sighex, sizeof(sighex));
	sighex[128] = 0;
	Q_snprintfz(out, outlen, "%s %i %s \"%s\" %s", pubhex, keyed ? 1 : 0, sighex, svname,
				acct_vm_code);
	Con_DPrintf("acct_sign: kind %i key %i on %s\n", kind, keyed ? 1 : 0, svname);
	return true;
}

/* Fixed Friends/menu operations: never account/link consent, receipts or lobby
   capabilities. Message targets include the exact text SHA256. MENU VM only. */
qboolean CL_FriendsSign(const char *nonce, const char *op, const char *who, char *out, size_t outlen)
{
	char msg[256], pubhex[65], sighex[129];
	qbyte sig[64];
	const char *p;

	if (outlen) out[0] = 0;
	if (cls.demoplayback)
		return false;
	if (!nonce || strlen(nonce) != 32 || !op || !who)
		return false;
	/*P1009: the two reads in any game -- their answer goes to the menu, never to
	  the server.  An op that acts only disconnected, or in a loopback game whose
	  server is this process: a remote server can drive the menu (stufftext
	  ui_key/ui_click), so it must not get a signature that posts or joins.*/
	if (strcmp(op, "list") && strcmp(op, "chat"))
	if (cls.state != ca_disconnected || CL_TryingToConnect())
	if (cls.state < ca_connected || cls.netchan.remote_address.type != NA_LOOPBACK)
		return false;
	for (p = nonce; *p; p++)
		if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
			return false;
	if (!strcmp(op, "list") || !strcmp(op, "presence") || !strcmp(op, "chat"))
	{
		if (strcmp(who, "-")) return false;
	}
	else if (!strcmp(op, "join"))
	{
		if (strlen(who) != 8) return false;
		for (p = who; *p; p++)
			if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
				return false;
	}
	else if (!strcmp(op, "message"))
	{
		const char *colon = strchr(who, ':');
		if (!colon || strlen(colon + 1) != 64)
			return false;
		if (!(colon == who + 1 && who[0] == '-'))
		{
			if (colon != who + 8) return false;
			for (p = who; p < colon; p++)
				if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
					return false;
		}
		for (p = colon + 1; *p; p++)
			if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
				return false;
	}
	else
		return false;
	if (!CL_Receipt_Key())
		return false;
	Q_snprintfz(msg, sizeof(msg), "FTESURF-FRIENDS 1\nserver play.proto.bar:443\nnonce %s\nop %s\nwho %s\n", nonce, op, who);
	Ed25519_Sign(sig, (const qbyte *)msg, strlen(msg), rcpt_sk);
	CL_Receipt_Hex(pubhex, sizeof(pubhex), rcpt_pk, sizeof(rcpt_pk), true);
	CL_Receipt_Hex(sighex, sizeof(sighex), sig, sizeof(sig), true);
	Q_snprintfz(out, outlen, "%s %s", pubhex, sighex);
	return true;
}

static qboolean CL_Receipt_IsHex(const char *s, size_t len)
{
	size_t i;
	if (!s || strlen(s) != len)
		return false;
	for (i = 0; i < len; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
			return false;
	return true;
}

/*
  P1090: a TAS project revision's author statement (TAS plan 5.2).  sha is the revision's
  P1089 hash, which covers its parent, map and inputs, so the statement checks offline;
  ticks is the input span this revision supplies over its parent.  Audience surfd, like
  FRIENDS.  Only in a game this process hosts (projects are local) and only under a key
  press, so a remote server's CSQC cannot collect one behind some other prompt.  The VM
  checks are acct_sign's: the code must be this connection's, one file, and its sha256 is
  the `code` line, so a cached or packed csprogs is named in what it asked for.
*/
#define TASREV_MAXTICK	16777215	/*the last tick a QC float holds exactly*/
qboolean CL_Receipt_TasRevSign(const char *sha, const char *parent, const char *mapcrc,
							   int from, int to, qboolean keyed, char *out, size_t outlen)
{
	char msg[400], pubhex[65], sighex[129];
	qbyte sig[64];

	if (outlen) out[0] = 0;
	if (!keyed)
	{
		Con_Printf("tasrev_sign: signed only under a key press. Refused.\n");
		return false;
	}
	if (cls.demoplayback || cls.state < ca_connected || cls.netchan.remote_address.type != NA_LOOPBACK)
	{
		Con_Printf("tasrev_sign: only in a game this client hosts. Refused.\n");
		return false;
	}
	if (!acct_vm_live || !NET_CompareAdr(&acct_vm_adr, &cls.netchan.remote_address))
	{
		Con_Printf("tasrev_sign: this game code is not the connected server's. Refused.\n");
		return false;
	}
	if (strlen(acct_vm_code) != 64 || acct_vm_mixed)
	{
		Con_Printf("tasrev_sign: this game code is not one file with a name. Refused.\n");
		return false;
	}
#ifndef CLIENTONLY
	{
		/*P1090 reviews 2-3: a hosted game still takes its CSQC from the download cache when one
		  matches the served file's md4 and size (CSQC_FindMainProgs), and any server can fill
		  that cache: the VM must be the very bytes this game served, as hashed when the map
		  spawned (sv_init.c), not whatever the CSQC-settable sv_csqc_progname names now.*/
		extern char sv_csqc_sha256[65];
		if (!sv.state || strlen(sv_csqc_sha256) != 64 || strcmp(sv_csqc_sha256, acct_vm_code))
		{
			Con_Printf("tasrev_sign: this game code is not the file this game serves. Refused.\n");
			return false;
		}
	}
#endif
	if (!CL_Receipt_IsHex(sha, 64) || !CL_Receipt_IsHex(mapcrc, 8)
		|| !parent || (strcmp(parent, "-") && !CL_Receipt_IsHex(parent, 64)))
		return false;
	if (from < 1 || to < from || to > TASREV_MAXTICK)
		return false;
	if (!CL_Receipt_Key())
		return false;
	Q_snprintfz(msg, sizeof(msg), "FTESURF-TASREV 1\nserver play.proto.bar:443\nsha %s\nparent %s\nmapcrc %s\nticks %i %i\ncode %s\n",
				sha, parent, mapcrc, from, to, acct_vm_code);
	Ed25519_Sign(sig, (const qbyte *)msg, strlen(msg), rcpt_sk);
	CL_Receipt_Hex(pubhex, sizeof(pubhex), rcpt_pk, sizeof(rcpt_pk), true);
	CL_Receipt_Hex(sighex, sizeof(sighex), sig, sizeof(sig), true);
	Q_snprintfz(out, outlen, "%s %s %s", pubhex, sighex, acct_vm_code);
	Con_DPrintf("tasrev_sign: %.8s ticks %i..%i\n", sha, from, to);
	return true;
}

/*
  Patch 625: THE SIGN-IN PAGE, IN THE PLAYER'S BROWSER.  Game code cannot name
  an address: it supplies the opener it made for a link (24 lowercase hex) and
  the address is ACCT_SITE with that after it.  So the worst a server's game
  code can do with this is open the real sign-in page, and pr_csqc.c lets it
  only while a device's key press is being delivered.
*/
qboolean CL_Receipt_Browse(const char *opener)
{
	static double last;
	char url[sizeof(ACCT_SITE) + 8 + ACCT_OPENERLEN];
	int i;

	if (cls.state < ca_connected || cls.demoplayback)
		return false;
	/*one a second at most, whatever is pressing keys*/
	if (last && realtime - last < 1.0 && realtime >= last)
		return false;
	if (strlen(opener) != ACCT_OPENERLEN)
		return false;
	for (i = 0; i < ACCT_OPENERLEN; i++)
	{
		char c = opener[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	}
	Q_snprintfz(url, sizeof(url), ACCT_SITE "?k=%s", opener);
	last = realtime;
	/*A harness has no use for a browser on the desk it shares, and needs to see
	  what would have been opened.  A switch, not a cvar: a server sets cvars.*/
	if (COM_CheckParm("-nobrowse"))
	{
		Con_Printf("acct_browse: %s (-nobrowse: not opened)\n", url);
		return true;
	}
#ifdef _WIN32
	{
		wchar_t wide[sizeof(url)];
		for (i = 0; url[i]; i++)
			wide[i] = (unsigned char)url[i];	/*ASCII by construction*/
		wide[i] = 0;
		return (INT_PTR)ShellExecuteW(NULL, L"open", wide, NULL, NULL, SW_SHOWNORMAL) > 32;
	}
#elif defined(__linux__)
	{
		/*no shell: the address is an argument, not a command line*/
		pid_t pid = fork();
		if (pid == 0)
		{
			execlp("xdg-open", "xdg-open", url, (char*)NULL);
			_exit(127);
		}
		return pid > 0;
	}
#else
	return false;
#endif
}

/*
  THE PRIMITIVE'S OWN FALSIFIER, in the shipping binary rather than in a test
  harness somebody has to remember to run.  RFC 8032 section 7.1's published
  vectors: a key derived from a seed, a signature over a message, and -- the arm
  that matters -- a tampered signature that must be REFUSED.  A verifier that
  returned true unconditionally would pass the first three.
*/
static void CL_Receipt_SelfTest_f(void)
{
	static const char *vec[][4] =
	{
		{"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
		 "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
		 "",
		 "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
		{"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
		 "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
		 "72",
		 "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
		{"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
		 "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
		 "af82",
		 "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
	};
	qbyte seed[32], wantpk[32], wantsig[64], msg[8], pk[32], sk[64], sig[64];
	size_t i, mlen;
	int bad = 0;

	for (i = 0; i < countof(vec); i++)
	{
		/*ONE RESULT PER STATEMENT, IN ORDER, and the first cut of this function
		  is why.  It computed all four columns inside the Con_Printf argument
		  list, with the refuse arm flipping a bit of `sig` -- and C does not
		  define the order arguments are evaluated in.  gcc took the last one
		  first, so the tampered signature was compared against the vector and
		  every `sig` column read FAIL while the primitive was correct.  A
		  selftest that reports a defect it caused itself is worse than none.*/
		qboolean okpk, oksig, okver, okref;

		Base16_DecodeBlock(vec[i][0], seed, sizeof(seed));
		Base16_DecodeBlock(vec[i][1], wantpk, sizeof(wantpk));
		mlen = Base16_DecodeBlock(vec[i][2], msg, sizeof(msg));
		Base16_DecodeBlock(vec[i][3], wantsig, sizeof(wantsig));

		Ed25519_FromSeed(pk, sk, seed);
		Ed25519_Sign(sig, msg, mlen, sk);

		okpk  = !memcmp(pk, wantpk, sizeof(pk));
		oksig = !memcmp(sig, wantsig, sizeof(sig));
		okver = Ed25519_Verify(wantsig, msg, mlen, wantpk);
		sig[63] ^= 1;
		okref = !Ed25519_Verify(sig, msg, mlen, pk);

		Con_Printf("ed25519 vector %u: pk %s  sig %s  verify %s  refuse %s\n",
				   (unsigned)i,
				   okpk ? "ok" : "FAIL", oksig ? "ok" : "FAIL",
				   okver ? "ok" : "FAIL", okref ? "ok" : "FAIL");
		if (!okpk || !oksig || !okver || !okref)
			bad++;
	}
	Con_Printf("ed25519 selftest: %s\n", bad ? "FAILED" : "all vectors ok");
}

/*
  PATCH 418 -- HANDING THE SIDECAR OVER.

  A lobby's `.rec` is the only one of the three evidence files that exists
  anywhere but the player's disk: Rec_ViewEnd drops the angle sidecar whenever
  the recording is on a remote server, and Rec_HidEnd keeps a journal only for a
  PB.  So the receipt of Patch 417 commits to digests of bytes nobody can check,
  and the cross-file consistency the whole design rests on is an offline claim
  about the player's own directory.

  THE CLIENT ARMS, THE SERVER ASKS, AND THE TWO NAME DIFFERENT THINGS.  The
  gamecode arms the file it has just written (`rec_ul_arm`), which is the only
  path this end will ever send; the server then asks for it (`rec_ul_send`,
  stuffed) and names only where ITS OWN copy lands.  A server that asks for a
  file this client did not arm gets nothing -- the request carries no path at
  all.

  WHY THE NETCHAN AND NOT HTTP.  The connection is already authenticated and
  already knows which run this is; an HTTP endpoint would need a new public
  write surface, a nonce to authenticate it, a raised body cap and a second
  retention story.

  WHAT IT COSTS, IN THE NUMBERS AND NOT THE ADJECTIVES.  A sidecar is 2.93 KB/s
  of run (20 KB for 7 s, measured), so a 2-minute run is ~352 KB.  The transport
  is one <=768-byte chunk per SERVER REQUEST -- CL_NextUpload sends one and
  returns, and only the reliable `nextul` stufftext re-enters it -- so that run
  is 459 round trips: about 25 s at 30 ms RTT and 80 s at 150 ms.  Upstream
  volume is ~371 KB total, ~15 KB/s, which is nothing; the WALL CLOCK is the
  cost, and the first draft of this paragraph said "seconds in a lobby", which
  was wrong by a factor of ten and is the reason the server had to learn what to
  do when the next run finishes first (see SV_RecUpload_f).

  THE JOURNAL DOES NOT FIT THROUGH HERE AT ALL.  It is 110 KB/s of run (8.38 MB
  for 76 s, measured) -- 38x the sidecar by rate -- so the 4 MiB cap below
  refuses anything past about 38 seconds of run, and even a run that fits is
  17,000 round trips: 16 minutes at 30 ms, 50 at 150 ms.  `run_evidence_ul 2`
  is therefore a LAN and short-run switch, not a way to collect journals from a
  public lobby; anything else needs a transport that is not this one.
*/
#define RCPT_ULMAX	(4*1024*1024)

/*
  FOUR SLOTS: TWO RUNS OF TWO FILES.

  It was two, which is one run -- and one run is not enough, because the server
  asks for a run's second file only when its first has ARRIVED.  A sidecar is
  one chunk per round trip (25 s at 30 ms RTT, 80 s at 150 ms for a two-minute
  run) and the next run can easily finish inside that, so the arms for the run
  still being asked about have to survive the next one.  Two runs is the bound
  the transport actually needs; the oldest arming is dropped when a fifth
  arrives, and a slot that is never asked for costs a path and nothing else.

  Which slot answers a request is decided by the run and the kind the server
  names, not by position -- see CL_Receipt_ULSend_f.
*/
#define RCPT_ULSLOTS 4
static char rcpt_ularm[RCPT_ULSLOTS][MAX_QPATH];
static cvar_t rcpt_upload = CVARFD("rec_upload", "1", CVAR_ARCHIVE|CVAR_NOTFROMSERVER,
	"Allow consented, per-run armed angle sidecars to be uploaded when the server requests them.");
static cvar_t rcpt_hid_upload = CVARFD("rec_hid_upload", "0", CVAR_ARCHIVE|CVAR_NOTFROMSERVER,
	"New online acceptance includes raw mouse/keyboard run journal uploads to the official HTTPS collector unless you opt out. "
	"Older saved answers do not grant the new scope. Stop cancels active uploads and blocks new ones; rec_hid 0 also stops local recording. "
	"Native confirmed permission is still required; evidence already received is not deleted.");
//ftesurf (P1101): the menu's mirror of data/consent.txt, read by every online gate in menu and
//csprogs. A plain QC cvar until now: a server's `seta rec_terms 8` set an override cvar() read.
//-1 until the menu publishes it, so a launch without the menu still reads consent.txt (Rec_TermsReport).
static cvar_t rcpt_terms = CVARFD("rec_terms", "-1", CVAR_NOTFROMSERVER|CVAR_NOSAVE,
	"The terms version accepted in data/consent.txt, set by the menu at boot and on acceptance; -1 before it has. Not saved; a server may not set it.");

#ifdef WEBCLIENT
#define HID_HTTP_HEAD 1024
#define HID_HTTP_MAX (64u * 1024u * 1024u)
static struct dl_download *rcpt_http;
static double rcpt_http_until;
static char rcpt_http_reply[128];
static cvar_t rcpt_http_test = CVARFD("rec_hid_testurl", "", CVAR_NOTFROMSERVER,
	"Local diagnostic only: a http://127.0.0.1:PORT/api/hid journal receiver. Empty uses the official HTTPS collector. Never accepts a remote URL.");

static void CL_Receipt_HTTPCancel(void)
{
	if (rcpt_http)
	{
		struct dl_download *dl = rcpt_http;
		rcpt_http = NULL;
		dl->notifycomplete = NULL;
		DL_Close(dl);
		Con_Printf("rec_ul_send: journal HTTP cancelled\n");
	}
}

static void CL_Receipt_HTTPDone(struct dl_download *dl)
{
	char reply[128];
	size_t len = dl->file ? VFS_GETLEN(dl->file) : 0;
	qboolean ok = false;
	if (dl->status == DL_FINISHED && dl->replycode == 200 && len < sizeof(reply))
	{
		if (dl->file && VFS_READ(dl->file, reply, len) == len)
		{
			reply[len] = 0;
			ok = !strcmp(reply, rcpt_http_reply);
		}
	}
	if (rcpt_http == dl)
		rcpt_http = NULL;
	Con_Printf(ok ? "rec_ul_send: journal HTTP stored and digest acknowledged\n" :
		CON_WARNING "rec_ul_send: journal HTTP not acknowledged (status %u); no evidence verdict\n", dl->replycode);
}

static qboolean CL_Receipt_HTTPName(const char *s, size_t maxlen, qboolean map)
{
	size_t n = 0;
	if (map && (*s == '.' || strstr(s, "..")))
		return false;
	for (; *s; s++, n++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
			(*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || (map && (*s == '.' || *s == '+'))))
			return false;
	return n && n <= maxlen;
}

static qboolean CL_Receipt_HTTPSend(const char *path, const char *nonce, const char *map, const char *runid)
{
	vfsfile_t *file;
	qofs_t size;
	char *body, statement[512], pubhex[65], sighex[129], hex[65];
	const char *url = "https://play.proto.bar/api/hid";
	qbyte digest[32], signature[64];
	size_t n;

	if (rcpt_http || !CL_Receipt_HTTPName(map, 64, true) || !CL_Receipt_HTTPName(runid, 96, false) || strlen(nonce) != 32)
		return false;
	for (n = 0; n < 32; n++)
		if (!((nonce[n] >= '0' && nonce[n] <= '9') || (nonce[n] >= 'a' && nonce[n] <= 'f')))
			return false;
	if (*rcpt_http_test.string)
	{
		char *end;
		unsigned long port;
		if (strncmp(rcpt_http_test.string, "http://127.0.0.1:", 17))
			return false;
		port = strtoul(rcpt_http_test.string + 17, &end, 10);
		if (!port || port > 65535 || strcmp(end, "/api/hid"))
			return false;
		url = rcpt_http_test.string;
	}
	if (!CL_Receipt_Key())
		return false;
	file = FS_OpenVFS(path, "rb", FS_GAME);
	if (!file)
		return false;
	size = VFS_GETLEN(file);
	if (size <= 0 || size > HID_HTTP_MAX)
	{
		VFS_CLOSE(file);
		return false;
	}
	/*One bounded owned buffer, transferred to HTTP (not copied by Put).
	  The HTTP transport has its own bounded send buffer; no journal-sized
	  allocation or traffic touches the game netchan.*/
	body = BZ_Malloc(HID_HTTP_HEAD + size);
	if (VFS_READ(file, body + HID_HTTP_HEAD, size) != size)
	{
		VFS_CLOSE(file);
		BZ_Free(body);
		return false;
	}
	VFS_CLOSE(file);
	CalcHash(&hash_sha2_256, digest, sizeof(digest), (const qbyte*)body + HID_HTTP_HEAD, size);
	CL_Receipt_Hex(hex, sizeof(hex), digest, sizeof(digest), true);
	Q_snprintfz(statement, sizeof(statement), "FTESURF-HID 1\nmap %s\nrunid %s\nnonce %s\nhid %s %u\n",
		map, runid, nonce, hex, (unsigned)size);
	Ed25519_Sign(signature, (const qbyte*)statement, strlen(statement), rcpt_sk);
	CL_Receipt_Hex(pubhex, sizeof(pubhex), rcpt_pk, sizeof(rcpt_pk), true);
	CL_Receipt_Hex(sighex, sizeof(sighex), signature, sizeof(signature), true);
	Q_snprintfz(body, HID_HTTP_HEAD, "%spub %s\nsig %s\n", statement, pubhex, sighex);
	n = strlen(body);
	memset(body + n, ' ', HID_HTTP_HEAD - n);
	body[HID_HTTP_HEAD - 1] = '\n';
	rcpt_http = HTTP_CL_Get(url, NULL, CL_Receipt_HTTPDone);
	if (!rcpt_http)
	{
		BZ_Free(body);
		return false;
	}
	rcpt_http->isquery = true;
	rcpt_http->sizelimit = 512;
	rcpt_http->headerlimit = 8192;
	rcpt_http->identityonly = true;
	/*The HTTP client's existing redirect guard rejects the FIRST redirect.
	  A collector may never forward this body to another host or plaintext.*/
	rcpt_http->redircount = 11;
	Q_strncpyz(rcpt_http->postmimetype, "application/octet-stream", sizeof(rcpt_http->postmimetype));
	rcpt_http->postdata = body;
	rcpt_http->postlen = HID_HTTP_HEAD + size;
	rcpt_http_until = Sys_DoubleTime() + 300;
	Q_snprintfz(rcpt_http_reply, sizeof(rcpt_http_reply), "stored %s %u\n", hex, (unsigned)size);
	Con_Printf("rec_ul_send: journal HTTP started (%u bytes)\n", (unsigned)size);
	return true;
}
#endif

void CL_Receipt_Think(void)
{
#ifdef WEBCLIENT
	if (rcpt_http && (!rcpt_upload.ival || !rcpt_hid_upload.ival || cls.state < ca_onserver || Sys_DoubleTime() >= rcpt_http_until))
		CL_Receipt_HTTPCancel();
#endif
}

static void CL_Receipt_ULArm_f(void)
{
	const char *name, *fallback;
	size_t l;
	int i;

	if (Cmd_FromGamecode())
	{	/*the same rule rec_sign has: a server may not choose what this client
		  sends, even though the gamecode it wrote can arm one of its own.*/
		Con_Printf("rec_ul_arm: a server cannot arm this client's evidence upload. Refused.\n");
		return;
	}
	if (Cmd_Argc() < 2 || !*Cmd_Argv(1) || !strcmp(Cmd_Argv(1), "-"))
	{	/*disarming is not an error: a run with no evidence says so*/
		CL_Receipt_Disarm();
		return;
	}

	/*THE SAME SANDBOX in_journal_end USES, plus a `data/` prefix, plus an
	  evidence extension.  Anything else is not this game's evidence and has no
	  business leaving the machine.*/
	if (!QC_FixFileName(Cmd_Argv(1), &name, &fallback) || strncmp(name, "data/", 5))
	{
		Con_DPrintf("rec_ul_arm: refused %s\n", Cmd_Argv(1));
		return;
	}
	l = strlen(name);
	if (l < 6 || (strcmp(name + l - 5, ".view") && strcmp(name + l - 4, ".hid")))
	{
		Con_DPrintf("rec_ul_arm: %s is not evidence\n", name);
		return;
	}
	for (i = 0; i < RCPT_ULSLOTS; i++)
	{
		if (!*rcpt_ularm[i])
		{
			Q_strncpyz(rcpt_ularm[i], name, sizeof(rcpt_ularm[i]));
			Con_DPrintf("rec_ul_arm[%i]: %s\n", i, name);
			return;
		}
	}
	/*FULL: the OLDEST offer goes, not this one.  Refusing the new arming was
	  the old behaviour and it is the wrong end to drop from -- the run being
	  armed is the one whose receipt was just signed.*/
	Con_DPrintf("rec_ul_arm: slots full, dropping %s for %s\n", rcpt_ularm[0], name);
	for (i = 1; i < RCPT_ULSLOTS; i++)
		Q_strncpyz(rcpt_ularm[i-1], rcpt_ularm[i], sizeof(rcpt_ularm[i-1]));
	Q_strncpyz(rcpt_ularm[RCPT_ULSLOTS-1], name, sizeof(rcpt_ularm[RCPT_ULSLOTS-1]));
}

/*
  PATCH 418: FORGET WHAT WAS ARMED, ON DISCONNECT.

  An arming is an offer to ONE server about ONE run.  Left standing across a
  disconnect it becomes an offer to whoever is asked next: join a server that
  ships no csprogs -- so nothing in the gamecode ever clears it -- and a single
  stuffed `rec_ul_send` would hand it the previous server's run evidence, with
  no run of its own involved.  Called from CL_Disconnect, beside CL_StopUpload.
*/
void CL_Receipt_Disarm(void)
{
	int i;
#ifdef WEBCLIENT
	CL_Receipt_HTTPCancel();
#endif
	for (i = 0; i < RCPT_ULSLOTS; i++)
		*rcpt_ularm[i] = 0;
}

/*
  SAYING NO, OUT LOUD.

  A client that cannot answer a request -- nothing armed for that run, or a file
  over its own cap -- used to simply not answer, and the server then held the
  destination for its full stale timeout AND refused the next runs' requests
  behind it.  `snap` is the stringcmd QuakeWorld already has for "I decline the
  upload you asked for" (SV_NoSnap_f), and it performs exactly the teardown the
  server needs: close nothing, delete the partial, clear the destination.
*/
static void CL_Receipt_ULDecline(void)
{
	if (cls.state >= ca_onserver)
		Cbuf_AddText("cmd snap\n", RESTRICT_LOCAL);
}

static void CL_Receipt_ULSend_f(void)
{
	const char *nonce, *kind, *base, *dot;
	int i;
	qboolean http;

	/*
	  THE REQUEST NAMES THE RUN AND THE KIND, AND STILL NO PATH.

	  It used to name nothing at all and this took the LOWEST armed slot, which
	  is right only while the answer is prompt.  It is not: a sidecar goes out
	  at one chunk per round trip, so the next run routinely ends first, clears
	  both slots (`rec_ul_arm -`) and re-arms them with its own files.  The
	  server's request for run A's JOURNAL was then answered with run B's
	  SIDECAR -- stored under run A's runid, where the checker holds it against
	  run A's signed journal digest and reports a mismatch about a player who
	  did nothing wrong.  Two reviewers derived that independently and neither
	  needed a cheat to do it.

	  The nonce is this run's own, which both ends already have (Patch 416), and
	  the kind is the extension of the destination the SERVER chose.  Neither
	  says anything about this machine's filesystem, so the asymmetry the whole
	  design rests on is untouched.
	*/
	if (Cmd_Argc() < 3)
	{
		Con_DPrintf("rec_ul_send: needs a run and a kind\n");
		return;
	}
	nonce = Cmd_Argv(1);
	kind = Cmd_Argv(2);
	http = (!strcmp(Cmd_Argv(0), "rec_ul_http") && Cmd_Argc() == 5 && !strcmp(kind, "hid"));
	if (!rcpt_upload.ival)
	{
		CL_Receipt_Disarm();
		if (!http)
			CL_Receipt_ULDecline();
		return;
	}
	/*A separate, default-off choice: earlier online terms explicitly promised
	  that raw journals stayed local. Never send HID through the legacy route.*/
	if (!strcmp(kind, "hid") && (!http || !rcpt_hid_upload.ival))
	{
		if (!http)
			CL_Receipt_ULDecline();
		return;
	}
	if (!http && CL_IsUploading())
	{
		Con_DPrintf("rec_ul_send: already sending\n");
		return;
	}
	for (i = 0; i < RCPT_ULSLOTS; i++)
	{
		if (!*rcpt_ularm[i])
			continue;
		base = strrchr(rcpt_ularm[i], '/');
		base = base ? base + 1 : rcpt_ularm[i];
		dot = strrchr(base, '.');
		if (!dot || strcmp(dot + 1, kind))
			continue;		/*a .view asked for, a .hid armed*/
		if ((size_t)(dot - base) != strlen(nonce) || strncmp(base, nonce, dot - base))
			continue;		/*armed, but for a different run*/
		break;
	}
	if (i == RCPT_ULSLOTS)
	{
		Con_DPrintf("rec_ul_send: nothing armed for %s %s\n", nonce, kind);
		if (!http)
			CL_Receipt_ULDecline();
		return;
	}
	/*
	  THE PATH IS OURS.  The server can ask, at a moment of its choosing, for
	  the file the gamecode armed for the run it names -- and for nothing else.
	  One arming is good for one send; a second request gets nothing until the
	  gamecode arms again.
	*/
	/*SAID OUT LOUD.  This used to be Con_DPrintf, so the one case that actually
	  happens -- a journal over the cap, which is any run past ~38 seconds --
	  was invisible on a shipping client AND left the server holding a
	  destination nobody would ever write to.  The player can see that their
	  file was not sent; the receipt still commits to its digest either way.*/
	if (http)
	{
#ifdef WEBCLIENT
		if (!CL_Receipt_HTTPSend(rcpt_ularm[i], nonce, Cmd_Argv(3), Cmd_Argv(4)))
#endif
			Con_Printf(CON_WARNING "rec_ul_send: journal HTTP not started (busy, unavailable, invalid, or over 64 MiB)\n");
	}
	else if (!CL_StartUploadFile(rcpt_ularm[i], RCPT_ULMAX))
	{
		Con_Printf(CON_WARNING "rec_ul_send: %s not sent -- missing, empty, or over the %i byte cap\n",
				   rcpt_ularm[i], RCPT_ULMAX);
		CL_Receipt_ULDecline();
	}
	*rcpt_ularm[i] = 0;		/*one arming, one send*/
	/*AND THE SLOTS COMPACT, so index order stays age order.  The eviction below
	  drops slot 0 as "the oldest", which stops being true the moment a middle
	  slot is consumed out of order -- a run armed two runs ago could then
	  outlive one armed since.*/
	for (; i + 1 < RCPT_ULSLOTS; i++)
		Q_strncpyz(rcpt_ularm[i], rcpt_ularm[i+1], sizeof(rcpt_ularm[i]));
	*rcpt_ularm[RCPT_ULSLOTS-1] = 0;
}

void CL_Receipt_Init(void)
{
	/*Register merges native protection flags while preserving an early +set
	  value. Cvar_Get alone does not retrofit NOTFROMSERVER on that value.*/
	Cvar_Register(&rcpt_upload, "Anti-cheat evidence");
	Cvar_Register(&rcpt_hid_upload, "Anti-cheat evidence");
	Cvar_Register(&rcpt_terms, "Anti-cheat evidence");
#ifdef WEBCLIENT
	Cvar_Get("fshidhttp", "1", CVAR_USERINFO|CVAR_NOSET, "Anti-cheat evidence");
	Cvar_Register(&rcpt_http_test, "Anti-cheat evidence");
	Cmd_AddCommandD("rec_ul_http", CL_Receipt_ULSend_f, "rec_ul_http nonce hid map run -- send only an opted-in armed journal to the official HTTPS collector. No legacy HID upload.");
#endif
	Cmd_AddCommandD("rec_sign", CL_Receipt_Sign_f,
					"rec_sign <nonce> <ticks> [viewpath] -- sign this run's evidence for the server.  Called by the gamecode at the end of a run.");
	Cmd_AddCommandD("rec_pubkey", CL_Receipt_PubKey_f,
					"Print this install's run-signing public key, which is the identity a ranked board knows you by.");
	Cmd_AddCommandD("ed25519_selftest", CL_Receipt_SelfTest_f,
					"Check the signing primitive against RFC 8032's published vectors.");
	/*Patch 418: the evidence upload.  Arm is the gamecode's; send is the
	  server's, and carries no path -- see CL_Receipt_ULArm_f.*/
	Cmd_AddCommandD("rec_ul_arm", CL_Receipt_ULArm_f,
					"rec_ul_arm <path> -- offer this run's evidence file for upload.  Called by the gamecode.");
	Cmd_AddCommandD("rec_ul_send", CL_Receipt_ULSend_f,
					"rec_ul_send <nonce> <view|hid> -- send the evidence the gamecode armed for that run.  The server asks; the file is this client's choice.");
}

#endif
