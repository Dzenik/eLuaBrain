// eLua shell 

#ifndef __SHELL_H__
#define __SHELL_H__

#include "term.h"

#define SHELL_WELCOMEMSG    "\neLua %s (STMBrain version) Copyright (C) 2007-2011 www.eluaproject.net\n"
#define SHELL_PROMPT        "brain> "
#define SHELL_PROMPT_COLOR  TERM_FGCOL_LIGHT_RED 
#define SHELL_COMMAND_COLOR TERM_FGCOL_LIGHT_GREEN
#define SHELL_ERRMSG        "Invalid command, type 'help' for help\n"
#define SHELL_MAXSIZE       50
#define SHELL_MAX_LUA_ARGS  8

int shell_init();
void shell_start();

#endif // #ifndef __SHELL_H__
