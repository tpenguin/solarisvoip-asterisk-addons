/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * format_mp3.c
 * Anthony Minessale <anthmct@yahoo.com>
 *
 * Derived from other asterisk sound formats by
 * Mark Spencer <markster@linux-support.net>
 *
 * Thanks to mpglib from http://www.mpg123.org/
 * and Chris Stenton [jacs@gnome.co.uk]
 * for coding the ability to play stereo and non-8khz files
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

#include "mpg123.h" 
#include "mpglib.h" 
#include <compat.h>
#include <asterisk/lock.h>
#include <asterisk/channel.h>
#include <asterisk/file.h>
#include <asterisk/logger.h>
#include <asterisk/sched.h>
#include <asterisk/module.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <sys/time.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <asterisk/endian.h>

#define MP3_BUFLEN 320
#define MP3_SCACHE 16384
#define MP3_DCACHE 8192

/* Based on format_wav.c */

struct ast_filestream {
	void *reserved[AST_RESERVED_POINTERS];
	/* This is what a filestream means to us */
	FILE *f; /* Descriptor */
	struct ast_frame fr;				/* Frame information */
	char waste[AST_FRIENDLY_OFFSET];	/* Buffer for sending frames, etc */
	char empty;							/* Empty character */
	int lasttimeout;
	int maxlen;
	struct timeval last;
	struct mpstr mp;
	char buf[MP3_BUFLEN];
	char sbuf[MP3_SCACHE];
	char dbuf[MP3_DCACHE];
	int buflen;
	int sbuflen;
	int dbuflen;
	int dbufoffset;
	int sbufoffset;
	int lastseek;
	int offset;
	long seek;
};


AST_MUTEX_DEFINE_STATIC(mp3_lock);
static int glistcnt = 0;

static char *name = "mp3";
static char *desc = "MP3 format [Any rate but 8000hz mono optimal]";
static char *exts = "mp3";

#define BLOCKSIZE 160
#define OUTSCALE 4096

#define GAIN -4		/* 2^GAIN is the multiple to increase the volume by */

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define htoll(b) (b)
#define htols(b) (b)
#define ltohl(b) (b)
#define ltohs(b) (b)
#else
#if __BYTE_ORDER == __BIG_ENDIAN
#define htoll(b)  \
          (((((b)      ) & 0xFF) << 24) | \
	       ((((b) >>  8) & 0xFF) << 16) | \
		   ((((b) >> 16) & 0xFF) <<  8) | \
		   ((((b) >> 24) & 0xFF)      ))
#define htols(b) \
          (((((b)      ) & 0xFF) << 8) | \
		   ((((b) >> 8) & 0xFF)      ))
#define ltohl(b) htoll(b)
#define ltohs(b) htols(b)
#else
#error "Endianess not defined"
#endif
#endif


static struct ast_filestream *mp3_open(FILE *f)
{
	struct ast_filestream *tmp;
	if ((tmp = malloc(sizeof(struct ast_filestream)))) {
		memset(tmp, 0, sizeof(struct ast_filestream));
		if (ast_mutex_lock(&mp3_lock)) {
			ast_log(LOG_WARNING, "Unable to lock mp3 list\n");
			free(tmp);
			return NULL;
		}
		InitMP3(&tmp->mp, OUTSCALE);
		tmp->dbuflen = 0;
		tmp->f = f;
		tmp->fr.data = tmp->buf;
		tmp->fr.frametype = AST_FRAME_VOICE;
		tmp->fr.subclass = AST_FORMAT_SLINEAR;
		/* datalen will vary for each frame */
		tmp->fr.src = name;
		tmp->fr.mallocd = 0;
		tmp->offset = 0;
		glistcnt++;
		ast_mutex_unlock(&mp3_lock);
		ast_update_use_count();
	}
	return tmp;
}


static void mp3_close(struct ast_filestream *s)
{
	if (ast_mutex_lock(&mp3_lock)) {
		ast_log(LOG_WARNING, "Unable to lock mp3 list\n");
		return;
	}

	ExitMP3(&s->mp);
	
	glistcnt--;
	ast_mutex_unlock(&mp3_lock);
	ast_update_use_count();
	fclose(s->f);
	free(s);
	s = NULL;

}

static int mp3_squeue(struct ast_filestream *s) 
{
	int res=0;
	s->lastseek = ftell(s->f);
	s->sbuflen = fread(s->sbuf, 1, MP3_SCACHE, s->f);
	if(s->sbuflen < 0) {
		ast_log(LOG_WARNING, "Short read (%d) (%s)!\n", s->sbuflen, strerror(errno));
		return -1;
	}
	res = decodeMP3(&s->mp,s->sbuf,s->sbuflen,s->dbuf,MP3_DCACHE,&s->dbuflen);
	if(res != MP3_OK)
		return -1;
	s->sbuflen -= s->dbuflen;
	s->dbufoffset = 0;
	return 0;
}
static int mp3_dqueue(struct ast_filestream *s) 
{
	int res=0;
	if((res = decodeMP3(&s->mp,NULL,0,s->dbuf,MP3_DCACHE,&s->dbuflen)) == MP3_OK) {
		s->sbuflen -= s->dbuflen;
		s->dbufoffset = 0;
	}
	return res;
}
static int mp3_queue(struct ast_filestream *s) {
	int res = 0, bytes = 0;
	if(s->seek) {
		ExitMP3(&s->mp);
		InitMP3(&s->mp, OUTSCALE);
		fseek(s->f, 0, SEEK_SET);
		s->sbuflen = s->dbuflen = s->offset = 0;
		while(s->offset < s->seek) {
			if(mp3_squeue(s))
				return -1;
			while(s->offset < s->seek && ((res = mp3_dqueue(s))) == MP3_OK) {
				for(bytes = 0 ; bytes < s->dbuflen ; bytes++) {
					s->dbufoffset++;
					s->offset++;
					if(s->offset >= s->seek)
						break;
				}
			}
			if(res == MP3_ERR)
				return -1;
		}
		
		s->seek = 0;
		return 0;
	}
	if(s->dbuflen == 0) {
		if(s->sbuflen) {
			res = mp3_dqueue(s);
			if(res == MP3_ERR)
				return -1;
		}
		if(! s->sbuflen || res != MP3_OK) {
			if(mp3_squeue(s))
				return -1;
		}
		
	}

	return 0;
}

static struct ast_frame *mp3_read(struct ast_filestream *s, int *whennext)
{

	int delay =0;
	int save=0;

	/* Send a frame from the file to the appropriate channel */

	if(mp3_queue(s))
		return NULL;

	if(s->dbuflen) {
		for(s->buflen=0; s->buflen < MP3_BUFLEN && s->buflen < s->dbuflen; s->buflen++) {
			s->buf[s->buflen] = s->dbuf[s->buflen+s->dbufoffset];
			s->sbufoffset++;
		}
		s->dbufoffset += s->buflen;
		s->dbuflen -= s->buflen;

		if(s->buflen < MP3_BUFLEN) {
			if(mp3_queue(s))
				return NULL;

			for(save = s->buflen; s->buflen < MP3_BUFLEN; s->buflen++) {
				s->buf[s->buflen] = s->dbuf[(s->buflen-save)+s->dbufoffset];
				s->sbufoffset++;
			}
			s->dbufoffset += (MP3_BUFLEN - save);
			s->dbuflen -= (MP3_BUFLEN - save);

		} 

	}
	
	s->offset += s->buflen;
	delay = s->buflen/2;
	s->fr.frametype = AST_FRAME_VOICE;
	s->fr.subclass = AST_FORMAT_SLINEAR;
	s->fr.offset = AST_FRIENDLY_OFFSET;
	s->fr.datalen = s->buflen;
	s->fr.data = s->buf;
	s->fr.mallocd = 0;
	s->fr.samples = delay;
	*whennext = delay;
	return &s->fr;
}


static int mp3_write(struct ast_filestream *fs, struct ast_frame *f)
{
	ast_log(LOG_ERROR,"I Can't write MP3 only read them.\n");
	return -1;

}


static int mp3_seek(struct ast_filestream *fs, long sample_offset, int whence)
{

	off_t min,max,cur;
	long offset=0,samples;
	samples = sample_offset * 2;

	min = 0;
	fseek(fs->f, 0, SEEK_END);
	max = ftell(fs->f) * 100;
	cur = fs->offset;

	if (whence == SEEK_SET)
		offset = samples + min;
	else if (whence == SEEK_CUR || whence == SEEK_FORCECUR)
		offset = samples + cur;
	else if (whence == SEEK_END)
		offset = max - samples;
	if (whence != SEEK_FORCECUR) {
		offset = (offset > max)?max:offset;
	}

	fs->seek = offset;
	return fs->seek;
	
}

static struct ast_filestream *mp3_rewrite(FILE *f, const char *comment) 
{
	ast_log(LOG_ERROR,"I Can't write MP3 only read them.\n");
	return NULL;
}

static int mp3_trunc(struct ast_filestream *fs) 
{

	ast_log(LOG_ERROR,"I Can't write MP3 only read them.\n");
	return -1;
}

static long mp3_tell(struct ast_filestream *fs)
{
	return fs->offset/2;
}

static char *mp3_getcomment(struct ast_filestream *s)
{
	return NULL;
}

int load_module()
{
	InitMP3Constants();
	return ast_format_register(name, exts, AST_FORMAT_SLINEAR,
							   mp3_open,
							   mp3_rewrite,
							   mp3_write,
							   mp3_seek,
							   mp3_trunc,
							   mp3_tell,
							   mp3_read,
							   mp3_close,
							   mp3_getcomment);
								
								
}

int unload_module()
{
	return ast_format_unregister(name);
}	

int usecount()
{
	int res;
	if (ast_mutex_lock(&mp3_lock)) {
		ast_log(LOG_WARNING, "Unable to lock mp3 list\n");
		return -1;
	}
	res = glistcnt;
	ast_mutex_unlock(&mp3_lock);
	return res;
}

char *description()
{
	return desc;
}


char *key()
{
	return ASTERISK_GPL_KEY;
}
