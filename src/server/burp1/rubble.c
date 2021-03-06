#include "include.h"

static int incexc_matches(const char *fullrealwork, const char *incexc)
{
	int ret=0;
	int got=0;
	FILE *fp=NULL;
	char buf[4096]="";
	const char *inc=NULL;
	char *old_incexc_path=NULL;
	if(!(old_incexc_path=prepend_s(fullrealwork, "incexc")))
		return -1;
	if(!(fp=open_file(old_incexc_path, "rb")))
	{
		// Assume that no incexc file could be found because the client
		// was on an old version. Assume resume is OK and return 1.
		ret=1;
		goto end;
	}
	inc=incexc;
	while((got=fread(buf, 1, sizeof(buf), fp))>0)
	{
		if(strlen(inc)<(size_t)got) break;
		if(strncmp(buf, inc, got)) break;
		inc+=got;
	}
	if(inc && strlen(inc)) ret=0;
	else ret=1;
end:
	close_fp(&fp);
	free_w(&old_incexc_path);
	return ret;
}

static int maybe_rebuild_manifest(struct sdirs *sdirs, int compress,
	struct conf *cconf)
{
	struct stat statp;
	if(lstat(sdirs->manifest, &statp))
		return backup_phase3_server_burp1(sdirs,
			1 /* recovery mode */, compress, cconf);

	unlink(sdirs->phase2data);
	unlink(sdirs->unchangeddata);
	return 0;
}

static int working_delete(struct asfd *asfd, struct sdirs *sdirs)
{
	// Try to remove it and start again.
	logp("deleting old working directory\n");
	if(recursive_delete(sdirs->rworking, NULL, 1 /* delete files */))
	{
		log_and_send(asfd, "Old working directory is in the way.\n");
		return -1;
	}
	// Get rid of the symlink.
	unlink(sdirs->working);
	return 0;
}

static int working_use(struct asfd *asfd, struct sdirs *sdirs,
	const char *incexc, int *resume, struct conf *cconf)
{
	// Use it as it is.

	logp("converting old working directory into the latest backup\n");

	// FIX THIS: There might be a partial file written that is not yet
	// logged to the manifest. It does no harm other than taking up some
	// disk space. Detect this and remove it.

	// Get us a partial manifest from the files lying around.
	if(maybe_rebuild_manifest(sdirs, 1 /* compress */, cconf)) return -1;

	// Now just rename the working link to be a finishing link,
	// then run this function again.
	// The rename() race condition is automatically recoverable here.
	if(do_rename(sdirs->working, sdirs->finishing)) return -1;

	return check_for_rubble_burp1(asfd, sdirs, incexc, resume, cconf);
}

static int working_resume(struct asfd *asfd, struct sdirs *sdirs,
	const char *incexc, int *resume, struct conf *cconf)
{
	if(cconf->restore_client)
	{
		// This client is not the original client, resuming might cause
		// all sorts of trouble.
		log_and_send(asfd, "Found interrupted backup - not resuming because the connected client is not the original");
		return -1;
	}

	logp("Found interrupted backup.\n");

	// Check that the current incexc configuration is the same
	// as before.
	switch(incexc_matches(sdirs->rworking, incexc))
	{
		case 1:
			// Attempt to resume on the next backup.
			logp("Will resume on the next backup request.\n");
			*resume=1;
			return 0;
		case 0:
			logp("Includes/excludes changed since last backup.\n");
			logp("Will treat last backup as finished.\n");
			return working_use(asfd, sdirs, incexc, resume, cconf);
		case -1:
		default:
			return -1;
	}
}

static int get_fullrealwork(struct asfd *asfd,
	struct sdirs *sdirs, struct conf *conf)
{
	struct stat statp;

	if(sdirs_get_real_working_from_symlink(sdirs, conf))
		return -1;

	if(lstat(sdirs->rworking, &statp))
	{
		logp("removing dangling working symlink %s -> %s\n",
			sdirs->working, sdirs->rworking);
		unlink(sdirs->working);
		free_w(&sdirs->rworking);
	}
	return 0;
}

static int recover_finishing(struct asfd *asfd,
	struct sdirs *sdirs, struct conf *cconf)
{
	logp("Found finishing symlink - attempting to complete prior backup!\n");
	if(backup_phase4_server_burp1(sdirs, cconf))
	{
		log_and_send(asfd, "Problem with prior backup. Please check the client log on the server.");
		return -1;
	}
	logp("Prior backup completed OK.\n");
	return 0;
}

static int recover_working(struct asfd *asfd,
	struct sdirs *sdirs, const char *incexc,
	int *resume, struct conf *cconf)
{
	int ret=-1;
	char msg[256]="";
	char *logpath=NULL;
	struct stat statp;
	char *phase1datatmp=NULL;
	const char *wdrm=cconf->recovery_method;

	// The working directory has not finished being populated.
	// Check what to do.
	if(get_fullrealwork(asfd, sdirs, cconf)) goto error;
	if(!sdirs->rworking) goto end;

	// We have found an old working directory - open the log inside
	// for appending.
	if(!(logpath=prepend_s(sdirs->rworking, "log"))
	  || set_logfp(logpath, cconf))
		goto error;

	logp("found old working directory: %s\n", sdirs->rworking);
	logp("working_dir_recovery_method: %s\n", wdrm);

	if(!(phase1datatmp=get_tmp_filename(sdirs->phase1data)))
		goto error;
	if(!lstat(phase1datatmp, &statp))
	{
		// Phase 1 did not complete - delete everything.
		logp("Phase 1 has not completed.\n");
		wdrm="delete";
	}

	if(!strcmp(wdrm, "delete"))
	{
		if(working_delete(asfd, sdirs))
			goto error;
	}
	else if(!strcmp(wdrm, "use"))
	{
		if(working_use(asfd, sdirs, incexc, resume, cconf))
			goto error;
	}
	else if(!strcmp(wdrm, "resume"))
	{
		if(working_resume(asfd, sdirs, incexc, resume, cconf))
			goto error;
	}
	else
	{
		snprintf(msg, sizeof(msg),
			"Unknown working_dir_recovery_method: %s\n", wdrm);
		log_and_send(asfd, msg);
		goto error;
	}

end:
	ret=0;
error:
	free_w(&logpath);
	free_w(&phase1datatmp);
	set_logfp(NULL, cconf); // fclose the logfp
	return ret;
}

static int recover_currenttmp(struct sdirs *sdirs)
{
	struct stat statp;
	logp("Found currenttmp symlink\n");
	if(stat(sdirs->currenttmp, &statp))
	{
		logp("But currenttmp is not pointing at something valid.\n");
		logp("Deleting it.\n");
		return unlink_w(sdirs->currenttmp, __func__);
	}

	if(!lstat(sdirs->current, &statp))
	{
		if(S_ISLNK(statp.st_mode))
		{
			logp("But current symlink already exists!\n");
			if(!stat(sdirs->current, &statp))
			{
				logp("And current symlink points at something valid.\n");
				logp("Deleting currenttmp.\n");
				return unlink_w(sdirs->currenttmp, __func__);
				
			}
			else
			{
				logp("But current symlink is not pointing at something valid.\n");
				logp("Replacing current with currenttmp.\n");
				return do_rename(sdirs->currenttmp,
					sdirs->current);
			}
		}
		else
		{
			logp("But current already exists and is not a symlink!\n");
			logp("Giving up.\n");
			return -1;
		}
	}
	else
	{
		logp("Renaming currenttmp to current\n");
		return do_rename(sdirs->currenttmp, sdirs->current);
	}
	return 0;
}

int check_for_rubble_burp1(struct asfd *asfd,
	struct sdirs *sdirs, const char *incexc,
	int *resume, struct conf *cconf)
{
	struct stat statp;

	if(!lstat(sdirs->finishing, &statp))
	{
		if(S_ISLNK(statp.st_mode))
			return recover_finishing(asfd, sdirs, cconf);
		log_and_send(asfd, "Finishing directory is not a symlink.\n");
		return -1;
	}

	if(!lstat(sdirs->working, &statp))
	{
		if(S_ISLNK(statp.st_mode))
			return recover_working(asfd,
				sdirs, incexc, resume, cconf);
		log_and_send(asfd, "Working directory is not a symlink.\n");
		return -1;
	}

	if(!lstat(sdirs->currenttmp, &statp))
	{
		if(S_ISLNK(statp.st_mode))
			return recover_currenttmp(sdirs);
		log_and_send(asfd, "Currenttmp directory is not a symlink.\n");
		return -1;
	}

	return 0;
}
