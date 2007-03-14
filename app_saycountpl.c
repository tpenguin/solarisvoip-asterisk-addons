/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * saycountpl application
 * 
 * Copyright (C) 2004, Andy Powell & TAAN Softworks Corp. 
 *
 */

#include <stdio.h>
#include <asterisk/file.h>
#include <asterisk/logger.h>
#include <asterisk/channel.h>
#include <asterisk/pbx.h>
#include <asterisk/module.h>
#include <asterisk/lock.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>


static char *tdesc = "Say polish counting words";

static char *app = "SayCountPL";

static char *synopsis = "Say the counting word the fits to a number";

static char *descrip =
"Polish grammar has some funny rules for counting words. for example 1 zloty, 2 zlote, 5 zlotych. This application will take the words for 1, 2-4 and 5 and\n"
"decide based on grammar rules which one to use with the number you pass to it.\n\n"
"Example: saycountpl(zloty,zlote,zlotych,122) will give: zlote\n";


STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int saywords(struct ast_channel *chan, char *word1, char *word2, char *word5, int num)
{
	/* Put this in a separate proc because it's bound to change */

	int md;
        int d=0;

	if (num >0) {
		md = num % 1000;
		if (md == 1) {
			ast_streamfile(chan, word1, chan->language);
			d = ast_waitstream(chan,"");
		}
		else {
			if (((md % 10) >= 2)  && ( (md % 10) <= 4 ) && ( ( md % 100) < 10 || (md %  100) > 20)) {
                	        ast_streamfile(chan, word2, chan->language);
                        	d = ast_waitstream(chan,"");
			}
			else {
                	        ast_streamfile(chan, word5, chan->language);
                        	d = ast_waitstream(chan,"");
			}
		}
	}

	return d;

}


static int sayword_exec(struct ast_channel *chan, void *data)
{
	int res=0;
	
        char *word1, *word2, *word5, *num;
        char *s;
	
        int inum;
	
	struct localuser *u;

	
	if (!data) {
		ast_log(LOG_WARNING, "You didn't pass any arguments - I need 4 arguments, word-1,word-2,word-5,number\n");
		return -1;
	}
	LOCAL_USER_ADD(u);
	/* Do our shit here */

        s = ast_strdupa((void *) data);

        word1 = strsep(&s, "|");
	word2 = strsep(&s, "|");
	word5 = strsep(&s, "|");
	num   = strsep(&s, "|");
					
	/* check to see if params passed */

        if (!word1 || !word2 || !word5 || !num) {
                ast_log(LOG_WARNING, "Saycountpl requires the arguments word-1|word-2|word-3|number\n");
                LOCAL_USER_REMOVE(u);
                return -1;
	}	
	
        if (sscanf(num, "%d", &inum) != 1) {
                ast_log(LOG_WARNING, "'%s' is not a valid number\n", num);
                LOCAL_USER_REMOVE(u);
                return -1;
        }
									
	/* do the saying part (after a bit of maths) */
	
	res = saywords(chan,word1,word2,word5,inum);


	
	LOCAL_USER_REMOVE(u);
	return res;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return ast_unregister_application(app);
}

int load_module(void)
{
	return ast_register_application(app, sayword_exec, synopsis, descrip);
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	int res;
	STANDARD_USECOUNT(res);
	return res;
}

char *key()
{
	return ASTERISK_GPL_KEY;
}


/* Get down on it... get down on it... */




